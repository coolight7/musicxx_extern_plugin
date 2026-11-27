# 插件界面：槽位、内容与块

插件**不写 Flutter 代码**：它声明"在哪个位置放什么"，musicxx 负责画。界面只有三样东西：

| 东西 | 是什么 | 谁决定用它 |
|---|---|---|
| **槽位**（slot） | 一个渲染位置：播放页的某个固定位置、控制栏某个按钮、播放页背景 / 歌曲图 / 动效、普通页面背景、插件页面、浮层……清单见 [ui-slots.md](ui-slots.md) | 主程序（按页面、路由或用户选择） |
| **内容**（content） | 插件为某个槽位生成的一棵块树（`{title?, blocks}` 或单个块） | 主程序按槽位 / 路由 / 用户选择挑一份，再让插件生成 |
| **声明项**（item） | 只有数据、没有界面树：主页入口、歌曲 / 歌单菜单项、添加歌曲条目、音效 | 主程序画在自己的列表里 |

- **一个槽位可以有很多份内容**（用内容 id 区分）：插入位置按 `order` 从上到下排；
  替换位置与可选样式槽位只认一份（`order` 小的优先，取不到内容就回到内置界面）。
- **内容在需要时才生成**：注册时只给"名字 + 依赖变量 + 生成函数"；主程序先用上一次的结果，
  同时按 `deps` 把变量当前值交给插件重出一份（串行，只保留最新一次）。
- 不想显示某份内容就**注销**它：内容没有 `enabled` 字段。
- 往插入 / 替换 / 样式槽位注册**声明项**会被插件框架拒绝：那些位置只收内容。

两种插件写法只有 API 名字不同（C++ 用 `uiContentRegister` / `musicxx.capability`，
JS 用 `musicxx.ui.content.register` / `musicxx.capability.register`），**字段与块类型完全一致**，
所以本文对两种插件都适用；语言相关见 [plugin-native-api.md](plugin-native-api.md)（C++）与
[plugin-js-api.md](plugin-js-api.md)（JS）。第一次写插件、想知道从哪一步开始，先看
[plugin-guide.md](plugin-guide.md)。

---

> **术语约定**：本文里的**主程序**指 musicxx（Flutter/Dart 业务层）；外置插件的 C++ 框架层叫
> **插件框架**；插件分**动态库插件**与 **JS 插件**。

### 1.0 契约冻结（下沉到 `cxx_pluginxx` 期间）

这一层的注册表与内容缓存正在从本包搬到子模块 `cxx_pluginxx`（`pluginxx/api/ui.h` +
`pluginxx/host/ui_content.h` + `pluginxx/host/ui_slots.h`），两个项目共用同一份实现。搬的时候：

- **块树字段、槽位形态、变量 id 一律不改名、不重排**（源头仍是
  `cxx_pluginxx_ui/schema/ui.def.json`、`tools/ui_slots.def.json`、`tools/ui_variables.def.json`）；
- 要改的点先写进本文 §7 的清单，一次改完；不允许"搬一半顺手改名"，
  否则插件作者要按不同版本改两遍；
- 不变的规则：渲染只读缓存、`rev` 丢弃晚到结果、同一份内容同时最多一个处理中生成请求、
  没有有效内容就画内置界面。宿主线程永不同步等插件线程。

## 1. 值、内容与刷新

### 1.1 值：字面量、变量与值表达式

界面的每个**值位置**（文本、尺寸、颜色、进度、开关……）可以写三种东西：

| 写法 | 例子 | 说明 |
|---|---|---|
| 字面量 | `"标题"` / `40` / `"#ff8800"` / `true` | 直接用 |
| 变量绑定 | `{"var": "musicxx.song.name"}` / `"第 {musicxx.playlist.index} 首"` | 解析前替换一次；清单见 [ui-variables.md](ui-variables.md) |
| 值表达式 | `{"kind":"smooth","attackMs":20,"releaseMs":260,"of":{"kind":"source","name":"musicxx.spectrum.level"}}` | 每帧求值（尺寸 / 颜色 / 进度 / 文本都能"算"），见 §2.3.1 |

变量有三种前缀，**能用在哪里由前缀决定**：

| 前缀 | 来源 | 可用范围 |
|---|---|---|
| `musicxx.*` | 主程序状态（歌曲、播放、主题、界面、环境量……） | 任何槽位 |
| `scope:musicxx.*` | 当前槽位的上下文（页面路由 / 参数、浮层参数、歌单信息、播放页前景三色、嵌套槽位参数） | 只有那个槽位（以及嵌在它里面的嵌套槽位） |
| `plugin.<插件id>.*` | 插件广播频道（可以跨插件） | 任何槽位 |

取不到值时：`{"var":…}` 有 `default` 用 `default`，否则按"这个键没写"处理；字符串里的
`{…}` 换成空串；两种情况都在日志里记一条 `missing`。

### 1.2 内容：注册、生成、刷新

```js
// JS：注册一份内容（名字与生成函数）。id 用短名，框架补成 plugin.<自己>.<id>
musicxx.ui.content.register({
    id: "songlistHeader",                        // 内容 id（地址 / 更新都用它）
    slot: "musicxx.ui.playing.songlist.header",  // 槽位（简称 playing.songlist.header 也行）
    title: "播放列表页顶部",                      // 给人看的名字（可选样式槽位必须给）
    depict: "一行当前歌曲 + 打开更多菜单",          // 一句话说明（可选）
    order: 10,                                   // 同一槽位里的排序
    deps: ["musicxx.song.name"],                 // 用到的变量（变化时重生成）
    // routes: ["/songs"],                        // 只对普通页面背景有意义（注册时给）
    minIntervalMs: 0,                             // 重生成的最短间隔（0 = 不限）
}, function (ctx) {
    // ctx = 声明过的依赖变量 → 当前值；返回一棵块树
    return { blocks: [ /* … 块 … */ ] };
});
```

```cpp
// C++：同一个注册，生成函数是同步回调
int32_t onGenerateHeader(const PluginxxHost *host, const PluginxxStringView *ctxJson,
                         PluginxxString *out);
uiContentRegister("songlistHeader", MUSICXX_PLUGIN_UI_SLOT_PLAYING_SONGLIST_HEADER,
                  R"(["musicxx.song.name"])", &onGenerateHeader, 10, 0,
                  "播放列表页顶部", "一行当前歌曲 + 打开更多菜单");
```

规则（插件框架在注册时校验，写错直接失败，不会等到渲染时才发现）：

- `id` 用**短名**，框架补成 `plugin.<插件id>.<id>`；写全名时必须属于本插件命名空间（冒充别人返回 `-6`）；
- `slot` 必须是已知槽位或**自己声明的槽位**（`musicxx.ui.slot.define`，见 §1.10），未知槽位返回 `-4`；
- `deps` 是变量 id 数组（`scope:` 开头的也要写进去），写错返回 `-1`；
- 同一个 `id` 再注册 = **覆盖**这一份（保留排序位置）；
- 内容数量与单份内容大小都不设上限；
- 变化会推 `musicxx.ui.contents` 事件（载荷带该插件的全部内容）。

生成函数的约定：

- **同步、要快**：只做"把 `ctx` 变成 JSON"这一件事。要等网络 / 文件就先返回"加载中"，拿到结果再
  `musicxx.ui.content.push(id, {json, data})`；
- 抛异常 / 返回非对象 → 这次生成失败，主程序保留上一份内容；
- 返回 `{json, data}` 时 `json` 是块树，`data` 是给自己用的附加数据（例如歌曲图槽位的
  `{"keepSpace": true, "fill": false}`）；直接返回块树也可以。

**什么时候重生成**：

```js
musicxx.ui.slot.update("musicxx.ui.playing.controlBar", ["wholeBar"]); // 指定内容
musicxx.ui.slot.update("musicxx.ui.playing.songlist.header");            // 不写 ids = 整个槽位
musicxx.ui.content.push("wholeBar", { json: { blocks: [] } });           // 直接推一份
```

- `deps` 里的变量变了 → 主程序先重新绑定（界面立刻看到新值），再让生成函数重出一份；
- 动作改完状态自己说一声（`slot.update`），**动作不返回视图**；
  **动作里的能力调用是异步的**：处理器里立刻 `slot.update` 画到的是动作还没落地时的状态
  （槽位镜像 `musicxx.state.uiSlots` 里的 `selectedId` / `itemId` 要等这次往返走完才更新），
  页面上就会“点了有反应、但高亮下一次点击才变”。要显示切换结果的，等结果（成功或失败）
  回来再 `slot.update` 一次；定时器 / 钩子里发起的动作没有页面，不用刷；
- 运行期改影响帧调度 / 分辨率的字段（`maxFps` / `resolutionScale` / `animate`）重新生成一次就生效；
- 刷新没有时间节流，但同一份内容同时最多一个处理中请求，期间的变化只保留最新一次；
  需要节流就写 `minIntervalMs`。

**列表与选择**：`musicxx.ui.slot.list({slot})` 看某个可选样式槽位的候选，
`musicxx.ui.slot.current({slot})` 看现在生效的是哪一份，`musicxx.ui.slot.set(id, {slot})`
换一份（`musicxx.previous` = 交还控制权，`musicxx.default` = 回到默认值）。
用户选中之前候选列表就要显示名字，所以可选样式槽位的内容**必须给 `title`**。

### 1.3 声明项（item）

声明项只有数据，由主程序画在自己的界面里。**只对这些类型有效**：

| 类型（全名） | `data` 字段 | 渲染位置 |
|---|---|---|
| `musicxx.ui.home.entry` | `title`（必填）、`subtitle?`、`icon?`、`action?` | 音乐主页的入口列表（排在内置入口之后，用户可以在主页排序里拖动） |
| `musicxx.ui.song.action` | `title`（必填）、`icon?`、`action?` | 歌曲列表项的「歌曲菜单」 |
| `musicxx.ui.playlist.action` | `title`（必填）、`icon?`、`action?` | 歌单页的列表菜单（动作参数带 `pid` / `name` / `songNum`） |
| `musicxx.ui.addsong.entry` | `title`（必填）、`subtitle?`、`icon?`、`action?` | 「添加歌曲」弹窗（常见写法：`call` 动作指向 `musicxx.ui.overlay.show`） |
| `musicxx.ui.audioEffect` | `title`（必填）、`subtitle?` / `depict?`、`filter`（必填）、`pitch?`、`cpuCost?` | 音效页面（见 §1.8） |

```js
musicxx.ui.item.register({
    name: "card",
    type: "home.entry",          // 简称或全名都行
    order: 100,
    data: { title: "我的入口", subtitle: "示例", action: { kind: "route", route: "ext://my_plugin/card" } },
});
musicxx.ui.item.unregister("card");
musicxx.ui.item.entries();       // 本插件已注册的声明项（脚本侧副本）
```

`icon` 写**插件自己带的 svg**（相对插件目录的路径，见 §1.6），**不能**写主程序的图标名。
**没有 `data.enabled`**：不想生效就注销，重新显示再注册。

### 1.4 动作（`data.action` / 块里的 `action`）

动作写法与块里的动作**完全一致**：

| 写法 | 字段 | 行为 |
|---|---|---|
| `"openSettings"` | — | **字符串短写**，等于 `call`：调本插件的能力 |
| `{"kind":"call"}` | `name`、`args?`、`timeoutMs?` | 调能力：短名 = 自己，`plugin.<别的插件>.<名字>` = 跨插件，`musicxx.*` = 应用 |
| `{"kind":"route"}` | `route: "ext://<插件id>/<内容 id>"` | 打开插件页面（可以指向任意插件） |
| `{"kind":"none"}` / 不写 | — | 纯展示，点了没有动作 |

- 注册时校验：`route` 必须 `ext://` 开头且带插件 id 与内容 id；`call` 必须有非空 `name`；
  `musicxx.*` 必须是官方能力名；
- **动作不返回视图**：改完状态用 `musicxx.ui.slot.update` / `content.push` 刷新；
  动作里的能力调用是异步的，**要等结果回来再刷新一次**（见 §1.2），否则页面上显示的还是切换前的状态；
- 动作参数里主程序会补上下文：歌曲菜单补 `sid` / `name` / `artist`，歌单菜单补 `pid` / `name` / `songNum`
  （插件自己写的 `args` 保留，冲突时以主程序补的为准）；
- `timeoutMs` 只对需要等结果的调用有意义（默认不限制，见 [plugin-capabilities.md](plugin-capabilities.md)）。

### 1.5 声明项的渲染注意（写插件前先确认）

可用的渲染点：`home.entry` 的 `subtitle` / `icon` 都会画出来、`playlist.action`
在歌单页的列表菜单里有位置。仍要注意的是：

- 入口与菜单项的图标只能来自插件自己（相对插件目录的路径，见 §1.6），写主程序图标名等于没写；
- 主页入口的文字是单行省略（标题、副标题各一行），别写长句；
- 想确认自己注册了哪些项：`musicxx.ui.slot.entries()`（JS）/ `uiSlotEntries()`（C++）。

### 1.6 图标：插件自己带 svg

插件要图标（主页入口与菜单项的 `icon`、页面里的 `Button.icon` / `Icon.name`）就写一条
**相对插件目录**的 svg 路径：

```text
my_plugin/
  plugin.yaml
  plugin.js                    # 或动态库
  resource/svg/logo.svg        # 声明处写 icon: "resource/svg/logo.svg"
  assets/icons/tip.svg         # 也可以按自己的目录结构放，写 icon: "assets/icons/tip.svg"
```

- 路径相对**插件目录**，可省 `.svg` 后缀（`"resource/svg/logo"` 与 `"resource/svg/logo.svg"`
  等价）；用 `/` 分隔，允许子目录；
- **`resource/svg/` 只是推荐位置**（示例插件都放这里，便于插件目录一眼看懂），不强制；
- 只认 svg：写成别的后缀（`logo.png`）按"没有图标"处理；
- 写法不合法（空路径、绝对路径、`..`、`\`）同样按"没有图标"处理，不会去插件目录外面找
  文件，也不会报错；- **不能引用主程序（musicxx）自己的图标名**：`host.info().ui` 里没有 `icons` 清单（musicxx 不发布），
  写主程序的 svg 名字只会在插件目录里找一个同名文件，找不到就按"没有图标"处理 ——
  主页入口 / 菜单项退回默认图标，页面里的 `Icon` 块退回 `glyph` 文本；`Button` 没写
  `label` 时用 `glyph` 当文字（终端 / 纯文本客户端也看得到），写了 `icon` 就只留一个图标位
  （框架只拼路径、不替插件判文件在不在，文件缺失时按钮里就是一个空位）；
- 路径由**框架**拼接：插件在 JSON 里只写相对路径，客户端拿到内容、构造界面时才拼上自己的插件
  目录（所以插件目录换到别的地方、同一个插件包在哪台机器上装，声明都不用改）；
- 颜色：主页入口 / 菜单项按 svg 自己的颜色画；页面里的 `Icon` 与 `Button` 会按 `color`
  **单色渲染**（`BlendMode.srcIn`），这两个位置用单色的形状图标（不带渐变、不带多色）更合适；
- 图标随插件目录一起分发（打包时把图标目录放进插件包；动态库插件在 `CMakeLists.txt` 里把
  资源目录写进 `ASSETS`，见 [plugin-native-api.md](plugin-native-api.md)）。

---

### 1.7 插入位置：在播放页的固定位置放一块自己的界面

播放页的几个子页面留了固定位置，插件可以在那里插一块自己声明的界面（与插件页面同一套块，不写 Flutter 代码）。
**一个位置就是一个 UI 类型**，类型名统一是 `musicxx.ui.playing.<页面>.<位置>`，注册时把类型写成位置本身，
`data` 里不再需要 `slot`：

| 类型（全名） | 页面 | 位置 | 是否跟随内容滚动 |
|---|---|---|---|
| `musicxx.ui.playing.lyric.top` | 歌词页 | 顶部（列表之上） | 否 |
| `musicxx.ui.playing.lyric.bottom` | 歌词页 | 底部（列表之下） | 否 |
| `musicxx.ui.playing.lyric.header` | 歌词页 | 列表内顶部（header） | **是** |
| `musicxx.ui.playing.lyric.footer` | 歌词页 | 列表内底部（footer） | **是** |
| `musicxx.ui.playing.moreInfo.header` | 更多信息页 | 内容开头 | 是（整页都在滚） |
| `musicxx.ui.playing.moreInfo.footer` | 更多信息页 | 内容末尾 | 是（整页都在滚） |
| `musicxx.ui.playing.songlist.top` | 播放列表页 | 顶部（列表之上） | 否 |
| `musicxx.ui.playing.songlist.bottom` | 播放列表页 | 底部（列表之下） | 否 |
| `musicxx.ui.playing.songlist.header` | 播放列表页 | 列表内顶部（header） | **是** |
| `musicxx.ui.playing.songlist.footer` | 播放列表页 | 列表内底部（footer） | **是** |

```js
// 在歌词页顶部放一行小字：动作（call / route）与页面里的块完全一样
musicxx.ui.content.register({
    id: "lyricTip",
    slot: "musicxx.ui.playing.lyric.top",
    title: "歌词页顶部提示",
    order: 10,                     // 同一位置多份内容时小者靠前
}, function () {
    return { blocks: [
        { kind: "Text", text: "歌词由插件提供", type: "caption", color: "hint" },
    ] };
});

// 要跟着状态换内容的，把变量写进 deps，生成函数里按 ctx 出不同的块
musicxx.ui.content.register({
    id: "playlistFilter",
    slot: "musicxx.ui.playing.songlist.header",
    title: "播放列表页顶部",
    deps: ["musicxx.song.name"],
}, function (ctx) {
    return { blocks: [
        { kind: "Text", text: "当前播放：" + (ctx["musicxx.song.name"] || "无") },
    ] };
});
```

```cpp
// C++：同一个注册，生成函数返回块树
uiContentRegister("lyricTip", MUSICXX_PLUGIN_UI_SLOT_PLAYING_LYRIC_TOP, "[]",
                  &onGenerateLyricTip, 10, 0, "歌词页顶部提示");
```

规则：

- 同一位置可以注册**多份内容**，按内容注册的 `order` 与注册顺序从上到下排列（不是替换关系）；
- 生成函数返回 `{blocks: […]}`（或单块、或 `{json, data}`）；
- 插入位置不需要 `title`（不画在任何列表里），给了也不影响渲染；
- **没有 `enabled`**：不想显示就 `musicxx.ui.content.unregister(id)`（重新显示再注册回来，会排到最后）；
- **插入块自己带页面留白**：插进去的地方页面已经给过边距，块只画自己的内容，要留白在块里写
  `padding` / `gap`；
- `top` / `bottom` 是**固定区域**（不随列表滚动）：放一两行、不要放很长的内容（窗口矮时会挤小中间的内容区）；
  长内容用 `header` / `footer`，它们跟着列表一起滚（歌词页与播放列表页都有 `header` 与 `footer`，
  更多信息页的 `header` / `footer` 就是内容的首尾）；
- **类型名就是位置**：写错一个字母（拼错页面或位置）时插件框架**直接拒绝注册**（未知 UI 类型，
  和把 `home.entry` 拼错一样），不会静默不显示；
- 取不到内容（不是内容注册、返回的东西解析不出块）→ 这一块不渲染，不影响同一位置的其它项与页面本身；
  要刷新就推一份新的（`content.push`，或 `slot.update("<槽位>", ["<内容 id>"])`）；
  真要改声明（换标题 / 换 `order`）就用 `register` 覆盖。
- 插入块里的逐帧动画用 `musicxx.AnimatedBuilder`（与页面里的写法一样，见 §2.3.2）；播放页的频谱来源
  `musicxx.spectrum.*` 在这些块里也能用。

---

### 1.8 播放页控制按钮栏：换掉整栏或换掉某一个按钮

播放页底部那一排控制按钮默认是「菜单 / 上一曲 / 播放暂停 / 下一曲 / 收藏」。插件可以**替换**它，
分两层，按需要选一层就够：

| 类型（全名） | 换掉什么 | 内置按钮原本做什么（插件要自己写这些动作） |
|---|---|---|
| `musicxx.ui.playing.controlBar` | **整块**控制按钮栏 | 五个按钮全部由插件摆、由插件写动作 |
| `musicxx.ui.playing.controlBar.menu` | 只换「菜单」按钮的显示 | 打开「更多」菜单：`musicxx.ui.playing.openMenu` |
| `musicxx.ui.playing.controlBar.previous` | 只换「上一曲」按钮的显示 | `musicxx.player.prev` |
| `musicxx.ui.playing.controlBar.playOrPause` | 只换「播放 / 暂停」按钮的显示 | `musicxx.player.toggle` |
| `musicxx.ui.playing.controlBar.next` | 只换「下一曲」按钮的显示 | `musicxx.player.next` |
| `musicxx.ui.playing.controlBar.favorite` | 只换「收藏」按钮的显示 | `musicxx.songlist.favorite` |

控制栏位置也是一份**内容**：`slot` 写 `musicxx.ui.playing.controlBar`（整栏）或
`musicxx.ui.playing.controlBar.<按钮>`（单个按钮），生成函数返回那块界面。

```js
// 只换「播放 / 暂停」这一个按钮：图标用插件自己的 svg，动作调官方能力
// 跟着播放状态换图标 = 声明 deps，播放状态一变主程序就让生成函数重出一份
musicxx.ui.content.register({
    id: "playBtn",
    slot: "musicxx.ui.playing.controlBar.playOrPause",
    title: "播放暂停按钮",
    deps: ["musicxx.player.playing"],
}, function (ctx) {
    const playing = ctx["musicxx.player.playing"] === true;
    return { blocks: [
        { kind: "Button", label: "", variant: "simple",
          icon: playing ? "resource/svg/pause.svg" : "resource/svg/play.svg",
          action: { kind: "call", name: "musicxx.player.toggle" } },
    ] };
});

// 整栏都由自己画：五个按钮，动作都调官方能力
musicxx.ui.content.register({
    id: "miniBar",
    slot: "musicxx.ui.playing.controlBar",
    title: "整条控制栏",
    deps: ["musicxx.player.playing", "musicxx.song.favorite", "musicxx.song.hasSong"],
}, miniBarView);
```

```cpp
// 只换「收藏」按钮：动作调官方能力 musicxx.songlist.favorite
uiContentRegister("favBtn", MUSICXX_PLUGIN_UI_SLOT_PLAYING_CONTROL_BAR_FAVORITE,
                  R"(["musicxx.song.favorite"])", &onGenerateFavButton, 0, 0,
                  "收藏按钮");
```

规则：

- **替换，不是插入**：有插件注册整栏时，内置的五个按钮不再显示，五个按钮级位置也**不再参与渲染**；
  只有整栏没有内容时才按按钮级位置逐个替换（没注册的那几个按钮照旧是内置的）。
- **同一个位置只认一份内容**：`order` 小的优先，同 `order` 按注册顺序。多个插件注册同一位置时，
  只有第一份生效，其余的**不报错也不显示**（插件管理页能看到原因）。
- **取不到内容就回退内置界面**：生成失败、插件被停用、内容还没到 —— 这些情况下画回内置按钮，
  用户仍然能暂停 / 切歌 / 收藏。日志里留一条原因（同一个位置只记一次）。
- **点击由插件自己的块接收**：主程序不在外面再包一层手势（否则一次点击会触发两次）。可点的块有两种：
  `Button`（自带按钮样式，`icon` 写插件自己的 svg），或者任何写了 `action` 的块（`Icon` / `Text` /
  `SizedBox` 这类，主程序会给它包一层点击）；动作写 `call` 调官方能力。
- **尺寸**：主程序只给高度（与内置栏一样，由页面按窗口大小决定），宽度由插件这一块自己占；
  整栏替换时建议用 `Row` + `mainAxisAlignment: "spaceBetween"`（kit 里的 `kit.row` 一样），
  与内置栏看起来一致。单个按钮的高低由它自己的 `size` 定（不写就是默认大小）—— 要做到"中间那个
  播放 / 暂停更大"，给它 `size: 256`（默认 `128u` 的两倍），整排高度跟着最高的按钮走。
- **状态用变量读，不要另设参数**：控制栏位置能用这些变量（写进 `deps` 才会重生成）：

  | 变量 | 含义 |
  |---|---|
  | `musicxx.player.playing` | 当前是否在播放 |
  | `musicxx.song.favorite` | 当前歌曲是否已在「我喜欢」里 |
  | `musicxx.song.hasSong` | 现在有没有在播的歌 |
  | `musicxx.song.name` / `musicxx.song.artist` / … | 当前歌曲的其他字段（见 [ui-variables.md](ui-variables.md)） |
  | `scope:musicxx.slot.colorMain` / `colorCross` / `colorBlock` | 播放页前景三色（`#aarrggbb`） |

  其余状态（播放模式、队列、设置项…）用 `musicxx.broadcast.read("musicxx.state.playlist")` 这类同步读，
  或者在自己的动作里改完状态后 `musicxx.ui.slot.update(slot, [id])` 让内容重出一份。
- **控制栏里的 `Button` 自己就是跟三色的**：在播放页上，没写 `color` 的按钮按页面前景三色画
  （内容色取普通色、底色取块色、禁用取减淡色），与内置按钮一致，换主题 / 换前景色时跟着变；
  写了 `color` 的按钮以描述为准。所以只有着色器、自绘块这类需要原始色值的地方才需要读三色变量。
- 插件卸载 / 停用 / 关闭『拟声++』后立刻回到内置界面；用户也可以在插件管理里停用插件来还原。

> 整栏替换之后，内置的「更多」菜单（选歌词 / 投屏 / 加入歌单 / 播放模式 / 沉浸模式…）就没有入口了。
> 两个办法：在自己画的栏里放一个按钮调 `musicxx.ui.playing.openMenu`；或者提醒用户把播放页右上角的
> 快捷功能设成「更多」（在那个按钮上长按 / 桌面右键 → 快捷功能 → 更多），它弹出的菜单与内置菜单按钮完全一样。

### 1.9 音效：把自己的音频滤镜加进音效页面

注册一项 `musicxx.ui.audioEffect`，它就会出现在『音效』插件的音效页面里（排在全部内置预设之后）。
用户选中后，这一段滤镜链与内置音效完全一样地生效：写进 mpv `af` 里应用自己的「音效」段。

```json
{
  "title": "低音炮",
  "subtitle": "加强 60~140 Hz",
  "filter": "lavfi=[volume=-3dB,equalizer=f=80:width_type=q:width=1:g=8,alimiter=limit=0.89:level=false:latency=false]",
  "pitch": 1.0,
  "cpuCost": 3
}
```

| 字段 | 必填 | 说明 |
|---|---|---|
| `title` | 是 | 音效名，音效页面与「已启用」提示用；为空时退回 UI 项的短名 |
| `subtitle` | 否 | 说明（页面第二行）；也接受 `depict`（与渲染槽位一致，同时给时 `depict` 优先） |
| `filter` | 是 | 完整的一段 mpv `af` 串，原样写进播放器。多条滤镜写成一条 `lavfi=[a,b,c]` 图；最后一条常常是 `alimiter=...`。**不写成图的话 mpv 会给每一条滤镜单独建一个滤镜图、各起一组「CPU 核数」的线程（全程空闲）** |
| `pitch` | 否 | 变调类音效的音调（0.25~2.0）。给了它就等于「小风车」那种音效：选中时会把用户自定义的音调复位 |
| `cpuCost` | 否 | 1~5 的性能负担（1 最省，默认 3），只用于页面上提示用户 |

没有 `enabled` 字段：音效项只描述“效果是什么”，**是否可用（『音效』插件有没有启用）、当前选中的是
哪一项，都由主程序渲染音效列表时判断**（与内置预设同一套逻辑）；不想出现在列表里就注销这一项。

`filter` 与 `pitch` 至少要有一个（只有 `pitch` 也可以，那就是纯变调），否则这一项会被跳过。

C++ 插件：

```cpp
uiItemRegister("bass", MUSICXX_PLUGIN_UI_SLOT_AUDIO_EFFECT,
    R"({"title":"低音炮","subtitle":"加强 60~140 Hz","cpuCost":3,
        "filter":"lavfi=[volume=-3dB,equalizer=f=80:width_type=q:width=1:g=8,alimiter=limit=0.89:level=false:latency=false]"})");
```

JS 插件：

```js
musicxx.ui.item.register({
  name: "bass",
  type: "audio.effect",          // 也可以写全名 musicxx.ui.audioEffect
  title: "低音炮",
  subtitle: "加强 60~140 Hz",
  cpuCost: 3,
  filter:
    "lavfi=[volume=-3dB,equalizer=f=80:width_type=q:width=1:g=8," +
    "alimiter=limit=0.89:level=false:latency=false]",
});
```

规则与注意：

- **与均衡器、自定义音调互斥**：开均衡器会关掉音效；带 `pitch` 的音效会先把用户音调复位。
  这与内置音效完全一样，插件不需要自己处理。
- **滤镜名必须是这份构建里真的存在的**（mpv 的 `af` 与 mediaxx 自带的 ffmpeg）：写错时这一项会失效
  （mpv 拒绝写入），不会影响应用自己的均衡器 / 音调段。写之前先看包内
  [plugin-api.md](plugin-api.md) 里列出的可用滤镜，或在应用的「播放内核信息」里查。
- **不要用 `lavfi-complex`**：那是另一条音频链路，运行期清理它会把音频输出弄坏；音效一律留在 `af` 里
  （需要图语法就写 `lavfi=[...]`，与内置的「人声消减」一样）。
- **逐帧表达式里的时间要挡住 NaN**：`volume=volume='0.5+0.5*cos(2*PI*0.1*t)':eval=frame` 这种写法，
  `t` 在**没有时间戳的帧**上是 NaN（音频链刚建立的第一帧就会碰到），ffmpeg 会记一条
  `Invalid value NaN for volume, setting to 0` 并把那一帧静音。写 `if(isnan(t),0,t)` 代替 `t`
  （内置的「3D环绕声」就是这么写的）。
- **延迟用 `aecho`，不要用 `adelay`**：这份 ffmpeg 的 `adelay` 在开头输出的是未初始化样本（实测 19 ms
  的延迟会出来 1824 个 NaN，后面的限幅器也带不走）；`aecho=in_gain=0:out_gain=1:delays=N:decays=1`
  是干净的延迟，开头是静音。
- **逐样本表达式很贵**：一条 `aeval` 的逐样本表达式处理 20 s 音频要 0.4~0.5 s，而
  `volume=...:eval=frame`（逐帧）只要 0.06 s（纯解码 0.07 s）。慢速、只需要一个增益的 LFO 用逐帧写法，
  别用 `aeval`。
- **自己写 LFO 增益时别让总音量跟着起伏**：两条声道用互补增益（`0.5+0.5cos` / `0.5-0.5cos`）做摆动时，
  中间相位两边同时变轻；交叉淡化也要用等功率权重（干 `cos`、暗 `sin`），按振幅相加会叠出鼓包。
  内置的「3D环绕声」在 `apulsator` 后面跟了一条功率补偿，可以照抄。
- **插件停用 / 卸载 / 关闭『拟声++』后**，这一项从列表消失；如果用户当时选的是它，会回退成「未启用音效」
  （不会把一段已经不存在的滤镜留在播放器上）。重新启用后需要用户再选一次。
- 一个插件可以注册多个音效，按 UI 项的 `order` 与注册顺序排列；`title` 是用户看到的唯一标识，
  改短名等于换了一个音效（旧选择失效）。
- 音效是“全局播放”的：选中后一直生效到用户取消或改用别的音效，不区分歌曲。

---

### 1.10 嵌套槽位：在自己的内容里再放一个槽位（`UISlot`）

插件可以在界面里**再放一个槽位**，让另一份内容显示在这个位置上：位置既可以是应用已有的，
也可以是自己声明的。块写法：

```json
{
  "kind": "UISlot",
  "slot": "plugin.other_plugin.ui.toolbar",
  "args": { "compact": true },
  "fallback": { "kind": "Text", "text": "对方插件没装", "type": "caption" }
}
```

声明自己的槽位（只有 `insert` / `replace` 两种 kind，名字落在自己的命名空间里）：

```js
musicxx.ui.slot.define({ id: "toolbar", kind: "insert" });   // plugin.<自己>.ui.toolbar
musicxx.ui.slot.undefine("toolbar");
```

短名会自动补成 `plugin.<自己>.ui.<名字>`（写自己的全名也行）；只改自己的那一份，
拿别人的全名去 `define` / `undefine` 会被拒。内容注册时 `slot` 写全名或短名都行。

- `args` 进变量 `scope:musicxx.slot.args`，只在**这个嵌套槽位里**的内容可见（`deps` 要声明它），
  这就是"父内容给子内容传参"的唯一办法；
- 深度上限 4；自己嵌自己、形成环、超出深度都画 `fallback`；
- 槽位被撤销 / 里面没有任何内容 / 内容还没生成 → 画 `fallback`（不写 `fallback` 就留空）；
- 嵌进来的内容照常绑变量、照常按 `deps` 重生成；它拿不到外层内容的局部动画通道，
  需要逐帧就自己套 `musicxx.AnimatedBuilder`；
- 自查：`musicxx.ui.slots()` 返回应用的槽位清单 + 自己声明的槽位 + 可绑定的变量。

两个插件互相嵌套的完整例子见 `plugins/example_js_ui_nested`（声明槽位的 `provider` 与往里面放内容的 `consumer`）。

### 1.11 搜索页的搜索类型（`musicxx.ui.search.type`）

搜索页顶部那排"本地歌曲 / B站 / 网盘……"标签，插件也能往里加：**在这个槽位上注册一份内容，就是一个搜索类型**。
注册里的 `title` 是标签名，`order` 决定它排在内置类型之后的哪个位置。用户切到这个标签、输入关键词、点搜索，
主程序把已提交的关键词写进作用域变量，再让插件生成结果界面。

```js
// JS：一个自己的搜索类型
musicxx.ui.content.register({
    id: "cloudSearch",
    slot: "musicxx.ui.search.type",
    title: "我的曲库",                  // 标签名
    depict: "搜索插件自己的曲库",        // 一句话说明（标签栏不画，自省能看到）
    order: 10,                        // 内置类型之后按它排
    deps: ["scope:musicxx.search.keyword", "scope:musicxx.search.request"],
}, function (ctx) {
    const keyword = ctx["scope:musicxx.search.keyword"] || "";
    if (!keyword) {
        return { blocks: [{ kind: "Text", text: "输入关键词后点搜索", type: "hint" }] };
    }
    const songs = cloudSearch(keyword);        // 插件自己取数据
    return { blocks: [{
        kind: "musicxx.SongListView",
        emptyText: "没有找到",
        items: songs.map((s) => ({
            id: s.id,
            title: s.title,
            subtitle: s.artist,
            data: { song: { type: "plugin.my_plugin.audio", src: s.src,
                            artist: s.artist, durationMs: s.durationMs } },
        })),
    }] };
});
```

```cpp
// C++：同一个注册
uiContentRegister("cloudSearch", MUSICXX_PLUGIN_UI_SLOT_SEARCH_TYPE,
                  R"(["scope:musicxx.search.keyword","scope:musicxx.search.request"])",
                  &onGenerateSearch, 10, 0, "我的曲库", "搜索插件自己的曲库");
```

三个作用域变量只在这个槽位里可用；`deps` 里写到的才会传进生成函数：

| 变量 | 类型 | 什么时候变 |
|---|---|---|
| `scope:musicxx.search.keyword` | 字符串 | 点搜索时写入已提交的关键词；从没搜过是空串 |
| `scope:musicxx.search.request` | 数字 | 每点一次搜索 +1（同一个词再点一次也会变） |
| `scope:musicxx.search.type` | 字符串 | 这份内容的短 id（一个插件注册多个类型时用来分辨） |

**什么时候生成内容**

- 用户切到这个标签时生成一次（这时关键词还是空串，正好用来画"请输入关键词"）；
- 之后只在上面这些变量变化、或插件自己调 `musicxx.ui.slot.update("musicxx.ui.search.type")` 时重生成；
- **输入框打字不触发**（给的是"点了搜索"的那个词）：插件不用自己做防抖，也不要在生成函数里等网络。
  数据要慢慢取，就先返回一份"正在搜索"，取到之后再调一次 `slot.update` 重画。

**结果直接写 `musicxx.SongListView`**

条目只要给 `data.song`（音源类型 + 资源 id，写法与 `musicxx.DirListView` 的歌曲项一样）：

```json
{
  "kind": "musicxx.SongListView",
  "emptyText": "没有找到",
  "items": [
    { "id": "a1", "title": "稻香", "subtitle": "周杰伦",
      "data": { "song": { "type": "plugin.my_plugin.audio", "src": "music/稻香.flac",
                          "artist": "周杰伦", "durationMs": 223000 } } }
  ],
  "itemMenu": [ { "title": "加入我的收藏", "action": "addFavorite" } ]
}
```

行样式（封面、歌名歌手、时长、当前播放的选中态）、点行播放（整份结果当作播放队列）、长按 / 右键的内置歌曲菜单、
空列表提示都由主程序画；`itemMenu` 的项排在内置菜单之后，点了派发给插件（参数带 `listId` / `index` / `itemId`）。
列表上面 / 下面要加"共 N 首""换个关键词"这类内容，用 `Column` 把 `Text` / `Button` 与它包在一起。
行高与排版用 `layout`（`auto` / `list` / `grid` / `multiList`）、`variant`（`card` / `plain`）、`height`、`padding` 调，
与内置歌曲列表同一套。

规则与注意：

- **`deps` 要把两个变量都写上**：漏掉 `scope:musicxx.search.keyword` 或 `.request`，点搜索时插件收不到新关键词，
  界面会一直停在切过去时的那一份；
- **空关键词是允许的**：插件类型没有"必须先输入"这条限制，空关键词画什么由插件自己定；
- **标签顺序**：内置类型在前，插件类型按 `order`（同 `order` 按注册顺序）跟在后面；标签栏只有文字，没有图标；
- **选中状态由主程序记**：用户这次选了插件类型，下次打开搜索页仍停在这个类型上；插件卸载后自动回到内置类型；
- **插件停用 / 关闭『拟声++』**：标签与结果一起消失，当时选的是它也会回到内置类型；
- **一份内容 = 一个类型**：`title` 就是标签名；同一个插件要提供多个类型就注册多份，用
  `scope:musicxx.search.type` 分辨当前是哪一份；
- **生成失败**（抛异常 / 返回结构不对）时这一份不会出现在标签栏；已经停在上面的用户再点一次搜索会重新请求；
- 结果区默认跟着页面滚：不给 `height` 时列表按内容高度排（整页滚），给 `height` 就固定高度自己滚；
  整块区域想占满就把根块写成 `Expanded`（或 `Stack` / `musicxx.Shader` / `musicxx.AdaptLayout`）；
- 自查：`musicxx.ui.slots()` 里能看到这个槽位与它能读的变量。

从外部打开某个插件类型：命令 / 链接的 `type` 写内容全名。

```text
musicxx://search/?type=plugin.my_plugin.cloudSearch&keyword=稻香
musicxx://call/?cmd=musicxx.ui.openSearch&args={"type":"plugin.my_plugin.cloudSearch","keyword":"稻香"}
```

插件自己也能跳过去：`await musicxx.capability.call("musicxx.ui.openSearch", { type: "plugin.my_plugin.cloudSearch", keyword })`。

## 2. 插件页面（`ext://<插件id>/<内容 id>`）

### 2.1 打开过程

1. 用户点入口 / 插件自己调 `musicxx.ui.openRoute("ext://my_plugin/settings")`；
2. 主程序在槽位 `musicxx.ui.page` 上找**你这个插件的**内容 id = `settings` 的那一份（地址最后一段就是内容 id）；
   内容 id 只用在本插件内部：不同插件可以用同一个短名（框架按 `plugin.<插件id>.<短名>` 存），
   打开页面时按全名取，不会串到别人的同名页面；
3. 主程序要一份生成结果：生成函数拿到声明过的 `deps` 变量，返回一棵块树；
4. 主程序渲染；页面内容里的按钮 / 输入框都走能力，页面本身不需要插件再调任何接口。

页面也是内容，所以**注册方式与插入位置完全一样**：

```js
musicxx.ui.content.register({
    id: "settings",                      // 地址就是 ext://my_plugin/settings
    slot: "musicxx.ui.page",
    title: "我的插件设置",
    deps: ["scope:musicxx.page.args"],   // 要读打开页面时带的参数就声明它
}, function (ctx) {
    return { title: "我的插件设置", blocks: [ /* ... */ ] };
});
```

```cpp
// C++：同一个注册，生成函数返回页面文档
uiContentRegister("settings", MUSICXX_PLUGIN_UI_SLOT_PAGE, "[]",
                  &onGenerateSettings, 0, 0, "我的插件设置");
```

动作改状态后要页面跟着变，就 `musicxx.ui.slot.update("musicxx.ui.page", ["settings"])`（动作不返回视图）。
页面参数用变量 `scope:musicxx.page.args`（打开时传的对象）与 `scope:musicxx.page.route`（当前地址）。

### 2.2 视图结构

```jsonc
{
  "title": "页面标题",          // 与 blocks 不能同时为空（都空 = 主程序提示插件没返回可用视图）
  "subtitle": "可选副标题",
  "blocks": [ /* 块列表，按声明顺序从上到下 */ ]
}
```

`view` 的写法主程序都认（同一个解析入口，不需要插件自己挑）：

| 写法 | 例子 | 什么时候用 |
|---|---|---|
| **一棵块** | `{kind: "musicxx.Shader", bundle: …}` | 最常见：整个背景 / 整个插入块就是一栖块 |
| 块数组 | `[{kind: "Text", …}, {kind: "Divider"}]` | 从上往下的几块，不要标题 |
| 完整文档 | `{title: "…", subtitle: "…", blocks: [...]}` | 插件页面：要标题 / 副标题 |

生成函数返回时外面再包一层 `{json: …}` 也行（`{json: {kind: …}}` 与直接返回块一样）。
**一棵块不用自己包 `Expanded` 或 `blocks`**：根上是 `musicxx.Shader` / `Stack` /
`musicxx.AdaptLayout` 时主程序直接拿紧约束铺满。

### 2.3 块类型（界面描述层的组件）

页面内容是**界面描述层**（`cxx_pluginxx_ui`，插件包里带的是它的子模块）的组件：插件只写一份
中立描述，客户端按自己的能力做降级适配 —— 同一份内容在图形界面与终端上都能画出来。
完整组件表、字段与适配规则见库的生成文档
`src/third_party/cxx_pluginxx_ui/docs/ui-schema.md`（每个组件都在那里，这里只列常用的）。

| 常用 `kind` | 字段 | 说明 |
|---|---|---|
| `Text` | `text`、`type`（`body`/`caption`/`title`）、`color`、`bold`、`dim`、`mono`、`wrap`、`maxLines`、`align`、`action` | 一段文字；层级用 `type`，语义色用 `color`（`normal`/`hint`/`accent`/`error`…）；`bold` 显式加粗（`type: title` 也是加粗） |
| `Divider` | — | 分隔线 |
| `Gap` | `size`（u，缺省 = 客户端默认行距） | 竖直留白 |
| `Button` | `label`（可选）、`variant`（`normal`/`full`/`simple`，默认 `normal`）、`highlight`（默认 `false`）、`icon`、`glyph`、`color`、`size`、`disabled`、`action` | 按钮，点击执行自己的 `action`；`icon` 是插件自带的 svg 路径（相对插件目录，见 §1.5）。内容按“文字 → 图标 → glyph”取：有 `label` 就画文字（可带图标），没 `label` 有 `icon` 就是**纯图标按钮**，都没写就用 `glyph` 当文字（终端 / 纯文本客户端靠它）；三者都空时不画按钮。`color` 是语义名（见上），写了就优先于主题的选中 / 禁用态内容色。`variant` 只定**样式与宽度**：`normal` 是带底色的默认按钮（宽度按文字内容），`simple` **无底色**（只画文字与图标，控制栏图标按钮、列表里的文字按钮用它），`full` 是**大号按钮**并**铺满父块宽度**（设置页里“一行一个”的按钮用它）；`highlight` 单独表示**选中态**（主色底，由主题决定；`simple` 平时无底色，选中时同样用主题的选中底色，与控制栏的选中按钮一致），默认不选中。`size` 是**按钮高度**（u，见下表下方的说明），不写用客户端默认。**`full` 的父块必须给得出宽度**（`Column` / `Block` / `Expanded` / 固定宽 `SizedBox`）：直接放进 `Row` 这类宽度不限的地方会报 `BoxConstraints forces an infinite width`，要横排就每个按钮各包一层 `Expanded`（`kit.actionsRow` 已经这么做） |
| `Block` | `title?`、`variant`（`card`/`inset`/`plain`）、`padding`、`margin`、`action`、`child` | 内容块：`card` 有底色 + 圆角，`inset` 是浅色底（要分组时用），`plain` 只有声明的留白（卡片里的列表行用它，行与行直接按列表排列） |
| `Row` / `Column` | `gap`、`main`（含 `spaceBetween`）、`cross`、`action`、`children` | 横向 / 纵向排列（多块） |
| `Expanded` / `Spacer` | `flex`、`child` | 按比例分剩余空间 / 纯占位 |
| `SizedBox` | `width`、`height`、`aspect`、`child` | 固定尺寸或占位 |
| `Padding` | `padding`、`child` | 内边距 |
| `ThemeScope` | `data`、`child` | 子树样式作用域（**只管颜色**）：见 §2.3.4 |
| `KV` / `Table` / `Tree` | 见库文档 | 键值 / 表格（列宽自动分配）/ 层级列表 |
| `Badge` / `Progress` | 见库文档 | 状态小标签 / 进度条 |
| `Control` | `control`（`buttons`/`select`/`checkbox`/`switch`/`text`/`number`/`password`）、`id`、`label`、`help`、`value`、`options`、`multiline`、`disabled`、`action` | 交互控件：**值变化即派发**动作（参数里带 `id` 与当前值）。`text`/`number`/`password` 用应用自己的输入框：**回车或失焦才派发**（不是逐字派发），`password` 默认遮住（右侧有眼睛按钮），纯文本降级固定打码；输入类控件要占满一行，**必须放在有宽度的容器里**（`Column` / `Block` / `Expanded` 等），直接放进 `Row` 会报 `RenderFlex children have non-zero flex but incoming width constraints are unbounded`；`select` 候选项只有一行标题（要“标题 + 多行描述”就用浮层的 `picker`） |
| `ListViewBase` | `items`（必填）、`layout`、`variant`、`height`、`padding`、`header`、`footer`、`emptyText`、`itemAction` | 真正的列表：列表项 `[{id?, title, subtitle?, trailing?, image?, icon?, color?, selected?, path?, isDir?, data?, custom?, action?}]`；`layout` 是 `auto`（跟应用设置）/`list`/`grid`/`multiList`；不给 `height` 就跟页面一起滚（条目多时给高度让它自己滚）。**只显示，没有工具条**；`custom` 是“整项内容用任意块画”，见 §2.5.1 |
| `ListView` | 同上 + `toolbar`、`menu`、`itemMenu`、`editAction`、`saveAction`、`addAction`、`removeAction`、`removeCheckedAction`、`removeTipTitle`、`positionAction`、`refreshAction`、`loadMoreAction`、`enableCheck`、`enableDrop`、`showCount` | 带工具条的列表（编辑 / 多选 / 加删 / 定位 / 菜单 / 下拉刷新 / 加载更多）：**只有写了对应字段才出现那一个按钮**，没写的自动不出；详见 §2.3.3 |
| `Icon` / `Image` / `Stack` / `Markdown` | 见库文档 | 可选组件：客户端不支持时由适配步骤降级（`Icon.glyph`、`Image.alt`）；`Icon.name` 是插件自带的 svg 路径（相对插件目录，见 §1.5） |
| `musicxx.DirListView` | `path`、`items`（必填，用法同列表，可带 `path`/`isDir`）、`multi`（默认关）、`showPath`、`pathAction`、`itemAction`、`selectAction`、`refreshAction`、`loadMoreAction`、`height`、`padding`、`variant`、`emptyText` | musicxx 专属：目录列表（**直接用应用内置的 `DirList` 画**）—— 一层目录的条目（文件夹/文件图标、标题 + 第二行、行尾文字）+ 左上角条目数 + 顶部路径栏（显示 `path`）+ 路径栏里的「上一级」按钮（给了 `pathAction` 才出现）+ 可选的多选勾选框（`multi` 打开才出现，勾选态取条目的 `selected`，点一下派发取反后的 `selected`）。给了 `refreshAction` / `loadMoreAction` 还能**在列表内下拉刷新 / 滚到底加载更多**（不加任何按钮）。**当前目录与选中态由插件保存**（客户端只派发动作，插件返回新视图）；返回键是“退出页面 / 浮层”（本块不自己进目录，要逐层往上退就给 `pathAction`）。行样式由应用定：`icon` / `subtitle` / `trailing` / `selected` / `data` 生效（条目带 `data.song` 就按内置的**歌曲行**画：封面 + 歌手），`color` / `image` / `custom` 在目录列表里不生效。不认识的客户端退成普通列表文本 |
| `musicxx.SongListView` | `items`（必填，用法同列表，每项带 `data.song`）、`layout`、`variant`、`height`、`padding`、`emptyText`、`itemMenu` | musicxx 专属：**歌曲结果模板**（搜索 / 浏览结果都行）—— 行样式、点击播放、内置歌曲菜单都由应用画，插件只给数据。条目带 `data.song`（与 `musicxx.DirListView` 的歌曲项同一套写法：`type` / `src` / `artist` / `name` / `fileName` / `size` / `durationMs` / `cover`）就能播；**点一行会把整份结果当作播放队列**并从这一首开始（是否进播放页跟随客户端设置）。没有 `data.song`（或没有 `src`）的条目照样画（标题 / 副标题 / `trailing`），只是点了不播。长按 / 右键出内置歌曲菜单（播放、加入歌单、加入播放列表、下载），`itemMenu` 的项排在后面、点了派发给插件（参数带 `listId` / `index` / `itemId` / `song`）。`emptyText` 画空列表；不给 `height` 就跟页面一起滚。不认识的客户端退成普通列表文本 |
| `musicxx.AdaptLayout` | `columnBody`（必填）、`rowBody`（必填）、`columnSecondBody`、`rowSecondBody`、`rowBodyRatio`（0.5）、`columnBodyRatio`（0.5）、`enableAnimations` | musicxx 专属：按窗口宽高比切换两套内容（宽高比 < 1 用 `columnBody`，否则 `rowBody`），给了第二套就按比例分两栏；对应应用内置的 `XXAdaptiveLayout`。**只用于页面 / 浮层的最外层**：它按父块给出的宽高分两栏，所以要放在给得出尺寸的地方（页面内容区、浮层内容区）；塞进 `Block` / 行内这类尺寸由内容决定的位置会得到布局断言。不认识的客户端用 `rowBody` |
| `musicxx.Shader` | `bundle`、`args`、`speed`、`maxFps`、`animate`、`resolutionScale` | musicxx 专属：页面里画一块插件着色器，尺寸由父块决定；见 [plugin-shader-bundle.md](plugin-shader-bundle.md) §9 |

两个名字先记住（写错不会画错东西，但会白写）：

- **`child` 是一块、`children` 是多块**。只放一块内容的容器用 `child`：`Block` / `Expanded` /
  `SizedBox` / `Padding` / `Align` / `Collapse` / `ThemeScope` / `musicxx.AnimatedBuilder` /
  5 个 `musicxx.*Transition`。写成数组也只取第一块（不同客户端行为可能不一致，别这么写）。
  要放多块先包 `Column`。`Row` / `Column` / `Stack` / 列表的 `header` / `footer` / 工具条、
  `musicxx.AdaptLayout` 的两套 body 仍然是 `children`。
- **`color` 是语义名，不是色值**：能写的是 `normal`（默认）/ `hint` / `accent` / `title` /
  `error` / `warning` / `success`，对应客户端主题里的那几个颜色（跟随主题与昼夜）。
  想指定具体颜色用 `ThemeScope` 的 `data`（那里才是 `#rrggbb` / `#aarrggbb`），见 §2.3.4。
  不要写 `tone` 这个旧名字，写它不认。


规则（解析层规则，与库文档一致）：

- 组件名（`kind`）是**首字母大写的驼峰**，解析时**忽略大小写**；
- 未知组件：有 `fallback` 就显示它，没有就跳过（不会报错）；未知字段忽略；未知枚举值取默认；
- 尺寸只有一个单位 `u`（允许小数）：图形界面 1u = 1 逻辑像素，终端按客户端的格大小换算；
  另有 `{"percent": 40}`（占父容器可用空间的比例）与 `"auto"`（内容决定）。
  percent 的基准是**直接父块这一轮拿到的可用空间**，所以父块在那一方向必须给得出尺寸：
  放在 `Stack`、固定宽高的 `SizedBox` 里算得出来；`Column` 的子块高度与 `Row` 的子块宽度
  是**无界**的（父块让子块自己决定大小），那里的 percent 没有参照，按内容尺寸画。
- 边距只写一个字段（`padding` / `margin`），值是数字（四边）、`{"horizontal":20,"vertical":8}` 或单边对象。

### 2.3.1 值表达式（尺寸 / 进度等字段也能"算"）

尺寸类字段（`SizedBox.width`/`height`、`Gap.size`、`Row`/`Column.gap`、`KV.keyWidth`、
`Table` 列宽、`Icon.size`、`Image.width`/`height`、`Progress.width`）与 `Progress.value`
除了写字面量，还能写**值表达式**：一个带 `kind` 的对象，图形界面用与着色器参数**同一套引擎**
每帧求值（来源、过渡、动画、组合都在一份声明里）：

```jsonc
{ "kind": "SizedBox",
  // 高度跟着当前音频响度呼吸（10 帧/秒的数据包一层 smooth 才不会有台阶感）
  "height": { "kind": "mul", "of": [
      { "kind": "smooth", "attackMs": 20, "releaseMs": 260,
        "of": { "kind": "source", "name": "musicxx.spectrum.level" } },
      { "kind": "lfo", "shape": "sine", "periodMs": 2600, "from": 120, "to": 160 } ] },
  "child": { "kind": "Progress", "value": { "kind": "source", "name": "plugin.demo.progress" }, "total": 100 } }
```

- 求值结果按 `u` 解释；写 `unit: "percent"`（或 `{"kind":"const","value":40,"unit":"percent"}`）
  就是"父容器比例"，与 `{"percent":40}` 一致；
- **文本字段**（`Text.text`、`Badge.text`、按钮文案、KV / Table / Tree 的文本…）也能写值表达式：
  求值结果当文本用，求值失败或没有上下文时用字面量作为回退（`{"kind":"format", …}` 可以把数值写进文案）；
- **公共字段 `visible`**（每个块都能写）：布尔字面量或值表达式，求值为假时这一块**不渲染**；
- 节点表、来源（`musicxx.theme.*` / `musicxx.icon.*` / `musicxx.spectrum.*` / `musicxx.env.*`、
  广播频道的当前值）、上限与降级规则见 [plugin-shader-bundle.md](plugin-shader-bundle.md) §7；
- 名字空间：`musicxx.` 前缀是框架保留的（派生来源与官方广播频道），**自定义名字允许含 `.`**
  （例如 `plugin.<插件id>.progress`）；
- 求值失败（未知 `kind`、参数写错）只**忽略该字段**并按缺省处理，日志里留一条原因；
- 当前版本：字段求值是"每次重画算一次"（`tween` / `lfo` 这类随时间变化的节点要等页面里的
  帧驱动接上才有动画效果）；着色器块（`musicxx.Shader`）本来就是每帧渲染，不受这一条限制。

### 2.3.2 动画（`musicxx.AnimatedBuilder` + 过渡块）

页面里要**逐帧动**的东西放在 `musicxx.AnimatedBuilder` 里：它声明一张**通道表**（名字 → 值表达式）
并驱动子树按帧重建，子树里的字段用 `{"kind":"source","name":"<通道名>"}` 读通道：

```jsonc
{ "kind": "musicxx.AnimatedBuilder",
  "maxFps": 30,                                  // 缺省 30；不限上限（想要 60 / 120 就写上去）
  "values": {
    "card.h": { "kind": "mul", "of": [
        { "kind": "smooth", "attackMs": 20, "releaseMs": 260,
          "of": { "kind": "source", "name": "musicxx.spectrum.level" } },
        { "kind": "lfo", "shape": "sine", "periodMs": 2600, "from": 60, "to": 90 } ] },
    "card.intro": { "kind": "tween", "from": 0, "to": 1, "durationMs": 900, "ease": "outCubic" }
  },
  "child": {
    "kind": "musicxx.FadeTransition", "value": { "kind": "source", "name": "card.intro" },
      "child": {
        "kind": "musicxx.SizeTransition", "axis": "vertical", "value": { "kind": "source", "name": "card.intro" },
          "child": { "kind": "SizedBox", "height": { "kind": "source", "name": "card.h" } } } }
}
```

| 块 | 字段 | 说明 |
|---|---|---|
| `musicxx.AnimatedBuilder` | `values`（通道表）、`maxFps`、`child` | 通道作用域 + 每帧重建；没有通道时零成本（不建帧调度） |
| `musicxx.SizeTransition` | `axis`（`vertical`/`horizontal`）、`axisAlignment`、`value`(0~1)、`curve`、`child` | 按进度把子块从 0 撑开 / 收拢 |
| `musicxx.FadeTransition` | `value`(0~1)、`curve`、`child` | 透明度 |
| `musicxx.SlideTransition` | `from`/`to`（`[x,y]`，单位 = 自身尺寸倍数）、`value`、`curve`、`child` | 位移 |
| `musicxx.ScaleTransition` | `from`/`to`（倍数）、`value`、`curve`、`child` | 缩放 |
| `musicxx.RotationTransition` | `from`/`to`（圈数）、`value`、`curve`、`child` | 旋转 |

规则与上限：

- `value` 可以写字面量、值表达式或通道引用；`curve` 是"通道没带缓动"时的简写（取
  `linear`/`inQuad`/`outQuad`/`inOutQuad`/`inCubic`/`outCubic`/`inOutCubic`/`inSine`/`outSine`/
  `inOutSine`/`outBack`/`outElastic`）；
- 每个 `AnimatedBuilder` 最多 **32 条通道**（超过的被忽略并记日志）；每帧只重建"读到值的块"；
- 帧调度走主程序的视口动画（**限帧 + 页面被遮挡 / 进后台时自动停**）；**动画等级低时只出构建那一帧**
  （等于静态，与主题的"低等级少动效"一致）；
- 过渡块只画自己（尺寸 / 透明度 / 位移 / 缩放 / 旋转），**子块完全不用改**；
- 状态（`smooth` 的上一帧值）跟着通道表走：同一份声明不被打断，声明变了重新开始；
- **没有 `AnimatedBuilder` 包裹的字段仍然是"每次重画算一次"**（插件可以用"能力返回新视图"刷新页面，
  但那是跳变，不是动画）。
- **kit 里有现成的拼装写法**：`kit.animScope({values, maxFps, child})` / `kit.fadeTransition({value, curve, child})` /
  `kit.sizeTransition({value, axis, curve, child})` / `kit.slideTransition({value, from, to, curve, child})` /
  `kit.scaleTransition({value, from, to, curve, child})` / `kit.rotationTransition({value, from, to, curve, child})`
  （参数与生成的说明见 `docs/musicxx-ui-kit.md`）。

### 2.3.3 列表块（`ListViewBase` / `ListView`）

列表项一般是**数据**（没有 `kind`）；写了 `custom` 时它的内容是块。两个块用同一套列表项：

```jsonc
{ "id": "weekly", "title": "每周新歌", "subtitle": "每周一更新", "trailing": "12 首",
  "image": "cover", "icon": "resource/svg/star.svg", "color": "hint", "selected": false,
  "action": "openPlaylist" }
// 自定义整项内容：写了 custom 就不再画 title / subtitle / trailing / 前置图标
{ "id": "row-1", "action": "openRow",
  "custom": [ { "kind": "Row", "children": [ /* 任意块 */ ] } ] }
```

| 字段 | 说明 |
|---|---|
| `id` | 稳定标识：列表相关动作的参数里会带 `itemId`（编辑保存、批量删除靠它认出是哪一项） |
| `title` | 主标题（必填；写了 `custom` 时可以不写） |
| `custom` | 整项内容（任意块数组）：写了就**只画它**（行尾自动插入的编辑 / 多选 / 拖拽按钮不受影响） |
| `subtitle` / `trailing` | 副标题（说明字号）/ 右侧状态文字 |
| `image` | 前置图片：`cover`（当前歌曲封面）或相对插件目录的路径 |
| `icon` | 前置图标（插件自带的 svg，相对插件目录）；与 `image` 同时给时 `image` 优先 |
| `color` | 主标题语义色 |
| `selected` | 是否高亮（**只由插件给的数据决定**，客户端不做点击自动高亮） |
| `path` / `isDir` | 这一项对应的路径 / 身份与“是不是目录项”（目录列表用它，动作参数里原样回带） |
| `data` | 这一项的附加数据（任意 JSON 对象）：描述层只当原文携带、**不解释内容**，由应用按自己的约定读。musicxx 认其中的 `data.song` = 这一项是一首歌：`{type, src, name?, artist?, fileName?, size?, durationMs?, cover?}`（字段与 `musicxx.playlist.addSong` 的 `songs[]` 同一套），只有目录列表用它 —— 带上以后这一行画成歌曲样式（封面 + 歌手 + 音源标签）。`src` 必填（空 = 不是歌曲）；`type` 不写时用本插件声明的唯一音源类型（声明了多个就得写）；`cover` 是插件目录相对路径或 http 链接 |
| `action` | 点这一项派发的动作（不给就用块上的 `itemAction`） |

所有列表动作的参数里都会带上：`listId`（块 id）、`index`、`itemId`、`item`（整项快照，含 `path` / `isDir`）。
目录列表块（`musicxx.DirListView`）把这些直接放在参数顶层（`path` / `isDir`），勾选框那个动作另带 `selected`。

> 目录列表的**行样式由应用定**（与内置目录列表一致：前置图标 + 标题 + 第二行 + 行尾文字 / 勾选框），
> 所以 `color` / `image` / `custom` 在它里面不生效，`subtitle` 没写时第二行显示 `path`。
> 写了 `data.song` 的条目按内置列表的**歌曲行**画：前置是封面（`cover`，没有就用默认歌曲图标）、
> 第二行是歌手（没写 `subtitle` 时），`subtitle` 写了就用 `subtitle`（例如文件大小）。
> 只是“画成歌曲”而已：点这一行仍照常派发 `itemAction`，加进歌单仍要调 `musicxx.playlist.addSong`。

目录列表的**刷新与翻页走列表自己的手势**（不用在页面里放按钮）：

| 字段 | 什么时候出现 / 派发时带什么 |
|---|---|
| `refreshAction` | 写了就能在列表内下拉刷新：动作跑完刷新头收起（返回 `false` 或 `{ok:false}` 按“没成功”处理）。参数只有 `listId` / `value`（= 当前目录） |
| `loadMoreAction` | 写了就能滚到底（或上拉）加载更多：动作返回 `false` 或 `{ok:false}` 时底部提示变成“加载失败”，其余算成功。参数同上 |

两个动作都是**先派发、后由插件返回新视图**：插件在动作里把新一页放进自己的数据再 `return {view: …}`，
客户端下一帧就用新条目重画（列表位置、勾选态都保留）。`loadMoreAction` 建议只在确实还有更多时给
（例如 `total > entries.length` 时），否则底部会一直提示“加载更多”。

`ListView` 的字段与工具条一一对应：

| 字段 | 什么时候出现 / 派发时带什么 |
|---|---|
| `editAction` | 写编辑 / 保存 / 批量删除任一个就出现编辑按钮；进入编辑前派发，返回 `false` 或 `{ok:false}` 就不进 |
| `saveAction` | 编辑态的保存按钮；参数带拖拽后的新顺序：`{items:[{index,id,title}…], itemIds:[…]}`；返回 `{ok:false}` 留在编辑态 |
| `removeCheckedAction` + `removeTipTitle` | 编辑态删除选中项；组件自带确认弹窗（标题用 `removeTipTitle`，条目用列表项标题），参数 `{indexes, itemIds, items}` |
| `addAction` | 加号按钮；派发后用插件返回的新视图刷新 |
| `removeAction` | 非编辑态的删除按钮（删什么由插件决定，组件不弹确认） |
| `positionAction` | 定位按钮；返回 `{index:n}` 就滚到第 n 项（没有 `index` 只派发） |
| `refreshAction` / `loadMoreAction` | 下拉刷新 / 加载更多（`await` 完就当完成） |
| `menu` | 顶部“更多”菜单**完全由插件定义**（`[{title, icon?, action?}]`），组件内置项不显示 |
| `itemMenu` | 每项的菜单（长按 / 右键），同样由插件定义，点某一项时参数带 `index` / `itemId` / `item` |
| `toolbar` | 工具条中间的自定义内容（任意块，建议只放一行高的块） |
| `enableCheck` / `enableDrop` | 多选 / 编辑态拖拽排序（缺省都开） |
| `showCount` | 工具条显示条目数（缺省关） |
| `header` / `footer` / `emptyText` | 列表头 / 尾（任意块）/ 空列表提示 |

```jsonc
{ "kind": "ListView",
  "id": "subs",
  "layout": "list",
  "items": [ /* … */ ],
  "toolbar": [ { "kind": "Text", "text": "共 12 项", "type": "caption" } ],
  "editAction": "beginEdit",
  "saveAction": "saveOrder",
  "menu": [ { "title": "全部刷新", "action": "refreshAll" } ],
  "itemMenu": [ { "title": "重命名", "action": "rename" } ] }
```

> 条目很多（> 200）时给 `height`：不给高度就跟页面一起滚（一次建出所有项）。

### 2.3.4 子树样式作用域（`ThemeScope`）

`ThemeScope` 包住**一块**内容，子树里的文字 / 图标 / 按钮 / 标签按 `data` 里写的颜色取色。
描述层只规定“`data` 是一个对象”，**字段含义由客户端定**；下面这一份是 musicxx 的定义（其它客户端
可能不认识、或者只看一部分 —— 不认识时按 `flatten` 适配，子块照常显示、只是没有样式）。

```jsonc
{
  "kind": "ThemeScope",
  "data": {
    "text":    { "main": "#ff202124", "cross": "#99202124" },
    "surface": { "primary": "#ff3a7bd5", "background": "#ffffffff" },
    "button":  { "content": "#ff202124", "selectContent": "#ffffffff" },
    "mask":    { "main": "#ffffffff", "cross": "#ccffffff", "block": "#33000000" },
    "shimmer": { "base": "#80000000" }
  },
  "child": { "kind": "Column", "children": [
    { "kind": "Text", "text": "这一段用插件自己的配色" },
    { "kind": "Button", "label": "确定", "action": "ok" }
  ] }
}
```

| 组 | 可写的键 | 影响什么 |
|---|---|---|
| `text` | `main` / `cross` / `title` / `titleBackground` | 正文色 / 次要文字色（`caption`、`hint`）/ 标题色 / 标题底 |
| `surface` | `primary` / `background` / `backgroundCross` / `error` / `disable` / `dialog` / `dialogBarrier` | 主色（`accent`、选中态）/ 底色 / 卡片底色 / 错误色 / 禁用色 / 弹层底与遮罩 |
| `button` | `content` / `simpleContent` / `selectContent` / `disableContent` | 按钮内容色（普通 / `simple` / 选中 / 禁用） |
| `mask` | `main` / `cross` / `block` | 播放页那三色（普通 / 减淡 / 块底）：块处在播放页里时，主程序画的按钮按这三色取色；**只换颜色**，不会把这块变成遮罩前景排版 |
| `shimmer` | `base` / `highlight` / `decoration` | 加载占位（骨架屏）的底色 / 高光 / 装饰 |

规则：

- 颜色写法是 `#rrggbb` / `#aarrggbb`（不带 `#` 也认）；**只有 `data` 里写色值**，块字段的
  `color` 仍然是语义名；
- **只覆盖写了的角色**，没写的从外层继承（嵌套 `ThemeScope` 时是最近一层，没嵌套时是客户端主题）；
- 不认识的组 / 键、解析不出来的颜色都跳过（不会让整块失效）；一个可用颜色都没有时按“没声明”处理，
  内容照常画；
- 只管颜色：圆角、阴影、渐变、字号、间距不开放；
- `mask` 只在**播放页**（页面前景就是那三色）里看得到效果：那时这块里的 `Button`（没写 `color`）
  按它取色（内容色取普通色、底色取块底色、禁用取减淡色）。普通页面里写 `mask` 不会改变这块的外观；
- 它是纯样式容器，不接手势（`action` 仍按公共字段语义作用于整块）。

用之前先确认客户端画得出来：看取视图时拿到的能力段（`args.ui.blocks`）里有没有 `ThemeScope`。
**子块只有一块时不要单独放进 `ThemeScope`**（没意义，直接写那个块）；`kit.themeScope({child, data})`
是现成写法（`data` 原样交给客户端，所以写的就是上面那几个组）。

### 2.4 用 kit 拼装（推荐写法）

写页面内容不必手拼这些块：**随插件分发的界面 kit** 把常用组合封装好了。
JS 插件在清单里按顺序加载 kit（`scripts: [pluginxx_ui_kit.js, musicxx_ui_kit.js, plugin.js]`），
然后：

```js
const kit = pluginxx.ui.kit;

// env 是客户端能力段（取页面时主程序会放进参数：args.ui）；不传 = 中立描述，由客户端适配
kit.listRow({ title: "切歌次数", trailing: "3" }, env);
kit.dirListView({ path: "/Music", items: entries, itemAction: "browse", multi: true }, env);
kit.adaptLayout({ columnBody: portrait, rowBody: landscape }, env);
kit.settingRow({ title: "背景动画速率", depict: "改完立即生效", value: "1x" }, env);
kit.switchRow({ id: "skipAds", title: "跳过广告曲目", value: true, action: "setSkipAds" }, env);
kit.button({ label: "刷新本页", variant: "full", highlight: true, action: "card" }, env);
```

C++ 插件用头文件里的 kit（命名空间 `musicxx::ui::kit`，由 `plugin_kit.h` 带进来）：

```cpp
namespace kit = musicxx::ui::kit;
Json row = kit.listRow(Json::object({{"title", "切歌次数"}, {"trailing", "3"}}));
blocks.push_back(pluginxx::ui::dumpItem(row));   // 序列化成 JSON 块
```

kit 组件一览（基础 kit + musicxx 扩展 kit 合并后）：`title` / `hint` / `text` / `badge` / `icon` /
`gap` / `divider` / `button` / `actionsRow` / `card` / `dirListView` / `adaptLayout` / `listRow` /
`section` / `kv` / `table` / `tree` / `sparkline` / `progressRow` / `settingRow` / `switchRow` /
`inputRow` / `shaderBlock` / `coverRow`（参数与生成的说明见 `docs/musicxx-ui-kit.md`）。

- kit 只做**拼装**，不含逻辑、不写死客户端的规则；需要按格对齐时用 `kit.cols(n, env)` / `kit.rows(n, env)`；
- **kit 随插件目录分发**（插件框架不提供）：`tools/sync_ui_kit.ps1` 会把两个 kit 文件复制进 JS 插件目录，
  C++ 插件用 SDK 头文件里的版本（改 kit 后跑 `tools/gen_ui_kit.ps1` 重新生成）。

### 2.5 布局约定

- 尺寸与边距都是描述层的 `u`：客户端按自己的单位换算（图形界面随窗口缩放），
  习惯写法：左右留白 `16~20`、行间距 `8~12`、间隔用小的 `Gap` / `SizedBox`；
- **块不带隐含边距**：客户端只把块画成它自己的样子（`Text` 就是一段文本、`Button` 就是一个按钮、
  `Divider` 就是一条线），边距只有描述里写的那几个（`padding` / `margin` / `gap`）；页面第一层
  的页面边距由客户端统一给一次，块与块、块内部的留白由 kit 模板或插件自己组合（kit 的
  `title` / `hint` / `text` / `button` / `divider` / `card` 都已经带上了自己的留白，直接写基础块
  就要自己写）；
- **加粗要显式写**：只有 `bold: true` 或 `type: "title"` 是粗体，其余都是常规字重；
- 能用相对布局就别写固定尺寸：`Expanded` / `Spacer` / `main: "spaceBetween"` / `percent` 优先；
- 任意块都可以带 `action`（`Button` 用自己的按钮点击语义）：带上之后**整块可点**，
  用来组合「可点的行」；可点的行建议放在 `Block`（有底色的卡片）里，用户才知道那是一行；
- **列表用 `ListViewBase` / `ListView`**（见 §2.3.3）：不要再用 `Block` + `Row` + `Column` 手拼列表行；
  只有“一两行固定内容”时才用 `listRow` / `settingRow` 这类行模板。

- **右侧的"状态/值"文本要自己约束宽度**：kit 的 `listRow` / `settingRow` 把右侧文本放在行尾、
  **不给宽度约束**（那一格适合短状态词，例如"生效中 / 3 首 / 内置背景"）。要在那一格写长文本
  （一整句话、带歌曲名或样式名）就别用这两个模板，自己组合 `Row` + 两个 `Expanded`
  （左 3 右 2，两侧都能换行），或者把长文本放到左侧那格（它可以换行）。窗口一窄（手机竖屏、
  桌面拉小），行尾那段固定宽度的文本会把整行撑出可见区域（`RenderFlex overflow`），
  同时把左侧说明挤成一字一行、整页被拉得很长。
- **按钮文案别写太长**：按钮里的文字不换行，文案超过一行可用宽度时字会顶破按钮
  （同样是 `RenderFlex overflow`）；超过一行放不下就缩短文案，把细节写进旁边的说明文本。
- **`variant: "full"` 的按钮要放在给得出宽度的地方**：它是“铺满父块宽度”，父块宽度必须
  有界（`Column` / `Block` / `Expanded` / 固定宽 `SizedBox`）。塞进 `Row`（宽度不限）会得到
  `BoxConstraints forces an infinite width`；要在横排里用就每个按钮各包一层 `Expanded`
  （`kit.actionsRow` 已经这么做）。其余两种样式按文字宽度画，不需要额外容器。
- **要改按钮大小就写 `size`**：它是**按钮高度**（单位 `u`），图标跟着缩放到高度的一半。
  不写就是客户端的默认大小（musicxx 是 `128u`），所以“两倍大”写 `256`。
  `size` 只对 `normal` / `simple` 生效：`full` 的高度由主题的大号按钮决定（写了也不读）。
  按钮不读父块尺寸，所以 `size` 写 `percent` 没有参照，按默认大小画。
- **`musicxx.AdaptLayout` 只能放在页面 / 浮层的最外层**：它按父块给出的宽高分两栏，放进
  `Block` 或行内（尺寸由内容决定）会得到布局断言。要“按窗口宽高比切换两套内容”就把它当页面
  根块用（根块是它时主程序会把它铺满内容区）。
- **行内的开关 / 勾选框要放在有宽度的盒子里**：`Control` 的 `switch` / `checkbox` 落在"宽度不限"
  的位置时行为随客户端实现而异（不然会出现 `RenderFlex ... unbounded` 的断言）。稳妥写法是给它一个
  **固定宽度的 `SizedBox`**（例如 190 个 `u`，与开关自身宽度相当）；"几选一"优先用一排 `Button` 块
  （每个块各自声明 `{id, value}` 参数、当前项用 `highlight: true` 高亮），不要用 `Control(buttons)`。

#### 2.5.1 常驻头部 + 自己滚的中间 + 常驻底部（列表项 `custom`）

默认情况下**整个页面一起滚**（内容再多也不会溢出）。要让上边的目录/按钮/筛选、下边的确认按钮
留在原地，只让中间那一段滚，就写成**一个顶层 `Column`，把中间那块包到 `Expanded` 里**，
中间放一个**列表块** —— 滚动区域只由列表块提供（描述层没有通用的滚动容器）。

- 中间是**目录 / 文件**：用 `musicxx.DirListView`（自带路径栏、条目数与「上一级」按钮；行的样子由应用定）；
  刷新与翻页也交给它（`refreshAction` / `loadMoreAction`，下拉 / 上拉触发，不占地方）；
- 中间是**任意内容**（筛选条、若干设置组、自定义行）：用 `ListViewBase`，每一项写 `custom` ——
  `custom` 是这个列表项的**整项内容**（任意块），行尾自动插入的编辑 / 多选 / 拖拽按钮不受影响。
  一段内容就是一项，几段内容就几项（行与行之间由 `ListViewBase` 自己排版）。

```jsonc
{ "kind": "Column", "children": [
    { "kind": "Text", "text": "常驻在顶部的内容" },
    { "kind": "Expanded", "child": {
        "kind": "ListViewBase", "items": [
            { "custom": [ /* 这一段很长，列表自己滚 */ ] },
            { "custom": [ /* 另一段 */ ] } ] } },
    { "kind": "Button", "label": "常驻在底部的按钮", "action": "apply" } ] }
```

规则（主程序按这个格式判断，浮层与插件页面都一样）：

- 触发“填满高度”的条件：**根块是 `musicxx.Shader` / `Stack` / `musicxx.AdaptLayout` / `Expanded`**，
  或者**根块是 `Column` 且其中有这些**（`Column` 里的 `musicxx.Shader` / `Stack` / `AdaptLayout` 会被
  当成 flex 子组件）；其它写法保持“整页一起滚”，不会因为忘了写 `Expanded` 而溢出；
- 这种描述要求客户端给**有界高度**：插件页面与浮层内容区都给了（浮层建议用
  `size: "full"` 打开，宽高都是可用尺寸的 90%，见 §2.7；高度铺满、宽度随内容就用 `fullHeightAutoWidth`）；
  对话框那种自由高度里客户端会退回普通排布；
- 列表块的 `height` 不给 = 跟随父给的可用高度（放在 `Expanded` 里时自己滚）；写了 `height` 就
  固定那个高度（不依赖父容器）；
- 列表块落在**没有可用高度**的地方（例如整页滚动里）时按内容高度铺开 —— 不报错，滚的只有页面；
- `musicxx.AdaptLayout` 靠“按窗口宽高比选一套 + 分两栅”工作，需要客户端给出可用尺寸：顶层直接
  写它时主程序会把它放进 `Expanded`；把它塞进 `Block` / 行内时只画当前该显示的那一套（不报错，
  也不会切换布局）；
- **固定部分会占用窗口高度**：头部与底部不会缩放，窗口很矮（桌面拉到几百像素高）时它们自己就可能
  超出内容区（客户端不会自动改回整页滚）。所以头部只放必要的几行；
- **kit 模板**：`kit.dirListView({path, items, itemAction, refreshAction, loadMoreAction})`、
  `kit.adaptLayout({columnBody, rowBody})`。

### 2.6 页面之间的跳转与刷新

- 跳转：按钮 / 可点块上用 `{"kind":"route","route":"ext://<插件id>/<内容 id>"}`（**允许指向任意插件**，
  含自己；页面由目标插件绘制，发起方拿不到对方的数据）。要跳**官方页面**
  （`musicxx.settings`、`musicxx.player`、`musicxx.search` 等）只能用动作 `musicxx.ui.openRoute`
  （白名单见 [plugin-api.md](plugin-api.md) §3.2.4）；UI 项与页面里的动作走的是**同一套判定**；
- 刷新当前页：`musicxx.ui.slot.update("musicxx.ui.page", ["<内容 id>"])`（动作不返回视图）；
- 页面参数用变量 `scope:musicxx.page.args`（打开时传的对象）与 `scope:musicxx.page.route`；
- 每个插件页面是**独立页面**（框架按路由 + 内容 id 区分），从 A 页跳到 B 页后返回能回到 A 页。

---

### 2.7 浮层与确认弹窗：用动作打开

**弹层属于“有过程、有结果”的事，所以走动作**（不往页面里加“浮层块”）。两种写法：

```js
// 1) 页面里的一行直接打开浮层（零代码）：那一行带 call 动作，参数就是能力参数
//    {"kind":"call","name":"musicxx.ui.overlay.show","args":{...}}
//    kit 的 overlayRow 已经包好这一层

// 2) 插件拿到结果后再改页面：那一行用 call 指向自己的能力，处理函数里自己调
musicxx.capability.register("pickQuality", async function () {
  const r = await musicxx.ui.pick({
    title: "音质",
    value: current,
    options: [{ value: "high", title: "高品质", describe: "文件较大，音质最好" }],
  });
  if (r && !r.cancelled) current = r.selected;
  // 动作不返回视图：改完内容让主程序把那一页再要一次
  musicxx.ui.slot.update("musicxx.ui.page", ["<那一页的内容 id>"]);
});
```

| 想要 | 用什么 |
|---|---|
| 确认一件事（是 / 否） | `musicxx.ui.dialog`，返回 `{ok, confirmed}`（也可以给 `blocks` 富内容） |
| 展示一块内容 / 收集一点输入 | `musicxx.ui.overlay({title, blocks, textConfirm})` |
| 让用户从若干项里选一个（每项能写多行说明） | `musicxx.ui.pick({title, options, value})` |
| 自己收起来 | `musicxx.ui.overlayClose()` |

要点：

- **等用户操作的动作要把超时设长**：JS 的 `dialog` / `overlay` / `pick` 缺省已给 10 分钟；
- **层数不限制**：一个插件可以同时开多层（后开的盖在上面），`overlayClose()` 不点名就关最上面那一层，
  点名（`id`）则关最上面那个名字对得上的层；插件卸载 / 插件页面退出会自动关掉它的全部浮层；
- 浮层里的按钮动作**不会自动关闭浮层**：插件 push 一份新内容（`content.push` / `slot.update("musicxx.ui.overlay", […])`）
  就能在浮层里换内容（做翻页 / 刷新），要关自己调 `overlayClose`；`textConfirm` 那个按钮一定关闭；
- `pick` 只做单选；多选就在自己的 `blocks` 里放 `Control(checkbox)` 或列表块；
- 浮层 / 确认弹窗的外观（圆角、遮罩、尺寸预设、动画等级）跟应用主题走，插件不控样式。

---

## 3. 插件自己的设置页与配置

框架**不提供**配置表单、也**不管理**插件的设置入口（没有「插件设置」类型或列表）：

- 设置界面就是插件自己注册的一个普通页面（`ext://<插件id>/<内容 id>`）；
- 入口由插件自己给：主页入口（`home.entry`）或某个功能页里的按钮（`route` 动作）；
- 配置值存在插件目录的 `config.json` 里，默认值由插件自己给：

| 语言 | 读 | 写 | 配置文件路径 |
|---|---|---|---|
| JS | `musicxx.storage.getConfig(key, 默认值)` | `musicxx.storage.setConfig(key, value)` | 插件目录下的 `config.json` |
| C++ | 自己读 `configPath()` 指向的文件 | 同左 | `configPath()` |

要点：

- **默认值写在插件里**（`getConfig` 的第二个参数），读不到就用默认值 —— 框架不再保存默认值，也没有
  清除「用户改过的值」之外的恢复手段；
- 值改动后要让界面反映出来：改完再调一次能力返回 `{view: ...}`，主程序就会用新视图刷新（示例见
  `plugins/example_js_playing_bg/plugin.js` 的 `cycleBackgroundRate`）；
- 私有 KV（JS：`musicxx.storage.get/set/remove/list`，不带 `namespace`）与 `config.json` 是两份数据：
  KV 适合缓存、运行计数这类插件自己的数据，`config.json` 适合「用户看得懂、可能手改」的设置项。

---

## 4. 全局字体与输入框字体控制

插件可以控制应用全局文本（所有 `XXText*` 组件）和输入框（原生输入框、`XXInput*`、插件自绘页输入框等）使用的字体：

1. **设置已有系统字体名**：
   ```js
   // 切换为指定字体
   await musicxx.ui.setFontFamily("Microsoft YaHei");

   // 恢复默认字体
   await musicxx.ui.setFontFamily("default"); // 或 "" 或 null
   ```
2. **加载并注册字体文件**：
   字体文件可以随插件目录分发（例如 `resource/fonts/my_font.ttf`），也可以直接指定机器上任何可读的
   字体文件（绝对路径 / `file://` / `~/…`），在运行时动态加载并生效：
   ```js
   // 加载并立即设为全局字体
   await musicxx.ui.loadFont({
     family: "MyPluginFont",
     path: "resource/fonts/my_font.ttf", // 相对插件目录；绝对路径也行
     apply: true,                         // 可选，默认 true
   });
   ```
3. **读 / 改官方广播频道**：
   ```js
   // 读当前生效字体（同步，走插件框架里的缓存）
   var current = musicxx.broadcast.read("musicxx.ui.fontFamily");
   // 改字体调能力（广播是只读的）
   await musicxx.ui.setFontFamily("SimSun");
   ```
4. **生命周期自动恢复**：
   当设置字体的插件被停用或卸载、或插件框架停止时，主程序会自动恢复为默认字体，避免残留不可用的字体配置。

### 4.1 在页面按钮里切换字体：**处理器里不要 `await` 这些能力**

换字体会重建整棵界面、耗时几百毫秒到几秒。页面按钮走的是 `call` →
能力调用，**应用线程全程阻塞等这次能力调用返回**；处理器里 `await`
`musicxx.ui.setFontFamily` / `loadFont` 就会与调用方互锁：

- 主程序表现：点一下**整个程序卡几秒**，然后插件收到 `action_timeout` 报"切换失败"，
  可字体其实已经切过去了（能力是在超时之后才轮到应用线程处理的）；
- 正确写法（可照抄 `plugins/example_js_font/plugin.js`）：
  1. **立刻返回**：处理器只改自己的状态（内容下一次生成时按新状态画），
     然后调 `musicxx.ui.slot.update("musicxx.ui.page", ["card"])` 让主程序再要一份；
  2. 能力**后台执行**：`musicxx.ui.setFontFamily(...)` 只跟 `.then(...)`，不 `await`；
  3. 成功后把模式写进自己的状态与 `config.json`；失败就回滚显示、异步提示（那时调用早已返回，
     用 `musicxx.ui.toast` 这类能力是安全的）。
- 想知道"主程序现在到底是什么字体"：`musicxx.broadcast.read("musicxx.ui.fontFamily")` **同步**读当前值
  （值就是字体名字符串，如 `"SimSun"` / `"default"`），不要为了显示状态再去 `await` 一次能力。

> 这条规则对**所有**"改应用状态"的能力都成立（播放页背景、播放控制、存储写入…）：能力处理器必须
> 快速返回，长活儿发给动作后在 `.then` 里收尾。处理器返回 Promise 时插件框架会一直等它结算
> （等待上界是 5 秒，超时就是上面那种"卡几秒 + 失败"）。

---

## 5. 可照抄的示例

| 示例 | 演示了什么 |
|---|---|
| `plugins/example_js/plugin.js` | 主页入口 + 歌曲菜单项；`card` 功能页与 `settings` 设置页；配置读写与页面刷新；页面内容用随插件分发的 kit 拼装 |
| `plugins/example_js_font/plugin.js` | 全局字体切换（微软雅黑 / 字体文件示例 / 拟声默认）+ 文字与输入框字体预览；**按钮动作"立刻返回 + 后台执行"的样板**（见 §4.1） |
| `plugins/example_js_playing_bg/plugin.js` | 播放页背景六种样式（封面三种 + 图案三种）+ 歌曲图接管 + 速率设置页 + 页面里内联的 `musicxx.Shader` 块（见 [plugin-shader-bundle.md](plugin-shader-bundle.md)） |
| `plugins/example_js_playing_slots/` | 播放页位置：插入块（歌词页顶部 / 播放列表页头部 / 更多信息页底部）、控制栏按钮级与整栏替换、音频动效（§1.6 / §1.7 与 [plugin-shader-bundle.md](plugin-shader-bundle.md) §5.4） |
| `plugins/example_js_slot_provider` / `plugins/example_js_slot_consumer` | 嵌套槽位：前者用 `musicxx.ui.slot.define` 声明自己的槽位并往里注册内容，后者用 `UISlot` 嵌进去（§1.10） |
| `plugins/example_native/example_native.cpp` | 同样的 UI 能力（C++）：主页入口、歌曲菜单、`card` 页、用 `musicxx::ui::kit` 拼装内容 |
---

## 6. 旧写法对照

| 旧写法 | 现在 |
|---|---|
| `musicxx.ui.item.register({name, type: "playing.lyric.top", data: {view: …}})` | `musicxx.ui.content.register({id, slot: "musicxx.ui.playing.lyric.top"}, generate)` |
| `data: {view: "<能力短名>"}`（主程序调同名能力取视图） | 生成函数；要按状态变就把变量写进 `deps` |
| 插入位置 / 替换位置 / 样式槽位的声明项 | 一律改成内容（声明项只留给入口 / 菜单 / 音效） |
| 页面 `ext://<插件>/<视图 id>` 由**同名能力**返回 `{view}` | 页面是 `musicxx.ui.page` 上的内容，`<视图 id>` 就是内容 id |
| 动作返回 `{view: …}` 刷新当前页 | `musicxx.ui.slot.update(slot, [id])` |
| `musicxx.ui.slot.update(名字)`（只给名字） | `musicxx.ui.slot.update(slot, [内容 id…])`（不写 ids = 整个槽位） |
| 样式槽位用 `data.mode` / `data.shader` / `data.args` 描述画面 | 内容里写 **块**：`{"kind":"musicxx.Shader","bundle":…,"args":…}`；歌曲图的占位用 `data.keepSpace` / `data.fill` |
| 页面背景用 `data.routes` 声明路由 | 内容注册时给 `routes` |
| `{"kind":"dispatch"}` / `{"kind":"command"}` 动作 | 都是 `{"kind":"call"}`（短名 = 自己、`plugin.<id>.*` = 别的插件、`musicxx.*` = 应用） |

可照抄的例子（`plugins/` 下）：

- `example_js_playing_slots`：插入位置 + 控制栏替换 + 音频动效 + 页面背景 + 两个页面；
- `example_js_playing_bg`：六种播放页背景 + 歌曲图接管 + 三个页面（一个插件多个脚本）；
- `example_js_slot_provider` / `example_js_slot_consumer`：插件声明自己的槽位、另一个插件用 `UISlot` 嵌进去；
- `example_native`：C++ 侧的内容注册 / 槽位声明 / `slot.update` 与 `content.push`；
- `example_js` / `example_js_font` / `example_override_media_source` / `example_ftp_source`：入口与页面、能力与配置。

---

## 7. 下沉期间要改的契约（先列清单，一次改完）

这一节只记"**打算改**的字段 / 名字"。没写进来的就按现状冻结（§1.0），下沉时原样搬。

当前清单：**空**（本轮只搬实现，不改块树、槽位名与变量 id）。

要新增或改名时在这里写一行，写清四件事，然后同一次提交里改完两个项目的插件文档、生成物与示例：

| 项 | 旧 | 新 | 影响 |
|---|---|---|---|
| （示例）块字段 | `blocks[].kind` | — | — |

