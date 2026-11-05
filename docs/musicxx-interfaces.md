# musicxx 的对外接口：链接 / 启动参数 / MCP / 命令行 / 本机 HTTP

> 这篇是 **musicxx（主程序）对外暴露的全部接口**的总表：谁在什么场景用哪条通道、怎么连上、
> 请求与响应是什么形状、权限边界在哪、出问题从哪里看。
>
> 先说结论：**七条通道共用同一份实现（命令注册表）和同一套参数名**，一次调用只有一种形状
> `{"cmd": "<入口 id>", "args": {…}}`。链接（applink）与进程启动参数只是"把这次调用说出来"
> 的两种额外写法，见 §1。
>
> 插件自己的接口（动作 / 变量 / 功能点 / 钩子 / 能力 / 界面）在 [plugin-api.md](plugin-api.md)；
> 用命令行装插件、改完刷新、看插件日志在 [plugin-agent-cli.md](plugin-agent-cli.md)。

**术语**：**主程序**指 musicxx（Flutter/Dart 业务层）；**插件框架**指外置插件的 C++ 层。

## 1. 七条通道，一份实现

**一次外部调用在所有通道里都是同一个形状**：

```json
{"cmd": "<入口 id>", "args": {…}}
```

结果也只有一个形状：`{ok, cmd?, data? | error?}`。入口 id 就是命令 id，实现在命令注册表里
**只有一份**（主程序 `lib/manager/cli/` 与 `lib/manager/entry/`）；每条通道只是"怎么把这次调用
说出来"不同 —— 详见 §1.1 与 §1.2、§1.3。

| 通道 | 谁在用 | 入口 | 说明 |
|---|---|---|---|
| **MCP 工具** | AI 客户端（Claude Code / Desktop / Cursor / 自研 agent） | `http://127.0.0.1:<端口>/mcp`（Streamable HTTP），或 `musicxx-cli mcp`（stdio 桥） | §5 |
| **本机命令行** | 人、脚本、agent | `musicxx-cli`（独立可执行文件）→ 主程序 `/cli` | §4 |
| **本机链接（applink / deep link）** | 手机点链接、桌面点 `musicxx://…`、其他应用跳过来；也可以由用户在应用里自己粘贴 | `musicxx://…` 交给系统 → 应用自己解析 | §1.1、§1.1.1 |
| **进程启动参数** | 文件关联、"用拟声打开"、命令行传参、第二个实例转发 | `musicxx <参数…>` | §1.2 |
| **插件内部** | 装在应用里的动态库插件 / JS插件 | 动作 / 变量 / 功能点 / 钩子 / 能力 | [plugin-api.md](plugin-api.md) |
| **局域网设备之间** | 另一台装了 musicxx / 流明的设备（控制页、一起听、资源代理） | `/control`、`/connectEvent`、`/getSrc`、`/parseSrc`、`/listenTogether`、`/ws/*` | §6 |
| **设备发现** | 同网段的设备互相发现 | mDNS 服务 `_boolxx-service._udp` + HTTP `/active` | §6.1 |

**同一件事在不同通道上的名字**（全部落到同一份实现：命令注册表）：

| 要做的事 | MCP | 命令行 | 插件动作 | 链接（applink） | 启动参数 |
|---|---|---|---|---|---|
| 读当前媒体信息 | `GetMediaInfo` | `musicxx-cli status`（命令 `status`） | —（读状态镜像 `musicxx.state.player`） | `musicxx://call/?cmd=status` | `--cmd status` |
| 暂停 | `MediaPlayOrPause`（`operator:"pause"`） | `musicxx-cli pause`（命令 `musicxx.player.pause`） | `musicxx.player.pause` | `musicxx://musicxx.player.pause` | `--cmd musicxx.player.pause` |
| 播放一个文件 / 链接 | `PlayMedia`（命令 `musicxx.player.playSrc`） | `musicxx-cli call musicxx.player.playSrc --args '{"src":"…"}'` | `musicxx.player.playSrc` | `musicxx://play/?url=…` | 直接给文件路径，或 `--uri …` |
| 跳搜索页 | `OpenSearch`（命令 `musicxx.ui.openSearch`） | `musicxx-cli call musicxx.ui.openSearch --args '{"keyword":"…"}'` | —（插件用 `musicxx.ui.openRoute`） | `musicxx://search/?keyword=…` | `--cmd musicxx.ui.openSearch --args '{"keyword":"…"}'` |
| 副歌区段 | `AnalyseAudioChorus`（能力工具） | `musicxx-cli feature call AnalyseAudioChorus` | `musicxx.feature.call` | — | — |
| 读前后台状态 | `GetAppLifecycle` | `musicxx-cli call app.lifecycle` | 变量 `musicxx.app.lifecycle`（读值 + 订阅） | `musicxx://call/?cmd=app.lifecycle` | `--cmd app.lifecycle` |
| 切到前台 / 后台 | `SwitchForegroundBackground`（`operator`） | `musicxx-cli call window.background`（命令 `window.foreground` / `window.background`） | `musicxx.app.background` / `musicxx.app.foreground` | `musicxx://musicxx.app.background` | `--cmd musicxx.app.background` |
| 改音量 / 速度 / 音调 / 进度 | `SetAppVolume` / `SetSpeed` / `SetPitch` / `SetCurrentPlaybackPosition` | `musicxx-cli volume` / `speed` / `pitch` / `seek` | `musicxx.player.setVolume` / `setSpeed` / `setPitch` / `seek` | `musicxx://call/?cmd=musicxx.player.setVolume&args=…` | `--cmd musicxx.player.setPitch --args '{"pitch":1.1}'` |
| 拦一次改播放值 / 收播放进度 | — | — | 钩子 `musicxx.player.seek` / `volume` / `speed` / `pitch` / `position` | — | — |
| 功能点清单 | `GetFeatureProviders` | `musicxx-cli featurePoint.list` | `musicxx.feature.point.list` | `musicxx://call/?cmd=featurePoint.list` | `--cmd featurePoint.list` |
| 音源类型与解析链 | `ListMediaSources` | `musicxx-cli source.list` | `musicxx.source.list` | `musicxx://call/?cmd=source.list` | `--cmd source.list` |
| 插件框架版本 / 提交 / 仓库地址 | `GetExternPlugins`（结果里的 `framework` 段） | `musicxx-cli version` / `list` / `plugin list` | 插件读 `host.info().hostVersion`（同一个版本号） | — | — |
| 装插件 | `InstallExternPlugin`（`path` 或 `url`） | `musicxx-cli plugin install`（路径或 `--url`） | **没有这个动作**（见 §7） | — | `--cmd externPlugin.install --args '{"path":"…"}'` |
| 局域网设备：扫描 / 列表 / 下发指令 | `ScanLanDevices` / `ListLanDevices` / `RunCommandOnDevice` | `musicxx-cli call lan.scan` / `lan.clients` / `lan.cli` | —（插件不参与设备控制） | `musicxx://call/?cmd=lan.clients` | `--cmd lan.scan` |
| 按链接下载文件 | `DownloadFile` | `musicxx-cli call file.download --args '{"url":…}'` | — | `musicxx://call/?cmd=file.download&url=…`（会先问用户） | `--cmd file.download --args '{"url":…}'` |
| 本机文件对外下载 | `ShareLocalFile` / `ListSharedFiles` / `UnshareLocalFile` | `musicxx-cli call file.share.register` / `.list` / `.unregister` | — | `musicxx://call/?cmd=file.share.register&path=…`（会先问用户） | `--cmd file.share.list` |

> **写链接或启动参数之前，先问一次清单**：`musicxx-cli list --json`（或 GET `/cli/list`）里有
> `commands`（入口 id、参数声明）与 `entries`（哪些入口能用链接调进来、链接形态与取值候选），
> 还有 `framework`（本机插件框架的版本 / 提交 / 仓库地址）。
> 不要照着文档硬编码 —— 清单会随能力注册表增长。

### 1.1 applink（协议 `musicxx://`）

系统把整条链接交给应用，应用解析成一次"统一调用"（参数名与命令声明的完全一致；下面这些
参数都要 **UrlEncode**，习惯写法是带尾部斜杠的 `musicxx://play/?…`，不写也能识别）：

| 链接 | 归一成 | 说明 |
|---|---|---|
| `musicxx://play/?media=<拟声音源字符串>` | `musicxx.player.playSrc {src}` | 推荐写法：音源格式的字符串（歌曲-详情-音源里可长按复制） |
| `musicxx://play/?filename=<文件名\|路径>` | `musicxx.player.playSrc {src, type:"local"}` | 按本地文件/缓存里的文件名找（不同设备路径不同时省事） |
| `musicxx://play/?url=<链接>` | `musicxx.player.playSrc {src, type:"link"}` | `https://…`、`file:///…` 或直接路径 |
| `musicxx://play/?search=<关键词>` | `history.playByInfo {info, tip:true}` | 在播放记录里模糊找一首并播放，没找到会弹提示 |
| `musicxx://search/?type=<类型>&keyword=<词>` | `musicxx.ui.openSearch {type, keyword}` | `type` 取值见 §4.3；两个参数都可省 |
| `bilibili://search/?keyword=<词>` | `musicxx.ui.openSearch {keyword, type:"bili"}` | 等同上一条的 `type=bili` |
| `musicxx://call/?cmd=<入口 id>&args=<JSON>` | 直接调用该入口 | 通用形态，`args` 省略时其余查询参数按参数名取值（值都是字符串） |
| `musicxx://call/?cmd=musicxx.player.setVolume&volume=0.5` | `musicxx.player.setVolume {volume:"0.5"}` | 查询参数名 = 命令参数名 |
| `musicxx://<入口 id>` | 直接调用该入口 | 例如 `musicxx://musicxx.player.toggle`（入口 id 大小写不敏感） |

- `musicxx://play/` 同时给多个参数时，按上表**从上到下**取第一个有值的（与旧版行为一致）；
  一个候选都没有 → 该链接解析失败（日志里有一行可读原因）；
- 不认识的链接（host 既不是 `play` / `search` / `call`，也不是入口 id）只记一行日志，不做事；
- **链接里的管理类入口要先问用户**：`externPlugin.*`（装 / 卸 / 启用 / 重载 / 重启插件）不直接拒、
  也不静默执行 —— 执行前弹窗列出命令与关键参数（`path` / `pid`…），点「允许」才执行，关掉窗口算
  拒绝（见 §7）。本机通道（命令行 / MCP / `/cli`）不问；应用内粘贴的链接与 applink 同一条规则
  （见 §1.1.1）；
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
  失败还会往应用日志里写一行「调用快捷链接失败 + 链接 + 原因」（`app.logs` / 设置里的日志页能看到）；
- **管理类入口（`externPlugin.*`）与 applink 同一条规则**：执行前再弹一次确认，列出命令与 `path` /
  `pid` 等参数，点「允许」才执行，关掉窗口算拒绝；
- 不需要 `/cli`、MCP、协议注册或 `musicxx-cli`：macOS / iOS / Linux 这些收不到 applink 的平台上，
  它就是唯一的手动执行入口。

**普遍用途**（这是它存在的理由，不只是调试工具）：

| 用途 | 怎么做 |
|---|---|
| 远程协助 | 让用户粘一条链接就完成一次操作：跳页面、搜索、播放、切主题、装插件。不用教对方开命令行 / 开 MCP / 装 CLI，也不用发可执行文件 |
| 装 / 更新插件 | 插件包先放到对方机器上，再给 `musicxx://call/?cmd=externPlugin.install&args=<UrlEncode 后的 {"path":"C:/…/x.zip","enable":true}>`（**要对方点「允许」**，见下）；插件包在公网上时直接用 `url`：`{"url":"https://…/x.zip","sha256":"…","enable":true}`；启用 / 停用 / 卸载 / 重载同理（`externPlugin.setEnabled` / `uninstall` / `reload`） |
| 排查“链接不生效” | 参数改完粘一次就能看到解析结果与报错，不用注册协议、不用退到桌面点链接 |
| 自测 / 演示 | 插件作者、写文档的人验证自己给出的链接示例是不是还能用 |

注意：

- **粘贴即执行**：这条通道唯一的确认就是用户自己按下的「确定」，所以只粘信任来源的链接；
  插件管理类会再问一次，但那句话同样可能被人教着点「允许」；
- 链接里引用的文件必须**在那台机器上存在**（`externPlugin.install` 的 `path` 是绝对路径），
  或者用 `url` 让应用自己下载（`file.download` 同一条路径，装完删临时包）；
- **`file.*` 与 `externPlugin.*` 一样要先问用户**：前者能往任意路径写文件、也能把本机文件开放给外部下载；
  后者装插件等于执行代码。
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

## 2. 先决条件与连接

| 条件 | 说明 |
|---|---|
| 主程序在运行 | MCP / 命令行 / 局域网这几条都由主程序的 HTTP 服务提供（applink 与启动参数本身就能把应用拉起来）。没在运行时命令行退出码 2（加 `--start` 可以先拉起） |
| HTTP 服务已绑定 | 主程序启动时就绑定（`InternetAddress.anyIPv4`，端口按设置，默认随机）：启动日志里有一行 `[HttpServer] 已启动: http://127.0.0.1:<端口>/` |
| `/cli` 所有平台都有 | 手机与桌面是同一份命令表（以前 `/cli` 只在桌面端开放）；`/mcp` 与局域网那几条在服务起来后都能用。安卓上也能用链接（§1.1） |
| 应用已走完启动 | 命令行 / MCP / HTTP 与"应用跑起来之后"进来的链接、启动参数都要等应用就绪：入口调用会先等（默认最多 15 秒），等不到返回 `app_not_ready`（命令行是 503 / 退出码 2）。**唯一不等的是启动流程自己**（首次链接、这次启动带的参数）：它们发生在"就绪"置位之前，与旧行为一致立即执行 |
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
本机（回环 / 链路本地 / 组播） → 局域网服务没开 → 档位「公开」 → 授权密令 → sign 同账号 → 弹窗
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
POST /cli                   {"cmd": "<入口 id>", "args": {…}}         ← 与命令行、MCP、插件动作同形
                          | {"uri": "musicxx://play/?url=…"}          ← 与链接（applink）同一条解析
                          | {"argv": ["--call", "…", "a.mp3"]}        ← 与进程启动参数同一条解析
```

**三种请求体归一成同一次调用**（`{"cmd", "args"}`），解析与执行都在主程序
`lib/manager/entry/`：`uri` 与 `argv` 只是"把别的通道的形态搬到 HTTP 上"，方便脚本与排障
（不用真的点一次链接）。一次请求只执行一条调用：`argv` 解析出多条时给 400，请分几次请求。

响应体只有一种形状：`{ok, cmd?, data? | error?}`。

| HTTP 状态码 | 什么时候出现 |
|---|---|
| 200 | 请求合理。**命令本身的失败也是 200 + `ok:false`**，调用方只看 `ok` |
| 400 | 请求体不是 JSON 对象 / 缺 `cmd` / `args` 不是对象 / `uri` 认不出来 / `argv` 解析出多条 |
| 403 | 非本机且没过鉴权门（`error` 里说明要令牌还是被用户拒绝） |
| 404 | 路径不认识 |
| 405 | 方法不对（只接受 GET / POST） |
| 503 | 应用还在启动收尾（`app_not_ready`；默认先等 15 秒才这么答） |

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
| `repositoryUrl` | 插件框架的 GitHub 仓库地址（写插件时对着它看钩子与文档） |
| `commitUrl` | 该版本提交的地址（没补记提交号时与 `repositoryUrl` 相同） |

- 这是**随应用一起编译进来的那份声明**（插件包 `lib/src/version.dart`）；
  插件框架库自己自报的版本要插件跑起来才读得到，在 `externPlugin.list` 的
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
  -ContentType 'application/json' -Body '{"cmd":"status"}'
```

## 4. 命令面（当前 118 条 = 内置与应用命令 33 + 动作命令 80 + 能力导出 2 + 变量 3）

> **条数会随注册表增长，一律以 `musicxx-cli list`（GET `/cli/list` 的 `commandCount`）为准**，
> 下面各节的数字只是写这份文档时的快照。
>
> 动作命令比“已实现的动作”多 4 条：过时分类 `musicxx.library.*` 里仍然可用的四个旧名字也被登记成
> 命令（见 §4.5 末尾）；`musicxx.player.playSrc` 由内置表登记，不重复计数。

### 4.1 命令从哪来

| 来源（`kind`） | 条数 | 谁登记的 | 内容 |
|---|---|---|---|
| `builtin` | 33 | `MusicxxCliCommandsSetup` + `MusicxxAppCommands` + `MusicxxEntryCommands` + `MusicxxEntries` | `status`、歌词、歌单、播放记录、定时停止、前后台状态与切换、外部插件管理、日志、功能点、**播放内核信息**、**统一入口的播放音源 / 跳搜索页 / 执行启动参数（`musicxx.entry.argv`）**、**共享与控制（设备扫描/列表/指令转发）**、**文件（按链接下载 / 注册静态文件服务）** |
| `action` | 80 | 主程序动作表 | 与插件、MCP 共用同一份实现；**命令 id 就是动作名**（含 4 条 `musicxx.library.*` 旧名） |
| `feature` | 2 | 应用能力导出 | `feature.list` / `feature.call` |
| `variable` | 3 | 变量目录 | `var.list` / `var.get` / `var.set` |

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
都是字符串，各通道就不用各写一遍转换。没有声明 `params` 的命令（动作那一批）把入参**原样交给
动作**，动作自己做宽松解析（数字与字符串可互转）。参数不对时答复形如
`{"ok":false,"error":"参数 xxx 非法: …"}`。

### 4.3 内置与应用命令（33 条）

| 命令 | 参数 | `data` |
|---|---|---|
| `status` | 无 | 当前媒体的结构化快照（标题 / 歌手 / 专辑 / 总时长 / 进度 / 状态 / 循环 / 速度 / 应用内音量 / 文件名 / 格式 / 采样率 / 码率）；没有播放过时带 `hasMedia:false` |
| `musicxx.player.playSrc` | `src?`(string，单个音源)、`sources?`(array，多个音源)、`type?`(string，缺省 `auto`；可写来源类型名 `bili` / `link` / `aliDrive` / `baiduPan` / `drive115` / `subsonic` / `jellyfin` / `emby` / `Lumenxx` / `local` / `localCache` / `LocalCacheId` / `LocalContentUrl` / `assest` / `myid` / `mylink` / `webdav` / `mycos`，或插件音源类型 id `plugin.<插件 id>.<类型名>` —— 这时 `src` 写插件自己的资源 id)、`filterExt?`(bool，只接受扫描配置里的媒体后缀)、`name?`(显示名) | `{count, single, name, sid, artist, sources, skipped?}`：按来源播放（本地路径 / `content://` / 文件链接 / 拟声音源字符串 / 任意来源类型 / 插件音源）。一个来源追加到当前播放列表并播，多个来源列成临时歌单。**这是 applink `musicxx://play/` 与启动参数裸文件背后的入口**，三条通道参数名一致；`type` 写了来源类型就按该类型存（`bili` 的 `src` 写 BV 号或视频链接），能不能播由来源自己的解析决定 |
| `lrc.current` | `word?`(bool) | `{hasLyric, word, lineCount, text?}`：当前歌词的 `.lrc` 文本，`word:true` 尽量给逐字（增强型） |
| `playlist.play` | `pid`(int,必填)、`type?`(local/net)、`index?`(int)、`songName?`(string) | `{pid, sid, name, artist}`：播歌单里的一首（按 `index` 或名字包含匹配找，都不给播第一首） |
| `history.playByInfo` | `info`(string,必填)、`tip?`(bool) | `{found, sid?, name?, artist?}`：在播放记录里按标题 / 歌手匹配最相关的一首并播放；`tip:true`（applink `musicxx://play/?search=` 用它）时没找到会弹界面提示 |
| `musicxx.ui.openSearch` | `type?`(枚举，见下方类型表)、`keyword?`(string) | `{route, type, keyword}`：打开搜索页并带上类型与关键词。**applink `musicxx://search/`、`bilibili://search/` 背后的入口** |
| `musicxx.entry.argv` | `argv`(array，必填：与命令行里写的一样、不含程序自身路径) | `{count, cmds:[…]}`：把一串启动参数按 §1.1 的规则解析成一次或多次命令调用并执行（Windows 上把第二实例的命令行交给已有实例用的就是它）；解析失败、或其中一条失败都整串拒绝；不能嵌套它自己 |
| `clocking.set` | `duration?`(秒 1~86400)、`enableStopAfterFinish?`、`enableSleepMode?`、`exitWhenStop?` | `{started, durationSeconds, durationText, …}`；什么都不给时沿用用户上次的设置（没有则 60 分钟） |
| `clocking.cancel` | 无 | `{ok:true}`（没设置过也算成功） |
| `window.foreground` / `window.background` | 无 | `{ok:true}` |
| `app.lifecycle` | 无 | `{foreground, lifecycle, state, backgroundMs, lastBackgroundMs}`：`foreground` = 是否前台；`lifecycle` = `foreground` / `background`；`state` = 原始生命周期状态名（`resumed` / `inactive` / `hidden` / `paused` / `detached`）；`backgroundMs` = 已经在后台多久（前台为 0）；`lastBackgroundMs` = 上一次在后台待了多久。与变量 `musicxx.app.lifecycle` 读的是同一份状态（变量只给 `foreground` / `background`） |
| `app.commands` | `action?`(string：`install` / `uninstall` / `status`，缺省 `status`) | `{…, text}`：把 `musicxx` / `musicxx-cli` 登记成系统里能直接输入的命令（Windows 写用户 PATH 与 App Paths；macOS / Linux 在 PATH 里的可写目录建入口），或者查 / 删这份登记；`locations` 是实际生效的位置，`problems` 是失败原因 |
| `externPlugin.list` | 无 | 插件框架现状 + 插件清单（字段见本节末尾那张表） |
| `externPlugin.install` | `path?`(string，本机绝对路径)、`url?`(string，下载链接)、`enable?`(bool)、`sha256?`(string) | `{ok, id, version, kind, author?, description?, permissions, enabled, path, sha256, isUpgrade, target, targetEntry, targets, fileCount, totalBytes, source, download?, status}`：`path` 与 `url` 二选一（都不给或都给都是命令失败）；`url` 分支先下到 `<系统缓存根>/musicxx/temp/plugin` 再装，**装完（无论成败）删临时包**；`sha256` 给了就比对，不一致不装 |
| `externPlugin.uninstall` | `id`(必填)、`keepData?`(bool) | 卸载结果 + 插件清单 |
| `externPlugin.setEnabled` | `id`(必填)、`enabled`(必填) | `{id, enabled, loaded, …}` + 插件清单 |
| `externPlugin.reload` | `id?`（不给 = 重扫目录并重载全部启用中的） | `{reloaded:[id…], failed:{id:原因}, …}` + 插件清单 |
| `externPlugin.restart` | 无 | 重启后的状态（`hostStarted` / `pluginCount` / `loadedCount` / `restarted`） |
| `externPlugin.logs` | `id?`、`offset?`(≥0)、`level?`(数值 800/900/1000)、`keyword?`、`limit?`(1~500，默认 50) | `{ok:true, buffer, id, level, keyword, logs:[{ts, time, type, plugin, detail}…], total, offset, count, hasMore}`，**新的在前**；`total` 是过滤后的条数 |
| `externPlugin.clearLogs` | 无 | `{cleared}` |
| `app.logs` | `file?`(error/info/both)、`offset?`、`level?`(名字或数值)、`keyword?`、`exclude?`、`limit?`(1~500) | `{logs:[{timeText, level, file, source, text}…], total, hasMore, fileSize, parseErrors, flushedHint}`，**新的在前** |
| `featurePoint.list` | `id?`、`provided?`(bool) | `{ok:true, points:[{id, title, type, available, callable, rows, stages, broadcasts, callDoc?, callStat?}…]}`；`rows` 是实现行（各层谁能提供），`stages` 是**前链与后链上的登记**（与 `rows` 分开），`broadcasts` 是**主程序自己注册的广播订阅**（只收“被使用了 / 处理器变了”的通知，不参与实现，插件不能注册） |
| `featurePoint.call` | `id`(必填)、`args?`(object)、`refresh?`(bool) | `{ok, id, value, by, identity, fromCache, ms}`；失败 `{ok:false, error, message}`（`error` 取 `not_callable` / `bad_args` / `no_impl` / `disabled` / `stopped` / `dropped` / `busy` / `failed`；`disabled` / `stopped` / `dropped` 是三种不同的“没有结果”） |
| `source.list` | 无 | `{ok:true, sources:[{id, title, depict, srcDoc, plugin, declaredBy}…], browse:[{type, plugin, title, tag, entries:[{view, title, order, root, icon, pageSize, sort, indexBar, write, search, addFolderSongs, download, capabilities}…]}…], resolveChain:[…]}`：插件声明的音源类型 + 按类型分组的可浏览分类（`musicxx.source.browse`，一个类型可以有多条 `entries`）+ 音源解析链现状（与 `featurePoint.list` 看 `musicxx.player.source.resolve` 是同一份） |
| `kernel.info` | `refresh?`(bool) | 播放内核能播什么、能用哪些过滤器（与 MCP 工具 `GetMediaKernelInfo`、插件动作 `musicxx.media.kernel` 同一份数据）：`mpvVersion` / `ffmpegVersion`（这份 ffmpeg 是裁剪构建，报 `unknown`）/ `kernelReady` / `audioFilters` + `videoFilters`（能直接写进 `af` / `vf` 的名字，每项 `{name, libavName?, depict?, flags, builtin}`；`builtin` 是 mpv 自带的那几个，重名时 `name` 给带 `lavfi-` 前缀的可用名字）/ `decoders`（`{codec, driver, depict?, hardware}`）/ `audioExtensions` + `videoExtensions`（应用扫描时认得的后缀）/ `counts` / `tookMs` |
| `lan.scan` | `reset?`(bool，缺省 true)、`wait?`(int ms，0~15000，缺省 3000) | `{ok, serverVersion, port, enabled, self, count, clients}`：刷新一次「共享与控制」的设备发现（重新声明本机服务 + 重新浏览 mDNS + 给已发现设备发心跳），最多等 `wait` 毫秒让发现结果进来。需要「共享与控制」开着、本机 HTTP 服务已绑定 |
| `lan.clients` | `availableOnly?`(bool，缺省 true)、`refresh?`(bool)、`limitServerVersion?`(int) | 同上形状：已发现设备列表。每一项：`clientId` / `displayName` / `name` / `viewName` / `platform` / `appType` / `appTypeName` / `sameApp` / `addr` / `httpPort` / `uid` / `sameUser` / `version` / `serverType`+`serverTypeName` / `acceptRemoteCtrl`+`acceptRemoteCtrlName` / `canControl`（本机现在能不能控制它）/ `available` / `mdnsLost` / `lastNotify` / `lastNotifyAgoMs` / `controlUrl`。`refresh:true` 先发一轮 `/active` 心跳 |
| `lan.cli` | `clientId?` / `addr?`（二选一）、`cmd?` + `args?`、`action?` / `id?` + `data?`(object) | `{ok, clientId, addr, httpPort, action?, via, cmd?, cmdId?, cmdName?, remoteOk, data, error?, tip?}`：把一条指令发给指定设备。**默认转发到对方的 `POST /cli`**（命令面），`cmd` + `args` 可以直接给对方的任意命令 id（`musicxx.player.pause`、`playlist.play`、`externPlugin.install`、`file.share.register` …），拿回结构化结果；`action` + `data` 是常用动作的简写，play / pause / next / prev / seek / volume / speed / loopMode 会换成对应命令走 `/cli`，其余（systemVolume / tempSpeed / recoverTempSpeed / theme / appToggle / exit / song / playlist / netPlaylist / addPlaylist / sort / lyric）命令面还没有对应命令，仍走老的 `/control` 数字 id（也可直接给 `id`）。`via` 说明这次走的哪条；`data` 是对方返回的数据。对方不允许远程调用（403 / 4001 / 4003）、不认识命令（404 / 4004）、还在启动（503）或没答复（离线）时命令失败并带回原因 |
| `file.download` | `url`(必填)、`path?`、`dir?`、`fileName?`、`overwrite?`(bool，缺省 false)、`sha256?`、`timeoutMs?`(缺省 120000) | `{ok, url, realUrl?, path, bytes, sha256, status, contentType, tookMs}`：按 http/https 链接下载到本机。先写 `<目标>.part`，成功且校验通过才改名；目标已存在且未给 `overwrite` 时失败。不给 `path`/`dir` 时落到 `<系统缓存根>/musicxx/temp/download/<从链接推导的文件名>` |
| `file.share.register` | `path`(必填，本机绝对路径，文件或目录)、`id?`、`title?`、`download?`(bool)、`ttlSeconds?`(int，缺省 0 = 不过期) | `{id, route, path, title, isDir, download, size, modified, createdAt, expiresAt, ttlSeconds, hitCount, bytesServed, lastHitAt, localUrl, lanUrls}`：把一个本机文件或目录注册成 HTTP 静态文件服务，拿到 `/share/<id>` 与可直接用的链接。route id 随机 32 位十六进制（可自定义，只能 `A-Za-z0-9_-`），**id 就是权限** |
| `file.share.list` | `id?` | `{ok, count, shares:[...]}`：当前注册的共享 |
| `file.share.unregister` | `id?` / `all?`(bool) | `{ok, removed}`：注销一个或全部共享，链接立刻失效 |

`musicxx.ui.openSearch` 的 `type` 取值（大小写不敏感，缺省保持当前搜索类型）：`localsong`（本地歌曲）、
`historysong`（播放记录）、`bili`（Bili）、`musiccloud`（音乐云盘）、`alidrive`（阿里云盘）、
`baidupan`（百度网盘）、`115drive`（115云盘）、`subsonic`、`jellyfin`、`emby`、`playlist`（歌单）。

插件清单的字段（`externPlugin.*` 的结果里都有，安装结果里它在 `status` 下）：
`hostStarted`、`platformSupported`、`supportPlus`、`enabled`、`safeMode`、`autoSafeMode`、
`lastError`、`pluginDir`、`pluginCount`、`loadedCount`、`framework`（`version` / `commit` / `date` /
`describe` / `repositoryUrl` / `commitUrl` / `libraryVersion` / `libraryMatches`）、
`capabilityCall`（`async` 或 `hostTooOld`）、
`pendingCapabilityCalls`、`plugins[]`（`id` / `version` / `kind` / `enabled` / `loaded` /
`supported` / `reason` / `target` / `path` / `permissions` / `error`）。

### 4.4 能力导出与变量（5 条）

| 命令 | 参数 | `data` |
|---|---|---|
| `feature.list` | 无 | `{features:[{id, title, depict, args, output}…]}`（与 MCP 工具同一份注册表） |
| `feature.call` | `id`(必填)、`args?`(object) | `{id, output, text?, json?}`；失败时是命令失败（`ok:false` + 原因） |
| `var.list` | 无 | `{variables:[…]}`：键、能力位、类型、候选值、当前值 |
| `var.get` | `key`(必填) | `{key, value, type, options, caps, title, depict, risk}` |
| `var.set` | `key`(必填)、`value`(必填，任意 JSON 值) | `{key, value}`；属主不接受时是命令失败（原因可读） |

变量清单与语义见 [plugin-vars.md](plugin-vars.md)。当前四个导出能力：
`GetAudioSpectrum`（整曲频谱，文本表）、`AnalyseAudioChorus`（副歌区段）、
`AnalyseImageMainColors`（图片主色调）、`GetMediaKernelInfo`（播放内核：版本 / 过滤器 / 解码器 / 后缀）。
它们的参数由能力自己声明，`feature.list` / MCP 的工具清单里就是同一份。

### 4.5 主程序动作（命令 id = 动作名）

参数与返回的完整说明在 [plugin-api.md](plugin-api.md) §3.2（本文不重复，避免两处不一致）。
这一节只给"有哪些、大致做什么"：

| 域 | 动作 |
|---|---|
| 应用前后台（2） | `musicxx.app.foreground` / `background`（与 `window.foreground` / `window.background` 同一份实现） |
| 播放（13） | `musicxx.player.play` / `pause` / `toggle` / `stop` / `next` / `prev` / `playSrc` / `seek` / `setVolume` / `setSpeed` / `setPitch` / `setQuality` / `setLoopMode` |
| 歌单（7） | `musicxx.playlist.queryLists` / `querySongs` / `play` / `addSong` / `removeSong` / `create` / `updateSong` |
| 播放列表（5） | `musicxx.songlist.querySongs` / `playSong` / `addSong` / `removeSong` / `updateSong`（读 / 改当前正在播的那一串歌） |
| 歌词（5） | `musicxx.lyric.current.get` / `current.update` / `current.updateContent` / `getBySrc` / `search`（读当前歌词；选拟声云歌词 + 改这首歌的偏移；用 LRC 文本临时设置当前歌词；按歌词源读内容；搜索云歌词） |
| 提示、弹层与页面（7） | `musicxx.ui.notify` / `toast` / `dialog` / `overlay.show` / `overlay.close` / `openOverlaySheet` / `openRoute` |
| 全局字体（3） | `musicxx.ui.setFontFamily` / `getFontFamily` / `loadFont` |
| 插件私有存储（4） | `musicxx.storage.get` / `set` / `delete` / `list` |
| 网络（2，保留） | `musicxx.net.fetch` / `net.download`（老插件直接调动作时用；JS 封装与新建插件走插件框架自带的 `musicxx.net` 表） |
| 渲染槽位（3） | `musicxx.render.list` / `current` / `select` |
| 封面与频谱（7） | `musicxx.media.palette` / `palette.set` / `cover` / `bindImage` / `unbindImage` / `spectrum` / `kernel`（播放内核信息：版本 / 能写进 `af` 与 `vf` 的过滤器 / 解码器 / 后缀） |
| 能力导出（2） | `musicxx.feature.list` / `feature.call` |
| 功能点（4） | `musicxx.feature.point.list` / `call` / `define` / `undefine` |
| 音源（4） | `musicxx.source.define` / `undefine` / `list` / `browse`（`browse` = 声明这个类型可浏览并挑歌，见 [plugin-api.md](plugin-api.md) §3.3） |
| 插件网盘 UI（3） | `musicxx.ui.pickSongs` / `pickDir` / `browseDrive`（挑歌 / 选目录 / 整页浏览，**一个类型可以有多个分类 `view`**，见 [plugin-api.md](plugin-api.md) §3.3） |
| 钩子自省（1） | `musicxx.hook.handlers` |
| 宿主（3） | `musicxx.host.openUrl` / `clipboard` / `getPath` |
| 统计上报（2） | `musicxx.stats.reportMemory` / `reportMetric` |

> 过时分类 `musicxx.library.*`：`querySonglists` / `querySongs` / `playSong` / `playSonglist` 这四条
> **仍然注册着**（主程序把它们指向同一份实现，老插件不改也能用），含义分别等同上面的
> `playlist.queryLists` / `playlist.querySongs` / `songlist.playSong` / `playlist.play`。新代码写新名字。
>
> 上表里的 `musicxx.player.playSrc` 由**内置表**登记（与 `musicxx.ui.openSearch`、`musicxx.entry.argv`
> 一起，见 §4.3），所以在命令表里只出现一次；插件里照样用 `musicxx.call("musicxx.player.playSrc", …)`，
> 参数与 §4.3 那张表一致。

> 另外，插件框架自己还有一张 `musicxx.net` 接口表（HTTP / TCP / WebSocket / UDP 的客户端与服务端，
> 客户端支持 https / wss，不经过主程序），见 [plugin-api.md](plugin-api.md) §3.2.6「内置网络能力」。
> JS 的 `musicxx.net.fetch` / `download` 已经用它实现（参数与返回形状与动作一致），
> 动作只留给直接调动作的老插件。

> 从命令行 / MCP 调这几个动作时**没有插件身份**：`musicxx.storage.*` 与
> `musicxx.net.download` 会落到"没有插件 id"的空目录下（应用源码注释里写明了这一点）。
> 要操作某个插件的私有数据，请让插件自己读写，别从命令行代劳。

### 4.6 名字存在、但没有实现的动作

下表都是**插件 SDK 里声明过的常量**（在 SDK 里能写），主程序当前**没有注册**，调它只会得到"动作未注册"：
`musicxx.player.setMediaType`、`musicxx.ui.setEntryBadge`、`musicxx.host.shareText`。

> 曾经的歌单写入动作（`musicxx.library.querySonglist` / `removeSong` / `createSonglist` / `setSongInfo`）
> 现在都有了实现的新名字：`musicxx.playlist.querySongs` / `removeSong` / `create` / `updateSong`；
> 旧名字本身仍没注册。`musicxx.library.search`（站内搜索）没有替代名字，仍未实现。
>
> 加歌用 `musicxx.playlist.addSong`（已实现）。SDK 里**没有** `musicxx.library.addSong` 这个常量，
> 照着写只会得到拼写错误类的报错。

### 4.7 `musicxx-cli` 的糖命令（CLI 名 → 命令 id）

糖命令只做**参数整理与排版**，业务全在上面那张命令表里：

| CLI | 命令 id |
|---|---|
| `status` / `play` / `pause` / `toggle` / `stop` / `next` / `prev` | `status` / `musicxx.player.play` / `pause` / `toggle` / `stop` / `next` / `prev` |
| `kernel [--refresh]` | `kernel.info`（播放内核信息：版本 / 能用的过滤器 / 解码器 / 后缀） |
| `seek <毫秒\|mm:ss\|hh:mm:ss\|N%>` | `musicxx.player.seek` |
| `volume <0~1\|0~100>` / `speed <0.25~8>` / `pitch <0.25~2>` / `loop <模式>` / `quality <名称>` | `musicxx.player.setVolume` / `setSpeed` / `setPitch` / `setLoopMode` / `setQuality` |
| `lrc [--word]` / `lyric [--limit N]` | `lrc.current`（`.lrc` 文本）/ `musicxx.lyric.current.get`（逐行结构化，带时间戳与译文） |
| `songs [--pid N]` / `songlists` / `play-song <sid>` / `play-list <pid> [--index N]` | `musicxx.songlist.querySongs`（不带 `--pid`）/ `musicxx.playlist.querySongs` / `musicxx.playlist.queryLists` / `musicxx.songlist.playSong` / `musicxx.playlist.play` |
| `toast <文本>` | `musicxx.ui.toast` |
| `feature list` / `feature call <id> --args JSON` | `feature.list` / `feature.call` |
| `featurePoint.list` / `featurePoint.call <id>` | `featurePoint.list` / `featurePoint.call`（CLI 名大小写不敏感，写 `featurepoint.list` 也一样） |
| `var list` / `var get <键>` / `var set <键> <值>` | `var.list` / `var.get` / `var.set` |
| `render list` / `render current` / `render select <id>` | `musicxx.render.list` / `current` / `select` |
| `plugin …`（装 / 启停 / 重载 / 卸载 / 日志 / 清日志 / 重启） | `externPlugin.*` |
| `logs [error\|info]` | `app.logs` |
| `commands [status\|install\|uninstall]` | `app.commands`（把 `musicxx` / `musicxx-cli` 登记成系统命令，或查 / 删这份登记） |
| `call <命令 id> [--args JSON]` | 任意命令（含全部动作名） |
| `open <musicxx://…>` | 不转发命令：按链接形态请求主程序（与手机点链接、启动参数 `--uri` 同一条解析） |
| `auth token [--host <ip> --port <端口>]` | 不转发命令：向对方要一把授权密令（只打印，不落盘；对方是「公开」档时告诉你不需要令牌） |
| `list` / `doctor` / `ping` / `watch` / `mcp` / `start` / `help` / `version` | 不转发命令：自描述、自检、事件流、stdio 桥、拉起应用 |

CLI 自己的选项与退出码：

| 选项 | 说明 |
|---|---|
| `--json` | stdout 只放那份 JSON（`{ok, cmd, data}` 或 `{ok:false, error}`，缩进多行）；提示与诊断走 stderr |
| `--endpoint <路径>` | 指定接入点文件（默认 `<系统临时目录>/musicxx.cli.json`） |
| `--port <端口>` | 直接指定端口（跳过接入点文件，排查用） |
| `--timeout <秒>` | 单次请求超时（默认 30；`featurePoint.call` / `featurepoint.list` / `feature` 这些命令自带更长默认值） |
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

### 4.8 事件流（`musicxx-cli watch`）

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

### 5.2 工具全表（当前 44 个）

工具名、说明、inputSchema 与文本形态**对外就是契约**（模型看到的就是它们），
声明全在 `lib/manager/cli/MusicxxMcpTools.dart`。下面按用途分组，
"参数"列里带 `?` 的是可选；工具的参数说明（英文）由服务端随工具一起下发，客户端不需要另外查。
**参数名与命令声明的完全一致**：同一个入口在 MCP 上看到的就是命令行上写的那个名字。

**读当前媒体与歌词**

| 工具 | 参数 | 等价命令 | 文本形态 |
|---|---|---|---|
| `GetMediaInfo` | 无 | `status` | 多行状态快照；没播放过时是 `Musicxx has not played any media yet` |
| `GetCurrentLrc` | 无 | `lrc.current` | 标准 `.lrc` 文本；没有歌词时 `<unknown>` |
| `GetCurrentEnhancedLrc` | 无 | `lrc.current`（`word:true`） | 增强型 `.lrc`（尽量逐字） |
| `GetAppLogs` | `file?`(error/info/both)、`offset?`、`level?`(名字或数值)、`keyword?`、`exclude?`、`limit?`(1~500) | `app.logs` | 一行一条，带分页信息 |

**歌单与播放**

| 工具 | 参数 | 等价命令 | 文本形态 |
|---|---|---|---|
| `GetPlaylistSongs` | 无 | `musicxx.songlist.querySongs` | 歌曲列表（序号 / 名称 / 歌手 / 专辑 / 时长） |
| `GetMineSonglists` | 无 | `musicxx.playlist.queryLists` | 先报条数；网络歌单（「我喜欢」排最前）再本地歌单 |
| `GetSonglistContent` | `pid`(必填)、`type`(local/net，必填) | `musicxx.playlist.querySongs` | 同 `GetPlaylistSongs` |
| `PlayTheSonglist` | `pid`(必填)、`type`(必填)、`songName?` | `playlist.play` | `Success load songlist, current play song name: …` |
| `MediaPlayByInfo` | `info`(必填) | `history.playByInfo` | `Success` / `No related songs found` |
| `PlayMedia` | `src?`／`sources?`（二选一）、`type?`（字符串：`auto` / 来源类型名 / 插件音源类型 id，取值同 §4.3 的 `musicxx.player.playSrc`）、`filterExt?`(bool)、`name?` | `musicxx.player.playSrc` | `Playing: <名字> (<n> media)`；没有可播的来源时工具报错 |
| `OpenSearch` | `type?`(枚举)、`keyword?` | `musicxx.ui.openSearch` | `Search page opened [<类型>] keyword=<词>` |

**播放控制**

| 工具 | 参数 | 等价命令 | 文本形态 |
|---|---|---|---|
| `MediaPlayOrPause` | `operator?`(play/pause；不写 = 切换) | `musicxx.player.play` / `pause` / `toggle` | `[play\|pause\|toggle] Success` |
| `MediaSwitch` | `operator`(必填，next/previous) | `musicxx.player.next` / `prev` | `[next\|previous] Success` |
| `SetCurrentPlaybackPosition` | `position?`(毫秒) **或** `percent?`(0~1) | `musicxx.player.seek` | `Success`；两个都不给报参数不合法 |
| `SetSpeed` | `speed`(必填，0.25~8) | `musicxx.player.setSpeed` | `Success` |
| `SetPitch` | `pitch`(必填，0.25~2；1 = 原调) | `musicxx.player.setPitch` | `Success` |
| `SetAppVolume` | `volume`(必填，0~1) | `musicxx.player.setVolume` | `Success` |
| `SetPlayLoopMode` | `mode`(必填：Order / RealRand / Only / SimulateRand / SimulateRandReflush) | `musicxx.player.setLoopMode` | `Success` |
| `SetTimedPlaybackStop` | `duration?`(秒 1~86400)、`enableStopAfterFinish?`、`enableSleepMode?`、`exitWhenStop?` | `clocking.set` | `Success to set timer (mm:ss)` / `Failed` |
| `RemoveTimedPlaybackStop` | 无 | `clocking.cancel` | `Success` |
| `SwitchForegroundBackground` | `operator`(必填，foreground/background) | `window.foreground` / `background` | `Switch to [foreground\|background] Success` |
| `GetAppLifecycle` | 无 | `app.lifecycle` | `The app is in the foreground (lifecycle state: resumed)` / `The app is in the background for 12s (lifecycle state: paused)` |

**外部插件管理**（细节见 [plugin-agent-cli.md](plugin-agent-cli.md)）

| 工具 | 参数 | 等价命令 |
|---|---|---|
| `InstallExternPlugin` | `path?`（绝对路径）**或** `url?`（下载链接）、`enable?`、`sha256?` | `externPlugin.install` |
| `ReloadExternPlugins` | `id?` | `externPlugin.reload` |
| `RestartExternPluginHost` | 无 | `externPlugin.restart` |
| `GetExternPlugins` | 无 | `externPlugin.list` |
| `UninstallExternPlugin` | `id`(必填)、`keepData?` | `externPlugin.uninstall` |
| `SetExternPluginEnabled` | `id`(必填)、`enabled`(必填) | `externPlugin.setEnabled` |
| `GetExternPluginLogs` | `id?`、`offset?`、`level?`(数值)、`keyword?`、`limit?`(1~500) | `externPlugin.logs` |
| `ClearExternPluginLogs` | 无 | `externPlugin.clearLogs` |

这些工具的文本里都**另外附一份现在的插件清单**（装了什么、启用 / 加载状态、失败原因），
一次调用就能确认结果，不用再问一次。`GetExternPlugins` 还带两行外置插件框架自己的信息：
`framework: <版本>, <日期>, commit <提交>` 与 `framework repository: <GitHub 仓库地址>`
（与 `musicxx-cli version` / `list` / `plugin list`、`/cli/ping`、`/cli/list` 是同一份）。

**局域网（共享与控制）与文件**

| 工具 | 参数 | 等价命令 | 文本形态 |
|---|---|---|---|
| `ListLanDevices` | `availableOnly?`、`refresh?`、`limitServerVersion?` | `lan.clients` | 本机信息一行 + 每台设备四行（设备 / 应用与地址 / 账号与同账号 / 分享与权限与可控制性 / 最后通信） |
| `ScanLanDevices` | `reset?`、`wait?`(ms) | `lan.scan` | 同上（扫描后重新给一份列表） |
| `RunCommandOnDevice` | `clientId?` / `addr?`、`cmd?` + `args?`、`action?` / `id?` + `data?`(object)、`token?` | `lan.cli` | 「发了哪条指令给哪台设备」+ 这次走的通道（`/cli` 或 `/control`）与对方返回的数据。`token` 是对方要求的授权密令（不同账号的设备要带；同账号可以不带） |
| `DownloadFile` | `url`(必填)、`path?`、`dir?`、`fileName?`、`overwrite?`、`sha256?`、`timeoutMs?` | `file.download` | 落盘路径、字节数、内容类型、sha256、重定向后的地址 |
| `ShareLocalFile` | `path`(必填)、`id?`、`title?`、`download?`、`ttlSeconds?` | `file.share.register` | 共享的标题 / 类型 / 路由 / 本机与局域网链接 / 过期时间 |
| `ListSharedFiles` | `id?` | `file.share.list` | 每条共享一行（标题 / 类型 / id / 路径 / 访问次数）+ 链接 |
| `UnshareLocalFile` | `id?` / `all?` | `file.share.unregister` | `Removed N shared file(s).` |

设备发现与指令转发需要两台设备都开着「共享与控制」，且对方「允许远程控制本机」的权限包含本机账号
（仅自己账号 = 双方登录同一个账号）；`lan.cli` 默认转发到对方的 `/cli`（命令面），只有命令面还没有对应命令的那几条动作才走 `/control` 数字指令，没有新增线协议。
本机文件对外下载走 `/share/<id>`（见 §6），目录挂在 `/share/<id>/` 下（相对路径不许穿出去），route id 就是权限，链接不要随便发出去；注册只在本次运行有效，程序重启即消失。

**功能点与能力**

| 工具 | 参数 | 等价命令 | 文本形态 |
|---|---|---|---|
| `GetFeatureProviders` | `id?`、`provided?` | `featurePoint.list` | 每个功能点一段：类型、有没有实现、可不可以调、逐行列出实现（生效 / 备选 / 关闭）+ `callDoc` + 被调过几次；有前链 / 后链登记时多一行「前链 / 后链：N 个」，有主程序广播订阅时多一行「广播：N 个订阅」 |
| `CallFeaturePoint` | `id`(必填)、`args?`、`refresh?` | `featurePoint.call` | 结果 JSON，**超大字符串字段压成摘要**（见下）；失败时的 `error` 与 `featurePoint.call` 同一套取值 |
| `GetAudioSpectrum` | 见能力声明（`path?`、`start?`、`duration?`、`bandCount?` 等） | `feature.call`（id = `GetAudioSpectrum`） | 文本表（每行一个时间步） |
| `AnalyseAudioChorus` | 见能力声明 | `feature.call`（id = `AnalyseAudioChorus`） | 文本结论（副歌区段） |
| `AnalyseImageMainColors` | 见能力声明（`path?` / `url?`） | `feature.call`（id = `AnalyseImageMainColors`） | 主色 + 4 组色值的文本 |
| `GetMediaKernelInfo` | `part?`(all / versions / filters / decoders / formats)、`refresh?` | `feature.call`（id = `GetMediaKernelInfo`）/ `kernel.info` | 分段文本：mpv/ffmpeg 版本、能写进 `af` / `vf` 的过滤器（含说明）、解码器（按编解码器分组，标出硬件）、应用认得的音视频后缀；`part` 用来只拿其中一段（全量文本里过滤器与解码器加起来几百行） |
| `ListMediaSources` | 无 | `source.list` | 插件声明的音源类型（id / 展示名 / 地址写法）+ 可浏览分类（按类型分组，每个分类一行）+ 音源解析链现状（生效 / 备选 / 关闭） |

后三个工具的名字**就是能力 id**，工具说明与参数也来自能力自己的声明 —— 新增能力会自动变成
新工具，工具清单会跟着长。

**MCP 只覆盖一部分命令**（工具表是手工声明的，不是命令表的镜像）：下面这些命令**没有对应的 MCP 工具**，
在 MCP 客户端里调不到 —— 要调它们请用命令行（`musicxx-cli call <命令 id> --args '<JSON>'`）
或 POST `/cli`：

| 没有 MCP 工具的命令 | 替代做法 |
|---|---|
| `var.list` / `var.get` / `var.set` | `musicxx-cli var list` / `var get <键>` / `var set <键> <值>`，或 `/cli` |
| `feature.list` / `feature.call` | 每个能力已经单独变成工具（`GetAudioSpectrum` …），原始清单用 `musicxx-cli feature list` |
| `app.commands` | `musicxx-cli commands [status\|install\|uninstall]` |
| `musicxx.entry.argv` | 命令行 `musicxx-cli open` / `call`，或 `/cli` 的 `{"argv": […]}` |
| 歌单 / 播放列表的写入动作（`musicxx.playlist.create` / `removeSong` / `updateSong`、`musicxx.songlist.addSong` / `removeSong` / `updateSong`） | `/cli`（`musicxx-cli call …`）—— 插件里仍然用 `musicxx.call` |
| `musicxx.lyric.*`（读与写都没有工具）、`musicxx.ui.*`（`openSearch` 除外，它有 `OpenSearch` 工具）、`musicxx.storage.*`、`musicxx.net.*`、`musicxx.render.*`、`musicxx.media.*`（动作本身没有工具：频谱 / 副歌 / 主色调 / 内核信息用 §5.2 里那几个同名能力工具）、`musicxx.source.define` / `undefine`、`musicxx.hook.handlers`、`musicxx.host.*`、`musicxx.stats.*` | 同上（`/cli` 或插件内 `musicxx.call`） |

判断某个命令有没有 MCP 工具，看 `GET /cli/list` 的 `commands[].id` 与 `tools/list` 的工具名：
**工具名与命令 id 不是一回事**（`GetPlaylistSongs` ↔ `musicxx.songlist.querySongs`），
只有 §5.2 表里列出的那些能对上。

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
| `/connectEvent` | WebSocket | 事件流（§4.8 / §6.4） | 本机放行；非本机过同一道门，但**握手不弹窗**（没身份直接 403，先取令牌再 `?token=`） |
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

`file.share.register` 把一个本机文件（或目录）登记成一条共享，
`http://<地址>:<端口>/share/<id>`（局域网地址在结果的 `lanUrls` 里）。

| 项 | 说明 |
|---|---|
| 方法 | `GET` / `HEAD`（其它回 405） |
| Range | 支持（响应 206 + `Content-Range`；越界 416 + `bytes */<长度>`），可用于播放器与断点续传 |
| 文件项 | `/share/<id>` 给文件（末尾多一个 `/` 也一样）；`?download=1` 或注册时 `download: true` 带 `Content-Disposition: attachment` |
| 目录项 | 目录挂在 `/share/<id>/<相对路径>` 下；`/share/<id>/` 给索引（HTML，`?format=json` 给结构化列表） |
| 越界 | 相对路径逐段解码后，`..`、带分隔符的段、绝对路径一律拒绝；归一化后必须仍在挂载目录内（符号链接指出去也拒绝）→ 403；未知 id / 已失效 404 |
| 访问控制 | route id 随机 32 位十六进制（可自定义），**猜不到 = 分享链接即权限**；非本机请求还要求「共享与控制」开着；没有其他写/删能力 |
| 失效 | 注册时 `ttlSeconds` 到期，或 `file.share.unregister`；**应用退出即全部失效**（不落库） |

常用于：把刚 `file.download` 下来的插件包交给另一台设备装、把本机录制的音频交给局域网，
或让另一个 agent 从这台机器取文件。

## 7. 边界与安全

- **装插件等于执行任意代码**：`/mcp` 与 `/cli` 都能装插件、启用插件（动态库插件与主程序
  同进程运行）。`/cli` 在**所有平台**都开（含手机）。两者都走**同一道鉴权门**
  （`lib/manager/netAuth/MusicxxNetAuth.dart`）：本机回环直接放行；非本机按「本机 → 服务没开 →
  「公开」档 → 授权密令 → `sign` 同账号 → 弹窗」逐条判，谁都没通过就是 403（`/mcp` 是 JSON-RPC
  `code: -32001`）。弹窗授权那一次回写 `X-Musicxx-Token`，客户端存下来复用；`/mcp` 的 CORS 是 `*`，
  所以**不要把端口暴露到不可信网络**；
- **链接与启动参数也算本机入口**：`musicxx://call/?cmd=…` 能调任意入口，而一条链接任何网页 /
  应用都能触发。所以链接里请求 `externPlugin.*`（装 / 卸 / 启用 / 重载 / 重启插件）与 `file.*`
  （往本机写文件、把本机文件开放给外部下载）这类管理入口时
  **先弹窗问用户**（`MusicxxEntries_c.uriSensitiveCmdPrefixes` + `askLinkSensitiveDo`）：列出命令与
  关键参数，点「允许」才执行，关掉窗口算拒绝；本机通道（命令行 / MCP / `/cli` / 用户自己敲的
  `--cmd`）不问。应用内「设置 → 界面 → 调用快捷链接」粘贴的链接（`manual` 通道，§1.1.1）与 applink
  同一条规则。新增“能装东西 / 能执行代码 / 能读写本机文件”的入口时要一起加进这张表；
- **`/share/<id>` 是“拿到链接就能下载”**：id 猜不到且可设过期时间，但没有账号校验；
  注册的是**文件或目录**（目录挂在 `/share/<id>/` 下，相对路径逐段校验、出不了挂载点），
  注册只在本次运行有效，**程序重启即全部消失**。
  不要把它当成需要鉴权的文件接口；需要限制访问时缩短 `ttlSeconds` 并及时 `file.share.unregister`；
- **插件装不了插件**：安装 / 重载 / 重启插件框架只有命令行与 MCP 有（插件的动作表里没有
  这几条），这是有意的：否则 JS插件能把自己提升到原生权限；
- **`/cli` 与 `/control` 是两套东西**：`/cli` 是命令面（全量命令、结构化结果，本机与远程都能调，
  远程要过远控权限）；`/control` 是局域网设备之间的老远程控制（数字 id、要签名、面窄）。
  本机与 agent 要下发命令请走 `/cli`（跨设备用 `lan.cli` 转发）；
- **卸载不可逆**：`externPlugin.uninstall` 默认连插件私有数据一起删（`keepData` 可保留）；
- **日志有上限**：插件日志是内存里的环形缓冲（最多 500 条，退出即失去）；应用自己的日志写在
  磁盘（`error.log` / `info.log`，按大小裁剪），最后几行可能还在写入缓冲里
  （结果里的 `flushedHint`）；
- **功能点调用零副作用**：`featurePoint.call` 只读值仓，不写值仓、不改应用状态 ——
  调一次封面取色不会换掉用户看到的背景色；
- **一次只让一个 agent 装 / 卸同一个插件**：两台同时操作会出现"文件是这次、记录是那次"的竞争。

## 8. 排障

| 现象 | 处理 |
|---|---|
| 命令行退出码 2（"拟声未在运行"） | `musicxx-cli doctor` 看接入点文件与连接；或 `--start` / `musicxx-cli start` 先拉起 |
| 接入点文件在但连不上 | 那是上次运行留下的残留（退出不清理），按"没在运行"处理 |
| 找不到 `musicxx-cli` | 看应用里「设置 → 共享与控制 → 命令行接口（CLI）」；它必须与主程序同目录（`--start` / `mcp` 靠这一点找主程序） |
| `app_not_ready`（503） | 应用还在启动收尾，稍后重试（命令行本身会先等 15 秒） |
| 403 | 非本机请求：开「共享与控制 → 允许远程控制本机」、在本机执行，或用授权密令（`musicxx-cli auth token --host <ip> --port <端口>` 要一把） |
| 退出码 5（协议不匹配） | 命令行与主程序不是同一次构建的产物，换成一起编译出来的那份 |
| MCP 客户端连不上 | 先 `musicxx-cli doctor`，再 `cd cli && dart run tool/mcp_probe.dart`（不经过客户端直接跑一遍 initialize / tools/list / tools/call，会说明卡在哪一步），最后 `musicxx-cli mcp --verbose` |
| 命令名/参数不确定 | `musicxx-cli list --json`（或 GET `/cli/list`）拿自描述清单（含 `entries` 链接形态），不要猜 |
| 点链接 / 启动参数没反应 | 应用日志里搜 `[入口]`：解析失败与不认识的链接都会写一行可读原因。桌面端先用 `musicxx-cli open "<链接>"` 复现（与点链接同一条解析），启动参数用 `musicxx-cli call <入口 id> --args '<JSON>'` 复现 |
| 链接里调插件管理没反应 | 链接里请求 `externPlugin.*` / `file.*` 会先弹确认框（§7），用户关掉窗口 = 拒绝；拒绝时日志里有一行「用户没有允许管理类入口」。本机通道（`musicxx-cli plugin …` / MCP / `/cli`）不问 |
| 动作答复"动作未注册" | 它在 §4.6 那张"名字存在但没实现"的表里；换用别的通道（能力导出 / 功能点）或看 [plugin-api.md](plugin-api.md) §3.4 |
| 工具/能力是不是最新 | 工具条数会随能力注册表增长：以 `tools/list`（或 `feature.list`）的实际结果为准 |
| 插件相关的排障 | [plugin-agent-cli.md](plugin-agent-cli.md) §10（`plugin list` / `plugin logs` / 主程序卡住的判断） |

---

**相关文档**：[plugin-api.md](plugin-api.md)（插件侧接口手册）、
[plugin-agent-cli.md](plugin-agent-cli.md)（用命令行 / MCP 管插件）、
[plugin-vars.md](plugin-vars.md)（变量）、[plugin-hooks.md](plugin-hooks.md)（钩子总表）、
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
- 动作有没有实现：看主程序 `lib/plugin/externPlugin/ExternActions.dart` 里有没有 `_add(...)` 那一行
  （SDK 常量存在不等于已实现）；
- 钩子已接入 / 未接入：`tools/hooks.def.json` 的 `wired` 字段（`plugin-hooks.md` 由它生成，
  `dart run tools/gen_contract.dart --check` 校验）。
