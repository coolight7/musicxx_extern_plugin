/// 测试夹具: 裁决型钩子处理器返回 Promise (异步裁决)
///
/// 用途:
/// - `musicxx.player.seek`: Promise 在宿主等待预算 (100 ms) 内结算 → 裁决生效;
/// - `musicxx.player.volume`: Promise 远超预算才结算 → 按"无裁决"继续
///   (不打断脚本、不计处理器失败), 迟到的结果被丢弃并计入 `asyncHookLateDrops`。
///
/// 注意: 本插件只用于原生测试, 不要安装到正式环境。

let runs = 0;      // 处理器被调用的次数 (验证"没有被暂停/跳过")
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

/// 裁决型钩子 (异步): 500 ms 后才结算 (宿主只等 100 ms, 因此按无裁决继续)
musicxx.hooks.register("musicxx.player.volume", { mode: "decision" }, function () {
    runs += 1;
    return new Promise(function (resolve) {
        setTimeout(function () {
            settled += 1;
            resolve({ action: "cancel" });
        }, 500);
    });
});

/// 能力探针: 回读宿主的异步裁决统计 (musicxx.stats.getSelf) 与脚本侧计数
musicxx.capability.register("probe", function () {
    const stats = musicxx.stats.getSelf() || {};
    return {
        pluginId: musicxx.pluginId,
        runs: runs,
        settled: settled,
        asyncHookSettled: stats.asyncHookSettled || 0,
        asyncHookTimeouts: stats.asyncHookTimeouts || 0,
        asyncHookLateDrops: stats.asyncHookLateDrops || 0,
    };
});

console.log("async_js 已加载 (仅测试用)");
