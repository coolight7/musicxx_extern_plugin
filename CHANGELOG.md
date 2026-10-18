# 变更记录（框架 / SDK / 插件框架库）

这个文件只记**会影响插件作者与接入方**的改动：协议、ABI、行为约定、破坏性变更。
细节说明在各篇作者文档里（`docs/`），这里给"从哪一版开始不一样了"。

版本按插件框架版本号（格式 `<年>.<月日>.<小版本号>`，见 `lib/src/version.dart`、
`src/include/musicxx_extern_plugin_api.h` 与 `docs/version-history.md`）与改动日期记。

## 未发布

### 新增：本地 HTTP 文件服务（仅 JS：`musicxx.net.serveFile/serveBytes/serverInfo/stopServer`）

- JS 运行时没有 socket，而播放器只认 `http(s)` 与本地文件 —— 现在插件可以把**本地文件**
  （`serveFile`）或**一段内容**（`serveBytes`，≤ 8 MiB）登记成 `http://127.0.0.1:<随机端口>/f/<token>` 地址：
  同步返回，支持 `GET` / `HEAD` + 单段 `Range`（206 / 416），文件按 64 KiB 边读边写。
- 安全边界：**只监听回环**；URL 里只有 128 bit 随机 token，服务端按 token 查表、不解析路径
  （拿到 URL 也读不到别的文件）；登记条数 ≤ 1024（淘汰最旧）、并发连接 ≤ 32。
- 生命周期跟着插件实例：停用 / 卸载 / 关『拟声++』/ 插件框架停机自动关掉（端口释放），`stopServer()` 可以提前关。
- 这是**代理**的一般机制：`musicxx.net.fetch/download` 取到的数据、FTP / 私有协议、HLS 分片、转码产物
  都可以先转成 HTTP 再给播放器，播放器只看到干净的本地 URL。写法见 `docs/plugin-api.md` §3.2.6
  与 `docs/plugin-js-api.md` §7.3.1；完整例子：`plugins/example_override_media_source`。

### 新增：示例插件 `example_override_media_source`

- 演示"插件参与音源解析"的三条路（各自有开关）：覆盖内置 Bili 解析（BV 号 → 直链 + 请求头）、
  自定义音源类型 `plugin_bili`（地址里也写 BV 号）、本地文件经插件 HTTP 服务播放（`plugin_proxy_local`）。
- 带自绘设置页（三个开关 + 计数 + 服务状态）、集成测试与 README；
  它同时也是"异步裁决钩子里可以等主程序动作"的样板（见下一条修正）。

### 修正：裁决型钩子里发起的动作请求不再等到超时

- 以前 Dart 侧回复插件发起的动作请求一律转给插件框架线程执行；而**异步派发的裁决型钩子**
  （`musicxx.player.source.resolve` 等）在派发期间正占着这条线程等 JS 处理器 —— 两边互等，JS 只能等到
  自己的超时（表现为 `动作失败: action_timeout`、钩子按「没有裁决」返回 null）。"在钩子里再去查一次
  接口"这类插件因此没法用。
- 现在回答动作请求**不经过框架线程**：先交给 JS 引擎自己的请求登记表（它本来就能在任意线程回答），
  只有动态库插件的请求才转给框架线程。插件侧不用改。
- 仍然只能等**主程序动作**（`musicxx.net.*` / `musicxx.storage.*` 等）：`musicxx.vars.*`、跨插件调用、
  `musicxx.hooks.*` 在异步裁决钩子里等会一直等到超时。写法见 `docs/plugin-js-api.md` §10。

### 构建：最低版本跟随主程序（macOS 12 / iOS 15）

- macOS 侧以前不传 `CMAKE_OSX_DEPLOYMENT_TARGET`，CMake 就按"构建机当前的系统版本"编译（实测产物是
  `minos 26.0`），现在由 `--macos-target` 显式给出（默认 12.0）；`src/CMakeLists.txt` 把
  `CMAKE_OSX_SYSROOT` / `CMAKE_OSX_ARCHITECTURES` / `CMAKE_OSX_DEPLOYMENT_TARGET` 转发给所有嵌套构建
  （以前只对 iOS 转发），空值不再传下去。
- iOS 侧默认最低版本从 14.0 改为 15.0，与主程序 `IPHONEOS_DEPLOYMENT_TARGET` 一致；主程序的两条构建
  阶段（`macos/tool/copy_extern_plugin_dylib.sh`、`ios/tool/embed_extern_plugin_dylib.sh`）会把 Xcode
  的部署目标传给这里的自动构建，两边不再各写一份默认值。
- 最低版本进了构建签名：取值变化会重置构建目录重新编译，不会拿旧目标的产物顶替。

### 新增：musicxx 对外接口总表 `docs/musicxx-interfaces.md`

- 新增 **`docs/musicxx-interfaces.md`**：musicxx（主程序）**对外暴露的全部接口**集中在一处，
  便于 AI 与脚本查找：五条通道怎么选、接入点文件字段、权限边界、`/cli` 的请求与响应形状、**74 条命令**
  （动作 50 / 应用命令 19 / 能力导出 2 / 变量 3）与参数解析规则、**31 个 MCP 工具**（28 个命令类 +
  3 个能力类）与各自的等价命令和文本形态、本机 HTTP 与 WebSocket 路由表、
  `/control` 的数字指令表、事件号表、服务版本门槛、安全边界与排障表。
- 与既有文档的分工：插件**自己**能调什么看 `docs/plugin-api.md`（动作 / 变量 / 功能点 / 钩子），
  用命令行管插件看 `docs/plugin-agent-cli.md`，本文补充"应用对外提供什么、怎么连上"。

### 修正：UI 项的 `route` 允许跨插件

- 以前主程序只接受 `ext://<自己>/<视图id>`，写成别的插件会提示「入口地址无效」（插件框架注册时并不拦）。
  现在**与动作 `musicxx.ui.openRoute` 同一套规则**：`route: "ext://<插件id>/<视图id>"` 允许指向任意
  插件（含自己），页面由**目标插件**自己绘制 —— 发起方拿不到对方的数据，跨插件跳转前请自行确认对方在。
- 地址解析规则（三处共用一份实现）：`ext://` 前缀 → 第一个 `/` 前后都不能为空 → 视图 id 是第一个
  斜杠之后的**全部内容**（`ext://demo/a/b` 的视图 id 就是 `a/b`），两端空白去掉。
  UI 项写法见 `docs/plugin-ui.md` §1.3，动作写法见 `docs/plugin-api.md` §9.1。

### 文档：独立出两种形态共用的接口手册 `docs/plugin-api.md`

- 新增 **`docs/plugin-api.md`**：JS插件与动态库插件**共用**的接口手册，写清每个接口
  "做什么、参数是什么、返回什么、什么时候用、有什么坑"，并给出两种写法（`musicxx.*` 与 SDK 方法）：
  状态镜像（键与字段）、**全部已实现动作的完整参考**（播放 / 歌单 / 歌词 / 提示与页面跳转 / 存储 /
  网络 / 渲染槽位 / 封面与频谱 / 字体 / 能力导出 / 功能点 / 钩子自省 / 宿主 / 统计，含参数、返回、
  失败情形与示例）、事件主题与载荷、变量、能力、功能点、钩子写法、界面入口、日志与自省，
  以及命名空间 / 容量限制 / 超时一览 / 错误码 / 通用排障。
  **接口语义以这份为准**，`docs/plugin-js-api.md` 与 `docs/plugin-native-api.md` 只保留各自的语言差异
  （JS：目录与清单、生命周期与硬约束、`musicxx` 全局对象、定时器与工具；动态库：清单与多目标打包、
  SDK、框架线程、接口表判定、常用接口的 C++ 写法、构建部署）。
- 补上原文档缺的内容：JS 侧 `musicxx.vars.*` 全量方法、`musicxx.ui.*`（字体 / 入口增改删 / 自省）、
  动态库侧 `requestAction` 的全部动作与写法示例、未接入钩子的名字含义（`plugin-api.md` §8.6）。
- 纠正的过期说法：动作超时（JS `timeoutMs <= 0` 仍按 5000 ms；动态库传 0 = 不设超时）、
  跨插件能力超时（`<= 0` 仍按 3 秒）、UI 动作的形态名（官方动作是 `{"kind":"command"}`，
  写成 `"action"` 不会被识别）。

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
