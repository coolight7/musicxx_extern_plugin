# 能力总表（插件开发文档）

> 本文件由 `tools/gen_contract.dart` 从 `tools/capabilities.def.json` 生成，请勿手改。

> 两张表：**能力**（前链改输入 / 实现链给结果 / 后链改输出）就是本文里的这些，**广播**（只通知、不回答）在 [plugin-broadcasts.md](plugin-broadcasts.md)。写法（注册、阶段顺序、裁决对象）见 [plugin-api.md](plugin-api.md) 第 8 节。

**能力** = 调用方发一次请求、等一个结果。同一个 id 只有一份声明，声明里写清：

| 字段 | 含义 |
|---|---|
| `impls` | `one` = 一个槽位（后登记的覆盖先登记的，跨插件也覆盖）；`many` = 多实现共存，按 (层, priority, 登记顺序) 组成链 |
| `merge` | 多实现的结果怎么合并：`first` = 第一个合法结果生效；`veto` = 任一实现能否决 |
| `phases` | 能参与哪些阶段：`before`（正序改输入）/ `impl`（给结果）/ `after`（逆序改输出）。**不含 `impl` = 实现固定由应用提供，插件只能注册 `before` / `after`** |
| `dispatch` | `sync` = 调用方等结果（插件侧只能同步处理器）；`async` = 不占用调用线程 |
| `timeoutMs` | 等实现的上限，超时按"没给"处理；`0` = 不设上限 |
| `cache` | `byIdentity` = 同一个请求身份不重复算；`none` = 每次都问实现 |
| `cli` | 命令面能调（命令行 / `/cli` / 链接）。块内可写 `title`（命令标题）、`consent`（从链接调时先问用户的原因） |
| `mcp` | 进 MCP 工具表。块内可写 `name`（工具名，PascalCase）、`format`（`text` / `json`） |
| `plugin` | 插件可以在 `musicxx.capability.call(id, args)` 里调 |

## 怎么读这张表

- **通道**列就是"谁能调它"：`命令` = 命令行 / `/cli` / 链接；`MCP 工具` = 给模型的工具；`插件` = 插件能调；`-` = 只能应用内部调（纯实现 / 干预类，插件只能注册前后链）；
- **可注册阶段**列写清插件能参与哪一步；只写 `before / after` 的能力**不能**注册实现（注册会被拒并说明原因）；
- **MCP 工具**列就是模型看到的工具名（`mcp.name`，不写时按 id 生成）；不做工具的能力写 `-`；
- 注册时写全名：官方 `musicxx.*`；实现 / 声明自己的能力时写名字（宿主补成 `plugin.<本插件 id>.<名字>`，名字可以含点，例如 `drive.list`）。

## 能力清单

| 能力 id | 通道 | 可注册阶段 | 合并 | 派发 | 超时 | 缓存 | MCP 工具 |
|---|---|---|---|---|---|---|---|
| `musicxx.player.beforePlaySong` | - | impl / after | veto | async | - | - | - |
| `musicxx.player.source.beforeParse` | - | impl / after | first | async | - | - | - |
| `musicxx.player.source.resolve` | - | impl | first | async | 5000 ms | 按身份 | - |
| `musicxx.player.seek` | 命令、插件 | 只能 before / after | - | async | 500 ms | - | - |
| `musicxx.player.volume` | - | 只能 before / after | - | async | 500 ms | - | - |
| `musicxx.player.speed` | - | 只能 before / after | - | async | 500 ms | - | - |
| `musicxx.player.pitch` | - | 只能 before / after | - | async | 500 ms | - | - |
| `musicxx.player.error` | - | impl / after | veto | async | - | - | - |
| `musicxx.lyric.load` | - | before / impl / after | first | async | 8000 ms | 按身份 | - |
| `musicxx.lyric.load.musicxx_server` | - | before / impl / after | first | async | 8000 ms | 按身份 | - |
| `musicxx.lyric.load.cache` | - | before / impl / after | first | async | 8000 ms | 按身份 | - |
| `musicxx.lyric.load.builtin` | - | before / impl / after | first | async | 8000 ms | 按身份 | - |
| `musicxx.lyric.load.builtout` | - | before / impl / after | first | async | 8000 ms | 按身份 | - |
| `musicxx.lyric.load.match` | - | before / impl / after | first | async | 8000 ms | 按身份 | - |
| `musicxx.media.spectrum.extract` | MCP 工具 | impl | first | async | 30000 ms | 按身份 | `GetAudioSpectrum` |
| `musicxx.media.chorus.analyse` | MCP 工具 | impl | first | async | 20000 ms | 按身份 | `AnalyseAudioChorus` |
| `musicxx.media.palette.analyse` | MCP 工具 | impl | - | async | 800 ms | 按身份 | `AnalyseImageMainColors` |
| `musicxx.feature.point.call` | 命令、插件 | before / impl / after | first | async | - | - | - |
| `musicxx.app.foreground` | 命令、插件 | impl | - | async | - | - | - |
| `musicxx.app.background` | 命令、插件 | impl | - | async | - | - | - |
| `musicxx.player.play` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.player.pause` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.player.toggle` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.player.stop` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.player.next` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.player.prev` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.player.playSrc` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.player.setVolume` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.player.setSpeed` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.player.setPitch` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.player.setQuality` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.player.setLoopMode` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.player.mpv.get` | 命令、MCP 工具、插件 | impl | - | async | 5000 ms | - | `GetMpvProperty` |
| `musicxx.player.mpv.set` | 命令、MCP 工具、插件 | impl | - | async | 5000 ms | - | `SetMpvProperty` |
| `musicxx.player.mpv.command` | 命令、MCP 工具、插件 | impl | - | async | 5000 ms | - | `RunMpvCommand` |
| `musicxx.player.mpv.filter` | 命令、MCP 工具、插件 | impl | - | async | 5000 ms | - | `SetMpvFilter` |
| `musicxx.songlist.list` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.songlist.listSongs` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.songlist.play` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.songlist.addSong` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.songlist.removeSong` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.songlist.create` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.songlist.updateSong` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.songlist.favorite` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.songlist.sync` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.playlist.listSongs` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.playlist.playSong` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.playlist.addSong` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.playlist.removeSong` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.playlist.updateSong` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.lyric.current.get` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.lyric.current.update` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.lyric.current.updateContent` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.lyric.getBySrc` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.lyric.search` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.ui.toast` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.ui.dialog` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.ui.overlay.show` | 插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.ui.overlay.close` | 插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.ui.overlay.showSheet` | 插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.ui.openRoute` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.ui.playing.openMenu` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.ui.drive.pickSongs` | 插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.ui.drive.pickDir` | 插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.ui.drive.browse` | 插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.ui.setFontFamily` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.ui.getFontFamily` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.ui.setAnimatedLevel` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.ui.setParticleAnimate` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.ui.setSongIconRotate` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.ui.setSongIconWave` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.theme.setCurrent` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.theme.setMode` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.theme.setTempMode` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.ui.loadFont` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.storage.get` | 插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.storage.set` | 插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.storage.delete` | 插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.storage.list` | 插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.net.fetch` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.net.download` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.render.list` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.render.current` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.render.select` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.player.audioEffect.list` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.player.audioEffect.get` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.player.audioEffect.set` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.media.palette.get` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.media.cover` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.media.cover.set` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.media.cover.resolve` | - | before / impl / after | first | async | 3000 ms | - | - |
| `musicxx.media.palette.set` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.media.bindImage` | 插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.media.unbindImage` | 插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.media.spectrum` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.media.kernel` | 命令、MCP 工具、插件 | impl | - | async | 5000 ms | - | `GetMediaKernelInfo` |
| `musicxx.feature.point.list` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.feature.point.define` | 插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.feature.point.undefine` | 插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.source.define` | 插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.source.undefine` | 插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.source.list` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.source.browse` | 插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.capability.subscribers` | 插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.host.openUrl` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.host.clipboard` | 命令、插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.host.getPath` | 插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.stats.reportMemory` | 插件 | impl | - | async | 5000 ms | - | - |
| `musicxx.stats.reportMetric` | 插件 | impl | - | async | 5000 ms | - | - |

## 载荷与回答

### `musicxx.player.beforePlaySong` 播放前

载荷 `{sid, song, mode, quality}`：`song` 是只读视图（`sid/name/artist/album/durationMs/year/genre/srcKey/audio[]/video[]`，不含音频直链），`mode` = 播放模式、`quality` = 当前音质。裁决只实现 `{"action":"skip"}` = 跳过本曲（走应用既有的切下一曲流程）；`patch`（换音源/改音质）当前版本未实现，会被记一条日志后忽略。异步派发：切歌不会被插件拖慢；等待期间若又来了新的切歌请求，本次裁决被丢弃（插件可以自己用 `sid` 判断）。

### `musicxx.player.source.beforeParse` 音源解析前

载荷 `{sid, srcKey, src, index, tryLocalOrCache}`：`src` 是音源视图（`type/empty/srcKey/quality/durationMs/hashMd5/size`），`index` = 本轮解析的音源下标。裁决：`{"action":"skip"}` = 本轮不解析该音源（调用方接着试下一个）；`patch.src = {"type":"Local|UrlLink|Bili|...","src":"<新地址>","info":{...}}` = 只替换本轮使用的音源（不改动歌曲实体的音源列表），字段不合法会被忽略。异步派发（不卡住解析），等待期间切歌则本次裁决作废。

### `musicxx.player.source.resolve` 音源解析

载荷 `{srcKey, type, src, crudeType, crudeSrc, fileName, durationMs, quality, isMediaSrc, pluginType, caller}`：主程序要把一个音源解析成**播放器内核能打开的地址与请求头**时派发一次（外置 → 内置插件 → 应用自身，第一个给出合法结果的生效）。`src` = 本轮要解析的音源（插件自己的音源就是它写下的资源 id / 路径；内置来源是已经解析出来的临时直链或本地路径 —— 实现这条能力的插件看得到这些内容，这是它能换地址的前提）；`crudeSrc` = 歌曲实体里保存的原始地址；`type` / `crudeType` = 对应的来源类型（`link` / `bili` / `aliDrive` / `local` 等；插件音源是空串）；`pluginType` = 本轮音源的插件音源类型 id（`plugin.<插件 id>.<类型名>`；不是插件音源则为空串）；`quality` = 用户当前音质偏好（枚举名小写）；`isMediaSrc` = false 表示这次要的是封面（媒体库来源）；`caller` = 谁在问（空串 = 主程序自己或命令行）。处理器返回 `{"src": {"path": "<地址>", "headers": {"Referer": "..."}, "kind": "url", "expireAtMs": 0, "size": 0, "durationMs": 0, "quality": "hq"}}`（`src` 也可以直接给一个地址字符串）：这就是播放器要打开的地址与请求头。`path` 支持网络地址、本地文件路径、`content://`、`asset://`；`kind` 取 `url` / `file` / `content` / `asset`，缺省按地址自己判断（判断错会让本地文件被当成网络流去做缓存）；`headers` 可省；`expireAtMs` 给临时地址的过期时间戳（毫秒，只用于排障）。**回答请用 `patch` 包一层**（写成 `patch.src`）：插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历。给自己的音源给出地址与请求头、给内置来源换地址（加代理、换 CDN、统一请求头）都是这条路。不返回 / 结果不合法 = 这次不提供，主程序接着问内置实现。返回 `{"action":"cancel"}` 是拦下这次调用：不再问别的实现，该音源会解析失败（只在明确知道它不该播时用）。异步派发：主程序最多等 5 s（超时按没给处理，改走内置实现），处理器请尽快返回。主程序只在"这条能力有处理器"时才派发。

### `musicxx.player.seek` 跳转播放位置

载荷 `{sid, from, to, durationMs}`：应用**要把播放位置改到 `to`（毫秒）**时派发一次。拖动进度条、点歌词、快捷键快进快退、局域网控制、命令行 / 能力 / 脚本、恢复播放进度都会走这里。`from` = 改之前的进度，`durationMs` = 总时长（未知为 0），`sid` = 当前歌曲会话 id。**跳转本身由应用执行（实现固定），插件只能插手两头**：`before` 回答 `{"patch":{"to":60000}}` = 换这次的目标位置，`{"action":"cancel"}` = 这次不跳（界面回弹）；`after` 回答 `{"value":{"patch":{"to":60000}}}` = 改最终位置，`{"action":"drop"}` = 这次的值作废（仍按 `before` 之后的目标位置跳：这次改动已经做过了，后链拦不下它）。`impl` 阶段不开放（注册被拒，返回码 -4）：跳不跳不能让插件换掉。目标位置最终会被夹到 0~总时长。**异步派发、最多等 0.5 s**：超时或不回答 = 按原目标跳；等待期间切歌则本次裁决作废（插件不用自己判断 `sid`）。处理器里不要再调 `musicxx.player.seek`（会再派发一次）。

### `musicxx.player.volume` 调整音量

载荷 `{from, to}`：应用**要把音量改成 `to`** 时派发一次（`from` = 当前音量，`to` = 这次要用的音量，都是 0~1，可能略大于 1 表示增益）。音量滑块、快捷键、静音 / 取消静音、局域网控制、命令行 / 能力、启动时恢复设置都会走这里。**改值本身由应用执行（实现固定），插件只能插手两头**：`before` 回答 `{"patch":{"to":0.5}}` = 改这次的目标值，`{"action":"cancel"}` = 这次不改（保持原音量，界面回弹）；`after` 回答 `{"value":{"patch":{"to":0.5}}}` = 改最终值，`{"action":"drop"}` = 这次的值作废（仍按 `before` 之后的目标值改：这次改动已经做过了，后链拦不下它）。`impl` 阶段不开放（注册被拒，返回码 -4）。最终值会被夹到应用的合法范围（0~1）。**异步派发、最多等 0.5 s**：超时或不回答 = 按原值执行。处理器里不要再调 `musicxx.player.setVolume`（会再派发一次）。

### `musicxx.player.speed` 调整播放速度

载荷 `{from, to}`：应用**要把播放速度改成 `to`** 时派发一次（`from` = 当前速度，`to` = 这次要用的速度，单位是倍速）。速度菜单、长按加速 / 减速、命令行 / 能力、定时停止的变速、启动时恢复设置都会走这里。**改值本身由应用执行（实现固定），插件只能插手两头**：`before` 回答 `{"patch":{"to":1.5}}` = 改这次的目标值，`{"action":"cancel"}` = 这次不改（界面回弹）；`after` 回答 `{"value":{"patch":{"to":1.5}}}` = 改最终值，`{"action":"drop"}` = 这次的值作废（仍按 `before` 之后的目标值改）。`impl` 阶段不开放（注册被拒，返回码 -4）。最终值会被夹到应用的合法范围（0.25~8）。**异步派发、最多等 0.5 s**。长按加 / 减速这类连续变化会连派多次，处理器要保持快，处理器里也不要再调 `musicxx.player.setSpeed`。

### `musicxx.player.pitch` 调整音调

载荷 `{from, to}`：应用**要把音调改成 `to`** 时派发一次（`from` = 当前音调，`to` = 这次要用的音调；1 是原调，范围 0.25~2）。播放页的音调菜单、快捷键、音效切换时的复位、启动时恢复设置都会走这里。**改值本身由应用执行（实现固定），插件只能插手两头**：`before` 回答 `{"patch":{"to":1.1}}` = 改这次的目标值，`{"action":"cancel"}` = 这次不改（界面回弹）；`after` 回答 `{"value":{"patch":{"to":1.1}}}` = 改最终值，`{"action":"drop"}` = 这次的值作废（仍按 `before` 之后的目标值改）。`impl` 阶段不开放（注册被拒，返回码 -4）。最终值会被夹到应用的合法范围（0.25~2）。**异步派发、最多等 0.5 s**。要主动改音调就调能力 `musicxx.player.setPitch`；处理器里不要再调它（会再派发一次）。

### `musicxx.player.error` 播放失败

载荷 `{sid, srcKey, isNowUseSrc, errorCount, durationMs, positionMs}`（`errorCount` 已包含本次失败）。裁决：`{"action":"stop"}` = 停止播放；`{"action":"skip"}` = 跳到下一曲；`{"action":"continue","patch":{"tryNextSrc":false}}` = 不再尝试当前源（跳过忽略错误并重试的分支，直接按主程序的换源/下一曲策略走）。异步派发（不占用调用线程，主程序不会被插件拖住）：等待期间切歌则本次裁决作废；处理器请尽快给出裁决，本次失败处理会等它结算。主程序只在"这条能力有处理器"时才派发。

### `musicxx.lyric.load` 歌词加载

载荷 `{sid, song, audio, parsed, path, enable, keyword, artist, lrcid, position, caller}`：主程序要为**一首歌加载歌词**时派发一次（外置 → 内置插件 → 应用自身，第一个给出合法歌词的生效）。`song` 是歌曲只读视图（`sid/name/artist/album/durationMs/srcKey/audio[]/video[]`，不含音频直链）；`audio` = 本轮音源视图（`type/srcKey/quality/durationMs/hashMd5/size`）；`parsed` = 实际播放用的那份音源（缓存文件 / 临时直链，字段同上）；`path` 只有本地、缓存、`content://`、asset 才有值（网络来源是空串 —— 要直链请自己走 `musicxx.net`）；`enable` = 这一轮允许做的事 `{musicxx_server（读拟声云歌词）, cache（读应用的歌词缓存）, builtin（读内置歌词：播放器元数据与媒体文件标签）, builtout（读同名外置歌词）, match（自动匹配）, upload（可自动上传）}`；`keyword` / `artist` = 自动匹配用的搜索串（前链可以改写）；`lrcid` > 0 = 强制用这一条云歌词；`position` 非空 = 强制读这个位置（文件路径，或歌词来源自己的标识）；`caller` = 谁在问（空串 = 主程序自己或命令行）。处理器回答 `{"patch": {"lyric": {"content": "[00:12.34]…", "lrcid": 0, "sourceType": "builtin", "offsetMs": 0}}}`：`content` 是 LRC 文本（主程序自己解析，不认别的格式），`sourceType` 取 `builtin` / `builtout` / `local` / `musicxx_server` / `match`（另有 `builtinCache` / `builtoutCache`；省掉按 `musicxx_server` 算；写 `builtin` / `builtout` 的歌词会像应用自己读到的那样进歌词缓存，并按 `enable.upload` 决定是否自动上传），`offsetMs` = 这首歌的歌词偏移（毫秒，可省）。`content` 为空、解析不出歌词行 = 这次不提供，主程序接着问下一个实现。`{"patch": {"lyric": {"disabled": true}}}` = 这首歌**禁用歌词**（这是合法结果：界面显示『禁用歌词』，后续读取全部不做）。`{"action": "cancel"}` 是拦下这次调用：这次不再问别的实现，也不写缓存。后链回答 `{"value": {"lyric": {…}}}` 换掉这次的结果，`{"action": "drop"}` = 这次按没有歌词处理。**回答请用 `patch` 包一层**：插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历。前链能改写 `keyword` / `artist` / `lrcid` / `position` 与 `enable` 里的各项（回答 `{"patch": {…}}`），`{"action": "cancel"}` = 这次不加载歌词。异步派发：主程序最多等 8 s（超时按没给处理，接着走应用自己的加载）。应用自己的实现会把这一轮拆成五个细分埋点按顺序询问（`musicxx.lyric.load.musicxx_server` → `.cache` → `.builtin` → `.builtout` → `.match`）；**插件在这里接管之后，细分埋点不会再被询问**。

### `musicxx.lyric.load.musicxx_server` 歌词加载 · 拟声云歌词

载荷与回答形状同 `musicxx.lyric.load`。这一阶段读**拟声云歌词**（自建服务 / 歌曲绑定的那个歌词来源）：用户给这首歌指定过的歌词（按音源身份记在应用里的那条），或歌曲自带的歌词来源（`myId` / 链接 / 云盘 / 媒体库 / 本地文件）。应用自己的实现在这里调自建服务的歌词接口、读绑定的本地歌词文件，命中时 `sourceType` 是 `musicxx_server` 或 `local`。`enable.musicxx_server` 为 false 时主程序不会问这个点（`enableCloudLyric: false` 的调用点是『只从文件读歌词』的场景）。回答 `{"patch": {"lyric": {"disabled": true}}}` 表示这首歌禁用歌词，后续阶段不再执行。

### `musicxx.lyric.load.cache` 歌词加载 · 歌词缓存

载荷与回答形状同 `musicxx.lyric.load`。这一阶段读**歌词缓存**：应用自己保存过的歌词内容（按音源身份、md5、sha1、大小与时长命中）。`enable.cache` 为 false 时不会被询问；命中时 `sourceType` 是 `builtinCache` / `builtoutCache`（这份缓存是上一次读到内置 / 外置歌词时写下的）。插件接管后可以换成自己的歌词库（例如本地收藏的歌词文件索引）。

### `musicxx.lyric.load.builtin` 歌词加载 · 内置歌词

载荷与回答形状同 `musicxx.lyric.load`。这一阶段读**内置歌词**，两条路合在一起：播放器当前音源**已经读到的元数据标签**（切歌后它比重新解码文件先到，应用最多等 300 ms），以及由媒体解码器**从音频文件 / 缓存流里读出的标签歌词**（`path` 就是那个文件；原来叫 `load_lyric_builtIn`）。会在四种情况下被询问：前链指定了 `position`（只读那个文件）、这一轮允许读缓存且来源类型允许（看播放器元数据）、解析出来的音源是缓存文件 / 链接（不管 `enable.builtin`，原来就是先读一遍它的标签）、或 `enable.builtin` 为 true 且来源类型允许。应用自己的实现还会按来源类型跳过明显没有标签的那些（Bili、asset、缓存 id 等），插件替换实现后不再受这条限制。`sourceType` 缺省 `builtin`（会进歌词缓存），命中后主程序还可能自动上传到自建服务（看 `enable.upload`）。

### `musicxx.lyric.load.builtout` 歌词加载 · 外置歌词

载荷与回答形状同 `musicxx.lyric.load`。这一阶段读**外置歌词**：与音频文件同名的 `.lrc`（本地、链接、WebDAV），以及云盘与媒体库各自的同名歌词查找（阿里云盘、百度网盘、115、Subsonic / Jellyfin / Emby 的歌词接口；最后两种的 `path` 是空串，标识在歌曲视图里）。`enable.builtout` 为 true 且来源类型允许、或前链指定了 `position` 时会被询问；应用自己的实现会跳过明显没有同名文件的来源（Bili、`myId`、`myLink`、缓存、asset 等），插件替换实现后不再受这条限制。`sourceType` 缺省 `builtout`（会进歌词缓存）。

### `musicxx.lyric.load.match` 歌词加载 · 自动匹配

载荷与回答形状同 `musicxx.lyric.load`。这一阶段做**自动匹配**：前面几个阶段都没拿到歌词时，按歌名 / 歌手 / hash / 大小 / 时长向自建服务要推荐歌词。`enable.match` 为 true 时会被询问（调用方决定；设置里的「自动匹配歌词」开关在主程序那边就并进了这个值）。应用自己的实现还会跳过 15 分钟以上、歌名为空或名字里有『纯音乐 / 伴奏 / instrumental』的歌（插件替换后不再受这条限制）。供参考：`keyword` / `artist` 可以在前链里改写（例如换成自己的搜索串）。`sourceType` 缺省 `match`。插件接管后可以换成自己的匹配方式（歌词站搜索、按音频指纹匹配等）；匹配不到就返回空，主程序按『这首歌没有歌词』处理。

### `musicxx.media.spectrum.extract`

载荷 `{srcKey, name, artist, kind, path, durationMs, frames, binCount, frameHz, maxSeconds, caller}`（`caller` = 谁在问：空串 = 主程序自己取值或命令行，非空 = 主动调用它的插件 id）：主程序要提取一首歌的**整曲频谱**时派发一次（`path` 是本地/缓存文件路径，不是网络直链；`kind` = `local` / `cache` / `content` / `other`；`frames` 按时长推算、只作提示）。处理器返回 `{"spectrum": {"spectrums": "<base64>", "waveform": "<base64，可省>", "binCount": 256, "frameHz": 10}}` 就用插件的数据替代内置提取（内置实现是主程序自己用 mediaxx 提取）：`spectrums` 是整曲频点（帧数 × 256 个字节，0~255 表示 -80~0 dB，默认每秒 10 帧），`waveform` 是每帧 1 字节 RMS 响度（0~255 表示 -60~+20 dB；省掉或长度对不上时按每帧频点均值补一份近似响度）。不返回 / 没有 `spectrum` / 数据不合法（不是 base64、长度不是 256 的整数倍、`binCount` 与 `frameHz` 不是 256 与 10）= 这次不提供，主程序接着用已登记的实现（内置『音乐动效』）。**回答请用 `patch` 包一层**（写成 `patch.spectrum`）：插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历，多个实现时裸字段会被后面的处理器覆盖（只有一个实现时两种写法等价）。异步派发：主程序最多等 30 s（超时按没给处理）；等待期间切歌则本次结果会被丢弃（可用 `srcKey` 自行校验）。主程序只在"这条能力有处理器"时才派发。返回 `{"action":"cancel"}` 是拦下这次调用：主程序这次不再询问其它实现（内置『音乐动效』也不跑，播放页频谱与副歌分析都拿不到数据），这次结果不写缓存；拦下只在本次询问有效，处理器被摘除（插件停用 / 卸载 / 关闭『拟声++』）后自动还原。要用它请先确认自己的数据已经算好（例如自己起线程提前算完），否则用户会看到功能凭空消失。**这个功能埋点对外开放调用**（能力 `musicxx.feature.point.call`）：外置插件、命令行与 agent 能主动调它并拿到原始数据，参数与结果见 `capability.list` 里的 `callDoc`。调用同样按这条处理器链问一遍，但**不写主程序的值仓、不改主程序状态**（只读值仓，绝不记录拦下标记），同一个功能埋点 + 同一个调用方的上一次调用没结束时拿到 `busy`。

### `musicxx.media.chorus.analyse`

载荷 `{srcKey, name, artist, kind, path, durationMs, hasWave, frames, binCount, frameHz, spectrums?, waveform?, config, caller}`（`caller` = 谁在问：空串 = 主程序自己取值或命令行，非空 = 主动调用它的插件 id）：主程序要分析一首歌的**副歌（高潮）区段**时派发一次。`hasWave` 为真时附带主程序已经拿到的整曲频谱（`spectrums` / `waveform` 的 base64，4 分钟歌曲约 800 KB —— 用不上的插件请不要注册这条能力）；`config` 是内置算法的参数（`bandCount` / `hopFrames` / `windowFrames` / `minRepeatLagSeconds` / `segmentSimilarity` / `minSegmentSeconds` / `maxSegmentSeconds` / `mergeGapSeconds`）。处理器返回 `{"chorus": {"sections": [{"start": 64000, "end": 100000, "confidence": 0.8, "energy": 0.7, "similarity": 0.9, "repeat": 3}], "totalDuration": 253000, "analyseTimeMs": 120}}`（时间单位毫秒）就用插件的区段替代内置分析；`sections` 是必需项（空数组 = 分析过了、这首歌没有副歌），每段至少要有 `start` 与 `end`，其余字段缺失按 0 处理。不返回 / 缺少 `sections` / 一个区段都解析不出来 = 这次不提供。**回答请用 `patch` 包一层**（写成 `patch.chorus`）：插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历，多个实现时裸字段会被后面的处理器覆盖（只有一个实现时两种写法等价）。异步派发：主程序最多等 20 s；等待期间切歌则本次结果会被丢弃（可用 `srcKey` 自行校验）。主程序只在"这条能力有处理器"时才派发。返回 `{"action":"cancel"}` 可以拦下这次调用：主程序这次不再走内置分析（进度条上的副歌标记就没有了），处理器被摘除后自动还原。**这个功能埋点对外开放调用**（能力 `musicxx.feature.point.call`）：外置插件、命令行与 agent 能主动调它（参数与结果见 `capability.list` 里的 `callDoc`）；调用会先取一次频谱，**这一层同样不写值仓**，也不改主程序状态，同一个功能埋点 + 同一个调用方的上一次调用没结束时拿到 `busy`。

### `musicxx.media.palette.analyse`

载荷 `{srcKey, name, artist, hasCover, night, caller, cover}`：主程序要为一张封面取色（内置实现是封面颜色分析）时派发一次。`caller` = 谁在问（空串 = 主程序自己取值或命令行，非空 = 主动调用它的插件 id）；`cover` = 这次算哪一张图：`{kind: current | file | url | data, path?, url?, bytes?, imageBase64?, coverKey}`（`current` = 当前歌曲的封面，`file` / `url` / `data` 是调用方指定的那一张；`path` / `url` 是对应的地址，`data` 给 `bytes` 与 `imageBase64` —— 图太大时只有 `bytes`；`coverKey` 是这次的身份，插件可以按它自己缓存）。老插件不认 `cover` 也能跑：它们只看 `srcKey` / `name` / `artist` / `night`，对 `current` 这一种用法的结果是对的；别的来源它们给不出（返回空），主程序接着走自己的分析 —— 不会给错数据。处理器返回 `{"colors": {"main": "#rrggbb", ...}}`（键可取 `main` / `light` / `lightMuted` / `dark` / `darkMuted`，写法 `#rrggbb` 或 `#aarrggbb`）就用插件提供的这套颜色替代内置分析：它写进与内置分析同一批字段（**只在主程序自己问当前封面时**；外置通过 `musicxx.feature.point.call` 调它时只拿数据、不写字段），内置背景、插件背景与 `musicxx.icon.*` 来源都会拿到；不返回 / 没有 `colors` / 颜色一个都解析不出来 = 这次不提供，主程序照旧走内置分析。**回答请用 `patch` 包一层**（写成 `patch.colors`）：插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历，多个实现时裸字段会被后面的处理器覆盖（只有一个实现时两种写法等价）。异步派发：不会卡住渲染准备；等待期间切歌时本次结果会被丢弃（可用 `srcKey` 自行校验）。返回 `{"action":"cancel"}` 可以拦下这次调用：主程序这次不再走内置取色（保持上一次的颜色），处理器被摘除后自动还原。**这个功能埋点对外开放调用**（能力 `musicxx.feature.point.call`）：外置插件、命令行与 agent 能主动调它并拿到 5 个原始色槽位，参数与结果见 `capability.list` 里的 `callDoc`。**调用没有副作用**：不写封面取色字段、不改内置背景（要改观感请用 `musicxx.media.palette.set`），同一个功能埋点 + 同一个调用方的上一次调用没结束时拿到 `busy`。

### `musicxx.feature.point.call` 调用功能埋点

三个阶段都在同一条通道上（`before` / `impl` / `after`），载荷都带 `phase` 以便同一个处理器分辨自己被叫的阶段：`before` 是 `{point, phase, args, caller}`（回答 `{"patch": {"args": {...}}}` 改这次调用的参数、`{"action": "cancel"}` 这次不执行实现）、`impl` 是 `{point, phase, args, caller}`（回答见下）、`after` 是 `{point, phase, args, value, caller}`（回答 `{"value": ...}` 改最终结果、`{"action": "drop"}` 这次没有结果）。以下是实现链（`impl`）的规则：有人要算一个**外置插件声明的功能埋点**时派发一次（`point` = 功能埋点 id，`args` = 调用方给的参数，`caller` = 调用方插件 id，空串 = 应用自己或命令行）。这是**一条通用的实现通道**：插件声明的功能埋点共用它，而不是每个点各占一条能力 id（插件框架只认固定的能力表，见 docs/plugin-guide.md）。处理器只实现自己负责的点：不是自己的点直接返回空（空 = 不回答）；是自己的点就返回 `{"patch": {"value": <任意 JSON>}}` —— **必须用 `patch` 包一层**，插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历，否则后面的处理器会覆盖你的结果。`value` 键就是“实现方给出了结果”的判据（值可以是 null，例如“算过了、结果是空”）。返回 `{"action":"cancel"}` 是拦下这次调用：这次不再问别的实现。**同一条通道上所有实现都会收到同一次调用**，请按 `point` 过滤，不要假设只有自己收到。不要回答自己发起的调用（`caller` 等于自己的插件 id 时返回空；SDK 糖会自动做）。主程序不设超时，最多等到调用方自己的动作超时；同一个功能埋点 + 同一个调用方的上一次调用没结束时，调用方拿到 `busy`。功能埋点由插件用动作 `musicxx.feature.point.define` 声明、`musicxx.feature.point.undefine` 收回，声明跟着插件生命周期走。

### `musicxx.player.mpv.get` 读 mpv 属性

参数 `name`（mpv 属性名，原样交给 mpv）。返回 `{ok, name, value}`，`value` 是 mpv 的文本形态（布尔是 yes / no）。**不做禁读**：`http-header-fields`（当前音源的请求头）、`path`（解析后的播放地址）也能读，里面可能有 token / cookie。节点型属性（`track-list` / `chapter-list`）要读子属性路径（`track-list/0/codec`），读不了时 `error` 是 `invalid_argument`。**高危接口**：直接操作播放内核，写错、执行错命令的后果由调用方承担（见 `docs/plugin-api.md` 的「mpv 控制接口」）。要观察属性变化就订阅广播 `musicxx.player.mpv.<属性名>`，不用调这条能力。

### `musicxx.player.mpv.set` 写 mpv 属性

参数 `name` / `value`（都是文本；布尔写 `yes` / `no`）。返回 `{ok, name, value, mpvCode}`，`mpvCode` 是 libmpv 的返回（0 = 成功）。**不判断属性归属**：`volume` / `pause` / `time-pos` / `af` / `demuxer-cache-dir` 这类会和应用抢同一个值、或往磁盘写东西的属性也能写；应用自管的属性写完之后界面与设置里的值不会跟着变（下一次应用设置会覆盖回来）。要长期生效的滤镜用 `musicxx.player.mpv.filter`（按槽位加），直接写 `af` 是整串替换。**高危接口**：后果由调用方承担。

### `musicxx.player.mpv.command` 执行 mpv 命令

动词 `command` 是 mpv 自己的说法（`mpv_command`）：这条能力就是「执行一条 mpv 命令」，不另起语义动词。参数 `args`（字符串数组，第一项是命令名，后面是参数，至少 1 项），原样交给 `mpv_command_async`。**没有白名单**：`run` / `subprocess`（执行本机程序）、`loadfile`（换片源）、`screenshot-to-file` / `dump-cache`（写任意路径）、`script-*`、`quit`、`playlist-*` 都会真的执行。返回 `{ok, command, mpvCode}`。不支持「一整行文本」的写法（libmpv 只有同步的 `mpv_command_string`，本接口只用异步接口）。**高危接口**：后果由调用方承担。

### `musicxx.player.mpv.filter` 音视频滤镜槽位

动词 `filter` 是 mpv 自己的说法（`af` / `vf` 的滤镜链）：这条能力管的就是滤镜槽位。参数 `action`（`set` / `remove` / `list`，缺省 `set`）、`target`（`audio` / `video`，缺省 `audio`）、`name`（槽位名，1~64 位字母数字与 `_ - .`）、`value`（滤镜串，`set` 必填）。插件调用时槽位键自动加 `plugin:<插件 id>:` 前缀，两个插件不会撞名、卸载时按前缀摘掉；应用自己的三段（均衡器 / 音效 / 音调）排在插件槽位前面。写入被 mpv 拒绝时先把槽位还原再写一次，应用原本的音效仍然生效。返回 `{ok, target, name, slot, value, chain}`；`list` 返回 `{ok, audio:{槽位:滤镜串}, video:{...}}`。**高危接口**：滤镜串写错的后果由调用方承担。

### `musicxx.songlist.favorite` 收藏当前歌曲

参数 `{favorite?}`：不写 = 按当前状态取反；写 `true` / `false` = 设成那个状态（已经是目标状态时不再发请求）。

返回 `{ok, favorite, srcKey}`：`favorite` = 动作之后当前歌曲在「我喜欢」里的状态，`srcKey` = 当前歌曲的音源身份（完整 md5 串，没有音源身份时为空串）。当前没有歌曲 / 歌曲没有音源身份 → `{ok:false, error:"当前没有可收藏的歌曲"}`。

行为与播放页的收藏按钮一致：未登录时不会收藏（应用自己提示登录），当前播放列表就是「我喜欢」时同步增删播放队列。

### `musicxx.ui.playing.openMenu` 播放页「更多」菜单

无参数，打开菜单后**立即**返回 `{ok:true}`（不等用户关掉菜单）。

菜单内容与内置的完全一样：歌词来源 / 选择歌词 / 刷新歌词 / 局域网投送 / 加入歌单 / 移除当前歌曲 / 快捷功能 / 视频开关 / 播放模式 / 时长显示 / 沉浸模式。插件替换了整块控制按钮栏（`musicxx.ui.playing.controlBar`）又想保留这个入口时用它。

### `musicxx.ui.setAnimatedLevel` 设置动画等级

参数 `{level}`：`level` 取 `undefined` / `disable` / `low` / `medium` / `high` / `max`（枚举 id，也接受原来的整数编码 -1/10/20/30/40/50）。走与设置页**完全相同**的写入路径（`theme_s.setEnableAnimatedLevel`），改完发布广播 `musicxx.ui.animatedLevel`。

回答 `{ok: true, level}`；取值非法时返回失败并说明原因（`level` 不能识别）。值没变时也算成功（不会重复写库）。

### `musicxx.ui.setParticleAnimate` 设置粒子动画

参数 `{value}`：布尔开关（也接受 `1` / `0` / `true` / `false` 文本）。走设置页同一条写入路径（`setting_s.enableSnappableAnimate.setValue`），改完发布广播 `musicxx.ui.particleAnimate`。

回答 `{ok: true, value}`；值解析不出布尔时返回失败并说明原因。

### `musicxx.ui.setSongIconRotate` 设置歌曲图旋转

参数 `{value}`：布尔开关（也接受 `1` / `0` / `true` / `false` 文本）。走设置页同一条写入路径（`setting_s.enableIconRotate.setValue`），改完发布广播 `musicxx.ui.songIconRotate`。

回答 `{ok: true, value}`；值解析不出布尔时返回失败并说明原因。

### `musicxx.ui.setSongIconWave` 设置歌曲图波浪

参数 `{value}`：布尔开关（也接受 `1` / `0` / `true` / `false` 文本）。走设置页同一条写入路径（`setting_s.enableIconWave.setValue`），改完发布广播 `musicxx.ui.songIconWave`。

回答 `{ok: true, value}`；值解析不出布尔时返回失败并说明原因。

### `musicxx.theme.setCurrent` 切换当前主题

参数 `{id}`：主题 id（`mimicryLight` / `mimicryNight`，也认明暗写法 `light` / `night`，即 `musicxx.theme.available` 里的 `kind`）。走 `theme_s.setIndex`：只切显示，**不改主题模式**、不写用户的模式设置、不暂停跟随（下一次跟随判断照样会改回来 —— 要“切了就停在那”用 `musicxx.theme.setTempMode`）。改完发布广播 `musicxx.theme.current`。

回答 `{ok: true, id}`；id 非法时返回失败并说明原因。

### `musicxx.theme.setMode` 设置主题模式

参数 `{mode}`：`light` / `night` / `followSystem` / `followTime` / `followScreenBrightness`（也接受原来的整数编码 1~5）。写进用户的持久设置并立即按新模式切（`theme_s.setUseModel`），改完发布广播 `musicxx.theme.mode`（以及 `musicxx.theme.current`、`musicxx.state.app` 里的 `isNight`）。

回答 `{ok: true, mode}`；取值非法时返回失败并说明原因。

### `musicxx.theme.setTempMode` 临时切换主题

参数 `{target}`：主题 id 或 `toggle`（按当前明暗互切）。与侧边抽屉按钮、局域网控制的临时切主题是同一语义：走 `theme_s.setCustom`，**会暂停主题跟随**（用户下次手动切模式才恢复）。

它表达的是一次操作、没有当前值，所以只有能力、没有对应广播。回答 `{ok: true, id}`；`target` 既不是主题 id 也不是 `toggle` 时返回失败并说明原因。

### `musicxx.player.audioEffect.list` 列出音效

音效 id 有两种：内置预设 `musicxx.<短名>`（例如 `musicxx.bassUp`），插件注册的音效就是它的 UI 项 id（`plugin.<插件id>.<短名>`）。`enabled` = 『音效』插件当前是否启用（关着时音效不会真的生效）。`selected` 标出当前选中的那一项。

### `musicxx.player.audioEffect.get` 当前音效

没有音效时返回 `{ok: true, enabled, id: "", title: ""}`，不是错误。

### `musicxx.player.audioEffect.set` 设置音效

`id` 必须来自 `musicxx.player.audioEffect.list`；未知 id 返回 `unknown_item`。设置会写库并立即应用（写 mpv `af`，与界面里选音效完全同一条路）。音效与均衡器、自定义音调互斥：选用音效会关掉均衡器；带 `pitch` 的音效会先把用户音调复位。`enabled` 为假表示『音效』插件没启用，配置已写入但当前不生效。停用 / 卸载插件后选中的插件音效会回退成「没有音效」。

### `musicxx.media.cover.set` 设置当前歌曲图片

把一张图片设为**当前正在播放歌曲的图**：播放页大图、通知栏、锁屏、桌面小组件、封面取色都读这一份，设置后立即生效。参数 `icon` 必给、`icon96` 可选，两者都支持三种写法：`{"data": "<图片字节的 base64>", "format": "png", "width": 0, "height": 0}` / `{"path": "<本地绝对路径>"}` / `{"url": "https://…"}`。`data` 的 `format` 可省（按内容识别）；给原始像素（`format: "rgba"`）时必须一起给 `width` / `height`。`data` 会写进图片缓存（`<音源身份>@cover-<内容键>`，给了 `icon96` 时再写一张 `…_96`），单张上限 16 MiB，解码失败会拒绝；`path` / `url` 原样使用、不复制（文件/地址失效时界面回退默认图）。可选 `sid` = 调用方认为的当前歌曲，与当前不一致返回 `stale`。`{"clear": true}` = 删掉这首歌的插件图并重新取一次封面（同时清掉 `set` 锁，回落应用自己的取值链，这条链仍然可以再问 `musicxx.media.cover.resolve`）。回答 `{ok, sid, srcKey, origin, icon, icon96, bytes, width, height}`：`icon` / `icon96` 是 `{kind, type, src}`（`kind` = `cache` / `local` / `network`；`icon96` 没给时为 null）；`bytes` / `width` / `height` 只在 `data` 形态下有意义（其余为 0）。失败回答 `{ok: false, error: …}`，`error` 取 `no_song`（当前没有正在播放的歌曲或它没有音源身份）/ `stale`（`sid` 与当前歌曲不一致）/ `invalid_argument`（参数形状不对、`path` 不存在 / 不是普通文件、`url` 不是 http(s)）/ `too_large`（单张超过 16 MiB）/ `decode_failed`（字节解不出图片）/ `write_failed`（写缓存失败）。**只对本次播放有效**：不写 `song.icon`、不落盘「下次还用这张」，切歌后按正常取值链重新取（要持久换封面请用 `musicxx.songlist.updateSong` / `musicxx.playlist.updateSong` 的 `cover`）。插件 `set` 过的这首歌本次播放内不再被问 `musicxx.media.cover.resolve`（免得应用自己的提取把刚定的图换掉），直到切歌、`clear` 或这张图本身加载失败。`icon96` 只是省一次解码的缩略图（取色与锁屏在它存在时优先用它），**当前歌曲图一般可以不给**，不给时消费方统一用整图。

### `musicxx.media.cover.resolve` 指定当前歌曲图片

主程序要决定**当前这首歌用哪张图**时派发一次。**每一次这首歌加载都会问一次**：切歌、重播、切走再切回来、换音源 / 切音质导致的重新解析、`musicxx.media.cover.set` 的 `clear` 之后、以及当前没有可用封面时的重试，各算一次。读封面的代码（播放页、通知栏、锁屏、桌面小组件、封面取色、`musicxx.media.cover`、渲染项 `cover`）只读应用保存的那一张，**不会触发这条能力**。载荷 `{sid, srcKey, name, artist, album, durationMs, audio, parsed, path, icon, icon96, cache, current, reason, force, caller}`：`audio` = 本轮音源视图（`type/src/customType/fileName/size/durationMs/quality`）；`parsed` = 实际播放用的那一份（本地缓存文件或临时直链，字段同上 + `kind`）；`path` 只有本地 / 缓存 / `content://` / asset 才有值（网络来源是空串，要直链请走 `musicxx.net`）；`icon` / `icon96` = 歌曲元数据里的封面（`song.icon` / `icon_96`），只报 `{hasCover, kind}`（`kind` = `local` / `cache` / `content` / `asset` / `network`；本地 / 缓存类要求文件存在，网络地址不探远程）；`cache` = `{exists, exists96}`，应用图片缓存里这首歌的图在不在；`current` = **现在实际显示的那张图** `{origin, by, icon, icon96}`（`origin` = `initial`（切歌时的初值：元数据封面 / 缓存 id）/ `builtin`（应用的三段取值给的）/ `plugin`（插件给的）/ `set`（插件 `set` 定的）/ `none`（已经清空）；`by` = 产出方 owner 或插件名，初值为空串；`icon` / `icon96` 只报 `{kind, type, src}`，不给直链）；`reason` = `load`（切歌 / 重播 / 切回来）/ `reparse`（换音源、切音质导致的重新解析，同一首歌的重新加载都算）/ `reload`（封面加载失败且当前没有可用图）/ `clear`（`set clear` 之后重算）；`force` = 真时内置实现跳过「缓存已存在」这条短路、重新提取；`caller` = 谁在问（空串 = 应用自己或命令行）。**回答**：`{"patch": {"cover": {"icon": {"data": "<base64>", "format": "png"}}}}`（图片写法与 `musicxx.media.cover.set` 完全一样，`icon` 必给、`icon96` 可选；**必须用 `patch` 包一层**：插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历）；`{"action": "cancel"}` = **这次不修改当前图片**（不取图、不刷通知栏与背景色，界面保持现在显示的那张）。`cancel` 同时就是「就用现在这张」的回答：`current` 显示的是缓存图时它等于「别提取了，就用缓存」，显示的是你上次给的图时它等于「继续用它」。空回答 / 形状不对 / 单张超过 16 MiB = 这次不提供，主程序接着问内置实现（歌曲信息 → 图片缓存 → 媒体内嵌图）。**后链**（`after`）拿到 `value`（`{origin, icon, icon96, width, height}` 的只读摘要，不带图片字节），回答 `{"value": {"cover": {…}}}` 换图（可以只换 `icon96`），或 `{"action": "drop"}` 让这次没有封面。**前链**（`before`）只认 `{"patch": {"force": true}}`（要求内置实现跳过缓存、重新提取）与 `{"action": "cancel"}`（这次不修改）。**三种结果**：有 `cover` = 就用它；前链或实现链 `cancel` = 什么都不做，保持当前图；后链 `drop` 或整条链都没有实现给值（最终为 null）= **清空**（界面回到默认图，没有单独的「没有封面」标记）。**记住结果不阻止下次询问**：这条能力每次加载都会被问到，应用只记住「这一次用了哪张」，用于失败重试与 `set clear`，不会因为记过就不再问你；唯一不重问的情况是插件用 `musicxx.media.cover.set` 定了这首歌的图（本次播放内锁住）。异步派发：最多等 3 s（超时按没给处理），不阻塞起播；插件实现**不受**内置实现那 1 秒延迟影响。处理器里不要再调 `musicxx.media.cover.set`（会多刷一次封面）。常见写法：完全接管用 `impl` 直接回答 `cover`；只在应用取不到时补一张时先看 `icon.hasCover` 与 `cache.exists`，两者都为假才回答；要改写应用已有的封面（加水印、换色、统一风格）用 `after`（它只在实现链拿到值时才会跑，应用本来就没有封面时请走 `impl`）。

### `musicxx.capability.subscribers` 能力订阅 / 实现行

参数 `id` = 能力 id（例如 `musicxx.media.spectrum.extract`），可选 `phase`（before / impl / after）。返回 `{capability, count, handlers}`，每项带 `phase`。命令面是 `capability.subscribers`（同一份实现，参数名同样是 `id`）；插件调用时用全名 `musicxx.capability.subscribers`（能力 id 以 musicxx. / plugin. 开头，命令名没有这个前缀）。
