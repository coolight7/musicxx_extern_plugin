/// 槽位使用方：在自己的界面里嵌**别的插件**的槽位
///
/// `UISlot` 块只做一件事：在这个位置上显示另一个槽位的内容。
///
/// - `slot`：要嵌的槽位（应用已有的位置，或插件声明的 `plugin.<插件 id>.ui.<名字>`）；
/// - `args`：传给那份内容的参数，在它那边是变量 `scope:musicxx.slot.args`（`deps` 里声明才会重生成）；
/// - `fallback`：槽位被撤销 / 里面没有内容 / 对方插件没装时画这个（不写就留空）。
///
/// 嵌套深度上限 4；自己嵌自己会按"循环"处理画 `fallback`。配套示例是
/// `example_js_slot_provider`（声明槽位并提供内容）。

const PROVIDER_SLOT = "plugin.example_js_slot_provider.ui.toolbar";
const PLUGIN_ID = musicxx.pluginId;

/// 播放页「歌词页底部」：嵌提供方的槽位
musicxx.ui.content.register({
    id: "nested",
    slot: "musicxx.ui.playing.lyric.bottom",
    title: "歌词页底部的嵌套槽位",
    depict: "这块里再嵌别的插件声明的槽位（UISlot）",
    order: 20,
}, function () {
    return {
        blocks: [
            {
                kind: "UISlot",
                slot: PROVIDER_SLOT,
                args: { compact: false },
                fallback: {
                    kind: "Text",
                    text: "没装『槽位提供方』示例插件，所以这里显示 fallback",
                    type: "caption",
                    color: "hint",
                },
            },
        ],
    };
});

/// 说明页：解释 UISlot 的三种结果（有内容 / 没内容 / 循环）
musicxx.ui.content.register({
    id: "card",
    slot: "musicxx.ui.page",
    title: "槽位使用方示例",
}, function () {
    return {
        title: "槽位使用方示例",
        subtitle: "用 UISlot 嵌别的插件声明的槽位",
        blocks: [
            {
                kind: "Text",
                text: "• 播放页 → 歌词页底部那块来自 `example_js_slot_provider`（UISlot 嵌的）。",
                type: "caption",
            },
            {
                kind: "Text",
                text: "• 下面这条是同一个槽位、换一组参数（compact: true）：提供方那份内容会读 " +
                    "`scope:musicxx.slot.args` 换样子。",
                type: "caption",
                color: "hint",
            },
            {
                kind: "UISlot",
                slot: PROVIDER_SLOT,
                args: { compact: true },
                fallback: {
                    kind: "Text",
                    text: "提供方没装，这里显示 fallback",
                    type: "caption",
                    color: "hint",
                },
            },
        ],
    };
});

/// 主页入口
musicxx.ui.item.register({
    name: "home",
    type: "home.entry",
    order: 151,
    data: {
        title: "槽位使用方示例",
        subtitle: "在自己的界面里嵌别的插件的槽位（UISlot）",
        action: { kind: "route", route: "ext://" + PLUGIN_ID + "/card" },
    },
});

/// 自检：两个页面 / 位置上的内容都注册上了
musicxx.capability.register("probe", function () {
    return {
        ok: true,
        providerSlot: PROVIDER_SLOT,
        contents: musicxx.ui.content.entries().length,
    };
});

musicxx.host.log(2, "槽位使用方已加载: 嵌入 " + PROVIDER_SLOT);
