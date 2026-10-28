# example_ftp_source —— FTP 音源示例插件

一个**完整可用**的插件音源示例：用户填 FTP 账号 → 在歌单的「添加歌曲」里浏览目录、多选 → 播放时
由插件把 FTP 文件转成本机 HTTP 地址交给播放器。

它把"插件提供一个自己的音源"需要的每一步都写了一遍，可以直接照抄：

| 能力 | 在这个插件里 |
|---|---|
| 声明自己的音源类型 | `musicxx.source.define`（`plugin.example_ftp_source.audio`） |
| 账号 / 配置存哪 | `musicxx.storage`（插件私有 KV，键 `account`） |
| 主页入口 + 配置页 | UI 项 `home.entry` + 能力 `accounts` 返回声明式视图 |
| 歌单「添加歌曲」里的入口 | UI 项 `addsong.entry` + 动作 `musicxx.ui.openOverlaySheet` |
| 挑歌浮层 | 能力 `browse`（列目录）+ 视图 `browse` |
| 加进歌单 | 动作 `musicxx.playlist.addSong` |
| 播放 | 钩子 `musicxx.player.source.resolve` + 本机 HTTP 转流服务 |
| 自检 | 能力 `probe` |

---

## 1. 装与跑

```powershell
# 1) 在插件包目录里构建示例（产物在 .native/output/<平台>-<架构>-<配置>/plugins/）
pwsh -NoProfile -File tools/build_examples.ps1

# 2) 装进应用（也可以用管理页「从压缩包安装」）
musicxx-cli plugin install "<绝对路径>/example_ftp_source-2026_1024_1.zip" --enable

# 3) 看日志 / 自检
musicxx-cli plugin logs example_ftp_source --limit 50
musicxx-cli call probe --plugin example_ftp_source
```

本机起一个测试用 FTP 服务器（需要 `pip install pyftpdlib`）：

```powershell
python -m pyftpdlib -p 2121 -d D:/tmp/ftproot -u test -P 123456
```

然后在应用里：**音乐主页 → FTP 音乐** → 填 `127.0.0.1` / `2121` / `test` / `123456` → 「保存并测试」，
看到"连接成功，根路径下读到 N 项"就通了。

## 2. 账号

单账号（示例只演示一个）。字段与缺省值：

| 字段 | 含义 | 缺省 |
|---|---|---|
| `host` | 服务器地址（IP 或域名） | 空（必填） |
| `port` | 端口 | `21` |
| `user` | 用户名；**留空 = 匿名登录**（`anonymous`） | 空 |
| `password` | 密码；界面上**不回显**，留空表示不修改 | 空 |
| `rootPath` | 浏览起点 | `/` |
| `encoding` | 文件名编码：`utf-8` / `gbk` | `utf-8` |
| `timeoutMs` | 连接与命令应答超时 | `10000` |
| `lastTestAt` / `lastOk` / `lastMessage` | 最近一次「测试连接」的结果 | — |

存在插件私有 KV（`musicxx.storage` 的键 `account`，文件在插件数据目录的 `data/kv.json`），
**不放 `config.json`**：升级安装是整目录替换，会丢。

## 3. 歌曲里的音源

```text
type = plugin.example_ftp_source.audio
src  = <账号指纹>/<FTP 绝对路径去掉开头 />

例：3f9c1d7b/Music/Album/a.flac
账号指纹 = fnv1a(host 小写 + ":" + port + "/" + user) 取 8 位十六进制（不含密码）
```

- 指纹只是"这首歌是哪个账号加的"的标记：换账号后旧歌仍然认得出来，播放时会给出可读原因；
- 文件名由主程序按 `src` 最后一段取，歌名 / 歌手由主程序按文件名识别（与本地扫描、B 站列表同一套规则）；
- 播放时插件返回 `{"patch":{"src":{"path":"http://127.0.0.1:<port>/s/<token>","kind":"url"}}}`，
  `kind` 必须是 `url`：主程序只对网络地址套 HTTP 缓存流。

## 4. 歌单「添加歌曲」

这里演示**两种写法**，推荐第一种（插件不用画界面）：

### 4.1 应用画界面（推荐）：`musicxx.source.browse` + `drive.list`

插件声明一个可浏览来源，主程序就在「添加歌曲」菜单里自动多一行，点开是**与内置网盘完全一样**的
弹窗（路径栏 / 排序 / 搜索 / 勾选 / 整目录递归 / 加歌单），数据由下面几个能力提供：

```js
musicxx.call("musicxx.source.browse", {
    id: TYPE_ID, title: "FTP 文件", root: "/",
    icon: "resource/svg/ftp.svg", search: true,
    capabilities: { list: "drive.list", search: "drive.search",
                    resolveSongs: "drive.resolveSongs" }
});

musicxx.capability.register("drive.list", function (args) {
    // args: {path, offset, limit, sort, reverse}；next 空串 = 没有下一页
    return FtpSource.list(account, args.path).then(function (r) { /* → {ok, entries, next} */ });
});
```

条目里的文件要带 `song: {type, src, fileName, size}`（`src` 是账号指纹 + 路径，与 §3 一致），
主程序就按内置歌曲行画、选中后直接加进歌单。协议完整字段见包内 `docs/plugin-api.md` §3.3。

想要**整页浏览**（与内置网盘页一样，带下载 / 重命名 / 删除 / 条目信息）时不用自己注册页面，
放一个主页入口指过来就行（示例插件的「FTP 文件」入口就是这么写的）：

```jsonc
{ "name": "browseDrive", "type": "home.entry", "order": 131,
  "data": { "title": "FTP 文件", "subtitle": "整页浏览与下载",
            "icon": "resource/svg/ftp.svg",
            "action": { "kind": "command", "name": "musicxx.ui.browseDrive",
                        "args": { "type": "plugin.example_ftp_source.audio" } } } }
```

`musicxx.ui.browseDrive` 只允许打开**自己声明的**来源，`path` 可选（起始目录），**立即返回**
`{ok, reason:"opened", type}`，不等用户关页面。

声明里写了 `write: true` 并实现下面四个能力，整页列表里就会多出「新建文件夹 / 重命名 / 删除 /
条目信息」（本示例都实现了，直接拿 FTP 的 `MKD` / `RNFR`+`RNTO` / `DELE`、`RMD` 来完成）：

| 能力 | 入参 | 返回 | 本示例怎么做 |
|---|---|---|---|
| `drive.mkdir` | `{parent, name}` | `{ok:true, path}` | `MKD <parent>/<name>` |
| `drive.rename` | `{path, newName}` | `{ok:true, path}` | `RNFR <path>` + `RNTO <同层>/<newName>` |
| `drive.remove` | `{paths:[…]}` | `{ok:true, removed}` | 先 `DELE`，服务器不认再 `RMD`（目录） |
| `drive.info` | `{path}` | `{ok:true, title, pairs}` | 列父目录把这一条挑出来 |

删除是**应用先弹确认**再问插件的；一条失败就整次当失败（已删掉的行下次刷新就对了）。

### 4.2 插件自己画浮层（老写法，留着做对照）

插件注册的 UI 项：

```jsonc
{ "name": "addSong", "type": "addsong.entry", "order": 930,
  "data": { "title": "添加FTP歌曲", "subtitle": "从 FTP 服务器选择",
            "icon": "resource/svg/ftp.svg",
            "action": { "kind": "command", "name": "musicxx.ui.openOverlaySheet",
                        "args": { "route": "ext://example_ftp_source/browse",
                                  "title": "添加FTP歌曲" } } } }
```

点它时主程序把当前歌单的 `pid` / `name` / `songNum` 合进动作参数，然后执行
`musicxx.ui.openOverlaySheet`：打开这个插件视图的底部浮层（`size: "full"`，
可用宽高的 90%），**等用户关掉浮层才返回**（所以歌单页能在 `await` 之后刷新列表）。

浮层的形状用 `musicxx.AdaptLayout` 分两套：竖屏一条竖列（**筛选择在最上面，列表占满中间并自己滚，
「添加 N 首」按钮在最下面**）；横屏两栅（**左栅放筛选与添加按钮，右边整块留给列表**，
`rowBodyRatio: 0.34`）。列表用目录列表块 `musicxx.DirListView`：文件夹与文件的图标、顶部路径栏
（栏里的「上一级」，在账号配的根目录下不给）与行尾的勾选框都由它画（行样式跟应用内置的目录列表
一致），插件只保存当前目录与选中态（写法见包内 `docs/plugin-ui.md` §2.5.1）。
文件条目带 `song`（`type` = 本插件的音源类型、`src` = 账号指纹 + 路径、`fileName` / `size`），
所以行里显示的是**内置的歌曲行**（默认歌曲图标 + 文件大小），以后插件能拿到封面时只需填上 `cover`。

**页面上不放刷新 / 翻页 / 账号这类按钮**：刷新与翻页走列表自己的手势（下拉刷新、滚到底加载下一批），
路径栏里的「上一级」只有离开根目录才出现；账号从主页入口「FTP 音乐」进。

浮层里的交互都走 `browse` 能力（每次返回新视图）：

| 动作 | 参数 |
|---|---|
| 进入目录 / 上一级 | `{op:"enter", path}` / `{op:"up"}` |
| 下拉刷新 / 滚到底加载下一批 | `{op:"refresh"}` / `{op:"more"}` |
| 按名称筛选 | `{op:"keyword", value}` |
| 勾选 / 取消（控件派发，带 `id` 与 `value`） | `{op:"select", path}` |
| 整行点击切换 | `{op:"toggle", path}` |
| 加入歌单 | 能力 `browseAdd` |
| 直接播第一首（界面不放按钮，留给脚本 / 自动化） | 能力 `browsePlay` |

加歌用的是主程序动作 `musicxx.playlist.addSong`（`playlist` = 一张歌单）：

```jsonc
{"pid": 123, "songs": [
  {"type": "plugin.example_ftp_source.audio",
   "src": "3f9c1d7b/Music/a.flac",
   "fileName": "a.flac",
   "size": 12345678}
]}
```

- `name` / `artist` 故意不写：主程序会按 `fileName` 识别；
- `dedup` 缺省开（同一路径重复添加会被跳过，返回里的 `skipped` 是跳过的数量）；
- 成功后插件提示"已添加 N 首到『歌单名』"并关掉浮层，歌单页自己刷新。

## 5. 播放链路

```text
点歌 → type = plugin.example_ftp_source.audio
  → 钩子 musicxx.player.source.resolve（最多 5 秒）
      → 校验指纹 → 第一次启动本机转流服务（127.0.0.1:<随机端口>）
      → 登记 token（同一 <指纹, 路径> 复用同一个地址）
      → 回答 http://127.0.0.1:<port>/s/<token>，kind = url
  → 主程序 HTTP 缓存流（GET / HEAD + 单区间 Range）→ proxy.js
      → FTP 控制连接（TYPE I / EPSV 或 PASV / REST / RETR）→ 数据连接
      → 写响应头 → 把数据片原样写给播放器（发送队列长了就 socket.pause 暂停 FTP 读）
  → 播放器解码
```

转流服务的行为：

| 请求 | 响应 |
|---|---|
| `GET` 无 Range | `200` + `Content-Length`（大小未知时靠关连接结束） |
| `GET` 单区间 `bytes=a-b` / `a-` / `-n` | `206` + `Content-Range` + `Content-Length` |
| 越界 | `416` + `Content-Range: bytes */<size>` |
| 多区间 | 按整段 `200`（主程序的缓存流只发单区间） |
| `HEAD` | 同上但不发正文 |
| 其它方法 | `405` |
| token 不认识 / 账号换过 | `404` |
| 同时转发超过 6 条 | `503` + `Retry-After: 1` |

每条连接只服务一个请求（响应总是 `Connection: close`）。

两个取巧的地方，改动时要知道：

- **区间的结束位置一律做到文件末尾**：FTP 的 `RETR` 没法“只发 100 字节”，要在 JS 里截断数据片就得
  重新编码 base64。所以 `bytes=1024-2047` 回的是 `bytes 1024-<末尾>/<总长>`，播放器按 `Content-Range`
  读自己需要的那一段（主程序的缓存流本来就只发 `bytes=<位置>-`）。
- **服务器不支持 `REST`** 时退回 `200` 从头发（播放不受影响，拖动会回到开头）；
  **服务器不支持 `SIZE`** 时不写 `Content-Length`、直接按整段发（也不能拖动）。

## 6. 限制（都是刻意的）

- **只有明文 FTP**：不做 FTPS / SFTP（插件框架的 socket 能力不做 TLS 客户端以外的协议）；
- **只有一个账号**：示例不演示多账号切换；
- **只读**：不上传（新建 / 重命名 / 删除见下面）；
- **没有封面 / 歌词 / 远端标签**：歌名与歌手来自文件名；
- **拖动依赖服务器的 `REST`**；**缓存与拖动依赖 `SIZE`**（服务器不支持时仍能播，但每次都从头读）；
- **字节会经过一次 JS 线程**：插件框架没有"两端直连"的桥接能力，示例用
  `socket.send` + `queuedBytes` + `socket.pause` 做背压；大文件长时间播放会持续占用 JS 线程的一部分
  时间（每个数据片一次 base64 字符串拷贝）。要更快只能让插件框架提供桥接；
- 目录一次显示 50 项（滚到底由列表自己请求下一批，还有剩余时才给加载更多），一次最多选 200 首。

## 7. 安全

- 账号只在本机的插件存储里；界面不回显已保存的密码；日志只记主机与端口，**不记密码**；
- 歌曲里只有指纹 + 路径，不含用户名与密码；
- 转流服务只监听 `127.0.0.1`、端口由系统分配，地址里只有 128 位随机 token；服务端按 token 查表，
  **不解析路径**（拿到别的 URL 也读不到别的文件）；
- 只接受本插件自己的音源类型（`ctx.pluginType !== TYPE_ID` 一律不回答），不会替别的插件解析。

## 8. 排障

按这个顺序看：

1. `musicxx-cli plugin logs example_ftp_source --limit 50` —— 插件自己的日志（账号、列目录、解析失败）；
2. `musicxx-cli call probe --plugin example_ftp_source` —— 一次拿到：是否配置、指纹、转流服务端口 /
   token 数 / 请求数、最近错误、各类调用计数；
3. `musicxx-cli call musicxx.source.list`（或 MCP `ListMediaSources`）—— 解析链里有没有本插件；
4. 播放仍然失败：先确认「测试连接」能读到目录；再确认 `probe` 里 `proxy.running` 是 `true`。

常见现象：

| 现象 | 原因 / 处理 |
|---|---|
| 主页没有「FTP 音乐」 | 插件没启用；或插件包版本比插件框架新（看管理页的加载失败原因） |
| 「添加歌曲」里没有「添加FTP歌曲」 | 插件框架太旧（不认识 `musicxx.ui.addsong.entry`），日志里有一条注册失败 |
| 账号页提示"没有 TCP socket 能力" | 插件框架太旧，没有 `musicxx.net.socket`；升级插件框架 |
| 中文文件名乱码 | 账号里的「文件名编码」改成 GBK；日志里若提示没有 `textEncode`，说明框架太旧 |
| 能列目录、播放报"没有可用的解析实现" | 账号指纹对不上（换过主机 / 端口 / 用户名），从歌单里重新添加一次 |
| 播放几秒就停 | 服务器限制了并发连接数（转流 + 缓存流会各开一条），看日志里的 FTP 应答码 |

## 9. 目录

| 文件 | 内容 |
|---|---|
| `plugin.yaml` | 清单（`kind: js`，`scripts` 按顺序：两个 kit → `ftp.js` → `proxy.js` → `ui.js` → `plugin.js`） |
| `plugin.js` | 注册（UI 项 / 能力 / 钩子 / 音源类型）、账号状态、浏览状态、加歌与播放 |
| `ftp.js` | FTP 客户端：控制连接、登录、被动模式、`MLSD` / `LIST` / `SIZE` / `REST` / `RETR`、编码转换 |
| `proxy.js` | 本机 HTTP 转流服务：请求解析、`Range`、token 表、数据转发与背压 |
| `ui.js` | 账号页与浏览页的视图拼装（kit） |
| `resource/svg/ftp.svg` | 主页入口与「添加歌曲」项的图标 |
| `pluginxx_ui_kit.js` / `musicxx_ui_kit.js` | 随插件分发的界面 kit（由 `tools/sync_ui_kit.ps1` 复制，不要手改） |
