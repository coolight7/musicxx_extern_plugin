# 能力总表（插件作者文档）

> 本文件由 `tools/gen_contract.dart` 从 `tools/capabilities.def.json` 生成，请勿手改。

**能力** = 调用方发一次请求、等一个结果。同一个 id 只有一份声明，声明里写清：

| 字段 | 含义 |
|---|---|
| `impls` | `one` = 一个槽位（后登记的覆盖先登记的，跨插件也覆盖）；`many` = 多实现共存，按 (层, priority, 登记顺序) 组成链 |
| `merge` | 多实现的结果怎么合并：`first` = 第一个合法结果生效；`veto` = 任一实现能否决 |
| `phases` | 能参与哪些阶段：`before`（正序改输入）/ `impl`（给结果）/ `after`（逆序改输出）。**不含 `impl` = 实现固定由应用提供，插件只能注册 `before` / `after`** |
| `dispatch` | `sync` = 调用方等结果（插件侧只能同步处理器）；`async` = 不占用调用线程 |
| `timeoutMs` | 等实现的上限，超时按"没给"处理；`0` = 不设上限 |
| `cache` | `byIdentity` = 同一个请求身份不重复算；`none` = 每次都问实现 |
| `usage` | 这条能力用来干什么。**功能埋点** = 只给应用自己当埋点调；**命令** = 外部（命令行 / MCP / 链接）能调；**插件服务** = 插件之间互相调 |
| `expose` | 谁能调：`cli` / `mcp` / `link` / `plugin` / `ui` / `page` / `drive` |
| `consent` | 链接触发时是否先问用户（`ask` = 问，`none` = 不问） |

## 怎么读这张表

- **用途**列就是界面与文档里的说法：`功能埋点` 对应代码里的"能力"（插件接管它的实现就是替代应用自己的这一段）；
- **可注册阶段**列写清插件能参与哪一步；只写 `before / after` 的能力**不能**注册实现（注册会被拒并说明原因）；
- **状态**列 `已接入` = 主程序已经在调用点接上（注册后会被调用）；`未接入` = 约定已定、主程序还没接（注册不报错，也不会触发）；
- 注册时写全名：官方 `musicxx.*`；实现 / 声明自己的能力时写名字（宿主补成 `plugin.<本插件 id>.<名字>`，名字可以含点，例如 `drive.list`）。

## 能力清单

| 能力 id | 用途 | 可注册阶段 | 合并 | 派发 | 超时 | 缓存 | 外部入口 | 状态 |
|---|---|---|---|---|---|---|---|---|
| `musicxx.app.exit` | 功能埋点 | impl | veto | sync | - | - | - | 未接入 |
| `musicxx.app.deepLink` | 功能埋点 | impl | veto | sync | - | - | - | 未接入 |
| `musicxx.player.beforePlaySong` | 功能埋点 | impl / after | veto | async | - | - | - | 已接入 |
| `musicxx.player.source.beforeParse` | 功能埋点 | impl / after | first | async | - | - | - | 已接入 |
| `musicxx.player.source.resolve` | 功能埋点 | impl | first | async | 5000 ms | 按身份 | - | 已接入 |
| `musicxx.player.seek` | 功能埋点、命令 | impl / after | veto | async | 500 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.player.volume` | 功能埋点 | impl / after | veto | async | 500 ms | - | - | 已接入 |
| `musicxx.player.speed` | 功能埋点 | impl / after | veto | async | 500 ms | - | - | 已接入 |
| `musicxx.player.pitch` | 功能埋点 | impl / after | veto | async | 500 ms | - | - | 已接入 |
| `musicxx.player.error` | 功能埋点 | impl / after | veto | async | - | - | - | 已接入 |
| `musicxx.media.notification` | 功能埋点 | impl | first | sync | - | - | - | 未接入 |
| `musicxx.media.info.analyse` | 功能埋点 | impl | first | async | - | - | - | 未接入 |
| `musicxx.song.beforeAdd` | 功能埋点 | impl | veto | sync | - | - | - | 未接入 |
| `musicxx.playlist.load` | 功能埋点 | 只能 before / after | - | async | - | - | - | 未接入 |
| `musicxx.local.scan.file` | 功能埋点 | impl | veto | sync | - | - | - | 未接入 |
| `musicxx.lyric.load` | 功能埋点 | before / impl / after | first | async | 8000 ms | 按身份 | - | 已接入 |
| `musicxx.lyric.load.musicxx_server` | 功能埋点 | before / impl / after | first | async | 8000 ms | 按身份 | - | 已接入 |
| `musicxx.lyric.load.cache` | 功能埋点 | before / impl / after | first | async | 8000 ms | 按身份 | - | 已接入 |
| `musicxx.lyric.load.builtin` | 功能埋点 | before / impl / after | first | async | 8000 ms | 按身份 | - | 已接入 |
| `musicxx.lyric.load.builtout` | 功能埋点 | before / impl / after | first | async | 8000 ms | 按身份 | - | 已接入 |
| `musicxx.lyric.load.match` | 功能埋点 | before / impl / after | first | async | 8000 ms | 按身份 | - | 已接入 |
| `musicxx.media.spectrum.extract` | 功能埋点 | impl | first | async | 30000 ms | 按身份 | - | 已接入 |
| `musicxx.media.chorus.analyse` | 功能埋点 | impl | first | async | 20000 ms | 按身份 | - | 已接入 |
| `musicxx.media.palette.provide` | 功能埋点 | impl | first | async | 800 ms | 按身份 | - | 已接入 |
| `musicxx.app.net.http.request` | 功能埋点 | 只能 before / after | - | async | - | - | - | 未接入 |
| `musicxx.net.server.route` | 功能埋点 | impl | first | async | - | - | - | 未接入 |
| `musicxx.net.mcp.tools` | 功能埋点 | impl | first | sync | - | - | - | 未接入 |
| `musicxx.download` | 功能埋点 | 只能 before / after | - | async | - | - | - | 未接入 |
| `musicxx.cache.beforeTrim` | 功能埋点 | impl | veto | sync | - | - | - | 未接入 |
| `musicxx.cache.pathRequest` | 功能埋点 | impl | first | sync | - | - | - | 未接入 |
| `musicxx.ui.route.resolve` | 功能埋点 | impl | first | sync | - | - | - | 未接入 |
| `musicxx.feature.point.call` | 功能埋点、命令 | before / impl / after | first | async | - | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.app.foreground` | 命令 | impl | - | async | - | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.app.background` | 命令 | impl | - | async | - | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.player.play` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.player.pause` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.player.toggle` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.player.stop` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.player.next` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.player.prev` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.player.playSrc` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.player.setVolume` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.player.setSpeed` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.player.setPitch` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.player.setQuality` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.player.setLoopMode` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.player.setMediaType` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 未接入 |
| `musicxx.playlist.queryLists` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.playlist.querySongs` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.playlist.play` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.playlist.addSong` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.playlist.removeSong` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.playlist.create` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.playlist.updateSong` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.playlist.syncSongs.sync` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.songlist.querySongs` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.songlist.playSong` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.songlist.addSong` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.songlist.removeSong` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.songlist.updateSong` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.library.querySonglists` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 未接入 |
| `musicxx.library.querySongs` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 未接入 |
| `musicxx.library.querySonglist` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 未接入 |
| `musicxx.library.playSong` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 未接入 |
| `musicxx.library.playSonglist` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 未接入 |
| `musicxx.library.removeSong` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 未接入 |
| `musicxx.library.createSonglist` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 未接入 |
| `musicxx.library.setSongInfo` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 未接入 |
| `musicxx.library.search` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 未接入 |
| `musicxx.lyric.current.get` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.lyric.current.update` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.lyric.current.updateContent` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.lyric.getBySrc` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.lyric.search` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.ui.toast` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.ui.dialog` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.ui.overlay.show` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.ui.overlay.close` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.ui.openOverlaySheet` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.ui.openRoute` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.ui.pickSongs` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.ui.pickDir` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.ui.browseDrive` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.ui.setEntryBadge` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 未接入 |
| `musicxx.ui.setFontFamily` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.ui.getFontFamily` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.ui.loadFont` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.storage.get` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.storage.set` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.storage.delete` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.storage.list` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.net.fetch` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.net.download` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.render.list` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.render.current` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.render.select` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.media.palette` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.media.cover` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.media.palette.set` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.media.bindImage` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.media.unbindImage` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.media.spectrum` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.media.kernel` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.feature.list` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.feature.call` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.feature.point.list` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.feature.point.define` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.feature.point.undefine` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.source.define` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.source.undefine` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.source.list` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.source.browse` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.hook.handlers` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.host.openUrl` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.host.shareText` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 未接入 |
| `musicxx.host.clipboard` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.host.getPath` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.stats.reportMemory` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |
| `musicxx.stats.reportMetric` | 命令 | impl | - | async | 5000 ms | - | cli/mcp/link/plugin | 已接入 |

## 载荷与回答（已接入的能力）

未接入的能力只占位：注册不会报错，也不会触发。已接入的每一条下面写清载荷字段与回答格式。

### `musicxx.player.beforePlaySong` 播放前

载荷 `{sid, song, mode, quality}`：`song` 是只读视图（`sid/name/artist/album/durationMs/year/genre/srcKey/audio[]/video[]`，不含音频直链），`mode` = 播放模式、`quality` = 当前音质。裁决只实现 `{"action":"skip"}` = 跳过本曲（走应用既有的切下一曲流程）；`patch`（换音源/改音质）当前版本未实现，会被记一条日志后忽略。异步派发：切歌不会被插件拖慢；等待期间若又来了新的切歌请求，本次裁决被丢弃（插件可以自己用 `sid` 判断）。

### `musicxx.player.source.beforeParse` 音源解析前

载荷 `{sid, srcKey, src, index, tryLocalOrCache}`：`src` 是音源视图（`type/empty/srcKey/quality/durationMs/hashMd5/size`），`index` = 本轮解析的音源下标。裁决：`{"action":"skip"}` = 本轮不解析该音源（调用方接着试下一个）；`patch.src = {"type":"Local|UrlLink|Bili|...","src":"<新地址>","info":{...}}` = 只替换本轮使用的音源（不改动歌曲实体的音源列表），字段不合法会被忽略。异步派发（不卡住解析），等待期间切歌则本次裁决作废。

### `musicxx.player.source.resolve` 音源解析

载荷 `{srcKey, type, src, crudeType, crudeSrc, fileName, durationMs, quality, isMediaSrc, pluginType, caller}`：主程序要把一个音源解析成**播放器内核能打开的地址与请求头**时派发一次（外置 → 内置插件 → 应用自身，第一个给出合法结果的生效）。`src` = 本轮要解析的音源（插件自己的音源就是它写下的资源 id / 路径；内置来源是已经解析出来的临时直链或本地路径 —— 注册这条钩子的插件看得到这些内容，这是它能换地址的前提）；`crudeSrc` = 歌曲实体里保存的原始地址；`type` / `crudeType` = 对应的来源类型（`link` / `bili` / `aliDrive` / `local` 等；插件音源是空串）；`pluginType` = 本轮音源的插件音源类型 id（`plugin.<插件 id>.<类型名>`；不是插件音源则为空串）；`quality` = 用户当前音质偏好（枚举名小写）；`isMediaSrc` = false 表示这次要的是封面（媒体库来源）；`caller` = 谁在问（空串 = 主程序自己或命令行）。处理器返回 `{"src": {"path": "<地址>", "headers": {"Referer": "..."}, "kind": "url", "expireAtMs": 0, "size": 0, "durationMs": 0, "quality": "hq"}}`（`src` 也可以直接给一个地址字符串）：这就是播放器要打开的地址与请求头。`path` 支持网络地址、本地文件路径、`content://`、`asset://`；`kind` 取 `url` / `file` / `content` / `asset`，缺省按地址自己判断（判断错会让本地文件被当成网络流去做缓存）；`headers` 可省；`expireAtMs` 给临时地址的过期时间戳（毫秒，只用于排障）。**回答请用 `patch` 包一层**（写成 `patch.src`）：插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历。给自己的音源给出地址与请求头、给内置来源换地址（加代理、换 CDN、统一请求头）都是这条路。不返回 / 结果不合法 = 这次不提供，主程序接着问内置实现。返回 `{"disable": true}` 是显式置空：这次不再问别的实现，该音源会解析失败（只在明确知道它不该播时用）。异步派发：主程序最多等 5 s（超时按没给处理，改走内置实现），处理器请尽快返回。主程序只在"该钩子有处理器"时才派发。

### `musicxx.player.seek` 跳转播放位置

载荷 `{sid, from, to, durationMs}`：应用**要把播放位置改到 `to`（毫秒）之前**派发一次。拖动进度条、点歌词、快捷键快进快退、局域网控制、命令行 / 动作 / 脚本、恢复播放进度都会走这里。`from` = 改之前的进度，`durationMs` = 总时长（未知为 0），`sid` = 当前歌曲会话 id。裁决：`{"action":"continue","patch":{"to":60000}}` = 换成别的目标位置；`{"action":"cancel"}`（或 `skip`）= 这次不跳（保持原位）。超出范围的目标会被应用夹到 0~总时长。**异步派发、最多等 0.5 s**：超时或不回答 = 按原目标跳；等待期间切歌则本次裁决作废（插件不用自己判断 `sid`）。处理器里不要再调 `musicxx.player.seek`（会再派发一次）。

### `musicxx.player.volume` 调整音量

载荷 `{from, to}`：应用**要把音量改成 `to` 之前**派发一次（`from` = 当前音量，`to` = 这次要用的音量，都是 0~1，可能略大于 1 表示增益）。音量滑块、快捷键、静音 / 取消静音、局域网控制、命令行 / 动作、启动时恢复设置都会走这里。裁决 `{"patch":{"to":0.5}}` = 换成别的音量；`{"action":"cancel"}` = 这次不改（保持原音量）。最终值会被夹到应用的合法范围（0~1）。**异步派发、最多等 0.5 s**：超时或不回答 = 按原值执行。处理器里不要再调 `musicxx.player.setVolume`（会再派发一次）。

### `musicxx.player.speed` 调整播放速度

载荷 `{from, to}`：应用**要把播放速度改成 `to` 之前**派发一次（`from` = 当前速度，`to` = 这次要用的速度，单位是倍速）。速度菜单、长按加速 / 减速、命令行 / 动作、定时停止的变速、启动时恢复设置都会走这里。裁决 `{"patch":{"to":1.5}}` = 换成别的速度；`{"action":"cancel"}` = 这次不改。最终值会被夹到应用的合法范围（0.25~8）。**异步派发、最多等 0.5 s**。长按加 / 减速这类连续变化会连派多次，处理器要保持快，处理器里也不要再调 `musicxx.player.setSpeed`。

### `musicxx.player.pitch` 调整音调

载荷 `{from, to}`：应用**要把音调改成 `to` 之前**派发一次（`from` = 当前音调，`to` = 这次要用的音调；1 是原调，范围 0.25~2）。播放页的音调菜单、快捷键、音效切换时的复位、启动时恢复设置都会走这里。裁决 `{"patch":{"to":1.1}}` = 换成别的音调；`{"action":"cancel"}` = 这次不改。最终值会被夹到应用的合法范围（0.25~2）。**异步派发、最多等 0.5 s**。要主动改音调用动作 `musicxx.player.setPitch`；处理器里不要再调它（会再派发一次）。

### `musicxx.player.error` 播放失败

载荷 `{sid, srcKey, isNowUseSrc, errorCount, durationMs, positionMs}`（`errorCount` 已包含本次失败）。裁决：`{"action":"stop"}` = 停止播放；`{"action":"skip"}`（或 `cancel`）= 跳到下一曲；`{"action":"continue","patch":{"tryNextSrc":false}}` = 不再尝试当前源（跳过忽略错误并重试的分支，直接按主程序的换源/下一曲策略走）。异步派发（不占用调用线程，主程序不会被插件拖住）：等待期间切歌则本次裁决作废；处理器请尽快给出裁决，本次失败处理会等它结算。主程序只在"该钩子有处理器"时才派发。

### `musicxx.lyric.load` 歌词加载

载荷 `{sid, song, audio, parsed, path, enable, keyword, artist, lrcid, position, caller}`：主程序要为**一首歌加载歌词**时派发一次（外置 → 内置插件 → 应用自身，第一个给出合法歌词的生效）。`song` 是歌曲只读视图（`sid/name/artist/album/durationMs/srcKey/audio[]/video[]`，不含音频直链）；`audio` = 本轮音源视图（`type/srcKey/quality/durationMs/hashMd5/size`）；`parsed` = 实际播放用的那份音源（缓存文件 / 临时直链，字段同上）；`path` 只有本地、缓存、`content://`、asset 才有值（网络来源是空串 —— 要直链请自己走 `musicxx.net`）；`enable` = 这一轮允许做的事 `{musicxx_server（读拟声云歌词）, cache（读应用的歌词缓存）, builtin（读内置歌词：播放器元数据与媒体文件标签）, builtout（读同名外置歌词）, match（自动匹配）, upload（可自动上传）}`；`keyword` / `artist` = 自动匹配用的搜索串（前链可以改写）；`lrcid` > 0 = 强制用这一条云歌词；`position` 非空 = 强制读这个位置（文件路径，或歌词来源自己的标识）；`caller` = 谁在问（空串 = 主程序自己或命令行）。处理器回答 `{"patch": {"lyric": {"content": "[00:12.34]…", "lrcid": 0, "sourceType": "builtin", "offsetMs": 0}}}`：`content` 是 LRC 文本（主程序自己解析，不认别的格式），`sourceType` 取 `builtin` / `builtout` / `local` / `musicxx_server` / `match`（另有 `builtinCache` / `builtoutCache`；省掉按 `musicxx_server` 算；写 `builtin` / `builtout` 的歌词会像应用自己读到的那样进歌词缓存，并按 `enable.upload` 决定是否自动上传），`offsetMs` = 这首歌的歌词偏移（毫秒，可省）。`content` 为空、解析不出歌词行 = 这次不提供，主程序接着问下一个实现。`{"patch": {"lyric": {"disabled": true}}}` = 这首歌**禁用歌词**（这是合法结果：界面显示『禁用歌词』，后续读取全部不做）。`{"disable": true}` 是框架级置空：这次不再问别的实现，也不写缓存。后链回答 `{"value": {"lyric": {…}}}` 换掉这次的结果，`{"action": "drop"}` = 这次按没有歌词处理。**回答请用 `patch` 包一层**：插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历。前链能改写 `keyword` / `artist` / `lrcid` / `position` 与 `enable` 里的各项（回答 `{"patch": {…}}`），`{"action": "stop"}` = 这次不加载歌词。异步派发：主程序最多等 8 s（超时按没给处理，接着走应用自己的加载）。应用自己的实现会把这一轮拆成五个细分埋点按顺序询问（`musicxx.lyric.load.musicxx_server` → `.cache` → `.builtin` → `.builtout` → `.match`）；**插件在这里接管之后，细分埋点不会再被询问**。

### `musicxx.lyric.load.musicxx_server` 歌词加载 · 拟声云歌词

载荷与回答形状同 `musicxx.lyric.load`。这一阶段读**拟声云歌词**（自建服务 / 歌曲绑定的那个歌词来源）：用户给这首歌指定过的歌词（按音源身份记在应用里的那条），或歌曲自带的歌词来源（`myId` / 链接 / 云盘 / 媒体库 / 本地文件）。应用自己的实现在这里调自建服务的歌词接口、读绑定的本地歌词文件，命中时 `sourceType` 是 `musicxx_server` 或 `local`。`enable.musicxx_server` 为 false 时主程序不会问这个点（`enableCloudLyric: false` 的调用点是『只从文件读歌词』的场景）。回答 `disabled` 表示这首歌禁用歌词，后续阶段不再执行。

### `musicxx.lyric.load.cache` 歌词加载 · 歌词缓存

载荷与回答形状同 `musicxx.lyric.load`。这一阶段读**歌词缓存**：应用自己保存过的歌词内容（按音源身份、md5、sha1、大小与时长命中）。`enable.cache` 为 false 时不会被询问；命中时 `sourceType` 是 `builtinCache` / `builtoutCache`（这份缓存是上一次读到内置 / 外置歌词时写下的）。插件接管后可以换成自己的歌词库（例如本地收藏的歌词文件索引）。

### `musicxx.lyric.load.builtin` 歌词加载 · 内置歌词

载荷与回答形状同 `musicxx.lyric.load`。这一阶段读**内置歌词**，两条路合在一起：播放器当前音源**已经读到的元数据标签**（切歌后它比重新解码文件先到，应用最多等 300 ms），以及由媒体解码器**从音频文件 / 缓存流里读出的标签歌词**（`path` 就是那个文件；原来叫 `load_lyric_builtIn`）。会在四种情况下被询问：前链指定了 `position`（只读那个文件）、这一轮允许读缓存且来源类型允许（看播放器元数据）、解析出来的音源是缓存文件 / 链接（不管 `enable.builtin`，原来就是先读一遍它的标签）、或 `enable.builtin` 为 true 且来源类型允许。应用自己的实现还会按来源类型跳过明显没有标签的那些（Bili、asset、缓存 id 等），插件替换实现后不再受这条限制。`sourceType` 缺省 `builtin`（会进歌词缓存），命中后主程序还可能自动上传到自建服务（看 `enable.upload`）。

### `musicxx.lyric.load.builtout` 歌词加载 · 外置歌词

载荷与回答形状同 `musicxx.lyric.load`。这一阶段读**外置歌词**：与音频文件同名的 `.lrc`（本地、链接、WebDAV），以及云盘与媒体库各自的同名歌词查找（阿里云盘、百度网盘、115、Subsonic / Jellyfin / Emby 的歌词接口；最后两种的 `path` 是空串，标识在歌曲视图里）。`enable.builtout` 为 true 且来源类型允许、或前链指定了 `position` 时会被询问；应用自己的实现会跳过明显没有同名文件的来源（Bili、`myId`、`myLink`、缓存、asset 等），插件替换实现后不再受这条限制。`sourceType` 缺省 `builtout`（会进歌词缓存）。

### `musicxx.lyric.load.match` 歌词加载 · 自动匹配

载荷与回答形状同 `musicxx.lyric.load`。这一阶段做**自动匹配**：前面几个阶段都没拿到歌词时，按歌名 / 歌手 / hash / 大小 / 时长向自建服务要推荐歌词。`enable.match` 为 true 时会被询问（调用方决定；设置里的「自动匹配歌词」开关在主程序那边就并进了这个值）。应用自己的实现还会跳过 15 分钟以上、歌名为空或名字里有『纯音乐 / 伴奏 / instrumental』的歌（插件替换后不再受这条限制）。供参考：`keyword` / `artist` 可以在前链里改写（例如换成自己的搜索串）。`sourceType` 缺省 `match`。插件接管后可以换成自己的匹配方式（歌词站搜索、按音频指纹匹配等）；匹配不到就返回空，主程序按『这首歌没有歌词』处理。

### `musicxx.media.spectrum.extract`

载荷 `{srcKey, name, artist, kind, path, durationMs, frames, binCount, frameHz, maxSeconds, caller}`（`caller` = 谁在问：空串 = 主程序自己取值或命令行，非空 = 主动调用它的插件 id）：主程序要提取一首歌的**整曲频谱**时派发一次（`path` 是本地/缓存文件路径，不是网络直链；`kind` = `local` / `cache` / `content` / `other`；`frames` 按时长推算、只作提示）。处理器返回 `{"spectrum": {"spectrums": "<base64>", "waveform": "<base64，可省>", "binCount": 256, "frameHz": 10}}` 就用插件的数据替代内置提取（内置实现是主程序自己用 mediaxx 提取）：`spectrums` 是整曲频点（帧数 × 256 个字节，0~255 表示 -80~0 dB，默认每秒 10 帧），`waveform` 是每帧 1 字节 RMS 响度（0~255 表示 -60~+20 dB；省掉或长度对不上时按每帧频点均值补一份近似响度）。不返回 / 没有 `spectrum` / 数据不合法（不是 base64、长度不是 256 的整数倍、`binCount` 与 `frameHz` 不是 256 与 10）= 这次不提供，主程序接着用已登记的实现（内置『音乐动效』）。**回答请用 `patch` 包一层**（写成 `patch.spectrum`）：插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历，多个实现时裸字段会被后面的处理器覆盖（只有一个实现时两种写法等价）。异步派发：主程序最多等 30 s（超时按没给处理）；等待期间切歌则本次结果会被丢弃（可用 `srcKey` 自行校验）。主程序只在"该钩子有处理器"时才派发。返回 `{"disable": true}` 是显式的**置空裁决**：主程序这次不再询问其它实现（内置『音乐动效』也不跑，播放页频谱与副歌分析都拿不到数据），这次结果不写缓存；置空只在本次询问有效，处理器被摘除（插件停用 / 卸载 / 关闭『拟声++』）后自动还原。要用它请先确认自己的数据已经算好（例如自己起线程提前算完），否则用户会看到功能凭空消失。**这个功能点对外开放调用**（动作 `musicxx.feature.point.call`）：外置插件、命令行与 agent 能主动调它并拿到原始数据，参数与结果见 `featurePoint.list` 里的 `callDoc`。调用同样按这条处理器链问一遍，但**不写主程序的值仓、不改主程序状态**（只读值仓，绝不记录置空标记），同一个功能点 + 同一个调用方的上一次调用没结束时拿到 `busy`。

### `musicxx.media.chorus.analyse`

载荷 `{srcKey, name, artist, kind, path, durationMs, hasWave, frames, binCount, frameHz, spectrums?, waveform?, config, caller}`（`caller` = 谁在问：空串 = 主程序自己取值或命令行，非空 = 主动调用它的插件 id）：主程序要分析一首歌的**副歌（高潮）区段**时派发一次。`hasWave` 为真时附带主程序已经拿到的整曲频谱（`spectrums` / `waveform` 的 base64，4 分钟歌曲约 800 KB —— 用不上的插件请不要注册这个钩子）；`config` 是内置算法的参数（`bandCount` / `hopFrames` / `windowFrames` / `minRepeatLagSeconds` / `segmentSimilarity` / `minSegmentSeconds` / `maxSegmentSeconds` / `mergeGapSeconds`）。处理器返回 `{"chorus": {"sections": [{"start": 64000, "end": 100000, "confidence": 0.8, "energy": 0.7, "similarity": 0.9, "repeat": 3}], "totalDuration": 253000, "analyseTimeMs": 120}}`（时间单位毫秒）就用插件的区段替代内置分析；`sections` 是必需项（空数组 = 分析过了、这首歌没有副歌），每段至少要有 `start` 与 `end`，其余字段缺失按 0 处理。不返回 / 缺少 `sections` / 一个区段都解析不出来 = 这次不提供。**回答请用 `patch` 包一层**（写成 `patch.chorus`）：插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历，多个实现时裸字段会被后面的处理器覆盖（只有一个实现时两种写法等价）。异步派发：主程序最多等 20 s；等待期间切歌则本次结果会被丢弃（可用 `srcKey` 自行校验）。主程序只在"该钩子有处理器"时才派发。返回 `{"disable": true}` 可以显式置空这个功能：主程序这次不再走内置分析（进度条上的副歌标记就没有了），处理器被摘除后自动还原。**这个功能点对外开放调用**（动作 `musicxx.feature.point.call`）：外置插件、命令行与 agent 能主动调它（参数与结果见 `featurePoint.list` 里的 `callDoc`）；调用会先取一次频谱，**这一层同样不写值仓**，也不改主程序状态，同一个功能点 + 同一个调用方的上一次调用没结束时拿到 `busy`。

### `musicxx.media.palette.provide`

载荷 `{srcKey, name, artist, hasCover, night, caller, cover}`：主程序要为一张封面取色（内置实现是封面颜色分析）时派发一次。`caller` = 谁在问（空串 = 主程序自己取值或命令行，非空 = 主动调用它的插件 id）；`cover` = 这次算哪一张图：`{kind: current | file | url | data, path?, url?, bytes?, imageBase64?, coverKey}`（`current` = 当前歌曲的封面，`file` / `url` / `data` 是调用方指定的那一张；`path` / `url` 是对应的地址，`data` 给 `bytes` 与 `imageBase64` —— 图太大时只有 `bytes`；`coverKey` 是这次的身份，插件可以按它自己缓存）。老插件不认 `cover` 也能跑：它们只看 `srcKey` / `name` / `artist` / `night`，对 `current` 这一种用法的结果是对的；别的来源它们给不出（返回空），主程序接着走自己的分析 —— 不会给错数据。处理器返回 `{"colors": {"main": "#rrggbb", ...}}`（键可取 `main` / `light` / `lightMuted` / `dark` / `darkMuted`，写法 `#rrggbb` 或 `#aarrggbb`）就用插件提供的这套颜色替代内置分析：它写进与内置分析同一批字段（**只在主程序自己问当前封面时**；外置通过 `musicxx.feature.point.call` 调它时只拿数据、不写字段），内置背景、插件背景与 `musicxx.icon.*` 来源都会拿到；不返回 / 没有 `colors` / 颜色一个都解析不出来 = 这次不提供，主程序照旧走内置分析。**回答请用 `patch` 包一层**（写成 `patch.colors`）：插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历，多个实现时裸字段会被后面的处理器覆盖（只有一个实现时两种写法等价）。异步派发：不会卡住渲染准备；等待期间切歌时本次结果会被丢弃（可用 `srcKey` 自行校验）。返回 `{"disable": true}` 可以显式关掉封面取色：主程序这次不再走内置取色（保持上一次的颜色），处理器被摘除后自动还原。**这个功能点对外开放调用**（动作 `musicxx.feature.point.call`）：外置插件、命令行与 agent 能主动调它并拿到 5 个原始色槽位，参数与结果见 `featurePoint.list` 里的 `callDoc`。**调用没有副作用**：不写封面取色字段、不改内置背景（要改观感请用 `musicxx.media.palette.set`），同一个功能点 + 同一个调用方的上一次调用没结束时拿到 `busy`。

### `musicxx.feature.point.call` 调用功能点

三个阶段都在同一条通道上（`before` / `impl` / `after`），载荷都带 `phase` 以便同一个处理器分辨自己被叫的阶段：`before` 是 `{point, phase, args, caller}`（回答 `{"patch": {"args": {...}}}` 改这次调用的参数、`{"action": "stop"}` 这次不执行实现）、`impl` 是 `{point, phase, args, caller}`（回答见下）、`after` 是 `{point, phase, args, value, caller}`（回答 `{"value": ...}` 改最终结果、`{"action": "drop"}` 这次没有结果）。以下是实现链（`impl`）的规则：有人要算一个**外置插件声明的功能点**时派发一次（`point` = 功能点 id，`args` = 调用方给的参数，`caller` = 调用方插件 id，空串 = 应用自己或命令行）。这是**一条通用的实现通道**：插件声明的功能点共用它，而不是每个点各占一个钩子 id（插件框架只认固定的钩子表，见 docs/plugin-guide.md）。处理器只实现自己负责的点：不是自己的点直接返回空（空 = 不回答）；是自己的点就返回 `{"patch": {"value": <任意 JSON>}}` —— **必须用 `patch` 包一层**，插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历，否则后面的处理器会覆盖你的结果。`value` 键就是“实现方给出了结果”的判据（值可以是 null，例如“算过了、结果是空”）。返回 `{"disable": true}` 是显式置空：这次不再问别的实现。**同一条通道上所有实现都会收到同一次调用**，请按 `point` 过滤，不要假设只有自己收到。不要回答自己发起的调用（`caller` 等于自己的插件 id 时返回空；SDK 糖会自动做）。主程序不设超时，最多等到调用方自己的动作超时；同一个功能点 + 同一个调用方的上一次调用没结束时，调用方拿到 `busy`。功能点由插件用动作 `musicxx.feature.point.define` 声明、`musicxx.feature.point.undefine` 收回，声明跟着插件生命周期走。
