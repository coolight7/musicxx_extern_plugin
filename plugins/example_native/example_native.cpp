/// 示例动态库插件: 演示能力注册、快照频道读取、主程序动作请求、界面内容与日志
///
/// 行为:
/// 1. 注册 `musicxx.player.beforePlaySong` (decision): 歌曲名含"广告" → 返回 skip 裁决;
/// 2. 订阅 `musicxx.song.changed` 广播: 收到推送后读快照频道并写日志;
/// 3. 注册 `musicxx.player.error` (decision): 连续错误 2 次后建议换源;
/// 4. 注册能力 `plugin.example_native.probe`: 返回本实例的自检信息 (线程归属、
///    命名空间校验返回码、处理器调用次数), 供原生测试读取;
/// 5. 把页面 `ext://example_native/card` 注册成 `musicxx.ui.page` 上的**内容**;
/// 6. 在包装点 `musicxx.lyric.load` 上登记**前链**与**后链**样本 (只在载荷带
///    `"example_native_demo":true` 时改值)。
///
/// 写法: 接口表调用一律 `co_await` (异步操作契约), 注册事务写成协程;
/// 处理器可以同步返回, 也可以写成 `asio::awaitable` 在等待期间让出框架线程。

#include "musicxx/plugin/api/plugin_kit.h"

#include <asio/awaitable.hpp>

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

namespace {

/// 当前线程标识文本 (用于验证"插件代码只在插件框架执行线程执行")
std::string threadIdText() {
  std::ostringstream oss;
  oss << std::this_thread::get_id();
  return oss.str();
}

/// 页面里的行：用 musicxx 扩展 kit 拼装（`listRow` = 卡片行：标题 + 说明 + 右侧状态）
std::string infoRowJson(const std::string &title, const std::string &subtitle,
                        const std::string &right) {
  namespace kit = musicxx::ui::kit;
  pluginxx::ui::Json params = pluginxx::ui::Json::object({
      {"title", title},
  });
  if (!subtitle.empty()) {
    params["subtitle"] = subtitle;
  }
  if (!right.empty()) {
    params["trailing"] = right;
  }
  return pluginxx::ui::dumpItem(kit::listRow(params)).dump();
}

/// 一个动作参数（kit 的 `action` 参数：字符串短写 = dispatch，或完整对象）
pluginxx::ui::Json dispatchAction(const std::string &name) {
  return pluginxx::ui::Json::object({
      {"kind", "dispatch"},
      {"name", name},
  });
}

/// 一个官方动作参数（`kind: command`，只允许 `musicxx.*`）
pluginxx::ui::Json commandAction(const std::string &name,
                                 pluginxx::ui::Json args) {
  return pluginxx::ui::Json::object({
      {"kind", "command"},
      {"name", name},
      {"args", std::move(args)},
  });
}

/// 插件实例上下文 (每个实例一份; 不得使用可变全局状态 —— 多实例规则)
struct ExampleCtx;

/// 生成函数是静态回调, 需要拿到当前实例来做 push (`g_self` 在 start 里设置)
thread_local ExampleCtx *g_self = nullptr;

/// 表调用失败原因 → 自检用的错误码
///
/// 插件面向的 ABI 现在只交回原因文本 (`arg` / `namespace` / `not_declared` /
/// `ended` / `state` …), 示例把文本折回 ABI 的数值码 (见 `pluginxx/api/result.h`),
/// 让原生测试的断言保持可读。没列出的原因统一给 -1 (`ERR_ARG` 量级)。
int32_t codeOfFailure(std::string_view what) {
  if (what.find("namespace") != std::string_view::npos) {
    return -6;
  }
  if (what.find("not_declared") != std::string_view::npos) {
    return -200;
  }
  if (what.find("ended") != std::string_view::npos) {
    return -202;
  }
  if (what.find("no_value") != std::string_view::npos) {
    return -201;
  }
  if (what.find("not_found") != std::string_view::npos ||
      what.find("unknown") != std::string_view::npos) {
    return -4;
  }
  if (what.find("state") != std::string_view::npos) {
    return -2;
  }
  return -1;
}

/// 跑一次表调用并把结果折成错误码 (0 = 成功)
template <typename Fn>
asio::awaitable<int32_t> tryCall(Fn fn) {
  try {
    co_await fn();
    co_return 0;
  } catch (const std::exception &e) {
    co_return codeOfFailure(e.what());
  } catch (...) {
    co_return -1;
  }
}

struct ExampleCtx : public musicxx::plugin::PluginBase {
  /// 连续播放错误计数 (仅示例)
  int32_t consecutiveErrors = 0;

  /// create 事务所在线程 (与能力处理器比对: 必须相同 = 单执行线程模型)
  std::string startThread;

  /// 能力注册自检
  int32_t unknownCapabilityRc = 0; ///< 应用没声明的官方 id: 期望 -200 (not_declared)
  int32_t foreignActionRc = 0;     ///< 发起他人命名空间动作: 期望 -1 (被拒绝)
  int32_t dupImplementRc = 0;      ///< 覆盖式重复注册: 期望 0
  int32_t fixedImplRc = 0;         ///< 给固定实现的能力注册 impl: 期望非 0 (只开放前链 / 后链)
  int32_t featurePointDefineRc = 0; ///< 功能埋点声明: 主程序在时 0, 原生测试宿主下非 0

  /* ==================== 内容生成函数 ==================== */
  //
  // 生成函数是 C 调用约定 (签名见 plugin_api.h): 拿到 context 后经 notify 恰好一次
  // 交回 `{"json":…,"data":…}`。这里都在同一帧里完成, 需要等网络时可以先返回
  // 一个"加载中"的结构, 拿到结果再用 `uiContentPush` 补一份。

  static void PLUGINXX_CALL onGenerateRow(const PluginxxHost *host,
                                          const PluginxxStringView *ctxJson,
                                          void * /*userData*/,
                                          const PluginxxOperatorNotify *notify,
                                          PluginxxString * /*errorOut*/) {
    std::string ctx = ctxJson && ctxJson->data
                          ? std::string{ctxJson->data,
                                        static_cast<size_t>(ctxJson->size)}
                          : std::string{"{}"};
    // 生成函数里 push 另一份内容: 表调用是异步的, 从同步回调里发起就投递出去, 不等结果
    if (g_self != nullptr) {
      g_self->spawn([](ExampleCtx &self,
                       pluginxx::OpCtl &) -> asio::awaitable<void> {
        try {
          co_await self.uiContentPush(
              "pushed",
              R"({"json":{"blocks":[{"kind":"Text","text":"生成时推送的内容"}]}})");
        } catch (const std::exception &) {
          // 推送失败不影响这一份内容
        }
      });
    }
    pluginxx::ui::Json blocks = pluginxx::ui::Json::array();
    blocks.push_back(pluginxx::ui::Json::object({
        {"kind", "Text"},
        {"text", "行内容"},
    }));
    blocks.push_back(pluginxx::ui::Json::object({
        {"kind", "Text"},
        {"text", ctx},
    }));
    pluginxx::ui::Json doc = pluginxx::ui::Json::object({
        {"json", pluginxx::ui::Json::object({{"blocks", blocks}})},
    });
    const std::string text = doc.dump();
    const PluginxxStringView view{text.data(), text.size()};
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &view);
  }

  /// `pushed` 那份内容的生成函数 (真正的值由 [onGenerateRow] 里的 push 推过来)
  static void PLUGINXX_CALL onGeneratePushed(
      const PluginxxHost * /*host*/, const PluginxxStringView * /*ctx*/,
      void * /*userData*/, const PluginxxOperatorNotify *notify,
      PluginxxString * /*errorOut*/) {
    static constexpr std::string_view kJson{
        R"({"json":{"blocks":[{"kind":"Text","text":"等待推送…"}]}})"};
    const PluginxxStringView view{kJson.data(), kJson.size()};
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &view);
  }

  /// 播放页背景的内容（根上是块树；无依赖变量，每次生成给同一份）
  static void PLUGINXX_CALL onGenerateBackground(
      const PluginxxHost * /*host*/, const PluginxxStringView * /*ctx*/,
      void * /*userData*/, const PluginxxOperatorNotify *notify,
      PluginxxString * /*errorOut*/) {
    static constexpr std::string_view kJson{
        R"({"json":{"kind":"Expanded","child":{"kind":"musicxx.Shader","bundle":"shader/bg.shaderbundle",)"
        R"("args":{"uColor1":{"kind":"source","name":"musicxx.icon.themeMapping.0"},)"
        R"("uColor2":{"kind":"source","name":"musicxx.icon.themeMapping.1"},)"
        R"("uColor3":{"kind":"source","name":"musicxx.icon.themeMapping.2"},)"
        R"("uColor4":{"kind":"source","name":"musicxx.icon.themeMapping.3"},)"
        R"("uCoverBack":{"kind":"texture","source":"musicxx.cover","size":384,"blur":28},)"
        R"("uLevel":{"kind":"smooth","attackMs":20,"releaseMs":260,"of":{"kind":"source","name":"musicxx.spectrum.level"}},)"
        R"("uBands":{"kind":"smooth","attackMs":20,"releaseMs":260,"of":{"kind":"source","name":"musicxx.spectrum.bands.0"}},)"
        R"("uBands2":{"kind":"smooth","attackMs":20,"releaseMs":260,"of":{"kind":"source","name":"musicxx.spectrum.bands.2"}}},)"
        R"("speed":1,"maxFps":16}}})"};
    const PluginxxStringView view{kJson.data(), kJson.size()};
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &view);
  }

  /// 普通页面背景的内容（无依赖变量；适用页面在注册时给）
  static void PLUGINXX_CALL onGeneratePageBackground(
      const PluginxxHost * /*host*/, const PluginxxStringView * /*ctx*/,
      void * /*userData*/, const PluginxxOperatorNotify *notify,
      PluginxxString * /*errorOut*/) {
    static constexpr std::string_view kJson{
        R"({"json":{"kind":"Expanded","child":{"kind":"musicxx.Shader","bundle":"shader/bg.shaderbundle","speed":1,"maxFps":8}}})"};
    const PluginxxStringView view{kJson.data(), kJson.size()};
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &view);
  }

  /// 说明页的内容（根上就是文档 `{title, subtitle, blocks}`）
  static void PLUGINXX_CALL onGenerateCard(const PluginxxHost * /*host*/,
                                           const PluginxxStringView * /*ctx*/,
                                           void * /*userData*/,
                                           const PluginxxOperatorNotify *notify,
                                           PluginxxString * /*errorOut*/) {
    const std::string doc =
        g_self != nullptr ? g_self->cardPageDoc() : std::string{"{\"blocks\":[]}"};
    const std::string content = std::string{"{\"json\":"} + doc + "}";
    const PluginxxStringView view{content.data(), content.size()};
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &view);
  }

  /* ==================== 自检字段 ==================== */

  int32_t uiHomeRc = -100;           ///< 注册主页入口声明项: 期望 0
  int32_t uiSongRc = -100;           ///< 注册歌曲菜单项: 期望 0
  int32_t uiForeignRc = -100;        ///< 注册他人命名空间的项: 期望 -6
  int32_t uiBadTypeRc = -100;        ///< 注册未知类型: 期望 -4
  int32_t uiBadDataRc = -100;        ///< 声明式内容缺少 title: 期望 -1
  int32_t uiNotifyRc = -100;         ///< 通知: 期望 0
  int32_t uiBackgroundRc = -100;     ///< 注册播放页背景样式: 期望 0
  int32_t uiPageBackgroundRc = -100; ///< 注册普通页面背景: 期望 0
  int32_t uiPageBackgroundBadRoutesRc = -100; ///< routes 写法不合法: 期望 -1

  int32_t uiContentRc = -100;
  int32_t uiContentDupSlotRc = -100;
  int32_t uiContentOverwriteRc = -100;
  int32_t uiContentBadSlotRc = -100;
  int32_t uiContentBadDepsRc = -100;
  int32_t uiContentUnregisterRc = -100;
  int32_t uiContentPushedRc = -100;
  int32_t uiPageRc = -100;
  int32_t uiSlotDefineRc = -100;
  int32_t uiSlotDefineAppRc = -100;
  int32_t uiSlotDefineBadKindRc = -100;
  int32_t uiSlotUpdateRc = -100;
  int32_t uiSlotUndefineRc = -100;

  int32_t badTopicSubscribeRc = 0; ///< 订阅非法主题: 期望 -1
  int32_t stateEvents = 0;
  int32_t pingEvents = 0;
  int32_t ownPublishRc = 0;
  int32_t foreignPublishRc = 0;

  int32_t streamEndSubRc = -100;
  int32_t streamEndValueRc = -100;
  int32_t streamEndRc = -100;
  int32_t streamEndPublishAfterRc = 0;
  int32_t streamEndValues = 0;
  int32_t streamEndEnds = 0;
  bool streamEndReasonOk = false;

  int32_t lastActionStatus = -100;
  int32_t lastActionPayloadLen = 0;

  bool netTableAvailable = false;
  int32_t netInfoRc = -100;

  std::string featurePointSampleId;
  int32_t featurePointSampleRc = -100;
  std::string featurePointUnprovidedId;

  /// 动作请求完成回调由协程自己读结果, 这里只留空实现 (不再用回调形态)
  static void PLUGINXX_CALL onProbeActionDone(
      void *, int32_t, const PluginxxStringView *) noexcept {}

  /* ==================== create 事务 ==================== */

  asio::awaitable<void> onCreate() {
    startThread = threadIdText();

    // 1) 跳过"广告"曲目 (决定型能力: 返回 {"action":"skip"})
    co_await implement(MUSICXX_CAPABILITY_PLAYER_BEFORE_PLAY_SONG, 0,
                       [this](std::string_view input, std::string &out) -> int32_t {
                         if (input.find("\xe5\xb9\xbf\xe5\x91\x8a") !=
                             std::string_view::npos) {
                           out = R"({"action":"skip","reason":"example_native: 广告曲目"})";
                           log.info("example_native: 命中广告曲目, 请求跳过");
                         }
                         return 0;
                       });

    // 2) 观察切歌: 切歌是广播频道 (`musicxx.song.changed`), 订阅后收推送
    co_await subscribeBroadcast(MUSICXX_BROADCAST_SONG_CHANGED,
                                [this](std::string_view) {
                                  log.info("example_native: 收到歌曲变化广播");
                                });

    // 3) 播放错误: 连续 2 次错误后建议换源 (不改写主程序既有错误策略)
    co_await implement(MUSICXX_CAPABILITY_PLAYER_ERROR, 0,
                       [this](std::string_view, std::string &out) -> int32_t {
                         ++consecutiveErrors;
                         if (consecutiveErrors >= 2) {
                           out = R"({"action":"continue","patch":{"tryNextSrc":true}})";
                         }
                         return 0;
                       });

    // 4) 命名空间自检: 官方 `musicxx.*` 归应用, 应用没声明的 id 不能注册
    unknownCapabilityRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await implement("musicxx.no.such.capability", 0,
                         [](std::string_view, std::string &) -> int32_t {
                           return 0;
                         });
    });
    // 固定实现的能力只开放前链 / 后链: 登记 impl 应被拒, 登记前链应成功
    fixedImplRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await implement(MUSICXX_CAPABILITY_PLAYER_VOLUME, 0,
                         [](std::string_view, std::string &) -> int32_t {
                           return 0;
                         });
    });
    dupImplementRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await implementBefore(MUSICXX_CAPABILITY_PLAYER_VOLUME, 0,
                               [](std::string_view, std::string &) -> int32_t {
                                 return 0;
                               });
    });

    // 4.0) 发起他人命名空间的动作: 只要返回码 (不会真的发出)
    foreignActionRc = co_await tryCall([this]() -> asio::awaitable<void> {
      (void)co_await capabilityCallWithTimeout("plugin.other_plugin.x", "{}",
                                               1000);
    });

    // 4.1) 阶段示例: 同一个包装点 (`musicxx.lyric.load`) 上的前链与后链
    co_await implementBefore(
        MUSICXX_CAPABILITY_LYRIC_LOAD, 0,
        [](std::string_view input, std::string &out) -> int32_t {
          const std::string text{input};
          if (text.find("\"example_native_demo\":true") == std::string::npos) {
            return 0;
          }
          if (text.find("\"cancel\":true") != std::string::npos) {
            out = R"({"action":"cancel"})";
            return 0;
          }
          out = R"({"patch":{"limit":50}})";
          return 0;
        });
    co_await implementAfter(
        MUSICXX_CAPABILITY_LYRIC_LOAD, 0,
        [](std::string_view input, std::string &out) -> int32_t {
          const std::string text{input};
          if (text.find("\"example_native_demo\":true") == std::string::npos) {
            return 0;
          }
          if (text.find("\"drop\":true") != std::string::npos) {
            out = R"({"action":"drop"})";
            return 0;
          }
          out = R"({"value":{"items":3,"by":"example_native"}})";
          return 0;
        });

    // 5) 广播订阅: 快照频道 + 插件框架发布的测试主题
    badTopicSubscribeRc =
        co_await tryCall([this]() -> asio::awaitable<void> {
          co_await subscribeBroadcast("foo.bar", [](std::string_view) {});
        });
    co_await subscribeBroadcast(MUSICXX_BROADCAST_STATE_SONG,
                                [this](std::string_view) { ++stateEvents; });
    co_await subscribeBroadcast("musicxx.test.ping",
                                [this](std::string_view) { ++pingEvents; });

    ownPublishRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await publishBroadcast("plugin.example_native.hello",
                                R"({"from":"example_native"})");
    });
    foreignPublishRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await publishBroadcast("plugin.other_plugin.hello",
                                R"({"from":"example_native"})");
    });

    // 5.1) 内置网络能力: 只做可用性与自省调用 (create 事务不起服务、不联网)
    netTableAvailable = netAvailable();
    if (netTableAvailable) {
      netInfoRc = co_await tryCall([this]() -> asio::awaitable<void> {
        (void)co_await netCall(musicxx::plugin::net_op::info, "{}");
      });
      co_await netSubscribe([](std::string_view) {});
    }

    // 6) 声明式界面: 主页入口 + 歌曲菜单项 + 通知
    uiHomeRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await uiItemRegister(
          "card", MUSICXX_PLUGIN_UI_SLOT_HOME_ENTRY,
          R"({"title":"示例插件","subtitle":"example_native 提供的入口","icon":"resource/svg/native.svg",
                 "action":{"kind":"route","route":"ext://example_native/card"}})",
          100);
    });
    uiSongRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await uiItemRegister(
          "songInfo", MUSICXX_PLUGIN_UI_SLOT_SONG_ACTION,
          R"({"title":"示例插件：查看歌曲信息",
                 "action":{"kind":"call","name":"probe","args":{"ui":"1"}}})",
          900);
    });
    uiForeignRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await uiItemRegister("plugin.other_plugin.card",
                              MUSICXX_PLUGIN_UI_SLOT_HOME_ENTRY,
                              R"({"title":"冒充他人"})", 0);
    });
    uiBadTypeRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await uiItemRegister("bad", "musicxx.ui.unknown",
                              R"({"title":"未知类型"})", 0);
    });
    uiBadDataRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await uiItemRegister("nodata", MUSICXX_PLUGIN_UI_SLOT_HOME_ENTRY,
                              R"({"subtitle":"缺少 title"})", 0);
    });
    uiNotifyRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await uiNotify(R"({"text":"example_native 已加载","kind":"info"})");
    });

    // 7) 播放页背景 / 普通页面背景 (可选样式, 用户在设置里选中后生效)
    uiBackgroundRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await uiContentRegister("playingBg",
                                 MUSICXX_PLUGIN_UI_SLOT_PLAYING_BACKGROUND,
                                 &ExampleCtx::onGenerateBackground,
                                 "原生示例晶格背景",
                                 "跟随封面配色与音乐律动的晶格化动态背景", {},
                                 "[]", 20);
    });
    uiPageBackgroundRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await uiContentRegister(
          "pageBg", MUSICXX_PLUGIN_UI_SLOT_PAGE_BACKGROUND,
          &ExampleCtx::onGeneratePageBackground, "原生示例页面背景", {},
          R"(["/songs","/setting/theme"])", "[]", 10);
    });
    uiPageBackgroundBadRoutesRc =
        co_await tryCall([this]() -> asio::awaitable<void> {
          co_await uiContentRegister(
              "pageBgBadRoutes", MUSICXX_PLUGIN_UI_SLOT_PAGE_BACKGROUND,
              &ExampleCtx::onGeneratePageBackground, "坏的页面背景", {},
              R"("/songs")", "[]", 11);
        });

    // 7.1) 内容: 挂在槽位上的界面 JSON, 由生成函数在需要时给出
    g_self = this;
    uiContentRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await uiContentRegister(
          "row1", MUSICXX_PLUGIN_UI_SLOT_PLAYING_SONGLIST_TOP,
          &ExampleCtx::onGenerateRow, "原生示例行", "插在播放列表顶部",
          {}, R"(["musicxx.song.name","musicxx.player.playing"])", 10);
    });
    uiContentDupSlotRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await uiContentRegister("row2",
                                 MUSICXX_PLUGIN_UI_SLOT_PLAYING_SONGLIST_TOP,
                                 &ExampleCtx::onGenerateRow, "原生示例行 2", {},
                                 {}, R"(["musicxx.song.name"])", 20);
    });
    uiContentOverwriteRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await uiContentRegister(
          "row1", MUSICXX_PLUGIN_UI_SLOT_PLAYING_SONGLIST_TOP,
          &ExampleCtx::onGenerateRow, "原生示例行", "插在播放列表顶部",
          {}, R"(["musicxx.song.name","musicxx.player.playing"])", 30);
    });
    uiContentBadSlotRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await uiContentRegister("badSlot", "musicxx.ui.unknown",
                                 &ExampleCtx::onGenerateRow);
    });
    uiContentBadDepsRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await uiContentRegister("badDeps",
                                 MUSICXX_PLUGIN_UI_SLOT_PLAYING_SONGLIST_TOP,
                                 &ExampleCtx::onGenerateRow, {}, {}, {},
                                 R"("musicxx.song.name")");
    });

    // 7.2) 注销一份内容 (只影响这一个 id)
    uiContentUnregisterRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await uiContentUnregister("row2");
    });

    // 7.2.1) 生成函数里会 push 的"另一份内容"
    uiContentPushedRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await uiContentRegister("pushed",
                                 MUSICXX_PLUGIN_UI_SLOT_PLAYING_MORE_INFO_HEADER,
                                 &ExampleCtx::onGeneratePushed, {}, {}, {}, "[]",
                                 10);
    });

    // 7.3) 插件声明的槽位: id 必须是 plugin.<自己>.ui.<名字>, kind 只能 insert / replace
    uiSlotDefineRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await uiSlotDefine("plugin.example_native.ui.toolbar", "insert");
    });
    uiSlotDefineAppRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await uiSlotDefine("musicxx.ui.home.entry", "insert");
    });
    uiSlotDefineBadKindRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await uiSlotDefine("plugin.example_native.ui.bad", "pageBackground");
    });

    // 7.4) 要求重新生成 (不要求槽位已显示)
    uiSlotUpdateRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await uiSlotUpdate(MUSICXX_PLUGIN_UI_SLOT_PLAYING_SONGLIST_TOP);
    });

    // 7.5) 撤销自己声明的槽位
    uiSlotUndefineRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await uiSlotUndefine("plugin.example_native.ui.toolbar");
    });

    // 8) 能力: 供测试查询本实例自检信息 (协程处理器: 里面可以 co_await 表调用)
    co_await implement(
        "plugin.example_native.probe", 0,
        [this](std::string_view, std::string_view args) -> asio::awaitable<std::string> {
          co_return co_await probeReport(args);
        });

    // 9) 能力: **异步完成** (协程形态): 等一段时间再给结果, 等待期间框架线程空闲
    //
    // `co_await sleepAsync` 期间插件框架执行线程是空闲的: 能处理别的插件、卸载与停机,
    // 调用方也能看到"已受理但结果还没到"这一状态。`args.waitMs` 控制等多久 (缺省 200 毫秒)。
    co_await implement(
        "plugin.example_native.asyncProbe", 0,
        [this](std::string_view, std::string_view args)
            -> asio::awaitable<std::string> {
          int64_t waitMs = 200;
          const size_t at = args.find("\"waitMs\":");
          if (at != std::string_view::npos) {
            int64_t parsed = 0;
            size_t pos = at + 9;
            while (pos < args.size() && args[pos] >= '0' && args[pos] <= '9') {
              parsed = parsed * 10 + (args[pos] - '0');
              ++pos;
            }
            if (parsed > 0) {
              waitMs = parsed;
            }
          }
          co_await pluginxx::sleepAsync(*this, waitMs);
          co_return std::string{R"({"ok":true,"async":true})"};
        });

    // 10) 功能埋点: 声明 + 实现
    featurePointSampleId = "plugin." + (co_await pluginId()) + ".sample";
    featurePointSampleRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await featurePointProvide(
          featurePointSampleId.c_str(),
          [](std::string_view, std::string_view argsJson) -> std::string {
            int64_t a = 0;
            int64_t b = 0;
            if (const size_t at = argsJson.find("\"a\":");
                at != std::string_view::npos) {
              size_t pos = at + 4;
              while (pos < argsJson.size() && argsJson[pos] >= '0' &&
                     argsJson[pos] <= '9') {
                a = a * 10 + (argsJson[pos] - '0');
                ++pos;
              }
            }
            if (const size_t bt = argsJson.find("\"b\":");
                bt != std::string_view::npos) {
              size_t pos = bt + 4;
              while (pos < argsJson.size() && argsJson[pos] >= '0' &&
                     argsJson[pos] <= '9') {
                b = b * 10 + (argsJson[pos] - '0');
                ++pos;
              }
            }
            return "{\"sum\":" + std::to_string(a + b) + "}";
          });
    });
    // 声明信息 (标题 / 说明) 走应用的能力; 主程序不在 (原生测试的宿主) 时会失败,
    // 埋点本身的实现与调用不受影响
    featurePointDefineRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await featurePointDefine(
          featurePointSampleId.c_str(),
          MusicxxPluginPointSpec{
              "示例功能埋点", "把两个数相加 (演示插件功能埋点的声明与实现)",
              "a、b (数字), 缺省 0", "{sum}"});
    });

    featurePointUnprovidedId = "plugin." + (co_await pluginId()) + ".unprovided";
    co_await featurePointProvide(
        featurePointUnprovidedId.c_str(),
        [](std::string_view, std::string_view) -> std::string {
          return R"({"never":true})";
        });

    // 11) 页面 `ext://example_native/card` 是 `musicxx.ui.page` 上的**内容**
    uiPageRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await uiContentRegister("card", MUSICXX_PLUGIN_UI_SLOT_PAGE,
                                 &ExampleCtx::onGenerateCard, "示例插件",
                                 "原生示例的说明页", {}, "[]", 10);
    });
  }

  asio::awaitable<void> onStop() {
    // 撤销自管资源 (示例没有线程/定时器; 注册由插件框架在 detach 时统一摘除)
    consecutiveErrors = 0;
    co_return;
  }

  /* ==================== 自检能力 ==================== */

  /// 发布本插件命名空间的广播 (id 归属由插件框架校验)
  asio::awaitable<int32_t> publishOwnEvent(const char *topic,
                                          const char *payload) {
    co_return co_await tryCall([this, topic, payload]() -> asio::awaitable<void> {
      co_await publishBroadcast(topic, payload);
    });
  }

  /// 流结束自检 (广播表 v3): 订阅 → 发布 → 结束 → 再发布
  asio::awaitable<void> runStreamEndCheck() {
    static constexpr const char *kTopic = "plugin.example_native.stream";
    streamEndValues = 0;
    streamEndEnds = 0;
    streamEndReasonOk = false;
    streamEndSubRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await subscribeBroadcastWithEnd(
          kTopic, [this](std::string_view) { ++streamEndValues; },
          [this](std::string_view reason) {
            ++streamEndEnds;
            streamEndReasonOk =
                reason.find("example_native") != std::string_view::npos;
          });
    });
    streamEndValueRc = co_await publishOwnEvent(kTopic, R"({"v":1})");
    streamEndRc = co_await tryCall([this]() -> asio::awaitable<void> {
      co_await endBroadcast(kTopic, R"({"reason":"example_native"})");
    });
    streamEndPublishAfterRc = co_await publishOwnEvent(kTopic, R"({"v":2})");
  }

  /// 读快照频道当前值 (没有当前值给空串)
  asio::awaitable<std::string> snapshotOf(const char *topic) {
    try {
      co_return co_await readBroadcast(topic);
    } catch (const std::exception &) {
      co_return std::string{};
    }
  }

  /// 自检报文 (协程: 里面要读广播与自省列表)
  asio::awaitable<std::string> probeReport(std::string_view args) {
    // 带 `appCapabilityCall` 时顺带发起一次应用能力调用 (验证超时保护)
    if (args.find("appCapabilityCall") != std::string_view::npos) {
      lastActionStatus = -2; ///< 发起中
      try {
        (void)co_await capabilityCallWithTimeout(
            "musicxx.ui.toast", R"({"info":"example_native 超时自检"})", 1000);
        lastActionStatus = PLUGINXX_OPERATOR_OK;
      } catch (const std::exception &) {
        lastActionStatus = PLUGINXX_OPERATOR_CANCELED;
      }
    }
    // 带 `uiRefresh` 时跑一次内容 / 槽位刷新自检
    if (args.find("uiRefresh") != std::string_view::npos) {
      (void)co_await tryCall([this]() -> asio::awaitable<void> {
        co_await uiSlotUpdate(MUSICXX_PLUGIN_UI_SLOT_PLAYING_SONGLIST_TOP);
      });
      (void)co_await tryCall([this]() -> asio::awaitable<void> {
        co_await uiContentRegister("row3",
                                   MUSICXX_PLUGIN_UI_SLOT_PLAYING_SONGLIST_TOP,
                                   &ExampleCtx::onGenerateRow, {}, {}, {}, "[]",
                                   40);
      });
      (void)co_await tryCall([this]() -> asio::awaitable<void> {
        co_await uiContentUnregister("row3");
      });
      (void)co_await tryCall([this]() -> asio::awaitable<void> {
        co_await uiSlotDefine("plugin.example_native.ui.tmp", "insert");
      });
      (void)co_await tryCall([this]() -> asio::awaitable<void> {
        co_await uiSlotUndefine("plugin.example_native.ui.tmp");
      });
    }
    // 带 `streamEndCheck` 时跑一次流结束自检
    if (args.find("streamEndCheck") != std::string_view::npos) {
      co_await runStreamEndCheck();
    }

    // 插件框架按本插件的形态报接口代次
    const std::string hostInfo = co_await hostInfoJson();
    std::string hostApiVersion = "0";
    {
      const std::string key = "\"apiVersion\":";
      const std::size_t pos = hostInfo.find(key);
      if (pos != std::string::npos) {
        const std::size_t begin = pos + key.size();
        const std::size_t end = hostInfo.find_first_of(",}", begin);
        if (end != std::string::npos) {
          hostApiVersion = hostInfo.substr(begin, end - begin);
        }
      }
    }
    const std::string entries = co_await uiItemEntries();
    const std::string contents = co_await uiContentEntries();
    const std::string slots = co_await uiSlotList();
    const std::string songSnapshot = co_await snapshotOf("musicxx.state.song");

    std::ostringstream oss;
    oss << "{\"hostApiVersion\":" << hostApiVersion
        << ",\"startThread\":\"" << startThread << "\""
        << ",\"callThread\":\"" << threadIdText() << "\""
        << ",\"unknownCapabilityRc\":" << unknownCapabilityRc
        << ",\"dupImplementRc\":" << dupImplementRc
        << ",\"fixedImplRc\":" << fixedImplRc
        << ",\"foreignActionRc\":" << foreignActionRc
        << ",\"badTopicSubscribeRc\":" << badTopicSubscribeRc
        << ",\"ownPublishRc\":" << ownPublishRc
        << ",\"foreignPublishRc\":" << foreignPublishRc
        << ",\"streamEndSubRc\":" << streamEndSubRc
        << ",\"streamEndValueRc\":" << streamEndValueRc
        << ",\"streamEndRc\":" << streamEndRc
        << ",\"streamEndPublishAfterRc\":" << streamEndPublishAfterRc
        << ",\"streamEndValues\":" << streamEndValues
        << ",\"streamEndEnds\":" << streamEndEnds
        << ",\"streamEndReasonOk\":" << (streamEndReasonOk ? 1 : 0)
        << ",\"stateEvents\":" << stateEvents
        << ",\"pingEvents\":" << pingEvents
        << ",\"lastActionStatus\":" << lastActionStatus
        << ",\"lastActionPayloadLen\":" << lastActionPayloadLen
        << ",\"consecutiveErrors\":" << consecutiveErrors
        << ",\"uiHomeRc\":" << uiHomeRc << ",\"uiSongRc\":" << uiSongRc
        << ",\"uiForeignRc\":" << uiForeignRc
        << ",\"uiBadTypeRc\":" << uiBadTypeRc
        << ",\"uiBadDataRc\":" << uiBadDataRc
        << ",\"uiNotifyRc\":" << uiNotifyRc
        << ",\"uiBackgroundRc\":" << uiBackgroundRc
        << ",\"uiPageBackgroundRc\":" << uiPageBackgroundRc
        << ",\"uiPageBackgroundBadRoutesRc\":" << uiPageBackgroundBadRoutesRc
        << ",\"uiEntries\":" << entries
        << ",\"uiContentRc\":" << uiContentRc
        << ",\"uiContentDupSlotRc\":" << uiContentDupSlotRc
        << ",\"uiContentOverwriteRc\":" << uiContentOverwriteRc
        << ",\"uiContentBadSlotRc\":" << uiContentBadSlotRc
        << ",\"uiContentBadDepsRc\":" << uiContentBadDepsRc
        << ",\"uiContentUnregisterRc\":" << uiContentUnregisterRc
        << ",\"uiContentPushedRc\":" << uiContentPushedRc
        << ",\"uiContents\":" << contents
        << ",\"uiSlotDefineRc\":" << uiSlotDefineRc
        << ",\"uiSlotDefineAppRc\":" << uiSlotDefineAppRc
        << ",\"uiSlotDefineBadKindRc\":" << uiSlotDefineBadKindRc
        << ",\"uiSlotUpdateRc\":" << uiSlotUpdateRc
        << ",\"uiSlotUndefineRc\":" << uiSlotUndefineRc
        << ",\"uiSlotList\":" << slots
        << ",\"stateLen\":" << songSnapshot.size()
        << ",\"featurePointId\":\"" << featurePointSampleId << "\""
        << ",\"featurePointRc\":" << featurePointSampleRc
        << ",\"featurePointUnprovidedId\":\"" << featurePointUnprovidedId
        << "\""
        << "}";
    co_return oss.str();
  }

  /* ==================== 说明页 ==================== */

  /// 说明页文档 (块树; 由客户端按自己的能力做降级)
  std::string cardPageDoc() {
    namespace kit = musicxx::ui::kit;
    using Json = pluginxx::ui::Json;
    int32_t uiOk = 0;
    if (uiHomeRc == 0) {
      ++uiOk;
    }
    if (uiSongRc == 0) {
      ++uiOk;
    }
    Json blocks = Json::array();
    blocks.push_back(pluginxx::ui::dumpItem(kit::hint(Json::object({
        {"text",
         "这个页面演示插件的声明式页面: 插件只返回块描述 (文本 / 布局 / "
         "按钮), 不写界面代码。"},
    }))));
    blocks.push_back(pluginxx::ui::dumpItem(pluginxx::ui::build::divider()));
    blocks.push_back(
        pluginxx::ui::dumpItem(kit::listRow(Json::object({
            {"title", "create 事务线程"},
            {"subtitle", "能力处理器在同一条线程上执行"},
            {"trailing", startThread},
        }))));
    blocks.push_back(
        pluginxx::ui::dumpItem(kit::listRow(Json::object({
            {"title", "当前调用线程"},
            {"subtitle", "与 create 事务线程相同 = 单执行线程"},
            {"trailing", threadIdText()},
        }))));
    blocks.push_back(
        pluginxx::ui::dumpItem(kit::listRow(Json::object({
            {"title", "连续播放错误"},
            {"subtitle", "累计 2 次后建议换源"},
            {"trailing", std::to_string(consecutiveErrors)},
        }))));
    blocks.push_back(
        pluginxx::ui::dumpItem(kit::listRow(Json::object({
            {"title", "快照频道变化事件"},
            {"trailing", std::to_string(stateEvents)},
        }))));
    blocks.push_back(
        pluginxx::ui::dumpItem(kit::listRow(Json::object({
            {"title", "musicxx.test.ping 事件"},
            {"trailing", std::to_string(pingEvents)},
        }))));
    blocks.push_back(
        pluginxx::ui::dumpItem(kit::listRow(Json::object({
            {"title", "已注册的 UI 项"},
            {"subtitle", "主页入口 / 歌曲菜单"},
            {"trailing", std::to_string(uiOk)},
        }))));
    blocks.push_back(pluginxx::ui::dumpItem(kit::button(Json::object({
        {"label", "刷新本页"},
        {"variant", "full"},
        {"highlight", true},
        {"action", dispatchAction("card")},
    }))));
    blocks.push_back(pluginxx::ui::dumpItem(kit::button(Json::object({
        {"label", "发送一条通知"},
        {"variant", "full"},
        {"action", commandAction("musicxx.ui.toast",
                                 Json::object({{"text", "来自 example_native 的通知"}}))},
    }))));

    Json doc = Json::object({
        {"title", "示例插件"},
        {"subtitle", "页面内容来自内容 plugin.example_native.card"},
        {"blocks", std::move(blocks)},
    });
    return doc.dump();
  }
};

/// create 事务: 只做注册 (协程里可以 co_await 表调用)
asio::awaitable<void> exampleStart(ExampleCtx &ctx) {
  co_await ctx.onCreate();
}

/// stop 事务: 撤销自管资源 (可重复调用)
asio::awaitable<void> exampleStop(ExampleCtx &ctx) {
  co_await ctx.onStop();
}

} // namespace

MUSICXX_PLUGIN_EXPORT(ExampleCtx, "example_native", "2026.1010.1",
                      "musicxx 外部插件示例 (能力/广播/界面/日志)", exampleStart,
                      exampleStop)
