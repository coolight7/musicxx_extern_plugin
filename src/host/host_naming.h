/// musicxx 外部插件宿主: 命名空间规则 (plan §3.5 的单一实现)
///
/// 规则:
/// - 官方标识 (`musicxx.*`): 领域接口表 IID、钩子 id、事件 type、动作、权限、UI 项类型;
/// - 插件自定义 (`plugin.<pluginId>.*`): 事件 topic、能力名、UI 项 id; 宿主校验归属,
///   禁止插件冒充他人命名空间 (发布事件 / 注册能力 / 发起动作都要过这道校验)。
#pragma once

#include <string>
#include <string_view>

namespace musicxx {
namespace extern_plugin {
namespace naming {

/// 官方标识判定: `musicxx.<域>.<名>`
inline bool isOfficial(std::string_view id) {
    return id.rfind("musicxx.", 0) == 0;
}

/// 插件自定义标识判定: `plugin.<pluginId>.<名>` 且归属给定插件
inline bool isOwnPlugin(std::string_view id, std::string_view pluginId) {
    if (pluginId.empty() || id.size() < 9 || id.substr(0, 7) != "plugin.") {
        return false;
    }
    const std::string prefix = "plugin." + std::string{pluginId} + ".";
    return id.size() > prefix.size() && id.substr(0, prefix.size()) == prefix;
}

/// 事件主题归属判定: 官方 `musicxx.*` 放行; 插件自定义主题必须属于本插件
inline bool isTopicOwnedBy(std::string_view topic, std::string_view pluginId) {
    if (topic.rfind("musicxx.", 0) == 0) {
        return true;
    }
    return isOwnPlugin(topic, pluginId);
}

} // namespace naming
} // namespace extern_plugin
} // namespace musicxx
