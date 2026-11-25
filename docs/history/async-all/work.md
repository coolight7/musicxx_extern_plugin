# 全异步化实施记录

## 当前进度

方案已确认（2026-11-25）。阶段 1-4（子模块、宿主派发、JS、C ABI）已改完并通过插件框架库构建；
Dart 包（阶段 5）与主程序（阶段 6）进行中。

按阶段推进，每完成一步在此记"改了哪些文件 + 怎么验证"。

## 任务列表

- [ ] 阶段 0：基线对照用例（慢处理器占住框架）+ 记录现有测试数量（数量已记，
      对照用例放到阶段 7 与测试改造一起做）
- [x] 阶段 1：子模块 pluginxx 删 `sync`、能力表 v9、插件 ABI 10、`AsyncReply`
- [x] 阶段 2：宿主派发改协程链并搬到插件线程；删 `run_sync`；原生内容生成搬插件线程；
      内部函数去 `Async` 后缀
- [x] 阶段 3：JS 处理器统一异步完成
- [x] 阶段 4：C ABI 7（全部操作提交化，含发布 / 订阅 / 日志 / 回复 / 关机；删 `plugin_load_sync`）
- [x] 阶段 5：Dart 包请求总线 + 全 `Future` + 本地缓存 + 去掉 `_async` 命名（`lib/` 分析 0 error / 0 warning）
- [x] 阶段 6：主程序接入（Store / 内容传输 / 广播取值 / 能力调用）（`lib` 分析 0 error / 0 warning）
- [ ] 阶段 7：示例插件、原生测试、Dart 测试改造
- [ ] 阶段 8：文档与版本号（C ABI 7 / 插件 ABI 10 / 框架 `2026.1125.1`）
- [ ] 阶段 9：全量验证与基准

## 已确认的边界

1. 纯注册表读也走提交（`plugin_list` / `ui_*` / `stats` / `debug_info` / `capability_handlers`）。
2. 无需结果的操作同样回一条确认事件，可以 `await`；不关心就 `.ignore()`。
3. 删除全部同步函数，名称不再带 `_async` / `Async`；只留 `host_create` / `host_destroy`、
   事件通道与纯函数。
4. 版本号：C ABI 6 → 7、插件 ABI 9 → 10、能力表 v8 → v9、框架 `2026.1125.1`。
5. 删除 Dart 侧 `capabilities.on/off`。
6. `initDo` 只等 `host.start` 的结果，扫描与加载放后台。

两个实施取舍：`host_create` / `host_destroy` 保持同步（句柄）；子模块 `sleepAsync` /
`asyncOffload` 不改名（共用库公开名字）。

## 基线（开工前记录）

- 原生测试：`musicxx_extern_plugin_test` 518 checks / 0 failed；`musicxx_pluginxx_test` 283 checks / 0 failed。
- 契约生成物：`dart run tools/gen_contract.dart --check` 一致。
- 版本：框架 `2026.1124.1`、插件 ABI 9、C ABI 6、能力表 v8、JS 3/3。

## 每步记录

### 阶段 1：子模块 `cxx_pluginxx`

- `include/pluginxx/api/tables.h`：`PluginxxCapabilitySpec` 删 `sync`（处理器只有 `run_start` 一种形态）；
  `PLUGINXX_IFACE_CAPABILITIES_VERSION` / `MIN_VERSION` 8 → 9。
- `include/pluginxx/host/capability_registry.h`：`CapabilityProvider` 删 `sync`。
- `src/capability_registry.cpp`：自省 JSON 不再输出 `sync`。
- `include/pluginxx/host/host_core.h`：删 `row.sync = spec.sync != 0`。
- 新增 `include/pluginxx/api/async_reply.h`：`pluginxx::AsyncReply<T>`（等待侧协程、完成侧任意线程，
  `set` / `cancel` 只认第一次；唤醒用 `expires_at(min)` 而不是 `cancel()`，避免"定时器还没开始等"时
  取消丢唤醒）+ `pluginxx::readyValue(std::string)`。
  **来源是 Boost.Asio**：头里写 `boost/asio/...` 再给 `namespace asio = ::boost::asio;`（与 `pluginxx/kit/kit.h`
  同一做法）—— 写 `<asio/...>` 时这条头文件不经过 `utilxx_base/asio_error.h`，拿不到全局别名，编译报
  "asio 不是类或命名空间名称"。
- 插件 ABI 代次 9 → 10：本包 `src/CMakeLists.txt` 与 `src/host/CMakeLists.txt` 的
  `MUSICXX_EXTERN_PLUGIN_MIN_API_VERSION` 一处覆盖。

### 阶段 2：宿主派发

- `src/host/musicxx_capability_spec.h`：删 `MusicxxCapabilityRunSyncFn` / `run_sync`，只留
  `run_start` + `run_cancel` + `user_data` + `phase` + `priority`。
- `src/host/musicxx_host.h` / `.cpp`：
  - 新增请求表（`RequestOutcome`、`acceptRequest` / `completeRequest` / `markRequestPlugin` /
    `aliasRequest` / `pushRequestResult` / `submitOnHostThread` / `submitCoroutine` /
    `finishRequestsOf` / `finishAllRequests`）；`completeRequest` 先按别名换到外层请求，再补一条结果事件。
  - `plugin_load` 拆成"受理 + `loadPluginTransaction` 协程"；`enable` / `disable` / `unload` 同样
    受理 + `*Work` 协程；新增 `reload`（内部 unload + load 两个请求，用 `aliasRequest` 只让外层回结果）。
  - `pluginCall` 换掉 `pluginCallAsync`，结果并入请求表（删 `musicxx.plugin.call.result`）。
  - 内部返回 awaitable 的函数去掉 `Async` 后缀（`loadNativeDir` / `invokeKindProvider` / `loadHostedKind` /
    `startHostedKind` / `stopHostedKind` / `unloadHostedKind`）。
- `src/host/musicxx_host_tables.cpp`：派发改协程链 —— 受理（任意线程，只记账）→ 框架 IO 线程取快照 →
  `pluginxx::pluginExecutionExecutor` 上逐个 `co_await` 处理器 → 回 IO 线程写统计并 `completeRequest`。
  删 `invokeSync` / `CapabilitySyncReply` / `DispatchMode`；`registerHandler` 只要求 `run_start`。
  `HandlerReplyBridge` 改成**自己管生命周期**（等待侧 / 完成侧各记一个"我走了"，谁最后走谁释放）：
  插件卸载或停机时等待侧先走，之后处理器才结算也不能写到已释放的桥。
- `src/host/musicxx_host_ui.cpp`：`ui.request` 受理 + 投递；原生 `generate(...)` 改到插件执行线程调用；
  完成统一走 `finishUiGeneration`（写 rev、推 `musicxx.ui.content`、再用同一载荷 `completeRequest`）。
  顺手修了 `generate` 抛未知异常时不收尾（请求一直不完成）的问题。

### 阶段 3：JS 引擎

- `implementationRunSync` → `implementationStart`（`PluginxxCapabilityStartFunction` 形态）：投递到 JS 线程调
  `invokeImplementation`，脚本返回 Promise 就等结算，同步返回立刻完成。
- 等待槽 `WaitSlot` 换成"等待凭据"（`implementationWaits_` + `begin/deliver/finish/clear`）：宿主不再等任何线程，
  结果直接交回 `notify->done`（由原子标记保证恰好一次）。实例停止时把这批凭据按"没有意见"收尾。
- JS 侧 API 形状没变（处理器本来就可以返回 Promise），JS 接口代次保持 3。

### 阶段 4：C ABI 7

- `src/include/musicxx_extern_plugin_api.h` 重写：所有操作 `int64_t* out_request_id`，
  删 `plugin_load_sync` / `plugin_call_async` / `capability_handler_count(_phase)` / `DISPATCH_SYNC|ASYNC`，
  `host_stop` → `host_shutdown`，新增结果事件 `musicxx.request.result` 与
  `MUSICXX_EXTERN_PLUGIN_FEATURE_REQUESTS`。
- `src/host/musicxx_extern_plugin_api.cpp` 重写：删 `onHostThread` / `ioCallSync`，每个入口"复制入参 + 受理 + 投递"；
  `host_start` / `host_shutdown` 就地做（线程起停无法投递）再 `completeRequest`。

**验证**：`pwsh -NoProfile -File tools/build_native.ps1` → 通过（插件框架库 + SDK 安装完成）。
阶段 1-4 用一次构建一起验证（三者互相依赖：C ABI 的受理路径要用新的派发链，JS 的处理器要用
`run_start`）。原生测试与示例插件还没改，`-WithTests -WithExamples` 要等阶段 7。

**与计划不一致的地方**：
- 计划里的 `PLUGINXX_RESULT_ERR_FULL` 在共用码表里不存在，改用 `PLUGINXX_RESULT_ERR_BUSY`（`-7`，名称 `busy`，
  语义就是"排队已满"）。
- `capability_stats` 的 C 入口本来就不收作用域参数（`(h, out_request_id, log)`），Dart 侧 `stats()` 因此
  也没有 `scope` 参数。

### 阶段 5：Dart 包

- 新增 `lib/src/requests.dart`（`MusicxxPluginRequests`）：一次提交 = 一个 `requestId` + 一条
  `musicxx.request.result`；失败**不抛异常**（放在 `result.error` 里），带可选 `timeout`
  （到点给超时结果，晚到的丢弃）。
- `lib/src/runtime.dart`：`init` / `dispose` / `log` / `debugInfo` 全部改成 `Future`；新增
  `requests` 与 `hasHost`（提交式入口按"宿主在不在"判能不能受理：`host.start` / `host.shutdown`
  本身就在这个阶段发出去）；事件泵新增 `requestResult` 分支；`_shouldPollFallback` 改看 `hasHost`
  （启动期间也要能把结果取回来）。
- `lib/src/manager.dart`：全部方法改 `Future`，走请求总线；`loadAsync` 删除（`load` 就是异步）；
  `call` 走请求总线（原 `plugin_call_async` 与 `musicxx.plugin.call.result` 删掉）；
  生命周期事件触发的 `list()` 刷新改成一次在途 + 结束后补跑一次（`scheduleRefresh`）。
- `lib/src/capabilities.dart`：删 Dart 实现链（`on` / `off` / `MusicxxPluginImplementation` /
  `MusicxxPluginCapabilityContext`）与 `dispatchAtPhase`；`dispatch(id, [payload, phase])` 返回
  `Future`，结果取 `data.result`（与旧的可写回 `ExternCapabilityVerdict` 同形）；
  `notify` 改 `Future<void>`；`refreshNativeHandlers` / `stats` 改 `Future`；删
  `refreshNativeHandlerCount` / `pendingAsyncDispatches` / `handleDecisionResultEvent`；
  `handleCapabilityChangedEvent` 按事件载荷里的 `capability` 整条替换（一条事件只带一条能力）。
- `lib/src/broadcasts.dart`：`publish` / `read` / `list` 改 `Future`；新增 `valueOf(id)` 读 Dart 侧
  缓存（`musicxx.broadcast.changed` 维护，订阅时插件框架先补发当前值）—— 每帧取值的路径不再过 FFI。
- `lib/src/app_capabilities.dart`：`respond` 改 `Future`（`capability.respond` 提交），调用点 `.ignore()`。
- `lib/src/events.dart`：新增 `requestResult`，删 `capabilityDecisionResult` / `pluginCallResult`。
- 重跑 ffigen 生成 `lib/src/bindings_generated.dart`（新头文件的 `int64_t*` 参数与删掉的入口）。

**验证**：`fvm flutter analyze --no-pub lib` → 0 error / 0 warning（包内 `test/` 留到阶段 7）。

### 阶段 6：主程序接入

- `ExternPluginStore`：`_scanAndLoad` / `_loadPlugin` / `_refreshPlugins` / `rescan` /
  `_reloadPluginInstance` / `stats` / `capabilityStats` / `debugInfo` / `channelsDebugText` /
  `refreshUIItems` 改 `Future`；`initDo` 只 `await _startHost()`（`host.start` 的结果），扫描与加载
  `unawaited(_scanAndLoad())` 放后台，`app_ready` 不再被插件拖慢。`statusInfo()` 里的
  `capabilityCall` 固定 `async`、`pendingCapabilityCalls` 改名 `pendingRequests`（读请求总线计数）。
- `ExternPluginArchive.extract`：`inspectPlugin` 改成返回 `Future`，预检 `await`。
- `ExternBroadcastHost_c`：`ExternBroadcastSink_c` 的 `publish` / `list` / `read` 改 `Future`；
  `publish` 不等结果（`.ignore()`），`read` / `list` / `debugText` 等结果。
- `ExternCapabilityCall`：删同步 `dispatch`，`dispatchAsync` 改名 `dispatch`（唯一入口，返回 `Future`）；
  `refreshHandlers` 改 `Future`。
- `MusicxxUiContentTransport`：`syncAll` / `requestContent` 改 `Future`；事件驱动的同步改 `unawaited`。
- `MusicxxUiVariableSources` / `render/PluginRenderArgs`：插件频道的值改读 `broadcasts.valueOf`
  （每帧路径不过 FFI）；`watchBroadcast` 调用改 `unawaited`。
- `MusicxxMpvPropertyWatch_c.hostCountsOf` 改 `Future`（权威订阅数对账一次，异步不阻塞）。
- `MusicxxCliCommandsSetup_c`：广播列表 / 取值两个入口改 `Future`，命令回调改 `async`。
- 管理页（`ExternPluginPage` 调试页、`ExternPluginDetailPage`）：统计与广播都要等插件框架，改成
  "进页取一次快照 + `FutureBuilder`"（`刷新统计` 丢掉快照重取），不在 `build` 里发请求。

**验证**：`lib` 下 0 error / 0 warning；`test/` 里的旧调用点（Dart 实现链、同步广播、旧的
`ExternPluginArchive` 回调形状）留到阶段 7。

## 未验证

（实施后补：只做了构建、没有实跑的平台与场景。）
