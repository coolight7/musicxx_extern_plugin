/// 多脚本示例的"kit"脚本（相当于随插件分发的 kit 文件）
///
/// 它在同一个 JS 上下文里先执行: 只往全局挂一个工具函数, 不注册任何钩子/能力。
/// 后面的 plugin.js 能直接用它 —— 这就是 kit 随插件目录分发的用法。

globalThis.multiScriptMarker = "kit-loaded";

function multiScriptRow(text) {
    return { kind: "Text", text: String(text) };
}
