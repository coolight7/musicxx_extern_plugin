# musicxx 的对外接口：链接 / 启动参数 / MCP / 命令行 / 本机 HTTP

> 这篇是 **musicxx（主程序）对外暴露的全部接口**的总表：谁在什么场景用哪条通道、怎么连上、
> 请求与响应是什么形状、权限边界在哪、出问题从哪里看。
>
> 先说结论：**七条通道共用同一份实现（命令注册表）和同一套参数名**，一次调用只有一种形状
> `{"cmd": "<入口 id>", "args": {…}}`。链接（applink）与进程启动参数只是"把这次调用说出来"
> 的两种额外写法，见 §1。
>
> 插件自己的接口（能力 / 广播 / 功能埋点 / 界面）在 [plugin-api.md](plugin-api.md)；
> 用命令行装插件、改完刷新、看插件日志在 [plugin-agent-cli.md](plugin-agent-cli.md)。

**术语**：**主程序**指 musicxx（Flutter/Dart 业务层）；**插件框架**指外置插件的 C++ 层。

## 1. 七条通道，一份实现

**一次外部调用在所有通道里都是同一个形状**：

```json
{"cmd": "<入口 id>", "args": {…}}
```

结果也只有一个形状：`{ok, cmd?, data? | error?, message?}`。成功给 `data`；失败时
**`error` 是名称**（`not_found` / `invalid_argument` / `not_allowed` / `no_impl` / `busy` /
`failed` / `not_ready` / `permission_denied`，调用方按它分支），**`message` 是可读句子**
（给人看：哪一步、怎么办）。数字返回码只存在于插件框架的 C / FFI 边界（码表见
[plugin-api.md](plugin-api.md) §11.4）。入口 id 就是命令 id，实现在命令注册表里**只有一份**
（主程序 `lib/manager/cli/` 与 `lib/manager/entry/`）；每条通道只是"怎么把这次调用说出来"
不同 —— 详见 §1.1 与 §1.2、§1.3。

| 通道 | 谁在用 | 入口 | 说明 |
|---|---|---|---|
| **MCP 工具** | AI 客户端（Claude Code / Desktop / Cursor / 自研 agent） | `http://127.0.0.1:<端口>/mcp`（Streamable HTTP），或 `musicxx-cli mcp`（stdio 桥） | §5 |
| **本机命令行** | 人、脚本、agent | `musicxx-cli`（独立可执行文件）→ 主程序 `/cli` | §4 |
| **本机链接（applink / deep link）** | 手机点链接、桌面点 `musicxx://…`、其他应用跳过来；也可以由用户在应用里自己粘贴 | `musicxx://…` 交给系统 → 应用自己解析 | §1.1、§1.1.1 |
| **进程启动参数** | 文件关联、"用拟声打开"、命令行传参、第二个实例转发 | `musicxx <参数…>` | §1.2 |
| **插件内部** | 装在应用里的动态库插件 / JS插件 | 能力 / 广播 / 功能埋点 | [plugin-api.md](plugin-api.md) |
| **局域网设备之间** | 另一台装了 musicxx / 流明的设备（控制页、一起听、资源代理） | `/control`、`/connectEvent`、`/getSrc`、`/parseSrc`、`/listenTogether`、`/ws/*` | §6 |
| **设备发现** | 同网段的设备互相发现 | mDNS 服务 `_boolxx-service._udp` + HTTP `/active` | §6.1 |

**同一件事在不同通道上的名字**（全部落到同一份实现：命令注册表）：

| 要做的事 | MCP | 命令行 | 插件能力 | 链接（applink） | 启动参数 |
|---|---|---|---|---|---|
| 读当前媒体信息 | `GetMediaInfo` | `musicxx-cli status`（命令 `musicxx.player.state`） | —（读广播 `musicxx.state.player`） | `musicxx://call/?cmd=musicxx.player.state` | `--cmd musicxx.player.state` |
| 播放 / 暂停 / 播放暂停 | `SetPlaybackState`（`operator`） | `musicxx-cli play` / `pause` / `toggle` | `musicxx.player.play` / `pause` / `toggle` | `musicxx://musicxx.player.pause` | `--cmd musicxx.player.pause` |
| 切歌 | `SwitchTrack`（`operator`） | `musicxx-cli next` / `prev` | `musicxx.player.next` / `prev` | `musicxx://musicxx.player.next` | `--cmd musicxx.player.next` |
| 播放一个文件 / 链接 | `PlayMedia`（命令 `musicxx.player.playSrc`） | `musicxx-cli call musicxx.player.playSrc --args '{"src":"…"}'` | `musicxx.player.playSrc` | `musicxx://play/?src=…`（也认 `media` / `filename` / `url`） | 直接给文件路径，或 `--uri …` |
| 跳搜索页 | `OpenSearch`（命令 `musicxx.ui.openSearch`） | `musicxx-cli call musicxx.ui.openSearch --args '{"keyword":"…"}'` | —（插件用 `musicxx.ui.openRoute`） | `musicxx://search/?keyword=…` | `--cmd musicxx.ui.openSearch --args '{"keyword":"…"}'` |
| 副歌区段 | `AnalyseAudioChorus`（能力工具） | `musicxx-cli capability call musicxx.media.chorus.analyse` | —（这条只给模型与命令行：声明里没有 `plugin` 块） | — | — |
| 读前后台状态 | `GetAppLifecycle` | `musicxx-cli call musicxx.app.state` | 广播 `musicxx.app.lifecycle`（读当前值 + 订阅） | `musicxx://call/?cmd=musicxx.app.state` | `--cmd musicxx.app.state` |
| 切到前台 / 后台 | `SwitchAppForeground`（`operator`） | `musicxx-cli call musicxx.app.background` | `musicxx.app.background` / `musicxx.app.foreground` | `musicxx://musicxx.app.background` | `--cmd musicxx.app.background` |
| 改音量 / 速度 / 音调 / 进度 | `SetVolume` / `SetSpeed` / `SetPitch` / `SeekTo` | `musicxx-cli volume` / `speed` / `pitch` / `seek` | `musicxx.player.setVolume` / `setSpeed` / `setPitch` / `seek` | `musicxx://call/?cmd=musicxx.player.setVolume&volume=0.5` | `--cmd musicxx.player.setPitch --args '{"pitch":1.1}'` |
| 拦一次改播放值 / 收播放进度 | — | — | 能力 `musicxx.player.seek` / `volume` / `speed` / `pitch` + 广播频道 `musicxx.player.position` | — | — |
| 能力清单（谁在实现、能不能调、怎么调） | `ListCapabilities` | `musicxx-cli capability list` | —（插件在 SDK 的 `musicxx.capability` 里看） | `musicxx://call/?cmd=musicxx.capability.list` | `--cmd musicxx.capability.list` |
| 功能埋点清单 | `ListCapabilities`（同一个数据源） | `musicxx-cli call musicxx.feature.point.list` | `musicxx.feature.point.list` | `musicxx://call/?cmd=musicxx.feature.point.list` | `--cmd musicxx.feature.point.list` |
| 音源类型与解析链 | `ListMediaSources` | `musicxx-cli call musicxx.source.list` | `musicxx.source.list` | `musicxx://call/?cmd=musicxx.source.list` | `--cmd musicxx.source.list` |
| 插件框架版本 / 提交 / 仓库地址 | `ListPlugins`（结果里的 `framework` 段） | `musicxx-cli version` / `list` / `plugin list` | 插件读 `host.info().hostVersion`（同一个版本号） | — | — |
| 装插件 | `InstallPlugin`（`path` 或 `url`） | `musicxx-cli plugin install`（路径或 `--url`） | **没有这条能力**（见 §7） | — | `--cmd musicxx.plugins.install --args '{"path":"…"}'` |
| 局域网设备：扫描 / 列表 / 下发指令 | `ScanLanDevices` / `ListLanDevices` / `RunCommandOnDevice` | `musicxx-cli call musicxx.lan.scan` / `.clients` / `.cli` | —（插件不参与设备控制） | `musicxx://call/?cmd=musicxx.lan.clients` | `--cmd musicxx.lan.scan` |
| 按链接下载文件 | `DownloadFile` | `musicxx-cli call musicxx.net.file.download --args '{"url":…}'` | — | `musicxx://call/?cmd=musicxx.net.file.download&url=…`（会先问用户） | `--cmd musicxx.net.file.download --args '{"url":…}'` |
| 本机文件对外下载 | `ShareLocalFile` / `ListSharedFiles` / `UnshareLocalFile` | `musicxx-cli call musicxx.net.file.share.register` / `.list` / `.unregister` | — | `musicxx://call/?cmd=musicxx.net.file.share.register&path=…`（会先问用户） | `--cmd musicxx.net.file.share.list` |

> **写链接或启动参数之前，先问一次清单**：`musicxx-cli list --json`（或 GET `/cli/list`）里有
> `commands`（入口 id、参数声明）与 `entries`（哪些入口能用链接调进来、链接形态与取值候选），
> 还有 `framework`（本机插件框架的版本 / 提交 / 仓库地址）。
> 不要照着文档硬编码 —— 清单会随能力注册表增长。

### 1.1 applink（协议 `musicxx://`）

系统把整条链接交给应用，应用解析成一次"统一调用"（参数名与命令声明的完全一致；下面这些
参数都要 **UrlEncode**，习惯写法是带尾部斜杠的 `musicxx://play/?…`，不写也能识别）：

| 链接 | 归一成 | 说明 |
|---|---|---|
| `musicxx://play/?src=<原样的音源>` | `musicxx.player.playSrc {src}` | 首选写法：音源字符串原样交给 `playSrc`（类型由它自己判断） |
| `musicxx://play/?media=<拟声音源字符串>` | `musicxx.player.playSrc {src}` | 老写法：音源格式的字符串（歌曲-详情-音源里可长按复制） |
| `musicxx://play/?filename=<文件名\|路径>` | `musicxx.player.playSrc {src, type:"local"}` | 按本地文件/缓存里的文件名找（不同设备路径不同时省事） |
| `musicxx://play/?url=<链接>` | `musicxx.player.playSrc {src, type:"link"}` | `https://…`、`file:///…` 或直接路径 |
| `musicxx://play/?search=<关键词>` | `musicxx.history.play {info, tip:true}` | 在播放记录里模糊找一首并播放，没找到会弹提示 |
| `musicxx://search/?type=<类型>&keyword=<词>` | `musicxx.ui.openSearch {type, keyword}` | `type` 取值见 §4.3；两个参数都可省 |
| `bilibili://search/?keyword=<词>` | `musicxx.ui.openSearch {keyword, type:"bili"}` | 等同上一条的 `type=bili` |
| `musicxx://call/?cmd=<入口 id>&args=<JSON>` | 直接调用该入口 | 通用形态，`args` 省略时其余查询参数按参数名取值（值都是字符串） |
| `musicxx://call/?cmd=musicxx.player.setVolume&volume=0.5` | `musicxx.player.setVolume {volume:"0.5"}` | 查询参数名 = 命令参数名 |
| `musicxx://<入口 id>` | 直接调用该入口 | 例如 `musicxx://musicxx.player.toggle`（入口 id 大小写不敏感） |

- `musicxx://play/` 同时给多个参数时，按上表**从上到下**取第一个有值的（与旧版行为一致）；
  一个候选都没有 → 该链接解析失败（日志里有一行可读原因）；
- 不认识的链接（host 既不是 `play` / `search` / `call`，也不是入口 id）只记一行日志，不做事；
- **链接里的管理类入口要先问用户**：命令自己声明了 `consentReason` 的那些（`musicxx.plugins.*`
  装 / 卸 / 启用 / 重载 / 重启插件，`musicxx.net.file.*` 写文件、把本机文件开放出去）不直接拒、
  也不静默执行 —— 执行前弹窗列出命令与关键参数（`path` / `pid`…），点「允许」才执行，关掉窗口算
  拒绝（见 §7）。判据是**命令自己的声明**（`MusicxxCliCommand_c.consentReason`），不是 id 前缀：
  新加的管理类命令必须自己写上，否则链接能静默执行。本机通道（命令行 / MCP / `/cli`）不问；
  应用内粘贴的链接与 applink 同一条规则（见 §1.1.1）；
- **系统能把链接递进来的只有 Windows 与 Android**（`GlobalStore.isSupportAppLink()`）：桌面端由应用在
  注册表里注册 `musicxx` 协议（Windows 是 `HKCU\Software\Classes\musicxx`，打开方式为 `"<程序>" "%1"`），
  点链接即以这条链接为启动参数拉起应用；Android 的 Deep Link intent filter 声明 `scheme=musicxx`
  （**host 不限**）与 `bilibili://search/`，所以新增入口形态不用改 manifest。manifest 里另有一条
  `https://musicxx.bool.run/…` 的 App Link 声明，那是站点跳转用的：应用收到它时按"不认识的链接"
  处理（只记一行日志，不做任何事）；
- macOS / iOS / Linux 收不到系统链接，用 §1.1.1 的应用内粘贴入口，或命令行 `musicxx-cli open`。
- 在桌面端不用真的点链接也能验证：`musicxx-cli open "musicxx://play/?url=…"`，
  或 POST `/cli` 的 `{"uri": "…"}`（同一份解析）。

### 1.1.1 应用内手动执行链接（设置 → 界面 → 调用快捷链接）

系统递过来的链接只有 Windows / Android 有，但**链接不必真的被“点开”才生效**：应用里就能直接执行
一条链接。设置 → 界面 →「调用快捷链接」→ 把链接粘进去 → 点「确定」。

- 解析与执行与系统点开链接**完全同一份**（`MusicxxEntries_c.handleUri`），支持的写法就是 §1.1 表里的
  全部（`musicxx://play/`、`musicxx://search/`、`musicxx://call/?cmd=…`、`musicxx://<入口 id>`、
  `bilibili://search/`）；通道记成 `manual`（控制台输出 `[入口][manual] <入口 id> args=…`）；
- 执行完界面顶部给一条提示：成功是「已执行：<入口 id>」，失败是原因（缺参数、命令报错、不认识链接）。
  失败还会往应用日志里写一行「调用快捷链接失败 + 链接 + 原因」（`musicxx.log.app` / 设置里的日志页能看到）；
- **管理类入口（`musicxx.plugins.*` / `musicxx.net.file.*`，判据是命令声明的 `consentReason`）与 applink
  同一条规则**：执行前再弹一次确认，列出命令与 `path` / `pid` 等参数，点「允许」才执行，关掉窗口算拒绝；
- 不需要 `/cli`、MCP、协议注册或 `musicxx-cli`：macOS / iOS / Linux 这些收不到 applink 的平台上，
  它就是唯一的手动执行入口。

**普遍用途**（这是它存在的理由，不只是调试工具）：

| 用途 | 怎么做 |
|---|---|
| 远程协助 | 让用户粘一条链接就完成一次操作：跳页面、搜索、播放、切主题、装插件。不用教对方开命令行 / 开 MCP / 装 CLI，也不用发可执行文件 |
| 装 / 更新插件 | 插件包先放到对方机器上，再给 `musicxx://call/?cmd=musicxx.plugins.install&args=<UrlEncode 后的 {"path":"C:/…/x.zip"}>`（**要对方点「允许」**，见下；新插件装完默认启用，要装成停用状态就写 `"enable":false`）；插件包在公网上时直接用 `url`：`{"url":"https://…/x.zip","sha256":"…"}`；启用 / 停用 / 卸载 / 重载同理（`musicxx.plugins.setEnabled` / `uninstall` / `reload`） |
| 排查“链接不生效” | 参数改完粘一次就能看到解析结果与报错，不用注册协议、不用退到桌面点链接 |
| 自测 / 演示 | 插件开发者、写文档的人验证自己给出的链接示例是不是还能用 |

注意：

- **粘贴即执行**：这条通道唯一的确认就是用户自己按下的「确定」，所以只粘信任来源的链接；
  插件管理类会再问一次，但那句话同样可能被人教着点「允许」；
- 链接里引用的文件必须**在那台机器上存在**（`musicxx.plugins.install` 的 `path` 是绝对路径），
  或者用 `url` 让应用自己下载（`musicxx.net.file.download` 同一条路径，装完删临时包）；
- **`musicxx.net.file.*` 与 `musicxx.plugins.*` 一样要先问用户**：前者能往任意路径写文件、也能把本机
  文件开放给外部下载；后者装插件等于执行代码。
- 只有界面提示，没有返回值给调用方 —— agent 要结构化结果时用命令行（§4）或 MCP（§5），
  这条适合“用户自己动手”的那一侧。

### 1.2 进程启动参数

启动参数（文件关联、"用拟声打开"、终端里直接传、第二个实例转发）也会变成统一调用：

| 写法 | 归一成 |
|---|---|
| `--call '<JSON>'` | JSON 里的 `cmd` + `args` |
| `--cmd <入口 id> [--args '<JSON>']` | 该入口 + 参数 |
| `--uri <musicxx://…>` | 按 §1.1 解析 |
| 裸参数：文件路径 / `content://` / 链接 / `musicxx://` 链接 | 链接按 §1.1 解析；其余文件一次交给 `musicxx.player.playSrc {sources:[…], filterExt:true}`（本地文件按扫描配置过滤后缀，一个文件追加到当前列表并播，多个列成临时歌单） |

- 可以混着写：`musicxx musicxx://play/?url=a.mp3 b.mp3` 会先播 `a.mp3`，再把 `b.mp3` 加进来；
- 命令行里若带上了程序自身路径（文件关联常见），会被丢掉（判据与单实例检查同一套）；
- 认不出来的选项（`--x`）与写错的 JSON 只记日志，不打断后面的参数；
- 参数里的 `--call` 与 `/cli`、MCP 的请求体是**同一个 JSON**，可以直接搬。

### 1.3 参数名与别名

**命令声明的参数名就是唯一的名字**，各通道都用它：

| 概念 | 参数名 | 备注 |
|---|---|---|
| 音源 | `src`（单个）/ `sources`（数组）/ `type`（`auto`/`local`/`link`）/ `name` | 链接上的 `media` / `filename` / `url` 是 `src` 的历史写法（见 §1.1） |
| 进度 | `positionMs`（毫秒）/ `percent`（0~1） | `musicxx.player.seek` 也接受 `position`（历史写法） |
| 音量 / 速度 / 音质 / 循环 | `volume`（0~1）/ `speed` / `quality` / `mode` | —— |
| 歌单 / 歌曲 | `pid` / `type`（local/net）/ `index` / `songName` / `sid` | —— |
| 播记录匹配 | `info` / `tip`（没找到是否弹界面提示） | —— |
| 搜索页 | `type` / `keyword` | —— |
| 日志 | `file` / `offset` / `level` / `keyword` / `exclude` / `limit` | 应用日志与插件日志同一套 |

- 参数字段与 MCP 的 inputSchema 是同一份（`MusicxxFeatureParam_c`：`string` / `number` /
  `integer` / `boolean` / `enum` / `array`）；
- **数字与布尔接受文本写法**（`"0.5"`、`"600"`、`"true"`），因为链接与 HTTP 查询串里都是字符串；
- 没有声明参数的入口按"宽松解析"处理（数字与字符串可互转），参数不对时的答复里带可读原因。

### 1.4 命名规则（加新名字之前先看这一节）

**R1 命名空间与分隔符**

- **所有对外 id 一律 `musicxx.` 开头**（能力、广播、命令、页面、候选 / 模式 id）；
- 插件自定义一律 `plugin.<插件 id>.<名字>`，两者不会混淆；
- 分隔符只用来分点：id 里不放冒号与中划线（`musicxx:xxx` 已全部改成 `musicxx.xxx`）；
- 段内允许小写字母、数字、下划线、lowerCamelCase；同一个 id 里不要混用两种分词风格；
- 段数：能力 / 广播 / 命令都 ≤ 3 段（不含 `musicxx.`），确有必要超出的在声明里写一句理由。

**R2 域（这张表以外的域要先讨论）**

`app` `player` `playlist` `songlist` `song` `lyric` `media` `source` `ui` `theme` `storage` `render`
`plugins` `net` `lan` `log` `capability` `broadcast` `stats` `host` `entry` `timer` `history`

- **`songlist` = 歌单（保存下来的列表）**、**`playlist` = 播放队列（正在播的那一串歌）**；
- `plugins` = 外部插件的装 / 卸 / 启停 / 日志；单数 `plugin.` 只属于插件自己的命名空间；
- `net` = 网络与文件（本机文件对外下载在 `musicxx.net.file.*`）；
- `song` = 当前这一首歌（广播 `musicxx.song.changed`）；
- **`state` 不是命令域，是广播快照的部分**：`musicxx.state.<对象>` 的对象固定为
  `app` / `player` / `song` / `playlist` / `songlists` / `lyric` / `env` / `renderSlots` / `spectrum`。
  它和命令的 `.state` 后缀是一个意思的两种形态：`musicxx.player.state` 是"读一次"，`musicxx.state.player` 是
  "变了就发、带当前值"。

**R3 动词（表外的动词要在声明里写理由）**

| 类别 | 动词 | 说明 |
|---|---|---|
| 读单个 | `get` | 有对应写操作时用它（`musicxx.storage.get` / `.set`） |
| 读集合 | `list`（集合）/ `listSongs`（集合里的歌） | `musicxx.source.list`、`musicxx.songlist.list` / `.listSongs` |
| 快照 | `state` | 只读的复合状态（`musicxx.player.state`、`musicxx.app.state`） |
| 设值 | `set` | 设一个标量或枚举（`musicxx.player.setVolume`） |
| 改对象 | `update` | 改一条已有记录（`musicxx.songlist.updateSong`） |
| 执行 | `play` `open` `close` `show` `select` `search` `analyse` `exec` | 语义动词，不硬套 `set` |
| 声明 | `define` / `undefine` | 声明"我自己的东西"（音源类型、能力） |
| 登记 | `register` / `unregister` | 登记"已存在的实现"（本机文件共享） |
| 删除 | `remove` / `delete` / `uninstall` | 从集合拿掉 / 删一条数据 / 卸载包 |
| 通知 | `toast` / `publish` | 界面提示 / 广播发布 |

**R4 各面怎么写**

| 面 | 规则 |
|---|---|
| 命令 / 能力 / 广播 / UI 类型 / 候选 id | `musicxx.` + 点分，段内 lowerCamelCase 或下划线 |
| MCP 工具 | PascalCase，`<Verb><Object>`；协议不接受点号，工具名写在能力的 `mcp.name` 里 |
| `musicxx-cli` 糖命令 | 小写 kebab；这是**前端名字**，不是 id |
| HTTP 路径 | 小写单词或 kebab；存量 camelCase 路径不改（跨设备老版本还在用） |
| `/control` 名 | PascalCase，只是数字 id 的说明，代码里冻结 |

**R5 一个词一件事**

对外只有"**能力**"与"**广播**"两个概念。能力 = 发一次请求、等一个结果；广播 = 属主改了东西、通知关心的人。
"钩子 / 动作 / 状态镜像 / 变量 / 能力导出"这些旧叫法都已退役；`feature` / `export` 两个词从对外 id 与
JS 对象里消失（`musicxx.feature.point.*` 是"一个 id 可以有多个实现"的那组**能力**，仍是一条能力）。

**R6 地址空间互不干扰**

官方是 `musicxx.*`，插件是 `plugin.<插件 id>.*`。页面 id 与候选 / 模式 id 也在官方命名空间里，但
**不与能力 / 广播共用地址空间**（同一个字符串可以既是候选 id 又是能力 id，按用途各查各的表）；
广播与命令也不共用：`musicxx.player.state` 是命令，`musicxx.state.player` 是广播。

## 2. 先决条件与连接

| 条件 | 说明 |
|---|---|
| 主程序在运行 | MCP / 命令行 / 局域网这几条都由主程序的 HTTP 服务提供（applink 与启动参数本身就能把应用拉起来）。没在运行时命令行退出码 2（加 `--start` 可以先拉起） |
| HTTP 服务已绑定 | 主程序启动时就绑定（`InternetAddress.anyIPv4`，端口按设置，默认随机）：启动日志里有一行 `[HttpServer] 已启动: http://127.0.0.1:<端口>/` |
| `/cli` 所有平台都有 | 手机与桌面是同一份命令表（以前 `/cli` 只在桌面端开放）；`/mcp` 与局域网那几条在服务起来后都能用。安卓上也能用链接（§1.1） |
| 应用已走完启动 | 命令行 / MCP / HTTP 与"应用跑起来之后"进来的链接、启动参数都要等应用就绪：入口调用会先等（默认最多 15 秒），等不到返回 `not_ready`（HTTP 503）。**唯一不等的是启动流程自己**（首次链接、这次启动带的参数）：它们发生在"就绪"置位之前，与旧行为一致立即执行 |
| 命令行文件 | `musicxx-cli` 与主程序**编译到同一目录**（Windows `musicxx-cli.exe`；Linux 是 bundle 根目录；macOS 在 `Musicxx.app/Contents/MacOS/musicxx-cli`）。找不到时看应用里「设置 → 共享与控制 → 命令行接口（CLI）」，那里写着当前路径与可复制的命令 |

### 2.1 接入点文件（端口从哪来）

独立进程不知道端口，所以主程序在 HTTP 服务绑定成功后写一个文件：

```
{系统临时目录}/musicxx.cli.json
```

| 字段 | 含义 |
|---|---|
| `protocol` | 协议版本（当前 `1`；命令行与主程序对不上时给"版本不匹配"，退出码 5） |
| `port` | 本机 HTTP 服务端口（命令行 / MCP / 局域网共用同一个） |
| `pid` | 主进程号（命令行靠 `/cli/ping` 核对"文件里记的是不是现在这个进程"） |
| `appVersion` | 应用版本 |
| `serverVersion` | 局域网服务版本（见 §6.5） |
| `installId` | 安装 ID（同一台机器多份安装时用于区分） |
| `startedAt` | 写入时间（毫秒时间戳） |

- **文件里没有凭据**：本机命令行不加 token（判定见 §2.2）；
- **退出不清理**：残留文件无害，一律以 `/cli/ping` 的 `pid` 为准；
- 原子写（先写 `.tmp` 再改名），不会读到写了一半的内容。

### 2.2 权限边界

**对外接口只过一道鉴权**（主程序 `lib/manager/netAuth/MusicxxNetAuth.dart`），判定顺序：

```text
本机（回环 + 本机自己的网卡地址；链路本地与组播不算） → 局域网服务没开 → 档位「公开」 → 授权密令 → sign 同账号 → 弹窗
```

- 档位只有两档：「询问」（默认，同账号与本机直通、其它来源弹窗）与「公开」（不看身份）；
- 授权密令是**全权限、不过期**的一把钥匙（相当于用户本人，不设数量上限），四种写法等价：
  `Authorization: Bearer <令牌>`、`X-Musicxx-Token: <令牌>`、`Token: <令牌>`、`?token=<令牌>`；
  应用里存的**就是密令明文**（列表标题就是它，点一下复制）。

| 入口 | 本机回环 | 非本机 |
|---|---|---|
| `/cli`（含 `/cli/list`、`/cli/ping`） | 直接可用，不需要任何配置 | 统一鉴权：同账号（`?sign=`）直通；不同账号要令牌或由用户点一次「允许」；没有身份时 403 |
| `/mcp` | 可用 | 同上（`OPTIONS` 预检不判定）；拒绝时是 JSON-RPC `error`（`code: -32001`） |
| `/control` | 放行 | 同上（可弹窗） |
| `/connectEvent`（事件流 WebSocket） | 放行 | 同一道门，但**握手不弹窗**：没有身份直接 403；先 `GET /auth/token` 拿令牌再 `?token=` 连 |
| `/getSrc`、`/parseSrc` | 放行 | 同一道门 + 「局域网服务」开着、且服务模式允许对外服务 |
| `/listenTogether`、`/ws/listenTogether` | — | **不进鉴权门**：邀请不看档位，跨账号邀请由被邀请方的确认弹窗把关；`/ws/listenTogether` 只看 `groupId` / `clientId`（再多一层会让用户点两次） |
| `/auth/token` | 发一把令牌 | 同一道门（可以弹窗），公开档直接告诉你“不需要令牌” |
| `/help` | 放行 | 放行（`/cli` 与 `/mcp` 的纯文本说明，不需要令牌） |
| `/active` | 放行 | 放行（未开局域网服务时只回"存活"，不吐设备信息） |
| `/share/<id>` | 放行 | 链接就是权限（不进鉴权门），但仍要求「局域网服务」开着 |

拿到令牌的两种方便写法：在应用里「共享与控制 → 授权密令」手动加，或者本机跑
`musicxx-cli auth token --host <ip> --port <端口>`（只打印，不落盘）。通过弹窗授权的那一次请求，
响应头会带 `X-Musicxx-Token`，客户端存下来复用即可。

**先看说明**：`GET /help` 不需要任何身份，返回 `text/plain` 的接口说明（地址、密令写法、
`/cli` 的三种请求体与答复形状、`/mcp` 的四条方法与会话头、其它接口清单），
示例里写的就是你当前访问的端口；手写客户端时先请求它一遍即可。

## 3. `/cli` 的协议形状

```
GET  /cli   |  /cli/list    自描述：命令、参数、来源、统一入口表、就绪状态
GET  /cli/ping              存活 + 是否就绪 + 端点信息
POST /cli                   {"cmd": "<入口 id>", "args": {…}}         ← 与命令行、MCP、插件能力同形
                          | {"uri": "musicxx://play/?url=…"}          ← 与链接（applink）同一条解析
                          | {"argv": ["--call", "…", "a.mp3"]}        ← 与进程启动参数同一条解析
```

**三种请求体归一成同一次调用**（`{"cmd", "args"}`），解析与执行都在主程序
`lib/manager/entry/`：`uri` 与 `argv` 只是"把别的通道的形态搬到 HTTP 上"，方便脚本与排障
（不用真的点一次链接）。一次请求只执行一条调用：`argv` 解析出多条时给 400，请分几次请求。

响应体只有一种形状：`{ok, cmd?, data? | error?, message?}`。失败时 `error` 是名称、`message` 是句子
（见 §1）。状态码只表达协议 / 权限 / 时机，**命令本身的失败仍是 200 + `ok:false`**：

| HTTP 状态码 | `error` | 什么时候出现 |
|---|---|---|
| 200 | —— / 命令自己的名称 | 请求合理。命令失败也是 200 + `ok:false`，调用方看 `ok` 与 `error` |
| 400 | `invalid_argument` | 请求体不是 JSON 对象 / 缺 `cmd` / `args` 不是对象 / `uri` 认不出来 / `argv` 解析出多条 |
| 403 | `permission_denied` | 非本机且没过鉴权门。**具体是哪一步**在 `data.deny`：`service_disabled` / `need_token` / `ask_denied` / `ask_timeout` / `ask_busy` / `ask_unavailable`（见 §7） |
| 404 | `not_found` | 路径不认识 |
| 405 | `method_not_allowed` | 方法不对（只接受 GET / POST） |
| 503 | `not_ready` | 应用还在启动收尾（默认先等 15 秒才这么答） |

`GET /cli/list` 的 `data` 里，除了 `commands`（入口 id、参数、来源）与 `entries`（哪些入口能用
链接调进来、链接形态与取值候选），还有 `entryArgv`（启动参数形态的说明，逐行）与 `framework`
（外置插件框架的版本，见下）。

`/cli/list` 与 `/cli/ping` 的 `data` 都有 `framework` 段 —— 外置插件框架（`musicxx_extern_plugin`）
自己的版本，就是本机 `musicxx-cli version` / `doctor` / `list` / `plugin list` 打印的那个：

| 字段 | 含义 |
|---|---|
| `version` | 版本号（`<年>.<月日>.<小版本号>`，例 `2026.1031.1`） |
| `commit` | 这个版本号首次出现的提交（短号；空串 = 还没补记） |
| `date` | 该版本的日期（`YYYY-MM-DD`） |
| `describe` | 一行说明：`2026.1031.1（2026-10-31 · commit 8314706）` |
| `repositoryUrl` | 插件框架的 GitHub 仓库地址（写插件时对着它看能力与文档） |
| `commitUrl` | 该版本提交的地址（没补记提交号时与 `repositoryUrl` 相同） |

- 这是**随应用一起编译进来的那份声明**（插件包 `lib/src/version.dart`）；
  插件框架库自己自报的版本要插件跑起来才读得到，在 `musicxx.plugins.list` 的
  `framework.libraryVersion`（两者不一致 = 库不是这次构建的产物）；
- 旧版主程序不给这一段：命令行只是少打一行，不影响其它命令。

- 路径对大小写与末尾斜杠容错（`/CLI/` 也认）；
- 不经命令行直连（排查时最省事）—— 先把 `17001` 换成接入点文件里的 `port`：

```bash
curl http://127.0.0.1:17001/cli/ping
curl -X POST http://127.0.0.1:17001/cli -H "content-type: application/json" \
     -d '{"cmd":"musicxx.player.setVolume","args":{"volume":0.5}}'
```

```powershell
Invoke-RestMethod -Uri 'http://127.0.0.1:17001/cli/list'
Invoke-RestMethod -Method Post -Uri 'http://127.0.0.1:17001/cli' `
  -ContentType 'application/json' -Body '{"cmd":"musicxx.player.state"}'
```

## 4. 命令面（当前 93 条 = 应用自己的命令 30 + 能力命令 63）

> **条数会随注册表增长，一律以 `musicxx-cli list`（GET `/cli/list` 的 `commandCount`）为准**，
> 下面各节的数字只是写这份文档时的快照。
>
> 能力命令 = 声明表里带 `cli` 块的能力（63 条，其中 `musicxx.net.fetch` / `download` 两条由插件框架
> 直接回答、不进命令表）；应用自己的命令 30 条。只有插件能调的（21 条，声明里只有 `plugin` 块）与
> 只给模型用的（3 条，只有 `mcp` 块）不在这个数里。

### 4.1 命令从哪来

| 来源（`kind`） | 条数 | 谁登记的 | 内容 |
|---|---|---|---|
| `builtin` | 30 | `MusicxxAppCommands` + `MusicxxCliCommandsSetup` + `MusicxxEntryCommands` + `MusicxxEntries` + `MusicxxLanCommands` + `MusicxxFileCommands` | 播放状态、歌词文本、歌单播放与同步、播放记录、定时停止、前后台状态、系统命令登记、外部插件管理、日志、能力自省（`musicxx.capability.*`）、音源清单、局域网（`musicxx.lan.*`）、静态文件（`musicxx.net.file.*`）、统一入口的播放音源 / 跳搜索页 / 执行启动参数（`musicxx.entry.argv`） |
| `action` | 63 | 应用能力表 | 与插件、MCP 共用同一份实现；**命令 id 就是能力名**；声明里没写 `cli` 块的能力不登记 |
| `broadcast` | 2 | 广播频道 | `musicxx.broadcast.list` / `musicxx.broadcast.read`（写状态调能力） |

命令表**自描述**：`musicxx-cli list --json`（或 GET `/cli/list`）里 `commands[]` 每项是
`{id, title, kind, description?, params: []}`，另有 `entries[]`（链接形态）。写脚本时先问一次，
不要硬编码参数表（条数会随能力注册表增长）。

### 4.2 参数怎么解析

声明了 `params` 的命令按声明解析，声明字段与 MCP 的 inputSchema 是**同一份**
（`MusicxxFeatureParam_c`）：

| 字段 | 含义 |
|---|---|
| `name` / `type` | 参数名；类型 `string` / `number` / `integer` / `boolean` / `enum` / `array` |
| `required` | 必填（没有默认值的参数通常是必填） |
| `defaultValue` | 缺省值（不传就用它） |
| `minimum` / `maximum` | 数值范围 |
| `options` | `enum` 的候选值 |
| `itemType` | `array` 的元素类型（目前只有 `string`） |

数字与布尔类型**也接受文本写法**（`"600"`、`"0.5"`、`"true"`）：链接与 HTTP 查询串里拿到的
都是字符串，各通道就不用各写一遍转换。没有声明 `params` 的命令（能力那一批）把入参**原样交给
能力**，能力自己做宽松解析（数字与字符串可互转）。参数不对时答复形如
`{"ok":false,"error":"invalid_argument","message":"参数 xxx 非法: …"}`：**`error` 是名称、`message`
是句子**（见 §1）。

### 4.3 应用自己的命令（30 条）

| 命令 | 参数 | `data` |
|---|---|---|
| `musicxx.player.state` | 无 | 当前媒体的结构化快照（标题 / 歌手 / 专辑 / 总时长 / 进度 / 状态 / 循环 / 速度 / 应用内音量 / 文件名 / 格式 / 采样率 / 码率）；没有播放过时带 `hasMedia:false` |
| `musicxx.lyric.current.lrc` | `word?`(bool) | `{hasLyric, word, lineCount, text?}`：当前歌词的 `.lrc` 文本，`word:true` 尽量给逐字（增强型） |
| `musicxx.songlist.play` | `pid`(int,必填)、`type?`(local/net)、`index?`(int)、`songName?`(string) | `{pid, sid, name, artist}`：播歌单里的一首（按 `index` 或名字包含匹配找，都不给播第一首） |
| `musicxx.songlist.sync` | `pid`(int,必填) | `{ok, pid, name, total, added, cancelled, failedSources, elapsedMs, sources:[…]}`：同步歌单绑定的音源（本地文件夹 / 歌手 / 专辑、Bili 收藏夹、WebDAV、云盘、媒体库…），把新增的歌加进歌单。`sources` 每项 `{index, id, type, name, loaded, added, error?}`。只有一部分音源失败时仍 `ok:true`（`failedSources` 是失败条数）；绑了哪些音源只能在应用界面里配，这条命令不收 `sources` |
| `musicxx.history.play` | `info`(string,必填)、`tip?`(bool) | `{found, sid?, name?, artist?}`：在播放记录里按标题 / 歌手匹配最相关的一首并播放；`tip:true`（applink `musicxx://play/?search=` 用它）时没找到会弹界面提示 |
| `musicxx.ui.openSearch` | `type?`(枚举，见下方类型表)、`keyword?`(string) | `{route, type, keyword}`：打开搜索页并带上类型与关键词。**applink `musicxx://search/`、`bilibili://search/` 背后的入口** |
| `musicxx.entry.argv` | `argv`(array，必填：与命令行里写的一样、不含程序自身路径) | `{count, cmds:[…]}`：把一串启动参数按 §1.1 的规则解析成一次或多次命令调用并执行（Windows 上把第二实例的命令行交给已有实例用的就是它）；其中一条失败就整串拒绝；不能嵌套它自己 |
| `musicxx.timer.set` | `duration?`(秒 1~86400)、`enableStopAfterFinish?`、`enableSleepMode?`、`exitWhenStop?` | `{started, durationSeconds, durationText, …}`；什么都不给时沿用用户上次的设置（没有则 60 分钟） |
| `musicxx.timer.cancel` | 无 | `{ok:true}`（没设置过也算成功） |
| `musicxx.app.state` | 无 | `{foreground, lifecycle, state, backgroundMs, lastBackgroundMs}`：`foreground` = 是否前台；`lifecycle` = `foreground` / `background`；`state` = 原始生命周期状态名；`backgroundMs` = 已经在后台多久；`lastBackgroundMs` = 上一次在后台待了多久。与广播 `musicxx.app.lifecycle` 读的是同一份状态 |
| `musicxx.app.commands` | `action?`(string：`install` / `uninstall` / `status`，缺省 `status`) | `{…, text}`：把 `musicxx` / `musicxx-cli` 登记成系统里能直接输入的命令，或者查 / 删这份登记；`locations` 是实际生效的位置，`problems` 是失败原因 |
| `musicxx.plugins.list` | 无 | 插件框架现状 + 插件清单（字段见本节末尾那张表） |
| `musicxx.plugins.install` | `path?`(本机绝对路径)、`url?`(下载链接)、`enable?`(bool，不给时新插件默认启用、覆盖安装沿用原状态)、`sha256?` | `{ok, id, version, kind, author?, description?, permissions, enabled, path, sha256, isUpgrade, target, targetEntry, targets, fileCount, totalBytes, source, download?, status}`：`path` 与 `url` 二选一；`url` 分支先下到 `<系统缓存根>/musicxx/temp/plugin` 再装，**装完（无论成败）删临时包**；`sha256` 给了就比对，不一致不装 |
| `musicxx.plugins.uninstall` | `id`(必填)、`keepData?`(bool) | 卸载结果 + 插件清单 |
| `musicxx.plugins.setEnabled` | `id`(必填)、`enabled`(必填) | `{id, enabled, loaded, …}` + 插件清单 |
| `musicxx.plugins.reload` | `id?`（不给 = 重扫目录并重载全部启用中的） | `{reloaded:[id…], failed:{id:原因}, …}` + 插件清单 |
| `musicxx.plugins.restart` | 无 | 重启后的状态（`hostStarted` / `pluginCount` / `loadedCount` / `restarted`） |
| `musicxx.log.plugin` | `id?`、`offset?`(≥0)、`level?`(数值 800/900/1000)、`keyword?`、`limit?`(1~500，默认 50) | `{ok:true, buffer, id, level, keyword, logs:[{ts, time, type, plugin, detail}…], total, offset, count, hasMore}`，**新的在前**；`total` 是过滤后的条数 |
| `musicxx.log.plugin.clear` | 无 | `{cleared}` |
| `musicxx.log.app` | `file?`(error/info/both)、`offset?`、`level?`(名字或数值)、`keyword?`、`exclude?`、`limit?`(1~500) | `{logs:[{timeText, level, file, source, text}…], total, hasMore, fileSize, parseErrors, flushedHint}`，**新的在前** |
| `musicxx.capability.list` | `id?`、`provided?`(bool) | `{ok:true, points:[…], broadcasts:[…]}`；每个能力一行：`id` / `title` / `type` / `available` / **`callable`（能不能被外置调）** / **`pluginCallable`（能不能被插件调）** / `callDoc` / `callStat` / `rows`（实现行）/ `stages`（前链与后链）/ `broadcasts`（应用内的频道订阅行） |
| `musicxx.capability.call` | `id`(必填)、`args?`(object)、`refresh?`(bool) | `{ok, id, value, by, identity, fromCache, ms}`；失败 `{ok:false, error, message}`：`error` 取 `not_found` / `not_callable` / `invalid_argument` / `not_allowed` / `no_impl` / `canceled` / `dropped` / `busy` / `failed`（`canceled` 与 `dropped` 是两种不同的"没有结果"：前者被拦下、后者被丢弃） |
| `musicxx.capability.subscribers` | `id`(必填)、`phase?`(before/impl/after) | 某条能力现在谁在实现、在哪个阶段、排第几 |
| `musicxx.source.list` | 无 | `{ok:true, sources:[{id, title, depict, srcDoc, plugin, declaredBy}…], browse:[…], resolveChain:[…]}`：插件声明的音源类型 + 按类型分组的可浏览分类 + 音源解析链现状（与 `musicxx.capability.list` 看 `musicxx.player.source.resolve` 是同一份） |
| `musicxx.lan.scan` | `reset?`(bool，缺省 true)、`wait?`(int ms，0~15000，缺省 3000) | `{ok, serverVersion, port, enabled, self, count, clients}`：刷新一次设备发现（重新声明本机服务 + 重新浏览 mDNS + 给已发现设备发心跳），最多等 `wait` 毫秒。需要「共享与控制」开着 |
| `musicxx.lan.clients` | `availableOnly?`(缺省 true)、`refresh?`、`limitServerVersion?` | 已发现设备列表：`clientId` / `displayName` / `addr` / `httpPort` / `uid` / `sameUser` / `version` / `serverType` / `acceptRemoteCtrl` / `canControl` / `available` / `mdnsLost` / `lastNotify` / `controlUrl` 等 |
| `musicxx.lan.cli` | `clientId?` / `addr?`（二选一）、`cmd?` + `args?`、`action?` / `id?` + `data?` | `{ok, clientId, addr, httpPort, action?, via, cmd?, cmdName?, remoteOk, data, error?, tip?}`：把一条指令发给指定设备。**默认转发到对方的 `POST /cli`**，`cmd` + `args` 可以给对方的任意命令 id；`action` 写法里能对上命令的也走 `/cli`，其余（systemVolume / tempSpeed / theme / appToggle / exit / song / playlist …）仍走老的 `/control` 数字 id |
| `musicxx.net.file.download` | `url`(必填)、`path?`、`dir?`、`fileName?`、`overwrite?`(缺省 false)、`sha256?`、`timeoutMs?`(缺省 120000) | `{ok, url, realUrl?, path, bytes, sha256, status, contentType, tookMs}`：按 http/https 链接下载到本机。先写 `<目标>.part`，成功且校验通过才改名；目标已存在且未给 `overwrite` 时失败 |
| `musicxx.net.file.share.register` | `path`(必填，本机绝对路径，文件或目录)、`id?`、`title?`、`download?`(bool)、`ttlSeconds?`(缺省 0 = 不过期) | `{id, route, path, title, isDir, download, size, modified, createdAt, expiresAt, ttlSeconds, hitCount, bytesServed, lastHitAt, localUrl, lanUrls}`：把一个本机文件或目录注册成 HTTP 静态文件服务，拿到 `/share/<id>` 与可直接用的链接。**route id 就是权限**（随机 32 位十六进制，可自定义，只能 `A-Za-z0-9_-`） |
| `musicxx.net.file.share.list` | `id?` | `{ok, count, shares:[...]}`：当前注册的共享 |
| `musicxx.net.file.share.unregister` | `id?` / `all?`(bool) | `{ok, removed}`：注销一个或全部共享，链接立刻失效 |

`musicxx.ui.openSearch` 的 `type` 取值（大小写不敏感，缺省保持当前搜索类型）：`localsong`（本地歌曲）、
`historysong`（播放记录）、`bili`（Bili）、`musiccloud`（音乐云盘）、`alidrive`（阿里云盘）、
`baidupan`（百度网盘）、`115drive`（115云盘）、`subsonic`、`jellyfin`、`emby`、`playlist`（歌单）。

插件清单的字段（`musicxx.plugins.*` 的结果里都有，安装结果里它在 `status` 下）：
`hostStarted`、`platformSupported`、`supportPlus`、`enabled`、`safeMode`、`autoSafeMode`、
`lastError`、`pluginDir`、`pluginCount`、`loadedCount`、`framework`（`version` / `commit` / `date` /
`describe` / `repositoryUrl` / `commitUrl` / `libraryVersion` / `libraryMatches`）、
`capabilityCall`（`async` 或 `hostTooOld`）、
`pendingCapabilityCalls`、`plugins[]`（`id` / `version` / `kind` / `enabled` / `loaded` /
`supported` / `reason` / `target` / `path` / `permissions` / `error`）。

### 4.4 广播（2 条）

| 命令 | 参数 | `data` |
|---|---|---|
| `musicxx.broadcast.list` | `prefix?`(string) | `{channels:[…]}`：频道、缓存属性（`cache` / `publish`）、属主、订阅数、当前值（官方频道来自 Dart 注册表，插件频道由插件框架补） |
| `musicxx.broadcast.read` | `id`(必填) | `{id, value}`；没有当前值时命令失败（`error` 为 `no_value`），频道不存在是 `not_found` |

广播是**只读**的：属主发布，别人只能读与订阅；要改应用状态请调对应的能力。

### 4.5 应用实现的能力（命令 id = 能力名，61 条）

参数与返回的完整说明在 [plugin-api.md](plugin-api.md) §3.2（本文不重复，避免两处不一致）。
这一节只给"有哪些、大致做什么"（下表是**能当命令调**的那些；只有插件能调、只给模型用的不在其中）：

| 域 | 能力 |
|---|---|
| 应用前后台（2） | `musicxx.app.foreground` / `background` |
| 播放控制（7） | `musicxx.player.play` / `pause` / `toggle` / `stop` / `next` / `prev` / `playSrc` |
| 播放参数（6） | `musicxx.player.seek` / `setVolume` / `setSpeed` / `setPitch` / `setQuality` / `setLoopMode` |
| 歌单（9） | `musicxx.songlist.list` / `listSongs` / `play` / `addSong` / `removeSong` / `create` / `updateSong` / `favorite` / `sync`（把歌单绑定的音源重新读一遍） |
| 播放列表（5） | `musicxx.playlist.listSongs` / `playSong` / `addSong` / `removeSong` / `updateSong`（读 / 改当前正在播的那一串歌） |
| 歌词（5） | `musicxx.lyric.current.get` / `current.update` / `current.updateContent` / `getBySrc` / `search`（读当前歌词；选拟声云歌词 + 改这首歌的偏移；用 LRC 文本临时设置当前歌词；按歌词源读内容；搜索云歌词） |
| 提示、弹层与页面（4） | `musicxx.ui.toast` / `dialog` / `openRoute` / `playing.openMenu`（打开播放页的「更多」菜单） |
| 全局字体（3） | `musicxx.ui.setFontFamily` / `getFontFamily` / `loadFont` |
| 界面开关（4） | `musicxx.ui.setAnimatedLevel` / `setParticleAnimate` / `setSongIconRotate` / `setSongIconWave` |
| 主题（3） | `musicxx.theme.setCurrent` / `setMode` / `setTempMode` |
| 渲染槽位（3） | `musicxx.render.list` / `current` / `select` |
| 封面、频谱与内核（5） | `musicxx.media.palette.get` / `cover` / `palette.set` / `spectrum` / `kernel`（播放内核信息：版本 / 能写进 `af` 与 `vf` 的过滤器 / 解码器 / 后缀） |
| 功能埋点（2） | `musicxx.feature.point.list` / `call` |
| 音源（1） | `musicxx.source.list` |
| 宿主（2） | `musicxx.host.openUrl` / `clipboard` |
| 网络（2，不进命令表） | `musicxx.net.fetch` / `download`：由插件框架收到就地回答，不再转给应用，所以命令行 / 链接调它们只会得到 `not_found` |

> 只有插件能调的能力（声明里只有 `plugin` 块）：`musicxx.storage.*`、`musicxx.stats.*`、
> `musicxx.host.getPath`、`musicxx.source.define` / `undefine` / `browse`、`musicxx.media.bindImage` /
> `unbindImage`、`musicxx.ui.drive.*`、`musicxx.ui.overlay.*`、`musicxx.feature.point.define` /
> `undefine`、`musicxx.capability.subscribers`。**它们不进命令表**（声明里没有 `cli` 块），
> 所以链接 / 命令行写它们的 id 得到 `not_found`；用 `musicxx.capability.call` 硬调会真的执行，
> 但**没有插件身份**（`musicxx.storage.*` 落到一个空插件目录下）。
>
> 只给模型用的 3 条（只有 `mcp` 块）：`musicxx.media.spectrum.extract`、`musicxx.media.chorus.analyse`、
> `musicxx.media.palette.analyse`——它们是 `GetAudioSpectrum` / `AnalyseAudioChorus` /
> `AnalyseImageMainColors` 三个工具背后的数据源。同样不在命令表里，**插件也调不到**：插件调这两类
> 能力会得到 `not_allowed`（声明里没有 `plugin` 块）；要算就用 `musicxx.media.spectrum` /
> `musicxx.media.palette.get`。

> 另外，插件框架自己还有一张网络接口表 `<应用名>.net`（HTTP / TCP / WebSocket / UDP 的客户端与服务端，
> 客户端支持 https / wss，不经过主程序），它是框架的**通用表**（`pluginxx/api/net.h`，本项目 IID
> 就是 `musicxx.net`，实现在 `cxx_utilxx` 的 `utilxx::PluginNetHub`），见 [plugin-api.md](plugin-api.md)
> §3.2.6「内置网络能力」。JS 的 `musicxx.net.fetch` / `download` 走这一套。
>
> 从命令行 / MCP 调这几条能力时**没有插件身份**：`musicxx.storage.*` 会落到"没有插件 id"的空目录下。
> 要操作某个插件的私有数据，请让插件自己读写，别从命令行代劳。

### 4.6 `musicxx-cli` 的糖命令（CLI 名 → 命令 id）

糖命令只做**参数整理与排版**，业务全在上面那张命令表里。完整的 CLI 说明（选项、退出码、
全部示例）在 `cli/README.md`：

| CLI | 命令 id |
|---|---|
| `status` | `musicxx.player.state` |
| `play` / `pause` / `toggle` / `stop` | `musicxx.player.play` / `pause` / `toggle` / `stop` |
| `next` / `prev` | `musicxx.player.next` / `prev` |
| `seek <毫秒\|mm:ss\|hh:mm:ss\|N%>` | `musicxx.player.seek` |
| `volume` / `speed` / `pitch` / `loop` / `quality` | `musicxx.player.setVolume` / `setSpeed` / `setPitch` / `setLoopMode` / `setQuality` |
| `lyric [--raw] [--word] [--limit N]` | `musicxx.lyric.current.get`（`--raw` 用 `musicxx.lyric.current.lrc` 的 `.lrc` 文本） |
| `playlist [--limit N] [--offset N]` | `musicxx.playlist.listSongs` |
| `playlist play <sid>` | `musicxx.playlist.playSong` |
| `songlists` | `musicxx.songlist.list` |
| `songlist songs <pid>` / `songlist play <pid>` / `songlist sync <pid>` | `musicxx.songlist.listSongs` / `musicxx.songlist.play` / `musicxx.songlist.sync` |
| `toast <文本>` | `musicxx.ui.toast` |
| `capability list` / `call` / `subscribers` | `musicxx.capability.list` / `.call` / `.subscribers` |
| `broadcast list [--prefix]` / `broadcast read <id>` | `musicxx.broadcast.list` / `.read` |
| `kernel [--refresh]` | `musicxx.media.kernel` |
| `render list` / `current` / `select <id>` | `musicxx.render.list` / `current` / `select` |
| `plugin list` / `install` / `enable` / `disable` / `uninstall` / `reload` / `restart` / `logs` | `musicxx.plugins.list` / `install` / `setEnabled` / `uninstall` / `reload` / `restart` / `musicxx.log.plugin`（`--clear` 用 `musicxx.log.plugin.clear`） |
| `logs [error\|info]` | `musicxx.log.app` |
| `commands [status\|install\|uninstall]` | `musicxx.app.commands` |
| `call <命令 id> [--args JSON]` | 任意命令（含全部能力名） |
| `open <musicxx://…>` | 不转发命令：按链接形态请求主进程（与手机点链接、启动参数 `--uri` 同一条解析） |
| `auth token [--host <ip> --port <端口>]` | 不转发命令：向对方要一把授权密令（只打印，不落盘） |
| `list` / `doctor` / `ping` / `watch` / `mcp` / `start` / `help` / `version` | 不转发命令：自描述、自检、事件流、stdio 桥、拉起应用 |

CLI 自己的选项与退出码：

| 选项 | 说明 |
|---|---|
| `--json` | stdout 只放那份 JSON（`{ok, cmd, data}` 或 `{ok:false, error, message}`，缩进多行）；提示与诊断走 stderr |
| `--endpoint <路径>` | 指定接入点文件（默认 `<系统临时目录>/musicxx.cli.json`） |
| `--port <端口>` | 直接指定端口（跳过接入点文件，排查用） |
| `--timeout <秒>` | 单次请求超时（默认 30） |
| `--host <ip>` | 连另一台设备（默认本机回环；跨设备要对方开着「共享与控制」） |
| `--token <密令>` | 带授权密令（非本机、不同账号时用；本机不需要） |
| `--start` / `--start-timeout <秒>` / `--app <路径>` | 没在运行时先拉起主程序（默认等 60 秒） |

| 退出码 | 含义 |
|---|---|
| 0 | 成功 |
| 1 | 参数错误（命令行自己发现的） |
| 2 | 应用没在运行（接入点文件过期 / 连不上） |
| 3 | 等主进程响应超时 |
| 4 | 命令执行失败（`ok:false` 或 HTTP 非 2xx，原因在 stderr） |
| 5 | 协议不匹配（主程序与命令行版本差太多） |

### 4.7 事件流（`musicxx-cli watch`）

事件流走 WebSocket `/connectEvent`（本机回环放行），连接建立时先推一批当前状态，
之后有变化才推。可看的事件名（`--events song,playerState` 只看这几类）：

`song`、`playerState`、`loadingMediaSrc`、`duration`、`current`、`songlist`、`lyric`、
`lyricSelectShowIndex`、`lyricSrc`、`isNightTheme`、`appVolume`、`systemVolume`、`speed`、
`playlistLoopMode`、`playlistInfo`、`chorus`（另有 `heartbeatCheck`，默认被过滤掉，用
`--heartbeat` 打开）。

选项：`--events`、`--count N`（收满就退）、`--for 秒`、`--heartbeat`、`--json`。
事件号与事件名的对应表在 §6.4 —— **两边必须对得上**，命令行侧硬编码了这张表。

## 5. MCP 工具面

### 5.1 怎么连

| 方式 | 配置 | 说明 |
|---|---|---|
| 直接 HTTP | 客户端支持 Streamable HTTP 时填 `http://127.0.0.1:<端口>/mcp` | 端口从接入点文件读；`POST` 对话、`GET` 拉事件、`DELETE` 结束 session，另有 `OPTIONS` 预检（带 CORS 头） |
| stdio 桥 | `musicxx-cli mcp` | 给只会启动 stdio MCP 服务器的客户端用；桥只做透明转发，**不解释、不改写**消息 |

```bash
claude mcp add musicxx -- "<安装目录>/musicxx-cli" mcp
```

```jsonc
// Claude Desktop / Cursor（Windows 路径里的反斜杠要写两次）
{
  "mcpServers": {
    "musicxx": {
      "command": "D:\\Program Files\\Musicxx\\musicxx-cli.exe",
      "args": ["mcp"]
    }
  }
}
```

- 服务自报：`serverInfo.name = musicxx-server`、`version = 应用版本`、
  `protocolVersion = 2024-11-05`，并带一段 `instructions`（告诉模型这是什么应用、中文名叫"拟声"）；
- **每个 session 一个 server 实例**（协议只允许一条 transport），`DELETE` 结束自己的 session；
- 桥的约定（不会退出、自动拉起应用、主进程重启后重发一次）见
  [plugin-agent-cli.md](plugin-agent-cli.md) §7；
- 不经过任何客户端直接验证：`cd cli && dart run tool/mcp_probe.dart`（跑一遍
  initialize → tools/list → tools/call，`--point` 换工具、`--args` 给参数）。

### 5.2 工具全表（44 个）

工具名、说明、inputSchema 与文本形态**对外就是契约**（模型看到的就是它们），
声明全在 `lib/manager/cli/MusicxxMcpTools.dart`。下面按用途分组，
"参数"列里带 `?` 的是可选；工具的参数说明（英文）由服务端随工具一起下发，客户端不需要另外查。
**参数名与命令声明的完全一致**：同一个入口在 MCP 上看到的就是命令行上写的那个名字。
工具名不是命令 id（`GetPlaylistSongs` ↔ `musicxx.playlist.listSongs`），"等价命令"列给出对应关系。

**读当前媒体、歌词与应用日志**

| 工具 | 参数 | 等价命令 | 文本形态 |
|---|---|---|---|
| `GetMediaInfo` | 无 | `musicxx.player.state` | 多行状态快照；没播放过时是 `Musicxx has not played any media yet` |
| `GetCurrentLrc` | `word?`(bool) | `musicxx.lyric.current.lrc` | 标准 `.lrc` 文本（`word:true` 尽量给逐字）；没有歌词时 `<unknown>` |
| `GetAppLogs` | `file?`(error/info/both)、`offset?`、`level?`(名字或数值)、`keyword?`、`exclude?`、`limit?`(1~500) | `musicxx.log.app` | 一行一条，带分页信息 |

**歌单、播放记录与搜索**

| 工具 | 参数 | 等价命令 | 文本形态 |
|---|---|---|---|
| `GetPlaylistSongs` | 无 | `musicxx.playlist.listSongs` | 歌曲列表（序号 / 名称 / 歌手 / 专辑 / 时长） |
| `ListSonglists` | 无 | `musicxx.songlist.list` | 先报条数；网络歌单（「我喜欢」排最前）再本地歌单 |
| `GetSonglistContent` | `pid`(必填)、`type`(local/net，必填) | `musicxx.songlist.listSongs` | 同 `GetPlaylistSongs` |
| `PlaySonglist` | `pid`(必填)、`type`(必填)、`songName?` | `musicxx.songlist.play` | `Success load songlist, current play song name: …` |
| `SyncSonglistSources` | `pid`(必填) | `musicxx.songlist.sync` | `Added: N song(s), now M song(s) in the playlist` + 每条音源一行（新增 / 共，读取失败的写原因） |
| `PlayFromHistory` | `info`(必填) | `musicxx.history.play` | `Success` / `No related songs found` |
| `PlayMedia` | `src?`／`sources?`（二选一）、`type?`、`filterExt?`(bool)、`name?` | `musicxx.player.playSrc` | `Playing: <名字> (<n> media)`；没有可播的来源时工具报错 |
| `OpenSearch` | `type?`(枚举)、`keyword?` | `musicxx.ui.openSearch` | `Search page opened [<类型>] keyword=<词>` |

**播放控制**

| 工具 | 参数 | 等价命令 | 文本形态 |
|---|---|---|---|
| `SetPlaybackState` | `operator?`(play/pause；不写 = 切换) | `musicxx.player.play` / `pause` / `toggle` | `[play\|pause\|toggle] Success` |
| `SwitchTrack` | `operator`(必填，next/previous) | `musicxx.player.next` / `prev` | `[next\|previous] Success` |
| `SeekTo` | `positionMs?` **或** `percent?`(0~1) | `musicxx.player.seek` | `Success`；两个都不给报参数不合法 |
| `SetSpeed` | `speed`(必填，0.25~8) | `musicxx.player.setSpeed` | `Success` |
| `SetPitch` | `pitch`(必填，0.25~2；1 = 原调) | `musicxx.player.setPitch` | `Success` |
| `SetVolume` | `volume`(必填，0~1) | `musicxx.player.setVolume` | `Success` |
| `SetLoopMode` | `mode`(必填：Order / RealRand / Only / SimulateRand / SimulateRandReflush) | `musicxx.player.setLoopMode` | `Success` |
| `SetStopTimer` | `duration?`(秒 1~86400)、`enableStopAfterFinish?`、`enableSleepMode?`、`exitWhenStop?` | `musicxx.timer.set` | `Success to set timer (mm:ss)` / `Failed` |
| `CancelStopTimer` | 无 | `musicxx.timer.cancel` | `Success` |
| `SwitchAppForeground` | `operator`(必填，foreground/background) | `musicxx.app.foreground` / `background` | `Switch to [foreground\|background] Success` |
| `GetAppLifecycle` | 无 | `musicxx.app.state` | `The app is in the foreground (lifecycle state: resumed)` / `The app is in the background for 12s (lifecycle state: paused)` |

**外部插件管理**（细节见 [plugin-agent-cli.md](plugin-agent-cli.md)）

| 工具 | 参数 | 等价命令 |
|---|---|---|
| `InstallPlugin` | `path?`（绝对路径）**或** `url?`（下载链接）、`enable?`、`sha256?` | `musicxx.plugins.install` |
| `ReloadPlugins` | `id?` | `musicxx.plugins.reload` |
| `RestartPluginHost` | 无 | `musicxx.plugins.restart` |
| `ListPlugins` | 无 | `musicxx.plugins.list` |
| `UninstallPlugin` | `id`(必填)、`keepData?` | `musicxx.plugins.uninstall` |
| `SetPluginEnabled` | `id`(必填)、`enabled`(必填) | `musicxx.plugins.setEnabled` |
| `GetPluginLogs` | `id?`、`offset?`、`level?`(数值)、`keyword?`、`limit?`(1~500) | `musicxx.log.plugin` |
| `ClearPluginLogs` | 无 | `musicxx.log.plugin.clear` |

这些工具的文本里都**另外附一份现在的插件清单**（装了什么、启用 / 加载状态、失败原因），
一次调用就能确认结果，不用再问一次。`ListPlugins` 还带两行外置插件框架自己的信息：
`framework: <版本>, <日期>, commit <提交>` 与 `framework repository: <GitHub 仓库地址>`
（与 `musicxx-cli version` / `list` / `plugin list`、`/cli/ping`、`/cli/list` 是同一份）。

**局域网（共享与控制）与文件**

| 工具 | 参数 | 等价命令 | 文本形态 |
|---|---|---|---|
| `ListLanDevices` | `availableOnly?`、`refresh?`、`limitServerVersion?` | `musicxx.lan.clients` | 本机信息一行 + 每台设备几行（设备 / 应用与地址 / 账号与同账号 / 分享与权限与可控制性 / 最后通信） |
| `ScanLanDevices` | `reset?`、`wait?`(ms) | `musicxx.lan.scan` | 同上（扫描后重新给一份列表） |
| `RunCommandOnDevice` | `clientId?` / `addr?`、`cmd?` + `args?`、`action?` / `id?` + `data?`(object)、`token?` | `musicxx.lan.cli` | 「发了哪条指令给哪台设备」+ 这次走的通道（`/cli` 或 `/control`）与对方返回的数据 |
| `DownloadFile` | `url`(必填)、`path?`、`dir?`、`fileName?`、`overwrite?`、`sha256?`、`timeoutMs?` | `musicxx.net.file.download` | 落盘路径、字节数、内容类型、sha256、重定向后的地址 |
| `ShareLocalFile` | `path`(必填)、`id?`、`title?`、`download?`、`ttlSeconds?` | `musicxx.net.file.share.register` | 共享的标题 / 类型 / 路由 / 本机与局域网链接 / 过期时间 |
| `ListSharedFiles` | `id?` | `musicxx.net.file.share.list` | 每条共享一行（标题 / 类型 / id / 路径 / 访问次数）+ 链接 |
| `UnshareLocalFile` | `id?` / `all?` | `musicxx.net.file.share.unregister` | `Removed N shared file(s).` |

设备发现与指令转发需要两台设备都开着「共享与控制」；`musicxx.lan.cli` 默认转发到对方的 `/cli`
（命令面），只有命令面还没有对应命令的那几条指令才走 `/control` 数字指令，没有新增线协议。
本机文件对外下载走 `/share/<id>`（见 §6.6），目录挂在 `/share/<id>/` 下，route id 就是权限；
注册只在本次运行有效，程序重启即消失。

**能力与功能埋点**

| 工具 | 参数 | 等价命令 | 文本形态 |
|---|---|---|---|
| `ListCapabilities` | `id?`、`provided?` | `musicxx.capability.list` | 每个能力一段：类型、有没有实现、能不能被外置调、**能不能被插件调**（`pluginCallable`）、逐行列出实现（生效 / 备选 / 关闭）+ `callDoc` + 被调过几次；有前链 / 后链登记时多一行，有主程序广播订阅时多一行 |
| `CallCapability` | `id`(必填)、`args?`、`refresh?` | `musicxx.capability.call` | 结果 JSON，**超大字符串字段压成摘要**（见 §5.3）；失败时 `error` 是名称、`message` 是句子 |
| `ListMediaSources` | 无 | `musicxx.source.list` | 插件声明的音源类型（id / 展示名 / 地址写法）+ 可浏览分类 + 音源解析链现状 |
| `GetAudioSpectrum` | `path?`、`start?`、`duration?`、`bandCount?` 等（能力自己的参数） | 能力 `musicxx.media.spectrum.extract` | 文本表（每行一个时间步） |
| `AnalyseAudioChorus` | 能力自己的参数 | 能力 `musicxx.media.chorus.analyse` | 文本结论（副歌区段） |
| `AnalyseImageMainColors` | `path?` / `url?` | 能力 `musicxx.media.palette.analyse` | 主色 + 4 组色值的文本 |
| `GetMediaKernelInfo` | `part?`(all / versions / filters / decoders / formats)、`refresh?` | 能力 `musicxx.media.kernel` | 分段文本：mpv/ffmpeg 版本、能写进 `af` / `vf` 的过滤器（含说明）、解码器（按编解码器分组，标出硬件）、认得的音视频后缀；`part` 用来只拿其中一段 |

最后四个工具直接跑能力自己的实现（不同于 `CallCapability`：它们把数据排成可读文本），
能力说明与参数也来自能力声明。

**MCP 只覆盖一部分命令**（工具表是手工声明的，不是命令表的副本）：下面这些命令**没有对应的 MCP 工具**，
在 MCP 客户端里调不到 —— 要调它们请用命令行（`musicxx-cli call <命令 id> --args '<JSON>'`）
或 POST `/cli`：

| 没有 MCP 工具的命令 | 替代做法 |
|---|---|
| `musicxx.broadcast.list` / `.read` | `musicxx-cli broadcast list` / `broadcast read <频道id>`，或 `/cli` |
| `musicxx.feature.point.list` / `.call` | `ListCapabilities` / `CallCapability` 是同一份数据；要原始 JSON 用 `/cli` |
| `musicxx.app.commands` | `musicxx-cli commands [status\|install\|uninstall]` |
| `musicxx.entry.argv` | 命令行 `musicxx-cli open` / `call`，或 `/cli` 的 `{"argv": […]}` |
| 歌单 / 播放列表的写入能力（`musicxx.songlist.create` / `addSong` / `removeSong` / `updateSong`、`musicxx.playlist.addSong` / `removeSong` / `updateSong`） | `/cli`（`musicxx-cli call …`）—— 插件里仍然用 `musicxx.capability.call` |
| `musicxx.lyric.*`（读与写都没有工具）、`musicxx.ui.*`（`openSearch` 除外，它有 `OpenSearch` 工具）、`musicxx.storage.*`、`musicxx.net.*`、`musicxx.render.*`、`musicxx.media.palette.*` / `cover` / `spectrum`、`musicxx.source.define` / `undefine`、`musicxx.capability.subscribers`、`musicxx.host.*`、`musicxx.stats.*` | 同上（`/cli` 或插件内 `musicxx.capability.call`） |

判断某个命令有没有 MCP 工具，看 `GET /cli/list` 的 `commands[].id` 与 `tools/list` 的工具名：
工具名与命令 id 不是一回事，只有 §5.2 表里列出的那些能对上。

### 5.3 文本形态与"压摘要"

MCP 给模型的是**文本**（不是 JSON 结构），所以每个工具都有固定的排版：歌曲列表、歌单列表、
状态快照、插件清单、日志都按人读的顺序行输出。没有专门排版的就是缩进 JSON。

`CallFeaturePoint` 的结果里，**超过 4 KB 的字符串字段会被压成**
`{"omitted":true,"bytes":N,"head":"<前 256 个字符>"}`：整曲频谱一次约 820 KB base64，
整块塞进模型上下文既没用又挤掉别的内容。只要可读的频谱文本请改用 `GetAudioSpectrum`
（那是"能力导出"那条路，输出是文本表）。

### 5.4 参数 → inputSchema

命令 / 能力的参数声明直接转成 MCP 的 inputSchema：

| 声明 `type` | inputSchema |
|---|---|
| `string` | `JsonString` |
| `integer` | `JsonInteger`（带 `minimum` / `maximum` / 默认值） |
| `number` | `JsonNumber`（同上） |
| `boolean` | `JsonBoolean` |
| `enum` | `JsonEnum`（候选 + 默认值） |
| `array` | `JsonArray`（元素按 `itemType`，目前只有字符串） |

## 6. 本机 HTTP / WebSocket 服务

同一个 HTTP 服务上挂着全部本地与局域网入口（`InternetAddress.anyIPv4`，端口就是接入点文件里那个）：

| 路径 | 方法 | 用途 | 权限 |
|---|---|---|---|
| `/help` | GET | 接口说明：`/cli` 与 `/mcp` 怎么用（`text/plain`） | 放行（不需要令牌） |
| `/auth/token` | GET | 主动要一把授权密令（全权限、不过期） | 统一鉴权门（可弹窗）；「公开」档直接告诉你不需要令牌 |
| `/cli`、`/cli/list`、`/cli/ping` | GET / POST | 本机命令行（§3），所有平台都有 | 本机放行；非本机走统一鉴权门（§2.2） |
| `/mcp` | POST / GET / DELETE / OPTIONS | MCP（§5） | 统一鉴权（`OPTIONS` 预检不判定） |
| `/active` | POST | 设备信息 / 存活应答（§6.1） | 放行（受局域网服务开关影响返回内容） |
| `/share/<id>[/相对路径]` | GET / HEAD | 本机文件的对外下载（§6.6）：`file.share.register` 注册过的文件或目录 | route id 就是权限；目录只许访问挂载点下面 |
| `/control` | POST | 局域网控制指令（数字命令 id，§6.2） | 统一鉴权门（可弹窗；同账号 `sign` 直通） |
| `/connectEvent` | WebSocket | 事件流（§4.7 / §6.4） | 本机放行；非本机过同一道门，但**握手不弹窗**（没身份直接 403，先取令牌再 `?token=`） |
| `/getSrc` | GET | 资源代理：把本机能拿到的音源交给请求方（§6.3） | 统一鉴权门 + 「局域网服务」开着、且服务模式允许对外 |
| `/parseSrc` | GET | 请求对方尝试解析音源（局域网里"谁能拿到就给谁"） | 同上 |
| `/listenTogether` | POST | 一起听的邀请与回执 | 不校验远控权限（被邀请方确认） |
| `/ws/listenTogether` | WebSocket | 一起听组内同步 | 要 `groupId` + `clientId` 匹配 |
| `/ws/controlEvent`、`/ws/overlayWindow` | WebSocket | 控制页事件、悬浮窗连接 | 各自握手时校验 |
| `/proxy` | — | **占位**，还没实现通用代理，不要当它能用 | — |

### 6.1 设备发现与 `/active`

- **发现**走 mDNS（DNS-SD）服务类型 `_boolxx-service._udp`，TXT 记录携带设备信息
  （`LanSelfInfo_c`，字段与 `/active` 返回的一致）：`cid`(安装 ID)、`appType`、`ver`(服务版本)、
  `httpPort`、`uid`、`name`(昵称)、`viewName`(设备显示名)、`plat`(平台)、
  `lanType`(服务模式)、`ctrlType`(远控权限)。**没有 TXT 记录的同类服务会被忽略**；
- 设备信息变化（登录、改昵称、改权限）会重新注册服务 —— 只注册一次的话别的设备会一直显示旧信息；
- **心跳**用 POST `/active`：应答是 `{"code":2000,"data":{…LanSelfInfo…},"tip":""}`；
  未开局域网服务时 `data` 是空串（只表示"活着"，不吐昵称与账号信息）；
- 同一网段优先、同安装 ID 去重：心跳只用于刷新"最近一次可见时间"。

### 6.2 `/control`（局域网控制，数字命令 id）

请求体是 `{"id": <数字>, "data": {…}}`（可带 `?sign=` 走权限判定），应答是
`{"code":2000,"data":…,"tip":""}` 或 `XXResponse_c` 形状的错误：`4001`（权限不足）、
`4004`（资源找不到）、`4010`（参数缺失）、`4011`（参数错误）。**做过事就回 2000**
（播放 / 暂停 / 上下曲 / 进度 / 循环模式曾经误回 4010，已修正）。

| id | 名称 | 作用 |
|---|---|---|
| 21 | `SetSong` | 播放一首歌（或一个列表），可带 `position` 起始进度 |
| 22 / 23 | `SetNetPlaylist` / `SetTempPlaylist` | 播放网络歌单 / 临时歌单 |
| 24 | `SetLyric` | 同步歌词 |
| 25 | `AddPlaylist` | 把歌曲加进播放列表 |
| 26 / 27 | `InviteListenTogether` / `InviteListenTogetherReply` | 一起听邀请 / 回执 |
| 28 | `SetPlaylistSort` | 调整对方的**播放队列**排序（歌单本体不动） |
| 31 | `SetTempThemeMode` | 临时切夜间模式 |
| 41 / 42 / 43 | `SetAppVolume` / `SetSystemVolume` / `SetSpeed` | 音量与速度 |
| 50 / 51 | `AppExit` / `AppForegroundOrBackground` | 退出、前后台 |
| 100~105 | `Play` / `Pause` / `SwitchPrevSong` / `SwitchNextSong` / `SetCurrent` / `SetPlaylistLoopMode` | 播放控制 |
| 106 / 107 | `SetTempSpeed` / `RecoverTempSpeed` | 长按上下曲临时加减速（**不写入对方的播放速度设置**） |

客户端要先看对方版本够不够（§6.5）：不够就退回老指令（例如临时变速退回 `SetSpeed`，
抬手时再还原）。

### 6.3 `/getSrc` 与 `/parseSrc`（资源代理）

- `/getSrc?isMediaSrc=<bool>&crudeSrc=<urlencode>&parsedSrc=<urlencode>`：请求方拿到本机能取的
  媒体（本地文件 / 缓存 / 已解析的直链），由本机把字节流出去 —— 局域网里"文件只在一台机器上"
  的场景靠它；
- `/parseSrc?src=<urlencode>`：请求对方试着解析一个音源（对方可能登录了对应的网盘 / 有缓存），
  成功时回可用的链接与必要请求头；
- 两者都要求「局域网服务」开着，非本机还要求服务模式允许对外服务；缓存代理与本地转换都走这里。

### 6.4 事件流

`/connectEvent` 推的消息是 `{"cmd": <数字>, "data": <字符串>}`：

| 号 | 名称 | 号 | 名称 |
|---|---|---|---|
| 2 | `heartbeatCheck` | 19 | `lyricSrc` |
| 11 | `playerState` | 20 | `isNightTheme` |
| 12 | `song` | 21 | `appVolume` |
| 13 | `loadingMediaSrc` | 22 | `systemVolume` |
| 14 | `duration` | 23 | `speed` |
| 15 | `current` | 24 | `playlistLoopMode` |
| 16 | `songlist` | 25 | `playlistInfo` |
| 17 | `lyric` | 26 | `chorus` |
| 18 | `lyricSelectShowIndex` | | |

**事件号与事件名两边必须对得上**：命令行侧在 `cli/lib/src/event_socket.dart` 里自带一份
名字表，主程序侧是 `EventSocketMsgId_c`。

### 6.5 服务版本门槛

局域网功能按对方**服务版本**（`serverVersion`，接入点文件与 `/active` 里都有）判定，
客户端用 `isAvailable(limitServerVersion: …)` 这类判断：

| 版本 | 含义 |
|---|---|
| 4 | 播放歌曲（`SetSong`）、播放歌单 |
| 5 | 临时主题、音量、歌词 |
| 6 | 把歌曲加进播放列表、退出、前后台 |
| 9 | 播放速度 |
| 10000 / 10001 | `/control` 基本指令、循环模式 |
| 10002 | 一起听邀请与回执全部走 HTTP |
| 10003 | 远程调整播放列表排序（`SetPlaylistSort`、`playlistInfo` 事件） |
| 10004 | 副歌区段（`chorus` 事件）、长按临时变速（`SetTempSpeed` / `RecoverTempSpeed`） |

### 6.6 `/share/<id>`（本机文件对外下载）

`musicxx.net.file.share.register` 把一个本机文件（或目录）登记成一条共享，
`http://<地址>:<端口>/share/<id>`（局域网地址在结果的 `lanUrls` 里）。

| 项 | 说明 |
|---|---|
| 方法 | `GET` / `HEAD`（其它回 405） |
| Range | 支持（响应 206 + `Content-Range`；越界 416 + `bytes */<长度>`），可用于播放器与断点续传 |
| 文件项 | `/share/<id>` 给文件（末尾多一个 `/` 也一样）；`?download=1` 或注册时 `download: true` 带 `Content-Disposition: attachment` |
| 目录项 | 目录挂在 `/share/<id>/<相对路径>` 下；`/share/<id>/` 给索引（HTML，`?format=json` 给结构化列表） |
| 越界 | 相对路径逐段解码后，`..`、带分隔符的段、绝对路径一律拒绝；归一化后必须仍在挂载目录内（符号链接指出去也拒绝）→ 403；未知 id / 已失效 404 |
| 访问控制 | route id 随机 32 位十六进制（可自定义），**猜不到 = 分享链接即权限**；非本机请求还要求「共享与控制」开着；没有其他写/删能力 |
| 失效 | 注册时 `ttlSeconds` 到期，或 `musicxx.net.file.share.unregister`；**应用退出即全部失效**（不落库） |

常用于：把刚 `musicxx.net.file.download` 下来的插件包交给另一台设备装、把本机录制的音频交给局域网，
或让另一个 agent 从这台机器取文件。

## 7. 边界与安全

- **装插件等于执行任意代码**：`/mcp` 与 `/cli` 都能装插件、启用插件（动态库插件与主程序
  同进程运行）。`/cli` 在**所有平台**都开（含手机）。两者都走**同一道鉴权门**
  （`lib/manager/netAuth/MusicxxNetAuth.dart`）：本机回环直接放行；非本机按「本机 → 服务没开 →
  「公开」档 → 授权密令 → `sign` 同账号 → 弹窗」逐条判，谁都没通过就是 403（`/mcp` 是 JSON-RPC
  `code: -32001`）。弹窗授权那一次回写 `X-Musicxx-Token`，客户端存下来复用；`/mcp` 的 CORS 是 `*`，
  所以**不要把端口暴露到不可信网络**；
- **链接与启动参数也算本机入口**：`musicxx://call/?cmd=…` 能调任意入口，而一条链接任何网页 /
  应用都能触发。所以链接里请求 `musicxx.plugins.*`（装 / 卸 / 启用 / 重载 / 重启插件）与
  `musicxx.net.file.*`（往本机写文件、把本机文件开放给外部下载）这类管理入口时**先弹窗问用户**
  （判据是命令声明的 `MusicxxCliCommand_c.consentReason`，由 `MusicxxEntries_c.askLinkSensitiveDo`
  弹框）：列出命令与关键参数，点「允许」才执行，关掉窗口算拒绝；本机通道（命令行 / MCP / `/cli` /
  用户自己敲的 `--cmd`）不问。应用内「设置 → 界面 → 调用快捷链接」粘贴的链接（`manual` 通道，
  §1.1.1）与 applink 同一条规则。**新增"能装东西 / 能执行代码 / 能读写本机文件"的命令时必须写上
  `consentReason`**，否则链接能静默执行它；
- **`/share/<id>` 是“拿到链接就能下载”**：id 猜不到且可设过期时间，但没有账号校验；
  注册的是**文件或目录**（目录挂在 `/share/<id>/` 下，相对路径逐段校验、出不了挂载点），
  注册只在本次运行有效，**程序重启即全部消失**。
  不要把它当成需要鉴权的文件接口；需要限制访问时缩短 `ttlSeconds` 并及时 `musicxx.net.file.share.unregister`；
- **插件装不了插件**：安装 / 重载 / 重启插件框架只有命令行与 MCP 有（插件的能力表里没有
  这几条），这是有意的：否则 JS插件能把自己提升到原生权限；
- **`/cli` 与 `/control` 是两套东西**：`/cli` 是命令面（全量命令、结构化结果，本机与远程都能调，
  远程要过远控权限）；`/control` 是局域网设备之间的老远程控制（数字 id、要签名、面窄）。
  本机与 agent 要下发命令请走 `/cli`（跨设备用 `lan.cli` 转发）；
- **卸载不可逆**：`musicxx.plugins.uninstall` 默认连插件私有数据一起删（`keepData` 可保留）；
- **日志有上限**：插件日志是内存里的环形缓冲（最多 500 条，退出即失去）；应用自己的日志写在
  磁盘（`error.log` / `info.log`，按大小裁剪），最后几行可能还在写入缓冲里
  （结果里的 `flushedHint`）；
- **功能埋点调用零副作用**：`capability.call` 只读值仓，不写值仓、不改应用状态 ——
  调一次封面取色不会换掉用户看到的背景色；
- **一次只让一个 agent 装 / 卸同一个插件**：两台同时操作会出现"文件是这次、记录是那次"的竞争。

## 8. 排障

| 现象 | 处理 |
|---|---|
| 命令行退出码 2（"拟声未在运行"） | `musicxx-cli doctor` 看接入点文件与连接；或 `--start` / `musicxx-cli start` 先拉起 |
| 接入点文件在但连不上 | 那是上次运行留下的残留（退出不清理），按"没在运行"处理 |
| 找不到 `musicxx-cli` | 看应用里「设置 → 共享与控制 → 命令行接口（CLI）」；它必须与主程序同目录（`--start` / `mcp` 靠这一点找主程序） |
| `not_ready`（503） | 应用还在启动收尾，稍后重试（命令行本身会先等 15 秒） |
| 403（`permission_denied`） | 看 `data.deny`：`service_disabled` = 没开局域网服务；`need_token` = 要授权密令（`musicxx-cli auth token --host <ip> --port <端口>` 要一把，或在本机执行）；`ask_denied` / `ask_timeout` / `ask_busy` = 本机确认弹窗那一步没通过 |
| 退出码 5（协议不匹配） | 命令行与主程序不是同一次构建的产物，换成一起编译出来的那份 |
| MCP 客户端连不上 | 先 `musicxx-cli doctor`，再 `cd cli && dart run tool/mcp_probe.dart`（不经过客户端直接跑一遍 initialize / tools/list / tools/call，会说明卡在哪一步），最后 `musicxx-cli mcp --verbose` |
| 命令名/参数不确定 | `musicxx-cli list --json`（或 GET `/cli/list`）拿自描述清单（含 `entries` 链接形态），不要猜 |
| 点链接 / 启动参数没反应 | 应用日志里搜 `[入口]`：解析失败与不认识的链接都会写一行可读原因。桌面端先用 `musicxx-cli open "<链接>"` 复现（与点链接同一条解析），启动参数用 `musicxx-cli call <入口 id> --args '<JSON>'` 复现 |
| 链接里调插件管理没反应 | 链接里请求 `musicxx.plugins.*` / `musicxx.net.file.*` 会先弹确认框（§7），用户关掉窗口 = 拒绝；拒绝时答复是 `permission_denied`，日志里有一行「用户没有允许管理类入口」。本机通道（`musicxx-cli plugin …` / MCP / `/cli`）不问 |
| 调用答复 `not_found` | 命令 / 能力 id 写错了：`musicxx-cli list --json` 与 `musicxx-cli capability list` 里查一下。`no_impl` 表示名字对、但现在没有实现；`not_allowed` 表示这条能力没有对插件开放（声明里没有 `plugin` 块） |
| 工具/能力是不是最新 | 工具条数会随能力注册表增长：以 `tools/list`（或 `musicxx-cli capability list`）的实际结果为准 |
| 插件相关的排障 | [plugin-agent-cli.md](plugin-agent-cli.md) §10（`plugin list` / `plugin logs` / 主程序卡住的判断） |

---

**相关文档**：[plugin-api.md](plugin-api.md)（插件侧接口手册）、
[plugin-agent-cli.md](plugin-agent-cli.md)（用命令行 / MCP 管插件）、
[plugin-broadcasts.md](plugin-broadcasts.md)（广播频道与取值）、[plugin-capabilities.md](plugin-capabilities.md)（能力总表）、
[plugin-guide.md](plugin-guide.md)（总入口）。

**维护者注**（改接口时一起改的地方）：命令与工具的实现都在主程序
`lib/manager/cli/`（`MusicxxCliCommandsSetup.dart`、`MusicxxAppCommands.dart`、
`MusicxxMcpTools.dart`、`MusicxxCliRoute.dart`、`MusicxxCliEndpoint.dart`），
**统一入口**（链接形态、启动参数、自省）在 `lib/manager/entry/`
（`MusicxxEntries.dart` 注册表 + 链接/启动参数解析、`MusicxxEntryCommands.dart` 入口命令），
链接的接收端是 `lib/store/GlobalStore.dart`（`handleAppLink` / `initSongIntent`）与
`lib/main.dart`（启动参数、单实例转发），
本机服务路由在 `lib/manager/NetServerExt.dart` 与 `lib/share/manager/NetServer.dart`，
命令行客户端在 `cli/`（协议与事件名表要跟主程序对齐）。这一节的内容都是从这些文件读出来的。

**数字怎么核**（不要手数）：

- 命令条数 / 分类：`musicxx-cli list --json` 里的 `data.commandCount` 与 `commands[].kind`；
- MCP 工具：`tools/list`（或主程序 `MusicxxMcpTools_c.all()`，它 = 固定工具 + 能力注册表）；
- 能力有没有实现：看主程序 `lib/plugin/externPlugin/ExternAppCapabilities.dart` 里有没有 `_add(...)` 那一行
  （SDK 常量存在不等于已实现）；
- 能力已接入 / 未接入：`tools/capabilities.def.json` 的 `implemented` 字段（`plugin-capabilities.md` 由它生成，
  `dart run tools/gen_contract.dart --check` 校验）。
