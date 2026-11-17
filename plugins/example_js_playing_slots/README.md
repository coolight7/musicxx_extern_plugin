# example_js_playing_slots —— 播放页位置示例插件

一个示例只教一件事：**插件能把界面放到播放页的哪些位置**。位置就是 UI 类型
（`musicxx.ui.playing.<页面>.<位置>`），写错页面或位置名时插件框架直接拒绝注册，不会静默不显示。

| 位置类别 | UI 类型 | 这个插件放了什么 |
|---|---|---|
| 插入块 | `musicxx.ui.playing.lyric.top` | 歌词页顶部一行提示（`view` 内联视图，可以在设置里关掉） |
| 插入块 | `musicxx.ui.playing.songlist.header` | 播放列表页头部一块说明卡片（`view` 能力） |
| 插入块 | `musicxx.ui.playing.moreInfo.footer` | 更多信息页列表末尾一块提示（`view` 能力） |
| 按钮级替换 | `musicxx.ui.playing.controlBar.menu` | 换掉「更多」按钮，点击执行 `musicxx.ui.playing.openMenu` |
| 按钮级替换 | `musicxx.ui.playing.controlBar.favorite` | 换掉收藏按钮，点击执行 `musicxx.songlist.favorite` |
| 整栏替换 | `musicxx.ui.playing.controlBar` | 整条控制栏（5 个按钮：切播放模式 / 上一曲 / 播放暂停 / 下一曲 / 收藏，按播放状态与当前模式换图标），默认关闭 |
| 渲染槽位 | `musicxx.ui.playing.visualizer` | 音频动效：4 根频谱柱 + 底部光晕（`shader/viz.shaderbundle`） |

两类位置的区别：**插入块**是"在页面里加一块"，可以有多个插件同时插入；
**替换位置**（控制栏整栏、按钮级）是"换掉内置的那一块"，同一个位置**只认一项**（`order` 小的优先），
取不到内容或插件停用时回退内置界面。

---

## 1. 装与跑

```powershell
pwsh -NoProfile -File tools/build_examples.ps1        # 构建（产物在 .native/output/<平台>-<架构>-<配置>/plugins/）
musicxx-cli plugin install "<绝对路径>/example_js_playing_slots-2026_1116_1.zip"
musicxx-cli call probe --plugin example_js_playing_slots
```

装好后打开播放页就能看到插入块与两个替换过的按钮；要试整栏替换与动效，进**音乐主页 →
示例播放页位置 → 设置**页：

- 「用整栏替换」：开关切换 `wholeBar`（开 = 注册整栏项、关 = 注销并回退内置栏）；
- 「歌词页顶部提示」：开 = 注册那一项，关 = `musicxx.ui.slot.unregister`（UI 项没有 `enabled` 字段了）；
- 「音频动效」：「用插件的动效」/「用内置动效」两个按钮切的是渲染槽位 `player.visualizer`
  （内置项 `musicxx.none` = 不显示）。

整栏替换里的第一个按钮是**切播放模式**：图标与文字反映当前模式（读广播快照
`musicxx.state.playlist` 的 `loopMode`），点一下在 单曲循环 → 顺序播放 → 真随机播放 → 单曲循环 之间轮。
动作走 `dispatch` → 插件自己的能力 `cycleLoopMode`：它先调 `musicxx.player.setLoopMode`（不 await），
再把新模式画进返回的视图 —— 画面立刻换，**不需要主程序为位置多加一个参数**。

别处改的播放模式（歌单页、锁屏、远程控制、命令行）也跟得上：插件订阅 `musicxx.state.playlist`，
读到 `loopMode` 真的变了就调 `musicxx.ui.slot.update("wholeBar")` —— 声明不变，主程序用当前声明
重新取一次 `controlBar` 视图（重画计数 `rev` + 1），图标随真实模式换。

设置页的按钮用 `variant: "full"`（大号、宽度铺满）：插件设置页里“一行一个”的按钮就用它。

## 2. 文件怎么分工

六个脚本在同一个 JS 上下文里按 `plugin.yaml` 的 `scripts` 顺序执行，顶层名字必须全局唯一，
注册与定时器统一放在最后一个文件：

| 文件 | 只做什么 |
|---|---|
| `pluginxx_ui_kit.js` / `musicxx_ui_kit.js` | 界面 kit（`pwsh tools/sync_ui_kit.ps1` 复制，不要手改） |
| `slots.js` | 三个插入块 + 两个按钮级项 + 整栏项的注册 / 注销 / 开关，以及主页入口 |
| `visualizer.js` | 音频动效项的数据（bundle、颜色、频谱参数）与"用插件 / 用内置"的切换 |
| `views.js` | 插入块的视图、按钮视图、整栏视图、说明页与设置页 |
| `plugin.js` | 配置读写、能力入口、全部顶层注册 |

`shader/viz.frag` 用 `shader/build_bundle.ps1` 编成 `viz.shaderbundle`；改了着色器就重跑那个脚本。

## 3. 设置项（存插件目录的 `config.json`）

| 键 | 含义 | 默认 |
|---|---|---|
| `wholeBar` | 是否用整栏替换控制栏 | `false` |
| `lyricTip` | 是否显示歌词页顶部那块提示 | `true` |

## 4. 用到的能力

| 能力 | 作用 |
|---|---|
| `probe` | 自检：UI 项数、整栏 / 插入块 / 动效是否注册 |
| `card` / `settings` | 说明页 / 设置页 |
| `songlistHeader` / `moreInfoFooter` | 两个插入块的视图（主程序取不到内容时这一块不显示） |
| `favoriteButton` / `controlBar` | 按钮与整栏的视图（主程序带 `playing` / `favorite` / `hasSong` 等状态参数） |
| `toggleWholeBar` / `toggleLyricTip` | 设置页的开关 |
| `useVisualizer` / `clearVisualizer` | 选插件动效 / 回到内置项 |

## 5. 容易踩的坑

- **插入块改不了列表下标**：主程序把顶部 / 底部那块包在第一行 / 最后一行外面（包在级联动画之外），
  插件只管画自己那块，不要假设自己拿到了整页；
- **按钮图标建议用自己的 svg**（`resource/svg/`），`icon` 只接受插件目录内的相对路径；
- **替换位置只给一块内容**：同一位置注册多项不会叠加，只有 `order` 最小的那一项生效；
- **动效项与内置样式在同一个列表**：不要自己去开关『音乐动效』插件，插件项照样可选；
  没有频谱数据时 `uEnv.z` 是 0，据此区分"静音"与"没数据"；
- **能力处理器必须同步返回**（长活儿交给动作或定时器），处理器里不要 `await` 应用实现的能力。
