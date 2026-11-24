# 实施记录

## 进度

- [x] 现状调查（基线构建失败点、新库 API 形状）
- [x] 方案文档
- [x] 阶段 1：业务表 ABI 异步化（`plugin_api.h` + 版本号）
- [x] 阶段 2：宿主表实现（`musicxx_host_tables.cpp` / `musicxx_host_ui.cpp` / `musicxx_host.cpp` / `musicxx_host.h`）
- [x] 阶段 2.5：内置 JS 运行器入口与广播订阅跟上新签名（`js/js_engine.*`）
- [x] 阶段 3：宿主派发不占 IO 线程（`runCapabilityHandlers` / `HandlerHolder`）
- [x] 阶段 4：原生 SDK kit 协程化（`plugin_kit.h`）
- [x] 阶段 5：JS 引擎与 JS SDK 异步化
- [x] 阶段 6：示例与原生测试
- [x] 阶段 7：文档、版本号、CHANGELOG、生成物

最终验证：

```
.native\build\windows-release\musicxx-extern-plugin-install\bin\musicxx_extern_plugin_test.exe .\plugins
  → 518 checks / 0 failed
.native\build\windows-release\musicxx-extern-plugin-install\bin\musicxx_pluginxx_test.exe
  → 283 checks / 0 failed
flutter test test/version_test.dart          → 通过
dart run tools/gen_contract.dart --check     → 通过（本阶段没有改能力 / 广播声明）
```

## 基线构建结论（改动前）

`tools/build_native.ps1` 在依赖重建后失败，错误集中在：

- `musicxx_host_tables.cpp(1909..1911)`：广播表成员函数指针与新版 `PluginxxBroadcastsIface` 不匹配；
- `musicxx_host.h(144)`：`pluginDestroySymbol()` 不再覆写任何基类虚函数；
- `js_engine.cpp(2673..2677)`：内置插件槽位表用了旧的 `get_info/create/start/stop/destroy` 形状，
  `PluginxxBuiltinInfo` 已没有 `start` 字段；
- `js_engine.cpp(3545..3552 / 4454..4463)`：`subscribe_broadcast(_ex)` 少了新的完成回调参数。

## 已完成的部分

### 阶段 1：业务表 ABI（提交 `47f6066`）

- `plugin_api.h`：`musicxx.host` v3 / `musicxx.ui` v4 / `musicxx.kind` v2 的成员全部改成
  `PluginxxOperatorHandle*(host, …, PluginxxOperatorCallback cb, void* cb_ud, PluginxxString* error_out)`；
  失败原因改成正文文本（`invalid_argument` / `invalid_state` / `not_found` / `not_declared` / `no_value` /
  `ended`），不再用负数编码。
- `MusicxxPluginUIContentSpec.generate` 改成
  `void(host, ctx_json, user_data, notify, error_out)`：内容生成可以稍后完成（协程 / 异步请求）。
- 入口符号去掉 `musicxx_plugin_start`（与 `pluginxx/api/entry.h` 的四件对齐：get_info / create / stop / destroy）；
  `MusicxxHostManager::entrySymbols()` 同步。

### 阶段 2：宿主表实现（提交 `eb33ed7` 的一部分）

- `musicxx_host_tables.cpp`：`musicxx.host` / `musicxx.kind` 与广播表（自定义入口）全部改用
  `pluginxx::startTableOp()`；参数在这一层复制成自有字符串后再排队，不依赖插件侧借用视图的存活时间。
- 广播表：`subscribe_broadcast(_ex)` 的完成载荷是订阅句柄的十进制文本；`read_broadcast` 的失败原因
  区分 `not_declared` / `no_value` / `ended`。失败原因统一由 `host_core.h` 的 `fail` 加上
  `pluginxx::result::nameOf(code) + ": "` 前缀。
- `musicxx_host_ui.cpp`：`musicxx.ui` 表 12 个成员全部改写成 `startTableOp` 形式；原生内容生成改成
  notify 形式（`NativeGenerateWaiter` + `nativeGenerateDoneTrampoline`），插件稍后完成也能写回缓存。
- `musicxx_host.cpp`：`musicxx.net` 表跟上 v2（`call` 改成异步操作契约，`request_id_out` 变成出参，
  `call_async` 删除）；完成由 `NetHub` 的网络线程触发，经 `OpCore::onDoneCore` 交回。
- `js_engine.*`：内置 JS 运行器的入口改成 `get_info(PluginxxInfo*)` /
  `create(host, void**, notify, error)`（构造 + 注册合一，可重入）/ `stop(ctx, notify, error)` /
  `destroy(ctx, notify, error)`；运行期与回放阶段的广播订阅改走 `subscribeBroadcastOnHostThread()`。

### 阶段 3：宿主派发不占 IO 线程（提交 `df8639e`）

- 能力处理器一律以 `HandlerHolder::start` 为入口：同步处理器在调用线程就地给结论，
  协程处理器改用 `HandlerHolder::startJob`（见下）。
- **插件代码改到独立执行线程**：`HostContext` 增加 `std::unique_ptr<asio::thread_pool> pluginThread`
  （单线程），`start()` 里 `pluginxx::bindPluginExecutor(runtime(), ...)`，`stop()` 里先解绑再收线程。
  在此之前插件代码跑在框架 IO 线程上：一个插件的协程或处理器慢一点，就会拖住框架收发消息。

### 阶段 4：原生 SDK kit 协程化（提交 `3c89499`）

- 新增 `tableCall` / `tableText` / `tableVoid` 三个协程帮助函数（内部走 `pluginxx::asyncTableOp`
  并把调用方协程的取消槽接上：协程被取消时正好在等的那次表调用会在主程序侧 `op_cancel`）。
- 全部业务接口改成 `asio::awaitable`：`hostInfoJson` / `configPath` / `pluginId` / `netCall` /
  `netSubscribe` / `capabilityCall`（+ `capabilityCallWithTimeout`）/ 广播 5 个 / `kind` 3 个 /
  界面 12 个 / 功能埋点 6 个。运行环境改用基类的 `configAsync` / `languageAsync` / `argsJsonAsync`。
- 能力登记改成协程形态（`co_await implement(...)`），处理器支持两种写法：
  `std::string(std::string_view)` 与 `asio::awaitable<std::string>(std::string_view)`。
  登记时按返回类型写 `spec.sync`（1 = 只给同步处理器；前链 / 后链只收同步的）。
- 生命周期事务支持三种写法：同步、协程（`asio::awaitable<void>`）、插件框架原始形参；
  导出宏 `MUSICXX_PLUGIN_EXPORT` 改成 `create` + `stop` 两件事（不再有 `_start` 符号）。
- 订阅完成载荷（十进制句柄）在这里解析成 `PluginxxSubscription*`，订阅处理器与流结束回调
  都通过 `EventHolder` 保活（生命周期 = 实例）。

### 阶段 5：JS 引擎与 JS SDK 异步化（提交 `6344600`）

- `musicxx.capability.*` / `musicxx.broadcast.*` / `musicxx.ui.*` / `musicxx.kind.*` 的异步成员
  返回 Promise，结果经 `ext.*` 回到 JS 线程 resolve；同步只保留 `broadcast.read`（读缓存）
  与不产生 IO 的取值。
- **能力处理器不再同步等待 JS**：`JsEngine::capabilityStart` 改成"受理即返回句柄"，
  `capabilityWaits_` 改存 `CapabilityWait{instance, deliver}`；JS 结算后由
  `ext.capabilityWaitResolve` → `deliverCapabilityWait` 补上完成通知。之前的写法在
  插件执行线程上等 JS，会与 JS 线程上的能力调用互锁。
- JS 插件首次 create 只建上下文、没跑脚本（`startInstance` 缺失），补上；失败则 `destroyInstance`。
- 广播订阅回执拿不到句柄（把 `std::shared_ptr<SubscribeCapture>*` 当对象用）改成在回调里解引用并
  `delete holder`，等待用 `WaitSlot`（10 s）。

### 阶段 6：示例与原生测试（提交 `eb33ed7` / `3c89499` 的一部分）

- `plugins/example_native*` 全部改成协程写法：create / stop 事务、`co_await` 注册、`asyncProbe`
  用 `co_await pluginxx::sleepAsync(*this, waitMs)`；失败原因改成文本（`codeOfFailure` 映射
  `namespace→-6` / `not_declared→-200` / `ended→-202` / `no_value→-201` / `not_found|unknown→-4` /
  `state→-2` / 其余 `-1`）；补回 `foreignActionRc` 自检（测试期望改为 `ERR_NOT_DECLARED`）。
- `src/tests/test_host.cpp`：加 `TestStderrLogSink`（`MUSICXX_TEST_LOG=1` 时打印框架日志）、
  断言改成新原因文本；`test_pluginxx.cpp` 删掉旧的元信息检查。

## 踩过的坑（按发现顺序）

1. **协程处理器走 provider root 会崩**：root 的析构帧由 asio 管，而能力行只需要"算一个值交回"。
   改成 job（`owner->spawn`）+ `AsyncJobHandle`（继承 `CancelableProvider`），经 `keepAsyncJob` /
   `asyncJobs_` 保活，`return job.get()` 交回句柄；`spec.cancel` 两形态共用。
2. **异步处理器返回空句柄被判"协议违约"**：受理能力时必须交回可取消句柄，不能返回 NULL。
3. **内容注册的 `routes` / `deps` 空串会被主程序当 JSON 解析**：`uiContentRegister` 里空串归一成 `"[]"`。
4. **`plugin_disable` / `plugin_enable` 原来只调协程就返回**（等于没做）：改成 C API 直接调 manager；
   manager 内 `isIoThread()` 时快速失败；disable 的 stop 事务用 `WaitSlot(30000)` 等。
   不能包在 `onHostThread` 里 —— `ioCallSync` 在 IO 线程上会就地执行，等于同步等自己。
5. **禁用期间协程驱动被拒**（`Disabled` 拒绝 lease）：`stopForDisable` 期间临时 `setState(Closing)`，
   结束后回 `Disabled`。
6. **JS 侧同步等 JS**：见阶段 5 的 `capabilityStart` 改写。
7. **订阅回执**：广播表的完成载荷是十进制句柄文本，解析错就会拿到野指针。

## 注意到的问题

- `PluginxxBroadcastsIface` 的订阅是异步的，但 JS 引擎在框架线程上建立订阅时依赖"内联完成"才拿得到句柄；
  已按"调用方必须在框架 IO 线程"写成 `subscribeBroadcastOnHostThread()`，并在注释里写明这一前提。
  以后若把订阅移到别的线程，必须改成回调形式。
- 原生内容生成的 `generate` 没有取消入口：主程序丢弃迟到的结果（`findUiContent` 找不到就不写），
  插件侧不需要处理取消。
- 应用声明过的能力仍然**只收同步处理器**（`musicxx_host_tables.cpp` 里 `spec.sync == 0` 直接拒绝）：
  这些点由应用就地派发并等结果。插件自己的能力可以给协程处理器。
- 计数变化：`plugin_disable` 现在真的会跑 `stop` 事务，原来自管线程 / 文件不回收的问题一并修掉了。

## 未验证

- Android / iOS / macOS / Linux 的原生构建与运行（只在本机 Windows Release 上跑过原生测试）；
- JS 侧只在 Windows 上跑过 `musicxx_extern_plugin_test` 里内置的 JS 用例与示例插件；
- 主程序（Flutter）侧的联调没有逐个能力过一遍：只保证 C ABI 形状未变、原生测试全部通过。
