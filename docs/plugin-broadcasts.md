# 广播总表（插件作者文档）

> 本文件由 `tools/gen_contract.dart` 从 `tools/capabilities.def.json` 生成，请勿手改。

> 原来的 `docs/plugin-hooks.md` 已经拆成两张表：**观察型钩子**（只通知、不回答）就是这里的频道，**决策型钩子**（能改输入、给结果、改输出）在 [plugin-capabilities.md](plugin-capabilities.md)。订阅 / 发布的写法见 [plugin-api.md](plugin-api.md) 第 8 节。

**广播** = 属主改了东西、通知所有关心的人。它没有返回值、不等待、允许丢中间值；
要不要"当前值"由声明决定：`cache: latest` 的频道订阅时就能拿到当前值，也能同步读；
`cache: none` 的是事件，只能等下一次通知。

| 字段 | 含义 |
|---|---|
| `cache` | `latest` = 有当前值（快照，只保留最后一份）；`none` = 只有事件 |
| `writable` | `owner` = 只有属主能写；`anyone` = 任何人都能写（变量） |
| `pushOnSubscribe` | 订阅时立刻收到当前缓存值（仅 `cache: latest`） |
| `coalesce` | 允许合并（允许丢中间值）；变量类是 `false`（写要回执，不能丢） |
| `scope` | `app` = 业务频道；`system` = 调试与自省频道（插件不能写） |

## 频道清单

| 广播 id | 缓存 | 写权限 | 订阅即推 | 合并 | 作用域 | 状态 |
|---|---|---|---|---|---|---|
| `musicxx.app.start` | none | owner | 否 | 允许 | app | 已接入 |
| `musicxx.app.ready` | none | owner | 否 | 允许 | app | 已接入 |
| `musicxx.app.background` | none | owner | 否 | 允许 | app | 已接入 |
| `musicxx.app.foreground` | none | owner | 否 | 允许 | app | 已接入 |
| `musicxx.app.upgrade` | none | owner | 否 | 允许 | app | 未接入 |
| `musicxx.player.source.resolved` | none | owner | 否 | 允许 | app | 已接入 |
| `musicxx.player.state` | none | owner | 否 | 允许 | app | 已接入 |
| `musicxx.player.position` | none | owner | 否 | 允许 | app | 已接入 |
| `musicxx.player.completed` | none | owner | 否 | 允许 | app | 已接入 |
| `musicxx.player.qualityChanged` | none | owner | 否 | 允许 | app | 未接入 |
| `musicxx.song.changed` | none | owner | 否 | 允许 | app | 已接入 |
| `musicxx.song.meta.writeback` | none | owner | 否 | 允许 | app | 未接入 |
| `musicxx.song.removed` | none | owner | 否 | 允许 | app | 未接入 |
| `musicxx.history.record` | none | owner | 否 | 允许 | app | 未接入 |
| `musicxx.local.scan.finished` | none | owner | 否 | 允许 | app | 未接入 |
| `musicxx.lyric.current` | none | owner | 否 | 允许 | app | 未接入 |
| `musicxx.media.cover.changed` | none | owner | 否 | 允许 | app | 已接入 |
| `musicxx.net.lan.event` | none | owner | 否 | 允许 | app | 未接入 |
| `musicxx.ui.page.enter` | none | owner | 否 | 允许 | app | 未接入 |
| `musicxx.ui.page.leave` | none | owner | 否 | 允许 | app | 未接入 |
| `musicxx.ui.theme.changed` | none | owner | 否 | 允许 | app | 未接入 |
| `musicxx.ui.user.action` | none | owner | 否 | 允许 | app | 未接入 |
| `musicxx.state.app` | latest | owner | 是 | 允许 | app | 已接入 |
| `musicxx.state.player` | latest | owner | 是 | 允许 | app | 已接入 |
| `musicxx.state.song` | latest | owner | 是 | 允许 | app | 已接入 |
| `musicxx.state.playlist` | latest | owner | 是 | 允许 | app | 已接入 |
| `musicxx.state.lyric` | latest | owner | 是 | 允许 | app | 已接入 |
| `musicxx.state.playlists` | latest | owner | 是 | 允许 | app | 已接入 |
| `musicxx.state.env` | latest | owner | 是 | 允许 | app | 已接入 |
| `musicxx.state.renderSlots` | latest | owner | 是 | 允许 | app | 已接入 |
| `musicxx.state.spectrum` | latest | owner | 是 | 允许 | app | 已接入 |
| `musicxx.state.changed` | none | owner | 否 | 允许 | system | 已接入 |
| `musicxx.event.published` | none | owner | 否 | 允许 | system | 已接入 |

## 动态频道（id 由属主在运行期决定）

| id 写法 | 缓存 | 写权限 | 订阅即推 | 合并 | 作用域 | 说明 |
|---|---|---|---|---|---|---|
| `musicxx.var.<属主>.<键>` | latest | anyone | 是 | 不合并 | app | 有属主的小值（配置 / 小状态）；写要回执，订阅时立刻收到当前值 |
| `plugin.<插件 id>.net` | none | owner | 否 | 允许 | app | 插件自己的网络请求进度 / 完成事件 |

## 载荷（已接入的频道）

### `musicxx.app.start` 应用启动

载荷 `{version, platform, branch, installId, lang}`。主程序启动阶段（业务 Store 与插件框架都就绪之后）派发一次：适合做初始化自己的状态、登记定时器这类事，观察型没有裁决。

### `musicxx.app.ready` 应用就绪

载荷 `{firstRun, restoredSong}`：`firstRun` = 是否首次启动，`restoredSong` = 恢复播放的歌曲（可能缺省）。首屏与数据都就绪后派发一次（比 `app.start` 晚），观察型。

### `musicxx.app.background` 应用进入后台

载荷 `{state}`：`state` = 触发这次通知的生命周期状态名（`paused` / `hidden` / `inactive` / `detached`）。应用**真的**进后台时派发一次（桌面端隐藏 / 最小化窗口，移动端切到后台）；窗口只是失去焦点（`inactive` 但画面仍可见）不算后台。派发前状态镜像已刷新（`musicxx.state.app.foreground` 为 false），处理器里同步读到的就是新状态；同一状态重复上报不会重复派发。观察型、异步：不等待处理器，不影响播放与缓存。要记“在后台待了多久”用 `musicxx.app.foreground` 的 `backgroundMs`；要读当前值用变量 `musicxx.app.lifecycle`（可订阅）或状态镜像。

### `musicxx.app.foreground` 应用回到前台

载荷 `{state, backgroundMs}`：`state` = 回到前台时的生命周期状态名（一般是 `resumed`），`backgroundMs` = 这次在后台待了多久（毫秒）。应用回到前台时派发一次；**只在前台与后台真的切换时**派发，所以启动阶段的初次 `resumed` 不会触发它（启动用 `musicxx.app.start` / `musicxx.app.ready`）。观察型、异步：不等待处理器；插件适合在这里做“回前台补一次刷新”（检查时间是否跨越太久、重新拉一次数据）。

### `musicxx.player.source.resolved` 音源解析完成

载荷 `{sid, srcKey, useSrcKey, parsedType, pathKind, hasHeaders, cacheStream}`：某个音源解析完成（拿到真正播放用的路径与请求头）之后派发。载荷里没有直链与 token，只报来源类型与路径种类（本地 / 缓存 / 远程流 / 带请求头）；观察型。

### `musicxx.player.state` 播放状态变化

载荷 `{state, sid, positionMs}`。`state` 是播放状态文本：播放 / 暂停 / 停止这三个事件给 `play`、`pause`、`stop`，其它状态变化给枚举名 `Play` / `Pause` / `Stop` / `Completed`；要判断当前是否在播放，请读状态镜像 `musicxx.state.env.isPlaying`（布尔）。观察型，只在播放状态变化时派发（播放进度不走这个频道）。

### `musicxx.player.position` 播放进度

载荷 `{sid, positionMs, durationMs}`：播放进度，**按 1 秒节流**推送（应用本来就有 1 Hz 的进度节流，直接复用它；暂停与缓冲时不推进）。`positionMs` = 当前播放位置（毫秒），`durationMs` = 总时长（还不知道时为 0），`sid` = 当前歌曲会话 id（切歌后以新 `sid` 为准）。观察型、异步：不等待处理器，不影响播放。它只报进度、不报状态：要判断“是否在播放”读状态镜像 `musicxx.state.env.isPlaying`，要“状态变了”订阅广播频道 `musicxx.player.state`；要读当前值用变量 `musicxx.player.position`（镜像 `musicxx.state.player.position` 仍只在启动、切歌与播放状态变化时刷新）。

### `musicxx.player.completed` 一曲播放完成

载荷 `{sid, playedMs}`：一曲播放完成（正常结束或按完成处理）之后派发；观察型。它只表示这首歌放完了，不代表已经切歌（切歌另有 `musicxx.song.changed`）。

### `musicxx.song.changed` 切歌

载荷 `{sid, prevSid, song}`：`song` 与 `beforePlaySong` 的同一个只读视图，`prevSid` = 上一首的 sid（没有则为 null）。切歌时派发；派发前插件框架会先刷新状态镜像，所以处理器里同步读 `musicxx.state.song` 拿到的就是新歌。观察型。

### `musicxx.media.cover.changed` 封面变化

载荷 `{srcKey, name, artist, hasCover, kind}`（`kind` = `local` / `cache` / `content` / `asset` / `network`，没有封面时为空串）：当前歌曲的封面发生变化（换歌、同一首歌换了封面图、封面被清空）时派发一次。**载荷里没有直链、也没有图片字节** —— 要图片自己调 `musicxx.media.cover`（可配 `bind` 绑成纹理），要在着色器里采样就声明渲染项的 `cover` / `image`。观察型、异步：不卡住播放与封面加载；同一个封面源被重复设置不会重复派发。用 `musicxx.broadcast.unsubscribe("musicxx.media.cover.changed")` 移除后就不再收到（插件停用 / 卸载、关闭『拟声++』时插件框架也会自动摘掉订阅）。

### `musicxx.state.changed` 状态镜像变化通知

过渡期仍在的总线话题：订阅具体状态键（`musicxx.state.*`）比订阅它更准。
