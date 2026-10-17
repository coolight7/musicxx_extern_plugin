# musicxx JS 插件作者指南（v1）

JS插件是**零编译**形态：一个目录（`plugin.yaml` + `plugin.js`）即可，插件框架用内置的 QuickJS
运行时执行脚本，脚本通过全局对象 `musicxx` 使用框架与主程序能力。

| 主题 | 文档 |
|---|---|
| **两种形态共用的接口**（状态镜像 / 动作 / 事件 / 变量 / 能力 / 功能点 / 钩子写法 / 约定 / 排障） | [plugin-api.md](plugin-api.md) |
| 总入口：选形态 / 环境准备 / 第一个插件 / 注意事项 / 发布检查 | [plugin-guide.md](plugin-guide.md) |
| 钩子 id / 模式 / 派发，以及已接入钩子的载荷与裁决语义 | [plugin-hooks.md](plugin-hooks.md) |
| 界面（UI 项、插件页面、设置页）的字段与块类型 | [plugin-ui.md](plugin-ui.md) |
| 播放页背景（shader bundle 打包与 uniform 约定） | [plugin-shader-bundle.md](plugin-shader-bundle.md) |
| 装插件、刷新、看日志（命令行 / MCP，给 AI agent 与自动化） | [plugin-agent-cli.md](plugin-agent-cli.md) |
| 动态库插件（钩子/能力/UI 的写法等价） | [plugin-native-api.md](plugin-native-api.md) |
| 可运行的完整示例 | `plugins/example_js/`、`plugins/example_js_shader/` |
| 单特性示例（多脚本加载 / 变量通道 / 异步裁决 / 全局字体） | `plugins/example_js_multi_script/`、`plugins/example_js_vars/`、`plugins/example_js_async/`、`plugins/example_js_font/` |

> **本文只写 JS 特有的东西**：目录与清单、生命周期与硬约束、`musicxx` 全局对象的写法、JS 线程、
> 定时器与工具、调试与坑。**接口本身"做什么、参数是什么、返回什么"在 [plugin-api.md](plugin-api.md)** ——
> 那份文档 JS 与动态库插件共用，两边的接口是同一批。

---

> **术语约定**：本文档中的**主程序**指的就是 **musicxx**（Flutter/Dart 业务层）；外置插件的 C++ 框架层称为**插件框架**；插件目前分为**动态库插件**与**JS插件**。


## 1. 一个 JS 插件长什么样

```text
my_plugin/
├── plugin.yaml     # 清单（必填）
├── plugin.js       # 脚本（entry / scripts 至少要有一个可用脚本；缺省就是它）
├── pluginxx_ui_kit.js  # 可选：随插件分发的界面 kit（见 §12）
├── shader/         # 可选：随插件分发的资源（如 shader bundle）
└── config.json     # 可选：插件自己的配置（运行时由脚本读写）
```

```yaml
name: my_plugin                  # 插件 id（唯一；同名视为同一插件的升级覆盖）
kind: js                         # js = 零编译脚本插件（缺省时按 entry 推导）
entry: plugin.js                 # 可省略（清单只要求 name）：缺省用 plugin.js
scripts: [pluginxx_ui_kit.js, plugin.js]   # 可选：按顺序执行的脚本（缺省取 entry / plugin.js）
version: 2026.1010.1            # 版本号 = `<年>.<月日>.<小版本号>`（例中为 2026-10-10 的第 1 版）
api_version: 1                   # 插件 API 版本：只要不低于插件框架支持的最低版本（当前 1）就照常加载；比插件框架更高也不会被拒
author: "你的名字"
description: "插件说明"
platforms: [windows, linux, macos, android, ios]   # 不写 = 不限（别名 win32 / osx 也认）
depends: [other_js_plugin]       # 必选依赖：缺失时拒绝加载
optional_depends: [maybe_plugin] # 可选依赖：只影响加载顺序

permissions:                     # 声明式权限：只做展示，运行时不校验
  - musicxx.player.control
  - musicxx.ui
  - musicxx.storage
```

`scripts` 里的文件在**同一个 JS 上下文**里按顺序执行（前一个脚本定义的全局量，后面的脚本
直接能用）：随插件分发的 kit 文件就放在这里。任一脚本顶层报错都算加载失败，错误信息里带
出错脚本的文件名；文件缺失在扫描阶段就会提示「JS 插件缺少脚本文件: &lt;名字&gt;」。
**用了 kit 就必须把 kit 文件写进 `scripts`**：漏写时脚本里访问 `pluginxx` 会以
`pluginxx is not defined`（错误里带脚本名）失败——插件框架不提供 kit。

安装：管理页「从压缩包安装」（`.zip`，顶层是插件目录内容），或把目录放进
`<应用支持目录>/musicxx/extern_plugin/plugins/<id>/` 后「重新扫描」。JS 插件在
**所有平台**都能用（包括只允许 JS 插件的 iOS / OHOS）。

> 多目标打包（`lib/<系统>-<架构>/` 分支、运行时按系统与架构选分支）是**动态库插件**的机制：
> 脚本本身跨平台，直接放在插件目录根即可；清单 `targets_dir` 对 JS 插件没有作用。

---

## 2. 生命周期与硬约束

| 阶段 | 发生了什么 |
|---|---|
| 加载（`kind: js`） | 插件框架为该插件登记一个合成实例 `js:<插件id>`，建立独立的 `JSRuntime`/`JSContext` |
| 脚本顶层执行 | 顶层**必须同步完成注册**（钩子 / 能力 / 订阅）；顶层抛异常 = 加载失败并回滚 |
| 启用中 | 钩子处理器与定时器工作；同步读状态镜像、异步发动作请求 |
| 停用（disable） | 脚本的注册全部摘除、定时器清空、`JSRuntime` 释放；**再次启用会重新执行一遍脚本** |
| 卸载（unload） | 实例销毁；插件目录与私有数据保留（除非用户在管理页选择彻底卸载） |

硬约束（踩过坑的都在这里）：

1. **顶层不能有 `await`**；异步逻辑放到钩子、定时器或 Promise 回调里。
2. 所有 JS 代码跑在插件框架的**一条共享 JS 线程**上（多个 JS插件共用一个线程、各自独立运行时）：
   回调要尽快返回，不要在里面做长时间同步计算。
3. 裁决型钩子的**推荐写法是同步返回**裁决对象；需要 `await` 才能决定时可以先同步判断
   "这次要不要裁决"，只在需要的那次返回 Promise（见 §4.3）。返回 Promise 时插件框架会一直等到它结算。
4. `require`、`fs`、`fetch`、原生模块都**不存在**；框架与主程序能力全部经 `musicxx.*`。
5. 插件之间不做隔离（同一条 JS 线程）；不要依赖全局副作用，状态放插件自己的存储里。
6. 钩子处理器抛异常只记日志与统计（**不会暂停派发、也不会卸载插件**）
   （超时不算失败；统计里看得到 `paused`）。
7. 脚本顶层与处理器里**都可以**读写 `musicxx`；但处理器里不要递归触发同一个钩子。
8. 脚本每次启用都会重新执行：**注册语句要写成幂等的**（重复执行不会累积副作用），
   定时器在 `stop` 时由插件框架清理，不要假设它还在。
9. **主程序整体卡住**（脚本死循环、长时间同步计算）时，插件框架不会打断脚本：主程序连续两次启动没走完
   会**自动进入安全模式（本次不运行不加载任何外部插件）**，用户也可以在管理页手动开关。
   启动阶段不要做重活，循环要保证能结束（详见 [plugin-guide.md](plugin-guide.md) §5.1 / §5.9）。

---

## 3. `musicxx` 全局对象

| 命名空间 | 用途 | 详细 |
|---|---|---|
| `musicxx.hooks` | 注册 / 注销钩子处理器、读谁在提供这个钩子 | §4；[plugin-api.md](plugin-api.md) §8 |
| `musicxx.state.get` | 同步读状态镜像 | §5 |
| `musicxx.host` | 框架信息、日志、配置路径 | §6 |
| `musicxx.call` | 通用动作请求（返回 Promise） | §7 |
| `musicxx.player` / `library` / `lyrics` / `storage` / `net` / `render` / `media` / `ui` / `stats` / `feature` | 分类便捷封装（都等价于 `musicxx.call`） | §7.1；[plugin-api.md](plugin-api.md) §3 |
| `musicxx.events` | 事件总线（发布 / 订阅） | §8；[plugin-api.md](plugin-api.md) §4 |
| `musicxx.vars` | 变量：读 / 写 / 订阅 / 注册自己的变量 | §9；[plugin-api.md](plugin-api.md) §5 |
| `musicxx.capability` | 注册自己的能力 / 调用别的插件的能力 | §10；[plugin-api.md](plugin-api.md) §6 |
| `musicxx.feature` | 功能点：调用 / 声明 / 实现 | §4.4；[plugin-api.md](plugin-api.md) §7 |
| `musicxx.timer` / `musicxx.util` | 定时器与工具 | §13 |
| `musicxx.version` / `musicxx.pluginId` | 运行时版本（插件框架版本，如 `"2026.1011.1"`，与 `musicxx.host.info().hostVersion` 同一个值）/ 本插件 id | — |
| `console` | `log` / `info` / `warn` / `error` 转发到框架日志 | §6 |

脚本里还有一个全局的 `setTimeout` / `setInterval` / `clearTimeout` / `clearInterval`
（与 `musicxx.timer` 同一实现）。除此之外**没有** `require` / `fs` / `fetch` / `process` 这些 Node 时代的对象。

---

## 4. 钩子

### 4.1 注册与注销

```js
musicxx.hooks.register(id, { mode, priority, ownerTag }, fn);
musicxx.hooks.unregister(id);
musicxx.hooks.has(id);
musicxx.hooks.handlers(id);   // Promise：谁注册了这个钩子（见下）
```

- `id` 必须是**全名**（官方 `musicxx.*`）；插件框架的约定表里没有的钩子会被拒绝；
- `mode`：`"observe"` = 只观察（返回值忽略）；其它值（含不写）= `"decision"` 裁决型；
- `priority`：小者先执行；同优先级按注册顺序；
- `ownerTag`：同一插件在同一个钩子上区分多个处理器的标记（**不要用不同 ownerTag 注册同一个钩子**，
  见 §15 常见坑）；
- `fn(ctx)`：`ctx` 是钩子载荷（JSON 对象，字段见 [plugin-hooks.md](plugin-hooks.md)）；
  返回 `null` / `undefined` = 不裁决。

```js
// 观察型：切歌时打一条日志
musicxx.hooks.register("musicxx.song.changed", { mode: "observe" }, (ctx) => {
    musicxx.host.log(2, "正在播放: " + ((ctx.song && ctx.song.name) || "未知"));
});
```

顶层同步注册会在脚本执行结束后由引擎统一登记；**运行期**调用 `register` / `unregister` 同样生效
（插件框架把它投递到框架线程执行，不在脚本线程等待）。

**想知道"这个钩子现在有谁在提供"**用 `musicxx.hooks.handlers(id)`（异步，读的是插件框架的权威数据）：

```js
musicxx.hooks.handlers("musicxx.media.spectrum.extract").then((info) => {
    // info = {hook, count, handlers:[{plugin, instance, kind, priority, ownerTag}], listAvailable}
    // handlers 按**调用顺序**（priority 升序，同优先级按注册顺序）给出
    if (info.count === 0) { musicxx.hooks.register("musicxx.media.spectrum.extract", fn); }
});
```

`listAvailable` 为 `false` 表示插件框架库太旧（只有数量，看 `count`）。主程序的自省（命令行
`featurePoint.list` / MCP `GetFeatureProviders` / 设置页说明）读的是同一份数据，所以"插件里看到的"
与"用户看到的"一致。

### 4.2 裁决型钩子的返回值

```js
{ action: "continue" | "skip" | "cancel" | "replace", patch: { ... }, error?: "说明" }
```

`action` 的含义由各调用点决定（例如 `beforePlaySong` 的 `skip` = 跳过本曲）。当前版本已接入的
裁决钩子只有三个，语义见 [plugin-hooks.md](plugin-hooks.md) 的「已接入钩子的载荷与裁决」：
`player.beforePlaySong`（只支持 `skip`）、`player.source.beforeParse`（`skip` / `patch.src`）、
`player.error`（`stop` / `skip` / `patch.tryNextSrc`）。

### 4.3 异步裁决（返回 Promise）

```js
musicxx.hooks.register("musicxx.player.speed", { mode: "decision" }, (ctx) => {
    if (ctx.to >= 0.25 && ctx.to <= 3) return null;      // 同步路径：最省时
    return new Promise((resolve) => {                     // 异步路径
        setTimeout(() => resolve({ action: "continue", patch: { to: 3 } }), 20);
    });
});
```

- 插件框架一直等到 Promise 结算：结算后裁决生效（没有等待超时，也不会按「无裁决」继续）；
- 所以异步路径要保证 Promise **一定会结算**（失败就 reject），否则调用点会一直等；
- 结算次数可在统计里看到（`asyncHookSettled`，见 `musicxx.stats.getSelf()`）。

### 4.4 功能点（声明 + 实现 + 调用）

> 概念、与能力的区别、通道的三条约定（`patch` 包装 / 按 `point` 过滤 / 不回答自己发起的调用）、
> 应用留出的三个可调点、调用零副作用与 `busy` 规则，都在 [plugin-api.md](plugin-api.md) §7。
> 这里只给 JS 的写法与 JS 特有的注意点。

```js
// 1) 声明自己的功能点（id 必须落在自己的命名空间里）
musicxx.feature.define({
    id: "plugin." + musicxx.pluginId + ".beat",
    title: "节拍检测",
    depict: "按 BPM 与拍号算出若干拍的时刻",
    args: "bpm（每分钟拍数，缺省 120）、beats（要几拍，缺省 4）",
    result: "{bpm, beats: [ms, ...], confident}",
});

// 2) 给出实现：返回的是**值**（糖自动包成 {"patch":{"value": …}}）
musicxx.feature.provide("plugin." + musicxx.pluginId + ".beat", function (args, ctx) {
    const bpm = (args && args.bpm) || 120;
    return { bpm: bpm, beats: [0, Math.round(60000 / bpm)], confident: 0.9 };
});
musicxx.feature.provide(["plugin.a.x", "plugin.a.y"], (args) => ({ ok: true }));  // 一组点共用一个实现

// 3) 调一个功能点（应用留出的，或别的插件声明的）
const colors = await musicxx.feature.callPoint("musicxx.media.palette.provide", {});

musicxx.feature.unprovide("plugin." + musicxx.pluginId + ".beat");   // 摘除实现
```

JS 侧需要记住的几条：

- `callPoint(id, args?, timeoutMs?)`：默认 60 秒（频谱要解码整曲）。
- `provide` 的 `fn(args, ctx)` **必须同步返回**：功能点走**同步裁决**通道（插件框架就地等结果），
  返回 Promise 拿不到结果（会记一条日志）。长任务请提前算好，或做成**能力**（`musicxx.capability.call`）。
- 糖自动做三件事：按 `point` 过滤（同一条通道上所有实现都收到同一次调用）、`ctx.caller` 等于自己时
  不回答（避免把自己算出的结果再问回去）、把返回值包成 `{"patch": {"value": …}}`。
  要返回"算过了、结果是空"就给 `null`（`undefined` 与空串 = 这次不回答）。
- 声明跟着插件生命周期走：禁用 / 卸载会撤销全部声明，重新启用时脚本再声明一遍（声明不写入磁盘）。
- 手写钩子处理器（不用糖）时，约定见 [plugin-hooks.md](plugin-hooks.md) 的
  `musicxx.feature.point.call`：**回答必须用 `patch` 包一层**，否则后面的处理器会覆盖你的结果。

---

## 5. 状态镜像（同步读应用现状）

> 键清单、每个键的字段、刷新时机、常见坑，都在 [plugin-api.md](plugin-api.md) §2。
> 这里只说 JS 侧的用法。

```js
const song = musicxx.state.get("musicxx.state.song");   // 没推送过 → null
const playing = (musicxx.state.get("musicxx.state.env") || {}).isPlaying === true;
```

要点（键与字段的完整清单见 [plugin-api.md](plugin-api.md) §2）：

- `state.get` 是**同步**读（读插件框架里的快照），没有网络与 IO，可以放在钩子处理器、定时器、
  页面能力处理器里；
- 可读的键：`musicxx.state.app` / `player` / `song` / `playlist` / `env` / `renderSlots` / `spectrum`
  （`musicxx.state.lyric` / `musicxx.state.library` 是预留的，当前版本没有推送，读到的永远是 `null`）；
- 键名固定（官方 `musicxx.state.*`），写错就是 `null`，不会报错；
- **播放进度不在镜像里**：`musicxx.state.player.position` 只在启动、切歌与播放状态变化时刷新，
  `musicxx.player.position` 钩子也尚未接入 —— 不要把它当每秒更新的进度用；
- 要在自绘界面里"跟着音乐动"，用 `musicxx.state.spectrum`（约 10 Hz）或着色器的
  `musicxx.spectrum.*` 来源（见 [plugin-shader-bundle.md](plugin-shader-bundle.md) §7）；
  识别格式：`status` = `ready` / `loading` / `none` / `off` / `unavailable`，
  `available` 为假时 `level` 与 `bands` 都是 0（原因读 `reason`）；
- 变化通知：订阅 `musicxx.state.changed`（载荷 `{key, value}`）：

  ```js
  musicxx.events.subscribe("musicxx.state.changed", (payload) => {
      if (payload.key === "musicxx.state.song") { /* 刷新自己的界面数据 */ }
  });
  ```

---

## 6. 框架信息与日志

```js
musicxx.host.info();
// { appVersion, platform, language, dataDir, userPluginDir, builtinPluginDir,
//   apiVersion, hostVersion, pluginId, ui? }
//  platform: windows / linux / macos / android / ios / ohos
//  dataDir:  插件数据根目录（私有 KV 在 <dataDir>/<插件id>/data/kv.json）
//  pluginId: 本插件 id（只有插件读时才带这个字段）
//  ui:       客户端界面能力段（见下）
//  字段逐个的说明见 plugin-api.md §2.5

musicxx.host.configPath();   // 本插件 config.json 的绝对路径
musicxx.host.log(2, "文字"); // 0 trace / 1 debug / 2 info / 3 warn / 4 error
console.log / info / warn / error   // 同上（映射到 info / warn / error）
```

`host.info().ui` 是**主程序如实上报的界面能力**，由界面描述层库生成（插件框架只做转发）：

```js
const ui = musicxx.host.info().ui;
ui.kind        // "gui"（图形界面）或 "tui"（终端）
ui.blocks      // 这个客户端真正支持的界面组件名（例如是否支持 "Icon" / "musicxx.Shader"）
ui.controls    // 支持的控件形态（buttons / select / checkbox / switch / text / number）
ui.gap         // 客户端默认行距（u）
ui.cell        // 只有终端有：每个字符格相当于多少 u（图形界面没有这个字段）
ui.limits      // 解析规模上限（当前全部为 0 = 不限制）
```

- **图标不在能力段里**：musicxx 不接受主程序自己的图标名（`ui.icons` 不发布），插件要图标就
  自带 svg —— 写一条**相对插件目录**的路径（`resource/svg/` 只是推荐位置，见
  [plugin-ui.md](plugin-ui.md) §1.5）。

- **判断"能不能用某个组件"要按 `blocks`/`controls` 判断**，不要按 `kind` 写两套内容；
  `kind` 只用于粗判断（比如终端不给图片、不推着色器）。
- 取插件页面（`ext://<插件id>/<视图id>` 对应的能力）时，主程序也会把同一份能力段放进能力参数
  （`{"view":"...","ui":{…}}`）：需要在**生成页面内容的那一刻**就知道目标的插件用这个参数。
- 这个字段只在客户端给了能力段时出现；没有时按 `undefined` 处理（当作"什么都能试，老客户端
  会忽略不认识的组件"）。

日志会进「管理页 → 插件详情 → 日志」；插件框架也会把它作为 `musicxx.plugin.log` 事件推给主程序。

---

## 7. 动作（异步请求）

```js
const result = await musicxx.call("musicxx.<域>.<动作>", { ...参数 }, 超时毫秒?);
```

- 默认超时 **5000 ms**；`timeoutMs` 传 0 或负数时仍按缺省值处理（**不是"一直等"**）。
  要等更久（解码整曲、等用户点弹窗）就把超时给大一点；
- Promise 拒绝时 `err.message` 里带原因，例如 `action_not_registered: xxx`、
  `请求失败：...`、`status=...`；
- **动作不做权限校验**：清单里的 `permissions` 只做展示，不会拒绝任何调用；
- 动作未注册（当前版本没实现）会立刻失败，不会挂到超时。
- **动作名不校验**：插件框架受理任意名字的动作，由主程序按「注册了没有」决定理不理它（没注册立刻失败）；
  官方动作是 `musicxx.<域>.<动作>`，插件自己的动作建议写成 `plugin.<包名>.*` 以免与别的插件撞名；
- **"动作执行了但条件不满足"与"调用失败"是两件事**：前者 Promise 成功、结果里 `{ok:false, error:"…"}`
  （例如找不到歌单）；后者 Promise 拒绝（动作没注册、处理器抛异常）；
- **每个动作做什么、参数与返回是什么**看 [plugin-api.md](plugin-api.md) §3（两边的接口是同一批）。

### 7.1 分类便捷封装

| 分类 | 方法 |
|---|---|
| `musicxx.hooks` | `register(id, options?, fn)` `unregister(id)` `has(id)` `handlers(id)` |
| `musicxx.player` | `play(args?)` `pause()` `toggle()` `stop()` `next()` `prev()` `seek(ms\|args)` `setVolume(v)` `setSpeed(v)` `setLoopMode(mode)`（改音质用全名 `musicxx.call("musicxx.player.setQuality", {quality})`，没有单独封装） |
| `musicxx.library` | `querySongs(args?)` `querySonglists()` `playSong(args)` `playSonglist(args)` |
| `musicxx.lyrics` | `getCurrent()` |
| `musicxx.feature` | `list()` `call(id, args?, timeoutMs?)` `points(options?)` `callPoint(id, args?, timeoutMs?)` `define(specOrId, spec?)` `undefine(id)` `provide(idOrIds, fn, options?)` `unprovide(id, fn?)` |
| `musicxx.vars` | `get(key, options?)` `getMany(keys, options?)` `peek(key)` `watch/unwatch(key)` `set(key, value)` `bind(key, fn)` `unbind(key, fn?)` `register(spec)` `unregister(key)` `has(key)` `info(key)` `list(prefix?)` `own()` `subscribers(key)` |
| `musicxx.storage` | `get(key, 默认值?)` `set(key, value)` `remove(key)` `list()` `getConfig(key, 默认值?)` `setConfig(key, value)` |
| `musicxx.net` | `fetch(options)` `download(options)` |
| `musicxx.render` | `list(args?)` `current(args?)` `select(id, args?)` |
| `musicxx.media` | `palette(args?)` `setPalette(args)` `cover(args?)` `bindImage(args)` `unbindImage(args)` `spectrum(args?)` |
| `musicxx.ui` | `notify(args)` `toast(args)` `dialog(args)` `openRoute(route, args?)` `setFontFamily(args)` `getFontFamily()` `loadFont(options)` `registerEntry(spec)` `updateEntry(name, data)` `unregisterEntry(name)` `entries()` |
| `musicxx.stats` | `getSelf()` `reportMemory(bytes)` `reportMetric(name, value)` |

**动作清单**（每个动作做什么、参数与返回、失败情形与示例）在 [plugin-api.md](plugin-api.md) §3.2；
本文不再重复。使用时记住三件事：

- **动作名没在上面的便捷封装里也没关系**：`musicxx.call("musicxx.<域>.<动作>", {参数})` 一样能调；
- **名字存在但当前版本没实现**的动作会立刻失败（`action_not_registered`）：
  `musicxx.player.setPitch` / `setMediaType`、`musicxx.library.querySonglist` / `addSong` /
  `removeSong` / `createSonglist` / `setSongInfo` / `search`、`musicxx.lyrics.getBySrc` / `set` /
  `sync` / `search`、`musicxx.ui.setEntryBadge`、`musicxx.host.shareText` —— 写插件前先按
  [plugin-api.md](plugin-api.md) §3.3 确认一遍；
- **要"改应用状态"而动作里没有**：多半有对应的钩子或变量（例如改播放速度用
  `musicxx.player.setSpeed`，想拦一次用户改速用钩子 `musicxx.player.speed`（还没接入））。

### 7.2 存储（插件私有数据）

> 两块地方（私有 KV / 用户可见的 `config.json`）、参数、返回、限制与坑，见
> [plugin-api.md](plugin-api.md) §3.2.5。JS 侧的便捷封装就是下面这几个：

| 用途 | API | 存放位置 |
|---|---|---|
| 私有 KV | `musicxx.storage.get/set/remove/list` | `<dataDir>/<插件id>/data/kv.json` |
| 用户可见的设置 | `musicxx.storage.getConfig/setConfig`（= 带 `namespace:"config"`） | 插件目录里的 `config.json` |

- `get(key, 默认值)` / `getConfig(key, 默认值)` 返回**值本身**：键不存在时返回你给的默认值
  （不给默认值就是 `null`）。**不要**再按对象去读应答，否则设置项读不出来；
- `list()` 返回 `{keys: [...]}`，键名是插件框架内部的存储键（私有 KV 的键带命名空间前缀）；
- 默认值由**你自己**在第二个参数里给：框架不保存默认值，也没有「恢复默认」的入口
  （用户直接编辑 `config.json` 即可）。

### 7.3 网络（主程序代理通道）

> 参数、限制、返回结构、下载落盘位置与失败语义见 [plugin-api.md](plugin-api.md) §3.2.6。

```js
const resp = await musicxx.net.fetch({
    url: "https://api.example.com/items?page=1",
    responseType: "json",             // text（缺省）/ json / bytes
    timeoutMs: 15000,                 // 1000 ~ 60000（缺省 15000）
});
// resp = { ok, status, url, headers, contentType,
//          body | bodyBase64, bodyBytes, receivedBytes, contentLength, truncated }
```

- **非 2xx 也按"请求完成"返回**（`ok:true` + `status`），只有传输失败 / 超时才 `ok:false`；
- `responseType:"json"` 目前也给文本（自己 `JSON.parse`）；要原始字节用 `"bytes"`（给 `bodyBase64`）；
- JS 封装会把动作超时放宽到 `timeoutMs + 5000`，所以 HTTP 超时不会被动作超时先打断。

```js
const file = await musicxx.net.download({ url: "https://.../a.mp3", fileName: "a.mp3" });
// file = { ok, status, path, bytes }，落在 <dataDir>/<插件id>/data/downloads/ 里
```

### 7.4 界面反馈与页面跳转

> 参数、白名单（12 个官方页面别名）、拒绝规则见 [plugin-api.md](plugin-api.md) §3.2.4。

```js
await musicxx.ui.notify({ text: "已完成" });                 // 站内提示（kind:"error" 走错误样式）
await musicxx.ui.toast({ text: "轻提示" });
const answer = await musicxx.ui.dialog({                     // 敏感操作前先问用户
    title: "删除缓存", content: "会删除该插件的缓存文件，是否继续？",
    textConfirm: "继续", textCancel: "取消",
});
if (answer && answer.confirmed) { /* 用户点了确认 */ }

await musicxx.ui.openRoute("ext://my_plugin/card");          // 插件页面（允许任意插件）
await musicxx.ui.openRoute("musicxx:settings");              // 官方页面（白名单键）
```

- 弹窗标题会自动带插件来源前缀（`『<插件id>』…`），避免插件伪装成主程序自己的提示；
- `openRoute` 的地址不完整（缺插件 id 或视图 id）或不在白名单里会被拒绝，错误信息里会列出全部可用别名。

---

## 8. 事件

```js
musicxx.events.subscribe("musicxx.state.changed", (payload, topic) => { /* ... */ });
musicxx.events.publish("plugin.my_plugin.hello", { at: Date.now() });
musicxx.events.unsubscribe("musicxx.state.changed");
```

- **订阅**：顶层声明或运行期订阅都可以；可订阅官方 `musicxx.*` 主题，以及其它插件的
  `plugin.<id>.*` 主题。同一主题的 **host 订阅只会建立一次**：`unsubscribe` 之后该主题的回调不再触发，
  但再次 `subscribe` 会复用原来那条订阅（不会重复建立）。
- **发布**：只能发布官方主题或**自己命名空间**（`plugin.<自己的插件 id>.*`）的主题；主题非法时
  `publish` 直接抛异常。
- 插件之间做协作时，用「对方的 `plugin.<对方id>.*` 主题」或能力调用（§10）都行：
  事件是单向通知，能力调用有返回值。

常用的可订阅主题（完整清单与载荷字段见 [plugin-api.md](plugin-api.md) §4）：
`musicxx.state.changed`（状态镜像键变化）、`musicxx.var.changed` / `musicxx.var.writeResult`（变量通道）、
`musicxx.ui.changed`（UI 项变化）、`musicxx.hook.changed`（钩子注册表变化）、
`musicxx.plugin.log` / `musicxx.plugin.error` / `musicxx.plugin.warn`（日志与告警）、
`musicxx.hook.decision.result`（异步裁决结果，调试用）。

---

## 9. 变量（有属主、可读、可写、可订阅变动的小值）

> 概念、与邻居的分工（镜像 / 动作 / 事件 / 变量怎么选）、三条读法的取舍、注册与 handler 模式、
> 错误码与超时，都在 [plugin-api.md](plugin-api.md) §5；官方变量目录见
> [plugin-vars.md](plugin-vars.md)。本节只给 JS 的 API 与写法。

```js
// 读：权威读（异步）/ 一次读多个 / 同步读缓存 / 变化回调 / 只保活缓存
var level = await musicxx.vars.get("musicxx.ui.animatedLevel");
var values = await musicxx.vars.getMany(["musicxx.ui.songIconRotate", "musicxx.ui.songIconWave"]);
musicxx.vars.watch("musicxx.ui.songIconWave");
var cached = musicxx.vars.peek("musicxx.ui.songIconWave");   // {value, revision, ageMs, stale} 或 null
var off = musicxx.vars.bind("musicxx.ui.animatedLevel", function (value, info) {
    musicxx.host.log(2, "动画等级变成 " + value + "（由 " + (info.by || "应用") + " 改的）");
});
off();                                   // bind 返回解绑函数；unbind(key, fn) 也可以

// 写：结果 {key, accepted, value, changed, error?}
var result = await musicxx.vars.set("musicxx.theme.mode", "night");
if (!result.accepted) { musicxx.host.log(3, "被拒绝: " + result.error); }

// 注册自己的变量（短名，框架补 plugin.<自己 id>. 前缀）
musicxx.vars.register({ key: "tip.start", caps: ["get", "set", "notify"], type: "bool", value: false,
                        title: "开场提示", depict: "为真时播放页显示一句开场提示" });
musicxx.vars.unregister("tip.start");

// 自省：has(key) / info(key) / list(prefix?) / own() / subscribers(key)
musicxx.vars.subscribers("tip.start");   // 判断"还要不要继续高频推"
```

handler 模式（值本来就在脚本里，不想再维护一份副本）：

```js
var tipStart = false;                    // 唯一真值在脚本里

musicxx.vars.register({
  key: "tip.start",
  caps: ["get", "set", "notify"],
  mode: "handler",
  type: "bool",
  refreshAfterMs: 5000,                  // 有人 watch 时框架最多 5 秒来取一次真实值
  onRead:  function () { return tipStart; },              // 回答 get（可以返回 Promise）
  onWrite: function (value) {                             // 别人写这个变量时由这里处理
    tipStart = (value === true);
    return tipStart;                                      // 返回值 = 提交给框架的最终值
  }
});

function setTipStart(next) { tipStart = next; musicxx.vars.set("tip.start", next); }  // 自己改了要主动提交
```

JS 侧要记住的几条：

- **`peek` 是同步的**（返回 `null` = 没缓存过），`get` / `set` 返回 Promise：
  `get` 失败走 Promise 拒绝（`变量不存在` / `该变量不可读` / `读取超时`）；`set` 被拒时
  `result.accepted === false`（不会抛异常）；
- `watch` 只是"订阅但不回调"，用来让 `peek` 有新鲜值（需要该变量声明了 `notify` 能力位）；
- `register` 的 `onRead` / `onWrite` **可以返回 Promise**（框架等它结算）；`onWrite` 抛异常会被转成一次
  "拒绝"，异常文案带给写入方；
- 声明跟着插件生命周期走：禁用 / 卸载会撤销自己的全部变量；每次启用脚本都会重新声明一遍；
- 键名可以写多级分组（`tip.start.detail`）；写全名 `plugin.<自己的插件id>.*` 也认；

## 10. 能力（注册 + 跨插件调用）

> 能力的定位（与"功能点"的分工）、注册与调用的约定、超时与失败原因见
> [plugin-api.md](plugin-api.md) §6；这里只给 JS 的写法与 JS 线程相关的注意点。

```js
musicxx.capability.register("probe", (args) => ({ ok: true, args }));
musicxx.capability.register("my_name", (args) => ({ ok: true }));   // 短名即可

const info = await musicxx.capability.call("example_native", "probe", {});
```

- 能力全名是 `plugin.<插件 id>.<短名>`（JS 写短名，框架补前缀）；
- 处理器可以**同步返回**可 JSON 序列化的结果，也可以返回 Promise（插件框架会等到它结算再给调用方结果）；
- **调用只有异步形态**（Dart 侧只受理不等待，结果经事件回填）：插件慢或卡住不会再让应用界面没反应，
  处理器里发起的动作也能被处理完（主程序那条路不会互锁）。
- **处理器里不要等动作**：主程序打开页面 / 点按钮会等能力结果，而主程序动作要由主程序线程执行 ——
  处理器里 `await` 一个动作会与动作执行互相等下去（表现是"整个主程序卡住几秒 + 调用插件能力失败"）。
  异步处理器本身没问题（例如等 JS 定时器、等跨插件调用），只是不要等**主程序**动作：
  - 要"现在这一帧"的数据（播放状态、频谱、槽位状态等）读状态镜像（`musicxx.state.get`）；
  - 要动作结果就异步读、把结果记在自己的状态里，由能力（页面）下一次调用时回读
    —— 见 `plugins/example_js` 的 `crossCall` 与 `plugins/example_js_shader` 的 `spectrumProbe`。
  同一条规则也适用于**同步派发**的钩子（`musicxx.player.beforePlaySong` 等调用点就地等待的钩子）：
  处理器里等动作会一直卡住调用线程。
- `capability.call(插件id, 能力名, 参数?, 超时毫秒?)`：

  | 目标 | 行为 |
  |---|---|
  | JS 插件 | 在共享 JS 线程上**直接调用**：处理器同步返回时结果立刻就有，返回 Promise 时等它结算（同一条线程，不跳线程） |
  | 动态库插件 / 内置目标 | 投递到框架线程执行，脚本**不阻塞**；结果回到 JS 线程（超时缺省 3 s；`≤ 0` 时插件框架仍按 3 秒处理，不是"一直等"） |

- 跨插件调用失败时 Promise 拒绝，`err.message` 里带原因
  （`plugin_capability_not_found: xxx` / `capability_call_failed` / `跨插件调用超时: ...`）；
- 插件页面也走能力：主程序打开 `ext://<插件id>/<视图id>` 时调用同名能力（见 §12）。

---

## 11. 渲染槽位与封面数据

插件可以把**预编译的 shader bundle** 注册成主程序的一种渲染样式（当前有两个槽位：「播放页背景」
`player.background` 与「播放页歌曲图」`player.icon`，后者见
[plugin-shader-bundle.md](plugin-shader-bundle.md) §5.3），由用户在设置里选中后生效；
也可以拉取封面颜色 / 字节自己算。打包方式、`format_version` 与全部字段见
[plugin-shader-bundle.md](plugin-shader-bundle.md)，本节只说 JS 侧怎么用。

> 渲染槽位的动作（`musicxx.render.*`）与封面 / 频谱动作（`musicxx.media.*`）的**参数、返回与状态含义**
> 在 [plugin-api.md](plugin-api.md) §3.2.7 与 §3.2.8（两种形态共用）；本节是 JS 的声明写法与注意点。

```js
// 接管播放页中间的歌曲图（类型简称 playing.icon）：不显示内容、只保留占位
// 也可以声明 shader（插件着色器绘制）或 view（声明式界面组合）—— 三个字段挑一个
musicxx.ui.registerEntry({
    name: "icon",
    type: "playing.icon",
    data: { title: "不显示歌曲图", mode: "none", keepSpace: true },
});

// 注册一种播放页背景样式（类型简称 playing.background，或写全名）
musicxx.ui.registerEntry({
    name: "bg",
    type: "playing.background",
    order: 20,
    data: {
        title: "流光背景",
        depict: "跟随封面配色的动态背景",
        shader: { bundle: "shader/bg.shaderbundle" },   // 插件目录内的相对路径
        // 封面纹理：主程序把当前歌曲封面解码、缩放、按 blur 预模糊后上传成 GPU 纹理，
        // 每帧绑定到 texture 指定的 sampler2D；信息写在 info 成员
        // （(纹理宽, 纹理高, 原图宽高比, 是否有封面)，没有封面时最后一位是 0）
        cover: { texture: "uCover", info: "uCoverInfo", size: 512, blur: 24 },
        // args：成员名 → 值表达式（可嵌套；这里是来源 + 过渡 + 组合）
        args: {
            uColor1: { kind: "source", name: "musicxx.icon.themeMapping.0" },
            uColor2: { kind: "source", name: "musicxx.icon.main", convert: true, fallback: "#8899aa" },
            uColor3: { kind: "source", name: "musicxx.theme.primary" },
            uColor4: { kind: "const", value: "#223344" },
            uLevel: {
                kind: "smooth", attackMs: 20, releaseMs: 260,
                of: { kind: "source", name: "musicxx.spectrum.level" },
            },
        },
        speed: 1,          // 时间推进速度（主程序直接用这个值，不做二次缩放）
        maxFps: 16,
        // 自定义播放页前景的三色（普通色 / 减淡色 / block 背景色）：
        // 选中本样式后，播放页的文本、歌词、进度条与块状背景都用这三色；
        // 只写一部分也行，没写的用主程序默认色（见 plugin-shader-bundle.md §8.1）
        foregroundColors: { main: "#ffe8f0ff", cross: "#80e8f0ff", block: "#1f5a7bb8" },
    },
});
```

```js
// 运行期改这三色（不用让用户重新选一次背景）：整体重登记同一项
musicxx.ui.updateEntry({
    name: "bg",
    type: "playing.background",
    data: { /* 与注册时同一份声明，只改 foregroundColors */ },
});
```

```js
// 有哪些可选样式（含主程序内置项：id 形如 builtin:Auto）
const r = await musicxx.render.list({ slot: "player.background" });
// r = { ok:true, slots:[{ slot, title, selectedId, items:[
//        { id, title, depict?, source:"builtin"|"plugin", plugin?, available, reason?, selected } ] }] }

await musicxx.render.select("plugin.my_plugin.bg");                  // 省略 slot 用默认槽位
await musicxx.render.select("builtin:Auto", { slot: "player.background" });   // 也可以切回内置
// 成功 → { ok:true, slot, id }；不可用 / 未知 → { ok:false, error }

const cur = await musicxx.render.current();     // { ok, slot, id, title, source:"builtin"|"plugin", plugin, night }

// 「现在是不是我在画」：同步读状态镜像
const slot = (musicxx.state.get("musicxx.state.renderSlots") || {})["player.background"];
if (slot && slot.itemId === "plugin.my_plugin.bg") { /* 我在画 */ }
```

```js
// 封面色（分析结果 + 主程序内置背景实际用的 4 色）
const palette = await musicxx.media.palette();     // 需要强制刷新时传 { force: true }
// { ok, hasCover, analyzed, night, srcKey, version,
//   raw: { main, light, lightMuted, dark, darkMuted, dominant:[...] },   // 颜色为 "#rrggbbaa"
//   background: ["#rrggbbaa", ...] }                                     // 4 个绘制色

// 封面字节（自己要分析像素时用）：size 16..512，format = jpeg(默认) / png / rgba
const cover = await musicxx.media.cover({ size: 96, format: "png", includePath: true });
// { ok, srcKey, kind:"local"|"cache"|"content"|"asset"|"network", path?, width, height,
//   format, bytes, sha256, data(base64), fromCache }
// 失败：{ ok:false, error:"no_cover" | "cover_decode_failed" | "读取封面失败：..." }

// 把封面（或自己的图片）绑成纹理给着色器采样
// 渲染项里用 `image: {key: "mini", texture: "uMini"}` 引用（见 plugin-shader-bundle.md §5.2）
const bound = await musicxx.media.cover({ size: 128, format: "rgba", bind: "mini" });
// { ..., bound: true, bindKey: "mini" } —— 复用了同一次解码的像素，不重复解码
await musicxx.media.bindImage({ key: "art", data: base64Png, format: "png" });
await musicxx.media.unbindImage({ key: "art" });

// 写封面取色：算好自己的颜色后交给主程序（与内置分析是同一批字段，
// 内置背景、插件背景与 musicxx.icon.* 来源都会用上）
await musicxx.media.setPalette({ main: "#ff8844", dark: "#20222a" });

// 注册"封面取色实现"（替代内置分析）：应用在需要封面色时先问插件
//
// **处理器里不要 await 主程序动作**（`musicxx.media.cover` 之类）：主程序是在需要颜色的
// 时候同步等这次裁决的（切换背景样式、切歌、渲染准备都在 await 这条链上），在处理器里
// 等动作会把两边互相拖住，表现是"切歌 / 换背景样式卡几秒"（实测踩过）。
// 正确写法：命中的缓存**同步**返回，没命中就返回 null（应用走它自己的快速取色），
// 同时在后台算，算好用 `musicxx.media.setPalette` 主动写回。
musicxx.hooks.register("musicxx.media.palette.provide", function (payload) {
    // payload = { srcKey, name, artist, hasCover, night }
    if (!payload || payload.hasCover !== true) return null;
    const cached = paletteCache[payload.srcKey];
    if (cached) return { colors: cached };      // 同步给（应用当场用上）
    warmPalette(payload.srcKey);                // 异步算，不 await
    return null;                                // 这次不提供 → 应用走内置快速取色
});

// 换歌时先把颜色算起来（观察型），应用来问时通常已经命中缓存
musicxx.hooks.register("musicxx.media.cover.changed", { mode: "observe" }, (payload) => {
    if (payload && payload.hasCover === true) warmPalette(payload.srcKey);
});

function warmPalette(key) {
    return musicxx.media.cover({ size: 64, format: "rgba" }).then((cover) => {
        if (!cover || cover.ok !== true) return null;
        const colors = myPaletteOfRgba(cover.data);   // 自己算（统计主要色点）
        paletteCache[key] = colors;
        return musicxx.media.setPalette(colors);      // 主动写回，颜色跟着换
    });
}

// 当前音频频谱（由插件提供的实现提取：内置『音乐动效』或注册了提取钩子的插件；
// 每帧 256 个频点、10 帧/秒）
const spec = await musicxx.media.spectrum({ bandCount: 16, unit: "normalized" });
// { ok, status:"ready"|"loading"|"none"|"off"|"unavailable", available, loading, reason,
//   source:"extracted"|"live", srcKey, name, artist, durationMs, positionMs,
//   frameHz:10, frameIndex, frames, binCount:256, bandCount, unit,
//   level, bands:[...], frameData?:[256 个频点] }   // frameData 需要 includeBins: true
// 没有数据（没有任何插件提供提取实现 / 正在提取 / 来源不支持）也回 ok:true，用 status 判断
// 频点与频带是"画图用"的数值（按帧归一 + 拉开高低差 + gamma，与内置动效画出来的高度一致，
// 不是绝对 dBFS；要绝对电平用 MCP 的 GetAudioSpectrum）

// 提供"整曲频谱提取"实现（替代内置提取）：应用要提取某首歌的频谱时先问插件
//
// 应用会等这次裁决（最多 30 s），数据量大（4 分钟歌曲约 600 KB 频点，base64 后约 800 KB），
// 所以只在自己本来就要分析音频时才注册它。给了数据 → 应用直接用它（进度条波形、播放页动效、
// 状态镜像、MCP 能力都用这一份）；返回 null → 应用接着用已登记的实现（装了『音乐动效』
// 就是它自己的提取）；都没有 → 这个功能整体不可用（用户会看到"没有插件提供频谱提取实现"）。
musicxx.hooks.register("musicxx.media.spectrum.extract", function (payload) {
    // payload = { srcKey, name, artist, kind, path, durationMs,
    //             frames, binCount, frameHz, maxSeconds }
    const hit = spectrumCache[payload.srcKey];
    if (!hit) return null;                    // 没算好/算不了 → 这次不提供
    // spectrums：整曲频点（帧数 × 256 个字节，0~255 表示 -80~0 dB，每秒 10 帧）
    // waveform ：每帧 1 字节 RMS 响度（0~255 表示 -60~+20 dB；省略时应用按每帧频点均值补）
    return { spectrum: { spectrums: hit.spectrums, waveform: hit.waveform } };
});

// 提供"副歌分析"实现（替代内置算法）：应用要分析副歌时先问插件
musicxx.hooks.register("musicxx.media.chorus.analyse", function (payload) {
    // payload = { srcKey, name, artist, kind, path, durationMs, hasWave,
    //             frames, binCount, frameHz, spectrums?, waveform?, config }
    // hasWave 为真时附带应用已经拿到的整曲频谱（base64，可能几百 KB）：可以用自己的算法
    // 分析同一份数据；只想按文件自己读音频就忽略它（用 payload.path）
    const hit = chorusCache[payload.srcKey];
    if (!hit) return null;
    // 区段形状：{ start, end, confidence, energy, similarity, repeat }（毫秒，start/end 必需）
    // sections 为空数组 = 分析过了、这首歌没有副歌（应用据此显示"没有副歌"）
    return { chorus: { sections: hit, totalDuration: payload.durationMs } };
});
```

- 三个"取值型"钩子的完整语义（载荷、数据格式、超时、置空裁决、失效规则）见
  [plugin-hooks.md](plugin-hooks.md)，调用方的写法与代价见 [plugin-api.md](plugin-api.md) §7：
  - **封面取色**（`musicxx.media.palette.provide`）：命中缓存就**同步**返回，未命中返回 `null`
    并异步预热，算好用 `musicxx.media.setPalette` 写回。**绝不要在处理器里 `await` 主程序动作**
    （切换背景样式、切歌、渲染准备都在等这次裁决，等动作会互相拖到超时 —— 表现是"卡几秒"）；
    应用最多等 800 ms（超时按"没给"处理）；
  - **整曲频谱**（`musicxx.media.spectrum.extract`）：应用最多等 30 s，数据量大（4 分钟歌曲的频点
    base64 后约 800 KB），**只在自己本来就要分析音频时才注册**；处理器里同步返回缓存或 `null`；
  - **副歌分析**（`musicxx.media.chorus.analyse`）：应用最多等 20 s，载荷里带上已拿到的整曲频谱
    （`hasWave` 为真），返回 `{chorus: {sections: [...]}}`（空数组 = 分析过了、这首歌没有副歌）。
- `musicxx.media.palette.set` 是**主动写**：只覆盖你给的键，其余保持不变；换歌后内置分析会照旧
  重跑（想持续提供就用上面的钩子）；
- **封面变化**（`musicxx.media.cover.changed`）是**观察型**：注册后，换歌、同一首歌换了封面图、
  封面被清空时各收到一次（同一个封面源被重复设置不会重复派发）。载荷
  `{srcKey, name, artist, hasCover, kind}`（`kind` = `local` / `cache` / `content` / `asset` /
  `network`）里**没有直链、也没有图片字节** —— 要图片自己调 `musicxx.media.cover`，要在着色器里
  采样就声明渲染项的 `cover` / `image`；**可以回退移除**，插件停用 / 卸载时插件框架也会自动摘掉：

```js
musicxx.hooks.register("musicxx.media.cover.changed", { mode: "observe" }, (ctx) => {
    // ctx = { srcKey, name, artist, hasCover, kind }
    myOverlay.refresh(ctx);          // 例如自己算色、刷新自己的界面、通知自己下载的素材
});

// 回退移除这个绑定（移除后不再收到通知）
musicxx.hooks.unregister("musicxx.media.cover.changed");
```

- 主程序**每帧**把 `args` 声明的成员写进 uniform（`uParams` / `uEnv` 是自动成员），所以
  「跟着封面/主题配色」这类需求什么都不用做（取不到来源时用参数里的固定值）；
  频谱来源（`musicxx.spectrum.*`，见 [plugin-shader-bundle.md](plugin-shader-bundle.md) §7）
  是例外：没有数据时写 0（静音），要看 `uEnv.z` 判断"现在有没有数据"；
- 只想让画面跟着音乐动，用着色器参数来源就够了（每帧现读、零成本）；需要把频谱画成
  自己的界面（柱状图、波形历史）时，用 `musicxx.state.get("musicxx.state.spectrum")`
  在自己的定时器里同步读（播放中约 10 Hz 更新），或按自己的节奏调用
  `musicxx.media.spectrum`；
- 想让画面跟着封面配色动，用 `args` 里的来源就够了（`musicxx.icon.*`，每帧现读、零成本）；
  要用**封面图本身**（铺底、圆盘、像素化…）就在渲染项里声明 `cover`：
  主程序解码 + 缩放 + 可选预模糊后上传成 GPU 纹理，每帧绑定给着色器，插件不搬字节
  （见 [plugin-shader-bundle.md](plugin-shader-bundle.md) §5.1）；
- `musicxx.media.cover` 这条动作是**读数据**用的（自己分析像素：哈希、取色、外部识别…）：
  返回 base64 字节 / 本地路径，**不参与绘制**，也不推任何直链（网络来源只给 `kind:"network"`，
  本地来源的路径只在 `includePath: true` 时给出）；
- 槽位状态镜像见 [plugin-shader-bundle.md](plugin-shader-bundle.md) §10：`itemId` = 现在由哪个插件项在画
  （为空 = 没有插件项在画），`selectedId` = 用户选中的是谁（可能是 `builtin:*`），
  `visible:false` = 主程序当前没有渲染（播放页被遮挡 / 切后台 / 播放页不在页面上），可以据此停掉耗时的绘制工作。

---

## 12. 界面（UI 项与插件页面）

完整字段、块类型与排版约定见 **[plugin-ui.md](plugin-ui.md)**（C++ 与 JS 共用同一份）。

```js
// 主页入口：点开自己的页面
musicxx.ui.registerEntry({
    name: "card",
    type: "home.entry",
    order: 120,
    data: {
        title: "我的插件",
        subtitle: "示例入口",
        action: { kind: "route", route: "ext://my_plugin/card" },
    },
});

// 也可以把 data 的字段平铺在对象上（等价写法）
musicxx.ui.registerEntry({ name: "clear", type: "song.action", order: 900,
    title: "用我的插件处理这首歌",
    action: { kind: "capability", name: "handleSong" } });

// 插件页面：主程序调用同名能力，脚本返回视图描述
musicxx.capability.register("card", function (args) {
    return {
        view: {
            title: "我的插件",
            subtitle: "页面内容来自能力 card",
            blocks: [
                { kind: "Text", text: "插件只返回块描述，渲染由主程序完成。", style: "cross" },
                { kind: "Divider" },
                { kind: "Button", title: "播放/暂停", style: "primary",
                  action: { kind: "command", name: "musicxx.player.toggle" } },
                { kind: "Button", title: "打开本插件设置页",
                  action: { kind: "route", route: "ext://my_plugin/settings" } },
            ],
        },
    };
});
```

- 能力返回 `{ view: {...} }`（推荐）或直接返回视图对象；返回视图时主程序会用新视图**刷新当前页面**
  （按钮改配置 + 立即刷新就是这么做的）；
- `musicxx.ui.updateEntry("card", 完整 data)` 是**整体替换**，只想改一个字段也要传完整 data；
- `musicxx.ui.entries()` 返回本插件已注册项的脚本侧镜像（不含框架快照里的 `id` 前缀处理，调试用）；
- UI 项的类型与字段、页面块类型、动作四种写法（`route` / `dispatch` / `command` / `none`）、
  按 `host.info().ui` 降级、图标要自带 svg —— 见 [plugin-ui.md](plugin-ui.md) 与
  [plugin-api.md](plugin-api.md) §9.1；本插件页面的路由是 `ext://<自己的插件id>/<视图id>`，
  视图 id 必须与能力短名一致。

---

## 13. 定时器与工具

```js
const t = musicxx.timer.setInterval(() => { /* ... */ }, 30000);
musicxx.timer.setTimeout(fn, 500);
musicxx.timer.clear(t);
// 全局别名：setTimeout / setInterval / clearTimeout / clearInterval（与 musicxx.timer 同一实现）

musicxx.util.formatTime(83000);       // "1:23"
musicxx.util.urlEncode("a b");        // "a%20b"
musicxx.util.urlDecode("a%20b");      // "a b"
musicxx.util.json;                    // JSON 对象（stringify / parse）
musicxx.util.now();                   // Date.now()
```

- 定时器由插件框架按实例管理，插件停用 / 卸载时自动清理；
- 精度：插件框架按 ≤ 50 ms 的粒度轮询到期定时器，所以 `setTimeout(fn, 0)` 到实际执行之间可能有几十毫秒
  延迟 —— 不要用它做高精度计时。

---

## 14. 一个完整例子

`plugins/example_js/plugin.js` 是可直接照抄的完整插件，它把常用东西都串了一遍：

| 位置 | 演示了什么 |
|---|---|
| `musicxx.hooks.register("musicxx.player.beforePlaySong")` | 裁决型钩子：命中就跳过本曲 |
| `musicxx.hooks.register("musicxx.song.changed")` | 观察型钩子 + 状态镜像 |
| `musicxx.hooks.register("musicxx.player.error")` | 错误裁决（换源建议） |
| `musicxx.hooks.register("musicxx.player.speed")` | 异步裁决（Promise）的写法 |
| `musicxx.timer.setInterval` | 后台定时任务（心跳日志） |
| `musicxx.ui.registerEntry` | 主页入口 + 歌曲菜单项 |
| `musicxx.capability.register("card"/"settings")` | 功能页与设置页（自绘） |
| `musicxx.storage.getConfig/setConfig` | 读写 `config.json`（默认值由脚本给） |
| `musicxx.capability.call` | 跨插件调用（JS ↔ 原生） |
| `musicxx.net.fetch` | 走主程序网络栈的请求 |
| `musicxx.stats.reportMetric` | 自报指标（只展示） |

只看渲染槽位的话读 `plugins/example_js_shader/plugin.js`（背景样式 + 速率设置页 + 页面内联 `Shader` 块）；
只关心某一个特性时读对应的单特性示例：`plugins/example_js_vars/`（变量通道）、
`plugins/example_js_multi_script/`（清单 `scripts` 多脚本）、`plugins/example_js_async/`（裁决处理器返回 Promise）、
`plugins/example_js_font/`（动态加载字体并设为全局字体）。

动态库侧的对照读：`plugins/example_native/example_native.cpp` 与 `plugins/example_js/plugin.js` **行为等价**
（钩子 / 状态镜像 / 动作 / 事件 / 能力 / 功能点 / UI / 通知各一遍），同一个功能想看两边写法时对照着读最省事。

---

## 15. 调试与排障

**开发流程**

```text
① 写 plugin.yaml + plugin.js
② 把整个目录放进用户插件目录（管理页「打开插件目录」），或打包 zip 用「从压缩包安装」
③ 管理页 →「重新扫描」→ 启用（加载时执行脚本顶层）
④ 触发钩子 / 打开页面看效果；日志看「插件详情 → 日志」
⑤ 改了 plugin.js：管理页「重载」（或禁用再启用）→ 脚本会重新执行
```

不点管理页也可以（AI agent / 自动化常用）：`musicxx-cli plugin install "<zip绝对路径>" --enable`
→ `plugin list` / `plugin logs` → 改完 `plugin reload <插件id>`。完整用法与 MCP 工具见
[plugin-agent-cli.md](plugin-agent-cli.md)。

**日志**：`console.*` 与 `musicxx.host.log(...)` 都进插件日志；插件框架自己的日志设环境变量
`MUSICXX_EXTERN_PLUGIN_LOG_STDERR=1` 可以打印到 stderr（排查加载失败时很有用）。

**统计**：管理页「外部插件 → 调试」分页与「插件详情 → 运行统计」能给到钩子调用次数 / 耗时 / 失败（只观测不限制：没有超时与暂停派发）、
插件声明的注册项与计数，以及每个 JS 实例的 `hooks` / `capabilities` / `subscriptions` /
`timers` / `pendingActions` / `jsRuns` / `errors` / `jsHeapBytes` / `asyncHookSettled`。
插件里也能自己读：`musicxx.stats.getSelf()`。

**应用卡住时**：主程序日志目录下有一份 `host_call.log`（进插件框架调用的同步轨迹，卡住后
**最后一行就是没有返回的那次调用**）；配合「插件详情 → 日志」一起看，就能区分是脚本死循环还是动作等待。

**排障**：接口本身的坑（动作没实现、镜像键写错、设置项读错、处理器里等动作……）整理在
[plugin-api.md](plugin-api.md) §12，那里是两种形态共用的一份；下面是 JS 特有的。

**常见坑**

| 现象 | 原因 / 处理 |
|---|---|
| 点入口提示「插件『<插件id>』没有提供『xxx』」 | 入口动作指向的能力没注册；页面视图 id 必须与能力短名一致 |
| 页面打开后提示「插件没有提供任何内容块」 | 视图没有 `blocks`，或块类型名全部拼错（未识别的块会被忽略，不报错） |
| 改了 `config.json` 页面没变 | 页面是打开时取的能力结果：改完再调一次能力返回 `{view:...}`，或重新打开页面 |
| 设置项改完重启又变回默认 | 读取时把「对象外壳」当成了值：`musicxx.storage.get/getConfig` 的应答**就是值本身** |
| 设置项被"改回去" | 读配置是异步的：读回来的旧值后到，会覆盖用户刚改的值（示例插件用「已经改过就不再覆盖」处理） |
| 动作一直失败 `action_not_registered` | 该动作当前版本没有实现（清单见 [plugin-api.md](plugin-api.md) §3.3） |
| 钩子不触发 | 该钩子在 [plugin-hooks.md](plugin-hooks.md) 里标「未接入」（主程序还没有接） |
| 处理器被调用多次 | 用**不同 `ownerTag` 注册了同一个钩子**：JS 侧只保留一个处理器，框架侧却留下多条注册，于是每次派发都会重复调用。同一个钩子只注册一次，或注销时用同一个 `ownerTag` |
| 定时器/初始化执行两遍 | 每次启用都会重新执行脚本：注册要写成幂等的 |
| 脚本报错但插件仍在运行 | 运行期异常只记日志（顶层抛异常才会加载失败）；先看日志 |
| 主程序整体卡住 | 脚本里有死循环或长时间同步计算（共享 JS 线程）。插件框架不打断脚本、也没有中断开关，只能重启主程序 —— 写脚本时自己保证循环能结束 |

---

## 16. v1 边界

| 边界 | 说明 |
|---|---|
| 异步裁决 | **已支持**：裁决处理器可以返回 Promise（插件框架一直等到结算，结算后裁决生效）；同步返回仍是最省时的写法 |
| 异步能力 | **已支持**：能力处理器可以返回 Promise（插件框架会等到结算再给调用方结果）；长时间后台任务仍建议用动作请求 / 事件 / 定时器 |
| 动作超时 | 缺省 5000 ms，`timeoutMs <= 0` 时**仍按 5000 ms**（JS 封装没有"不设超时"这一档；要长时间等就显式给大一点的值） |
| 跨插件能力超时 | 缺省 3 s；`<= 0` 时插件框架仍按 3 s 处理（不是"一直等"） |
| 资源限制 | 插件框架不限制 JS 内存、执行时长与 JS插件数量；死循环会占住共享 JS 线程（没有中断开关） |
| 定时器精度 | 框架线程按 ≤ 50 ms 粒度轮询，不适合高精度计时 |
| 与动态库插件的差别 | JS插件不能直接调用系统 API（没有 `require`/`fs`/`fetch`），联网走 `musicxx.net.fetch`；不能注册原生线程；能力处理器可以是异步的（返回 Promise，插件框架等它结算） |
