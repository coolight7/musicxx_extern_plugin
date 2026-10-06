# 插件界面：UI 项、插件页面与设置页

插件**不写 Flutter 代码**。界面有两个层次，都只做声明、由 musicxx 渲染：

| 层次 | 插件做什么 | 宿主渲染在哪 |
|---|---|---|
| **UI 项** | 注册入口 / 菜单项（类型 + JSON 内容） | 宿主自己的界面：音乐主页入口列表、歌曲菜单、设置里的「播放页面背景」列表 |
| **插件页面** | 注册一个能力，返回视图描述（`{view:{title, blocks}}`） | `ext://<插件id>/<视图id>` 路由页面 |

两边的写法只有 API 名字不同（C++ 用 `uiRegister`/`capability`，JS 用 `musicxx.ui.registerEntry`/`musicxx.capability.register`），
**字段与块类型完全一致**，所以这份文档对两种插件都适用。语言相关的 API 见
[plugin-native-api.md](plugin-native-api.md)（C++）与 [plugin-js-api.md](plugin-js-api.md)（JS）。
第一次写插件、想知道从哪一步开始，先看 [plugin-guide.md](plugin-guide.md)。

---

## 1. UI 项

### 1.1 四种类型

| 类型（全名） | `data` 字段 | 渲染位置 | 当前版本的实际表现 |
|---|---|---|---|
| `musicxx.ui.home.entry` | `title`（必填）、`subtitle?`、`icon?`、`action?` | 音乐主页的入口列表（排在宿主内置入口之后，用户可以在主页排序里拖动） | `title` 显示在按钮上、`subtitle` 显示在下一行（小字）；`icon` 写宿主内置的 svg 名（不写用默认图标；写不存在的名字退回默认图标） |
| `musicxx.ui.song.action` | `title`（必填）、`icon?`、`action?` | 歌曲列表项的「歌曲菜单」 | 显示 `title`；`icon` 同上；副标题位置固定显示 `外部插件：<插件id>` |
| `musicxx.ui.playlist.action` | `title`（必填）、`icon?`、`action?` | 歌单页（歌曲列表页）的列表菜单 | 显示 `title`；动作参数里会带上这张歌单的 `pid` / `name` / `songNum` |
| `musicxx.ui.playing.background` | 见 [plugin-shader-bundle.md](plugin-shader-bundle.md)（封面纹理 `cover` 见该文 §5.1） | 「设置 → 播放页面背景」的样式列表 | 用户选中后生效 |
| `musicxx.ui.playing.icon` | 见 [plugin-shader-bundle.md](plugin-shader-bundle.md) §5.3 | 「设置 → 主题 → 播放页歌曲图」的接管项 | 选中后由该项接管播放页中间的歌曲图：`shader`（着色器绘制）/ `view`（界面组合）/ `none`（只占位、不显示内容） |

`icon` 的取值来自客户端能力段里的 `icons`（`host.info().ui.icons`）：写客户端不认识的名字
不会出错，只是退回默认图标。想知道当前客户端认哪些名字就读能力段，不要写死某一份清单。

### 1.2 注册

```cpp
// C++：短名 + 类型 + data + order（小者靠前，同 order 按注册顺序）
uiRegister("card", MUSICXX_PLUGIN_UI_TYPE_HOME_ENTRY,
           R"({"title":"我的入口","subtitle":"示例","action":{"kind":"route","route":"ext://my_plugin/card"}})",
           100);
```

```js
// JS：{name, type, order, data}（也可以把 data 的字段平铺在对象上）
musicxx.ui.registerEntry({
    name: "card",
    type: "home.entry",              // 简称或全名都可以
    order: 100,
    data: {
        title: "我的入口",
        subtitle: "示例",
        action: { kind: "route", route: "ext://my_plugin/card" },
    },
});
```

规则（宿主在注册时校验，写错就直接失败，不会等到渲染时才发现）：

- `name` 用**短名**，宿主补成 `plugin.<插件id>.<短名>`；写全名时也必须属于本插件命名空间（冒充别人返回 `-6`）；
- 同一个项名再次注册 = **覆盖**（保留原来的排序位置）；
- UI 项数量与单条 `data` 大小都不设上限（早期版本的 64 个 / 64 KiB 已移除）；
- 类型必须是上表四种之一（`-4`），`data` 必须是 JSON 对象（`-1`），需要的 `title` 不能为空（`-1`）；
- 变化会推送 `musicxx.ui.changed` 事件（载荷带该插件的**全部**项，应用侧整批替换）。

更新与注销：

```js
musicxx.ui.updateEntry("card", { ...完整 data... });  // 整体替换，不是合并
musicxx.ui.unregisterEntry("card");
musicxx.ui.entries();                                  // 本插件已注册的项（脚本侧镜像）
```

> `updateEntry` 是**整体替换**：只想改一个字段也要把完整的 `data` 传回去，否则剩下的字段会丢。
> `maxFps` / `resolutionScale` / `animate` 这类影响帧调度或分辨率创建的字段，运行期改完需要用户
> 重新选中该项才完全生效；`speed`、颜色参数这类每帧现算的字段立即生效。

### 1.3 UI 项动作（`data.action`）

动作写法与插件页面里的动作**完全一致**（同一套写法与同一个解析器）：

| 写法 | 字段 | 行为 |
|---|---|---|
| `"openSettings"` | — | **字符串短写**，等于 `dispatch`：调用本插件的能力 |
| `{"kind":"dispatch"}` | `name: "<短名>"`, `args?` | 调用**本插件**的能力；返回视图描述时直接打开页面（入口与页面可以合并成一次调用） |
| `{"kind":"route"}` | `route: "ext://<插件id>/<视图id>"` | 打开插件页面（**允许指向任意插件**：页面由被跳转插件自己绘制，发起方拿不到对方的数据） |
| `{"kind":"command"}` | `name: "musicxx.<域>.<动作>"`, `args?` | 执行宿主动作（与插件自己调 `musicxx.call` 同一份实现） |
| `{"kind":"none"}` / 不写 | — | 纯展示项，点了没有动作 |

- 注册时校验：`route` 必须 `ext://` 开头且带插件 id 与视图 id；`dispatch` 必须有非空 `name`；
  `command` 的 `name` 必须是官方 `musicxx.*`；
- **动作指向的能力必须真的注册过**：没注册时宿主提示
  「插件『<插件id>』没有提供『<能力名>』」，`example_native` / `example_js` 的 `card` 页是可照抄的例子；
- `dispatch` 动作成功时的表现：返回视图描述就直接打开页面（入口与页面合成一次调用），
  否则提示「『<入口标题>』执行完成」；
- 动作参数里宿主会补上当前上下文：歌曲菜单项补 `sid`/`name`/`artist`，歌单菜单项补
  `pid`/`name`/`songNum`（插件自己写的 `args` 保留，冲突时以宿主补的为准）。

### 1.4 当前版本的渲染缺口（写插件前先确认）

这一版把之前缺的渲染点补齐了：`home.entry` 的 `subtitle` / `icon` 都会画出来、`playlist.action`
在歌单页的列表菜单里有位置。仍要注意的是：

- 入口的图标只在客户端认识的名字里挑（能力段 `icons`），写别的名字会退回默认图标；
- 主页入口的文字是单行省略（标题、副标题各一行），别写长句；
- 想确认自己注册了哪些项：`musicxx.ui.entries()`（JS）/ `uiEntries()`（C++）。

---

## 2. 插件页面（`ext://<插件id>/<视图id>`）

### 2.1 打开过程

1. 用户点入口 / 插件自己调 `musicxx.ui.openRoute("ext://my_plugin/settings")`；
2. 宿主打开路由页面，调用**与被跳转插件同名的能力**：视图 id = 能力短名
   （`ext://my_plugin/settings` → 能力 `plugin.my_plugin.settings`），参数是
   `{"view": "<视图id>", "ui": {客户端界面能力段}}`；
3. 能力返回视图描述：`{"view": {...}}`（推荐）或直接返回视图对象；
4. 宿主渲染；能力返回的每个块都不需要插件再调用任何接口。

**能力必须注册**（在 `start` 事务 / 脚本顶层注册），并且**同步返回**：

```js
musicxx.capability.register("settings", function (args) {
    return { view: { title: "我的插件设置", blocks: [ /* ... */ ] } };
});
```

```cpp
capability(*this, "plugin.my_plugin.settings",
           [](std::string_view, std::string_view) -> std::string {
               return R"({"view":{"title":"我的插件设置","blocks":[ ... ]}})";
           });
```

### 2.2 视图结构

```jsonc
{
  "title": "页面标题",          // 与 blocks 不能同时为空（都空 = 宿主提示插件没返回可用视图）
  "subtitle": "可选副标题",
  "blocks": [ /* 块列表，按声明顺序从上到下 */ ]
}
```

### 2.3 块类型（界面描述层的组件）

页面内容是**界面描述层**（`cxx_pluginxx_ui`，插件包里带的是它的子模块）的组件：插件只写一份
中立描述，客户端按自己的能力做降级适配 —— 同一份内容在图形界面与终端上都能画出来。
完整组件表、字段与适配规则见库的生成文档
`src/third_party/cxx_pluginxx_ui/docs/ui-schema.md`（每个组件都在那里，这里只列常用的）。

| 常用 `kind` | 字段 | 说明 |
|---|---|---|
| `Text` | `text`、`type`（`body`/`caption`/`title`）、`tone`、`bold`、`dim`、`mono`、`wrap`、`maxLines`、`align`、`action` | 一段文字；层级用 `type`，语义色用 `tone`（`normal`/`hint`/`accent`/`error`…）；`bold` 显式加粗（`type: title` 也是加粗） |
| `Divider` | — | 分隔线 |
| `Gap` | `size`（u，缺省 = 客户端默认行距） | 竖直留白 |
| `Button` | `label`、`variant`（`primary`/`secondary`/`ghost`/`link`）、`icon`、`disabled`、`action` | 按钮，点击执行自己的 `action` |
| `Block` | `title?`、`variant`（`card`/`inset`/`plain`）、`padding`、`margin`、`action`、`children` | 内容块：`card` 有底色 + 圆角，`inset` 是浅色底（要分组时用），`plain` 只有声明的留白（卡片里的列表行用它，行与行直接按列表排列） |
| `Row` / `Column` | `gap`、`main`（含 `spaceBetween`）、`cross`、`action`、`children` | 横向 / 纵向排列 |
| `Expanded` / `Spacer` | `flex`、`children` | 按比例分剩余空间 / 纯占位 |
| `SizedBox` | `width`、`height`、`aspect`、`children` | 固定尺寸或占位 |
| `Padding` | `padding`、`children` | 内边距 |
| `KV` / `Table` / `Tree` | 见库文档 | 键值 / 表格（列宽自动分配）/ 层级列表 |
| `Badge` / `Progress` | 见库文档 | 状态小标签 / 进度条 |
| `Control` | `control`（`buttons`/`select`/`checkbox`/`switch`/`text`/`number`）、`id`、`label`、`value`、`options`、`action` | 交互控件：**值变化即派发**动作（参数里带 `id` 与当前值） |
| `Icon` / `Image` / `Stack` / `Markdown` | 见库文档 | 可选组件：客户端不支持时由适配步骤降级（`Icon.glyph`、`Image.alt`） |
| `musicxx.Shader` | `bundle`、`args`、`speed`、`maxFps`、`animate`、`resolutionScale` | musicxx 专属：页面里画一块插件着色器，尺寸由父块决定；见 [plugin-shader-bundle.md](plugin-shader-bundle.md) §9 |

规则（解析层规则，与库文档一致）：

- 组件名（`kind`）是**首字母大写的驼峰**，解析时**忽略大小写**；
- 未知组件：有 `fallback` 就显示它，没有就跳过（不会报错）；未知字段忽略；未知枚举值取默认；
- 尺寸只有一个单位 `u`（允许小数）：图形界面 1u = 1 逻辑像素，终端按客户端的格大小换算；
  另有 `{"percent": 40}`（占父容器可用空间的比例）与 `"auto"`（内容决定）；
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
  "children": [ { "kind": "Progress", "value": { "kind": "source", "name": "plugin.demo.progress" }, "total": 100 } ] }
```

- 求值结果按 `u` 解释；写 `unit: "percent"`（或 `{"kind":"const","value":40,"unit":"percent"}`）
  就是"父容器比例"，与 `{"percent":40}` 一致；
- **文本字段**（`Text.text`、`Badge.text`、按钮文案、KV / Table / Tree 的文本…）也能写值表达式：
  求值结果当文本用，求值失败或没有上下文时用字面量作为回退（`{"kind":"format", …}` 可以把数值写进文案）；
- **公共字段 `visible`**（每个块都能写）：布尔字面量或值表达式，求值为假时这一块**不渲染**；
- 节点表、来源（`musicxx.theme.*` / `musicxx.icon.*` / `musicxx.spectrum.*` / `musicxx.env.*`、
  变量目录里的自定义键）、上限与降级规则见 [plugin-shader-bundle.md](plugin-shader-bundle.md) §7；
- 名字空间：`musicxx.` 前缀是框架保留的（派生来源与官方变量），**自定义名字允许含 `.`**
  （例如 `plugin.<插件id>.progress`）；
- 求值失败（未知 `kind`、参数写错）只**忽略该字段**并按缺省处理，日志里留一条原因；
- 当前版本：字段求值是"每次重画算一次"（`tween` / `lfo` 这类随时间变化的节点要等页面里的
  帧驱动接上才有动画效果）；着色器块（`musicxx.Shader`）本来就是每帧渲染，不受这一条限制。

### 2.3.2 动画（`musicxx.AnimatedBuilder` + 过渡块）

页面里要**逐帧动**的东西放在 `musicxx.AnimatedBuilder` 里：它声明一张**通道表**（名字 → 值表达式）
并驱动子树按帧重建，子树里的字段用 `{"kind":"source","name":"<通道名>"}` 读通道：

```jsonc
{ "kind": "musicxx.AnimatedBuilder",
  "maxFps": 30,                                  // 缺省 30，上限 30
  "values": {
    "card.h": { "kind": "mul", "of": [
        { "kind": "smooth", "attackMs": 20, "releaseMs": 260,
          "of": { "kind": "source", "name": "musicxx.spectrum.level" } },
        { "kind": "lfo", "shape": "sine", "periodMs": 2600, "from": 60, "to": 90 } ] },
    "card.intro": { "kind": "tween", "from": 0, "to": 1, "durationMs": 900, "ease": "outCubic" }
  },
  "children": [
    { "kind": "musicxx.FadeTransition", "value": { "kind": "source", "name": "card.intro" },
      "children": [
        { "kind": "musicxx.SizeTransition", "axis": "vertical", "value": { "kind": "source", "name": "card.intro" },
          "children": [ { "kind": "SizedBox", "height": { "kind": "source", "name": "card.h" } } ] } ] }
  ] }
```

| 块 | 字段 | 说明 |
|---|---|---|
| `musicxx.AnimatedBuilder` | `values`（通道表）、`maxFps`、`children` | 通道作用域 + 每帧重建；没有通道时零成本（不建帧调度） |
| `musicxx.SizeTransition` | `axis`（`vertical`/`horizontal`）、`axisAlignment`、`value`(0~1)、`curve`、`children` | 按进度把子块从 0 撑开 / 收拢 |
| `musicxx.FadeTransition` | `value`(0~1)、`curve`、`children` | 透明度 |
| `musicxx.SlideTransition` | `from`/`to`（`[x,y]`，单位 = 自身尺寸倍数）、`value`、`curve`、`children` | 位移 |
| `musicxx.ScaleTransition` | `from`/`to`（倍数）、`value`、`curve`、`children` | 缩放 |
| `musicxx.RotationTransition` | `from`/`to`（圈数）、`value`、`curve`、`children` | 旋转 |

规则与上限：

- `value` 可以写字面量、值表达式或通道引用；`curve` 是"通道没带缓动"时的简写（取
  `linear`/`inQuad`/`outQuad`/`inOutQuad`/`inCubic`/`outCubic`/`inOutCubic`/`inSine`/`outSine`/
  `inOutSine`/`outBack`/`outElastic`）；
- 每个 `AnimatedBuilder` 最多 **32 条通道**（超过的被忽略并记日志）；每帧只重建"读到值的块"；
- 帧调度走宿主的视口动画（**限帧 + 页面被遮挡 / 进后台时自动停**）；**动画等级低时只出构建那一帧**
  （等于静态，与主题的"低等级少动效"一致）；
- 过渡块只画自己（尺寸 / 透明度 / 位移 / 缩放 / 旋转），**子块完全不用改**；
- 状态（`smooth` 的上一帧值）跟着通道表走：同一份声明不被打断，声明变了重新开始；
- **没有 `AnimatedBuilder` 包裹的字段仍然是"每次重画算一次"**（插件可以用"能力返回新视图"刷新页面，
  但那是跳变，不是动画）。
- **kit 里有现成装配**：`kit.animScope({values, maxFps, children})` / `kit.fadeTransition({value, curve, children})` /
  `kit.sizeTransition({value, axis, curve, children})` / `kit.slideTransition({value, from, to, curve, children})` /
  `kit.scaleTransition({value, from, to, curve, children})` / `kit.rotationTransition({value, from, to, curve, children})`
  （参数与生成的说明见 `docs/musicxx-ui-kit.md`）。

### 2.4 用 kit 装配（推荐写法）

写页面内容不必手拼这些块：**随插件分发的界面 kit** 把常用组合封装好了。
JS 插件在清单里按顺序加载 kit（`scripts: [pluginxx_ui_kit.js, musicxx_ui_kit.js, plugin.js]`），
然后：

```js
const kit = pluginxx.ui.kit;

// env 是客户端能力段（取页面时宿主会放进参数：args.ui）；不传 = 中立描述，由客户端适配
kit.listRow({ title: "切歌次数", trailing: "3" }, env);
kit.settingRow({ title: "背景动画速率", depict: "改完立即生效", value: "1x" }, env);
kit.switchRow({ id: "skipAds", title: "跳过广告曲目", value: true, action: "setSkipAds" }, env);
kit.button({ label: "刷新本页", variant: "primary", action: "card" }, env);
```

C++ 插件用头文件里的 kit（命名空间 `musicxx::ui::kit`，由 `plugin_kit.h` 带进来）：

```cpp
namespace kit = musicxx::ui::kit;
Json row = kit.listRow(Json::object({{"title", "切歌次数"}, {"trailing", "3"}}));
blocks.push_back(pluginxx::ui::dumpItem(row));   // 序列化成 JSON 块
```

kit 组件一览（基础 kit + musicxx 扩展 kit 合并后）：`title` / `hint` / `text` / `badge` / `icon` /
`gap` / `divider` / `button` / `actionsRow` / `card` / `listRow` / `section` / `kv` / `table` /
`tree` / `sparkline` / `progressRow` / `settingRow` / `switchRow` / `inputRow` / `shaderBlock` /
`coverRow`（参数与生成的说明见 `docs/musicxx-ui-kit.md`）。

- kit 只做**装配**，不含逻辑、不写死客户端的规则；需要按格对齐时用 `kit.cols(n, env)` / `kit.rows(n, env)`；
- **kit 随插件目录分发**（宿主不提供）：`tools/sync_ui_kit.ps1` 会把两个 kit 文件复制进 JS 插件目录，
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
- **没有列表块**：列表行由 `Block` + `Row` + `Expanded` + `Column` + `Text` 组合，kit 的 `listRow`
  就是这个组合（示例见 `plugins/example_js/plugin.js`）。
- **右侧的"状态/值"文本要自己约束宽度**：kit 的 `listRow` / `settingRow` 把右侧文本放在行尾、
  **不给宽度约束**（那一格适合短状态词，例如"生效中 / 3 首 / 内置背景"）。要在那一格写长文本
  （一整句话、带歌曲名或样式名）就别用这两个模板，自己组合 `Row` + 两个 `Expanded`
  （左 3 右 2，两侧都能换行），或者把长文本放到左侧那格（它可以换行）。窗口一窄（手机竖屏、
  桌面拉小），行尾那段固定宽度的文本会把整行撑出可见区域（`RenderFlex overflow`），
  同时把左侧说明挤成一字一行、整页被拉得很长。
- **按钮文案别写太长**：按钮里的文字不换行，文案超过一行可用宽度时字会顶破按钮
  （同样是 `RenderFlex overflow`）；超过一行放不下就缩短文案，把细节写进旁边的说明文本。

### 2.6 页面之间的跳转与刷新

- 跳转：按钮 / 可点块上用 `{"kind":"route","route":"ext://<插件id>/<视图id>"}`；
  也可以跳到别的插件的页面（页面由对方绘制），或跳到少量官方页面
  （`musicxx:settings`、`musicxx:player`、`musicxx:search` 等，见 [plugin-js-api.md](plugin-js-api.md) §「打开官方页面」）；
- 刷新当前页：能力返回新的 `{"view": {...}}`，宿主直接用新视图重画当前页面（不用重新打开）；
- 每个插件视图是**独立页面**（框架按路由 + 视图 id 区分），从 A 页跳到 B 页后返回能回到 A 页。

---

## 3. 设置页与配置

框架**不提供**配置表单、也**不管理**插件的设置入口（没有「插件设置」类型或列表）：

- 设置界面就是插件自己注册的一个普通页面（`ext://<插件id>/<视图id>`）；
- 入口由插件自己给：主页入口（`home.entry`）或某个功能页里的按钮（`route` 动作）；
- 配置值存在插件目录的 `config.json` 里，默认值由插件自己给：

| 语言 | 读 | 写 | 配置文件路径 |
|---|---|---|---|
| JS | `musicxx.storage.getConfig(key, 默认值)` | `musicxx.storage.setConfig(key, value)` | 插件目录下的 `config.json` |
| C++ | 自己读 `configPath()` 指向的文件 | 同左 | `configPath()` |

要点：

- **默认值写在插件里**（`getConfig` 的第二个参数），读不到就用默认值 —— 框架不再保存默认值，也没有
  清除「用户改过的值」之外的恢复手段；
- 值改动后要让界面反映出来：改完再调一次能力返回 `{view: ...}`，宿主就会用新视图刷新（示例见
  `plugins/example_js_shader/plugin.js` 的 `cycleBackgroundRate`）；
- 私有 KV（JS：`musicxx.storage.get/set/remove/list`，不带 `namespace`）与 `config.json` 是两份数据：
  KV 适合缓存、运行计数这类插件自己的数据，`config.json` 适合「用户看得懂、可能手改」的设置项。

---

## 4. 可照抄的示例

| 示例 | 演示了什么 |
|---|---|
| `plugins/example_js/plugin.js` | 主页入口 + 歌曲菜单项；`card` 功能页与 `settings` 设置页；配置读写与页面刷新；页面内容用随插件分发的 kit 装配 |
| `plugins/example_js_shader/plugin.js` | 播放页背景样式（两种：晶格 / 光圈）+ 速率设置页 + 页面里内联的 `musicxx.Shader` 块（见 [plugin-shader-bundle.md](plugin-shader-bundle.md)） |
| `plugins/example_native/example_native.cpp` | 同样的 UI 能力（C++）：主页入口、歌曲菜单、`card` 页、用 `musicxx::ui::kit` 装配内容 |
| `plugins/example_js_multi_script/` | 清单 `scripts` 多脚本装载：先跑随插件分发的 kit，再跑插件脚本 |
