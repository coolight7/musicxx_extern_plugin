/// musicxx 外部插件示例 (JS, 零编译): 自定义播放页面背景 + shader 动画速率
///
/// 这个插件只演示"插件渲染槽位"这一条链路, 内容就两件事:
/// - 声明一种播放页背景样式 (`musicxx.ui.playing.background`): 画面由一个预编译好的
///   shader bundle 画 (插件不写界面代码), 用户在『设置 → 播放页面背景』里选中后才生效;
///   未选中时零成本 (宿主不读 bundle、不分析封面)。
/// - 把动画速率做成插件自己的设置项 (0.5x / 1x / 2x), 改完立即生效。
///
/// 页面 (框架不管理插件设置入口, 入口由插件自己给):
/// - `ext://example_js_shader/card` (主页入口): 当前生效项与渲染状态, 一键切换背景;
/// - `ext://example_js_shader/settings`: 改速率 (值存在插件目录的 config.json)。
/// 两个页面都由脚本返回声明式块 (Text / Divider / Button 与布局块), 排版与控件由宿主渲染。
///
/// 这两件事原先和钩子/网络/页面示例一起放在 `example_js` 里, 2026-09 拆成独立插件,
/// 便于单独照抄。要看"内置背景样式换成插件渲染"的完整流程 (着色器写法、bundle 打包、
/// uniform 契约), 见 `docs/plugin-shader-bundle.md` 与 shader/ 目录。
///
/// 约束: 脚本顶层必须**同步**完成注册 (顶层不能用 await); 异步逻辑放到钩子或定时器里。

/// 背景样式在设置列表里的名字与副标题
const BG_TITLE = "示例晶格背景";
const BG_DEPICT = "跟随封面配色的晶格化动态背景";

/// 本插件背景样式的 UI 项短名与完整 id (完整 id 由宿主拼成 plugin.<插件id>.<短名>)
const BG_ITEM_NAME = "bg";
const BG_ITEM_ID = "plugin.example_js_shader." + BG_ITEM_NAME;

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

/// 背景样式的完整声明
///
/// `musicxx.ui.updateEntry` 是**整体替换**, 所以每次都要从这一个函数取完整 data,
/// 不能只传改动的字段。
function backgroundData(rate) {
    return {
        title: BG_TITLE,
        depict: BG_DEPICT,
        shader: { bundle: "shader/bg.shaderbundle" },
        colors: { source: "background" },
        speed: bgSpeedOf(rate),
        maxFps: 16,
        animate: true,
        foregroundStyle: "mask",
    };
}

let appliedBgRate = normalizeBgRate(DEFAULT_BG_RATE);

/// 应用背景动画速率: 运行期改自己的背景声明
///
/// 宿主收到后会刷新候选; 正在使用的背景立即用新速度 (不用重新选中, `speed` 每帧现算)。
/// 改 `maxFps` / `resolutionScale` 这类要换帧调度的字段则不在这里生效, 需要用户重新选中。
function applyBackgroundRate(rate) {
    const value = normalizeBgRate(rate);
    if (value === appliedBgRate) {
        return;
    }
    appliedBgRate = value;
    musicxx.ui.updateEntry(BG_ITEM_NAME, backgroundData(value));
    musicxx.host.log(2, "背景动画速率 → " + bgRateText(value));
}

/// 注册播放页背景样式 (顶层先按 1x 声明; 读到 config.json 后再换成实际倍率)
musicxx.ui.registerEntry({
    name: BG_ITEM_NAME,
    type: "playing.background",
    order: 20,
    data: backgroundData(appliedBgRate),
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
function backgroundStateText() {
    const slot = backgroundSlot();
    const selected = slot.selectedId || "";
    const current = slot.itemId || "";
    // 镜像还没反映这次请求(动作还在往返)时先显示"已请求"; 反映之后标记就没用了
    if (lastBackgroundRequest !== "" && selected !== lastBackgroundRequest) {
        return "已请求";
    }
    lastBackgroundRequest = "";
    if (current === BG_ITEM_ID) {
        return "生效中";
    }
    if (selected === BG_ITEM_ID) {
        return "已选中";
    }
    if (selected.indexOf("builtin:") === 0) {
        return "内置背景";
    }
    return selected === "" ? "无状态" : ("其它插件: " + selected);
}

/// 本插件的渲染状态 (尺寸 / 是否在动)
///
/// `itemId` 是本插件时才有渲染可言: `visible:false` 表示宿主停止渲染(播放页被遮挡、
/// 切到后台, 或者播放页当前根本不在页面上), 插件可以据此停掉自己的重活;
/// `animate:false` 表示这个样式只画一帧。
function renderStateText() {
    const slot = backgroundSlot();
    if (slot.itemId !== BG_ITEM_ID) {
        return "未生效";
    }
    if (slot.visible === false) {
        return "未在渲染";
    }
    const size = (slot.width && slot.height) ? (slot.width + "x" + slot.height) : "—";
    return (slot.animate === false ? "静态一帧" : "动画中") + ", " + size;
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

/// 写一个配置项: 先更新脚本侧记账, 再异步写 config.json (失败只记日志)
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

/// 一行 "标题 + 说明 + 右侧状态" 的排版
///
/// 行放进**内容块** (`Block` = 应用里的卡片底色与边距), 等于原来的列表条目;
/// 行本身用布局块组合 (Row / Expanded / Column / SizedBox / Text)。
/// `action` 可选, 带上就是整块可点。
function infoRow(title, subtitle, right, action) {
    const left = {
        kind: "Column",
        children: subtitle
            ? [{ kind: "Text", text: title },
            { kind: "SizedBox", height: 6 },
            { kind: "Text", text: subtitle, style: "cross" }]
            : [{ kind: "Text", text: title }],
    };
    const children = [{ kind: "Expanded", child: left }];
    if (right) {
        children.push({ kind: "SizedBox", width: 30 });
        children.push({ kind: "Text", text: right, style: "cross" });
    }
    const block = {
        kind: "Block",
        inContent: true,                             // 内容块里的浅色底
        margin: { top: 30, bottom: 30 },             // 数值是设计像素
        padding: { top: 30, bottom: 30 },
        child: { kind: "Row", children: children },
    };
    if (action) {
        block.action = action;
    }
    return block;
}

/// 插件自绘设置页: 宿主打开 ext://example_js_shader/settings 时调用同名能力取页面描述
///
/// `override` 用来让按钮点完当场显示新值 (动作返回 {view:...} 时宿主直接刷新当前页)。
function settingsView(override) {
    const rate = (override && typeof override.bgRate === "number")
        ? normalizeBgRate(override.bgRate)
        : configuredBgRate();
    return {
        title: "示例背景插件设置",
        subtitle: "页面由插件绘制; 值保存在插件目录的 config.json",
        blocks: [
            {
                kind: "Text",
                text: "速率是插件自己的设置项: 改完用 musicxx.ui.updateEntry 重新声明背景样式, 正在使用的背景立即用新速度。",
                style: "cross",
            },
            { kind: "Divider" },
            {
                kind: "Block",
                child: infoRow("背景动画速率",
                    "本插件背景的时间推进速度（1x = 基准速度 1，可选 0.5x / 1x / 2x），改完立即生效",
                    bgRateText(rate)),
            },
            {
                kind: "Button",
                title: "切换背景动画速率（0.5x / 1x / 2x）",
                style: "primary",
                action: { kind: "capability", name: "cycleBackgroundRate", args: { view: "settings" } },
            },
            {
                kind: "Button",
                title: "使用本插件的背景",
                action: { kind: "capability", name: "useBackground", args: { id: BG_ITEM_ID, view: "settings" } },
            },
            {
                kind: "Button",
                title: "切回内置背景",
                action: { kind: "capability", name: "useBackground", args: { id: "builtin:Auto", view: "settings" } },
            },
            {
                kind: "Button",
                title: "打开插件说明页",
                action: { kind: "route", route: "ext://example_js_shader/card" },
            },
        ],
    };
}

musicxx.capability.register("settings", function () {
    return { view: settingsView(null) };
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
        view: (args && args.view === "settings") ? settingsView({ bgRate: next }) : cardView(),
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
        // 动作成功 = 选择已经落地 (镜像同步更新): 不再需要"已请求"提示
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
        view: (args && args.view === "settings") ? settingsView(null) : cardView(),
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
function cardView() {
    return {
        title: "JS 背景插件示例",
        subtitle: "• 页面内容来自能力 `card`, 渲染由宿主完成",
        blocks: [
            {
                kind: "Text",
                text: "• 本插件把预编译好的 shader bundle 注册成一种播放页背景样式: 用户在『设置 → 播放页面背景』里选中后才生效。",
                style: "cross",
            },
            { kind: "Divider" },
            {
                kind: "Block",
                child: {
                    kind: "Column",
                    children: [
                        infoRow("播放页背景", "当前生效项 (读状态镜像 musicxx.state.renderSlots)",
                            backgroundStateText()),
                        infoRow("渲染状态", "页面被遮挡或切到后台时宿主会停止渲染", renderStateText()),
                        infoRow("背景动画速率", "点这一条循环切换 0.5x / 1x / 2x (1x 是基准速度)",
                            bgRateText(configuredBgRate()),
                            { kind: "capability", name: "cycleBackgroundRate", args: { view: "card" } }),
                    ]
                }
            },
            {
                kind: "Button",
                title: "刷新本页",
                style: "primary",
                action: { kind: "capability", name: "card" },
            },
            {
                kind: "Button",
                title: "使用本插件的背景",
                style: "primary",
                action: { kind: "capability", name: "useBackground", args: { id: BG_ITEM_ID, view: "card" } },
            },
            {
                kind: "Button",
                title: "切回内置背景",
                action: { kind: "capability", name: "useBackground", args: { id: "builtin:Auto", view: "card" } },
            },
            {
                kind: "Button",
                title: "打开本插件设置页",
                action: { kind: "route", route: "ext://example_js_shader/settings" },
            },
        ],
    };
}

musicxx.capability.register("card", function () {
    return { view: cardView() };
});

console.log("example_js_shader 已加载 (pid=" + musicxx.pluginId + ")");
