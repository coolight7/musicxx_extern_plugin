# 任务：插件框架全异步化（删掉同步处理器与 Dart 同步等待）

## 目标

插件框架与主程序之间的调用**只有一种形状**：提交返回 `requestId`，结果经事件交回，
Dart 侧包成 `Future`。具体删掉两类同步形状：

- 插件侧同步处理器：`PluginxxCapabilitySpec.sync`、`MusicxxPluginCapabilitySpec.run_sync`、
  JS 的 `JsEngine::implementationRunSync`，以及宿主在框架 IO 线程上就地跑处理器链的做法；
- Dart 侧同步等待：`plugin_load_sync`、`capability_dispatch` 的 `DISPATCH_SYNC`、
  `ui_request`，以及全部经 `onHostThread`（`ioCallSync`）等待的查询 / 命令。

理由是插件框架要支持动态库、JS 与以后可能的其它语言，跨语言调用不适合同步；
两套处理器写法也让插件作者与文档都要多背一份。

## 范围

改：

- 子模块 `src/third_party/cxx_pluginxx`（能力规格删 `sync`、能力表 v9、`AsyncReply`）；
- 本包 `src/include/musicxx_extern_plugin_api.h`（C ABI 6 → 7）、`src/host/**`、
  `src/sdk/**`、`src/host/js/**`、`plugins/**`、`src/tests/**`；
- Dart 包 `lib/src/**`（请求总线、全 `Future`、缓存）；
- 主程序 `lib/plugin/externPlugin/**` 与 `test/modules/extern_plugin_test.dart`；
- 文档与版本号。

不改：能力 / 广播的**语义**（前链正序累积、后链逆序替换、merge 规则、`cancel` / `skip` /
`drop` / `stop`、订阅限频、事件里的 `rev` 与丢弃过期结果）。

## 状态

**已实施完成**（2026-11-25 开工，同一轮走完阶段 1-9）。`plan.md` §8 的六条边界按原样实现：

1. 纯注册表读也走提交，全部异步化；
2. 无需结果的操作同样支持异步等待（保证顺序 / 确认前置完成）；
3. 删除全部同步函数，名称不再带 `_async` / `Async`；
4. 版本号：C ABI 7、插件 ABI 10、能力表 v9、框架 `2026.1125.1`；
5. 删除 Dart 侧 `capabilities.on/off`；
6. `initDo` 只等 `host.start` 的结果，插件扫描与加载放后台。

阶段划分与每阶段验证见 [plan.md](plan.md) §5，进度记录见 [work.md](work.md)。

## 关键决策

- **一条规则**：C ABI 的每个操作都只有"提交 + 结果事件"一种形状；返回 `0` = 已受理且一定有
  一条 `musicxx.request.result`，返回负码 = 未受理且不会有结果。
- **"没有结果"不等于"不能等待"**：发布、订阅、日志、回复、关机同样回一条确认，
  要顺序或要确认前置完成时直接 `await`；不关心就 `.ignore()`。不设"通知式入口"这一分类。
- **只保留两类同步函数**：`host_create` / `host_destroy`（句柄），以及事件通道与纯函数
  （`poll_events` / `set_wake_callback` / `api_version` / `version` / `result_name` /
  `feature_bits` / 内存与字符串函数）。`host_start` / `host_shutdown` 走提交。
- **名称不加 `Async`**：`plugin_load_sync`、`plugin_call_async`、`loadAsync`、`dispatchAsync`
  这类命名统一去掉；宿主内部返回 `asio::awaitable` 的函数也不加后缀。子模块共用的
  `sleepAsync` / `asyncOffload` 例外，不改名。
- **插件代码只在插件执行线程**（JS 在 JS 线程）：框架 IO 线程只做注册表、事件与统计。
- **Dart 不为读一个值同步调 FFI**：广播当前值、处理器计数/列表、已加载插件都读本地缓存，
  缓存由事件维护；冷数据用一次提交补。
- **结果事件不裁剪**：队列满时拒绝受理（`PLUGINXX_RESULT_ERR_FULL`），不替调用方丢结果。
- **不保留旧写法**：插件 ABI 9 → 10、C ABI 6 → 7、能力表 v8 → v9；旧插件与旧 Dart 包都要重编。

## 结论

- **接口形状收敛成一种**：C ABI 每个操作都是"提交拿 `requestId` → 结果事件
  `musicxx.request.result`"，返回 0 = 已受理且恰好一条结果、负码 = 未受理不会有事件；
  同步入口只剩 `host_create` / `host_destroy`、事件通道与纯函数。能力处理器只剩 `run_start`
  一种形态（不再有 `sync` 标记），前链 / 后链也能写协程。
- **Dart 侧**：`MusicxxPluginRuntime` 的调用全是 `Future`，`capabilities.on/off` 与同步派发删除；
  热路径（广播当前值、处理器计数、已加载插件）读本地缓存，缓存由事件维护。
- **验证**：原生 283 + 516 checks、包内 67 项、主程序 1786 项全过；Windows 上构建并实跑，
  两个 JS 示例插件正常加载与工作，关窗口退出码 0、无新崩溃报告。
- **实施中改掉的判断**：`host.shutdown` / 卸载不再把自己那条请求按 `plugin_unloaded` /
  `host_stopped` 收尾（以前调用方拿到 -2）；`plugin_call` 在**受理阶段**校验目标（未声明能力 /
  实例不存在 → 负码且不推事件）；`_teardown()` 改成等 shutdown 完成再返回，重启 / 开关切换 /
  清理全部插件都 `await` 它（不等就可能同时存在两个宿主）。
- **留给后续的**：应用退出（`BXAppState.dispose()`）是同步的，等不了 `host.shutdown`，
  插件 `stop` 在退出时不保证跑完；其它平台（Android / iOS / macOS / Linux）没有构建；
  动态库插件没有装进应用实跑（由包内 Dart 测试与原生测试覆盖）。
