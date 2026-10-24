# 变更记录（框架 / SDK / 插件框架库）

这个文件只记**会影响插件作者与接入方**的改动：协议、ABI、行为约定、破坏性变更。
细节说明在各篇作者文档里（`docs/`），这里给"从哪一版开始不一样了"。

版本按插件框架版本号（格式 `<年>.<月日>.<小版本号>`，见 `lib/src/version.dart`、
`src/include/musicxx_extern_plugin_api.h` 与 `docs/version-history.md`）与改动日期记。

## 未发布

### 新增：插件网络能力 `musicxx.net`（HTTP / TCP / WebSocket，JS 与动态库插件共用）

- 接口表 `musicxx.net`（`MusicxxPluginNetIface`，v1；SDK 里是 `iface.net`）：三个入口
  `call` / `call_async` / `cancel`，操作写成 `op + 参数 JSON → 结果 JSON`。老插件框架库上没有
  这张表（`queryInterface` 返回 `nullptr`），能力位是 `MUSICXX_EXTERN_PLUGIN_FEATURE_NET = 0x40`。
- 六种角色：HTTP 客户端 / HTTP 服务端 / TCP 客户端 / TCP 服务 / WebSocket 客户端 / WebSocket 服务。
  WebSocket 的握手、掩码、分片、`ping`/`pong` 与关闭握手由插件框架处理。
- **客户端支持 `https://` 与 `wss://`**：OpenSSL 以静态库链进插件框架（不随包带 `libssl` /
  `libcrypto`），证书链与主机名校验默认开；`tlsVerify` / `caFile` / `caPem` / `serverName`
  可以调。**服务端不做 TLS**（`http.serve` / `ws.listen` 只监听明文）。构建时关掉 TLS
  （`--no-tls` / `-NoTls`）的版本里 `https` / `wss` 得到 `tls_unsupported`；
  `net.info` 的 `tls` / `schemes` / `tlsProvider` 能看出当前构建的能力。
- **服务端默认只监听 `127.0.0.1`**，端口 `0` 由系统分配；显式 `host: "0.0.0.0"` 才会暴露给同网段。
- 事件主题 `plugin.<插件id>.net`，`kind` 区分 `http.request` / `http.response` /
  `socket.listening|accepted|connected|data|closed|serverClosed` /
  `ws.listening|open|message|pong|close|serverClosed`。
  **JS 侧由插件框架直接投递到 JS 线程**（不经事件总线）：这样“在返回 Promise 的能力/钩子里
  `await` 网络事件”不会与“框架线程等 Promise”互相等待；动态库插件仍用事件表订阅（框架线程派发）。
- JS：新增 `musicxx.net.http.*` / `musicxx.net.socket.*` / `musicxx.net.ws.*` 与
  `musicxx.net.on / onHttpRequest / onSocketEvent / onWsEvent / info`；
  **`musicxx.net.fetch` / `download` 改为用这套实现**（参数与返回形状不变，不再走主程序动作，
  因此也不再继承主程序的代理设置）；`serveFile / serveBytes / serverInfo / stopServer` 不变。
- 新增两个 op：`http.fetch`（= 老的 `musicxx.net.fetch`）与 `http.download`（正文流式写入
  `<data_dir>/<插件id>/data/downloads/`）。老的 `musicxx.net.fetch` / `download` **动作**仍然注册在
  主程序里（给直接调动作的插件用），新插件用 op / JS 封装。
- SDK：`netAvailable()` / `netCall` / `netCallAsync` / `netCancel` / `netSubscribe`，
  op 名常量在 `musicxx::plugin::net_op`（含 `httpFetch` / `httpDownload`）。
- 上限：每个插件同类服务 ≤ 8、连接 ≤ 64、HTTP 客户端并发 ≤ 16、单个 HTTP 服务等回答的请求 ≤ 64、
  HTTP 正文 ≤ 8 MiB、WebSocket 单条消息 ≤ 8 MiB、单条连接发送队列 ≤ 4 MiB。
- 新增示例 `plugins/example_js_net`（载入 1.5 s 后自动跑一遍 HTTP / TCP / WebSocket 的
  客户端 + 服务端自检，结果写日志；也可调 `selftest` / `status` / `info` 能力）。
- 预留表少了一张：`musicxx.net` 从“IID 已冻结、表体未实现”移到已实现；
  `musicxx.player` / `library` / `lyrics` / `storage` / `stats` 仍未实现。
- 版本号 2026.1024.1 → 2026.1024.2。

### 新增：歌词加载钩子 `musicxx.lyric.load` 与五个细分阶段

- `musicxx.lyric.load`（裁决型，`firstNonNull`，`before` + `impl` + `after`，**已接入**）：主程序要为
  一首歌加载歌词时派发一次。插件可以直接给出歌词内容（接管整条加载：应用自己的读取不再执行），
  也可以只在前链改写参数、在后链改结果。载荷是这一轮加载的全部输入（歌曲视图、音源视图、
  允许项 `enable`、匹配用的 `keyword` / `artist`、强制 `lrcid` / `position`、`caller`），
  回答是 `{"patch": {"lyric": {"content": "<LRC 文本>", "sourceType": "builtin", "offsetMs": 0}}}`；
  `{"patch": {"lyric": {"disabled": true}}}` = 这首歌禁用歌词。
- 五个细分点 **只在应用自己的实现里**按顺序询问，插件接管了 `musicxx.lyric.load` 之后它们不会再被触发：
  `musicxx.lyric.load.musicxx_server`（拟声云歌词：歌曲绑定的歌词来源）→
  `musicxx.lyric.load.cache`（歌词缓存：应用自己保存过的歌词内容）→
  `musicxx.lyric.load.builtin`（内置歌词：播放器元数据 + 媒体文件标签）→
  `musicxx.lyric.load.builtout`（外置歌词：同名 `.lrc` / 云盘与媒体库的歌词接口）→
  `musicxx.lyric.load.match`（自动匹配）。
- 五个细分点都是裁决型 + `firstNonNull` + `before` + `impl` + `after`：可以替换某一个阶段的实现，
  也可以只在该阶段前后插手（例如前链改 `keyword`、后链修歌词文本）。载荷与回答形状与主钩子**完全一致**，
  只是 `enable` 决定这一阶段是否会被询问。
- 钩子总数 **60 → 66**。写法见 `docs/plugin-hooks.md`。

### 破坏性：钩子改成「一个调用 = 前链 + 实现链 + 后链」，16 条 id 被删除或合并

**钩子 id 现在只表示一次调用**，参与方式（`before` / `impl` / `after`）由注册时的 `phase` 表达：
同一条钩子上可以分别注册前链（改输入 / `stop`）、实现链（给结果）与后链（改输出 / `drop`）。
**后链是逆序执行的**（优先级降序、同优先级注册顺序也反）：外置插件最后改输出，应用自己的兜底
实现不能盖掉插件的改写。

- **删除（注册会报“未知钩子”）**：`musicxx.lyric.load.before`、`musicxx.lyric.provider`、
  `musicxx.lyric.transform`、`musicxx.lyric.searchStr`、`musicxx.lyric.loaded`、
  `musicxx.player.beforeOpen`、`musicxx.media.wave.ready`、`musicxx.media.chorus.analysed`。
  这些条目从来没有被主程序派发过（注册成功、永远不触发），删掉只会让失败更早更清楚。
  歌词这几条换成了上面新增的 `musicxx.lyric.load` 一条调用 + 五个细分点（`before` / `loaded` 现在是它的阶段）。
- **合并（成对的两端现在是同一条钩子的两个阶段）**：
  - `musicxx.net.request.before` + `musicxx.net.response.after` → `musicxx.net.request`（`before` + `after`）
  - `musicxx.download.before` + `musicxx.download.completed` → `musicxx.download`（`before` + `after`）
  - `musicxx.icon.request` + `musicxx.icon.generated` → `musicxx.icon.fetch`（`before` + `impl` + `after`）
  - `musicxx.playlist.loaded` + `musicxx.playlist.filter` → `musicxx.playlist.load`（`before` + `after`）

  这四条新 id 主程序**还没有接入**（注册不会报错、当前版本也不会触发）；它们没有 `impl` 阶段的
  那三条属于“包装点”：实现固定是应用自己，插件只在前后插手。
- **新增阶段声明（会真的派发）**：`musicxx.feature.point.call`（`before` + `impl` + `after`）、
  `musicxx.player.seek` / `volume` / `speed` / `pitch` / `error` / `beforePlaySong` /
  `source.beforeParse`（`impl` + `after`）。后链能改最终裁决：回答 `{"value": {...}}`，
  `{"action":"drop"}` 表示这次没有裁决。
- **注册规则**（C ABI）：`MusicxxPluginHookSpec` 末尾追加 `phase` 成员，写它时把 `version` 设成
  `MUSICXX_PLUGIN_HOOK_SPEC_VERSION`（2）；**老插件写 `version = 1` 照旧可用**（等价于 `impl`）。
  覆盖式注册的键变成 `(实例, 钩子, ownerTag, 阶段)`；`unregister_hook` 仍按
  `(实例, 钩子, ownerTag)` 摘，即该 ownerTag 在所有阶段的登记一起摘掉。`musicxx.hooks` 表版本
  1 → 2（`MIN_VERSION` 与基线大小不变）。
- **只接受钩子声明过的阶段**，且阶段处理器必须是同步处理器（`hook_sync`）：否则注册被拒绝
  （`-1`，日志写清原因）。声明了却不派发的阶段等于“注册成功但永远不触发”，所以只声明真的会派发的。
- **新增 C ABI**：`musicxx_extern_plugin_hook_emit_phase` / `..._hook_count_phase` /
  `..._hook_handlers_phase`，以及能力位 `MUSICXX_EXTERN_PLUGIN_FEATURE_HOOK_PHASES`。旧符号语义
  不变：`hook_emit` = `impl` 阶段，`hook_count` / `hook_handlers` = **全部阶段**的总数 / 全部列表
  （每项多一个 `phase` 字段）。
- **自省**：`hook_info` 增加 `phases` 与 `phaseCount`；`musicxx.hook.changed` 增加 `phaseCount`，
  处理器列表每项带 `phase`；`hook_stats` 每行带 `phase`。
- **JS**：`musicxx.hooks.register(id, { phase }, fn)`、`unregister(id[, phase])`、
  `has(id[, phase])`、`handlers(id[, phase])`。
- **native SDK**：`musicxx::plugin::HookPhase`、`hook(id, phase, priority, fn)`、
  `hookBefore` / `hookAfter`；注册前会先读 `hook_info` 的 `phases`，没声明就返回 `-1`。
- 钩子总数 **72 → 60**。写法见 `docs/plugin-hooks.md` 开头的「阶段」段与
  `docs/plugin-api.md` §8.1。


### 新增：`musicxx.player.position` / `seek` / `volume` / `speed` / `pitch` 已接入

- `musicxx.player.position`（观察型，**1 秒一条**）：载荷 `{sid, positionMs, durationMs}`。
  只报进度、不报状态（“是否在播放”读 `musicxx.state.env.isPlaying`）；状态镜像里的
  `position` 仍只在启动、切歌与播放状态变化时刷新，要实时值读变量 `musicxx.player.position`。
- `musicxx.player.seek`（裁决型）：应用**要把播放位置改到 `to` 之前**派发，载荷
  `{sid, from, to, durationMs}`；`patch.to`（毫秒）改目标位置，`cancel` / `skip` = 这次不跳。
- `musicxx.player.volume` / `speed` / `pitch`（裁决型）：应用**要把音量 / 速度 / 音调改成 `to`
  之前**派发，载荷 `{from, to}`；`patch.to` 改新值，`cancel` / `skip` = 这次不改。
- 这四个裁决型都是**异步派发**，应用最多等 **0.5 秒**（超时按“没意见”走原值）；
  界面上的所有入口（滑块、快捷键、长按加减速、局域网控制、命令行 / 动作、脚本、恢复设置）
  都走同一处，所以“用动作改值”也会先问钩子 —— **处理器里不要反过来再调这些动作**。
- 配套：动作 `musicxx.player.setPitch`（之前只在 SDK 里留了名字），状态镜像
  `musicxx.state.player` 多出 `pitch`，变量目录多出 `musicxx.player.position`（只读），
  命令行多出糖命令 `musicxx-cli pitch <0.25~2>`、MCP 多出 `SetPitch`。写法见
  `docs/plugin-hooks.md`、`docs/plugin-vars.md` 与 `docs/musicxx-interfaces.md`。

### 新增：`musicxx.app.background` / `musicxx.app.foreground` 已接入

- 两个观察型钩子现在会真的派发：应用**进后台** / **回前台**各一次，载荷分别是 `{state}` 与
  `{state, backgroundMs}`（`state` = 触发这次通知的生命周期状态名）。窗口只是失焦、下拉通知栏这类
  一闪而过的中间态不算后台；桌面端隐藏 / 最小化窗口、移动端切到后台算后台。
- 同时补上配套的读写渠道：导出变量 `musicxx.app.lifecycle`（`get` + `notify`，值
  `foreground` / `background`）、动作 `musicxx.app.foreground` / `musicxx.app.background`
  （与命令行 `window.foreground` / `window.background` 同一份实现），状态镜像
  `musicxx.state.app` 多出 `foreground`（布尔）与 `lifecycleState`（原始状态名）两个字段。
- 写法见 `docs/plugin-hooks.md`（两个钩子的载荷）、`docs/plugin-vars.md`（变量）与
  `docs/musicxx-interfaces.md`（命令行 `app.lifecycle` / MCP `GetAppLifecycle`）。

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
  插件音源类型 `plugin_bili`（类型 + 资源 id，`src` 里写 BV 号）、本地文件经插件 HTTP 服务播放（`plugin_proxy_local`）。
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
