/// 对照示例: 观察型钩子的处理函数里死循环
///
/// 演示/验证宿主的可选执行上限（`jsExecGuardMs`，默认关闭）：
/// - 默认配置下宿主不会中断脚本，因此这个插件只用来验证「显式开启上限之后」的行为：
///   死循环会被 QuickJS 中断，共享 JS 线程不会被永久占住，其它 JS 插件照常工作；
/// - 顶层注册是同步且快速的（不阻塞装载），死循环只发生在钩子被触发之后。
///
/// 注意: 触发这个钩子会让脚本一直转，只用于演示与测试。

musicxx.hooks.register("musicxx.player.completed", { mode: "observe" }, function () {
    // 故意死循环: 只有开启可选执行上限时才会被打断
    let spins = 0;
    while (true) {
        spins += 1;
    }
});

console.log("example_js_spin 已加载 (对照示例)");
