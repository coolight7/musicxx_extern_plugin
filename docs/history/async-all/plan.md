# 全异步化方案（删掉同步处理器与 Dart 同步等待）

## 1. 背景

`cxx_pluginxx` 的业务接口表已经改成"受理返回操作句柄 + 完成回调恰好一次"的异步契约，
插件 ABI 已抬到 9（框架版本 `2026.1124.1`）。但"宿主 ↔ 插件"之间还留着两类同步形状：

1. **插件处理器可以是同步的**：`PluginxxCapabilitySpec.sync`、宿主内部的
   `MusicxxPluginCapabilitySpec.run_sync`、JS 引擎的 `JsEngine::implementationRunSync`。
   宿主派发时在框架 IO 线程上就地调用它们，同步取结果。
2. **Dart 侧一批入口提交后原地等结果**：`plugin_load_sync`、`capability_dispatch` 的
   `DISPATCH_SYNC` 模式、`ui_request`，以及十几条用 `onHostThread`（`pluginxx::ioCallSync`）
   投递到框架线程并等待的查询 / 命令。

为什么要一次改掉：

- 插件框架要同时支持动态库插件、JS 插件与以后可能的其它语言。同步处理器在跨语言实现里
  必须"一方阻塞等另一方"，线程数一多就是互锁；JS 侧已经因为这个原因把处理器拆成两种写法。
- 同步处理器与异步处理器并存，插件作者要判断"这个 id 能不能用异步"，宿主也要为两种写法
  各写一条路径（`invokeSync` 与 `run_start`），文档与测试都翻倍。
- Dart 侧同步等待会把 Flutter 平台线程按在 FFI 调用里，插件慢就直接表现为界面卡顿。

用户决策（2026-11-25）：趁插件 ABI 刚抬过、主程序还没发布，**删掉同步处理器，删掉 Dart 的
同步等待，统一成异步**，不保留旧写法；**纯注册表读也走提交**；**无需结果的操作同样支持
异步等待**（要保证顺序或确认前置操作完成时可以直接 `await`）；**所有异步名称不再带
`_async` / `Async` 字样**（默认就是异步，不需要强调）。

## 2. 现状调查

### 2.1 线程模型

| 线程 | 创建位置 | 跑什么 |
|---|---|---|
| 框架 IO 线程（1 条） | `HostContext::thread`（`musicxx_host.cpp` 的 `start()`） | 注册表读写、事件推送、广播调度、超时定时器；**现在还在就地跑能力处理器与原生内容生成** |
| 插件执行线程（1 条） | `HostContext::pluginThread`（`musicxx_host.cpp` 的 `start()`，`pluginxx::bindPluginExecutor`） | 生命周期事务、插件协程、`pluginxx::asyncOffload` 的工作 |
| 工作线程池（2 条） | `HostContext::workers` | 网络表、`postToWorkerThread` |
| JS 线程（1 条） | `JsEngine::worker_`（`js_engine.cpp`） | 脚本求值、JS 处理器、JS 内容生成 |
| Dart UI isolate | Flutter | 包层对象、事件泵、主程序 |

IO 线程与插件执行线程不是同一条；Dart 与它们也都不是同一条线程。三者的等待方向必须单向：
**Dart 不等原生、框架线程不等插件线程、插件线程可以等框架线程**（插件调表）。

### 2.2 同步点清单

**（a）C ABI 里提交后等结果的入口**

| 入口 | 现在怎么等 | 等待期间被占住的 |
|---|---|---|
| `plugin_load_sync` | `WaitSlot` + `timeout_ms` | Dart 线程 |
| `capability_dispatch` / `capability_dispatch_phase`（`DISPATCH_SYNC`） | `WaitSlot`，处理器链在 IO 线程就地跑 | Dart 线程 + 框架 IO 线程 |
| `ui_request` | `onHostThread`，IO 线程就地调插件 `generate` | Dart 线程 + 框架 IO 线程 |
| `plugin_scan` / `plugin_inspect` / `plugin_list` / `plugin_set_args` / `plugin_get_config_path` | `onHostThread` | Dart 线程 |
| `ui_snapshot` / `ui_contents` / `ui_slots` | `onHostThread` | Dart 线程 |
| `stats` / `debug_info` / `stats_config` / `set_config` / `set_language` | `onHostThread` | Dart 线程 |
| `broadcast_read` / `broadcast_list` / `broadcast_watch` | `onHostThread` | Dart 线程 |
| `broadcast_publish` | 直接调注册表（本来就不转投） | 很短，但仍是同步 FFI |
| `capability_handler_count(_phase)` / `capability_handlers(_phase)` / `capability_stats` | `onHostThread` | Dart 线程 |
| `capability_app_respond` | 直接回答（不能转投，见下） | 很短 |
| `host_stop(timeout_ms)` | 等线程退出 | Dart 线程 |

`capability_app_respond` 现在的注释写着"不能转投 IO 线程再回答"：触发回复的处理器常常正占着
IO 线程等 JS（`JsEngine::implementationRunSync`）。把处理器链搬离 IO 线程之后这条限制消失，
它也可以走提交。

**（b）插件侧的同步处理器**

| 位置 | 现状 |
|---|---|
| `pluginxx` 的 `include/pluginxx/api/tables.h`：`PluginxxCapabilitySpec.sync` | `1` = 只给同步处理器；宿主 `onCapabilityRowAdded` 对应用声明的 id **拒绝** `sync == 0` |
| `src/host/musicxx_capability_spec.h`：`MusicxxPluginCapabilitySpec.run_sync` / `MusicxxCapabilityRunSyncFn` | 宿主内部的同步处理器形态 |
| `src/host/musicxx_host_tables.cpp`：`runCapabilityHandlers` / `invokeSync` / `CapabilitySyncReply` | 处理器链同步执行并当场合并结果 |
| `src/host/js/js_engine.cpp`：`JsEngine::implementationRunSync` | 在框架 IO 线程上 `slot->wait` 等脚本的 Promise 结算 |

**（c）Dart 包**

| 文件 | 同步入口 |
|---|---|
| `lib/src/manager.dart` | `load`（`plugin_load_sync`）、`scan`、`inspect`、`list`、`enable` / `disable` / `unload` / `reload`、`setArgs`、`configPath`、`stats`、`uiSnapshot` / `uiContents` / `uiSlots`、`requestUiContent`、`watchBroadcast`、`statsConfig` |
| `lib/src/capabilities.dart` | `dispatch` / `dispatchAtPhase`、`refreshNativeHandlers`、`on` / `off`（Dart 实现链，生产代码零调用点） |
| `lib/src/broadcasts.dart` | `read` / `list` / `publish` |
| `lib/src/runtime.dart` | `init`（`host_start`）、`debugInfo`、`log`、`dispose`（`host_stop(5000)`） |

**（d）主程序里的调用点**

| 位置 | 用途 |
|---|---|
| `ExternPluginStore._scanAndLoad` / `_loadPlugin` | 启动收尾：扫描 + 逐个加载（`onInitAppLast` 里 `await extern_plugin_s.initDo()`） |
| `ExternPluginStore.setPluginEnabled` / `reloadPlugin` / `_reloadPluginInstance` / `unloadPlugin` / `uninstall` / `rescan` | 管理页与命令行 |
| `ExternPluginStore.refreshUIItems` / `statsOf` / `debugInfo` | 界面项快照与自省 |
| `MusicxxUiContentTransport.syncAll` / `requestContent` | 内容注册表快照、按需生成 |
| `ExternBroadcastHost.read` / `list` / `publish` | 广播发布与自省 |
| `MusicxxUiVariableSources` / `render/PluginRenderArgs.variableValue` | **每帧/每次取值**读广播当前值 |
| `ExternCapabilityCall.dispatch` / `refreshHandlers` | 同步派发（生产代码无 `dispatch` 调用点）、自省回查 |

### 2.3 已经异步、这次不要重做的部分

- 事件泵：`NativeCallable.listener` + `poll_events`，`MusicxxPluginRuntime._dispatchEvent`。
- `plugin_load` / `enable` / `disable` / `unload` / `reload` 的结果事件。
- `plugin_call_async` + `musicxx.plugin.call.result`。
- `capability_dispatch(ASYNC)` + `musicxx.capability.decision.result`。
- 界面内容结果事件 `musicxx.ui.content`。
- 插件 → 应用能力：`musicxx.capability.app.request` / `capability_app_respond`。
- 应用侧裁决点 `MusicxxDecidePoint_c.decide` / `decideRequest`（已经是 `Future`，
  全部调用点都 `await`；`ExternCapabilityCall.dispatchAsync` 是唯一生产入口）。

## 3. 目标与原则

### 3.1 一条规则

**C ABI 只有一种操作形状：提交返回 `requestId`，结果经事件 `musicxx.request.result` 交回。**

```c
int32_t musicxx_extern_plugin_<操作>(
    MusicxxExternPluginHost*              h,
    /* 入参 */ ...,
    int64_t*                             out_request_id,  /* 受理成功时写出 */
    MusicxxExternPluginString*           log);            /* 未受理时写原因 */
```

- 返回 `0` = **已受理**，`out_request_id` 有效，之后**恰好一条**结果事件；
- 返回负数 = **未受理**，不会推结果事件（参数错 / 状态错 / 队列满 / 没这个东西）；
- 结果事件：`{requestId, op, ok, data?, error?}`。没有数据的操作 `data` 为 `null`——
  **"没有结果"与"不能等待"是两件事**：发布、订阅、写日志、关机同样回一条确认，
  调用方想保证顺序（先发布再读、先订阅再取值）时直接 `await` 即可。
- 三种形状之外不新增第四种：**不设"通知式入口"**，也不设"同步入口"。

**允许保留的同步函数只有两类**（它们不涉及插件代码，也没有可等待的工作）：

| 类别 | 函数 | 原因 |
|---|---|---|
| 句柄 | `host_create` / `host_destroy` | 要返回 / 释放原生句柄，无法经事件交回 |
| 纯函数与事件通道 | `poll_events` / `set_wake_callback` / `api_version` / `version` / `result_name` / `feature_bits` / `malloc` / `free` / `strdup_n` / `string_free` | 读常量、读写内存、取事件；`poll_events` 本身是事件泵 |

`host_start` / `host_shutdown` 改成提交式（`host.ready` / `host.stopped` 作为结果事件的数据），
Dart `init()` / `dispose()` 因此也能 `await` 到"真的起来了 / 真的停了"。

### 3.2 四条不变量

1. **终局保证**：受理成功 ⇒ 一定有一条结果事件。插件卸载、框架停机、请求被取消都按失败回一条，
   调用方不会悬挂。
2. **结果不丢**：结果事件不参与队列裁剪；事件队列满时**拒绝受理**（返回
   `PLUGINXX_RESULT_ERR_FULL`），而不是丢结果。周期性 / 高频操作由调用方自己控制频率。
3. **插件代码只跑在插件执行线程**（JS 生成函数与 JS 处理器在 JS 线程）：框架 IO 线程不再调
   插件的任何回调。
4. **Dart 不等待原生**：包层 API 全部返回 `Future`；需要"当前值"的地方读 Dart 侧缓存，
   缓存由事件维护（不为读一个值去同步调 FFI）。不关心结果的调用可以 `.ignore()`，
   但**能等**是默认能力。

### 3.3 概念与命名

- **能力 = 要结果**（一个 `Future`，带合并后的裁决）；**广播 = 不要结果**（只有确认）。
  入口形状只有一个，区别在声明与返回值，不在协议。
- **查询也是提交**：`scan` / `inspect` / `list` / `ui_snapshot` / `ui_contents` / `ui_slots` /
  `stats` / `debug_info` / `broadcast_list` 都走同一条协议。
- **不再有 `_async` / `Async` 后缀**：默认全异步，名字里强调它反而是噪声。删除
  `plugin_load_sync`、`plugin_call_async`、`loadAsync`、`dispatchAsync` 这类命名，
  统一成 `plugin_load`、`plugin_call`、`load`、`dispatch`。宿主内部返回 `asio::awaitable`
  的函数也不加后缀（靠返回类型区分）。
  **例外**：`cxx_pluginxx` 子模块里已有的 `sleepAsync` / `asyncOffload` 是共用库的公开名字
  （多项目在用），这次不改名，只在本包对外入口与宿主内部执行重命名。

## 4. 详细设计

### 4.1 C ABI 7：所有操作提交化

**版本**：`MUSICXX_EXTERN_PLUGIN_API_VERSION` 6 → 7。

**结果事件**（唯一完成通道）：

```json
{"seq": 812, "type": "musicxx.request.result", "requestId": 42,
 "op": "plugin.load", "ok": true, "data": {...}}
{"seq": 813, "type": "musicxx.request.result", "requestId": 43,
 "op": "broadcast.publish", "ok": true, "data": null}
{"seq": 814, "type": "musicxx.request.result", "requestId": 44,
 "op": "capability.dispatch", "ok": false,
 "error": {"code": -4, "reason": "not_found", "message": "…"}}
```

`op` 取值与 C ABI 函数一一对应，便于日志与排障：

| `op` | 入口 |
|---|---|
| `host.start` / `host.shutdown` | 起停 |
| `plugin.scan` / `plugin.inspect` / `plugin.list` | 扫描、预检、已加载快照 |
| `plugin.load` / `plugin.enable` / `plugin.disable` / `plugin.unload` / `plugin.reload` | 生命周期 |
| `plugin.setArgs` / `plugin.configPath` | 参数与配置路径 |
| `plugin.call` | Dart → 插件能力调用 |
| `capability.dispatch` | 能力（含 `phase` 参数） |
| `capability.handlers` / `capability.stats` | 自省 |
| `broadcast.publish` / `broadcast.watch` / `broadcast.read` / `broadcast.list` | 广播 |
| `ui.snapshot` / `ui.contents` / `ui.slots` / `ui.request` | 界面 |
| `stats` / `debug.info` / `stats.config` / `host.config` / `host.language` / `log` | 统计、配置、日志 |

**入口清单（改后）**

| 入口 | 形状 | 说明 |
|---|---|---|
| `host_create` / `host_destroy` | 同步（句柄） | 只做构造 / 释放 |
| `host_start` | 提交式 | 结果 = `{ok:true}`，同时照发领域通知 `musicxx.host.ready` |
| `host_shutdown` | 提交式 | 结果在框架线程与插件线程都停好之后回；同时照发 `musicxx.host.stopped` |
| `set_language` / `set_config` | 提交式 | 失败能等、能报原因 |
| `plugin_scan` / `plugin_inspect` / `plugin_list` | 提交式 | 扫描与预检含文件 I/O，不该占 Dart 线程 |
| `plugin_load` | 提交式 | **删除 `plugin_load_sync`** |
| `plugin_enable` / `plugin_disable` / `plugin_unload` / `plugin_reload` | 提交式 | 领域事件仍照发，结果事件是权威完成信号 |
| `plugin_set_args` / `plugin_get_config_path` | 提交式 | |
| `plugin_call`（原 `plugin_call_async`） | 提交式 | 结果并入 `musicxx.request.result`，删掉 `musicxx.plugin.call.result` |
| `capability_dispatch` / `capability_dispatch_phase` | 提交式 | 删 `flags` 与 `timeout_ms`；广播型 id 由宿主按声明回 `{handled:true}`，不再单独设通知入口 |
| `capability_handlers` / `capability_handlers_phase` / `capability_stats` | 提交式 | |
| `capability_handler_count(_phase)` | **删除** | Dart 侧计数由 `musicxx.capability.changed` 事件维护；要精确回查用 `capability_handlers` |
| `capability_app_respond` | 提交式 | 处理器链搬离 IO 线程之后，可以放心排队 |
| `broadcast_publish` / `broadcast_watch` | 提交式 | 回一条确认；要顺序就 `await` |
| `broadcast_read` / `broadcast_list` | 提交式 | 冷读；热路径读 Dart 缓存 |
| `ui_snapshot` / `ui_contents` / `ui_slots` / `ui_request` | 提交式 | `ui.request` 的结果就是今天的 `musicxx.ui.content` 载荷 |
| `log` | 提交式 | 回一条确认；调用方通常 `.ignore()` |
| `debug_info` / `stats` / `stats_config` | 提交式 | |
| `poll_events` / `set_wake_callback` / `feature_bits` / `api_version` / `version` / `result_name` / 内存与字符串函数 | 不变 | 事件通道与纯函数 |

**合并掉的事件**：`musicxx.plugin.call.result`、`musicxx.capability.decision.result`、
`musicxx.ui.content` 三条不再单独发，内容并入 `musicxx.request.result`（`op` 区分来源）。
领域通知事件照旧（`musicxx.host.ready` / `stopped`、`musicxx.plugin.loaded` / `unloaded`、
`musicxx.capability.changed`、`musicxx.ui.contents` / `slots`、`musicxx.broadcast.changed`、
`musicxx.ui.changed` 等），它们不是"某次请求的结果"。

**订阅一个广播频道时补发当前值**：`broadcast_watch(id, 1)` 成功后宿主先推一条
`musicxx.broadcast.changed`（带当前值，没有值就不推），再回 `broadcast.watch` 的确认。
Dart 缓存因此不需要单独冷读。

**一张请求表管终局**：受理、完成、宿主停机都用同一张表（今天的 `pendingCalls_` 扩成
`pendingRequests_`）。`host_shutdown` 时表里剩下的每一条都补一条 `ok=false`、
`error.reason = host_stopped` 的结果，Dart 侧不会留悬挂 `Future`。

**队列满时拒绝受理**：结果事件不裁剪，事件队列达到上限就返回 `PLUGINXX_RESULT_ERR_FULL`。
高频调用（日志、进度发布）由调用方降频或合并，框架不替调用方丢结果。

**建立与修复**：`plugin.load` 的结果里带上该插件这次登记的处理器清单，Dart 用它
一次性填好缓存；插件框架重启或怀疑丢事件时，用 `capability.handlers` 逐条回查。

### 4.2 pluginxx：删同步契约

`src/third_party/cxx_pluginxx`（子模块，独立提交）：

- `include/pluginxx/api/tables.h`：`PluginxxCapabilitySpec` 删 `sync` 字段；
  核对 `timeout_ms` 是否还有读者（现在只有登记路径在写），没有就一起删。
- `PLUGINXX_IFACE_CAPABILITIES_VERSION` 8 → 9，`MIN_VERSION` 同步；
  `PLUGINXX_API_VERSION` / `PLUGINXX_MIN_API_VERSION` 9 → 10
  （由本包 `src/CMakeLists.txt` 的 `MUSICXX_EXTERN_PLUGIN_MIN_API_VERSION` 一处覆盖）。
- `include/pluginxx/host/host_core.h`：`registerCapability` 不再区分 `sync`；
  删掉 `onCapabilityRowAdded` 里"只接受同步处理器"的判据（改由宿主判断阶段合法性）。
- `src/host/capability_registry.cpp`：自省 JSON 不再输出 `sync`。
- 新增 `pluginxx::AsyncReply<T>`（与现有 `WaitSlot<T>` 并列）：协程侧 `co_await`、回调侧
  `set` 恰好一次；`set` 可能来自任意线程，内部把唤醒投递回等待方执行器。
- 子模块测试补：只用异步处理器登记、注销、派发。

### 4.3 SDK：协程处理器是唯一形态

`src/sdk/include/musicxx/plugin/api/plugin_kit.h`：

- `implement` / `implementBefore` / `implementAfter` / `implementWithTag` 只保留
  `asio::awaitable<std::string>(std::string_view)` 形态，删掉返回 `std::string` / `int32_t` 的同步重载。
- 简单处理器写 `co_return`；需要"立刻给结果"的给一个 `pluginxx::readyValue(std::string)`
  之类的构造函数，避免作者为了一个返回值写完整协程骨架。
- 生命周期入口不变（已经是"协程或立即完成的协程"）。
- 本包对插件导出的方法名不加 `Async` 后缀（现在也没有）；子模块的 `sleepAsync` 保持不变。
- 文档同步：`docs/plugin-native-api.md` §4、`docs/plugin-api.md` 示例。

### 4.4 宿主：派发搬到插件线程，改成协程链

**派发流程（改后）**

```text
Dart 提交 capability_dispatch
  → IO 线程：记账（requestId）、取本阶段处理器快照、排序（before 正序 / after 逆序）
  → 插件执行线程：逐个 co_await 处理器
        rowStart(..., notify)  →  co_await AsyncReply
        取消（卸载 / 停机）→ 当作"没有意见"，链继续
  → 插件执行线程：按原规则合并（before 累积 patch、after 替换 value、
        impl 按 merge = first / veto / all，cancel / skip / drop 提前结束）
  → IO 线程：写统计、推 musicxx.request.result
```

广播型 id：声明是 `notify` 的，链跑完只推一条 `{handled:true, called:N}`，不做合并。

`musicxx_host_tables.cpp`：

- `runCapabilityHandlers` 改成 `asio::awaitable<std::string>`（不加 `Async` 后缀），
  合并规则与今天的代码逐条对应（快照、排序、`cancel` / `skip` / `drop` / `stop` 的语义都不改）。
- 删 `invokeSync` / `CapabilitySyncReply`；改成 `co_await invokeHandler(h, payload)`。
- 统计写回仍按 `(实例, ownerTag, 阶段)` 查活表，避免快照里的旧项写回。
- `dispatchCapabilityPhase` 改成"受理 + 完成回调"：受理阶段只做记账与参数检查（任意线程，不碰
  `handlers_`），然后把"取快照"投到 IO 线程；处理器链在插件执行器
  （`pluginxx::pluginExecutionExecutor(runtime())`）上跑，完成时回 IO 线程推结果。
  不再有 `DispatchMode`：要不要合并由声明的 `merge` 决定，要不要结果由调用方决定。

`musicxx_host.h`：

- `MusicxxPluginCapabilitySpec` 删 `run_sync` 与 `MusicxxCapabilityRunSyncFn`；
  `registerCapabilityHandler` 只接受 `run_start`。
- `CapabilityHandler.rowStart` 成为唯一形态（`spec.run_start` 那套旧字段一并删）。
- 内部返回 awaitable 的函数去掉 `Async` 后缀（`loadNativeDirAsync` → `loadNativeDir`、
  `invokeKindProviderAsync` → `invokeKindProvider`、`loadHostedKindAsync` / `startHostedKindAsync` /
  `stopHostedKindAsync` / `unloadHostedKindAsync` 同理）。

`musicxx_host_ui.cpp`：

- 原生插件的 `generate(...)` 调用投递到插件执行线程（今天是 IO 线程就地调）；
  完成仍回 IO 线程写注册表并推结果。JS 路径不变（本来就在 JS 线程）。

`requestAppCapability`（插件 → 应用）：

- 语义不变（应用声明里可以带 `timeoutMs`，取最小正数），这次只跟随表结构改动。

`musicxx_extern_plugin_api.cpp`：

- 删掉 `onHostThread`（`ioCallSync`）的全部使用；每个提交式入口只做"复制入参 + 分配
  requestId + 投递"，不再等待。
- `host_start` / `host_shutdown` 也走同一张请求表。

### 4.5 JS 引擎：处理器统一异步完成

`src/host/js/js_engine.cpp`：

- 删 `JsEngine::implementationRunSync`；改为 `implementationStart(...)`：投递到 JS 线程调
  `invokeImplementation`，脚本返回 Promise 就等它结算，结算后 `notify.done`；
  同步返回的值立刻完成。
- 删 `beginDispatchWait` / `finishDispatchWait` / `implementationWaitResolve` 这一对等待槽
  （或收编成 `AsyncReply`），JS 线程不再为"宿主同步派发"服务。
- 应用声明的 id 不再要求同步处理器，脚本里可以 `await` 后再回答。
- JS 接口代次保持 3：JS 侧能调的形状没变（处理器本来就可以返回 Promise），变的只是宿主
  不再为同步派发阻塞 JS 线程。

### 4.6 Dart 包：请求总线 + 本地缓存

**新增** `lib/src/requests.dart`：

```dart
/// 一次提交的结果；ok=false 时 error 带原因，不抛异常
class MusicxxPluginRequestResult {
  final int requestId;
  final String op;
  final bool ok;
  final Object? data;
  final MusicxxPluginRequestError? error;
}

class MusicxxPluginRequests {
  /// 提交一次请求；call 只负责"受理"，返回 requestId
  Future<MusicxxPluginRequestResult> submit(
    String op,
    int Function(Pointer<Int64> outId) call,
  );
  void handleResult(MusicxxPluginEvent event); // requestId → completer
  void handleDisposed();                        // 全部按 host_stopped 收尾
}
```

`submit` 返回的 `Future` **不会因为操作失败而抛异常**（失败在 `result.error` 里）；
包层的类型化 API（`load` / `scan` / …）在 `ok == false` 时抛 `MusicxxPluginApiException`，
方便业务用 `try/catch`。不关心结果的调用用 `.ignore()` 或包层的 `void` 便利方法，
不会产生"未处理的异步异常"。

**API 对照（旧 → 新）**

| 旧 | 新 |
|---|---|
| `plugins.load(id)`（同步）/ `loadAsync(id)` | `Future<void> load(id)` |
| `plugins.scan()` / `inspect()` / `list()` | `Future<...>` 同名 |
| `plugins.enable/disable/unload/reload/setArgs/configPath` | `Future<...>` 同名 |
| `plugins.requestUiContent(...)`（返回 int） | `Future<MusicxxPluginUIContentResult>` |
| `plugins.uiSnapshot/uiContents/uiSlots` | `Future<...>` 同名 |
| `plugins.watchBroadcast(id)` / `stats` / `statsConfig` | `Future<void>` / `Future<...>` |
| `capabilities.dispatch(id, payload)`（同步） | 删除；`dispatchAsync` 改名 `dispatch`，返回 `Future` |
| `capabilities.dispatchAtPhase`（同步） | 删除；`dispatch` 带 `phase` 参数 |
| `capabilities.on/off`（Dart 实现链） | 删除（生产代码零调用点） |
| `capabilities.refreshNativeHandlers` | `Future<...>` |
| `broadcasts.read(id)` / `list()` | `valueOf(id)`（读缓存）/ `Future<...> list()`；冷读用 `read` |
| `broadcasts.publish(id, value)` | `Future<void> publish(...)`（可 `.ignore()`） |
| `runtime.init(...)` | `Future<void>`（等 `host.start` 的结果） |
| `runtime.dispose()` | `Future<void>`（等 `host.shutdown` 的结果） |
| `runtime.debugInfo()` / `log(...)` | `Future<...>` / `Future<void>` |

**缓存规则**

| 数据 | 来源 | 读法 |
|---|---|---|
| 广播当前值 | `broadcast_watch` 补发 + `musicxx.broadcast.changed` | `broadcasts.valueOf(id)`（不过 FFI） |
| 处理器计数 / 列表 | `musicxx.capability.changed` + `plugin.load` 结果里的清单 | `capabilities.nativeHandlerCount` / `handlersOf` |
| 已加载插件 | 生命周期事件 + 一次 `plugin.list` 提交 | `plugins.loaded` |
| 界面内容注册表 | `musicxx.ui.contents` / `musicxx.ui.slots` 事件 + 一次快照提交 | `MusicxxUiContentTransport` 自己那份副本 |

### 4.7 主程序接入

- `ExternPluginStore`：
  - `initDo` 只等 `runtime.init()`（`host.start` 的结果），随后 `unawaited(_scanAndLoad())`；
    `app_ready` 不再被插件加载拖住（现在是同步扫描 + 逐个阻塞加载）。
  - `_scanAndLoad` / `_loadPlugin` / `setPluginEnabled` / `reloadPlugin` / `_reloadPluginInstance` /
    `unloadPlugin` / `uninstall` / `rescan` 改成 `Future`，管理页需要 loading 与"正在加载"状态。
  - `refreshUIItems` / `statsOf` / `debugInfo` 改成 `Future`，调用点 await 或放后台刷新。
  - 生命周期事件到达时的 `list()` 刷新做合并（一次在途请求 + 结束后补跑一次），避免事件风暴。
- `MusicxxUiContentTransport`：`syncAll` / `requestContent` 改 `Future`；`requestContent` 以结果
  事件为准交给 `ContentHub`。
- `ExternBroadcastHost` / `MusicxxUiVariableSources` / `render/PluginRenderArgs`：
  `broadcasts.read` 全部换成本地缓存 `valueOf`；按需 `watch`（页面卸载时退订）；
  `publish` 需要顺序的地方 `await`，其余 `.ignore()`。
- `ExternCapabilityCall`：删 `dispatch`（同步），`dispatch`（原 `dispatchAsync`）保留为唯一入口；
  `refreshHandlers` 改 `Future`，`ExternAppCapabilities` 里的调用点 `await`。

## 5. 分步实施

| 阶段 | 内容 | 主要文件 | 验证 |
|---|---|---|---|
| 0 | 基线：加"慢处理器拖住框架"的对照用例，记录现有测试数量 | `src/tests/**` | 现有原生测试全绿 |
| 1 | pluginxx 删 `sync`、能力表 v9、插件 ABI 10、`AsyncReply` | 子模块 `include/**`、`src/**` | 子模块测试 |
| 2 | 宿主派发改协程链 + 插件线程；删 `run_sync`；原生内容生成搬插件线程；内部函数去 `Async` 后缀 | `src/host/musicxx_host*.cpp/.h` | 原生测试 |
| 3 | JS 处理器统一异步完成 | `src/host/js/js_engine.cpp` | 原生测试（JS 用例） |
| 4 | C ABI 7：全部操作提交化（含发布 / 订阅 / 日志 / 关机），删同步入口与 `plugin_load_sync` | `src/include/**`、`src/host/musicxx_extern_plugin_api.cpp` | 原生测试 + ffigen |
| 5 | Dart 包：请求总线、全 Future、缓存、删同步 API 与 `_async` 命名 | `lib/src/**` | 包内 `flutter test` |
| 6 | 主程序接入 | `lib/plugin/externPlugin/**` | `flutter analyze` + 模块测试 |
| 7 | 示例插件、原生测试、Dart 测试改造 | `plugins/**`、`src/tests/**`、`test/**` | 全部测试 |
| 8 | 文档与版本号 | 包内 `docs/**`、主程序 `docs/architecture/08,09`、`AGENTS.md` §7.6 | `gen_contract --check`、`version_test` |
| 9 | 全量验证与基准 | — | 见第 6 节 |

每阶段独立构建并提交（子模块先提交，再更新本包指针）。

## 6. 验证清单

```powershell
pwsh -NoProfile -File tools/build_native.ps1 -WithExamples -WithTests
cd .native\build\windows-release\musicxx-extern-plugin-install
$env:PATH="$PWD\bin;$env:PATH"; .\bin\musicxx_extern_plugin_test.exe .\plugins
.\bin\musicxx_pluginxx_test.exe
fvm dart run tools/gen_contract.dart --check
fvm flutter test test/version_test.dart
cd <主程序>; flutter test test/main_test.dart --dart-define=TEST_MODULES=extern_plugin
flutter analyze --no-pub --no-fatal-infos lib test
```

手工确认：

- 启动：两个动态库插件 + 一个 JS 插件同时启用，启动过程不卡（对比基准 `app_ready` 时间）；
- 慢插件：一个处理器 `co_await sleep(2s)`，期间管理页仍可打开、其它插件的能力照常回答；
- 顺序：`await publish(...)` 之后立刻 `read` 能读到刚发布的值；`await watch(id)` 之后能收到
  补发的当前值；
- 播放页：插件背景 / 字体 / 控制栏替换都正常，切歌与换色刷新正常（每帧取广播值的路径）；
- 管理页：安装 → 启用 → 停用 → 重新加载 → 卸载，每步都有明确状态与失败原因；
- 退出：`dispose` 等 `host.shutdown` 结果，没有悬挂请求，Windows 关闭不产生
  `native_crash_*.log`；
- 插件侧：插件在处理器里调 `musicxx.ui.slot.update` / 能力调用不再互相等。

平台：Windows 实跑；Android / iOS / macOS / Linux 只做构建，结果写进 `work.md`。

## 7. 风险与对策

| 风险 | 对策 |
|---|---|
| 派发从同步变异步，调用点漏了 `await` | 返回类型变化会让旧的同步调用点编译失败；剩下"把 `Future` 丢掉"的地方逐个核对，必要时开 `discarded_futures` 检查 |
| 快速连点（音量 / 进度 / 切歌）产生乱序结果 | 结果里带 requestId，调用点只认最后一次；需要"后发先至"保护的地方加序号 |
| 高频操作（日志、进度发布）每条约一条结果事件 | 结果事件不裁剪、队列满就拒绝受理；调用方降频或合并，框架不替调用方丢结果 |
| 未 await 的失败 Future 变成未处理异常 | `submit` 只回结果对象不抛异常；类型化 API 才抛；不关心结果的调用用 `.ignore()` 或包层 `void` 便利方法 |
| 插件线程单条，慢处理器拖慢同线程的其它插件 | 与今天一致；文档写清"插件代码都在插件线程"，异步等待期间线程仍可推进别的协程 |
| 请求泄漏（受理后宿主销毁） | 请求表随 `host.shutdown` 全部按 `host_stopped` 收尾；`dispose` 等 shutdown 结果 |
| 每帧读广播改成缓存后取不到值 | `broadcast_watch` 补发当前值；挂载点进入/退出时成对 watch/unwatch；测试覆盖 |
| `app_ready` 时序变化 | 插件扫描/加载放后台；启动收尾只等 `host.start` 的结果 |
| 测试改造量大（原生 518 + 283 checks、Dart 模块） | 阶段 7 单独做，先给测试加 `callAndWait` 帮助函数，再逐条改断言 |

## 8. 已确认的边界

以下六条已由用户确认（2026-11-25），实施时不再讨论：

1. **纯注册表读也走提交**：`plugin_list` / `ui_*` / `stats` / `debug_info` / `capability_handlers`
   全部返回 `Future`，热路径由 Dart 缓存兜住。
2. **无需结果的操作同样支持异步等待**：发布、订阅、日志、回复、关机都回一条确认事件，
   要顺序或要确认前置完成时直接 `await`；不关心就 `.ignore()`。
3. **删除全部同步函数，名称不再带 `_async` / `Async`**：`plugin_load_sync`、`plugin_call_async`、
   `loadAsync`、`dispatchAsync` 这类命名统一去掉；只保留句柄类（`host_create` / `host_destroy`）、
   事件通道与纯函数同步可见（见 §3.1）。
4. **版本号**：C ABI 6 → 7、插件 ABI 9 → 10、能力表 v8 → v9、框架版本 `2026.1125.1`。
5. **删除 Dart 侧 `capabilities.on/off`**（Dart 实现链，生产代码零调用点）。
6. **启动时序**：`initDo` 只等 `host.start` 的结果，插件扫描与加载放后台，`app_ready` 不因插件变慢。

实施中需要留意的两个取舍（不改方向，只影响写法）：

- `host_create` / `host_destroy` 必须同步返回（句柄无法经事件交回）；`host_start` / `host_shutdown`
  已改成提交式。
- `cxx_pluginxx` 的 `sleepAsync` / `asyncOffload` 是共用库的公开名字，这次不改名。
