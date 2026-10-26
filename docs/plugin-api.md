# 插件接口参考（JS 与动态库插件共用）

> **术语约定**：本文档中的**主程序**指的就是 **musicxx**（Flutter/Dart 业务层）；外置插件的 C++ 框架层称为**插件框架**；插件分为**动态库插件**与**JS插件**。

这篇是**两种插件形态共用的接口手册**：状态镜像、动作、事件、变量、能力、功能点、钩子、界面入口、日志与自省，
以及命名空间、超时、容量这些两边一样的约定。

**为什么共用**：JS插件与动态库插件能用的接口是**同一批**（同一套钩子表、同一份动作注册表、同一份状态镜像、
同一套变量与功能点协议），只是写法不同 —— JS 用 `musicxx.*` 的 Promise 封装，动态库插件用 SDK 的
方法 + 异步完成通知。所以"某个接口做什么、参数是什么、返回什么、什么情况下用、有什么坑"只写在这里，
两篇形态文档只写各自的语言差异：

| 想知道 | 读这篇 |
|---|---|
| 目录结构、清单字段、生命周期、JS 线程约束、`musicxx` 全局对象 | [plugin-js-api.md](plugin-js-api.md) |
| 清单与多目标打包、SDK、CMake、框架线程、构建部署、接口表判定、插件形态（运行器） | [plugin-native-api.md](plugin-native-api.md) |
| 钩子 id / 模式 / 派发 / 合并策略 / 已接入钩子的载荷与裁决 | [plugin-hooks.md](plugin-hooks.md)（生成物） |
| 官方变量（`musicxx.*`）的键目录 | [plugin-vars.md](plugin-vars.md) |
| 界面（UI 项字段、插件页面块类型、设置页） | [plugin-ui.md](plugin-ui.md) |
| 播放页背景 / 内联块的着色器 | [plugin-shader-bundle.md](plugin-shader-bundle.md) |
| 装插件、刷新、看日志（命令行 / MCP） | [plugin-agent-cli.md](plugin-agent-cli.md) |

---

## 0. 两种形态的写法对照

同一件事，两边怎么写（左列是 JS，右列是 C++）：

| 要做的事 | JS插件 | 动态库插件 | 详见 |
|---|---|---|---|
| 注册裁决钩子 | `musicxx.hooks.register(id, {mode:"decision"}, fn)` | `hook(id, priority, fn)`（`fn` 里填 `out`） | §8 |
| 注册观察钩子 | `musicxx.hooks.register(id, {mode:"observe"}, fn)` | `observe(id, fn)` | §8 |
| 注销钩子 | `musicxx.hooks.unregister(id)` | `unregisterHook(id)` | §8 |
| 读状态镜像 | `musicxx.state.get("musicxx.state.song")` | `stateJson(MUSICXX_STATE_SONG)` | §2 |
| 让应用做一件事 | `await musicxx.call("musicxx.player.toggle")` | `requestAction("musicxx.player.toggle", "{}", &notify)` | §3 |
| 读 / 写变量 | `await musicxx.vars.get(key)` / `musicxx.vars.set(key, v)` | `varsGet` / `varsSet` / `varsPeek` | §5 |
| 订阅事件 | `musicxx.events.subscribe(topic, fn)` | `subscribeTopic(topic, fn)` | §4 |
| 发布事件 | `musicxx.events.publish(topic, data)` | `iface.events->publish(host, topic, json)` | §4 |
| 暴露自己的能力 | `musicxx.capability.register("probe", fn)` | `capability(*this, "plugin.<id>.probe", fn)` | §6 |
| 调别的插件的能力 | `await musicxx.capability.call(id, "probe", args)` | `co_await capabilityCall(cap, method, args)` | §6 |
| 声明 / 实现 / 调用功能点 | `musicxx.feature.define/provide/callPoint` | `featurePointDefine/Provide/Call` | §7 |
| 声明界面项 | `musicxx.ui.registerEntry({name, type, data})` | `uiRegister("card", MUSICXX_PLUGIN_UI_TYPE_HOME_ENTRY, json)` | §9 |
| 写日志 | `musicxx.host.log(2, "文字")` / `console.log(...)` | `log.info("文字")` | §10 |
| 读框架信息 | `musicxx.host.info()` | `hostInfoJson()` | §2.5 |
| 读自己的配置路径 | `musicxx.host.configPath()` | `configPath()` | §3.2.5 |

**两边的差异只在写法与线程模型上**：

| 差异 | JS插件 | 动态库插件 |
|---|---|---|
| 异步写法 | 返回 Promise（`await`） | 结果经 `PluginxxOperatorNotify::done` 回调，或协程 `pluginxx::Task<T>` |
| 谁能长时间占线程 | 都不行；JS 共用一条 JS 线程，原生共用一条框架线程 | 同左，但原生可以把重活交给 `pluginxx::offload` 线程池 |
| 能不能直接用系统 API | 不能（没有 `require`/`fs`/`fetch`），联网走 `musicxx.net.*` | 可以（自己起线程、自己联网） |
| 定时器 | 内置 `musicxx.timer` / 全局 `setTimeout` | 自己起线程 / 定时器，在 `stop` 里回收；或 `pluginxx::sleep` |
| 能力处理器可以异步 | 可以（返回 Promise） | 可以（返回 `pluginxx::Task<T>`） |
| 功能点处理器 | **必须同步返回** | **必须同步返回** |
| 读自己的统计 | `musicxx.stats.getSelf()` | 没有便捷接口（看管理页「插件详情 → 运行统计」） |

---

## 1. 先选通道：要读、要改、要做的事该走哪条

七条通道的分工（选错了会很难写，硬凑更麻烦）：

| 你的需要 | 用哪条 | 为什么 |
|---|---|---|
| 跟着应用的事件做事，或者想拦一下（跳过本曲、换源、停止播放） | **钩子** §8 | 唯一能"被应用调用"的通道；能拿到事件载荷 |
| 高频读实时数据（播放状态、当前歌曲、频谱槽位状态） | **状态镜像** §2 | 框架同步读，不用往返；一次推送多个读者 |
| 一次性命令（播放控制 / 列歌单 / 发通知 / 开页面 / 写插件自己的数据） | **动作** §3 | 有返回值、有错误码，由主程序执行 |
| 一件"已经发生了"的事（没有当前值） | **事件** §4 | 单向广播，最轻 |
| 一份有当前值、要能改、改了要让别人知道的小状态 | **变量** §5 | 有属主、可读可写可订阅；框架帮你去重与合并 |
| 把自己的功能给别的插件用 | **能力** §6 | 有返回值、参数与结果由你自己定 |
| 让"这个功能谁都能实现"（应用或第三方按 id 来调） | **功能点** §7 | 可以有多个实现，按优先级问 |

> 记法：**镜像 = 读现状**，**动作 = 让应用做**，**事件 = 通知发生了**，
> **变量 = 有值可改可订阅**，**钩子 = 应用来问/告诉我**，**能力 = 我提供给别人调**，
> **功能点 = 谁都能实现的一个 id**。

---

## 2. 状态镜像（同步读应用现状）

插件框架把应用的关键状态做成 JSON 快照，插件**同步**读：不用往返、不占线程、不怕应用忙。

```js
// JS：读一次（没有推送过的键返回 null）
const song = musicxx.state.get("musicxx.state.song");
const playing = (musicxx.state.get("musicxx.state.env") || {}).isPlaying === true;
```

```cpp
// C++：读一次（没有推送过的键返回空串）
const std::string songJson = stateJson(MUSICXX_STATE_SONG);
const utilxx_base::Json env = utilxx_base::Json::parse(stateJson(MUSICXX_STATE_ENV));
```

### 2.1 有哪些键

| 键 | 内容 | 什么时候刷新 |
|---|---|---|
| `musicxx.state.app` | `{version, versionStr, platform, lang, branch, installId, isNight, firstRun, foreground, lifecycleState}` | 启动时一次；主题/语言/前后台变化时重推 |
| `musicxx.state.player` | `{state, position, duration, volume, speed, pitch, quality, srcKey, mediaType, cacheLength}` | 启动、切歌、播放状态变化 |
| `musicxx.state.song` | 当前歌曲只读视图（字段见下） | 切歌 |
| `musicxx.state.playlist` | `{pid, name, count, songNum, type, index, loopMode, autoPlayMode}` | 歌单 / 播放位置变化 |
| `musicxx.state.env` | `{isPlaying, page, lanServerOn, userLogged}` | 播放状态、路由、本机服务、登录态变化 |
| `musicxx.state.renderSlots` | 每个渲染槽位的运行状态（见 §9.2） | 生效项 / 可见性 / 昼夜 / 尺寸变化时按需推 |
| `musicxx.state.spectrum` | 当前音频频谱（字段见 §3.2.8） | 播放中约 10 Hz（节流 100 ms）；停止、切歌、拿到/读不到数据时各推一次 |
| `musicxx.state.lyric` / `musicxx.state.library` | 预留 | **当前版本没有推送**（读到的永远是空） |

只出现一次 `musicxx.state.app` → `musicxx.state.env` 的字段含义：

- `musicxx.state.app.version` 是整型版本号（与 `versionStr` 是同一件事的两种写法）；
  `installId` 是安装级随机 id（可当匿名标识用）；`branch` 是渠道名；`isNight` 见下方"坑"。
- `musicxx.state.app.foreground` 是"现在是否前台"（布尔），`lifecycleState` 是原始生命周期状态名
  （`resumed` / `inactive` / `hidden` / `paused` / `detached`）。进后台 / 回前台的**事件**用钩子
  `musicxx.app.background` / `musicxx.app.foreground`（后者带 `backgroundMs`）；要订阅变化用变量
  `musicxx.app.lifecycle`。
- `musicxx.state.player.quality` 是音质枚举名（`Undefined` / `Lower` / `Standard` / `HQ` / `SQ` / `Hifi` / `Upper`），
  `mediaType` 是媒体类型枚举名；`cacheLength` 是已缓存时长（毫秒）；`pitch` 是当前音调（1 = 原调）。
  这几个值都是“变化时才刷新”的快照：要“变的时候拦一下”用钩子 `musicxx.player.volume` /
  `musicxx.player.speed` / `musicxx.player.pitch`，要当场读当前进度用变量 `musicxx.player.position`。
- `musicxx.state.playlist.loopMode` 是循环模式枚举名（`Order` / `Only` / `RealRand` / `SimulateRand` / `SimulateRandReflush`），
  `autoPlayMode` 是 `auto` 或 `off`；`index` 是当前曲目在歌单里的下标。
- `musicxx.state.env.page` 是当前路由名（字符串，可用于"现在在哪个页面"的判断）。

### 2.2 歌曲视图长什么样

`musicxx.state.song` 与钩子载荷里的 `song` 是**同一份结构**（没有歌曲时是空对象）：

```jsonc
{
  "sid": 123,                    // 会话内歌曲 id（切歌/重播会变，别当持久标识）
  "name": "歌名", "artist": "歌手", "album": "专辑",
  "durationMs": 253000, "year": 2024, "genre": "流行",
  "srcKey": "<音源身份的完整 md5，没有音源时是 null>",
  "audio": [ { "type": "Local|UrlLink|Bili|...", "empty": false, "srcKey": "<md5>",
               "quality": "Standard", "durationMs": 253000, "hashMd5": "..", "size": 8123456 } ],
  "video": [ { "type": "...", "empty": false, "durationMs": 0, "width": 0, "height": 0, "fps": 0, "size": 0 } ]
}
```

**载荷与镜像里永远没有音频直链和 token**：`src` 正文不给，只给来源类型与稳定 key（`srcKey` 是音源身份，
同一首歌在不同设备上也一样，适合做缓存键）。要地址就自己走 `musicxx.net.*` 动作或让应用做事。

### 2.3 订阅状态变化

每个键更新都会推事件 `musicxx.state.changed`（载荷 `{key, value}`），两边订阅方式一样（见 §4）：

```js
musicxx.events.subscribe("musicxx.state.changed", (payload) => {
    if (payload.key === "musicxx.state.song") { refreshMyPanel(payload.value); }
});
```

```cpp
subscribeTopic("musicxx.state.changed", [](std::string_view json) {
    // json = {"key":"musicxx.state.song","value":{...}}
});
```

### 2.4 坑

- **`musicxx.state.app.isNight` 是启动时的快照**：运行期切了主题它不会跟着变。要"当前值"就当场问一次：
  动作 `musicxx.media.palette` 或 `musicxx.render.current` 的 `night`；
  `musicxx.state.renderSlots` 里条目的 `night` 也只在槽位状态变化时刷新，同样不能当当前值用。
- **播放进度要自己拿**：`musicxx.state.player.position` 只在启动、切歌、播放状态变化时刷新（**不是实时值**）。
  要按秒推进度：用钩子 `musicxx.player.position`（每秒一条，带 `sid` / `positionMs` / `durationMs`），
  或读变量 `musicxx.player.position`（当场读当前值）。不要以为读镜像就能拿到实时进度。
- **要"是否在播放"读 `musicxx.state.env.isPlaying`**（布尔），不要靠 `player.state` 的字符串判断
  （它是"播放/暂停/停止事件"与枚举名的混合值，见 §3.2.1 的说明）。
- **键名是固定的官方键**，写错就是读不到（`get` 返回 `null` / `stateJson` 返回空串），不会报错。
- 镜像里**不放**临时直链、token、插件自己的数据。插件自己的状态放 §5 的变量或私有存储里。

### 2.5 框架与应用信息（`host.info`）

```js
const info = musicxx.host.info();
// { appVersion, platform, language, dataDir, userPluginDir, builtinPluginDir,
//   apiVersion, hostVersion, pluginId, ui? }
```

```cpp
const std::string infoJson = hostInfoJson();   // 同一份 JSON
```

| 字段 | 含义 |
|---|---|
| `appVersion` | musicxx 版本（字符串） |
| `platform` | `windows` / `linux` / `macos` / `android` / `ios` / `ohos` |
| `language` | 应用当前语言（如 `zh-cn`） |
| `dataDir` | **插件数据根目录**（插件私有 KV 在 `<dataDir>/<插件id>/data/kv.json`） |
| `userPluginDir` / `builtinPluginDir` | 用户插件目录 / 随包插件目录 |
| `apiVersion` | 插件 API 版本（当前 3）；插件可以据此降级 |
| `hostVersion` | **插件框架自己的版本**（如 `2026.1011.1`，见 [version-history.md](version-history.md)） |
| `pluginId` | 本插件 id（**只有插件读时才带这个字段**；主程序读同一份信息时没有它） |
| `ui` | 客户端界面能力段（只有客户端配置过时才出现，见 [plugin-ui.md](plugin-ui.md)） |

运行环境（`arch` / `logDir` / `pluginDirs` 等）在 SDK 侧还有一份：动态库插件用 `config()`（JSON），
JS 插件用动作 `musicxx.host.getPath` 拿插件目录、数据目录与日志目录。

---

## 3. 动作（让应用做一件事）

### 3.1 通用约定

```js
const result = await musicxx.call("musicxx.<域>.<动作>", { 参数 }, 超时毫秒?);
// 也有分类便捷封装（都等价于 musicxx.call）：
await musicxx.player.toggle();
await musicxx.storage.set("k", 1);
```

```cpp
// 动态库插件：动作只有异步形态，结果经完成通知回来（恰好一次）
static void onActionDone(void* ud, int32_t status, const PluginxxStringView* json) {
    // status == 0 → json 是结果；否则是错误（json 里通常有 {"ok":false,"error":"..."}）
}
PluginxxOperatorNotify notify{&onActionDone, this};
int64_t requestId = 0;
requestAction("musicxx.player.toggle", "{}", &notify, 5000, &requestId);
```

规则（两边一样）：

- **超时**：JS 的 `musicxx.call(name, args, timeoutMs)` 缺省 **5000 ms**，`timeoutMs <= 0` 时按缺省值处理（不会"一直等"）；
  动态库的 `requestAction(..., timeoutMs)` 缺省 **5000 ms**，**传 0 = 不设超时**（框架不建定时器，一直等主程序回复）；
  到点后框架终结这次请求并推 `musicxx.action.cancel` 让主程序收尾。
  个别动作需要更长时间（如 `musicxx.feature.call` 要解码整曲），调用方自己加大。
- **返回**：成功是 `{ok:true, ...}` 或动作自己的结果；失败是 `{ok:false, error:"说明"}`；
  动作没注册会立刻失败（`action_not_registered: <动作名>`），不会挂到超时。
- **不做权限校验**：清单里的 `permissions` 只做展示，不会因为没声明就拒绝调用（见 [plugin-native-api.md](plugin-native-api.md) §8）。
- **动作名不校验**：插件框架受理任意名字的动作，由主程序按"注册了没有"决定理不理它。
  官方动作是 `musicxx.<域>.<动作>`；插件自己的动作建议写成 `plugin.<包名>.*` 以免撞名。
- **Result 里可能出现 `ok:false` 但调用成功**两种情况要分清：
  - 调用失败（动作没注册 / 处理器抛异常）→ JS 的 Promise **拒绝**（`err.message` 带原因）；C++ 的 `status != 0`；
  - 动作执行了但条件不满足（参数错、找不到歌单）→ Promise 成功、结果里 `{ok:false, error:"..."}`。

### 3.2 全部已实现的动作

下面每一节都是"当前版本注册过、调用能得到结果"的动作。**没有列在这里的 `musicxx.*` 名字都还没实现**
（调用会得到 `action_not_registered`，见 §3.3）。

#### 3.2.1 播放控制（`musicxx.player.*`）

| 动作 | 参数 | 返回 | 说明 |
|---|---|---|---|
| `musicxx.player.play` | 无 | `{ok:true}` | 开始播放 |
| `musicxx.player.pause` | 无 | `{ok:true}` | 暂停 |
| `musicxx.player.toggle` | 无 | `{ok:true}` | 播放/暂停切换 |
| `musicxx.player.stop` | 无 | `{ok:true}` | 停止 |
| `musicxx.player.next` | 无 | `{ok:true}` | 下一曲（走应用的切歌流程） |
| `musicxx.player.prev` | 无 | `{ok:true}` | 上一曲 |
| `musicxx.player.playSrc` | `{src}`（单个音源）或 `{sources}`（数组）；`{type}`：`auto`/`local`/`link`（缺省 `auto`）；`{filterExt}`：只接受扫描配置里允许的媒体后缀；`{name}` 显示名 | `{ok:true, count, single, name, sid, artist, sources, skipped?}` | 按来源播放：本地路径 / `content://` / 文件链接 / 拟声音源字符串。**和 applink（`musicxx://play/`）、启动参数、命令行、MCP 是同一个入口、同一套参数名**；一个来源追加到当前播放列表并播，多个来源一次列成临时歌单。没有可播的来源 → `{ok:false,error:"没有可播放的音源"}` |
| `musicxx.player.seek` | `{positionMs}` 或 `{position}`（毫秒），或 `{percent}`（0~1） | `{ok:true}` | 都缺 → `{ok:false,error:"缺少参数 positionMs"}` |
| `musicxx.player.setVolume` | `{volume}`（0~1；> 1 视为百分比，自动除以 100，最后钳到 0~1） | `{ok:true}` | 缺参数 → `{ok:false,...}` |
| `musicxx.player.setSpeed` | `{speed}`（倍速） | `{ok:true}` | |
| `musicxx.player.setPitch` | `{pitch}`（音调；1 = 原调，应用夹到 0.25~2） | `{ok:true}` | 缺参数 → `{ok:false,...}`；不改变播放速度 |
| `musicxx.player.setQuality` | `{quality}`：`Undefined`/`Lower`/`Standard`/`HQ`/`SQ`/`Hifi`/`Upper`（忽略大小写） | `{ok:true}` | 未知取值 → `{ok:false,error:"未知音质：x"}`；等价于用户在设置里改"音质" |
| `musicxx.player.setLoopMode` | `{mode}`：`Order`/`Only`/`RealRand`/`SimulateRand`/`SimulateRandReflush` | `{ok:true}` | 未知取值 → `{ok:false,error:"未知播放模式：x"}` |

```js
await musicxx.player.setVolume(0.6);       // 便捷封装：setVolume(60) 会当作 60%
await musicxx.player.seek(64000);          // 数字 = 毫秒
await musicxx.call("musicxx.player.seek", { percent: 0.5 });
await musicxx.player.setSpeed(1.25);
await musicxx.player.setPitch(1.1);        // 音调偏高：速度不变
const mode = (musicxx.state.get("musicxx.state.playlist") || {}).loopMode;   // 读当前值
```

```cpp
requestAction("musicxx.player.setLoopMode", R"({"mode":"Only"})", &notify, 5000);
requestAction("musicxx.player.seek", R"({"positionMs":64000})", &notify, 5000);
```

- 这些动作都是"替用户按一下按钮"：会走应用既有的流程（例如 `stop` 会做它平时的收尾），不要绕过它们直接改状态。
- **这四个值都有“要变之前拦一下”的钩子**：`musicxx.player.seek` / `musicxx.player.volume` /
  `musicxx.player.speed` / `musicxx.player.pitch`（见 §8 与 [plugin-hooks.md](plugin-hooks.md)）：
  插件用动作改值时也会先问钩子，所以钩子不能反过来再调这些动作（会再派发一次）。
- 没有 `setMediaType`（见 §3.3）。

#### 3.2.2 歌单与歌曲（`musicxx.library.*`）

**`musicxx.library.querySonglists`** —— 列出歌单

- 参数：无。
- 返回：`{mine:[歌单], favorite:[歌单], local:[歌单], currentPid, currentName}`；
  歌单项是 `{pid, name, count, songNum, listen, type}`（`count` = 已加载的曲目数，`songNum` = 歌单声明的曲目数，
  `type` 是歌单类型枚举名）。
- `favorite` 是"我喜欢"（一直在最前），`mine` 是账号歌单，`local` 是本地歌单。

**`musicxx.library.querySongs`** —— 列出一个歌单里的歌

- 参数：`{pid?, type?, limit?=200, offset?=0}`。
  - 不给 `pid`（或给的是当前播放列表的 `pid`）→ 读当前播放列表；
  - 给 `type`（`local` = 本地歌单，其它值按在线歌单）可以在歌单还没进当前账号的歌单表时按 `pid` 直接查；
- 返回：`{pid, total, songs:[{sid, name, artist, album, durationMs}]}`，读当前播放列表时还会带 `available`。
- 歌单加载失败 → `{ok:false, error:"歌单无法加载：<pid>"}`。

**`musicxx.library.playSong`** —— 播当前歌单里的某一首

- 参数：`{sid}`；**必须在这一首在当前播放列表里**，否则 `{ok:false, error:"当前歌单中未找到歌曲：<sid>"}`。
- 返回：`{ok:true}`。

**`musicxx.library.playSonglist`** —— 播放一个歌单

- 参数：`{pid, index?}`（`index` 越界或缺失按 0 处理）；歌单为空 → `{ok:false, error:"歌单为空或无法加载：<pid>"}`。
- 返回：`{ok:true}`。

```js
const lists = await musicxx.library.querySonglists();
await musicxx.library.playSonglist({ pid: lists.local[0].pid, index: 3 });

const page = await musicxx.library.querySongs({ pid: lists.local[0].pid, limit: 50, offset: 100 });
```

```cpp
// 想先拿歌单再决定播哪一个：在完成通知里读结果，再发第二个动作
requestAction("musicxx.library.querySonglists", "{}", &notify, 5000);
```

**这首歌的完整音源列表在状态镜像 / 钩子载荷里**（`musicxx.state.song.audio[]`），
`querySongs` 只给列表用的简报字段（省流量）。

#### 3.2.3 歌词（`musicxx.lyrics.*`）

**`musicxx.lyrics.getCurrent`** —— 当前歌曲的歌词行

- 参数：`{limit?=200}`。
- 返回：`{sid, lrcid, name, artist, lineCount, index, lines:[{timeMs, text}]}`；
  `index` = 当前播放到的行号，`lineCount` 是总行数（`lines` 被 `limit` 截断时用它算进度），
  `timeMs` 为 `null` 表示这一行没有时间戳（纯文本行）。
- 没有歌词时 `lines` 是空数组，`lrcid` 为 `-1`。

```js
const lrc = await musicxx.lyrics.getCurrent();
const line = lrc.lines[Math.min(lrc.index, lrc.lines.length - 1)];
```

#### 3.2.4 提示、弹窗与页面跳转（`musicxx.ui.*`）

| 动作 | 参数 | 返回 | 说明 |
|---|---|---|---|
| `musicxx.ui.notify` | `{text}`（也接受 `message`）；`{kind:"error"}` 走错误样式 | `{ok:true}` | 站内提示，会记一条插件日志 |
| `musicxx.ui.toast` | `{text}` | `{ok:true}` | 轻提示（更轻，不记日志） |
| `musicxx.ui.dialog` | `{title?, content?, blocks?, textConfirm?, textCancel?}` | `{ok:true, confirmed:bool}` | 需要用户确认的操作。`content` 是纯文本，`blocks` 是富内容（页面块数组；两个都给时文本在上）。标题不加插件前缀（面向普通用户）；取消 / 关闭一律回 `confirmed:false` |
| `musicxx.ui.overlay.show` | `{preset?, id?, title?, depict?, blocks?, options?, value?, textConfirm?, textCancel?, size?, dismissible?}` | `{ok, reason, value?}` / `{ok, selected}` / `{ok, cancelled:true}` | 打开一层浮层（sheet），**等用户关掉它**。参数与返回见 §3.2.4.1 |
| `musicxx.ui.overlay.close` | `{id?}` | `{ok:true, closed:bool}` | 收起这个插件当前那一层浮层（给了 `id` 就只关名字对得上的那一层；没开着的浮层时 `closed:false`） |
| `musicxx.ui.openRoute` | `{route, arguments?}` | `{ok:true}` 或 `{ok:false,error}` | 打开插件页面或官方页面，规则见下 |

`musicxx.ui.openRoute` 允许两类目标（别的地址会被拒绝，错误信息里会列出全部可用别名）：

1. **任意插件的声明式页面**：`ext://<插件id>/<视图id>`（含本插件；跨插件页面由对方插件自己绘制，
   发起方拿不到对方的数据）；
2. **官方页面白名单**（只认这些别名键，插件不接触应用内部路由名）：

| 别名键 | 页面 | 别名键 | 页面 |
|---|---|---|---|
| `musicxx:settings` | 设置 | `musicxx:player` | 播放页 |
| `musicxx:about` | 关于 | `musicxx:home` | 音乐主页 |
| `musicxx:logs` | 日志 | `musicxx:userHome` | 用户页 |
| `musicxx:plugins` | 插件管理 | `musicxx:search` | 搜索 |
| `musicxx:externPlugins` | 外部插件管理 | `musicxx:localSongs` | 本地歌曲 |
| `musicxx:history` | 播放记录 | `musicxx:lyrics` | 歌词 |

```js
await musicxx.ui.notify({ text: "已刷新" });
await musicxx.ui.toast("轻提示");                    // 也可以直接传字符串
const answer = await musicxx.ui.dialog({ content: "删除缓存？", textConfirm: "删除", textCancel: "算了" });
if (answer.confirmed) { /* ... */ }
await musicxx.ui.openRoute("ext://my_plugin/settings");
await musicxx.ui.openRoute("musicxx:localSongs");
```

```cpp
// C++ 侧只有声明式提示（fire-and-forget）；弹窗与开页面走同名动作
uiNotify(R"({"text":"已刷新"})");
requestAction("musicxx.ui.dialog", R"({"content":"删除缓存？"})", &notify, 0);   // 0 = 一直等用户点
```

> 弹窗与浮层都是**用户操作**：等它们的时候把超时设长一点（动态库侧传 0 表示不设超时；
> JS 侧的 `musicxx.ui.dialog` / `overlay` / `pick` 缺省已经给了 10 分钟），
> 否则用户还没点就已经超时了。

#### 3.2.4.1 浮层（`musicxx.ui.overlay.show`）

浮层是"代码打开、等结果"的动作：插件打开一层 sheet，用户在浮层里操作，关掉后动作返回
**怎么关的**。页面里想让某一行打开浮层，就让那一行带一个 `command` 动作指向它（同一个函数、同一份参数）。

| 参数 | 类型 | 说明 |
|---|---|---|
| `preset` | string | `custom`（缺省，内容 = `blocks`）/ `picker`（选择列表，见下） |
| `id` | string | 可选：给这层浮层一个名字，`overlay.close` 可以点名关它 |
| `title` / `depict` | string | 标题 / 副标题 |
| `blocks` | array | `preset: custom` 的内容（页面块数组，可以放按钮 / 控件 / 列表） |
| `options` | array | `preset: picker` 的候选项：`[{value, title, describe?, selected?}]`（`describe` 可多行） |
| `value` | any | `preset: picker` 的当前值（对应候选项打上选中标记） |
| `textConfirm` / `textCancel` | string | 底部按钮文案（不给就没有那个按钮） |
| `size` | string | 尺寸预设：`autoHeightAutoWidth`（缺省）/ `autoHeightFullWidth` / `full` / `fullHeightAutoWidth` / `columnAutoHeightRowFullHeightAutoWidth` |
| `dismissible` | bool | 缺省 `true`：点遮罩 / 返回键就能关；写 `false` 时必须点按钮（或插件自己 `overlay.close`） |

返回：

| 情况 | 返回 |
|---|---|
| 点了 `textConfirm` 的按钮 | `{ok:true, reason:"confirmed", value?}` |
| 点了 `textCancel` 的按钮 | `{ok:true, reason:"cancelled"}` |
| 点关闭 / 点遮罩 / 返回键 / 被程序关掉（新浮层顶掉、插件卸载、插件页面退出） | `{ok:true, reason:"closed"}` |
| `preset: picker` 选中一项 | `{ok:true, selected:<该项的 value>}` |
| `preset: picker` 取消 / 关掉 | `{ok:true, cancelled:true}` |
| 参数不全（`custom` 没给 `blocks`、`picker` 没给 `options`） | `{ok:false, error}` |

规则：

- **每个插件同时最多一层**：再打开会先关掉旧的（旧的回 `reason:"closed"`，不会一直等）；
- 浮层里的按钮动作**不会自动关闭浮层**：插件可以返回 `{"view": …}` 让浮层换一份内容（做"浮层里翻页 / 刷新"），
  要关就自己调 `musicxx.ui.overlay.close`；`textConfirm` 那个按钮一定关闭；
- `preset: picker` 只做**单选**：点一项就关闭并返回该项的值（需要多选就在自己的 `blocks` 里放 `Control(checkbox)`）；
- `show` 的返回**一定有一个终局**（插件卸载、插件页面退出也会结算），不会悬挂。

```js
// 打开一层浮层等结果
const r = await musicxx.ui.overlay({
  title: "运行详情",
  blocks: [kit.kv({ pairs: [{ k: "模型", v: "gpt-5" }] })],
  textConfirm: "知道了",
});
if (r.reason === "confirmed") { /* ... */ }

// 选择列表：标题 + 多行描述
const picked = await musicxx.ui.pick({
  title: "音质",
  value: current,
  options: [
    { value: "high", title: "高品质", describe: "文件较大，音质最好" },
    { value: "low", title: "流畅", describe: "省流量" },
  ],
});
if (picked && !picked.cancelled) current = picked.selected;

// 自己收起（例如浮层里的动作已经做完）
await musicxx.ui.overlayClose();
```

```cpp
// C++：requestAction 传 0 = 一直等用户操作
requestAction("musicxx.ui.overlay.show", R"({"title":"运行详情","blocks":[],"textConfirm":"知道了"})", &callback, 0);
requestAction("musicxx.ui.overlay.close", R"({})", &callback, 0);
```

#### 3.2.5 插件私有存储与配置（`musicxx.storage.*`）

两块地方、两套语义：

| 用途 | 动作 / 参数 | 存放位置 |
|---|---|---|
| 私有 KV（不需要给用户看的数据） | `musicxx.storage.get/set/delete/list`，`namespace` 省略或写别的值 | `<dataDir>/<插件id>/data/kv.json` |
| 用户可见的设置 | 同上，`namespace:"config"` | **插件目录里的 `config.json`**（用户可以直接编辑） |

| 动作 | 参数 | 返回 |
|---|---|---|
| `musicxx.storage.get` | `{key, namespace?}` | **值本身**（键不存在时是空应答；JS 封装回退到默认值） |
| `musicxx.storage.set` | `{key, value, namespace?}` | `{ok:true}` 或 `{ok:false, error}` |
| `musicxx.storage.delete` | `{key, namespace?}` | 同上（键不存在也算成功） |
| `musicxx.storage.list` | `{namespace?}` | `{keys:[...]}`（每次最多 500 个，升序；键名是内部存储键，私有 KV 的键带 `命名空间:` 前缀，`config` 命名空间给的是配置文件里的键名） |

```js
// JS 便捷封装：第二个参数是默认值
const greet = await musicxx.storage.get("greeting", "你好");
await musicxx.storage.set("greeting", "早上好");
const theme = await musicxx.storage.getConfig("theme", "dark");   // = namespace:"config"
await musicxx.storage.setConfig("theme", "light");
```

```cpp
// C++：读写配置文件可以直接用 configPath()（拿路径自己读写），
// 走私有 KV 就用动作（注意应答就是值本身，不是对象外壳）
requestAction("musicxx.storage.get", R"({"key":"greeting"})", &notify, 5000);
requestAction("musicxx.storage.set", R"({"key":"greeting","value":"早上好"})", &notify, 5000);
```

必须记住的几条：

- **`get` / `getConfig` 的应答就是值本身**：不要按"外面还有一层对象"去读，否则设置项读不出来
  （表现是"改完重启又变回默认"）。
- `set` 返回成功 = **已经写盘**；`config.json` 每次是整份重写，不要用它存高频变化的数据。
- 值可以是任意 JSON（对象、数组、数字、布尔、null）。**不设容量配额**，但每次写入都整文件重写，
  所以别把它当数据库用。
- 默认值由**你自己**在读取时给：框架不保存默认值，也没有"恢复默认"入口（用户直接编辑 `config.json` 即可）。
- 读配置是异步的：**读回来的旧值可能后到并覆盖用户刚改的值**，自己加"已经改过就不再覆盖"的保护。
- 私有 KV 与 `config.json` 都是**按插件隔离**的，别的插件看不到。

#### 3.2.6 网络（`musicxx.net.*`）

**这些能力全部由插件框架自己实现**（原生代码，跑在插件框架自己的网络线程上），不经过主程序：
`musicxx.net` 权限只表示"允许使用这条通道"，框架**不限制可访问的域名**。动态库插件也可以不用它、
自己用系统 API 联网；JS 插件没有原生网络能力，用这套最方便。

OpenSSL 以**静态库**链进插件框架，所以客户端能直接连 https / wss，应用包里不需要带
`libssl` / `libcrypto`。**服务端不做 TLS**（插件自己起服务只监听 http / ws）。
构建时关掉 TLS（`--no-tls` / `-NoTls`）的版本里，`https` / `wss` 会得到 `tls_unsupported`。

| 接口（JS） | 内部 op | 参数 | 返回 |
|---|---|---|---|
| `musicxx.net.fetch` / `http.fetch` | `http.fetch` | `{url, method?, headers?, body?, timeoutMs?, responseType?, maxBytes?, tlsVerify?, caFile?, caPem?, serverName?}` | 见下 |
| `musicxx.net.download` / `http.download` | `http.download` | `{url, fileName?, headers?, timeoutMs?, tlsVerify?, caFile?, caPem?, serverName?}`（只支持 `GET`） | `{ok, status, path, bytes}` |

`fetch` 参数与限制：

| 参数 | 取值 | 缺省 |
|---|---|---|
| `url` | `http` / `https`，必须有主机名 | 必填 |
| `method` | `GET`/`POST`/`PUT`/`PATCH`/`DELETE`/`HEAD`（大小写不敏感） | `GET` |
| `headers` | 对象；键必须是合法 HTTP 头名（可见 ASCII、不含 `:`），值会转成字符串 | 空 |
| `body` | 对象/数组 → 自动 JSON 编码；字符串原样发送 | 空 |
| `timeoutMs` | 1000 ~ 120000 | 15000 |
| `responseType` | `text` / `json` / `bytes`（`json` 也按文本给，自己 `JSON.parse`；`bytes` 给 `bodyBase64`） | `text` |
| `maxBytes` | 1024 ~ 16777216（16 MiB）；读满就断开并标 `truncated` | 2 MiB |
| `tlsVerify` | 是否校验证书链与主机名；**只在自签名/调试时关** | `true` |
| `caFile` / `caPem` | 额外信任的 CA（文件路径 / PEM 内容）；是**追加**信任，不替换系统根证书 | 空 |
| `serverName` | 覆盖 SNI 与主机名校验用的名字（IP 访问、把域名解析到本机调试时用） | 空（用 url 里的主机名） |

`fetch` 返回：

```jsonc
{ "ok": true, "status": 200, "url": "…", "headers": {…}, "contentType": "…",
  "body": "…文本…",           // responseType = text / json 时
  "bodyBase64": "…",          // responseType = bytes 时
  "bodyBytes": 1234,          // 写进结果的字节数
  "receivedBytes": 1234,      // 实际从网络读到的字节数
  "contentLength": 1234,      // 响应头声明的长度（-1 = 未知）
  "truncated": false }
```

- **非 2xx 也按"请求完成"返回**（`ok:true` + `status`），状态码交给插件判断；
  只有传输失败 / TLS 失败 / 超时才 `ok:false`。
- 响应体是**流式读取**的：读满 `maxBytes` 就断开连接并置 `truncated:true`，不会把整份响应吃进内存。
- **信任库**：系统根证书（Windows 读系统证书库，Linux/macOS/Android 读常见 bundle 与证书目录）
  始终参与校验；`caFile` / `caPem` 是在此基础上追加。自签名的自建服务要么把它自己的 CA
  通过 `caPem` 给进来，要么 `tlsVerify:false`（生产环境不要这么干）。

`download` 落盘位置固定在插件自己的数据目录里：`<dataDir>/<插件id>/data/downloads/<文件名>`；
`fileName` 只能是**纯文件名**（带路径分隔符或 `..` 的会被改写/拒绝），不写就按 URL 推导。
正文**边读边写**，不进内存，所以大文件也不会把进程吃满。

```js
const resp = await musicxx.net.fetch({ url: "https://example.com/a.json", responseType: "json" });
if (resp.ok && resp.status === 200) { const data = JSON.parse(resp.body); }

const file = await musicxx.net.download({ url: "https://example.com/a.mp3" });
```

```cpp
// 动态库插件: 直接走网络表 (与 JS 是同一个实现; netFetch 是方便糖, 也可以用 httpRequest)
netCall(net_op::httpFetch, R"({"url":"https://example.com/a.json","timeoutMs":20000})");
// 也可以完全不用这套, 自己用系统的网络库
```

老的动作 `musicxx.net.fetch` / `musicxx.net.download` 仍然注册在主程序里（给历史上直接调动作的
插件用，走主程序网络栈、会继承代理设置），但 **JS 封装 `musicxx.net.fetch` / `download` 已经改用
插件框架自己实现**，两边参数与返回形状一致。新插件请用本节这套。

##### 静态内容（`http.serve` 的 `static` / `http.route`）：把文件与内存内容变成 http 地址

JS 运行时里没有 socket；要把"播放器打不开的东西"（本地文件、插件自己取到的远端数据、
HLS 播放列表与分片、转码产物）交给播放器，就在自己的 HTTP 服务上挂几条**静态路由**：
命中后由插件框架直接回响应，**不发 `http.request` 事件** —— 不用写处理器，也不用等回答。

| 接口 | 参数 | 返回 |
|---|---|---|
| `musicxx.net.http.serve(options)` | `{id?, host?, port?, path?, staticOnly?, static?: [route…], requestTimeoutMs?, maxBodyBytes?, maxConnections?}` | `{ok, id, host, port, url, routes?}` |
| `musicxx.net.http.route(options)` | `{serverId, file \| bytes \| bytesBase64, path?, contentType?, fileName?}` | `{ok, serverId, path, url, size, mtimeMs, contentType, fileName, reused, routes, routeCount}` / `{ok:false, error, message}` |
| `musicxx.net.http.unroute(options)` | `{serverId, path}` | `{ok, serverId, path, removed, routes}` |
| `musicxx.net.http.info()` | —— | 服务快照；每个服务带 `routes`（`path` / `url` / `size` / `contentType` / `fileName` / `file` / `mtimeMs`） |

一条静态路由（`static` 数组的每一项与 `http.route` 的参数是同一个形状）：

| 字段 | 说明 |
|---|---|
| `file` | 本地文件绝对路径（UTF-8）；与 `bytes` / `bytesBase64` 三选一 |
| `bytes` / `bytesBase64` | 内存内容（`bytes` 是文本、`bytesBase64` 是二进制），单条 ≤ 8 MiB |
| `path` | 请求路径：必须 `/` 开头、不含 `..`、在服务的 `path` 前缀之下（服务带前缀时路由路径也要带，例 `path: "/api"` → 路由 `/api/f/…`）；不写就自动生成 `<前缀>/f/<32 位十六进制>/<文件名>`（猜不到） |
| `contentType` | 响应的 Content-Type；不写按文件名推，再不行 `application/octet-stream` |
| `fileName` | 只用于 URL 尾段与 Content-Type 推导（会去掉路径分隔符与其他怪字符） |

```js
// 起一个只发静态内容的服务（第一次用到时起一次，之后复用）
let server = null;
function fileServer() {
    if (server) { return server; }
    const info = musicxx.net.http.info();
    server = (info.servers || []).find(function (item) { return item.id === "files"; }) ||
        musicxx.net.http.serve({ id: "files", port: 0, staticOnly: true });
    return server;
}

function serveLocalFile(path, name) {
    const routed = musicxx.net.http.route({
        serverId: fileServer().id,
        file: path,
        fileName: name,
    });
    if (!routed.ok) { return null; }   // routed.error / routed.message 说明原因
    // http://127.0.0.1:52341/f/<token>/a.mp3
    return { patch: { src: { path: routed.url, kind: "url", size: routed.size } } };
}

// 也可以在起服务时一次挂好；摘掉用 unroute
const hls = musicxx.net.http.serve({
    id: "hls",
    port: 0,
    staticOnly: true,
    static: [{ bytes: "#EXTM3U\n#EXT-X-VERSION:3\nseg0.ts\n", fileName: "index.m3u8",
               contentType: "application/vnd.apple.mpegurl" }],
});
musicxx.net.http.unroute({ serverId: hls.id, path: hls.routes[0].path });
```

必须知道的几条：

- **只监听回环**：默认 `host: "127.0.0.1"`，局域网访问不到。自动生成的路径里只有随机 token，
  服务端**按路径查表、不解析路径**，拿到 URL 也读不到表以外的文件。
- **支持 `GET` / `HEAD` 与单段 `Range`**（`206` + `Content-Range`；越界 `416`），文件按 64 KiB
  边读边写；播放器拖动进度就靠它，否则会退化成整段重下。多段 `Range` 当成"没有 Range"整段返回。
- **`staticOnly: true`**：没命中任何静态路由的请求直接回 404，不再发给插件（纯文件服务都该打开，
  否则每个 404 都要等到 `requestTimeoutMs` 才回 504）。不写这项时未命中的请求照旧发
  `http.request`，于是"同一个服务既能挂文件、也能自己回答接口"。
- **同一个文件 / 同一段内容重复登记返回同一个 URL**（按路径 + 大小 + 修改时间，或内容比较）。
- **生命周期跟着插件实例**：停用 / 卸载 / 关『拟声++』/ 插件框架停机时服务一起关掉（端口释放），
  路由也跟着没了；提前关服务用 `http.stop(id)`，只摘一条用 `http.unroute`。
- 容量：单条内存内容 ≤ 8 MiB（大文件用 `file`）、每个服务路由 ≤ 1024、服务数 ≤ 8、
  连接 ≤ 64（见 `musicxx.net.info()` 的 `limits`）。
- **老版本的插件框架没有静态路由**（只有 `musicxx.net.http.serve`）：调用前先看
  `typeof musicxx.net.http.route === "function"`，没有就自己降级（例如直接返回本地路径
  `kind:"file"`）。
- 这是**代理**的一般机制：`musicxx.net.fetch` / `download` 取到的数据 → `route`（小的给 `bytes`、
  大的落盘给 `file`）→ 播放器只看到干净的本地 URL；FTP、私有协议、格式转换、统一鉴权都可以
  这么做。完整示例见 `plugins/example_override_media_source`（本地文件代理）与
  `plugins/example_js_net`（`serve({static})` / `route` / `unroute` 自检）。

##### 内置网络能力（`musicxx.net.http / socket / ws`，JS 与动态库插件共用）

`musicxx.net.fetch` / `download` 就是 `http.fetch` / `http.download` 的别名（同一个实现、
结果形状也一样）；`http.request` 是它的原始形态，多给你 `statusText` / `redirects` / `bodyBase64`
这些细节。下面这套是同一套 socket 实现里的另外三组：HTTP 客户端与服务端、TCP、WebSocket。

| | `http.fetch` / `http.download` / `http.request` | `socket.*` / `ws.*` |
|---|---|---|
| 协议 | http / https | 原始 TCP / WebSocket（ws / **wss**） |
| 能起服务 | 能（http.serve，只 http） | 能（socket.listen / ws.listen，只明文） |
| 典型用途 | 拉取远端数据、下载文件、插件自己的小 API | 插件自己的音源服务、局域网小服务、长连接 |

**三条硬约定**：

- **客户端支持 `https://` 与 `wss://`**（OpenSSL 静态链接，证书链与主机名校验默认开）；
  **服务端只监听明文**：`http.serve` / `ws.listen` 没有 TLS 参数，也不需要。
  构建里关掉 TLS 时 `https` / `wss` 返回 `tls_unsupported`，可以用 `musicxx.net.info()` 的
  `tls` 字段先看一眼（`false` = 这个构建没有 TLS）。
- **服务端默认只监听回环 `127.0.0.1`**，端口由系统分配（`port: 0`）。要同网段可访问必须显式写
  `host: "0.0.0.0"` —— 框架不会替你拦，但你要清楚这意味着什么。
- **事件是异步的**：JS 侧由插件框架**直接投递到 JS 线程**（所以可以在返回 Promise 的能力里
  `await` 网络事件）；动态库插件用事件表订阅 `plugin.<插件id>.net`（在**插件框架线程**派发，
  处理器里不能阻塞）。

能力上限（超过按可读错误返回，不排队）：每个插件同类服务 ≤ 8、连接 ≤ 64、同时进行的 HTTP 客户端
请求 ≤ 16、单个 HTTP 服务等回答的请求 ≤ 64、HTTP 正文 ≤ 8 MiB、WebSocket 单条消息 ≤ 8 MiB、
单条连接发送队列 ≤ 4 MiB。

**HTTP 客户端**（`http.request`）参数：

| 参数 | 说明 |
|---|---|
| `url` | 必填，`http://host[:port]/path?query` 或 `https://…` |
| `method` | `GET`（缺省）/ `POST` / `PUT` / `PATCH` / `DELETE` / `HEAD` |
| `headers` | 对象；`Content-Length` / `Connection` / `Transfer-Encoding` 由框架生成，写了也被忽略 |
| `body` | 字符串正文 |
| `bodyBase64` | 二进制正文（与 `body` 二选一） |
| `json` | 任意 JSON：自动序列化并补 `Content-Type: application/json` |
| `timeoutMs` | 1000 ~ 120000，缺省 15000（整次请求，含解析、连接与 TLS 握手） |
| `maxBytes` | 正文上限 1024 ~ 16 MiB，缺省 8 MiB；读满就断开并标 `truncated:true` |
| `followRedirects` | 缺省 `true`（301/302/303/307/308） |
| `maxRedirects` | 0 ~ 5，缺省 3 |
| `tlsVerify` | https 时是否校验证书链与主机名，缺省 `true` |
| `caFile` / `caPem` / `serverName` | 追加信任的 CA / 覆盖 SNI 与主机名校验名（见上一节） |

响应（`http.response` 事件 / 动态库插件 `call_async` 的完成载荷 / JS Promise 的结果）：

```jsonc
{ "ok": true, "requestId": 12, "url": "…", "status": 200, "statusText": "OK",
  "headers": {"content-type": "application/json"}, "contentType": "application/json",
  "contentLength": 34, "receivedBytes": 34, "body": "…文本（合法 UTF-8 才有）…",
  "bodyBase64": "…", "bodyBytes": 34, "truncated": false, "redirects": 0, "elapsedMs": 12 }
```

失败（连接 / TLS / 解析 / 超时 / 取消）是 `{ok:false, requestId, error, message}`：
`bad_url` / `unsupported_scheme` / `resolve_failed` / `connect_failed` / `write_failed` /
`read_failed` / `bad_response` / `incomplete` / `timeout` / `cancelled`，以及 TLS 相关的
`tls_unsupported`（这个构建没有 TLS）/ `tls_config`（`caFile` / `caPem` 读不出来）/
`tls_cert_invalid`（证书链或主机名不对）/ `tls_handshake_failed`（协议协商失败）。
下载到本地时还可能是 `no_data_dir`（插件数据目录未就绪）/ `write_failed`（文件建不出来或写失败）。

**HTTP 服务端**：`http.serve`（`id?`, `host?`, `port?`, `path?` 前缀, `staticOnly?`,
`static?` 静态内容清单（见上一节）, `maxBodyBytes?`, `requestTimeoutMs?`, `maxConnections?`）
→ `{ok, id, host, port, url, routes?}`。每个请求发一条
`http.request`，**必须**用 `http.respond` 回答（`status` / `headers` / `contentType` /
`body`|`bodyBase64`），否则 `requestTimeoutMs`（缺省 30 s）后自动回 504 并关连接。
支持 `Connection: keep-alive`；分块请求体不支持（回 411，请用 `Content-Length`）。

**TCP**：`socket.connect` / `socket.listen` 之后用同一套 `socket.send` / `socket.close`
（服务端连接用事件里的 `id`，并带 `serverId` 区分来源）。字节以 `dataBase64` 给出，
合法 UTF-8 时再给 `text` / `data`。连接事件：`socket.connected` / `socket.accepted` /
`socket.data` / `socket.closed`（`by` = `remote` / `local` / `error` / `plugin`）。

**WebSocket**：`ws.connect`（`url` 或 `host`/`port`/`path`）/ `ws.listen` / `ws.send` /
`ws.ping` / `ws.close`。`wss://` 会先做 TLS 握手（同样支持 `tlsVerify` / `caFile` / `caPem` /
`serverName`）。握手、掩码、分片、`ping`/`pong` 与关闭握手都由框架处理；
文本帧给 `text`，二进制帧给 `dataBase64`；`ws.close` 带 `code` / `reason` / `by`。

**事件一览**（都在主题 `plugin.<插件id>.net`，载荷带 `kind`）：

| kind | 关键字段 |
|---|---|
| `http.listening` / `http.serverClosed` | `id, host, port, url` |
| `http.request` | `serverId, requestId, method, path, query, target, version, headers, remote, body, bodyBase64, bodyBytes` |
| `http.response` | 见上面的响应结构 |
| `socket.listening` / `socket.serverClosed` | `id, host, port` |
| `socket.accepted` | `serverId, id, remote, local` |
| `socket.connected` | `id, remote, local` |
| `socket.data` | `id, serverId?, dataBase64, text?, bytes` |
| `socket.closed` | `id, serverId?, by, error?` |
| `ws.listening` / `ws.serverClosed` | `id, host, port, url` |
| `ws.open` | `id, serverId?, url?, path?, remote, headers, subprotocol?` |
| `ws.message` | `id, serverId?, binary, text?, dataBase64, bytes` |
| `ws.pong` | `id, serverId?, bytes` |
| `ws.close` | `id, serverId?, by, code?, reason?, error?` |

```js
// JS：起 HTTP 服务，回答请求
musicxx.net.http.onRequest((req) => {
    musicxx.net.http.respond(req.requestId, { status: 200, body: "ok" });
});
const server = musicxx.net.http.serve({ port: 0 });       // {ok, id, host, port, url}
const resp = await musicxx.net.http.request(server.url + "/ping");
```

```cpp
// 动态库插件：同一套 op（结果 JSON 自己解析）
std::string out;
netCall(musicxx::plugin::net_op::httpServe, R"({"port":0})", out);
netSubscribe([](std::string_view event) { /* kind == "http.request" … */ });
```

**跟着插件实例走**：停用 / 卸载 / 关『拟声++』/ 插件框架停机时，该插件的服务、连接与未完成的
请求一起关掉（端口释放）。完整示例：`plugins/example_js_net`（六种角色一次跑完，载入后自动自检）。
旧插件框架没有这套能力：JS 先看 `typeof musicxx.net.http === "object"`，动态库插件看
`pluginxx::queryInterface<MusicxxPluginNetIface>(...)` 是否为 `nullptr`。

#### 3.2.7 渲染槽位（`musicxx.render.*`）

用来查询"有哪些可选样式"、读"现在选中/生效的是谁"、并且**允许插件切样式**（包括切回内置样式）。

| 动作 | 参数 | 返回 |
|---|---|---|
| `musicxx.render.list` | `{slot?}` | `{ok:true, slots:[{slot, title, selectedId, items:[...]}]}` |
| `musicxx.render.current` | `{slot?}` | `{ok:true, slot, id, title, source:"builtin"\|"plugin", plugin, night}`；没有生效项 → `{ok:false,error:"no_item"}` |
| `musicxx.render.select` | `{id, slot?}` | `{ok:true, slot, id}`；不可用 / 未知 → `{ok:false,error}` |

条目字段：`{id, title, depict?, source, plugin?, available, reason?, selected}`。
不给 `slot` 时用默认槽位（当前是 `player.background`，播放页背景）；槽位名还有 `player.icon`（播放页歌曲图）。
槽位与样式声明的完整说明见 [plugin-shader-bundle.md](plugin-shader-bundle.md)。

```js
const all = await musicxx.render.list();                        // 默认槽位
await musicxx.render.select("plugin.my_plugin.bg");             // 切到我的样式
await musicxx.render.select("builtin:Auto", { slot: "player.background" });  // 切回内置
```

**"现在是不是我在画"**用状态镜像同步判断（渲染线程里每帧都会读，不要用动作）：

```js
const slots = musicxx.state.get("musicxx.state.renderSlots") || {};
const mine = slots["player.background"];
if (mine && mine.itemId === "plugin.my_plugin.bg" && mine.visible) { /* 我在画，而且看得见 */ }
```

条目字段：`{itemId, plugin, selectedId, night, visible, animate, width, height, maxFps}`。
`visible:false` = 现在没渲染（播放页被遮挡 / 切后台），插件可以据此停掉耗时的计算。

#### 3.2.8 封面与频谱数据（`musicxx.media.*`）

封面**只作为数据来源**（不参与画面绘制、不推直链）；要在着色器里用封面，声明渲染项的 `cover` / `image`。

**`musicxx.media.palette`** —— 封面取色结果 + 内置背景实际用的 4 色

- 参数：`{force?:bool}`（`true` = 强制重新分析）。
- 返回：`{ok, hasCover, analyzed, night, source:"plugin"|"builtin", srcKey, version,
  raw:{main, light, lightMuted, dark, darkMuted, dominant:[…]}, background:[4 个 "#rrggbbaa"]}`；
  `source` 告诉你这套颜色是插件通过取色钩子给的，还是应用自己分析出来的。

**`musicxx.media.palette.set`** —— 把插件算好的颜色交给应用（写的是与内置分析同一批字段）

- 参数：`main` / `light` / `lightMuted` / `dark` / `darkMuted`（`#rrggbb` 或 `#aarrggbb`，至少给一个），
  没给的键保持原样。
- 返回：`{ok:true}`；一个颜色都没给 → `{ok:false, error:"至少要给一个颜色…"}`。
- 写完之后内置背景、插件背景与 `musicxx.icon.*` 来源都会用这套颜色。

**`musicxx.media.cover`** —— 当前封面的字节（自己要分析像素时用）

- 参数：`{size?=96, format?="jpeg", includePath?, bind?}`：
  - `size` 16~512（最长边等比缩放，超出会被钳到范围内）；
  - `format` = `jpeg`（缺省，质量 88）/ `png` / `rgba`（未压缩，有体积上限）；
  - `includePath` = 是否返回本地路径（只有本地 / 缓存来源有路径）；
  - `bind` = 把这次解码的像素**同时绑成纹理**，用这个键登记进插件图像表（渲染项用 `image` 引用它，
    复用同一次解码，不重复解码）。
- 返回：`{ok, srcKey, kind:"local"|"cache"|"content"|"asset"|"network", path?, width, height,
  format, bytes, sha256, data(base64), fromCache}`；失败 `{ok:false, error:"no_cover" |
  "cover_decode_failed" | "读取封面失败：…"}`。

**`musicxx.media.bindImage` / `musicxx.media.unbindImage`** —— 把插件自己的图片绑成纹理给着色器采样

- `bindImage`：`{key, data(base64), format:"png"|"jpeg"|"rgba", width?, height?}`（`rgba` 必须给宽高），
  返回 `{ok, key, width, height}`；键要 1~64 位字母数字与 `_ - .`，每个插件能绑的张数有上限。
- `unbindImage`：`{key}` → `{ok:true, released:bool}`。

**`musicxx.media.spectrum`** —— 当前音频频谱（由插件提供的提取实现算出来的那一份）

- 参数：`{bandCount?=16, unit?="normalized", includeBins?}`：
  - `bandCount` 1~64（不必整除 256）；
  - `unit` = `normalized`（0~1，缺省）/ `db` / `raw`；
  - `includeBins` = 附带当前帧的 256 个原始频点。
- **频点按频率分段，不是等宽的**：0 ~ 3840 Hz 每 30 Hz 一段（128 段），3.84 ~ 12 kHz 每段是前一段的
  1.01491 倍（约每倍频程 47 段），12 ~ 20 kHz 每段是前一段的 1.01007 倍（约每倍频程 69 段），低频在前。`bands` 是把这些频点再按频率分成 `bandCount` 段后的值
  （低频一列一段、高频按等倍频程加宽，每段取它覆盖的频点里**最大的那一段**），所以靠高频的一个频带
  覆盖的频率更宽（16 个频带时第 0 个是 0 ~ 30 Hz，第 11 个是 2.6 ~ 3.8 kHz，最后一个是 13.3 ~ 20 kHz）。
- **数值是"画图用"的**：`bands` 与 `frameData` 与内置『音乐动效』画出来的高度是同一套数值 ——
  每一帧按它自己的最高频点归一（最高点画到满刻度的九成，比它低 50 dB 的位置落到 0），再把挤在
  画布中下部的高低差拉开（比中线高的往上、低的往下，顶部与底部不动），最后按 gamma 1.2 拉一档；
  极轻的帧（最高点不到满刻度的四分之一）不做归一，静音保持全 0。
  好处是插件画柱状/光环时长年在低位的数值也能占满画面，代价是**不再等于绝对 dBFS**
  （`unit:"db"` 给的是"调整后数值落在 -80~0 dB 刻度上的 dB"）；要绝对电平做分析请用
  MCP 工具 `GetAudioSpectrum`（那是整曲提取的原始电平）。
- **数据本身已经适合直接渲染**：不用再自己抬增益、减底噪或取指数（那会把大多数频带压到 0）。
  确实要改对比度时，改完要拉回 0~1：`v` = 一个频带值、`m` = 这一帧的最大频带值，
  **放大差异**用 `pow(v / m, k)`（`k` > 1，先取指数再除回数值范围），**缩小差异**用
  `m * log(1 + a * v / m) / log(1 + a)`（`a` > 0，先取对数再乘回数值范围）；
  着色器写法与例子见 [plugin-shader-bundle.md](plugin-shader-bundle.md) §7。
- 返回：`{ok, status, available, loading, reason, source, srcKey, name, artist, durationMs,
  positionMs, frameHz, frameIndex, framePosition, frames, binCount, bandCount, unit, level,
  bands[], frameData?}`。
- **没有数据不是错误**：`ok` 仍然是 `true`，用 `status` 判断 ——
  `ready`（有数据）/ `loading`（正在提取）/ `none`（还没有对象）/ `off`（没有任何插件提供提取实现）/
  `unavailable`（来源不支持、时长过长或分析失败），原因写在 `reason` 里。`available` 为假时
  `level` 与 `bands` 全是 0，长度仍然等于请求的 `bandCount`（不要按数组长度猜有没有数据）。

```js
const cover = await musicxx.media.cover({ size: 96, format: "png", includePath: true });
if (cover.ok) { const bytes = cover.data; /* base64 */ }

const spec = await musicxx.media.spectrum({ bandCount: 16, includeBins: true });
if (spec.available) { drawBars(spec.bands); }

// 想要"每帧都新鲜"的频谱（着色器、自绘动效）读状态镜像，约 10 Hz：
const live = musicxx.state.get("musicxx.state.spectrum");
if (live && live.available) { drawBars(live.bands); }
```

**从插件提供频谱与副歌实现**（替代应用内置的那一份）：钩子 `musicxx.media.spectrum.extract`
与 `musicxx.media.chorus.analyse`，载荷、数据格式、超时与失效规则见 [plugin-hooks.md](plugin-hooks.md)，
写法示例见 [plugin-js-api.md](plugin-js-api.md) §11（C++ 同构）。

#### 3.2.9 全局字体（`musicxx.ui.setFontFamily` / `getFontFamily` / `loadFont`）

| 动作 | 参数 | 返回 |
|---|---|---|
| `musicxx.ui.setFontFamily` | `{fontFamily}`（`default` / `""` / `null` = 恢复默认） | `{ok:true, fontFamily, isCustom}` |
| `musicxx.ui.getFontFamily` | 无 | `{ok:true, fontFamily, defaultFontFamily, customFontFamily, isCustom, ownerPluginId}` |
| `musicxx.ui.loadFont` | `{family, path?\|data?, apply?=true}` | `{ok:true, family, applied, fontFamily}` |

- `loadFont` 的 `path` 是**相对插件目录**的字体文件路径（不能逃出插件目录），`data` 是 base64 字体字节，
  两者给一个；`apply` 为真时加载完立刻设为全局字体。字体文件上限 100 MiB。
- **换字体会重建整棵界面，动作本身可能耗时几百毫秒到几秒**：
  在声明式页面的按钮处理器里调用时必须"发出去就返回"（`.then` 收尾），
  `await` 它会与同步进插件框架的调用方互相等到超时（见 [plugin-ui.md](plugin-ui.md) §4.1）。

```js
await musicxx.ui.setFontFamily({ fontFamily: "我的字体" });
await musicxx.ui.loadFont({ family: "我的字体", path: "resource/font/my.ttf" });
await musicxx.ui.setFontFamily({ fontFamily: "default" });     // 恢复
```

#### 3.2.10 应用能力导出（`musicxx.feature.list` / `musicxx.feature.call`）

这两个动作读的是"**应用自己能做什么**"（与 MCP 工具、本机命令行共用同一份实现），
输出是给模型/脚本看的文本或 JSON：

| 动作 | 参数 | 返回 |
|---|---|---|
| `musicxx.feature.list` | 无 | `{ok:true, features:[{id, title, depict, args, output:"text"\|"json"}]}` |
| `musicxx.feature.call` | `{id, args}` | `{ok:true, text?, json?, output}` 或 `{ok:false, error}` |

- 典型能力：`GetAudioSpectrum`（整曲频谱的文本表）、`AnalyseAudioChorus`（副歌区段）等；
  **要解码整曲的长任务给更长的超时**（JS 封装 `musicxx.feature.call(id, args, timeoutMs)` 缺省 30 秒）。
- 想要**原始数据**而不是文本结论，用 §3.2.11 的功能点调用（`musicxx.feature.point.call`）。

```js
const list = await musicxx.feature.list();
const chorus = await musicxx.feature.call("AnalyseAudioChorus", {});
```

#### 3.2.11 功能点（`musicxx.feature.point.*`）

读"应用留出了哪些扩展点、现在谁在实现"，以及**主动调一个功能点拿原始数据**。
概念与写法见 §7，这里只列动作：

| 动作 | 参数 | 返回 |
|---|---|---|
| `musicxx.feature.point.list` | `{id?, provided?}` | `{ok:true, points:[{id, title, type, depict, available, callable, callDoc, callStat, rows:[{type, layer, owner, name, priority, enabled, effective, note}], stages:[{owner, name, layer, priority, enabled, before, after, runs, stops, drops, errors}], lastStoppedBy?, lastDroppedBy?, lastRewrittenBy?}]}` |
| `musicxx.feature.point.call` | `{id, args?, refresh?}` | `{ok, id, value, by, identity, fromCache, ms}` 或 `{ok:false, error, message}` |
| `musicxx.feature.point.define` | `{id, title?, depict?, args?, result?}` | `{ok:true, id}` 或 `{ok:false, error}` |
| `musicxx.feature.point.undefine` | `{id}` | `{ok, removed}` |

- `rows` 里的每一行是"这个功能点的一个实现"：`layer` = `extern`（外置插件钩子）/ `builtin`（内置插件）/
  `core`（应用兜底），`effective` = 现在真正生效的那一个；`note` 会写清"谁把它关掉了"（`disable` 裁决）。
- `stages` 是**同一个功能点的前链与后链**（与 `rows` 是两回事）：`before` / `after` 为真表示这条
  登记管哪一段，`runs` / `stops` / `drops` / `errors` 是计数；顺序按前链（层 → 优先级 → 登记顺序），
  **后链逆序执行**（所以这个列表的最后一行是最先改输出的）。`lastStoppedBy` / `lastDroppedBy` /
  `lastRewrittenBy` 是最近一次调用里"谁停下 / 谁丢弃 / 谁最后改了输出"，只用于排障。
  概念见 [plugin-api.md](plugin-api.md) §8.1。
- `callable` / `callDoc` 说明"这个点能不能被主动调用、参数与结果是什么形状"：
  **调之前先看 `callDoc`**。
- `musicxx.feature.point.call` 的 `error` 取值：
  `not_callable`（这个点不能主动调）/ `bad_args`（参数不符合 `callDoc`）/ `no_impl`（没有任何实现）/
  `disabled`（被置空裁决关掉了）/ `stopped`（前链把这次调用停下了）/ `dropped`（后链把结果丢弃了）/
  `busy`（同一功能点 + 同一调用方的上一次还没结束）/ `failed`（实现报错）。这三种"没有结果"是不同的事：
  `disabled` 是某个实现说"别问别人"、`stopped` 是前链不让执行、`dropped` 是实现跑完了但结果被丢掉。
- **调用零副作用**：只读值仓、不写值仓、不改应用状态。想让用户看到效果要用对应的设置动作
  （例如 `musicxx.media.palette.set`）。
- `refresh:true` 跳过值仓重新算一遍（一般不用：值仓里就是最近一次的结果）。

```js
const points = await musicxx.feature.points({ provided: true });   // 只看有实现的点
const colors = await musicxx.feature.callPoint("musicxx.media.palette.provide", {});
```

#### 3.2.12 钩子自省（`musicxx.hook.handlers`）

| 动作 | 参数 | 返回 |
|---|---|---|
| `musicxx.hook.handlers` | `{hook, phase?}` | `{ok:true, hook, phase, count, handlers:[{plugin, instance, kind, priority, phase, ownerTag}], listAvailable, phasesAvailable}` |

- `handlers` 按**调用顺序**排列（`priority` 升序，同优先级按注册顺序）；
- `phase` 是可选参数：`before` / `impl` / `after`，不给就是全部阶段（按 `before → impl → after` 拼接）；
  每一条 handler 都带自己的 `phase`；
- `listAvailable:false` = 插件框架库太旧，只能报数量（看 `count`）；`phasesAvailable:false` = 库没有
  分阶段接口，`before` / `after` 阶段现在不会有处理器；
- 同一个数据也能在管理页「外部插件 → 调试」和 `featurePoint.list` 里看到。

```js
const info = await musicxx.hooks.handlers("musicxx.media.spectrum.extract");   // JS 封装
if (info.count === 0) { /* 现在没人提供，我要不要注册 */ }
```

#### 3.2.13 宿主（`musicxx.host.*`）

| 动作 | 参数 | 返回 | 说明 |
|---|---|---|---|
| `musicxx.host.openUrl` | `{url}` | `{ok:true}` | 交给系统处理（协议不限，系统认不认是另一回事）；会先弹一次确认 |
| `musicxx.host.clipboard` | `{text}` | `{ok:true}` | 写系统剪贴板 |
| `musicxx.host.getPath` | 无 | `{pluginId, pluginDir, dataDir, logDir, platform}` | 插件目录 / 数据目录 / 日志目录，排查时很有用 |

`musicxx.host.shareText` 目前**没有实现**（见 §3.3）。

#### 3.2.14 统计上报（`musicxx.stats.*`）

| 动作 | 参数 | 返回 | 说明 |
|---|---|---|---|
| `musicxx.stats.reportMemory` | `{bytes}` | `{ok:true}` | 自报内存占用（只展示，不影响运行） |
| `musicxx.stats.reportMetric` | `{name, value}` | `{ok:true}` | 自报自定义指标（只展示） |

```js
musicxx.stats.reportMetric("hitCount", hits);       // 便捷封装，不 await 也不会报错
```

读自己的运行统计：JS 插件用 `musicxx.stats.getSelf()`（钩子 / 能力 / 订阅 / 定时器 / 未完成动作 /
脚本执行次数 / 错误数 / JS 堆大小 / 异步裁决结算数）；动态库插件目前只能看管理页
「插件详情 → 运行统计」。

#### 3.2.15 音源（`musicxx.source.*`）

插件自己的音源：在歌曲里存成"类型 + 资源 id" —— 类型 id 写成
`plugin.<你的插件 id>.<类型名>`，`src` 就是你自己定义的资源 id / 路径。主程序的内置实现
**不解析**这种类型，播放时问钩子 `musicxx.player.source.resolve`（见 §8.6）。三个动作做声明与自省：

| 动作 | 参数 | 返回 |
|---|---|---|
| `musicxx.source.define` | `{id, title?, depict?, srcDoc?}` | `{ok:true, id}` 或 `{ok:false, error}` |
| `musicxx.source.undefine` | `{id}` | `{ok, removed}` |
| `musicxx.source.list` | 无 | `{ok:true, sources:[{id, title, depict, srcDoc, plugin, declaredBy}], resolveChain:[…]}` |

- `id` 必须形如 `plugin.<你的插件 id>.<类型名>`；**声明只用于自省与更清楚的
  报错**：音源能不能播，看的是你有没有注册 `musicxx.player.source.resolve` 的处理器，不是有没有声明。
- 声明跟着插件生命周期走（停用 / 卸载 / 关『拟声++』自动撤销），不写入磁盘；重新启用时脚本会再声明一遍。
- `musicxx.source.list` 的 `resolveChain` 与 `musicxx.feature.point.list` 里看
  `musicxx.player.source.resolve` 是同一份数据（命令行 `source.list`、MCP `ListMediaSources` 也读它）。

```js
await musicxx.call("musicxx.source.define", {
    id: "plugin.my_plugin.flac", title: "我的高解析音源", srcDoc: "track/<歌曲 id>"
});
await musicxx.call("musicxx.player.playSrc", {
    src: "track/1", type: "plugin.my_plugin.flac"
});
```

#### 3.2.16 应用前后台（`musicxx.app.*`）

| 动作 | 参数 | 返回 | 说明 |
|---|---|---|---|
| `musicxx.app.foreground` | 无 | `{ok:true, requested:"foreground", foreground}` | 把主窗口显示到前台（移动端把应用拉到前台）。与命令行 `window.foreground` 是同一份实现 |
| `musicxx.app.background` | 无 | `{ok:true, requested:"background", foreground}` | 把主窗口隐藏到后台（移动端退到后台），播放不受影响。与命令行 `window.background` 是同一份实现 |

```js
await musicxx.call("musicxx.app.background");        // 隐藏窗口（用户看不见，但仍在播放）
const state = await musicxx.vars.get("musicxx.app.lifecycle");   // "foreground" / "background"
const off = musicxx.vars.bind("musicxx.app.lifecycle", (v) => musicxx.host.log(2, "现在：" + v));
```

- 切完**不要**假定状态已经变了：真正的状态由应用的生命周期事件决定，变了之后会派发钩子并刷新
  状态镜像 `musicxx.state.app`（`foreground` / `lifecycleState`），处理器里读到的才是新值。
- 要"进后台 / 回前台"做事，用钩子（§8）而不是在这里轮询动作；要当前值用变量
  `musicxx.app.lifecycle`（可订阅）或状态镜像。
- 用户可能正在看着窗口：隐藏前先想清楚（这类动作是"替用户按一下按钮"，不是给插件自己让路的）。

### 3.3 名字存在、但当前版本没有实现的动作

调用这些会立刻得到 `action_not_registered`（**别把功能押在它们上面**）：

`musicxx.player.setMediaType` · `musicxx.library.querySonglist` ·
`musicxx.library.addSong` · `musicxx.library.removeSong` · `musicxx.library.createSonglist` ·
`musicxx.library.setSongInfo` · `musicxx.library.search` · `musicxx.lyrics.getBySrc` ·
`musicxx.lyrics.set` · `musicxx.lyrics.sync` · `musicxx.lyrics.search` · `musicxx.ui.setEntryBadge` ·
`musicxx.host.shareText`

要“改应用状态”的能力缺口，用对应的**钩子**或**变量**补：例如想改播放速度 / 音调用动作
`musicxx.player.setSpeed` / `musicxx.player.setPitch`（已实现）；想拦一次设置变更用钩子
`musicxx.player.speed` / `musicxx.player.pitch`（已接入，见 [plugin-hooks.md](plugin-hooks.md)）。

---

## 4. 事件（已发生的事）

```js
musicxx.events.subscribe("musicxx.state.changed", (payload, topic) => { /* ... */ });
musicxx.events.publish("plugin.my_plugin.hello", { at: Date.now() });   // 只能发自己命名空间
musicxx.events.unsubscribe("musicxx.state.changed");
```

```cpp
subscribeTopic("musicxx.state.changed", [](std::string_view json) { /* 载荷 JSON */ });
// 发布：主题归属由插件框架校验
PluginxxStringView topic = pluginxxView("plugin.my_plugin.hello");
PluginxxStringView data  = pluginxxView(R"({"at":1})");
iface.events->publish(host, &topic, &data);
```

规则：

- **订阅**：官方 `musicxx.*` 主题与其它插件的 `plugin.<id>.*` 主题都能订；
  同一主题**只建立一条 host 订阅**（JS 侧再 `subscribe` 是复用，不是叠加）。
- **发布**：只能发官方主题或**自己命名空间**（`plugin.<自己的插件 id>.*`）；主题非法时
  JS 的 `publish` 直接抛异常、C++ 返回非 0。
- 插件之间协作：单向通知用事件，要返回值用能力调用（§6）。
- 事件处理器**要快**：它跑在插件框架线程 / JS 线程上，慢就是别人一起等。

常用主题（都是可订阅的）：

| 主题 | 什么时候来 | 载荷 |
|---|---|---|
| `musicxx.state.changed` | 状态镜像某个键变化 | `{key, value}` |
| `musicxx.var.changed` | 变量变化 | `{key, value, prev, by, revision, ts, removed?, coalesced?}`（`by` 空串 = 应用改的） |
| `musicxx.var.writeResult` | 自己发起的变量写入最终结果 | `{requestId, key, accepted, value, error?}` |
| `musicxx.ui.changed` | UI 项变化（可能是自己） | `{action, id, items}` |
| `musicxx.hook.changed` | 钩子处理器注册表变化 | `{id, hook, mode, count, action, handlers?}` |
| `musicxx.plugin.log` | 插件日志 | `{id, level, message}` |
| `musicxx.plugin.error` / `musicxx.plugin.warn` | 插件失败 / 告警 | 含原因 |
| `musicxx.hook.decision.result` | 异步裁决结果（调试用） | `{callId, hook, ...}` |
| `musicxx.app.start` / `musicxx.app.ready` … | 见 [plugin-hooks.md](plugin-hooks.md) | 钩子载荷 |

> 应用事件（`musicxx.state.changed` 之类）是**事件总线**上的主题；**钩子**是另一条通道
> （有模式、有合并策略、可能等裁决）。别把两者混起来：想在切歌时做事，注册钩子 `musicxx.song.changed`；
> 想在状态变了刷新自己的显示，订阅 `musicxx.state.changed`。

---

## 5. 变量（有属主、可读、可写、可订阅变动的小值）

**定位**：一份有当前值的小状态/配置 —— 要读、要改、改了要让别人知道。
真值在属主那里（应用的设置项，或你自己插件里的状态），插件框架只保留一份服务同步读的缓存。

与邻居的分工（选哪条通道看这里，别硬凑）：

| 需要 | 用哪条 |
|---|---|
| 每秒多次读的实时数据（播放进度、频谱） | 状态镜像（§2） |
| 一次性命令（播放、发通知、开页面） | 动作（§3） |
| 广播"发生了某事"（没有当前值） | 事件（§4） |
| **有当前值、要能改、改了要让别人知道** | **变量（本节）** |
| 钩子/热路径里**同步**判断 | 变量 + `watch`（保活缓存）+ `peek` |

### 5.1 三条读法

| 写法（JS） | 对应 C++ | 时延与新鲜度 | 适合 |
|---|---|---|---|
| `await musicxx.vars.get(key)` | `varsGet(key, &notify)` | 一次往返；值来自属主（权威） | 启动时读一次、低频读、要绝对正确 |
| `musicxx.vars.peek(key)` | `varsPeek(key)` | 查框架线程的表（纳秒级）；可能旧，返回里带 `ageMs` / `stale` | 钩子处理器、按帧 / 按事件的热路径（先用 `watch` 保活） |
| `musicxx.vars.bind(key, fn)` / `vars.watch(key)` | `varsSubscribe(keysJson)` + 订阅 `musicxx.var.changed` / `varsWatch` | 变化时回调 / 只保证 `peek` 新鲜 | 跟着变化做事 / 只要同步读不要回调 |

```js
// 权威读
const level = await musicxx.vars.get("musicxx.ui.animatedLevel");
const values = await musicxx.vars.getMany(["musicxx.ui.songIconRotate", "musicxx.ui.songIconWave"]);

// 热路径：先保活，再同步读
musicxx.vars.watch("musicxx.ui.songIconWave");
const cached = musicxx.vars.peek("musicxx.ui.songIconWave");   // {value, revision, ageMs, stale} 或 null

// 订阅变化
const off = musicxx.vars.bind("musicxx.ui.animatedLevel", (value, info) => {
    musicxx.host.log(2, "动画等级变成 " + value + "（由 " + (info.by || "应用") + " 改的）");
});
```

```cpp
// C++：同步读缓存（热路径），结果里带 revision / ageMs / stale
std::string cached = varsPeek("musicxx.ui.songIconWave");
// 异步权威读：结果经完成通知回来
PluginxxOperatorNotify notify{&onVarDone, this};
varsGet("musicxx.ui.animatedLevel", &notify);
// 订阅变化：登记关心之后，事件走 musicxx.var.changed（再用 subscribeTopic 订它）
varsSubscribe(R"(["musicxx.ui.animatedLevel"])");
```

读失败：JS 的 `get` 走 Promise 拒绝（`变量不存在` / `该变量不可读` / `读取超时`），
同步的 `peek` 返回 `null`（没有缓存过就是没有，不抛异常）。

### 5.2 写

```js
const result = await musicxx.vars.set("musicxx.theme.mode", "night");
if (!result.accepted) { musicxx.host.log(3, "被拒绝: " + result.error); }
```

```cpp
std::string out = varsSet("musicxx.ui.particleAnimate", "false");
// 立即结算时 out 是结果；交给属主写入时是 {"pending":true,"requestId":N}，
// 最终结果经事件 `musicxx.var.writeResult` 送达
```

写入一律**交给属主**：官方键由应用写入（走用户在设置里改的同一条路径）；你自己注册的 `declared`
变量由插件框架代存（立即结算）；`handler` 变量转回你自己的 `onWrite`。

### 5.3 注册自己的变量

键名写**短名**（`tip.start`），插件框架补成 `plugin.<你的插件id>.tip.start`；分组可以多级。

```js
// 声明式（缺省）：值由框架代存，set 即提交 —— 零代码
musicxx.vars.register({
  key: "tip.start",
  caps: ["get", "set", "notify"],   // 可省，缺省 ["get"]
  type: "bool",                      // bool|number|string|json（展示 + 粗校验）
  value: false,                      // 缓存初值（不是真值来源）
  title: "开场提示",
  depict: "为真时播放页显示一句开场提示",
  write: "any"                       // any（默认，谁都能写）| owner（只有自己）
});
```

```cpp
MusicxxPluginVarSpec spec{};
spec.version = 1;
spec.struct_size = sizeof(spec);
spec.key         = pluginxxView("tip.start");
spec.caps        = MUSICXX_PLUGIN_VAR_CAP_GET | MUSICXX_PLUGIN_VAR_CAP_SET | MUSICXX_PLUGIN_VAR_CAP_NOTIFY;
spec.write_scope = MUSICXX_PLUGIN_VAR_WRITE_ANY;
spec.mode        = MUSICXX_PLUGIN_VAR_MODE_DECLARED;
spec.type        = pluginxxView("bool");
spec.value_json  = pluginxxView("false");
spec.meta_json   = pluginxxView(R"({"title":"开场提示"})");
varsRegister(spec);
```

**handler 模式**：值本来就在你自己手里（不想再维护一份副本，或写入要立刻做别的事）：

```js
var tipStart = false;                       // 唯一真值在你这里

musicxx.vars.register({
  key: "tip.start",
  caps: ["get", "set", "notify"],
  mode: "handler",
  type: "bool",
  refreshAfterMs: 5000,                     // 有人 watch 时框架最多 5 秒来取一次真实值
  onRead:  function () { return tipStart; },             // 回答 get
  onWrite: function (value) {                            // 别人写这个变量时由这里处理
    tipStart = (value === true);
    return tipStart;                                     // 返回值 = 提交给框架的最终值
  }
});

// 插件内部改了自己的状态：主动提交一次，别人才看得到
function setTipStart(next) { tipStart = next; musicxx.vars.set("tip.start", next); }
```

```cpp
// C++ 的 handler 模式：订阅 musicxx.var.read / musicxx.var.write 两个事件，
// 处理完用 varsRespond 回答（也可以对自己的键 set 一次 = 提交，框架把它当回执与回答）
varsRegister(spec);                            // spec.mode = MUSICXX_PLUGIN_VAR_MODE_HANDLER
subscribeTopic("musicxx.var.read", [this](std::string_view json) {
    // json = {"requestId":N,"key":"plugin.my_plugin.tip.start"}
    varsRespond(requestId, /*ok=*/1, "true", nullptr);
});
subscribeTopic("musicxx.var.write", [this](std::string_view json) {
    // json = {"requestId":N,"key":"...","value":...,"by":"..."}
    varsRespond(requestId, 1, "true", nullptr);   // 拒绝时 ok=0 并把原因写进 error
});
```

### 5.4 自省

```js
musicxx.vars.has("musicxx.theme.available");     // 登记过没有（读注册表，不读值）
musicxx.vars.info("plugin.other.flag");          // {key,caps,mode,owner,type,title,value,revision,ageMs,stale,subscribers}
musicxx.vars.list("plugin.other.");              // 列出（值字段是缓存值）
musicxx.vars.own();                              // 自己注册的变量
musicxx.vars.subscribers("tip.start");           // 有多少人在看（订阅 + watch）
musicxx.vars.unregister("tip.start");
```

C++ 同名方法：`varsInfo` / `varsList` / `varsUnregister`（`own` 与 `subscribers` 没有单独方法，
用 `varsInfo` 读 `subscribers` 字段）。

### 5.5 高频与超时

- **不禁止高频，建议节流**：声明 `throttleMs`（推送侧合并）与 `notifyThrottleMs`
  （框架侧合并通知，载荷带 `coalesced`）。超过约 5 Hz 的键请务必声明。
- **没人看就不推**：没人订阅或 watch 的键，值变化不产生任何 FFI / 事件 / 回调。
- 超时：读默认 2 秒、写（应用键）5 秒 /（handler 属主）3 秒；超时按"读取超时 / 未回执"结算，
  **缓存保持不变**。读 / 写请求的未回执上限各 32 条。
- 变量数量、键长、值大小、订阅数都**不设上限**。

官方变量（`musicxx.*`）的键目录见 [plugin-vars.md](plugin-vars.md)：由应用声明并提供，
没声明的键读 / 写一律返回"未找到"。

---

## 6. 能力（插件之间的调用）

**能力 = "应用或别的插件按名字调我"**，参数与结果由插件自己定。

```js
musicxx.capability.register("probe", (args) => ({ ok: true, args }));       // 短名，全名自动补前缀
const info = await musicxx.capability.call("example_native", "probe", {});
```

```cpp
capability(*this, "plugin.my_plugin.probe",
    [this](std::string_view method, std::string_view args) -> std::string {
        return R"({"ok":true})";
    });

// 调用别的插件（异步；协程形态推荐）
pluginxx::Task<void> probeOther() {
    auto result = co_await capabilityCall("plugin.other_plugin.probe", "probe", R"({"n":1})");
    co_return;
}
```

- 能力全名是 `plugin.<插件 id>.<短名>`（JS 用短名注册，框架补前缀；C++ 直接写全名）。
- 处理器可以**同步返回**，也可以异步：
  - JS 返回 Promise（框架等它结算）；
  - C++ 返回 `pluginxx::Task<T>`（长任务请 `co_await pluginxx::offload(*this, work)` 或
    `pluginxx::sleep(*this, ms)`，不要占住框架线程）；
- 跨插件调用**只有异步形态**（调用方不会被慢插件卡住）。
- 超时缺省 3 秒（调用方自己给；`<= 0` 时插件框架仍按 3 秒处理，不是"一直等"）。
- 失败原因：`plugin_capability_not_found: <名字>` / `capability_call_failed` / `跨插件调用超时: ...`。
- **处理器里不要等主程序动作**：打开插件页面 / 点按钮时主程序在等能力结果，而主程序动作要由主程序线程执行，
  两边互等会让**整个应用卡住几秒**。要"现在这一帧"的数据读状态镜像；要动作结果就异步读回来、
  记进自己的状态，由页面下一次调用时回读。
- 插件页面就是能力：主程序打开 `ext://<插件id>/<视图id>` 时调用**同名能力**（见 §9.1）。

写给谁看：

| 目标 | 行为 |
|---|---|
| JS 插件 | 在共享 JS 线程上**直接调用**：处理器同步返回时结果立刻就有，返回 Promise 时等它结算 |
| 动态库插件 / 内置目标 | 投递到框架线程执行，脚本不阻塞；结果经事件回到 JS 线程 |

---

## 7. 功能点（声明 + 实现 + 调用）

功能点是"**这个功能谁都能实现**，应用与别的插件按 id 来调"。与能力的区别：

| | 能力 | 功能点 |
|---|---|---|
| 谁调 | 应用（打开页面 / 按名字调）或别的插件 | 应用、别的插件、命令行、agent 都能按 id 调 |
| 实现个数 | 一个插件的一个名字只有一个处理器 | **可以有多个实现**，按优先级依次问 |
| 参数 | 自己定 | 声明时写清（`args` / `result` / `callDoc`），调用方按它传 |

三件事：**声明**（说清这个点是什么）、**实现**（给出处理器）、**调用**（按 id 拿结果）。

```js
// 1) 声明（id 必须落在自己的命名空间里）
musicxx.feature.define({
    id: "plugin." + musicxx.pluginId + ".beat",
    title: "节拍检测",
    depict: "按 BPM 与拍号算出若干拍的时刻",
    args: "bpm（每分钟拍数，缺省 120）、beats（要几拍，缺省 4）",
    result: "{bpm, beats: [ms, ...], confident}"
});

// 2) 实现：返回的是**值**（糖自动包成 {"patch":{"value": …}}）
musicxx.feature.provide("plugin." + musicxx.pluginId + ".beat", function (args) {
    const bpm = (args && args.bpm) || 120;
    return { bpm: bpm, beats: [0, Math.round(60000 / bpm)], confident: 0.9 };
});
musicxx.feature.provide(["plugin.a.x", "plugin.a.y"], (args) => ({ ok: true }));   // 一组点共用一个实现

// 3) 调用（应用留出的点，或别的插件声明的点）
const colors = await musicxx.feature.callPoint("musicxx.media.palette.provide", {});

musicxx.feature.unprovide("plugin." + musicxx.pluginId + ".beat");
```

```cpp
// 1) 声明（结果经完成通知回来）
PluginxxOperatorNotify defineDone{&onDefineDone, this};
featurePointDefine("plugin.my_plugin.beat",
    MusicxxPluginPointSpec{"节拍检测", "按 BPM 与拍号算出若干拍的时刻",
                          "bpm（缺省 120）、beats（缺省 4）", "{bpm, beats:[ms,...], confident}"},
    &defineDone, 5000);

// 2) 实现：返回的是**值**（糖包成 {"patch":{"value": …}}）
featurePointProvide("plugin.my_plugin.beat", [](std::string_view argsJson) -> std::string {
    return R"({"bpm":128,"beats":[0,469,938],"confident":0.9})";
});

// 3) 调用：只有异步形态，结果在完成通知里
PluginxxOperatorNotify callDone{&onCallDone, this};
featurePointCall("musicxx.media.palette.provide", R"({"path":"D:/covers/a.jpg"})", 60000, &callDone);
```

必须记住的约定：

- **回答必须用 `patch` 包一层**（裸字段会被后面的处理器覆盖）：写钩子处理器时是
  `{"patch":{"value":…}}`；`provide` 的糖会替你包。
- **按 `point` 过滤 + 不回答自己发起的调用**：同一条通道上所有实现都会收到同一次调用，
  `caller` 等于自己的插件 id 时要返回空（糖自动做这两件事）。
- **`value` 键的存在 = "我答了"**（值可以是 `null`，表示"算过了、结果是空"）；返回空串 / `undefined` = 不回答。
- **处理器必须同步返回**：功能点走同步裁决通道，返回 Promise / `Task<T>` 拿不到结果（会记一条日志）。
  长任务请提前算好，或做成**能力**（§6）。
- **前链 / 后链**：同一个点在通道上还有 `before` / `after` 两个阶段（同一条 id，用 `phase` 区分）——
  前链改这次调用的 `args`（或 `{"action":"stop"}` 这次不执行），后链改最终 `value`（或
  `{"action":"drop"}` 这次没有结果）；只有 `impl` 阶段算"有人实现"。`provide` 的糖只登记实现链，
  要插手前后链直接用钩子 API（`hooks.register("musicxx.feature.point.call", {phase:"after"}, fn)`）。
  概念与顺序见 §8.1。
- 声明跟着插件生命周期走：禁用 / 卸载会撤销全部声明，重新启用时脚本 / `start` 里再声明一遍。
- 同一个功能点 + 同一个调用方的上一次调用没结束时，调用方拿到 `busy`（不排队）。

应用**留出的、可以被主动调用**的功能点有三个（`featurePoint.list` 的 `callDoc` 写着参数与结果形状）：

| 功能点 id | 作用 | 调用的代价 |
|---|---|---|
| `musicxx.media.spectrum.extract` | 整曲频谱（帧数 × 256 个频点 + 每帧响度） | 要解码整曲，慢（缺省超时 60 秒） |
| `musicxx.media.chorus.analyse` | 副歌（高潮）区段 | 会先取一次频谱，慢 |
| `musicxx.media.palette.provide` | 封面取色（5 个色槽位） | 要分析一张图 |

---

## 8. 钩子（应用来问我 / 告诉我）

钩子 id、模式（`observe` / `decision`）、派发方式（`sync` / `async`）、合并策略，以及**哪些钩子已经被应用接入**，
都在生成物 [plugin-hooks.md](plugin-hooks.md) 里；本节说"怎么写、为什么这么写"。

### 8.1 三个阶段：前链 / 实现链 / 后链

**钩子 id 表示一次调用；参与方式由注册时的 `phase` 表达。** 不要再找 `.before` / `.after` 后缀的 id ——
那些成对的 id 已经合并成一条钩子上的两个阶段。

| 阶段 | 什么时候跑 | 能做什么 | 不能做什么 | 回答格式 | 结束整条链 |
|---|---|---|---|---|---|
| `before` | 进入调用之前，串行、**正序** | 改输入（累加）、停下（这次不执行实现） | 给数据（那是实现链的事） | `{"patch": {...}}` | `{"action":"stop"}` |
| `impl` | 实现链，串行 | 给结果 / 裁决 | 改输入 / 改输出 | 与以前一样（§8.2） | 按钩子的合并策略 |
| `after` | 拿到结果之后，串行、**逆序** | 改输出（累加）、丢弃（这次没有结果） | 造出本来没有的结果 | `{"value": <新结果>}` | `{"action":"drop"}` |

```text
线头（离调用方最近）                              线尾（离实现最近）
extern  ---------->  builtin  ---------->  core
   ^                                          |
   +---------------  后链（逆序） <------------+
```

- **前链正序**：每一步拿到上一步改过的输入（不是所有人看同一份原始载荷）；
- **后链逆序**：每一步拿到上一步改过的输出。所以**外置插件最后改输出**，应用自己的兜底实现
  不能盖掉插件的改写（与实现链里“外置优先”同向）。这一点反直觉，但必须这么做；
- 同一个插件对同一个钩子可以分别注册三个阶段（同一个 `ownerTag` 互不覆盖）；
  `unregister` 按 `(钩子, ownerTag)` 摘，一个 `ownerTag` 在所有阶段的登记一起摘掉；
- **前链 / 后链必须是同步处理器**（要拿到这一步的结果才能跑下一步）：用异步处理器注册会被拒绝；
- 只能注册钩子**声明过的**阶段（生成物 `plugin-hooks.md` 的「阶段」列）；没声明就注册会被
  拒绝（C++ 返回 `-1` 并记日志，JS 在后台拒绝并记日志），因为“注册成功但永远不触发”
  比报错更难查；
- 三种“没有结果”含义不同，别混：实现链的 `{"disable": true}`（不再问别人）、前链的 `stop`
  （这次不执行实现）、后链的 `drop`（实现跑完了但结果丢掉）。
- **载荷里不自动带 `phase`**：同一个函数要服务多个阶段就各注册一份自己的闭包，不要指望从载荷里
  分辨阶段。唯一例外是插件功能点的通用通道 `musicxx.feature.point.call` —— 它的载荷带 `phase`，
  因为一个插件点可能用同一段代码处理三个阶段（见 §7）。

包装点（网络请求 / 下载 / 歌单读取这类“实现固定是应用自己”的钩子）没有 `impl` 阶段，
插件只在前后插手；取值点（频谱 / 副歌 / 取色 / 音源解析）通常只有 `impl`。

### 8.1.1 注册与注销

```js
musicxx.hooks.register(id, { mode, priority, ownerTag, phase }, fn);  // 顶层同步注册或运行期注册都行
musicxx.hooks.unregister(id);           // 摘掉这个钩子上自己的全部阶段
musicxx.hooks.unregister(id, "after");  // 只摘那个阶段
musicxx.hooks.has(id);                  // 自己注册了没有（缺省看 impl 阶段）
musicxx.hooks.has(id, "after");         // 某个阶段注册了没有
musicxx.hooks.handlers(id);              // Promise：谁在提供这个钩子（§3.2.12）
musicxx.hooks.handlers(id, "after");     // 只看一个阶段（缺省 = 全部阶段）
```

```cpp
hook(MUSICXX_PLUGIN_HOOK_PLAYER_BEFORE_PLAY_SONG, 0,      // 裁决型，priority = 0
     [](std::string_view input, std::string& out) -> int32_t {
         if (input.find("广告") != std::string_view::npos) { out = R"({"action":"skip"})"; }
         return 0;      // 返回 0 且 out 非空 = 给出裁决；out 空 = 不裁决
     });

observe(MUSICXX_PLUGIN_HOOK_SONG_CHANGED,                  // 观察型
        [](std::string_view input, std::string&) -> int32_t { /* 返回值忽略 */ return 0; });

// 阶段: 同一条钩子上的前链 / 后链（必须是同步处理器）
hook(MUSICXX_PLUGIN_HOOK_PLAYER_SEEK, HookPhase::After, 0,
     [](std::string_view input, std::string& out) -> int32_t {
         out = R"({"value":{"action":"continue","patch":{"to":60000}}})";
         return 0;
     });
hookBefore(MUSICXX_PLUGIN_HOOK_PLAYER_SEEK, 0, ...);   // 等价写法
hookAfter (MUSICXX_PLUGIN_HOOK_PLAYER_SEEK, 0, ...);

unregisterHook(MUSICXX_PLUGIN_HOOK_SONG_CHANGED);
```

- `mode`：`observe` = 只通知（返回值忽略）；`decision` = 可裁决（返回 `null` = 不表态）。
- `priority`：小者先执行，同优先级按注册顺序。应用询问"这个扩展点谁来实现"时的层间顺序是
  「**外置插件 → 内置插件 → 应用自己兜底**」：外置排最前是刻意的，用户装的插件要能替代内置功能；
  想知道某个功能点现在是谁在生效，用动作 `musicxx.feature.point.list`（§3.2.11）。
- `ownerTag`：同一插件在同一个钩子上区分多个处理器用的标记。
  **不要用不同 `ownerTag` 注册同一个钩子**：JS 侧只保留一个处理器、框架侧却留下多条注册，
  表现为处理器被重复调用。同一个钩子只注册一次。
- 钩子必须在约定表里（`musicxx.*` 全名），**未知钩子注册会被拒绝**：C++ 侧注册调用返回 `-4`；
  JS 侧由插件框架在后台拒绝并记日志（`register` 本身不会报错），所以写错 id 的表现是
  **处理器永远不被调用**。JS 的 `musicxx.hooks.has(id)` 只看脚本自己注册没注册，不代表插件框架受理了。
  插件自定义事件主题 / 能力名 / UI 项 id 才用 `plugin.<自己的插件 id>.*`。
- **只有"已接入"的钩子会触发**：当前 21 个应用派发的钩子（`plugin-hooks.md` 的「主程序是否已接入」列
  标「已接入」的共 22 行 —— 多出来的第 22 条是插件功能点的通用通道 `musicxx.feature.point.call`，
  由调用方触发、不是应用主动派发）；其余钩子注册成功但不会触发 —— 别把功能押在未接入的钩子上（它们打算做什么见 §8.7）。

### 8.2 裁决对象

实现链（以及普通裁决钩子）的回答与以前一样：

```jsonc
{ "action": "continue" | "skip" | "cancel" | "replace",
  "patch": { /* 钩子专属字段 */ },
  "error": "可选说明" }
```

- 返回 `null` / 空对象 = “不裁决”，交给下一个处理器；`action` 的语义由各调用点决定
  （例如 `beforePlaySong` 的 `skip` = 跳过本曲）。
- **裁决是“建议”**：调用点会校验裁决的身份字段（如 `sid`），过期的裁决会被丢掉 ——
  异步派发的钩子尤其要注意（等待期间可能已经切歌 / 切列表）。
- **处理器失败只记统计**：抛异常 / 返回失败不会暂停派发、也不会卸载插件。

前链 / 后链用自己的两种回答（见 §8.1）：

```jsonc
// before：只认 patch（一步接一步合并进本轮载荷），action=stop 结束整条链
{ "patch": { "to": 60000 }, "reason": "可选说明" }
{ "action": "stop" }

// after：只认 value（替换当前结果），action=drop 结束整条链
{ "value": { "action": "continue", "patch": { "to": 60000 } } }
{ "action": "drop" }
```

- **`patch` / `value` 是一层包装**：其余字段只是顺带说明（`reason` 之类），不参与合并；
- `before` 的 `patch` 里能写什么由那个钩子的载荷字段决定（嵌套对象递归合并）；
- `after` 的 `value` 键存在就算答了（值可以是 `null`，表示“算过了、结果是空”）；
- 没意见就返回空，不要为了“不改”返回一份原样的载荷。

### 8.3 异步裁决

```js
// musicxx.player.speed 的载荷是 {from, to}：把不合法的新值换成合法值
musicxx.hooks.register("musicxx.player.speed", { mode: "decision" }, (ctx) => {
    if (ctx.to >= 0.25 && ctx.to <= 3) return null;     // 同步路径：最省时
    return new Promise((resolve) => {                    // 异步路径
        setTimeout(() => resolve({ patch: { to: 3 } }), 20);
    });
});
```

- 插件框架**一直等到 Promise / 异步处理器结算**（框架侧没有等待超时）；结算后裁决生效。
- 但**调用点可能自己设了上限**：`musicxx.player.seek` / `volume` / `speed` / `pitch` 在界面操作路径上，
  应用最多等 **0.5 秒**（超时按“没意见”走原值），`musicxx.player.source.resolve` 最多等 5 秒，
  `musicxx.player.error` 等它结算。**每个钩子具体等多久看 [plugin-hooks.md](plugin-hooks.md) 的说明**。
- 所以异步路径要保证**一定会结算**（失败就 reject）；不结算就是调用点一直等到它的上限。
- 结算次数在统计里看得到（`asyncHookSettled`）。

### 8.4 处理器里不要做这些

- **不要阻塞**：网络、大文件、编解码、`sleep` 都不要直接在处理器里做 ——
  它跑在插件框架线程 / JS 线程上，慢就是业务与其它插件一起等。
  动态库插件用 `pluginxx::offload` / `pluginxx::sleep`；JS 用定时器或把工作拆成两段。
- **不要等主程序动作**（同步派发的钩子尤其致命）：调用点就地等你的返回值，而动作要由主程序线程执行，两边互等。
- 必要的长任务提前算好（例如换歌时预热缓存），处理器里只做"查缓存 → 返回或返回空"。

### 8.5 取值型钩子的"置空"与"不提供"

频谱提取、副歌分析、封面取色这三个取值型钩子支持显式置空：

- 返回 `{"disable": true}` = "这次不要用别的实现"（应用自己的兜底也不会被问），应用会记一条日志并在自省里
  标出是谁关的；处理器被摘除（停用 / 卸载 / 关闭『拟声++』）后自动还原。
- 返回 `null` / 空 = "这次不提供"（链条继续问下一个实现）。
- **不要拿 `disable` 当"我没算完"的挡箭牌**：那样用户会看到功能凭空消失。

### 8.6 音源解析与插件音源（`musicxx.player.source.resolve`）

应用在**把一个音源变成播放器能打开的地址与请求头**时派发这条钩子（`decision` + `firstNonNull` +
异步）。顺序是 **外置插件 → 内置插件（云盘 / 媒体库）→ 应用自身（本地文件 / 链接 / Bili）**：
你注册了处理器就排在最前，既给自己的音源写地址，也能给内置来源换地址。

```js
musicxx.hooks.register("musicxx.player.source.resolve", { mode: "decision" }, (ctx) => {
    if (ctx.pluginType !== "plugin.my_plugin.flac") return null;   // 不是我的音源：不回答
    const track = ctx.src;    // src 就是你自己写下的资源 id
    return { patch: { src: {
        path: `https://cdn.example/${track}.flac`,
        headers: { Referer: "https://example/" },
        kind: "url"
    } } };
});
```

#### 插件音源怎么写进歌曲

插件音源（类型 id `plugin.<插件 id>.<类型名>`）在歌曲里存成**类型 + 资源 id**：

| 部件 | 内容 | 例 |
|---|---|---|
| 类型 | 你声明并注册解析的类型 id | `plugin.my_plugin.flac` |
| `src` | 你自己定义的资源 id / 路径（怎么解释由你决定） | `track-123` |

类型串存在音源信息里（序列化后形如 `{"type":"plugin.my_plugin.flac", …}track-123`），
应用侧的入口（`musicxx.player.playSrc`、applink）写 `type` 时直接给类型 id 即可：

```bash
musicxx-cli call musicxx.player.playSrc --args '{"src":"track-123","type":"plugin.my_plugin.flac"}'
```

载荷字段：`srcKey`（音源身份 md5）、`type`（本轮音源类型；插件音源是空串）与 `crudeType`
（歌曲里保存的原始类型）、`src`（本轮要解析的地址 / 资源 id）与 `crudeSrc`（原始地址）、
`fileName` / `durationMs` / `quality`（文件名、时长、用户音质偏好）、`isMediaSrc`
（`false` = 这次要的是封面）、`pluginType`（本轮音源的插件音源类型 id，不是插件音源时为 `null`）、
`caller`（谁在问，空 = 应用自己）。

回答：`{"src": {"path": "<地址>", "headers": {...}, "kind": "url"}}`（`src` 也可以直接给地址字符串）：

- `path` 可以是网络地址、本地文件路径、`content://`、`asset://`；
- `kind` 取 `url` / `file` / `content` / `asset`，**缺省按地址自己判断**。写错会让本地文件被当成
  网络流去套 HTTP 缓存代理（地址还会被当成 URI 改写），能写就写；
- `headers` 是这个地址要用到的请求头（可以省）；`expireAtMs` / `size` / `durationMs` / `quality` 可选；
- **用 `patch` 包一层**（多个实现时裸字段会被后面的处理器盖掉，只有一个实现时两种写法等价）；
- 返回空 / 结果不合法 = 这次不提供，应用接着问内置实现；返回 `{"disable": true}` = 这次不再问
  别的实现，该音源会解析失败（只在明确知道它不该播时用）。

注意：

- **载荷里带来源字符串**（插件自己的音源就是你自己写下的资源 id，内置来源是已经解析出来的临时直链或
  本地路径）—— 这是"能换地址"的前提，注册这条钩子的插件看得到这些内容。
- 应用最多等你 **5 秒**（超时按"没给"处理）；地址是临时直链时自己缓存好，处理器里越快越好。
- 观看型的 `musicxx.player.source.resolved` 只报"种类"，**不给地址**：要看播放是否成功用它。

### 8.7 未接入钩子的名字说明

下面这些钩子**约定已经定下来、主程序还没接**（注册不会触发，将来接入时的载荷以
`plugin-hooks.md` 为准）。列在这里是让你知道"这个名字打算做什么"，别猜：

| 钩子 | 打算做什么 |
|---|---|
| `musicxx.app.exit` | 退出前让插件拦一下（`decision`） |
| `musicxx.app.deepLink` | 外部链接进来时让插件拦一下 / 处理 |
| `musicxx.app.upgrade` | 版本升级后的通知（做数据迁移） |
| `musicxx.player.qualityChanged` | 音质切换的通知 |
| `musicxx.media.notification` | 系统媒体通知的内容（标题/封面）让插件改写 |
| `musicxx.song.info.analyse` | 让插件提供歌名 / 歌手识别（替代应用不联网的识别） |
| `musicxx.song.meta.writeback` | 歌曲信息回写时的通知 |
| `musicxx.song.beforeAdd` | 加歌前拦一下 |
| `musicxx.song.removed` | 歌曲被移除的通知 |
| `musicxx.playlist.load` / `musicxx.songlist.filter` | 歌单读取的前后插手（`after` 就是原来的“过滤”，能读到最终列表）、歌曲列表的过滤（可增删条目） |
| `musicxx.history.record` | 写播放记录时的通知 |
| `musicxx.local.scan.file` / `musicxx.local.scan.finished` | 本地扫描时逐文件裁决（跳过）/ 扫描完成通知 |
| `musicxx.lyric.current` | 当前歌词行变化通知（歌词加载已按 `musicxx.lyric.load` 接入，见 `plugin-hooks.md`） |
| `musicxx.icon.fetch` | 让插件提供封面图（`impl` 给图，前链改尺寸 / 来源，后链改最终那张图） |
| `musicxx.media.info.request` | 让插件提供媒体信息（时长、码率等） |
| `musicxx.net.request` | 应用发外网请求的前后插手（前链改请求 / 取消，后链读改响应） |
| `musicxx.net.server.route` / `musicxx.net.mcp.tools` / `musicxx.net.lan.event` | 本机服务的路由交给插件、MCP 工具清单追加、局域网事件通知 |
| `musicxx.download` | 下载的前后插手（前链改文件名 / 目录或取消，后链拿成品路径） |
| `musicxx.cache.beforeTrim` / `musicxx.cache.pathRequest` | 缓存裁剪前裁决 / 缓存路径由插件指定 |
| `musicxx.ui.home.entries` / `musicxx.ui.song.actions` / `musicxx.ui.playlist.actions` / `musicxx.ui.route.resolve` | 首页入口、歌曲菜单、歌单菜单的**钩子形式**（现在用 UI 项声明，见 §9.1）；路由解析 |
| `musicxx.ui.page.enter` / `musicxx.ui.page.leave` / `musicxx.ui.theme.changed` / `musicxx.ui.user.action` | 页面进入 / 离开、主题变化、用户操作的通知 |
| `musicxx.ui.notify` | 应用要弹提示时让插件拦一下 / 改写 |
| `musicxx.script.bound` / `musicxx.script.disposed` / `musicxx.script.action.executed` | 歌曲脚本绑定 / 解绑 / 执行通知 |
| `musicxx.clocking.tick` | 定时停止的倒计时 tick |
| `musicxx.listenTogether.event` | "一起听"的事件通知 |

---

## 9. 界面与渲染（入口）

### 9.1 界面（完整字段见 [plugin-ui.md](plugin-ui.md)）

声明式界面：插件只声明"长什么样、点了做什么"，渲染由应用完成。

```js
musicxx.ui.registerEntry({                        // 主页入口
    name: "card", type: "home.entry", order: 120,
    data: { title: "我的插件", subtitle: "示例入口",
            action: { kind: "route", route: "ext://my_plugin/card" } }
});

musicxx.ui.registerEntry({                        // 歌曲菜单项（data 字段也可以平铺）
    name: "clear", type: "song.action", order: 900,
    title: "用我的插件处理这首歌",
    action: { kind: "capability", name: "handleSong" }
});

musicxx.capability.register("card", (args) => ({   // 插件页面：主程序调用同名能力取视图描述
    view: { title: "我的插件", blocks: [
        { kind: "Text", text: "插件只返回块描述，渲染由主程序完成。" },
        { kind: "Button", title: "播放/暂停", style: "primary",
          action: { kind: "command", name: "musicxx.player.toggle" } }
    ]}
}));
```

```cpp
uiRegister("card", MUSICXX_PLUGIN_UI_TYPE_HOME_ENTRY,
           R"({"title":"我的插件","action":{"kind":"route","route":"ext://my_plugin/card"}})", 120);
uiRegister("bg", MUSICXX_PLUGIN_UI_TYPE_PLAYING_BACKGROUND, backgroundJson, 20);
uiUpdate("card", nextJson);      // 整体替换声明（改一个字段也要传完整 data）
uiUnregister("card");
uiNotify(R"({"text":"已刷新"})");
```

- UI 类型（短名 / 全名）：`home.entry`（主页入口）、`song.action`（歌曲菜单）、`playlist.action`（歌单菜单）、
  `playing.background`（播放页背景）、`playing.icon`（播放页歌曲图）。
- **页面是"取一次"的快照**：改完配置要让能力再返回一次 `{view: …}`（或重新打开页面）。
- **入口 / 按钮指向的能力必须真的注册**：只声明入口不实现能力时，打开页面只会提示
  「插件『<插件id>』没有提供『xxx』」。
- 动作写法只有四种：`route` / `dispatch` / `command` / `none`。
  入口/菜单项里的 `route` 与动作 `musicxx.ui.openRoute` 走同一套规则：**允许指向任意插件**的页面
  （`ext://<插件id>/<视图id>`，含自己）。跨插件时页面由**对方插件**自己绘制，发起方拿不到对方的数据，
  所以只该跳"两边都装得上"的公共页面，跳之前用 `musicxx.plugin.list` 之类的办法确认对方在。
- **组件要按客户端能力降级**：`musicxx.host.info().ui`（`blocks` / `controls` / `kind`）告诉你这个
  客户端支持哪些块与控件；图标要插件**自己带 svg**（写相对插件目录的路径，能力段里没有图标清单）。
- 字重只来自 `bold: true` 或 `type: "title"`；块没有隐含边距，要留白自己写 `margin` / `padding`。

### 9.2 渲染槽位与着色器（完整说明见 [plugin-shader-bundle.md](plugin-shader-bundle.md)）

- 播放页背景（`playing.background`）与播放页歌曲图（`playing.icon`）可以交给插件：
  `shader`（插件着色器绘制）、`view`（界面视图组合）、`mode`（只保留占位 / 不显示）三选一。
- 着色器是**打包期编译好的 shader bundle**（`format_version` 必须与应用使用的 Flutter 版本一致）。
- 需要封面纹理时在渲染项里声明 `cover` / `image`：应用负责解码、缩放、预模糊、上传纹理，
  每帧把纹理绑给着色器（插件不搬字节）。
- 每帧都会求值的参数用 `args` 声明（成员名 → 值表达式），应用每帧现读现写；
  频谱来源（`musicxx.spectrum.*`）是例外：**没有数据时写 0（静音）**，要看 `uEnv.z` 判断有没有数据。
- 运行状态（谁在画、是否可见、尺寸、昼夜）读 §3.2.7 的 `musicxx.state.renderSlots`。

---

## 10. 日志、统计与自省

### 10.1 日志

```js
musicxx.host.log(2, "文字");        // 0 trace / 1 debug / 2 info / 3 warn / 4 error
console.log("也可以");              // 映射到 info；warn / error 同理
```

```cpp
log.trace("…"); log.debug("…"); log.info("…"); log.warn("…"); log.error("…");
```

日志进「管理页 → 外部插件 → 插件详情 → 日志」，也作为 `musicxx.plugin.log` 事件回传；
插件框架自己的日志设环境变量 `MUSICXX_EXTERN_PLUGIN_LOG_STDERR=1` 可以打印到 stderr。
**日志不要当数据通道用**（高频写日志会拖慢应用）。

### 10.2 自省三件套

| 想知道 | 用 |
|---|---|
| 这个钩子现在有谁在提供、排第几 | 动作 `musicxx.hook.handlers`（§3.2.12）；命令行 `musicxx-cli featurePoint.list` 与 MCP `GetFeatureProviders` 读的是同一份数据 |
| 应用留出了哪些功能点、谁在实现、谁关掉了 | 动作 `musicxx.feature.point.list`（§3.2.11） |
| 应用自己能做什么（给模型看的文本结论） | 动作 `musicxx.feature.list`（§3.2.10） |
| 我自己跑了多少、错了几次 | JS：`musicxx.stats.getSelf()`；两边都看得到：管理页「插件详情 → 运行统计」 |

### 10.3 应用卡住时怎么查

主程序日志目录下有一份 `host_call.log`（进插件框架调用的同步轨迹）：**卡住后最后一行就是没有返回的那次调用**。
配合「插件详情 → 日志」就能区分是脚本死循环、动作等待，还是插件框架本身的问题。

---

## 11. 通用约定

### 11.1 命名空间

- 官方标识一律 `musicxx.*`（钩子 / 事件主题 / 动作 / UI 类型 / 权限 / 状态镜像键 / 变量键）；
- 插件自定义一律 `plugin.<自己的插件 id>.*`（事件主题、能力名、UI 项 id、变量键、功能点 id）；
- 冒充他人命名空间会被拒绝（返回 `-6`）。**动作名与钩子 id 不受此限**：
  动作任意名字都受理（由主程序决定理不理），钩子必须是约定表里已有的。

### 11.2 容量与限制（当前版本）

| 项目 | 限制 |
|---|---|
| 状态镜像单键大小 | 不限制（必须是合法 JSON。镜像由应用按需推送、插件只读；高频数据用变量并声明 `throttleMs`） |
| 插件私有 KV / `config.json` | 不设配额（每次写入整文件重写，别当数据库） |
| `musicxx.storage.list` | 每次最多 500 个键 |
| 变量数量 / 键长 / 值大小 / 订阅数 | 不限制 |
| 变量读 / 写未回执 | 各 ≤ 32 条；读超时 2 s、写 3 s（应用键 5 s） |
| `musicxx.net.fetch` / `download` | 超时 1~60 s、`maxBytes` 1 KiB~16 MiB |
| `musicxx.media.cover` | `size` 16~512、`rgba` 有体积上限、结果缓存 16 条 |
| 静态路由（`http.serve({static})` / `http.route`） | 单条内存内容 ≤ 8 MiB；每个服务 ≤ 1024 条；服务 ≤ 8；连接 ≤ 64；只监听 127.0.0.1 |
| `musicxx.media.bindImage` | 键 1~64 位 `[A-Za-z0-9_.-]`、每插件张数有上限、最长边有上限 |
| `musicxx.ui.loadFont` | 字体文件 ≤ 100 MiB |
| 性能 / 内存 / 网络 | 插件框架不做限制（插件的内存、耗时、数量都不设上限；自己别把应用拖死） |

### 11.3 超时一览

| 场景 | JS 缺省 | 动态库缺省 | 备注 |
|---|---|---|---|
| 主程序动作 | 5000 ms（`<=0` 按缺省） | 5000 ms（**0 = 不设超时**） | 需要更久自己加大（如解码整曲） |
| 跨插件能力调用 | 3000 ms（`<=0` 按 3000） | 框架不设等待超时（受理失败会立刻返回） | 只有异步形态 |
| 功能点调用 | 60000 ms | 60000 ms | 频谱要解码整曲 |
| 变量读 / 写 | 2 s / 6 s 兜底 | 2 s / 3 s（应用键 5 s） | 超时按"读取超时 / 未回执"结算，缓存不变 |
| 钩子等待 | **没有超时** | **没有超时** | 异步裁决不结算就是调用点一直等 |

### 11.4 插件框架的错误码（动态库插件返回码）

| 码 | 含义 | 常见原因 |
|---|---|---|
| `0` | 成功 | — |
| `-1` | 参数非法 | `data` 不是 JSON 对象、缺 `title`、结构体大小不符 |
| `-2` | 状态错误 | 实例未启动 / 已禁用 / 正在卸载 |
| `-3` | JSON 非法 | 载荷解析失败 |
| `-4` | 未找到 | 注册未知钩子、未知 UI 类型、注销不存在的项 |
| `-5` | 超时 | 动作请求 / 能力调用超时 |
| `-6` | 权限拒绝 | 命名空间非法（冒充他人事件主题 / UI 项 id / 变量键） |
| `-7` | 队列满 | 保留值（当前没有容量类限制） |
| `-99` | 内部异常 | 插件框架内部错误（看日志） |

JS 插件拿到的是**文字**（异常信息 / Promise 拒绝原因）：`action_not_registered: xxx`、
`请求失败：…`、`status=…`、`变量不存在`、`写入被拒绝: …`。

### 11.5 停用 / 卸载时会自动发生什么

插件被禁用或卸载时，插件框架**统一摘除**：

- 全部钩子处理器；全部已声明能力；全部事件订阅；全部 UI 项声明；全部功能点声明与实现；
- **自己起的网络服务与静态路由**（端口立刻释放，旧 URL 立即失效）；
- 定时器（JS）；未完成的动作请求（JS 的 Promise 被拒绝，原因"插件已停止"）；
- 自管的线程 / 定时器 / 临时文件由你在 `stop` 事务里自己回收（动态库插件）。

重新启用 = 重新跑一遍注册：**注册语句要写成幂等的**（重复执行不会累积副作用）。

---

## 12. 常见坑（通用部分）

| 现象 | 原因 / 处理 |
|---|---|
| 动作一直 `action_not_registered` | 动作当前版本没实现（§3.3）；或名字拼错 |
| 钩子不触发 | 该钩子在 `plugin-hooks.md` 里标"未接入"；或插件被禁用；或注册失败（看返回码 / 返回值） |
| 处理器被调用多次 | 用不同 `ownerTag` 注册了同一个钩子（§8.1） |
| 定时器 / 初始化执行两遍 | 每次启用都会重新执行注册（写成幂等） |
| 设置项改完重启又变回默认 | 把 `storage.get` 的"值本身"当成了对象外壳（§3.2.5） |
| 设置项被"改回去" | 读配置是异步的：读回来的旧值后到，覆盖了用户刚改的值（加"已经改过就不再覆盖"的保护） |
| 点入口提示「插件没有提供『xxx』」 | 入口动作指向的能力没注册（页面视图 id 必须与能力短名一致） |
| 页面打开后提示「插件没有提供任何内容块」 | 视图没有 `blocks`，或块类型名全部拼错（未识别的块会被静默忽略） |
| 改了 `config.json` 页面没变 | 页面是打开时取的能力结果：改完再返回一次 `{view:…}`，或重新打开页面 |
| 切歌 / 换背景样式卡几秒 | 在同步裁决的处理器（取色、频谱提取）里 `await` 了主程序动作 → 两边互等（§8.4） |
| 主程序整体卡住 | 处理器里做了阻塞操作 / JS 死循环（框架不打断） |
| 颜色 / 频谱"凭空消失" | 有人返回了 `{"disable": true}`（§8.5）；用 `featurePoint.list` 看是谁 |
| 网络请求拿不到数据 | 看 `status`（非 2xx 也 `ok:true`）与 `truncated`（读满 `maxBytes` 就断开） |
| 生成的界面在别的客户端样式不对 | 按 `host.info().ui` 的 `blocks` / `controls` 降级，不要按 `kind` 写两套 |
| 不确定"现在谁在实现这个功能" | `musicxx.hook.handlers` / `musicxx.feature.point.list` / 命令行 `featurePoint.list` |

---

## 13. 相关文档

- [plugin-guide.md](plugin-guide.md) —— 总入口：选形态、环境、第一个插件、注意事项、调试、发布检查
- [plugin-js-api.md](plugin-js-api.md) —— JS插件：目录与清单、生命周期与硬约束、`musicxx` 全局对象、JS 特有的坑
- [plugin-native-api.md](plugin-native-api.md) —— 动态库插件：清单与多目标打包、SDK、线程、构建、部署、排障
- [plugin-hooks.md](plugin-hooks.md) —— 钩子总表与已接入钩子的载荷 / 裁决（生成物）
- [plugin-vars.md](plugin-vars.md) —— 官方变量目录
- [plugin-ui.md](plugin-ui.md) —— 界面字段、块类型、插件页面与设置页
- [plugin-shader-bundle.md](plugin-shader-bundle.md) —— shader bundle 打包、uniform、参数表达式
- [plugin-agent-cli.md](plugin-agent-cli.md) —— 命令行 / MCP：装插件、刷新、看日志
- [musicxx-interfaces.md](musicxx-interfaces.md) —— musicxx 对外暴露的**全部接口**：MCP 工具全表、命令行命令全表、本机 HTTP 与 WebSocket 路由、权限边界
- `plugins/` —— 官方示例（`example_js` 与 `example_native` 行为等价，可以对照着读）
