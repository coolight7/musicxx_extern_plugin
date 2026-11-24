/// 多目标打包示例插件: 一个插件包里放多个系统/架构分支
///
/// 打包结构（仿 APK 的 `lib/<abi>/`，清单只有一份）：
///
/// ```text
/// example_native_multi/
///   plugin.yaml                             清单 (name/entry/kind/version ...)
///   lib/windows-x64/example_native_multi.dll
///   lib/linux-x64/example_native_multi.so
///   lib/linux-arm64/example_native_multi.so
///   lib/android-arm64-v8a/example_native_multi.so
/// ```
///
/// 插件框架扫描与加载时按当前系统/CPU 架构选一个分支（优先级与别名见
/// `src/host/host_target.h`），选择结果会出现在管理页的插件信息里。
///
/// 插件自己也能知道"这次加载的是哪一份构建"：SDK 构建助手在传 `TARGET_TAG` 时会
/// 注入编译宏 `MUSICXX_PLUGIN_BUILD_TAG`（下面的能力把它报出来，便于确认插件框架
/// 选中的分支与预期一致）。
///
/// 规定: 注册事务里只做注册, 不阻塞、不发起网络/大文件操作。

#include "musicxx/plugin/api/plugin_kit.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace {

/// 本份构建的目标标签（由 SDK 助手按构建目标注入；手工构建时缺省 unknown）
#ifndef MUSICXX_PLUGIN_BUILD_TAG
#define MUSICXX_PLUGIN_BUILD_TAG "unknown"
#endif

/// 实例上下文：每个实例一份，不要放可变全局状态
struct MultiCtx : public musicxx::plugin::PluginBase {
  /// 观察能力的调用次数（只用于自检）
  int32_t songChangedHits = 0;

  /// 注册事务：只做注册
  asio::awaitable<void> onStart() {
    log.info(std::string{"多目标示例插件已启动, 本分支 = "} + MUSICXX_PLUGIN_BUILD_TAG);

    // 切歌是广播频道 (`musicxx.song.changed`): 订阅后累加计数（不裁决、不做重活）
    co_await subscribeBroadcast(MUSICXX_BROADCAST_SONG_CHANGED,
                                [this](std::string_view) { ++songChangedHits; });

    // 前端链示例（`implementBefore` / `implementAfter`）不在这里：它属于"完整用法"，
    // 已经放在 `example_native` 里（带演示标记，不会误伤真实调用）。
    // 这个示例只讲一件事：一个包里放多个系统/架构分支，加载的是哪一份。

    // 能力：报出本份构建的分支标签 + 插件框架上报的平台 + 能力计数
    co_await implement(
        "plugin.example_native_multi.which", 0,
        [this](std::string_view) -> asio::awaitable<std::string> {
          const std::string info = co_await hostInfoJson();
          std::string platform;
          const std::size_t pos = info.find("\"platform\"");
          if (pos != std::string::npos) {
            const std::size_t begin = info.find(':', pos);
            const std::size_t first = info.find('"', begin);
            const std::size_t last = info.find('"', first + 1);
            if (first != std::string::npos && last != std::string::npos) {
              platform = info.substr(first + 1, last - first - 1);
            }
          }
          co_return std::string{"{\"tag\":\""} + MUSICXX_PLUGIN_BUILD_TAG +
              "\",\"platform\":\"" + platform + "\",\"hits\":" +
              std::to_string(songChangedHits) + "}";
        });
  }

  /// 撤销事务：撤销自管资源（本示例没有）
  asio::awaitable<void> onStop() {
    songChangedHits = 0;
    co_return;
  }
};

} // namespace

MUSICXX_PLUGIN_EXPORT(
    MultiCtx,
    "example_native_multi",
    "2026.1010.1",
    "多目标打包示例（包内多个系统/架构分支）",
    [](MultiCtx &ctx) -> asio::awaitable<void> { co_await ctx.onStart(); },
    [](MultiCtx &ctx) -> asio::awaitable<void> { co_await ctx.onStop(); })
