# 接口异步化方案

## 1. 背景

`cxx_pluginxx` 做了一次彻底重构：

- 所有接口表成员都是**异步操作契约**：成员返回 `PluginxxOperatorHandle*` 表示"已受理"，
  结果经 `PluginxxOperatorCallback`（或 `PluginxxOperatorNotify`）恰好一次交回；返回 `NULL`
  表示未受理（同时写 `error_out`）。
- 插件协程统一到 `asio::awaitable`：`pluginxx::kit` 提供 `sleep` / `offload` / `invoke_cap` /
  表调用包装；生命周期入口改成 `void + 完成通知`；`create` 事务会 `co_await` 插件的
  `onHostReady()`。
- 宿主侧有 `pluginxx::startTableOp()`：把"受理 + 在自己的 IO 线程执行工作体 + 交回结果"收敛到一处。

本包的业务表（`musicxx.host` / `musicxx.ui` / `musicxx.kind`）还是同步形态：成员直接返回
`int32_t` 或写 `PluginxxString*`。这与新库无法共存（基线构建在函数指针类型上直接报错）。

## 2. 现状调查（文件与函数一级）

| 位置 | 现状 | 问题 |
|---|---|---|
| `src/sdk/include/musicxx/plugin/api/plugin_api.h` | `MusicxxPluginHostIface` / `MusicxxPluginUIIface` / `MusicxxPluginKindIface` 全是同步成员；内容生成回调 `generate` 同步返回 | 与异步库不兼容；插件处理器里不能等主程序 |
| `src/host/musicxx_host_tables.cpp` | `xx_host_get_info` / `xx_kind_*` 同步；`xx_declare_broadcast` / `xx_publish_broadcast` / `xx_read_broadcast` / `xx_subscribe_broadcast*` / `xx_end_broadcast` 仍是同步签名 | 广播表成员签名已被库改成异步 |
| `src/host/musicxx_host_tables.cpp` `runCapabilityHandlers` | 在框架 IO 线程就地调插件处理器；异步处理器只接受"start 返回前就 done" | 插件协程要推进必须回到 IO 线程 → 与"IO 线程正等结果"互锁 |
| `src/host/musicxx_host.h` `MusicxxHostInstance` | 覆写 `pluginDestroySymbol()` | 基类已删掉该虚函数（改成加载期填 `lifecycleDestroy`） |
| `src/host/js/js_engine.cpp` | 内置插件槽位表用旧的 `get_info/create/start/stop/destroy` 签名；广播订阅调用缺新的回调参数 | 编译失败 |
| `src/sdk/include/musicxx/plugin/api/plugin_kit.h` | 多数接口同步（`uiItemRegister` / `uiNotify` / `kindRegister` / `implement` / `capabilityCall` 回调形态…） | 插件写不了"先读配置再注册"这类顺序逻辑，只能嵌套回调 |
| `js/`（引擎内建的 `musicxx` 全局对象） | 读广播 / 注册 UI / 调能力大多是同步返回 | 会等框架线程，脚本一多就互相卡 |

## 3. 目标形态

### 3.1 业务表（C ABI）

三张表统一成：

```c
PluginxxOperatorHandle*(PLUGINXX_CALL* <成员>)(
    const PluginxxHost*      host,
    ...参数...,
    PluginxxOperatorCallback cb,
    void*                    cb_ud,
    PluginxxString*          error_out
);
```

- 成功：`status = PLUGINXX_OPERATOR_OK`，`payload` 是结果（JSON 文本；无结果时为空）。
- 业务失败：`status = PLUGINXX_OPERATOR_FAILED`，`payload` 是失败原因（如 `not_declared`、
  `not_owner`）。**不再用 `-1/-2/-4/-6` 这类负数码**：插件侧要区分原因时读文本。
- 受理失败：返回 `NULL` + `error_out`，完成回调不会被调用。

版本号：`musicxx.host` v3、`musicxx.ui` v4、`musicxx.kind` v2；插件 ABI 代次 8 → 9
（`MUSICXX_EXTERN_PLUGIN_MIN_API_VERSION` / `PLUGINXX_API_VERSION` 同步）。

内容生成（框架 → 插件）也改成操作契约：

```c
void(PLUGINXX_CALL* generate)(
    const PluginxxHost*           host,
    const PluginxxStringView*     ctx_json,
    void*                         user_data,
    const PluginxxOperatorNotify* notify,   /* 恰好一次；payload = {"json":…,"data":…} */
    PluginxxString*               error_out
);
```

### 3.2 动态库插件 SDK（asio 协程）

- 所有会走表的接口返回 `asio::awaitable<T>`（例如 `co_await ctx.uiContentRegister(...)`）。
- 统一结果类型 `pluginxx::TableOpResult`；SDK 再包一层小工具：
  `awaitJson(...)`（拿 payload 文本）、`awaitOk(...)`（只要成败）。
- 能力处理器支持协程：`implement(id, priority, [](std::string_view args) -> asio::awaitable<std::string>)`，
  SDK 用 `pluginxx::startProviderRoot` 起协程，完成时回调框架的 `notify`。
- 生命周期：`MUSICXX_PLUGIN_EXPORT` 的 start/stop 允许写成 `asio::awaitable<void>`（事务里可以
  `co_await` 注册）；同步写法仍然可用（SDK 内部包成一条立即完成的协程）。

### 3.3 JS 插件 SDK

- `musicxx.*` 里所有要过主程序的调用返回 Promise：`await musicxx.ui.slot.register({...})`、
  `await musicxx.broadcast.read(key)`、`await musicxx.capability.call(...)`。
- 引擎内每个 JS 实例维护"未完成调用表"：JS 线程发起 → 投递到框架线程 → 结果回 JS 线程后
  resolve/reject。JS 线程从不等待框架线程。
- 能力处理器返回 Promise 的路径保持（框架等它结算），但框架侧的等待改到工作线程。

### 3.4 宿主侧不阻塞 IO 线程

- 表成员：直接用 `pluginxx::startTableOp()`（调用方在 IO 线程时内联，其它线程排队）。
- 能力派发：`runCapabilityHandlers` 改成"在 IO 线程取快照 → 在工作线程逐个调、等待 →
  回 IO 线程写统计"。IO 线程在等待期间继续服务插件驱动 ticket，插件协程因此能推进。
- `musicxx.ui` 的内容生成：JS 已经走 `requestUiContent`（投递 + 回调）；原生插件改成
  `generate(..., notify, ...)`，宿主拿 notify 完成后推事件，Dart 侧入口不变。

## 4. 分步实施

1. **ABI**：改 `plugin_api.h`（三张表 + 生成回调），版本号与 `IfaceMeta` 同步。
2. **宿主表实现**：`musicxx_host_tables.cpp` / `musicxx_host_ui.cpp` / `musicxx_host.h`
   按新契约重写；补 `pluginDestroySymbol` 删除、内置插件入口新签名。
3. **宿主派发**：`runCapabilityHandlers` 改工作线程执行 + 等待槽；`dispatchCapability*` 保持签名。
4. **原生 SDK kit**：`plugin_kit.h` 全面协程化（含能力处理器协程、生命周期协程）。
5. **JS 引擎与 JS SDK**：表调用改 Promise；能力处理器/内容生成的等待不再占框架 IO 线程。
6. **示例与测试**：`plugins/example_native*`、`plugins/example_js*`、`src/tests/test_host.cpp`、
   `src/tests/test_pluginxx.cpp`。
7. **文档与版本**：`docs/plugin-native-api.md`、`docs/plugin-js-api.md`、`docs/plugin-api.md`、
   `docs/plugin-capabilities.md`（生成物重跑）、`CHANGELOG.md`、版本号三处同步。

每完成一步就构建一次（`tools/build_native.ps1`），可独立通过的阶段单独提交。

## 5. 验证

```powershell
pwsh -NoProfile -File tools/build_native.ps1                 # 框架库 + SDK
pwsh -NoProfile -File tools/build_examples.ps1 -RunTests      # 示例插件 + 原生测试
flutter test                                                 # Dart 侧整套流程
dart run tools/gen_contract.dart --check                     # 生成物一致
```

另外手工确认：

- 插件协程里 `co_await sleep(...)` / `co_await uiContentRegister(...)` 期间，主程序 UI 不卡；
- 能力处理器写成协程（中间 `co_await` 一次 `sleep`）仍能拿到结果；
- JS 脚本 `await musicxx.broadcast.read(...)` 正常返回，且不再有同步跨线程等待。

## 6. 风险与边界

- **不保留旧写法**：插件 ABI 8 → 9，旧插件必须重编（主程序最低版本已抬高、尚未发布）。
- **Dart 侧不改**：`capability_dispatch` 仍同步返回；为不让 IO 线程被占住，等待放在工作线程。
- **等待上界**：能力派发沿用现有等待语义（不设超时），但调用方（Dart）可以不再等。
- 网络表（`musicxx.net`）是插件框架通用表，本次只跟随库的签名变化，不改语义。
