/// 示例动态库插件: 演示钩子注册、状态镜像读取、宿主动作请求、能力注册与日志
///
/// 行为:
/// 1. 注册 `musicxx.player.beforePlaySong` (decision): 歌曲名含"广告" → 返回
/// skip 裁决;
/// 2. 注册 `musicxx.song.changed` (observe): 读状态镜像里的歌曲名并写日志;
/// 3. 注册 `musicxx.player.error` (decision): 连续错误 2 次后建议换源;
/// 4. 注册能力 `plugin.example_native.probe`: 返回本实例的自检信息 (线程归属、
///    命名空间校验返回码、处理器调用次数), 供宿主原生测试读取。
/// 5. 注册能力 `plugin.example_native.card`: 返回声明式页面
/// (主页入口与播放页附加
///    信息块都打开
///    `ext://example_native/card`)。入口声明的页面必须有**同名能力**,
///    否则点开只会提示"未声明该能力"。
///
/// 规定: start 事务里只做注册, 不阻塞、不发起网络/大文件操作。

#include "musicxx/plugin/api/plugin_kit.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace {

/// 当前线程标识文本 (用于验证"插件代码只在宿主线程执行")
std::string threadIdText() {
  std::ostringstream oss;
  oss << std::this_thread::get_id();
  return oss.str();
}

/// 声明式页面里一行 "标题 (+说明) + 右侧状态" 的排版
///
/// 行放进**内容块** (`Block` = 宿主里的卡片底色与边距), 等于原来的列表条目;
/// 行本身由布局块组合出来 (Row / Expanded / Column / SizedBox / Text)。
/// 想让整块可点, 就在外面这一层的对象上加 `"action":{...}`。
std::string infoRowJson(const std::string &title, const std::string &subtitle,
                        const std::string &right) {
  std::ostringstream left;
  left << "{\"kind\":\"Text\",\"text\":\"" << title << "\"}";
  if (!subtitle.empty()) {
    left << ",{\"kind\":\"SizedBox\",\"height\":6}"
            ",{\"kind\":\"Text\",\"style\":\"cross\",\"text\":\""
         << subtitle << "\"}";
  }
  std::ostringstream row;
  row << "{\"kind\":\"Row\",\"children\":[{\"kind\":\"Expanded\",\"child\":"
         "{\"kind\":\"Column\",\"children\":["
      << left.str() << "]}}";
  if (!right.empty()) {
    row << ",{\"kind\":\"SizedBox\",\"width\":30}"
           ",{\"kind\":\"Text\",\"style\":\"cross\",\"text\":\""
        << right << "\"}";
  }
  row << "]}";
  // 边距是设计像素 (50/50/20 = 左右留白 + 行间距), 由宿主换算
  return "{\"kind\":\"Block\",\"inContent\":true,\"margin\":{\"left\":50,"
         "\"right\":50,\"bottom\":20},\"child\":" +
         row.str() + "}";
}

/// 能力调用的空完成通知 (示例里只做探测, 不等待完成)
void PLUGINXX_CALL probeActionDone(void *, int32_t,
                                   const PluginxxStringView *) {}

/// 插件实例上下文 (每个实例一份; 不得使用可变全局状态 —— 多实例铁律)
struct ExampleCtx : public musicxx::plugin::PluginBase {
  /// 连续播放错误计数 (仅示例)
  int32_t consecutiveErrors = 0;

  /// start 事务所在线程 (与钩子/能力处理器比对: 必须相同 = 单宿主线程模型)
  std::string startThread;

  /// 自检结果 (命名空间校验返回码)
  int32_t unknownHookRc = 0; ///< 注册未知前缀钩子: 期望 -4 (未知钩子)
  int32_t foreignActionRc = 0; ///< 发起他人命名空间动作: 期望 -6 (权限拒绝)
  int32_t dupHookRc = 0; ///< 覆盖式重复注册: 期望 0

  /// 声明式 UI 扩展自检
  int32_t uiHomeRc = -100; ///< 注册主页入口项: 期望 0
  int32_t uiSongRc = -100; ///< 注册歌曲菜单项: 期望 0
  int32_t uiForeignRc = -100; ///< 注册他人命名空间的项: 期望 -6 (权限拒绝)
  int32_t uiBadTypeRc = -100; ///< 注册未知类型: 期望 -4 (未知类型)
  int32_t uiBadDataRc = -100; ///< 声明式内容缺少 title: 期望 -1 (参数非法)
  int32_t uiUpdateRc = -100; ///< 更新自己的项: 期望 0
  int32_t uiNotifyRc = -100; ///< 通知 (fire-and-forget 动作): 期望 0
  int32_t uiBackgroundRc = -100; ///< 注册播放页背景样式: 期望 0

  /// 事件订阅自检
  int32_t badTopicSubscribeRc = 0; ///< 订阅非法主题: 期望 -1 (宿主拒绝)
  int32_t stateEvents = 0;         ///< musicxx.state.changed 收到次数
  int32_t pingEvents = 0;          ///< musicxx.test.ping 收到次数
  int32_t ownPublishRc = 0; ///< 发布本插件命名空间事件: 期望 0
  int32_t foreignPublishRc = 0; ///< 发布他人命名空间事件: 期望非 0 (宿主拒绝)

  /// 动作请求自检
  int32_t lastActionStatus = -100; ///< 最近一次动作请求终态 (-100 = 从未发起)
  int32_t lastActionPayloadLen = 0;
  int64_t lastActionRequestId = 0;

  /// 动作请求完成回调 (宿主线程调用)
  static void PLUGINXX_CALL onProbeActionDone(
      void *ud, int32_t status, const PluginxxStringView *payload) noexcept {
    auto *self = static_cast<ExampleCtx *>(ud);
    if (!self) {
      return;
    }
    self->lastActionStatus = status;
    self->lastActionPayloadLen =
        (payload && payload->data) ? static_cast<int32_t>(payload->size) : 0;
  }

  int32_t onStart() {
    startThread = threadIdText();

    // 1) 跳过"广告"曲目 (决定型钩子: 返回 {"action":"skip"})
    hook(MUSICXX_PLUGIN_HOOK_PLAYER_BEFORE_PLAY_SONG, 0,
         [this](std::string_view input, std::string &out) -> int32_t {
           // 载荷: {"sid":"...","song":{"name":"...","artist":"..."},...}
           if (input.find("\xe5\xb9\xbf\xe5\x91\x8a") !=
               std::string_view::npos) {
             out = R"({"action":"skip","reason":"example_native: 广告曲目"})";
             log.info("example_native: 命中广告曲目, 请求跳过");
           }
           return 0;
         });

    // 2) 观察切歌 (只读状态镜像 + 日志)
    observe(MUSICXX_PLUGIN_HOOK_SONG_CHANGED,
            [this](std::string_view input, std::string &out) -> int32_t {
              (void)out;
              (void)input;
              const std::string song = stateJson("musicxx.state.song");
              log.info("example_native: 歌曲变化事件; 状态镜像长度=" +
                       std::to_string(song.size()));
              return 0;
            });

    // 3) 播放错误: 连续 2 次错误后建议换源 (不改写宿主既有错误策略)
    hook(MUSICXX_PLUGIN_HOOK_PLAYER_ERROR, 0,
         [this](std::string_view input, std::string &out) -> int32_t {
           (void)input;
           ++consecutiveErrors;
           if (consecutiveErrors >= 2) {
             out = R"({"action":"continue","patch":{"tryNextSrc":true}})";
           }
           return 0;
         });

    // 4) 命名空间自检: 未知前缀的钩子必须被宿主拒绝
    unknownHookRc =
        hook("foo.bar.baz", 0,
             [](std::string_view, std::string &) -> int32_t { return 0; });
    // 同一 (实例, 钩子, owner_tag) 覆盖式注册: 再注册一次应成功
    dupHookRc =
        observe(MUSICXX_PLUGIN_HOOK_SONG_CHANGED,
                [](std::string_view, std::string &) -> int32_t { return 0; });
    // 他人命名空间的动作用请求必须被拒绝 (此处只探测返回码, 不会真的发出)
    {
      PluginxxOperatorNotify notify{&probeActionDone, nullptr};
      foreignActionRc =
          requestAction("plugin.other_plugin.x", "{}", &notify, 1000);
    }

    // 5) 事件订阅 (宿主事件总线): 状态镜像变化 + 宿主发布的测试主题
    badTopicSubscribeRc = subscribeTopic("foo.bar", [](std::string_view) {
      // 非法主题: 宿主应在订阅时就拒绝 (返回 -1), 该处理器不会被调用
    });
    subscribeTopic("musicxx.state.changed", [this](std::string_view data) {
      ++stateEvents;
      (void)data;
    });
    subscribeTopic("musicxx.test.ping", [this](std::string_view data) {
      ++pingEvents;
      (void)data;
    });
    // 事件发布命名空间自检: 自己命名空间可发, 他人命名空间被拒绝
    ownPublishRc = publishOwnEvent("plugin.example_native.hello",
                                   "{\"from\":\"example_native\"}");
    foreignPublishRc = publishOwnEvent("plugin.other_plugin.hello",
                                       "{\"from\":\"example_native\"}");

    // 6) 声明式 UI 扩展: 注册主页入口与歌曲菜单项 + 通知
    //    注意: UI 项只声明"长什么样、点了做什么", 渲染由宿主 (Dart 侧) 负责。
    uiHomeRc = uiRegister(
        "card", MUSICXX_PLUGIN_UI_TYPE_HOME_ENTRY,
        R"({"title":"示例插件","subtitle":"example_native 提供的入口","icon":"addition",
                 "action":{"kind":"route","route":"ext://example_native/card"}})",
        100);
    uiSongRc = uiRegister("songInfo", MUSICXX_PLUGIN_UI_TYPE_SONG_ACTION,
                          R"({"title":"示例插件：查看歌曲信息",
                 "action":{"kind":"capability","name":"probe","args":{"ui":"1"}}})",
                          900);
    uiForeignRc = uiRegister("plugin.other_plugin.card",
                             MUSICXX_PLUGIN_UI_TYPE_HOME_ENTRY,
                             R"({"title":"冒充他人"})", 0);
    uiBadTypeRc =
        uiRegister("bad", "musicxx.ui.unknown", R"({"title":"未知类型"})", 0);
    uiBadDataRc = uiRegister("nodata", MUSICXX_PLUGIN_UI_TYPE_HOME_ENTRY,
                             R"({"subtitle":"缺少 title"})", 0);
    uiUpdateRc = uiUpdate(
        "card",
        R"({"title":"示例插件","subtitle":"example_native 提供的入口","icon":"addition",
                "action":{"kind":"route","route":"ext://example_native/card"}})");
    uiNotifyRc = uiNotify(R"({"text":"example_native 已加载","kind":"info"})");

    // 7) 播放页背景样式 (musicxx.ui.playing.background):
    //    插件把一个预编译的 shader bundle 注册成一种"播放页背景样式",
    //    用户在『设置 → 播放页面背景』里选中后才生效; 未选中时零成本。
    //    这里注册一个晶格化（随机点最近邻切块）的动态背景, 配色来自宿主的 4 个绘制色。
    uiBackgroundRc = uiRegister(
        "playingBg", MUSICXX_PLUGIN_UI_TYPE_PLAYING_BACKGROUND,
        R"({"title":"原生示例晶格背景","depict":"跟随封面配色的晶格化动态背景",
            "shader":{"bundle":"shader/bg.shaderbundle"},
            "colors":{"source":"background"},"speed":4,"maxFps":16,
            "foregroundStyle":"mask"})",
        20);

    // 8) 能力: 供宿主/测试查询本实例自检信息 (能力名遵循 plugin.<id>.<名>)
    capability(
        *this, "plugin.example_native.probe",
        [this](std::string_view, std::string_view args) -> std::string {
          // args 里带 requestAction 时顺带发起一次动作请求
          // (用于验证宿主超时保护)
          if (args.find("requestAction") != std::string_view::npos) {
            PluginxxOperatorNotify notify{&ExampleCtx::onProbeActionDone, this};
            lastActionStatus = -2; ///< 发起中
            requestAction("musicxx.test.neverRespond", "{}", &notify, 1000,
                          &lastActionRequestId);
            log.info("example_native: 已发起动作请求 musicxx.test.neverRespond "
                     "(等待宿主超时)");
          }
          std::ostringstream oss;
          oss << "{\"startThread\":\"" << startThread << "\""
              << ",\"callThread\":\"" << threadIdText() << "\""
              << ",\"unknownHookRc\":" << unknownHookRc
              << ",\"dupHookRc\":" << dupHookRc
              << ",\"foreignActionRc\":" << foreignActionRc
              << ",\"badTopicSubscribeRc\":" << badTopicSubscribeRc
              << ",\"ownPublishRc\":" << ownPublishRc
              << ",\"foreignPublishRc\":" << foreignPublishRc
              << ",\"stateEvents\":" << stateEvents
              << ",\"pingEvents\":" << pingEvents
              << ",\"lastActionStatus\":" << lastActionStatus
              << ",\"lastActionPayloadLen\":" << lastActionPayloadLen
              << ",\"consecutiveErrors\":" << consecutiveErrors
              << ",\"uiHomeRc\":" << uiHomeRc << ",\"uiSongRc\":" << uiSongRc
              << ",\"uiForeignRc\":" << uiForeignRc
              << ",\"uiBadTypeRc\":" << uiBadTypeRc
              << ",\"uiBadDataRc\":" << uiBadDataRc
              << ",\"uiUpdateRc\":" << uiUpdateRc
              << ",\"uiNotifyRc\":" << uiNotifyRc
              << ",\"uiBackgroundRc\":" << uiBackgroundRc
              << ",\"stateLen\":" << stateJson("musicxx.state.song").size()
              << "}";
          return oss.str();
        });

    // 8) 能力: 声明式页面 `ext://example_native/card`
    //    主页入口与播放页附加信息块都指向这个页面:
    //    宿主打开页面时调用**同名能力**, 插件返回视图描述 (文本 / 分隔线 /
    //    布局块 / 按钮块), 渲染由宿主完成。 页面里的按钮可以调用本插件能力
    //    (返回 {view:...} 时直接刷新当前页), 也可以执行官方动作
    //    (这里演示发一条站内提示)。
    //
    //    块类型名是首字母大写的驼峰 (Row / Column / Expanded / SizedBox /
    //    Padding / Text ...), 宿主解析时忽略大小写; 宽高与内边距按设计像素
    //    交给宿主换算 (XXSizedBox / XXEdgeInsets)。
    capability(
        *this, "plugin.example_native.card",
        [this](std::string_view, std::string_view) -> std::string {
          const int32_t uiOk = (uiHomeRc == 0 ? 1 : 0) +
                               (uiSongRc == 0 ? 1 : 0) +
                               (uiBackgroundRc == 0 ? 1 : 0);
          std::ostringstream oss;
          oss << "{\"view\":{\"title\":\"示例插件\""
              << ",\"subtitle\":\"页面内容来自能力 "
                 "plugin.example_native.card\""
              << ",\"blocks\":["
              << "{\"kind\":\"Text\",\"style\":\"cross\",\"text\":"
                 "\"这个页面演示"
                 "插件的声明式页面: 插件只返回块描述 (文本 / 布局 / 按钮), "
                 "不写界面"
                 "代码。\"}"
              << ",{\"kind\":\"Divider\"}"
              << ","
              << infoRowJson("start 事务线程",
                             "钩子与能力处理器都在这一条线程上执行", startThread)
              << ","
              << infoRowJson("当前调用线程", "与 start 事务线程相同 = 单宿主线程",
                             threadIdText())
              << ","
              << infoRowJson("连续播放错误", "累计 2 次后建议换源",
                             std::to_string(consecutiveErrors))
              << ","
              << infoRowJson("状态镜像变化事件", "",
                             std::to_string(stateEvents))
              << ","
              << infoRowJson("musicxx.test.ping 事件", "",
                             std::to_string(pingEvents))
              << ","
              << infoRowJson("已注册的 UI 项", "主页入口 / 歌曲菜单",
                             std::to_string(uiOk))
              << ","
              << infoRowJson("状态镜像 musicxx.state.song",
                             "宿主推送的最近一份歌曲快照 (字节)",
                             std::to_string(stateJson("musicxx.state.song")
                                                .size()))
              << ",{\"kind\":\"Button\",\"title\":\"刷新本页\","
                 "\"style\":\"primary\",\"action\":{\"kind\":\"capability\","
                 "\"name\":\"card\"}}"
              << ",{\"kind\":\"Button\",\"title\":\"发送一条通知\","
                 "\"action\":{\"kind\":\"action\",\"name\":\"musicxx.ui."
                 "notify\","
                 "\"args\":{\"text\":\"来自 example_native 的通知\"}}}"
              << "]}}";
          return oss.str();
        });

    return 0;
  }

  /// 发布本插件命名空间的事件 (经内核 events 表; 主题归属由宿主校验)
  int32_t publishOwnEvent(const char *topic, const char *payload) {
    if (!iface.events || !iface.events->publish) {
      return -1;
    }
    PluginxxStringView topicView = pluginxxView(topic);
    PluginxxStringView dataView = pluginxxView(payload);
    return iface.events->publish(host, &topicView, &dataView);
  }

  int32_t onStop() {
    // 撤销自管资源 (示例没有线程/定时器; 钩子/能力注册由宿主在 detach
    // 时兜底摘除)
    consecutiveErrors = 0;
    return 0;
  }
};

/// start 事务: 只做注册 (禁止阻塞)
///
/// 入口约定 (musicxx 插件 SDK): 返回 0 = 事务成功, 非 0 = 失败; SDK
/// 负责按内核契约 调用完成通知 (见 `musicxx/plugin/api/plugin_kit.h` 的
/// `runLifecycleEntry`)。
int32_t exampleStart(ExampleCtx &ctx) { return ctx.onStart(); }

/// stop 事务: 撤销自管资源 (可重复调用)
int32_t exampleStop(ExampleCtx &ctx) { return ctx.onStop(); }

} // namespace

MUSICXX_PLUGIN_EXPORT(ExampleCtx, "example_native", "1.0.0",
                      "musicxx 外部插件示例 (钩子/状态镜像/日志)", exampleStart,
                      exampleStop)
