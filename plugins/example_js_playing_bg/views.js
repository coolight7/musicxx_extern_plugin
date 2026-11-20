/// example_js_playing_bg 的页面脚本: 说明页 / 设置页 / 频谱页
///
/// 插件不写界面代码：页面是"块描述"（文本 / 行 / 分组 / 按钮 / 内联 shader 块），
/// 由客户端按自己的能力渲染（`kit` 是随插件分发的界面 kit）。
///
/// - `card`: 说明页（主页入口指向它）；
/// - `settings`: 设置页（六种样式的参数、开关与操作）；
/// - `spectrumProbe`: 频谱页（快照频道 + 异步动作 + 内联 Shader 块 + 动画块示例）。
///
/// 处理器一律**同步返回**页面：要读实时数据读快照频道，动作结果异步读、读完记下来回读
/// （处理器里等动作会与调用方互锁，只能等到超时）。

/// 界面 kit（基础 kit + musicxx 扩展 kit，随插件目录分发）
const kit = pluginxx.ui.kit;

/// 卡片现在只收一块内容：多行先包一层 Column（`kit.card` 的 `child` 是单个块）
function columnOf(children) {
    return { kind: "Column", children: children };
}

/// 客户端能力段（插件框架取页面时会放进参数）
function viewEnv(args) {
    if (args && typeof args.ui === "object" && args.ui !== null) {
        return args.ui;
    }
    return null;
}

/* ==================== 槽位状态文字 ==================== */

/// 当前生效的样式：`blur` / `card` / … 之一，或者 `builtin` / `other`
///
/// 已经在画的看 `itemId`（现在由谁在画），还没生效的看 `selectedId`（用户选中了谁）：
/// 切换要经过一次动作往返，选中后到生效之间用后者，按钮才算"点得动、有反应"。
function currentStyle() {
    const slot = slotMirrorOf(BACKGROUND_SLOT) || {};
    const itemStyle = styleOfItem(slot.itemId || "");
    if (itemStyle) {
        return itemStyle;
    }
    const selectedStyle = styleOfItem(slot.selectedId || "");
    if (selectedStyle) {
        return selectedStyle;
    }
    const selected = slot.selectedId || "";
    if (selected === "" || selected.indexOf("musicxx.") === 0) {
        return STYLE_BUILTIN;
    }
    return STYLE_OTHER;
}

/// 背景槽位的状态文本：是不是本插件在画 / 选中了本插件但还没渲染 / 内置 / 别人的样式
///
/// 镜像里的 `selectedId` 是"用户选中的是谁"（可能是 `musicxx.*` 内置项），`itemId` 是
/// "现在由哪个插件项在画"（没有插件项生效时为空）。两个字段分开读，才能既说清"内置背景"，
/// 又说清本插件到底有没有在画。
function backgroundStateText() {
    const slot = slotMirrorOf(BACKGROUND_SLOT) || {};
    const selected = slot.selectedId || "";
    const current = slot.itemId || "";
    // 镜像还没反映这次请求（动作还在往返）时先显示"已请求"；反映之后标记就没用了
    if (lastBackgroundRequest !== "" && selected !== lastBackgroundRequest) {
        return "已请求";
    }
    lastBackgroundRequest = "";
    if (styleOfItem(current) !== "") {
        return "生效中";
    }
    if (styleOfItem(selected) !== "") {
        return "已选中";
    }
    if (selected.indexOf("musicxx.") === 0) {
        return "内置背景";
    }
    return selected === "" ? "无状态" : ("其它插件: " + selected);
}

/// 正在画的是哪一套样式（本插件的短名 / 内置背景 / 别人的样式）
function styleTextOfCurrent() {
    const slot = slotMirrorOf(BACKGROUND_SLOT) || {};
    const current = styleOfItem(slot.itemId || "");
    if (current !== "") {
        return STYLE_SHORT[current];
    }
    const selected = styleOfItem(slot.selectedId || "");
    if (selected !== "") {
        return STYLE_SHORT[selected] + "（未渲染）";
    }
    const id = slot.selectedId || "";
    if (id.indexOf("musicxx.") === 0) {
        return "内置背景";
    }
    return id === "" ? "无状态" : id;
}

/// 本插件的渲染状态（尺寸）
///
/// `itemId` 是本插件时才有渲染可言：`visible:false` 表示主程序停止渲染（播放页被遮挡、
/// 切到后台，或者播放页当前根本不在页面上），插件可以据此停掉自己的耗时工作。
/// 槽位状态里不再有 `animate` / `maxFps`：它们是每个着色器块自己的字段。
function renderStateText() {
    const slot = slotMirrorOf(BACKGROUND_SLOT) || {};
    if (styleOfItem(slot.itemId || "") === "") {
        return "未生效";
    }
    if (slot.visible === false) {
        return "未在渲染";
    }
    const size = (slot.width && slot.height) ? (slot.width + "x" + slot.height) : "—";
    return "渲染中, " + size;
}

/// 歌曲图槽位的状态文本
function iconStateText() {
    const slot = slotMirrorOf(ICON_SLOT) || {};
    if (!slot || Object.keys(slot).length === 0) {
        return "无状态";
    }
    if (slot.itemId === ITEM_ID_ICON) {
        return (slot.visible === true) ? "已隐藏" : "已隐藏 · 未渲染";
    }
    if (slot.selectedId === ITEM_ID_ICON && slot.itemId !== ITEM_ID_ICON) {
        return "检查中";
    }
    // 没有选中项、或选中项是内置显示（`musicxx.circle` / `musicxx.square` /
    // `musicxx.adaptive` 这些形状）时，画的是主程序自己的歌曲图
    if (!slot.selectedId || slot.selectedId.indexOf(BUILTIN_ICON_PREFIX) === 0) {
        return "显示中";
    }
    return "别的样式";
}

/* ==================== 通用排版 ==================== */

/// 一行「标题 + 说明 + 右侧状态」
///
/// 两侧都放进 `Expanded`（左 3 右 2），文字各自在给定宽度里换行 —— 不论状态文字多长
/// 都不会溢出（`kit.listRow` 的右侧文本没有宽度约束，长文本会把整行撑出可见区域）。
function infoRow(title, depict, value) {
    const left = {
        kind: "Expanded",
        flex: 3,
        child: {
            kind: "Column",
            gap: 2,
            children: [
                { kind: "Text", text: title },
                { kind: "Text", text: depict, type: "caption", color: "hint" },
            ],
        },
    };
    const children = [left];
    if (value) {
        children.push({
            kind: "Expanded",
            flex: 2,
            child: {
                kind: "Text",
                text: value,
                type: "caption",
                color: "hint",
                align: "end",
            },
        });
    }
    return {
        kind: "Block",
        variant: "plain",
        padding: { horizontal: 10, vertical: 6 },
        child: {
            kind: "Row",
            gap: 10,
            cross: "center",
            children: children,
        },
    };
}

/// 开关行：标题 + 说明在左（可换行），开关在右
///
/// 为什么不用 kit 的 `switchRow`：它把 `Control` 当成外层 `Row` 的右端元素，而"行内控件"在
/// 宽度无约束时会踩渲染层的 `RenderFlex children have non-zero flex but incoming width
/// constraints are unbounded`（`Row` 给非弹性子元素的就是无限宽，`Expanded` 不能这么放）。
/// 这里给控件一个**固定宽度**的盒子：控件在盒子里定宽绘制，界面表现一致、也不会断言。
/// 宽度取 `XXSwitchBool` 自己的尺寸（190×100 设计像素，是定宽控件）。
const SWITCH_BOX_WIDTH = 190;

function switchControl(id, title, depict, value) {
    return {
        kind: "Block",
        variant: "plain",
        padding: { horizontal: 10, vertical: 6 },
        child: {
            kind: "Row",
            gap: 12,
            cross: "center",
            children: [
                {
                    kind: "Expanded",
                    flex: 3,
                    child: {
                        kind: "Column",
                        gap: 2,
                        children: [
                            { kind: "Text", text: title },
                            { kind: "Text", text: depict, type: "caption", color: "hint" },
                        ],
                    },
                },
                {
                    kind: "SizedBox",
                    width: SWITCH_BOX_WIDTH,
                    child: {
                        kind: "Control",
                        control: "switch",
                        id: id,
                        value: value,
                        action: { kind: "call", name: "setOption" },
                    },
                },
            ],
        },
    };
}

/// 互斥的几选一：标题 + 说明 + 一排按钮（当前项高亮）
///
/// 用 `Button` 块（各自声明 `{id, value}` 参数）而不是 `Control(buttons)`：按钮块撑满可用宽度
/// （`width: double.infinity`），窄窗口里也放得下。
function choiceControl(title, help, id, current, viewId, options) {
    return {
        kind: "Block",
        variant: "plain",
        padding: { horizontal: 10, vertical: 6 },
        child: {
            kind: "Column",
            gap: 6,
            children: [
                { kind: "Text", text: title },
                { kind: "Text", text: help, type: "caption", color: "hint" },
                {
                    kind: "Row",
                    gap: 12,
                    children: options.map(function (option) {
                        return {
                            kind: "Expanded",
                            child: {
                                kind: "Button",
                                label: option.label,
                                // 当前项用主按钮的样子（与候选项按钮控件的选中态一致）
                                highlight: option.value === current,
                                action: {
                                    kind: "call",
                                    name: "setOption",
                                    args: {
                                        id: id,
                                        value: option.value,
                                        view: viewId,
                                    },
                                },
                            },
                        };
                    }),
                },
            ],
        },
    };
}

/// 样式选择（说明页与设置页共用）：六种样式各一个按钮 + 内置背景
///
/// [viewId] 决定动作返回哪一页去刷新（原地刷新当前页）。
function modeChoiceControl(viewId) {
    const style = currentStyle();
    function buttonOfItem(itemId, label) {
        return {
            kind: "Expanded",
            child: {
                kind: "Button",
                label: label,
                highlight: styleOfItem(itemId) === style,
                action: { kind: "call", name: "useBackground", args: { id: itemId, view: viewId } },
            },
        };
    }
    return {
        kind: "Block",
        variant: "plain",
        padding: { horizontal: 10, vertical: 6 },
        child: {
            kind: "Column",
            gap: 6,
            children: [
                { kind: "Text", text: "封面类（画面由当前歌曲封面做出来）" },
                {
                    kind: "Row",
                    gap: 12,
                    children: [
                        buttonOfItem(ITEM_ID_BLUR, "模糊"),
                        buttonOfItem(ITEM_ID_CARD, "沉浸"),
                        buttonOfItem(ITEM_ID_VINYL, "黑胶"),
                    ],
                },
                { kind: "Text", text: "图案类（画面由着色器画出来）" },
                {
                    kind: "Row",
                    gap: 12,
                    children: [
                        buttonOfItem(ITEM_ID_LATTICE, "晶格"),
                        buttonOfItem(ITEM_ID_RING, "光圈"),
                        buttonOfItem(ITEM_ID_BOUND, "插件纹理铺底"),
                    ],
                },
                {
                    kind: "Row",
                    gap: 12,
                    children: [{
                        kind: "Expanded",
                        child: {
                            kind: "Button",
                            label: "内置背景",
                            highlight: (STYLE_BUILTIN === style),
                            action: {
                                kind: "call",
                                name: "useBackground",
                                args: { id: BUILTIN_BG_ID, view: viewId },
                            },
                        },
                    }],
                },
            ],
        },
    };
}

/// 两个页面共用的状态区（样式 / 渲染 / 歌曲图 / 取色 / 前景色 / 最近一次操作）
function statusRows() {
    return [
        kit.listRow({
            title: "播放页背景",
            subtitle: "渲染槽位 player.background（快照频道同步读）",
            trailing: backgroundStateText(),
        }, viewEnv(null)),
        infoRow(
            "当前样式",
            "生效中 = 本插件在画；『未渲染』表示选中了但播放页没打开",
            styleTextOfCurrent()),
        infoRow(
            "渲染状态",
            "页面被遮挡或切到后台时主程序会停止渲染",
            renderStateText()),
        infoRow(
            "播放页歌曲图",
            "渲染槽位 player.icon：接管后保留占位、不画内容",
            iconStateText()),
        infoRow(
            "封面取色",
            "钩子 musicxx.media.palette.analyse：统计封面里出现最多的 4 个色点（" +
            paletteDetailText() + "）；『沉浸』的渐变底与『黑胶』的底色用的就是它写的颜色",
            paletteStateText()),
        infoRow(
            "自动调整前景色",
            "按封面主色 + 亮暗主题推播放页的三色（普通色 / 减淡色 / 块状背景色），" +
            "保留主色色相、按当前样式估背景亮度保证对比度；本插件的背景被选中时生效，" +
            "换歌 / 换样式 / 主题切换后重新推",
            foregroundStateText()),
        infoRow(
            "最近一次操作",
            lastLinkNote || "（无）",
            ""),
    ];
}

/// 频谱状态文字（读快照频道 `musicxx.state.spectrum`，播放中约 10 Hz 推送）
///
/// `available` 为假时 `level` / `bands` 都是 0，用 `status` 说明原因
/// （"正在提取" / "未启用音乐动效" / ...）。不要按数值判断"有没有数据"。
function spectrumStateText() {
    const s = musicxx.broadcast.read("musicxx.state.spectrum");
    if (!s) {
        return "无状态";
    }
    if (s.available === true) {
        const level = (typeof s.level === "number" && isFinite(s.level)) ? s.level : 0;
        return "有数据（响度 " + Math.round(level * 100) + "%）";
    }
    switch (s.status) {
        case "loading":
            return "正在提取";
        case "off":
            return "『音乐动效』未启用";
        case "unavailable":
        default:
            return "无数据";
    }
}

/* ==================== 设置页 ==================== */

/// 设置页：状态、样式选择、参数（封面三种 + 图案三种）、开关与操作
function settingsView(args) {
    const env = viewEnv(args);
    return {
        title: PLUGIN_TITLE + " · 设置",
        subtitle: "六种背景样式的参数与开关；值保存在插件目录的 config.json",
        blocks: [
            kit.hint({
                text: "• 本插件注册了六种播放页背景样式：『" + STYLE_TITLES[STYLE_BLUR] + "』、『" +
                    STYLE_TITLES[STYLE_CARD] + "』、『" + STYLE_TITLES[STYLE_VINYL] + "』与『" +
                    STYLE_TITLES[STYLE_LATTICE] + "』、『" + STYLE_TITLES[STYLE_RING] + "』、『" +
                    STYLE_TITLES[STYLE_BOUND] + "』。它们占同一个槽位 player.background，" +
                    "「设置 → 主题 → 播放页面背景」里同时只能选中一个。",
            }, env),
            kit.card({ child: columnOf(statusRows()) }, env),
            kit.card({
                title: "样式",
                child: columnOf([modeChoiceControl(SETTINGS_VIEW_ID)]),
            }, env),
            kit.card({
                title: "参数",
                child: columnOf([
                    choiceControl("模糊档位（『模糊』）", "封面纹理的预模糊强度：越大越糊，14 还能看出封面内容",
                        "blur", configuredBlur(), SETTINGS_VIEW_ID,
                        BLUR_CHOICES.map(function (value) {
                            return { value: value, label: String(value) };
                        })),
                    choiceControl("背景放大倍数（" + ZOOM_MIN + "~" + ZOOM_MAX + "）",
                        "『模糊』与『沉浸』共用：数值越大画面越局部（1 = 封面铺满、内容最完整）",
                        "zoom", configuredZoom(), SETTINGS_VIEW_ID,
                        ZOOM_CHOICES.map(function (value) {
                            return { value: value, label: value + "×" };
                        })),
                    choiceControl("图片占比（『沉浸』）",
                        "图片至少占屏幕的比例：越大图片越多、渐变底越少",
                        "share", configuredShare(), SETTINGS_VIEW_ID,
                        SHARE_CHOICES.map(function (value) {
                            return { value: value, label: Math.round(value * 100) + "%" };
                        })),
                    choiceControl("唱片转速（『黑胶』）",
                        "盘心的封面转一圈的快慢：1× 是 " + VINYL_SPIN_SECONDS + " 秒一圈",
                        "spin", configuredSpin(), SETTINGS_VIEW_ID,
                        SPIN_CHOICES.map(function (value) {
                            return { value: value, label: value + "×" };
                        })),
                    kit.settingRow({
                        title: "背景动画速率",
                        depict: "『晶格』『光圈』『插件纹理铺底』的时间推进速度" +
                            "（1x = 基准速度 1，可选 0.5x / 1x / 2x），改完立即生效",
                        value: bgRateText(configuredBgRateValue()),
                    }, env),
                    kit.button({
                        label: "切换背景动画速率（0.5x / 1x / 2x）",
                        variant: "full",
                        highlight: true,
                        action: { kind: "call", name: "cycleBackgroundRate", args: { view: SETTINGS_VIEW_ID } },
                    }, env),
                    kit.settingRow({
                        title: "插件纹理",
                        depict: "把当前歌曲封面绑成纹理（『" + STYLE_TITLES[STYLE_BOUND] + "』的底层就是它）",
                        value: boundImageState,
                    }, env),
                    kit.settingRow({
                        title: "封面变化通知",
                        depict: "注册处理器后收到 `musicxx.media.cover.changed`（只在封面真的换了时派发；" +
                            "载荷里没有图片字节）。取色预热也用这条频道",
                        value: coverHookBound ? "已绑定" : "已移除",
                    }, env),
                ]),
            }, env),
            kit.card({
                title: "开关",
                child: columnOf([
                    switchControl("ripple", "热浪扭曲波纹",
                        "关掉后『模糊』只剩缓慢漂移、『沉浸』完全静止",
                        configuredRipple()),
                    switchControl("hideIcon", "隐藏歌曲图",
                        "默认关。打开后，背景是本插件的样式时接管播放页中间的歌曲图" +
                        "（保留占位、不显示内容）",
                        configuredHideIcon()),
                    switchControl("autoForeground", "自动调整前景色",
                        "默认开。按封面取色的主色 + 亮暗主题推一套播放页前景" +
                        "（白昼偏深、夜间偏浅，保留主色的色相，按当前样式估出的背景亮度" +
                        "保证对比度）；关掉就用主题默认前景色",
                        configuredAutoForeground()),
                ]),
            }, env),
            kit.card({
                title: "操作",
                child: columnOf([
                    kit.button({
                        label: "把当前封面绑成纹理",
                        variant: "full",
                        action: { kind: "call", name: "bindCoverTexture", args: { view: SETTINGS_VIEW_ID } },
                    }, env),
                    kit.button({
                        label: "释放绑定的纹理",
                        variant: "full",
                        action: { kind: "call", name: "unbindCoverTexture", args: { view: SETTINGS_VIEW_ID } },
                    }, env),
                    kit.button({
                        label: coverHookBound ? "移除封面变化处理器" : "绑定封面变化处理器",
                        variant: "full",
                        action: { kind: "call", name: "toggleCoverHook", args: { view: SETTINGS_VIEW_ID } },
                    }, env),
                    kit.button({
                        label: "手动隐藏歌曲图（10 分钟内不自动跟随）",
                        variant: "full",
                        action: { kind: "call", name: "takeIcon", args: { view: SETTINGS_VIEW_ID } },
                    }, env),
                    kit.button({
                        label: "交还歌曲图（恢复上一个形状）",
                        variant: "full",
                        action: { kind: "call", name: "releaseIcon", args: { view: SETTINGS_VIEW_ID } },
                    }, env),
                    kit.button({
                        label: "清空取色缓存",
                        variant: "full",
                        action: { kind: "call", name: "clearPaletteCache", args: { view: SETTINGS_VIEW_ID } },
                    }, env),
                    kit.button({
                        label: "打开说明页",
                        variant: "full",
                        action: { kind: "route", route: "ext://" + PLUGIN_ID + "/" + HOME_VIEW_ID },
                    }, env),
                ]),
            }, env),
        ],
    };
}

/* ==================== 说明页 ==================== */

/// 说明页（主页入口）：六种样式各是什么样、怎么选、歌曲图与前景色怎么跟着变
function cardView(args) {
    const env = viewEnv(args);
    return {
        title: PLUGIN_TITLE,
        subtitle: "六种播放页背景 + 歌曲图接管 + 封面取色 + 自动前景色",
        blocks: [
            kit.hint({
                text: "• 本插件把六种播放页背景样式用块树注册（UI 项的 `view` 里一棵 `musicxx.Shader` 块）：" +
                    "用户在「设置 → 主题 → 播放页面背景」里选中一种后生效（未选中时插件框架不读 bundle、" +
                    "不分析封面，零成本）。背景就是一棵块树，想叠文字 / 图片就多放几个块。",
            }, env),
            kit.hint({
                text: "• 封面类三种 —— 『" + STYLE_TITLES[STYLE_BLUR] + "』：" + STYLE_DEPICTS[STYLE_BLUR] +
                    "；『" + STYLE_TITLES[STYLE_CARD] + "』：" + STYLE_DEPICTS[STYLE_CARD] +
                    "；『" + STYLE_TITLES[STYLE_VINYL] + "』：" + STYLE_DEPICTS[STYLE_VINYL] + "。",
            }, env),
            kit.hint({
                text: "• 图案类三种 —— 『" + STYLE_TITLES[STYLE_LATTICE] + "』：" + STYLE_DEPICTS[STYLE_LATTICE] +
                    "；『" + STYLE_TITLES[STYLE_RING] + "』：" + STYLE_DEPICTS[STYLE_RING] +
                    "；『" + STYLE_TITLES[STYLE_BOUND] + "』：" + STYLE_DEPICTS[STYLE_BOUND] +
                    "。『晶格』与『插件纹理铺底』用同一个 bundle，区别就是底层纹理来自当前歌曲封面" +
                    "（`args` 里的 `musicxx.cover`）还是插件自己绑定的图片（`musicxx.image.<key>`）。",
            }, env),
            kit.hint({
                text: "• 『光圈』的声明用了频谱来源：`spectrum.level`（响度）与 `spectrum.bands64.0..15`" +
                    "（64 个频带，每项 4 个写在 xyzw）。没有数据时插件框架写 0，尖角长度为 0、" +
                    "只剩圆心光源与底色；要读封面字节（而不是采样）用 `musicxx.media.cover` 动作。",
            }, env),
            kit.hint({
                text: "• 『自动调整前景色』（默认开）按封面取色的主色推一套播放页前景：" +
                    "保留主色的色相与彩色程度（文字看着就是主色的深浅版），明度按亮暗主题给倾向；" +
                    "对比度按当前样式估出的背景亮度算，要求逐条放宽 —— 先争取高对比 + 明显的颜色，" +
                    "不够就依次让一点对比度，最后才让颜色。换歌、换样式与主题切换后用能力 " +
                    "`musicxx.ui.playing.foreground.set` 交给应用，关掉就交还主题默认前景色。",
            }, env),
            kit.hint({
                text: "• 封面取色由本插件提供（钩子 musicxx.media.palette.analyse）：把封面缩到 " +
                    PALETTE_COVER_SIZE + "×" + PALETTE_COVER_SIZE + " 读 RGBA，按 4 bit/通道统计出现次数最多的 " +
                    PALETTE_TOP_COLORS + " 个色点。它替代内置的封面颜色分析，内置背景与插件背景都会用上这套颜色。",
            }, env),
            kit.card({ child: columnOf(statusRows()) }, env),
            kit.card({
                title: "选择样式",
                child: columnOf([modeChoiceControl(HOME_VIEW_ID)]),
            }, env),
            kit.button({ label: "刷新本页", variant: "full", highlight: true, action: "card" }, env),
            kit.button({
                label: "读取当前频谱（镜像 + 异步动作）",
                variant: "full",
                action: { kind: "call", name: "spectrumProbe", args: { view: "card" } },
            }, env),
            kit.button({
                label: "打开设置页",
                variant: "full",
                highlight: true,
                action: { kind: "route", route: "ext://" + PLUGIN_ID + "/" + SETTINGS_VIEW_ID },
            }, env),
        ],
    };
}

/* ==================== 频谱页 ==================== */

/// 频谱动作结果的说明文本（动作 `musicxx.media.spectrum` 的返回）
///
/// 没有数据不是错误：结果里用 `status` / `available` / `loading` 说明原因，
/// 这时 `level` 与 `bands` 都是 0，但数组长度仍按请求给（结构稳定，不用判键在不在）。
function spectrumActionText(s, err) {
    if (err) {
        return "读取失败: " + err;
    }
    const bands = (s.bands || []).map(function (v) {
        return String(Math.round(v * 100));
    }).join(" / ");
    const where = (typeof s.positionMs === "number" && typeof s.durationMs === "number")
        ? ("，位置 " + Math.round(s.positionMs / 1000) + "s / " + Math.round(s.durationMs / 1000) + "s")
        : "";
    const head = "status = " + s.status
        + "，available = " + (s.available === true)
        + "，unit = " + s.unit
        + "，bandCount = " + s.bandCount
        + "，频带(%) = " + bands
        + where;
    return (s.reason ? (head + "（原因: " + s.reason + "）") : head);
}

/// 最近一次异步读取到的动作结果（还没读回 / 失败都在这里说明）
///
/// 为什么不在处理器里 `await` 动作：主程序打开页面、点按钮都是**同步进插件框架**调用这个
/// 能力（主程序线程全程等插件框架返回），而动作要由应用线程执行 —— 处理器里等动作就会与
/// 调用方互相等下去，最后只能等到超时。所以这一页当场用快照频道画，动作结果异步读、
/// 读完记下来回读。
let spectrumActionState = { pending: 0, text: "", error: "" };

/// 异步读一次动作结果（不等待；读完记进 spectrumActionState，由页面回读）
function refreshSpectrumAction() {
    spectrumActionState.pending = 1;
    musicxx.media.spectrum({ bandCount: 8, unit: "normalized" }).then(function (s) {
        spectrumActionState = {
            pending: 0,
            text: spectrumActionText(s, null),
            error: "",
        };
        musicxx.host.log(2, "动作 musicxx.media.spectrum 返回: " + spectrumActionState.text);
    }, function (err) {
        spectrumActionState = { pending: 0, text: "", error: err.message };
        musicxx.host.log(3, "动作 musicxx.media.spectrum 失败: " + err.message);
    });
}

/// 动作结果那一行的说明（第一次打开时通常已经有值：插件加载时就发起过一次读取）
function spectrumActionResultText() {
    if (spectrumActionState.error !== "") {
        return "读取失败: " + spectrumActionState.error;
    }
    if (spectrumActionState.pending === 1) {
        return "读取中（动作已发起，读回后下一次刷新这一页就能看到）";
    }
    if (spectrumActionState.text === "") {
        return "还没读回结果（点按钮会发起读取，读回后再点一次就能看到）";
    }
    return spectrumActionState.text;
}

/// 动作结果那一行的短状态（具体内容看下面的说明）
function spectrumActionShortText() {
    if (spectrumActionState.pending === 1) {
        return "读取中…";
    }
    if (spectrumActionState.error !== "") {
        return "读取失败";
    }
    return (spectrumActionState.text === "") ? "还没有结果" : "已读回";
}

/// 频谱探测页: 处理器**同步返回** (镜像当场可用, 动作结果取自上一次异步读取)
function spectrumProbeView(args) {
    const env = viewEnv(args);
    return {
        title: "当前音频频谱",
        subtitle: "数据来自内置『音乐动效』插件提取的频谱",
        blocks: [
            kit.listRow({
                title: "快照频道",
                subtitle: "musicxx.state.spectrum（同步读，播放中约 10 Hz 刷新）",
                trailing: spectrumStateText(),
            }, env),
            kit.listRow({
                title: "动作结果",
                subtitle: "musicxx.media.spectrum（异步读取，这一行是最近一次读回的结果）",
                trailing: spectrumActionShortText(),
            }, env),
            kit.hint({ text: "动作结果: " + spectrumActionResultText() }, env),
            kit.hint({
                text: "• 没有数据时 status 是 loading（正在提取）/ off（未启用音乐动效）/ " +
                    "unavailable（来源不是本地或缓存、时长超限），这时值恒为 0，请按 status 判断。",
            }, env),
            kit.hint({
                text: "• 这一页也演示了『能力处理器里不能等动作』这条规则：要实时数据读快照频道，" +
                    "要动作结果就异步读、把结果记下来回读。",
            }, env),
            // 动画示例：值表达式 + 动画块（musicxx.AnimatedBuilder / SizeTransition / FadeTransition）
            //
            // 一次声明就够：通道表里定义"这一页用到的几个值"，子树里的字段用
            // {"kind":"source","name":...} 读它们；插件框架每帧求值并只重建读到值的块。
            kit.card({
                title: "动画示例",
                child: columnOf([
                    kit.hint({
                        text: "下面这块由 musicxx.AnimatedBuilder 驱动：高度跟着响度呼吸（smooth + lfo），" +
                            "进场先淡入再撑开（tween 驱动 SizeTransition / FadeTransition）。" +
                            "没有频谱数据时高度取 lfo 的下限，所以画面仍然在动。",
                    }, env),
                    {
                        kind: "musicxx.AnimatedBuilder",
                        maxFps: 30,
                        values: {
                            // 进场进度：打开这一页后从 0 到 1（once，停在终点）
                            "demo.intro": {
                                kind: "tween",
                                from: 0,
                                to: 1,
                                durationMs: 900,
                                ease: "outCubic",
                            },
                            // 呼吸高度：响度（快起慢落）× 缓慢摆动，取不到频谱时是 60~90 的摆动
                            "demo.h": {
                                kind: "mul",
                                of: [
                                    {
                                        kind: "smooth",
                                        attackMs: 20,
                                        releaseMs: 260,
                                        of: { kind: "source", name: "musicxx.spectrum.level" },
                                    },
                                    {
                                        kind: "lfo",
                                        shape: "sine",
                                        periodMs: 2600,
                                        from: 60,
                                        to: 90,
                                    },
                                ],
                            },
                        },
                        child: {
                                kind: "musicxx.FadeTransition",
                                value: { kind: "source", name: "demo.intro" },
                                child: {
                                        kind: "musicxx.SizeTransition",
                                        axis: "vertical",
                                        value: { kind: "source", name: "demo.intro" },
                                        child: {
                                                kind: "SizedBox",
                                                height: { kind: "source", name: "demo.h" },
                                                child: { kind: "Text", text: "跟着音乐呼吸的一块（高度由通道驱动）", type: "caption", color: "hint" },
                                            },
                                    },
                            },
                    },
                    kit.hint({
                        text: "• 值表达式也能写在普通字段上（尺寸 / 进度 / 文本 / visible）。" +
                            "节点表与来源见 docs/plugin-shader-bundle.md §7。",
                    }, env),
                ]),
            }, env),
            // 页面里也能直接画一块着色器（`musicxx.Shader` 块）：用 `SizedBox` 给它确定的高度
            kit.hint({ text: "• 下面两块是页面内联的 Shader 块：上面是晶格，下面是光圈（跟着频谱起伏）：" }, env),
            kit.card({
                child: columnOf([
                    {
                        kind: "SizedBox", height: 300, child: {
                                                           kind: "Column",
                                                           children: [
                                kit.shaderBlock({
                                    bundle: "shader/bg.shaderbundle",
                                    speed: 1,
                                    maxFps: 16,
                                    args: {
                                        // 颜色来源：主题色 + 封面提取色（取不到时用 fallback）
                                        uColor1: { kind: "source", name: "musicxx.theme.primary" },
                                        uColor2: { kind: "source", name: "musicxx.icon.main", convert: true, fallback: "#8899aa" },
                                        uColor3: { kind: "source", name: "musicxx.icon.dark", fallback: "#223344" },
                                        uColor4: { kind: "source", name: "musicxx.icon.themeMapping.3" },
                                    },
                                }, env),
                            ]
                                                       }
                    },
                ]),
            }, env),
            kit.card({
                child: columnOf([
                    {
                        kind: "SizedBox", height: 300, child: {
                                                           kind: "Column",
                                                           children: [
                                kit.shaderBlock({
                                    bundle: "shader/ring.shaderbundle",
                                    speed: 1,
                                    maxFps: 16,
                                    args: ringArgs(
                                        { kind: "source", name: "musicxx.theme.primary" },
                                        { kind: "source", name: "musicxx.icon.main", convert: true, fallback: "#8899aa" },
                                        { kind: "source", name: "musicxx.icon.dark", fallback: "#223344" },
                                        { kind: "source", name: "musicxx.icon.themeMapping.3" }
                                    ),
                                }, env),
                            ]
                                                       }
                    },
                ]),
            }, env),
            kit.button({
                label: "重新读取（镜像实时 + 发起一次动作读取）",
                variant: "full",
                highlight: true,
                action: { kind: "call", name: "spectrumProbe", args: { view: "spectrum" } },
            }, env),
            kit.button({
                label: "返回说明页",
                action: { kind: "route", route: "ext://" + PLUGIN_ID + "/" + HOME_VIEW_ID },
            }, env),
        ],
    };
}

/// 动作从哪个页面发起就返回哪个页面（插件框架用返回值直接刷新当前页）
function pageView(args) {
    return (args && args.view === HOME_VIEW_ID) ? cardView(args) : settingsView(args);
}
