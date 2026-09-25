/// 测试用 JS 插件: **故意留下语法错误**, 用于验证
/// "脚本错误 → start 事务失败 → 宿主回滚且继续可用" (对应用例 test_load_failure_graceful)。
///
/// 下面是未闭合的括号: QuickJS 在 JS_Eval 阶段就会抛异常。
musicxx.hooks.register("musicxx.player.beforePlaySong", { mode: "decision" }, function (ctx) {
    return { action: "skip" };
}
