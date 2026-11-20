# 变量一览

> 本文件由 `tools/gen_contract.dart` 生成，定义来源 `tools/ui_variables.def.json`。

变量可以写在界面的任何值位置：`{"var": "音乐歌曲名变量"}` 或字符串里的 `{变量 id}`。
内容注册时在 `deps` 里声明用到的变量，主程序只订阅这些：变化时先重新绑定（立刻看到新值），
再让插件重新生成一次结构（串行，只保留最新一次）。

三种前缀：

| 前缀 | 来源 | 可用范围 |
|---|---|---|
| `musicxx.` | 主程序状态 | 任何槽位 |
| `scope:musicxx.` | 主程序状态 | 只在自己的槽位（以及嵌在里面的嵌套槽位） |
| `plugin.` | 插件广播频道 | 任何槽位（可以跨插件） |

## 全局变量

| 变量 id | 类型 | 说明 |
|---|---|---|
| `musicxx.app.foreground` | `bool` | 应用是否在前台 |
| `musicxx.app.lifecycle` | `string` | 应用生命周期状态（ground / background 等） |
| `musicxx.app.version` | `string` | 应用版本号 |
| `musicxx.app.lang` | `string` | 界面语言（zh_Hans / en_US …） |
| `musicxx.app.platform` | `string` | 运行平台（windows / android / ios / linux / macos） |
| `musicxx.theme.night` | `bool` | 是否夜间主题 |
| `musicxx.theme.mode` | `string` | 主题模式（跟随系统 / 亮 / 暗） |
| `musicxx.theme.current` | `string` | 当前主题名 |
| `musicxx.ui.animatedLevel` | `string` | 界面动画等级 |
| `musicxx.ui.songIconRotate` | `bool` | 歌曲图是否旋转 |
| `musicxx.ui.songIconWave` | `bool` | 歌曲图是否随音频起伏 |
| `musicxx.ui.fontFamily` | `string` | 界面字体 |
| `musicxx.player.state` | `string` | 播放状态（idle / playing / paused / completed …） |
| `musicxx.player.playing` | `bool` | 现在是否在播放 |
| `musicxx.player.positionMs` | `number` | 当前播放位置（毫秒） |
| `musicxx.player.durationMs` | `number` | 当前歌曲总时长（毫秒） |
| `musicxx.player.volume` | `number` | 音量（0~100） |
| `musicxx.player.speed` | `number` | 播放速度 |
| `musicxx.player.pitch` | `number` | 音调 |
| `musicxx.song.hasSong` | `bool` | 有没有当前歌曲 |
| `musicxx.song.sid` | `number` | 当前歌曲的会话 id（每次切歌都会变） |
| `musicxx.song.name` | `string` | 当前歌曲名 |
| `musicxx.song.artist` | `string` | 当前歌曲歌手 |
| `musicxx.song.album` | `string` | 当前歌曲专辑 |
| `musicxx.song.subtitle` | `string` | 当前歌曲副标题 |
| `musicxx.song.durationMs` | `number` | 当前歌曲时长（毫秒） |
| `musicxx.song.srcKey` | `string` | 当前歌曲的音源身份 |
| `musicxx.song.favorite` | `bool` | 当前歌曲是否已收藏 |
| `musicxx.song.hasCover` | `bool` | 当前歌曲有没有封面 |
| `musicxx.playlist.length` | `number` | 当前播放队列的长度 |
| `musicxx.playlist.index` | `number` | 当前歌曲在播放队列里的位置（从 0 起） |
| `musicxx.playlist.loopMode` | `string` | 循环模式 |
| `musicxx.playlist.autoPlayMode` | `string` | 自动播放策略 |
| `musicxx.lyric.hasLyric` | `bool` | 当前歌曲有没有歌词 |
| `musicxx.lyric.offsetMs` | `number` | 歌词偏移（毫秒） |
| `musicxx.lyric.line` | `number` | 当前高亮的歌词行下标（没有歌词时为 -1） |
| `musicxx.env.windowWidth` | `number` | 窗口逻辑宽度 |
| `musicxx.env.windowHeight` | `number` | 窗口逻辑高度 |
| `musicxx.env.dpi` | `number` | 窗口所在屏幕的像素比 |
| `musicxx.env.isDesktop` | `bool` | 是不是桌面平台 |
| `musicxx.env.isMobile` | `bool` | 是不是移动平台 |
| `musicxx.spectrum.available` | `bool` | 现在有没有频谱数据 |
| `musicxx.spectrum.level` | `number` | 频谱响度（0~1） |

## 作用域变量

| 变量 id | 类型 | 可用槽位 | 说明 |
|---|---|---|---|
| `scope:musicxx.page.route` | `string` | `musicxx.ui.page`<br>`musicxx.ui.page.background` | 当前插件页面的地址（ext://<插件id>/<内容 id>） |
| `scope:musicxx.page.args` | `json` | `musicxx.ui.page` | 打开插件页面时带的参数 |
| `scope:musicxx.overlay.id` | `string` | `musicxx.ui.overlay` | 浮层的内容 id |
| `scope:musicxx.overlay.args` | `json` | `musicxx.ui.overlay` | 打开浮层时带的参数 |
| `scope:musicxx.songlist.pid` | `string` | `musicxx.ui.page.background` | 歌单内容页打开的歌单 id |
| `scope:musicxx.songlist.name` | `string` | `musicxx.ui.page.background` | 歌单内容页打开的歌单名 |
| `scope:musicxx.songlist.songNum` | `number` | `musicxx.ui.page.background` | 歌单的歌曲数 |
| `scope:musicxx.songlist.sort` | `string` | `musicxx.ui.page.background` | 歌单的排序方式 |
| `scope:musicxx.songlist.isLocal` | `bool` | `musicxx.ui.page.background` | 歌单是不是本地歌单 |
| `scope:musicxx.songlist.cover` | `string` | `musicxx.ui.page.background` | 歌单封面路径（没有封面时为空串） |
| `scope:musicxx.songlist.loading` | `bool` | `musicxx.ui.page.background` | 歌单是否还在加载 |
| `scope:musicxx.slot.colorMain` | `string` | `musicxx.ui.playing.controlBar`<br>`musicxx.ui.playing.controlBar.menu`<br>`musicxx.ui.playing.controlBar.previous`<br>`musicxx.ui.playing.controlBar.playOrPause`<br>`musicxx.ui.playing.controlBar.next`<br>`musicxx.ui.playing.controlBar.favorite` | 播放页前景普通色（#aarrggbb） |
| `scope:musicxx.slot.colorCross` | `string` | `musicxx.ui.playing.controlBar`<br>`musicxx.ui.playing.controlBar.menu`<br>`musicxx.ui.playing.controlBar.previous`<br>`musicxx.ui.playing.controlBar.playOrPause`<br>`musicxx.ui.playing.controlBar.next`<br>`musicxx.ui.playing.controlBar.favorite` | 播放页前景减淡色（#aarrggbb） |
| `scope:musicxx.slot.colorBlock` | `string` | `musicxx.ui.playing.controlBar`<br>`musicxx.ui.playing.controlBar.menu`<br>`musicxx.ui.playing.controlBar.previous`<br>`musicxx.ui.playing.controlBar.playOrPause`<br>`musicxx.ui.playing.controlBar.next`<br>`musicxx.ui.playing.controlBar.favorite` | 播放页前景的块底色（#aarrggbb） |
| `scope:musicxx.slot.args` | `json` | 任何（嵌套槽位） | 外层内容通过 UISlot.args 传给这个嵌套槽位的参数 |

## 求不到值时

- `{"var": …}`：有 `default` 用 `default`，没有就按"这个键没写"处理（字段用自己的缺省值）；
- 字符串里的 `{变量 id}`：换成空串；
- 两种情况都会在日志里记一条 `missing`（同一份内容只记一次）。
