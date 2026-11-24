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

已完成（2026-11-24）：

- 业务表 ABI 异步化（`musicxx.host` v3 / `musicxx.ui` v4 / `musicxx.kind` v2），入口符号去掉 `start`；
- 宿主三张业务表与广播 / 网络表按异步操作契约重写，失败原因改成原因文本；
- 原生 SDK kit 全量协程化（业务方法 `co_await`、事务与能力处理器可写成协程）；
- 插件代码移到独立的插件执行线程（单线程），框架 IO 线程只做登记与派发；
- JS 引擎与 JS SDK 异步化（能力处理器返回 Promise 时宿主立即返回，结算后补完成）；
- 示例插件、原生测试、文档、版本号（`2026.1124.1`，插件 ABI 9）同步完成。

原生测试全绿：`musicxx_extern_plugin_test` 518 checks / 0 failed，`musicxx_pluginxx_test`
283 checks / 0 failed。未验证 Android / iOS / macOS / Linux。

## 关键决策

- **不保留旧写法**：主程序的最低支持版本已经抬高且未发布，接口直接换成异步契约，
  不提供同步兼容层、不在文档里留"老写法"。
- **失败用原因文本，不用数字码**：数字只留在 C ABI 与 `pluginxx/api/result.h`；
  SDK 的业务方法统一抛异常（取消是 `CanceledException`），`what()` 以原因名开头。
- **插件代码不与框架 IO 同线程**：插件执行线程单条、顺序执行，卡住只影响这个插件；
  框架收发与主程序界面不受影响。
- **应用声明的能力仍然只收同步处理器**：这些点由应用就地派发并当场要结果，异步处理器会被拒绝；
  插件自己声明的能力可以给协程处理器。
- **插件 ABI 代次 8 → 9**：接口表签名变化属于破坏性变更，`api_version` 与示例清单一起抬。

结论与遗留见 [work.md](work.md) 的「注意到的问题」与「未验证」两节。
