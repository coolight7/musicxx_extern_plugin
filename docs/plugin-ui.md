# 插件界面：UI 项、插件页面与设置页

插件**不写 Flutter 代码**。界面有两个层次，都只做声明、由 musicxx 渲染：

| 层次 | 插件做什么 | 宿主渲染在哪 |
|---|---|---|
| **UI 项** | 注册入口 / 菜单项（类型 + JSON 内容） | 宿主自己的界面：音乐主页入口列表、歌曲菜单、设置里的「播放页面背景」列表 |
| **插件页面** | 注册一个能力，返回视图描述（`{view:{title, blocks}}`） | `ext://<插件id>/<视图id>` 路由页面 |

两边的写法只有 API 名字不同（C++ 用 `uiRegister`/`capability`，JS 用 `musicxx.ui.registerEntry`/`musicxx.capability.register`），
**字段与块类型完全一致**，所以这份文档对两种插件都适用。语言相关的 API 见
[plugin-native-api.md](plugin-native-api.md)（C++）与 [plugin-js-api.md](plugin-js-api.md)（JS）。

---

## 1. UI 项

### 1.1 四种类型

| 类型（全名） | `data` 字段 | 渲染位置 | 当前版本的实际表现 |
|---|---|---|---|
| `musicxx.ui.home.entry` | `title`（必填）、`subtitle?`、`icon?`、`action?` | 音乐主页的入口列表（排在宿主内置入口之后，用户可以在主页排序里拖动） | 只显示 `title`：`subtitle` 与 `icon` 目前未使用（图标位留空） |
| `musicxx.ui.song.action` | `title`（必填）、`icon?`、`action?` | 歌曲列表项的「歌曲菜单」 | 显示 `title`；`icon` 写宿主内置的 svg 名（不写用默认图标；写不存在的名字不显示图标）；副标题位置固定显示 `外部插件：<插件id>` |
| `musicxx.ui.playlist.action` | `title`（必填）、`icon?`、`action?` | 歌单菜单 | **当前版本没有渲染入口**：注册会成功，但界面上看不到（见 §1.4） |
| `musicxx.ui.playing.background` | 见 [plugin-shader-bundle.md](plugin-shader-bundle.md) | 「设置 → 播放页面背景」的样式列表 | 用户选中后生效 |

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
- 每个插件最多 **64 个** UI 项，单条 `data` 最多 **64 KiB**；
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

| `kind` | 字段 | 行为 |
|---|---|---|
| `route` | `route: "ext://<插件id>/<视图id>"` | 打开插件页面（**允许指向任意插件**：页面由被跳转插件自己绘制，发起方拿不到对方的数据） |
| `capability` | `name: "<短名>"`, `args?` | 调用**本插件**的能力；返回视图描述时直接打开页面（入口与页面可以合并成一次调用） |
| `action` | `name: "musicxx.<域>.<动作>"`, `args?` | 执行宿主动作（与插件自己调 `musicxx.call` 同一份实现） |
| `none` / 不写 | — | 纯展示项，点了没有动作 |

- 注册时校验：`route` 必须 `ext://` 开头且带插件 id 与视图 id；`capability` 必须有非空 `name`；
  `action` 的 `name` 必须是官方 `musicxx.*`；
- **动作指向的能力必须真的注册过**：没注册时宿主提示
  「插件『<插件id>』没有提供『<能力名>』」，`example_native` / `example_js` 的 `card` 页是可照抄的例子；
- `capability` 动作成功时的表现：返回视图描述就直接打开页面（入口与页面合成一次调用），
  否则提示「『<入口标题>』执行完成」。

### 1.4 当前版本的渲染缺口（写插件前先确认）

- `musicxx.ui.playlist.action`：注册成功，但应用侧没有渲染点，界面上不会出现；
- `home.entry` 的 `subtitle` / `icon`：主页入口按钮当前只画标题；
- 主页入口的文字就是它显示在按钮上的样子：想换文案就改 `title`；需要图标生效就用歌曲菜单项
  （`song.action` 的 `icon` 是生效的）。

想确认自己注册了哪些项：`musicxx.ui.entries()`（JS）/ `uiEntries()`（C++）读自己注册的项；
应用侧快照是 `musicxx_extern_plugin_ui_snapshot`（应用开发者/调试用）；界面表现直接看主页、
歌曲菜单与「设置 → 播放页面背景」列表。

---

## 2. 插件页面（`ext://<插件id>/<视图id>`）

### 2.1 打开过程

1. 用户点入口 / 插件自己调 `musicxx.ui.openRoute("ext://my_plugin/settings")`；
2. 宿主打开路由页面，调用**与被跳转插件同名的能力**：视图 id = 能力短名
   （`ext://my_plugin/settings` → 能力 `plugin.my_plugin.settings`），参数 `{"view": "<视图id>"}`；
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

### 2.3 块类型

块类型名（`kind`）是**首字母大写的驼峰**，解析时**忽略大小写**（`Text` / `text` / `TEXT` 等价）；
认不出来的块被忽略（向前兼容，不会报错）。

| `kind` | 字段 | 说明 |
|---|---|---|
| `Text` | `text`、`style`（`main` 默认 / `cross` / `thin`） | 一段文字 |
| `Divider` | — | 分隔线 |
| `Button` | `title`、`style`（`normal` 默认 / `primary`）、`action` | 按钮，点击执行自己的 `action` |
| `Block` | `child`、`inContent`、`margin`、`padding`、`action` | 内容块（应用的卡片：底色 + 圆角 + 边距），行与分组放它里面；`inContent: true` 用内容块内的浅色底（适合列表行）；不带 `margin`/`padding` 时用宿主默认边距 |
| `Row` | `children` | 横向排列（要占满剩余宽度就用 `Expanded`） |
| `Column` | `children` | 纵向排列（从左边开始） |
| `Expanded` | `child`、`flex`（默认 1，小于 1 按 1） | 占满剩余空间；**只在 `Row` / `Column` 的直接子块位置生效**，放别处退化成普通子块 |
| `SizedBox` | `width` / `height`、`child` | 固定尺寸或占位（不写 `child` 就是纯占位） |
| `Padding` | `left` / `right` / `top` / `bottom`（单边写法）或 `padding`（数字 = 四边相同，或对象 `{all, horizontal, vertical, left, right, top, bottom}`）、`child` | 内边距；两种写法都写时以单边字段为准 |
| `Shader` | `bundle`、`args`、`speed`、`maxFps`、`animate`、`resolutionScale` | 页面里画一块插件着色器，尺寸由父块决定；见 [plugin-shader-bundle.md](plugin-shader-bundle.md) §9 |

`Block` 的 `margin` / `padding` 与其它布局数值一样接受**数字**（四边相同）或对象
（`all` / `horizontal` / `vertical` / `left` / `right` / `top` / `bottom`）。

### 2.4 布局约定

- 所有尺寸与边距都是**设计像素**：宿主按屏幕尺寸换算（和页面里其它内容一样随窗口缩放），
  不是固定的物理像素。习惯写法：左右留白 `50`、行间距 `20~30`、间隔用小 `SizedBox`；
- 任意块都可以带 `action`（`Button` 用自己的按钮点击语义）：带上之后**整块可点**，
  用来组合「可点的行」；
- 可点区域不做高亮反馈以外的额外视觉处理，所以可点的行建议放在 `Block` 里（有底色的卡片），
  用户才知道那是一行；
- **没有列表块**：列表行由 `Block`（内容块）+ `Row` + `Expanded` + `Column` + `SizedBox` + `Text`
  组合出来，见下面的 `infoRow`。

### 2.5 一行设置的排版（`infoRow`）

左边「标题 + 说明」占满剩余宽度，右边是状态文字；外面套内容块（卡片），需要整行可点就在
`Block` 上加 `action`。C++ 版见 `plugins/example_native/example_native.cpp` 的 `infoRowJson`，
JS 版见 `plugins/example_js/plugin.js` 的 `infoRow`：

```js
function infoRow(title, subtitle, right, action) {
    const left = {
        kind: "Column",
        children: subtitle
            ? [{ kind: "Text", text: title },
               { kind: "SizedBox", height: 6 },
               { kind: "Text", text: subtitle, style: "cross" }]
            : [{ kind: "Text", text: title }],
    };
    const children = [{ kind: "Expanded", child: left }];
    if (right) {
        children.push({ kind: "SizedBox", width: 30 });
        children.push({ kind: "Text", text: right, style: "cross" });
    }
    const block = {
        kind: "Block",
        inContent: true,
        margin: { left: 50, right: 50, bottom: 20 },
        child: { kind: "Row", children: children },
    };
    if (action) {
        block.action = action;   // 整行可点
    }
    return block;
}
```

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
| `plugins/example_js/plugin.js` | 主页入口 + 歌曲菜单项；`card` 功能页与 `settings` 设置页；配置读写与页面刷新 |
| `plugins/example_js_shader/plugin.js` | 播放页背景样式 + 速率设置页 + 页面里内联的 `Shader` 块（见 [plugin-shader-bundle.md](plugin-shader-bundle.md)） |
| `plugins/example_native/example_native.cpp` | 同样的 UI 能力（C++）：主页入口、歌曲菜单、`card` 页、`infoRowJson` |
