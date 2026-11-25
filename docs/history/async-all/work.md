# 全异步化实施记录

## 当前进度

方案已确认（2026-11-25）。阶段 1-9 已走完：代码、测试、文档与版本号都改完，Windows 上构建并
实跑过一次（JS 示例插件加载、播放页背景与能力调用都正常，关窗口退出码 0、没有新的崩溃报告）。
没有验证的平台与场景记在文末「未验证」。

按阶段推进，每完成一步在此记"改了哪些文件 + 怎么验证"。

## 任务列表

- [x] 阶段 0：基线数量已记录（原生 518 + 283 checks）；"慢处理器占住框架"的对照用例没有单独加，
      改由原生测试里"`create` 里发起能力请求、等它 5 s 超时才能继续"与"停机前受理一个异步完成的能力调用"覆盖
- [x] 阶段 1：子模块 pluginxx 删 `sync`、能力表 v9、插件 ABI 10、`AsyncReply`
- [x] 阶段 2：宿主派发改协程链并搬到插件线程；删 `run_sync`；原生内容生成搬插件线程；
      内部函数去 `Async` 后缀
- [x] 阶段 3：JS 处理器统一异步完成
- [x] 阶段 4：C ABI 7（全部操作提交化，含发布 / 订阅 / 日志 / 回复 / 关机；删 `plugin_load_sync`）
- [x] 阶段 5：Dart 包请求总线 + 全 `Future` + 本地缓存 + 去掉 `_async` 命名（`lib/` 分析 0 error / 0 warning）
- [x] 阶段 6：主程序接入（Store / 内容传输 / 广播取值 / 能力调用）（`lib` 分析 0 error / 0 warning）
- [x] 阶段 7：示例插件、原生测试、Dart 测试改造
      （原生 283 + 516 checks、包内 67 项、主程序 1786 项全过）
- [x] 阶段 8：文档与版本号（C ABI 7 / 插件 ABI 10 / 框架 `2026.1125.1`）
- [x] 阶段 9：全量验证（原生 283 + 516 checks、包内 67 项、主程序 1786 项、Windows 构建与实跑）

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

### 阶段 7：示例插件（已完成）

- `src/sdk/include/musicxx/plugin/api/plugin_kit.h`：删 `spec.sync = ...`（能力表已经没有这个字段）；
  处理器只有一种形态 —— 受理时给回句柄、结束时调一次 `notify.done`，同步写法的结果在 `start`
  里当场交回。`handlerReturnsAwaitable` 保留（现在只用来区分写法，不再影响登记）。
- `plugins/example_native_fail`：删 `spec.sync = 1` 与前链"必须是同步处理器"的注释。
- JS 示例插件不用改：JS 接口形状没变（处理器本来就可以返回 Promise）。

**验证**：`pwsh -NoProfile -File tools/build_native.ps1 -WithExamples` → 通过（插件框架库 + SDK +
四个示例插件）。

**待办（阶段 7 剩余）**：
- 主程序 `test/`（`feature_provide_test` 里的 Dart 实现链、`broadcast_value_test` 的假 sink、
  `cli_endpoint_test` 的广播回调、`extern_plugin_test` 的 `dispatchAsync` / `inspectPlugin` 形状）；
- `feature_provide_test` 原来靠 `capabilities.on/off` 在单测里跑 Dart 实现链，这条链已删：
  这些用例要改成"假的能力派发源"（覆写 `dispatch` 或直接构造裁决）。

### 阶段 7：原生测试与包内测试（已完成）

- `src/tests/test_host.cpp`：顶部加一层保持旧签名的同步包装（`t_host_start` / `t_plugin_load_sync` /
  `t_capability_dispatch*` / `t_broadcast_*` / `t_ui_*` / `t_stats*` / `t_debug_info` 等）：提交后等
  `musicxx.request.result` 按 `requestId` 配对，等到的其它事件攒起来放回（`pollEvents` 合并）。
  - 等待上限默认 30 s：插件 `create` 里发起的应用能力请求在原生测试宿主下没人回答，要等能力
    自带的 5 s 超时才能继续；
  - `waitCapabilityResult` 先从攒下的事件里找目标 `requestId`（等卸载结果时会把能力结果攒起来）；
  - 代次断言不再写死数字：`plugin_inspect` 报的 `kindInterfaceVersion` / `kindMinInterfaceVersion`
    两处必须相等，插件探针的 `hostApiVersion` 与它相等；
  - 旧事件名（`musicxx.plugin.call.result` / `musicxx.capability.decision.result`）的用例改成等
    `musicxx.request.result`，回执断言改成 "结果直接在回执里、没有 callId / async 标记"。
- 宿主两处账本修正（测试暴露的真问题）：
  - `finishRequestsOf` / `finishAllRequests` 支持排除"正在做这件事的那条请求"：卸载与停机以前
    会把自己那条请求按 `plugin_unloaded` / `host_stopped` 收尾，调用方拿到 -2 而不是卸载结论；
  - `plugin_call` 在受理阶段校验目标（未声明能力 / 实例不存在 / 归属不对 → 返回负码、不推事件，
    与协议一致），在框架线程上时直接判定，避免自己等自己。
- 包内 `test/`：
  - 现在返回 `Future` 的调用点全部补 `await`（`scan` / `inspect` / `load` / `unload` / `stats` /
    `uiSnapshot` / `uiSlots` / `registrationsOf` / `dispatch` / `notify` / 广播读写）；
  - `dispatchAsync` → `dispatch`，`dispatchAtPhase` → `dispatch(id, payload, phase)`；
  - 删掉 Dart 实现链用例（`capabilities.on` / `off` / `hasDartImplementation`）与异步派发计数；
  - `multi_isolate_test` 改成"事件队列是进程级共享"：worker 只提交并把 `requestId` 报回来，
    主 isolate 统一取结果并检查每条请求恰好一条结果；
  - 修 `capabilities.refreshNativeHandlers`：插件框架回的是 `{capability, count, handlers}`，
    之前把整个 `data` 当数组，恒为空列表；
  - `MusicxxPluginVersion.apiVersion = 7` 成为加载库时的默认期望值，`version_test` 加一致性用例。

**验证**：
- `pwsh -NoProfile -File tools/build_native.ps1 -WithTests -RunTests` → `musicxx_pluginxx_test`
  283 checks、`musicxx_extern_plugin_test` 516 checks 全绿；
- `fvm flutter analyze --no-pub lib test` → 无问题；`fvm flutter test --no-pub` → 67 项全过；
- `fvm dart run tools/gen_contract.dart --check` → 生成物与定义一致。

### 阶段 7 补充：主程序 `test/`（已完成）

- `feature_provide_test`：原来靠 `capabilities.on/off` 在单测里跑 Dart 实现链，这条链已删。
  新增 `_FakeCapabilities extends MusicxxPluginCapabilities`（`on` / `off` / `active` / `hasHandlers` /
  `nativeHandlerCount` / `dispatch`），handler 形参改成 `Map<String, Object?>? Function(Map<String, Object?> input)`，
  用例只验最终裁决（合并规则在 C++ 里）。
- `broadcast_value_test`：假 sink 的 `publish` / `list` / `read` 改成 `Future`；`host.list()` / `read()` /
  `debugInfo()` / `debugText()` 补 `await`；`ExternBroadcastHost_c.publish` 对出口的**异步失败**补一条日志
  （以前是 `.ignore()`，失败无声），同步抛仍然当场返回 `false`。
- `cli_endpoint_test` / `mpv_control_test`：广播回调改成 `async`。
- `extern_plugin_test`：`ExternPluginArchive.extract` 的 `inspectPlugin` 回调改成 `Future`（本地 helper 仍收
  同步假值、内部包一层）；`dispatchAsync` → `dispatch`；快速路径基准改成和"空 `await`"比（`dispatch` 是
  `Future`，循环里每次 `await` 本身就占 3 µs 上下，`publish` 仍是 0.03 µs）。

**验证**：`flutter test test/main_test.dart --no-pub` → 1786 项全过；
`flutter analyze --no-pub --no-fatal-infos lib test` → 0 error / 0 warning。

### 阶段 8：版本号与 CHANGELOG（先做的部分）

- `lib/src/version.dart`：`2026.1124.1` → `2026.1125.1`（日期 `2026-11-25`）；`pubspec.yaml` 同步；
  `src/include/musicxx_extern_plugin_api.h` 的 `MUSICXX_EXTERN_PLUGIN_API_VERSION` 早已是 7、版本串已是
  `2026.1125.1`。
- `CHANGELOG.md`：新增一节“处理器与 Dart 入口全异步，C ABI 7”（插件 AB​I 10、能力表 v9、只在有的同步
  函数、合并掉的三个事件、队列满拒绝受理、Dart 包全 `Future` 与 `valueOf` 读缓存）。
- **验证**：`fvm flutter test test/version_test.dart` → 7 项全过；
  `fvm dart run tools/gen_contract.dart --check` → 生成物与定义一致。
### 阶段 8：文档同步（已完成）

包内 `docs/`：

- `plugin-native-api.md`：删掉"同步处理器 / `sync = 1` / 前链后链只收同步处理器"；改成"普通函数与协程两种
  写法、任何阶段都收"；排障表两条改成叙述处理器阻塞的后果；`api_version` 与接口代次 `9 → 10`。
- `plugin-api.md`：派发方式一句改成"只有提交 + 结果事件"；`musicxx.capability.decision.result` 从"内部事件"
  清单删掉（裁决结果就是这次请求的返回值）；`await dispatchAsync` → `dispatch`。
- `plugin-guide.md` / `plugin-js-api.md`：前链 / 后链"必须同步处理器"改成"两种写法都收"；
  `decision.result` 从内部事件清单删掉。
- `tools/gen_capabilities.dart`：生成文档里 `dispatch` 字段的说明改成"现在全是 async"；
  `tools/capabilities.def.json` 剩下两条 `"dispatch": "sync"`（`musicxx.ui.slot.info` / `.vars`）改 async；
  重新生成 `plugin-capabilities.md` / `capability_ids.g.dart` / `capability_ids.g.h`。
- `version-history.md`：插件 ABI `9 → 10`、C ABI `6 → 7`、native 接口代次对 `9/9 → 10/10`，
  并加一行 `2026.1125.1`（提交号待补记）。

主程序 `docs/`：

- `docs/architecture/08-extern-plugin.md`：线程一段改成"只有提交与事件两种交互"；能力派发一段删掉
  `sync` / `dispatchAsync` / `callId` / `decision.result`，改成 `requestId` + `musicxx.request.result`；
  第 10 条重写成"每个操作只有一种形状：提交 + 结果事件"（受理语义、确认事件、同步入口只剩哪些、
  `host.shutdown` 收尾、纯注册表读也走提交 + 本地缓存）；两条踩坑记录标注"全异步化后不会再互锁"。
- `docs/architecture/09-extern-plugin-integration.md`：`await dispatchAsync` → `dispatch`；
  "回执只服务异步裁决的 callId"与"接入的能力一律走异步派发（sync 的能力没接入）"改成现在的说法。
- `AGENTS.md` §7.6：`await dispatchAsync` → `await dispatch`；C ABI `6 → 7`、native 接口代次 `8/8 → 10/10`。

**验证**：`fvm dart run tools/gen_contract.dart --check` → 一致；`fvm flutter analyze --no-pub lib test` → 无问题；
`fvm flutter test --no-pub` → 67 项全过。

### 阶段 9：全量验证（已完成）

按 `plan.md` §6 的命令逐条跑：

| 命令 | 结果 |
|---|---|
| `pwsh -NoProfile -File tools/build_native.ps1 -WithTests -RunTests` | 构建标记 `framework, examples, tests`；`musicxx_pluginxx_test` 283 checks、`musicxx_extern_plugin_test` 516 checks，0 failed |
| `dart run tools/gen_contract.dart --check`（`fvm`） | 生成物与定义一致 |
| `fvm flutter test test/version_test.dart` | 8 项全过 |
| 包内 `fvm flutter analyze --no-pub lib test` / `fvm flutter test --no-pub` | 无问题 / 67 项全过 |
| 主程序 `flutter analyze --no-pub --no-fatal-infos lib test` | 0 error / 0 warning（138 条 info 是既有的样式提示） |
| 主程序 `flutter test test/main_test.dart --no-pub` | 1786 项全过 |
| 主程序 `flutter build windows --debug --no-pub` | 成功（约 14 分钟）；`musicxx_extern_plugin.dll`（36 MB）与 `libmediaxx.dll` 一起进输出目录 |

**Windows 实跑**（本机已有 profile，插件目录里是两个 JS 示例插件 `example_ftp_source` /
`example_js_playing_bg`）：

- 启动日志：`外部插件框架已启动`（库路径指向 `.native/output/windows-x64-release`）→
  `广播已挂上插件框架` → 两个插件各自的加载日志；`example_ftp_source` 的 FTP 测试服务端起来了；
- 播放页背景：`example_js_playing_bg` 收到 `封面变化` 广播、调 `musicxx.media.spectrum`（返回
  `status = none`）、调 `musicxx.media.palette` 写回取色 —— 广播订阅、能力调用、插件能力回调
  在真实应用里都走通；
- 音源解析：`example_ftp_source` 的解析处理器被问了一次并安静让给后面的实现（日志
  `跳过不是资源 id 的地址`）；
- 播放：恢复到上次那首歌并正常起播（`playback_start` → `playback`），插件背景生效；
- 退出：关窗口后进程退出码 0，`log/` 下没有新的 `native_crash_*.log`（最新一份是 11-25 10:19，
  本次运行在 15:34 / 15:39 / 15:48 三次，都没留下崩溃报告）。

**顺手修掉一个收尾竞态**（主程序 `ExternPluginStore`）：`_teardown()` 改成 `Future`，`await host.dispose()`
（内部会等 `host.shutdown` 的结果再释放句柄）；`restartHost` / `followPlusSwitch(关闭)` / `clearAllPlugins`
改成 `await _teardown()` —— 全异步化之后 shutdown 不再是同步调用，不等它就去建新宿主会出现两个宿主
同时存在。`onClose`（同步回调）保持提交后不管。

反过来说，应用**退出**时仍然不等 shutdown（`BXAppState.dispose()` 是同步的，没有可等待的退出钩子）；
关窗口走的是引擎销毁，插件 `stop` 不保证跑完，这一点与本次改动前一致，已记在「未验证」的后续事项里。

## 未验证

（实施后补：只做了构建与单测，Windows 上实跑过；下面这些没有验证。）

- **平台**：只构建了 Windows x64（debug）。Android / iOS / macOS / Linux 的插件框架库没有重建，
  主程序也没有在这四个平台上构建。
- **动态库插件**：应用插件目录里只有两个 JS 示例插件，没有把动态库示例插件装进去实跑。
  动态库插件的加载 / 能力派发 / 停用 / 卸载由包内 `host_test.dart`（真实 DLL）与原生测试覆盖。
- **退出时的插件收尾**：`BXAppState.dispose()` 是同步的，等不了 `host.shutdown`；关窗口走引擎销毁，
  插件的 `stop` 不保证跑完（与改动前一致）。Windows 上关窗口退出码 0、没有新的崩溃报告，但
  "退出时每个插件的 `stop` 都跑完"没有验证。
- **性能**：没有跑 `benchmark/`，`app_ready` 时序只有启动日志能看出插件加载在后台，没有基线对比数字。
- **慢插件**：处理器里 `co_await sleep(2s)` 期间管理页仍可用的手工确认没做（原生测试里等 30 s 的
  用例间接覆盖了"框架线程不被处理器占住"）。
- **应用能力回环**：动态库插件在协程处理器里 `co_await capabilityCall` 再回到应用这条完整链路，
  只在原生测试里间接跑过（执行器与事件通路），没有在真实插件里跑过。
