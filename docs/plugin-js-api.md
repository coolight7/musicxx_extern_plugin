# musicxx JS 插件作者指南（v1）

JS 插件是**零编译**形态：一个目录（`plugin.yaml` + `plugin.js`）即可，宿主用内置的 QuickJS
运行时执行脚本，脚本通过全局对象 `musicxx` 使用宿主能力。

| 主题 | 文档 |
|---|---|
| 钩子 id / 模式 / 派发 / 预算，以及已埋点钩子的载荷与裁决语义 | [plugin-hooks.md](plugin-hooks.md) |
| 界面（UI 项、插件页面、设置页）的字段与块类型 | [plugin-ui.md](plugin-ui.md) |
| 播放页背景（shader bundle 打包与 uniform 契约） | [plugin-shader-bundle.md](plugin-shader-bundle.md) |
| C++ 动态库插件（钩子/能力/UI 的写法等价） | [plugin-native-api.md](plugin-native-api.md) |
| 可运行的完整示例 | `plugins/example_js/`、`plugins/example_js_shader/` |

---

## 1. 一个 JS 插件长什么样

```text
my_plugin/
├── plugin.yaml     # 清单（必填）
├── plugin.js       # 脚本（必填；清单 entry 也可指向别的 .js 文件）
├── shader/         # 可选：随插件分发的资源（如 shader bundle）
└── config.json     # 可选：插件自己的配置（运行时由脚本读写）
```

```yaml
name: my_plugin                  # 插件 id（唯一；同名视为同一插件的升级覆盖）
kind: js                         # js = 零编译脚本插件（缺省时按 entry 推导）
entry: plugin.js                 # 可省略，缺省就是 plugin.js
version: 1.0.0
api_version: 1                   # 兼容的插件 API 版本（宿主当前是 1）
author: "你的名字"
description: "插件说明"
platforms: [windows, linux, macos, android, ios]   # 不写 = 不限
depends: [other_js_plugin]       # 必选依赖：缺失时拒绝加载
optional_depends: [maybe_plugin] # 可选依赖：只影响加载顺序

permissions:                     # 声明式权限：只做展示，运行时不校验
  - musicxx.player.control
  - musicxx.ui
  - musicxx.storage
```

安装：管理页「从压缩包安装」（`.zip`，顶层是插件目录内容），或把目录放进
`<应用支持目录>/musicxx/extern_plugin/plugins/<id>/` 后「重新扫描」。JS 插件在
**所有平台**都能用（包括只允许 JS 插件的 iOS / OHOS）。

---

## 2. 生命周期与硬约束

| 阶段 | 发生了什么 |
|---|---|
| 装载（`kind: js`） | 宿主为该插件登记一个合成实例 `js:<插件id>`，建立独立的 `JSRuntime`/`JSContext` |
| 脚本顶层执行 | 顶层**必须同步完成注册**（钩子 / 能力 / 订阅）；顶层抛异常 = 装载失败并回滚 |
| 启用中 | 钩子处理器与定时器工作；同步读状态镜像、异步发动作请求 |
| 停用（disable） | 脚本的注册全部摘除、定时器清空、`JSRuntime` 释放；**再次启用会重新执行一遍脚本** |
| 卸载（unload） | 实例销毁；插件目录与私有数据保留（除非用户在管理页选择彻底卸载） |

硬约束（踩过坑的都在这里）：

1. **顶层不能有 `await`**；异步逻辑放到钩子、定时器或 Promise 回调里。
2. 所有 JS 代码跑在宿主的**一条共享 JS 线程**上（多个 JS 插件共用一个线程、各自独立运行时）：
   回调要尽快返回，不要在里面做长时间同步计算。
3. 裁决型钩子的**推荐写法是同步返回**裁决对象；需要 `await` 才能决定时可以先同步判断
   "这次要不要裁决"，只在需要的那次返回 Promise（见 §4.3）。JS 处理器链最多等 **100 ms**。
4. `require`、`fs`、`fetch`、原生模块都**不存在**；宿主能力全部经 `musicxx.*`。
5. 插件之间不做隔离（同一条 JS 线程）；不要依赖全局副作用，状态放插件自己的存储里。
6. 钩子处理器抛异常只记日志并跳过本次裁决；**同一个处理器连续 3 次失败会被暂停派发 60 秒**
   （超时不算失败；统计里看得到 `paused`）。
7. 脚本顶层与处理器里**都可以**读写 `musicxx`；但处理器里不要递归触发同一个钩子。
8. 脚本每次启用都会重新执行：**注册语句要写成幂等的**（重复执行不会累积副作用），
   定时器在 `stop` 时由宿主清理，不要假设它还在。

---

## 3. `musicxx` 全局对象

| 命名空间 | 用途 |
|---|---|
| `musicxx.hooks` | 注册 / 注销钩子处理器 |
| `musicxx.state.get` | 同步读状态镜像 |
| `musicxx.host` | 宿主信息、日志、配置路径 |
| `musicxx.call` | 通用动作请求（返回 Promise） |
| `musicxx.player` / `library` / `lyrics` / `storage` / `net` / `render` / `media` / `ui` / `stats` | 分类便捷封装（都等价于 `musicxx.call`） |
| `musicxx.events` | 事件总线（发布 / 订阅） |
| `musicxx.capability` | 注册自己的能力 / 调用别的插件的能力 |
| `musicxx.timer` / `musicxx.util` | 定时器与工具 |
| `musicxx.version` / `musicxx.pluginId` | 运行时版本（`"0.1.0"`）/ 本插件 id |
| `console` | `log` / `info` / `warn` / `error` 转发到宿主日志 |

---

## 4. 钩子

### 4.1 注册与注销

```js
musicxx.hooks.register(id, { mode, priority, ownerTag }, fn);
musicxx.hooks.unregister(id);
musicxx.hooks.has(id);
```

- `id` 必须是**全名**（官方 `musicxx.*`）；宿主的契约表里没有的钩子会被拒绝；
- `mode`：`"observe"` = 只观察（返回值忽略）；其它值（含不写）= `"decision"` 裁决型；
- `priority`：小者先执行；同优先级按注册顺序；
- `ownerTag`：同一插件在同一个钩子上区分多个处理器的标记（**不要用不同 ownerTag 注册同一个钩子**，
  见 §14 常见坑）；
- `fn(ctx)`：`ctx` 是钩子载荷（JSON 对象，字段见 [plugin-hooks.md](plugin-hooks.md)）；
  返回 `null` / `undefined` = 不裁决。

```js
// 观察型：切歌时打一条日志
musicxx.hooks.register("musicxx.song.changed", { mode: "observe" }, (ctx) => {
    musicxx.host.log(2, "正在播放: " + ((ctx.song && ctx.song.name) || "未知"));
});
```

顶层同步注册会在脚本执行结束后由引擎统一登记；**运行期**调用 `register` / `unregister` 同样生效
（宿主把它投递到宿主线程执行，不在脚本线程等待）。

### 4.2 裁决型钩子的返回值

```js
{ action: "continue" | "skip" | "cancel" | "replace", patch: { ... }, error?: "说明" }
```

`action` 的含义由各调用点决定（例如 `beforePlaySong` 的 `skip` = 跳过本曲）。当前版本已埋点的
裁决钩子只有三个，语义见 [plugin-hooks.md](plugin-hooks.md) 的「已埋点钩子的载荷与裁决」：
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

- 宿主最多等 **100 ms**：预算内结算 → 裁决生效；超预算才结算 → 按「无裁决」继续
  （**不打断脚本、不计失败**，迟到的结果被丢弃，可在统计里看到
  `asyncHookSettled` / `asyncHookTimeouts` / `asyncHookLateDrops`）；
- 所以异步路径要保证「拿不到结果时降级为不裁决」，并让来源明显快于 100 ms。

---

## 5. 状态镜像（同步只读）

```js
const song = musicxx.state.get("musicxx.state.song");   // 没推送过 → null
```

| 键 | 内容 | 当前版本 |
|---|---|---|
| `musicxx.state.app` | `{version, versionStr, platform, lang, branch, installId, isNight, firstRun}` | 已推送 |
| `musicxx.state.player` | `{state, position, duration, volume, speed, quality, srcKey, mediaType, cacheLength}` | 已推送（启动 / 切歌 / 播放状态变化时刷新） |
| `musicxx.state.song` | 当前歌曲的只读视图（`sid/name/artist/album/durationMs/year/genre/srcKey/audio[]/video[]`） | 已推送 |
| `musicxx.state.playlist` | `{pid, name, count, songNum, type, index, loopMode, autoPlayMode}` | 已推送 |
| `musicxx.state.env` | `{isPlaying, page, lanServerOn, userLogged}` | 已推送 |
| `musicxx.state.lyric` / `musicxx.state.library` | 预留 | **当前版本没有推送**（`get` 返回 `null`） |
| `musicxx.state.renderSlots` | 渲染槽位运行状态（谁在画、是否可见、尺寸、昼夜） | 按需推送（见 [plugin-shader-bundle.md](plugin-shader-bundle.md) §10） |

- 判断「是否在播放」用 `musicxx.state.env.isPlaying`（布尔）：`musicxx.state.player.state` 的取值
  在暂停/播放/停止事件里是小写 `play`/`pause`/`stop`，其它变化是枚举名 `Play`/`Pause`/`Stop`/`Completed`；
- **播放进度当前版本没有推送**：`musicxx.state.player.position` 只在启动、切歌与播放状态变化时刷新，
  `musicxx.player.position` 钩子也尚未埋点 —— 不要把它当每秒更新的进度用；
- 镜像里**不放临时直链与 token**；需要地址请自己请求（见 §7 网络）；
- 每个键更新都会推事件 `musicxx.state.changed`（载荷 `{key, value}`）：

  ```js
  musicxx.events.subscribe("musicxx.state.changed", (payload) => {
      if (payload.key === "musicxx.state.song") { /* 刷新自己的界面数据 */ }
  });
  ```

---

## 6. 宿主信息与日志

```js
musicxx.host.info();
// { appVersion, platform, language, dataDir, userPluginDir, builtinPluginDir, apiVersion, hostVersion }
//  platform: windows / linux / macos / android / ios / ohos
//  dataDir:  插件数据根目录（私有 KV 在 <dataDir>/<插件id>/data/kv.json）

musicxx.host.configPath();   // 本插件 config.json 的绝对路径
musicxx.host.log(2, "文字"); // 0 trace / 1 debug / 2 info / 3 warn / 4 error
console.log / info / warn / error   // 同上（映射到 info / warn / error）
```

日志会进「管理页 → 插件详情 → 日志」；宿主也会把它作为 `musicxx.plugin.log` 事件推给应用侧。

---

## 7. 动作（异步请求）

```js
const result = await musicxx.call("musicxx.<域>.<动作>", { ...参数 }, 超时毫秒?);
```

- 默认超时 **5000 ms**（宿主把实际生效值限制在 1 s ~ 60 s；超时会拒绝 Promise）；
- Promise 拒绝时 `err.message` 里带原因，例如 `action_not_registered: xxx`、
  `请求失败：...`、`status=...`；
- **动作不做权限校验**：清单里的 `permissions` 只做展示，不会拒绝任何调用；
- 动作未注册（当前版本没实现）会立刻失败，不会挂到超时。

### 7.1 分类便捷封装

| 分类 | 方法 |
|---|---|
| `musicxx.player` | `play(args?)` `pause()` `toggle()` `stop()` `next()` `prev()` `seek(ms\|args)` `setVolume(v)` `setSpeed(v)` `setLoopMode(mode)` |
| `musicxx.library` | `querySongs(args?)` `querySonglists()` `playSong(args)` `playSonglist(args)` |
| `musicxx.lyrics` | `getCurrent()` |
| `musicxx.storage` | `get(key, 默认值?)` `set(key, value)` `remove(key)` `list()` `getConfig(key, 默认值?)` `setConfig(key, value)` |
| `musicxx.net` | `fetch(options)` `download(options)` |
| `musicxx.render` | `list(args?)` `current(args?)` `select(id, args?)` |
| `musicxx.media` | `palette(args?)` `cover(args?)` |
| `musicxx.ui` | `notify(args)` `toast(args)` `dialog(args)` `openRoute(route, args?)` |
| `musicxx.stats` | `getSelf()` `reportMemory(bytes)` `reportMetric(name, value)` |

**当前版本真正注册的动作**（写全名调用时的清单）：

| 动作 | 参数 | 结果 |
|---|---|---|
| `musicxx.player.play` / `pause` / `toggle` / `stop` / `next` / `prev` | 无 | `{ok:true}` |
| `musicxx.player.seek` | `{positionMs}`（或 `position`） | `{ok:true}` |
| `musicxx.player.setVolume` | `{volume}`（0~1；> 1 视为百分比） | `{ok:true}` |
| `musicxx.player.setSpeed` | `{speed}` | `{ok:true}` |
| `musicxx.player.setQuality` | `{quality}`（音质枚举名） | `{ok:true}` |
| `musicxx.player.setLoopMode` | `{mode}`（循环模式枚举名） | `{ok:true}` |
| `musicxx.library.querySonglists` | 无 | `{mine:[...], local:[...], currentPid, currentName}`（每项 `{pid,name,count,songNum,type}`） |
| `musicxx.library.querySongs` | `{pid?, limit?=200, offset?=0}` | `{pid, total, songs:[{sid,name,artist,album,durationMs}]}` |
| `musicxx.library.playSong` | `{sid}`（须在当前歌单里） | `{ok:true}` |
| `musicxx.library.playSonglist` | `{pid, index?}` | `{ok:true}` |
| `musicxx.lyrics.getCurrent` | `{limit?=200}` | `{sid, lrcid, name, artist, lineCount, index, lines:[{timeMs,text}]}` |
| `musicxx.ui.notify` | `{text, kind?}`（`kind:"error"` 走错误提示） | `{ok:true}` |
| `musicxx.ui.toast` | `{text}` | `{ok:true}` |
| `musicxx.ui.dialog` | `{title?, content, textConfirm?, textCancel?}` | `{ok:true, confirmed:bool}` |
| `musicxx.ui.openRoute` | `{route, arguments?}` | `{ok:true}` 或 `{ok:false,error}` |
| `musicxx.storage.get` | `{key, namespace?}` | **值本身**（键不存在 → 空应答，JS 封装回退到默认值） |
| `musicxx.storage.set` | `{key, value, namespace?}` | `{ok:true}` 或 `{ok:false,error}` |
| `musicxx.storage.delete` | `{key, namespace?}` | 同上 |
| `musicxx.storage.list` | `{namespace?}` | `{keys:[...]}` |
| `musicxx.net.fetch` | 见 §7.2 | 见 §7.2 |
| `musicxx.net.download` | 见 §7.2 | `{ok, status, path, bytes}` |
| `musicxx.render.list` / `current` / `select` | 见 §10 | 见 §10 |
| `musicxx.media.palette` | `{force?}` | 见 §10 |
| `musicxx.media.cover` | `{size?, format?, includePath?}` | 见 §10 |
| `musicxx.stats.reportMemory` | `{bytes}` | `{ok:true}` |
| `musicxx.stats.reportMetric` | `{name, value}` | `{ok:true}` |
| `musicxx.host.openUrl` | `{url}`（只允许 http/https） | `{ok:true}` |
| `musicxx.host.clipboard` | `{text}` | `{ok:true}` |
| `musicxx.host.getPath` | 无 | `{pluginId, pluginDir, dataDir, logDir, platform}` |

> 名字里有、但**当前版本没有实现**的动作（调用会得到 `action_not_registered`）：
> `musicxx.player.setPitch` / `setMediaType`、`musicxx.library.querySonglist` / `addSong` /
> `removeSong` / `createSonglist` / `setSongInfo` / `search`、`musicxx.lyrics.getBySrc` / `set` /
> `sync` / `search`、`musicxx.ui.setEntryBadge`。写插件前先按这张表确认。

### 7.2 存储（插件私有数据）

| 用途 | API | 落点 |
|---|---|---|
| 私有 KV | `musicxx.storage.get/set/remove/list` | `<插件数据目录>/<插件id>/data/kv.json` |
| 用户可见的设置 | `musicxx.storage.getConfig/setConfig`（= 带 `namespace:"config"`） | 插件目录里的 `config.json` |

取值语义（很容易踩的坑）：

- `get(key, 默认值)` / `getConfig(key, 默认值)` 返回**值本身**；键不存在时返回你给的默认值
  （不给默认值就是 `null`）；
- `set` / `remove` 成功 = 已经写盘；`config.json` 是整份重写的，所以**不要**用它存高频变化的数据；
- `list()` 返回 `{keys: [...]}`；键名数组是宿主内部的存储键（私有 KV 的键带命名空间前缀）；
- 默认值由**你自己**在 `getConfig` 的第二个参数里给：框架不保存默认值，也没有「恢复默认」的入口
  （用户直接编辑 `config.json` 即可）。

### 7.3 网络（宿主代理通道）

```js
const resp = await musicxx.net.fetch({
    url: "https://api.example.com/items?page=1",
    method: "GET",                    // GET / POST / PUT / PATCH / DELETE / HEAD（缺省 GET）
    headers: { "X-Token": "..." },    // 只允许合法头名；宿主不注入任何身份信息
    body: { a: 1 },                   // 对象自动 JSON 编码；字符串原样发送
    timeoutMs: 15000,                 // 1000 ~ 60000（缺省 15000）
    responseType: "json",             // text（缺省）/ json / bytes
    maxBytes: 2097152,                // 读进内存的响应体上限（缺省 2 MiB，上限 16 MiB）
});
// resp = { ok, status, url, headers, contentType,
//          body | bodyBase64, bodyBytes, receivedBytes, contentLength, truncated }
```

- **非 2xx 也按「请求完成」返回**（`ok: true` + `status`），状态码交给你判断；
  只有传输失败 / 超时才 `ok: false`（此时 `error` 是 `请求失败：...`）；
- 响应体是**流式读取**的：读满 `maxBytes` 就断开并置 `truncated: true`
  （`receivedBytes` = 实际从网络读到的字节数，`contentLength` = 响应头声明的总长度，-1 = 未知）；
- 宿主**不限定可访问的域名**（任意 http/https 都可以）；`musicxx.net` 权限只表示
  「允许使用宿主这条通道」，不代表插件不能自己联网（动态库插件可以直接用系统 API）；
- `musicxx.net.fetch` 的动作预算会按 `timeoutMs + 5 s` 放宽，所以 HTTP 超时不会被动作超时先打断。

下载到插件自己的数据目录（`<data>/downloads/`）：

```js
const file = await musicxx.net.download({ url: "https://.../a.mp3", fileName: "a.mp3" });
// file = { ok, status, path, bytes }
```

- 只支持 `GET` / `HEAD`；`fileName` 只能是**纯文件名**（含路径分隔符或 `..` 会被拒绝），
  不写时按 URL 推导；落点固定在插件自己的数据目录内。

### 7.4 界面反馈与页面跳转

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

- 弹窗标题会自动带插件来源前缀（`『<插件id>』…`），避免插件伪装成宿主自己的提示；
- `openRoute` 允许两类目标：**任意插件**的 `ext://<插件id>/<视图id>` 页面（含本插件；跨插件页面
  由对方插件自己绘制，发起方拿不到对方的数据），以及官方页面白名单：

  | 别名键 | 页面 |
  |---|---|
  | `musicxx:settings` / `musicxx:about` / `musicxx:logs` | 设置 / 关于 / 日志 |
  | `musicxx:plugins` / `musicxx:externPlugins` | 插件管理 / 外部插件管理 |
  | `musicxx:player` / `musicxx:home` / `musicxx:userHome` | 播放页 / 音乐主页 / 用户页 |
  | `musicxx:search` / `musicxx:localSongs` / `musicxx:history` / `musicxx:lyrics` | 搜索 / 本地歌曲 / 播放记录 / 歌词 |

  地址不完整（缺插件 id 或视图 id）或不在白名单里会被拒绝，错误信息里会列出全部可用别名。

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
- 插件之间做协作时，用「对方的 `plugin.<对方id>.*` 主题」或能力调用（§9）都行：
  事件是单向通知，能力调用有返回值。

常用的可订阅主题：

| 主题 | 什么时候来 |
|---|---|
| `musicxx.state.changed` | 状态镜像某个键变化（`{key, value}`） |
| `musicxx.ui.changed` | 某个插件（可能是自己）的 UI 项变化（`{action, id, items}`） |
| `musicxx.hook.changed` | 钩子处理器注册表变化（`{id, hook, mode, count, action}`） |
| `musicxx.plugin.log` | 插件日志（`{id, level, message}`） |
| `musicxx.plugin.error` / `musicxx.plugin.warn` | 插件失败 / 告警（含处理器被暂停的提示） |
| `musicxx.hook.decision.result` | 异步裁决结果（`{callId, hook, ...}`，调试用） |

---

## 9. 能力（注册 + 跨插件调用）

```js
musicxx.capability.register("probe", (args) => ({ ok: true, args }));
musicxx.capability.register("my_name", (args) => ({ ok: true }));   // 短名即可

const info = await musicxx.capability.call("example_native", "probe", {});
```

- 能力全名是 `plugin.<插件 id>.<短名>`；应用侧（Dart）用
  `MusicxxPluginManager.call(id, "probe", args)`，宿主用同名能力名调用；
- 处理器必须**同步返回**可 JSON 序列化的结果；返回 Promise 会以
  `capability_async_not_supported` 失败（要异步就把结果写进自己的状态，或改用动作 + 事件）；
- `capability.call(插件id, 能力名, 参数?, 超时毫秒?)`：

  | 目标 | 行为 |
  |---|---|
  | JS 插件 | 在共享 JS 线程上**直接调用**，结果立即就绪 |
  | 动态库插件 | 投递到宿主线程执行，脚本**不阻塞**；结果经 `ext.onCapabilityResult` 回到 JS 线程（超时缺省 3 s、下限 1 s） |

- 跨插件调用失败时 Promise 拒绝，`err.message` 里带原因
  （`plugin_capability_not_found: xxx` / `capability_call_failed` / `跨插件调用超时: ...`）；
- 插件页面也走能力：宿主打开 `ext://<插件id>/<视图id>` 时调用同名能力（见 §11）。

---

## 10. 渲染槽位与封面数据

插件可以把**预编译的 shader bundle** 注册成宿主的一种渲染样式（当前只有「播放页背景」一个槽位），
由用户在设置里选中后生效；也可以拉取封面颜色 / 字节自己算。打包方式、`format_version` 与全部字段见
[plugin-shader-bundle.md](plugin-shader-bundle.md)，本节只说 JS 侧怎么用。

```js
// 注册一种播放页背景样式（类型简称 playing.background，或写全名）
musicxx.ui.registerEntry({
    name: "bg",
    type: "playing.background",
    order: 20,
    data: {
        title: "流光背景",
        depict: "跟随封面配色的动态背景",
        shader: { bundle: "shader/bg.shaderbundle" },   // 插件目录内的相对路径
        args: [
            { name: "uColor1", source: "icon.themeMapping.0" },
            { name: "uColor2", source: "icon.main", convert: true, value: "#8899aa" },
            { name: "uColor3", source: "theme.primary" },
            { name: "uColor4", value: "#223344" },
        ],
        speed: 1,          // 时间推进速度（宿主直接用这个值，不做二次缩放）
        maxFps: 16,
    },
});
```

```js
// 有哪些可选样式（含宿主内置项：id 形如 builtin:Auto）
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
// 封面色（分析结果 + 宿主内置背景实际用的 4 色）
const palette = await musicxx.media.palette();     // 需要强制刷新时传 { force: true }
// { ok, hasCover, analyzed, night, srcKey, version,
//   raw: { main, light, lightMuted, dark, darkMuted, dominant:[...] },   // 颜色为 "#rrggbbaa"
//   background: ["#rrggbbaa", ...] }                                     // 4 个绘制色

// 封面字节（自己要分析像素时用）：size 16..512，format = jpeg(默认) / png / rgba
const cover = await musicxx.media.cover({ size: 96, format: "png", includePath: true });
// { ok, srcKey, kind:"local"|"cache"|"content"|"asset"|"network", path?, width, height,
//   format, bytes, sha256, data(base64), fromCache }
// 失败：{ ok:false, error:"no_cover" | "cover_decode_failed" | "读取封面失败：..." }
```

- 宿主**每帧**把 `args` 声明的成员写进 uniform（`uParams` / `uEnv` 是自动成员），所以
  「跟着封面/主题配色」这类需求什么都不用做（取不到来源时用参数里的固定值）；
- 封面不参与画面绘制：宿主不上传封面贴图，也不推任何直链（网络来源只给 `kind:"network"`，
  本地来源的路径只在 `includePath: true` 时给出）；
- 槽位状态镜像见 [plugin-shader-bundle.md](plugin-shader-bundle.md) §10：`itemId` = 现在由哪个插件项在画
  （为空 = 没有插件项在画），`selectedId` = 用户选中的是谁（可能是 `builtin:*`），
  `visible:false` = 宿主当前没有渲染（播放页被遮挡 / 切后台 / 播放页不在页面上），可以据此停掉耗时的绘制工作。

---

## 11. 界面（UI 项与插件页面）

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

// 插件页面：宿主调用同名能力，脚本返回视图描述
musicxx.capability.register("card", function (args) {
    return {
        view: {
            title: "我的插件",
            subtitle: "页面内容来自能力 card",
            blocks: [
                { kind: "Text", text: "插件只返回块描述，渲染由宿主完成。", style: "cross" },
                { kind: "Divider" },
                { kind: "Button", title: "播放/暂停", style: "primary",
                  action: { kind: "action", name: "musicxx.player.toggle" } },
                { kind: "Button", title: "打开本插件设置页",
                  action: { kind: "route", route: "ext://my_plugin/settings" } },
            ],
        },
    };
});
```

- 能力返回 `{ view: {...} }`（推荐）或直接返回视图对象；返回视图时宿主会用新视图**刷新当前页面**
  （按钮改配置 + 立即刷新就是这么做的）；
- `musicxx.ui.updateEntry("card", 完整 data)` 是**整体替换**，只想改一个字段也要传完整 data；
- `musicxx.ui.entries()` 返回本插件已注册项的脚本侧镜像（不含宿主快照里的 `id` 前缀处理，调试用）。

---

## 12. 定时器与工具

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

- 定时器由宿主按实例管理，插件停用 / 卸载时自动清理；
- 精度：宿主按 ≤ 50 ms 的粒度轮询到期定时器，所以 `setTimeout(fn, 0)` 到实际执行之间可能有几十毫秒
  延迟 —— 不要用它做高精度计时。

---

## 13. 一个完整例子

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
| `musicxx.net.fetch` | 走宿主网络栈的请求 |
| `musicxx.stats.reportMetric` | 自报指标（只展示） |

只看渲染槽位的话读 `plugins/example_js_shader/plugin.js`（背景样式 + 速率设置页 + 页面内联 `Shader` 块）。

---

## 14. 调试与排障

**开发流程**

```text
① 写 plugin.yaml + plugin.js
② 把整个目录放进用户插件目录（管理页「打开插件目录」），或打包 zip 用「从压缩包安装」
③ 管理页 →「重新扫描」→ 启用（装载时执行脚本顶层）
④ 触发钩子 / 打开页面看效果；日志看「插件详情 → 日志」
⑤ 改了 plugin.js：管理页「重载」（或禁用再启用）→ 脚本会重新执行
```

**日志**：`console.*` 与 `musicxx.host.log(...)` 都进插件日志；宿主自己的日志设环境变量
`MUSICXX_EXTERN_PLUGIN_LOG_STDERR=1` 可以打印到 stderr（排查装载失败时很有用）。

**统计**：管理页「外部插件 → 调试」分页与「插件详情 → 运行统计」能给到钩子调用次数 / 耗时 /
超时 / 失败 / 是否暂停派发，以及每个 JS 实例的 `hooks` / `capabilities` / `subscriptions` /
`timers` / `pendingActions` / `jsRuns` / `errors` / `jsHeapBytes` / `execGuardHits` /
`asyncHookSettled` / `asyncHookTimeouts` / `asyncHookLateDrops`。插件里也能自己读：
`musicxx.stats.getSelf()`。

**常见坑**

| 现象 | 原因 / 处理 |
|---|---|
| 点入口提示「插件『<插件id>』没有提供『xxx』」 | 入口动作指向的能力没注册；页面视图 id 必须与能力短名一致 |
| 页面打开后提示「插件没有提供任何内容块」 | 视图没有 `blocks`，或块类型名全部拼错（未识别的块会被忽略，不报错） |
| 改了 `config.json` 页面没变 | 页面是打开时取的能力结果：改完再调一次能力返回 `{view:...}`，或重新打开页面 |
| 设置项改完重启又变回默认 | 读取时把「对象外壳」当成了值：`musicxx.storage.get/getConfig` 的应答**就是值本身** |
| 设置项被"改回去" | 读配置是异步的：读回来的旧值后到，会覆盖用户刚改的值（示例插件用「已经改过就不再覆盖」处理） |
| 动作一直失败 `action_not_registered` | 该动作当前版本没有实现（见 §7.1 的表尾） |
| 钩子不触发 | 该钩子在 [plugin-hooks.md](plugin-hooks.md) 里标「未埋点」（应用侧还没有接）；或处理器被暂停派发 |
| 处理器被调用多次 | 用**不同 `ownerTag` 注册了同一个钩子**：JS 侧只保留一个处理器，宿主侧却留下多条注册，于是每次派发都会重复调用。同一个钩子只注册一次，或注销时用同一个 `ownerTag` |
| 定时器/初始化执行两遍 | 每次启用都会重新执行脚本：注册要写成幂等的 |
| 脚本报错但插件仍在运行 | 运行期异常只记日志（顶层抛异常才会装载失败）；先看日志 |
| 应用整体卡住 | 脚本里有死循环或长时间同步计算（共享 JS 线程）。可在宿主配置里开启可选的执行上限 `jsExecGuardMs`（默认关闭）来中断超长脚本 |

---

## 15. v1 边界

| 边界 | 说明 |
|---|---|
| 异步裁决 | **已支持**：裁决处理器可以返回 Promise（100 ms 预算内结算生效，超时按「不裁决」且不计失败）；同步返回仍是最省时的写法 |
| 异步能力 | 能力处理器必须同步返回（返回 Promise 会失败）；需要异步时用动作请求 / 事件 / 定时器 |
| 资源限制 | 宿主不限制 JS 内存与执行时长；死循环会占住共享 JS 线程。可选执行上限 `jsExecGuardMs` 是「用户自选的保护」，默认关闭 |
| 定时器精度 | 宿主线程按 ≤ 50 ms 粒度轮询，不适合高精度计时 |
| 与动态库插件的差别 | JS 插件不能直接调用系统 API（没有 `require`/`fs`/`fetch`），联网走 `musicxx.net.fetch`；不能注册原生线程；能力处理器不能异步 |
