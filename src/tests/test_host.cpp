/// musicxx 外部插件宿主原生测试 (不依赖 Dart)
///
/// 覆盖 (plan §13.1 的首批用例):
/// - test_host_lifecycle: create/start/stop/destroy 幂等;
/// - test_plugin_load: 扫描 + 装载示例插件 + 钩子注册生效;
/// - test_hooks: decision 钩子裁决 (skip) / 观察钩子 / 未命中时零处理器;
/// - test_namespace: 未知钩子注册被拒 (由宿主实现保证, 这里校验已注册集合);
/// - 卸载后钩子注册全部摘除 (无残留)。
///
/// 运行: musicxx_extern_plugin_test <example_native 插件目录>

#include "musicxx_extern_plugin_api.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {

int g_failed = 0;
int g_checks = 0;

void check(bool ok, const char *what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("  [FAIL] %s\n", what);
  } else {
    std::printf("  [ ok ] %s\n", what);
  }
}

MusicxxExternPluginStringView view(const std::string &s) {
  MusicxxExternPluginStringView v{};
  v.data = s.data();
  v.size = s.size();
  return v;
}

/// 指针版视图 (C ABI 入参形态)
///
/// 注意: 槽位是滚动复用的静态数组, 因此**一次调用里的视图个数必须 ≤ 槽位数**
/// (C++ 不规定实参求值顺序, 槽位太少会让后写入的值覆盖先写入的视图)。
MusicxxExternPluginStringView *viewP(const std::string &s) {
  static MusicxxExternPluginStringView slots[8];
  static int next = 0;
  auto &v = slots[next++ % 8];
  v.data = s.data();
  v.size = s.size();
  return &v;
}

MusicxxExternPluginStringView viewC(const char *s) {
  MusicxxExternPluginStringView v{};
  v.data = s;
  v.size = std::strlen(s);
  return v;
}

MusicxxExternPluginStringView *viewCP(const char *s) {
  static MusicxxExternPluginStringView slots[8];
  static int next = 0;
  auto &v = slots[next++ % 8];
  v.data = s;
  v.size = std::strlen(s);
  return &v;
}

void freeStr(MusicxxExternPluginString &s) {
  musicxx_extern_plugin_string_free(&s);
}

std::string take(MusicxxExternPluginString &s) {
  std::string out;
  if (s.data) {
    out.assign(s.data, static_cast<size_t>(s.size));
  }
  freeStr(s);
  return out;
}

/// 从扫描结果 JSON (扁平数组) 中取出指定插件 id 的片段 (到下一个条目的 "id"
/// 之前)
///
/// 用途: 断言某个插件的 `valid`/`supported` 字段 ——
/// 入口文件按平台解析后的结果是否正确 (清单按 Linux 写 entry, Windows/macOS
/// 由内核修正扩展名)。
std::string scanItemOf(const std::string &scanJson, const std::string &id) {
  const std::string key = "\"id\":\"" + id + "\"";
  const auto pos = scanJson.find(key);
  if (pos == std::string::npos) {
    return {};
  }
  const auto next = scanJson.find("\"id\":\"", pos + key.size());
  return scanJson.substr(pos, next == std::string::npos ? std::string::npos
                                                        : next - pos);
}

/// 取 JSON 字符串字段的裸值 (测试专用极简解析; 不支持转义)
std::string jsonStringField(const std::string &json, const std::string &key) {
  const std::string needle = "\"" + key + "\":\"";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) {
    return {};
  }
  const auto begin = pos + needle.size();
  const auto end = json.find('"', begin);
  if (end == std::string::npos) {
    return {};
  }
  return json.substr(begin, end - begin);
}

/// 取 JSON 整数字段的裸文本 (测试专用)
std::string jsonIntField(const std::string &json, const std::string &key) {
  const std::string needle = "\"" + key + "\":";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) {
    return {};
  }
  auto begin = pos + needle.size();
  auto end = begin;
  while (end < json.size() &&
         (std::isdigit(static_cast<unsigned char>(json[end])) ||
          json[end] == '-')) {
    ++end;
  }
  return json.substr(begin, end - begin);
}

} // namespace

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0); // debug: unbuffered stdout
  const std::string pluginDir = (argc > 1) ? argv[1] : "";
  std::printf("musicxx_extern_plugin native test (pluginDir=%s)\n",
              pluginDir.c_str());

  check(musicxx_extern_plugin_api_version() ==
            MUSICXX_EXTERN_PLUGIN_API_VERSION,
        "api_version 匹配");

  MusicxxExternPluginString log{};
  MusicxxExternPluginString version{};
  check(musicxx_extern_plugin_version(&version) == MUSICXX_EXTERN_PLUGIN_OK &&
            version.size > 0,
        "version 可读");
  freeStr(version);

  MusicxxExternPluginHostConfig cfg{};
  cfg.struct_size = sizeof(MusicxxExternPluginHostConfig);
  const std::string appV = "0.0.0-test";
  const std::string plat = "windows";
  const std::string lang = "zh-cn";
  cfg.app_version = view(appV);
  cfg.platform = view(plat);
  cfg.language = view(lang);
  cfg.user_plugin_dir = view(pluginDir);
  cfg.log_level = 2;
  cfg.flags = MUSICXX_EXTERN_PLUGIN_FLAG_DEBUG_OBSERVE_EVENTS;

  auto *host = musicxx_extern_plugin_host_create(&cfg, &log);
  if (!host) {
    std::printf("host_create failed: %s\n", take(log).c_str());
    return 2;
  }
  check(host != nullptr, "host_create");

  check(musicxx_extern_plugin_host_start(host, &log) ==
            MUSICXX_EXTERN_PLUGIN_OK,
        "host_start");

  // 扫描
  MusicxxExternPluginString scan{};
  const auto scanRc = musicxx_extern_plugin_plugin_scan(host, &scan, &log);
  const std::string scanJson = take(scan);
  check(scanRc == MUSICXX_EXTERN_PLUGIN_OK, "plugin_scan 成功");
  std::printf("  [info] scanJson=%s\n", scanJson.substr(0, 400).c_str());
  // 扫描应同时发现原生与 JS 示例插件 (M3: JS 插件零编译)
  check(scanJson.find("example_native") != std::string::npos,
        "扫描发现 example_native");
  check(scanJson.find("example_js") != std::string::npos,
        "扫描发现 example_js (JS 插件)");
  check(scanJson.find("\"kind\":\"js\"") != std::string::npos,
        "JS 插件的 kind 为 js");
  check(scanJson.find("本次运行未启用 JS 运行时") == std::string::npos,
        "JS 运行时可用 (未报未启用)");

  // 动态库插件的入口解析: 清单按 Linux 写 entry (`example_native.so`),
  // Windows/macOS 上由内核
  // 修正扩展名。扫描阶段的校验必须与加载阶段用同一套解析,
  // 否则会出现"扫描判缺库文件、 加载却能命中"的不一致
  // (本用例就是那次缺陷的回归防护)。
  {
    const std::string nativeItem = scanItemOf(scanJson, "example_native");
    check(!nativeItem.empty(), "扫描结果含 example_native 条目");
    check(nativeItem.find("\"supported\":true") != std::string::npos,
          "动态库插件按平台解析 entry 后仍为可用 (未被判库文件缺失)");
    check(nativeItem.find("库文件缺失") == std::string::npos,
          "动态库插件未被判为库文件缺失");
  }

  // 装载 (同步)
  MusicxxExternPluginString empty{};
  const auto loadRc = musicxx_extern_plugin_plugin_load_sync(
      host, viewCP("example_native"), viewCP(R"({"enabled":true})"), 10000,
      &log);
  if (loadRc != MUSICXX_EXTERN_PLUGIN_OK) {
    std::printf("  [info] load rc=%d log=%s\n", loadRc, take(log).c_str());
  }
  check(loadRc == MUSICXX_EXTERN_PLUGIN_OK, "plugin_load_sync 成功");

  // 钩子处理器数量
  int32_t count = 0;
  check(musicxx_extern_plugin_hook_count(
            host, viewCP("musicxx.player.beforePlaySong"), &count, &log) ==
                MUSICXX_EXTERN_PLUGIN_OK &&
            count == 1,
        "beforePlaySong 处理器数为 1");
  check(musicxx_extern_plugin_hook_count(
            host, viewCP("musicxx.player.completed"), &count, &log) ==
                MUSICXX_EXTERN_PLUGIN_OK &&
            count == 0,
        "未注册钩子的处理器数为 0");

  // 事件泵: 应能取到 host.ready / plugin.loaded / hook.changed
  {
    MusicxxExternPluginString events{};
    const auto rc = musicxx_extern_plugin_poll_events(host, 200, &events, &log);
    const std::string eventsJson = take(events);
    check(rc == MUSICXX_EXTERN_PLUGIN_OK ||
              rc == MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT,
          "poll_events 可调用");
    check(eventsJson.find("musicxx.plugin.loaded") != std::string::npos ||
              eventsJson.find("musicxx.host.ready") != std::string::npos,
          "事件包含 host.ready / plugin.loaded");
    // UI 项注册/更新会推送 musicxx.ui.changed; 通知走动作请求通道
    // (musicxx.ui.notify)
    check(eventsJson.find("musicxx.ui.changed") != std::string::npos,
          "UI 项变化推送 musicxx.ui.changed 事件");
    check(eventsJson.find("musicxx.ui.notify") != std::string::npos,
          "通知走动作请求通道 (musicxx.ui.notify)");
  }

  // decision 钩子: 广告曲目 → skip
  {
    MusicxxExternPluginString out{};
    const std::string payload =
        R"({"sid":"s1","song":{"name":"广告插播 - 测试","artist":"x"},"mode":"normal"})";
    const auto rc = musicxx_extern_plugin_hook_emit(
        host, viewCP("musicxx.player.beforePlaySong"), viewP(payload),
        MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 200, &out, &log);
    const std::string result = take(out);
    check(rc == MUSICXX_EXTERN_PLUGIN_OK, "hook_emit(sync) 返回成功");
    check(result.find("\"skip\"") != std::string::npos,
          "广告曲目被裁决为 skip");
  }

  // decision 钩子: 普通曲目 → 无裁决
  {
    MusicxxExternPluginString out{};
    const std::string payload =
        R"({"sid":"s2","song":{"name":"普通歌曲","artist":"y"}})";
    const auto rc = musicxx_extern_plugin_hook_emit(
        host, viewCP("musicxx.player.beforePlaySong"), viewP(payload),
        MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 200, &out, &log);
    const std::string result = take(out);
    check(rc == MUSICXX_EXTERN_PLUGIN_OK, "hook_emit(sync) 普通曲目成功");
    check(result.find("\"skip\"") == std::string::npos, "普通曲目无 skip 裁决");
  }

  // 观察型钩子 (异步派发)
  {
    MusicxxExternPluginString out{};
    const std::string payload = R"({"sid":"s3","song":{"name":"观察目标"}})";
    const auto rc = musicxx_extern_plugin_hook_emit(
        host, viewCP("musicxx.song.changed"), viewP(payload),
        MUSICXX_EXTERN_PLUGIN_HOOK_ASYNC, 0, &out, &log);
    freeStr(out);
    check(rc == MUSICXX_EXTERN_PLUGIN_OK, "hook_emit(async) 入队成功");
  }

  // 裁决型钩子的异步派发 (plan §5.3): 立即拿到 callId, 结果经事件回传
  {
    MusicxxExternPluginString out{};
    const std::string payload =
        R"({"sid":"s4","song":{"name":"广告插播 - 异步","artist":"z"}})";
    const auto rc = musicxx_extern_plugin_hook_emit(
        host, viewCP("musicxx.player.beforePlaySong"), viewP(payload),
        MUSICXX_EXTERN_PLUGIN_HOOK_ASYNC, 200, &out, &log);
    const std::string ack = take(out);
    check(rc == MUSICXX_EXTERN_PLUGIN_OK, "裁决型钩子异步派发返回成功");
    check(ack.find("\"async\":true") != std::string::npos,
          "异步派发确认 async=true");
    const std::string callId = jsonIntField(ack, "callId");
    check(!callId.empty(), "异步派发返回 callId");

    // 结果经 `musicxx.hook.decision.result` 事件回传 (等一会儿让宿主线程跑完)
    std::string eventsJson;
    bool seen = false;
    for (int i = 0; i < 50 && !seen; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds{20});
      MusicxxExternPluginString events{};
      const auto rcEv =
          musicxx_extern_plugin_poll_events(host, 500, &events, &log);
      const std::string batch = take(events);
      if (rcEv == MUSICXX_EXTERN_PLUGIN_OK) {
        eventsJson += batch;
      }
      seen =
          eventsJson.find("musicxx.hook.decision.result") != std::string::npos;
    }
    check(seen, "异步裁决结果经事件回传");
    const size_t at = eventsJson.find("musicxx.hook.decision.result");
    if (at != std::string::npos) {
      const std::string tail = eventsJson.substr(at);
      check(tail.find("\"callId\":" + callId) != std::string::npos,
            "结果事件带同一个 callId");
      check(tail.find("\"skip\"") != std::string::npos, "异步裁决结果为 skip");
      check(tail.find("\"hook\":\"musicxx.player.beforePlaySong\"") !=
                std::string::npos,
            "结果事件带钩子 id");
    }

    // 异步派发同样有等待预算: 派发过程不能卡住 Dart 侧 (emit 立即返回已由上面的
    // ack 证明)
    MusicxxExternPluginString out2{};
    const auto rc2 = musicxx_extern_plugin_hook_emit(
        host, viewCP("musicxx.player.beforePlaySong"),
        viewCP(R"({"sid":"s5","song":{"name":"普通歌曲"}})"),
        MUSICXX_EXTERN_PLUGIN_HOOK_ASYNC, 200, &out2, &log);
    const std::string ack2 = take(out2);
    check(rc2 == MUSICXX_EXTERN_PLUGIN_OK, "异步派发普通曲目成功");
    check(ack2.find("\"callId\"") != std::string::npos,
          "异步派发每次都分配 callId");
  }

  // 未知钩子: 处理器数为 0 且派发不报错
  {
    MusicxxExternPluginString out{};
    const auto rc = musicxx_extern_plugin_hook_emit(
        host, viewCP("musicxx.plugin.unknownHook"), viewCP("{}"),
        MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 50, &out, &log);
    const std::string result = take(out);
    check(rc == MUSICXX_EXTERN_PLUGIN_OK, "未知钩子派发不报错");
    check(result.find("\"handled\":false") != std::string::npos,
          "未知钩子无处理器");
  }

  // 状态镜像
  {
    const std::string key = "musicxx.state.song";
    const std::string val = R"({"sid":"s1","name":"镜像歌曲"})";
    check(musicxx_extern_plugin_state_update(host, viewP(key), viewP(val),
                                             &log) == MUSICXX_EXTERN_PLUGIN_OK,
          "state_update 成功");
    MusicxxExternPluginString listed{};
    check(musicxx_extern_plugin_plugin_list(host, &listed, &log) ==
              MUSICXX_EXTERN_PLUGIN_OK,
          "plugin_list 成功");
    const std::string listJson = take(listed);
    check(listJson.find("example_native") != std::string::npos,
          "plugin_list 含已装载插件");
  }

  // 统计与调试信息
  {
    MusicxxExternPluginString stats{};
    check(musicxx_extern_plugin_stats(host, nullptr, &stats, &log) ==
              MUSICXX_EXTERN_PLUGIN_OK,
          "stats 可读");
    const std::string statsJson = take(stats);
    check(statsJson.find("example_native") != std::string::npos,
          "stats 含插件条目");

    // 事件计数与速率 (plan §4.11): 插件发布的 plugin.<id>.* 事件归属到该插件
    // (example_native 的 start 事务里发布过一次 plugin.example_native.hello)
    const auto at = statsJson.find(R"("id":"example_native")");
    const std::string entry =
        (at == std::string::npos) ? std::string{} : statsJson.substr(at, 600);
    const std::string eventsText = jsonIntField(entry, "events");
    check(!eventsText.empty() && std::stoll(eventsText) >= 1,
          "插件发布的事件计入统计 (plugin.<id>.* 归属)");
    check(entry.find("\"eventsPerSec\":") != std::string::npos,
          "stats 含事件速率字段");
    check(entry.find("\"eventsAvgPerSec\":") != std::string::npos,
          "stats 含平均事件速率字段");

    // 钩子统计的熔断剩余时间 (管理页据此显示"还有多久恢复派发")
    MusicxxExternPluginString hookStats{};
    check(musicxx_extern_plugin_hook_stats(host, &hookStats, &log) ==
              MUSICXX_EXTERN_PLUGIN_OK,
          "hook_stats 可读");
    const std::string hookStatsJson = take(hookStats);
    check(hookStatsJson.find("\"paused\":false") != std::string::npos,
          "hook_stats 含派发暂停标记");
    check(hookStatsJson.find("\"pausedRemainMs\":0") != std::string::npos,
          "hook_stats 含熔断剩余时间 (未熔断为 0)");

    MusicxxExternPluginString info{};
    check(musicxx_extern_plugin_debug_info(host, &info, &log) ==
              MUSICXX_EXTERN_PLUGIN_OK,
          "debug_info 可读");
    freeStr(info);
  }

  // 插件能力调用 (Dart → 插件) + 线程模型 / 命名空间自检
  {
    // 先让宿主发布一个测试主题 (插件已订阅), 再读探针 —— 两者都投递到宿主线程,
    // 因此顺序确定 (先进先出), 探针一定看到已处理的事件
    MusicxxExternPluginString pubLog{};
    const auto pubRc = musicxx_extern_plugin_event_publish(
        host, viewCP("musicxx.test.ping"), viewCP(R"({"n":1})"), &pubLog);
    check(pubRc == MUSICXX_EXTERN_PLUGIN_OK, "event_publish 合法主题成功");
    freeStr(pubLog);

    MusicxxExternPluginString badPubLog{};
    const auto badPubRc = musicxx_extern_plugin_event_publish(
        host, viewCP("foo.bar"), viewCP("{}"), &badPubLog);
    check(badPubRc == MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION,
          "event_publish 非法主题被拒绝");
    freeStr(badPubLog);

    MusicxxExternPluginString out{};
    MusicxxExternPluginString callLog{};
    const auto rc = musicxx_extern_plugin_plugin_call(
        host, viewCP("example_native"), viewCP("plugin.example_native.probe"),
        viewCP("{}"), 3000, &out, &callLog);
    const std::string probe = take(out);
    const std::string callErr = take(callLog);
    check(rc == MUSICXX_EXTERN_PLUGIN_OK, "plugin_call(probe) 成功");
    std::printf("  [info] plugin_call rc=%d log=%s\n", rc, callErr.c_str());
    std::printf("  [info] probe=%s\n", probe.c_str());

    // ==================== 声明式 UI 扩展 (plan §5.6, 动态库插件)
    // ====================
    //
    // 插件只做声明 (类型 + JSON 内容), 宿主负责渲染;
    // 这里校验注册/更新/拒绝/快照/事件。
    {
      check(jsonIntField(probe, "uiHomeRc") == "0",
            "注册主页入口项成功 (rc=0)");
      check(jsonIntField(probe, "uiSongRc") == "0",
            "注册歌曲菜单项成功 (rc=0)");
      check(jsonIntField(probe, "uiForeignRc") == "-6",
            "他人命名空间的项被拒绝 (-6)");
      check(jsonIntField(probe, "uiBadTypeRc") == "-4",
            "未知 UI 项类型被拒绝 (-4)");
      check(jsonIntField(probe, "uiBadDataRc") == "-1",
            "缺少 title 的声明被拒绝 (-1)");
      check(jsonIntField(probe, "uiUpdateRc") == "0",
            "更新自己的项成功 (rc=0)");
      check(jsonIntField(probe, "uiNotifyRc") == "0",
            "通知受理成功 (fire-and-forget)");

      MusicxxExternPluginString snap{};
      const auto snapRc = musicxx_extern_plugin_ui_snapshot(host, &snap, &log);
      const std::string snapshot = take(snap);
      check(snapRc == MUSICXX_EXTERN_PLUGIN_OK, "ui_snapshot 成功");
      check(snapshot.find("plugin.example_native.card") != std::string::npos,
            "快照含主页入口项");
      check(snapshot.find("plugin.example_native.songInfo") !=
                std::string::npos,
            "快照含歌曲菜单项");
      check(snapshot.find("plugin.example_native.overlayText") !=
                    std::string::npos &&
                snapshot.find("\"kind\":\"text\"") != std::string::npos &&
                snapshot.find("\"kind\":\"progress\"") != std::string::npos,
            "快照含播放页附加信息块 (text/progress 两种内容)");
      check(snapshot.find("musicxx.ui.home.entry") != std::string::npos,
            "快照含官方 UI 项类型");
      check(snapshot.find("plugin") != std::string::npos,
            "快照项带所属插件 id");
      check(snapshot.find("示例插件") != std::string::npos,
            "更新后的声明内容生效");
      check(snapshot.find("plugin.other_plugin.card") == std::string::npos,
            "他人命名空间的项不在快照里");
      check(snapshot.find("musicxx.ui.unknown") == std::string::npos,
            "未知类型不在快照里");
      check(snapshot.find("nodata") == std::string::npos, "非法声明不在快照里");
    }

    // 单宿主线程模型: start 事务与能力处理器必须在同一条线程上执行 (plan
    // §2.3.1)
    const std::string startThread = jsonStringField(probe, "startThread");
    const std::string callThread = jsonStringField(probe, "callThread");
    check(!startThread.empty() && startThread == callThread,
          "插件代码全部在宿主线程执行");

    // 钩子 / 动作命名空间校验 (plan §3.5)
    check(jsonIntField(probe, "unknownHookRc") == "-4",
          "未知前缀钩子注册被拒绝");
    check(jsonIntField(probe, "foreignActionRc") == "-6",
          "他人命名空间动作被拒绝");
    check(jsonIntField(probe, "dupHookRc") == "0",
          "同一钩子覆盖式重复注册成功");
    // 事件命名空间与订阅 (plan §3.5 / §4.9)
    check(jsonIntField(probe, "badTopicSubscribeRc") == "-1",
          "非法事件主题订阅被拒绝");
    check(jsonIntField(probe, "ownPublishRc") == "0",
          "本插件命名空间事件可发布");
    check(jsonIntField(probe, "foreignPublishRc") != "0",
          "他人命名空间事件发布被拒绝");
    check(jsonIntField(probe, "pingEvents") == "1", "插件收到宿主发布的事件");
    check(jsonIntField(probe, "stateEvents") != "0",
          "状态镜像变化通知到订阅插件");
    // 状态镜像可被插件同步读到 (上一段推送过 musicxx.state.song)
    check(jsonIntField(probe, "stateLen") != "0", "插件可同步读状态镜像");
  }

  // 动作请求超时保护 (plan §4.8): 插件请求 → Dart 不回复 → 宿主到点终结并推
  // cancel 事件
  {
    MusicxxExternPluginString out{};
    const auto rc = musicxx_extern_plugin_plugin_call(
        host, viewCP("example_native"), viewCP("probe"),
        viewCP(R"({"requestAction":"musicxx.test.neverRespond"})"), 3000, &out,
        &log);
    freeStr(out);
    check(rc == MUSICXX_EXTERN_PLUGIN_OK, "probe 触发插件动作请求成功");

    // 动作超时下限 1s; 等它过期后取事件与探针
    std::this_thread::sleep_for(std::chrono::milliseconds{1600});

    MusicxxExternPluginString events{};
    const auto rcEv =
        musicxx_extern_plugin_poll_events(host, 500, &events, &log);
    const std::string eventsJson = take(events);
    check(rcEv == MUSICXX_EXTERN_PLUGIN_OK ||
              rcEv == MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT,
          "poll_events 可调用");
    check(eventsJson.find("musicxx.action.request") != std::string::npos,
          "Dart 侧收到动作请求事件");
    check(eventsJson.find("musicxx.action.cancel") != std::string::npos,
          "动作超时后收到取消事件");

    MusicxxExternPluginString probeOut{};
    const auto rcProbe = musicxx_extern_plugin_plugin_call(
        host, viewCP("example_native"), viewCP("probe"), viewCP("{}"), 3000,
        &probeOut, &log);
    const std::string probe2 = take(probeOut);
    check(rcProbe == MUSICXX_EXTERN_PLUGIN_OK, "超时后探针可读");
    // PLUGINXX_OPERATOR_CANCELLED == 1: 超时按取消终结,
    // 插件侧收到恰好一次完成通知
    check(jsonIntField(probe2, "lastActionStatus") == "1",
          "动作超时按取消终结并回调插件");
  }

  // 能力调用错误路径: 未加载插件 / 未声明能力
  {
    MusicxxExternPluginString out{};
    const auto rc = musicxx_extern_plugin_plugin_call(
        host, viewCP("no_such_plugin"), viewCP("probe"), viewCP("{}"), 1000,
        &out, &log);
    freeStr(out);
    check(rc == MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND,
          "plugin_call 未加载插件返回未找到");

    MusicxxExternPluginString out2{};
    const auto rc2 = musicxx_extern_plugin_plugin_call(
        host, viewCP("example_native"), viewCP("noSuchCapability"),
        viewCP("{}"), 1000, &out2, &log);
    freeStr(out2);
    check(rc2 == MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND,
          "plugin_call 未声明能力返回未找到");
  }

  // 装载失败安全降级 (plan §11.1): 不崩溃、明确失败、宿主继续可用
  {
    MusicxxExternPluginString loadLog{};
    const auto rc = musicxx_extern_plugin_plugin_load_sync(
        host, viewCP("definitely_missing_plugin"), viewCP("{}"), 3000,
        &loadLog);
    const std::string loadErr = take(loadLog);
    check(rc != MUSICXX_EXTERN_PLUGIN_OK, "装载不存在的插件返回失败");
    check(rc != MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT,
          "失败是明确失败而非挂住超时");
    check(!loadErr.empty(), "失败原因有可读文本");
    std::printf("  [info] load-fail rc=%d log=%s\n", rc, loadErr.c_str());

    MusicxxExternPluginString again{};
    const auto rc2 = musicxx_extern_plugin_plugin_load_sync(
        host, viewCP("definitely_missing_plugin"), viewCP("{}"), 3000, &again);
    take(again);
    check(rc2 != MUSICXX_EXTERN_PLUGIN_OK, "失败后不会静默重试成功");

    // 宿主仍可正常工作
    MusicxxExternPluginString listed{};
    check(musicxx_extern_plugin_plugin_list(host, &listed, &log) ==
                  MUSICXX_EXTERN_PLUGIN_OK &&
              take(listed).find("example_native") != std::string::npos,
          "装载失败后宿主仍可用");
  }

  // 禁用 → 钩子摘除, 启用 → 重新注册
  {
    check(musicxx_extern_plugin_plugin_disable(
              host, viewCP("example_native"), &log) == MUSICXX_EXTERN_PLUGIN_OK,
          "plugin_disable 成功");
    int32_t afterDisable = 0;
    musicxx_extern_plugin_hook_count(
        host, viewCP("musicxx.player.beforePlaySong"), &afterDisable, &log);
    check(afterDisable == 0, "禁用后钩子处理器被摘除");
  }

  // 卸载
  {
    check(musicxx_extern_plugin_plugin_unload(host, viewCP("example_native"),
                                              &log) == MUSICXX_EXTERN_PLUGIN_OK,
          "plugin_unload 成功");
    int32_t after = 0;
    musicxx_extern_plugin_hook_count(
        host, viewCP("musicxx.player.beforePlaySong"), &after, &log);
    check(after == 0, "卸载后无钩子残留");
    // 幂等: 重复卸载视为已达目标状态 (Dart 侧状态不同步/重复点击都会走到这里)
    check(musicxx_extern_plugin_plugin_unload(host, viewCP("example_native"),
                                              &log) == MUSICXX_EXTERN_PLUGIN_OK,
          "重复卸载幂等成功");
    // 卸载后能力调用应返回未找到 (注册已随实例摘除)
    MusicxxExternPluginString out{};
    const auto rc = musicxx_extern_plugin_plugin_call(
        host, viewCP("example_native"), viewCP("probe"), viewCP("{}"), 1000,
        &out, &log);
    freeStr(out);
    check(rc != MUSICXX_EXTERN_PLUGIN_OK, "卸载后能力调用失败");
  }

  // ==================== JS 插件 (plan §4.4 / §4.6, M3) ====================
  //
  // 同一套用例在 native/js 两条链路上跑: decision 裁决、observe
  // 通知、能力探针、 状态镜像读取、事件订阅、定时器、禁用/卸载摘除。
  {
    MusicxxExternPluginString loadLog{};
    const auto jsLoadRc = musicxx_extern_plugin_plugin_load_sync(
        host, viewCP("example_js"), viewCP(R"({"enabled":true})"), 15000,
        &loadLog);
    if (jsLoadRc != MUSICXX_EXTERN_PLUGIN_OK) {
      std::printf("  [info] js load rc=%d log=%s\n", jsLoadRc,
                  take(loadLog).c_str());
    } else {
      freeStr(loadLog);
    }
    check(jsLoadRc == MUSICXX_EXTERN_PLUGIN_OK, "JS 插件装载成功 (example_js)");

    int32_t jsHookCount = 0;
    check(musicxx_extern_plugin_hook_count(
              host, viewCP("musicxx.player.beforePlaySong"), &jsHookCount,
              &log) == MUSICXX_EXTERN_PLUGIN_OK &&
              jsHookCount == 1,
          "JS 钩子处理器已注册 (beforePlaySong = 1)");

    // decision 钩子: 广告曲目 → skip
    {
      MusicxxExternPluginString out{};
      const std::string payload =
          R"({"sid":"js1","song":{"name":"广告插播 - JS 测试","artist":"x"}})";
      const auto rc = musicxx_extern_plugin_hook_emit(
          host, viewCP("musicxx.player.beforePlaySong"), viewP(payload),
          MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 300, &out, &log);
      const std::string result = take(out);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "JS 钩子派发返回成功");
      check(result.find("\"skip\"") != std::string::npos,
            "JS 裁决: 广告曲目 skip");
    }

    // decision 钩子: 普通曲目 → 无裁决
    {
      MusicxxExternPluginString out{};
      const std::string payload =
          R"({"sid":"js2","song":{"name":"普通歌曲 JS"}})";
      const auto rc = musicxx_extern_plugin_hook_emit(
          host, viewCP("musicxx.player.beforePlaySong"), viewP(payload),
          MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 300, &out, &log);
      const std::string result = take(out);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "JS 钩子 (普通曲目) 派发成功");
      check(result.find("\"skip\"") == std::string::npos,
            "JS 裁决: 普通曲目无 skip");
    }

    // decision 钩子: 播放错误 → patch.tryNextSrc
    {
      MusicxxExternPluginString out{};
      const std::string payload =
          R"({"sid":"js3","srcKey":"k3","errorCode":-1})";
      const auto rc = musicxx_extern_plugin_hook_emit(
          host, viewCP("musicxx.player.error"), viewP(payload),
          MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 300, &out, &log);
      const std::string result = take(out);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "JS 错误钩子派发成功");
      check(result.find("tryNextSrc") != std::string::npos,
            "JS 裁决: 首个错误建议换源");
    }

    // observe 钩子 (异步): 切歌通知 + 脚本日志
    {
      MusicxxExternPluginString out{};
      const std::string payload =
          R"({"sid":"js4","song":{"name":"观察目标 JS"}})";
      const auto rc = musicxx_extern_plugin_hook_emit(
          host, viewCP("musicxx.song.changed"), viewP(payload),
          MUSICXX_EXTERN_PLUGIN_HOOK_ASYNC, 0, &out, &log);
      freeStr(out);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "JS 观察钩子入队成功");

      // 等 JS 线程执行完 (含脚本定时器 50ms)
      std::this_thread::sleep_for(std::chrono::milliseconds{500});
      MusicxxExternPluginString events{};
      const std::string eventsJson = [&] {
        musicxx_extern_plugin_poll_events(host, 500, &events, &log);
        return take(events);
      }();
      check(eventsJson.find("musicxx.plugin.log") != std::string::npos,
            "JS 日志经事件回传");
      check(eventsJson.find("切歌") != std::string::npos,
            "JS 观察钩子已执行 (切歌通知)");
    }

    // 能力探针 (Dart → JS): 自检信息 + 状态镜像同步读
    {
      MusicxxExternPluginString out{};
      const auto rc = musicxx_extern_plugin_plugin_call(
          host, viewCP("example_js"), viewCP("probe"), viewCP("{}"), 5000, &out,
          &log);
      const std::string probe = take(out);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "JS 能力调用成功 (probe)");
      check(jsonStringField(probe, "pluginId") == "example_js",
            "JS 能力读到插件 id");
      check(jsonIntField(probe, "hookCount") == "4", "JS 注册了 4 个钩子");
      check(jsonIntField(probe, "songChangedCount") == "1",
            "JS 观察钩子计数为 1 (钩子真的执行了)");
      check(jsonStringField(probe, "currentSongName") == "镜像歌曲",
            "JS 同步读状态镜像 (musicxx.state.song)");
      check(jsonStringField(probe, "hostPlatform") == "windows",
            "JS 读到宿主信息 (平台)");
      // JS 侧的声明式 UI 项 (顶层注册 → 宿主线程回放)
      // 3 项 = 主页入口 + 歌曲菜单 + 播放页附加信息块
      // (设置界面是插件自己的页面, 不是 UI 项)
      check(jsonIntField(probe, "uiEntries") == "3",
            "JS 注册了 3 个 UI 项 (脚本侧记账)");
      check(jsonIntField(probe, "selfStatsHooks") == "4",
            "JS 能读自己的统计 (stats.getSelf 的钩子计数)");
    }

    // JS 插件的 UI 项进入宿主快照 (与动态库插件同一注册表)
    {
      MusicxxExternPluginString snap{};
      const auto rc = musicxx_extern_plugin_ui_snapshot(host, &snap, &log);
      const std::string snapshot = take(snap);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "ui_snapshot 可读 (含 JS 项)");
      check(snapshot.find("plugin.example_js.card") != std::string::npos,
            "JS 插件的主页入口项进入快照");
      check(snapshot.find("plugin.example_js.songInfo") != std::string::npos,
            "JS 插件的菜单项进入快照");
      // 框架没有"插件设置页"类型: 设置界面改成插件自己的页面 (同名能力),
      // 不再注册 UI 项, 快照里也不该出现 settings.page
      check(snapshot.find("musicxx.ui.settings.page") == std::string::npos,
            "快照里没有设置页类型 (框架不管理插件设置入口)");
      check(snapshot.find("plugin.example_js.settings") == std::string::npos,
            "JS 插件不再注册设置页入口项");
      check(
          snapshot.find("plugin.example_js.overlayInfo") != std::string::npos &&
              snapshot.find("\"position\":\"player.top\"") != std::string::npos,
          "JS 插件的播放页附加信息块进入快照 (含 position)");
    }

    // 插件自己的设置界面: 就是插件页面的同名能力
    // (框架不管理设置入口、也不渲染设置控件, 页面内容全部由插件给)
    {
      MusicxxExternPluginString out{};
      const auto rc = musicxx_extern_plugin_plugin_call(
          host, viewCP("example_js"), viewCP("settings"),
          viewCP(R"({"view":"settings"})"), 5000, &out, &log);
      const std::string view = take(out);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK,
            "设置界面能力调用成功 (插件自绘页面)");
      check(view.find("\"blocks\"") != std::string::npos,
            "设置界面返回声明式块 (由插件给出结构)");
      check(view.find("config.json") != std::string::npos,
            "设置界面内容由插件给出 (说明自己的 config.json 用法)");
    }

    // ============ 跨插件能力调用 (plan §7.2 capability.call) ============
    //
    // JS 插件调用其它插件的能力: JS 目标同线程直接调用;
    // 原生目标投递到宿主线程执行, 脚本侧不等待 (返回
    // Promise)。这里用"最后一次结果"记账, 由 probe 能力回读。
    {
      // 原生示例插件在前面的用例里被卸载了, 这里临时再装一次作为被调用方
      MusicxxExternPluginString reloadLog{};
      const auto reloadRc = musicxx_extern_plugin_plugin_load_sync(
          host, viewCP("example_native"), viewCP("{}"), 8000, &reloadLog);
      check(reloadRc == MUSICXX_EXTERN_PLUGIN_OK,
            "临时装载动态库插件 (跨插件调用被调用方)");

      MusicxxExternPluginString out{};
      const auto callRc = musicxx_extern_plugin_plugin_call(
          host, viewCP("example_js"), viewCP("crossCall"),
          viewCP(R"({"target":"example_native","method":"probe"})"), 5000, &out,
          &log);
      const std::string callJson = take(out);
      check(callRc == MUSICXX_EXTERN_PLUGIN_OK, "触发跨插件调用 (JS → 原生)");
      check(jsonIntField(callJson, "accepted") == "1", "跨插件调用被受理");

      // 等回执回到 JS 线程 (宿主线程执行 + 投递回 JS 线程)
      std::this_thread::sleep_for(std::chrono::milliseconds{800});
      MusicxxExternPluginString probe3{};
      const auto probe3Rc = musicxx_extern_plugin_plugin_call(
          host, viewCP("example_js"), viewCP("probe"), viewCP("{}"), 3000,
          &probe3, &log);
      const std::string probe3Json = take(probe3);
      check(probe3Rc == MUSICXX_EXTERN_PLUGIN_OK, "回读跨插件调用结果");
      check(jsonIntField(probe3Json, "crossPending") == "0",
            "跨插件调用已结束 (无挂起)");
      check(jsonIntField(probe3Json, "crossOk") == "1",
            "跨插件调用成功 (JS → 原生能力)");
      check(jsonIntField(probe3Json, "crossKeys") != "0",
            "跨插件调用拿到结果内容");

      check(musicxx_extern_plugin_plugin_unload(host, viewCP("example_native"),
                                                &log) ==
                MUSICXX_EXTERN_PLUGIN_OK,
            "卸载临时装载的动态库插件");
    }

    // ============ 可选 JS 执行上限 (js_exec_guard, plan §4.10) ============
    //
    // 默认关闭 (决策 13: 宿主不主动打断脚本)。这里模拟用户在配置里显式开启,
    // 验证: 死循环脚本会被中断 (共享 JS 线程不会永久被占住),
    // 宿主与其它插件不受影响。
    {
      check(musicxx_extern_plugin_set_config(host,
                                             viewP(R"({"jsExecGuardMs":400})"),
                                             &log) == MUSICXX_EXTERN_PLUGIN_OK,
            "开启可选 JS 执行上限 (jsExecGuardMs)");
      MusicxxExternPluginString loadLog{};
      const auto spinRc = musicxx_extern_plugin_plugin_load_sync(
          host, viewCP("spin_js"), viewCP("{}"), 8000, &loadLog);
      check(spinRc == MUSICXX_EXTERN_PLUGIN_OK,
            "死循环夹具装载成功 (顶层不阻塞)");

      // 触发它的观察钩子 (异步派发: 宿主入队即返回, 不等脚本)
      MusicxxExternPluginString out{};
      const auto emitRc = musicxx_extern_plugin_hook_emit(
          host, viewCP("musicxx.player.completed"),
          viewP(R"({"sid":"spin","playedMs":1})"),
          MUSICXX_EXTERN_PLUGIN_HOOK_ASYNC, 0, &out, &log);
      freeStr(out);
      check(emitRc == MUSICXX_EXTERN_PLUGIN_OK, "触发死循环钩子 (入队即返回)");

      // 等执行上限生效 (400 ms) 并留出余量
      std::this_thread::sleep_for(std::chrono::milliseconds{1500});

      // 统计里有中断计数 (只观测不限制: 中断只影响本次调用, 不卸载插件)
      MusicxxExternPluginString stats{};
      const auto statsRc =
          musicxx_extern_plugin_stats(host, nullptr, &stats, &log);
      const std::string statsJson = take(stats);
      check(statsRc == MUSICXX_EXTERN_PLUGIN_OK, "stats 可读 (执行上限)");
      check(statsJson.find("spin_js") != std::string::npos,
            "统计含死循环夹具插件");
      check(statsJson.find("execGuardHits") != std::string::npos,
            "统计含执行上限中断计数");

      // 宿主与其它 JS 插件仍可用 (证明共享 JS 线程已恢复)
      MusicxxExternPluginString probe2{};
      const auto probe2Rc = musicxx_extern_plugin_plugin_call(
          host, viewCP("example_js"), viewCP("probe"), viewCP("{}"), 3000,
          &probe2, &log);
      take(probe2);
      check(probe2Rc == MUSICXX_EXTERN_PLUGIN_OK,
            "中断后共享 JS 线程仍可服务其它插件");

      // 卸载夹具 (脚本线程已恢复) 并恢复默认 (关闭执行上限)
      check(musicxx_extern_plugin_plugin_unload(
                host, viewCP("spin_js"), &log) == MUSICXX_EXTERN_PLUGIN_OK,
            "死循环夹具可卸载");
      check(musicxx_extern_plugin_set_config(host,
                                             viewP(R"({"jsExecGuardMs":0})"),
                                             &log) == MUSICXX_EXTERN_PLUGIN_OK,
            "关闭可选执行上限 (恢复默认)");
    }

    // 禁用 → 钩子摘除, 启用 → 重新注册 (脚本重跑)
    {
      check(musicxx_extern_plugin_plugin_disable(
                host, viewCP("example_js"), &log) == MUSICXX_EXTERN_PLUGIN_OK,
            "JS 插件禁用成功");
      int32_t afterDisable = 0;
      musicxx_extern_plugin_hook_count(
          host, viewCP("musicxx.player.beforePlaySong"), &afterDisable, &log);
      check(afterDisable == 0, "JS 插件禁用后钩子被摘除");

      check(musicxx_extern_plugin_plugin_enable(
                host, viewCP("example_js"), &log) == MUSICXX_EXTERN_PLUGIN_OK,
            "JS 插件重新启用成功");
      std::this_thread::sleep_for(std::chrono::milliseconds{300});
      int32_t afterEnable = 0;
      musicxx_extern_plugin_hook_count(
          host, viewCP("musicxx.player.beforePlaySong"), &afterEnable, &log);
      check(afterEnable == 1, "JS 插件启用后钩子重新注册");
    }

    // 调试信息含 JS 运行时状态
    {
      MusicxxExternPluginString debug{};
      const auto rc = musicxx_extern_plugin_debug_info(host, &debug, &log);
      const std::string debugJson = take(debug);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "debug_info 可读");
      check(debugJson.find("\"available\":true") != std::string::npos,
            "调试信息含 JS 运行时");
      check(debugJson.find("example_js") != std::string::npos,
            "调试信息含 JS 插件");
      // 共享 JS 线程的排队观测 (plan §4.11: 只观测不限制)
      check(debugJson.find("\"queueDepth\":") != std::string::npos,
            "调试信息含 JS 任务队列深度");
      check(debugJson.find("\"queueWaitMaxMs\":") != std::string::npos,
            "调试信息含 JS 排队等待时长");

      MusicxxExternPluginString stats{};
      check(musicxx_extern_plugin_stats(host, nullptr, &stats, &log) ==
                MUSICXX_EXTERN_PLUGIN_OK,
            "stats 可读 (含 JS 段)");
      const std::string statsJson = take(stats);
      check(statsJson.find("\"queueDepth\":") != std::string::npos,
            "stats 含 JS 任务队列深度");
      check(statsJson.find("\"queueWaitLastMs\":") != std::string::npos,
            "stats 含 JS 最近一次排队等待时长");
    }

    // 卸载 → 注册全部摘除
    {
      check(musicxx_extern_plugin_plugin_unload(
                host, viewCP("example_js"), &log) == MUSICXX_EXTERN_PLUGIN_OK,
            "JS 插件卸载成功");
      int32_t after = 0;
      musicxx_extern_plugin_hook_count(
          host, viewCP("musicxx.player.beforePlaySong"), &after, &log);
      check(after == 0, "JS 插件卸载后无钩子残留");
      MusicxxExternPluginString out{};
      const auto rc = musicxx_extern_plugin_plugin_call(
          host, viewCP("example_js"), viewCP("probe"), viewCP("{}"), 1000, &out,
          &log);
      freeStr(out);
      check(rc != MUSICXX_EXTERN_PLUGIN_OK, "JS 插件卸载后能力调用失败");
    }
  }

  // ==================== JavaScript 异步裁决 (裁决处理器返回 Promise)
  // ====================
  //
  // 语义 (plan §4.10 的等待预算 + §7.2 的 JS API):
  // - Promise 在宿主等待预算 (100 ms) 内结算 → 裁决照常生效;
  // - 超过预算 → 按"无裁决"继续: 不打断脚本、不计处理器失败;
  // 迟到的结算被丢弃并计数。
  {
    MusicxxExternPluginString loadLog{};
    const auto jsAsyncRc = musicxx_extern_plugin_plugin_load_sync(
        host, viewCP("async_js"), viewCP(R"({"enabled":true})"), 8000,
        &loadLog);
    check(jsAsyncRc == MUSICXX_EXTERN_PLUGIN_OK,
          "异步裁决夹具装载成功 (async_js)");
    if (jsAsyncRc != MUSICXX_EXTERN_PLUGIN_OK) {
      std::printf("  [info] async_js rc=%d log=%s\n", jsAsyncRc,
                  take(loadLog).c_str());
    } else {
      freeStr(loadLog);
    }

    int32_t seekHandlers = 0;
    musicxx_extern_plugin_hook_count(host, viewCP("musicxx.player.seek"),
                                     &seekHandlers, &log);
    check(seekHandlers == 1, "异步裁决处理器已注册 (player.seek = 1)");

    // 1) Promise 在预算内结算 (30 ms) → 裁决生效
    {
      MusicxxExternPluginString out{};
      const auto rc = musicxx_extern_plugin_hook_emit(
          host, viewCP("musicxx.player.seek"),
          viewP(R"({"sid":"async1","fromMs":1000,"toMs":2000})"),
          MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 300, &out, &log);
      const std::string result = take(out);
      std::printf("  [info] async seek result=%s\n", result.c_str());
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "异步裁决钩子派发成功");
      check(jsonIntField(result, "toMs") == "12345",
            "Promise 结算的 patch 生效 (toMs)");
    }

    // 2) Promise 超过等待预算 (500 ms) → 无裁决, 且不等满 500 ms
    {
      MusicxxExternPluginString out{};
      const auto t0 = std::chrono::steady_clock::now();
      const auto rc = musicxx_extern_plugin_hook_emit(
          host, viewCP("musicxx.player.volume"),
          viewP(R"({"sid":"async2","from":0.5,"to":0.9})"),
          MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 300, &out, &log);
      const std::string result = take(out);
      const auto elapsedMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0)
              .count();
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "超预算的异步裁决派发返回成功");
      check(result.find("\"result\"") == std::string::npos,
            "超预算的 Promise 不产生裁决");
      check(elapsedMs < 300, "宿主没有等到 Promise 结算 (按等待预算返回)");
    }

    // 3) 等迟到的那次结算落地 (500 ms) + 余量: 结果被丢弃、进程不崩
    std::this_thread::sleep_for(std::chrono::milliseconds{900});

    // 4) 超时不算失败 (不熔断): 再次派发仍会调用该处理器
    {
      MusicxxExternPluginString out{};
      const auto rc = musicxx_extern_plugin_hook_emit(
          host, viewCP("musicxx.player.volume"),
          viewP(R"({"sid":"async3","from":0.1,"to":0.2})"),
          MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 300, &out, &log);
      freeStr(out);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK,
            "迟到结算之后仍可派发 (处理器未被暂停)");
    }

    // 5) 统计: 预算内结算 1 次 / 超预算 2 次 (第 4 步又一次) / 迟到丢弃 1 次
    {
      MusicxxExternPluginString out{};
      const auto rc = musicxx_extern_plugin_plugin_call(
          host, viewCP("async_js"), viewCP("probe"), viewCP("{}"), 5000, &out,
          &log);
      const std::string probe = take(out);
      std::printf("  [info] async probe=%s\n", probe.c_str());
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "异步裁决夹具能力调用成功 (probe)");
      check(jsonIntField(probe, "asyncHookSettled") == "1",
            "统计: 预算内结算 1 次");
      check(jsonIntField(probe, "asyncHookTimeouts") == "2",
            "统计: 超预算 2 次");
      check(jsonIntField(probe, "asyncHookLateDrops") == "1",
            "统计: 迟到结算被丢弃 1 次");
      check(jsonIntField(probe, "runs") == "3",
            "处理器每次都被调用 (未被熔断或跳过)");
    }

    // 6) 钩子统计里没有"失败"(超时 ≠ 失败), 只有耗时记录
    {
      MusicxxExternPluginString stats{};
      const auto rc = musicxx_extern_plugin_hook_stats(host, &stats, &log);
      const std::string statsJson = take(stats);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "hook_stats 可读");
      const auto at = statsJson.find("musicxx.player.volume");
      const std::string entry =
          (at == std::string::npos)
              ? std::string{}
              : statsJson.substr(at, statsJson.find(']', at) - at);
      check(entry.find("\"failures\":0") != std::string::npos,
            "超预算的异步裁决不计处理器失败");
    }

    // 7) 卸载 → 无残留 (含定时器与等待条目)
    {
      check(musicxx_extern_plugin_plugin_unload(
                host, viewCP("async_js"), &log) == MUSICXX_EXTERN_PLUGIN_OK,
            "异步裁决夹具卸载成功");
      int32_t seekAfter = 0;
      int32_t volumeAfter = 0;
      musicxx_extern_plugin_hook_count(host, viewCP("musicxx.player.seek"),
                                       &seekAfter, &log);
      musicxx_extern_plugin_hook_count(host, viewCP("musicxx.player.volume"),
                                       &volumeAfter, &log);
      check(seekAfter == 0 && volumeAfter == 0, "异步裁决夹具卸载后无钩子残留");
    }
  }

  // 脚本错误安全降级: 非法脚本的插件不应装载成功, 宿主继续可用
  {
    MusicxxExternPluginString loadLog{};
    const auto rc = musicxx_extern_plugin_plugin_load_sync(
        host, viewCP("broken_js"), viewCP("{}"), 5000, &loadLog);
    const std::string errText = take(loadLog);
    check(rc != MUSICXX_EXTERN_PLUGIN_OK, "脚本错误的 JS 插件装载失败");
    check(rc != MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT,
          "脚本错误是明确失败而非超时");
    std::printf("  [info] broken_js rc=%d log=%s\n", rc, errText.c_str());
    MusicxxExternPluginString listed{};
    check(musicxx_extern_plugin_plugin_list(host, &listed, &log) ==
              MUSICXX_EXTERN_PLUGIN_OK,
          "脚本错误后宿主仍可用");
    freeStr(listed);
  }

  // 插件全部卸载后: UI 项应全部摘除 (无残留)
  {
    MusicxxExternPluginString snap{};
    const auto snapRc = musicxx_extern_plugin_ui_snapshot(host, &snap, &log);
    const std::string snapshot = take(snap);
    check(snapRc == MUSICXX_EXTERN_PLUGIN_OK, "ui_snapshot 可读 (全部卸载后)");
    check(snapshot.find("plugin.example_native.card") == std::string::npos,
          "动态库插件卸载后 UI 项无残留");
    check(snapshot.find("plugin.example_js.card") == std::string::npos,
          "JS 插件卸载后 UI 项无残留");
  }

  // ==================== 入口符号契约 (plan §4.1 P0 / §13.1 test_entry_symbols)
  // ====================
  //
  // 夹具 bad_entry_native 的库文件导出了 get_info/create/destroy, 但**没有**
  // start/stop。 契约要求 start/stop 成对存在 (create 只构造, start
  // 才是注册事务), 宿主必须在 "查找入口符号"阶段就拒绝装载: 明确失败
  // (不是超时)、有可读原因、不留注册残留。
  {
    MusicxxExternPluginString loadLog{};
    const auto rc = musicxx_extern_plugin_plugin_load_sync(
        host, viewCP("bad_entry_native"), viewCP("{}"), 5000, &loadLog);
    const std::string errText = take(loadLog);
    check(rc != MUSICXX_EXTERN_PLUGIN_OK, "缺 start/stop 入口的库被拒绝装载");
    check(rc != MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT,
          "拒绝是明确失败而非挂住超时");
    check(errText.find("入口符号") != std::string::npos,
          "失败原因指向缺失的入口符号");
    std::printf("  [info] bad-entry rc=%d log=%s\n", rc, errText.c_str());

    // 未装载 + 未注册任何钩子 (此时其它插件都已卸载)
    MusicxxExternPluginString listed{};
    const std::string listJson = [&] {
      musicxx_extern_plugin_plugin_list(host, &listed, &log);
      return take(listed);
    }();
    check(listJson.find("bad_entry_native") == std::string::npos,
          "缺入口的库不出现在已装载列表");
    int32_t badEntryHooks = -1;
    musicxx_extern_plugin_hook_count(host, viewCP("musicxx.song.changed"),
                                     &badEntryHooks, &log);
    check(badEntryHooks == 0, "缺入口的库没有注册任何钩子");
  }

  // ==================== 熔断与派发暂停 (plan §4.10 / §13.1)
  // ====================
  //
  // 夹具 fail_native 注册两个处理器:
  //   musicxx.song.changed     → 每次失败 (返回非 0)
  //   musicxx.player.completed → 每次成功
  // 期望: 连续 3 次失败后只暂停出问题的处理器 (推送 musicxx.plugin.error
  // 说明原因), 同一插件的另一个处理器照常工作, 插件本身保持装载
  // (熔断不等于卸载)。
  {
    MusicxxExternPluginString loadLog{};
    const auto loadRc = musicxx_extern_plugin_plugin_load_sync(
        host, viewCP("fail_native"), viewCP("{}"), 8000, &loadLog);
    if (loadRc != MUSICXX_EXTERN_PLUGIN_OK) {
      std::printf("  [info] fail_native load rc=%d log=%s\n", loadRc,
                  take(loadLog).c_str());
    } else {
      freeStr(loadLog);
    }
    check(loadRc == MUSICXX_EXTERN_PLUGIN_OK, "熔断夹具装载成功 (fail_native)");

    int32_t fixtureHooks = -1;
    musicxx_extern_plugin_hook_count(host, viewCP("musicxx.song.changed"),
                                     &fixtureHooks, &log);
    check(fixtureHooks == 1, "失败处理器的处理器数为 1 (其余插件已卸载)");

    // 连续 3 次派发: 每次都真的调用了处理器 (called=1); 第 3 次触发熔断
    for (int i = 1; i <= 3; ++i) {
      MusicxxExternPluginString out{};
      const auto rc = musicxx_extern_plugin_hook_emit(
          host, viewCP("musicxx.song.changed"),
          viewP(R"({"sid":"brk","song":{"name":"熔断目标"}})"),
          MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 200, &out, &log);
      const std::string result = take(out);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "失败处理器派发可调用");
      check(jsonIntField(result, "called") == "1",
            "失败处理器被调用 (每次计数 1)");
    }

    // 第 4 次: 处理器已在熔断期内 → 直接跳过 (不再调用, 也就不会再累加失败)
    {
      MusicxxExternPluginString out{};
      const auto rc = musicxx_extern_plugin_hook_emit(
          host, viewCP("musicxx.song.changed"),
          viewP(R"({"sid":"brk","song":{"name":"熔断目标"}})"),
          MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 200, &out, &log);
      const std::string result = take(out);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "熔断期内派发不报错");
      check(jsonIntField(result, "called") == "0",
            "熔断期内处理器被跳过 (called=0)");
    }

    // 处理器级明细 (失败次数 / 暂停标记) 来自钩子统计接口 `hook_stats`:
    // 宿主对"连续失败"的处理是**只暂停派发**, 不卸载插件 (plan §4.10)
    {
      MusicxxExternPluginString stats{};
      const auto statsRc = musicxx_extern_plugin_hook_stats(host, &stats, &log);
      const std::string statsJson = take(stats);
      check(statsRc == MUSICXX_EXTERN_PLUGIN_OK, "hook_stats 可读 (熔断)");
      check(statsJson.find("\"calls\":3") != std::string::npos,
            "失败处理器只被调用 3 次");
      check(statsJson.find("\"failures\":3") != std::string::npos,
            "失败次数累计为 3");
      check(statsJson.find("\"paused\":true") != std::string::npos,
            "熔断后处理器标记为暂停");
      check(statsJson.find("\"failures\":0") != std::string::npos,
            "同插件的正常处理器无失败记录");
    }

    // 聚合统计 (宿主/插件维度) 仍然可读: 记录观测数据, 不做任何惩罚
    {
      MusicxxExternPluginString stats{};
      const auto statsRc =
          musicxx_extern_plugin_stats(host, nullptr, &stats, &log);
      const std::string statsJson = take(stats);
      check(statsRc == MUSICXX_EXTERN_PLUGIN_OK, "stats 可读 (熔断)");
      check(statsJson.find("fail_native") != std::string::npos,
            "聚合统计含熔断夹具插件");
      check(statsJson.find("\"calls\":3") != std::string::npos,
            "聚合统计含钩子调用次数");
    }

    // 熔断事件 (说明原因, 便于用户/开发者定位)
    {
      MusicxxExternPluginString events{};
      const std::string eventsJson = [&] {
        musicxx_extern_plugin_poll_events(host, 500, &events, &log);
        return take(events);
      }();
      check(eventsJson.find("musicxx.plugin.error") != std::string::npos,
            "熔断推送 musicxx.plugin.error 事件");
      check(eventsJson.find("handler_failed") != std::string::npos,
            "熔断事件说明原因是处理器失败");
    }

    // 同一插件的另一个处理器不受影响 (熔断粒度 = 单个处理器)
    {
      MusicxxExternPluginString out{};
      const auto rc = musicxx_extern_plugin_hook_emit(
          host, viewCP("musicxx.player.completed"),
          viewP(R"({"sid":"brk","playedMs":1000})"),
          MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 200, &out, &log);
      const std::string result = take(out);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "同插件的另一个处理器派发成功");
      check(jsonIntField(result, "called") == "1", "另一个处理器不受熔断影响");
    }

    // 插件本身仍在装载状态 (熔断只暂停派发)
    {
      MusicxxExternPluginString probe{};
      const auto probeRc = musicxx_extern_plugin_plugin_call(
          host, viewCP("fail_native"), viewCP("plugin.fail_native.probe"),
          viewCP("{}"), 3000, &probe, &log);
      const std::string probeJson = take(probe);
      check(probeRc == MUSICXX_EXTERN_PLUGIN_OK,
            "熔断后插件仍可响应能力调用 (未被卸载)");
      check(jsonIntField(probeJson, "failingCalls") == "3",
            "失败处理器只被调用 3 次 (熔断期内未被调用)");
      check(jsonIntField(probeJson, "goodCalls") == "1",
            "正常处理器按预期被调用 1 次");
    }

    // 卸载后无残留 (熔断状态随处理器一起消失)
    {
      check(musicxx_extern_plugin_plugin_unload(
                host, viewCP("fail_native"), &log) == MUSICXX_EXTERN_PLUGIN_OK,
            "熔断夹具卸载成功");
      int32_t after = -1;
      musicxx_extern_plugin_hook_count(host, viewCP("musicxx.song.changed"),
                                       &after, &log);
      check(after == 0, "熔断夹具卸载后无钩子残留");
    }
  }

  check(musicxx_extern_plugin_host_stop(host, 5000, &log) ==
            MUSICXX_EXTERN_PLUGIN_OK,
        "host_stop");
  check(musicxx_extern_plugin_host_stop(host, 1000, &log) ==
            MUSICXX_EXTERN_PLUGIN_OK,
        "host_stop 幂等");
  musicxx_extern_plugin_host_destroy(host);

  std::printf("\nchecks=%d failed=%d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
