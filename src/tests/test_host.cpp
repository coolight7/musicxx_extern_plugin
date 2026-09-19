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

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("  [FAIL] %s\n", what);
    } else {
        std::printf("  [ ok ] %s\n", what);
    }
}

MusicxxExternPluginStringView view(const std::string& s) {
    MusicxxExternPluginStringView v{};
    v.data = s.data();
    v.size = s.size();
    return v;
}

/// 指针版视图 (C ABI 入参形态)
///
/// 注意: 槽位是滚动复用的静态数组, 因此**一次调用里的视图个数必须 ≤ 槽位数**
/// (C++ 不规定实参求值顺序, 槽位太少会让后写入的值覆盖先写入的视图)。
MusicxxExternPluginStringView* viewP(const std::string& s) {
    static MusicxxExternPluginStringView slots[8];
    static int                          next = 0;
    auto&                               v    = slots[next++ % 8];
    v.data                                   = s.data();
    v.size                                   = s.size();
    return &v;
}

MusicxxExternPluginStringView viewC(const char* s) {
    MusicxxExternPluginStringView v{};
    v.data = s;
    v.size = std::strlen(s);
    return v;
}

MusicxxExternPluginStringView* viewCP(const char* s) {
    static MusicxxExternPluginStringView slots[8];
    static int                          next = 0;
    auto&                               v    = slots[next++ % 8];
    v.data                                   = s;
    v.size                                   = std::strlen(s);
    return &v;
}

void freeStr(MusicxxExternPluginString& s) {
    musicxx_extern_plugin_string_free(&s);
}

std::string take(MusicxxExternPluginString& s) {
    std::string out;
    if (s.data) {
        out.assign(s.data, static_cast<size_t>(s.size));
    }
    freeStr(s);
    return out;
}

/// 取 JSON 字符串字段的裸值 (测试专用极简解析; 不支持转义)
std::string jsonStringField(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":\"";
    const auto        pos    = json.find(needle);
    if (pos == std::string::npos) {
        return {};
    }
    const auto begin = pos + needle.size();
    const auto end   = json.find('"', begin);
    if (end == std::string::npos) {
        return {};
    }
    return json.substr(begin, end - begin);
}

/// 取 JSON 整数字段的裸文本 (测试专用)
std::string jsonIntField(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":";
    const auto        pos    = json.find(needle);
    if (pos == std::string::npos) {
        return {};
    }
    auto begin = pos + needle.size();
    auto end   = begin;
    while (end < json.size()
           && (std::isdigit(static_cast<unsigned char>(json[end])) || json[end] == '-')) {
        ++end;
    }
    return json.substr(begin, end - begin);
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0); // debug: unbuffered stdout
    const std::string pluginDir = (argc > 1) ? argv[1] : "";
    std::printf("musicxx_extern_plugin native test (pluginDir=%s)\n", pluginDir.c_str());

    check(musicxx_extern_plugin_api_version() == MUSICXX_EXTERN_PLUGIN_API_VERSION, "api_version 匹配");

    MusicxxExternPluginString log{};
    MusicxxExternPluginString version{};
    check(
        musicxx_extern_plugin_version(&version) == MUSICXX_EXTERN_PLUGIN_OK && version.size > 0,
        "version 可读"
    );
    freeStr(version);

    MusicxxExternPluginHostConfig cfg{};
    cfg.struct_size        = sizeof(MusicxxExternPluginHostConfig);
    const std::string appV = "0.0.0-test";
    const std::string plat = "windows";
    const std::string lang = "zh-cn";
    cfg.app_version        = view(appV);
    cfg.platform           = view(plat);
    cfg.language           = view(lang);
    cfg.user_plugin_dir    = view(pluginDir);
    cfg.log_level          = 2;
    cfg.flags              = MUSICXX_EXTERN_PLUGIN_FLAG_DEBUG_OBSERVE_EVENTS;

    auto* host = musicxx_extern_plugin_host_create(&cfg, &log);
    if (!host) {
        std::printf("host_create failed: %s\n", take(log).c_str());
        return 2;
    }
    check(host != nullptr, "host_create");

    check(musicxx_extern_plugin_host_start(host, &log) == MUSICXX_EXTERN_PLUGIN_OK, "host_start");

    // 扫描
    MusicxxExternPluginString scan{};
    const auto scanRc = musicxx_extern_plugin_plugin_scan(host, &scan, &log);
    const std::string scanJson = take(scan);
    check(scanRc == MUSICXX_EXTERN_PLUGIN_OK, "plugin_scan 成功");
    std::printf("  [info] scanJson=%s\n", scanJson.substr(0, 400).c_str());
    // 扫描应同时发现原生与 JS 示例插件 (M3: JS 插件零编译)
    check(scanJson.find("example_native") != std::string::npos, "扫描发现 example_native");
    check(scanJson.find("example_js") != std::string::npos, "扫描发现 example_js (JS 插件)");
    check(scanJson.find("\"kind\":\"js\"") != std::string::npos, "JS 插件的 kind 为 js");
    check(scanJson.find("本次运行未启用 JS 运行时") == std::string::npos, "JS 运行时可用 (未报未启用)");

    // 装载 (同步)
    MusicxxExternPluginString empty{};
    const auto loadRc = musicxx_extern_plugin_plugin_load_sync(
        host,
        viewCP("example_native"),
        viewCP(R"({"enabled":true})"),
        10000,
        &log
    );
    if (loadRc != MUSICXX_EXTERN_PLUGIN_OK) {
        std::printf("  [info] load rc=%d log=%s\n", loadRc, take(log).c_str());
    }
    check(loadRc == MUSICXX_EXTERN_PLUGIN_OK, "plugin_load_sync 成功");

    // 钩子处理器数量
    int32_t count = 0;
    check(
        musicxx_extern_plugin_hook_count(
            host,
            viewCP("musicxx.player.beforePlaySong"),
            &count,
            &log
        )
            == MUSICXX_EXTERN_PLUGIN_OK
            && count == 1,
        "beforePlaySong 处理器数为 1"
    );
    check(
        musicxx_extern_plugin_hook_count(host, viewCP("musicxx.player.completed"), &count, &log)
                == MUSICXX_EXTERN_PLUGIN_OK
            && count == 0,
        "未注册钩子的处理器数为 0"
    );

    // 事件泵: 应能取到 host.ready / plugin.loaded / hook.changed
    {
        MusicxxExternPluginString events{};
        const auto rc = musicxx_extern_plugin_poll_events(host, 200, &events, &log);
        const std::string eventsJson = take(events);
        check(rc == MUSICXX_EXTERN_PLUGIN_OK || rc == MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT, "poll_events 可调用");
        check(
            eventsJson.find("musicxx.plugin.loaded") != std::string::npos
                || eventsJson.find("musicxx.host.ready") != std::string::npos,
            "事件包含 host.ready / plugin.loaded"
        );
    }

    // decision 钩子: 广告曲目 → skip
    {
        MusicxxExternPluginString out{};
        const std::string        payload
            = R"({"sid":"s1","song":{"name":"广告插播 - 测试","artist":"x"},"mode":"normal"})";
        const auto rc = musicxx_extern_plugin_hook_emit(
            host,
            viewCP("musicxx.player.beforePlaySong"),
            viewP(payload),
            MUSICXX_EXTERN_PLUGIN_HOOK_SYNC,
            200,
            &out,
            &log
        );
        const std::string result = take(out);
        check(rc == MUSICXX_EXTERN_PLUGIN_OK, "hook_emit(sync) 返回成功");
        check(result.find("\"skip\"") != std::string::npos, "广告曲目被裁决为 skip");
    }

    // decision 钩子: 普通曲目 → 无裁决
    {
        MusicxxExternPluginString out{};
        const std::string        payload = R"({"sid":"s2","song":{"name":"普通歌曲","artist":"y"}})";
        const auto               rc      = musicxx_extern_plugin_hook_emit(
            host,
            viewCP("musicxx.player.beforePlaySong"),
            viewP(payload),
            MUSICXX_EXTERN_PLUGIN_HOOK_SYNC,
            200,
            &out,
            &log
        );
        const std::string result = take(out);
        check(rc == MUSICXX_EXTERN_PLUGIN_OK, "hook_emit(sync) 普通曲目成功");
        check(result.find("\"skip\"") == std::string::npos, "普通曲目无 skip 裁决");
    }

    // 观察型钩子 (异步派发)
    {
        MusicxxExternPluginString out{};
        const std::string        payload = R"({"sid":"s3","song":{"name":"观察目标"}})";
        const auto               rc      = musicxx_extern_plugin_hook_emit(
            host,
            viewCP("musicxx.song.changed"),
            viewP(payload),
            MUSICXX_EXTERN_PLUGIN_HOOK_ASYNC,
            0,
            &out,
            &log
        );
        freeStr(out);
        check(rc == MUSICXX_EXTERN_PLUGIN_OK, "hook_emit(async) 入队成功");
    }

    // 未知钩子: 处理器数为 0 且派发不报错
    {
        MusicxxExternPluginString out{};
        const auto                rc = musicxx_extern_plugin_hook_emit(
            host,
            viewCP("musicxx.plugin.unknownHook"),
            viewCP("{}"),
            MUSICXX_EXTERN_PLUGIN_HOOK_SYNC,
            50,
            &out,
            &log
        );
        const std::string result = take(out);
        check(rc == MUSICXX_EXTERN_PLUGIN_OK, "未知钩子派发不报错");
        check(result.find("\"handled\":false") != std::string::npos, "未知钩子无处理器");
    }

    // 状态镜像
    {
        const std::string key = "musicxx.state.song";
        const std::string val = R"({"sid":"s1","name":"镜像歌曲"})";
        check(
            musicxx_extern_plugin_state_update(host, viewP(key), viewP(val), &log)
                == MUSICXX_EXTERN_PLUGIN_OK,
            "state_update 成功"
        );
        MusicxxExternPluginString listed{};
        check(
            musicxx_extern_plugin_plugin_list(host, &listed, &log) == MUSICXX_EXTERN_PLUGIN_OK,
            "plugin_list 成功"
        );
        const std::string listJson = take(listed);
        check(listJson.find("example_native") != std::string::npos, "plugin_list 含已装载插件");
    }

    // 统计与调试信息
    {
        MusicxxExternPluginString stats{};
        check(
            musicxx_extern_plugin_stats(host, nullptr, &stats, &log) == MUSICXX_EXTERN_PLUGIN_OK,
            "stats 可读"
        );
        const std::string statsJson = take(stats);
        check(statsJson.find("example_native") != std::string::npos, "stats 含插件条目");

        MusicxxExternPluginString info{};
        check(
            musicxx_extern_plugin_debug_info(host, &info, &log) == MUSICXX_EXTERN_PLUGIN_OK,
            "debug_info 可读"
        );
        freeStr(info);
    }

    // 插件能力调用 (Dart → 插件) + 线程模型 / 命名空间自检
    {
        // 先让宿主发布一个测试主题 (插件已订阅), 再读探针 —— 两者都投递到宿主线程,
        // 因此顺序确定 (先进先出), 探针一定看到已处理的事件
        MusicxxExternPluginString pubLog{};
        const auto pubRc = musicxx_extern_plugin_event_publish(
            host,
            viewCP("musicxx.test.ping"),
            viewCP(R"({"n":1})"),
            &pubLog
        );
        check(pubRc == MUSICXX_EXTERN_PLUGIN_OK, "event_publish 合法主题成功");
        freeStr(pubLog);

        MusicxxExternPluginString badPubLog{};
        const auto badPubRc = musicxx_extern_plugin_event_publish(
            host,
            viewCP("foo.bar"),
            viewCP("{}"),
            &badPubLog
        );
        check(badPubRc == MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION, "event_publish 非法主题被拒绝");
        freeStr(badPubLog);

        MusicxxExternPluginString out{};
        MusicxxExternPluginString callLog{};
        const auto rc = musicxx_extern_plugin_plugin_call(
            host,
            viewCP("example_native"),
            viewCP("plugin.example_native.probe"),
            viewCP("{}"),
            3000,
            &out,
            &callLog
        );
        const std::string probe   = take(out);
        const std::string callErr = take(callLog);
        check(rc == MUSICXX_EXTERN_PLUGIN_OK, "plugin_call(probe) 成功");
        std::printf("  [info] plugin_call rc=%d log=%s\n", rc, callErr.c_str());
        std::printf("  [info] probe=%s\n", probe.c_str());

        // 单宿主线程模型: start 事务与能力处理器必须在同一条线程上执行 (plan §2.3.1)
        const std::string startThread = jsonStringField(probe, "startThread");
        const std::string callThread  = jsonStringField(probe, "callThread");
        check(!startThread.empty() && startThread == callThread, "插件代码全部在宿主线程执行");

        // 钩子 / 动作命名空间校验 (plan §3.5)
        check(jsonIntField(probe, "unknownHookRc") == "-4", "未知前缀钩子注册被拒绝");
        check(jsonIntField(probe, "foreignActionRc") == "-6", "他人命名空间动作被拒绝");
        check(jsonIntField(probe, "dupHookRc") == "0", "同一钩子覆盖式重复注册成功");
        // 事件命名空间与订阅 (plan §3.5 / §4.9)
        check(jsonIntField(probe, "badTopicSubscribeRc") == "-1", "非法事件主题订阅被拒绝");
        check(jsonIntField(probe, "ownPublishRc") == "0", "本插件命名空间事件可发布");
        check(jsonIntField(probe, "foreignPublishRc") != "0", "他人命名空间事件发布被拒绝");
        check(jsonIntField(probe, "pingEvents") == "1", "插件收到宿主发布的事件");
        check(jsonIntField(probe, "stateEvents") != "0", "状态镜像变化通知到订阅插件");
        // 状态镜像可被插件同步读到 (上一段推送过 musicxx.state.song)
        check(jsonIntField(probe, "stateLen") != "0", "插件可同步读状态镜像");
    }

    // 动作请求超时保护 (plan §4.8): 插件请求 → Dart 不回复 → 宿主到点终结并推 cancel 事件
    {
        MusicxxExternPluginString out{};
        const auto                rc = musicxx_extern_plugin_plugin_call(
            host,
            viewCP("example_native"),
            viewCP("probe"),
            viewCP(R"({"requestAction":"musicxx.test.neverRespond"})"),
            3000,
            &out,
            &log
        );
        freeStr(out);
        check(rc == MUSICXX_EXTERN_PLUGIN_OK, "probe 触发插件动作请求成功");

        // 动作超时下限 1s; 等它过期后取事件与探针
        std::this_thread::sleep_for(std::chrono::milliseconds{1600});

        MusicxxExternPluginString events{};
        const auto rcEv = musicxx_extern_plugin_poll_events(host, 500, &events, &log);
        const std::string eventsJson = take(events);
        check(rcEv == MUSICXX_EXTERN_PLUGIN_OK || rcEv == MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT, "poll_events 可调用");
        check(
            eventsJson.find("musicxx.action.request") != std::string::npos,
            "Dart 侧收到动作请求事件"
        );
        check(
            eventsJson.find("musicxx.action.cancel") != std::string::npos,
            "动作超时后收到取消事件"
        );

        MusicxxExternPluginString probeOut{};
        const auto                rcProbe = musicxx_extern_plugin_plugin_call(
            host,
            viewCP("example_native"),
            viewCP("probe"),
            viewCP("{}"),
            3000,
            &probeOut,
            &log
        );
        const std::string probe2 = take(probeOut);
        check(rcProbe == MUSICXX_EXTERN_PLUGIN_OK, "超时后探针可读");
        // PLUGINXX_OPERATOR_CANCELLED == 1: 超时按取消终结, 插件侧收到恰好一次完成通知
        check(jsonIntField(probe2, "lastActionStatus") == "1", "动作超时按取消终结并回调插件");
    }

    // 能力调用错误路径: 未加载插件 / 未声明能力
    {
        MusicxxExternPluginString out{};
        const auto rc = musicxx_extern_plugin_plugin_call(
            host,
            viewCP("no_such_plugin"),
            viewCP("probe"),
            viewCP("{}"),
            1000,
            &out,
            &log
        );
        freeStr(out);
        check(rc == MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND, "plugin_call 未加载插件返回未找到");

        MusicxxExternPluginString out2{};
        const auto rc2 = musicxx_extern_plugin_plugin_call(
            host,
            viewCP("example_native"),
            viewCP("noSuchCapability"),
            viewCP("{}"),
            1000,
            &out2,
            &log
        );
        freeStr(out2);
        check(rc2 == MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND, "plugin_call 未声明能力返回未找到");
    }

    // 装载失败安全降级 (plan §11.1): 不崩溃、明确失败、宿主继续可用
    {
        MusicxxExternPluginString loadLog{};
        const auto                rc = musicxx_extern_plugin_plugin_load_sync(
            host,
            viewCP("definitely_missing_plugin"),
            viewCP("{}"),
            3000,
            &loadLog
        );
        const std::string loadErr = take(loadLog);
        check(rc != MUSICXX_EXTERN_PLUGIN_OK, "装载不存在的插件返回失败");
        check(rc != MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT, "失败是明确失败而非挂住超时");
        check(!loadErr.empty(), "失败原因有可读文本");
        std::printf("  [info] load-fail rc=%d log=%s\n", rc, loadErr.c_str());

        MusicxxExternPluginString again{};
        const auto                rc2 = musicxx_extern_plugin_plugin_load_sync(
            host,
            viewCP("definitely_missing_plugin"),
            viewCP("{}"),
            3000,
            &again
        );
        take(again);
        check(rc2 != MUSICXX_EXTERN_PLUGIN_OK, "失败后不会静默重试成功");

        // 宿主仍可正常工作
        MusicxxExternPluginString listed{};
        check(
            musicxx_extern_plugin_plugin_list(host, &listed, &log) == MUSICXX_EXTERN_PLUGIN_OK
                && take(listed).find("example_native") != std::string::npos,
            "装载失败后宿主仍可用"
        );
    }

    // 禁用 → 钩子摘除, 启用 → 重新注册
    {
        check(
            musicxx_extern_plugin_plugin_disable(host, viewCP("example_native"), &log)
                == MUSICXX_EXTERN_PLUGIN_OK,
            "plugin_disable 成功"
        );
        int32_t afterDisable = 0;
        musicxx_extern_plugin_hook_count(host, viewCP("musicxx.player.beforePlaySong"), &afterDisable, &log);
        check(afterDisable == 0, "禁用后钩子处理器被摘除");
    }

    // 卸载
    {
        check(
            musicxx_extern_plugin_plugin_unload(host, viewCP("example_native"), &log)
                == MUSICXX_EXTERN_PLUGIN_OK,
            "plugin_unload 成功"
        );
        int32_t after = 0;
        musicxx_extern_plugin_hook_count(host, viewCP("musicxx.player.beforePlaySong"), &after, &log);
        check(after == 0, "卸载后无钩子残留");
        // 幂等: 重复卸载视为已达目标状态 (Dart 侧状态不同步/重复点击都会走到这里)
        check(
            musicxx_extern_plugin_plugin_unload(host, viewCP("example_native"), &log)
                == MUSICXX_EXTERN_PLUGIN_OK,
            "重复卸载幂等成功"
        );
        // 卸载后能力调用应返回未找到 (注册已随实例摘除)
        MusicxxExternPluginString out{};
        const auto               rc = musicxx_extern_plugin_plugin_call(
            host,
            viewCP("example_native"),
            viewCP("probe"),
            viewCP("{}"),
            1000,
            &out,
            &log
        );
        freeStr(out);
        check(rc != MUSICXX_EXTERN_PLUGIN_OK, "卸载后能力调用失败");
    }

    // ==================== JS 插件 (plan §4.4 / §4.6, M3) ====================
    //
    // 同一套用例在 native/js 两条链路上跑: decision 裁决、observe 通知、能力探针、
    // 状态镜像读取、事件订阅、定时器、禁用/卸载摘除。
    {
        MusicxxExternPluginString loadLog{};
        const auto                jsLoadRc = musicxx_extern_plugin_plugin_load_sync(
            host,
            viewCP("example_js"),
            viewCP(R"({"enabled":true})"),
            15000,
            &loadLog
        );
        if (jsLoadRc != MUSICXX_EXTERN_PLUGIN_OK) {
            std::printf("  [info] js load rc=%d log=%s\n", jsLoadRc, take(loadLog).c_str());
        } else {
            freeStr(loadLog);
        }
        check(jsLoadRc == MUSICXX_EXTERN_PLUGIN_OK, "JS 插件装载成功 (example_js)");

        int32_t jsHookCount = 0;
        check(
            musicxx_extern_plugin_hook_count(
                host,
                viewCP("musicxx.player.beforePlaySong"),
                &jsHookCount,
                &log
            )
                == MUSICXX_EXTERN_PLUGIN_OK
                && jsHookCount == 1,
            "JS 钩子处理器已注册 (beforePlaySong = 1)"
        );

        // decision 钩子: 广告曲目 → skip
        {
            MusicxxExternPluginString out{};
            const std::string        payload
                = R"({"sid":"js1","song":{"name":"广告插播 - JS 测试","artist":"x"}})";
            const auto rc = musicxx_extern_plugin_hook_emit(
                host,
                viewCP("musicxx.player.beforePlaySong"),
                viewP(payload),
                MUSICXX_EXTERN_PLUGIN_HOOK_SYNC,
                300,
                &out,
                &log
            );
            const std::string result = take(out);
            check(rc == MUSICXX_EXTERN_PLUGIN_OK, "JS 钩子派发返回成功");
            check(result.find("\"skip\"") != std::string::npos, "JS 裁决: 广告曲目 skip");
        }

        // decision 钩子: 普通曲目 → 无裁决
        {
            MusicxxExternPluginString out{};
            const std::string        payload = R"({"sid":"js2","song":{"name":"普通歌曲 JS"}})";
            const auto               rc      = musicxx_extern_plugin_hook_emit(
                host,
                viewCP("musicxx.player.beforePlaySong"),
                viewP(payload),
                MUSICXX_EXTERN_PLUGIN_HOOK_SYNC,
                300,
                &out,
                &log
            );
            const std::string result = take(out);
            check(rc == MUSICXX_EXTERN_PLUGIN_OK, "JS 钩子 (普通曲目) 派发成功");
            check(result.find("\"skip\"") == std::string::npos, "JS 裁决: 普通曲目无 skip");
        }

        // decision 钩子: 播放错误 → patch.tryNextSrc
        {
            MusicxxExternPluginString out{};
            const std::string        payload = R"({"sid":"js3","srcKey":"k3","errorCode":-1})";
            const auto               rc      = musicxx_extern_plugin_hook_emit(
                host,
                viewCP("musicxx.player.error"),
                viewP(payload),
                MUSICXX_EXTERN_PLUGIN_HOOK_SYNC,
                300,
                &out,
                &log
            );
            const std::string result = take(out);
            check(rc == MUSICXX_EXTERN_PLUGIN_OK, "JS 错误钩子派发成功");
            check(result.find("tryNextSrc") != std::string::npos, "JS 裁决: 首个错误建议换源");
        }

        // observe 钩子 (异步): 切歌通知 + 脚本日志
        {
            MusicxxExternPluginString out{};
            const std::string        payload = R"({"sid":"js4","song":{"name":"观察目标 JS"}})";
            const auto               rc      = musicxx_extern_plugin_hook_emit(
                host,
                viewCP("musicxx.song.changed"),
                viewP(payload),
                MUSICXX_EXTERN_PLUGIN_HOOK_ASYNC,
                0,
                &out,
                &log
            );
            freeStr(out);
            check(rc == MUSICXX_EXTERN_PLUGIN_OK, "JS 观察钩子入队成功");

            // 等 JS 线程执行完 (含脚本定时器 50ms)
            std::this_thread::sleep_for(std::chrono::milliseconds{500});
            MusicxxExternPluginString events{};
            const std::string        eventsJson = [&] {
                musicxx_extern_plugin_poll_events(host, 500, &events, &log);
                return take(events);
            }();
            check(eventsJson.find("musicxx.plugin.log") != std::string::npos, "JS 日志经事件回传");
            check(eventsJson.find("切歌") != std::string::npos, "JS 观察钩子已执行 (切歌通知)");
        }

        // 能力探针 (Dart → JS): 自检信息 + 状态镜像同步读
        {
            MusicxxExternPluginString out{};
            const auto rc = musicxx_extern_plugin_plugin_call(
                host,
                viewCP("example_js"),
                viewCP("probe"),
                viewCP("{}"),
                5000,
                &out,
                &log
            );
            const std::string probe = take(out);
            check(rc == MUSICXX_EXTERN_PLUGIN_OK, "JS 能力调用成功 (probe)");
            check(jsonStringField(probe, "pluginId") == "example_js", "JS 能力读到插件 id");
            check(jsonIntField(probe, "hookCount") == "3", "JS 注册了 3 个钩子");
            check(
                jsonIntField(probe, "songChangedCount") == "1",
                "JS 观察钩子计数为 1 (钩子真的执行了)"
            );
            check(
                jsonStringField(probe, "currentSongName") == "镜像歌曲",
                "JS 同步读状态镜像 (musicxx.state.song)"
            );
            check(
                jsonStringField(probe, "hostPlatform") == "windows",
                "JS 读到宿主信息 (平台)"
            );
        }

        // 禁用 → 钩子摘除, 启用 → 重新注册 (脚本重跑)
        {
            check(
                musicxx_extern_plugin_plugin_disable(host, viewCP("example_js"), &log)
                    == MUSICXX_EXTERN_PLUGIN_OK,
                "JS 插件禁用成功"
            );
            int32_t afterDisable = 0;
            musicxx_extern_plugin_hook_count(
                host,
                viewCP("musicxx.player.beforePlaySong"),
                &afterDisable,
                &log
            );
            check(afterDisable == 0, "JS 插件禁用后钩子被摘除");

            check(
                musicxx_extern_plugin_plugin_enable(host, viewCP("example_js"), &log)
                    == MUSICXX_EXTERN_PLUGIN_OK,
                "JS 插件重新启用成功"
            );
            std::this_thread::sleep_for(std::chrono::milliseconds{300});
            int32_t afterEnable = 0;
            musicxx_extern_plugin_hook_count(
                host,
                viewCP("musicxx.player.beforePlaySong"),
                &afterEnable,
                &log
            );
            check(afterEnable == 1, "JS 插件启用后钩子重新注册");
        }

        // 调试信息含 JS 运行时状态
        {
            MusicxxExternPluginString debug{};
            const auto rc = musicxx_extern_plugin_debug_info(host, &debug, &log);
            const std::string debugJson = take(debug);
            check(rc == MUSICXX_EXTERN_PLUGIN_OK, "debug_info 可读");
            check(debugJson.find("\"available\":true") != std::string::npos, "调试信息含 JS 运行时");
            check(debugJson.find("example_js") != std::string::npos, "调试信息含 JS 插件");
        }

        // 卸载 → 注册全部摘除
        {
            check(
                musicxx_extern_plugin_plugin_unload(host, viewCP("example_js"), &log)
                    == MUSICXX_EXTERN_PLUGIN_OK,
                "JS 插件卸载成功"
            );
            int32_t after = 0;
            musicxx_extern_plugin_hook_count(
                host,
                viewCP("musicxx.player.beforePlaySong"),
                &after,
                &log
            );
            check(after == 0, "JS 插件卸载后无钩子残留");
            MusicxxExternPluginString out{};
            const auto               rc = musicxx_extern_plugin_plugin_call(
                host,
                viewCP("example_js"),
                viewCP("probe"),
                viewCP("{}"),
                1000,
                &out,
                &log
            );
            freeStr(out);
            check(rc != MUSICXX_EXTERN_PLUGIN_OK, "JS 插件卸载后能力调用失败");
        }
    }

    // 脚本错误安全降级: 非法脚本的插件不应装载成功, 宿主继续可用
    {
        MusicxxExternPluginString loadLog{};
        const auto rc = musicxx_extern_plugin_plugin_load_sync(
            host,
            viewCP("broken_js"),
            viewCP("{}"),
            5000,
            &loadLog
        );
        const std::string errText = take(loadLog);
        check(rc != MUSICXX_EXTERN_PLUGIN_OK, "脚本错误的 JS 插件装载失败");
        check(rc != MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT, "脚本错误是明确失败而非超时");
        std::printf("  [info] broken_js rc=%d log=%s\n", rc, errText.c_str());
        MusicxxExternPluginString listed{};
        check(
            musicxx_extern_plugin_plugin_list(host, &listed, &log) == MUSICXX_EXTERN_PLUGIN_OK,
            "脚本错误后宿主仍可用"
        );
        freeStr(listed);
    }

    check(musicxx_extern_plugin_host_stop(host, 5000, &log) == MUSICXX_EXTERN_PLUGIN_OK, "host_stop");
    check(musicxx_extern_plugin_host_stop(host, 1000, &log) == MUSICXX_EXTERN_PLUGIN_OK, "host_stop 幂等");
    musicxx_extern_plugin_host_destroy(host);

    std::printf("\nchecks=%d failed=%d\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
