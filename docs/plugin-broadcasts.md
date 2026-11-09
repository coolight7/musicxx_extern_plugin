# 广播总表（插件作者文档）

> 本文件由 `tools/gen_contract.dart` 从 `tools/capabilities.def.json` 生成，请勿手改。

> 两张表：**广播频道**（只通知、不回答、不参与合并）就是本文里的这些，**能力**（能改输入、给结果、改输出）在 [plugin-capabilities.md](plugin-capabilities.md)。订阅 / 发布的写法见 [plugin-api.md](plugin-api.md) 第 8 节。

**广播** = 属主改了东西、通知所有关心的人。它没有返回值、不等待、允许丢中间值；
要不要"当前值"由声明决定：`cache: latest` 的频道订阅时就能拿到当前值，也能同步读；
`cache: none` 没有当前值，只能等下一次通知。

广播只做四件事：**声明自己的频道 / 发布 / 读当前值 / 订阅**。
它**不提供写入**（没有"给别人的频道写值"这个操作，也没有回执）：
要改应用状态一律调能力（见 [plugin-capabilities.md](plugin-capabilities.md)），
结果的校验与拒绝原因由能力告诉你。

| 字段 | 含义 |
|---|---|
| `cache` | `latest` = 有当前值（只保留最后一份）；`none` = 没有当前值，只能等下一次通知 |
| `publish` | `owner` = 只有属主能发布；`anyone` = 任何人都能发布（共享频道） |
| `pushOnSubscribe` | 订阅时立刻收到当前缓存值（仅 `cache: latest`） |
| `coalesce` | 允许合并（允许丢中间值）；带当前值的频道一般写 `false`，每次发布都通知 |
| `scope` | `app` = 业务频道；`system` = 调试与自省频道（插件不能发布） |
| `type` | 值的类型提示（`bool` / `number` / `string` / `json`，可选；只用于文档与自省） |
| `options` | 枚举取值域（稳定 id，不用本地化文案；可选，只用于文档与自省） |

## 频道清单

| 广播 id | 缓存 | 谁能发布 | 订阅即推 | 合并 | 作用域 | 状态 |
|---|---|---|---|---|---|---|
| `musicxx.app.start` | none | owner | 否 | 允许 | app | 已接入 |
| `musicxx.app.ready` | none | owner | 否 | 允许 | app | 已接入 |
| `musicxx.app.background` | none | owner | 否 | 允许 | app | 已接入 |
| `musicxx.app.foreground` | none | owner | 否 | 允许 | app | 已接入 |
| `musicxx.app.upgrade` | none | owner | 否 | 允许 | app | 未接入 |
| `musicxx.app.lifecycle` | latest | owner | 是 | 不合并 | app | 已接入 |
| `musicxx.player.source.resolved` | none | owner | 否 | 允许 | app | 已接入 |
| `musicxx.player.state` | none | owner | 否 | 允许 | app | 已接入 |
| `musicxx.player.position` | latest | owner | 是 | 不合并 | app | 已接入 |
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
| `musicxx.ui.animatedLevel` | latest | owner | 是 | 不合并 | app | 已接入 |
| `musicxx.ui.particleAnimate` | latest | owner | 是 | 不合并 | app | 已接入 |
| `musicxx.ui.songIconRotate` | latest | owner | 是 | 不合并 | app | 已接入 |
| `musicxx.ui.songIconWave` | latest | owner | 是 | 不合并 | app | 已接入 |
| `musicxx.ui.fontFamily` | latest | owner | 是 | 不合并 | app | 已接入 |
| `musicxx.theme.available` | latest | owner | 是 | 不合并 | app | 已接入 |
| `musicxx.theme.current` | latest | owner | 是 | 不合并 | app | 已接入 |
| `musicxx.theme.mode` | latest | owner | 是 | 不合并 | app | 已接入 |
| `musicxx.state.app` | latest | owner | 是 | 允许 | app | 已接入 |
| `musicxx.state.player` | latest | owner | 是 | 允许 | app | 已接入 |
| `musicxx.state.song` | latest | owner | 是 | 允许 | app | 已接入 |
| `musicxx.state.playlist` | latest | owner | 是 | 允许 | app | 已接入 |
| `musicxx.state.lyric` | latest | owner | 是 | 允许 | app | 已接入 |
| `musicxx.state.playlists` | latest | owner | 是 | 允许 | app | 已接入 |
| `musicxx.state.env` | latest | owner | 是 | 允许 | app | 已接入 |
| `musicxx.state.renderSlots` | latest | owner | 是 | 允许 | app | 已接入 |
| `musicxx.state.spectrum` | latest | owner | 是 | 允许 | app | 已接入 |

## 官方频道当前值

下面这些频道带当前值（`cache: latest` + 订阅即推），`type` / `options` 说明值长什么样：

| 广播 id | 类型 | 取值 | 要改它请调能力 |
|---|---|---|---|
| `musicxx.app.lifecycle` | string | foreground / background | （只读，由应用自己决定） |
| `musicxx.player.position` | json | - | `musicxx.player.seek` |
| `musicxx.ui.animatedLevel` | string | undefined / disable / low / medium / high / max | `musicxx.ui.setAnimatedLevel` |
| `musicxx.ui.particleAnimate` | bool | - | `musicxx.ui.setParticleAnimate` |
| `musicxx.ui.songIconRotate` | bool | - | `musicxx.ui.setSongIconRotate` |
| `musicxx.ui.songIconWave` | bool | - | `musicxx.ui.setSongIconWave` |
| `musicxx.ui.fontFamily` | string | - | `musicxx.ui.setFontFamily` |
| `musicxx.theme.available` | json | - | （只读，由应用自己决定） |
| `musicxx.theme.current` | string | mimicryLight / mimicryNight | `musicxx.theme.setCurrent` / `musicxx.theme.tempSwitch` |
| `musicxx.theme.mode` | string | light / night / followSystem / followTime / followScreenBrightness | `musicxx.theme.setMode` |

## 订阅频率限制

高频频道（典型是 `musicxx.player.position`）不必让应用降频：**每个订阅方自己声明通知频率**，
互不影响。订阅时多传一个 `options`：

```json
{ "intervalMs": 200, "mode": "delay" }
```

| 字段 | 含义 |
|---|---|
| `intervalMs` | 通知间隔（毫秒）；`0` / 缺省 = 不限频，每次发布都通知 |
| `mode` | `delay`（默认）= 间隔内只保留**最新一条**，间隔到点补发一次；`drop` = 间隔内的通知直接丢掉，只有距上次通知已满 `intervalMs` 的才通知 |

几条约定：

- **订阅后的第一条通知立即到达**；`pushOnSubscribe` 补的当前值算第一条，也是限频窗口的起点；
- 计时用单调时钟，改系统时间不影响；`intervalMs` 传负数按 0 处理，`mode` 不认识按 `delay`；
- 退订 / 插件停用：等着的补发一并丢弃，不会再回调；
- 例：`intervalMs = 200`，广播时刻 `0 / 50 / 100 / 250 / 300` 时，
  不限频收到 `0 50 100 250 300`，`delay` 收到 `0`（值=0）、`200`（值=100）、`400`（值=300），
  `drop` 收到 `0`、`250`。

只看候选值范围、临时把 `intervalMs` 调大都不需要重新订阅；换频率就退订后重新订阅。

## 表外的频道（id 自己造）

这张表只是**预设**：应用与插件都可以用表里没有的 id 发布 / 订阅，不需要事先登记。表外 id 的默认属性是「没有当前值（`cache: none`）、只有属主能发布、允许合并、作用域 `app`」；属主按 id 前缀判：`plugin.<插件 id>.*` 归那个插件，其它归应用。

- 插件自定义频道：`plugin.<你的插件 id>.<名字>`（网络请求的进度 / 完成建议叫 `.net`），只有你能发，别的插件能订阅；
- 应用自己的运行期频道（例如埋点广播 `musicxx.point.<埋点 id>.<种类>`）同理，不需要在定义文件里占一行。

要当前值（订阅时就能拿到）/ 允许别人发布这类属性时，才需要在定义文件的 `broadcasts` 里占一行。

## 载荷（已接入的频道）

### `musicxx.app.start` 应用启动

载荷 `{version, platform, branch, installId, lang}`。主程序启动阶段（业务 Store 与插件框架都就绪之后）派发一次：适合做初始化自己的状态、登记定时器这类事。它只通知，没有返回值，也不参与裁决。

### `musicxx.app.ready` 应用就绪

载荷 `{firstRun, restoredSong}`：`firstRun` = 是否首次启动，`restoredSong` = 恢复播放的歌曲（可能缺省）。首屏与数据都就绪后发布一次（比 `app.start` 晚）；只通知，没有返回值。

### `musicxx.app.background` 应用进入后台

载荷 `{state}`：`state` = 触发这次通知的生命周期状态名（`paused` / `hidden` / `inactive` / `detached`）。应用**真的**进后台时派发一次（桌面端隐藏 / 最小化窗口，移动端切到后台）；窗口只是失去焦点（`inactive` 但画面仍可见）不算后台。发布前会先更新广播 `musicxx.state.app`（`foreground` 为 false），订阅回调里同步读到的就是新状态；同一状态重复上报不会重复发布。只通知、异步：不等待订阅方，不影响播放与缓存。要记“在后台待了多久”用 `musicxx.app.foreground` 的 `backgroundMs`；要读当前值订阅广播 `musicxx.app.lifecycle`（`cache: latest`）或同步读 `musicxx.state.app`。

### `musicxx.app.foreground` 应用回到前台

载荷 `{state, backgroundMs}`：`state` = 回到前台时的生命周期状态名（一般是 `resumed`），`backgroundMs` = 这次在后台待了多久（毫秒）。应用回到前台时发布一次；**只在前台与后台真的切换时**发布，所以启动阶段的初次 `resumed` 不会触发它（启动用 `musicxx.app.start` / `musicxx.app.ready`）。只通知、异步：不等待订阅方；插件适合在这里做“回前台补一次刷新”（检查时间是否跨越太久、重新拉一次数据）。

### `musicxx.app.lifecycle` 前后台状态

值：`foreground` / `background`。应用在前台与后台之间切换时发布，订阅即拿到当前值。

要“在后台待了多久 / 什么时候回前台”订阅广播 `musicxx.app.foreground`（载荷带 `backgroundMs`）或 `musicxx.app.background`；要完整状态同步读 `musicxx.state.app`。**只读**：应用的前后台状态由系统与应用自己决定，没有写入入口。

### `musicxx.player.source.resolved` 音源解析完成

载荷 `{sid, srcKey, useSrcKey, parsedType, pathKind, hasHeaders, cacheStream}`：某个音源解析完成（拿到真正播放用的路径与请求头）之后发布。载荷里没有直链与 token，只报来源类型与路径种类（本地 / 缓存 / 远程流 / 带请求头）；只通知，没有返回值。

### `musicxx.player.state` 播放状态变化

载荷 `{state, sid, positionMs}`。`state` 是播放状态文本：播放 / 暂停 / 停止这三个事件给 `play`、`pause`、`stop`，其它状态变化给枚举名 `Play` / `Pause` / `Stop` / `Completed`；要判断当前是否在播放，请读广播 `musicxx.state.env.isPlaying`（布尔，`cache: latest`）。只在播放状态变化时发布（播放进度不走这条广播）。

### `musicxx.player.position` 播放进度

载荷 `{sid, positionMs, durationMs}`：播放进度，**不限频**（播放内核上报一次就发布一次，暂停与缓冲时不推进；同一位置重复上报由应用按毫秒去重）。`positionMs` = 当前播放位置（毫秒），`durationMs` = 总时长（还不知道时为 0），`sid` = 当前歌曲会话 id（切歌后以新 `sid` 为准）。

**这条广播带当前值**（`cache: latest`）：订阅即推，订阅时先拿到当前值，之后按播放内核节奏收到新值；也能 `read` 读当前值。**订阅频率由订阅方自己声明**：`subscribe(id, fn, {intervalMs, mode})`——`intervalMs` = 通知间隔（0 = 不限），`mode` = `delay`（间隔内只留最新一条，到点补发，默认）/ `drop`（间隔内的通知直接丢掉）。只想要 1 秒一次就是 `{intervalMs: 1000, mode: "delay"}`。

**广播是只读的**：要跳转 / 拖动进度，调用能力 `musicxx.player.seek`（参数 `positionMs` / `percent`），广播侧没有写入入口。它只报进度、不报状态：要判断“是否在播放”读广播 `musicxx.state.env.isPlaying`，要“状态变了”订阅广播 `musicxx.player.state`。

### `musicxx.player.completed` 一曲播放完成

载荷 `{sid, playedMs}`：一曲播放完成（正常结束或按完成处理）之后发布；只通知，没有返回值。它只表示这首歌放完了，不代表已经切歌（切歌另有 `musicxx.song.changed`）。

### `musicxx.song.changed` 切歌

载荷 `{sid, prevSid, song}`：`song` 与 `beforePlaySong` 的同一个只读视图，`prevSid` = 上一首的 sid（没有则为 null）。切歌时发布；发布前插件框架会先更新广播 `musicxx.state.song`，所以订阅回调里同步读到的就是新歌。只通知，没有返回值。

### `musicxx.media.cover.changed` 封面变化

载荷 `{srcKey, name, artist, hasCover, kind}`（`kind` = `local` / `cache` / `content` / `asset` / `network`，没有封面时为空串）：当前歌曲的封面发生变化（换歌、同一首歌换了封面图、封面被清空）时发布一次。**载荷里没有直链、也没有图片字节** —— 要图片自己调能力 `musicxx.media.cover`（可配 `bind` 绑成纹理），要在着色器里采样就声明渲染项的 `cover` / `image`。只通知、异步：不卡住播放与封面加载；同一个封面源被重复设置不会重复发布。用 `musicxx.broadcast.unsubscribe("musicxx.media.cover.changed")` 移除后就不再收到（插件停用 / 卸载、关闭『拟声++』时插件框架也会自动摘掉订阅）。

### `musicxx.ui.animatedLevel` 动画等级

值：`undefined` / `disable` / `low` / `medium` / `high` / `max`（枚举 id，不是本地化文案）。用户或插件改了动画等级后发布，订阅即拿到当前值。

**只读**：要改等级调用能力 `musicxx.ui.setAnimatedLevel`（参数 `level`），那里会校验取值并说明拒绝原因。

### `musicxx.ui.particleAnimate` 粒子动画

值：`true` / `false`。设置里改了之后发布，订阅即拿到当前值。**只读**：要改调用能力 `musicxx.ui.setParticleAnimate`（参数 `value`）。

### `musicxx.ui.songIconRotate` 歌曲图旋转

值：`true` / `false`（方形歌曲图下不生效）。设置里改了之后发布，订阅即拿到当前值。**只读**：要改调用能力 `musicxx.ui.setSongIconRotate`（参数 `value`）。

### `musicxx.ui.songIconWave` 歌曲图波浪

值：`true` / `false`（方形歌曲图下不生效）。设置里改了之后发布，订阅即拿到当前值。**只读**：要改调用能力 `musicxx.ui.setSongIconWave`（参数 `value`）。

### `musicxx.ui.fontFamily` 全局字体

值：字体名，或 `default`（没有自定义字体）。字体换了之后发布，订阅即拿到当前值。

**只读**：切字体调用能力 `musicxx.ui.setFontFamily`（参数 `fontFamily`，空串 / `default` 恢复默认）；要默认字体与字体属主插件这些额外信息调 `musicxx.ui.getFontFamily`。

### `musicxx.theme.available` 可用主题

值：`[{id, title, kind}]`，`kind` = `light` / `night`。主题目录是静态的，应用启动与主题注册后发布一次，订阅即拿到当前值。要看“现在是哪个主题”订阅 `musicxx.theme.current`。

### `musicxx.theme.current` 当前主题

值：主题 id（`mimicryLight` / `mimicryNight`）。显示的主题变了之后发布，订阅即拿到当前值。

**只读**：切主题调用能力 `musicxx.theme.setCurrent`（参数 `id`，只切显示、不改模式、不暂停跟随）或 `musicxx.theme.tempSwitch`（一次操作，会暂停主题跟随）。

### `musicxx.theme.mode` 主题切换模式

值：`light` / `night` / `followSystem` / `followTime` / `followScreenBrightness`（枚举 id，不是本地化文案）。模式改了之后发布，订阅即拿到当前值。

**只读**：要改模式调用能力 `musicxx.theme.setMode`（参数 `mode`），它会写进用户的持久设置并立即按新模式切。
