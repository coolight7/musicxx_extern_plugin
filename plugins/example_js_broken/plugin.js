/// 对照示例: **故意留下语法错误**的插件脚本
///
/// 演示/验证「脚本错误 → start 事务失败 → 宿主回滚且继续可用」。
/// 下面这个函数的括号没有闭合: QuickJS 在求值阶段就会抛异常, 因此本插件永远装载失败 ——
/// 这是刻意的（对照示例），不要照着它写自己的插件。

musicxx.hooks.register("musicxx.player.beforePlaySong", { mode: "decision" }, function (ctx) {
    return { action: "skip" };
}
