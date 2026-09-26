# 插件渲染：shader bundle 打包与 uniform 契约

插件可以把**打包期编译好的 shader bundle** 注册成一种宿主渲染样式（当前只有「播放页背景」
一个渲染槽位），由用户在「设置 → 播放页面背景」里选中后生效；同一个 bundle 也能画在插件自己的
页面里（`Shader` 块）。

插件不写 Dart/Flutter 代码，只交出 bundle + 一份参数声明。相关文档：界面与页面块类型见
[plugin-ui.md](plugin-ui.md)，JS 与 C++ 的写法见 [plugin-js-api.md](plugin-js-api.md) /
[plugin-native-api.md](plugin-native-api.md)。

> 现成可编译的例子：`plugins/example_js_shader/shader/` 与 `plugins/example_native/shader/`
> —— 「晶格化」背景（随机点最近邻切块，配色用宿主的 4 个绘制色）。基线版本 3.47.5 的 Flutter SDK
> 编译出来的 bundle 可以直接用（`format_version = 2`）。

---

## 1. 四个步骤

```text
① 写两个着色器：顶点（全屏三角形）+ 片元（声明 uniform 结构体 MusicxxRenderInfo）
② 用 impellerc 编译成一个 bg.shaderbundle（五个后端一次编完，一个文件全平台通用）
③ 在插件里注册一种样式：UI 项 musicxx.ui.playing.background（绑定 bundle + 声明 args）
④ 用户在「设置 → 播放页面背景」里选中它 → 生效（未选中时零成本：不读 bundle、不分析封面）
```

---

## 2. 目录结构

```text
<插件目录>/
├── plugin.yaml
├── plugin.js / <插件库文件>
└── shader/
    ├── bundle.json           impellerc 的 bundle 描述
    ├── shaders/bg.vert       顶点着色器
    ├── shaders/bg.frag       片元着色器
    ├── build_bundle.ps1      打包脚本（照抄示例）
    └── bg.shaderbundle       编译产物（随插件分发）
```

`bundle.json`：

```json
{
  "MusicxxRenderVertex": { "type": "vertex", "file": "shaders/bg.vert" },
  "MusicxxRenderFragment": { "type": "fragment", "file": "shaders/bg.frag" }
}
```

这两个键就是**入口名**。宿主默认按 `MusicxxRenderVertex` / `MusicxxRenderFragment` 查找，
也可以在 UI 项里改（一般不需要）：

```jsonc
"shader": {
  "bundle": "shader/bg.shaderbundle",   // 必填：插件目录内的相对路径（不能是绝对路径，不能含 ..）
  "vertex": "MusicxxRenderVertex",      // 可选
  "fragment": "MusicxxRenderFragment"   // 可选
}
```

---

## 3. 编译

示例插件的打包脚本（`shader/build_bundle.ps1`，照抄即可）：

```powershell
pwsh -NoProfile -File shader/build_bundle.ps1                       # 用当前 flutter 的 impellerc
pwsh -NoProfile -File shader/build_bundle.ps1 -FlutterRoot C:\Users\me\fvm\versions\3.47.5
```

手工调用：

```powershell
# Windows：FLUTTER_ROOT 为你使用的 Flutter SDK
$spec = ((Get-Content shader/bundle.json -Raw) -replace "`r?`n", ' ').Trim()
& "$FLUTTER_ROOT/bin/cache/artifacts/engine/windows-x64/impellerc.exe" `
    --shader-bundle="$spec" --sl=shader/bg.shaderbundle
```

```bash
# Linux / macOS：引擎目录按平台（linux-x64 / darwin-x64 / darwin-arm64），impellerc 无 .exe 后缀
"$FLUTTER_ROOT/bin/cache/artifacts/engine/linux-x64/impellerc" \
  --shader-bundle="$(tr -d '\n' < shader/bundle.json)" --sl=shader/bg.shaderbundle
```

> **`--shader-bundle=` 的内容必须压成一行**：`bundle.json` 带换行时会被 shell 拆成多个参数，
> impellerc 会报 `Target shading language file name was empty`。示例脚本里就是用
> `-replace "\`r?\`n", ' '`（PowerShell）/ `tr -d '\n'`（bash）先压平的。

- 一次编译出 5 个后端（metal_ios / metal_desktop / opengl_es / opengl_desktop / vulkan），
  一个文件全平台通用，不需要按平台分别编；
- 任一后端编译失败，打包就失败（不会产出半成品）；
- bundle 里有 `file_identifier = "IPSB"`；宿主加载前会自己读字节校验 `format_version`。

---

## 4. 格式版本（硬性）

- `format_version` 由引擎版本决定：**当前（Flutter 3.47.x）是 2**；
- 版本不符时，该样式在设置列表里显示
  `shader bundle 版本不符（1 != 2），请用当前 Flutter SDK 重新打包`，**不可选**，播放页保持内置背景；
- 所以：**请用与目标应用相同的 Flutter 大版本编译**，换 Flutter 版本后重新跑一次打包脚本。

---

## 5. uniform 契约

宿主每帧填充一个固定名字的结构体 `MusicxxRenderInfo`：

```glsl
uniform MusicxxRenderInfo {
  vec4 uParams;   // (目标宽 px, 目标高 px, 经过的秒数, 插件声明的 speed)
  vec4 uEnv;      // x = 是否夜间(0/1), y = 是否有有效的封面配色(0/1), z/w = 保留
  vec4 uColor1;   // 下面这些由插件在 args 里声明（名字随便取，见 §7）
  vec4 uColor2;
  vec4 uColor3;
  vec4 uColor4;
  vec4 uTint;     // 例：{"name":"uTint","source":"theme.primary"}
} render_info;
```

规则：

- **结构体名与成员名必须完全一致**（宿主按名字寻址写入）；成员偏移必须 16 字节对齐；
- 结构体大小必须是 **16 的倍数且 ≥ 16**，否则该样式判为不可用并给出原因；
- `uParams` / `uEnv` 由宿主自动写（不用在 `args` 里声明）；
- **可以少声明成员**（宿主跳过不写，着色器读到 0），但不能声明错误的名字
  （成员在结构体里不存在时宿主会跳过；声明了名字却没有对应成员不会报错，只是没有值）；
- 颜色是 `0..1` 的浮点（sRGB 分量）；`uEnv.y = 0` 表示当前没有有效的封面配色分析结果
  （用 `icon.*` 来源的参数这时会用插件给的固定值兜底）。

**关于时间**：`uParams.z` 是这块渲染视图从创建起累计的**真实秒数，没有乘速度**；`uParams.w` 才是
插件声明的 `speed`。想跟随速率变化，着色器要自己乘：

```glsl
float t = render_info.uParams.z * render_info.uParams.w;   // 速率变了立即见效
```

也注意 `animate: false` 时 `uParams.z` 恒为 0，并且渲染视图被遮挡 / 切后台期间不推进渲染、
但时间不重置（回到前台可能跳一段）—— 连续动画建议用周期函数（示例用 `sin` / `fract`）。

---

## 6. 着色器写法

顶点（宿主绑定 3 个顶点的全屏三角形，覆盖整个裁剪空间，不做任何变换）：

```glsl
#version 460 core
layout(location = 0) in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); }
```

片元：

```glsl
#version 460 core
uniform MusicxxRenderInfo {
  vec4 uParams;
  vec4 uEnv;
  vec4 uColor1;
  vec4 uColor2;
  vec4 uColor3;
  vec4 uColor4;
} render_info;

layout(location = 0) out vec4 frag_color;

void main() {
  vec2 uv = gl_FragCoord.xy / max(render_info.uParams.xy, vec2(1.0));
  float t  = render_info.uParams.z * render_info.uParams.w;
  vec3 a  = render_info.uColor1.rgb;
  vec3 b  = render_info.uColor2.rgb;
  frag_color = vec4(mix(a, b, 0.5 + 0.5 * sin(uv.x * 6.0 + t)), 1.0);
}
```

要点：

- 结构体必须在**片元着色器**里声明（宿主按片元入口查它的 uniform 槽）；
- `gl_FragCoord.xy` 是像素坐标，除以 `uParams.xy` 得到 `0..1` 的 uv；
- 输出写在 `layout(location = 0) out`；
- 完整效果参考 `plugins/example_js_shader/shader/shaders/bg.frag`（晶格化：Worley 噪声切块 +
  4 色双线性混合 + 夜间压暗 + 轻微暗角）。

---

## 7. 参数（`args`）

着色器的每个 `vec4` 成员都由 `args` 里的一项声明（播放页背景的 `data.args`、页面里 `Shader` 块的
`args` 是同一套写法）：

```jsonc
"args": [
  { "name": "uColor1", "source": "icon.themeMapping.0" },              // 具名来源（内置背景用的那 4 色）
  { "name": "uColor2", "source": "icon.main", "convert": true, "value": "#8899aa" },  // 封面提取色 + 兜底值
  { "name": "uColor3", "source": "theme.primary" },                    // 主题主色
  { "name": "uTint",   "value": "#ff8800" },                          // 固定颜色
  { "name": "uMix",    "value": [0.5, 0.25, 0, 1] },                   // 固定 vec4
  { "name": "uFlag",   "value": 1 },                                   // 固定标量（写在 x，其余为 0）
  { "name": "uNight",  "value": "#ffffff", "valueNight": "#101010" }   // 固定值也分昼夜
]
```

| 字段 | 说明 |
|---|---|
| `name` | uniform 成员名：字母或下划线开头，最长 32 字符（`^[A-Za-z_][A-Za-z0-9_]{0,31}$`）；结构体里没有这个成员时宿主跳过不写 |
| `source` | 具名来源（见下表）；空 = 只用固定值 |
| `convert` | 只对 `icon.*` 有意义：取到的封面色是否套宿主的昼夜转换（默认 false，原样给） |
| `value` | 固定值（白昼用）：数字（写到 x）、1~4 个数字的数组、`#rrggbb` / `#aarrggbb`；也是 `source` 取不到时的兜底 |
| `valueNight` | 固定值的夜间版本（缺省回退 `value`） |

| `source` | 取到的颜色 |
|---|---|
| `theme.primary` | 主题主色 |
| `theme.background` / `theme.backgroundCross` | 主题背景色 / 第二背景色 |
| `theme.textMain` / `theme.textCross` | 主要 / 次要文字色 |
| `theme.textTitle` / `theme.titleBackground` | 标题文字色 / 标题底色 |
| `theme.button` / `theme.buttonSelect` | 按钮内容色 / 按钮选中内容色 |
| `theme.error` / `theme.wave` | 错误提示色 / 歌曲图波浪色 |
| `icon.main` / `icon.light` / `icon.lightMuted` / `icon.dark` / `icon.darkMuted` | 当前歌曲封面的提取色（分析结果原始色；`convert: true` 时套昼夜转换） |
| `icon.dominant.0` .. `icon.dominant.3` | 封面提取色的主色候选 |
| `icon.themeMapping.0` .. `icon.themeMapping.3` | 封面颜色**经主题/背景映射后的 4 个绘制色**：内置播放页背景实际用的就是这 4 色（已经套过昼夜转换与观感归一，`convert` 对它不再生效；没有分析结果时是兜底色） |

- 一份声明最多 **16 项**；名字非法、既没有来源也没有固定值的项会被**直接忽略**（不是报错）；
- 只认 `args`：旧的 `data.colors` 字段**已移除** —— 还写着它的插件不会解析它（播放页背景会用默认的
  内置 4 色），宿主日志里会给一条迁移提示；
- 播放页背景**完全不写 `args`** 时，宿主默认给 `uColor1..4 ← icon.themeMapping.0..3`
  （观感与内置背景一致）；只想用主题色时显式声明 `args` 即可（也不会再套默认 4 色）。

---

## 8. 其它字段与上限

| 字段 | 默认 | 范围 | 说明 |
|---|---|---|---|
| `title` | 必填 | — | 设置列表里的样式名 |
| `depict` | `""` | — | 副标题（写 `subtitle` 也可以，且优先） |
| `enabled` | `true` | — | false = 不在设置列表里出现 |
| `args` | 空 | ≤ 16 项 | 着色器参数（见 §7）；背景槽位不声明时默认给内置 4 色 |
| `speed` | 4 | 0..20 | 时间推进速度，写进 `uParams.w`（着色器要自己乘，见 §5）。**由插件自己决定**，宿主不做二次缩放 |
| `maxFps` | 16 | 1..30 | 帧率上限 |
| `resolutionScale` | 1.0 | 0.25..1.0 | 降采样后由宿主放大（省 GPU） |
| `animate` | true | — | false = 只渲染一帧（时间恒为 0） |
| `scrim` | 0 | 0..0.8 | 背景上叠一层暗化（页面里的 `Shader` 块没有这个字段） |
| `foregroundStyle` | `mask` | `mask` / `neumorphism` | 播放页前景（歌曲名/图标）用浅色遮罩还是常规样式；未知值按 `mask` |

上限：bundle 文件 ≤ **4 MiB**；单条 UI 项 `data` ≤ **64 KiB**；渲染目标最长边由宿主限制在 **1280 px**
（超出的部分按比例缩小，再交给 Flutter 放大）；每个插件最多 64 个 UI 项。

字段写错只会退化成默认表现（数值会被 clamp），不会让整项不可用；不可用的原因只来自
bundle 本身（文件不存在 / 版本不符 / 结构体不符）与运行期加载失败。

---

## 9. 页面里内联画一块着色器（`Shader` 块）

同一个 bundle 也能画在插件自己的页面里（`ext://<插件id>/<视图id>` 的视图描述）：

```jsonc
{"kind": "SizedBox", "height": 300, "child": {
    "kind": "Shader",
    "bundle": "shader/bg.shaderbundle",
    "speed": 1,
    "maxFps": 16,
    "args": [
      {"name": "uColor1", "source": "theme.primary"},
      {"name": "uColor2", "source": "icon.main", "convert": true, "value": "#8899aa"}
    ]}}
```

- **尺寸由父块决定**：不写尺寸就用父块给的空间，所以通常要像上面这样用 `SizedBox`
  （或 `Expanded`、`Row` 里的 `Expanded`）给它确定的高度；父块给不出确定尺寸时这一块**留空**
  并记一条日志（不崩、也不画占位）；
- `bundle` 只能是**本插件目录内**的相对路径；`args` / `speed` / `maxFps` / `animate` /
  `resolutionScale` 与背景样式同一套语义（`scrim` 在页面块里没有意义）；
- 页面不可见（被路由遮挡、进后台）时自动停帧；渲染失败时留空 + 日志（页面块不做背景那套重试）；
- 完整示例：`plugins/example_js_shader/plugin.js` 的 `settings` 页（内联块 + 一键切换背景）。

---

## 10. 运行期：查询、切换与状态镜像

```js
// 有哪些可选样式（含内置项）
const r = await musicxx.render.list({ slot: "player.background" });
// r.slots[0] = { slot, title, selectedId, items: [
//     { id, title, depict?, source:"builtin"|"plugin", plugin?, available, reason?, selected } ] }

// 一键使用（也可以切到内置样式）
await musicxx.call("musicxx.render.select", { slot: "player.background", id: "plugin.my_plugin.bg" });
await musicxx.render.select("builtin:Auto");     // 省略 slot 用默认槽位
// 成功 → { ok:true, slot, id }；不可用 / 未知槽位 → { ok:false, error:"<原因>" }

// 现在是谁在画（同步读状态镜像）
const slot = (musicxx.state.get("musicxx.state.renderSlots") || {})["player.background"];
```

- 内置候选 id 是 `builtin:<内置样式名>`，例如 `builtin:Auto`（内置换算出来的列表见
  `musicxx.render.list` 的返回）；
- 镜像 `musicxx.state.renderSlots` 每个槽位一个条目，字段：

  | 字段 | 含义 |
  |---|---|
  | `itemId` | 现在由哪个**插件项**在画（为空 = 没有插件项在画） |
  | `plugin` | 生效项所属插件 |
  | `selectedId` | 用户选中的是谁（可能是内置样式 `builtin:*`） |
  | `visible` | 宿主当前是否在渲染（播放页被遮挡 / 切后台 / 播放页不在页面上 → false） |
  | `night` | 是否夜间表现 |
  | `animate` / `maxFps` / `width` / `height` | 该样式的渲染参数与当前像素尺寸（不可见时为 0） |

- 槽位条目**一直存在**：用户切回内置样式时 `itemId` 为空而 `selectedId` 是 `builtin:*`，
  插件据此就能说清「内置背景」而不是猜「没有条目意味着什么」；只有宿主停止（关闭『拟声++』）
  才清空；
- 需要更细的颜色数据用 `musicxx.media.palette`（分析结果 + 宿主 4 色），需要封面像素用
  `musicxx.media.cover`（`size` 16..512，`format` = `jpeg`/`png`/`rgba`，`data` 是 base64）。

---

## 11. 动画速率由插件自己提供

宿主用的时间推进速度就是插件声明的 `speed`（写进 `uParams.w`），不做二次缩放。要允许用户调速率，
就把它做成**插件自己设置页里的一个设置项**（值存插件目录的 `config.json`）：

```js
var BG_BASE_SPEED = 1;                 // 本插件的基准速度（= 1×）
var BG_RATE_OPTIONS = [0.5, 1, 2];     // 可选倍率

function bgData(rate) {
    return {
        title: "流光背景",
        shader: { bundle: "shader/bg.shaderbundle" },
        args: [ /* ... */ ],
        speed: BG_BASE_SPEED * rate,   // 声明给宿主的速度
        maxFps: 16,
    };
}

musicxx.ui.registerEntry({ name: "bg", type: "playing.background", data: bgData(1) });

function applyRate(rate) {
    // updateEntry 是**整体替换**：要把完整 data 传回去
    musicxx.ui.updateEntry("bg", bgData(rate));
    musicxx.storage.setConfig("bgRate", rate);
}
```

- 运行期改 `speed` 会推 `musicxx.ui.changed`，宿主刷新候选后**正在使用的背景立即用新速度**
  （不用重新选中：`speed` 每帧现算，着色器里乘上 `uParams.w` 就见效）；
- `maxFps` / `resolutionScale` / `animate` 这类要改帧调度或分辨率创建的字段，运行期改完需要
  **重新选中该项**才完全生效；
- 示例插件 `example_js_shader` 就是这么做的：它的设置页有「背景动画速率」（0.5× / 1× / 2×），
  基准速度定为 1（即 `speed` 1/2/4）；`example_native` 的 C++ 版声明 `speed: 4`。

---

## 12. 排查

| 现象 | 原因 |
|---|---|
| 设置里显示「bundle 文件不存在」 | `shader.bundle` 的相对路径写错，或 bundle 没随插件分发（native 插件要用 `ASSETS` 复制进产物目录） |
| 「shader.bundle 必须是插件目录内的相对路径」 | 写了绝对路径、含 `..`、含反斜杠或以 `/` 开头 |
| 「shader bundle 版本不符（1 != 2）」 | bundle 是用别的 Flutter 版本编译的，用当前 SDK 重新打包 |
| 「不是可用的 shader bundle（读不到格式版本）」 | 文件不是 impellerc 产物（或为空 / 被截断） |
| 「bundle 超过 4 MiB 上限」 | 换更小的着色器，或减少内联常量表 |
| 「bundle 里找不到片元入口『MusicxxRenderFragment』」 | `bundle.json` 的键名与 `shader.fragment` 不一致 |
| 「着色器没有声明 uniform 结构体 MusicxxRenderInfo」 | 结构体名写错，或只在顶点着色器里声明 |
| 「MusicxxRenderInfo.uColor1 偏移非法」 | 结构体布局不符（成员顺序 / 类型不对，或插了非 16 字节对齐的成员） |
| 页面里那块 `Shader` 一直是空白 | 父块没有给出确定尺寸（用 `SizedBox` / `Expanded` 给它高度）；或 bundle 不可用（日志里有原因） |
| 选中后回到内置背景 | 加载或渲染报错被停用（连续失败 3 次才停用，其间保留最后一帧）；看宿主日志与「外部插件 → 调试」里的背景段落 |
| 动画不动 | `animate: false`、`speed: 0`，或着色器没有把 `uParams.z` 乘上 `uParams.w`；播放页被遮挡 / 切后台时本来就不渲染（`visible: false`） |
| 画面比预期快/慢 | `uParams.z` 是真实秒数、`uParams.w` 是插件声明的速度：宿主的基准速度就是插件给的值（示例把 1 当 1×） |
