# 钩子总表（插件作者文档）

> **术语约定**：本文档中的**主程序**指的就是 **musicxx**（Flutter/Dart 业务层）；外置插件的 C++ 框架层称为**插件框架**；插件目前分为**动态库插件**与**JS插件**。
>
> 本文件由 `tools/gen_contract.dart` 从 `tools/hooks.def.json` 生成，请勿手改。
>
> 注册钩子时必须写全名（官方 `musicxx.*`）；插件自定义事件/能力/UI 项用
> `plugin.<pluginId>.*`。插件框架会拒绝未知钩子与未知前缀。
>
> 钩子怎么写（两种形态的注册写法、优先级、裁决对象、异步裁决、处理器里不该做的事）与未接入的钩子打算做什么，见 [plugin-api.md](plugin-api.md) §8。

## 阶段：一个钩子 = 前链 + 实现链 + 后链

**钩子 id 表示一次调用；参与方式（before / impl / after）由注册时的 `phase` 表达。**
不再用 `.before` / `.after` 后缀造新 id（那样一条调用会变成好几条钩子）。

| 阶段 | 何时跑 | 能做什么 | 不能做什么 | 回答格式 | 结束整条链 |
|---|---|---|---|---|---|
| `before` | 进入调用之前，串行、**正序** | 改输入（累加）、停下（这次不执行实现） | 给数据（那是实现链的事） | `{"patch": {...}}` | `{"action":"stop"}` |
| `impl` | 实现链，串行 | 给结果 / 裁决 | 改输入 / 改输出 | 与以前一样 | 按钩子声明的合并策略 |
| `after` | 拿到结果之后，串行、**逆序** | 改输出（累加）、丢弃（这次没有结果） | 造出本来没有的结果 | `{"value": <新结果>}` | `{"action":"drop"}` |

**顺序**：一条钩子按 `(优先级升序, 注册顺序)` 排成一条链。

```text
线头（离调用方最近）                              线尾（离实现最近）
extern  ---------->  builtin  ---------->  core
   ^                                          |
   +---------------  后链（逆序） <------------+
```

- **前链正序**：线头 → 线尾，每一步拿到**上一步改过**的输入；
- **后链逆序**：线尾 → 线头，每一步拿到**上一步改过**的输出。所以外置插件**最后**改输出，应用自己的兜底实现不能盖掉插件的改写（与实现链里“外置优先”同向）；
- 同一插件对同一个钩子可以分别注册三个阶段（同一个 `ownerTag` 互不覆盖）；`unregister` 按 `(hook, ownerTag)` 摘，一个 `ownerTag` 在所有阶段的登记一起摘掉；
- **阶段处理器必须是同步处理器**（要拿到这一步的结果才能跑下一步），注册异步处理器会被拒绝；
- 三种“没有结果”含义不同：实现链的 `{"disable": true}`（不再问别人）、前链的 `stop`（这次不执行实现）、后链的 `drop`（实现跑完但结果丢掉）。

没有声明的阶段登记会被拒绝（日志写清“这个钩子没有 before 阶段”）。

## 怎么读这张表

- **主程序是否已接入**：`已接入` = 主程序已经在调用点接上这个钩子（当前版本会派发，插件注册后会被调用）；`未接入` = 钩子约定已经定下来、主程序**还没有接**（注册不会报错，但当前版本不会触发）。想知道某个钩子此刻有没有处理器，看管理页「外部插件 → 调试」分页的钩子统计（`hooks` 段），或在插件里调 `musicxx.hooks.has(id)`（只反映自己注册没注册）。
- **模式**：`observe` = 只通知（返回值忽略、不等待）；`decision` = 可裁决，返回 `null` 表示这次不表态。
- **派发**：`sync` = 调用点就地等待（占用调用线程）；`async` = 不占用调用线程，裁决结果经 `musicxx.hook.decision.result` 事件回传（调用点用 `decideAsync` 时）。
- **合并策略**：多个处理器给出裁决时怎么合并（`firstNonNull` 首个非空生效并停止询问、`anyCancel` 任一 cancel/skip 即生效、`allMerge` 全部合并、`lastWrite` 最后一个生效）。
- **阶段**：这个钩子支持哪几个阶段（`impl` = 实现链；`before` = 前链，正序改输入；`after` = 后链，逆序改输出）。只列**真的会派发**的阶段 —— 注册了不派发的阶段等于“注册成功但永远不触发”。
- **耗时**：插件框架不设置等待超时（不因为处理器慢而中断派发、也不按耗时暂停某个处理器），处理器耗时只记进统计（管理页调试分页可看）。处理器请自己保持短小：它跑在插件框架线程上，慢就是别人一起等。
- **怎么写**：注册与注销、优先级与执行顺序、裁决对象外壳、异步裁决、自省，以及“处理器里不要等主程序动作”这类坑，见 `docs/plugin-api.md` §8。

| 钩子 id | 模式 | 阶段 | 派发 | 合并策略 | 主程序是否已接入 |
|---|---|---|---|---|---|
| `musicxx.app.start` | observe | impl | async | lastWrite | 已接入 |
| `musicxx.app.ready` | observe | impl | async | lastWrite | 已接入 |
| `musicxx.app.background` | observe | impl | async | lastWrite | 已接入 |
| `musicxx.app.foreground` | observe | impl | async | lastWrite | 已接入 |
| `musicxx.app.exit` | decision | impl | sync | anyCancel | 未接入 |
| `musicxx.app.deepLink` | decision | impl | sync | anyCancel | 未接入 |
| `musicxx.app.upgrade` | observe | impl | async | lastWrite | 未接入 |
| `musicxx.player.beforePlaySong` | decision | impl+after | async | anyCancel | 已接入 |
| `musicxx.player.source.beforeParse` | decision | impl+after | async | firstNonNull | 已接入 |
| `musicxx.player.source.resolved` | observe | impl | async | lastWrite | 已接入 |
| `musicxx.player.source.resolve` | decision | impl | async | firstNonNull | 已接入 |
| `musicxx.player.state` | observe | impl | async | lastWrite | 已接入 |
| `musicxx.player.position` | observe | impl | async | lastWrite | 已接入 |
| `musicxx.player.seek` | decision | impl+after | async | anyCancel | 已接入 |
| `musicxx.player.volume` | decision | impl+after | async | anyCancel | 已接入 |
| `musicxx.player.speed` | decision | impl+after | async | anyCancel | 已接入 |
| `musicxx.player.pitch` | decision | impl+after | async | anyCancel | 已接入 |
| `musicxx.player.error` | decision | impl+after | async | anyCancel | 已接入 |
| `musicxx.player.completed` | observe | impl | async | lastWrite | 已接入 |
| `musicxx.player.qualityChanged` | observe | impl | async | lastWrite | 未接入 |
| `musicxx.media.notification` | decision | impl | sync | lastWrite | 未接入 |
| `musicxx.song.changed` | observe | impl | async | lastWrite | 已接入 |
| `musicxx.song.info.analyse` | decision | impl | sync | firstNonNull | 未接入 |
| `musicxx.song.meta.writeback` | observe | impl | async | lastWrite | 未接入 |
| `musicxx.song.beforeAdd` | decision | impl | sync | anyCancel | 未接入 |
| `musicxx.song.removed` | observe | impl | async | lastWrite | 未接入 |
| `musicxx.playlist.load` | decision | before+after | async | allMerge | 未接入 |
| `musicxx.songlist.filter` | decision | impl | async | allMerge | 未接入 |
| `musicxx.history.record` | observe | impl | async | lastWrite | 未接入 |
| `musicxx.local.scan.file` | decision | impl | sync | anyCancel | 未接入 |
| `musicxx.local.scan.finished` | observe | impl | async | lastWrite | 未接入 |
| `musicxx.lyric.current` | observe | impl | async | lastWrite | 未接入 |
| `musicxx.lyric.load` | decision | before+impl+after | async | firstNonNull | 已接入 |
| `musicxx.lyric.load.musicxx_server` | decision | before+impl+after | async | firstNonNull | 已接入 |
| `musicxx.lyric.load.cache` | decision | before+impl+after | async | firstNonNull | 已接入 |
| `musicxx.lyric.load.builtin` | decision | before+impl+after | async | firstNonNull | 已接入 |
| `musicxx.lyric.load.builtout` | decision | before+impl+after | async | firstNonNull | 已接入 |
| `musicxx.lyric.load.match` | decision | before+impl+after | async | firstNonNull | 已接入 |
| `musicxx.icon.fetch` | decision | before+impl+after | async | firstNonNull | 未接入 |
| `musicxx.media.info.request` | decision | impl | async | firstNonNull | 未接入 |
| `musicxx.media.spectrum.extract` | decision | impl | async | firstNonNull | 已接入 |
| `musicxx.media.chorus.analyse` | decision | impl | async | firstNonNull | 已接入 |
| `musicxx.media.palette.provide` | decision | impl | async | firstNonNull | 已接入 |
| `musicxx.media.cover.changed` | observe | impl | async | lastWrite | 已接入 |
| `musicxx.net.request` | decision | before+after | async | anyCancel | 未接入 |
| `musicxx.net.server.route` | decision | impl | async | firstNonNull | 未接入 |
| `musicxx.net.mcp.tools` | decision | impl | sync | allMerge | 未接入 |
| `musicxx.net.lan.event` | observe | impl | async | lastWrite | 未接入 |
| `musicxx.download` | decision | before+after | async | anyCancel | 未接入 |
| `musicxx.cache.beforeTrim` | decision | impl | sync | anyCancel | 未接入 |
| `musicxx.cache.pathRequest` | decision | impl | sync | firstNonNull | 未接入 |
| `musicxx.ui.home.entries` | decision | impl | sync | allMerge | 未接入 |
| `musicxx.ui.song.actions` | decision | impl | sync | allMerge | 未接入 |
| `musicxx.ui.playlist.actions` | decision | impl | sync | allMerge | 未接入 |
| `musicxx.ui.route.resolve` | decision | impl | sync | firstNonNull | 未接入 |
| `musicxx.ui.page.enter` | observe | impl | async | lastWrite | 未接入 |
| `musicxx.ui.page.leave` | observe | impl | async | lastWrite | 未接入 |
| `musicxx.ui.theme.changed` | observe | impl | async | lastWrite | 未接入 |
| `musicxx.ui.notify` | decision | impl | sync | anyCancel | 未接入 |
| `musicxx.ui.user.action` | observe | impl | async | lastWrite | 未接入 |
| `musicxx.script.bound` | observe | impl | async | lastWrite | 未接入 |
| `musicxx.script.disposed` | observe | impl | async | lastWrite | 未接入 |
| `musicxx.script.action.executed` | observe | impl | async | lastWrite | 未接入 |
| `musicxx.clocking.tick` | observe | impl | async | lastWrite | 未接入 |
| `musicxx.listenTogether.event` | observe | impl | async | lastWrite | 未接入 |
| `musicxx.feature.point.call` | decision | before+impl+after | async | firstNonNull | 已接入 |

## 已接入钩子的载荷与裁决

下面是主程序**已经接入**的钩子：处理器拿到的载荷字段与裁决语义都在这里。尚未接入的钩子只定下了 id / 模式 / 派发 / 合并策略，载荷字段在主程序接入时补齐（接入后会写进 `tools/hooks.def.json` 的 `doc` 字段并重新生成本文件）。

> 载荷统一是 JSON 对象；不裁决时返回 `null`（JS）或把出参留空（C++）。
> 载荷里不放音频直链与 token：需要地址时请用 `musicxx.net` / 主程序动作自行获取。

### `musicxx.app.start`

- 模式：`observe`；阶段：`impl`；派发：`async`；合并策略：`lastWrite`
- 载荷 `{version, platform, branch, installId, lang}`。主程序启动阶段（业务 Store 与插件框架都就绪之后）派发一次：适合做初始化自己的状态、登记定时器这类事，观察型没有裁决。

### `musicxx.app.ready`

- 模式：`observe`；阶段：`impl`；派发：`async`；合并策略：`lastWrite`
- 载荷 `{firstRun, restoredSong}`：`firstRun` = 是否首次启动，`restoredSong` = 恢复播放的歌曲（可能缺省）。首屏与数据都就绪后派发一次（比 `app.start` 晚），观察型。

### `musicxx.app.background`

- 模式：`observe`；阶段：`impl`；派发：`async`；合并策略：`lastWrite`
- 载荷 `{state}`：`state` = 触发这次通知的生命周期状态名（`paused` / `hidden` / `inactive` / `detached`）。应用**真的**进后台时派发一次（桌面端隐藏 / 最小化窗口，移动端切到后台）；窗口只是失去焦点（`inactive` 但画面仍可见）不算后台。派发前状态镜像已刷新（`musicxx.state.app.foreground` 为 false），处理器里同步读到的就是新状态；同一状态重复上报不会重复派发。观察型、异步：不等待处理器，不影响播放与缓存。要记“在后台待了多久”用 `musicxx.app.foreground` 的 `backgroundMs`；要读当前值用变量 `musicxx.app.lifecycle`（可订阅）或状态镜像。

### `musicxx.app.foreground`

- 模式：`observe`；阶段：`impl`；派发：`async`；合并策略：`lastWrite`
- 载荷 `{state, backgroundMs}`：`state` = 回到前台时的生命周期状态名（一般是 `resumed`），`backgroundMs` = 这次在后台待了多久（毫秒）。应用回到前台时派发一次；**只在前台与后台真的切换时**派发，所以启动阶段的初次 `resumed` 不会触发它（启动用 `musicxx.app.start` / `musicxx.app.ready`）。观察型、异步：不等待处理器；插件适合在这里做“回前台补一次刷新”（检查时间是否跨越太久、重新拉一次数据）。

### `musicxx.player.beforePlaySong`

- 模式：`decision`；阶段：`impl+after`；派发：`async`；合并策略：`anyCancel`
- 载荷 `{sid, song, mode, quality}`：`song` 是只读视图（`sid/name/artist/album/durationMs/year/genre/srcKey/audio[]/video[]`，不含音频直链），`mode` = 播放模式、`quality` = 当前音质。裁决只实现 `{"action":"skip"}` = 跳过本曲（走应用既有的切下一曲流程）；`patch`（换音源/改音质）当前版本未实现，会被记一条日志后忽略。异步派发：切歌不会被插件拖慢；等待期间若又来了新的切歌请求，本次裁决被丢弃（插件可以自己用 `sid` 判断）。

### `musicxx.player.source.beforeParse`

- 模式：`decision`；阶段：`impl+after`；派发：`async`；合并策略：`firstNonNull`
- 载荷 `{sid, srcKey, src, index, tryLocalOrCache}`：`src` 是音源视图（`type/empty/srcKey/quality/durationMs/hashMd5/size`），`index` = 本轮解析的音源下标。裁决：`{"action":"skip"}` = 本轮不解析该音源（调用方接着试下一个）；`patch.src = {"type":"Local|UrlLink|Bili|...","src":"<新地址>","info":{...}}` = 只替换本轮使用的音源（不改动歌曲实体的音源列表），字段不合法会被忽略。异步派发（不卡住解析），等待期间切歌则本次裁决作废。

### `musicxx.player.source.resolved`

- 模式：`observe`；阶段：`impl`；派发：`async`；合并策略：`lastWrite`
- 载荷 `{sid, srcKey, useSrcKey, parsedType, pathKind, hasHeaders, cacheStream}`：某个音源解析完成（拿到真正播放用的路径与请求头）之后派发。载荷里没有直链与 token，只报来源类型与路径种类（本地 / 缓存 / 远程流 / 带请求头）；观察型。

### `musicxx.player.source.resolve`

- 模式：`decision`；阶段：`impl`；派发：`async`；合并策略：`firstNonNull`
- 载荷 `{srcKey, type, src, crudeType, crudeSrc, fileName, durationMs, quality, isMediaSrc, pluginType, caller}`：主程序要把一个音源解析成**播放器内核能打开的地址与请求头**时派发一次（外置 → 内置插件 → 应用自身，第一个给出合法结果的生效）。`src` = 本轮要解析的音源（插件自己的音源就是它写下的资源 id / 路径；内置来源是已经解析出来的临时直链或本地路径 —— 注册这条钩子的插件看得到这些内容，这是它能换地址的前提）；`crudeSrc` = 歌曲实体里保存的原始地址；`type` / `crudeType` = 对应的来源类型（`link` / `bili` / `aliDrive` / `local` 等；插件音源是空串）；`pluginType` = 本轮音源的插件音源类型 id（`plugin.<插件 id>.<类型名>`；不是插件音源则为空串）；`quality` = 用户当前音质偏好（枚举名小写）；`isMediaSrc` = false 表示这次要的是封面（媒体库来源）；`caller` = 谁在问（空串 = 主程序自己或命令行）。处理器返回 `{"src": {"path": "<地址>", "headers": {"Referer": "..."}, "kind": "url", "expireAtMs": 0, "size": 0, "durationMs": 0, "quality": "hq"}}`（`src` 也可以直接给一个地址字符串）：这就是播放器要打开的地址与请求头。`path` 支持网络地址、本地文件路径、`content://`、`asset://`；`kind` 取 `url` / `file` / `content` / `asset`，缺省按地址自己判断（判断错会让本地文件被当成网络流去做缓存）；`headers` 可省；`expireAtMs` 给临时地址的过期时间戳（毫秒，只用于排障）。**回答请用 `patch` 包一层**（写成 `patch.src`）：插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历。给自己的音源给出地址与请求头、给内置来源换地址（加代理、换 CDN、统一请求头）都是这条路。不返回 / 结果不合法 = 这次不提供，主程序接着问内置实现。返回 `{"disable": true}` 是显式置空：这次不再问别的实现，该音源会解析失败（只在明确知道它不该播时用）。异步派发：主程序最多等 5 s（超时按没给处理，改走内置实现），处理器请尽快返回。主程序只在"该钩子有处理器"时才派发。

### `musicxx.player.state`

- 模式：`observe`；阶段：`impl`；派发：`async`；合并策略：`lastWrite`
- 载荷 `{state, sid, positionMs}`。`state` 是播放状态文本：播放 / 暂停 / 停止这三个事件给 `play`、`pause`、`stop`，其它状态变化给枚举名 `Play` / `Pause` / `Stop` / `Completed`；要判断当前是否在播放，请读状态镜像 `musicxx.state.env.isPlaying`（布尔）。观察型，只在播放状态变化时派发（播放进度不走这个钩子）。

### `musicxx.player.position`

- 模式：`observe`；阶段：`impl`；派发：`async`；合并策略：`lastWrite`
- 载荷 `{sid, positionMs, durationMs}`：播放进度，**按 1 秒节流**推送（应用本来就有 1 Hz 的进度节流，直接复用它；暂停与缓冲时不推进）。`positionMs` = 当前播放位置（毫秒），`durationMs` = 总时长（还不知道时为 0），`sid` = 当前歌曲会话 id（切歌后以新 `sid` 为准）。观察型、异步：不等待处理器，不影响播放。它只报进度、不报状态：要判断“是否在播放”读状态镜像 `musicxx.state.env.isPlaying`，要“状态变了”用钩子 `musicxx.player.state`；要读当前值用变量 `musicxx.player.position`（镜像 `musicxx.state.player.position` 仍只在启动、切歌与播放状态变化时刷新）。

### `musicxx.player.seek`

- 模式：`decision`；阶段：`impl+after`；派发：`async`；合并策略：`anyCancel`
- 载荷 `{sid, from, to, durationMs}`：应用**要把播放位置改到 `to`（毫秒）之前**派发一次。拖动进度条、点歌词、快捷键快进快退、局域网控制、命令行 / 动作 / 脚本、恢复播放进度都会走这里。`from` = 改之前的进度，`durationMs` = 总时长（未知为 0），`sid` = 当前歌曲会话 id。裁决：`{"action":"continue","patch":{"to":60000}}` = 换成别的目标位置；`{"action":"cancel"}`（或 `skip`）= 这次不跳（保持原位）。超出范围的目标会被应用夹到 0~总时长。**异步派发、最多等 0.5 s**：超时或不回答 = 按原目标跳；等待期间切歌则本次裁决作废（插件不用自己判断 `sid`）。处理器里不要再调 `musicxx.player.seek`（会再派发一次）。

### `musicxx.player.volume`

- 模式：`decision`；阶段：`impl+after`；派发：`async`；合并策略：`anyCancel`
- 载荷 `{from, to}`：应用**要把音量改成 `to` 之前**派发一次（`from` = 当前音量，`to` = 这次要用的音量，都是 0~1，可能略大于 1 表示增益）。音量滑块、快捷键、静音 / 取消静音、局域网控制、命令行 / 动作、启动时恢复设置都会走这里。裁决 `{"patch":{"to":0.5}}` = 换成别的音量；`{"action":"cancel"}` = 这次不改（保持原音量）。最终值会被夹到应用的合法范围（0~1）。**异步派发、最多等 0.5 s**：超时或不回答 = 按原值执行。处理器里不要再调 `musicxx.player.setVolume`（会再派发一次）。

### `musicxx.player.speed`

- 模式：`decision`；阶段：`impl+after`；派发：`async`；合并策略：`anyCancel`
- 载荷 `{from, to}`：应用**要把播放速度改成 `to` 之前**派发一次（`from` = 当前速度，`to` = 这次要用的速度，单位是倍速）。速度菜单、长按加速 / 减速、命令行 / 动作、定时停止的变速、启动时恢复设置都会走这里。裁决 `{"patch":{"to":1.5}}` = 换成别的速度；`{"action":"cancel"}` = 这次不改。最终值会被夹到应用的合法范围（0.25~8）。**异步派发、最多等 0.5 s**。长按加 / 减速这类连续变化会连派多次，处理器要保持快，处理器里也不要再调 `musicxx.player.setSpeed`。

### `musicxx.player.pitch`

- 模式：`decision`；阶段：`impl+after`；派发：`async`；合并策略：`anyCancel`
- 载荷 `{from, to}`：应用**要把音调改成 `to` 之前**派发一次（`from` = 当前音调，`to` = 这次要用的音调；1 是原调，范围 0.25~2）。播放页的音调菜单、快捷键、音效切换时的复位、启动时恢复设置都会走这里。裁决 `{"patch":{"to":1.1}}` = 换成别的音调；`{"action":"cancel"}` = 这次不改。最终值会被夹到应用的合法范围（0.25~2）。**异步派发、最多等 0.5 s**。要主动改音调用动作 `musicxx.player.setPitch`；处理器里不要再调它（会再派发一次）。

### `musicxx.player.error`

- 模式：`decision`；阶段：`impl+after`；派发：`async`；合并策略：`anyCancel`
- 载荷 `{sid, srcKey, isNowUseSrc, errorCount, durationMs, positionMs}`（`errorCount` 已包含本次失败）。裁决：`{"action":"stop"}` = 停止播放；`{"action":"skip"}`（或 `cancel`）= 跳到下一曲；`{"action":"continue","patch":{"tryNextSrc":false}}` = 不再尝试当前源（跳过忽略错误并重试的分支，直接按主程序的换源/下一曲策略走）。异步派发（不占用调用线程，主程序不会被插件拖住）：等待期间切歌则本次裁决作废；处理器请尽快给出裁决，本次失败处理会等它结算。主程序只在"该钩子有处理器"时才派发。

### `musicxx.player.completed`

- 模式：`observe`；阶段：`impl`；派发：`async`；合并策略：`lastWrite`
- 载荷 `{sid, playedMs}`：一曲播放完成（正常结束或按完成处理）之后派发；观察型。它只表示这首歌放完了，不代表已经切歌（切歌另有 `musicxx.song.changed`）。

### `musicxx.song.changed`

- 模式：`observe`；阶段：`impl`；派发：`async`；合并策略：`lastWrite`
- 载荷 `{sid, prevSid, song}`：`song` 与 `beforePlaySong` 的同一个只读视图，`prevSid` = 上一首的 sid（没有则为 null）。切歌时派发；派发前插件框架会先刷新状态镜像，所以处理器里同步读 `musicxx.state.song` 拿到的就是新歌。观察型。

### `musicxx.playlist.load`

- 模式：`decision`；阶段：`before+after`；派发：`async`；合并策略：`allMerge`
- 包装点（实现固定是应用自己：读取由主程序做）：主程序读完一个歌单时派发。`before` 前链（串行、正序）改这次读取的参数（例如要读哪一份）；`after` 后链（串行、**逆序**）能读到最终列表并过滤它 —— 这就是原来的 `musicxx.playlist.filter`，只是现在和“加载完成”是同一个调用的两端，不必再记两条钩子。原来的 `musicxx.playlist.loaded` / `musicxx.playlist.filter` 是它的两个端点。载荷字段在主程序接入时补齐：当前版本注册不会报错，也不会触发。

### `musicxx.lyric.load`

- 模式：`decision`；阶段：`before+impl+after`；派发：`async`；合并策略：`firstNonNull`
- 载荷 `{sid, song, audio, parsed, path, enable, keyword, artist, lrcid, position, caller}`：主程序要为**一首歌加载歌词**时派发一次（外置 → 内置插件 → 应用自身，第一个给出合法歌词的生效）。`song` 是歌曲只读视图（`sid/name/artist/album/durationMs/srcKey/audio[]/video[]`，不含音频直链）；`audio` = 本轮音源视图（`type/srcKey/quality/durationMs/hashMd5/size`）；`parsed` = 实际播放用的那份音源（缓存文件 / 临时直链，字段同上）；`path` 只有本地、缓存、`content://`、asset 才有值（网络来源是空串 —— 要直链请自己走 `musicxx.net`）；`enable` = 这一轮允许做的事 `{musicxx_server（读拟声云歌词）, cache（读应用的歌词缓存）, builtin（读内置歌词：播放器元数据与媒体文件标签）, builtout（读同名外置歌词）, match（自动匹配）, upload（可自动上传）}`；`keyword` / `artist` = 自动匹配用的搜索串（前链可以改写）；`lrcid` > 0 = 强制用这一条云歌词；`position` 非空 = 强制读这个位置（文件路径，或歌词来源自己的标识）；`caller` = 谁在问（空串 = 主程序自己或命令行）。处理器回答 `{"patch": {"lyric": {"content": "[00:12.34]…", "lrcid": 0, "sourceType": "builtin", "offsetMs": 0}}}`：`content` 是 LRC 文本（主程序自己解析，不认别的格式），`sourceType` 取 `builtin` / `builtout` / `local` / `musicxx_server` / `match`（另有 `builtinCache` / `builtoutCache`；省掉按 `musicxx_server` 算；写 `builtin` / `builtout` 的歌词会像应用自己读到的那样进歌词缓存，并按 `enable.upload` 决定是否自动上传），`offsetMs` = 这首歌的歌词偏移（毫秒，可省）。`content` 为空、解析不出歌词行 = 这次不提供，主程序接着问下一个实现。`{"patch": {"lyric": {"disabled": true}}}` = 这首歌**禁用歌词**（这是合法结果：界面显示『禁用歌词』，后续读取全部不做）。`{"disable": true}` 是框架级置空：这次不再问别的实现，也不写缓存。后链回答 `{"value": {"lyric": {…}}}` 换掉这次的结果，`{"action": "drop"}` = 这次按没有歌词处理。**回答请用 `patch` 包一层**：插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历。前链能改写 `keyword` / `artist` / `lrcid` / `position` 与 `enable` 里的各项（回答 `{"patch": {…}}`），`{"action": "stop"}` = 这次不加载歌词。异步派发：主程序最多等 8 s（超时按没给处理，接着走应用自己的加载）。应用自己的实现会把这一轮拆成五个细分埋点按顺序询问（`musicxx.lyric.load.musicxx_server` → `.cache` → `.builtin` → `.builtout` → `.match`）；**插件在这里接管之后，细分埋点不会再被询问**。

### `musicxx.lyric.load.musicxx_server`

- 模式：`decision`；阶段：`before+impl+after`；派发：`async`；合并策略：`firstNonNull`
- 载荷与回答形状同 `musicxx.lyric.load`。这一阶段读**拟声云歌词**（自建服务 / 歌曲绑定的那个歌词来源）：用户给这首歌指定过的歌词（按音源身份记在应用里的那条），或歌曲自带的歌词来源（`myId` / 链接 / 云盘 / 媒体库 / 本地文件）。应用自己的实现在这里调自建服务的歌词接口、读绑定的本地歌词文件，命中时 `sourceType` 是 `musicxx_server` 或 `local`。`enable.musicxx_server` 为 false 时主程序不会问这个点（`enableCloudLyric: false` 的调用点是『只从文件读歌词』的场景）。回答 `disabled` 表示这首歌禁用歌词，后续阶段不再执行。

### `musicxx.lyric.load.cache`

- 模式：`decision`；阶段：`before+impl+after`；派发：`async`；合并策略：`firstNonNull`
- 载荷与回答形状同 `musicxx.lyric.load`。这一阶段读**歌词缓存**：应用自己保存过的歌词内容（按音源身份、md5、sha1、大小与时长命中）。`enable.cache` 为 false 时不会被询问；命中时 `sourceType` 是 `builtinCache` / `builtoutCache`（这份缓存是上一次读到内置 / 外置歌词时写下的）。插件接管后可以换成自己的歌词库（例如本地收藏的歌词文件索引）。

### `musicxx.lyric.load.builtin`

- 模式：`decision`；阶段：`before+impl+after`；派发：`async`；合并策略：`firstNonNull`
- 载荷与回答形状同 `musicxx.lyric.load`。这一阶段读**内置歌词**，两条路合在一起：播放器当前音源**已经读到的元数据标签**（切歌后它比重新解码文件先到，应用最多等 300 ms），以及由媒体解码器**从音频文件 / 缓存流里读出的标签歌词**（`path` 就是那个文件；原来叫 `load_lyric_builtIn`）。会在四种情况下被询问：前链指定了 `position`（只读那个文件）、这一轮允许读缓存且来源类型允许（看播放器元数据）、解析出来的音源是缓存文件 / 链接（不管 `enable.builtin`，原来就是先读一遍它的标签）、或 `enable.builtin` 为 true 且来源类型允许。应用自己的实现还会按来源类型跳过明显没有标签的那些（Bili、asset、缓存 id 等），插件替换实现后不再受这条限制。`sourceType` 缺省 `builtin`（会进歌词缓存），命中后主程序还可能自动上传到自建服务（看 `enable.upload`）。

### `musicxx.lyric.load.builtout`

- 模式：`decision`；阶段：`before+impl+after`；派发：`async`；合并策略：`firstNonNull`
- 载荷与回答形状同 `musicxx.lyric.load`。这一阶段读**外置歌词**：与音频文件同名的 `.lrc`（本地、链接、WebDAV），以及云盘与媒体库各自的同名歌词查找（阿里云盘、百度网盘、115、Subsonic / Jellyfin / Emby 的歌词接口；最后两种的 `path` 是空串，标识在歌曲视图里）。`enable.builtout` 为 true 且来源类型允许、或前链指定了 `position` 时会被询问；应用自己的实现会跳过明显没有同名文件的来源（Bili、`myId`、`myLink`、缓存、asset 等），插件替换实现后不再受这条限制。`sourceType` 缺省 `builtout`（会进歌词缓存）。

### `musicxx.lyric.load.match`

- 模式：`decision`；阶段：`before+impl+after`；派发：`async`；合并策略：`firstNonNull`
- 载荷与回答形状同 `musicxx.lyric.load`。这一阶段做**自动匹配**：前面几个阶段都没拿到歌词时，按歌名 / 歌手 / hash / 大小 / 时长向自建服务要推荐歌词。`enable.match` 为 true 时会被询问（调用方决定；设置里的「自动匹配歌词」开关在主程序那边就并进了这个值）。应用自己的实现还会跳过 15 分钟以上、歌名为空或名字里有『纯音乐 / 伴奏 / instrumental』的歌（插件替换后不再受这条限制）。供参考：`keyword` / `artist` 可以在前链里改写（例如换成自己的搜索串）。`sourceType` 缺省 `match`。插件接管后可以换成自己的匹配方式（歌词站搜索、按音频指纹匹配等）；匹配不到就返回空，主程序按『这首歌没有歌词』处理。

### `musicxx.icon.fetch`

- 模式：`decision`；阶段：`before+impl+after`；派发：`async`；合并策略：`firstNonNull`
- 取值点（插件可以替代实现）：主程序要为一张封面 / 图标取图时派发。`before` 前链（串行、正序）改这次请求的尺寸 / 来源；`impl` 实现链给图（第一个给出合法结果的生效）；`after` 后链（串行、**逆序**）改最终那张图。原来的 `musicxx.icon.request` / `musicxx.icon.generated` 是它的两个端点。载荷字段在主程序接入时补齐：当前版本注册不会报错，也不会触发。

### `musicxx.media.spectrum.extract`

- 模式：`decision`；阶段：`impl`；派发：`async`；合并策略：`firstNonNull`
- 载荷 `{srcKey, name, artist, kind, path, durationMs, frames, binCount, frameHz, maxSeconds, caller}`（`caller` = 谁在问：空串 = 主程序自己取值或命令行，非空 = 主动调用它的插件 id）：主程序要提取一首歌的**整曲频谱**时派发一次（`path` 是本地/缓存文件路径，不是网络直链；`kind` = `local` / `cache` / `content` / `other`；`frames` 按时长推算、只作提示）。处理器返回 `{"spectrum": {"spectrums": "<base64>", "waveform": "<base64，可省>", "binCount": 256, "frameHz": 10}}` 就用插件的数据替代内置提取（内置实现是主程序自己用 mediaxx 提取）：`spectrums` 是整曲频点（帧数 × 256 个字节，0~255 表示 -80~0 dB，默认每秒 10 帧），`waveform` 是每帧 1 字节 RMS 响度（0~255 表示 -60~+20 dB；省掉或长度对不上时按每帧频点均值补一份近似响度）。不返回 / 没有 `spectrum` / 数据不合法（不是 base64、长度不是 256 的整数倍、`binCount` 与 `frameHz` 不是 256 与 10）= 这次不提供，主程序接着用已登记的实现（内置『音乐动效』）。**回答请用 `patch` 包一层**（写成 `patch.spectrum`）：插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历，多个实现时裸字段会被后面的处理器覆盖（只有一个实现时两种写法等价）。异步派发：主程序最多等 30 s（超时按没给处理）；等待期间切歌则本次结果会被丢弃（可用 `srcKey` 自行校验）。主程序只在"该钩子有处理器"时才派发。返回 `{"disable": true}` 是显式的**置空裁决**：主程序这次不再询问其它实现（内置『音乐动效』也不跑，播放页频谱与副歌分析都拿不到数据），这次结果不写缓存；置空只在本次询问有效，处理器被摘除（插件停用 / 卸载 / 关闭『拟声++』）后自动还原。要用它请先确认自己的数据已经算好（例如自己起线程提前算完），否则用户会看到功能凭空消失。**这个功能点对外开放调用**（动作 `musicxx.feature.point.call`）：外置插件、命令行与 agent 能主动调它并拿到原始数据，参数与结果见 `featurePoint.list` 里的 `callDoc`。调用同样按这条处理器链问一遍，但**不写主程序的值仓、不改主程序状态**（只读值仓，绝不记录置空标记），同一个功能点 + 同一个调用方的上一次调用没结束时拿到 `busy`。

### `musicxx.media.chorus.analyse`

- 模式：`decision`；阶段：`impl`；派发：`async`；合并策略：`firstNonNull`
- 载荷 `{srcKey, name, artist, kind, path, durationMs, hasWave, frames, binCount, frameHz, spectrums?, waveform?, config, caller}`（`caller` = 谁在问：空串 = 主程序自己取值或命令行，非空 = 主动调用它的插件 id）：主程序要分析一首歌的**副歌（高潮）区段**时派发一次。`hasWave` 为真时附带主程序已经拿到的整曲频谱（`spectrums` / `waveform` 的 base64，4 分钟歌曲约 800 KB —— 用不上的插件请不要注册这个钩子）；`config` 是内置算法的参数（`bandCount` / `hopFrames` / `windowFrames` / `minRepeatLagSeconds` / `segmentSimilarity` / `minSegmentSeconds` / `maxSegmentSeconds` / `mergeGapSeconds`）。处理器返回 `{"chorus": {"sections": [{"start": 64000, "end": 100000, "confidence": 0.8, "energy": 0.7, "similarity": 0.9, "repeat": 3}], "totalDuration": 253000, "analyseTimeMs": 120}}`（时间单位毫秒）就用插件的区段替代内置分析；`sections` 是必需项（空数组 = 分析过了、这首歌没有副歌），每段至少要有 `start` 与 `end`，其余字段缺失按 0 处理。不返回 / 缺少 `sections` / 一个区段都解析不出来 = 这次不提供。**回答请用 `patch` 包一层**（写成 `patch.chorus`）：插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历，多个实现时裸字段会被后面的处理器覆盖（只有一个实现时两种写法等价）。异步派发：主程序最多等 20 s；等待期间切歌则本次结果会被丢弃（可用 `srcKey` 自行校验）。主程序只在"该钩子有处理器"时才派发。返回 `{"disable": true}` 可以显式置空这个功能：主程序这次不再走内置分析（进度条上的副歌标记就没有了），处理器被摘除后自动还原。**这个功能点对外开放调用**（动作 `musicxx.feature.point.call`）：外置插件、命令行与 agent 能主动调它（参数与结果见 `featurePoint.list` 里的 `callDoc`）；调用会先取一次频谱，**这一层同样不写值仓**，也不改主程序状态，同一个功能点 + 同一个调用方的上一次调用没结束时拿到 `busy`。

### `musicxx.media.palette.provide`

- 模式：`decision`；阶段：`impl`；派发：`async`；合并策略：`firstNonNull`
- 载荷 `{srcKey, name, artist, hasCover, night, caller, cover}`：主程序要为一张封面取色（内置实现是封面颜色分析）时派发一次。`caller` = 谁在问（空串 = 主程序自己取值或命令行，非空 = 主动调用它的插件 id）；`cover` = 这次算哪一张图：`{kind: current | file | url | data, path?, url?, bytes?, imageBase64?, coverKey}`（`current` = 当前歌曲的封面，`file` / `url` / `data` 是调用方指定的那一张；`path` / `url` 是对应的地址，`data` 给 `bytes` 与 `imageBase64` —— 图太大时只有 `bytes`；`coverKey` 是这次的身份，插件可以按它自己缓存）。老插件不认 `cover` 也能跑：它们只看 `srcKey` / `name` / `artist` / `night`，对 `current` 这一种用法的结果是对的；别的来源它们给不出（返回空），主程序接着走自己的分析 —— 不会给错数据。处理器返回 `{"colors": {"main": "#rrggbb", ...}}`（键可取 `main` / `light` / `lightMuted` / `dark` / `darkMuted`，写法 `#rrggbb` 或 `#aarrggbb`）就用插件提供的这套颜色替代内置分析：它写进与内置分析同一批字段（**只在主程序自己问当前封面时**；外置通过 `musicxx.feature.point.call` 调它时只拿数据、不写字段），内置背景、插件背景与 `musicxx.icon.*` 来源都会拿到；不返回 / 没有 `colors` / 颜色一个都解析不出来 = 这次不提供，主程序照旧走内置分析。**回答请用 `patch` 包一层**（写成 `patch.colors`）：插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历，多个实现时裸字段会被后面的处理器覆盖（只有一个实现时两种写法等价）。异步派发：不会卡住渲染准备；等待期间切歌时本次结果会被丢弃（可用 `srcKey` 自行校验）。返回 `{"disable": true}` 可以显式关掉封面取色：主程序这次不再走内置取色（保持上一次的颜色），处理器被摘除后自动还原。**这个功能点对外开放调用**（动作 `musicxx.feature.point.call`）：外置插件、命令行与 agent 能主动调它并拿到 5 个原始色槽位，参数与结果见 `featurePoint.list` 里的 `callDoc`。**调用没有副作用**：不写封面取色字段、不改内置背景（要改观感请用 `musicxx.media.palette.set`），同一个功能点 + 同一个调用方的上一次调用没结束时拿到 `busy`。

### `musicxx.media.cover.changed`

- 模式：`observe`；阶段：`impl`；派发：`async`；合并策略：`lastWrite`
- 载荷 `{srcKey, name, artist, hasCover, kind}`（`kind` = `local` / `cache` / `content` / `asset` / `network`，没有封面时为空串）：当前歌曲的封面发生变化（换歌、同一首歌换了封面图、封面被清空）时派发一次。**载荷里没有直链、也没有图片字节** —— 要图片自己调 `musicxx.media.cover`（可配 `bind` 绑成纹理），要在着色器里采样就声明渲染项的 `cover` / `image`。观察型、异步：不卡住播放与封面加载；同一个封面源被重复设置不会重复派发。用 `musicxx.hooks.unregister("musicxx.media.cover.changed")` 移除后就不再收到（插件停用 / 卸载、关闭『拟声++』时插件框架也会自动摘掉处理器）。

### `musicxx.net.request`

- 模式：`decision`；阶段：`before+after`；派发：`async`；合并策略：`anyCancel`
- 包装点（实现固定是应用自己：请求由主程序发）：主程序发一次外网请求时派发。`before` 前链（串行、正序）改请求或取消 —— 回答 `{"patch": {…}}` 合并进这次请求，`{"action":"cancel"}` = 这次不发；`after` 后链（串行、**逆序**）读或改响应 —— 回答 `{"value": {…}}` 替换响应，`{"action":"drop"}` = 按“没有响应”处理。原来的 `musicxx.net.request.before` / `musicxx.net.response.after` 是它的两个阶段。载荷字段在主程序接入时补齐：当前版本注册不会报错，也不会触发。

### `musicxx.download`

- 模式：`decision`；阶段：`before+after`；派发：`async`；合并策略：`anyCancel`
- 包装点（实现固定是应用自己：下载由主程序做）：主程序下载一首歌时派发。`before` 前链（串行、正序）改目标文件名 / 目录，或者取消这次下载；`after` 后链（串行、**逆序**）拿到下载好的成品路径，可以改写它或者丢弃这次结果。原来的 `musicxx.download.before` / `musicxx.download.completed` 是它的两个阶段。载荷字段在主程序接入时补齐：当前版本注册不会报错，也不会触发。

### `musicxx.feature.point.call`

- 模式：`decision`；阶段：`before+impl+after`；派发：`async`；合并策略：`firstNonNull`
- 三个阶段都在同一条通道上（`before` / `impl` / `after`），载荷都带 `phase` 以便同一个处理器分辨自己被叫的阶段：`before` 是 `{point, phase, args, caller}`（回答 `{"patch": {"args": {...}}}` 改这次调用的参数、`{"action": "stop"}` 这次不执行实现）、`impl` 是 `{point, phase, args, caller}`（回答见下）、`after` 是 `{point, phase, args, value, caller}`（回答 `{"value": ...}` 改最终结果、`{"action": "drop"}` 这次没有结果）。以下是实现链（`impl`）的规则：有人要算一个**外置插件声明的功能点**时派发一次（`point` = 功能点 id，`args` = 调用方给的参数，`caller` = 调用方插件 id，空串 = 应用自己或命令行）。这是**一条通用的实现通道**：插件声明的功能点共用它，而不是每个点各占一个钩子 id（插件框架只认固定的钩子表，见 docs/plugin-guide.md）。处理器只实现自己负责的点：不是自己的点直接返回空（空 = 不回答）；是自己的点就返回 `{"patch": {"value": <任意 JSON>}}` —— **必须用 `patch` 包一层**，插件框架的 firstNonNull 只在收到 `patch` 或 `action` 时才停止遍历，否则后面的处理器会覆盖你的结果。`value` 键就是“我答了”的判据（值可以是 null，例如“算过了、结果是空”）。返回 `{"disable": true}` 是显式置空：这次不再问别的实现。**同一条通道上所有实现都会收到同一次调用**，请按 `point` 过滤，不要假设只有自己收到。不要回答自己发起的调用（`caller` 等于自己的插件 id 时返回空；SDK 糖会自动做）。主程序不设超时，最多等到调用方自己的动作超时；同一个功能点 + 同一个调用方的上一次调用没结束时，调用方拿到 `busy`。功能点由插件用动作 `musicxx.feature.point.define` 声明、`musicxx.feature.point.undefine` 收回，声明跟着插件生命周期走。

## 派发方式

| 派发 | 含义 | 调用点写法 |
|---|---|---|
| `sync` | 调用点就地等待裁决（占用调用线程） | `decide(...)` |
| `async` | 调用点本身是 Future：异步派发，结果经事件回传 | `await decideAsync(...)` |

两种方式对插件处理器是**透明的**：处理器照常返回裁决对象即可；区别只在插件框架侧（同步派发阻塞调用线程，异步派发不阻塞、结果经 `musicxx.hook.decision.result` 回传）。
异步派发的调用点在拿到结果前可能已经切歌/切列表，**插件要自己在载荷里带上身份字段（`sid` 等）并在调用点校验**，插件框架只负责丢弃过期结果。

## 裁决对象通用外壳

```jsonc
{ "action": "continue" | "skip" | "cancel" | "replace",
  "patch": { /* 钩子专属字段 */ },
  "error": "可选说明" }
```

- 返回 `null` / 空对象表示“不裁决”，交给下一个处理器；
- `action` 的主程序语义由各调用点决定（例如 `beforePlaySong` 的 `skip` 表示跳过本曲）；
- 处理器必须尽快返回：它跑在插件框架线程上，慢就是业务与其它插件一起等（插件框架不设置等待超时、也不会强行中断）。

## 处理器失败

- 处理器抛异常 / 返回失败只记日志与统计，**不影响其它处理器与插件**（插件框架不会因为失败次数暂停派发、也不会卸载插件）；
- 处理器耗时只记统计（管理页「外部插件 → 调试」分页可看单处理器平均/最大耗时）；
- 派发期间注册/注销钩子不会破坏本轮遍历：插件框架在派发前对处理器列表做快照。
