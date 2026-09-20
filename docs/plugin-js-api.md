# musicxx JS 插件作者指南（v1）

> 对应方案：`resource/history/extern-plugin-impl/plan.md` 的 M3（零编译 JS 插件，§4.4 / §4.6 / §7.2）。
> 本文只写"插件作者需要的部分"：目录结构、`musicxx` API、生命周期与硬约束。
> 钩子总表（id / 模式 / 合并策略 / 载荷）见 `plugin-hooks.md`（由 `tools/hooks.def.json` 生成）。

## 1. 一个 JS 插件长什么样

```text
my_plugin/
├── plugin.yaml     # 清单（必填）
└── plugin.js       # 脚本（必填；清单 entry 也可指向别的 .js 文件）
```

`plugin.yaml` 示例：

```yaml
name: my_plugin                  # 唯一 id（宿主与 Dart 侧都用它）
kind: js                         # js = 零编译脚本插件
entry: plugin.js                 # 可省略，缺省即 plugin.js
version: 1.0.0
api_version: 1                   # 宿主插件 API 版本（当前 1）
author: "你的名字"
description: "插件说明"
platforms: [windows, linux, macos, android, ios]

permissions:                     # 声明式权限：安装时一次性确认，运行时只校验
  - musicxx.player.control
  - musicxx.ui
  - musicxx.storage

# 可选：宿主生成的配置表单（读写插件目录下的 config.json）
settings_schema:
  - { key: "enabledFeature", type: "bool", title: "启用特性", default: true }
  - { key: "apiKey", type: "string", title: "API Key", secret: true }
  - { key: "mode", type: "select", title: "模式", default: "a",
      options: [ { value: "a", label: "模式 A" }, { value: "b", label: "模式 B" } ] }
```

安装方式：管理页「从压缩包安装」（打包成 `.zip`，顶层就是插件目录内容）或直接把目录放进插件目录后扫描。

## 2. 生命周期

| 阶段 | 发生了什么 |
|---|---|
| 装载（`kind: js`） | 宿主为该插件登记一个合成内置实例 `js:<插件id>`，建立独立 `JSRuntime`/`JSContext` |
| 脚本顶层执行 | 顶层**必须同步**完成注册（钩子/能力/订阅）；顶层抛异常 = 装载失败并回滚 |
| 启用中 | 钩子处理器与定时器持续工作；宿主读取（状态镜像）与写入（动作请求）都可用 |
| 停用（disable） | 脚本注册全部摘除、定时器清空、`JSRuntime` 释放；再次启用会**重新执行一遍脚本** |
| 卸载（unload） | 实例销毁；插件目录与私有数据保留（除非用户在管理页选择彻底卸载） |

**硬约束**

1. 脚本顶层不能有 `await`（顶层 `await` 不允许）；异步逻辑请放到钩子、定时器或 Promise 回调里。
2. 全部 JS 代码运行在宿主的**一条共享 JS 线程**上（多个 JS 插件共用），因此任何回调都要尽快返回，不要在里面做长时间同步计算；需要耗时工作请拆成小片段（定时器/异步动作）。
3. 裁决型钩子是**同步**的：处理器必须同步返回裁决对象（返回 Promise 会被忽略并记日志）。处理时间超过 100 ms 的部分按"无裁决"处理（不打断脚本）。
4. `require`、`fs`、`fetch`、原生模块都不存在；所有宿主能力都要经 `musicxx.*`（异步动作请求）。
5. 插件之间不做隔离（同进程、同一 JS 线程）；不要依赖全局副作用，状态请放在插件私有存储里。

## 3. `musicxx` 全局对象

### 3.1 钩子

```js
musicxx.hooks.register(id, { mode: "decision" | "observe", priority: 0, ownerTag: "" }, fn);
musicxx.hooks.unregister(id);
musicxx.hooks.has(id);
```

- `mode`：`decision`（可裁决，同步返回；默认）或 `observe`（只观察，返回值忽略）。
- `priority`：小者先执行；同一个 `(插件, 钩子 id, ownerTag)` 重复注册是**覆盖**。
- `fn(ctx)`：`ctx` 是钩子载荷（JSON 对象，字段见 `plugin-hooks.md`）。
- 裁决返回形如 `{ action: "continue" | "skip" | "cancel" | "replace", patch: { ... }, error?: "" }`；
  返回 `null` / `undefined` 表示"不裁决"。

```js
musicxx.hooks.register("musicxx.player.beforePlaySong", { mode: "decision" }, (ctx) => {
    if (ctx.song && ctx.song.name.includes("广告")) return { action: "skip" };
    return null;
});
```

### 3.2 状态镜像（同步只读）

```js
const song = musicxx.state.get("musicxx.state.song");   // null = 该键还没推送过
```

常用键：`musicxx.state.app` / `player` / `song` / `playlist` / `lyric` / `library` / `env`（由 musicxx 推送，字段见方案 §4.7）。

### 3.3 宿主信息与日志

```js
musicxx.host.info();        // { appVersion, platform, language, dataDir, userPluginDir, ... }
musicxx.host.log(2, "文字"); // 0 trace / 1 debug / 2 info / 3 warn / 4 error → 宿主日志 + 事件
musicxx.host.configPath();  // 本插件 config.json 路径（宿主按插件目录推导）
console.log/info/warn/error // 转发到宿主日志
```

### 3.4 动作（异步，Promise）

所有动作统一入口是 `musicxx.call(名字, 参数, 超时毫秒)`（默认 5s，下限 1s、上限 60s）。
失败时 Promise 拒绝，`err.message` 里带 `permission_denied` / `action_not_registered` / `action_timeout` 等原因。

```js
await musicxx.player.play({ pid: 123, index: 4 });
await musicxx.player.next();
await musicxx.player.setVolume(0.6);
await musicxx.ui.notify({ text: "你好" });
await musicxx.storage.set("myKey", { a: 1 });
const value = await musicxx.storage.get("myKey", null);
const songs = await musicxx.library.querySongs({ pid: 123, keyword: "周杰伦" });
const lrc   = await musicxx.lyrics.getCurrent();
```

分类便捷封装（都等价于对应 `musicxx.call`）：

| 分类 | 方法 |
|---|---|
| `musicxx.player` | `play` `pause` `toggle` `stop` `next` `prev` `seek` `setVolume` `setSpeed` `setLoopMode` |
| `musicxx.library` | `querySongs` `querySonglists` `playSong` `playSonglist` |
| `musicxx.lyrics` | `getCurrent` |
| `musicxx.storage` | `get` `set` `remove` `list` `getConfig` `setConfig` |
| `musicxx.net` | `fetch` `download`（宿主代理通道，需 `musicxx.net` 权限） |
| `musicxx.ui` | `notify` `toast` `dialog` `openRoute`（需 `musicxx.ui` 权限） |
| `musicxx.stats` | `getSelf` `reportMemory` `reportMetric`（只观测不限制） |

动作全名与权限的对应关系见方案 §4.8；未授权动作会被 Dart 侧直接拒绝（`permission_denied`）。

**界面反馈**：

```js
await musicxx.ui.notify({ text: "已完成" });                       // 站内提示（失败也提示）
const answer = await musicxx.ui.dialog({                          // 确认弹窗（敏感操作前先问用户）
    title: "删除缓存", content: "会删除该插件的缓存文件，是否继续？",
    textConfirm: "继续", textCancel: "取消",
});
if (answer && answer.confirmed) { /* 用户点了确认 */ }

await musicxx.ui.openRoute("ext://my_plugin/card");               // 只能打开本插件自己的页面
await musicxx.ui.openRoute("musicxx:settings");                   // 官方页面用白名单键（见下）
```

- 弹窗标题会自动带上插件来源前缀（`『<插件id>』…`），避免插件伪装成宿主自己的提示；
- `openRoute` 只允许两类目标：**本插件自己的** `ext://<本插件id>/<视图id>`，以及官方页面白名单
  （`musicxx:settings` 设置页、`musicxx:plugins` 插件管理、`musicxx:player` 播放页、`musicxx:home` 音乐主页、
  `musicxx:search` 搜索、`musicxx:localSongs` 本地歌曲、`musicxx:history` 播放记录、`musicxx:lyrics` 歌词、`musicxx:about` 关于）；
  其它地址会被拒绝并返回 `不允许打开的页面`。

**插件配置（`config.json`）**：用户在管理页「详情 → 配置」里改的值与 `settings_schema` 声明的默认值都存在插件目录的 `config.json`。
脚本用 `musicxx.storage.getConfig(key, 默认值)` 读、`setConfig(key, value)` 写（等价于 `namespace: "config"` 的 `storage.get/set`），
与私有 KV（`kv.json`）互不影响。

**网络（宿主代理通道）**：

```js
const resp = await musicxx.net.fetch({
    url: "https://api.example.com/items?page=1",
    method: "GET",                     // GET/POST/PUT/PATCH/DELETE/HEAD（缺省 GET）
    headers: { "X-Token": "..." },     // 只允许合法头名；宿主不注入任何身份信息
    body: { a: 1 },                    // 对象自动 JSON 编码；字符串原样发送
    timeoutMs: 15000,                  // 1000 ~ 60000（缺省 15000）
    responseType: "json",              // text（缺省，body 是字符串）/ json / bytes（bodyBase64）
    maxBytes: 2097152,                 // 响应体上限（超出截断并标记 truncated），上限 16 MiB
});
// resp = { ok, status, url, headers, contentType, body | bodyBase64, bodyBytes, truncated }
if (resp.ok && resp.status === 200) { /* 用 resp.body */ }

// 下载到插件私有数据目录（<data>/downloads/）：
const file = await musicxx.net.download({ url: "https://.../a.mp3", fileName: "a.mp3" });
// file = { ok, status, path, bytes }
```

- **非 2xx 也会正常返回**（`ok: true` + `status`），由脚本自己判断；只有传输失败/超时才是 `ok: false`；
- `ok: false` 时 `error` 是 `请求失败：...`（网络/超时）；宿主不限定可访问的域名（任意 http/https 地址都可以请求）；
- 插件**不需要**这条通道也能联网（原生插件可用系统 API；JS 由于宿主内没有 `fetch`，用本通道最方便）——
  `musicxx.net` 权限只表示"允许使用宿主代理通道"（plan §7.4 第 5 条）；
- `fileName` 只能是文件名：路径分隔符与 `..` 会被拒绝，落点固定在插件自己的数据目录内。

### 3.5 声明式 UI 扩展（不写 Flutter 代码）

插件只做**声明**，渲染由宿主（musicxx 应用）负责。支持的类型：

| `type` | 用途 | `data` 必需字段 |
|---|---|---|
| `home.entry`（`musicxx.ui.home.entry`） | 功能主页入口按钮 | `title` |
| `song.action`（`musicxx.ui.song.action`） | 歌曲菜单项 | `title` |
| `playlist.action` | 歌单菜单项 | `title` |
| `settings.page` | 设置页 | `title` |
| `overlay.widget` | 播放页只读信息块（应用侧已渲染） | `position`、`content` |

`overlay.widget` 的 `position` 与 `content.kind`（应用侧定义，其他取值会被忽略/归一化）：

| 字段 | 取值 | 说明 |
|---|---|---|
| `position` | `player.top` | 沉浸式播放页顶部栏下方 |
| | `player.bottom` | 进度条上方（**未识别的位置按这里渲染**） |
| `content.kind` | `text` | `{kind:"text", text:"..."}` |
| | `markdown` | 同上（首期按纯文本渲染） |
| | `list` | `{kind:"list", items:["a", {title:"b", depict:"c"}]}` |
| | `progress` | `{kind:"progress", title?:"...", value:1, total:4}` |

```js
musicxx.ui.registerEntry({
    name: "overlayInfo",
    type: "overlay.widget",
    order: 20,
    data: {
        position: "player.top",
        title: "附加信息",
        content: { kind: "text", text: "只读展示；点击可跳转插件页面" },
        action: { kind: "route", route: "ext://example_js/card" },
    },
});
```

```js
musicxx.ui.registerEntry({
    name: "card",                       // 短名；id 会自动变成 plugin.<本插件id>.card
    type: "home.entry",                 // 简称或全名都可以
    order: 100,                         // 小者靠前（同权重按注册顺序）
    data: {
        title: "我的入口",
        subtitle: "示例插件提供的入口",
        icon: "addition",               // musicxx 内置 svg 名（未知名字会退化为默认图标）
        action: { kind: "route", route: "ext://example_js/card" },
    },
});

musicxx.ui.updateEntry("card", { title: "新标题", subtitle: "已更新", icon: "addition" });
musicxx.ui.unregisterEntry("card");
musicxx.ui.entries();                    // 本插件已注册的项（脚本侧镜像）
```

动作（`data.action`）三种形态：

| 形态 | 字段 | 说明 |
|---|---|---|
| 打开插件页面 | `{kind:"route", route:"ext://<本插件id>/<视图id>"}` | 只能指向本插件；见下方"插件页面" |
| 调用本插件能力 | `{kind:"capability", name:"<短名>", args:{...}}` | 宿主调用 `plugin.<本插件id>.<短名>` |
| 调用官方动作 | `{kind:"action", name:"musicxx.<域>.<动作>", args:{...}}` | 与 `musicxx.call` 同一份实现与权限规则 |

**插件页面（`ext://<插件id>/<视图id>`）**：宿主打开页面时会调用插件的**同名能力**
（短名 = 视图 id，参数 `{"view":"<视图id>"}`），脚本返回视图描述：

```js
musicxx.capability.register("card", function (args) {
    return {
        title: "插件页面",
        subtitle: "可选副标题",
        blocks: [
            { kind: "text", text: "一段说明", style: "main" },     // main | cross | thin
            { kind: "divider" },
            { kind: "button", title: "播放/暂停", style: "primary",
              action: { kind: "action", name: "musicxx.player.toggle" } },
            { kind: "list", items: [
                { id: "a", title: "条目", subtitle: "说明", right: "3", icon: "addition",
                  action: { kind: "capability", name: "openItem", args: { id: "a" } } },
            ]},
        ],
    };
});
```

- 能力处理器必须**同步返回**（返回 Promise 会被拒绝，见 §3.6）；
- 能力返回 `{ view: {...} }` 时，宿主用新视图直接刷新当前页面（翻页/刷新）；
- 未识别的块类型会被忽略（向前兼容）。

**设置页（`settings.page`）**：应用「设置」里会出现插件声明的设置页入口，页内控件读写插件的 `config.json`
（与清单 `settings_schema` 生成的表单是同一份配置）：

```js
musicxx.ui.registerEntry({
    name: "settings",
    type: "settings.page",
    order: 100,
    data: {
        title: "我的插件设置",
        depict: "可选说明",
        groups: [
            {
                title: "基础",
                items: [
                    { kind: "switch", key: "enabledFeature", title: "启用特性", depict: "说明文字" },
                    { kind: "input", key: "apiKey", title: "API Key", placeholder: "粘贴密钥" },
                    { kind: "password", key: "secret", title: "密钥" },       // 默认隐藏
                    { kind: "number", key: "limit", title: "上限", min: 1, max: 100 },
                    { kind: "select", key: "mode", title: "模式",
                      options: [ { value: "a", label: "模式 A" }, { value: "b", label: "模式 B" } ] },
                    { kind: "text", key: "note", title: "备注" },             // 多行
                    { kind: "info", text: "只读说明（不写入配置）" },
                    { kind: "button", title: "立即执行",
                      action: { kind: "capability", name: "runNow" } },
                    { kind: "divider" },
                ],
            },
        ],
    },
});
```

- 控件值改动后立刻写入 `config.json`（`kind` 为 `info`/`button`/`divider` 的项不写配置）；
- `musicxx.ui.updateEntry("settings", { ... })` 可以整批换掉页面结构（脚本运行期也能改）；
- 与清单 `settings_schema` 的区别：`settings_schema` 由宿主生成表单（插件无需声明结构），
  `settings.page` 由插件完全控制页面结构（可带 `info`/`button`）。

### 3.6 事件

```js
musicxx.events.subscribe("musicxx.state.changed", (payload, topic) => { /* ... */ });
musicxx.events.publish("plugin.my_plugin.hello", { at: Date.now() });
musicxx.events.unsubscribe("musicxx.state.changed");
```

- 订阅：顶层声明或运行期动态订阅都可以；可订阅官方 `musicxx.*` 或其它插件的 `plugin.<id>.*` 主题。
  `unsubscribe` 之后宿主侧订阅仍保留，但该主题的回调不再触发（同一主题再次订阅会复用原订阅）。
- 发布：只能发布官方主题或**自己命名空间**（`plugin.<自己的插件 id>.*`）的主题，冒充他人会被拒绝。

### 3.7 能力（注册 + 跨插件调用）

```js
// 注册：能力全名 = plugin.<插件 id>.<短名>；Dart 侧用 MusicxxPluginManager.call(id, method) 调用
musicxx.capability.register("probe", (args) => ({ ok: true, args }));

// 调用别的插件（返回 Promise）：
const info = await musicxx.capability.call("example_native", "probe", {});
const again = await musicxx.capability.call("example_js", "plugin.example_js.probe"); // 全名也可以
```

- 处理器必须**同步返回**可 JSON 序列化的结果（返回 Promise 会以 `capability_async_not_supported` 失败）。
- `capability.call(插件id, 能力名, 参数?, 超时毫秒?)`：
  - 目标是 **JS 插件** → 在共享 JS 线程上直接调用（同一线程，结果立即就绪）；
  - 目标是**原生插件** → 由宿主线程执行、脚本**不阻塞**（宿主线程可能正在等 JS 处理器），
    超时默认 3 s（下限 1 s）；失败时 Promise 拒绝，`err.message` 里带原因。

### 3.8 统计与自检

```js
musicxx.stats.getSelf();                       // 本实例统计（钩子/能力/订阅/定时器/脚本执行/堆用量/执行上限命中）
musicxx.stats.reportMemory(1024 * 1024);       // 可选：自报内存占用（只观测不限制）
musicxx.stats.reportMetric("cacheHits", 42);   // 可选：自报自定义指标（名字须 plugin.<id>.<名>）
```

统计只用于展示与排障（plan §4.11），任何指标超标都**不会**导致插件被暂停或卸载；
原生插件的内存无法精确统计，`reportMemory` 是"插件自报"，仅供参考。

### 3.9 定时器与工具

```js
const t = musicxx.timer.setInterval(() => { /* ... */ }, 30000);
musicxx.timer.clear(t);
musicxx.timer.setTimeout(fn, 500);
// 全局别名: setTimeout / setInterval / clearTimeout / clearInterval

musicxx.util.formatTime(83000);      // "1:23"
musicxx.util.urlEncode("a b");       // "a%20b"
musicxx.util.json.stringify({a: 1});
musicxx.util.now();

musicxx.version   // 宿主 JS 运行时版本
musicxx.pluginId  // 本插件 id
```

定时器由宿主按实例管理：插件停用/卸载时自动清理。

## 4. 一个最小可用例子

```js
// 跳过名字里带"广告"的曲目，并在切歌时记录一条日志
musicxx.hooks.register("musicxx.player.beforePlaySong", { mode: "decision" }, (ctx) => {
    const name = (ctx.song && ctx.song.name) || "";
    return name.includes("广告") ? { action: "skip" } : null;
});

musicxx.hooks.register("musicxx.song.changed", { mode: "observe" }, (ctx) => {
    musicxx.host.log(2, "正在播放: " + ((ctx.song && ctx.song.name) || "未知"));
});

musicxx.capability.register("ping", () => ({ pong: true }));
```

完整示例见仓库 `plugins/example_js/`（与 `plugins/example_native/` 行为等价）。

## 5. 调试与排障

- 管理页「详情」里有插件日志流（`console.*` 与 `musicxx.host.log` 都进这里）；
- 脚本顶层报错会让装载失败并在管理页显示原因；运行期异常只记日志、不影响宿主；
- 宿主调试信息（管理页「调试信息」/`musicxx_extern_plugin_debug_info`）里有 JS 运行时的 `plugins[]` 段：
  `hooks/capabilities/subscriptions/timers/pendingActions/jsRuns/errors` 都可直接读到；
- 观察型钩子的回传事件（`musicxx.hook.observe`、`musicxx.js.console`）默认只在 debug 构建开启。

## 6. v1 已知边界（后续版本计划）

| 边界 | 说明 |
|---|---|
| 异步裁决处理器 | 裁决处理器的 JS 函数必须**同步返回**裁决对象；返回 Promise 时按"不裁决"处理（需要异步数据的场景请先取好数据，或用 `musicxx.hooks.register(..., {mode:"observe"})` + 动作请求）。宿主对裁决型钩子有等待预算（声明为 `dispatch: "async"` 的钩子由宿主异步派发，Dart 侧不阻塞） |
| 异步能力 | 能力处理器必须同步返回结果（返回 Promise 会失败） |
| 资源限制 | 宿主不限制 JS 内存/执行时长（决策 13）；死循环会占住共享 JS 线程。可在配置里显式开启**可选执行上限**（`jsExecGuardMs`），开启后单次脚本执行超时会被中断并计入统计 |
