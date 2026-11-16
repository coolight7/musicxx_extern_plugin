/// example_js_playing_slots 的音频动效: 渲染槽位 `playing.visualizer`
///
/// 播放页的音频动效（内置是『音乐动效』的四种样式）能交给插件：注册 UI 项
/// `musicxx.ui.playing.visualizer` 后，它与内置样式在**同一份列表**里选
/// （设置 → 主题 →「播放页音频动效」）。
///
/// 三种形态与播放页歌曲图一样：`shader`（本示例）/ `view`（用声明式界面逐帧动）/ `none`。
/// 本示例用预编译的 `shader/viz.shaderbundle` 画 4 根跟着低频带起伏的柱子 + 底部呼吸光晕，
/// 频谱用 `musicxx.spectrum.*` 来源现读（没有数据时看着色器里的 `uEnv.z`）。
///
/// 注意：
/// - 动效区**不接手势**（封面点击、拖动切歌、迷你歌词要继续可用），需要交互的功能别放这里；
/// - 没安装『音乐动效』时插件项仍可选，但频谱全是 0（没有来源）；
/// - 它只是一个候选：用户没选就不渲染，选中 / 切回"不显示"都走 `musicxx.ui.slot.select`。

/// 频谱来源的"快起慢落"声明（频谱是 10 帧/秒，直接取会有台阶感）
function smoothSpectrum(name) {
    return {
        kind: "smooth",
        attackMs: 20,
        releaseMs: 260,
        of: { kind: "source", name: name },
    };
}

/// 动效项的完整声明
///
/// 渲染输入的写法与背景样式同一套（`args` = 成员名 → 值表达式）；
/// 与背景不同的是这里**不默认给 4 个绘制色**，要颜色得自己声明。
function visualizerEntryData() {
    return {
        title: "示例频谱柱",
        depict: "4 根柱子跟着低频带起伏，底部光晕随响度呼吸（插件着色器绘制）",
        shader: { bundle: "shader/viz.shaderbundle" },
        args: {
            uColor1: { kind: "source", name: "musicxx.icon.themeMapping.0" },
            uColor2: { kind: "source", name: "musicxx.icon.themeMapping.2" },
            uLevel: smoothSpectrum("musicxx.spectrum.level"),
            uBands0: smoothSpectrum("musicxx.spectrum.bands.0"),
        },
        // 时间推进速度与帧率：动效只要 30 fps 就够（再高看不出差别，白烧 GPU）
        speed: 1,
        maxFps: 30,
        animate: true,
    };
}

/// 选中本插件的动效项
function usePluginVisualizer() {
    musicxx.ui.slot.select(VISUALIZER_ITEM_ID, { slot: VISUALIZER_SLOT_ID }).then(function (r) {
        musicxx.host.log(2, "选中插件音频动效: " + JSON.stringify(r || {}));
    }, function (err) {
        musicxx.host.log(3, "选中插件音频动效失败: " + errText(err));
    });
}

/// 切回"不显示"（内置动效里的空选项）
function useNoVisualizer() {
    musicxx.ui.slot.select(NONE_VISUALIZER_ID, { slot: VISUALIZER_SLOT_ID }).then(function (r) {
        musicxx.host.log(2, "切回不显示音频动效: " + JSON.stringify(r || {}));
    }, function (err) {
        musicxx.host.log(3, "切回不显示音频动效失败: " + errText(err));
    });
}

/// 动效槽位的状态（快照频道同步读）
function visualizerSlot() {
    const slots = musicxx.broadcast.read("musicxx.state.uiSlots") || {};
    return slots[VISUALIZER_SLOT_ID] || null;
}

/// 动效槽位的状态文本（设置页 / 说明页的状态行）
function visualizerStateText() {
    const slot = visualizerSlot();
    if (!slot || Object.keys(slot).length === 0) {
        return "无状态";
    }
    const current = (typeof slot.itemId === "string") ? slot.itemId : "";
    if (current === VISUALIZER_ITEM_ID) {
        return (slot.visible === true) ? "本插件动效生效中" : "本插件动效（未在渲染）";
    }
    const selected = (typeof slot.selectedId === "string") ? slot.selectedId : "";
    if (selected === VISUALIZER_ITEM_ID) {
        return "已选中 · 等待生效";
    }
    if (selected === "" || selected === NONE_VISUALIZER_ID ||
        selected.indexOf("musicxx.") === 0) {
        return "内置/不显示";
    }
    return "其它插件项";
}
