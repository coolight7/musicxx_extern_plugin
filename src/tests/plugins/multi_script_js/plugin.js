/// 多脚本夹具的插件脚本（`scripts` 列表里的第二个）
///
/// 顶层同步注册一个能力, 返回自己的状态 —— 用来证明前一个脚本（kit.js）已经执行过:
/// 没有按顺序执行时 `multiScriptMarker` 是 undefined。
musicxx.capability.register("probe", function () {
    var marker = (typeof globalThis.multiScriptMarker === "string")
        ? globalThis.multiScriptMarker
        : "";
    return {
        pluginId: musicxx.pluginId,
        marker: marker,
        rowKind: multiScriptRow("x").kind,
        hostUiKind: (function () {
            const info = musicxx.host.info();
            return (info.ui && info.ui.kind) ? String(info.ui.kind) : "";
        })(),
    };
});

musicxx.host.log(2, "multi_script_js 已加载 (marker=" + globalThis.multiScriptMarker + ")");
