/// musicxx 外部插件示例 (JS, 零编译): 自定义播放页面背景 + shader 动画速率
///
/// 这个插件只演示"插件渲染槽位"这一条链路, 内容就两件事:
/// - 声明播放页背景样式 (`musicxx.ui.playing.background`), 本插件给了两种可选样式:
///   『示例晶格背景』(`shader/bg.shaderbundle`) 与『光圈』(`shader/ring.shaderbundle`,
///   频谱圆环: 圆圈是基线, 圆上左右对称的频带尖角跟着音乐向内/向外突出, 相邻尖角之间、
///   同一个尖角的内外沿之间都用直线相连 —— 整圈是一张长在圆上的"蛛网")。
///   画面都由预编译好的 shader bundle 画 (插件不写界面代码), 用户在
///   『设置 → 播放页面背景』里选中后才生效; 未选中时零成本 (宿主不读 bundle、不分析封面)。
/// - 把动画速率做成插件自己的设置项 (0.5x / 1x / 2x), 改完立即生效 (两种样式一起改)。
/// - 频谱的两条读法 (『读取当前频谱』按钮): 状态镜像 (`musicxx.state.spectrum`, 同步、实时)
///   与动作 (`musicxx.media.spectrum`, 异步)。动作**不能在能力处理器里等** —— 能力是同步
///   进宿主调用的, 处理器里 await 动作会与调用方互锁; 这里异步读、结果记下来回读。
///
/// 页面 (框架不管理插件设置入口, 入口由插件自己给):
/// - `ext://example_js_shader/card` (主页入口): 当前生效项与渲染状态, 一键切换背景;
/// - `ext://example_js_shader/settings`: 改速率 (值存在插件目录的 config.json);
/// - 频谱页 (`spectrumProbe` 能力): 镜像与动作结果 (见上面第三条)。
/// 页面都由脚本返回声明式内容 (用随插件分发的界面 kit 装配, 见 plugin.yaml 的 `scripts`),
/// 排版与控件由客户端渲染。
///
/// 这两件事原先和钩子/网络/页面示例一起放在 `example_js` 里, 2026-09 拆成独立插件,
/// 便于单独照抄。要看"内置背景样式换成插件渲染"的完整流程 (着色器写法、bundle 打包、
/// uniform 契约), 见 `docs/plugin-shader-bundle.md` 与 shader/ 目录。
///
/// 约束: 脚本顶层必须**同步**完成注册 (顶层不能用 await); 异步逻辑放到钩子或定时器里。

/// 界面 kit（基础 kit + musicxx 扩展 kit，随插件目录分发）
const kit = pluginxx.ui.kit;

/// 背景样式在设置列表里的名字与副标题
const BG_TITLE = "示例晶格背景";
const BG_DEPICT = "跟随封面配色与音乐律动的晶格化动态背景";

/// 第二个背景样式: 『光圈』(频谱圆环)
///
/// 画面 = 细亮线画的基线圆 + 圆上左右对称的 16 个频带尖峰 + 圆心光源:
/// 从正上方(12 点)往下, 角度位置对应频率由低到高, 左右两侧互为镜像,
/// 每个频带同时向圆外与圆内突出, 突出的高度就是这一帧的振幅。
const RING_TITLE = "光圈";
const RING_DEPICT = "圆圈是基线, 圆上左右对称的频带尖角跟着音乐向内/向外突出, 相邻尖角用直线连成蛛网";

/// 本插件背景样式的 UI 项短名与完整 id (完整 id 由宿主拼成 plugin.<插件id>.<短名>)
const BG_ITEM_NAME = "bg";
const BG_ITEM_ID = "plugin.example_js_shader." + BG_ITEM_NAME;
const RING_ITEM_NAME = "ring";
const RING_ITEM_ID = "plugin.example_js_shader." + RING_ITEM_NAME;

/// 样式全名 → 设置列表里的名字 (状态文字与页面按钮用它把 id 说成人话)
const BACKGROUND_TITLES = {};
BACKGROUND_TITLES[BG_ITEM_ID] = BG_TITLE;
BACKGROUND_TITLES[RING_ITEM_ID] = RING_TITLE;

/// 内置样式的候选 id (设置页与说明页的"切回内置背景"用它)
const BUILTIN_AUTO_ID = "builtin:Auto";

/// 基准速度与可选倍率
///
/// 声明给宿主的 `speed` = 基准速度 x 倍率, 宿主每帧直接用它推进时间 (不做二次缩放),
/// `speed` 最后会写进 uniform 的 `uParams.w`。基准速度定为 1
/// (2026-09 起把原先声明的 4 重新定义为 1x, 即整体降速到原来的 0.25x)。
const BG_BASE_SPEED = 1;
const BG_RATE_OPTIONS = [0.5, 1, 2];
const DEFAULT_BG_RATE = 1;

/// 把任意值归一到允许的倍率 (非法值回退 1x)
function normalizeBgRate(value) {
    const rate = Number(value);
    for (let i = 0; i < BG_RATE_OPTIONS.length; ++i) {
        if (BG_RATE_OPTIONS[i] === rate) {
            return rate;
        }
    }
    return DEFAULT_BG_RATE;
}

/// 倍率的显示文本 (1 → "1x"、0.5 → "0.5x")
function bgRateText(rate) {
    return String(normalizeBgRate(rate)) + "x";
}

/// 倍率 → 声明给宿主的时间推进速度
function bgSpeedOf(rate) {
    return BG_BASE_SPEED * normalizeBgRate(rate);
}

/// 频谱来源的"快起慢落"声明
///
/// 频谱数据是 10 帧/秒，直接取会有台阶感（每 100 ms 跳一次）：`smooth` 让值在
/// 跳变时快速跟上、回落时平滑过渡（`attackMs` 小、`releaseMs` 大 = 电平表手感）。
/// 宿主还会按播放位置在两帧频谱之间插值，两者叠加后画面才是连续的。
function smoothSpectrum(name) {
    return {
        kind: "smooth",
        attackMs: 20,
        releaseMs: 260,
        of: { kind: "source", name: name },
    };
}

/// 『示例晶格背景』的完整声明
///
/// `musicxx.ui.updateEntry` 是**整体替换**, 所以每次都要从这一个函数取完整 data,
/// 不能只传改动的字段。
///
/// `args` 只有一种写法: **成员名 → 值表达式**（`{kind: "...", ...}`，可以像 widget 一样
/// 嵌套）。这里的晶格背景用了三类节点: `source`（具名来源：主题色 `theme.*` / 封面提取色
/// `icon.*` / 映射后的 4 色 `icon.themeMapping.0..3` / 当前音频频谱 `spectrum.*`）、
/// `smooth`（过渡）、`mul` + `lfo`（把响度乘上一个缓慢的呼吸曲线）。
/// 不声明 `args` 时宿主默认给 `icon.themeMapping.0..3`。
function latticeBackgroundData(rate) {
    return {
        title: BG_TITLE,
        depict: BG_DEPICT,
        shader: { bundle: "shader/bg.shaderbundle" },
        args: {
            uColor1: { kind: "source", name: "musicxx.icon.themeMapping.0" },
            uColor2: { kind: "source", name: "musicxx.icon.themeMapping.1" },
            uColor3: { kind: "source", name: "musicxx.icon.themeMapping.2" },
            uColor4: { kind: "source", name: "musicxx.icon.themeMapping.3" },
            // 频谱（内置『音乐动效』提取的数据）: 每帧现读, 零成本
            // uLevel = 当前响度 × 呼吸曲线, uBands = 最低的 4 个频带, uBands2 = 第 8~11 个频带
            uLevel: {
                kind: "mul",
                of: [
                    smoothSpectrum("musicxx.spectrum.level"),
                    { kind: "lfo", shape: "sine", periodMs: 2600, from: 0.92, to: 1.0 },
                ],
            },
            uBands: smoothSpectrum("musicxx.spectrum.bands.0"),
            uBands2: smoothSpectrum("musicxx.spectrum.bands.2"),
        },
        speed: bgSpeedOf(rate),
        maxFps: 16,
        animate: true,
        foregroundStyle: "mask",
    };
}

/// 『光圈』的完整声明 (频谱圆环, 与上面同一套字段语义)
///
/// 与晶格背景的区别只在 bundle 与参数: 这个着色器要把 16 个频带全部映射到圆上,
/// 所以声明 `spectrum.bands.0..3` 四项 (每项 = 4 个连续频带, 低频在前, 写在 xyzw),
/// 再加一个亮色的线条色 (基线圆与蛛网边线用它画; 用 `const` 节点, 昼夜各一个值)。
/// 频带同样包一层 `smooth`: 尖角跟着音乐起伏时才不会一格一格地跳。
/// 没有频谱数据时宿主写 0, 圆上的尖角长度为 0, 只剩基线圆与圆心光源。
function ringBackgroundData(rate) {
    return {
        title: RING_TITLE,
        depict: RING_DEPICT,
        shader: { bundle: "shader/ring.shaderbundle" },
        args: {
            uColor1: { kind: "source", name: "musicxx.icon.themeMapping.0" },
            uColor2: { kind: "source", name: "musicxx.icon.themeMapping.1" },
            uColor3: { kind: "source", name: "musicxx.icon.themeMapping.2" },
            uColor4: { kind: "source", name: "musicxx.icon.themeMapping.3" },
            // 线条色: 没有来源, 昼夜各给一个固定值 (夜间稍暗, 免得抢前景文字)
            uLine: { kind: "const", value: "#f6faff", night: "#dbe7f7" },
            // 频谱: uLevel = 当前响度, uBands0..uBands3 = 16 个频带 (低频在前)
            uLevel: smoothSpectrum("musicxx.spectrum.level"),
            uBands0: smoothSpectrum("musicxx.spectrum.bands.0"),
            uBands1: smoothSpectrum("musicxx.spectrum.bands.1"),
            uBands2: smoothSpectrum("musicxx.spectrum.bands.2"),
            uBands3: smoothSpectrum("musicxx.spectrum.bands.3"),
        },
        speed: bgSpeedOf(rate),
        maxFps: 16,
        animate: true,
        foregroundStyle: "mask",
    };
}

let appliedBgRate = normalizeBgRate(DEFAULT_BG_RATE);

/// 应用背景动画速率: 运行期改自己的背景声明 (两种样式一起改)
///
/// 宿主收到后会刷新候选; 正在使用的背景立即用新速度 (不用重新选中, `speed` 每帧现算)。
/// 改 `maxFps` / `resolutionScale` 这类要换帧调度的字段则不在这里生效, 需要用户重新选中。
function applyBackgroundRate(rate) {
    const value = normalizeBgRate(rate);
    if (value === appliedBgRate) {
        return;
    }
    appliedBgRate = value;
    musicxx.ui.updateEntry(BG_ITEM_NAME, latticeBackgroundData(value));
    musicxx.ui.updateEntry(RING_ITEM_NAME, ringBackgroundData(value));
    musicxx.host.log(2, "背景动画速率 → " + bgRateText(value));
}

/// 注册播放页背景样式 (顶层先按 1x 声明; 读到 config.json 后再换成实际倍率)
///
/// 两种样式是同一个槽位里的两个候选: 注册顺序与权重决定设置列表里的先后
/// (晶格在前 order 20, 光圈在后 order 21), 用户可以任选其一, 也可以随时切回内置样式。
musicxx.ui.registerEntry({
    name: BG_ITEM_NAME,
    type: "playing.background",
    order: 20,
    data: latticeBackgroundData(appliedBgRate),
});

/// 『光圈』: 与晶格背景同一个槽位 (`musicxx.ui.playing.background`), 只是换一个 bundle
musicxx.ui.registerEntry({
    name: RING_ITEM_NAME,
    type: "playing.background",
    order: 21,
    data: ringBackgroundData(appliedBgRate),
});

/// 读状态镜像: 播放页背景槽位的当前状态 (同步读取, 不用等动作往返)
function backgroundSlot() {
    const slots = musicxx.state.get("musicxx.state.renderSlots") || {};
    return slots["player.background"] || {};
}

/// 最近一次"一键使用"请求的样式 id
///
/// 切换要经过一次动作往返 (musicxx.render.select), 页面却是先于动作结果画出来的:
/// 这里先记住请求, 让页面当场就能看到"点了有反应"; 镜像一反映这次请求就把它清掉。
let lastBackgroundRequest = "";

/// 当前生效项的文字说明
///
/// 镜像里的 `selectedId` 是"用户选中的是谁"(可能是 `builtin:*`), `itemId` 是"现在由哪个
/// 插件项在画"(没有插件项生效时为空)。两个字段分开读, 才既能说清"内置背景", 又能说清
/// 本插件到底有没有在画。
///
/// 本插件有两种样式, 这里说"是不是本插件在画"就够了; 具体是哪一种看 `backgroundStyleText`。
function backgroundStateText() {
    const slot = backgroundSlot();
    const selected = slot.selectedId || "";
    const current = slot.itemId || "";
    // 镜像还没反映这次请求(动作还在往返)时先显示"已请求"; 反映之后标记就没用了
    if (lastBackgroundRequest !== "" && selected !== lastBackgroundRequest) {
        return "已请求";
    }
    lastBackgroundRequest = "";
    if (ownStyleTitle(current) !== "") {
        return "生效中";
    }
    if (ownStyleTitle(selected) !== "") {
        return "已选中";
    }
    if (selected.indexOf("builtin:") === 0) {
        return "内置背景";
    }
    return selected === "" ? "无状态" : ("其它插件: " + selected);
}

/// 本插件的样式名 (UI 项全名 → 设置列表里的名字；不是本插件的样式返回空串)
function ownStyleTitle(id) {
    return BACKGROUND_TITLES[id] || "";
}

/// 正在画的样式名: 本插件的哪一种, 或者是内置/别人的样式
///
/// `backgroundStateText` 只说"是不是本插件在画", 两种样式共用一个说法,
/// 想知道到底是『示例晶格背景』还是『光圈』就看这一行。
function backgroundStyleText() {
    const slot = backgroundSlot();
    const current = ownStyleTitle(slot.itemId || "");
    if (current !== "") {
        return current;
    }
    const selected = ownStyleTitle(slot.selectedId || "");
    if (selected !== "") {
        return selected + "（未渲染）";
    }
    const id = slot.selectedId || "";
    if (id.indexOf("builtin:") === 0) {
        return "内置背景";
    }
    return id === "" ? "无状态" : id;
}

/// 本插件的渲染状态 (尺寸 / 是否在动)
///
/// `itemId` 是本插件时才有渲染可言: `visible:false` 表示宿主停止渲染(播放页被遮挡、
/// 切到后台, 或者播放页当前根本不在页面上), 插件可以据此停掉自己的耗时工作;
/// `animate:false` 表示这个样式只画一帧。
function renderStateText() {
    const slot = backgroundSlot();
    if (ownStyleTitle(slot.itemId || "") === "") {
        return "未生效";
    }
    if (slot.visible === false) {
        return "未在渲染";
    }
    const size = (slot.width && slot.height) ? (slot.width + "x" + slot.height) : "—";
    return (slot.animate === false ? "静态一帧" : "动画中") + ", " + size;
}

/// 频谱状态文字 (读状态镜像 `musicxx.state.spectrum`, 播放中约 10 Hz 推送)
///
/// 镜像里的数据是"现在这一帧"的响度与 16 个频带(都是 0~1); `available` 为假时
/// `level` / `bands` 都是 0, 用 `status` 说明原因 ("正在提取" / "未启用音乐动效" / ...)。
function spectrumStateText() {
    const s = musicxx.state.get("musicxx.state.spectrum");
    if (!s) {
        return "无状态";
    }
    if (s.available === true) {
        return "有数据（响度 " + Math.round((s.level || 0) * 100) + "%）";
    }
    switch (s.status) {
        case "loading":
            return "正在提取";
        case "off":
            return "『音乐动效』未启用";
        case "unavailable":
            return "该来源没有频谱";
        default:
            return "无数据";
    }
}

/// 配置读取: config.json 由插件自己读写 (设置页按钮与手改文件都改它)
///
/// 用 `getConfig(键, 默认值)` 异步读, 默认值由脚本给; 读到之后再换成实际倍率。
let configBgRate = null;

/// 当前生效的倍率 (配置还没读到 / 值非法时用默认值)
function configuredBgRate() {
    const value = configBgRate;
    return (typeof value === "number") ? normalizeBgRate(value) : DEFAULT_BG_RATE;
}

function refreshConfig() {
    musicxx.storage.getConfig("bgRate", DEFAULT_BG_RATE).then(function (value) {
        // 读取是异步的, 结果可能后于用户操作才到: 已经改过速率就不再覆盖
        // (否则用户刚切到的 2x 会被读回来的旧值/默认值改回去)
        if (configBgRate !== null) {
            return;
        }
        const rate = normalizeBgRate(value);
        configBgRate = rate;
        applyBackgroundRate(rate);
    }, function () { return null; });
}
refreshConfig();

/// 写一个配置项: 先更新脚本里记下的值, 再异步写 config.json (失败只记日志)
function saveConfig(key, value) {
    if (key === "bgRate") {
        configBgRate = value;
    }
    musicxx.storage.setConfig(key, value).then(function () {
        return null;
    }, function (err) {
        musicxx.host.log(3, "写配置失败: " + err.message);
    });
}

/// 页面用的能力摘要: 客户端取页面时会把能力段放进参数 (`{"view":…,"ui":{…}}`)
///
/// 传给 kit 后它会挑"这个客户端支持的那个变体" (例如客户端不支持图片时用文字行);
/// 拿不到就传 null: kit 产出中立描述, 由客户端自己的适配步骤收口。
function viewEnv(args) {
    if (args && typeof args.ui === "object" && args.ui !== null) {
        return args.ui;
    }
    return null;
}

/// 插件自绘设置页: 宿主打开 ext://example_js_shader/settings 时调用同名能力取页面描述
///
/// `override` 用来让按钮点完当场显示新值 (动作返回 {view:...} 时宿主直接刷新当前页)。
function settingsView(args, override) {
    const env = viewEnv(args);
    const rate = (override && typeof override.bgRate === "number")
        ? normalizeBgRate(override.bgRate)
        : configuredBgRate();
    return {
        title: "示例背景插件设置",
        subtitle: "页面由插件绘制; 值保存在插件目录的 config.json",
        blocks: [
            kit.hint({
                text: "速率是插件自己的设置项: 改完用 musicxx.ui.updateEntry 重新声明背景样式, 正在使用的背景立即用新速度。",
            }, env),
            kit.hint({
                text: "• 本插件注册了两种背景样式:『" + BG_TITLE + "』(晶格化) 与『" + RING_TITLE + "』(频谱圆环)。两者占同一个槽位 player.background, 设置里同时只能选中一个。",
            }, env),
            kit.hint({
                text: "• 背景声明里的 `spectrum.level` / `spectrum.bands.0..3` 每帧现读当前音频频谱（内置『音乐动效』提取），没有数据时宿主写 0，画面与不带频谱时一致。",
            }, env),
            kit.hint({
                text: "• `args` 只有一种写法：成员名 → 值表达式（`{\"kind\": \"...\"}`，可以嵌套）。来源用 `source`、常量用 `const`、过渡用 `smooth`、动画用 `tween` / `lfo`、组合用 `mul` / `mix` / `clamp` 等；宿主每帧求值一次，插件不用在着色器里写时间与滤波逻辑。",
            }, env),
            kit.divider({}, env),
            kit.card({ children: [
                kit.listRow({
                    title: "生效样式",
                    subtitle: "本插件两种样式里现在在画的是哪一种（读状态镜像）",
                    trailing: backgroundStyleText(),
                }, env),
                kit.listRow({
                    title: "播放页背景",
                    subtitle: "生效中 = 本插件在画（按下面按钮即可换样式）",
                    trailing: backgroundStateText(),
                }, env),
            ] }, env),
            // 页面里也能直接画一块着色器（`musicxx.Shader` 块）：用 `SizedBox` 给它确定的高度，
            // 参数同样用 `args` 声明 —— 两块都带频谱参数（与背景声明同一套来源），会跟着音乐起伏
            kit.hint({ text: "• 下面两块是页面内联的 Shader 块（同一个 bundle）：上面是晶格，下面是光圈；两块都带频谱参数，会跟着音乐起伏：" }, env),
            kit.card({
                children: [
                    { kind: "SizedBox", height: 300, children: [
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
                                // 频谱（与背景声明同一套来源）：晶格跟着响度与中高频"呼吸"
                                uLevel: smoothSpectrum("musicxx.spectrum.level"),
                                uBands: smoothSpectrum("musicxx.spectrum.bands.0"),
                                uBands2: smoothSpectrum("musicxx.spectrum.bands.2"),
                            },
                        }, env),
                    ] },
                ],
            }, env),
            kit.card({
                children: [
                    { kind: "SizedBox", height: 300, children: [
                        kit.shaderBlock({
                            bundle: "shader/ring.shaderbundle",
                            speed: 1,
                            maxFps: 16,
                            args: {
                                uColor1: { kind: "source", name: "musicxx.theme.primary" },
                                uColor2: { kind: "source", name: "musicxx.icon.main", convert: true, fallback: "#8899aa" },
                                uColor3: { kind: "source", name: "musicxx.icon.dark", fallback: "#223344" },
                                uColor4: { kind: "source", name: "musicxx.icon.themeMapping.3" },
                                uLine: { kind: "const", value: "#f6faff" },
                                uLevel: smoothSpectrum("musicxx.spectrum.level"),
                                uBands0: smoothSpectrum("musicxx.spectrum.bands.0"),
                                uBands1: smoothSpectrum("musicxx.spectrum.bands.1"),
                                uBands2: smoothSpectrum("musicxx.spectrum.bands.2"),
                                uBands3: smoothSpectrum("musicxx.spectrum.bands.3"),
                            },
                        }, env),
                    ] },
                ],
            }, env),
            kit.settingRow({
                title: "背景动画速率",
                depict: "本插件背景的时间推进速度（1x = 基准速度 1，可选 0.5x / 1x / 2x），改完立即生效",
                value: bgRateText(rate),
            }, env),
            kit.button({
                label: "切换背景动画速率（0.5x / 1x / 2x）",
                variant: "primary",
                action: { kind: "dispatch", name: "cycleBackgroundRate", args: { view: "settings" } },
            }, env),
            kit.button({
                label: "使用『" + RING_TITLE + "』（频谱圆环）",
                variant: "primary",
                action: { kind: "dispatch", name: "useBackground", args: { id: RING_ITEM_ID, view: "settings" } },
            }, env),
            kit.button({
                label: "使用『" + BG_TITLE + "』（晶格化）",
                action: { kind: "dispatch", name: "useBackground", args: { id: BG_ITEM_ID, view: "settings" } },
            }, env),
            kit.button({
                label: "切回内置背景",
                action: { kind: "dispatch", name: "useBackground", args: { id: BUILTIN_AUTO_ID, view: "settings" } },
            }, env),
            kit.button({
                label: "打开插件说明页",
                action: { kind: "route", route: "ext://example_js_shader/card" },
            }, env),
        ],
    };
}

musicxx.capability.register("settings", function (args) {
    return { view: settingsView(args, null) };
});

/// 设置页按钮: 循环切换背景动画速率 (0.5x → 1x → 2x → 0.5x)
musicxx.capability.register("cycleBackgroundRate", function (args) {
    const current = configuredBgRate();
    let index = 0;
    for (let i = 0; i < BG_RATE_OPTIONS.length; ++i) {
        if (BG_RATE_OPTIONS[i] === current) {
            index = i;
        }
    }
    const next = BG_RATE_OPTIONS[(index + 1) % BG_RATE_OPTIONS.length];
    saveConfig("bgRate", next);
    applyBackgroundRate(next);
    return {
        view: (args && args.view === "settings") ? settingsView(args, { bgRate: next }) : cardView(args),
    };
});

/// 一键切换播放页背景 (也可以切到内置样式: id 用 "builtin:<内置样式名>")
///
/// `musicxx.render.select` 是宿主官方动作: 只允许选中**可用**项, 选中后立即生效,
/// 用户在设置里随时能改回来。能力处理器必须同步返回, 所以这里只说明"已请求",
/// 实际结果看页面刷新后的状态 (或 musicxx.render.current)。
function requestBackground(id) {
    // 先记一条日志: 排查"按钮有没有点到插件"时就看宿主日志里有没有这一行
    musicxx.host.log(2, "收到切换播放页背景请求: " + id);
    lastBackgroundRequest = id;
    musicxx.call("musicxx.render.select", { slot: "player.background", id: id }).then(function (r) {
        // 动作成功 = 选择已经生效 (镜像同步更新): 不再需要"已请求"提示
        lastBackgroundRequest = "";
        musicxx.host.log(2, "切换播放页背景成功: " + JSON.stringify(r));
    }, function (err) {
        // 失败时镜像里不会有这次请求, 留着标记会让页面一直显示"已请求"
        lastBackgroundRequest = "";
        musicxx.host.log(3, "切换播放页背景失败: " + err.message);
    });
}

musicxx.capability.register("useBackground", function (args) {
    const id = (args && args.id) ? String(args.id) : BG_ITEM_ID;
    requestBackground(id);
    return {
        view: (args && args.view === "settings") ? settingsView(args, null) : cardView(args),
    };
});

/// 主页入口: 打开插件说明页 (页面内容来自同名能力 `card`)
musicxx.ui.registerEntry({
    name: "card",
    type: "home.entry",
    order: 120,
    data: {
        title: "JS 背景插件示例",
        subtitle: "example_js_shader 提供的入口",
        icon: "addition",
        action: { kind: "route", route: "ext://example_js_shader/card" },
    },
});

/// 插件说明页: 当前生效项 / 渲染状态 / 速率, 以及一键切换
function cardView(args) {
    const env = viewEnv(args);
    return {
        title: "JS 背景插件示例",
        subtitle: "• 页面内容来自能力 `card`, 渲染由客户端完成",
        blocks: [
            kit.hint({
                text: "• 本插件把预编译好的 shader bundle 注册成一种播放页背景样式: 用户在『设置 → 播放页面背景』里选中后才生效。",
            }, env),
            kit.divider({}, env),
            kit.card({ children: [
                kit.listRow({
                    title: "生效样式",
                    subtitle: "本插件两种样式（晶格 / 光圈）里现在在画的是哪一种",
                    trailing: backgroundStyleText(),
                }, env),
                kit.listRow({
                    title: "播放页背景",
                    subtitle: "当前生效项 (读状态镜像 musicxx.state.renderSlots)",
                    trailing: backgroundStateText(),
                }, env),
                kit.listRow({
                    title: "渲染状态",
                    subtitle: "页面被遮挡或切到后台时宿主会停止渲染",
                    trailing: renderStateText(),
                }, env),
                kit.listRow({
                    title: "背景动画速率",
                    subtitle: "点这一条循环切换 0.5x / 1x / 2x (1x 是基准速度)",
                    trailing: bgRateText(configuredBgRate()),
                    action: { kind: "dispatch", name: "cycleBackgroundRate", args: { view: "card" } },
                }, env),
                kit.listRow({
                    title: "音频频谱",
                    subtitle: "状态镜像 musicxx.state.spectrum（同步、实时）；完整结果点下面的按钮（异步读动作）",
                    trailing: spectrumStateText(),
                }, env),
            ] }, env),
            kit.button({ label: "刷新本页", variant: "primary", action: "card" }, env),
            kit.button({
                label: "读取当前频谱（镜像 + 异步动作）",
                action: { kind: "dispatch", name: "spectrumProbe", args: { view: "card" } },
            }, env),
            kit.button({
                label: "使用『" + RING_TITLE + "』（频谱圆环）",
                variant: "primary",
                action: { kind: "dispatch", name: "useBackground", args: { id: RING_ITEM_ID, view: "card" } },
            }, env),
            kit.button({
                label: "使用『" + BG_TITLE + "』（晶格化）",
                action: { kind: "dispatch", name: "useBackground", args: { id: BG_ITEM_ID, view: "card" } },
            }, env),
            kit.button({
                label: "切回内置背景",
                action: { kind: "dispatch", name: "useBackground", args: { id: BUILTIN_AUTO_ID, view: "card" } },
            }, env),
            kit.button({
                label: "打开本插件设置页",
                action: { kind: "route", route: "ext://example_js_shader/settings" },
            }, env),
        ],
    };
}

musicxx.capability.register("card", function (args) {
    return { view: cardView(args) };
});

/// 频谱动作结果的说明文本 (动作 `musicxx.media.spectrum` 的返回)
///
/// 没有数据不是错误: 结果里用 `status` / `available` / `loading` 说明原因,
/// 这时 `level` 与 `bands` 都是 0, 但数组长度仍按请求给 (结构稳定, 不用判键在不在)。
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

/// 最近一次异步读取到的动作结果 (还没读回 / 失败都在这里说明)
///
/// 为什么不在这里 `await` 动作:
/// 应用侧打开页面、点按钮都是**同步进宿主**调用这个能力的 (应用线程全程等宿主返回),
/// 而动作要由应用线程执行 —— 处理器里等动作就会与调用方互相等下去, 最后只能等到超时,
/// 表现是"整个应用卡住几秒 + 调用插件能力失败, 未在预算内完成"。
/// 所以这一页当场用状态镜像 (同步、实时) 画出来, 动作结果异步读取、读完记下来回读
/// (与 `example_js` 的 `crossCall` 同一做法)。
let spectrumActionState = { pending: 0, text: "", error: "" };

/// 异步读一次动作结果 (不等待; 读完记进 spectrumActionState, 由页面回读)
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

/// 动作结果那一行的说明 (第一次打开时通常已经有值: 插件装载时就发起过一次读取)
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

/// 动作结果那一行的短状态 (具体内容看下面的说明)
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
                title: "状态镜像",
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
                text: "• 没有数据时 status 是 loading（正在提取）/ off（未启用音乐动效）/ unavailable（来源不是本地或缓存、时长超限），这时值恒为 0，请按 status 判断，不要按数值判断。",
            }, env),
            kit.hint({
                text: "• 这一页也演示了『能力处理器里不能等动作』这条规则：能力是同步进宿主调用的（应用线程在等它返回），处理器里 await 动作会与调用方互锁、只能等到超时；要实时数据读状态镜像，要动作结果就异步读、把结果记下来回读（本页与 example_js 的 crossCall 都是这么做的）。",
            }, env),
            // 动画示例：值表达式 + 动画块（musicxx.AnimatedBuilder / SizeTransition / FadeTransition）
            //
            // 一次声明就够：通道表里定义"这一页用到的几个值"，子树里的字段用 {"kind":"source","name":...}
            // 读它们；宿主每帧求值并只重建读到值的块。没有动画块包裹的字段按"每次重画算一次"。
            kit.card({
                title: "动画示例",
                children: [
                    kit.hint({
                        text: "下面这块由 musicxx.AnimatedBuilder 驱动：高度跟着响度呼吸（smooth + lfo），进场先淡入再撑开（tween 驱动 SizeTransition / FadeTransition）。没有频谱数据时高度取 lfo 的下限，所以画面仍然在动。",
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
                        children: [
                            {
                                kind: "musicxx.FadeTransition",
                                value: { kind: "source", name: "demo.intro" },
                                children: [
                                    {
                                        kind: "musicxx.SizeTransition",
                                        axis: "vertical",
                                        value: { kind: "source", name: "demo.intro" },
                                        children: [
                                            {
                                                kind: "SizedBox",
                                                height: { kind: "source", name: "demo.h" },
                                                children: [
                                                    { kind: "Text", text: "跟着音乐呼吸的一块（高度由通道驱动）", type: "caption", tone: "hint" },
                                                ],
                                            },
                                        ],
                                    },
                                ],
                            },
                        ],
                    },
                    kit.hint({
                        text: "• 值表达式也能写在普通字段上（尺寸 / 进度 / 文本 / visible）：例如 `Progress.value` 写成 `{\"kind\":\"source\",\"name\":\"plugin.<插件id>.<键>\"}` 就能跟着插件推的变量动。上面的块手写了 JSON；kit 里有现成装配（kit.animScope / kit.fadeTransition / kit.sizeTransition / kit.slideTransition / kit.scaleTransition / kit.rotationTransition）。节点表与来源见 docs/plugin-shader-bundle.md §7。",
                    }, env),
                ],
            }, env),
            kit.button({
                label: "重新读取（镜像实时 + 发起一次动作读取）",
                variant: "primary",
                action: { kind: "dispatch", name: "spectrumProbe", args: { view: "spectrum" } },
            }, env),
            kit.button({
                label: "返回说明页",
                action: { kind: "route", route: "ext://example_js_shader/card" },
            }, env),
        ],
    };
}

/// 打开 / 点按钮都走这里: 处理器同步返回页面, 同时发起一次异步动作读取
///
/// 顺序很重要：**先按上一次读回的结果画页面，再发起新的读取** —— 反过来的话页面读到的
/// `pending` 永远是自己刚设的那一次，只能一直显示"读取中"。
///
/// 也不能写成 `musicxx.media.spectrum(...)` 然后 `return ...then(...)`：
/// 那样处理器要等动作结算, 而动作只有应用线程能执行、应用线程又卡在这次能力调用里。
musicxx.capability.register("spectrumProbe", function (args) {
    const view = spectrumProbeView(args);
    refreshSpectrumAction();
    return { view: view };
});

// 插件装载时先读一次: 第一次打开这一页时动作结果通常已经就位
refreshSpectrumAction();

console.log("example_js_shader 已加载 (pid=" + musicxx.pluginId + ")");
