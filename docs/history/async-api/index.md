# 任务：插件框架接口异步化（cxx_pluginxx 重构成 asio 协程之后）

## 目标

把 `musicxx_extern_plugin` 的插件侧接口整体改成异步，与重构后的 `cxx_pluginxx` 对齐：

- `musicxx.host` / `musicxx.ui` / `musicxx.kind` 三张业务表改成"统一操作原语"（`start` 返回
  `PluginxxOperatorHandle*`，结果经完成回调恰好一次交回）；
- 动态库插件 SDK（`plugin_kit.h`）改成 asio 协程写法：能 `co_await` 的接口全部给协程形态，
  生命周期事务与能力处理器都可以写成协程；
- JS 插件 SDK（`musicxx.*`）改成 Promise 异步，脚本里不再有"同步等主程序"的调用；
- 主程序（宿主）一侧的等待不再占用框架 IO 线程（IO 线程只做登记表与派发，长等待放到工作线程 /
  协程），这样插件协程才能在等待期间继续推进。

## 范围

改：`src/sdk/include/musicxx/plugin/api/plugin_api.h`、`plugin_kit.h`、`src/host/**`、
`js/`、`plugins/` 示例、`src/tests/**`、`docs/**`、版本号与 `CHANGELOG.md`。

不改：主程序（Dart）侧的 C ABI 形状与调用方式。除"派发可能变慢/变快"之外，Dart 侧看到的
入口不变（`capability_dispatch` 仍是同步 FFI，内部改成不占框架 IO 线程）。

## 状态

- 依赖：`cxx_pluginxx` 已由上游重构（表调用一律异步操作契约、插件协程统一到 `asio::awaitable`）；
  本包当前代码停留在重构前的同步 ABI，基线构建失败（业务表函数指针不匹配、内置插件入口签名、
  广播表订阅签名）。
- 已完成：业务表 ABI 异步化、宿主侧三张表与广播/网络表的实现、JS 内置运行器入口（框架库单目标构建通过）。
- 待完成：原生 SDK kit 协程化、宿主派发移出 IO 线程、JS SDK 异步化、示例与原生测试、文档与版本号。
- 结论：按阶段重写，不保留旧写法（主程序最低版本已抬高，未发布）。

详细方案见 [plan.md](plan.md)，实施记录与剩余工作见 [work.md](work.md)。
