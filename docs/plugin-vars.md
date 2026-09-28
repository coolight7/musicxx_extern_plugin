# musicxx 官方变量目录（`musicxx.*`）

> 本文件是**官方变量**的清单与口径说明。变量属于「第五条通道」：有属主、可读、可写、
> 可订阅变动的小值（配置 / 小状态）。通道本身的能力见 `docs/plugin-js-api.md`
> （JS 插件）与 `docs/plugin-native-api.md`（动态库插件）的「变量」章节。
>
> **真值在应用侧**：宿主里没有第二份真值，只保留一份服务同步读（`peek`）的缓存。
> 没有列在这里的官方键**不存在**：读/写会返回「未找到」——应用没有声明它。
> 加一条官方变量 = 在应用侧登记实现 + 更新本文件。

## 说明

| 列 | 含义 |
|---|---|
| `caps` | 能力位（`get` 可读 / `set` 可写 / `notify` 可订阅变化）；三者可只实现一部分 |
| 值 | 取值口径；枚举一律用**稳定字符串 id**（不要用本地化文案），部分也接受原有 int 编码 |
| 风险 | 管理页排序用：`low` 纯观感 / `medium` 会影响用户持久设置 / `high` 影响数据 |

写官方变量由**应用落地**（走用户在设置里改的同一条路径），因此
`set` 的 Promise/事件回执里带的是应用最终接受的值：被拒绝时 `accepted=false` +
`error` 说明原因。

## 界面动画（`musicxx.ui.*`）

| 键 | caps | 值 | 风险 | 说明 |
|---|---|---|---|---|
| `musicxx.ui.animatedLevel` | get+set+notify | `undefined` / `disable` / `low` / `medium` / `high` / `max`（也接受 int `-1/10/20/30/40/50`） | low | 界面动画等级；越低越省电，`disable` 关闭装饰性动画 |
| `musicxx.ui.particleAnimate` | get+set+notify | bool | low | 点击/拖动的粒子效果 |
| `musicxx.ui.songIconRotate` | get+set+notify | bool | low | 播放时圆形歌曲图旋转（方形歌曲图下不生效） |
| `musicxx.ui.songIconWave` | get+set+notify | bool | low | 播放时歌曲图随节拍起伏（方形歌曲图下不生效） |

## 主题（`musicxx.theme.*`）

| 键 | caps | 值 | 风险 | 说明 |
|---|---|---|---|---|
| `musicxx.theme.available` | get | `[{id, title, kind}]`，`kind` = `light` / `night` | low | **只读**。主题目录（纯静态，不带"当前选中"）；要看现在是哪个读 `current` |
| `musicxx.theme.current` | get+set+notify | `mimicryLight` / `mimicryNight` | low | 当前显示的主题。`set` **只切显示**：不改模式、不写库、不暂停跟随；要"一次性切换"用 `tempSwitch` |
| `musicxx.theme.mode` | get+set+notify | `light` / `night` / `followSystem` / `followTime` / `followScreenBrightness`（也接受 int `1~5`） | medium | 主题切换模式。`set` 会写用户的持久设置并立即按新模式切主题 |
| `musicxx.theme.tempSwitch` | **set**（只写） | 主题 id 或 `toggle` | low | 临时切换一次（`toggle` = 按当前明暗互切）；会暂停主题跟随，与侧边抽屉按钮同一语义。它表达的是一次操作，所以没有 `get`、没有 `notify` |

## 使用示例（JS）

```js
// 读一次（异步、权威）
var level = await musicxx.vars.get("musicxx.ui.animatedLevel");

// 用户改设置时跟着调整自己的行为
var off = musicxx.vars.bind("musicxx.ui.animatedLevel", function (value, info) {
  if (value === "disable") { musicxx.host.log(2, "动画已关闭"); }
});

// 插件也想改：由应用落地，应用不接受时会给出原因
var result = await musicxx.vars.set("musicxx.theme.mode", "night");
if (!result.accepted) { musicxx.host.log(3, "写入被拒绝: " + result.error); }

// 只在热路径里同步判断：先保活缓存，再 peek
musicxx.vars.watch("musicxx.ui.songIconWave");
var cached = musicxx.vars.peek("musicxx.ui.songIconWave");   // {value, revision, ageMs, stale} 或 null
```

## 维护约定

1. 新增官方键：先在应用侧登记（`lib/manager/MusicxxVariableCatalog.dart`），本文件补一行；
2. 应用侧回归用例（`test/modules/extern_variables_test.dart`）会断言目录的键数量与命名规范
   与登记表一致——**键数量/形状对不上就会失败**；
3. 键名用 `musicxx.<域>.<名>`，枚举取值用稳定 id（不用本地化文案）；
4. 只有真的存在通知源时才声明 `notify`（只有读源就只声明 `get`，像
   `musicxx.theme.available` 那样；只表达一次操作的不要 `get`，像 `tempSwitch`）。
