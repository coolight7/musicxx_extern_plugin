/// musicxx 外部插件示例 (JS, 零编译)
///
/// 行为与 `plugins/example_native` 等价, 用来验证两条链路一致:
/// - `musicxx.player.beforePlaySong` 裁决: 含"广告"的曲目 → skip;
/// - `musicxx.song.changed` 观察: 切歌时记录名称并累加计数;
/// - `musicxx.player.error` 裁决: 首次错误时建议换源 (patch.tryNextSrc);
/// - `musicxx.player.speed` 裁决 (异步): 处理器返回 Promise 也能生效 (限速演示);
/// - `example_js.probe` 能力: 返回自检信息 (计数/线程/状态镜像读取)。
/// - 声明式设置页 + 宿主网络代理通道 (musicxx.net.fetch) 演示。
///
/// 约束 (plan §7.2): 脚本顶层必须**同步**完成注册 (顶层不能用 await);
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
musicxx.hooks.register("musicxx.player.beforePlaySong", { mode: "decision", priority: 0 }, function (ctx) {
    const name = (ctx && ctx.song && ctx.song.name) ? String(ctx.song.name) : "";
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

/// 定时器: 每 30 秒写一次日志 (演示定时器链路; 卸载/禁用时宿主自动清理)
musicxx.timer.setInterval(function () {
    timerTicks += 1;
    musicxx.host.log(2, "example_js 心跳: 切歌 " + songChangedCount + " 次, 错误 " + errorCount + " 次");
}, 30000);

/// 声明式 UI 扩展 (plan §5.6): 主页入口项 + 歌曲菜单项
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

/// 声明式设置页 (plan §5.6): 应用「设置 → 插件设置」里会出现这个页面,
/// 页内控件读写插件目录的 config.json (与清单 settings_schema 是同一份配置)。
musicxx.ui.registerEntry({
    name: "settings",
    type: "settings.page",
    order: 120,
    data: {
        title: "JS 示例插件设置",
        depict: "示例：开关 / 文本 / 数字 / 下拉 / 说明 / 按钮",
        groups: [
            {
                title: "基础",
                items: [
                    { kind: "switch", key: "skipAds", title: "跳过广告曲目", depict: "播放前裁决：名字含『广告』的曲目直接跳过" },
                    { kind: "number", key: "heartbeatMs", title: "心跳间隔(毫秒)", min: 5000, max: 600000 },
                    { kind: "input", key: "greeting", title: "启动提示语", placeholder: "加载时弹出的提示" },
                    { kind: "select", key: "logLevel", title: "日志级别", default: "info",
                      options: [ { value: "debug", label: "调试" }, { value: "info", label: "信息" } ] },
                    { kind: "info", text: "提示：这里改的值会立刻写入 config.json；脚本可用 musicxx.storage.getConfig 读取。" },
                    // 只读块（不写配置）：进度条与列表，用来展示插件自己的状态
                    { kind: "progress", title: "示例进度", depict: "只读：由插件声明 value/total", value: 1, total: 4 },
                    { kind: "list", title: "本插件注册的钩子", items: [
                        { title: "musicxx.player.beforePlaySong", depict: "跳过广告曲目" },
                        { title: "musicxx.song.changed", depict: "切歌日志" },
                        { title: "musicxx.player.speed", depict: "异步裁决：限速" },
                    ] },
                    { kind: "button", title: "测试网络通道", action: { kind: "capability", name: "fetchEcho" } },
                ],
            },
        ],
    },
});

/// 播放页附加信息块 (musicxx.ui.overlay.widget, 只读展示)
///
/// 位置: `player.top`(顶部栏下方) / `player.bottom`(进度条上方);
/// 内容: `content.kind` = text / markdown / list / progress。
/// 这里注册一个 Markdown 块 (点击跳转到本插件页面), 与原生示例插件的纯文本块对应。
musicxx.ui.registerEntry({
    name: "overlayInfo",
    type: "overlay.widget",
    order: 20,
    data: {
        position: "player.top",
        title: "JS 示例插件",
        content: { kind: "markdown", text: '**example_js**：附加信息块（只读，点击查看插件页面）\n\n- 支持 `text` / `markdown` / `list` / `progress`\n- 内容只展示，不参与播放逻辑' },
        action: { kind: "route", route: "ext://example_js/card" },
    },
});

/// 通知 (等价动作 `musicxx.ui.notify`; fire-and-forget)
musicxx.ui.notify({ text: "example_js 已加载", kind: "info" }).then(function () {
    return null;
}, function (err) {
    musicxx.host.log(3, "发送通知失败: " + err.message);
});

/// 能力: 跨插件调用 (JS → 原生/内置插件; 结果为 Promise, 宿主线程执行)
///
/// - 目标是 JS 插件时同线程直接调用 (结果立即就绪);
/// - 目标是原生插件时投递到宿主线程执行, 脚本不阻塞 (宿主线程可能正等 JS 处理器)。
/// 能力处理器必须**同步返回** (plan §7.2), 所以跨插件调用的结果用"最后一次结果"记账,
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

/// 配置读取: config.json 由用户改 (设置页/手改文件), 脚本侧读用 getConfig (异步)。
/// 能力处理器必须同步返回, 所以这里把最近一次读到的值记账, 由 probe 回读。
musicxx.storage.configCache = { skipAds: null, greeting: "" };
function refreshConfig() {
    musicxx.storage.getConfig("skipAds").then(function (value) {
        musicxx.storage.configCache.skipAds = value;
    }, function () { return null; });
    musicxx.storage.getConfig("greeting").then(function (value) {
        musicxx.storage.configCache.greeting = value === null ? "" : value;
    }, function () { return null; });
}
refreshConfig();

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
        // 自读统计 (plan §4.11: 只观测不限制; 插件可据此显示自己的用量)
        selfStatsHooks: (musicxx.stats.getSelf() || {}).hooks || 0,
        crossPending: crossCallState.pending,
        crossOk: crossCallState.ok,
        crossKeys: crossCallState.keys,
        crossError: crossCallState.error,
        // 配置读取 (config.json; 缺省值由清单 settings_schema / 设置页声明)
        configSkipAds: musicxx.storage.configCache.skipAds,
        configGreeting: musicxx.storage.configCache.greeting,
    };
});

/// 能力: 演示宿主网络代理通道 (musicxx.net.fetch)
///
/// 说明 (plan §7.4 第 5 条): 这条通道是**可选便利能力**, `musicxx.net` 权限只表示
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

console.log("example_js 已加载 (pid=" + musicxx.pluginId + ")");
