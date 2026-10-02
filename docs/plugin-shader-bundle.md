# 插件渲染：shader bundle 打包与 uniform 契约

插件可以把**打包期编译好的 shader bundle** 注册成一种宿主渲染样式（当前只有「播放页背景」
一个渲染槽位），由用户在「设置 → 播放页面背景」里选中后生效；同一个 bundle 也能画在插件自己的
页面里（`Shader` 块）。

插件不写 Dart/Flutter 代码，只交出 bundle + 一份参数声明。相关文档：界面与页面块类型见
[plugin-ui.md](plugin-ui.md)，JS 与 C++ 的写法见 [plugin-js-api.md](plugin-js-api.md) /
[plugin-native-api.md](plugin-native-api.md)。

> 现成可编译的例子：`plugins/example_js_shader/shader/` 与 `plugins/example_native/shader/`
> —— 「晶格化」背景（随机点最近邻切块，配色用宿主的 4 个绘制色）。`example_js_shader` 里还有
> 第二个 bundle：**「光圈」背景**（`shaders/ring.frag`：基线圆 + 圆上左右对称的 16 个频谱尖角 +
> 中心光源；尖角之间、尖角内外沿之间都用直线连成一张"蛛网"），照抄同一份打包脚本即可编译。
> 基线版本 3.47.5 的 Flutter SDK
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
    ├── bundle.json           impellerc 的 bundle 描述（晶格背景）
    ├── bundle_ring.json      第二个 bundle 的描述（光圈背景）
    ├── shaders/bg.vert       顶点着色器（两个 bundle 共用一个全屏三角形）
    ├── shaders/bg.frag       片元着色器（晶格）
    ├── shaders/ring.frag     片元着色器（光圈：基线圆 + 频谱尖角连成的蛛网）
    ├── build_bundle.ps1      打包脚本（照抄示例；它按描述文件逐个编译）
    ├── bg.shaderbundle       编译产物（晶格，随插件分发）
    └── ring.shaderbundle     编译产物（光圈，随插件分发）
```

一个插件可以有**多个** bundle（同一个槽位注册多项，或者不同槽位/页面各用一个），
每个 bundle 一份描述文件 + 一个产物文件即可。

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
  vec4 uEnv;      // x = 是否夜间(0/1), y = 是否有有效的封面配色(0/1),
                  // z = 现在是否有音频频谱数据(0/1), w = 保留
  vec4 uColor1;   // 下面这些由插件在 args 里声明（名字随便取，见 §7）
  vec4 uColor2;
  vec4 uColor3;
  vec4 uColor4;
  vec4 uTint;     // 例：{"name":"uTint","source":"musicxx.theme.primary"}
} render_info;
```

规则：

- **结构体名与成员名必须完全一致**（宿主按名字寻址写入）；成员偏移必须 16 字节对齐；
- 结构体大小必须是 **16 的倍数且 ≥ 16**，否则该样式判为不可用并给出原因；
- `uParams` / `uEnv` 由宿主自动写（不用在 `args` 里声明）；
- **可以少声明成员**（宿主跳过不写，着色器读到 0），但不能声明错误的名字
  （成员在结构体里不存在时宿主会跳过；声明了名字却没有对应成员不会报错，只是没有值）；
- 颜色是 `0..1` 的浮点（sRGB 分量）；`uEnv.y = 0` 表示当前没有有效的封面配色分析结果
  （用 `musicxx.icon.*` 来源的参数这时会用插件给的固定值）；
- `uEnv.z = 1` 表示**现在有音频频谱数据**（`musicxx.spectrum.*` 来源这时才是真实的声压与频带）；
  没有数据时 `musicxx.spectrum.*` 一律是 0（静音），要区分"静音"与"没有数据"就看这一位。

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
- **渲染目标是左上角为原点的像素坐标：`gl_FragCoord.y` 越大越靠下**（与画布一致，宿主不会再翻）。
  要按"画面上方"判断（上下不对称的效果）就自己翻一下 y：
  `vec2 p = (gl_FragCoord.xy - 0.5 * res) / (0.5 * min(res.x, res.y)); p.y = -p.y;`
  —— 光圈示例就是这么做的（正上方 = 最低频，写反了频率轴会上下颠倒）；
- 输出写在 `layout(location = 0) out`；
- 完整效果参考 `plugins/example_js_shader/shader/shaders/bg.frag`（晶格化：Worley 噪声切块 +
  4 色双线性混合 + 夜间压暗 + 轻微暗角）与同目录的 `ring.frag`（光圈：极坐标把 16 个频带铺到
  圆上，每个频率点向内外凸出尖角、相邻点与内外沿之间用直线连成蛛网 + 中心光源）。

**写法限制（一份源码要过所有后端）**：源码先被 `impellerc` 编成 SPIR-V，运行时再按后端翻译一次
（GLES 后端翻译成 **GLSL ES 1.00**，也就是 `#version 100`）：

- 内置函数**只用浮点版**：GLSL ES 1.00 里 `clamp` / `min` / `max` / `abs` 只有浮点重载，
  对 `int` 用它们会翻译出编不过的代码 —— 表现为选中后报「渲染失败」、连续失败 3 次被停用
  （`clamp(int(index), 0, 15)` 就是踩过的坑，见 `ring.frag` 的 `clampBandIndex`）。
  整型的钳制用 `if`（或三元表达式）自己写；要 `int` 下标时先在浮点上钳制再 `int(...)`；
- 整型的比较、加减与 `float(i)` / `int(f)` 转换都是安全的；数组下标只能用常量或循环下标
  （GLSL ES 1.00 不允许任意变量当下标）；
- 循环上界写常量（`for (int i = 0; i < 5; ++i)`）；
- 拿不准的写法就在本地验证一次：`impellerc --runtime-stage-gles --gles-language-version=100
  --input=<片元> --input-type=frag --spirv=<临时>.spv --sl=<临时>.glsl` 会把后端要编的那份
  GLSL 打出来（整型 `clamp` 这种问题直接在输出里就能看到）。

---

## 7. 参数（`args`）：值表达式

着色器的每个 `vec4` 成员都在 `args` 里声明，**只有一种写法**：`args` 是一个对象，
键是 uniform 成员名（字母或下划线开头，最长 32 字符），值是一条**值表达式**：
`{"kind": "<节点类型>", ...参数}`。表达式可以像 widget 一样互相嵌套，宿主**每帧**从根节点
求值一次，结果直接写进 uniform —— 插件不用在着色器里重复写时间/滤波逻辑。

**名字空间与解析顺序**（与页面/`AnimatedBuilder` 里用的是同一套值系统）：

- **`musicxx.*` 是框架/应用保留的**：派生来源（主题色 / 封面取色 / 频谱 / 环境量）与官方变量；
  插件不要登记这个前缀的自定义变量；
- **其它名字（允许含 `.`）是自定义的**：`AnimatedBuilder.values` 里的局部通道，或变量目录里的键
  （应用登记、插件登记，如 `plugin.<插件id>.<名>`）；名字里的 `.` 不再用来判别"系统还是自定义"；
- `source` 取值的顺序：**局部作用域（最近一层 `AnimatedBuilder`）→ 变量目录 → 派生来源**；
  都取不到就用 `fallback` / 零值，并记一条日志；
- 需要绕开顺序时写 `scope`：`"auto"`（缺省，按上面的顺序）、`"local"`（只看局部）、
  `"var"`（跳过局部，只看变量与派生来源）。

**值的类型**：只有两类 —— `vec4`（数值 / 颜色 / 向量；标量位置取 `.x`）与**文本**；
开关用 `vec4` 的 0/1（非 0 即真）；尺寸位置可用 `unit: "percent"`（或不带 `kind` 的
`{"percent":40}`，两者等价）。

```jsonc
"args": {
  // 来源：取不到时的兜底值写在节点里（fallback / fallbackNight）
  "uColor1": { "kind": "source", "name": "musicxx.icon.themeMapping.0" },
  "uColor2": { "kind": "source", "name": "musicxx.icon.main", "convert": true, "fallback": "#8899aa" },

  // 常量：数字（广播到 4 个分量）/ 4 个数字的数组 / 颜色；night = 夜间换一个值
  "uTint": { "kind": "const", "value": "#ff8800" },
  "uMix":  { "kind": "const", "value": [0.5, 0.25, 0, 1] },
  "uLine": { "kind": "const", "value": "#f6faff", "night": "#dbe7f7" },

  // 过渡：频谱是 10 帧/秒，包一层 smooth 就没有台阶感（快起慢落 = 电平表手感）
  "uLevel": { "kind": "smooth", "attackMs": 20, "releaseMs": 260,
              "of": { "kind": "source", "name": "musicxx.spectrum.level" } },

  // 动画：时间轴（一次性 / 循环 / 往返）与周期振荡
  "uSpin":   { "kind": "tween", "from": 0, "to": 6.2832, "durationMs": 8000, "repeat": "loop" },
  "uBreath": { "kind": "lfo", "shape": "sine", "periodMs": 2600, "from": 0.92, "to": 1.0 },

  // 组合：波形 × 呼吸曲线、两个颜色按另一条通道混合
  "uWave": { "kind": "mul", "of": [
      { "kind": "smooth", "attackMs": 20, "releaseMs": 260,
        "of": { "kind": "source", "name": "musicxx.spectrum.bands.0" } },
      { "kind": "lfo", "shape": "sine", "periodMs": 2600, "from": 0.92, "to": 1.0 } ] },
  "uTint2": { "kind": "mix",
      "a": { "kind": "source", "name": "musicxx.theme.primary" },
      "b": { "kind": "source", "name": "musicxx.icon.main" },
      "t": { "kind": "lfo", "shape": "triangle", "periodMs": 6000, "from": 0, "to": 1 } },

  // 钳制与重映射
  "uGain": { "kind": "remap", "of": { "kind": "source", "name": "musicxx.spectrum.level" },
             "inMin": 0, "inMax": 1, "outMin": 0.6, "outMax": 1.4 }
}
```

值统一是 `vec4`（4 个 float）：数字常量广播到 4 个分量，数组按位取前 4 个（缺的补 0），
颜色写 `#rrggbb` / `#aarrggbb`。运算节点都是**逐分量**的。

### 7.1 节点

| 节点 | 参数 | 说明 |
|---|---|---|
| `source` | `name`(必填)、`convert`、`scope`(auto / local / var)、`fallback`、`fallbackNight` | 具名来源（见 7.2）；取不到时用 `fallback`（夜间优先 `fallbackNight`） |
| `const` | `value`(必填)、`night`、`unit`(`u` / `percent`) | 常量：数字、4 个数字的数组、`#rrggbb` / `#aarrggbb`、文本 |
| `smooth` | `of`(必填)、`ms` 或 `attackMs` + `releaseMs`（缺省 150 / 150） | 一阶过渡：上升用 `attackMs`、回落用 `releaseMs`（"快起慢落"）；文本直接透传 |
| `tween` | `from`(0)、`to`(1)、`durationMs`(1000)、`delayMs`(0)、`ease`(linear)、`repeat`(once / loop / pingpong)、`phase`(0) | 时间轴过渡；`repeat` 不是 once 时 `phase`（0~1）用来错开相位 |
| `lfo` | `shape`(sine / triangle / saw / square)、`periodMs`(1000)、`from`(0)、`to`(1)、`phase`(0) | 周期振荡（呼吸、扫光、摆动） |
| `add` / `mul` / `min` / `max` | `of`: 节点数组（≥1） | 逐分量组合 |
| `mix` | `a`、`b`、`t` | 逐分量按 `t` 混合（`t` 写常量时就是 lerp）；任一边是文本时按 `t` 的开关选值 |
| `clamp` | `of`、`min`(0)、`max`(1) | 逐分量钳制 |
| `remap` | `of`、`inMin`(0)、`inMax`(1)、`outMin`(0)、`outMax`(1) | 线性重映射 |
| `format` | `value`、`text`(模板，`{0}` 占位) | 值 → 文本（数字按整数/小数自动格式化；`{{`/`}}` 是字面大括号） |
| `concat` | `of`: 节点数组（≥1，可直接写字符串） | 文本拼接 |
| `cmp` | `a`、`b`、`op`(`lt`/`le`/`gt`/`ge`/`eq`/`ne`) | 比较 → 0/1（两边都是文本时按字典序比较） |
| `logicAnd` / `logicOr` | `of`: 节点数组（≥1） | 逻辑与 / 或 → 0/1 |
| `logicNot` | `of` | 逻辑非 → 0/1 |
| `select` | `cond`、`then`、`else` | 按条件选值（数值或文本都能选） |

`ease` 可选：`linear` / `inQuad` / `outQuad` / `inOutQuad` / `inCubic` / `outCubic` /
`inOutCubic` / `inSine` / `outSine` / `inOutSine` / `outBack` / `outElastic`
（不认识的按 `linear` 处理，日志里会提示）。

### 7.2 来源

| `source.name` | 取到的值 |
|---|---|
| `musicxx.theme.primary` | 主题主色 |
| `musicxx.theme.background` / `musicxx.theme.backgroundCross` | 主题背景色 / 第二背景色 |
| `musicxx.theme.textMain` / `musicxx.theme.textCross` | 主要 / 次要文字色 |
| `musicxx.theme.textTitle` / `musicxx.theme.titleBackground` | 标题文字色 / 标题底色 |
| `musicxx.theme.button` / `musicxx.theme.buttonSelect` | 按钮内容色 / 按钮选中内容色 |
| `musicxx.theme.error` / `musicxx.theme.wave` | 错误提示色 / 歌曲图波浪色 |
| `musicxx.icon.main` / `musicxx.icon.light` / `musicxx.icon.lightMuted` / `musicxx.icon.dark` / `musicxx.icon.darkMuted` | 当前歌曲封面的提取色（分析结果原始色；`convert: true` 时套昼夜转换） |
| `musicxx.icon.dominant.0` .. `musicxx.icon.dominant.3` | 封面提取色的主色候选 |
| `musicxx.icon.themeMapping.0` .. `musicxx.icon.themeMapping.3` | 封面颜色**经主题/背景映射后的 4 个绘制色**：内置播放页背景实际用的就是这 4 色（已经套过昼夜转换与观感归一，`convert` 对它不再生效；没有分析结果时是固定的默认色） |
| `musicxx.spectrum.level` | 当前音频响度（0~1）写在 4 个分量（x = y = z = w） |
| `musicxx.spectrum.bands.0` .. `musicxx.spectrum.bands.3` | 当前音频的 16 个频带（低频在前，0~1；每项 4 个连续频带写在 xyzw） |
| `musicxx.env.night` / `musicxx.env.hasPalette` / `musicxx.env.hasSpectrum` | 环境量：是否夜间 / 有没有封面配色 / 有没有频谱数据（0 或 1，写在 4 个分量）；配 `mix` 就能写出"按环境切换"的值 |
| **其它任意名字（可含 `.`）** | 变量目录里的键：应用登记的官方变量、插件登记的自定义变量（如 `plugin.<插件id>.<名>`）；值是数字 / 布尔 / 文本，宿主按字段期望的类型转换 |
| **局部通道名（`AnimatedBuilder.values` 的键）** | 优先于上面两类；只在那一层作用域里可见（页面里的用法见 `plugin-ui.md`） |

**频谱来源（`musicxx.spectrum.*`）** 读的是内置『音乐动效』插件提取的数据（每 100 ms 一帧）：

- 想要全部 16 个频带就写 `musicxx.spectrum.bands.0` ~ `musicxx.spectrum.bands.3` 四项（每项 4 个连续频带）——
  『光圈』示例就是这么用的：它把 16 个频带按左右对称摆到圆周上（每项对应圆上的一个频率点，
  第 i 项的分量 = 第 4i..4i+3 个频带），每个点向内外凸出一个尖角；
- **数据本身的过渡由宿主自动做**：按播放位置在两帧频谱之间插值（就是内置动效"自动插入过渡值"
  的做法），所以 10 帧/秒的数据在几十帧/秒的渲染里也是连续的；只有逐帧数据（结果里的
  `source` 是 `live`）时没有下一帧可用，这时用 `smooth` 节点做过渡；
- 没有数据时**写全 0（静音），不看 `fallback`**：要区分"静音 / 没启用 / 正在加载"就看
  `musicxx.env.hasSpectrum`（或着色器里的 `uEnv.z`）；
- 16 个频带是 256 个频点按线性分组取平均（与能力的 `GetAudioSpectrum` 同一口径，
  频带 0 最低、频带 15 最高）；想要别的口径或整曲数据用 `musicxx.media.spectrum` 动作自己算；
- 能取到数据的条件：正在播放**本地/缓存**的音频（网络流要先有本地缓存），时长不超过
  15 分钟，且内置『音乐动效』插件处于启用状态。

### 7.3 时间与状态

- `tween` / `lfo` 用的是**动画时间**：与着色器里的 `t = uParams.z * uParams.w` 同一口径，
  插件设置页里的"动画速率"会一起带动它们；`animate: false` 时时间恒为 0（动画停在起点）；
- `smooth` 的状态（上一帧输出）跟着**参数表**走：插件重新声明**同一份参数**时沿用旧状态
  （只是改了 `speed` 这类字段不会打断正在跑的过渡）；声明变了、换 bundle 或换样式时从新声明
  开始 —— 第一次求值直接取目标值，不会从 0 慢慢爬上来；
- 首帧、长时间没出帧（不可见恢复、卡顿）之后的那一步不做过渡（不会"一步跳完"）。

### 7.4 上限与降级

- 一份 `args` 最多 **16 个成员**；单个成员最多 **32 个节点**、**8 层**嵌套；
  `durationMs` / `periodMs` 会钳制在 16 ms ~ 3600000 ms；
- 写错的项**被忽略并记一条日志**（未知 `kind`、缺必填参数、成员名非法、超过上限…），
  不影响同一份声明里的其它成员，也不会让整个样式不可用；
- 播放页背景**完全不写 `args`** 时，宿主默认给 `uColor1..4 ← musicxx.icon.themeMapping.0..3`
  （观感与内置背景一致）；页面里的 `Shader` 块不做这个兜底（不写就没有颜色）；
- 旧的 `data.colors` 字段与旧的扁平 `args` 写法（数组 + `source` / `value` / `valueNight` /
  `convert` 字段）**都已移除**：还写着的插件不会解析 `args`（当作"没有参数"），
  宿主日志里会给一条迁移提示。

---

## 8. 其它字段与上限

| 字段 | 默认 | 范围 | 说明 |
|---|---|---|---|
| `title` | 必填 | — | 设置列表里的样式名 |
| `depict` | `""` | — | 副标题（写 `subtitle` 也可以，且优先） |
| `enabled` | `true` | — | false = 不在设置列表里出现 |
| `args` | 空 | ≤ 16 个成员 | 着色器参数：成员名 → 值表达式（见 §7）；背景槽位不声明时默认给内置 4 色 |
| `speed` | 4 | 0..20 | 时间推进速度，写进 `uParams.w`（着色器要自己乘，见 §5）。**由插件自己决定**，宿主不做二次缩放 |
| `maxFps` | 16 | 1..30 | 帧率上限 |
| `resolutionScale` | 1.0 | 0.25..1.0 | 降采样后由宿主放大（省 GPU） |
| `animate` | true | — | false = 只渲染一帧（时间恒为 0） |
| `scrim` | 0 | 0..0.8 | 背景上叠一层暗化（页面里的 `Shader` 块没有这个字段） |
| `foregroundStyle` | `mask` | `mask` / `neumorphism` | 播放页前景（歌曲名/图标）用浅色遮罩还是常规样式；未知值按 `mask` |

渲染目标最长边由宿主限制在 **1280 px**
（超出的部分按比例缩小，再交给 Flutter 放大）；bundle 体积、路径长度与 UI 项数量都不设上限。

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
    "args": {
      "uColor1": {"kind": "source", "name": "musicxx.theme.primary"},
      "uColor2": {"kind": "source", "name": "musicxx.icon.main", "convert": true, "fallback": "#8899aa"}
    }}}
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

**当前音频频谱**（内置『音乐动效』提取的数据）有两条读法，着色器与 JS 各用一条：

```js
// ① 着色器参数（每帧现读，零成本）：见 §7 的 musicxx.spectrum.* 来源
// ② JS 侧读一帧快照（异步动作）：当前这一帧的响度与频带
const s = await musicxx.media.spectrum({ bandCount: 16, unit: "normalized" });
// s = { ok:true, status:"ready"|"loading"|"none"|"off"|"unavailable",
//       available, loading, reason, source:"extracted"|"live",
//       srcKey, name, artist, durationMs, positionMs,
//       frameHz:10, frameIndex, frames, binCount:256, bandCount, unit,
//       level, bands:[...], frameData? }        // frameData 需要 includeBins:true

// ③ 同步读状态镜像（播放中约 10 Hz 推送，适合插件自己的绘制循环）
const live = musicxx.state.get("musicxx.state.spectrum");
// { status, available, loading, reason, source, srcKey,
//   frameHz, frameIndex, frames, bandCount, level, bands:[16] }
```

- 没有数据（未启用『音乐动效』/ 正在提取 / 来源不支持）都回 `ok: true`：
  `status` = `off` / `loading` / `none` / `unavailable`，`available` 为假、`bands` 与 `level` 是 0；
- `unit` 可选 `normalized`（0~1，缺省）/ `db`（频点 -80~0 dB，响度 -60~+20 dB）/ `raw`（0~255）；
  想自己分析整曲频谱用能力 `GetAudioSpectrum`（`musicxx.feature.call`）。

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
        args: { /* 成员名 → 值表达式，见 §7 */ },
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
  基准速度定为 1（即 `speed` 1/2/4）；它注册了两种背景样式（晶格 / 光圈），改速率时
  **两项都要重新声明**（`musicxx.ui.updateEntry` 是整体替换，逐个调用即可）；
  `example_native` 的 C++ 版声明 `speed: 4`。

---

## 12. 排查

| 现象 | 原因 |
|---|---|
| 设置里显示「bundle 文件不存在」 | `shader.bundle` 的相对路径写错，或 bundle 没随插件分发（native 插件要用 `ASSETS` 复制进产物目录） |
| 「shader.bundle 必须是插件目录内的相对路径」 | 写了绝对路径、含 `..`、含反斜杠或以 `/` 开头 |
| 「shader bundle 版本不符（1 != 2）」 | bundle 是用别的 Flutter 版本编译的，用当前 SDK 重新打包 |
| 「不是可用的 shader bundle（读不到格式版本）」 | 文件不是 impellerc 产物（或为空 / 被截断） |
| 「不是可用的 shader bundle（读不到格式版本）」 | 用当前 Flutter SDK 的 `impellerc --shader-bundle` 重新编译 |
| 「bundle 里找不到片元入口『MusicxxRenderFragment』」 | `bundle.json` 的键名与 `shader.fragment` 不一致 |
| 「着色器没有声明 uniform 结构体 MusicxxRenderInfo」 | 结构体名写错，或只在顶点着色器里声明 |
| 「MusicxxRenderInfo.uColor1 偏移非法」 | 结构体布局不符（成员顺序 / 类型不对，或插了非 16 字节对齐的成员） |
| 页面里那块 `Shader` 一直是空白 | 父块没有给出确定尺寸（用 `SizedBox` / `Expanded` 给它高度）；或 bundle 不可用（日志里有原因） |
| 选中后回到内置背景 | 加载或渲染报错被停用（连续失败 3 次才停用，其间保留最后一帧）；看宿主日志与「外部插件 → 调试」里的背景段落 |
| 动画不动 | `animate: false`、`speed: 0`，或着色器没有把 `uParams.z` 乘上 `uParams.w`；播放页被遮挡 / 切后台时本来就不渲染（`visible: false`） |
| `musicxx.spectrum.*` 一直是 0（画面不跟着音乐动） | `uEnv.z` 为 0 = 现在没有频谱数据：内置『音乐动效』插件没启用、还在提取、歌曲不是本地/缓存来源（网络流要先有本地缓存）、或时长超过 15 分钟。要用 `uEnv.z` 判断，别把 0 当成"音乐静音" |
| 画面比预期快/慢 | `uParams.z` 是真实秒数、`uParams.w` 是插件声明的速度：宿主的基准速度就是插件给的值（示例把 1 当 1×） |
