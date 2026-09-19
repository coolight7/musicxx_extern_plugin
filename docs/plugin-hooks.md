# 钩子总表（插件作者文档）

> 本文件由 `tools/gen_contract.dart` 从 `tools/hooks.def.json` 生成，请勿手改。
>
> 注册钩子时必须写全名（官方 `musicxx.*`）；插件自定义事件/能力/UI 项用
> `plugin.<pluginId>.*`。宿主会拒绝未知钩子与未知前缀。

| 钩子 id | 模式 | 合并策略 | 软/硬预算 (ms) | 阶段 |
|---|---|---|---|---|
| `musicxx.app.start` | observe | lastWrite | 0 / 0 | P0 |
| `musicxx.app.ready` | observe | lastWrite | 0 / 0 | P0 |
| `musicxx.app.background` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.app.foreground` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.app.exit` | decision | anyCancel | 30 / 100 | P1 |
| `musicxx.app.deepLink` | decision | anyCancel | 30 / 100 | P1 |
| `musicxx.app.upgrade` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.player.beforePlaySong` | decision | anyCancel | 30 / 100 | P0 |
| `musicxx.player.source.beforeParse` | decision | firstNonNull | 50 / 200 | P0 |
| `musicxx.player.source.resolved` | observe | lastWrite | 0 / 0 | P0 |
| `musicxx.player.beforeOpen` | decision | firstNonNull | 50 / 200 | P1 |
| `musicxx.player.state` | observe | lastWrite | 0 / 0 | P0 |
| `musicxx.player.position` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.player.seek` | decision | anyCancel | 30 / 100 | P1 |
| `musicxx.player.volume` | decision | anyCancel | 30 / 100 | P1 |
| `musicxx.player.speed` | decision | anyCancel | 30 / 100 | P1 |
| `musicxx.player.pitch` | decision | anyCancel | 30 / 100 | P1 |
| `musicxx.player.error` | decision | anyCancel | 30 / 100 | P0 |
| `musicxx.player.completed` | observe | lastWrite | 0 / 0 | P0 |
| `musicxx.player.qualityChanged` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.media.notification` | decision | lastWrite | 30 / 100 | P1 |
| `musicxx.song.changed` | observe | lastWrite | 0 / 0 | P0 |
| `musicxx.song.info.analyse` | decision | firstNonNull | 30 / 100 | P1 |
| `musicxx.song.meta.writeback` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.song.beforeAdd` | decision | anyCancel | 30 / 100 | P1 |
| `musicxx.song.removed` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.playlist.loaded` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.playlist.filter` | decision | allMerge | 50 / 200 | P1 |
| `musicxx.songlist.filter` | decision | allMerge | 50 / 200 | P1 |
| `musicxx.history.record` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.local.scan.file` | decision | anyCancel | 30 / 100 | P1 |
| `musicxx.local.scan.finished` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.lyric.load.before` | decision | firstNonNull | 50 / 200 | P1 |
| `musicxx.lyric.loaded` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.lyric.transform` | decision | allMerge | 50 / 200 | P1 |
| `musicxx.lyric.current` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.lyric.searchStr` | decision | firstNonNull | 30 / 100 | P1 |
| `musicxx.lyric.provider` | decision | firstNonNull | 50 / 200 | P1 |
| `musicxx.icon.request` | decision | firstNonNull | 50 / 200 | P1 |
| `musicxx.icon.generated` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.media.info.request` | decision | firstNonNull | 50 / 200 | P1 |
| `musicxx.media.wave.ready` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.media.chorus.analysed` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.net.request.before` | decision | anyCancel | 50 / 200 | P1 |
| `musicxx.net.response.after` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.net.server.route` | decision | firstNonNull | 100 / 500 | P1 |
| `musicxx.net.mcp.tools` | decision | allMerge | 50 / 200 | P1 |
| `musicxx.net.lan.event` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.download.before` | decision | anyCancel | 50 / 200 | P1 |
| `musicxx.download.completed` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.cache.beforeTrim` | decision | anyCancel | 30 / 100 | P1 |
| `musicxx.cache.pathRequest` | decision | firstNonNull | 30 / 100 | P1 |
| `musicxx.ui.home.entries` | decision | allMerge | 30 / 100 | P1 |
| `musicxx.ui.song.actions` | decision | allMerge | 30 / 100 | P1 |
| `musicxx.ui.playlist.actions` | decision | allMerge | 30 / 100 | P1 |
| `musicxx.ui.route.resolve` | decision | firstNonNull | 50 / 200 | P1 |
| `musicxx.ui.page.enter` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.ui.page.leave` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.ui.theme.changed` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.ui.notify` | decision | anyCancel | 30 / 100 | P1 |
| `musicxx.ui.user.action` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.script.bound` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.script.disposed` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.script.action.executed` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.clocking.tick` | observe | lastWrite | 0 / 0 | P1 |
| `musicxx.listenTogether.event` | observe | lastWrite | 0 / 0 | P1 |

## 裁决对象通用外壳

```jsonc
{ "action": "continue" | "skip" | "cancel" | "replace",
  "patch": { /* 钩子专属字段 */ },
  "error": "可选说明" }
```

- 返回 `null` / 空对象表示"不裁决"，交给下一个处理器；
- `action` 的宿主语义由各调用点决定（例如 `beforePlaySong` 的 `skip` 表示跳过本曲）；
- 处理器必须尽快返回：宿主对整链有等待预算，超时按"无裁决"继续（不打断插件）。
