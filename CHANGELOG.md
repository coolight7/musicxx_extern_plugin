# 变更记录（框架 / SDK / 插件框架库）

这个文件只记**会影响插件作者与接入方**的改动：协议、ABI、行为约定、破坏性变更。
细节说明在各篇作者文档里（`docs/`），这里给"从哪一版开始不一样了"。

版本按插件框架版本号（格式 `<年>.<月日>.<小版本号>`，见 `lib/src/version.dart`、
`src/include/musicxx_extern_plugin_api.h` 与 `docs/version-history.md`）与改动日期记。

## 未发布

### 约定：接口表的可用性判定改为「最低版本 + 基线字节数」

- 依赖库 `cxx_pluginxx` / `cxx_utilxx_base` / `cxx_pluginxx_ui` 已更新。其中表判定不再写死
  `version == 1`：**表可用 = `version >= X_MIN_VERSION` 且 `struct_size >= X_BASELINE_SIZE`**
  （实现见 `pluginxx/api/iface_meta.h`）。
- 四张 musicxx 专用表（`musicxx.hooks` / `musicxx.host` / `musicxx.ui` / `musicxx.vars`）随之补齐
  `X_MIN_VERSION` 与 `X_BASELINE_SIZE`，SDK 在 `plugin_kit.h` 里为每张表给出 `pluginxx::IfaceMeta` 特化。
  现在都是 v1、基线 = 整表大小，即"表可用 = 全部成员可用"（**行为不变**）。
- 对插件作者的要求：以后**追加在表尾的成员要用 `pluginxx::ifaceHas<&表::成员>(t)` 判定**再取值 ——
  老主程序给的表比插件期望的短，只判空指针会读到紧邻的另一块内存。写法见
  `docs/plugin-native-api.md` §4.1。
- 加载判据统一：随包内置插件与动态库插件现在都只拒绝**低于**最低 API 版本
  （`PLUGINXX_MIN_API_VERSION`，当前 1）的声明；声明更高不再被内置路径拒绝。
- 文档与注释的称呼与依赖库统一（「内核」→「插件框架」等），只改文字，不影响代码。

### 新增：插件框架版本号声明

- 插件框架从 `2026.1011.1` 起**声明自己的版本号**，格式 `<年>.<月日>.<小版本号>`：
  Dart 侧 `MusicxxPluginVersion`（`lib/src/version.dart`，含版本 / 提交 / 日期 / 仓库地址），
  C 侧 `MUSICXX_EXTERN_PLUGIN_VERSION`（`src/include/musicxx_extern_plugin_api.h`），
  `pubspec.yaml` 的 `version` 同步。
- 读版本号的接口值随之变成这个格式：`musicxx_extern_plugin_version()`、
  `host.info().hostVersion`、JS 的 `musicxx.version`（以前是 `0.1.0`）。
  **只是版本字符串的取值变化**，字段名、ABI、判断逻辑都没变。
- 插件清单的 `version` 改用同一套格式（例 `2026.1010.1`，以前是 `20261010.1.0`）：
  打包出来的安装包名随之变成 `<插件 id>-2026_1010_1.zip`。
  **旧清单不用改**：`version` 是自由字符串，插件框架与安装流程都不校验它的格式。
- musicxx 版本与插件框架版本 / 提交 / 日期的对照表、改版本号的流程：
  `docs/version-history.md`。

### 破坏性：能力调用只有异步形态

- **C ABI**：删除 `musicxx_extern_plugin_plugin_call`（同步、调用线程等待）；
  新增 `musicxx_extern_plugin_plugin_call_async`（受理即返回 `request_id`，结果经事件
  `musicxx.plugin.call.result` 回填）。
  - **换符号名是刻意的**：同名改签名时混用新旧库会用新参数去打旧函数（内存越界），
    换名之后"插件框架库太旧"能被探测到 —— 报一条可读错误，**不退回同步**。
  - **受理成功 ⇒ 一定有终局**：正常结果、执行失败、插件卸载（`plugin_unloaded`）、
    插件框架停机（`host_stopped`）四条路都会推一条结果事件，调用方不会一直悬挂。
- **Dart SDK**：`MusicxxPluginManager.call(...)` 由同步返回值改成返回 `Future<Object?>`。
  同步写法（`final v = host.plugins.call(...)`）编译不过，改成 `await`；需要一个"不等"的
  入口时自己 `unawaited(...)`。
- **被调方约定**：能力处理器可以异步完成（native 返回 `pluginxx::Task<T>`，JS 返回 Promise），
  但不能写阻塞处理器 —— 长任务用 `co_await pluginxx::offload(*this, work)`（native）
  或自己的后台任务。
- **JS → JS 的异步能力**：以前处理器返回 Promise 会拿到 `capability_async_without_wait`，
  现在能正常拿到结果（同一条 JS 线程就地调用，结算后回执）。
- 主程序侧的行为变化：打开插件页面 / 点按钮不再"整个界面等插件框架返回"（不再有
  "调用插件能力失败，未在时限内完成"这种因互锁产生的报错）。

### 新增：功能点（应用留出的扩展点 + 插件自定义功能点）

- 新的动作：`musicxx.feature.point.call`（调一个功能点）、`musicxx.feature.point.define`（声明）、
  `musicxx.feature.point.undefine`（收回）。
- 新的钩子（第 71 条）：`musicxx.feature.point.call` —— **插件自定义功能点的通用实现通道**。
  不要为每个功能点造新钩子（插件框架只认固定钩子表，未知 id 直接拒绝注册）。
- 新的 JS 糖：`musicxx.feature.callPoint/define/provide/unprovide`。
- 新的 native 糖：`featurePointDefine/Provide/Unprovide/Call/List`（`PluginBase`）。
- 主程序目前对外开放调用的功能点：整曲频谱 `musicxx.media.spectrum.extract`、副歌区段
  `musicxx.media.chorus.analyse`、封面取色 `musicxx.media.palette.provide`。
  **调用零副作用**：不写主程序的值仓、不改主程序状态。
- 封面取色钩子的载荷新增 `cover` 段（"这次算哪一张图"）：`current` / `file` / `url` / `data`。
  老插件不认它也能跑（对 `current` 这一种用法的结果是对的）。

### 约定：回答形状与 `caller`

- 取值型钩子与功能点通道的处理器**回答请用 `patch` 包一层**（`{"patch": {...}}`）：
  插件框架的 `firstNonNull` 只在收到 `patch`（或 `action`）时才停止遍历，裸字段会被后面的处理器覆盖。
  一个实现时两种写法等价，多个实现时裸字段会表现为"谁最后注册用谁的"。三个已有钩子的 `doc`
  也补了这句说明（不改行为）。
