/// musicxx 外部插件示例 (JS, 零编译)
///
/// 行为与 `plugins/example_native` 等价, 用来验证两条链路一致:
/// - `musicxx.player.beforePlaySong` 裁决: 含"广告"的曲目 → skip;
/// - `musicxx.song.changed` 观察: 切歌时记录名称并累加计数;
/// - `musicxx.player.error` 裁决: 首次错误时建议换源 (patch.tryNextSrc);
/// - `musicxx.player.speed` 裁决 (异步): 处理器返回 Promise 也能生效 (限速演示);
/// - `example_js.probe` 能力: 返回自检信息 (计数/线程/状态镜像读取)。
/// - `example_js.card` 能力: 主页入口与播放页附加信息块打开的插件页面 (`ext://example_js/card`)。
/// - 插件自绘设置页 (`ext://example_js/settings`, 从 card 页的按钮进入; 框架不管理设置入口)
///   + 宿主网络代理通道 (musicxx.net.fetch) 演示。
/// - 播放页背景 (shader bundle) 的动画速率是插件自己的设置项 (设置页里 0.5× / 1× / 2×):
///   改完用 `musicxx.ui.updateEntry` 重新声明, 宿主刷新候选后立即生效;
///   1× 的基准速度定为 1 (原先声明 4, 即整体降速到原来的 0.25×)。
///
/// 约束: 脚本顶层必须**同步**完成注册 (顶层不能用 await);
/// 异步逻辑放到钩子或定时器里。

const BUDGET_KEY = "example_js.songChanged";

let songChangedCount = 0;
let lastSongName = "";
let errorCount = 0;
let timerTicks = 0;

/// 读当前歌曲名 (来自宿主状态镜像的只读快照, 同步读取)
function currentSongName() {
    const song = musicxx.state.get("musicxx.state.song");
    if (song && typeof song.name === "string") {
        return song.name;
    }
    return "";
}

/// 裁决型钩子: 播放前跳过"广告"曲目
/// 开关来自插件自己的配置 (config.json 的 skipAds; 脚本给默认值 true,
/// 在自绘设置页里切换, 见下面的 settingsView)。
musicxx.hooks.register("musicxx.player.beforePlaySong", { mode: "decision", priority: 0 }, function (ctx) {
    const name = (ctx && ctx.song && ctx.song.name) ? String(ctx.song.name) : "";
    if (!skipAdsEnabled()) {
        return null;
    }
    if (name.indexOf("广告") >= 0) {
        musicxx.host.log(2, "跳过广告曲目: " + name);
        return { action: "skip" };
    }
    return null;
});

/// 观察型钩子: 切歌通知 (异步执行, 不阻塞调用点)
musicxx.hooks.register("musicxx.song.changed", { mode: "observe" }, function (ctx) {
    songChangedCount += 1;
    lastSongName = (ctx && ctx.song && ctx.song.name) ? String(ctx.song.name) : "";
    musicxx.host.log(2, "切歌: " + lastSongName);
    // 计数落到插件私有存储 (经宿主动作, 异步; 失败只记日志)
    musicxx.storage.set(BUDGET_KEY, songChangedCount).then(function () {
        return null;
    }, function (err) {
        musicxx.host.log(3, "写插件存储失败: " + err.message);
    });
});

/// 裁决型钩子: 播放错误 (首个错误建议换源, 之后交给宿主原有策略)
musicxx.hooks.register("musicxx.player.error", { mode: "decision", priority: 0 }, function (ctx) {
    errorCount += 1;
    if (errorCount === 1) {
        return { action: "continue", patch: { tryNextSrc: true } };
    }
    return null;
});

/// 裁决型钩子 (异步裁决): 处理器返回 Promise 也能生效 —— 宿主最多等 100 ms,
/// 超时按"不裁决"继续 (不打断脚本, 也不计为处理器失败)。
/// 这里演示"限速"这一常见需求: 先把 0.25~3 之外的速度夹回来。
/// 说明: 真实插件通常在这里 `await` 一次异步来源 (存储/网络/其它插件能力);
/// 用 Promise 只是让"异步裁决"这条路走通, 拿不到结果时插件应当不裁决。
musicxx.hooks.register("musicxx.player.speed", { mode: "decision" }, function (ctx) {
    const to = (ctx && typeof ctx.to === "number") ? ctx.to : 1;
    if (to >= 0.25 && to <= 3) {
        return null;   // 同步路径: 不需要裁决时立刻返回 (最省时)
    }
    return new Promise(function (resolve) {
        setTimeout(function () {
            resolve({ action: "continue", patch: { to: Math.min(3, Math.max(0.25, to)) } });
        }, 20);
    });
});

/// 定时器: 按"心跳间隔"配置写日志 (演示定时器链路; 卸载/禁用时宿主自动清理)
///
/// 间隔是插件的设置项 (默认 30 秒, 设置页里能改, 也可以直接编辑 config.json):
/// 顶层先按默认值注册, 读到配置里的值后再换成实际间隔。
const DEFAULT_HEARTBEAT_MS = 30000;
const HEARTBEAT_OPTIONS = [10000, 30000, 60000];

let heartbeatTimer = 0;
let appliedHeartbeatMs = 0;

/// 把任意值归一到允许的心跳间隔 (非法值回退默认)
function normalizeHeartbeat(value) {
    const ms = Number(value);
    for (let i = 0; i < HEARTBEAT_OPTIONS.length; ++i) {
        if (HEARTBEAT_OPTIONS[i] === ms) {
            return ms;
        }
    }
    return DEFAULT_HEARTBEAT_MS;
}

/// 按间隔 (重新) 注册心跳定时器; 间隔没变则什么都不做
function applyHeartbeat(ms) {
    const value = normalizeHeartbeat(ms);
    if (value === appliedHeartbeatMs && heartbeatTimer !== 0) {
        return;
    }
    appliedHeartbeatMs = value;
    if (heartbeatTimer !== 0) {
        musicxx.timer.clear(heartbeatTimer);
    }
    heartbeatTimer = musicxx.timer.setInterval(function () {
        timerTicks += 1;
        musicxx.host.log(2, "example_js 心跳: 切歌 " + songChangedCount + " 次, 错误 " + errorCount + " 次");
    }, value);
    musicxx.host.log(2, "心跳间隔 → " + value + "ms");
}

applyHeartbeat(DEFAULT_HEARTBEAT_MS);

/// 声明式 UI 扩展: 主页入口项 + 歌曲菜单项
/// 说明: UI 项只做声明 (标题/图标/动作), 渲染由宿主负责; 运行期也能再注册/更新/注销。
musicxx.ui.registerEntry({
    name: "card",
    type: "home.entry",
    order: 110,
    data: {
        title: "JS 示例插件",
        subtitle: "example_js 提供的入口",
        icon: "addition",
        action: { kind: "route", route: "ext://example_js/card" },
    },
});

musicxx.ui.registerEntry({
    name: "songInfo",
    type: "song.action",
    order: 910,
    data: {
        title: "JS 示例插件：查看歌曲信息",
        action: { kind: "capability", name: "probe", args: { from: "ui" } },
    },
});

/// 播放页背景样式 (musicxx.ui.playing.background)
///
/// 插件把一个预编译好的 shader bundle 声明成一种"播放页背景样式": 用户在
/// 『设置 → 播放页面背景』里选中后才生效 (未选中时零成本: 不加载 bundle、不分析封面)。
/// 4 个绘制色由宿主每帧写进 uniform (见 docs/plugin-shader-bundle.md);
/// shader/ 目录里带 bundle.json 与编译好的 bg.shaderbundle (打包脚本 shader/build_bundle.ps1)。
///
/// `speed` 是时间推进速度, 由插件自己决定 (设置页里的"背景动画速率"就是它):
/// 1× = 基准速度 1 (2026-09 起把原先声明的 4 重新定义为 1×, 即整体降速到原来的 0.25×),
/// 可选 0.5× / 1× / 2×。
const BG_BASE_SPEED = 1;
const BG_RATE_OPTIONS = [0.5, 1, 2];

/// 把任意值归一到允许的动画速率倍率 (非法值回退 1×)
function normalizeBgRate(value) {
    const rate = Number(value);
    for (let i = 0; i < BG_RATE_OPTIONS.length; ++i) {
        if (BG_RATE_OPTIONS[i] === rate) {
            return rate;
        }
    }
    return 1;
}

/// 倍率的显示文本 (1 → "1×"、0.5 → "0.5×")
function bgRateText(rate) {
    return String(normalizeBgRate(rate)) + "×";
}

/// 倍率 → 声明给宿主的时间推进速度
function bgSpeedOf(rate) {
    return BG_BASE_SPEED * normalizeBgRate(rate);
}

/// 背景样式的完整声明 (updateEntry 是整体替换, 所以每次都从这一个函数取)
function backgroundData(rate) {
    return {
        title: "示例晶格背景",
        depict: "跟随封面配色的晶格化动态背景",
        shader: { bundle: "shader/bg.shaderbundle" },
        colors: { source: "background" },
        speed: bgSpeedOf(rate),
        maxFps: 16,
        animate: true,
        foregroundStyle: "mask",
    };
}

let appliedBgRate = normalizeBgRate(1);

/// 应用背景动画速率: 运行期改自己的背景声明 (宿主会刷新候选, 选中时立即生效)
function applyBackgroundRate(rate) {
    const value = normalizeBgRate(rate);
    if (value === appliedBgRate) {
        return;
    }
    appliedBgRate = value;
    musicxx.ui.updateEntry("bg", backgroundData(value));
    musicxx.host.log(2, "背景动画速率 → " + bgRateText(value));
}

musicxx.ui.registerEntry({
    name: "bg",
    type: "playing.background",
    order: 20,
    data: backgroundData(appliedBgRate),
});

/// 读状态镜像: 当前是不是本插件的背景在画 (同步读取)
function backgroundState() {
    const slots = musicxx.state.get("musicxx.state.renderSlots") || {};
    return slots["player.background"] || {};
}

/// 最近一次"一键使用"请求的样式 id
/// 宿主切换要经过一次动作往返，这里先记住请求，让页面当场就能看到"点了有反应"
let lastBackgroundRequest = "";

function backgroundStateText() {
    const current = backgroundState().itemId || "";
    if (lastBackgroundRequest !== "" && current !== lastBackgroundRequest) {
        return "已请求： " + lastBackgroundRequest;
    }
    if (current === "plugin.example_js.bg") {
        return "本插件生效中";
    }
    return current === "" ? "内置背景" : ("其它: " + current);
}

/// 通知 (等价动作 `musicxx.ui.notify`; fire-and-forget)
///
/// 提示语是插件设置项 (`greeting`): 等配置读完之后再弹，用户改了提示语下次启动就会看到。
function notifyGreeting(greeting) {
    const text = (typeof greeting === "string" && greeting !== "")
        ? greeting
        : "example_js 已加载";
    musicxx.ui.notify({ text: text, kind: "info" }).then(function () {
        return null;
    }, function (err) {
        musicxx.host.log(3, "发送通知失败: " + err.message);
    });
}

/// 能力: 跨插件调用 (JS → 原生/内置插件; 结果为 Promise, 宿主线程执行)
///
/// - 目标是 JS 插件时同线程直接调用 (结果立即就绪);
/// - 目标是动态库插件时投递到宿主线程执行, 脚本不阻塞 (宿主线程可能正等 JS 处理器)。
/// 能力处理器必须**同步返回**, 所以跨插件调用的结果用"最后一次结果"记账,
/// 由 probe 能力回读 (测试用; 真实插件应把异步结果写进自己的状态或 UI 项)。
let crossCallState = { pending: 0, ok: 0, keys: 0, error: "" };

musicxx.capability.register("crossCall", function (args) {
    const target = (args && args.target) ? String(args.target) : "example_native";
    const method = (args && args.method) ? String(args.method) : "probe";
    crossCallState = { pending: 1, ok: 0, keys: 0, error: "" };
    musicxx.capability.call(target, method, {}).then(function (result) {
        crossCallState = { pending: 0, ok: 1, keys: Object.keys(result || {}).length, error: "" };
        musicxx.host.log(2, "跨插件调用成功: " + target + " 返回 " + crossCallState.keys + " 个字段");
    }, function (err) {
        crossCallState = { pending: 0, ok: 0, keys: 0, error: err.message };
        musicxx.host.log(3, "跨插件调用失败: " + err.message);
    });
    return { accepted: 1, target: target, method: method };
});

/// 配置读取: config.json 由插件自己读写 (自绘设置页 + 手改文件都改它),
/// 用 getConfig 异步读、给默认值 (框架不再提供 settings_schema 默认值)。
/// 能力处理器必须同步返回, 所以这里把最近一次读到的值记账, 由 probe / 设置页回读。
musicxx.storage.configCache = {
    skipAds: null,
    greeting: "",
    heartbeatMs: null,
    bgRate: null,
};

/// 是否开启"跳过广告" (默认开启; 配置还没读到时也按默认值处理)
function skipAdsEnabled() {
    return musicxx.storage.configCache.skipAds !== false;
}

function refreshConfig() {
    musicxx.storage.getConfig("skipAds", true).then(function (value) {
        musicxx.storage.configCache.skipAds = (typeof value === "boolean") ? value : true;
    }, function () { return null; });
    musicxx.storage.getConfig("greeting", "example_js 已加载").then(function (value) {
        const text = (value === null || value === undefined) ? "" : String(value);
        musicxx.storage.configCache.greeting = text;
        // 读到了配置才弹提示: 用户设置的提示语才真正用得上
        notifyGreeting(text);
    }, function () { return null; });
    musicxx.storage.getConfig("heartbeatMs", DEFAULT_HEARTBEAT_MS).then(function (value) {
        const ms = normalizeHeartbeat(value);
        musicxx.storage.configCache.heartbeatMs = ms;
        applyHeartbeat(ms);
    }, function () { return null; });
    musicxx.storage.getConfig("bgRate", 1).then(function (value) {
        const rate = normalizeBgRate(value);
        musicxx.storage.configCache.bgRate = rate;
        applyBackgroundRate(rate);
    }, function () { return null; });
}
refreshConfig();

/// 插件自绘设置页: 宿主打开 ext://example_js/settings 时调用同名能力取页面描述。
///
/// 设置界面属于插件自己的页面 (框架不管理设置入口, 也不渲染设置控件): 入口就是
/// card 页里的『打开本插件设置页』按钮, 页面只用 text / list / button 块画出插件
/// 自己的配置界面; 值改动由按钮触发能力写回 config.json (返回 {view:...} 让宿主
/// 直接用新页面刷新)。
function settingsView(override) {
    const cache = musicxx.storage.configCache;
    const skipAds = (override && typeof override.skipAds === "boolean")
        ? override.skipAds
        : skipAdsEnabled();
    const greeting = (override && typeof override.greeting === "string")
        ? override.greeting
        : (cache.greeting || "");
    const heartbeatMs = (override && typeof override.heartbeatMs === "number")
        ? override.heartbeatMs
        : configuredHeartbeatMs();
    const bgRate = (override && typeof override.bgRate === "number")
        ? override.bgRate
        : configuredBgRate();
    return {
        title: "JS 示例插件设置",
        subtitle: "页面由插件绘制; 值保存在插件目录的 config.json",
        blocks: [
            {
                kind: "text",
                text: "这些设置由脚本用 musicxx.storage.getConfig/setConfig 读写, 没有宿主提供的表单。",
                style: "cross",
            },
            { kind: "divider" },
            {
                kind: "list",
                items: [
                    { id: "skipAds", title: "跳过广告曲目", subtitle: "播放前裁决: 名字含『广告』的曲目直接跳过", right: skipAds ? "已开启" : "已关闭" },
                    { id: "greeting", title: "启动提示语", subtitle: "加载时弹出的提示", right: greeting === "" ? "(未设置)" : greeting },
                    { id: "heartbeatMs", title: "心跳间隔(毫秒)", subtitle: "定时器写日志的间隔, 改完立即换成新间隔", right: String(heartbeatMs) },
                    { id: "bgRate", title: "背景动画速率", subtitle: "本插件背景的时间推进速度（1× 是基准，可选 0.5× / 1× / 2×），改完立即生效", right: bgRateText(bgRate) },
                ],
            },
            {
                kind: "button",
                title: skipAds ? "关闭『跳过广告曲目』" : "开启『跳过广告曲目』",
                style: "primary",
                action: { kind: "capability", name: "toggleSkipAds" },
            },
            {
                kind: "button",
                title: "切换背景动画速率（0.5× / 1× / 2×）",
                style: "primary",
                action: { kind: "capability", name: "cycleBackgroundRate" },
            },
            {
                kind: "button",
                title: "切换心跳间隔（10 / 30 / 60 秒）",
                action: { kind: "capability", name: "cycleHeartbeat" },
            },
            {
                kind: "button",
                title: "把提示语改回默认值",
                action: { kind: "capability", name: "resetGreeting", args: { value: "example_js 已加载" } },
            },
            { kind: "button", title: "测试网络通道", action: { kind: "capability", name: "fetchEcho" } },
        ],
    };
}

/// 写一个配置项: 先更新脚本侧记账, 再异步写 config.json (失败只记日志)
function saveConfig(key, value) {
    musicxx.storage.configCache[key] = value;
    musicxx.storage.setConfig(key, value).then(function () {
        return null;
    }, function (err) {
        musicxx.host.log(3, "写配置失败: " + err.message);
    });
}

/// 当前生效的心跳间隔 (配置还没读到 / 值非法时用默认值)
function configuredHeartbeatMs() {
    const value = musicxx.storage.configCache.heartbeatMs;
    return (typeof value === "number") ? value : DEFAULT_HEARTBEAT_MS;
}

/// 当前生效的背景动画速率倍率 (配置还没读到 / 值非法时用 1×)
function configuredBgRate() {
    const value = musicxx.storage.configCache.bgRate;
    return (typeof value === "number") ? normalizeBgRate(value) : 1;
}

musicxx.capability.register("settings", function () {
    return { view: settingsView(null) };
});

/// 设置页按钮: 切换"跳过广告曲目"
///
/// 功能页 (`card`) 的列表项也复用这个能力: 用 `args.view` 说明要刷新成哪个页面
/// (缺省 = 设置页)。
musicxx.capability.register("toggleSkipAds", function (args) {
    const next = !skipAdsEnabled();
    saveConfig("skipAds", next);
    musicxx.host.log(2, "设置页: 跳过广告曲目 → " + next);
    return {
        view: (args && args.view === "card") ? cardView() : settingsView({ skipAds: next }),
    };
});

/// 设置页按钮: 把提示语改回默认值
musicxx.capability.register("resetGreeting", function (args) {
    const value = (args && args.value) ? String(args.value) : "example_js 已加载";
    saveConfig("greeting", value);
    return { view: settingsView({ greeting: value }) };
});

/// 设置页按钮: 循环切换背景动画速率 (0.5× → 1× → 2× → 0.5×)
///
/// 速率是插件自己的设置项: 改完用 `musicxx.ui.updateEntry` 重新声明背景样式,
/// 宿主刷新候选后正在使用的背景立即用新速度 (无需重新选中)。
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
        view: (args && args.view === "card") ? cardView() : settingsView({ bgRate: next }),
    };
});

/// 设置页按钮: 循环切换心跳间隔 (10 → 30 → 60 秒 → 10)
musicxx.capability.register("cycleHeartbeat", function () {
    const current = configuredHeartbeatMs();
    let index = 0;
    for (let i = 0; i < HEARTBEAT_OPTIONS.length; ++i) {
        if (HEARTBEAT_OPTIONS[i] === current) {
            index = i;
        }
    }
    const next = HEARTBEAT_OPTIONS[(index + 1) % HEARTBEAT_OPTIONS.length];
    saveConfig("heartbeatMs", next);
    applyHeartbeat(next);
    return { view: settingsView({ heartbeatMs: next }) };
});

/// 能力: 供 Dart 侧 `plugin_call` 探针调用
musicxx.capability.register("probe", function (args) {
    const info = musicxx.host.info();
    return {
        pluginId: musicxx.pluginId,
        hookCount: 4,
        songChangedCount: songChangedCount,
        lastSongName: lastSongName,
        errorCount: errorCount,
        timerTicks: timerTicks,
        hostPlatform: info.platform || "",
        currentSongName: currentSongName(),
        uiEntries: musicxx.ui.entries().length,
        // 自读统计 (只观测不限制; 插件可据此显示自己的用量)
        selfStatsHooks: (musicxx.stats.getSelf() || {}).hooks || 0,
        crossPending: crossCallState.pending,
        crossOk: crossCallState.ok,
        crossKeys: crossCallState.keys,
        crossError: crossCallState.error,
        // 配置读取 (config.json; 默认值由脚本在 getConfig 里给)
        configSkipAds: musicxx.storage.configCache.skipAds,
        configGreeting: musicxx.storage.configCache.greeting,
    };
});

/// 能力: 演示宿主网络代理通道 (musicxx.net.fetch)
///
/// 说明: 这条通道是**可选便利能力**, `musicxx.net` 权限只表示
/// "允许用宿主代理通道"; 宿主不限定可访问的域名 (任意 http/https 地址都可以请求)。
/// 非 2xx 也会正常返回 (status 交给脚本判断); 只有传输失败/超时才是 ok=false + error。
let fetchState = { pending: 0, ok: 0, status: 0, bytes: 0, error: "" };
musicxx.capability.register("fetchEcho", function (args) {
    const url = (args && args.url) ? String(args.url) : "https://api.github.com/zen";
    fetchState = { pending: 1, ok: 0, status: 0, bytes: 0, error: "" };
    musicxx.net.fetch({ url: url, timeoutMs: 10000 }).then(function (resp) {
        fetchState = {
            pending: 0,
            ok: (resp && resp.ok) ? 1 : 0,
            status: (resp && resp.status) ? resp.status : 0,
            bytes: (resp && resp.bodyBytes) ? resp.bodyBytes : 0,
            error: (resp && resp.ok) ? "" : ((resp && resp.error) || "unknown"),
        };
        musicxx.host.log(2, "net.fetch 完成: ok=" + fetchState.ok + " status=" + fetchState.status);
    }, function (err) {
        fetchState = { pending: 0, ok: 0, status: 0, bytes: 0, error: err.message };
        musicxx.host.log(3, "net.fetch 失败: " + err.message);
    });
    return { accepted: 1, url: url };
});

/// 能力: 回读最近一次 net.fetch 的结果 (测试用)
musicxx.capability.register("fetchProbe", function () {
    return {
        pending: fetchState.pending,
        ok: fetchState.ok,
        status: fetchState.status,
        bytes: fetchState.bytes,
        error: fetchState.error,
    };
});

/// 能力: 重新读取 config.json (设置页改完配置后调用)
musicxx.capability.register("reloadConfig", function () {
    refreshConfig();
    return { accepted: 1 };
});

/// 插件页面: 主页入口与播放页附加信息块都指向 `ext://example_js/card`
///
/// 宿主打开这个页面时调用**同名能力** (`card`), 由脚本返回视图描述 ——
/// 页面内容由插件给, 排版与控件仍由宿主渲染 (text / divider / list / button 块)。
/// 页面里的按钮可以调用本插件能力 (返回 `{view:...}` 时直接刷新当前页)、
/// 执行官方动作, 或跳到另一个插件页面 (这里跳本插件的设置页)。
function cardView() {
    const cross = crossCallState.pending === 1
        ? "调用中"
        : (crossCallState.ok === 1
            ? ("成功, 返回 " + crossCallState.keys + " 个字段")
            : (crossCallState.error === "" ? "未调用" : ("失败: " + crossCallState.error)));
    const net = fetchState.pending === 1
        ? "请求中"
        : (fetchState.error !== ""
            ? ("失败: " + fetchState.error)
            : (fetchState.ok === 1 ? ("HTTP " + fetchState.status + ", " + fetchState.bytes + " 字节") : "未调用"));
    return {
        title: "JS 示例插件",
        subtitle: "页面内容来自能力 `card`, 渲染由宿主完成",
        blocks: [
            {
                kind: "text",
                text: "这个页面演示插件的声明式页面: 插件只返回块描述 (文本 / 列表 / 按钮), 不写界面代码。",
                style: "cross",
            },
            { kind: "divider" },
            {
                kind: "list",
                items: [
                    {
                        id: "songChanged",
                        title: "切歌次数",
                        subtitle: lastSongName === "" ? "还没有切过歌" : ("最后播放: " + lastSongName),
                        right: String(songChangedCount),
                    },
                    {
                        id: "errorCount",
                        title: "播放错误次数",
                        subtitle: "第 1 次错误建议换源, 之后交给宿主原有策略",
                        right: String(errorCount),
                    },
                    {
                        id: "timerTicks",
                        title: "定时器心跳",
                        subtitle: "每 " + configuredHeartbeatMs() + " 毫秒写一条日志",
                        right: String(timerTicks),
                    },
                    {
                        id: "uiEntries",
                        title: "已注册的 UI 项",
                        subtitle: "主页入口 / 歌曲菜单 / 播放页背景",
                        right: String(musicxx.ui.entries().length),
                    },
                    {
                        id: "crossCall",
                        title: "跨插件调用",
                        subtitle: "目标: example_native 的 probe 能力",
                        right: cross,
                    },
                    {
                        id: "netFetch",
                        title: "宿主网络通道",
                        subtitle: "musicxx.net.fetch 最近一次结果",
                        right: net,
                    },
                    {
                        id: "background",
                        title: "播放页背景",
                        subtitle: "在『设置 → 播放页面背景』里也能选; 这里读的是状态镜像",
                        right: backgroundStateText(),
                    },
                    {
                        id: "bgRate",
                        title: "背景动画速率",
                        subtitle: "点这一条循环切换 0.5× / 1× / 2×（1× 是基准速度）",
                        right: bgRateText(configuredBgRate()),
                        action: { kind: "capability", name: "cycleBackgroundRate", args: { view: "card" } },
                    },
                    {
                        id: "skipAds",
                        title: "跳过广告曲目",
                        subtitle: "点这一条直接切换 (等于设置页里的开关)",
                        right: skipAdsEnabled() ? "已开启" : "已关闭",
                        action: { kind: "capability", name: "toggleSkipAds", args: { view: "card" } },
                    },
                ],
            },
            {
                kind: "button",
                title: "刷新本页",
                style: "primary",
                action: { kind: "capability", name: "card" },
            },
            {
                kind: "button",
                title: "调用 example_native 的能力",
                action: { kind: "capability", name: "crossCall", args: { target: "example_native", method: "probe" } },
            },
            { kind: "button", title: "测试宿主网络通道", action: { kind: "capability", name: "fetchEcho" } },
            {
                kind: "button",
                title: "打开本插件设置页",
                action: { kind: "route", route: "ext://example_js/settings" },
            },
            {
                kind: "button",
                title: "使用本插件的动态背景",
                style: "primary",
                action: { kind: "capability", name: "useBackground", args: { id: "plugin.example_js.bg" } },
            },
            {
                kind: "button",
                title: "切回内置背景",
                action: { kind: "capability", name: "useBackground", args: { id: "builtin:Auto" } },
            },
        ],
    };
}

/// 一键切换播放页背景 (也可以切到内置样式: id 用 "builtin:<内置样式名>")
///
/// `musicxx.render.select` 是宿主官方动作: 选中后立即生效, 用户在设置里随时能改回来。
/// 能力处理器必须同步返回, 所以这里只说明"已请求", 实际结果看 card 页刷新后的状态。
function useBackground(args) {
    const id = (args && args.id) ? String(args.id) : "plugin.example_js.bg";
    // 先记一条：确认"按钮有没有点到插件"（排查时看宿主日志里有没有这一行）
    musicxx.host.log(2, "收到切换播放页背景请求: " + id);
    lastBackgroundRequest = id;
    musicxx.call("musicxx.render.select", { slot: "player.background", id: id }).then(function (r) {
        musicxx.host.log(2, "切换播放页背景成功: " + JSON.stringify(r));
    }, function (err) {
        musicxx.host.log(3, "切换播放页背景失败: " + err.message);
    });
    return { requested: 1, id: id };
}

musicxx.capability.register("useBackground", function (args) {
    useBackground(args);
    return { view: cardView() };
});

musicxx.capability.register("card", function () {
    return { view: cardView() };
});

console.log("example_js 已加载 (pid=" + musicxx.pluginId + ")");
