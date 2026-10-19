# example_override_media_source（插件音源解析示例）

演示**插件参与音源解析**：主程序要把一首歌的音源换成"播放器内核能打开的地址 + 请求头"时，
会先问钩子 `musicxx.player.source.resolve`（外置插件 → 内置插件 → 应用自身，第一个给出合法结果的生效）。
这个插件在该钩子上注册一个处理器，按载荷里的来源类型分流。

当前实现的功能：

| # | 来源 | 音源字符串写什么 | 插件返回什么 |
|---|---|---|---|
| 1 | 内置 Bili（`type = "bili"`） | 歌曲实体里是 BV 号（钩子载荷的 `crudeSrc`） | B 站音频直链 + `Referer/Origin/User-Agent` |
| 2 | `plugin.<插件 id>.plugin_bili`（自定义类型） | BV 号，例 `type = "plugin.…plugin_bili"` + `src = "BV1GJ411x7h7"` | 同一份 B 站直链 + 请求头 |
| 3 | `plugin.<插件 id>.plugin_proxy_local`（自定义类型） | 本地文件路径，例 `type = "plugin.…plugin_proxy_local"` + `src = "D:/music/a.mp3"` | `http://127.0.0.1:<端口>/f/<token>`（插件框架的本地 HTTP 文件服务） |

## 文件与开关

```text
example_override_media_source/
├── plugin.yaml          # 清单（scripts 里按顺序加载两个界面 kit 与本脚本）
├── plugin.js            # 全部逻辑
├── pluginxx_ui_kit.js   # 随插件分发的界面 kit（tools/sync_ui_kit.ps1 复制）
├── musicxx_ui_kit.js    # musicxx 扩展 kit（switchRow 在它里面）
├── resource/svg/        # 主页入口图标
└── config.json          # 用户改过的设置（插件自己写；没有就是默认值）
```

设置项（`config.json`，设置页里是开关）：

| 键 | 默认 | 关掉之后 |
|---|---|---|
| `overrideBili` | `true` | 不接管内置 Bili 解析，主程序原有逻辑照旧 |
| `pluginBiliEnabled` | `true` | 自定义音源 `plugin_bili` 不接管（用这个来源的歌曲会解析失败） |
| `proxyLocalEnabled` | `true` | 本地文件音源 `plugin_proxy_local` 不接管（该来源解析失败） |

设置页：主页入口「音源解析示例」→ `ext://example_override_media_source/settings`。
页面上还能看到被问到的次数、成功次数与最近一次错误。

## 功能 1：覆盖内置 Bili 解析

主程序在问这条钩子之前，已经把 Bili 歌曲的 BV 号换成了临时直链（`MediaPlayer_c.parseMediaSrc`），
所以钩子载荷里：

- `type = "bili"`；
- `src` = 已经换好的 `https://...m4s?...` 临时直链；
- `crudeSrc` = 歌曲里保存的 **BV 号**；
- `quality` = 用户音质偏好（默认 `upper`）。

插件做的事（`plugin.js` 的 `resolveBili`）：

1. 从 `crudeSrc` 取 BV 号（取不到再看 `src`）；
2. `GET https://api.bilibili.com/x/web-interface/view?bvid=...` 拿 `cid` 与 `pages`；
3. `GET https://api.bilibili.com/x/player/playurl?...&fnval=16&platform=pc` 拿 `dash.audio`；
4. 按用户音质偏好选一路（30216 / 30232 / 30280 / 30250 / 30251），回答
   `{patch: {src: {path: 直链, headers: {Referer, Origin, User-Agent}, kind: "url", ...}}}`。

### 两个必须知道的限制

- **多 P 视频不接管**：钩子载荷里没有分 P 序号（`biliPIndex` 存在歌曲实体的 `appInfo1` 里，
  载荷只给只读视图，不带它）。插件看到 `pages.length > 1` 时直接返回 null，让主程序已经算好的
  那一份继续生效 —— 否则第 2 P 会被播成第 1 P。
- **匿名解析**：`musicxx.net.fetch` 不带主程序的 B 站登录 Cookie，所以只能拿到匿名档位
  （一般最高 192k）。要大会员档位就得自己带 Cookie（例如让用户填在设置里），这超出示例范围。

另外：

- B 站音频 CDN 要求 `Referer`，且对 `HEAD` 请求返回 404 —— 播放器与主程序缓存流发的都是
  `GET + Range`，所以正常；
- 同一个 BV 的 preload / 播放 / 封面 / 媒体信息会连着问好几次，插件按 `BV + 音质` 缓存 10 分钟
  （直链自身还有 `deadline`，快过期时也会重新解析）。

### 手动验证

```bash
# 用统一入口播放一首 B 站歌曲（BV 号就是歌曲的音源）
musicxx-cli call musicxx.player.playSrc --args '{"src":"BV1GJ411x7h7","type":"bili"}'
```

播放时看插件日志（管理页 → 插件 → 日志，或 `musicxx-cli plugin logs example_override_media_source`）；
关掉开关后再播同一首歌，插件处理器返回 null，主程序内置解析照旧。

## 功能 2：自定义音源类型 `plugin_bili`

插件可以在**不改主程序来源枚举**的前提下声明自己的音源类型（`plugin.<插件 id>.<类型名>`）。
有两种写法，**新写法优先**：

| 写法 | `type` | `src` | 钩子载荷 |
|---|---|---|---|
| 新写法（推荐） | `plugin.example_override_media_source.plugin_bili` | `BV1GJ411x7h7`（自己的标识） | `pluginType` = 类型 id，`customType` = 类型 id，`src` = 标识 |
| 老写法（兼容） | `link` | `plugin.example_override_media_source.plugin_bili://BV1GJ411x7h7` | `pluginType` = 类型 id，`customType` = 空，`src` = 完整地址 |

新写法里 `src` 就是资源 id / 路径本身（不再塞假链接），类型串存在歌曲的音源信息里
（序列化后是 `{"type":"plugin.…plugin_bili", …}BV1GJ411x7h7`）。
主程序内置实现不接管这类来源（核心层看到自定义类型就返回"不解析"），而是问同一条钩子。

`<插件自己的标识>` 怎么解释完全由插件决定，本示例把它当 BV 号
（`opaqueOf()`：老写法取 `://` 之后的部分，新写法就是 `src` 本身）。

声明（`declareSourceTypes()`，脚本加载时跑一次）：

```js
musicxx.call("musicxx.source.define", {
    id: "plugin.example_override_media_source.plugin_bili",
    title: "B 站（插件解析）",
    depict: "地址里写 BV 号，插件自己查 B 站接口换播放地址与请求头",
    srcDoc: "BV1GJ411x7h7",     // 给用户与自省看的地址写法示例
});
```

- 声明的用处是**自省与更清楚的报错**：能不能播看的是有没有注册 `musicxx.player.source.resolve`
  的处理器，不是有没有声明；
- 声明跟着插件生命周期走（停用 / 卸载 / 关『拟声++』自动撤销），不写磁盘，每次启用都会重新声明；
- 查清单：`musicxx-cli call musicxx.source.list`、MCP `ListMediaSources`、插件动作 `musicxx.source.list`；
- 只写标识（`BV1GJ411x7h7`）也存得下（主程序会补类型），但自己拼全更清楚。

手动验证：

```bash
# 新写法
musicxx-cli call musicxx.player.playSrc --args '{"src":"BV1GJ411x7h7","type":"plugin.example_override_media_source.plugin_bili"}'
# 老写法（一样能播）
musicxx-cli call musicxx.player.playSrc --args '{"src":"plugin.example_override_media_source.plugin_bili://BV1GJ411x7h7","type":"link"}'
```

## 功能 3：本地文件走插件 HTTP 服务 `plugin_proxy_local`

### 为什么需要框架提供

JS 运行时里**没有任何 socket / 文件 API**，而播放器只认 `http(s)` 与本地文件。
要把"播放器打不开的东西"交给它，必须有东西在本地监听一个端口 —— 那是插件框架内置的能力：

```js
musicxx.net.serveFile(path, { fileName, contentType })   // → { ok, url, size, mtimeMs, reused }
musicxx.net.serveBytes(text, { encoding, contentType, fileName })
musicxx.net.serverInfo()   // { ok, running, url, port, files, requests, bytesServed }
musicxx.net.stopServer()
```

本示例的 `answerProxyLocal()` 只做三步：去掉 `file://` 前缀并归一路径 → `serveFile` → 回答地址：

```js
return { patch: { src: { path: served.url, kind: "url", size: served.size } } };
```

地址写法（两种都认）：

```text
plugin.example_override_media_source.plugin_proxy_local://D:/music/a.mp3
plugin.example_override_media_source.plugin_proxy_local://file:///D:/music/a.mp3
```

### 服务长什么样

- 只监听 `127.0.0.1`，端口由系统分配（不固定、不开放局域网）；
- URL 里是 128 bit 随机 token：服务端**按 token 查表**，不解析路径，拿到 URL 也读不到别的文件；
- 支持 `GET` / `HEAD` 与**单段 `Range`**（`206` + `Content-Range`，越界 `416`）—— 播放器拖动进度靠它，
  否则会退化成整段重下；文件按 64 KiB 边读边写，不进内存；
- 同一个文件重复登记返回同一个 URL；登记上限 1024 条（淘汰最旧）、并发连接上限 32；
- 跟着插件实例：停用 / 卸载 / 关『拟声++』/ 插件框架停机时自动关掉，端口立即释放。

旧版插件框架没有这四个接口：示例会退化为直接返回本地路径（`kind: "file"`），写一条警告，
Play 仍然能放（播放器自己读文件），只是不再经过 HTTP。

### 手动验证

```bash
# 把 <本地文件> 换成一个真实存在的音频/视频文件
musicxx-cli call musicxx.player.playSrc --args '{"src":"D:/music/a.mp3","type":"plugin.example_override_media_source.plugin_proxy_local"}'
```

设置页会显示服务状态（端口 / 登记数 / 请求数）；也可以用 `curl` 直接验：

```bash
curl -I  "http://127.0.0.1:<端口>/f/<token>/a.mp3"              # 200 + Content-Length + Accept-Ranges
curl -H "Range: bytes=0-1023" "http://127.0.0.1:<端口>/f/<token>/a.mp3"   # 206 + Content-Range
curl "http://127.0.0.1:<端口>/f/deadbeef/a.mp3"                  # 404（只按 token 查表）
```

地址从哪里来：设置页的「本地代理」计数旁边就是最近一次登记的地址，或者用能力 `serveText`
（`musicxx.capability.call` 调 `serveText({text})`）随地验一下 `serveBytes`。

### 这套机制还能做什么（重点）

示例只代理本地文件，是为了不联网、结果可复现；同一套机制可以：

| 场景 | 怎么做 |
|---|---|
| **代理外部网络** | `musicxx.net.fetch/download` 取数据 → 小内容 `serveBytes`，大文件 `download` 落盘再 `serveFile` |
| **播放器不支持的协议**（FTP / SFTP / 私有协议） | 插件自己取流（或先落盘）→ 转成 HTTP 给播放器 |
| **格式 / 容器转换** | 插件转码后的产物 `serveFile`；HLS 的 `m3u8` 用 `serveBytes`、分片用 `serveFile` |
| **统一鉴权 / 请求头** | Cookie、签名、防盗链留在插件里，播放器只看到干净的本地 URL |
| **边下边播** | 边取边写临时文件，写一段就 `serveFile`（注意：重登同一路径会命中旧登记，改变后要重新登记） |

限制也要知道：`serveBytes` 是内存内容（≤ 8 MiB）；服务跟着插件实例（插件卸载即失效）；
播放器可能并发开多条连接（框架已给 4 条连接线程、上限 32）。

## 开发与调试

```bash
# 同步界面 kit（改了 kit 或第一次建目录后）
pwsh -NoProfile -File tools/sync_ui_kit.ps1 -Plugin example_override_media_source
# 构建示例插件（框架有改动时会一起重建）
pwsh -NoProfile -File tools/build_examples.ps1
# 装进正在运行的应用并重载
musicxx-cli plugin install "<zip 绝对路径>" --enable
musicxx-cli plugin reload example_override_media_source
musicxx-cli plugin logs example_override_media_source --limit 50
```

## 写作参考

| 主题 | 文档 |
|---|---|
| 插件总入口（选形态 / 第一个插件 / 注意事项） | 包内 `docs/plugin-guide.md` |
| 钩子总表与 `musicxx.player.source.resolve` 的载荷 / 裁决 | 包内 `docs/plugin-hooks.md` |
| 音源解析链、插件音源地址写法、自省命令 | 包内 `docs/plugin-api.md` §3.2.6 / §3.2.15 |
| 本地 HTTP 文件服务（`musicxx.net.serveFile` 等） | 包内 `docs/plugin-api.md` §3.2.6、`docs/plugin-js-api.md` §7.3.1 |
| JS 插件写法（顶层注册、处理器不能等动作） | 包内 `docs/plugin-js-api.md` |
