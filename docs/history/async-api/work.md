# 实施记录

## 进度

- [x] 现状调查（基线构建失败点、新库 API 形状）
- [x] 方案文档
- [x] 阶段 1：业务表 ABI 异步化（`plugin_api.h` + 版本号）
- [x] 阶段 2：宿主表实现（`musicxx_host_tables.cpp` / `musicxx_host_ui.cpp` / `musicxx_host.cpp` / `musicxx_host.h`）
- [x] 阶段 2.5：内置 JS 运行器入口与广播订阅跟上新签名（`js/js_engine.*`）
- [ ] 阶段 3：宿主派发不占 IO 线程（`runCapabilityHandlers`）
- [ ] 阶段 4：原生 SDK kit 协程化（`plugin_kit.h`）
- [ ] 阶段 5：JS 引擎与 JS SDK 异步化
- [ ] 阶段 6：示例与原生测试
- [ ] 阶段 7：文档、版本号、CHANGELOG、生成物

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
  失败原因改成正文文本（`arg` / `state` / `namespace` / `not_found` / `no_value` / `ended`），不再用负数编码。
- `MusicxxPluginUIContentSpec.generate` 改成
  `void(host, ctx_json, user_data, notify, error_out)`：内容生成可以稍后完成（协程 / 异步请求）。
- 入口符号去掉 `musicxx_plugin_start`（与 `pluginxx/api/entry.h` 的四件对齐：get_info / create / stop / destroy）；
  `MusicxxHostManager::entrySymbols()` 同步。

### 阶段 2：宿主表实现

- `musicxx_host_tables.cpp`：`musicxx.host` / `musicxx.kind` 与广播表（自定义入口）全部改用
  `pluginxx::startTableOp()`；参数在这一层复制成自有字符串后再排队，不依赖插件侧借用视图的存活时间。
- 广播表：`subscribe_broadcast(_ex)` 的完成载荷是订阅句柄的十进制文本；`read_broadcast` 的失败原因
  区分 `not_declared` / `no_value` / `ended`。
- `musicxx_host_ui.cpp`：`musicxx.ui` 表 12 个成员全部改写成 `startTableOp` 形式；原生内容生成改成
  notify 形式（`NativeGenerateWaiter` + `nativeGenerateDoneTrampoline`），插件稍后完成也能写回缓存。
- `musicxx_host.cpp`：`musicxx.net` 表跟上 v2（`call` 改成异步操作契约，`request_id_out` 变成出参，
  `call_async` 删除）；完成由 `NetHub` 的网络线程触发，经 `OpCore::onDoneCore` 交回。
- `js_engine.*`：内置 JS 运行器的入口改成 `get_info(PluginxxInfo*)` /
  `create(host, void**, notify, error)`（构造 + 注册合一，可重入）/ `stop(ctx, notify, error)` /
  `destroy(ctx, notify, error)`；运行期与回放阶段的广播订阅改走 `subscribeBroadcastOnHostThread()`。

### 验证

```
cmake --build .native/build/windows-release/e/host --target musicxx_extern_plugin --config Release
```

通过（0 error）。**没有**跑完整 `tools/build_native.ps1`：SDK 头 `plugin_kit.h` 与示例 / 原生测试还没迁移，
它们会失败（见下）。

## 剩余工作（按顺序）

1. **`plugin_kit.h` 协程化**（阶段 4，核心待办）
   - 新增 `tableCall` / `tableText` / `tableVoid` 三个协程帮助函数（`pluginxx::asyncTableOp` + 取消槽）；
   - `hostInfoJson` / `configPath` / `pluginId` / UI 12 个接口 / kind 3 个接口 / 广播 5 个接口 /
     功能埋点 6 个接口全部改成 `asio::awaitable<…>`；
   - 能力登记改成协程形态，处理器允许写成 `asio::awaitable<std::string>`（用
     `pluginxx::detail::startProviderRoot` 起协程，取消经 `CancelableProvider::requestCancel`）；
   - `MUSICXX_PLUGIN_EXPORT` 的 start/stop 允许 `asio::awaitable<void>`，同步写法保留；
   - 删掉 `ActionNotifyHolder` / `capabilityCall(notify)` 回调形态（协程形态已够用）。
2. **宿主派发不占 IO 线程**（阶段 3）
   - `runCapabilityHandlers` 改成"IO 线程取快照 → 工作线程逐个调用并等待 → 回 IO 线程写统计"，
     否则插件协程处理器在等表调用时，IO 线程正被派发占住 → 互锁。
3. **JS SDK 异步化**（阶段 5）
   - `musicxx.broadcast.read` / `musicxx.ui.*` / `musicxx.capability.*` / `musicxx.kind.*` 改成返回 Promise；
   - 引擎按调用 id 登记未完成请求，结果回 JS 线程 resolve；
   - 文档同步 `docs/plugin-js-api.md`。
4. **示例与原生测试**（阶段 6）：`plugins/example_native*`、`plugins/example_js*`、
   `src/tests/test_host.cpp`、`src/tests/test_pluginxx.cpp` 按新写法改；然后跑
   `tools/build_examples.ps1 -RunTests` 与 `flutter test`。
5. **文档与版本**（阶段 7）：`docs/plugin-native-api.md`、`docs/plugin-api.md`、`CHANGELOG.md`、
   三处版本号（`lib/src/version.dart`、`musicxx_extern_plugin_api.h`、`pubspec.yaml`）、
   `MUSICXX_EXTERN_PLUGIN_MIN_API_VERSION` 8 → 9、`dart run tools/gen_contract.dart`。

## 注意到的问题

- `PluginxxBroadcastsIface` 的订阅是异步的，但 JS 引擎在框架线程上建立订阅时依赖"内联完成"才拿得到句柄；
  已按"调用方必须在框架 IO 线程"写成 `subscribeBroadcastOnHostThread()`，并在注释里写明这一前提。
  以后若把订阅移到别的线程，必须改成回调形式。
- 原生内容生成的 `generate` 没有取消入口：主程序丢弃迟到的结果（`findUiContent` 找不到就不写），
  插件侧不需要处理取消。
