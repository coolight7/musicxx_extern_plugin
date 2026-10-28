# 插件界面：UI 项、插件页面与设置页

插件**不写 Flutter 代码**。界面有两个层次，都只做声明、由 musicxx 渲染：

| 层次 | 插件做什么 | 主程序渲染在哪 |
|---|---|---|
| **UI 项** | 注册入口 / 菜单项（类型 + JSON 内容） | 主程序自己的界面：音乐主页入口列表、歌曲菜单、设置里的「播放页面背景」列表 |
| **插件页面** | 注册一个能力，返回视图描述（`{view:{title, blocks}}`） | `ext://<插件id>/<视图id>` 路由页面 |

两边的写法只有 API 名字不同（C++ 用 `uiRegister`/`capability`，JS 用 `musicxx.ui.registerEntry`/`musicxx.capability.register`），
**字段与块类型完全一致**，所以这份文档对两种插件都适用。语言相关的 API 见
[plugin-native-api.md](plugin-native-api.md)（C++）与 [plugin-js-api.md](plugin-js-api.md)（JS）。
第一次写插件、想知道从哪一步开始，先看 [plugin-guide.md](plugin-guide.md)。

---

> **术语约定**：本文档中的**主程序**指的就是 **musicxx**（Flutter/Dart 业务层）；外置插件的 C++ 框架层称为**插件框架**；插件目前分为**动态库插件**与**JS插件**。


## 1. UI 项

### 1.1 六种类型

| 类型（全名） | `data` 字段 | 渲染位置 | 当前版本的实际表现 |
|---|---|---|---|
| `musicxx.ui.home.entry` | `title`（必填）、`subtitle?`、`icon?`、`action?` | 音乐主页的入口列表（排在主程序内置入口之后，用户可以在主页排序里拖动） | `title` 显示在按钮上、`subtitle` 显示在下一行（小字）；`icon` 写**插件自己带的 svg 路径**（相对插件目录，见 §1.5；不写用默认图标，文件缺失也退回默认图标） || `musicxx.ui.song.action` | `title`（必填）、`icon?`、`action?` | 歌曲列表项的「歌曲菜单」 | 显示 `title`；`icon` 同上；副标题位置固定显示 `外部插件：<插件id>` |
| `musicxx.ui.playlist.action` | `title`（必填）、`icon?`、`action?` | 歌单页（歌曲列表页）的列表菜单 | 显示 `title`；动作参数里会带上这张歌单的 `pid` / `name` / `songNum` |
| `musicxx.ui.addsong.entry` | `title`（必填）、`subtitle?`、`icon?`、`action?` | 歌曲列表页 [+] → 「添加歌曲」弹窗（在**内置来源之后**、“从其他歌单导入”这类工具项之前） | 显示 `title` / `subtitle`；动作参数同样带 `pid` / `name` / `songNum`。常见写法：`command` 动作指向 `musicxx.ui.openOverlaySheet`，把插件自己的挑歌视图打开成底部浮层（见 [plugin-api.md](plugin-api.md) §3.2.4.2） |
| `musicxx.ui.playing.background` | 见 [plugin-shader-bundle.md](plugin-shader-bundle.md)（封面纹理 `cover` 见该文 §5.1） | 「设置 → 播放页面背景」的样式列表 | 用户选中后生效 |
| `musicxx.ui.playing.icon` | 见 [plugin-shader-bundle.md](plugin-shader-bundle.md) §5.3 | 「设置 → 主题 → 播放页歌曲图」的接管项 | 选中后由该项接管播放页中间的歌曲图：`shader`（着色器绘制）/ `view`（界面组合）/ `none`（只占位、不显示内容） |

`icon` 写的是**插件自己带的 svg**：一条**相对插件目录**的路径（见 §1.5），**不能**写主程序
（musicxx）自己的图标名 —— 主程序的 svg 名字不会命中任何文件，等于"没有图标"，各渲染点会按
各自的规则退回默认图标。

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

规则（插件框架在注册时校验，写错就直接失败，不会等到渲染时才发现）：

- `name` 用**短名**，插件框架补成 `plugin.<插件id>.<短名>`；写全名时也必须属于本插件命名空间（冒充别人返回 `-6`）；
- 同一个项名再次注册 = **覆盖**（保留原来的排序位置）；
- UI 项数量与单条 `data` 大小都不设上限（早期版本的 64 个 / 64 KiB 已移除）；
- 类型必须是上表六种之一（`-4`），`data` 必须是 JSON 对象（`-1`），需要的 `title` 不能为空（`-1`）；
- 变化会推送 `musicxx.ui.changed` 事件（载荷带该插件的**全部**项，主程序整批替换）。

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
| `{"kind":"route"}` | `route: "ext://<插件id>/<视图id>"` | 打开插件页面，**允许指向任意插件**（含自己）：跨插件时页面由对方插件自己绘制、发起方拿不到对方的数据。与动作 `musicxx.ui.openRoute` 同一套规则 |
| `{"kind":"command"}` | `name: "musicxx.<域>.<动作>"`, `args?` | 执行主程序动作（与插件自己调 `musicxx.call` 同一份实现） |
| `{"kind":"none"}` / 不写 | — | 纯展示项，点了没有动作 |

- 注册时校验：`route` 必须 `ext://` 开头且带插件 id 与视图 id；`dispatch` 必须有非空 `name`；
  `command` 的 `name` 必须是官方 `musicxx.*`；
- **动作指向的能力必须真的注册过**：没注册时主程序提示
  「插件『<插件id>』没有提供『<能力名>』」，`example_native` / `example_js` 的 `card` 页是可照抄的例子；
- `dispatch` 动作成功时的表现：返回视图描述就直接打开页面（入口与页面合成一次调用），
  否则提示「『<入口标题>』执行完成」；
- 动作参数里主程序会补上当前上下文：歌曲菜单项补 `sid`/`name`/`artist`，歌单菜单项补
  `pid`/`name`/`songNum`（插件自己写的 `args` 保留，冲突时以主程序补的为准）。

### 1.4 当前版本的渲染缺口（写插件前先确认）

这一版把之前缺的渲染点补齐了：`home.entry` 的 `subtitle` / `icon` 都会画出来、`playlist.action`
在歌单页的列表菜单里有位置。仍要注意的是：

- 入口与菜单项的图标只能来自插件自己（相对插件目录的路径，见 §1.5），写主程序图标名等于没写；
- 主页入口的文字是单行省略（标题、副标题各一行），别写长句；
- 想确认自己注册了哪些项：`musicxx.ui.entries()`（JS）/ `uiEntries()`（C++）。

### 1.5 图标：插件自己带 svg

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
  文件，也不会报错；
- **不能引用主程序（musicxx）自己的图标名**：`host.info().ui` 里没有 `icons` 清单（musicxx 不发布），
  写主程序的 svg 名字只会在插件目录里找一个同名文件，找不到就按"没有图标"处理 ——
  主页入口 / 菜单项退回默认图标，页面里的 `Icon` 块退回 `glyph` 文本；**按钮例外**：
  `Button.icon` 没有回落，文件缺失就是按钮里一个空位（框架只拼路径，不替插件判文件在不在）；
- 路径由**框架**拼接：插件在 JSON 里只写相对路径，客户端拿到内容、构造界面时才拼上自己的插件
  目录（所以插件目录换到别的地方、同一个插件包在哪台机器上装，声明都不用改）；
- 颜色：主页入口 / 菜单项按 svg 自己的颜色画；页面里的 `Icon` 与 `Button` 会按 `tone`
  **单色渲染**（`BlendMode.srcIn`），这两个位置用单色的形状图标（不带渐变、不带多色）更合适；
- 图标随插件目录一起分发（打包时把图标目录放进插件包；动态库插件在 `CMakeLists.txt` 里把
  资源目录写进 `ASSETS`，见 [plugin-native-api.md](plugin-native-api.md)）。

---

## 2. 插件页面（`ext://<插件id>/<视图id>`）

### 2.1 打开过程

1. 用户点入口 / 插件自己调 `musicxx.ui.openRoute("ext://my_plugin/settings")`；
2. 主程序打开路由页面，调用**与被跳转插件同名的能力**：视图 id = 能力短名
   （`ext://my_plugin/settings` → 能力 `plugin.my_plugin.settings`），参数是
   `{"view": "<视图id>", "ui": {客户端界面能力段}}`；
3. 能力返回视图描述：`{"view": {...}}`（推荐）或直接返回视图对象；
4. 主程序渲染；能力返回的每个块都不需要插件再调用任何接口。

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
  "title": "页面标题",          // 与 blocks 不能同时为空（都空 = 主程序提示插件没返回可用视图）
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
| `Button` | `label`、`variant`（`primary`/`secondary`/`ghost`/`link`）、`icon`、`disabled`、`action` | 按钮，点击执行自己的 `action`；`icon` 是插件自带的 svg 路径（相对插件目录，见 §1.5） |
| `Block` | `title?`、`variant`（`card`/`inset`/`plain`）、`padding`、`margin`、`action`、`children` | 内容块：`card` 有底色 + 圆角，`inset` 是浅色底（要分组时用），`plain` 只有声明的留白（卡片里的列表行用它，行与行直接按列表排列） |
| `Row` / `Column` | `gap`、`main`（含 `spaceBetween`）、`cross`、`action`、`children` | 横向 / 纵向排列 |
| `Expanded` / `Spacer` | `flex`、`children` | 按比例分剩余空间 / 纯占位 |
| `SizedBox` | `width`、`height`、`aspect`、`children` | 固定尺寸或占位 |
| `Padding` | `padding`、`children` | 内边距 |
| `KV` / `Table` / `Tree` | 见库文档 | 键值 / 表格（列宽自动分配）/ 层级列表 |
| `Badge` / `Progress` | 见库文档 | 状态小标签 / 进度条 |
| `Control` | `control`（`buttons`/`select`/`checkbox`/`switch`/`text`/`number`/`password`）、`id`、`label`、`help`、`value`、`options`、`multiline`、`disabled`、`action` | 交互控件：**值变化即派发**动作（参数里带 `id` 与当前值）。`text`/`number`/`password` 用应用自己的输入框：**回车或失焦才派发**（不是逐字派发），`password` 默认遮住（右侧有眼睛按钮），纯文本降级固定打码；`select` 候选项只有一行标题（要“标题 + 多行描述”就用浮层的 `picker`） |
| `ListViewBase` | `items`（必填）、`layout`、`variant`、`height`、`padding`、`header`、`footer`、`emptyText`、`itemAction` | 真正的列表：列表项 `[{id?, title, subtitle?, trailing?, image?, icon?, tone?, selected?, path?, isDir?, custom?, action?}]`；`layout` 是 `auto`（跟应用设置）/`list`/`grid`/`multiList`；不给 `height` 就跟页面一起滚（条目多时给高度让它自己滚）。**只显示，没有工具条**；`custom` 是“整项内容用任意块画”，见 §2.5.1 |
| `ListView` | 同上 + `toolbar`、`menu`、`itemMenu`、`editAction`、`saveAction`、`addAction`、`removeAction`、`removeCheckedAction`、`removeTipTitle`、`positionAction`、`refreshAction`、`loadMoreAction`、`enableCheck`、`enableDrop`、`showCount` | 带工具条的列表（编辑 / 多选 / 加删 / 定位 / 菜单 / 下拉刷新 / 加载更多）：**只有写了对应字段才出现那一个按钮**，没写的自动不出；详见 §2.3.3 |
| `Icon` / `Image` / `Stack` / `Markdown` | 见库文档 | 可选组件：客户端不支持时由适配步骤降级（`Icon.glyph`、`Image.alt`）；`Icon.name` 是插件自带的 svg 路径（相对插件目录，见 §1.5） |
| `musicxx.DirListView` | `path`、`items`（必填，用法同列表，可带 `path`/`isDir`）、`multi`（默认关）、`showPath`、`pathAction`、`itemAction`、`selectAction`、`refreshAction`、`loadMoreAction`、`height`、`padding`、`variant`、`emptyText` | musicxx 专属：目录列表（**直接用应用内置的 `DirList` 画**）—— 一层目录的条目（文件夹/文件图标、标题 + 第二行、行尾文字）+ 左上角条目数 + 顶部路径栏（显示 `path`）+ 路径栏里的「上一级」按钮（给了 `pathAction` 才出现）+ 可选的多选勾选框（`multi` 打开才出现，勾选态取条目的 `selected`，点一下派发取反后的 `selected`）。给了 `refreshAction` / `loadMoreAction` 还能**在列表内下拉刷新 / 滚到底加载更多**（不加任何按钮）。**当前目录与选中态由插件保存**（客户端只派发动作，插件返回新视图）；返回键是“退出页面 / 浮层”（本块不自己进目录，要逐层往上退就给 `pathAction`）。行样式由应用定：`icon` / `subtitle` / `trailing` / `selected` 生效，`tone` / `image` / `custom` 在目录列表里不生效。不认识的客户端退成普通列表文本 |
| `musicxx.AdaptLayout` | `columnBody`（必填）、`rowBody`（必填）、`columnSecondBody`、`rowSecondBody`、`rowBodyRatio`（0.5）、`columnBodyRatio`（0.5）、`enableAnimations` | musicxx 专属：按窗口宽高比切换两套内容（宽高比 < 1 用 `columnBody`，否则 `rowBody`），给了第二套就按比例分两栏；对应应用内置的 `XXAdaptiveLayout`。不认识的客户端用 `rowBody` |
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
- 帧调度走主程序的视口动画（**限帧 + 页面被遮挡 / 进后台时自动停**）；**动画等级低时只出构建那一帧**
  （等于静态，与主题的"低等级少动效"一致）；
- 过渡块只画自己（尺寸 / 透明度 / 位移 / 缩放 / 旋转），**子块完全不用改**；
- 状态（`smooth` 的上一帧值）跟着通道表走：同一份声明不被打断，声明变了重新开始；
- **没有 `AnimatedBuilder` 包裹的字段仍然是"每次重画算一次"**（插件可以用"能力返回新视图"刷新页面，
  但那是跳变，不是动画）。
- **kit 里有现成的拼装写法**：`kit.animScope({values, maxFps, children})` / `kit.fadeTransition({value, curve, children})` /
  `kit.sizeTransition({value, axis, curve, children})` / `kit.slideTransition({value, from, to, curve, children})` /
  `kit.scaleTransition({value, from, to, curve, children})` / `kit.rotationTransition({value, from, to, curve, children})`
  （参数与生成的说明见 `docs/musicxx-ui-kit.md`）。

### 2.3.3 列表块（`ListViewBase` / `ListView`）

列表项一般是**数据**（没有 `kind`）；写了 `custom` 时它的内容是块。两个块用同一套列表项：

```jsonc
{ "id": "weekly", "title": "每周新歌", "subtitle": "每周一更新", "trailing": "12 首",
  "image": "cover", "icon": "resource/svg/star.svg", "tone": "hint", "selected": false,
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
| `tone` | 主标题语义色 |
| `selected` | 是否高亮（**只由插件给的数据决定**，客户端不做点击自动高亮） |
| `path` / `isDir` | 这一项对应的路径 / 身份与“是不是目录项”（目录列表用它，动作参数里原样回带） |
| `action` | 点这一项派发的动作（不给就用块上的 `itemAction`） |

所有列表动作的参数里都会带上：`listId`（块 id）、`index`、`itemId`、`item`（整项快照，含 `path` / `isDir`）。
目录列表块（`musicxx.DirListView`）把这些直接放在参数顶层（`path` / `isDir`），勾选框那个动作另带 `selected`。

> 目录列表的**行样式由应用定**（与内置目录列表一致：前置图标 + 标题 + 第二行 + 行尾文字 / 勾选框），
> 所以 `tone` / `image` / `custom` 在它里面不生效，`subtitle` 没写时第二行显示 `path`。

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
kit.button({ label: "刷新本页", variant: "primary", action: "card" }, env);
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
- **行内的开关 / 勾选框要放在有宽度的盒子里**：`Control` 的 `switch` / `checkbox` 落在"宽度不限"
  的位置时行为随客户端实现而异（历史上出现过 `RenderFlex ... unbounded` 的断言）。稳妥写法是给它一个
  **固定宽度的 `SizedBox`**（例如 190 个 `u`，与开关自身宽度相当）；"几选一"优先用一排 `Button` 块
  （每个块各自声明 `{id, value}` 参数、当前项用 `variant: "primary"` 高亮），不要用 `Control(buttons)`。

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
    { "kind": "Expanded", "children": [
        { "kind": "ListViewBase", "items": [
            { "custom": [ /* 这一段很长，列表自己滚 */ ] },
            { "custom": [ /* 另一段 */ ] } ] } ] },
    { "kind": "Button", "label": "常驻在底部的按钮", "action": "apply" } ] }
```

规则（主程序按这个形状判断，浮层与插件页面都一样）：

- 触发“填满高度”的条件有三种：**顶层直接写 `Expanded`**、**顶层直接写 `musicxx.AdaptLayout`**，
  或者**顶层只有一个 `Column` 且它里面有 `Expanded` / `musicxx.AdaptLayout`**；其它写法保持
  “整页一起滚”，不会因为忘了写 `Expanded` 而溢出；
- 这种描述要求客户端给**有界高度**：插件页面与浮层内容区都给了（浮层建议用
  `size: "full"` 打开，宽高都是可用尺寸的 90%，见 §2.7；高度铺满、宽度随内容就用 `fullHeightAutoWidth`）；
  对话框那种自由高度里客户端会退回普通排布；
- 列表块的 `height` 不给 = 跟随父给的可用高度（放在 `Expanded` 里时自己滚）；写了 `height` 就
  固定那个高度（不依赖父容器）；
- 列表块落在**没有可用高度**的地方（例如整页滚动里）时按内容高度铺开 —— 不报错，滚的只有页面；
- `musicxx.AdaptLayout` 靠“按窗口宽高比选一套 + 分两栅”工作，需要客户端给出可用尺寸：顶层直接
  写它时主程序会把它放进 `Expanded`；把它塞进 `Block` / 行内时只画当前该显示的那一套（不报错，
  也不会切换布局）；
- **固定部分会吃掉窗口高度**：头部与底部不会缩放，窗口很矮（桌面拉到几百像素高）时它们自己就可能
  超出内容区（客户端不会自动改回整页滚）。所以头部只放必要的几行；
- **kit 模板**：`kit.dirListView({path, items, itemAction, refreshAction, loadMoreAction})`、
  `kit.adaptLayout({columnBody, rowBody})`。

### 2.6 页面之间的跳转与刷新

- 跳转：按钮 / 可点块上用 `{"kind":"route","route":"ext://<插件id>/<视图id>"}`（**允许指向任意插件**，
  含自己；页面由目标插件绘制，发起方拿不到对方的数据）。要跳**官方页面**
  （`musicxx:settings`、`musicxx:player`、`musicxx:search` 等）只能用动作 `musicxx.ui.openRoute`
  （白名单见 [plugin-api.md](plugin-api.md) §3.2.4）；UI 项与页面里的动作走的是**同一套判定**；
- 刷新当前页：能力返回新的 `{"view": {...}}`，主程序直接用新视图重画当前页面（不用重新打开）；
- 每个插件视图是**独立页面**（框架按路由 + 视图 id 区分），从 A 页跳到 B 页后返回能回到 A 页。

---

### 2.7 浮层与确认弹窗：用动作打开

**弹层属于“有过程、有结果”的事，所以走动作**（不往页面里加“浮层块”）。两种写法：

```js
// 1) 页面里的一行直接打开浮层（零代码）：那一行带 command 动作，参数就是动作参数
//    {"kind":"command","name":"musicxx.ui.overlay.show","args":{...}}
//    kit 的 overlayRow 已经包好这一层

// 2) 插件拿到结果后再改页面：那一行用 dispatch，处理函数里自己调
musicxx.capability.register("pickQuality", async function () {
  const r = await musicxx.ui.pick({
    title: "音质",
    value: current,
    options: [{ value: "high", title: "高品质", describe: "文件较大，音质最好" }],
  });
  if (r && !r.cancelled) current = r.selected;
  return { view: buildView() };      // 返回新视图，页面立刻更新
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
- 每个插件同时只有**一层**浮层（再开会关掉旧的）；插件卸载 / 插件页面退出会自动关；
- 浮层里的按钮动作**不会自动关闭浮层**：插件返回 `{"view": …}` 就能在浮层里换内容（做翻页 / 刷新），
  要关自己调 `overlayClose`；`textConfirm` 那个按钮一定关闭；
- `pick` 只做单选；多选就在自己的 `blocks` 里放 `Control(checkbox)` 或列表块；
- 浮层 / 确认弹窗的外观（圆角、遮罩、尺寸预设、动画等级）跟应用主题走，插件不控样式。

---

## 3. 插件自己的设置页与配置

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
- 值改动后要让界面反映出来：改完再调一次能力返回 `{view: ...}`，主程序就会用新视图刷新（示例见
  `plugins/example_js_shader/plugin.js` 的 `cycleBackgroundRate`）；
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
2. **加载并注册插件自带的字体文件**：
   插件可以将 `.ttf` / `.otf` 字体文件随插件目录分发（例如 `resource/fonts/my_font.ttf`），在运行时动态加载并生效：
   ```js
   // 加载并立即设为全局字体
   await musicxx.ui.loadFont({
     family: "MyPluginFont",
     path: "resource/fonts/my_font.ttf", // 相对插件目录的相对路径
     apply: true,                         // 可选，默认 true
   });
   ```
3. **通过官方变量读取/设置**：
   ```js
   // 读当前生效字体
   var current = await musicxx.vars.get("musicxx.ui.fontFamily");
   // 设置字体
   await musicxx.vars.set("musicxx.ui.fontFamily", "SimSun");
   ```
4. **生命周期自动恢复**：
   当设置字体的插件被停用或卸载、或插件框架停止时，主程序会自动恢复为默认字体，避免残留不可用的字体配置。

### 4.1 在页面按钮里切换字体：**处理器里不要 `await` 这些动作**

换字体会重建整棵界面、耗时几百毫秒到几秒。页面按钮走的是 `dispatch` →
`plugin_call`，**应用线程全程阻塞等这次能力调用返回**；处理器里 `await`
`musicxx.ui.setFontFamily` / `loadFont` 就会与调用方互锁：

- 主程序表现：点一下**整个程序卡几秒**，然后插件收到 `action_timeout` 报"切换失败"，
  可字体其实已经切过去了（动作是在超时之后才轮到应用线程处理的）；
- 正确写法（可照抄 `plugins/example_js_font/plugin.js`）：
  1. **立刻返回**：先按新模式画页面，处理器直接返回视图；
  2. 动作**后台执行**：`musicxx.ui.setFontFamily(...)` 只跟 `.then(...)`，不 `await`；
  3. 成功后把模式写进自己的状态与 `config.json`；失败就回滚显示、异步提示（那时调用早已返回，
     用 `musicxx.ui.notify` 这类动作是安全的）；
  4. 应用在 `dispatch` 拿到视图后 **500ms 会再静默取一次页面**，所以后台改好的真实状态会自动显示，
     插件不需要额外做刷新逻辑（原理见 `ExternPluginViewBody._scheduleQuietReload`）。
- 想知道"主程序现在到底是什么字体"：`musicxx.vars.watch("musicxx.ui.fontFamily")` 保活后
  用 `musicxx.vars.peek("musicxx.ui.fontFamily")` **同步**读缓存（值形如 `{value, revision, ageMs, stale}`），
  不要为了显示状态再 `await` 一次 `vars.get`。

> 这条规则对**所有**"改应用状态"的动作都成立（播放页背景、播放控制、存储写入…）：能力处理器必须
> 快速返回，长活儿发给动作后在 `.then` 里收尾。处理器返回 Promise 时插件框架会一直等它结算
> （`plugin_call` 的等待上界是 5 秒，超时就是上面那种"卡几秒 + 失败"）。

---

## 5. 可照抄的示例

| 示例 | 演示了什么 |
|---|---|
| `plugins/example_js/plugin.js` | 主页入口 + 歌曲菜单项；`card` 功能页与 `settings` 设置页；配置读写与页面刷新；页面内容用随插件分发的 kit 拼装 |
| `plugins/example_js_font/plugin.js` | 全局字体切换（微软雅黑 / 字体文件示例 / 拟声默认）+ 文字与输入框字体预览；**按钮动作"立刻返回 + 后台执行"的样板**（见 §4.1） |
| `plugins/example_js_shader/plugin.js` | 播放页背景样式（两种：晶格 / 光圈）+ 速率设置页 + 页面里内联的 `musicxx.Shader` 块（见 [plugin-shader-bundle.md](plugin-shader-bundle.md)） |
| `plugins/example_native/example_native.cpp` | 同样的 UI 能力（C++）：主页入口、歌曲菜单、`card` 页、用 `musicxx::ui::kit` 拼装内容 |
| `plugins/example_js_multi_script/` | 清单 `scripts` 多脚本加载：先跑随插件分发的 kit，再跑插件脚本 |
