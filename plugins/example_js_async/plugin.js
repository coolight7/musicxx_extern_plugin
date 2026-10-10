/// 对照示例: 裁决型钩子处理器返回 Promise (异步裁决)
///
/// 演示/验证两件事:
/// - `musicxx.player.seek`: Promise 很快 (30 ms) 结算 → 裁决生效;
/// - `musicxx.player.volume`: Promise 1.5 秒后才结算 → 插件框架**一直等到它结算**
///   (没有等待超时, 也不会按"无裁决"继续), 裁决同样生效。
///
/// 注意: 它会改变拖动进度/音量的裁决结果, 只用于演示与测试。
/// 处理器返回 Promise 时请自己保证它能结算: 插件框架会一直等, 不结算就是调用点一直等。
///
/// 这两个钩子 (`musicxx.player.seek` / `musicxx.player.volume`) 当前版本**主程序还没有
/// 接入** (约定已固定): 这份示例由原生测试直接派发它们来验证插件框架行为, 在主程序里跑不会触发。

let runs = 0;      // 处理器被调用的次数 (验证"没有被跳过")
let settled = 0;   // 脚本侧 Promise 结算的次数

/// 裁决型钩子 (异步): 30 ms 后返回 patch
musicxx.hooks.register("musicxx.player.seek", { mode: "decision" }, function () {
    runs += 1;
    return new Promise(function (resolve) {
        setTimeout(function () {
            settled += 1;
            resolve({ action: "continue", patch: { toMs: 12345 } });
        }, 30);
    });
});

/// 裁决型钩子 (异步): 1.5 秒后才结算 (插件框架会等到它结算, 裁决照常生效)
musicxx.hooks.register("musicxx.player.volume", { mode: "decision" }, function () {
    runs += 1;
    return new Promise(function (resolve) {
        setTimeout(function () {
            settled += 1;
            resolve({ action: "cancel" });
        }, 1500);
    });
});

/// 能力探针: 回读插件框架的异步裁决统计 (musicxx.stats.getSelf) 与脚本侧计数
musicxx.capability.register("probe", function () {
    const stats = musicxx.stats.getSelf() || {};
    return {
        pluginId: musicxx.pluginId,
        runs: runs,
        settled: settled,
        asyncHookSettled: stats.asyncHookSettled || 0,
    };
});

console.log("example_js_async 已加载 (对照示例)");
