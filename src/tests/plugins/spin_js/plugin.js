/// 测试夹具: 观察型钩子的处理函数里死循环
///
/// 用途 (plan §4.10 的可选执行上限):
/// - 默认配置下宿主**不会**中断脚本 (决策 13), 因此这个插件只用来验证"显式开启
///   `jsExecGuardMs` 之后"的行为: 死循环会被 QuickJS 中断, 共享 JS 线程不会永久被占住;
/// - 顶层注册是同步且快速的 (不阻塞装载), 死循环只发生在钩子被触发之后。
///
/// 注意: 本插件只用于原生测试, 不要安装到正式环境。

musicxx.hooks.register("musicxx.player.completed", { mode: "observe" }, function () {
    // 故意死循环: 只有开启可选执行上限时才会被打断
    let spins = 0;
    while (true) {
        spins += 1;
    }
});

console.log("spin_js 已加载 (仅测试用)");
