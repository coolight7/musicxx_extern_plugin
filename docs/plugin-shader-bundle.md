# 插件渲染：shader bundle 打包与 uniform 契约

插件可以把自己写的着色器做成**播放页背景样式**（未来还会有别的"渲染槽位"）。插件不写 Dart/Flutter
代码，只交出**打包期编译好的 shader bundle**，由宿主在运行期加载、按帧渲染。

本文是插件作者的必读项；应用侧实现见 musicxx 仓库的
`lib/plugin/externPlugin/render/`（通用层）。

## 1. 三个步骤

```text
① 写两个着色器（全屏三角形顶点 + 片元，片元里声明 uniform 结构体）
② 用 impellerc 编译成 bg.shaderbundle（五个后端一次编完，一个文件全平台通用）
③ 注册 UI 项 musicxx.ui.playing.background，把 bundle 的相对路径写进去
```

用户在「设置 → 播放页面背景」里选中它之后才生效；未选中时宿主不读 bundle、不分析封面（零成本）。

> 现成的可编译例子：`plugins/example_js_shader/shader/` 与 `plugins/example_native/shader/` 里的
> 「晶格化」背景（随机点最近邻切块，配色用宿主的 4 个绘制色）。应用原本自带的"晶格化"内置样式
> 已移除，这个效果现在由示例插件提供（正好演示"内置样式换成插件渲染"的完整流程）。

## 2. 目录结构（示例见 `plugins/example_js_shader/shader/`）

```text
<插件目录>/
├── plugin.yaml
├── plugin.js / <插件库文件>
└── shader/
    ├── bundle.json           impellerc 的 bundle 描述
    ├── shaders/bg.vert       顶点着色器
    ├── shaders/bg.frag       片元着色器
    └── bg.shaderbundle       编译产物（随插件分发）
```

`bundle.json`：

```json
{
  "MusicxxRenderVertex": { "type": "vertex", "file": "shaders/bg.vert" },
  "MusicxxRenderFragment": { "type": "fragment", "file": "shaders/bg.frag" }
}
```

上面两个键就是入口名。宿主默认按 `MusicxxRenderVertex` / `MusicxxRenderFragment` 查找，
也可以在 UI 项里改：

```jsonc
"shader": {
  "bundle": "shader/bg.shaderbundle",   // 必填：插件目录内的相对路径（禁止绝对路径与 ..）
  "vertex": "MusicxxRenderVertex",      // 可选
  "fragment": "MusicxxRenderFragment"   // 可选
}
```

## 3. 编译

```bash
# Windows（示例插件的打包脚本）
pwsh -NoProfile -File shader/build_bundle.ps1

# 手工调用（FLUTTER_ROOT 为你使用的 Flutter SDK）
"$FLUTTER_ROOT/bin/cache/artifacts/engine/windows-x64/impellerc.exe" \
  --shader-bundle="$(tr -d '\n' < shader/bundle.json)" \
  --sl=shader/bg.shaderbundle
```

> **`--shader-bundle=` 的内容必须压成一行**：`bundle.json` 带换行时会被 shell 拆成多个参数，
> impellerc 会报 `Target shading language file name was empty`。示例脚本里就是用
> `-replace "\`r?\`n", ' '`（PowerShell）/ `tr -d '\n'`（bash）先压平的。

- 一次编译出 5 个后端（metal_ios / metal_desktop / opengl_es / opengl_desktop / vulkan），
  一个文件全平台通用，不需要按平台分别编；
- 任一后端编译失败，打包就失败（不会产出半成品）；
- Linux/macOS 用同目录下的 `impellerc`（无 `.exe` 后缀）。

## 4. 格式版本（硬性）

- bundle 是 flatbuffer，带 `file_identifier = "IPSB"`，其中 `format_version` 由引擎版本决定
  （当前 **2**，Flutter 3.47.x 编译出来就是 2）；
- 宿主**加载前**会自己读字节校验这个版本：不符时该样式在设置列表里显示
  `shader bundle 版本不符（x != 2）…`，**不可选**，播放页保持内置背景；
- 所以：**请用与目标应用相同的 Flutter 大版本编译**。换 Flutter 版本后请重新跑一次打包脚本。

## 5. uniform 契约

宿主每帧填充一个固定结构体 `MusicxxRenderInfo`：

```glsl
uniform MusicxxRenderInfo {
  vec4 uParams;   // (目标宽, 目标高, 时间秒, 速度)  —— 宿主自动写
  vec4 uEnv;      // x = 是否夜间(0/1); y = 调色板是否有效(0/1); z,w 保留  —— 宿主自动写
  vec4 uColor1;   // 后面这些由插件在 args 里声明（名字随便取）
  vec4 uColor2;
  vec4 uColor3;
  vec4 uColor4;
  vec4 uTint;     // 例：{"name":"uTint","source":"theme.primary"}
} render_info;
```

规则：

- **结构体名与成员名必须完全一致**（宿主按名字寻址写入）；成员偏移必须 16 字节对齐；
- `uParams` / `uEnv` 由宿主自动写（不用声明也能少写）；
- **可以少声明成员**（宿主跳过不写，着色器读到 0），但不能声明错误的名字；
- 结构体大小为 16 的倍数且 ≥ 16，否则该样式判为不可用并给出原因；
- 颜色是 `0..1` 的浮点（sRGB 分量），`uEnv.y = 0` 表示当前没有有效的封面颜色分析结果（插件参数里用封面取色时可能取不到，宿主会用插件给的固定值兜底）。

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
  vec4 uParams; vec4 uEnv; vec4 uColor1; vec4 uColor2; vec4 uColor3; vec4 uColor4;
} render_info;
layout(location = 0) out vec4 frag_color;
void main() {
  vec2 uv = gl_FragCoord.xy / max(render_info.uParams.xy, vec2(1.0));
  frag_color = vec4(mix(render_info.uColor1.rgb, render_info.uColor2.rgb, uv.x), 1.0);
}
```

## 7. 参数（`args`）

着色器的每个 `vec4` 成员都由 `args` 里的一项声明（播放页背景的 `data.args`、页面里
`Shader` 块的 `args`，同一套写法）：

```jsonc
"args": [
  { "name": "uColor1", "source": "icon.themeMapping.0" },  // 具名来源（内置背景那 4 色）
  { "name": "uColor2", "source": "icon.main", "convert": true },  // 封面提取色（套昼夜转换）
  { "name": "uColor3", "source": "theme.primary" },       // 主题主色
  { "name": "uTint",   "value": "#ff8800" },              // 固定颜色
  { "name": "uMix",    "value": [0.5, 0.25, 0, 1] },      // 固定 vec4
  { "name": "uFlag",   "value": 1 },                      // 固定标量（写到 x）
  { "name": "uNight",  "value": "#ffffff", "valueNight": "#101010" }  // 固定值也分昼夜
]
```

| 字段 | 说明 |
|---|---|
| `name` | uniform 成员名（字母或下划线开头，最长 32 字符；结构体里没有就跳过不写） |
| `source` | 具名来源（见下表）；空 = 只用固定值 |
| `convert` | 只对 `icon.*` 有意义：取到的封面色是否套宿主昼夜转换（默认 false，原样给） |
| `value` | 固定值（白昼用）：数字 / 1~4 个数字的数组 / `#rrggbb` / `#aarrggbb`；也是 `source` 取不到时的兜底 |
| `valueNight` | 固定值的夜间版本（缺省回退 `value`） |

| `source` | 取到的色 |
|---|---|
| `theme.primary` | 主题主色 |
| `theme.background` / `theme.backgroundCross` | 主题背景色 / 第二背景色 |
| `theme.textMain` / `theme.textCross` | 主要 / 次要文字色 |
| `theme.textTitle` / `theme.titleBackground` | 标题文字色 / 标题底色 |
| `theme.button` / `theme.buttonSelect` | 按钮内容色 / 按钮选中内容色 |
| `theme.error` / `theme.wave` | 错误提示色 / 歌曲图波浪色 |
| `icon.main` / `icon.light` / `icon.lightMuted` / `icon.dark` / `icon.darkMuted` | 当前歌曲封面提取色（分析结果的原始色；没有分析结果时取不到 → 用固定值兜底） |
| `icon.dominant.0` .. `icon.dominant.3` | 封面提取色的主色候选 |
| `icon.themeMapping.0` .. `icon.themeMapping.3` | 封面颜色**经主题/背景映射后的 4 个绘制色**：就是内置播放页背景实际用的那 4 色（已按昼夜转换与观感归一，`convert` 不再生效；没有分析结果时是兜底色） |

- 一份声明最多 16 项；名字非法、既没有来源也没有固定值的项被直接忽略；
- `icon.*` 需要封面颜色分析结果（宿主会按需触发一次，见 §10）；
- 播放页背景**不声明 `args` 时**默认给 `uColor1..4 ← icon.themeMapping.0..3`（观感与内置背景一致）；
  只想用参数里的主题色时，显式声明 `args` 即可（就不会套用默认 4 色）。
- 旧的 `data.colors` 字段**已移除**：仍然写着它的插件不会解析它（播放页背景会用默认的内置 4 色），并在宿主日志里得到一条迁移提示。

## 8. 其它字段与上限

| 字段 | 默认 | 范围 | 说明 |
|---|---|---|---|
| `title` | 必填 | — | 设置列表里的样式名 |
| `depict` | `""` | — | 副标题 |
| `enabled` | `true` | — | false = 不在设置里出现 |
| `args` | 空 | ≤ 16 项 | 着色器参数（见 §7）；背景槽位不声明时默认给内置背景 4 色 |
| `speed` | 4 | 0..20 | 时间推进速度，写进 `uParams.w`。**由插件自己决定**（想给用户一个速率开关就把它做成插件自己的设置项，见 §11）；宿主不做二次缩放 |
| `maxFps` | 16 | 1..30 | 帧率上限（按 `ViewportAnimation` 限制） |
| `resolutionScale` | 1.0 | 0.25..1.0 | 降采样后由 Flutter 放大 |
| `animate` | true | — | false = 只渲染一帧 |
| `scrim` | 0 | 0..0.8 | 背景上叠一层暗化 |
| `foregroundStyle` | `mask` | `mask` / `neumorphism` | 页面前景（歌曲名/图标）用浅色遮罩还是常规样式；未知值按 `mask` |

bundle 文件 ≤ 4 MiB，单条 UI 项 `data` ≤ 64 KiB；渲染目标最长边由宿主限制在 1280 像素。

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
  并记一条日志（不会崩、也不会画占位）；
- `bundle` 只能是本插件目录内的相对路径；`args` / `speed` / `maxFps` / `animate` /
  `resolutionScale` 与背景样式同一套语义（`scrim` 在页面块里没有意义，写也不生效）；
- 页面不可见（被路由遮挡、进后台）时自动停帧；渲染失败时留空 + 日志（页面块不做背景那套重试）；
- 完整示例见 `plugins/example_js_shader/plugin.js` 的 `card` 页（内联块 + 一键切换背景）。

## 10. 运行期：查询与切换

```js
// 查询所有可选样式（含内置项）
const r = await musicxx.render.list({ slot: "player.background" });
// 一键使用（也可以切到内置：id 写 "builtin:Auto"）
await musicxx.call("musicxx.render.select", { slot: "player.background", id: "plugin.my_plugin.bg" });
// 当前是不是我在画（同步读状态镜像）
const slot = (musicxx.state.get("musicxx.state.renderSlots") || {})["player.background"];
if (slot && slot.itemId === "plugin.my_plugin.bg") { /* 我在画 */ }
```

- `musicxx.render.select` 只允许选中**可用**项；不可用时返回 `{ok:false, error:"<原因>"}`；
- 镜像 `musicxx.state.renderSlots` 含 `itemId` / `selectedId` / `visible` / `night` / `width` /
  `height` / `animate` / `maxFps`；
  - `itemId` = 现在由哪个插件项在画（为空 = 没有插件项在画），`selectedId` = 用户选中的是谁
    （可能是内置样式 `builtin:*`）；槽位条目**一直存在**，所以"用户切回内置了"也能读出来，不用猜
    "没有条目"是什么意思；
  - `visible:false` 时宿主没有渲染这份样式：播放页被遮挡 / 切后台，或者播放页当前不在页面上
    （此时 `width`/`height` 为 0）。插件可以据此停掉自己的重活；
- 需要更细的颜色数据用 `musicxx.media.palette`（分析结果 + 宿主 4 色），需要封面像素用
  `musicxx.media.cover`（`size` 16..512，`format` = `jpeg`/`png`/`rgba`，`data` 是 base64）。

## 11. 动画速率由插件自己提供

时间推进速度就是插件声明的 `speed`（写进 `uParams.w`），宿主不做二次缩放。用户想调速率时，
界面与取值都由**插件自己**决定 —— 做法就是插件自绘设置页里的一个设置项（见 `plugin-js-api.md`
的「插件自己的设置页」），值存插件目录的 `config.json`：

```js
var BG_BASE_SPEED = 1;                 // 本插件的基准速度（= 1×）
var BG_RATE_OPTIONS = [0.5, 1, 2];     // 可选倍率

function bgData(rate) {
    return {
        title: "流光背景",
        shader: { bundle: "shader/bg.shaderbundle" },
        speed: BG_BASE_SPEED * rate,   // 声明给宿主的速度
        maxFps: 16,
    };
}

// 顶层先按 1× 注册；读到 config.json 里的倍率后再改
musicxx.ui.registerEntry({ name: "bg", type: "playing.background", data: bgData(1) });

function applyRate(rate) {
    // updateEntry 是**整体替换**：要把完整 data 传回去
    musicxx.ui.updateEntry("bg", bgData(rate));
    musicxx.storage.setConfig("bgRate", rate);
}
```

- 运行期改速率会推 `musicxx.ui.changed`，宿主刷新候选后**正在使用的背景立即用新速度**
  （不用重新选中；`speed` 是每帧现算的）；
- `maxFps`、`resolutionScale`、`animate` 这类要改帧调度/分辨率创建的字段，运行期改完需要
  重新选中该项才会完全生效；
- 示例插件 `example_js_shader` 就这么做：设置页里的「背景动画速率」（0.5× / 1× / 2×）就是它自己的设置项
  （这个插件只演示播放页背景与速率，照抄它最直接）；它把基准速度定为 **1**（此前声明 4，
  等价于整体降速到原来的 0.25×）、并把 1 定义为新的 1×。

## 12. 排查

| 现象 | 原因 |
|---|---|
| 设置里样式显示"不可用：bundle 文件不存在" | 清单/`shader.bundle` 写的相对路径错了，或 bundle 没随插件分发（native 插件需要 `ASSETS`） |
| "shader bundle 版本不符（1 != 2）…" | bundle 是用旧 Flutter 编译的，用当前 SDK 重新打包 |
| "着色器没有声明 uniform 结构体 MusicxxRenderInfo" | 结构体名写错，或只在顶点着色器里声明 |
| "…uColor1 偏移非法" | 结构体布局不符（成员顺序/类型不对，或插入了非 16 字节对齐的成员） |
| 选中后回到内置背景 | 加载或渲染报错被停用；看宿主日志（`ExternPluginLog`）与「外部插件 → 调试」里的背景段落 |
