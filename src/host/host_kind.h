/// 插件形态（kind）注册表（musicxx 侧薄包装）
///
/// 注册表本体在子模块 `pluginxx/host/kind_registry.h`，两个项目共用同一份规则：
/// 形态名格式、缺 provider / loadCapability 拒绝、自带形态只能被接管不能改代次、
/// 撤销归提供者、未登记形态按 C ABI 对待。这里只补 musicxx 自己的两项事实：
///
/// - **内置 `native`**：由插件框架自己加载动态库，接口代次对跟随插件 ABI
///   （`MUSICXX_EXTERN_PLUGIN_MIN_API_VERSION`）；
/// - **内置 `js`**：内置 JS 运行器解释执行，按 JS 接口代次对判
///   （当前代次 3 / 兼容下限 3）。它可以被插件接管（`overrideBuiltin`），撤销后回到内置实现。
///
/// 判定"由谁实现"只看 `provider`：空 = 插件框架自己，非空 = 由这个插件托管。
/// "这个包能不能作为这种形态跑"由提供者回答（`validateCapability`，可选）：
/// 框架不按形态名写分支，只负责把目录交给提供者，拿到拒绝理由就原样告诉用户。
///
/// 线程约定：只在插件框架 IO 线程读写（与其它插件注册同一串行上下文）。
#pragma once

#include "musicxx/plugin/api/plugin_api.h"

#include "pluginxx/host/kind_registry.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace musicxx {
namespace extern_plugin {

/// 内置 JS 运行器的接口代次对（与 C 接口代次是**两套独立编号**，不要互相比较）
///
/// - 当前代次 `jsInterfaceVersion = 3`：广播只剩读与订阅（没有 `musicxx.broadcast.get` /
///   `write` / `watch` 与 `undeclare`，声明字段是 `publish`，订阅带 `intervalMs` /
///   `mode`）—— **删接口**，所以下限一起抬到 3；同一批破坏性改动还包括
///   `musicxx.ui.slot.update(名字)` 不再带 `data`（参见 `docs/plugin-ui.md` §1.2）；
/// - 兼容下限 `jsMinInterfaceVersion = 3`：只接受按这一代接口写的 JS 插件。下限与当前代次相同
///   = 没有跨代兼容；以后只做新增时再单独抬当前代次（参考 C 接口的 `PLUGINXX_MIN_API_VERSION`）。
inline constexpr int32_t jsInterfaceVersion = 3;
inline constexpr int32_t jsMinInterfaceVersion = 3;

/// 一种插件形态的声明（与通用侧同名结构完全一致，不要在这里加字段）
using MusicxxPluginKindSpec = pluginxx::PluginxxKindSpec;

/// musicxx 的形态注册表：通用注册表 + `native` / `js` 两项内置形态
class MusicxxPluginKindRegistry : public pluginxx::PluginxxKindRegistry {
public:
  MusicxxPluginKindRegistry()
      : pluginxx::PluginxxKindRegistry(defaultBuiltins(),
                                       MUSICXX_EXTERN_PLUGIN_MIN_API_VERSION) {}

private:
  /// 内置形态：顺序即 `kindList()` 与自省列表里的顺序
  static std::vector<pluginxx::PluginxxKindSpec> defaultBuiltins() {
    std::vector<pluginxx::PluginxxKindSpec> builtins;

    pluginxx::PluginxxKindSpec native;
    native.kind = "native";
    native.builtin = true;
    native.requiresPluginAbi = true;
    native.interfaceVersion = MUSICXX_EXTERN_PLUGIN_MIN_API_VERSION;
    native.minInterfaceVersion = MUSICXX_EXTERN_PLUGIN_MIN_API_VERSION;
    native.describe =
        "native plugin: loaded by the framework, uses the generic iface tables";
    builtins.push_back(std::move(native));

    pluginxx::PluginxxKindSpec js;
    js.kind = "js";
    js.builtin = true;
    js.requiresPluginAbi = false;
    js.interfaceVersion = jsInterfaceVersion;
    js.minInterfaceVersion = jsMinInterfaceVersion;
    js.describe = "js plugin: interpreted by the built-in JS runtime, no C iface tables";
    builtins.push_back(std::move(js));

    return builtins;
  }
};

} // namespace extern_plugin
} // namespace musicxx
