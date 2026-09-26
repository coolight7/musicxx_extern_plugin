# 钩子总表（插件作者文档）

> 本文件由 `tools/gen_contract.dart` 从 `tools/hooks.def.json` 生成，请勿手改。
>
> 注册钩子时必须写全名（官方 `musicxx.*`）；插件自定义事件/能力/UI 项用
> `plugin.<pluginId>.*`。宿主会拒绝未知钩子与未知前缀。

## 怎么读这张表

- **阶段**：`P0` = 应用侧**已经埋点**（当前版本会派发，插件注册后会被调用）；`P1` = 钩子契约已冻结、应用侧**尚未埋点**（注册不会报错，但当前版本不会触发）。想知道某个钩子此刻有没有处理器，看管理页「外部插件 → 调试」分页的钩子统计（`hooks` 段），或在插件里调 `musicxx.hooks.has(id)`（只反映自己注册没注册）。
- **模式**：`observe` = 只通知（返回值忽略、不等待、没有预算）；`decision` = 可裁决，返回 `null` 表示这次不表态。
- **派发**：`sync` = 调用点就地等待（占用调用线程）；`async` = 不占用调用线程，裁决结果经 `musicxx.hook.decision.result` 事件回传（调用点用 `decideAsync` 时）。
- **合并策略**：多个处理器给出裁决时怎么合并（`firstNonNull` 首个非空生效并停止询问、`anyCancel` 任一 cancel/skip 即生效、`allMerge` 全部合并、`lastWrite` 最后一个生效）。
- **软/硬预算**：软预算 = 整条处理器链最多等多久（超时按“无裁决”继续，不打断插件）；硬预算 = 单个处理器耗时超过它只记一条 `musicxx.plugin.warn` 与统计。JS 插件的处理器链还有一层固定上限：最多等 100 ms（Promise 超预算按不裁决处理）。

| 钩子 id | 模式 | 派发 | 合并策略 | 软/硬预算 (ms) | 阶段 |
|---|---|---|---|---|---|
| `musicxx.app.start` | observe | async | lastWrite | 0 / 0 | P0 |
| `musicxx.app.ready` | observe | async | lastWrite | 0 / 0 | P0 |
| `musicxx.app.background` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.app.foreground` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.app.exit` | decision | sync | anyCancel | 30 / 100 | P1 |
| `musicxx.app.deepLink` | decision | sync | anyCancel | 30 / 100 | P1 |
| `musicxx.app.upgrade` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.player.beforePlaySong` | decision | async | anyCancel | 30 / 100 | P0 |
| `musicxx.player.source.beforeParse` | decision | async | firstNonNull | 50 / 200 | P0 |
| `musicxx.player.source.resolved` | observe | async | lastWrite | 0 / 0 | P0 |
| `musicxx.player.beforeOpen` | decision | async | firstNonNull | 50 / 200 | P1 |
| `musicxx.player.state` | observe | async | lastWrite | 0 / 0 | P0 |
| `musicxx.player.position` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.player.seek` | decision | sync | anyCancel | 30 / 100 | P1 |
| `musicxx.player.volume` | decision | sync | anyCancel | 30 / 100 | P1 |
| `musicxx.player.speed` | decision | sync | anyCancel | 30 / 100 | P1 |
| `musicxx.player.pitch` | decision | sync | anyCancel | 30 / 100 | P1 |
| `musicxx.player.error` | decision | sync | anyCancel | 30 / 100 | P0 |
| `musicxx.player.completed` | observe | async | lastWrite | 0 / 0 | P0 |
| `musicxx.player.qualityChanged` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.media.notification` | decision | sync | lastWrite | 30 / 100 | P1 |
| `musicxx.song.changed` | observe | async | lastWrite | 0 / 0 | P0 |
| `musicxx.song.info.analyse` | decision | sync | firstNonNull | 30 / 100 | P1 |
| `musicxx.song.meta.writeback` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.song.beforeAdd` | decision | sync | anyCancel | 30 / 100 | P1 |
| `musicxx.song.removed` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.playlist.loaded` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.playlist.filter` | decision | async | allMerge | 50 / 200 | P1 |
| `musicxx.songlist.filter` | decision | async | allMerge | 50 / 200 | P1 |
| `musicxx.history.record` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.local.scan.file` | decision | sync | anyCancel | 30 / 100 | P1 |
| `musicxx.local.scan.finished` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.lyric.load.before` | decision | async | firstNonNull | 50 / 200 | P1 |
| `musicxx.lyric.loaded` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.lyric.transform` | decision | async | allMerge | 50 / 200 | P1 |
| `musicxx.lyric.current` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.lyric.searchStr` | decision | sync | firstNonNull | 30 / 100 | P1 |
| `musicxx.lyric.provider` | decision | async | firstNonNull | 50 / 200 | P1 |
| `musicxx.icon.request` | decision | async | firstNonNull | 50 / 200 | P1 |
| `musicxx.icon.generated` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.media.info.request` | decision | async | firstNonNull | 50 / 200 | P1 |
| `musicxx.media.wave.ready` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.media.chorus.analysed` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.net.request.before` | decision | async | anyCancel | 50 / 200 | P1 |
| `musicxx.net.response.after` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.net.server.route` | decision | async | firstNonNull | 100 / 500 | P1 |
| `musicxx.net.mcp.tools` | decision | sync | allMerge | 50 / 200 | P1 |
| `musicxx.net.lan.event` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.download.before` | decision | async | anyCancel | 50 / 200 | P1 |
| `musicxx.download.completed` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.cache.beforeTrim` | decision | sync | anyCancel | 30 / 100 | P1 |
| `musicxx.cache.pathRequest` | decision | sync | firstNonNull | 30 / 100 | P1 |
| `musicxx.ui.home.entries` | decision | sync | allMerge | 30 / 100 | P1 |
| `musicxx.ui.song.actions` | decision | sync | allMerge | 30 / 100 | P1 |
| `musicxx.ui.playlist.actions` | decision | sync | allMerge | 30 / 100 | P1 |
| `musicxx.ui.route.resolve` | decision | sync | firstNonNull | 50 / 200 | P1 |
| `musicxx.ui.page.enter` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.ui.page.leave` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.ui.theme.changed` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.ui.notify` | decision | sync | anyCancel | 30 / 100 | P1 |
| `musicxx.ui.user.action` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.script.bound` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.script.disposed` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.script.action.executed` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.clocking.tick` | observe | async | lastWrite | 0 / 0 | P1 |
| `musicxx.listenTogether.event` | observe | async | lastWrite | 0 / 0 | P1 |

## 已埋点钩子的载荷与裁决（P0）

下面是应用侧**已经埋点**的钩子：处理器拿到的载荷字段与裁决语义都在这里。P1 钩子只冻结了 id / 模式 / 派发 / 合并策略，载荷字段在应用侧接入时补齐（接入后会写进 `tools/hooks.def.json` 的 `doc` 字段并重新生成本文件）。

> 载荷统一是 JSON 对象；不裁决时返回 `null`（JS）或把出参留空（C++）。
> 载荷里不放音频直链与 token：需要地址时请用 `musicxx.net` / 宿主动作自行获取。

### `musicxx.app.start`

- 模式：`observe`；派发：`async`；合并策略：`lastWrite`；软/硬预算：`0 / 0` ms
- 载荷 `{version, platform, branch, installId, lang}`。应用启动阶段（业务 Store 与插件宿主都就绪之后）派发一次：适合做初始化自己的状态、登记定时器这类事，观察型没有裁决。

### `musicxx.app.ready`

- 模式：`observe`；派发：`async`；合并策略：`lastWrite`；软/硬预算：`0 / 0` ms
- 载荷 `{firstRun, restoredSong}`：`firstRun` = 是否首次启动，`restoredSong` = 恢复播放的歌曲（可能缺省）。首屏与数据都就绪后派发一次（比 `app.start` 晚），观察型。

### `musicxx.player.beforePlaySong`

- 模式：`decision`；派发：`async`；合并策略：`anyCancel`；软/硬预算：`30 / 100` ms
- 载荷 `{sid, song, mode, quality}`：`song` 是只读视图（`sid/name/artist/album/durationMs/year/genre/srcKey/audio[]/video[]`，不含音频直链），`mode` = 播放模式、`quality` = 当前音质。裁决只实现 `{"action":"skip"}` = 跳过本曲（走应用既有的切下一曲流程）；`patch`（换音源/改音质）当前版本未实现，会被记一条日志后忽略。异步派发：切歌不会被插件拖慢；等待期间若又来了新的切歌请求，本次裁决被丢弃（插件可以自己用 `sid` 判断）。

### `musicxx.player.source.beforeParse`

- 模式：`decision`；派发：`async`；合并策略：`firstNonNull`；软/硬预算：`50 / 200` ms
- 载荷 `{sid, srcKey, src, index, tryLocalOrCache}`：`src` 是音源视图（`type/empty/srcKey/quality/durationMs/hashMd5/size`），`index` = 本轮解析的音源下标。裁决：`{"action":"skip"}` = 本轮不解析该音源（调用方接着试下一个）；`patch.src = {"type":"Local|UrlLink|Bili|...","src":"<新地址>","info":{...}}` = 只替换本轮使用的音源（不改动歌曲实体的音源列表），字段不合法会被忽略。异步派发（不卡住解析），等待期间切歌则本次裁决作废。

### `musicxx.player.source.resolved`

- 模式：`observe`；派发：`async`；合并策略：`lastWrite`；软/硬预算：`0 / 0` ms
- 载荷 `{sid, srcKey, useSrcKey, parsedType, pathKind, hasHeaders, cacheStream}`：某个音源解析完成（拿到真正播放用的路径与请求头）之后派发。载荷里没有直链与 token，只报来源类型与路径种类（本地 / 缓存 / 远程流 / 带请求头）；观察型。

### `musicxx.player.state`

- 模式：`observe`；派发：`async`；合并策略：`lastWrite`；软/硬预算：`0 / 0` ms
- 载荷 `{state, sid, positionMs}`。`state` 是播放状态文本：播放 / 暂停 / 停止这三个事件给 `play`、`pause`、`stop`，其它状态变化给枚举名 `Play` / `Pause` / `Stop` / `Completed`；要判断当前是否在播放，请读状态镜像 `musicxx.state.env.isPlaying`（布尔）。观察型，只在播放状态变化时派发（播放进度不走这个钩子）。

### `musicxx.player.error`

- 模式：`decision`；派发：`sync`；合并策略：`anyCancel`；软/硬预算：`30 / 100` ms
- 载荷 `{sid, srcKey, isNowUseSrc, errorCount, durationMs, positionMs}`（`errorCount` 已包含本次失败）。裁决：`{"action":"stop"}` = 停止播放；`{"action":"skip"}`（或 `cancel`）= 跳到下一曲；`{"action":"continue","patch":{"tryNextSrc":false}}` = 不再尝试当前源（跳过忽略错误并重试的分支，直接按应用的换源/下一曲策略走）。这是**同步派发**的钩子，调用点只等 120 ms，超时按无裁决继续。

### `musicxx.player.completed`

- 模式：`observe`；派发：`async`；合并策略：`lastWrite`；软/硬预算：`0 / 0` ms
- 载荷 `{sid, playedMs}`：一曲播放完成（正常结束或按完成处理）之后派发；观察型。它只表示这首歌放完了，不代表已经切歌（切歌另有 `musicxx.song.changed`）。

### `musicxx.song.changed`

- 模式：`observe`；派发：`async`；合并策略：`lastWrite`；软/硬预算：`0 / 0` ms
- 载荷 `{sid, prevSid, song}`：`song` 与 `beforePlaySong` 的同一个只读视图，`prevSid` = 上一首的 sid（没有则为 null）。切歌时派发；派发前宿主会先刷新状态镜像，所以处理器里同步读 `musicxx.state.song` 拿到的就是新歌。观察型。

## 派发方式

| 派发 | 含义 | 调用点写法 |
|---|---|---|
| `sync` | 调用点就地等待裁决（占用调用线程，有等待预算） | `decide(...)` |
| `async` | 调用点本身是 Future：异步派发，结果经事件回传 | `await decideAsync(...)` |

两种方式对插件处理器是**透明的**：处理器照常返回裁决对象即可；区别只在宿主侧（同步派发阻塞调用线程，异步派发不阻塞、结果经 `musicxx.hook.decision.result` 回传）。
异步派发的调用点在拿到结果前可能已经切歌/切列表，**插件要自己在载荷里带上身份字段（`sid` 等）并在调用点校验**，宿主只负责丢弃过期结果。

## 裁决对象通用外壳

```jsonc
{ "action": "continue" | "skip" | "cancel" | "replace",
  "patch": { /* 钩子专属字段 */ },
  "error": "可选说明" }
```

- 返回 `null` / 空对象表示“不裁决”，交给下一个处理器；
- `action` 的宿主语义由各调用点决定（例如 `beforePlaySong` 的 `skip` 表示跳过本曲）；
- 处理器必须尽快返回：宿主对整链有等待预算，超时按“无裁决”继续（不打断插件）。

## 处理器失败与熔断

- 处理器抛异常 / 返回失败只记日志与统计，**不影响其它处理器与插件**；
- 同一个处理器**连续 3 次失败**会被宿主临时暂停派发 60 秒（只暂停这一个处理器，同插件的其它处理器照常工作，插件也不会被卸载）；熔断状态与管理页里的剩余时间见 `hook_stats()` 的 `paused` / `pausedRemainMs`；
- 超过硬预算只是慢，不计失败（记 `musicxx.plugin.warn`，`code = handler_slow`）；
- 派发期间注册/注销钩子不会破坏本轮遍历：宿主在派发前对处理器列表做快照。
