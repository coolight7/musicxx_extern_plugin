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
/// 宿主扫描与装载时按当前系统/CPU 架构选一个分支（优先级与别名见
/// `src/host/host_target.h`），选择结果会出现在管理页的插件信息里。
///
/// 插件自己也能知道"这次加载的是哪一份构建"：SDK 构建助手在传 `TARGET_TAG` 时会
/// 注入编译宏 `MUSICXX_PLUGIN_BUILD_TAG`（下面的能力把它报出来，便于确认宿主
/// 选中的分支与预期一致）。
///
/// 规定: start 事务里只做注册, 不阻塞、不发起网络/大文件操作。

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
  /// 观察钩子的调用次数（只用于自检）
  int32_t songChangedHits = 0;

  /// start 事务：只做注册
  int32_t onStart() {
    log.info(std::string{"多目标示例插件已启动, 本分支 = "} +
             MUSICXX_PLUGIN_BUILD_TAG);

    // 观察型钩子：切歌时累加计数（不裁决、不做重活）
    observe(MUSICXX_PLUGIN_HOOK_SONG_CHANGED,
            [this](std::string_view, std::string &) -> int32_t {
              ++songChangedHits;
              return 0;
            });

    // 能力：报出本份构建的分支标签 + 宿主上报的平台 + 钩子计数
    capability(*this, "plugin.example_native_multi.which",
               [this](std::string_view, std::string_view) -> std::string {
                 std::string platform;
                 const std::string info = hostInfoJson();
                 const std::size_t pos = info.find("\"platform\"");
                 if (pos != std::string::npos) {
                   const std::size_t begin = info.find(':', pos);
                   const std::size_t first = info.find('"', begin);
                   const std::size_t last = info.find('"', first + 1);
                   if (first != std::string::npos && last != std::string::npos) {
                     platform = info.substr(first + 1, last - first - 1);
                   }
                 }
                 return std::string{"{\"tag\":\""} + MUSICXX_PLUGIN_BUILD_TAG +
                        "\",\"platform\":\"" + platform + "\",\"hits\":" +
                        std::to_string(songChangedHits) + "}";
               });
    return 0;
  }

  /// stop 事务：撤销自管资源（本示例没有）
  int32_t onStop() {
    songChangedHits = 0;
    return 0;
  }
};

int32_t multiStart(MultiCtx &ctx) { return ctx.onStart(); }
int32_t multiStop(MultiCtx &ctx) { return ctx.onStop(); }

} // namespace

MUSICXX_PLUGIN_EXPORT(
    MultiCtx,
    "example_native_multi",
    "1.0.0",
    "多目标打包示例（包内多个系统/架构分支）",
    multiStart,
    multiStop
)
