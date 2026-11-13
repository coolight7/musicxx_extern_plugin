# 插件接口参考（JS 与动态库插件共用）

> **术语约定**：本文档中的**主程序**指的就是 **musicxx**（Flutter/Dart 业务层）；外置插件的 C++ 框架层称为**插件框架**；插件分为**动态库插件**与**JS插件**。

这篇是**两种插件形态共用的接口手册**：能力（请求 → 结果）、广播（只通知，不回答）、界面入口、日志与自省，
以及命名空间、超时、容量这些两边一样的约定。

**为什么共用**：JS插件与动态库插件能用的接口是**同一批**（同一份能力声明、同一份广播频道、
同一份应用实现的能力表），只是写法不同 —— JS 用 `musicxx.*` 的 Promise 封装，动态库插件用 SDK 的
方法 + 异步完成通知。所以"某个接口做什么、参数是什么、返回什么、什么情况下用、有什么坑"只写在这里，
两篇形态文档只写各自的语言差异：

| 想知道 | 读这篇 |
|---|---|
| 目录结构、清单字段、生命周期、JS 线程约束、`musicxx` 全局对象 | [plugin-js-api.md](plugin-js-api.md) |
| 清单与多目标打包、SDK、CMake、框架线程、构建部署、接口表判定、插件形态（运行器） | [plugin-native-api.md](plugin-native-api.md) |
| 能力 id / 能不能参与 / 参与哪一步 / 载荷与回答 | [plugin-capabilities.md](plugin-capabilities.md)（生成物） |
| 广播频道 id / 有没有当前值 / 谁能发布 / 载荷 | [plugin-broadcasts.md](plugin-broadcasts.md)（生成物） |
| 界面（UI 项字段、插件页面块类型、设置页） | [plugin-ui.md](plugin-ui.md) |
| 播放页背景 / 内联块的着色器 | [plugin-shader-bundle.md](plugin-shader-bundle.md) |
| 装插件、刷新、看日志（命令行 / MCP） | [plugin-agent-cli.md](plugin-agent-cli.md) |

---

## 0. 两件事：能力（请求 → 结果）与广播（只通知，不回答）

插件跟应用之间的交互只有两类。先把这两类分清，后面的各节都是它们的具体形态：

| | 能力 | 广播 |
|---|---|---|
| 形态 | 发一次请求、等一个结果 | 属主改了东西，通知所有关心的人 |
| 有返回值 | 有（可能失败、可能超时） | 没有；不等待、允许丢中间值 |
| 谁发起 | 调用方（插件、应用、命令行） | 属主发布 |
| 典型 | 应用实现的能力 §3、插件服务 §6、功能埋点 §7、应用来问插件 §8 | 带当前值的频道 §2、没有当前值的频道 §4、官方频道 §5 |
| 声明在 | `tools/capabilities.def.json` 的 `capabilities` → [plugin-capabilities.md](plugin-capabilities.md) | 同一个文件的 `broadcasts` → [plugin-broadcasts.md](plugin-broadcasts.md) |

**同一个 id 只有一份声明**，写清"能不能参与、参与哪一步（前链 / 实现链 / 后链）、派发方式、合并策略、
超时、缓存、谁能调"。这套叫法在整条链路上是同一套：Dart 侧是 `MusicxxCapabilityId` / `MusicxxBroadcastId`，
C 侧是 `musicxx_extern_plugin_capability_*` 与 SDK 的 `implement(...)`，作者文档就是这一份生成的
`plugin-capabilities.md` / `plugin-broadcasts.md`。

> 只有这两个概念，没有第三个：能写的东西都是广播，值只能由属主发布；
> 要改应用的状态调能力。

同一件事，两边怎么写（左列是 JS，右列是 C++）：

| 要做的事 | JS插件 | 动态库插件 | 详见 |
|---|---|---|---|
| 实现一条决策型能力（应用来问你） | `musicxx.capability.implement(id, {phase, priority, ownerTag}, fn)` | `implement(id, priority, fn)`（`fn` 里填 `out`） | §8 |
| 订阅一个广播频道（可声明频率） | `musicxx.broadcast.subscribe(id, fn, {intervalMs, mode})` | `subscribeTopic(id, fn, {intervalMs, mode})` | §2 / §4 |
| 注销 / 退订 | `musicxx.capability.unregister(id)` / `musicxx.broadcast.unsubscribe(id)` | `unimplement(id)` / `iface.broadcasts->unsubscribe(...)` | §8 |
| 读一条频道的当前值（`cache: latest` 才有） | `musicxx.broadcast.read("musicxx.state.song")` | `broadcastJson(MUSICXX_BROADCAST_STATE_SONG)` / `broadcasts->read(...)` | §2 / §5 |
| 让应用做一件事 | `await musicxx.capability.call("musicxx.player.toggle")` | `capabilityCall("musicxx.player.toggle", "{}", &notify)` | §3 |
| 发布自己频道的值 | `musicxx.broadcast.publish(topic, data)` | `iface.broadcasts->publish_broadcast(host, channel, json, err)` | §4 |
| 暴露自己的能力 | `musicxx.capability.register("probe", fn)` | `capability(*this, "plugin.<id>.probe", fn)` | §6 |
| 调别的插件的能力 | `await musicxx.capability.call(id, "probe", args)` | `co_await capabilityCall(cap, method, args)` | §6 |
| 声明 / 实现 / 调用功能埋点 | `musicxx.feature.define/provide/callPoint` | `featurePointDefine/Provide/Call` | §7 |
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
| 功能埋点处理器 | **必须同步返回** | **必须同步返回** |
| 读自己的统计 | `musicxx.stats.getSelf()` | 没有便捷接口（看管理页「插件详情 → 运行统计」） |

---

## 1. 先选通道：这件事是能力还是广播

| 你的需要 | 用哪条 | 为什么 |
|---|---|---|
| 跟着应用的通知做事（切歌了、播放状态变了、封面变了） | **广播** §2 / §4 | 单向通知，最轻；`cache: latest` 的频道还能同步读当前值 |
| 想拦一下 / 参与一次调用（跳过本曲、换源、停止播放、改歌词加载、改频谱数据） | **能力 · 应用来问插件** §8 | 唯一能"被应用调用"的通道；能拿到载荷、能回答 |
| 高频读实时数据（播放状态、当前歌曲、频谱槽位状态） | **广播 · `cache: latest` 的频道** §2 | 同步读，不用往返 |
| 一次性命令（播放控制 / 列歌单 / 发通知 / 开页面 / 写插件自己的数据） | **能力 · 应用实现的** §3 | 有返回值、有错误码，由应用执行 |
| 一件"已经发生了"的事（没有当前值） | **广播 · `cache: none` 的频道** §4 | 单向通知，最轻 |
| 改应用的状态 | **能力 · 应用实现的** §3 | 有返回值、有错误码；广播是只读的，改完应用自己会发广播 |
| 把自己的功能给别的插件用 | **能力 · 插件服务** §6 | 有返回值、参数与结果由你自己定 |
| 让"这个功能谁都能实现"（应用或第三方按 id 来调） | **能力 · 功能埋点** §7 | 可以有多个实现，按优先级问 |

> 记法：**能力 = 问一件事、等一个结果**（应用实现的是让应用做，插件服务是给别的插件用，
> 功能埋点是按 id 问，§8 是应用来问你）；**广播 = 告诉一声**（`cache: latest` 的带着当前值，
> `cache: none` 的只有这一次通知）。广播**只能读与订阅**：要改应用的状态调能力，插件自己的状态自己存。
>
> **JS 侧的入口就两个**：**能力** 全部走 `musicxx.capability`（实现 / 注册 / 调用 / 自省），
> **广播** 全部走 `musicxx.broadcast`（读当前值 / 订阅 / 发布自己频道的值 / 声明自己的频道）。
> 一个频道有没有当前值由声明里的 `cache` 决定，不需要再记别的分类。

---

## 2. 状态广播（`cache: latest`）：同步读应用现状

应用把关键状态发布到 `musicxx.state.*` 这些 `cache: latest` 的广播频道：插件订阅即拿到当前值，
也能同步读缓存；**频道 id 与载荷稳定**（声明源是 `tools/capabilities.def.json`）。

插件框架把这些值做成 JSON，插件**同步**读：不用往返、不占线程、不怕应用忙。

```js
// JS：读一次（没有推送过的键返回 null）
const song = musicxx.broadcast.read("musicxx.state.song");
const playing = (musicxx.broadcast.read("musicxx.state.env") || {}).isPlaying === true;
```

```cpp
// C++：读一次（没有推送过的键返回空串）
const std::string songJson = broadcastJson(MUSICXX_BROADCAST_STATE_SONG);
const utilxx_base::Json env =
    utilxx_base::Json::parse(broadcastJson(MUSICXX_BROADCAST_STATE_ENV));
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
| `musicxx.state.lyric` / `musicxx.state.playlists` | 预留 | **当前版本没有推送**（读到的永远是空） |

只出现一次 `musicxx.state.app` → `musicxx.state.env` 的字段含义：

- `musicxx.state.app.version` 是整型版本号（与 `versionStr` 是同一件事的两种写法）；
  `installId` 是安装级随机 id（可当匿名标识用）；`branch` 是渠道名；`isNight` 见下方"坑"。
- `musicxx.state.app.foreground` 是"现在是否前台"（布尔），`lifecycleState` 是原始生命周期状态名
  （`resumed` / `inactive` / `hidden` / `paused` / `detached`）。进后台 / 回前台的**通知**用广播频道
  `musicxx.app.background` / `musicxx.app.foreground`（后者带 `backgroundMs`）；要订阅变化用广播
  `musicxx.app.lifecycle`（`cache: latest`，值是 `foreground` / `background`）。
- `musicxx.state.player.quality` 是音质枚举名（`Undefined` / `Lower` / `Standard` / `HQ` / `SQ` / `Hifi` / `Upper`），
  `mediaType` 是媒体类型枚举名；`cacheLength` 是已缓存时长（毫秒）；`pitch` 是当前音调（1 = 原调）。
  这几个值都是“变化时才刷新”的：要“变的时候拦一下”用能力 `musicxx.player.volume` /
  `musicxx.player.speed` / `musicxx.player.pitch`，要当场读当前进度用广播 `musicxx.player.position`。
- `musicxx.state.playlist.loopMode` 是循环模式枚举名（`Order` / `Only` / `RealRand` / `SimulateRand` / `SimulateRandReflush`），
  `autoPlayMode` 是 `auto` 或 `off`；`index` 是当前曲目在歌单里的下标。
- `musicxx.state.env.page` 是当前路由名（字符串，可用于"现在在哪个页面"的判断）。

### 2.2 歌曲视图长什么样

`musicxx.state.song` 与能力载荷里的 `song` 是**同一份结构**（没有歌曲时是空对象）：

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

**载荷与这些频道里永远没有音频直链和 token**：`src` 正文不给，只给来源类型与稳定 key（`srcKey` 是音源身份，
同一首歌在不同设备上也一样，适合做缓存键）。要地址就自己走 `musicxx.net.*` 能力或让应用做事。

### 2.3 订阅状态变化

每条状态广播都带当前值：订阅哪条就收到哪条的更新（订阅时还会先推一次当前值）。两边订阅方式一样（见 §4）：

```js
musicxx.broadcast.subscribe("musicxx.state.song", (song) => {
    refreshMyPanel(song);
});
```

```cpp
subscribeTopic(MUSICXX_BROADCAST_STATE_SONG, [](std::string_view json) {
    // json = 当前歌曲的视图
});
```

### 2.4 坑

- **`musicxx.state.app.isNight` 是启动时的值**：运行期切了主题它不会跟着变。要"当前值"就当场问一次：
  能力 `musicxx.media.palette` 或 `musicxx.render.current` 的 `night`；
  `musicxx.state.renderSlots` 里条目的 `night` 也只在槽位状态变化时刷新，同样不能当当前值用。
- **播放进度要自己拿**：`musicxx.state.player.position` 只在启动、切歌、播放状态变化时刷新（**不是实时值**）。
  要按秒推进度：订阅广播频道 `musicxx.player.position`（不限频，带 `sid` / `positionMs` / `durationMs`，
  订阅时声明 `intervalMs`），或当场 `read` 它。不要以为读这个频道就能拿到实时进度。
- **要"是否在播放"读 `musicxx.state.env.isPlaying`**（布尔），不要靠 `player.state` 的字符串判断
  （它是"播放/暂停/停止事件"与枚举名的混合值，见 §3.2.1 的说明）。
- **键名是固定的官方键**，写错就是读不到（`get` 返回 `null` / `broadcastJson` 返回空串），不会报错。
- 这些频道里**不放**临时直链、token、插件自己的数据。插件自己的状态自己存（私有存储或自己命名空间下的频道）。

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
JS 插件用能力 `musicxx.host.getPath` 拿插件目录、数据目录与日志目录。

---

## 3. 应用实现的能力（能力：让应用做一件事）

> **应用实现的能力也属于能力**：应用执行、宿主转发并带回结果，不是第二个概念。
> JS 用 `musicxx.capability.call`，原生插件用 SDK 的 `capabilityCall`。

> **能力就是能力**：每个应用实现的能力在 `musicxx.capabilities` 里都有一条应用声明。
> **没声明的名字会被直接拒绝**（不再“什么都转给应用再回未找到”）。
> 超时语义不变：`<= 0` = 不设超时（一直等应用回复），缺省 5000 ms。

### 3.1 通用约定

```js
const result = await musicxx.capability.call("musicxx.<域>.<名字>", { 参数 }, 超时毫秒?);
// 也有分类便捷封装（都等价于上面这一行）：
await musicxx.player.toggle();
await musicxx.storage.set("k", 1);
```

```cpp
// 动态库插件：只有异步形态，结果经完成通知回来（恰好一次）
static void onCapabilityDone(void* ud, int32_t status, const PluginxxStringView* json) {
    // status == 0 → json 是结果；否则是错误（json 里通常有 {"ok":false,"error":"..."}）
}
PluginxxOperatorNotify notify{&onCapabilityDone, this};
capabilityCall("musicxx.player.toggle", "{}", &notify, 5000);
```

规则（两边一样）：

- **超时**：JS 的 `musicxx.capability.call(name, args, timeoutMs)` 缺省 **5000 ms**，`timeoutMs <= 0` 时按缺省值处理（不会"一直等"）；
  动态库的 `capabilityCall(..., timeoutMs)` 缺省 **5000 ms**，**传 0 = 不设超时**（框架不建定时器，一直等主程序回复）；
  到点后框架终结这次请求并推 `musicxx.capability.app.cancel` 让应用收尾。
  个别能力需要更长时间（如 `musicxx.feature.call` 要解码整曲），调用方自己加大。
- **返回**：成功是 `{ok:true, ...}` 或能力自己的结果；失败是 `{ok:false, error:"说明"}`；
  没注册会立刻失败（`app_capability_not_registered: <能力 id>`），不会挂到超时。
- **不做权限校验**：清单里的 `permissions` 只做展示，不会因为没声明就拒绝调用（见 [plugin-native-api.md](plugin-native-api.md) §8）。
- **名字不校验**：插件框架受理任意名字的调用，由应用按"注册了没有"决定理不理它。
  官方能力是 `musicxx.<域>.<名字>`；插件自己的能力建议写成 `plugin.<包名>.*` 以免撞名。
- **Result 里可能出现 `ok:false` 但调用成功**两种情况要分清：
  - 调用失败（能力没注册 / 处理器抛异常）→ JS 的 Promise **拒绝**（`err.message` 带原因）；C++ 的 `status != 0`；
  - 调用执行了但条件不满足（参数错、找不到歌单）→ Promise 成功、结果里 `{ok:false, error:"..."}`。

### 3.2 全部已实现的能力

下面每一节都是"当前版本注册过、调用能得到结果"的能力。**没有列在这里的 `musicxx.*` 名字都还没实现**
（调用会得到 `action_not_registered`，见 §3.4）。

#### 3.2.1 播放控制（`musicxx.player.*`）

| 能力 | 参数 | 返回 | 说明 |
|---|---|---|---|
| `musicxx.player.play` | 无 | `{ok:true}` | 开始播放 |
| `musicxx.player.pause` | 无 | `{ok:true}` | 暂停 |
| `musicxx.player.toggle` | 无 | `{ok:true}` | 播放/暂停切换（与播放页的播放按钮同一行为：歌曲快结束时按播放会按设置走下一曲） |
| `musicxx.player.stop` | 无 | `{ok:true}` | 停止 |
| `musicxx.player.next` | 无 | `{ok:true}` | 下一曲（走应用的切歌流程） |
| `musicxx.player.prev` | 无 | `{ok:true}` | 上一曲 |
| `musicxx.player.playSrc` | `{src}`（单个音源）或 `{sources}`（数组）；`{type}`：`auto`（缺省）/ 来源类型名（`bili` / `link` / `local` / `webdav` / `subsonic` …）/ 插件音源类型 id；`{filterExt}`：只接受扫描配置里允许的媒体后缀（缺省 false）；`{name}` 显示名（只在给一个来源时用） | `{ok:true, count, single, name, sid, artist, sources, skipped?}` | 按来源播放：本地路径 / `content://` / 文件链接 / 拟声音源字符串 / 插件音源。**和 applink（`musicxx://play/`）、启动参数、命令行、MCP 是同一个入口、同一套参数名**；一个来源追加到当前播放列表并播，多个来源一次列成临时歌单。`src` 与 `sources` 都没给 → `{ok:false,error:"缺少参数 src（或 sources）"}`；全部被跳过 → `{ok:false,error:"没有可播放的音源（被跳过的：…）"}` |
| `musicxx.player.seek` | `{positionMs}` 或 `{position}`（毫秒），或 `{percent}`（0~1） | `{ok:true}` | 都缺 → `{ok:false,error:"缺少参数 positionMs"}` |
| `musicxx.player.setVolume` | `{volume}`（0~1；> 1 视为百分比，自动除以 100，最后钳到 0~1） | `{ok:true}` | 缺参数 → `{ok:false,...}` |
| `musicxx.player.setSpeed` | `{speed}`（倍速） | `{ok:true}` | |
| `musicxx.player.setPitch` | `{pitch}`（音调；1 = 原调，应用夹到 0.25~2） | `{ok:true}` | 缺参数 → `{ok:false,...}`；不改变播放速度 |
| `musicxx.player.setQuality` | `{quality}`：`Undefined`/`Lower`/`Standard`/`HQ`/`SQ`/`Hifi`/`Upper`（忽略大小写） | `{ok:true}` | 未知取值 → `{ok:false,error:"未知音质：x"}`；等价于用户在设置里改"音质" |
| `musicxx.player.setLoopMode` | `{mode}`：`Order`/`Only`/`RealRand`/`SimulateRand`/`SimulateRandReflush` | `{ok:true}` | 未知取值 → `{ok:false,error:"未知播放模式：x"}` |

```js
await musicxx.player.setVolume(0.6);       // 便捷封装：setVolume(60) 会当作 60%
await musicxx.player.seek(64000);          // 数字 = 毫秒
await musicxx.capability.call("musicxx.player.seek", { percent: 0.5 });
await musicxx.player.setSpeed(1.25);
await musicxx.player.setPitch(1.1);        // 音调偏高：速度不变
const mode = (musicxx.broadcast.read("musicxx.state.playlist") || {}).loopMode;   // 读当前值
```

```cpp
capabilityCall("musicxx.player.setLoopMode", R"({"mode":"Only"})", &notify, 5000);
capabilityCall("musicxx.player.seek", R"({"positionMs":64000})", &notify, 5000);
```

- 这些能力都是"替用户按一下按钮"：会走应用既有的流程（例如 `stop` 会做它平时的收尾），不要绕过它们直接改状态。
- **这四个值都有“要变之前拦一下”的能力**：`musicxx.player.seek` / `musicxx.player.volume` /
  `musicxx.player.speed` / `musicxx.player.pitch`（见 §8 与 [plugin-capabilities.md](plugin-capabilities.md)）：
  插件通过能力改值时也会先问这条能力，所以它的处理器不能反过来再调这些能力（会再派发一次）。
- 没有 `setMediaType`（见 §3.4）。

#### 3.2.2 歌单（`musicxx.playlist.*`）与播放列表（`musicxx.songlist.*`）

三个词在这份文档里含义固定，不再混用；能力名的前缀就是它们，从名字就能看出在操作哪一个：

| 词 | 含义 | 出现在哪里 |
|---|---|---|
| `playlist` | **单张歌单**（`pid` / `name` / `songs` / `songNum`），保存下来的列表 | `musicxx.playlist.*` |
| `playlists` | **多张歌单的集合**（我的 / 我喜欢 / 本地） | `musicxx.playlist.queryLists` 的 `mine` / `local` / `favorite` |
| `songlist` | **播放列表**（当前正在播的那一串歌，不单独保存） | `musicxx.songlist.*`、广播 `musicxx.state.playlist` |

**`musicxx.playlist.queryLists`** —— 列出歌单

- 参数：无。
- 返回：`{mine:[歌单], favorite:[歌单], local:[歌单], currentPid, currentName}`；
  歌单项是 `{pid, name, count, songNum, listen, type}`（`count` = 已加载的曲目数，`songNum` = 歌单声明的曲目数，
  `type` 是歌单类型枚举名）。
- `favorite` 是"我喜欢"（一直在最前），`mine` 是账号歌单，`local` 是本地歌单。
- `currentPid` / `currentName` 是**当前播放列表**的 pid 与名字：接着读它的歌用 `musicxx.songlist.querySongs`。

**`musicxx.playlist.querySongs`** —— 读一张歌单里的歌

- 参数：`{pid, type?, limit?=200, offset?=0, detail?}`。
  - `pid` 给的是当前播放列表的 `pid`（或不给）时，读的是当前播放列表 —— 老行为，留着不为难老插件；
    新代码要读当前播放列表请用 `musicxx.songlist.querySongs`；
  - 给 `type`（`local` = 本地歌单，其它值按在线歌单）可以在歌单还没进当前账号的歌单表时按 `pid` 直接查；
  - `detail: true` 时每首歌给完整字段（在简报字段基础上多 `type` / `src` / `fileName` / `size` /
    `track` / `year` / `genre` / `cover` / `createdTime` / `lastedTime`），改歌信息前先读一遍用；
- 返回：`{pid, total, songs:[{sid, name, artist, album, durationMs}]}`，读当前播放列表时还会带 `available`。
- 歌单加载失败 → `{ok:false, error:"歌单无法加载：<pid>"}`。

**`musicxx.songlist.querySongs`** —— 读当前播放列表里的歌

- 参数：`{limit?=200, offset?=0}`；参数含义与返回同上，另外带 `available`（当前播放列表还能不能用）。

**`musicxx.songlist.playSong`** —— 播当前播放列表里的某一首

- 参数：`{sid}`；**这首歌必须在当前播放列表里**，否则 `{ok:false, error:"当前播放列表里没有这首歌：<sid>"}`。
- 返回：`{ok:true}`。

**`musicxx.playlist.play`** —— 播放一张歌单

- 参数：`{pid, index?}`（`index` 越界或缺失按 0 处理）；歌单为空 → `{ok:false, error:"歌单为空或无法加载：<pid>"}`。
- 返回：`{ok:true}`。

```js
const lists = await musicxx.playlist.queryLists();
await musicxx.playlist.play({ pid: lists.local[0].pid, index: 3 });

const page = await musicxx.playlist.querySongs({ pid: lists.local[0].pid, limit: 50, offset: 100 });
const queue = await musicxx.songlist.querySongs();     // 当前播放列表
```

```cpp
// 想先拿歌单再决定播哪一个：在完成通知里读结果，再再调一次能力
capabilityCall("musicxx.playlist.queryLists", "{}", &notify, 5000);
```

**这首歌的完整音源列表在广播 `musicxx.state.song` / 能力载荷里**（`musicxx.state.song.audio[]`），
`querySongs` 只给列表用的简报字段（省流量）。

> **过时的分类 `musicxx.library.*`**（旧文档与老插件还在用）：名字保留可用，主程序把它们注册成同一份实现，
> 新代码不要再写 —— 一个前缀里混着"歌单"与"播放列表"两个概念。对照：
>
> | 过时 | 现在用 |
> |---|---|
> | `musicxx.library.querySonglists` | `musicxx.playlist.queryLists` |
> | `musicxx.library.querySongs` | `musicxx.playlist.querySongs`（带 `pid`）/ `musicxx.songlist.querySongs`（当前播放列表） |
> | `musicxx.library.playSong` | `musicxx.songlist.playSong` |
> | `musicxx.library.playSonglist` | `musicxx.playlist.play` |
>
> 从未实现的 `querySonglist` / `removeSong` / `createSonglist` / `setSongInfo` 现在都有了实现的新名字
> （`musicxx.playlist.querySongs` / `removeSong` / `create` / `updateSong`，见下面几节）；
> 旧名字本身仍没注册，也没有别名。`musicxx.library.search`（站内搜索）没有替代名字，仍未实现。

**`musicxx.playlist.addSong`** —— 把歌加进**一张歌单**（插件音源用）

- 参数：

```jsonc
{
  "pid": 123,                 // 必填：目标歌单（本地歌单 / 自己的在线歌单 / “我喜欢”都行）
  "dedup": true,              // 可选，缺省 true：同一首歌已经在歌单里就跳过
  "songs": [                  // 首数不限（早期版本的 200 首上限已移除）；只有一首时也可以写 "song": {...}
    {
      "type": "plugin.example_ftp_source.audio",  // 必填：必须是调用方自己的音源类型
      "src": "3f9c1d7b/Music/a.flac",              // 必填：插件自己的资源 id
      "name": "歌名",            // 可选：空则按 fileName 交给主程序识别
      "artist": "歌手",          // 可选
      "fileName": "a.flac",      // 可选：主程序用它取文件名 / 识别歌名与歌手
      "size": 12345678,          // 可选：字节
      "durationMs": 0,           // 可选：毫秒
      "cover": {"type": "url", "src": "https://…"}    // 可选：本地路径或 http(s) 地址
    }
  ]
}
```

- 返回：`{ok:true, pid, name, total, added, skipped}`（`total` = 加完之后的曲目数，
  `added` = 真的加进去的，`skipped` = 被去重跳过的）；失败是 `{ok:false, error}`：
  未找到歌单 / 歌单无法加载 / 不属于自己的歌单 / 音源类型不属于本插件 / 保存失败（已回滚）。
  **有一首不合法就整批失败**（不会只加进去一部分），`error` 里会列出前几首的原因。
- 规则：
  - `type` 必须以 `plugin.<自己的插件id>.` 开头（防止插件伪造别人的音源）；
    主程序本机调用（CLI / MCP）时只要求 `plugin.` 前缀；
  - 加歌方式与内置“添加歌曲”一致：**往前插**（当前播放的歌不会因此换掉），写播放记录标签；
  - 加进的就是当前播放队列时，队列会重新刷新（列表 / 页面跟着变）。

```js
// 加完再看结果：处理器里不要 await（会与主程序互相等）
musicxx.capability.call("musicxx.playlist.addSong", { pid: 123, songs: songs }).then((r) => {
  musicxx.host.log(2, r && r.ok ? ("已添加 " + r.added + " 首") : ("添加失败: " + ((r && r.error) || "")));
});
```

> 插件想让用户“选目录 / 选文件 → 加进歌单”，可以在「添加歌曲」弹窗里注册一个
> `musicxx.ui.addsong.entry` 项（见 §9.1 与 [plugin-ui.md](plugin-ui.md)），
> 点击后由主程序打开你自己的挑歌界面（`musicxx.ui.openOverlaySheet`），
> 选完后调这条能力。

**`musicxx.playlist.removeSong`** —— 从一张歌单里移除歌曲

- 参数：`{pid, sid}` 或 `{pid, sids:[…]}`。
- 返回：`{ok:true, pid, name, total, removed, removedSids, notFound}`；`notFound` 是歌单里没找到的那几个
  sid，不算失败（一首都没删到也是 `ok:true` + `removed:0`）。
- 失败：`{ok:false, error}` —— 未找到歌单 / 无法加载 / 不是自己的歌单 / 保存失败（那时已把歌退回原位）。
- **只改保存下来的歌单**：目标是当前播放队列时，不会去改正在播的那一串；要改队列用
  `musicxx.songlist.removeSong`。

**`musicxx.playlist.create`** —— 新建一张歌单

- 参数：`{name, type?, songs?, dedup?}`；`name` 是 1 ~ 50 个字符，`type` 是 `local`（缺省）或
  `net`（在线歌单，需要先登录），`songs` 的写法与 `musicxx.playlist.addSong` 完全一样（建完顺手加进去）。
- 返回：`{ok:true, pid, name, type, total}`（`type` 是 `local` / `net`）。
- 失败：`{ok:false, error}`。歌单建好了但加歌失败时，返回里仍带 `pid` / `name`，`ok` 为 false。

**`musicxx.playlist.updateSong`** —— 改一张歌单里某首歌的信息

- 参数：`{pid, sid, …要改的字段}`，没给的字段保持原值：

| 字段 | 说明 |
|---|---|
| `name` | 歌名（1 ~ 1000 个字符） |
| `artist` | 歌手（0 ~ 1000 个字符，空串 = 清空） |
| `album` / `genre` | 专辑 / 分类（空串 = 清空） |
| `track` / `year` | 音轨号 / 年份 |
| `durationMs` | 时长（毫秒，不能是负数） |
| `src` | 音源地址 / 资源 id（不能为空） |
| `type` | 音源类型：`plugin.<自己的插件id>.…` 或 `XXSrcType_e` 的枚举名（`Local` / `UrlLink` …） |
| `cover` | 封面，写法与 `addSong` 的 `cover` 一样；改封面会清掉旧缩略图 |

- `sid` 不能改（它是定位用的）。改完会自动记一条修改时间。
- 返回：`{ok:true, pid, sid, song:{…}}`（`song` 与 `querySongs` 的 `detail: true` 是同一套字段）。
- 失败：`{ok:false, error}`；保存失败会把这首歌恢复原样。

**`musicxx.playlist.song.favorite`** —— 收藏 / 取消收藏当前歌曲

与播放页的收藏按钮、锁屏音乐的收藏是同一份实现。

- 参数：`{favorite?}`（布尔）。不写 = 按当前状态取反；写 `true` / `false` = 设成那个状态
  （已经是目标状态时不再发请求）。文本写法 `"true"` / `"1"` 也算真。
- 返回：`{ok:true, favorite, srcKey}`——`favorite` 是动作之后当前歌曲在「我喜欢」里的状态，
  `srcKey` 是当前歌曲的音源身份（完整 md5 串）。
- 未登录时不会收藏（应用自己提示登录），返回的 `favorite` 仍是 `false`。
- 当前播放列表就是「我喜欢」时，队列里的这一首也会同步增删。
- 当前没有歌曲 / 歌曲没有音源身份：`{ok:false, error:"当前没有可收藏的歌曲"}`。

**`musicxx.playlist.syncSongs.sync`** —— 同步歌单音源（把音源里新增的歌加进歌单）

歌单可以绑定“音源”（本地文件夹 / 歌手 / 专辑、Bili 收藏夹、WebDAV、云盘、
Subsonic / Jellyfin / Emby 视图等），应用里「歌单右上角三个点 - 同步歌单音源」会把这些来源
重新读一遍，把新增的歌加进歌单。这条能力就是那件事的非界面入口。

- 参数：`{pid}`（必填：目标歌单，本地歌单 / 自己的在线歌单 / “我喜欢”）。
- 返回：

```jsonc
{
  "ok": true,
  "pid": 123,
  "name": "歌单名",
  "total": 20,            // 同步之后歌单里的曲目数
  "added": 3,             // 这次新加进去的歌
  "cancelled": false,     // 界面上点了「终止」才会是 true
  "failedSources": 1,     // 读取失败的音源条数（部分失败仍然 ok:true）
  "elapsedMs": 1234,
  "sources": [            // 每一条音源一条
    {"index": 0, "id": 2, "type": "本地歌曲-文件夹", "name": "『1』音乐",
     "loaded": 8, "added": 3},
    {"index": 1, "id": 20, "type": "WebDAV-文件夹", "name": "『1』远端",
     "loaded": null, "added": 0, "error": "读取音源失败"}
  ]
}
```

- `loaded` 是这条音源读到多少首（`null` = 读取失败），`added` 是这一条新加进歌单多少首；
  已经在歌单里的歌按音源身份去重跳过。
- 失败：`{ok:false, error}` —— 未找到歌单 / 歌单无法加载 / 没有修改权限 / 没有绑定音源 /
  每条音源都读取失败 / 保存失败（已回滚）/ 这个歌单正在同步。
  前面三种（未找到 / 无法加载 / 没权限）与“这个歌单正在同步”属于**没开始同步**，不会往信息栏放提示；
  已经开始读写的失败才会留下提示。
- **只有一部分音源读取失败时仍然是成功**（`failedSources` 与 `sources[].error` 照报），
  免得一条坏音源把整次同步判死。
- 歌单绑定了哪些音源只能在应用界面里配，这条能力不收 `sources`；它只按已经绑好的音源同步。
- 新歌**往前插**（与「加歌」一致），保存后写歌单关联；目标就是当前播放列表时会刷新队列。
- **应用的信息栏会显示一条提示**：不是用户从界面点「开始同步音源」触发的（插件 / MCP / 命令行），
  用户就不知道有人在同步 —— 因此同步期间显示进度，**成功后 30 秒自动移除，失败则保留**
  （点开能看到每一条音源的结果）。同步任务本身在应用里跑，调用方超时放弃也不影响它跑完。

```js
// 插件里：先把歌单列表读出来，再同步用户选中的那一张
// 同步可能要几十秒，而 musicxx.capability.call 缺省 5 秒就超时 —— 要结果就把超时给大一点
const r = await musicxx.capability.call("musicxx.playlist.syncSongs.sync", { pid: 123 }, 120000);
if (r && r.ok) musicxx.host.log(2, "新增 " + r.added + " 首");
```

> 调用方超时也不影响同步继续跑（它在应用里自己跑完，信息栏里能看到结果）；
> 插件里不需要结果时不要 `await`，直接在完成通知里写日志。

**`musicxx.songlist.addSong`** —— 往当前播放列表里加歌

- 参数：`{songs | song, dedup?, afterSid?}`，歌曲格式与 `musicxx.playlist.addSong` 一样
  （`type` 的命名空间规则也一样）；`afterSid` 插到那一首之后，不给就插到最前（正在播的歌不会因此换掉）。
- 返回：`{ok:true, pid, name, total, added, skipped}`。
- 只改队列：**不写进任何歌单**，也不写播放记录标签（真播到它时才算一次播放）。

**`musicxx.songlist.removeSong`** —— 从当前播放列表里移除歌曲

- 参数：`{sid}` 或 `{sids:[…]}`。
- 返回：`{ok:true, total, removed, removedSids, notFound}`。
- 移除正在播的那一首**不会停**（与界面上「从播放列表移除」一致，这一首照常播完，再按新队列往下走）。

**`musicxx.songlist.updateSong`** —— 改当前播放列表里某首歌的信息

- 参数：`{sid, …要改的字段}`，字段与 `musicxx.playlist.updateSong` 完全一样，只是不需要 `pid`。
- 返回：`{ok:true, sid, song:{…}}`。
- 正在播的那一首改完会立即刷新播放信息（名字 / 歌手 / 封面），不用重新播。

> 写类的四条共同规则（上面几条能力与 `syncSongs.sync` 都适用）：
> 1. **能改哪些歌单**：本地歌单、自己创建的在线歌单、“我喜欢”；别人的歌单回
>    `{ok:false, error:"没有修改这个歌单的权限：<pid>"}`；
> 2. **保存失败会回滚**：内存里的歌单与存盘内容不会不一致；
> 3. **歌单 ≠ 队列**：`musicxx.playlist.*` 改的是保存下来的歌单（下次打开才看得到变化），
>    `musicxx.songlist.*` 改的是正在播的那一串（立即生效，不写盘）；
> 4. **没开始就失败的会直接回错误**（未找到 / 没权限 / 正在同步），不会留下信息栏提示。
>
> 改歌信息前建议先 `querySongs` + `detail: true` 读一遍，再把要改的字段回传。

#### 3.2.3 歌词（`musicxx.lyric.*`）

歌词能力统一在单数域 `musicxx.lyric.*`（与能力 / 广播频道一致）。旧的复数域
`musicxx.lyrics.*` 已删除，**不保留别名**，调用会得到 `action_not_registered`。

> 类似名称的广播频道 `musicxx.lyric.current`："当前歌词行变了"的通知。

**`musicxx.lyric.current.get`** —— 当前歌曲的歌词信息和内容

- 参数：`{limit?, word?}`。`limit` 是折叠后的行数上限（缺省 0 = 全部），`word: true` 时每行附带逐字时间。
- 返回：
  `{ok, hasLyric, sid, songName, songArtist, lrcid, name, artist, sourceType, timeType, lineCount,
  index, rawIndex, offsetMs, songOffsetMs, globalOffsetMs, hasTranslate, showTranslate, lines}`。
  - `lrcid` 为 `-1` 表示当前歌词不是拟声云歌词（本地 / 内置 / 临时内容）；
  - `name` / `artist` 是**歌词自身**的名称与歌手，当前歌曲的信息在 `songName` / `songArtist`；
  - `index` 是当前行在 `lines` 里的下标（`rawIndex` 是应用内部的原始行号）；
  - `offsetMs` 是实际生效的偏移（全局 + 这首歌的绑定），`songOffsetMs` / `globalOffsetMs` 拆开给；
  - `lines` 里**一句 = 一行原文 + 同组的全部译文行**：译文放在 `translate` 数组里，可以有多行；
    `timeMs` 是**毫秒**，没有时间戳的行为 `null`；`word` 是 `[{timeMs, index}]`（字符下标），
    只给**真的带逐字时间戳**的行（按比例模拟出来的逐字时间不算，免得给出并不存在的精细度）。
- 没有歌词时 `hasLyric:false`、`lrcid:-1`、`lineCount:0`、`lines:[]`，不是错误。

```js
const lrc = await musicxx.lyric.current.get({ limit: 80 });
const line = lrc.lines[Math.min(lrc.index, lrc.lines.length - 1)];
```

**`musicxx.lyric.current.update`** —— 改当前歌曲的拟声云歌词与这首歌的歌词时间偏移

- 参数：`{lrcid?, offsetMs?}`，两者至少给一个。
  - `lrcid`：拟声云歌词 id（必须 > 0），只能是**拟声云歌词** —— 一般来自
    `musicxx.lyric.search` 的结果或 `current.get` 的 `lrcid`；读不到就不改任何东西。
  - `offsetMs`：这首歌的歌词偏移（毫秒，**正 = 歌词提前、负 = 延后**，限制在 ±600000）。
- 只改当前正在播放的这一首：换云歌词时按界面「选择歌词」的同一份实现落绑定
  （当前歌词、`songLrcMap`、当前歌曲的 `lyric` 字段、歌单保存），并保留 / 更新偏移；
  **偏移的保留规则**：换到**另一条**云歌词又没给 `offsetMs` 时偏移归零（与界面「选择歌词」一致）；
  换的是同一条、或只给 `offsetMs` 时保留原偏移。只给 `offsetMs` 时，当前歌词必须是拟声云歌词。
- 返回：`{ok, sid, lrcid, songOffsetMs, offsetMs, sourceType, lineCount, persisted}`。
  `persisted:false` = 没有可保存的歌单（临时播放列表），内存里的绑定仍然生效。

```js
// 从搜索结果里挑一首用上，并把歌词提前 0.5 秒
await musicxx.lyric.current.update({ lrcid: 12345, offsetMs: 500 });
```

**`musicxx.lyric.current.updateContent`** —— 用 LRC 文本设置当前歌词（只管显示）

- 参数：`{lrc}`（标准或增强型 LRC 文本，最长 200000 个字符）。
- **不写歌词缓存、不上传、不改歌曲绑定的歌词、不改来源类型**；切歌或重新加载歌词后恢复成
  绑定来源的歌词。适合"自己算了一份歌词先看看效果"这类场景。
- 解析不出歌词行（空文本 / 只有占位行）时返回 `{ok:false, error}`。
- 返回：`{ok, sid, lineCount, timeType, hasTranslate}`。

```js
await musicxx.lyric.current.updateContent("[00:01.00]第一行\n[00:02.50]第二行");
```

**`musicxx.lyric.getBySrc`** —— 按歌词源读取歌词内容（不要求是当前歌曲）

- 参数：`{lrcid}`，或 `{type, src}`：
  - `myid`：`src` 是拟声云歌词 id（等价于给 `lrcid`）；
  - `local`：`src` 是本地 `.lrc` 文件路径；
  - `link`：`src` 是 http(s) 链接。
- 另可给 `refresh: true`（云歌词跳过内容缓存重新读取）、`limit`、`word`。
- 返回与 `current.get` 同一套行格式：`{ok, found, lrcid, sourceType, name, artist, timeType,
  lineCount, lines}`；`found:false` 表示没读到（不是错误）。`name` / `artist` 优先取歌词库里
  缓存的元信息，其次取 LRC 标签。
- 其它来源类型（云盘 / 媒体库）需要各自的额外参数，这里不支持，会返回可读错误。

**`musicxx.lyric.search`** —— 搜索拟声云歌词

- 参数：`{keyword}`（也接受 `info` / `q`），或 `{name, artist}`（按应用的识别规则生成搜索串）；
  `limit` 缺省 20、最大 50（超出按 50，`<= 0` 按 20）；关键词超过 100 个字符会被截断（与界面搜索框一致）。
- 返回：`{ok, keyword, count, lyrics:[{lrcid, name, artist, depict, timeType, timeTypeName, love,
  used}]}`。
- 搜索结果**只有元信息**（搜索接口本身不返回歌词内容）；要内容用 `musicxx.lyric.getBySrc`
  或直接 `musicxx.lyric.current.update` 选上它。
- 空关键词返回 `{ok:false, error}`；网络失败返回 `{ok:false, error:"搜索失败"}`。

```js
const found = await musicxx.lyric.search({ name: "晴天", artist: "周杰伦" });
if (found.count > 0) {
  const content = await musicxx.lyric.getBySrc({ lrcid: found.lyrics[0].lrcid });
}
```

#### 3.2.4 提示、弹窗与页面跳转（`musicxx.ui.*`）

| 能力 | 参数 | 返回 | 说明 |
|---|---|---|---|
| `musicxx.ui.toast` | `{text}`；`{kind:"error"}` 走错误样式 | `{ok:true}` | 轻提示，**固定记一条插件日志**（原来的 `musicxx.ui.notify` 已合并到这里） |
| `musicxx.ui.dialog` | `{title?, content?, blocks?, textConfirm?, textCancel?}` | `{ok:true, confirmed:bool}` | 需要用户确认的操作。`content` 是纯文本，`blocks` 是富内容（页面块数组；两个都给时文本在上）。标题不加插件前缀（面向普通用户）；取消 / 关闭一律回 `confirmed:false` |
| `musicxx.ui.overlay.show` | `{preset?, id?, title?, depict?, blocks?, options?, value?, textConfirm?, textCancel?, size?, dismissible?}` | `{ok, reason, value?}` / `{ok, selected}` / `{ok, cancelled:true}` | 打开一层浮层（sheet），**等用户关掉它**。参数与返回见 §3.2.4.1 |
| `musicxx.ui.overlay.close` | `{id?}` | `{ok:true, closed:bool}` | 关这个插件**最上面那一层**浮层（给了 `id` 就关最上面那个名字对得上的层；没有开着的浮层时 `closed:false`） |
| `musicxx.ui.openOverlaySheet` | `{route, title?, arguments?}` | `{ok, reason, title?}` | 把**插件视图**打开成一层浮层，等用户关掉它。与 `overlay.show` 的区别：内容是插件视图，浮层里的动作能返回 `{"view": …}` 刷新内容（翻目录 / 改选择）。参数见 §3.2.4.2；入口项里的常见写法也是它 |
| `musicxx.ui.openRoute` | `{route, arguments?}` | `{ok:true}` 或 `{ok:false,error}` | 打开插件页面或官方页面，规则见下 |
| `musicxx.ui.playing.openMenu` | 无 | `{ok:true}` | 打开播放页的「更多」菜单（与底部控制栏的菜单按钮、右上角的「更多」快捷功能同一份内容）。打开后立即返回，不等用户关掉 |

`musicxx.ui.openRoute` 允许两类目标（别的地址会被拒绝，错误信息里会列出全部可用别名）：

1. **任意插件的声明式页面**：`ext://<插件id>/<视图id>`（含本插件；跨插件页面由对方插件自己绘制，
   发起方拿不到对方的数据）；
2. **官方页面白名单**（只认这些别名键，插件不接触应用内部路由名）：

| 别名键 | 页面 | 别名键 | 页面 |
|---|---|---|---|
| `musicxx.settings` | 设置 | `musicxx.player` | 播放页 |
| `musicxx.about` | 关于 | `musicxx.home` | 音乐主页 |
| `musicxx.logs` | 日志 | `musicxx.userHome` | 用户页 |
| `musicxx.plugins` | 插件管理 | `musicxx.search` | 搜索 |
| `musicxx.externPlugins` | 外部插件管理 | `musicxx.localSongs` | 本地歌曲 |
| `musicxx.history` | 播放记录 | `musicxx.lyrics` | 歌词 |

```js
await musicxx.ui.toast({ text: "已刷新" });
await musicxx.ui.toast("轻提示");                    // 也可以直接传字符串
const answer = await musicxx.ui.dialog({ content: "删除缓存？", textConfirm: "删除", textCancel: "算了" });
if (answer.confirmed) { /* ... */ }
await musicxx.ui.openRoute("ext://my_plugin/settings");
await musicxx.ui.openRoute("musicxx.localSongs");
```

```cpp
// C++ 侧只有声明式提示（fire-and-forget）；弹窗与开页面走同名能力
uiNotify(R"({"text":"已刷新"})");
capabilityCall("musicxx.ui.dialog", R"({"content":"删除缓存？"})", &notify, 0);   // 0 = 一直等用户点
```

> 弹窗与浮层都是**用户操作**：等它们的时候把超时设长一点（动态库侧传 0 表示不设超时；
> JS 侧的 `musicxx.ui.dialog` / `overlay` / `pick` 缺省已经给了 10 分钟），
> 否则用户还没点就已经超时了。

#### 3.2.4.1 浮层（`musicxx.ui.overlay.show`）

浮层是"代码打开、等结果"的调用：插件打开一层 sheet，用户在浮层里操作，关掉后调用返回
**怎么关的**。页面里想让某一行打开浮层，就让那一行带一个 `command` 动作指向它（同一个函数、同一份参数）。

| 参数 | 类型 | 说明 |
|---|---|---|
| `preset` | string | `custom`（缺省，内容 = `blocks`）/ `picker`（选择列表，见下） |
| `id` | string | 可选：给这层浮层一个名字，`overlay.close` 可以点名关它 |
| `title` / `depict` | string | 标题 / 副标题 |
| `blocks` | array | `preset: custom` 的内容（页面块数组，可以放按钮 / 控件 / 列表） |
| `options` | array | `preset: picker` 的候选项：`[{value, title, describe?, selected?}]`（`describe` 可多行）；也可以直接写字符串数组（每项的值与标题都是这个字符串） |
| `value` | any | `preset: picker` 的当前值（对应候选项打上选中标记） |
| `textConfirm` / `textCancel` | string | 底部按钮文案（不给就没有那个按钮） |
| `size` | string | 尺寸预设：`autoHeightAutoWidth`（缺省）/ `autoHeightFullWidth` / `full`（可用宽高的 90%）/ `fullHeightAutoWidth` / `columnAutoHeightRowFullHeightAutoWidth` |
| `dismissible` | bool | 缺省 `true`：点遮罩 / 返回键就能关；写 `false` 时必须点按钮（或插件自己 `overlay.close`） |

返回：

| 情况 | 返回 |
|---|---|
| 点了 `textConfirm` 的按钮 | `{ok:true, reason:"confirmed", value?}` |
| 点了 `textCancel` 的按钮 | `{ok:true, reason:"cancelled"}` |
| 点关闭 / 点遮罩 / 返回键 / 被程序关掉（插件卸载、插件页面退出） | `{ok:true, reason:"closed"}` |
| `preset: picker` 选中一项 | `{ok:true, selected:<该项的 value>}` |
| `preset: picker` 取消 / 关掉 | `{ok:true, cancelled:true}` |
| 参数不全（`custom` 没给 `blocks`、`picker` 没给 `options`） | `{ok:false, error}` |

规则：

- **一个插件可以同时开多层**：后开的盖在上面（不限制层数）；`musicxx.ui.overlay.close` 不点名时关
  **最上面那一层**，给了 `id` 就关最上面那个名字对得上的层（被盖住的层也能关，不会误关别的层）；
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
// C++：capabilityCall 的超时传 0 = 一直等用户操作
capabilityCall("musicxx.ui.overlay.show", R"({"title":"运行详情","blocks":[],"textConfirm":"知道了"})", &callback, 0);
capabilityCall("musicxx.ui.overlay.close", R"({})", &callback, 0);
```

#### 3.2.4.2 插件视图浮层（`musicxx.ui.openOverlaySheet`）

`overlay.show` 的内容是调用方给的静态 `blocks`；这一条的内容是**插件自己的视图**，
所以能在浮层里翻页（浮层里的按钮调用返回 `{"view": …}` 就换成新内容）。

| 参数 | 类型 | 说明 |
|---|---|---|
| `route` | string | 必填，`ext://<插件id>/<视图id>`（地址规则与 `openRoute` 一致） |
| `title` | string | 可选，浮层标题；不给就用视图里的 `title`，都没有就是“插件页面” |
| `arguments` | object | 可选，原样作为插件能力的参数（主程序会再补上 `view` 与 `ui`）；不给时把顶层除 `route` / `title` / `arguments` / `size` / `dismissible` / `textConfirm` / `textCancel` 之外的键当作参数 |
| `size` / `dismissible` / `textCancel` / `textConfirm` | — | 与 `overlay.show` 同名参数含义一致 |

- 返回：`{ok, reason, title?}`（`reason` 是 `confirmed` / `cancelled` / `closed`）；
  取不到插件视图时是 `{ok:false, error}`（不会开一个空浮层）；
- **这次调用在浮层关掉之后才完成**，所以调用方正好能在关闭后刷新自己的列表；
- 插件在能力处理器里调它时**不要 await**（会与应用实现的能力互相等）；用入口项（`command` 动作）
  或页面里的按钮（`command`）调都不受这个限制 —— 那是主程序在执行。

```js
// 页面或入口项里的写法：点了就开一层，由主程序等你关（零代码）
musicxx.ui.registerEntry({
  name: "pickSongs",
  type: "addsong.entry",
  data: {
    title: "从我的服务器挑歌",
    subtitle: "自己画的挑歌界面",
    action: {
      kind: "command",
      name: "musicxx.ui.openOverlaySheet",
      args: { route: "ext://my_plugin/picker", title: "挑歌" },
    },
  },
});
```

> 能用 `musicxx.source.browse` 让主程序画界面时（§3.3），不要走这一条：自己画要自己处理
> 列目录 / 勾选 / 加歌 / 刷新，主程序那套还多了搜索、排序、整目录递归与分类切换。

#### 3.2.5 插件私有存储与配置（`musicxx.storage.*`）

两块地方、两套语义：

| 用途 | 能力 / 参数 | 存放位置 |
|---|---|---|
| 私有 KV（不需要给用户看的数据） | `musicxx.storage.get/set/delete/list`，`namespace` 省略或写别的值 | `<dataDir>/<插件id>/data/kv.json` |
| 用户可见的设置 | 同上，`namespace:"config"` | **插件目录里的 `config.json`**（用户可以直接编辑） |

| 能力 | 参数 | 返回 |
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
// 走私有 KV 就用能力（注意应答就是值本身，不是对象外壳）
capabilityCall("musicxx.storage.get", R"({"key":"greeting"})", &notify, 5000);
capabilityCall("musicxx.storage.set", R"({"key":"greeting","value":"早上好"})", &notify, 5000);
```

必须记住的几条：

- **`get` / `getConfig` 的应答就是值本身**：不要按"外面还有一层对象"去读，否则设置项读不出来
  （表现是"改完重启又变回默认"）。
- `set` 返回成功 = **已经写盘**；`config.json` 每次是整份重写，不要用它存高频变化的数据。
- 值可以是任意 JSON（对象、数组、数字、布尔）；**写 `null` 会存成空串 `""`**（读回来是空串，不是 `null`）。
  **不设容量配额**，但每次写入都整文件重写，所以别把它当数据库用。
- 默认值由**你自己**在读取时给：框架不保存默认值，也没有"恢复默认"入口（用户直接编辑 `config.json` 即可）。
- 读配置是异步的：**读回来的旧值可能后到并覆盖用户刚改的值**，自己加"已经改过就不再覆盖"的保护。
- 私有 KV 与 `config.json` 都是**按插件隔离**的，别的插件看不到。

#### 3.2.6 网络（`musicxx.net.*`）

**这些能力全部由插件框架自己实现**（原生代码，跑在插件框架自己的网络线程上），不经过主程序：
`musicxx.net` 权限只表示"允许使用这条通道"，框架**不限制可访问的域名**。动态库插件也可以不用它、
自己用系统 API 联网；JS 插件没有原生网络能力，用这套最方便。

OpenSSL 以**静态库**链进插件框架，所以客户端能直接连 https / wss，应用包里不需要带
`libssl` / `libcrypto`。**服务端不做 TLS**（插件自己起服务只监听 http / ws）。

**所有平台的插件框架都带 TLS**：`https` / `wss` 恒可用，不存在"某个构建里没有"的情况
（缺 OpenSSL 时构建阶段就失败，不会发布一个功能残缺的版本）。`musicxx.net.info()` 的
`tls` 恒为 `true`、`schemes` 包含 `http` / `https` / `ws` / `wss`，`tlsProvider` 是运行期
OpenSSL 版本。所以 `https` 失败只剩真正的原因：`tls_config`（`caFile` / `caPem` 读不出来）、
`tls_cert_invalid`（证书链或主机名不对）、`tls_handshake_failed`（协议协商失败）。

| 接口（JS） | 内部 op | 参数 | 返回 |
|---|---|---|---|
| `musicxx.net.fetch` / `http.fetch` | `http.fetch` | `{url, method?, headers?, body?, timeoutMs?, responseType?, maxBytes?, tlsVerify?, caFile?, caPem?, serverName?}` | 见下 |
| `musicxx.net.download` / `http.download` | `http.download` | `{url, fileName?, headers?, method?, timeoutMs?, tlsVerify?, caFile?, caPem?, serverName?}` | `{ok, status, path, bytes}` |

`fetch` 参数与限制：

| 参数 | 取值 | 缺省 |
|---|---|---|
| `url` | `http` / `https`，必须有主机名 | 必填 |
| `method` | 任意合法 HTTP 方法名（大小写不敏感；不再只限常见的六个） | `GET` |
| `headers` | 对象；键必须是合法 HTTP 头名（可见 ASCII、不含 `:`），值会转成字符串 | 空 |
| `body` | 对象/数组 → 自动 JSON 编码；字符串原样发送 | 空 |
| `timeoutMs` | **不限范围**：缺省 15000；`<= 0` = 不设超时 | 15000 |
| `responseType` | `text` / `json` / `bytes`（`json` 也按文本给，自己 `JSON.parse`；`bytes` 给 `bodyBase64`） | `text` |
| `maxBytes` | **不限范围，缺省不限制**；自己给一个上限时读满就断开并标 `truncated` | 不限制 |
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
  只有传输失败 / TLS 失败 / 超时才 `ok:false`。**`download` 是例外**：非 2xx 直接回
  `ok:false`（不把错误页正文写成文件）。
- 失败时的 `error` 是归类后的字符串（`resolve_failed` / `connect_failed` / `tls_*` /
  `incomplete` / `timeout` / `cancelled` 等），`message` 是具体原因。
- 响应体是**流式读取**的：给了 `maxBytes` 且读满就断开连接并置 `truncated:true`；
  不给 `maxBytes` = 整份读到内存（大文件用 `download`，它边读边写盘）。`responseType` 为 `bytes` 时
  返回前还要 base64 一份，峰值内存约为响应体大小的 2.3 倍。
  > 待办：`fetch` 将来会支持把响应体直接流给插件（不先整个收进内存）；在那之前要处理大文件，
  > 请用 `download` 落盘再自己读。
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

老的能力名 `musicxx.net.fetch` / `musicxx.net.download` 仍然可用（历史上直接调这些名字的插件不会坏）：
它们由**插件框架直接回答**，走的就是本节这套原生实现，参数与返回形状完全一致，主程序不再
另写一份代理。新插件请用本节这套。

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
- **支持 `GET` / `HEAD` 与单段 `Range`**（`bytes=a-b` / `bytes=a-` / `bytes=-N`；`206` +
  `Content-Range`，越界 `416`），文件按 64 KiB 边读边写；播放器拖动进度、读文件尾部元数据
  都靠它，否则会退化成整段重下。多段 `Range` 当成"没有 Range"整段返回。
- **`staticOnly: true`**：没命中任何静态路由的请求直接回 404，不再发给插件（纯文件服务都该打开，
  否则每个 404 都要等到 `requestTimeoutMs` 才回 504）。不写这项时未命中的请求照旧发
  `http.request`，于是"同一个服务既能挂文件、也能自己回答接口"。
- **同一个文件 / 同一段内容重复登记返回同一个 URL**（按路径 + 大小 + 修改时间，或内容比较）。
- **生命周期跟着插件实例**：停用 / 卸载 / 关『拟声++』/ 插件框架停机时服务一起关掉
  （端口释放，已建立的连接也一并断开），路由也跟着没了；提前关服务用 `http.stop(id)`，
  只摘一条用 `http.unroute`。
- 容量：单条内存内容 ≤ 8 MiB（大文件用 `file`）、每个服务路由 ≤ 1024、服务数 ≤ 8、
  连接 ≤ 64（见 `musicxx.net.info()` 的 `limits`）。
- **老版本的插件框架没有静态路由**（只有 `musicxx.net.http.serve`）：调用前先看
  `typeof musicxx.net.http.route === "function"`，没有就自己降级（例如直接返回本地路径
  `kind:"file"`）。
- 这是**代理**的一般机制：`musicxx.net.fetch` / `download` 取到的数据 → `route`（小的给 `bytes`、
  大的落盘给 `file`）→ 播放器只看到干净的本地 URL；FTP、私有协议、格式转换、统一鉴权都可以
  这么做。完整示例见 `plugins/example_override_media_source`（本地文件代理）与
  `plugins/example_js_net`（`serve({static})` / `route` / `unroute` 自检）；FTP 这种要自己连服务器、
  边读边转的完整做法见 `plugins/example_ftp_source`（TCP 客户端 + 回环监听 + `socket.pause` 背压）。

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
  所有平台的插件框架都带 TLS（`musicxx.net.info()` 的 `tls` 恒为 `true`），
  不会出现某个构建里没有 `https` / `wss` 的情况。
- **服务端默认只监听回环 `127.0.0.1`**，端口由系统分配（`port: 0`）。要同网段可访问必须显式写
  `host: "0.0.0.0"` —— 框架不会替你拦，但你要清楚这意味着什么。
- **事件是异步的**：JS 侧由插件框架**直接投递到 JS 线程**（所以可以在返回 Promise 的能力里
  `await` 网络事件）；动态库插件用事件表订阅 `plugin.<插件id>.net`（在**插件框架线程**派发，
  处理器里不能阻塞）。

能力上限（超过按可读错误返回，不排队）：每个插件同类服务 ≤ 8、连接 ≤ 64、同时进行的 HTTP 客户端
请求 ≤ 16、单个 HTTP 服务等回答的请求 ≤ 64、**HTTP 服务端**请求正文缺省 ≤ 8 MiB
（`maxBodyBytes` 可调）、WebSocket 单条消息 ≤ 8 MiB、
单条连接发送队列 ≤ 4 MiB、UDP 通道 ≤ 32。HTTP **客户端**的 `timeoutMs` / `maxBytes` 不设上限
（见下一张表）。

**HTTP 客户端**（`http.request`）参数：

| 参数 | 说明 |
|---|---|
| `url` | 必填，`http://host[:port]/path?query` 或 `https://…` |
| `method` | 任意合法方法名（缺省 `GET`；不再限白名单，只要求是合法的请求方法符号） |
| `headers` | 对象；`Content-Length` / `Connection` / `Transfer-Encoding` 由框架生成，写了也被忽略 |
| `body` | 字符串正文 |
| `bodyBase64` | 二进制正文（与 `body` 二选一） |
| `json` | 任意 JSON：自动序列化并补 `Content-Type: application/json` |
| `timeoutMs` | **不限范围**，缺省 15000（整次请求，含解析、连接与 TLS 握手）；`<= 0` = 不设超时 |
| `maxBytes` | **不限范围，缺省不限制**（正文全读进内存）；自己给了上限时读满就断开并标 `truncated:true`；大文件用 `http.download` |
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
`tls_config`（`caFile` / `caPem` 读不出来）/ `tls_cert_invalid`（证书链或主机名不对）/
`tls_handshake_failed`（协议协商失败）。
下载到本地时还可能是 `no_data_dir`（插件数据目录未就绪）/ `write_failed`（文件建不出来或写失败）。

`message` 里的原因文本**统一是 UTF-8**（含系统给出的部分，例如「目标计算机积极拒绝」）。
以前的版本在中文 Windows 上把系统错误文本按本机 ANSI 编码直接塞进来，中文会显示成乱码。

**HTTP 服务端**：`http.serve`（`id?`, `host?`, `port?`, `path?` 前缀, `staticOnly?`,
`static?` 静态内容清单（见上一节）, `maxBodyBytes?`, `requestTimeoutMs?`, `maxConnections?`）
→ `{ok, id, host, port, url, routes?}`。每个请求发一条
`http.request`，**必须**用 `http.respond` 回答（`status` / `headers` / `contentType` /
`body`|`bodyBase64`），否则 `requestTimeoutMs`（缺省 30 s）后自动回 504 并关连接。
支持 `Connection: keep-alive`；请求体可以是 `Content-Length`，也可以分块传输（chunked）。

**TCP**：`socket.connect` / `socket.listen` 之后用同一套 `socket.send` / `socket.close`
（服务端连接用事件里的 `id`，并带 `serverId` 区分来源）。字节以 `dataBase64` 给出，
合法 UTF-8 时再给 `text` / `data`。连接事件：`socket.connected` / `socket.accepted` /
`socket.data` / `socket.closed`（`by` = `remote` / `local` / `error` / `plugin`）。

`socket.close` **会先把发送队列里已经排队的字节写完再关**（最多等 5 秒；对端一直不读就强关），
所以"发完最后一段就 close"不会丢掉尾巴。`close` 之后这条连接不再接受 `send`（回 `closed`）。

**转发大流量时要做背压**：读是连续的，而发送队列上限 4 MiB（超了 `socket.send` 回 `queue_full`）。
所以：

- `socket.send` 的结果里带 `queuedBytes`（还没写出去的字节数）；
- `socket.pause`（`{id, paused}`）暂停 / 恢复**读**：`paused: true` 之后这条连接不再给 `data` 事件，
  已收的字节照旧发出；下游缓过来再 `paused: false`。

```js
// 典型写法（把上游字节转给下游）：下游队列长了就暂停上游读
musicxx.net.socket.send(clientId, { dataBase64: ev.dataBase64 }).then((r) => {
  if (!r || !r.ok) return;
  if (r.queuedBytes > 1024 * 1024) musicxx.net.socket.pause(upstreamId, true);
});
// 下游 drain（或定时查一次 queuedBytes 降下来）后再
musicxx.net.socket.pause(upstreamId, false);
```

**文本编码与 base64**（FTP / 局域网共享这类协议的名字经常不是 UTF-8，而 QuickJS 没有
`TextDecoder`）：

| 函数（JS） | 参数 | 返回 |
|---|---|---|
| `musicxx.util.textEncode` | `{text, encoding?}`（缺省 utf-8） | `{ok, base64, bytes}` |
| `musicxx.util.textDecode` | `{base64, encoding?}`（`base64` 可以是字符串或**字符串数组**） | `{ok, text, bytes}` |
| `musicxx.util.base64Encode` | `{text}` | `{ok, base64}` |
| `musicxx.util.base64Decode` | `{base64}` | `{ok, text}` |

- 编码名交给原生 iconv：`utf-8` / `gbk` / `gb18030` / `big5` …；转换失败是
  `{ok:false, error:"bad_encoding"}`，不解码成功就是入力不合法（`bad_base64`）；
- 分片给数组时由原生层**先拼字节再转编码**（直接拼 base64 文本会因 padding 出错）；
- 都是**同步**返回（目录列表要当场解出文件名）；动态库插件不需要这一层（直接链 `utilxx_base`）。

**WebSocket**：`ws.connect`（`url` 或 `host`/`port`/`path`）/ `ws.listen` / `ws.send` /
`ws.ping` / `ws.close`。`wss://` 会先做 TLS 握手（同样支持 `tlsVerify` / `caFile` / `caPem` /
`serverName`）。握手、掩码、分片、`ping`/`pong` 与关闭握手都由框架处理；
文本帧给 `text`，二进制帧给 `dataBase64`；`ws.close` 带 `code` / `reason` / `by`。

**UDP（广播 / 组播 / 单播）**：JS 与动态库插件共用同一套通道。

| 接口（JS） | 内部 op | 参数 | 返回 |
|---|---|---|---|
| `musicxx.net.udp.open(options)` | `udp.open` | `{id?, host?, port?, multicast?: "239.1.2.3" \| ["239.1.2.3"], reuseAddress?=true}` | `{ok, id, host, port, multicast}` |
| `musicxx.net.udp.send(options)` | `udp.send` | `{id, host\|"address"\|"to", port?, data?\|text?\|dataBase64?}` | `{ok, id, host, port, bytes}` |
| `musicxx.net.udp.join(id, group)` / `.leave` | `udp.join` / `udp.leave` | `{id, group}` | `{ok, id, group, multicast}` |
| `musicxx.net.udp.close(id)` | `udp.close` | `{id}` | `{ok, id}` |
| `musicxx.net.udp.info()` | `udp.info` | — | `{ok, channels:[{plugin, id, host, port, multicast, receivedPackets, receivedBytes, sentPackets, sentBytes}]}` |
| `musicxx.net.udp.on("message"\|"open"\|"closed", fn)` / `.onMessage(fn)` | — | — | 同 `musicxx.net` |

- `host` 缺省 `0.0.0.0`（收广播 / 组播就用它，单播也能收到），`port: 0` = 系统分配（结果里回填实际端口）。
- **广播**：通道建好就打开了 `SO_BROADCAST`，直接 `send({host:"255.255.255.255"})` 或子网广播地址
  （`192.168.1.255`）就行，不需要额外声明。
- **组播**：建通道时在 `multicast` 里声明，或运行期 `udp.join` / `udp.leave`；
  组播地址必须是合法的组播 IP，否则报 `bad_group`（当前只支持 **IPv4 组播**）。
  收组播一般绑定 `0.0.0.0:端口`。
- `reuseAddress` 缺省开（组播 / 多进程监听同一端口要用）；不要时写 `false`。
- **单包容积**：UDP 本身的上限约 64 KiB，超过由系统报错（框架不拆分、不分片）。
- 收包事件：`{kind:"udp.message", id, host, port, bytes, dataBase64, text?, data?}`
  （`text` / `data` 只在合法 UTF-8 时给）。
- 通道跟着插件实例：停用 / 卸载 / 关『拟声++』/ 插件框架停机时一起关掉（端口释放）。

```js
// 局域网发现：开一个通道收广播，再广播一次自己
musicxx.net.udp.on("message", (ev) => {
    console.log("来自", ev.host, ev.port, ev.text);
});
const ch = musicxx.net.udp.open({ port: 45678 });   // 0.0.0.0:45678，可收广播
musicxx.net.udp.send({ id: ch.id, host: "255.255.255.255", port: 45678, data: "musicxx.hello" });

// 组播：加入一个组，收别的设备发的包
const mc = musicxx.net.udp.open({ port: 37020, multicast: ["239.255.42.99"] });
```

```cpp
// 动态库插件：同一套 op（收发都不需要额外集成）
std::string out;
netCall(musicxx::plugin::net_op::udpOpen, R"({"port":45678})", out);
netCall(musicxx::plugin::net_op::udpSend, R"({"id":"udp1","host":"255.255.255.255","port":45678,"data":"hi"})", out);
```

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
| `ws.close` | `id, serverId?, by, code?, reason?, error?` |

> `ws.pong` **不再单独发事件**（ping/pong 由框架自己应答，插件不会收到 pong）。
| `udp.open` | `id, host, port` |
| `udp.message` | `id, host, port, bytes, dataBase64, text?` |
| `udp.closed` | `id, reason`（`local_close` / `plugin_unloaded`） |

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

**跟着插件实例走**：停用 / 卸载 / 关『拟声++』/ 插件框架停机时，该插件的服务、连接、UDP 通道与
未完成的请求一起关掉（端口释放）。完整示例：`plugins/example_js_net`（HTTP / TCP / WebSocket / UDP
一次跑完，载入后自动自检）。旧插件框架没有这套能力：JS 先看 `typeof musicxx.net.http === "object"`，
动态库插件看 `pluginxx::queryInterface<PluginxxNetIface>(host, PLUGINXX_IFACE_NET)` 是否为 `nullptr`。

#### 3.2.7 渲染槽位（`musicxx.render.*`）

用来查询"有哪些可选样式"、读"现在选中/生效的是谁"、并且**允许插件切样式**（包括切回内置样式）。

| 能力 | 参数 | 返回 |
|---|---|---|
| `musicxx.render.list` | `{slot?}` | `{ok:true, slots:[{slot, title, selectedId, items:[...]}]}` |
| `musicxx.render.current` | `{slot?}` | `{ok:true, slot, id, title, source:"builtin"\|"plugin", plugin, night}`；没有生效项 → `{ok:false,error:"no_item"}` |
| `musicxx.render.select` | `{id, slot?}` | `{ok:true, slot, id}`；不可用 / 未知 → `{ok:false,error}` |

条目字段：`{id, title, depict?, source, plugin?, available, reason?, selected}`。
不给 `slot` 时用默认槽位（当前是 `player.background`，播放页背景）；槽位名还有 `player.icon`（播放页歌曲图）
与 `player.visualizer`（播放页音频动效）。
槽位与样式声明的完整说明见 [plugin-shader-bundle.md](plugin-shader-bundle.md)。

```js
const all = await musicxx.render.list();                        // 默认槽位
await musicxx.render.select("plugin.my_plugin.bg");             // 切到本插件的样式
await musicxx.render.select("musicxx.auto", { slot: "player.background" });  // 切回内置

// 三个槽位的 id 写法同一套：内置项 `musicxx.<名字>`（背景 / 动效用枚举名，歌曲图用短名），
// 交还控制权用 `musicxx.previous`（恢复 `musicxx.default` 重置掉的值，没有记录时不动），
// `musicxx.default` = 写回该设置项的默认值（背景 `musicxx.auto`、动效 `musicxx.none`、歌曲图 `musicxx.circle`）
await musicxx.render.select("musicxx.square", { slot: "player.icon" });
await musicxx.render.select("musicxx.previous", { slot: "player.icon" });
```

**"现在是不是本插件在画"**用广播 `musicxx.state.renderSlots` 同步判断（渲染线程里每帧都会读，不要调能力）：

```js
const slots = musicxx.broadcast.read("musicxx.state.renderSlots") || {};
const mine = slots["player.background"];
if (mine && mine.itemId === "plugin.my_plugin.bg" && mine.visible) { /* 本插件在画，而且看得见 */ }
```

条目字段：`{itemId, plugin, selectedId, night, visible, animate, width, height, maxFps}`。
`visible:false` = 现在没渲染（播放页被遮挡 / 切后台），插件可以据此停掉耗时的计算。

#### 3.2.8 封面与频谱数据（`musicxx.media.*`）

封面**只作为数据来源**（不参与画面绘制、不推直链）；要在着色器里用封面，声明渲染项的 `cover` / `image`。

**`musicxx.media.palette`** —— 封面取色结果 + 内置背景实际用的 4 色

- 参数：`{force?:bool}`（`true` = 强制重新分析）。
- 返回：`{ok, hasCover, analyzed, night, source:"plugin"|"builtin", srcKey, version,
  raw:{main, light, lightMuted, dark, darkMuted, dominant:[…]}, background:[4 个 "#rrggbbaa"]}`；
  `source` 告诉你这套颜色是插件通过取色能力给的，还是应用自己分析出来的。

**`musicxx.media.palette.set`** —— 把插件算好的颜色交给应用（写的是与内置分析同一批字段）

- 参数：`main` / `light` / `lightMuted` / `dark` / `darkMuted`（`#rrggbb` 或 `#aarrggbb`，至少给一个），
  没给的键保持原样。
- 返回：`{ok:true}`；一个颜色都没给 → `{ok:false, error:"至少要给一个颜色…"}`。
- 写完之后内置背景、插件背景与 `musicxx.icon.*` 来源都会用这套颜色。

**`musicxx.media.cover`** —— 当前封面的字节（自己要分析像素时用）

- 参数：`{size?=96, format?="jpeg", includePath?, bind?}`：
  - `size` 16~1024（最长边等比缩放，超出会被钳到范围内）；
  - `format` = `jpeg`（缺省，质量 88）/ `png` / `rgba`（未压缩，字节数必然 ≤ `size × size × 4`，最大一档 4 MiB）；
  - `includePath` = 是否返回本地路径（只有本地 / 缓存来源有路径）；
  - `bind` = 把这次解码的像素**同时绑成纹理**，用这个键登记进插件图像表（渲染项用 `image` 引用它，
    复用同一次解码，不重复解码）。绑成功时返回里多 `bound:true` + `bindKey`；绑失败是
    `bound:false` + `bindError`（不影响这次拿到的字节）；带 `bind` 的调用**不进结果缓存**
    （插件可能先解绑再重新绑，命中缓存会让它拿不到新纹理）。
- 返回：`{ok, srcKey, kind:"local"|"cache"|"content"|"asset"|"network", path?, width, height,
  format, bytes, sha256, data(base64), fromCache, bound?, bindKey?, bindError?}`；失败 `{ok:false, error:"no_cover" |
  "cover_decode_failed" | "读取封面失败：…"}`。

**`musicxx.media.bindImage` / `musicxx.media.unbindImage`** —— 把插件自己的图片绑成纹理给着色器采样

- `bindImage`：`{key, data(base64), format:"png"|"jpeg"|"rgba", width?, height?}`（`rgba` 必须给宽高），
  返回 `{ok, key, width, height}`；键要 1~64 位字母数字与 `_ - .`，**张数与尺寸都不限制**
  （显存由你自己控制，插件框架停止 / 关『拟声++』时全部释放）。
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

// 想要"每帧都新鲜"的频谱（着色器、自绘动效）读广播 `musicxx.state.spectrum`，约 10 Hz：
const live = musicxx.broadcast.read("musicxx.state.spectrum");
if (live && live.available) { drawBars(live.bands); }
```

**从插件提供频谱与副歌实现**（替代应用内置的那一份）：能力 `musicxx.media.spectrum.extract`
与 `musicxx.media.chorus.analyse`，载荷、数据格式、超时与失效规则见 [plugin-capabilities.md](plugin-capabilities.md)，
写法示例见 [plugin-js-api.md](plugin-js-api.md) §11（C++ 同构）。

**`musicxx.media.kernel`** —— 播放内核信息：能播什么、能用哪些过滤器

- 参数：`{part?="all", refresh?}`：`part` = `all` / `versions` / `filters` / `decoders` / `formats`
  （只裁剪 `text`，`kernel` 永远是完整数据）；`refresh` = 重新采集而不是用缓存
  （内核运行期不会变，一般不用）。
- 返回：`{ok, kernel, text}`，`kernel` 的字段：

| 字段 | 内容 |
|---|---|
| `mpvVersion` | 内嵌的 libmpv 版本（如 `mpv v0.41.0-dev-g975d4d7b2`） |
| `ffmpegVersion` | ffmpeg 版本。**这份 ffmpeg 是裁剪构建、不带版本号，值就是 `unknown`**（照实返回） |
| `kernelReady` | 播放器是否已经给出属性；为假时 `mpvVersion` / 两个过滤器表靠 mpv 的那一半 / `decoders` 是空的 |
| `audioFilters` | 能**直接写进 mpv `af`** 的名字，每项 `{name, libavName?, depict?, flags, builtin}`（`depict` 是 ffmpeg 的说明，**裁剪构建里很多过滤器没有说明**，没有就不带这个键） |
| `videoFilters` | 能**直接写进 mpv `vf`** 的名字，字段同上 |
| `decoders` | mpv 能用的解码器，每项 `{codec, driver, depict?, hardware}`（同一个编解码器可能有多个驱动） |
| `audioExtensions` / `videoExtensions` | 应用扫描时认得的后缀（小写、排序） |
| `counts` | 各数组长度，另有 `audioFiltersBuiltin` / `videoFiltersBuiltin` / `hardwareDecoders` |
| `tookMs` | 这次采集的耗时 |

- **`audioFilters[].name` 就是能写进 `af` 的名字**：这份列表是"链接进来的 ffmpeg 过滤器"与
  "mpv 自己编译进来的过滤器"的并集。`builtin: true` 的是 mpv 自带的那几个（`format` / `scaletempo` /
  `rubberband` …，它们没有 ffmpeg 的说明）；两边重名时（例如 `format`）mpv 把 ffmpeg 那个叫
  `lavfi-format`，`name` 给的就是这个能用的名字，`libavName` 才是 ffmpeg 里的原名。
- **不要按上游 ffmpeg 文档假设**：应用内嵌的 ffmpeg 只链接了二十来个音频过滤器与不到十个视频过滤器，
  文档里有的 `dynaudnorm` / `superequalizer` / `extrastereo` / `haas` / `treble` / `deesser` 等**没有**。
  写音效、拼 `af` / `vf` 之前先读这一份。
- `decoders` 的 `hardware` 按驱动名后缀判断（`h264_qsv` / `av1_cuvid` / `hevc_videotoolbox` 是硬件，
  `aac_fixed` 这种精度变体不是），用来判断"要不要指望硬件解码"。
- 与 MCP 工具 `GetMediaKernelInfo`、命令行 `kernel.info` 是同一份数据（那边给结构化 JSON 与文本）。
- 缓存：第一次读之后会缓存；内核在运行期不会变，只有你在播放器还没就绪时读到过才需要 `refresh`。

```js
const info = await musicxx.media.kernel({ part: "filters" });
const af = new Set(info.kernel.audioFilters.map((f) => f.name));
if (af.has("equalizer")) { /* 可以拼 "equalizer=f=80:width_type=q:width=1:g=5" */ }
const hw = info.kernel.decoders.filter((d) => d.hardware).map((d) => d.driver);
```

#### 3.2.9 全局字体（`musicxx.ui.setFontFamily` / `getFontFamily` / `loadFont`）

| 能力 | 参数 | 返回 |
|---|---|---|
| `musicxx.ui.setFontFamily` | `{fontFamily}`（`default` / `""` / `null` = 恢复默认） | `{ok:true, fontFamily, isCustom}` |
| `musicxx.ui.getFontFamily` | 无 | `{ok:true, fontFamily, defaultFontFamily, customFontFamily, isCustom, ownerPluginId}` |
| `musicxx.ui.loadFont` | `{family, path?\|data?, apply?=true}` | `{ok:true, family, applied, fontFamily}` |

- `loadFont` 的 `path` 是**任意可读的文件路径**：绝对路径（`C:\…` / `/…`）、`file://` 链接、
  `~/…`（当前用户目录）都直接使用，相对路径按**插件目录**解析（随包分发字体就写相对路径）。
  框架不限制插件能读哪个文件（动态库插件与主程序同进程，本来就能读写整台机器的文件）。
  `path` 与 `data`（base64 字体字节）给一个即可；`apply` 为真时加载完立刻设为全局字体。
  字体文件最大 100 MiB（超过直接回错误）。
- **换字体会重建整棵界面，调用本身可能耗时几百毫秒到几秒**：
  在声明式页面的按钮处理器里调用时必须"发出去就返回"（`.then` 收尾），
  `await` 它会与同步进插件框架的调用方互相等到超时（见 [plugin-ui.md](plugin-ui.md) §4.1）。

```js
await musicxx.ui.setFontFamily({ fontFamily: "我的字体" });
await musicxx.ui.loadFont({ family: "我的字体", path: "resource/font/my.ttf" });
await musicxx.ui.loadFont({ family: "我的字体", path: "C:/Windows/Fonts/msyh.ttc" }); // 任意路径都行
await musicxx.ui.setFontFamily({ fontFamily: "default" });     // 恢复
```

#### 3.2.10 应用能力导出（`musicxx.feature.list` / `musicxx.feature.call`）

这两条能力读的是"**应用自己能做什么**"（与 MCP 工具、本机命令行共用同一份实现），
输出是给模型/脚本看的文本或 JSON：

| 能力 | 参数 | 返回 |
|---|---|---|
| `musicxx.feature.list` | 无 | `{ok:true, features:[{id, title, depict, args, output:"text"\|"json"}]}` |
| `musicxx.feature.call` | `{id, args}` | `{ok:true, text?, json?, output}` 或 `{ok:false, error}` |

- 典型能力：`GetAudioSpectrum`（整曲频谱的文本表）、`AnalyseAudioChorus`（副歌区段）等；
  **要解码整曲的长任务给更长的超时**（JS 封装 `musicxx.feature.call(id, args, timeoutMs)` 缺省 30 秒）。
- 想要**原始数据**而不是文本结论，用 §3.2.11 的功能埋点调用（`musicxx.feature.point.call`）。

```js
const list = await musicxx.feature.list();
const chorus = await musicxx.feature.call("AnalyseAudioChorus", {});
```

#### 3.2.11 功能埋点（`musicxx.feature.point.*`）

读"应用留出了哪些扩展点、现在谁在实现"，以及**主动调一个功能埋点拿原始数据**。
概念与写法见 §7，这里只列能力：

| 能力 | 参数 | 返回 |
|---|---|---|
| `musicxx.feature.point.list` | `{id?, provided?}` | `{ok:true, points:[{id, title, type, depict, available, callable, callDoc, callStat, rows:[{type, layer, owner, name, priority, enabled, effective, note}], stages:[{owner, name, layer, priority, enabled, before, after, runs, cancels, drops, errors}], broadcasts:[…], lastCanceledBy?, lastDroppedBy?, lastRewrittenBy?, stageReentries?}]}` |
| `musicxx.feature.point.call` | `{id, args?, refresh?}` | `{ok, id, value, by, identity, fromCache, ms, canceledBy?, droppedBy?}` 或 `{ok:false, error, message}` |
| `musicxx.feature.point.define` | `{id, title?, depict?, args?, result?}` | `{ok:true, id}` 或 `{ok:false, error}` |
| `musicxx.feature.point.undefine` | `{id}` | `{ok, removed}` |

- `rows` 里的每一行是"这个功能埋点的一个实现"：`layer` = `extern`（外置插件实现）/ `builtin`（内置插件）/
  `core`（应用兜底），`effective` = 现在真正生效的那一个；`note` 会写清"谁把它拦下了"（`cancel` 裁决）。
- `stages` 是**同一个功能埋点的前链与后链**（与 `rows` 是两回事）：`before` / `after` 为真表示这条
  登记管哪一段，`runs` / `cancels` / `drops` / `errors` 是计数；顺序按前链（层 → 优先级 → 登记顺序），
  **后链逆序执行**（所以这个列表的最后一行是最先改输出的）。`lastCanceledBy` / `lastDroppedBy` /
  `lastRewrittenBy` 是最近一次调用里"谁拦下 / 谁丢弃 / 谁最后改了输出"，只用于排障。
  概念见 [plugin-api.md](plugin-api.md) §8.1。
- `callable` / `callDoc` 说明"这个点能不能被主动调用、参数与结果是什么形状"：
  **调之前先看 `callDoc`**。`broadcasts` 是谁订阅了这个埋点的广播通知（只收通知，既不参与实现也不影响结果）；
  `stageReentries`（>0 才带）是后链重入次数，用来发现"后链又改了一次自己的输出"。
- `musicxx.feature.point.call` 的 `error` 取值：
  `not_callable`（这个点不能主动调）/ `bad_args`（参数不符合 `callDoc`）/ `no_impl`（没有任何实现）/
  `canceled`（被拦下了：前链或实现链）/ `dropped`（后链把结果丢弃了）/
  `busy`（同一功能埋点 + 同一调用方的上一次还没结束）/ `failed`（实现报错）。这三种"没有结果"是不同的事：
  `canceled` 是这次调用被拦下（之后谁都不问）、`dropped` 是实现跑完了但结果被丢掉；`skip` 不是错误，
  它只是这一份实现没参与。
- **调用零副作用**：只读值仓、不写值仓、不改应用状态。想让用户看到效果要用对应的设置能力
  （例如 `musicxx.media.palette.set`）。
- `refresh:true` 跳过值仓重新算一遍（一般不用：值仓里就是最近一次的结果）。

```js
const points = await musicxx.feature.points({ provided: true });   // 只看有实现的点
const colors = await musicxx.feature.callPoint("musicxx.media.palette.provide", {});
```

#### 3.2.12 能力自省（`musicxx.capability.subscribers`）

| 能力 | 参数 | 返回 |
|---|---|---|
| `musicxx.capability.subscribers` | `{id, phase?}` | `{ok:true, capability, phase, count, handlers:[{plugin, instance, kind, priority, phase, ownerTag}]}` |

- `handlers` 按**调用顺序**排列（`priority` 升序，同优先级按注册顺序）；
- `phase` 是可选参数：`before` / `impl` / `after`，不给就是全部阶段（按 `before → impl → after` 拼接）；
  每一条 handler 都带自己的 `phase`；
- `listAvailable:false` = 插件框架库太旧，只能报数量（看 `count`）；`phasesAvailable:false` = 库没有
  分阶段接口，`before` / `after` 阶段现在不会有处理器；
- 同一个数据也能在管理页「外部插件 → 调试」和 `capability.list` 里看到。

```js
const info = await musicxx.capability.handlers("musicxx.media.spectrum.extract");   // JS 封装
if (info.count === 0) { /* 现在没人提供，本插件要不要注册 */ }
```

#### 3.2.13 宿主（`musicxx.host.*`）

| 能力 | 参数 | 返回 | 说明 |
|---|---|---|---|
| `musicxx.host.openUrl` | `{url}` | `{ok:true}` | 交给系统处理（协议不限，系统认不认是另一回事）；会先弹一次确认 |
| `musicxx.host.clipboard` | `{text}` | `{ok:true}` | 写系统剪贴板 |
| `musicxx.host.getPath` | 无 | `{pluginId, pluginDir, dataDir, logDir, platform}` | 插件目录 / 数据目录 / 日志目录，排查时很有用 |

`musicxx.host.shareText` 目前**没有实现**（见 §3.4）。

#### 3.2.14 统计上报（`musicxx.stats.*`）

| 能力 | 参数 | 返回 | 说明 |
|---|---|---|---|
| `musicxx.stats.reportMemory` | `{bytes}` | `{ok:true}` | 自报内存占用（只展示，不影响运行） |
| `musicxx.stats.reportMetric` | `{name, value}` | `{ok:true}` | 自报自定义指标（只展示） |

```js
musicxx.stats.reportMetric("hitCount", hits);       // 便捷封装，不 await 也不会报错
```

读自己的运行统计：JS 插件用 `musicxx.stats.getSelf()`（能力实现 / 订阅 / 定时器 / 未完成的能力调用 /
脚本执行次数 / 错误数 / JS 堆大小 / 异步裁决结算数）；动态库插件目前只能看管理页
「插件详情 → 运行统计」。

#### 3.2.15 音源（`musicxx.source.*`）

插件自己的音源：在歌曲里存成"类型 + 资源 id" —— 类型 id 写成
`plugin.<你的插件 id>.<类型名>`，`src` 就是你自己定义的资源 id / 路径。主程序的内置实现
**不解析**这种类型，播放时问能力 `musicxx.player.source.resolve`（见 §8.6）。四条能力做声明与自省：

| 能力 | 参数 | 返回 |
|---|---|---|
| `musicxx.source.define` | `{id, title?, tag?, depict?, srcDoc?}` | `{ok:true, id}` 或 `{ok:false, error}` |
| `musicxx.source.undefine` | `{id, view?}` | `{ok, removed}`；给了 `view` 就只撤那一个分类（音源类型与 `[标签]` 不变）；不给 `view` 时连类型声明一起撤，返回里另带 `browseRemoved`（撤掉几个浏览分类） |
| `musicxx.source.list` | 无 | `{ok:true, sources:[{id, title, tag, depict, srcDoc, plugin, declaredBy}], browse:[…], resolveChain:[…]}` |
| `musicxx.source.browse` | `id`(必填)、`view?`(分类 id，缺省 `default`)、`title?`、`order?`、`root?`、`icon?`、`pageSize?`、`sort?`、`indexBar?`、`write?`、`search?`、`addFolderSongs?`、`download?`、`capabilities?` | `{ok:true, type, view, title, root}`：声明这个类型**可浏览并挑歌**（一个类型可以有多个分类 `view`），参数与规则见 §3.3 |

- `id` 必须形如 `plugin.<你的插件 id>.<类型名>`；**声明只用于自省与更清楚的
  报错**：音源能不能播，看的是你有没有注册 `musicxx.player.source.resolve` 的处理器，不是有没有声明。
- `tag` 是**列表标签里的短名**（上限 12 个字/字符，超出截断）：歌单、下载列表的歌曲行显示成 `[FTP]`
  这种方括号标签。不写就用 `title`，再没有就用类型 id 的最后一段；**没声明**（插件没装 / 停用）
  时也会显示类型名，只是名字没那么好看。`title` / `depict` / `srcDoc` 也有长度上限（120 / 400 / 1000，超出截断）。
- 声明跟着插件生命周期走（停用 / 卸载 / 关『拟声++』自动撤销），不写入磁盘；重新启用时脚本会再声明一遍。
- `musicxx.source.list` 的 `resolveChain` 与 `musicxx.feature.point.list` 里看
  `musicxx.player.source.resolve` 是同一份数据（命令行 `source.list`、MCP `ListMediaSources` 也读它）。

```js
await musicxx.capability.call("musicxx.source.define", {
    id: "plugin.my_plugin.flac", title: "我的高解析音源",
    tag: "FLAC",                 // 歌单里的标签：显示成 [FLAC]
    srcDoc: "track/<歌曲 id>"
});
await musicxx.capability.call("musicxx.player.playSrc", {
    src: "track/1", type: "plugin.my_plugin.flac"
});
```

#### 3.2.16 应用前后台（`musicxx.app.*`）

| 能力 | 参数 | 返回 | 说明 |
|---|---|---|---|
| `musicxx.app.foreground` | 无 | `{ok:true, requested:"foreground", foreground}` | 把主窗口显示到前台（移动端把应用拉到前台）。与命令行 `window.foreground` 是同一份实现 |
| `musicxx.app.background` | 无 | `{ok:true, requested:"background", foreground}` | 把主窗口隐藏到后台（移动端退到后台），播放不受影响。与命令行 `window.background` 是同一份实现 |

```js
await musicxx.capability.call("musicxx.app.background");        // 隐藏窗口（用户看不见，但仍在播放）
const state = musicxx.broadcast.read("musicxx.app.lifecycle");   // "foreground" / "background"
const off = musicxx.broadcast.subscribe("musicxx.app.lifecycle", (v) => musicxx.host.log(2, "现在：" + v));
```

- 切完**不要**假定状态已经变了：真正的状态由应用的生命周期事件决定，变了之后会发布广播并刷新
  广播 `musicxx.state.app`（`foreground` / `lifecycleState`），订阅回调里读到的才是新值。
- 要"进后台 / 回前台"做事，用广播频道（§2 / §4）而不是在这里轮询能力；要当前值 `read`
  `musicxx.app.lifecycle`（可订阅）或同步读 `musicxx.state.app`。
- 用户可能正在看着窗口：隐藏前先想清楚（这类能力是"替用户按一下按钮"，不是给插件自己让路的）。

### 3.3 插件网盘：让应用提供目录列表与挑歌界面

内置网盘（百度 / 阿里 / 115 / WebDAV / Subsonic / Jellyfin / Emby / 本地目录）共用一套目录列表：
路径栏、排序、搜索、勾选、整目录递归（最多 5 层）、加歌单 / 加播放队列、下载、重命名、
新建目录、删除、条目信息。**插件不需要自己画这些**：声明一个可浏览来源 + 实现 `drive.list`，
「添加歌曲」菜单里就会多一行，点开就是同一个弹窗。

**一个音源类型可以有多个分类**（界面上是可切换的一排 tab，对齐内置 Emby 的
「歌曲 / 歌手 / 专辑 / 歌单」）：`musicxx.source.browse` **可以重复调用**，`id` 相同、`view`（分类 id）
不同，每次给一套自己的标题 / 图标 / 根目录 / 页大小 / 排序方式 / 索引栅 / 读写能力。
分类**不进音源身份**：歌曲里存的仍是类型 id，缓存目录、播放记录、`[标签]` 不会因为分类而分叉。

```js
// 同一个类型声明几个分类就有几个 tab（跟着插件生命周期撤销，不写磁盘）
const TYPE_ID = "plugin.my_ftp.audio";

// 「歌曲」：一屏歌曲，支持搜索
musicxx.capability.call("musicxx.source.browse", {
    id: TYPE_ID, view: "songs", title: "歌曲", order: 10,
    root: "/", search: true,
    capabilities: { list: "drive.list", search: "drive.search", resolveSongs: "drive.resolveSongs" }
});
// 「歌手」：字母索引栅，只允许按名称 / 歌曲数排序，不能下载
musicxx.capability.call("musicxx.source.browse", {
    id: TYPE_ID, view: "artists", title: "歌手", order: 20,
    root: "/artists", indexBar: true, sort: ["name", "songCount"], download: false
});
// 「专辑」：同一个处理器按 view 分支就够了；也可以给每个分类换自己的能力名
musicxx.capability.call("musicxx.source.browse", {
    id: TYPE_ID, view: "albums", title: "专辑", order: 30,
    root: "/albums", indexBar: true, sort: ["name", "createdTime"],
    capabilities: { list: "drive.list.albums" }
});

// 列一页（必需）：载荷里的 view 告诉你这是哪个分类在要数据
musicxx.capability.register("drive.list", async function (args) {
    // args: {requestId, type, view, path, offset, limit, sort, reverse}
    const page = await listOf(args.view, args.path, args.offset, args.limit);
    return { ok: true, entries: page.items, next: page.hasMore ? "2" : "" };
});

// 撤销：只撤一个分类，或者撤这个类型的全部分类
musicxx.capability.call("musicxx.source.undefine", { id: TYPE_ID, view: "albums" });   // 只撤「专辑」
musicxx.capability.call("musicxx.source.undefine", { id: TYPE_ID });                  // 全撤（类型声明也撤）
```

用途分工是固定的：

| 能力 | 管什么 | 可以调几次 |
|---|---|---|
| `musicxx.source.define` | **类型**：身份、列表 `[标签]`、播放时的解析说明 | 一个类型一次（重复 = 覆盖） |
| `musicxx.source.browse` | **分类**：界面上的一个浏览入口 | 一个分类一次，同类型可多次 |

只 `define` 不 `browse` 的类型只能播放、不能浏览挑歌；只 `browse` 不 `define` 也能浏览，
但列表里没有 `[标签]`、也没有类型说明。

最简单的用法（单分类，等价于 `view: "default"`）：

```js
// 1) 声明：本插件的这个音源类型可以浏览（跟着插件生命周期撤销，不写磁盘）
musicxx.capability.call("musicxx.source.browse", {
    id: TYPE_ID,                    // 必填：自己命名空间下的音源类型 id
    title: "FTP 文件",              // 菜单 / 页面标题
    root: "/",                      // 根目录（插件自己的标识）
    icon: "resource/svg/ftp.svg",   // 菜单里那一行的图标（相对插件目录）
    search: true,                   // 有搜索按钮
    capabilities: {                  // 出现某个键 = 这个能力实现了（名字可以改成自己的）
        list: "drive.list",
        search: "drive.search",
        resolveSongs: "drive.resolveSongs"
    }
});

// 2) 列一页（必需）
musicxx.capability.register("drive.list", async function (args) {
    // args: {requestId, path, offset, limit, sort, reverse}
    const page = await myList(args.path);              // 自己的列目录
    return {
        ok: true,
        entries: page.items.map((it) => ({
            name: it.name,
            path: it.path,          // 插件自己的标识，应用只原样回传
            isDir: it.isDir,
            size: it.size || 0,
            // 文件要带 song（类型 + 资源 id），应用才按歌曲行画、才能加进歌单
            song: it.isDir ? undefined : {
                type: TYPE_ID,
                src: it.resId,
                fileName: it.name,
                size: it.size || 0
            }
        })),
        next: page.hasMore ? "2" : ""    // 下一页偏移；空串 = 没有下一页
    };
});

// 3) （可选，强烈建议）整目录一次读歌：勾了文件夹就能一次拿到里面的歌
musicxx.capability.register("drive.resolveSongs", async function (args) {
    // args: {requestId, paths, recursive, maxDepth}
    return { ok: true, songs: await mySongsUnder(args.paths) };
});
```

声明的参数：

| 参数 | 必填 | 说明 |
|---|---|---|
| `id` | 是 | 音源类型 id，必须形如 `plugin.<自己的插件 id>.<类型名>` |
| `view` | — | **分类 id**：只允许 `a-z 0-9 _ -`，缺省 `default`；同一类型内唯一 |
| `title` | — | 分类名（界面上的 tab 文字，缺省用 `view`） |
| `icon` | — | 分类图标（**相对插件目录**的 svg，写法与 UI 项一致）；用在「添加歌曲」那一行 |
| `order` | — | 同一类型内的顺序（升序，缺省 0；同值按声明顺序） |
| `root` | — | 这个分类的根标识，缺省 `/`；应用只当字符串传回来 |
| `pageSize` | — | 建议一页多少条（只要大于 0，不再有上限；缺省 100） |
| `sort` | — | 这个分类允许的排序方式，缺省 `["name","size","createdTime","changedTime"]`；`[]` = 不要排序按钮 |
| `indexBar` | — | 是否给字母索引栅（歌手 / 专辑这类列表用），缺省 false |
| `write` | — | 允许新建目录 / 重命名 / 删除（缺省 false，那三个入口不出现） |
| `search` | — | 有搜索（缺省 false；也可以靠 `capabilities.search` 表达） |
| `addFolderSongs` | — | 勾文件夹「整目录加歌」那个菜单项（缺省 true） |
| `download` | — | 条目菜单里的「下载」（缺省 true） |
| `capabilities` | — | 覆盖能力名；出现某个键 = **这个分类**实现了这个能力 |

- 应答：`{ok:true, type, view, title, root}`；不合法是 `{ok:false, error:"一句能看懂的原因"}`。
- 同一个 `(id, view)` 再声明一次 = 覆盖（tab 位置不变），不同 `view` = 追加；
  **分类数量不限、`view` / `title` / `root` / `icon` 也不限长度**（`view` 仍只能用 `a-z 0-9 _ -`）。
- `sort` 只能是 `name` / `size` / `createdTime` / `changedTime` / `artist` / `album` /
  `duration` / `songCount`；写别的会被拒（不会静默忽略）。
- 允许两个分类共用同一个 `root` 与同一套能力名（技术上没问题，只是两个 tab 看起来一样）。

应用会问的能力（都是普通能力调用，参数里带 `requestId`，请原样放回返回里；
**`type` 与 `view` 恒有值**，`view` 永远不是空串）：

| 能力 | 什么时候问 | 入参 | 返回 |
|---|---|---|---|
| `drive.list` | 加载一个目录（每个分类必需） | `{type, view, path, offset, limit, sort, reverse}` | `{ok:true, entries:[…], next}` |
| `drive.resolveSongs` | 勾了文件夹要一次读完里面的歌 | `{type, view, paths, recursive, maxDepth}` | `{ok:true, songs:[…], truncated?}` |
| `drive.search` | 搜索框 | `{type, view, keyword, path, limit, offset}` | `{ok:true, entries:[…], next}` |
| `drive.mkdir` | 「创建文件夹」 | `{type, view, parent, name}` | `{ok:true, path}` |
| `drive.rename` | 「重命名」 | `{type, view, path, newName}` | `{ok:true, path}` |
| `drive.remove` | 「删除」（应用先弹确认） | `{type, view, paths:[…]}` | `{ok:true, removed}` |
| `drive.info` | 「歌曲信息」 | `{type, view, path}` | `{ok:true, title, pairs:[{k,v}]}` |
| `drive.open` | 点歌曲那一行（不给就用应用的默认行为） | `{type, view, path, isDir}` | `{ok:true, played:true}` |

`path` 的含义由分类决定（根就是这个分类的 `root`），应用**只原样回传、不解析**，
所以「歌手」分类的 `/` 与「歌曲」分类的 `/` 互不相干。

约定与上限（应用那一侧已经做，插件不用重复）：

- **分页**：`offset` 缺省是空串（第一页），`next` 给空串 / 省略表示没有下一页；含义由插件
  决定（页号、游标、字节偏移都行），应用只回传。单页条目超过 `limit` 会被截断。
  **不分页的来源（例如直接把一个目录读完的 FTP）什么都不用做**：不给 `next` 就是“只有这一页”，
  列表底部不会出现「加载更多」，也不会响应上拉加载。
- **排序**：`sort` 的取值见上面的声明列表，`reverse` 是布尔。插件排不了就忽略；应用拿到后仍会
  本地再排一次（与内置网盘一致）。每个分类**各自记住**用户选的排序方式（重启后仍是各自
  的选择），所以「歌手」按名称、「歌曲」按添加时间互不影响。
- **条目**：`path` 必填（空的那一条会被丢掉）；`name` / `artist` / `fileName` / `cover` 都**不限长度**
  （早期版本的 1 KiB / 2 KiB 截断已移除）；同一页里重复的 `path` / `song.src` 只留第一条。
  目录项不要给 `song`。
- **歌曲**：`song` 里 `type` 省略时用这个来源自己的类型；`src` 必填（插件自己的资源 id，
  播放时由 `musicxx.player.source.resolve` 换地址）；`cover` 可以是 `http` 链接或**相对插件
  目录**的路径；只给 `fileName` 时应用按文件名拆歌名与歌手（与本地扫描同一套规则）。
  **同一首歌从两个分类加进歌单，音源身份是同一个**（缓存、播放记录、封面都不分叉）。
- **失败**：返回 `{ok:false, error:"人能看懂的一句话", retryable?}`。`drive.list` 失败时
  列表显示“加载失败，点此重试”；`drive.resolveSongs` 失败时应用退回逐层 `drive.list`。
- **写操作**：只有声明了 `write: true` 才出「新建文件夹 / 重命名 / 删除」；`drive.info` 单独
  声明（给了才出「条目信息」）。删除是**应用先弹确认**再调 `drive.remove`。
- **下载与封面**：能不能下载、能不能放都走 `musicxx.player.source.resolve`（下载同样是"先解析
  成地址再取字节"），所以不实现解析的插件既放不了也下不了。条目没给 `cover` 时，歌单里那一行
  要等这首歌**播过一次**（应用按音源身份把封面存进图片缓存，键是
  `md5(类型 + 资源 id)`）之后才显示封面。
- **限制**：整目录递归最多 5 层；`drive.resolveSongs` 返回多少首就收多少（不再有上限）；同一个插件的
  同名能力应用会串行调用（最多排 2 条），多个分类共用同一个排队队列，不要在处理器里长时间阻塞。

宿主提供的界面能力（插件自己的界面里想让应用开挑歌弹窗时用）：

| 能力 | 参数 | 返回 |
|---|---|---|
| `musicxx.ui.pickSongs` | `type`（必填）、`view?`（浏览哪个分类，缺省第一个）、`title?`、`path?`、`pid?`（目标歌单，缺省当前播放列表）、`multiple?`（缺省 true）、`allowSelectDir?`（缺省 true）、`search?`（缺省 true）、`pageSize?` | `{ok, added, skipped, cancelled, view}` |
| `musicxx.ui.pickDir` | 同上（`multiple` 缺省 false、`allowSelectDir` 恒为 true） | `{ok, paths, items:[{view, path}], cancelled, view}` |
| `musicxx.ui.browseDrive` | `type`（必填）、`view?`（起始分类）、`path?`（起始目录） | `{ok, reason:"opened", type, view}`（**不等用户关页面**） |

```js
// 挑歌并加进当前歌单（插件自己的按钮里用）
musicxx.capability.call("musicxx.ui.pickSongs", { type: TYPE_ID, view: "artists" });   // 不要 await

// 在自己插件的主页入口里打开整页浏览（与内置网盘页一模一样）
musicxx.ui.registerEntry({
    name: "drive", type: "home.entry",
    data: { title: "我的 FTP", icon: "resource/svg/ftp.svg",
            action: { kind: "command", name: "musicxx.ui.browseDrive",
                      args: { type: TYPE_ID, view: "songs" } } }
});
```

- 弹窗**一次只浏览一个分类**：打开哪个分类由 `view` 决定（不给就是这个音源类型的第一个分类），
  弹窗里没有切换。返回值里的 `view` 是这次浏览的分类；`pickDir` 的 `items` 每条另带
  `{view, path}`（目录的 `path` 只在它自己的分类里有意义）。
- `pickSongs` / `pickDir` **要等用户关掉弹窗才完成**：在能力处理器里调它们时**不要 await**
  （会与应用实现的能力互相等，和 `musicxx.ui.openOverlaySheet` 同一个问题）。`browseDrive` 相反：
  它打开页面后立即返回 `{ok, reason:"opened", type, view}`，不等用户关页面。
- 插件调它们只能打开**自己声明**的来源（`type` 必须是自己的）；应用自己调（如菜单里那一行）
  没有这个限制。
- 推荐用法：「添加歌曲」里那一行由应用自动生成（**一个音源类型一行**，副标题列出分类），
  插件不用注册 `addsong.entry`（自己画浮层的写法仍然可用，见 `plugins/example_ftp_source`）。
- 自省：`musicxx.source.list` 的 `browse` 段按类型分组，每组带 `entries`（每个分类一行：
  `view` / `title` / `order` / `root` / `pageSize` / `sort` / `indexBar` / `write` / `search` /
  `addFolderSongs` / `download` / `capabilities`）。

### 3.4 名字存在、但当前版本没有实现的能力

调用这些会立刻得到 `action_not_registered`（**别把功能押在它们上面**）：

`musicxx.player.setMediaType` · `musicxx.ui.setEntryBadge` · `musicxx.host.shareText`

从未实现的旧名字（`musicxx.library.querySonglist` / `removeSong` / `createSonglist` / `setSongInfo`）
现在都有实现的新名字（`musicxx.playlist.querySongs` / `removeSong` / `create` / `updateSong`），
但旧名字本身仍没注册；`musicxx.library.search`（站内搜索）没有替代名字，仍未实现。

要“改应用状态”的能力缺口，用对应的**能力**补：例如想改播放速度 / 音调用能力
`musicxx.player.setSpeed` / `musicxx.player.setPitch`（已实现）；想拦一次设置变更用能力
`musicxx.player.speed` / `musicxx.player.pitch`（已接入，见 [plugin-capabilities.md](plugin-capabilities.md)）。

---

## 4. 没有当前值的广播（`cache: none`）：已经发生的事

> **JS 写法**：`musicxx.broadcast.publish` / `subscribe` / `unsubscribe`（与其它频道同一组入口，
> 按频道声明区分投递方式）。以下是两边的写法：

```js
musicxx.broadcast.subscribe("musicxx.state.song", (song, topic) => { /* ... */ });
musicxx.broadcast.publish("plugin.my_plugin.hello", { at: Date.now() });   // 只能发自己命名空间
musicxx.broadcast.unsubscribe("musicxx.state.song");
```

```cpp
subscribeTopic(MUSICXX_BROADCAST_STATE_SONG, [](std::string_view json) { /* 载荷 JSON */ });
// 发布：频道归属由插件框架校验
PluginxxStringView channel = pluginxxView("plugin.my_plugin.hello");
PluginxxStringView data    = pluginxxView(R"({"at":1})");
iface.broadcasts->publish_broadcast(host, &channel, &data, nullptr);
```

规则：

- **订阅**：官方 `musicxx.*` 主题与其它插件的 `plugin.<id>.*` 主题都能订；
  同一主题**只建立一条 host 订阅**（JS 侧再 `subscribe` 是复用，不是叠加）。
- **发布**：只能发官方主题或**自己命名空间**（`plugin.<自己的插件 id>.*`）；主题非法时
  JS 的 `publish` 直接抛异常、C++ 返回非 0。
- 插件之间协作：单向通知用广播，要返回值用能力调用（§6）。
- **自己的频道不必申报**：广播频道表只是一份**预设**（常用频道带标题 / 属性），表里没有的 id 照样能发、能订，
  默认属性是「没有当前值、只有属主能发布、允许合并」；网络请求的进度 / 完成约定叫 `plugin.<你的插件 id>.net`。
  名字只能落在自己的命名空间里，发别的插件或官方频道都会被拒。
- 订阅回调**要快**：它跑在插件框架线程 / JS 线程上，慢就是别人一起等。

常用频道（都在**插件总线上**，可以订阅）：

| 频道 | 什么时候来 | 载荷 |
|---|---|---|
| `musicxx.broadcast.subscribers` | 某个频道的订阅数变了（插件框架回传，应用用它做自省与 `cache: none` 的闸门） | `{id, subscribers}` |
| `plugin.<其它插件 id>.*` | 对方插件发布 | 由对方定义 |

> **下面这些名字不是总线频道**：它们是插件框架 → 应用的内部事件（与能力派发、UI 项、日志同一条内部队列），
> 插件 `subscribe` 它们**不报错但永远收不到回调**：`musicxx.plugin.log` / `musicxx.plugin.error` /
> `musicxx.plugin.warn` / `musicxx.ui.changed` / `musicxx.capability.changed` / `musicxx.capability.decision.result`。
> 要看这些数据用别的办法：能力注册表用能力 `musicxx.capability.subscribers`，自己的 UI 项用 JS 的
> `musicxx.ui.entries()`，写日志用 `musicxx.host.log` / `console.*`；异步裁决的结果就是
> `await dispatchAsync` 的返回值（`musicxx.capability.decision.result` 只是同一份结果的调试回传）。

> 应用的 `musicxx.song.changed` 之类也是**广播**上的频道；**能力**是另一条通道
> （有阶段、有合并策略、可能等裁决）。别把两者混起来：想在切歌时做事，订阅广播频道
> `musicxx.song.changed`；想拦一次切歌，实现能力 `musicxx.player.beforePlaySong`；
> 想在状态变了刷新自己的显示，订阅对应的状态广播（例如 `musicxx.state.song`）。

### 4.1 结束一条流（`end`）

频道可以表示一段**有限的东西**（一次任务的结果流、一次会话的通知流）。属主可以把它**结束**：

- 结束之后：**发布被拒**（广播表返回 `-5`，`PLUGINXX_RESULT_ERR_ENDED`；JS 侧 `publish`
  抛错），`read` 拿不到当前值、全部订阅先用 `reason` 回调一次再摘掉；
- **再次声明同一个 id = 开一条新流**（清掉结束标记与缓存）；重复结束是幂等成功；
- 只有属主能结束：别人的频道（含官方 `musicxx.*`）结束不了。

```cpp
// 订阅时多给一个结束回调（动态库插件）
subscribeTopicWithEnd("plugin.my_plugin.task",
                      [](std::string_view progress) { /* 值 */ },
                      [](std::string_view reason)   { /* 属主给的 reason JSON */ });

// 结束时：只有属主能结束自己的频道
endTopic("plugin.my_plugin.task", R"({"reason":"done"})");
```

```js
// JS 侧：subscribe 的第 4 个参数是结束回调（短名会自动补成 plugin.<本插件 id>.）
musicxx.broadcast.subscribe("task", function (progress) {
    /* 值 */
}, null, function (reason) {
    /* 属主给的 reason JSON */
});

// 结束时（只有属主能结束）；失败会抛错，错误信息里写着原因
musicxx.broadcast.end("task", { reason: "done" });
```

返回码（动态库侧，即广播表）：`0` = 已结束（重复结束也是 `0`）；`-3` = 不是自己的命名空间；
`-1` = 频道不存在 / 不是这条频道的属主（原因同时写进日志）。JS 侧的 `end` 走插件框架的
错误码（`-2` = 插件登记还没回放完，`-4` = 频道不存在 / 不是属主，`-6` = 不是自己
命名空间下的频道），并统一折成抛错，脚本里用 `try / catch` 接。

> `-5` 是**广播表**的结果码，与插件框架自己的错误码（那里的 `-5` 是超时）不是一套，
> 两套常量不要混着判。

---

## 5. 官方广播频道（应用是属主，插件只能读与订阅）

> **这一节讲应用发布的、带当前值的官方频道**：`musicxx.app.lifecycle`、
> `musicxx.ui.animatedLevel` / `particleAnimate` / `songIconRotate` / `songIconWave` / `fontFamily`、
> `musicxx.theme.available` / `current` / `mode`、`musicxx.player.position`。它们都是 `cache: latest`：
> 订阅即拿到当前值，也能同步读缓存。**完整目录、字段与取值域见
> [plugin-broadcasts.md](plugin-broadcasts.md)**（生成物）。
>
> **JS 写法**：`musicxx.broadcast.read` / `subscribe` / `info` / `list`。
> **原生写法**：`broadcastJson(<频道宏>)` / `subscribeTopic(<频道宏>, fn, options)`。
>
> **广播是只读的**：插件不能写应用的频道。要改应用的状态调对应的能力（例如
> `musicxx.ui.setAnimatedLevel` / `musicxx.theme.setCurrent`）；插件自己的状态放私有存储，
> 要给别人看就发布自己命名空间下的频道（§4）。

**定位**：应用自己的一小块状态 —— 插件要读它、跟着它变。真值在应用那里，
插件框架只保留一份服务同步读的缓存（应用发布时更新、订阅时补当前值）。

与邻居的分工（选哪条通道看这里，别硬凑）：

| 需要 | 用哪条 |
|---|---|
| 每秒多次读的实时数据（播放进度、频谱槽位） | 带当前值的广播（§2、本节） |
| 一次性命令（播放、发通知、开页面） | 能力（§3） |
| 广播"发生了某事"（没有当前值） | `cache: none` 的广播（§4） |
| **想改应用的状态**（动画等级、主题、字体、播放位置…） | **能力**（§3），改完应用自己会发广播 |
| 能力处理器 / 热路径里**同步**判断 | 带当前值的广播 + `read`（同步走缓存） |

### 5.1 读当前值

同步读，走插件框架里的缓存（没有往返）；没发布过就是空。

| 写法（JS） | 原生侧 |
|---|---|
| `musicxx.broadcast.read(id)` | `broadcastJson(id)`（字符串；没有当前值时是空串） |

```js
const level = musicxx.broadcast.read("musicxx.ui.animatedLevel");      // "medium"
const inFront = musicxx.broadcast.read("musicxx.app.lifecycle") === "foreground";
```

```cpp
const std::string level = broadcastJson(MUSICXX_BROADCAST_UI_ANIMATED_LEVEL);
```

`cache: none` 的频道（`musicxx.player.state` 这类事件）永远没有当前值：`read` 返回 `null` / 空串，
要知道它们发生就订阅（§4）。

### 5.2 订阅变化

`pushOnSubscribe` 的频道在订阅时先补一次当前值（算第一条通知，也占限频窗口的起点）；
每个订阅方自己声明通知频率（`intervalMs` / `mode`），互不影响：

```js
const off = musicxx.broadcast.subscribe("musicxx.player.position", (pos, info) => {
    draw(pos.positionMs, pos.durationMs);
}, { intervalMs: 500, mode: "delay" });     // 每 500 ms 最多一条，只留最新
```

```cpp
const int32_t sub = subscribeTopic(MUSICXX_BROADCAST_PLAYER_POSITION,
    [](std::string_view json) { /* 载荷 {sid, positionMs, durationMs} */ },
    PluginxxHost::SubscribeOptions{500, "delay"});
```

- 第一条立即到达；窗口内 `delay` 只留最新一条到点补发、`drop` 直接丢；
- `intervalMs` 缺省 / `0` = 不限频（应用侧不再另做节流）；
- 播放进度是**不限频**广播：应用每报告一次位置就发一条，按自己需要的频率订阅即可；
- 解绑函数的句柄随插件停止自动撤销，不需要时调 `off()` 也行。

### 5.3 要改它：调能力

广播**不提供写入口**。对应的能力见 §3，例如：

| 想改 | 调能力 |
|---|---|
| 动画等级 | `musicxx.ui.setAnimatedLevel` |
| 粒子动画 / 歌曲图旋转 / 歌曲图波浪 | `musicxx.ui.setParticleAnimate` / `setSongIconRotate` / `setSongIconWave` |
| 全局字体 | `musicxx.ui.setFontFamily` |
| 当前主题 / 切换模式 / 临时切一下 | `musicxx.theme.setCurrent` / `setMode` / `tempSwitch` |
| 播放位置 | `musicxx.player.seek` |

写完之后应用会发一次对应的广播，订阅了就能看到新值（不需要自己维护副本）。

---

## 6. 插件服务（能力：给别的插件用）

**能力 = "应用或别的插件按名字调用本插件"**，参数与结果由插件自己定。

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
- 超时缺省 3 秒（调用方自己给）；**传 `0` / 负数 = 不设超时（一直等）**，不是“仍按 3 秒”。
- 失败原因：`plugin_capability_not_found: <名字>` / `capability_call_failed` / `跨插件调用超时: ...`。
- **处理器里不要等应用实现的能力**：打开插件页面 / 点按钮时主程序在等能力结果，而应用实现的能力要由应用线程执行，
  两边互等会让**整个应用卡住几秒**。要"现在这一帧"的数据读带当前值的广播；要能力的结果就异步读回来、
  记进自己的状态，由页面下一次调用时回读。
- 插件页面就是能力：主程序打开 `ext://<插件id>/<视图id>` 时调用**同名能力**（见 §9.1）。

写给谁看：

| 目标 | 行为 |
|---|---|
| JS 插件 | 在共享 JS 线程上**直接调用**：处理器同步返回时结果立刻就有，返回 Promise 时等它结算 |
| 动态库插件 / 内置目标 | 投递到框架线程执行，脚本不阻塞；结果经事件回到 JS 线程 |

---

## 7. 功能埋点（能力：谁都能实现的一个 id）

功能埋点是"**这个功能谁都能实现**，应用与别的插件按 id 来调"。与能力的区别：

| | 能力 | 功能埋点 |
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

- **回答必须用 `patch` 包一层**（裸字段会被后面的处理器覆盖）：写能力处理器时是
  `{"patch":{"value":…}}`；`provide` 的糖会替你包。
- **按 `point` 过滤 + 不回答自己发起的调用**：同一条通道上所有实现都会收到同一次调用，
  `caller` 等于自己的插件 id 时要返回空（糖自动做这两件事）。
- **`value` 键的存在 = 实现方给出了结果**（值可以是 `null`，表示"算过了、结果是空"）；返回空串 / `undefined` = 不回答。
- **处理器必须同步返回**：功能埋点走同步裁决通道，返回 Promise / `Task<T>` 拿不到结果（会记一条日志）。
  长任务请提前算好，或做成**能力**（§6）。
- **前链 / 后链**：同一个点在通道上还有 `before` / `after` 两个阶段（同一条 id，用 `phase` 区分）——
  前链改这次调用的 `args`（或 `{"action":"cancel"}` 拦下这次），后链改最终 `value`（或
  `{"action":"drop"}` 这次没有结果）；只有 `impl` 阶段算"有人实现"。`provide` 的糖只登记实现链，
  要插手前后链直接用能力 API（`musicxx.capability.implement("musicxx.feature.point.call", {phase:"after"}, fn)`）。
  概念与顺序见 §8.1。
- 声明跟着插件生命周期走：禁用 / 卸载会撤销全部声明，重新启用时脚本 / `start` 里再声明一遍。
- 同一个功能埋点 + 同一个调用方的上一次调用没结束时，调用方拿到 `busy`（不排队）。

应用**留出的、可以被主动调用**的功能埋点有三个（`capability.list` 的 `callDoc` 写着参数与结果形状）：

| 功能埋点 id | 作用 | 调用的代价 |
|---|---|---|
| `musicxx.media.spectrum.extract` | 整曲频谱（帧数 × 256 个频点 + 每帧响度） | 要解码整曲，慢（缺省超时 60 秒） |
| `musicxx.media.chorus.analyse` | 副歌（高潮）区段 | 会先取一次频谱，慢 |
| `musicxx.media.palette.provide` | 封面取色（5 个色槽位） | 要分析一张图 |

---

## 8. 应用来问插件（能力：决策型）

> **登记入口（插件 ABI 5）**：`musicxx.hooks` 表、`MusicxxPluginHookSpec` 与 `hook` 词汇都已删除。
> JS 用 `musicxx.capability.implement`；原生插件往 `musicxx.capabilities` 登记（SDK 的
> `implement` / `implementBefore` / `implementAfter` / `unimplement` 就是这么做的）。
> **通知型的那一批**是一组广播频道，JS 用 `musicxx.broadcast.subscribe`，原生用 `subscribeTopic`。
> 本节剩下的阶段、裁决对象与载荷语义对两种形态同样有效。

能力 id、模式（`observe` / `decision`）、派发方式（`sync` / `async`）、合并策略，以及**哪些能力已经被应用接入**，
都在生成物 [plugin-capabilities.md](plugin-capabilities.md) 里；本节说"怎么写、为什么这么写"。

### 8.1 三个阶段：前链 / 实现链 / 后链

**能力 id 表示一次调用；参与方式由注册时的 `phase` 表达。** 不要再找 `.before` / `.after` 后缀的 id ——
那些成对的 id 已经合并成一条能力上的两个阶段。

| 阶段 | 什么时候跑 | 能做什么 | 不能做什么 | 回答格式 | 结束整条链 |
|---|---|---|---|---|---|
| `before` | 进入调用之前，串行、**正序** | 改输入（累加）、拦下（这次不执行实现） | 给数据（那是实现链的事） | `{"patch": {...}}` | `{"action":"cancel"}` |
| `impl` | 实现链，串行 | 给结果 / 裁决 | 改输入 / 改输出 | 与以前一样（§8.2） | 按声明的合并策略（`first` 下任一回答都结束本轮遍历）；`cancel` 一律停止 |
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
- 同一个插件对同一条能力可以分别注册三个阶段（同一个 `ownerTag` 互不覆盖）；
  `unregister` 按 `(能力 id, ownerTag)` 摘，一个 `ownerTag` 在所有阶段的登记一起摘掉；
- **前链 / 后链必须是同步处理器**（要拿到这一步的结果才能跑下一步）：用异步处理器注册会被拒绝；
- 只能注册能力**声明过的**阶段（生成物 `plugin-capabilities.md` 的「可注册阶段」列）；没声明就注册会被
  拒绝（C++ 返回 `-1` 并记日志，JS 在后台拒绝并记日志），因为“注册成功但永远不触发”
  比报错更难查；
- 三种“没有结果”含义不同，别混：`{"action":"skip"}` 是**这一份实现**这次不参与（应用按“没有结果”
  接着问下一层实现；同一条能力的处理器列表里它仍算一次回答，`veto` 下就此结束遍历）；`{"action":"cancel"}`
  是**拦下这次调用**（前链：实现不跑；实现链：不再问任何实现，这次没有结果）；`{"action":"drop"}`
  是后链把已经算出来的结果作废。
- **一个处理只有一个词**：拦下这次调用 = `cancel`（前链 / 实现链通用），这一份不参与 = `skip`，
  结果作废 = `drop`。`stop` 只保留在 `musicxx.player.error`（= 停止播放），它不是阶段词。
- **载荷里不自动带 `phase`**：同一个函数要服务多个阶段就各注册一份自己的闭包，不要指望从载荷里
  分辨阶段。唯一例外是插件功能埋点的通用通道 `musicxx.feature.point.call` —— 它的载荷带 `phase`，
  因为一个插件点可能用同一段代码处理三个阶段（见 §7）。

包装点（网络请求 / 下载 / 歌单读取这类「实现固定是应用自己」的能力）没有 `impl` 阶段，
插件只在前后插手；取值点（频谱 / 副歌 / 取色 / 音源解析）通常只有 `impl`。

**“改播放值”的四个能力是拦截点，不是可接管的能力**：`musicxx.player.seek` / `volume` /
`speed` / `pitch` 的实现固定是应用自己（声明里 `impls: one`、`phases` 只有 `before` / `after`），
因为“把播放器改到这个值”是应用做的事，不能让插件换掉（多插件时用户看到的“拖到 50% 却停在 30%”
无法解释）。插件在这四个点上的位置：

| 阶段 | 能做什么 | 回答 |
|---|---|---|
| `before` | 改这次的目标值 / 拦下（这次不改，界面回弹） | `{"patch":{"to":0.5}}` / `{"action":"cancel"}` |
| `after` | 改最终值 / 把这次的值作废（值已经改过，拦不下） | `{"value":{"patch":{"to":0.5}}}` / `{"action":"drop"}` |

注册它们的 `impl` 会被拒（C++ 返回 `-4`，日志写“does not open phase impl”）。
后链在这四个点上**一定被调用**（以前只在插件给了裁决时才会跑）；`drop` 之后应用按
`before` 之后的目标值执行 —— 不要把它当“取消这次操作”，要取消请用 `before` 的 `cancel`。

### 8.1.1 注册与注销

```js
musicxx.capability.implement(id, { phase, priority, ownerTag }, fn);  // 顶层同步注册或运行期注册都行
musicxx.capability.unregister(id);           // 摘掉这条能力上自己的全部阶段
musicxx.capability.unregister(id, "after");  // 只摘那个阶段
musicxx.capability.has(id);                  // 自己注册了没有（缺省看 impl 阶段）
musicxx.capability.has(id, "after");         // 某个阶段注册了没有
musicxx.capability.handlers(id);              // Promise：谁在提供这条能力（§3.2.12）
musicxx.capability.handlers(id, "after");     // 只看一个阶段（缺省 = 全部阶段）
```

```cpp
implement(MUSICXX_CAPABILITY_PLAYER_BEFORE_PLAY_SONG, 0,   // 裁决型，priority = 0
          [](std::string_view input, std::string& out) -> int32_t {
              if (input.find("广告") != std::string_view::npos) { out = R"({"action":"skip"})"; }
              return 0;   // 返回 0 且 out 非空 = 给出裁决；out 空 = 不裁决
          });

observe(MUSICXX_BROADCAST_SONG_CHANGED,                    // 通知型（广播频道）
        [](std::string_view input, std::string&) -> int32_t { /* 返回值忽略 */ return 0; });

// 阶段: 同一条能力上的前链 / 后链（必须是同步处理器）
implement(MUSICXX_CAPABILITY_PLAYER_SEEK, CapabilityPhase::After, 0,
          [](std::string_view input, std::string& out) -> int32_t {
              out = R"({"value":{"action":"continue","patch":{"to":60000}}})";
              return 0;
          });
implementBefore(MUSICXX_CAPABILITY_PLAYER_SEEK, 0, ...);   // 等价写法
implementAfter (MUSICXX_CAPABILITY_PLAYER_SEEK, 0, ...);

unimplement(MUSICXX_CAPABILITY_PLAYER_SEEK);
```

- 只有两种参与方式：**实现**（`implement`，返回 `null` / 空 = 不表态）与**订阅广播**
  （`observe` / `musicxx.broadcast.subscribe`，返回值忽略）。没有第三个"模式"参数。
- `priority`：小者先执行，同优先级按注册顺序。应用询问"这个扩展点谁来实现"时的层间顺序是
  「**外置插件 → 内置插件 → 应用自己兜底**」：外置排最前是刻意的，用户装的插件要能替代内置功能；
  想知道某个功能埋点现在是谁在生效，用能力 `musicxx.feature.point.list`（§3.2.11）。
- `ownerTag`：同一插件在同一条能力上区分多个处理器用的标记。
  **不要用不同 `ownerTag` 注册同一条能力**：JS 侧只保留一个处理器、框架侧却留下多条注册，
  表现为处理器被重复调用。同一条能力只注册一次。
- **能力 id**：官方 `musicxx.*` 归应用（应用没声明的官方 id 会被拒，错误码 `-2`；写错 id 的表现是
  **处理器永远不被调用**）；自己造的能力用 `plugin.<自己的插件 id>.<名字>`，**不需要在声明表里**，
  注册即生效（宿主按 `plugin.<id>.` 判断归属，默认「多实现、三个阶段都开」）。
  频道 / UI 项 / 广播频道的 id 同一条规则。JS 的 `musicxx.capability.has(id)` 只看脚本自己注册没注册，
  不代表插件框架受理了。
- **只有"已接入"的能力会触发**：以 [plugin-capabilities.md](plugin-capabilities.md) 的「状态」列为准
  （那一列由 `tools/capabilities.def.json` 的 `implemented` 字段生成，别手数）：标「已接入」的会真的派发，
  标「未接入」的注册成功但不会触发 —— 别把功能押在未接入的能力上（它们打算做什么见 §8.7）。
  其中 `musicxx.feature.point.call` 是插件功能埋点的通用通道，由调用方触发、不是应用主动派发。

### 8.2 裁决对象

实现链（以及普通裁决型能力）的回答与以前一样：

```jsonc
{ "action": "continue" | "cancel",
  "patch": { /* 这条能力专属的字段 */ },
  "error": "可选说明" }
```

- 返回 `null` / 空对象 = “不裁决”，交给下一个处理器；`action` 的语义由各调用点决定
  （例如 `beforePlaySong` 的 `skip` = 跳过本曲，`musicxx.player.error` 的 `stop` = 停止播放）。
- **裁决是“建议”**：调用点会校验裁决的身份字段（如 `sid`），过期的裁决会被丢掉 ——
  异步派发的能力尤其要注意（等待期间可能已经切歌 / 切列表）。
- **处理器失败只记统计**：抛异常 / 返回失败不会暂停派发、也不会卸载插件。

前链 / 后链用自己的两种回答（见 §8.1）：

```jsonc
// before：只认 patch（一步接一步合并进本轮载荷），action=cancel 拦下整条链
{ "patch": { "to": 60000 }, "reason": "可选说明" }
{ "action": "cancel" }

// after：只认 value（替换当前结果），action=drop 结束整条链
{ "value": { "action": "continue", "patch": { "to": 60000 } } }
{ "action": "drop" }
```

- **`patch` / `value` 是一层包装**：其余字段只是顺带说明（`reason` 之类），不参与合并；
- `before` 的 `patch` 里能写什么由那条能力的载荷字段决定（嵌套对象递归合并）；
- `after` 的 `value` 键存在就算答了（值可以是 `null`，表示“算过了、结果是空”）；
- 没意见就返回空，不要为了“不改”返回一份原样的载荷。

### 8.3 异步裁决

```js
// musicxx.player.speed 的载荷是 {from, to}：把不合法的新值换成合法值
// （“改播放值”的四个能力实现固定，插件只能注册前链 / 后链）
musicxx.capability.implement("musicxx.player.speed", { phase: "before" }, (ctx) => {
    if (ctx.to >= 0.25 && ctx.to <= 3) return null;     // 同步路径：最省时
    return new Promise((resolve) => {                    // 异步路径
        setTimeout(() => resolve({ patch: { to: 3 } }), 20);
    });
});
```

- 插件框架**一直等到 Promise / 异步处理器结算**（框架侧没有等待超时）；结算后裁决生效。
- 但**调用点可能自己设了上限**：`musicxx.player.seek` / `volume` / `speed` / `pitch` 在界面操作路径上，
  应用最多等 **0.5 秒**（超时按“没意见”走原值），`musicxx.player.source.resolve` 最多等 5 秒，
  `musicxx.player.error` 等它结算。**每条能力具体等多久看 [plugin-capabilities.md](plugin-capabilities.md) 的「超时」列**。
- 所以异步路径要保证**一定会结算**（失败就 reject）；不结算就是调用点一直等到它的上限。
- 结算次数在统计里看得到（`asyncDispatchSettled`）。

### 8.4 处理器里不要做这些

- **不要阻塞**：网络、大文件、编解码、`sleep` 都不要直接在处理器里做 ——
  它跑在插件框架线程 / JS 线程上，慢就是业务与其它插件一起等。
  动态库插件用 `pluginxx::offload` / `pluginxx::sleep`；JS 用定时器或把工作拆成两段。
- **不要等应用实现的能力**（同步派发的能力尤其致命）：调用点就地等你的返回值，而应用实现的能力要由应用线程执行，两边互等。
- 必要的长任务提前算好（例如换歌时预热缓存），处理器里只做"查缓存 → 返回或返回空"。

### 8.5 取值型能力的"拦下"与"不提供"

频谱提取、副歌分析、封面取色这三个取值型能力可以拦下整次调用：

- 返回 `{"action":"cancel"}` = 拦下这次调用（应用自己的兜底也不会被问），应用会记一条日志并在自省里
  标出是谁拦的；处理器被摘除（停用 / 卸载 / 关闭『拟声++』）后自动还原。
- 返回 `null` / 空 / `{"action":"skip"}` = "这次不提供"（链条继续问下一个实现）。
- **不要拿 `cancel` 当"实现方还没算完"的挡箭牌**：那样用户会看到功能凭空消失。

### 8.6 音源解析与插件音源（`musicxx.player.source.resolve`）

应用在**把一个音源变成播放器能打开的地址与请求头**时派发这条能力（`decision` + `firstNonNull` +
异步）。顺序是 **外置插件 → 内置插件（云盘 / 媒体库）→ 应用自身（本地文件 / 链接 / Bili）**：
你注册了处理器就排在最前，既给自己的音源写地址，也能给内置来源换地址。

```js
musicxx.capability.implement("musicxx.player.source.resolve", { mode: "decision" }, (ctx) => {
    if (ctx.pluginType !== "plugin.my_plugin.flac") return null;   // 不是本插件声明的音源类型：不回答
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
- 返回空 / 结果不合法 = 这次不提供，应用接着问内置实现；返回 `{"action":"cancel"}` = 拦下这次调用
  （不再问别的实现），该音源会解析失败（只在明确知道它不该播时用）。

注意：

- **载荷里带来源字符串**（插件自己的音源就是你自己写下的资源 id，内置来源是已经解析出来的临时直链或
  本地路径）—— 这是"能换地址"的前提，注册这条能力的插件看得到这些内容。
- 应用最多等你 **5 秒**（超时按"没给"处理）；地址是临时直链时自己缓存好，处理器里越快越好。
- 观看型的 `musicxx.player.source.resolved` 只报"种类"，**不给地址**：要看播放是否成功用它。

### 8.7 未接入能力的名字说明

下面这些能力**约定已经定下来、主程序还没接**（注册不会触发，载荷字段也还没有定；
接入后会写进 `tools/capabilities.def.json` 的 `doc` 字段并生成到 `plugin-capabilities.md`）。
列在这里是让你知道"这个名字打算做什么"，别猜：

| 能力 id | 打算做什么 |
|---|---|
| `musicxx.app.deepLink` | 外部链接进来时让插件拦一下 / 处理 |
| `musicxx.app.exit` | 应用退出前让插件拦一下 / 做收尾 |
| `musicxx.app.upgrade` | 版本升级后的通知（做数据迁移） |
| `musicxx.player.qualityChanged` | 音质切换的通知 |
| `musicxx.media.notification` | 系统媒体通知的内容（标题/封面）让插件改写；用户可见的提示用能力 `musicxx.ui.toast` |
| `musicxx.media.info.analyse` | 让插件提供歌名 / 歌手 / 媒体信息识别（替代应用不联网的识别） |
| `musicxx.song.meta.writeback` | 歌曲信息回写时的通知 |
| `musicxx.song.beforeAdd` | 加歌前拦一下 |
| `musicxx.song.removed` | 歌曲被移除的通知 |
| `musicxx.playlist.load` | 歌单读取的前后插手（`after` 就是原来的“过滤”，能读到最终列表） |
| `musicxx.history.record` | 写播放记录时的通知 |
| `musicxx.local.scan.file` / `musicxx.local.scan.finished` | 本地扫描时逐文件裁决（跳过）/ 扫描完成通知 |
| `musicxx.lyric.current` | 当前歌词行变化通知（歌词加载已按 `musicxx.lyric.load` 接入，见 `plugin-capabilities.md`） |
| `musicxx.app.net.http.request` | 应用发外网请求的前后插手（前链改请求 / 取消，后链读改响应）；原 `musicxx.net.request`，改名是为了与插件框架自己的 `musicxx.net.*` 网络能力分开 |
| `musicxx.net.server.route` / `musicxx.net.mcp.tools` / `musicxx.net.lan.event` | 本机服务的路由交给插件、MCP 工具清单追加、局域网事件通知 |
| `musicxx.download` | 下载的前后插手（前链改文件名 / 目录或取消，后链拿成品路径） |
| `musicxx.cache.beforeTrim` / `musicxx.cache.pathRequest` | 缓存裁剪前裁决 / 缓存路径由插件指定 |
| `musicxx.ui.route.resolve` | 路由解析 |
| `musicxx.ui.page.enter` / `musicxx.ui.page.leave` / `musicxx.ui.theme.changed` / `musicxx.ui.user.action` | 页面进入 / 离开、主题变化、用户操作的通知 |

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
  `addsong.entry`（「添加歌曲」弹窗里的来源项）、
  `playing.background`（播放页背景）、`playing.icon`（播放页歌曲图）、
  `playing.visualizer`（播放页音频动效）、`playing.controlBar` 与
  `playing.controlBar.menu` / `.previous` / `.playOrPause` / `.next` / `.favorite`（播放页底部控制按钮栏，
  **替换**内置栏或其中的一个按钮，见 [plugin-ui.md](plugin-ui.md) §1.7）、以及十个播放页子页面插入块
  `playing.lyric.top` / `playing.lyric.bottom` / `playing.lyric.header` / `playing.lyric.footer` /
  `playing.moreInfo.header` / `playing.moreInfo.footer` /
  `playing.songlist.top` / `playing.songlist.bottom` / `playing.songlist.header` / `playing.songlist.footer`
  （**类型本身就是位置**，`data` 里不需要 `slot`；`top` / `bottom` 是固定区域，`header` / `footer` 在列表滚动内容里）。
- **页面内容是打开时取一次的**：改完配置要让能力再返回一次 `{view: …}`（或重新打开页面）。
- **入口 / 按钮指向的能力必须真的注册**：只声明入口不实现能力时，打开页面只会提示
  「插件『<插件id>』没有提供『xxx』」。
- 动作写法只有四种：`route` / `dispatch` / `command` / `none`。
  入口/菜单项里的 `route` 与能力 `musicxx.ui.openRoute` 走同一套规则：**允许指向任意插件**的页面
  （`ext://<插件id>/<视图id>`，含自己）。跨插件时页面由**对方插件**自己绘制，发起方拿不到对方的数据，
  所以只该跳"两边都装得上"的公共页面，跳之前用 `musicxx.plugin.list` 之类的办法确认对方在。
- **组件要按客户端能力降级**：`musicxx.host.info().ui`（`blocks` / `controls` / `kind`）告诉你这个
  客户端支持哪些块与控件；图标要插件**自己带 svg**（写相对插件目录的路径，能力段里没有图标清单）。
- 字重只来自 `bold: true` 或 `type: "title"`；块没有隐含边距，要留白自己写 `margin` / `padding`。

### 9.2 渲染槽位与着色器（完整说明见 [plugin-shader-bundle.md](plugin-shader-bundle.md)）

- 播放页背景（`playing.background`）、播放页歌曲图（`playing.icon`）与播放页音频动效
  （`playing.visualizer`）可以交给插件：`shader`（插件着色器绘制）、`view`（界面视图组合）、
  `none`（只保留占位 / 不显示）三选一。动效槽位与内置『音乐动效』的四种样式在同一个列表里选，
  **没安装那个插件时插件样式照样可用**。
- 页面插入块把界面放进播放页固定位置（歌词页 / 更多信息页 / 播放列表页）：十个位置各一个类型
  （`musicxx.ui.playing.<页面>.<位置>`），字段与规则见 [plugin-ui.md](plugin-ui.md) §1.6。
- 播放页底部控制按钮栏可以整栏或单个按钮换成插件画的：六个类型，参数（`playing` / `favorite` /
  `song` / `colors` …）与动作写法见 [plugin-ui.md](plugin-ui.md) §1.7。
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
| 这条能力现在有谁在实现、排第几 | 能力 `musicxx.capability.subscribers`（§3.2.12）；命令行 `musicxx-cli capability.subscribers` 读同一份数据 |
| 应用留出了哪些功能埋点、谁在实现、谁关掉了 | 能力 `musicxx.feature.point.list`（§3.2.11） |
| 应用自己能做什么（给模型看的文本结论） | 能力 `musicxx.feature.list`（§3.2.10） |
| 本插件跑了多少、错了几次 | JS：`musicxx.stats.getSelf()`；两边都看得到：管理页「插件详情 → 运行统计」 |

### 10.3 应用卡住时怎么查

主程序日志目录下有一份 `host_call.log`（进插件框架调用的同步轨迹）：**卡住后最后一行就是没有返回的那次调用**。
配合「插件详情 → 日志」就能区分是脚本死循环、能力等待，还是插件框架本身的问题。

---

## 11. 通用约定

### 11.1 命名空间

- 官方标识一律 `musicxx.*`（能力 / 广播频道 / UI 类型 / 权限 / 频道 id）；
- 插件自定义一律 `plugin.<自己的插件 id>.*`（广播频道、能力名、UI 项 id、功能埋点 id）；
- 冒充他人命名空间会被拒绝（返回 `-6`）。**调用的名字不受此限**：
  任意名字都受理（由应用决定理不理），官方能力必须是声明表里已有的。

### 11.2 容量与限制（当前版本）

| 项目 | 限制 |
|---|---|
| 单条广播的值大小 | 不限制（必须是合法 JSON。值由应用按需发布、插件只读；高频数据用没有当前值的频道并按订阅方自己的频率接收） |
| 插件私有 KV / `config.json` | 不设配额（每次写入整文件重写，别当数据库） |
| `musicxx.storage.list` | 每次最多 500 个键 |
| 广播频道数量 / 单条载荷大小 / 每个频道的订阅数 | 不限制；高频频道由订阅方自己声明 `intervalMs` |
| `musicxx.net.fetch` / `download` | **超时与大小不限制**（`timeoutMs` 缺省 15000、`<=0` = 不设；`maxBytes` 缺省不限制）；方法名不限白名单，只要合法 |
| `musicxx.media.cover` | `size` 16~1024（`format:"rgba"` 的字节数 ≤ `size×size×4`，最大一档 4 MiB）、结果缓存 16 条 |
| 静态路由（`http.serve({static})` / `http.route`） | 单条内存内容 ≤ 8 MiB；每个服务 ≤ 1024 条；服务 ≤ 8；连接 ≤ 64；只监听 127.0.0.1 |
| UDP 通道（`musicxx.net.udp.*`） | 每插件 ≤ 32 条；单包受系统 UDP 上限（约 64 KiB）约束 |
| `musicxx.media.bindImage` | 键 1~64 位 `[A-Za-z0-9_.-]`；**张数与尺寸都不限制**（`format: "rgba"` 仍要高宽与字节数匹配） |
| `musicxx.ui.loadFont` | 字体文件 ≤ 100 MiB（超过直接回错误）；`path` 不限范围（绝对路径 / `file://` / `~` / 相对插件目录） |
| `musicxx.playlist.addSong` | 一次首数与字段长度都**不限制** |
| 插件网盘声明（`musicxx.source.browse`） | 分类数量、标题 / 根目录 / 图标 / 能力名的长度、页大小（>0 即可）、`resolveSongs` 首数、条目文本长度都**不限制** |
| 插件浮层（`musicxx.ui.overlay.*`） | **层数不限制**（一个插件可以同时开多层，`overlay.close` 关最上面那层） |
| 安装包解包 | 大小 / 条目数 / 解包体积都**不限制**；仍拒绝绝对路径 / `..` 逃逸 / 符号链接 / 非普通文件 / 大小写重名 |
| 界面动画（`musicxx.AnimatedBuilder`） | 通道数量与 `maxFps` 都**不限制**（`maxFps` 缺省 30；`<=0` 用缺省） |
| 渲染帧率（`maxFps`） | 不限上限（缺省 16，`<=0` 用缺省）；`resolutionScale` 仍夹 0.25~1、渲染目标最长边 1280 |
| 性能 / 内存 / 网络 | 插件框架不做限制（插件的内存、耗时、数量都不设上限；自己别把应用拖死） |

### 11.3 超时一览

| 场景 | JS 缺省 | 动态库缺省 | 备注 |
|---|---|---|---|
| 应用实现的能力 | 5000 ms（`<=0` 按缺省） | 5000 ms（**0 = 不设超时**） | 需要更久自己加大（如解码整曲） |
| 跨插件能力调用 | 3000 ms（不传 = 3000；**传 `0` / 负数 = 一直等**） | 框架不设等待超时（受理失败会立刻返回） | 只有异步形态 |
| 功能埋点调用 | 60000 ms | 60000 ms | 频谱要解码整曲 |
| 能力等待 | **没有超时** | **没有超时** | 异步裁决不结算就是调用点一直等 |

### 11.4 插件框架的错误码（动态库插件返回码）

| 码 | 含义 | 常见原因 |
|---|---|---|
| `0` | 成功 | — |
| `-1` | 参数非法 | `data` 不是 JSON 对象、缺 `title`、结构体大小不符 |
| `-2` | 状态错误 | 实例未启动 / 已禁用 / 正在卸载 |
| `-3` | JSON 非法 | 载荷解析失败 |
| `-4` | 未找到 | 注册未知能力、未知 UI 类型、注销不存在的项 |
| `-5` | 超时 | 能力请求 / 能力调用超时 |
| `-6` | 权限拒绝 | 命名空间非法（冒充他人事件主题 / UI 项 id / 广播频道 id） |
| `-7` | 队列满 | 保留值（当前没有容量类限制） |
| `-99` | 内部异常 | 插件框架内部错误（看日志） |

JS 插件拿到的是**文字**（异常信息 / Promise 拒绝原因）：`action_not_registered: xxx`、
`请求失败：…`、`status=…`、`频道未声明: …`。

### 11.5 停用 / 卸载时会自动发生什么

插件被禁用或卸载时，插件框架**统一摘除**：

- 全部能力处理器；全部已声明能力；全部事件订阅；全部 UI 项声明；全部功能埋点声明与实现；
- **自己起的网络服务与静态路由**（端口立刻释放，旧 URL 立即失效）；
- 定时器（JS）；未完成的能力请求（JS 的 Promise 被拒绝，原因"插件已停止"）；
- 自管的线程 / 定时器 / 临时文件由你在 `stop` 事务里自己回收（动态库插件）。

重新启用 = 重新跑一遍注册：**注册语句要写成幂等的**（重复执行不会累积副作用）。

---

## 12. 常见坑（通用部分）

| 现象 | 原因 / 处理 |
|---|---|
| 调用一直 `action_not_registered` | 能力当前版本没实现（§3.4）；或名字拼错 |
| 处理器不触发 | 这条能力在 `plugin-capabilities.md` 里标"未接入"；或插件被禁用；或注册失败（看返回码 / 返回值） |
| 处理器被调用多次 | 用不同 `ownerTag` 注册了同一条能力（§8.1） |
| 定时器 / 初始化执行两遍 | 每次启用都会重新执行注册（写成幂等） |
| 设置项改完重启又变回默认 | 把 `storage.get` 的"值本身"当成了对象外壳（§3.2.5） |
| 设置项被"改回去" | 读配置是异步的：读回来的旧值后到，覆盖了用户刚改的值（加"已经改过就不再覆盖"的保护） |
| 点入口提示「插件没有提供『xxx』」 | 入口指向的能力没注册（页面视图 id 必须与能力短名一致） |
| 页面打开后提示「插件没有提供任何内容块」 | 视图没有 `blocks`，或块类型名全部拼错（未识别的块会被静默忽略） |
| 改了 `config.json` 页面没变 | 页面是打开时取的能力结果：改完再返回一次 `{view:…}`，或重新打开页面 |
| 切歌 / 换背景样式卡几秒 | 在同步裁决的处理器（取色、频谱提取）里 `await` 了应用实现的能力 → 两边互等（§8.4） |
| 主程序整体卡住 | 处理器里做了阻塞操作 / JS 死循环（框架不打断） |
| 颜色 / 频谱"凭空消失" | 有人返回了 `{"action":"cancel"}`（§8.5）；用 `capability.list` 看是谁 |
| 网络请求拿不到数据 | 看 `status`（非 2xx 也 `ok:true`）与 `truncated`（读满 `maxBytes` 就断开） |
| 生成的界面在别的客户端样式不对 | 按 `host.info().ui` 的 `blocks` / `controls` 降级，不要按 `kind` 写两套 |
| 不确定"现在谁在实现这个功能" | `musicxx.capability.subscribers` / `musicxx.feature.point.list` / 命令行 `capability.list` |

---

## 13. 相关文档

- [plugin-guide.md](plugin-guide.md) —— 总入口：选形态、环境、第一个插件、注意事项、调试、发布检查
- [plugin-js-api.md](plugin-js-api.md) —— JS插件：目录与清单、生命周期与硬约束、`musicxx` 全局对象、JS 特有的坑
- [plugin-native-api.md](plugin-native-api.md) —— 动态库插件：清单与多目标打包、SDK、线程、构建、部署、排障
- [plugin-capabilities.md](plugin-capabilities.md) —— 能力总表：能不能参与、参与哪一步、载荷与回答（生成物）
- [plugin-broadcasts.md](plugin-broadcasts.md) —— 广播总表：频道、有没有当前值、谁能发布、载荷与取值域（生成物）
- [plugin-ui.md](plugin-ui.md) —— 界面字段、块类型、插件页面与设置页
- [plugin-shader-bundle.md](plugin-shader-bundle.md) —— shader bundle 打包、uniform、参数表达式
- [plugin-agent-cli.md](plugin-agent-cli.md) —— 命令行 / MCP：装插件、刷新、看日志
- [musicxx-interfaces.md](musicxx-interfaces.md) —— musicxx 对外暴露的**全部接口**：MCP 工具全表、命令行命令全表、本机 HTTP 与 WebSocket 路由、权限边界
- `plugins/` —— 官方示例（`example_js` 与 `example_native` 行为等价，可以对照着读）
