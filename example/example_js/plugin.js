/// musicxx 外部插件示例 (JS, 零编译)
///
/// 行为与 `example/example_native` 等价, 用来验证两条链路一致:
/// - `musicxx.player.beforePlaySong` 裁决: 含"广告"的曲目 → skip;
/// - `musicxx.song.changed` 观察: 切歌时记录名称并累加计数;
/// - `musicxx.player.error` 裁决: 首次错误时建议换源 (patch.tryNextSrc);
/// - `example_js.probe` 能力: 返回自检信息 (计数/线程/状态镜像读取)。
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

/// 定时器: 每 30 秒写一次日志 (演示定时器链路; 卸载/禁用时宿主自动清理)
musicxx.timer.setInterval(function () {
    timerTicks += 1;
    musicxx.host.log(2, "example_js 心跳: 切歌 " + songChangedCount + " 次, 错误 " + errorCount + " 次");
}, 30000);

/// 能力: 供 Dart 侧 `plugin_call` 探针调用
musicxx.capability.register("probe", function (args) {
    const info = musicxx.host.info();
    return {
        pluginId: musicxx.pluginId,
        hookCount: 3,
        songChangedCount: songChangedCount,
        lastSongName: lastSongName,
        errorCount: errorCount,
        timerTicks: timerTicks,
        hostPlatform: info.platform || "",
        currentSongName: currentSongName(),
    };
});

console.log("example_js 已加载 (pid=" + musicxx.pluginId + ")");
