# 钩子总表（插件作者文档）

> 本文件由 `tools/gen_contract.dart` 从 `tools/hooks.def.json` 生成，请勿手改。
>
> 注册钩子时必须写全名（官方 `musicxx.*`）；插件自定义事件/能力/UI 项用
> `plugin.<pluginId>.*`。宿主会拒绝未知钩子与未知前缀。

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

## 派发方式

| 派发 | 含义 | 调用点写法 |
|---|---|---|
| `sync` | 调用点就地等待裁决（占用调用线程，有等待预算） | `decide(...)` |
| `async` | 调用点本身是 Future：异步派发，结果经事件回传 | `await decideAsync(...)` |

两种方式对插件处理器是**透明的**：处理器照常返回裁决对象即可；区别只在宿主侧（同步派发阻塞调用线程，异步派发不阻塞、结果经 `musicxx.hook.decision.result` 回传）。

## 裁决对象通用外壳

```jsonc
{
    "action": "continue" | "skip" | "cancel" | "replace",
    "patch": { /* 钩子专属字段 */ },
    "error": "可选说明" 
}
```

- 返回 `null` / 空对象表示"不裁决"，交给下一个处理器；
- `action` 的宿主语义由各调用点决定（例如 `beforePlaySong` 的 `skip` 表示跳过本曲）；
- 处理器必须尽快返回：宿主对整链有等待预算，超时按"无裁决"继续（不打断插件）。
