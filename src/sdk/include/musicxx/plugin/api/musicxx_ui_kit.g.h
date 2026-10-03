// 本文件由 tools/gen_ui.dart 生成，请勿手工修改。
// 定义来源：schema/ui.def.json / schema/kit.def.json；扩展 kit 定义：schema/musicxx-ui-kit.def.json
#pragma once

// kit：共享便捷组件。只装配、不含逻辑，也不引用客户端专属块。
// 参数用 utilxx_base::Json 传（对象），键即组件参数名：
//   musicxx::ui::kit::listRow({{"title", "切歌次数"}, {"trailing", "3"}})
// 传 env（客户端能力摘要）时按目标选择更合适的变体；不传 env 时产出中立描述，
// 由客户端的 adapt() 收口。

#include <pluginxx/ui/item.h>
#include <pluginxx/ui/kit_runtime.h>

#include <utilxx_base/json.h>

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

namespace musicxx {
namespace ui {
namespace kit {

/// kit 版本
inline constexpr int kKitVersion = 1;

/// 组件模板（键 = 组件名，值是 {variants, params} 的 JSON 文本）
inline pluginxx::ui::Json kitTemplate(const std::string_view name) {
    static const std::map<std::string_view, std::string_view> kTable = {
        {"title",
         R"KIT({"variants":[{"template":{"kind":"Block","variant":"plain","padding":{"vertical":15},"children":[{"kind":"Text","text":"$text","type":"title"}]}}],"params":{"text":null}})KIT"},
        {"hint",
         R"KIT({"variants":[{"template":{"kind":"Block","variant":"plain","padding":{"vertical":15},"children":[{"kind":"Text","text":"$text","type":"caption","tone":"hint"}]}}],"params":{"text":null}})KIT"},
        {"text",
         R"KIT({"variants":[{"template":{"kind":"Block","variant":"plain","padding":{"vertical":15},"children":[{"kind":"Text","text":"$text","tone":"$tone","mono":"$mono"}]}}],"params":{"text":null,"tone":"normal","mono":false}})KIT"},
        {"badge",
         R"KIT({"variants":[{"template":{"kind":"Badge","text":"$text","tone":"$tone"}}],"params":{"text":null,"tone":"accent"}})KIT"},
        {"icon",
         R"KIT({"variants":[{"requires":["Icon"],"template":{"kind":"Icon","name":"$name","glyph":"$glyph","size":"$size","tone":"$tone"}},{"template":{"kind":"Text","text":"$glyph","mono":true,"tone":"$tone"}}],"params":{"name":null,"glyph":null,"size":null,"tone":"normal"}})KIT"},
        {"gap",
         R"KIT({"variants":[{"template":{"kind":"Gap","size":"$size"}}],"params":{"size":null}})KIT"},
        {"divider",
         R"KIT({"variants":[{"template":{"kind":"Block","variant":"plain","padding":{"vertical":15},"children":[{"kind":"Divider"}]}}],"params":{}})KIT"},
        {"button",
         R"KIT({"variants":[{"template":{"kind":"Block","variant":"plain","padding":{"vertical":15},"children":[{"kind":"Button","label":"$label","variant":"$variant","icon":"$icon","disabled":"$disabled","action":"$action"}]}}],"params":{"label":null,"variant":"secondary","icon":null,"disabled":false,"action":null}})KIT"},
        {"actionsRow",
         R"KIT({"variants":[{"template":{"kind":"Row","gap":12,"children":{"$map":"buttons","wrap":{"kind":"Expanded","children":["$item"]}}}}],"params":{"buttons":null}})KIT"},
        {"card",
         R"KIT({"variants":[{"template":{"kind":"Block","title":"$title","variant":"$variant","padding":"$padding","margin":"$margin","children":"$children"}}],"params":{"title":null,"variant":"card","padding":{"horizontal":50,"vertical":50},"margin":{"vertical":50},"children":null}})KIT"},
        {"listRow",
         R"KIT({"variants":[{"template":{"kind":"Block","variant":"plain","padding":{"horizontal":10,"vertical":6},"action":"$action","children":[{"kind":"Row","gap":10,"cross":"center","children":[{"kind":"Expanded","children":[{"kind":"Column","gap":2,"children":[{"kind":"Text","text":"$title"},{"$require":"subtitle","kind":"Text","text":"$subtitle","type":"caption","tone":"hint"}]}]},{"$require":"trailing","kind":"Text","text":"$trailing","type":"caption","tone":"hint"}]}]}}],"params":{"title":null,"subtitle":null,"trailing":null,"action":null}})KIT"},
        {"section",
         R"KIT({"variants":[{"template":{"kind":"Column","gap":8,"children":[{"kind":"Text","text":"$title","type":"title"},{"kind":"Column","children":"$rows"}]}}],"params":{"title":null,"rows":null}})KIT"},
        {"kv",
         R"KIT({"variants":[{"template":{"kind":"KV","pairs":"$pairs","sep":"$sep","keyWidth":"auto"}}],"params":{"pairs":null,"sep":null}})KIT"},
        {"table",
         R"KIT({"variants":[{"template":{"kind":"Table","header":"$header","columns":"$columns","rows":"$rows"}}],"params":{"columns":null,"rows":null,"header":true}})KIT"},
        {"tree",
         R"KIT({"variants":[{"template":{"kind":"Tree","connector":"$connector","nodes":"$nodes"}}],"params":{"nodes":null,"connector":true}})KIT"},
        {"sparkline",
         R"KIT({"variants":[{"template":{"kind":"Sparkline","data":"$data","height":"$height","glyphStyle":"$glyphStyle","showLast":"$showLast","tone":"$tone"}}],"params":{"data":null,"height":1,"glyphStyle":"block","showLast":true,"tone":"accent"}})KIT"},
        {"progressRow",
         R"KIT({"variants":[{"template":{"kind":"Row","gap":12,"cross":"center","children":[{"$require":"label","kind":"Text","text":"$label"},{"kind":"Expanded","children":[{"kind":"Progress","value":"$value","total":"$total","unit":"$unit"}]}]}}],"params":{"label":null,"value":null,"total":100,"unit":"%"}})KIT"},
        {"settingRow",
         R"KIT({"variants":[{"template":{"kind":"Block","variant":"plain","padding":{"horizontal":10,"vertical":6},"action":"$action","children":[{"kind":"Row","gap":12,"cross":"center","children":[{"kind":"Expanded","children":[{"kind":"Column","gap":2,"children":[{"kind":"Text","text":"$title"},{"$require":"depict","kind":"Text","text":"$depict","type":"caption","tone":"hint"}]}]},{"$require":"value","kind":"Text","text":"$value","type":"caption","tone":"hint"}]}]}}],"params":{"title":null,"depict":null,"value":null,"action":null}})KIT"},
        {"switchRow",
         R"KIT({"variants":[{"template":{"kind":"Block","variant":"plain","padding":{"horizontal":10,"vertical":6},"children":[{"kind":"Row","gap":12,"cross":"center","children":[{"kind":"Expanded","children":[{"kind":"Column","gap":2,"children":[{"kind":"Text","text":"$title"},{"$require":"depict","kind":"Text","text":"$depict","type":"caption","tone":"hint"}]}]},{"kind":"Control","control":"switch","id":"$id","value":"$value","action":"$action"}]}]}}],"params":{"id":null,"title":null,"depict":null,"value":null,"action":null}})KIT"},
        {"inputRow",
         R"KIT({"variants":[{"template":{"kind":"Block","variant":"plain","padding":{"horizontal":10,"vertical":6},"children":[{"kind":"Column","gap":6,"children":[{"kind":"Text","text":"$title"},{"$require":"depict","kind":"Text","text":"$depict","type":"caption","tone":"hint"},{"kind":"Control","control":"text","id":"$id","value":"$value","multiline":"$multiline","action":"$action"}]}]}}],"params":{"id":null,"title":null,"depict":null,"value":null,"multiline":false,"action":null}})KIT"},
        {"shaderBlock",
         R"KIT({"variants":[{"requires":["musicxx.Shader"],"template":{"kind":"musicxx.Shader","bundle":"$bundle","args":"$args","speed":"$speed","maxFps":"$maxFps","animate":"$animate","resolutionScale":"$resolutionScale"}},{"template":{"kind":"Text","text":"（这个客户端不支持插件着色器）","type":"caption","tone":"hint"}}],"params":{"bundle":null,"args":null,"speed":1,"maxFps":null,"animate":true,"resolutionScale":1}})KIT"},
        {"coverRow",
         R"KIT({"variants":[{"requires":["Image"],"template":{"kind":"Block","variant":"plain","padding":{"horizontal":10,"vertical":6},"action":"$action","children":[{"kind":"Row","gap":10,"cross":"center","children":[{"kind":"Image","source":"file","src":"$cover","width":"$size","height":"$size","radius":4,"alt":"$title"},{"kind":"Expanded","children":[{"kind":"Column","gap":2,"children":[{"kind":"Text","text":"$title"},{"$require":"subtitle","kind":"Text","text":"$subtitle","type":"caption","tone":"hint"}]}]},{"$require":"trailing","kind":"Text","text":"$trailing","type":"caption","tone":"hint"}]}]}},{"template":{"kind":"Block","variant":"plain","padding":{"horizontal":10,"vertical":6},"action":"$action","children":[{"kind":"Row","gap":10,"cross":"center","children":[{"kind":"Expanded","children":[{"kind":"Column","gap":2,"children":[{"kind":"Text","text":"$title"},{"$require":"subtitle","kind":"Text","text":"$subtitle","type":"caption","tone":"hint"}]}]},{"$require":"trailing","kind":"Text","text":"$trailing","type":"caption","tone":"hint"}]}]}}],"params":{"cover":null,"title":null,"subtitle":null,"trailing":null,"size":40,"action":null}})KIT"},
        {"animScope",
         R"KIT({"variants":[{"template":{"kind":"musicxx.AnimatedBuilder","values":"$values","children":"$children"}},{"template":{"kind":"musicxx.AnimatedBuilder","values":"$values","$require":"maxFps","maxFps":"$maxFps","children":"$children"}}],"params":{"values":null,"maxFps":null,"children":null}})KIT"},
        {"fadeTransition",
         R"KIT({"variants":[{"template":{"kind":"musicxx.FadeTransition","value":"$value","children":"$children"}},{"template":{"kind":"musicxx.FadeTransition","value":"$value","$require":"curve","curve":"$curve","children":"$children"}}],"params":{"value":null,"curve":null,"children":null}})KIT"},
        {"sizeTransition",
         R"KIT({"variants":[{"template":{"kind":"musicxx.SizeTransition","value":"$value","children":"$children"}},{"template":{"kind":"musicxx.SizeTransition","value":"$value","$require":"curve","curve":"$curve","children":"$children"}},{"template":{"kind":"musicxx.SizeTransition","value":"$value","$require":"curve","axis":"$axis","curve":"$curve","children":"$children"}}],"params":{"value":null,"axis":null,"curve":null,"children":null}})KIT"},
        {"scaleTransition",
         R"KIT({"variants":[{"template":{"kind":"musicxx.ScaleTransition","value":"$value","children":"$children"}},{"template":{"kind":"musicxx.ScaleTransition","value":"$value","$require":"curve","curve":"$curve","children":"$children"}},{"template":{"kind":"musicxx.ScaleTransition","value":"$value","$require":"curve","from":"$from","to":"$to","curve":"$curve","children":"$children"}}],"params":{"value":null,"from":null,"to":null,"curve":null,"children":null}})KIT"},
        {"rotationTransition",
         R"KIT({"variants":[{"template":{"kind":"musicxx.RotationTransition","value":"$value","children":"$children"}},{"template":{"kind":"musicxx.RotationTransition","value":"$value","$require":"curve","from":"$from","to":"$to","curve":"$curve","children":"$children"}}],"params":{"value":null,"from":null,"to":null,"curve":null,"children":null}})KIT"},
        {"slideTransition",
         R"KIT({"variants":[{"template":{"kind":"musicxx.SlideTransition","value":"$value","children":"$children"}},{"template":{"kind":"musicxx.SlideTransition","value":"$value","$require":"curve","from":"$from","to":"$to","curve":"$curve","children":"$children"}}],"params":{"value":null,"from":null,"to":null,"curve":null,"children":null}})KIT"},
    };
    const auto it = kTable.find(name);
    if (it == kTable.end()) {
        return pluginxx::ui::Json::object();
    }
    return pluginxx::ui::Json::parse(it->second);
}

/// 按格换算成 u（count 列），env 为空时用库默认格大小
inline double cols(const int count, const pluginxx::ui::Capabilities* env = nullptr) {
    return static_cast<double>(count) *
           (env != nullptr ? env->cell.width : pluginxx::ui::gen::kDefaultCellWidth);
}
/// 按格换算成 u（count 行），env 为空时用库默认格大小
inline double rows(const int count, const pluginxx::ui::Capabilities* env = nullptr) {
    return static_cast<double>(count) *
           (env != nullptr ? env->cell.height : pluginxx::ui::gen::kDefaultCellHeight);
}

/// kit 组件（参数说明见生成的 docs/kit.md）

/// 标题行（覆盖基础 kit：自带上下留白，框架不再给隐含边距）
/// 参数：text(text, 必填)
inline pluginxx::ui::Item title(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("title", params, env, &kitTemplate);
}

/// 次要说明行（覆盖基础 kit：自带上下留白，框架不再给隐含边距）
/// 参数：text(text, 必填)
inline pluginxx::ui::Item hint(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("hint", params, env, &kitTemplate);
}

/// 正文行（覆盖基础 kit：自带上下留白，框架不再给隐含边距）
/// 参数：text(text, 必填)、tone(tone, 默认 normal)、mono(bool, 默认 false)
inline pluginxx::ui::Item text(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("text", params, env, &kitTemplate);
}

/// 状态小标签
/// 参数：text(text, 必填)、tone(tone, 默认 accent)
inline pluginxx::ui::Item badge(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("badge", params, env, &kitTemplate);
}

/// 图标（目标不支持 Icon 时退化成 glyph 文本）
/// 参数：name(string)、glyph(string)、size(size)、tone(tone, 默认 normal)
inline pluginxx::ui::Item icon(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("icon", params, env, &kitTemplate);
}

/// 竖直留白（缺省用客户端默认行距）
/// 参数：size(size, 默认 gap)
inline pluginxx::ui::Item gap(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("gap", params, env, &kitTemplate);
}

/// 分隔线（覆盖基础 kit：自带上下留白）
/// 参数：
inline pluginxx::ui::Item divider(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("divider", params, env, &kitTemplate);
}

/// 按钮（覆盖基础 kit：自带上下留白）
/// 参数：label(text, 必填)、variant(enum:buttonVariant, 默认 secondary)、icon(string)、disabled(bool, 默认 false)、action(action)
inline pluginxx::ui::Item button(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("button", params, env, &kitTemplate);
}

/// 一排等宽按钮（按钮列表里的每一项占一等份）
/// 参数：buttons(items, 必填)
inline pluginxx::ui::Item actionsRow(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("actionsRow", params, env, &kitTemplate);
}

/// 内容块（卡片，覆盖基础 kit：留白在这里声明，框架不再给隐含边距）
/// 参数：title(text)、variant(enum:blockVariant, 默认 card)、padding(edges, 默认 {horizontal: 50, vertical: 50})、margin(edges, 默认 {vertical: 50})、children(items)
inline pluginxx::ui::Item card(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("card", params, env, &kitTemplate);
}

/// 卡片里的一行（不带底色：卡片里直接按列表排列；右侧状态用说明字号）
/// 参数：title(text, 必填)、subtitle(text)、trailing(text)、action(action)
inline pluginxx::ui::Item listRow(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("listRow", params, env, &kitTemplate);
}

/// 小节标题 + 若干行
/// 参数：title(text, 必填)、rows(items, 必填)
inline pluginxx::ui::Item section(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("section", params, env, &kitTemplate);
}

/// 键值块（键列按最长键自适应）
/// 参数：pairs(pairs, 必填)、sep(string)
inline pluginxx::ui::Item kv(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("kv", params, env, &kitTemplate);
}

/// 表格（未指定的列宽由客户端自动分配）
/// 参数：columns(columns, 必填)、rows(rows)、header(bool, 默认 true)
inline pluginxx::ui::Item table(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("table", params, env, &kitTemplate);
}

/// 层级列表
/// 参数：nodes(nodes, 必填)、connector(bool, 默认 true)
inline pluginxx::ui::Item tree(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("tree", params, env, &kitTemplate);
}

/// 迷你趋势图
/// 参数：data(numbers, 必填)、height(int, 默认 1)、glyphStyle(enum:sparkStyle, 默认 block)、showLast(bool, 默认 true)、tone(tone, 默认 accent)
inline pluginxx::ui::Item sparkline(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("sparkline", params, env, &kitTemplate);
}

/// 一行进度（覆盖基础 kit：musicxx 默认按百分比显示数值）
/// 参数：label(text)、value(float, 必填)、total(float, 默认 100)、unit(string, 默认 %)
inline pluginxx::ui::Item progressRow(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("progressRow", params, env, &kitTemplate);
}

/// 设置行：标题 + 说明 + 右侧当前值（整行可点且不带底色，右侧值用说明字号）
/// 参数：title(text, 必填)、depict(text)、value(text)、action(action)
inline pluginxx::ui::Item settingRow(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("settingRow", params, env, &kitTemplate);
}

/// 开关行：标题 + 说明 + 右侧开关（不带底色，值变化即派发动作、参数带 id 与当前值）
/// 参数：id(string, 必填)、title(text, 必填)、depict(text)、value(json)、action(action)
inline pluginxx::ui::Item switchRow(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("switchRow", params, env, &kitTemplate);
}

/// 输入行：标题 + 说明 + 输入框（不带底色，输入完成即派发动作）
/// 参数：id(string, 必填)、title(text, 必填)、depict(text)、value(json)、multiline(bool, 默认 false)、action(action)
inline pluginxx::ui::Item inputRow(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("inputRow", params, env, &kitTemplate);
}

/// 插件着色器块（musicxx 专属组件；终端之类不认识它的客户端会跳过）
/// 参数：bundle(string, 必填)、args(json)、speed(float, 默认 1)、maxFps(int)、animate(bool, 默认 true)、resolutionScale(float, 默认 1)
inline pluginxx::ui::Item shaderBlock(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("shaderBlock", params, env, &kitTemplate);
}

/// 带封面的行：封面 + 标题 + 副标题 + 可选右侧状态（不带底色；取不到图片时显示标题）
/// 参数：cover(string, 必填)、title(text, 必填)、subtitle(text)、trailing(text)、size(size, 默认 40)、action(action)
inline pluginxx::ui::Item coverRow(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("coverRow", params, env, &kitTemplate);
}

/// 动画作用域：声明通道表（名字 → 值表达式）并逐帧驱动子树（musicxx.AnimatedBuilder）
/// 参数：values(json, 必填)、maxFps(int)、children(items, 必填)
inline pluginxx::ui::Item animScope(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("animScope", params, env, &kitTemplate);
}

/// 透明度过渡：按 value（0~1，字面量或值表达式）淡入淡出；curve 是简写缓动
/// 参数：value(value, 必填)、curve(enum:ease)、children(items, 必填)
inline pluginxx::ui::Item fadeTransition(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("fadeTransition", params, env, &kitTemplate);
}

/// 尺寸过渡：按 value（0~1）把子块从 0 撑开 / 收拢（axis 缺省竖直）
/// 参数：value(value, 必填)、axis(enum:axis)、curve(enum:ease)、children(items, 必填)
inline pluginxx::ui::Item sizeTransition(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("sizeTransition", params, env, &kitTemplate);
}

/// 缩放过渡：按 value（0~1）从 from 缩放到 to（缺省 0 → 1）
/// 参数：value(value, 必填)、from(value)、to(value)、curve(enum:ease)、children(items, 必填)
inline pluginxx::ui::Item scaleTransition(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("scaleTransition", params, env, &kitTemplate);
}

/// 旋转过渡：按 value（0~1）从 from 转到 to（单位 = 圈数，缺省 0 → 1）
/// 参数：value(value, 必填)、from(value)、to(value)、curve(enum:ease)、children(items, 必填)
inline pluginxx::ui::Item rotationTransition(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("rotationTransition", params, env, &kitTemplate);
}

/// 位移过渡：按 value（0~1）从 from 移到 to（单位 = 自身尺寸的倍数，如 [0, 0.1] → [0, 0]）
/// 参数：value(value, 必填)、from(value)、to(value)、curve(enum:ease)、children(items, 必填)
inline pluginxx::ui::Item slideTransition(
    const pluginxx::ui::Json& params = pluginxx::ui::Json::object(),
    const pluginxx::ui::Capabilities* env = nullptr
) {
    return pluginxx::ui::detail::expandKit("slideTransition", params, env, &kitTemplate);
}

} // namespace kit
} // namespace ui
} // namespace musicxx
