/// 槽位提供方：声明一个自己的槽位，并往里面放一份内容
///
/// 槽位 = 一个渲染位置。应用已经有很多位置（播放页、控制栏、页面背景……），
/// 插件也能声明自己的：`plugin.<插件 id>.ui.<名字>`，`kind` 只能是 `insert` / `replace`。
/// 声明之后**任何插件**（包括自己）都能用 `musicxx.ui.content.register({slot: …})` 往里注册内容，
/// 再用 `{"kind":"UISlot","slot":"plugin.<插件 id>.ui.<名字>"}` 块把它显示出来。
///
/// 配套示例：`example_js_slot_consumer`（在自己的界面里嵌这个槽位）。

/// 槽位短名与全名
const SLOT_NAME = "toolbar";
const SLOT_ID = "plugin." + musicxx.pluginId + ".ui." + SLOT_NAME;

/// 1) 声明槽位（`insert` = 可以放多份内容、按 order 排）
musicxx.ui.slot.define({ id: SLOT_NAME, kind: "insert" });

/// 2) 往自己的槽位里放一份内容
///
/// `scope:musicxx.slot.args` 是外层 `UISlot.args` 传进来的参数：只有嵌在别人的界面里时才有值。
/// 声明成 deps 之后，参数变了（外层换了一份内容）会让这份内容重新生成。
musicxx.ui.content.register({
    id: "hint",
    slot: SLOT_ID,
    title: "提供方的一段话",
    depict: "声明自己槽位的插件放进去的内容，外层用 UISlot.args 控制紧凑模式",
    deps: ["scope:musicxx.slot.args"],
}, function (ctx) {
    const args = (ctx && ctx["scope:musicxx.slot.args"]) || {};
    const compact = args.compact === true;
    return {
        blocks: [
            {
                kind: "Text",
                text: "这段来自提供方插件『" + musicxx.pluginId + "』" +
                    (compact ? "（紧凑模式）" : ""),
                type: "caption",
                color: "hint",
            },
            {
                kind: "Button",
                label: "提供方的按钮",
                variant: "simple",
                action: { kind: "call", name: "toast" },
            },
        ],
    };
});

/// 按钮动作：弹一条提示（证明嵌进来的内容里的动作也照常工作）
musicxx.capability.register("toast", function () {
    musicxx.ui.toast("提供方的按钮被点了");
    return { ok: true };
});

/// 自检：槽位声明是否生效、内容是否注册上
musicxx.capability.register("probe", function () {
    const slots = musicxx.ui.slots().slots.filter(function (item) {
        return item.id === SLOT_ID;
    });
    const contents = musicxx.ui.content.entries().filter(function (item) {
        return item.slot === SLOT_ID;
    });
    return {
        ok: true,
        slotId: SLOT_ID,
        // 数字（1 / 0）：原生测试按整数字段读
        declared: slots.length > 0 ? 1 : 0,
        contents: contents.length,
    };
});

/// 主页入口：说明这个插件只做"提供槽位"这件事
musicxx.ui.item.register({
    name: "home",
    type: "home.entry",
    order: 150,
    data: {
        title: "槽位提供方示例",
        subtitle: "声明自己的槽位，让别的插件把内容嵌进来",
        action: { kind: "call", name: "toast" },
    },
});

musicxx.host.log(2, "槽位提供方已加载: " + SLOT_ID);
