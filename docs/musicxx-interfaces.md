# musicxx 的对外接口：MCP / 命令行 / 本机 HTTP

> 这篇是 **musicxx（主程序）对外暴露的全部接口**的总表：谁在什么场景用哪条通道、怎么连上、
> 请求与响应是什么形状、权限边界在哪、出问题从哪里看。
>
> 插件自己的接口（动作 / 变量 / 功能点 / 钩子 / 能力 / 界面）在 [plugin-api.md](plugin-api.md)；
> 用命令行装插件、改完刷新、看插件日志在 [plugin-agent-cli.md](plugin-agent-cli.md)。

**术语**：**主程序**指 musicxx（Flutter/Dart 业务层）；**插件框架**指外置插件的 C++ 层。

## 1. 五条通道，先选对

| 通道 | 谁在用 | 入口 | 说明 |
|---|---|---|---|
| **MCP 工具** | AI 客户端（Claude Code / Desktop / Cursor / 自研 agent） | `http://127.0.0.1:<端口>/mcp`（Streamable HTTP），或 `musicxx-cli mcp`（stdio 桥） | §5 |
| **本机命令行** | 人、脚本、agent | `musicxx-cli`（独立可执行文件）→ 主程序 `/cli` | §4 |
| **插件内部** | 装在应用里的动态库插件 / JS插件 | 动作 / 变量 / 功能点 / 钩子 / 能力 | [plugin-api.md](plugin-api.md) |
| **局域网设备之间** | 另一台装了 musicxx / 流明的设备（控制页、一起听、资源代理） | `/control`、`/connectEvent`、`/getSrc`、`/parseSrc`、`/listenTogether`、`/ws/*` | §6 |
| **设备发现** | 同网段的设备互相发现 | mDNS 服务 `_boolxx-service._udp` + HTTP `/active` | §6.1 |

**同一件事在不同通道上的名字**（三条通道调的是**同一份实现**：命令注册表
`lib/manager/cli/`，MCP 与命令行都只是它的两个外壳）：

| 要做的事 | MCP | 命令行 | 插件动作 |
|---|---|---|---|
| 读当前媒体信息 | `GetMediaInfo` | `musicxx-cli status`（命令 `status`） | —（读状态镜像 `musicxx.state.player`） |
| 暂停 | `MediaPlayOrPause`（`operator:"pause"`） | `musicxx-cli pause`（命令 `musicxx.player.pause`） | `musicxx.player.pause` |
| 副歌区段 | `AnalyseAudioChorus`（能力工具） | `musicxx-cli feature call AnalyseAudioChorus` | `musicxx.feature.call` |
| 功能点清单 | `GetFeatureProviders` | `musicxx-cli featurePoint.list` | `musicxx.feature.point.list` |
| 装插件 | `InstallExternPlugin` | `musicxx-cli plugin install` | **没有这个动作**（见 §7） |

## 2. 先决条件与连接

| 条件 | 说明 |
|---|---|
| 主程序在运行 | 五条通道都由主程序的 HTTP 服务提供。没在运行时命令行退出码 2（加 `--start` 可以先拉起） |
| HTTP 服务已绑定 | 主程序启动时就绑定（`InternetAddress.anyIPv4`，端口按设置，默认随机）：启动日志里有一行 `[HttpServer] 已启动: http://127.0.0.1:<端口>/` |
| `/cli` 只在桌面端 | Android / iOS 没有 `/cli`（按平台判断）；`/mcp` 与局域网那几条在服务起来后都能用 |
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

| 入口 | 本机回环 | 非本机 |
|---|---|---|
| `/cli`（含 `/cli/list`、`/cli/ping`） | 直接可用，不需要任何配置 | 要「共享与控制 → 允许远程控制本机」判定通过（`?sign=`），否则 403 |
| `/mcp` | 可用（**本身不带鉴权**） | 只要端口连得上就能调，见 §7 |
| `/control` | **仍要签名** | 要签名（`isAcceptRemoteCtrl(sign)`） |
| `/connectEvent`（事件流 WebSocket） | 放行（`musicxx-cli watch` 靠它） | 要签名 |
| `/getSrc`、`/parseSrc` | 放行 | 要「局域网服务」开着、且服务模式允许对外服务 |
| `/listenTogether`、`/ws/listenTogether` | — | 不经过远控权限校验：跨账号邀请是否接受由**被邀请方**的确认弹窗决定 |
| `/active` | 放行 | 放行（未开局域网服务时只回"存活"，不吐设备信息） |

## 3. `/cli` 的协议形状

```
GET  /cli   |  /cli/list    自描述：命令、参数、来源、就绪状态
GET  /cli/ping              存活 + 是否就绪 + 端点信息
POST /cli                   {"cmd": "<命令 id>", "args": {…}} → 执行一条命令
```

响应体只有一种形状：`{ok, cmd?, data? | error?}`。

| HTTP 状态码 | 什么时候出现 |
|---|---|
| 200 | 请求合理。**命令本身的失败也是 200 + `ok:false`**，调用方只看 `ok` |
| 400 | 请求体不是 JSON 对象 / 缺 `cmd` / `args` 不是对象 |
| 403 | 非本机且没有远程控制权限 |
| 404 | 路径不认识 |
| 405 | 方法不对（只接受 GET / POST） |
| 503 | 应用还在启动收尾（`app_not_ready`；默认先等 15 秒才这么答） |

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

## 4. 命令面（当前 74 条 = 动作 50 + 应用命令 19 + 能力导出 2 + 变量 3）

### 4.1 命令从哪来

| 来源（`kind`） | 条数 | 谁登记的 | 内容 |
|---|---|---|---|
| `builtin` | 19 | `MusicxxCliCommandsSetup` + `MusicxxAppCommands` | `status`、歌词、歌单、播放记录、定时停止、前后台、外部插件管理、日志、功能点 |
| `action` | 50 | 主程序动作表 | 与插件、MCP 共用同一份实现；**命令 id 就是动作名** |
| `feature` | 2 | 应用能力导出 | `feature.list` / `feature.call` |
| `variable` | 3 | 变量目录 | `var.list` / `var.get` / `var.set` |

命令表**自描述**：`musicxx-cli list --json`（或 GET `/cli/list`）里 `commands[]` 每项是
`{id, title, kind, description?, params: []}`。写脚本时先问一次，不要硬编码参数表
（条数会随能力注册表增长）。

### 4.2 参数怎么解析

声明了 `params` 的命令按声明解析，声明字段与 MCP 的 inputSchema 是**同一份**
（`MusicxxFeatureParam_c`）：

| 字段 | 含义 |
|---|---|
| `name` / `type` | 参数名；类型 `string` / `number` / `integer` / `boolean` / `enum` |
| `required` | 必填（没有默认值的参数通常是必填） |
| `defaultValue` | 缺省值（不传就用它） |
| `minimum` / `maximum` | 数值范围 |
| `options` | `enum` 的候选值 |

没有声明 `params` 的命令（动作那一批）把入参**原样交给动作**，动作自己做宽松解析
（数字与字符串可互转、布尔认 `true` / `false` 文本）。参数不对时答复形如
`{"ok":false,"error":"参数 xxx 非法: …"}`。

### 4.3 内置与应用命令（19 条）

| 命令 | 参数 | `data` |
|---|---|---|
| `status` | 无 | 当前媒体的结构化快照（标题 / 歌手 / 专辑 / 总时长 / 进度 / 状态 / 循环 / 速度 / 应用内音量 / 文件名 / 格式 / 采样率 / 码率）；没有播放过时带 `hasMedia:false` |
| `lrc.current` | `word?`(bool) | `{hasLyric, word, lineCount, text?}`：当前歌词的 `.lrc` 文本，`word:true` 尽量给逐字（增强型） |
| `playlist.play` | `pid`(int,必填)、`type?`(local/net)、`index?`(int)、`songName?`(string) | `{pid, sid, name, artist}`：播歌单里的一首（按 `index` 或名字包含匹配找，都不给播第一首） |
| `history.playByInfo` | `info`(string,必填) | `{found, sid?, name?, artist?}`：在播放记录里按标题 / 歌手匹配最相关的一首并播放 |
| `clocking.set` | `duration?`(秒 1~86400)、`enableStopAfterFinish?`、`enableSleepMode?`、`exitWhenStop?` | `{started, durationSeconds, durationText, …}`；什么都不给时沿用用户上次的设置（没有则 60 分钟） |
| `clocking.cancel` | 无 | `{ok:true}`（没设置过也算成功） |
| `window.foreground` / `window.background` | 无 | `{ok:true}` |
| `externPlugin.list` | 无 | 插件框架现状 + 插件清单（字段见本节末尾那张表） |
| `externPlugin.install` | `path`(string,必填)、`enable?`(bool) | `{ok, id, version, kind, author?, description?, permissions, enabled, path, sha256, isUpgrade, target, targetEntry, targets, fileCount, totalBytes}` + `status` |
| `externPlugin.uninstall` | `id`(必填)、`keepData?`(bool) | 卸载结果 + 插件清单 |
| `externPlugin.setEnabled` | `id`(必填)、`enabled`(必填) | `{id, enabled, loaded, …}` + 插件清单 |
| `externPlugin.reload` | `id?`（不给 = 重扫目录并重载全部启用中的） | `{reloaded:[id…], failed:{id:原因}, …}` + 插件清单 |
| `externPlugin.restart` | 无 | 重启后的状态（`hostStarted` / `pluginCount` / `loadedCount` / `restarted`） |
| `externPlugin.logs` | `id?`、`offset?`(≥0)、`level?`(数值 800/900/1000)、`keyword?`、`limit?`(1~500，默认 50) | `{ok:true, buffer, id, level, keyword, logs:[{ts, time, type, plugin, detail}…], total, offset, count, hasMore}`，**新的在前**；`total` 是过滤后的条数 |
| `externPlugin.clearLogs` | 无 | `{cleared}` |
| `app.logs` | `file?`(error/info/both)、`offset?`、`level?`(名字或数值)、`keyword?`、`exclude?`、`limit?`(1~500) | `{logs:[{timeText, level, file, source, text}…], total, hasMore, fileSize, parseErrors, flushedHint}`，**新的在前** |
| `featurePoint.list` | `id?`、`provided?`(bool) | `{ok:true, points:[{id, title, type, available, callable, rows, callDoc?, callStat?}…]}` |
| `featurePoint.call` | `id`(必填)、`args?`(object)、`refresh?`(bool) | `{ok, id, value, by, identity, fromCache, ms}`；失败 `{ok:false, error, message}`（`error` 取 `not_callable` / `bad_args` / `no_impl` / `disabled` / `busy` / `failed`） |

插件清单的字段（`externPlugin.*` 的结果里都有，安装结果里它在 `status` 下）：
`hostStarted`、`platformSupported`、`supportPlus`、`enabled`、`safeMode`、`autoSafeMode`、
`lastError`、`pluginDir`、`pluginCount`、`loadedCount`、`framework`（版本 / 提交 / 日期 /
`libraryVersion` / `libraryMatches`）、`capabilityCall`（`async` 或 `hostTooOld`）、
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

变量清单与语义见 [plugin-vars.md](plugin-vars.md)。当前三个导出能力：
`GetAudioSpectrum`（整曲频谱，文本表）、`AnalyseAudioChorus`（副歌区段）、
`AnalyseImageMainColors`（图片主色调）。它们的参数由能力自己声明，
`feature.list` / MCP 的工具清单里就是同一份。

### 4.5 主程序动作（50 条，命令 id = 动作名）

参数与返回的完整说明在 [plugin-api.md](plugin-api.md) §3.2（本文不重复，避免两处不一致）。
这一节只给"有哪些、大致做什么"：

| 域 | 动作 |
|---|---|
| 播放（11） | `musicxx.player.play` / `pause` / `toggle` / `stop` / `next` / `prev` / `seek` / `setVolume` / `setSpeed` / `setQuality` / `setLoopMode` |
| 歌单与歌曲（4） | `musicxx.library.querySonglists` / `querySongs` / `playSong` / `playSonglist` |
| 歌词（1） | `musicxx.lyrics.getCurrent` |
| 提示与页面（4） | `musicxx.ui.notify` / `toast` / `dialog` / `openRoute` |
| 全局字体（3） | `musicxx.ui.setFontFamily` / `getFontFamily` / `loadFont` |
| 插件私有存储（4） | `musicxx.storage.get` / `set` / `delete` / `list` |
| 网络（2） | `musicxx.net.fetch` / `net.download` |
| 渲染槽位（3） | `musicxx.render.list` / `current` / `select` |
| 封面与频谱（6） | `musicxx.media.palette` / `palette.set` / `cover` / `bindImage` / `unbindImage` / `spectrum` |
| 能力导出（2） | `musicxx.feature.list` / `feature.call` |
| 功能点（4） | `musicxx.feature.point.list` / `call` / `define` / `undefine` |
| 钩子自省（1） | `musicxx.hook.handlers` |
| 宿主（3） | `musicxx.host.openUrl` / `clipboard` / `getPath` |
| 统计上报（2） | `musicxx.stats.reportMemory` / `reportMetric` |

> 从命令行 / MCP 调这几个动作时**没有插件身份**：`musicxx.storage.*` 与
> `musicxx.net.download` 会落到"没有插件 id"的空目录下（应用源码注释里写明了这一点）。
> 要操作某个插件的私有数据，请让插件自己读写，别从命令行代劳。

### 4.6 名字存在、但没有实现的动作

下面这些名字在插件 SDK 里能写，主程序当前**没有注册**，调它只会得到"动作未注册"：
`musicxx.player.setPitch` / `setMediaType`、`musicxx.library.querySonglist` / `addSong` /
`removeSong` / `createSonglist` / `setSongInfo` / `search`、`musicxx.lyrics.getBySrc` /
`set` / `sync` / `search`、`musicxx.ui.setEntryBadge`、`musicxx.host.shareText`。

### 4.7 `musicxx-cli` 的糖命令（CLI 名 → 命令 id）

糖命令只做**参数整理与排版**，业务全在上面那张命令表里：

| CLI | 命令 id |
|---|---|
| `status` / `play` / `pause` / `toggle` / `stop` / `next` / `prev` | `status` / `musicxx.player.play` / `pause` / `toggle` / `stop` / `next` / `prev` |
| `seek <毫秒\|mm:ss\|hh:mm:ss\|N%>` | `musicxx.player.seek` |
| `volume <0~1\|0~100>` / `speed <0.25~8>` / `loop <模式>` / `quality <名称>` | `musicxx.player.setVolume` / `setSpeed` / `setLoopMode` / `setQuality` |
| `lrc [--word]` | `lrc.current` |
| `songs [--pid N]` / `songlists` / `play-song <sid>` / `play-list <pid> [--index N]` | `musicxx.library.querySongs` / `querySonglists` / `playSong` / `playlist.play` |
| `toast <文本>` | `musicxx.ui.toast` |
| `feature list` / `feature call <id> --args JSON` | `feature.list` / `feature.call` |
| `featurePoint.list` / `featurePoint.call <id>` | `featurePoint.list` / `featurePoint.call` |
| `var list` / `var get <键>` / `var set <键> <值>` | `var.list` / `var.get` / `var.set` |
| `render list` / `render current` / `render select <id>` | `musicxx.render.list` / `current` / `select` |
| `plugin …`（装 / 启停 / 重载 / 卸载 / 日志 / 清日志 / 重启） | `externPlugin.*` |
| `logs [error\|info]` | `app.logs` |
| `call <命令 id> [--args JSON]` | 任意命令（含全部动作名） |
| `list` / `doctor` / `ping` / `watch` / `mcp` / `start` / `help` / `version` | 不转发命令：自描述、自检、事件流、stdio 桥、拉起应用 |

CLI 自己的选项与退出码：

| 选项 | 说明 |
|---|---|
| `--json` | stdout 只放那份 JSON（`{ok, cmd, data}` 或 `{ok:false, error}`，缩进多行）；提示与诊断走 stderr |
| `--endpoint <路径>` | 指定接入点文件（默认 `<系统临时目录>/musicxx.cli.json`） |
| `--port <端口>` | 直接指定端口（跳过接入点文件，排查用） |
| `--timeout <秒>` | 单次请求超时（默认 30；`featurePoint.call` 这类命令默认 60） |
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

### 5.2 工具全表（当前 31 个）

工具名、说明、inputSchema 与文本形态**对外就是契约**（模型看到的就是它们），
声明全在 `lib/manager/cli/MusicxxMcpTools.dart`。下面按用途分组，
"参数"列里带 `?` 的是可选；工具的参数说明（英文）由服务端随工具一起下发，客户端不需要另外查。

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
| `GetPlaylistSongs` | 无 | `musicxx.library.querySongs` | 歌曲列表（序号 / 名称 / 歌手 / 专辑 / 时长） |
| `GetMineSonglists` | 无 | `musicxx.library.querySonglists` | 先报条数；网络歌单（「我喜欢」排最前）再本地歌单 |
| `GetSonglistContent` | `pid`(必填)、`type`(local/net，必填) | `musicxx.library.querySongs` | 同 `GetPlaylistSongs` |
| `PlayTheSonglist` | `pid`(必填)、`type`(必填)、`songName?` | `playlist.play` | `Success load songlist, current play song name: …` |
| `MediaPlayByInfo` | `info`(必填) | `history.playByInfo` | `Success` / `No related songs found` |

**播放控制**

| 工具 | 参数 | 等价命令 | 文本形态 |
|---|---|---|---|
| `MediaPlayOrPause` | `operator?`(play/pause；不写 = 切换) | `musicxx.player.play` / `pause` / `toggle` | `[play\|pause\|toggle] Success` |
| `MediaSwitch` | `operator`(必填，next/previous) | `musicxx.player.next` / `prev` | `[next\|previous] Success` |
| `SetCurrentPlaybackPosition` | `position?`(毫秒) **或** `percent?`(0~1) | `musicxx.player.seek` | `Success`；两个都不给报参数不合法 |
| `SetSpeed` | `speed`(必填，0.25~8) | `musicxx.player.setSpeed` | `Success` |
| `SetAppVolume` | `volume`(必填，0~1) | `musicxx.player.setVolume` | `Success` |
| `SetPlayLoopMode` | `mode`(必填：Order / RealRand / Only / SimulateRand / SimulateRandReflush) | `musicxx.player.setLoopMode` | `Success` |
| `SetTimedPlaybackStop` | `duration?`(秒 1~86400)、`enableStopAfterFinish?`、`enableSleepMode?`、`exitWhenStop?` | `clocking.set` | `Success to set timer (mm:ss)` / `Failed` |
| `RemoveTimedPlaybackStop` | 无 | `clocking.cancel` | `Success` |
| `SwitchForegroundBackground` | `operator`(必填，foreground/background) | `window.foreground` / `background` | `Switch to [foreground\|background] Success` |

**外部插件管理**（细节见 [plugin-agent-cli.md](plugin-agent-cli.md)）

| 工具 | 参数 | 等价命令 |
|---|---|---|
| `InstallExternPlugin` | `path`(必填，绝对路径)、`enable?` | `externPlugin.install` |
| `ReloadExternPlugins` | `id?` | `externPlugin.reload` |
| `RestartExternPluginHost` | 无 | `externPlugin.restart` |
| `GetExternPlugins` | 无 | `externPlugin.list` |
| `UninstallExternPlugin` | `id`(必填)、`keepData?` | `externPlugin.uninstall` |
| `SetExternPluginEnabled` | `id`(必填)、`enabled`(必填) | `externPlugin.setEnabled` |
| `GetExternPluginLogs` | `id?`、`offset?`、`level?`(数值)、`keyword?`、`limit?`(1~500) | `externPlugin.logs` |
| `ClearExternPluginLogs` | 无 | `externPlugin.clearLogs` |

这些工具的文本里都**另外附一份现在的插件清单**（装了什么、启用 / 加载状态、失败原因），
一次调用就能确认结果，不用再问一次。

**功能点与能力**

| 工具 | 参数 | 等价命令 | 文本形态 |
|---|---|---|---|
| `GetFeatureProviders` | `id?`、`provided?` | `featurePoint.list` | 每个功能点一段：类型、有没有实现、可不可以调、逐行列出实现（生效 / 备选 / 关闭）+ `callDoc` + 被调过几次 |
| `CallFeaturePoint` | `id`(必填)、`args?`、`refresh?` | `featurePoint.call` | 结果 JSON，**超大字符串字段压成摘要**（见下） |
| `GetAudioSpectrum` | 见能力声明（`path?`、`start?`、`duration?`、`bandCount?` 等） | `feature.call`（id = `GetAudioSpectrum`） | 文本表（每行一个时间步） |
| `AnalyseAudioChorus` | 见能力声明 | `feature.call`（id = `AnalyseAudioChorus`） | 文本结论（副歌区段） |
| `AnalyseImageMainColors` | 见能力声明（`path?` / `url?`） | `feature.call`（id = `AnalyseImageMainColors`） | 主色 + 4 组色值的文本 |

后三个工具的名字**就是能力 id**，工具说明与参数也来自能力自己的声明 —— 新增能力会自动变成
新工具，工具清单会跟着长。

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

## 6. 本机 HTTP / WebSocket 服务

同一个 HTTP 服务上挂着全部本地与局域网入口（`InternetAddress.anyIPv4`，端口就是接入点文件里那个）：

| 路径 | 方法 | 用途 | 权限 |
|---|---|---|---|
| `/cli`、`/cli/list`、`/cli/ping` | GET / POST | 本机命令行（§3） | 本机放行；非本机要远控权限 |
| `/mcp` | POST / GET / DELETE / OPTIONS | MCP（§5） | 不带鉴权 |
| `/active` | POST | 设备信息 / 存活应答（§6.1） | 放行（受局域网服务开关影响返回内容） |
| `/control` | POST | 局域网控制指令（数字命令 id，§6.2） | 要签名 |
| `/connectEvent` | WebSocket | 事件流（§4.8 / §6.4） | 本机放行；非本机要签名 |
| `/getSrc` | GET | 资源代理：把本机能拿到的音源交给请求方（§6.3） | 本机放行；非本机要服务模式允许对外 |
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
`{"code":2000,"data":…,"tip":""}` 或 `{"code":4010,…}`（参数缺失）等 `XXResponse_c` 形状。

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

## 7. 边界与安全

- **装插件等于执行任意代码**：`/mcp` 与 `/cli` 都能装插件、启用插件（动态库插件与主程序
  同进程运行）。命令行只对本机开放，**MCP 本身不带鉴权** —— 端口能被连上就能调，
  请把「共享与控制」的对外服务范围收紧，不要把它暴露到不可信网络；
- **插件装不了插件**：安装 / 重载 / 重启插件框架只有命令行与 MCP 有（插件的动作表里没有
  这几条），这是有意的：否则 JS插件能把自己提升到原生权限；
- **`/cli` 与 `/control` 是两套东西**：`/cli` 是"第二进程 → 主进程拿结果"（本机、结构化、
  命令面全）；`/control` 是局域网设备之间的远程控制（数字 id、要签名、面窄）。本机要下发
  命令请走 `/cli`；
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
| 403 | 非本机请求：开「共享与控制 → 允许远程控制本机」，或在本机执行 |
| 退出码 5（协议不匹配） | 命令行与主程序不是同一次构建的产物，换成一起编译出来的那份 |
| MCP 客户端连不上 | 先 `musicxx-cli doctor`，再 `cd cli && dart run tool/mcp_probe.dart`（不经过客户端直接跑一遍 initialize / tools/list / tools/call，会说明卡在哪一步），最后 `musicxx-cli mcp --verbose` |
| 命令名/参数不确定 | `musicxx-cli list --json`（或 GET `/cli/list`）拿自描述清单，不要猜 |
| 动作答复"动作未注册" | 它在 §4.6 那张"名字存在但没实现"的表里；换用别的通道（能力导出 / 功能点）或看 [plugin-api.md](plugin-api.md) §3.3 |
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
本机服务路由在 `lib/manager/NetServerExt.dart` 与 `lib/share/manager/NetServer.dart`，
命令行客户端在 `cli/`（协议与事件名表要跟主程序对齐）。这一节的内容都是从这些文件读出来的。
