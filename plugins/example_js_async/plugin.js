/// 对照示例: 裁决型钩子的处理器返回 Promise (异步裁决)
///
/// 演示/验证两件事:
/// - `musicxx.player.seek` 前链: Promise 很快 (30 ms) 结算 → 改过的目标位置生效;
/// - `musicxx.player.volume` 前链: Promise 1.5 秒后才结算 → 插件框架**一直等到它结算**
///   (没有等待超时, 也不会按"不裁决"继续), 拦下这次改音量。
///
/// 这两个能力的"改值"由应用自己执行 (实现固定): 插件只能在 `before` 改目标值 / 拦下,
/// 在 `after` 改最终值 / 作废, `impl` 阶段注册会被拒绝。
/// 这份示例由原生测试直接派发前链来验证插件框架行为。
/// 处理器返回 Promise 时请自己保证它能结算: 插件框架会一直等, 不结算就是调用点一直等。

let runs = 0;      // 处理器被调用的次数 (验证"没有被跳过")
let settled = 0;   // 脚本侧 Promise 结算的次数

/// 前链 (异步): 30 ms 后给出新的目标位置
musicxx.capability.implement("musicxx.player.seek", { phase: "before" }, function () {
    runs += 1;
    return new Promise(function (resolve) {
        setTimeout(function () {
            settled += 1;
            resolve({ patch: { to: 12345 } });
        }, 30);
    });
});

/// 前链 (异步): 1.5 秒后才结算 (插件框架会等到它结算): 拦下这次改音量
musicxx.capability.implement("musicxx.player.volume", { phase: "before" }, function () {
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
        asyncDispatchSettled: stats.asyncDispatchSettled || 0,
    };
});

console.log("example_js_async 已加载 (对照示例)");
