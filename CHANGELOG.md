# 变更记录（框架 / SDK / 插件框架库）

这个文件只记**会影响插件作者与接入方**的改动：协议、ABI、行为约定、破坏性变更。
细节说明在各篇作者文档里（`docs/`），这里给"从哪一版开始不一样了"。

版本按插件框架版本号（格式 `<年>.<月日>.<小版本号>`，见 `lib/src/version.dart`、
`src/include/musicxx_extern_plugin_api.h` 与 `docs/version-history.md`）与改动日期记。

## 未发布

### 界面描述层：列表项 `custom`、`musicxx.DirListView`、`musicxx.AdaptLayout`

这一版让列表项能装任意块，并为 musicxx 加了两个专属组件：

- **列表项 `custom`**：`ListViewBase` / `ListView` / `musicxx.DirListView` 的每一项除了
  `title` / `subtitle` / `trailing` / `image` / `icon` / `tone` / `selected` / `action`，还能写
  `custom: [ 任意块 ]` —— 写了就**整项内容**由它画（行尾自动插入的编辑 / 多选 / 拖拽按钮不受影响）。
  列表项另支持 `path`（这一项的路径 / 身份）与 `isDir`（目录项），动作参数里原样回带。
  → 以前“要自定义行只能用 `Block` + `Row` 叠出来”的写法，现在可以放进真正的列表（能滚、能定位）。
- **`musicxx.DirListView`（musicxx 专属）**：一层目录的条目 + 顶部路径栏（路径栏里带「上一级」按钮，
  给了 `pathAction` 才出现）+ 可选的多选勾选框，**行与顶部栏直接用应用内置的目录列表画**
  （文件夹/文件图标、标题 + 第二行、左上角条目数、勾选框样式都跟应用一致）。当前目录与勾选态由插件
  保存，客户端只派发动作（`itemAction` / `selectAction` / `pathAction`）；返回键是“退出页面 /
  浮层”，要逐层往上退就给 `pathAction`（根目录别再给）。**刷新与翻页也走列表自己的手势**：
  给了 `refreshAction` / `loadMoreAction` 就能下拉刷新 / 滚到底加载更多（不用在页面里放按钮；
  返回 `false` 或 `{ok:false}` 算没成功，上拉会提示“加载失败”；还有更多时才给 `loadMoreAction`）。
  行样式由应用定，所以 `tone` / `image` / `custom` 在它里面不生效；不认识的客户端退成普通列表文本。
- **列表项 `song`（这一项是一首歌）**：条目带 `song: {type, src, name?, artist?, fileName?,
  size?, durationMs?, cover?}`（与 `musicxx.playlist.addSong` 的 `songs[]` 同一套字段）时，
  目录列表就把这一行画成内置的**歌曲行**（封面 + 歌手 + 音源标签），不必自己拼 `Row` 画。
  `src` 必填（空 = 不是歌曲）；`type` 不写就用本插件声明的唯一音源类型。只是画得一样：点行仍派发
  `itemAction`，加进歌单仍要调 `musicxx.playlist.addSong`。
- **`musicxx.AdaptLayout`（musicxx 专属）**：按窗口宽高比切换两套内容（竖屏 `columnBody` /
  横屏 `rowBody`），给了第二套就按比例分两栏，对应应用内置的 `XXAdaptiveLayout`；
  不认识的客户端用 `rowBody`。
- **滚动区域只能由列表块给**：`ListViewBase`（`height` 不给、放在 `Expanded` 里就自己滚，条目内容用
  `custom`）或 `musicxx.DirListView` —— 描述层没有通用的滚动容器块。
- kit：新增 `kit.dirListView(...)` / `kit.adaptLayout(...)`（各自带“客户端不支持时”的降级写法）。
- 说明见 `docs/plugin-ui.md` §2.3 / §2.5.1，字段见库文档 `docs/ui-schema.md`。



### 新增：添加歌曲入口 `addsong.entry`

歌曲列表页 [+] → 「添加歌曲」弹窗里现在有插件的位置：注册一个 `addsong.entry` 项即可出现在
**内置来源之后**、工具项之前。点击时动作参数会带上当前歌单的 `pid` / `name` / `songNum`。

- 常量：`MUSICXX_PLUGIN_UI_TYPE_ADDSONG_ENTRY`（C）/ `MusicxxPluginUIType.addSongEntry`（Dart SDK）/ JS 短名 `addsong.entry`；
- 与入口项同一个校验规则：`title` 必填、动作三种写法（`route` / `dispatch` / `command`）；
- 旧插件框架不认识这个类型 → `uiRegister` 返回 `-4`（插件记日志继续跑即可）。

### 新增：插件视图浮层 `musicxx.ui.openOverlaySheet`

`musicxx.ui.overlay.show` 的内容是静态 `blocks`；这一个打开的是**插件自己的视图**，浮层里的动作
返回 `{"view": …}` 就能翻页 / 刷新。参数 `{route, title?, arguments?, size?, dismissible?}`，
返回 `{ok, reason, title?}`，**动作在浮层关掉之后才完成**（调用方正好在关闭后刷新列表）。
典型用法就是「添加歌曲」入口：挑完歌 → `musicxx.playlist.addSong` → 关掉浮层。

### 新增：`musicxx.playlist.addSong`（往一张歌单里加歌）

参数 `{pid, dedup?, songs:[{type, src, name?, artist?, fileName?, size?, durationMs?, cover?}]}`，
返回 `{ok, pid, name, total, added, skipped}`。`type` 必须是调用方自己的音源类型
（`plugin.<插件id>.` 前缀）。加歌方式与内置“添加歌曲”一致（去重、往前插、写播放记录标签、
当前队列会刷新）。从未实现的 `musicxx.library.addSong` 常量已删除（用它改了名）。

### 新增：`socket.pause`（背压）与 `socket.send` 返回 `queuedBytes`

插件转发大流量（把 FTP / 自定义协议的字节转给播放器）时必须有背压：发送队列上限 4 MiB，
超了 `socket.send` 回 `queue_full`。现在可以在队列变长时 `socket.pause(id, true)` 暂停上游读
（不再有 `data` 事件），缓过来再 `socket.pause(id, false)`。`socket.send` 的结果多了
`queuedBytes`。

### 修正：`socket.send` / `ws.send` 的「一个对象」写法

`socket.send({id, dataBase64})` 这种写法之前会被当成 `send(id, data)` 解析，参数整个丢掉
（回 `bad_args` / `缺少连接 id`），只有 `send(id, data)` 两个参数的形式能用。现在两种写法都认：
`{id, dataBase64}` / `{id, data}` / `{id, text}` 都能发，转发二进制不用再拆成两个参数。
`ws.send` 同样是两种写法。

### 新增：`musicxx.util.textEncode` / `textDecode` / `base64Encode` / `base64Decode`

JS 侧现在能做“文本 ↔ 指定编码的字节”转换（原生 iconv）：`{text, encoding}` → `{base64}`，
`{base64: 分片数组, encoding}` → `{text}`。给 FTP（GBK 文件名）、局域网共享这类协议用；
QuickJS 没有 `TextDecoder`，所以放在原生层。都是同步返回。

### 新增：插件形态（kind）可以由插件提供，`api_version` 只约束“按 C 接口编译”的形态

插件框架原本把 `native` / `js` 两种形态写死在扫描与加载里。这一版把它们收进一张**形态注册表**，
并新增接口表 `musicxx.kind`：插件可以登记"我支持 kind=X"，交出加载 / 卸载能力，于是
**把 JS 运行器搬到插件里、以后再接 Python 运行器，都不用改插件框架**。

- 内置两项：`native`（动态库插件，按 C ABI 编译）与 `js`（内置 JS 运行器，不按 C ABI 编译）。
  `kindList()` 能列出全部形态及其提供者。
- **内置形态可以被插件接管**：登记时写 `"overrideBuiltin": true` 就能把 `js` 交给插件跑
  （把 JS 运行器搬进插件这条路），撤销登记后自动回到内置实现。`native` 不允许接管。
- 插件登记的形态跟着插件生命周期撤销，不写磁盘；`native` / `js` 不能被覆盖；`loadCapability` /
  `unloadCapability` 必须写在本插件自己的命名空间（`plugin.<插件id>.`）下。
- 扫描 / 安装预检 / 加载统一按注册表判定：没登记的 `kind` 判为"当前跑不了"，原因写
  "先安装并启用提供这个形态的插件"；登记了但提供者没加载，原因写"由插件 X 提供，但它没有加载"。
- **每个形态有自己的接口代次对**（`interfaceVersion` / `minInterfaceVersion`）：清单 `api_version`
  表示"插件按哪一代接口写的"，判定是 `>= 该形态的兼容下限`；插件读到的 `host.info().apiVersion` /
  `config().apiVersion` 是该形态的**当前代次**（动态库插件 3、JS 插件 2，两套编号独立）。
  - 内置 `native`：3 / 3（C 接口代次，通用表变了就是破坏性变更）；
  - 内置 `js`：**2 / 1** —— 变量通道、网络、功能点调用、静态路由、界面描述层 v2 这些都是**新增**，
    所以只抬当前代次，下限仍 1：写 `api_version: 1` 的 JS 插件照常加载；
  - 运行器登记的形态：`requiresPluginAbi: true` 的用框架的 C 接口那一对；解释执行的自己写
    （不写 = 不判）。
  - 将来 JS 接口发生**破坏性变更**（改名 / 删接口 / 改语义）时，把 JS 的兼容下限抬到当时
    的当前代次，老插件就会被判"跑不了"；与 C 接口的 `PLUGINXX_MIN_API_VERSION` 同一套规则。
- **`api_version` 的判定只在插件框架一处**：按所属形态的兼容下限判，结论写进扫描/预检的
  `supported` / `reason`；安装预检（Dart）不再自己拿 C 接口下限重判一遍（只在老版本插件框架库
  不报形态事实时回退到旧规则）。
- **"这个包能不能跑"交给运行器判定**（新增可选 `validateCapability`）：加载前先调它一次，
  回答 `{"ok":false,"error":"..."}` 就拒绝加载（理由原样给用户）。安装预检也问它 ——
  `plugin_inspect` 是同步 C ABI，所以它只把形态事实（`kindKnown` / `kindRequiresPluginAbi` /
  `kindInterfaceVersion` / `kindMinInterfaceVersion` / `kindProvider` / `kindProviderId` /
  `kindValidateCapability`）报出来，由主程序在解包预检里
  发那句 `{"action":"validate",...}` 请求。
- 托管插件：加载 / 卸载 / 启停都转告运行器（能力请求与回答见 `plugin_api.h` 的
  `MUSICXX_PLUGIN_IFACE_KIND`），`plugin_list` 与扫描结果把它们列成独立插件（`hosted: true`、
  `provider` / `providerId`）。**它们的钩子 / UI / 能力记在运行器插件名下**（插件框架不为托管插件
  创建实例），这是当前版本的边界。
- **示例 JS 插件的 `api_version` 回到 1**：上一版为了配合插件 ABI 代次把全部示例清单抬到 3，但
  JS 接口有自己的代次（当前 2、兼容下限 1）。示例 JS 清单因此恢复 `1`（清单里带一行注释说明），
  动态库示例保持 `3`。
- 写法见 `docs/plugin-native-api.md` §4.3；SDK 便捷函数 `kindRegister` / `kindUnregister` / `kindList`。


### 破坏性：依赖库 `cxx_*` 升级到「作用域引用」一代，插件 API 契约代次 1 → 3

依赖库（`cxx_pluginxx` / `cxx_utilxx_base` / `cxx_pluginxx_ui`）去掉了一批只在某个主程序里
有意义的成员，把「接口表」收紧成与业务无关的通用表。**按旧头文件编出来的插件不能再加载**
（调用约定变了，硬跑会读到错位的参数），因此插件 ABI 契约代次从 1 升到 3。

- **通用接口表的版本**：`pluginxx.events` / `pluginxx.capabilities` / `pluginxx.config` /
  `pluginxx.cancel` 升到 **v3**；`scheduler` / `coroutine_runtime` / `plugins` / `json` / `log` /
  `tasks` 仍是 v1。v3 的含义：一次加载、可挂到多个作用域（作用域由主程序定义），且
  **主程序业务语义不进入通用表**。
- **事件订阅要带作用域引用**：`events->subscribe(host, scope, topic, handler, ud)`。
  `scope = nullptr` 表示订阅主程序总线（插件在音乐类主程序里就传它，SDK 的 `subscribeTopic`
  已经这么做）；传了带 id 的作用域时，musicxx 没有该作用域，订阅返回空。
- **调能力要带作用域引用**：`capabilities->invoke_capability_async(host, scope, cap, method, args, cb, ud, err)`；
  SDK 的 `capabilityCall` 传 `nullptr`（音乐类主程序没有作用域）。
- **`pluginxx.config` 少了两个成员**：`get_tool_prompt` 与 `get_session_work_dir` 交回主程序业务表
  （musicxx 从来没用它们）；插件要看运行环境用 `config()`，插件自己的目录用 `configPath()`。
  SDK 的 `workDir()` 随之删除。
- **`pluginxx.cancel` 只剩一个成员**：`is_cancelled(session_id)` 删除，换成通用的
  `op_cancel(handle)`（异步操作统一在它上面取消，与 `scheduler` / `capabilities` / `tasks` 同语义）；
  SDK 的 `sessionCancelled()` 随之删除。
- **`PLUGINXX_API_VERSION` 不再由依赖库固定**：`cxx_pluginxx` 的默认值是 1（它被多个主程序共用），
  主程序构建时用 CMake 变量 `PLUGINXX_API_VERSION` / `PLUGINXX_MIN_API_VERSION` 覆盖。本包在
  `src/CMakeLists.txt` 里固定为 3 并下发到插件框架库与插件 SDK；**插件作者用
  `musicxx_plugin_add_target` 构建时这个定义自动带上**，自己拼 CMake 的必须补
  `PLUGINXX_API_VERSION=3`（`plugin_api.h` 的 `MUSICXX_PLUGIN_API_VERSION` 就是它）。
- **清单的 `api_version`：动态库插件写 3**：低于 3 的在扫描/加载阶段被拒（原因写「插件声明的 API 版本
  低于当前插件框架支持的最低版本」）。**已有的动态库插件必须按新头文件重新编译**，否则加载阶段会
  报 API 版本过低或入口无效。**JS 插件不受影响**（它不调 C 接口表，这项判定不管它），示例 JS 插件
  保持 `api_version: 1` —— 清单里写的是“插件调哪一代通用接口表”，而不调就不需要抬高。
- **旧插件框架库**：`musicxx_extern_plugin` 库与主程序必须同一次构建（管理页会提示库不是这次构建的产物）。
- **清单资源声明更通用了**：`skill` / `memory` / `mcp` 改成 `resources:` 列表（`type` 由主程序定义），
  旧写法仍能解析成同名 `type`；musicxx 不解释资源声明，行为不变。
- 版本号 2026.1025.1 → 2026.1026.1。


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
  因此也不再继承主程序的代理设置）；静态内容（把自己手里的文件 / 内存内容交给播放器）
  并到同一个 HTTP 服务上：`http.serve` 的 `static` / `http.route`，见下文「HTTP 服务的静态内容路由」。
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

### 新增：HTTP 服务的静态内容路由（`http.serve({static})` / `http.route`）

- JS 运行时没有 socket，而播放器只认 `http(s)` 与本地文件 —— 现在把**本地文件**或**一段内容**
  （≤ 8 MiB）挂到自己的 HTTP 服务上，就能变成 `http://127.0.0.1:<端口>/f/<token>/<文件名>`：
  `http.serve({id, staticOnly, static: [{file|bytes, path?, contentType?, fileName?}]})` 一次挂好，
  或者起服务后 `http.route({serverId, file|bytes, …})` 逐条加、`http.unroute({serverId, path})` 摘掉。
- **命中静态路由的请求不进插件**：由框架直接回 200 / 206 / 416（支持 `GET` / `HEAD` 与单段
  `Range`，文件按 64 KiB 边读边写），不用写处理器，也不用等回答。
- `staticOnly: true` = 没命中就直接 404（纯文件服务都该开）；不开时未命中的请求照旧发
  `http.request`，同一个服务既能挂文件也能自己回答接口。
- 安全边界：**只监听回环**；自动生成的路径里只有 128 bit 随机 token，服务端按路径查表、
  不解析路径（拿到 URL 也读不到路由以外的文件）；每个服务路由 ≤ 1024、服务 ≤ 8、连接 ≤ 64。
- 生命周期跟着插件实例：停用 / 卸载 / 关『拟声++』/ 插件框架停机时服务与路由一起失效（端口释放）。
- 这是**代理**的一般机制：`musicxx.net.fetch/download` 取到的数据、FTP / 私有协议、HLS 分片、
  转码产物都可以先转成 HTTP 再给播放器，播放器只看到干净的本地 URL。写法见 `docs/plugin-api.md`
  §3.2.6 与 `docs/plugin-js-api.md` §7.3.1；完整例子：`plugins/example_override_media_source`、
  `plugins/example_js_net`（`selftest` 里的 `static` 一段）。

### 新增：示例插件 `example_override_media_source`

- 演示"插件参与音源解析"的三条路（各自有开关）：覆盖内置 Bili 解析（BV 号 → 直链 + 请求头）、
  插件音源类型 `plugin_bili`（类型 + 资源 id，`src` 里写 BV 号）、本地文件经插件 HTTP 服务的静态路由播放（`plugin_proxy_local`）。
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
