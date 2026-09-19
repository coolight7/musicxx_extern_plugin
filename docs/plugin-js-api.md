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
| `musicxx.storage` | `get` `set` `remove` `list` |
| `musicxx.ui` | `notify` `toast` |

动作全名与权限的对应关系见方案 §4.8；未授权动作会被 Dart 侧直接拒绝（`permission_denied`）。

### 3.5 事件

```js
musicxx.events.subscribe("musicxx.state.changed", (payload, topic) => { /* ... */ });
musicxx.events.publish("plugin.my_plugin.hello", { at: Date.now() });
```

- 订阅：**顶层声明**（v1 不支持运行时动态订阅）；可订阅官方 `musicxx.*` 或其它插件的 `plugin.<id>.*` 主题。
- 发布：只能发布官方主题或**自己命名空间**（`plugin.<自己的插件 id>.*`）的主题，冒充他人会被拒绝。

### 3.6 能力（供 Dart / 其它插件调用）

```js
musicxx.capability.register("probe", (args) => ({ ok: true, args }));
```

- 能力全名 = `plugin.<插件 id>.<短名>`，Dart 侧 `MusicxxPluginManager.call(id, method)` 可调用。
- v1：能力处理器必须**同步返回**可 JSON 序列化的结果（返回 Promise 会以 `capability_async_not_supported` 失败）；
  `musicxx.capability.call`（跨插件调用）暂未支持。

### 3.7 定时器与工具

```js
const t = musicxx.timer.setInterval(() => { /* ... */ }, 30000);
musicxx.timer.clear(t);
musicxx.timer.setTimeout(fn, 500);
// 全局别名: setTimeout / setInterval / clearTimeout / clearInterval

musicxx.util.formatTime(83000);      // "1:23"
musicxx.util.urlEncode("a b");       // "a%20b"
musicxx.util.json.stringify({a: 1});
musicxx.util.now();
```

定时器由宿主按实例管理：插件停用/卸载时自动清理。

### 3.8 其它

```js
musicxx.version   // 宿主 JS 运行时版本
musicxx.pluginId  // 本插件 id
```

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

完整示例见仓库 `example/example_js/`（与 `example/example_native/` 行为等价）。

## 5. 调试与排障

- 管理页「详情」里有插件日志流（`console.*` 与 `musicxx.host.log` 都进这里）；
- 脚本顶层报错会让装载失败并在管理页显示原因；运行期异常只记日志、不影响宿主；
- 宿主调试信息（管理页「调试信息」/`musicxx_extern_plugin_debug_info`）里有 JS 运行时的 `plugins[]` 段：
  `hooks/capabilities/subscriptions/timers/pendingActions/jsRuns/errors` 都可直接读到；
- 观察型钩子的回传事件（`musicxx.hook.observe`、`musicxx.js.console`）默认只在 debug 构建开启。

## 6. v1 已知边界（后续版本计划）

| 边界 | 说明 |
|---|---|
| `capability.call` | 跨插件调用能力（v1 只支持注册） |
| 动态订阅/注销 | 订阅只在顶层声明（v1 不支持运行期增删） |
| 异步裁决 | 裁决型钩子必须同步返回（plan §5.4 的 runner isolate 派发落地后可支持） |
| 声明式 UI | `musicxx.ui.registerEntry` 等 UI 扩展项（M5） |
| 网络 | 宿主代理通道 `musicxx.net.fetch`（需要 `musicxx.net` 权限；M5 落地后可用） |
| 资源限制 | 宿主不限制 JS 内存/执行时长（决策 13）；死循环会占住共享 JS 线程，可由管理页禁用该插件 |
