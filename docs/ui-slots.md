# 槽位（渲染位置）一览

> 本文件由 `tools/gen_contract.dart` 生成，定义来源 `tools/ui_slots.def.json`。

槽位是**主程序预留的渲染位置**。插件往槽位上注册内容（`musicxx.ui.content.register`），
主程序只读缓存渲染，**不会在渲染路径上等插件**。`kind` 决定内容的用法：

| kind | 含义 |
|---|---|
| `item` | 声明项：出现在主程序自己的列表里（`musicxx.ui.item.register`），不含界面树 |
| `insert` | 插入：一个位置可以有多份内容，按 `order` 从上到下排 |
| `replace` | 替换：只认一份内容（`order` 最小），没有内容时画内置界面 |
| `selectable` | 可选样式：插件声明候选，用户在选择列表里选 |
| `pageBackground` | 普通页面背景：按路由自动生效，不用用户选择 |
| `searchType` | 搜索类型：一份内容就是一个类型（注册里的 `title` 是标签名），用户在页面上现选，不落设置 |
| `page` | 插件页面：地址是 `ext://<插件id>/<内容 id>` |
| `overlay` | 插件浮层：`musicxx.ui.overlay.show` / `.showSheet` 打开 |

## 清单

| 槽位 id | kind | 简称 | 位置 | 说明 |
|---|---|---|---|---|
| `musicxx.ui.home.entry` | `item` | `home.entry` | 功能主页 · 入口列表 | 功能主页上的一个入口。data = {title, subtitle?, icon?, action?}；点它时派发 action。 |
| `musicxx.ui.song.action` | `item` | `song.action` | 各处歌曲菜单 · 菜单项 | 歌曲菜单里的一项。data = {title, icon?, action?}；点它时派发 action。 |
| `musicxx.ui.playlist.action` | `item` | `playlist.action` | 歌单菜单 · 菜单项 | 歌单菜单里的一项。data = {title, icon?, action?}；点它时派发 action，参数里带当前歌单。 |
| `musicxx.ui.addsong.entry` | `item` | `addsong.entry` | 「添加歌曲」弹窗 · 来源列表 | 「添加歌曲」弹窗里的一个来源。data = {title, subtitle?, icon?, action?}；点击时参数里带当前歌单（pid / name / songNum）。 |
| `musicxx.ui.audioEffect` | `item` | `audioEffect` | 音效页面 · 音效列表 | 一段音频滤镜。data = {title, subtitle?, filter, pitch?, cpuCost?}；pitch 夹在 0.25~2.0，cpuCost 夹在 1~5（取整）。 |
| `musicxx.ui.playing.background` | `selectable` | `playing.background` | 播放页 · 整页背景（用户选择生效） | 候选样式，靠 content 给出界面。声明项 data = {title, depict?}；界面内容注册在同一个槽位上。 |
| `musicxx.ui.playing.icon` | `selectable` | `playing.icon` | 播放页 · 歌曲图 / 视频（用户选择生效） | 候选样式，靠 content 给出界面。声明项 data = {title, depict?}；界面内容注册在同一个槽位上。 |
| `musicxx.ui.playing.icon.image` | `selectable` | `playing.icon.image` | 播放页 · 歌曲图里的图片内容（用户选择生效） | 只替换歌曲图里的图片内容：点击播放 / 暂停、拖动切歌、形状裁剪、描边与加载提示仍由应用画。声明项 data = {title, depict?}；界面内容注册在同一个槽位上。 |
| `musicxx.ui.playing.visualizer` | `selectable` | `playing.visualizer` | 播放页 · 动效区（用户选择生效） | 候选样式，靠 content 给出界面。声明项 data = {title, depict?}；界面内容注册在同一个槽位上。 |
| `musicxx.ui.page.background` | `pageBackground` | `page.background` | 所有普通页面 · 整页背景（按路由自动生效） | 声明项 data = {title, depict?, routes?}；界面内容注册在同一个槽位上，按 routes 与注册顺序生效。 |
| `musicxx.ui.playing.controlBar` | `replace` | `playing.controlBar` | 播放页 · 整栏（替换内置界面） | 整栏由插件画。同一个槽位只认 order 最小的一份内容，没有内容时画内置控制栏。 |
| `musicxx.ui.playing.controlBar.menu` | `replace` | `playing.controlBar.menu` | 播放页 · 控制栏「菜单」按钮（替换显示） | 只替换这一个按钮的显示；内置按钮的行为是打开「更多」菜单（musicxx.ui.playing.openMenu）。 |
| `musicxx.ui.playing.controlBar.previous` | `replace` | `playing.controlBar.previous` | 播放页 · 控制栏「上一曲」按钮（替换显示） | 只替换这一个按钮的显示；对应能力 musicxx.player.prev。 |
| `musicxx.ui.playing.controlBar.playOrPause` | `replace` | `playing.controlBar.playOrPause` | 播放页 · 控制栏「播放 / 暂停」按钮（替换显示） | 只替换这一个按钮的显示；对应能力 musicxx.player.toggle；画面要跟播放状态走就用变量 musicxx.player.playing。 |
| `musicxx.ui.playing.controlBar.next` | `replace` | `playing.controlBar.next` | 播放页 · 控制栏「下一曲」按钮（替换显示） | 只替换这一个按钮的显示；对应能力 musicxx.player.next。 |
| `musicxx.ui.playing.controlBar.favorite` | `replace` | `playing.controlBar.favorite` | 播放页 · 控制栏「收藏」按钮（替换显示） | 只替换这一个按钮的显示；对应能力 musicxx.songlist.favorite；收藏状态用变量 musicxx.song.favorite。 |
| `musicxx.ui.playing.lyric.top` | `insert` | `playing.lyric.top` | 播放页 · 歌词页 · 列表之上（不随内容滚动） | 同一位置可以有多份内容，按 order 从上到下排列。 |
| `musicxx.ui.playing.lyric.bottom` | `insert` | `playing.lyric.bottom` | 播放页 · 歌词页 · 列表之下（不随内容滚动） | 同一位置可以有多份内容，按 order 从上到下排列。 |
| `musicxx.ui.playing.lyric.header` | `insert` | `playing.lyric.header` | 播放页 · 歌词页 · 列表的 header（跟随内容滚动） | 同一位置可以有多份内容，按 order 从上到下排列。 |
| `musicxx.ui.playing.lyric.footer` | `insert` | `playing.lyric.footer` | 播放页 · 歌词页 · 列表的 footer（跟随内容滚动） | 同一位置可以有多份内容，按 order 从上到下排列。 |
| `musicxx.ui.playing.moreInfo.header` | `insert` | `playing.moreInfo.header` | 播放页 · 更多信息页 · 整页滚动内容的最前面 | 同一位置可以有多份内容，按 order 从上到下排列。 |
| `musicxx.ui.playing.moreInfo.footer` | `insert` | `playing.moreInfo.footer` | 播放页 · 更多信息页 · 整页滚动内容的最后面 | 同一位置可以有多份内容，按 order 从上到下排列。 |
| `musicxx.ui.playing.songlist.top` | `insert` | `playing.songlist.top` | 播放页 · 播放列表页 · 列表之上（不随内容滚动） | 同一位置可以有多份内容，按 order 从上到下排列。 |
| `musicxx.ui.playing.songlist.bottom` | `insert` | `playing.songlist.bottom` | 播放页 · 播放列表页 · 列表之下（不随内容滚动） | 同一位置可以有多份内容，按 order 从上到下排列。 |
| `musicxx.ui.playing.songlist.header` | `insert` | `playing.songlist.header` | 播放页 · 播放列表页 · 列表的 header（跟随内容滚动） | 同一位置可以有多份内容，按 order 从上到下排列。 |
| `musicxx.ui.playing.songlist.footer` | `insert` | `playing.songlist.footer` | 播放页 · 播放列表页 · 列表的 footer（跟随内容滚动） | 同一位置可以有多份内容，按 order 从上到下排列。 |
| `musicxx.ui.search.type` | `searchType` | `search.type` | 搜索页 · 类型标签栏 + 结果区（用户选中哪个就画哪个） | 一份内容 = 一个搜索类型：注册时的 title 是标签名、depict 是说明、order 决定标签顺序；插件在 deps 里写上 scope:musicxx.search.keyword / request，用户点搜索时应用把关键词与请求序号写进作用域变量再让生成函数重画。结果列表建议直接用 musicxx.SongListView 模板（只给 items，行样式与点击播放由应用画）。 |
| `musicxx.ui.page` | `page` | `page` | 插件页面 ext://<插件id>/<内容 id> · 整页（插件自己发起） | 内容 id 就是页面地址里的那一段；作用域变量 scope:musicxx.page.*。 |
| `musicxx.ui.overlay` | `overlay` | `overlay` | 浮层（musicxx.ui.overlay.show / showSheet） · 整层（插件自己发起） | 内容 id 就是能力参数里的 id；作用域变量 scope:musicxx.overlay.*。 |

## 写法

```js
musicxx.ui.content.register({
    id: "row1",
    slot: "playing.songlist.top",       // 简称或全名
    title: "示例行",                    // 给人看的名字, 可选样式槽位的候选列表用它
    depict: "插在播放列表顶部",          // 一句话说明 (可省)
    deps: ["musicxx.song.name"],        // 只订阅用到的变量
    order: 10,
}, function (ctx) {
    return { json: { blocks: [ /* … */ ] } };
});
```

插件也可以声明自己的槽位（`musicxx.ui.slot.define`，`kind` 只允许 `insert` /
`replace`），再在界面里用 `UISlot` 块嵌进去，让别的插件往那里填内容。
