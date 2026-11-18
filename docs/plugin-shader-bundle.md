# 插件渲染：shader bundle 打包与 uniform 约定

插件可以把**打包期编译好的 shader bundle** 画在插件自己的页面里、或者当成主程序的渲染样式（当前有三个**由用户选择**的渲染槽位：
「播放页背景」`player.background`、「播放页歌曲图」`player.icon`（见 §5.3）与「播放页音频动效」
`player.visualizer`（见 §5.4）；另有一个**按页面路由自动生效**的「普通页面背景」`page.background`
（见 §5.5）），由用户在设置里选中后生效（命令行 / agent 用
`musicxx-cli ui-slot list|set` 切，见 [plugin-agent-cli.md](plugin-agent-cli.md) §5.5）。

**着色器只有一种写法：块 `musicxx.Shader`**（见 §9）。两个背景都只声明一棵块树（UI 项的 `view`），
着色器块就是树里的一块（`bundle` + `args` + 帧率 / 分辨率 / 速度）；歌曲图 / 动效的 `shader` 形态
与它共用同一份解析。**纹理（当前歌曲封面 / 插件绑定的图片）写在块的 `args` 里**（§5.1 / §5.2）。

插件不写 Dart/Flutter 代码，只交出 bundle + 一份参数声明。相关文档：界面与页面块类型见
[plugin-ui.md](plugin-ui.md)，JS 与 C++ 的写法见 [plugin-js-api.md](plugin-js-api.md) /
[plugin-native-api.md](plugin-native-api.md)，整体流程与注意事项见 [plugin-guide.md](plugin-guide.md)。

> 现成可编译的例子：`plugins/example_js_playing_bg/shader/` 与 `plugins/example_native/shader/`。
> `example_js_playing_bg` 一个插件就有五个 bundle，两种用法各有一组：
> - **封面类**（纹理来自当前歌曲封面）：「模糊」`shaders/heat.frag`、「沉浸」`shaders/card.frag`、
>   「黑胶」`shaders/vinyl.frag`；
> - **图案类**（不碰封面纹理）：「晶格」`shaders/bg.frag`（随机点最近邻切块，配色用主程序的 4 个绘制色）、
>   **「光圈」**`shaders/ring.frag`（从正上方开始顺时针排 64 个点、一个点一个
>   频带，尖角向内外凸出、相邻点之间用直线连成一张"蛛网"，不画基线圆）。
> 五个 bundle 共用一份顶点着色器 `shaders/full.vert`，照抄同一份 `build_bundle.ps1` 即可编译。
> 基线版本 3.47.5 的 Flutter SDK
> 编译出来的 bundle 可以直接用（`format_version = 2`）。
> 它同时演示了歌曲图槽位（§5.3）与封面取色能力 —— 见 `plugins/example_js_playing_bg/plugin.js`。

---

> **术语约定**：本文档中的**主程序**指的就是 **musicxx**（Flutter/Dart 业务层）；外置插件的 C++ 框架层称为**插件框架**；插件目前分为**动态库插件**与**JS插件**。


## 1. 四个步骤

```text
① 写两个着色器：顶点（全屏三角形）+ 片元（声明 uniform 结构体 MusicxxRenderInfo）
② 用 impellerc 编译成一个 bg.shaderbundle（五个后端一次编完，一个文件全平台通用）
③ 在插件里注册一种样式：UI 项 musicxx.ui.playing.background，`view` 里放一块 `musicxx.Shader`
   （普通页面背景用 musicxx.ui.page.background，见 §5.5）
④ 用户在「设置 → 播放页面背景」里选中它 → 生效（未选中时零成本：不读 bundle、不分析封面）
```

---

## 2. 目录结构

```text
<插件目录>/
├── plugin.yaml
├── plugin.js / <插件库文件>
└── shader/
    ├── bundle_lattice.json   impellerc 的 bundle 描述（晶格）
    ├── bundle_ring.json      第二个 bundle 的描述（光圈）
    ├── shaders/full.vert     顶点着色器（全屏三角形，五个 bundle 共用）
    ├── shaders/bg.frag       片元着色器（晶格）
    ├── shaders/ring.frag     片元着色器（光圈：64 个频带尖角连成的蛛网，不画基线圆）
    ├── build_bundle.ps1      打包脚本（照抄示例；它按描述文件逐个编译）
    ├── bg.shaderbundle       编译产物（晶格，随插件分发）
    └── ring.shaderbundle     编译产物（光圈，随插件分发）
```

一个插件可以有**多个** bundle（同一个槽位注册多项，或者不同槽位/页面各用一个），
每个 bundle 一份描述文件 + 一个产物文件即可。
（`example_js_playing_bg` 就是这么排的：五个 bundle 共用一份顶点着色器 `shaders/full.vert`，
描述文件 `bundle_heat.json` / `bundle_card.json` / `bundle_vinyl.json` / `bundle_lattice.json` /
`bundle_ring.json` 分别编出 `heat` / `card` / `vinyl` / `bg` / `ring` 五个 `.shaderbundle`，
同一个 `build_bundle.ps1` 逐个编译。）

`bundle_lattice.json`：

```json
{
  "MusicxxRenderVertex": { "type": "vertex", "file": "shaders/full.vert" },
  "MusicxxRenderFragment": { "type": "fragment", "file": "shaders/bg.frag" }
}
```

这两个键就是**入口名**。主程序默认按 `MusicxxRenderVertex` / `MusicxxRenderFragment` 查找，
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
- bundle 里有 `file_identifier = "IPSB"`；主程序加载前会自己读字节校验 `format_version`。

---

## 4. 格式版本（硬性）

- `format_version` 由引擎版本决定：**当前（Flutter 3.47.x）是 2**；
- 版本不符时，该样式在设置列表里显示
  `shader bundle 版本不符（1 != 2），请用当前 Flutter SDK 重新打包`，**不可选**，播放页保持内置背景；
- 所以：**请用与目标应用相同的 Flutter 大版本编译**，换 Flutter 版本后重新跑一次打包脚本。

---

## 5. uniform 约定

主程序每帧填充一个固定名字的结构体 `MusicxxRenderInfo`：

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
  vec4 uCoverInfo; // 声明了封面纹理时由主程序自动写：见 §5.1
} render_info;
```

规则：

- **结构体名与成员名必须完全一致**（主程序按名字寻址写入）；成员偏移必须 16 字节对齐；
- 结构体大小必须是 **16 的倍数且 ≥ 16**，否则该样式判为不可用并给出原因；
- `uParams` / `uEnv` 由主程序自动写（不用在 `args` 里声明）；声明了纹理（`args` 里 `kind: "texture"`）时，
  纹理信息成员（缺省名 `<成员名>Info`，见 §5.1）同样由主程序自动写，也不要在 `args` 里声明；
- **可以少声明成员**（主程序跳过不写，着色器读到 0），但不能声明错误的名字
  （成员在结构体里不存在时主程序会跳过；声明了名字却没有对应成员不会报错，只是没有值）；
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

## 5.1 封面纹理：把当前歌曲封面交给着色器采样

播放页背景经常要用当前歌曲封面（模糊铺底、圆心盘、扭曲、像素化…）。**不要把封面字节读进插件再想办法画**
—— 能力 `musicxx.media.cover` 那套是给"读数据"用的（插件脚本拿到的是 base64 字节，进不了 GPU）。
在着色器的 `args` 里声明一条**纹理**（`"kind": "texture"`），剩下的事由主程序做：解码 → 缩放
（可选中心裁剪成正方形）→ 可选**预模糊**（引擎的图像滤镜，GPU 一次）→ 上传成 GPU 纹理 → 每帧绑定到
你指定的 `sampler2D`。

```jsonc
// 背景样式（musicxx.ui.playing.background）的 view：块 musicxx.Shader 的 args 里
{
  "kind": "musicxx.Shader",            // 根块直接写着色器就铺满整页（见 §5.5）
  "bundle": "shader/bg.shaderbundle",
  "args": {
    // 成员名就是着色器里的 sampler 名
    "uCover": {"kind": "texture", "source": "musicxx.cover",
               "size": 512, "blur": 0, "square": false, "smooth": true},
    "uZoom": {"kind": "const", "value": 2}
    },
    "speed": 1, "maxFps": 24, "animate": true
  }]
}
```

```glsl
uniform sampler2D uCover;            // 名字与 args 成员名一致

uniform MusicxxRenderInfo {
  vec4 uCoverInfo;                   // 信息成员名缺省是 <成员名>Info；不用在 args 里声明
} render_info;

void main() {
  vec2 uv = gl_FragCoord.xy / max(render_info.uParams.xy, vec2(1.0));
  // 没有封面时 uCoverInfo.w 是 0：这时不要用采样结果（主程序绑的是 1×1 透明占位纹理）
  float coverOn = render_info.uCoverInfo.w > 0.5 ? 1.0 : 0.0;
  vec3 coverColor = texture(uCover, uv).rgb;
  ...
}
```

| 字段 | 默认 | 范围 | 说明 |
|---|---|---|---|
| 成员名（= `sampler2D` 名） | — | GLSL 标识符 | `args` 的成员名就是采样器名；插件在自己的着色器里声明同名 `uniform sampler2D` |
| `kind` | — | `texture` | 固定写 `"texture"`：这一条从参数表里拆出来当纹理声明（剩下的成员才是参数） |
| `source` | 必填 | `musicxx.cover` / `musicxx.songlist.cover` / `musicxx.image.<key>` | 纹理来源：当前歌曲封面（§5.1）/ 当前打开的歌单封面（§5.6，只在歌单页面内可用）/ 插件自己绑定的图片（§5.2） |
| `info` | `<成员名>Info` | GLSL 标识符，或 `""` | 信息成员名；写 `""` 表示不要这个成员；与成员名同名时退回缺省名 |
| `size` | 512 | 128..1024 | （只对封面）纹理最长边（px）：等比缩放、**不放大** |
| `blur` | 0 | 0..64 | （只对封面）预模糊强度（sigma，px）；0 = 原图。**注意这是"纹理上的" sigma**：纹理铺满屏幕（还可能被插件再放大）后，屏幕上看到的模糊 ≈ `blur × 纹理→屏幕的放大倍数` —— 512 的纹理铺在 1280 宽的窗口里放大 5 倍左右，`blur: 34` 会糊成一片渐变，`blur: 10~16` 才看得出封面内容 |
| `square` | false | — | （只对封面）true = 中心裁剪成正方形（`size` × `size`），false = 保持原图比例 |
| `smooth` | true | — | 采样过滤：true = 线性（缩放平滑），false = 最近邻（像素风） |

- 一个着色器**最多 4 张纹理**（每张占一份显存与一个采样器）；信息成员计入参数成员上限（80）；
- 写错的一项只丢自己（记一条日志，带原因），不影响同一份 `args` 里的其它成员，也不会让整项不可用；
- `musicxx.cover` 与 `musicxx.image.<key>` 是同一条声明的两种 `source`：图片项只有 `smooth` 有意义
  （尺寸由插件绑定时决定），写 `size` / `blur` / `square` 也不会报错，只是不起作用。
- 还有一条 `musicxx.songlist.cover`（歌单页面里的封面，§5.6）：参数与 `musicxx.cover` 完全一致，
  只是源图换成"当前打开的歌单的封面"，而且**只在歌单页面内可用**。

`uCoverInfo = (纹理宽, 纹理高, 原图宽高比, 是否有封面)`：

- 纹理是**等比缩放**的（`square: false` 时宽高比就是原图），拿 `uCoverInfo.xy` 可以算自己的 uv
  —— 例如"铺满画面、超出裁掉"（cover 裁切）的写法见
  `plugins/example_js_playing_bg/shader/shaders/bg.frag`；
- `uCoverInfo.w = 0` 表示这一帧**没有封面**（还没加载好 / 这首歌没有封面）：主程序仍然会绑定纹理，
  但内容是 1×1 透明占位 —— **不要靠采样到的颜色判断有没有封面**，看这一位；
- `uCoverInfo.z` 是原图宽高比（`square: true` 时纹理是方的，这一位仍是原图比例）。

约定与代价：

- **纹理坐标与渲染目标同向**：渲染目标是左上角原点（`gl_FragCoord.y` 向下增大），纹理也是
  v = 0 在图片顶部，直接用 `uv` 采样就是正立的；把坐标翻过（`p.y` 向上为正）的着色器要记得翻回去
  （`vec2(disk.x, -disk.y) * 0.5 + 0.5`，见 `ring.frag` 的圆心封面）；
- **"铺满 / 放大"的采样步长要逐分量相除**：`uCoverInfo.xy` 是纹理尺寸、`uParams.xy` 是渲染目标尺寸，
  把纹理按 cover 铺满（超出裁掉）时采样步长是 `res / (texSize * scale)`，其中
  `scale = max(res.x / texSize.x, res.y / texSize.y) * 放大倍数`。写成 `texSize * scale / res`（倒数）
  在屏幕与纹理宽高比不同时会把画面**拉伸变形**（表现为"左右压缩上下拉伸"），叠加的坐标偏移还会被
  `clamp` 到纹理边缘吃掉（看起来完全没有动画）。示例见
  `plugins/example_js_playing_bg/shader/shaders/heat.frag`（铺满全屏）与同目录的 `card.frag`
  （只在卡片里铺满、还要居中裁剪）以及同目录的 `bg.frag`；
- 想让画面只显示封面的一部分（"放大"背景），**用自己声明的 `args` 成员传倍率**（上面例子的 `uZoom`），
  在着色器里按"总体放大倍率"算采样步长：`coverScale = max(fillScale, uZoom.x)`、
  `uvStep = res / (texSize * coverScale)`（`fillScale = max(res.x/texSize.x, res.y/texSize.y)`
  是铺满所需的最小倍率）—— `plugins/example_js_playing_bg` 就是这么做的（设置页可切 1× / 1.5× / 2× /
  2.5× / 3×，真机自检里有"倍率越大画面越局部"的回归用例；`card.frag` 是同一个套路的第二种用法：
  卡片尺寸固定、只把纹理在卡片里放大，居中取景）；
- 要把封面裁成**圆形**（唱片标签、头像那样的圆形封面），声明 `"square": true`（纹理是中心裁剪的
  正方形），再在圆内用 `uv = 圆内坐标 × 0.5 + 0.5` 采样：圆内正好对应纹理的内切圆
  （`plugins/example_js_playing_bg` 的『黑胶』盘心就是这么做的，见 `shaders/vinyl.frag`）。
- 没声明纹理时**零成本**：不解码、不上传、不占显存（一张 512×512 的纹理约 1 MiB）；
  连参数里都没问 `musicxx.env.hasCover` 时，渲染每帧也不会去读"现在有没有封面"；
- **不保留解码结果**：纹理只活在"正在用它的那个渲染视图"里 —— 视图销毁、用户切回内置样式、
  插件停用或卸载、关闭『拟声++』时都会立即释放（丢引用并释放包装用的图像）；没有跨视图缓存，
  也不会留一份"以后可能用到"的解码图；解码用的（独立命名空间的）图片缓存按
  （provider + 解码尺寸）键控、**不在每次准备前清空**（每清一次就会让切歌时反复解码、
  网络封面还要重新下载，表现为"切歌卡几秒"）；纹理准备还有 120ms 防抖（切歌时封面源
  常连着变几次）；
- 封面变化（换歌、同一首歌换封面）时主程序重新准备一次（几毫秒），**不是每帧上传**；模糊也是一次性
  预生成，不是每帧做 —— 想在着色器里自己模糊当然也行（多采样几次，代价自己算）；
- 同一张封面可以被多个渲染声明各自使用（背景的块与页面里的 `Shader` 块各拿一份纹理，各自释放，互不影响）；
- 封面可能带透明（PNG）：采样到的 `a` 就是原图的 alpha，透明处 `rgb` 是 0，需要不透明底就自己 `mix`；
- **着色器里声明了 `sampler2D`，就一定要在 `args` 里有同名的纹理声明**：主程序只绑定 `args` 成员名
  写出的名字；只声明了纹理而着色器里没有对应 uniform 时主程序会记一条日志并跳过（安全），反过来
  （着色器有 sampler、`args` 里没声明）就是**采样一张从未绑定的纹理**，在部分后端上行为未定义
  （软件后端实测会崩）。声明了封面纹理而当前没有封面时主程序绑的是 1×1 透明占位纹理，
  采样是安全的 —— 用 `uCoverInfo.w` 判断要不要用它。

---

## 5.2 插件绑定的图片：把插件自己的图交给着色器

封面纹理只能画"当前歌曲封面"。要让着色器画**插件自己准备的图**（自己下载的素材、脚本合成的图片、
处理过的封面），就先把图片交给主程序绑定，再在着色器里用 `musicxx.image.<key>` 这条来源引用它：

```js
// 1) 把图片交给主程序（主程序解码 + 上传成 GPU 纹理，登记进这个插件的图像表）
await musicxx.capability.call("musicxx.media.bindImage", {
    key: "mini",            // 本地名字，纹理声明里用它引用
    data: base64,           // 图片字节（png / jpeg / rgba）
    format: "png",          // png / jpeg / rgba（rgba 还要 width / height）
});

// 或者"读封面顺便绑"：复用同一次解码的像素，不重复解码
await musicxx.capability.call("musicxx.media.cover", { size: 96, format: "rgba", bind: "mini" });

// 不再需要时释放（不释放也会随插件停用/卸载、插件框架停止自动摘掉）
await musicxx.capability.call("musicxx.media.unbindImage", { key: "mini" });
```

```jsonc
// 2) 着色器的 args 里声明（与封面同一条语法，只是 source 换掉）
{
  "kind": "musicxx.Shader",
  "bundle": "shader/bg.shaderbundle",
  "args": {
    "uMini": {"kind": "texture", "source": "musicxx.image.mini", "smooth": true}
  }
}
```

```glsl
uniform sampler2D uMini;                 // 名字 = args 成员名

uniform MusicxxRenderInfo {
  vec4 uMiniInfo;                        // 缺省 <成员名>Info；不要就用 "info": ""
} render_info;

void main() {
  vec2 uv = gl_FragCoord.xy / max(render_info.uParams.xy, vec2(1.0));
  // 插件还没绑定 / 已解绑时这一位是 0（主程序绑的是 1×1 透明占位纹理）
  float hasImage = render_info.uMiniInfo.w > 0.5 ? 1.0 : 0.0;
  vec3 color = texture(uMini, uv).rgb;
  ...
}
```

| 字段 | 默认 | 说明 |
|---|---|---|
| `source` | 必填 | `musicxx.image.<key>`：`<key>` 就是 `bindImage` 时写的键（1..64 位字母数字与 `_ - .`）；写错格式的这一条被忽略并记日志 |
| `info` | `<成员名>Info` | 信息成员名：`(纹理宽, 纹理高, 宽高比, 是否有内容)`；写成 `""` 或与成员名同名时不写信息成员 |
| `smooth` | true | 采样过滤（false = 最近邻） |

约定与代价：

- **图片由插件提供，主程序只做解码与上传**（不检查内容）：张数与尺寸都**不限制**
  （显存由插件自己控制）；`rgba` 格式仍要求宽高大于 0、字节数匹配；
- **绑定是临时资源**：插件卸载 / 停用、插件框架停止、关闭『拟声++』时整张表被摘掉（只丢引用，
  交给垃圾回收）；渲染视图每帧只做一次整数比较，没绑定就是零成本；
- **没声明图片纹理的渲染**：不查表、不绑定、不占显存；声明了但还没绑定时绑 1×1 透明占位纹理；
- 插件用同一个 key 重新绑定（换成新图）时，正在画的背景会在下一帧用上新图；
- 采样坐标与封面纹理完全一致（渲染目标与纹理都是 y 向下；把 `p.y` 翻过（向上为正）的着色器
  要翻回去，见 `ring.frag` 的圆心封面）；
- 与封面一样，**着色器里声明了 `sampler2D` 就必须在 `args` 里有同名的纹理声明** —— 采样一张
  从未绑定的纹理在部分后端上行为未定义。

---

## 5.3 播放页歌曲图槽位（`musicxx.ui.playing.icon`）：接管中间那张歌曲图

除了背景，插件还能接管沉浸式播放页**中间那张歌曲图**（内置显示是"封面图 + 播放状态按钮 / 视频"）。
它是一个独立的渲染槽位 `player.icon`：用户在「设置 → 主题 → 播放页面歌曲图」里选择，或插件用
`musicxx.ui.slot.set(id, {slot: "player.icon"})` 切换。

内置显示也在这份列表里，按形状分成三条候选：`musicxx.circle`（圆形，会跟着旋转与波浪）、
`musicxx.square`（方形）、`musicxx.adaptive`（按封面比例自适应，不裁剪）。
另外两个 id 不进入列表，是动作：

- `musicxx.previous`：恢复主程序记下的"上一个形状"（`musicxx.default` 重置掉的那个）；
  没有记录时不动设置项，重复调用也安全 —— **交还歌曲图用它**；
- `musicxx.default`：把形状写回设置项的默认值（圆形）；写之前会把原来的值记下来，
  只在确实想重置用户设置时用。

`musicxx.ui.slot.list` 回复里 `source: "builtin"` 的是三条形状；`selectedId` 在内置时是它们的 id。
三个槽位（背景 / 歌曲图 / 动效）的内置项 id 与两个动作 id 是同一套写法，见下面 §10。

```jsonc
// 渲染项（musicxx.ui.playing.icon 的 data）—— 三种形态挑一种
{
  // 1) shader：插件着色器绘制（渲染输入与块 musicxx.Shader 同一份解析）
  "title": "我的歌曲图", "mode": "shader",
  "shader": {"bundle": "shader/icon.shaderbundle"},
  "args": {                       // 纹理与参数都写在 args 里（见 §5.1 / §7）
    "uCover": {"kind": "texture", "source": "musicxx.cover", "size": 512},
    "uTint":  {"kind": "source", "name": "musicxx.icon.themeMapping.0"}
  },
  "speed": 1, "maxFps": 30, "resolutionScale": 1.0, "animate": true,

  // 2) view：插件用声明式界面组合（`view` 写视图 id，或直接给视图对象）
  //    "mode": "view",
  //    "view": "iconView",           // 同名能力取页面描述（快照语义）
  //    "view": {"blocks": [...]},    // 内联视图

  // 3) none：不显示内容（`keepSpace` 缺省 true = 保留原来的占位尺寸）
  //    "mode": "none", "keepSpace": true
}
```

| 字段 | 默认 | 说明 |
|---|---|---|
| `mode` | 按字段推断 | `shader`（有 `shader.bundle`）/ `view`（有 `view`）/ `none`（都没有）；显式写错的值按推断走并记日志 |
| `shader` | — | `shader` 形态的 bundle（`{bundle, vertex?, fragment?}`），必填 `bundle` |
| `view` | — | `view` 形态：视图 id 字符串（调插件同名能力取页面）或内联视图对象 |
| `keepSpace` | `true` | 只有 `none` 形态用：是否保留占位尺寸（false = 完全不占位，播放页其余元素会跟着上移） |
| `fill` | `false` | `shader` 形态：false = 在可用宽度里居中画一个方形（边长 = 内置歌曲图的高度），true = 铺满可用区域 |
| `args` / `speed` / `maxFps` / `resolutionScale` / `animate` | 同块 | 渲染输入与块 `musicxx.Shader` 共用同一份解析（纹理也写在 `args` 里，见 §5.1） |

约定与代价：

- **接管是"整个显示"**：原来的"点击歌曲图播放 / 暂停"、滑动切歌（`musicxx.ui` 设置里的
  「滑动切换歌曲」）、视频播放按钮都不再出现。需要这些交互的插件自己在界面里放按钮 ——
  `view` 形态可以声明动作（`dispatch` 调自己的能力、`command` 调官方能力如
  `musicxx.player.toggle`）；
- `shader` 形态的渲染区域就是**内置歌曲图占的那块**：`uParams.xy` 是它的像素尺寸（不是整屏），
  插件按这个尺寸画；`animate: false` 时只出一帧；
- 没有额外 uniform 成员：通用成员（`uParams` / `uEnv`）够描述"画在多大的一块里"，
  颜色 / 环境量用 `args` 里的来源按需声明（与块一致）；
- 槽位状态广播：`musicxx.state.uiSlots["player.icon"]`（`itemId` = 现在谁在画、
  `visible` / `width` / `height` 由挂载点上报）。插件可以据此知道"是不是自己在画"，
  例如"背景是自己时才接管歌曲图"这种跟随（`plugins/example_js_playing_bg` 就是这么做的：它的六种
  背景样式共用同一个歌曲图接管项，靠 `itemId` 判断背景是不是自己）；
- 失效回退：`view` 取不到内容、连续渲染失败、插件停用 / 卸载 / 关闭『拟声++』时自动回退内置显示，
  原因写在设置列表的那一行。

完整示例：`plugins/example_js_playing_bg/`（六种播放页背景 + 歌曲图接管 + 封面取色三件事一起做的插件）。

---

## 5.4 播放页音频动效槽位（`musicxx.ui.playing.visualizer`）：自己画频谱动画

播放页的音频可视化（内置是『音乐动效』的四种样式：方块 / 竖线 / 粒子 / 雾气）也能交给插件：
注册 UI 项 `musicxx.ui.playing.visualizer`，它与内置四种样式在**同一个列表**里选
（设置 → 主题 →「播放页音频动效」，『音乐动效』设置页里的样式列表也是同一份）。

**没安装『音乐动效』不影响你的样式**：插件项不靠那个插件的开关，列表里照样可选。频谱数据仍然要有
来源：注册能力 `musicxx.media.spectrum.extract`（自己算）或依赖内核逐帧频谱（播放本地 / 缓存音频时
就有），否则 `musicxx.spectrum.*` 全是 0（看 `uEnv.z` 区分“静音”与“没数据”）。

```jsonc
// 注册项（musicxx.ui.playing.visualizer 的 data）—— 写法与播放页歌曲图完全一致
{
  "title": "我的频谱", "depict": "低频起伏 + 呼吸光晕",
  "shader": {"bundle": "shader/viz.shaderbundle"},
  "args": {
    "uCover": {"kind": "texture", "source": "musicxx.cover", "size": 256},
    "uBass":  {"kind": "smooth", "attackMs": 20, "releaseMs": 260,
                "of": {"kind": "source", "name": "musicxx.spectrum.bands.0"}},
    "uTint":  {"kind": "source", "name": "musicxx.icon.themeMapping.0"}
  },
  "speed": 1, "maxFps": 30, "animate": true
}

// 或者用声明式界面（不写 shader，块里用 musicxx.AnimatedBuilder 逐帧动）
// {"title": "柱状条", "view": "visualizerView"}
// {"title": "不显示", "mode": "none"}
```

规则：

- 三种形态与播放页歌曲图一致：`shader`（有 `shader.bundle`）/ `view`（有 `view`：视图 id / 视图对象 /
  裸块数组）/ `none`（不显示）；不写 `mode` 时按字段推断；
- 渲染区域是**播放页的动效区**（封面下方那一块，`Positioned.fill` 给到的整个可用区域）：
  `uParams.xy` 就是它的像素尺寸。内置动效自己是在底部留 50 的水平边距、120 高的区域里画；
  插件想跟着这个习惯就在界面 / 着色器里自己留边；
- **不接手势**（与内置动效一样）：封面点击、拖动切歌、迷你歌词要继续可用，所以外层一直包着
  `IgnorePointer`。需要交互的功能不要放在这里；
- 纹理与参数（`args` / `speed` / `maxFps` / `resolutionScale` / `animate`）与块 `musicxx.Shader`
  同一套解析（视觉尺寸 / 颜色自己声明）；播放页前景三色只属于背景（`musicxx.ui.playing.foreground.set`
  只认当前生效的背景样式），动效里写不出来；
- 槽位状态广播：`musicxx.state.uiSlots["player.visualizer"]`（字段与背景 / 歌曲图相同）；
- 失效回退：`view` 取不到内容、连续渲染失败 3 次、插件停用 / 卸载 → 自动回退到
  用户原本的内置选择（或“不显示”），原因写在设置列表那一行。

---

## 5.5 普通页面背景槽位（`musicxx.ui.page.background`）：给普通页面铺一层背景

歌单页、设置页、插件自己的页面这类**普通页面**也能由插件铺背景。它和播放页背景一样是
`title` + `view`（一棵块树），区别只有两点：**不需要用户选择**（由 `routes` 决定哪些页面用它），
以及同一页面命中多项时**最新注册的生效**（它没有选择，所以不读 `order`）。

```jsonc
// UI 项 musicxx.ui.page.background（JS 短名 page.background）
{
  "title": "纸张纹理",                       // 必填（与播放页背景一致）
  "depict": "浅色、静态",                    // 可选，调试信息与插件详情用
  "routes": ["/songs", "/setting/theme"],    // 可选；不写或空数组 = 所有普通页面
  "view": {                                  // 必填：块对象 / 块数组 / 视图 id
    "kind": "musicxx.Shader",
    "bundle": "shader/page.shaderbundle",
    "args": {"uCover": {"kind": "texture", "source": "musicxx.cover", "size": 256, "blur": 18}},
    "speed": 1, "maxFps": 8
  }
}
```

- **哪些页面算普通页面**：走主程序页面外壳（`PageBase` / 首页标签页 / 问候页 / 登录页）的页面。
  **弹窗、底部浮层、提示不包含**；**播放页**（`/musicWarp`）、它的全屏视频
  （`/musicWarp/fullScreenVideo`）、模拟时钟 / 弹幕浮窗这类悬浮窗口与启动时的数据初始化页也不包含。
- **路由 id** 就是主程序的路由名：`/`（音乐主页）、`/home/user`、`/search`、`/songs`（歌单页）、
  `/songs/local`、`/song/history`、`/setting`、`/setting/theme`、`/plugins/manager`、
  `/plugins/externPlugin/view`（插件自己的页面）…… 完整清单不在这里列（版本之间会变），
  运行期可以读 `musicxx.state.env` 广播的 `page` 字段拿到当前路由，先打日志确认再写进 `routes`。
- **`routes` 的取舍**：写了这个路由的项优先于“所有页面”的项；同一优先级里**注册顺序最靠后**的生效
  （同名重新注册 = 覆盖，保留原位置；注销后重新注册 = 新注册，排到最后）。只画一项，不叠加。
- **根块就是画布**：直接写一块 `musicxx.Shader`（或 `Stack`）就铺满整页 —— 渲染层发现根块要铺满时
  会给它紧约束。以前要求的“用 `Expanded` 包一层”现在不需要（包了也照样能用，根上的 `Expanded` 会被解开）；
  把着色器放进滚动的 `Column` 里时仍要包 `Expanded`。
- 想不显示就 `musicxx.ui.slot.unregister`；用户要关就停用插件或关闭『拟声++』。
- **没有“当前选中项”**：`musicxx.ui.slot.list/current/set` 不包含这个槽位，也没有设置页。
- 失败回退：视图取不到内容 / 解析失败 / 连续渲染失败 3 次、插件停用 / 卸载 → 这一项被跳过
  （换下一条或回主题背景），原因在管理页「调试」页签里能看到。
- 页面被别的路由盖住 / 进后台时由渲染视图自己停帧（与播放页背景同一套规则）。
- 背景不接手势：外层包着 `IgnorePointer`，块里的 `Button` / `Control` 不会响应。
- 一个页面外壳只应有一层 `WindowBase`：同一路由里嵌套两层时两层都会尝试渲染（现在的页面没有这种结构）。
- 给**歌曲页面**（`/songs`）铺背景时，想画"这张歌单的封面"就用纹理来源 `musicxx.songlist.cover`：
  它只在这个页面内可用，见 §5.6；`plugins/example_js_playing_slots` 的 `pageCover` 是完整例子。

---

## 5.6 歌单封面纹理（`musicxx.songlist.cover`）：只在歌单页面内可用

歌曲页面（路由 `/songs`）打开的是某一张歌单，它的封面（用户设的图，没设时用第一首歌的图）
可以由着色器直接采样。参数与 §5.1 的封面纹理**完全一样**（`size` / `blur` / `square` / `smooth` / `info`），
只是 `source` 换掉：

```jsonc
// 普通页面背景（page.background，§5.5）的 view 里
{
  "kind": "musicxx.Shader",
  "bundle": "shader/page_cover.shaderbundle",
  "args": {
    "uCover": {"kind": "texture", "source": "musicxx.songlist.cover", "size": 768},
    // 主色调：同一张图缩小 + 重度模糊，采样一次近似"这张图的平均色"
    "uTint": {"kind": "texture", "source": "musicxx.songlist.cover",
              "size": 128, "blur": 40, "square": true}
  },
  "animate": false,
  "resolutionScale": 0.6
}
```

- **局部可用**：只有**歌曲页面子树里**才拿得到封面。主程序由歌单页面（`SongsPage`）把打开的歌单
  交给页面背景与页面里的块；别的地方（播放页背景、别的普通页面、插件自己的页面）声明这条来源时
  拿不到封面 —— 绑的是 1×1 透明占位纹理，`u<成员名>Info.w` 是 0，照 §5.1 的写法回退即可；
  这种情况下主程序日志里会有一条"歌单封面纹理只在歌单页面内可用"；
- **封面会变**：换一张歌单、用户改了歌单封面、网络歌单加载完歌曲（封面这时才有）都会重新准备纹理，
  渲染视图在那之后补一帧；封面还没到时主程序会自己重试几次，不用插件轮询；
- **只用于画面**：它不影响 `musicxx.media.cover` / `musicxx.media.palette.*` 那套"当前歌曲封面"
  的能力与取色，两回事；
- 拿主色调不需要额外的取色接口：像上面那样再声明一条同来源的小纹理（`size: 128, blur: 40`），
  模糊到位后采样一次就相当于这张图的平均色。

---

## 6. 着色器写法

顶点（主程序绑定 3 个顶点的全屏三角形，覆盖整个裁剪空间，不做任何变换）：

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

- 结构体必须在**片元着色器**里声明（主程序按片元入口查它的 uniform 槽）；
- `gl_FragCoord.xy` 是像素坐标，除以 `uParams.xy` 得到 `0..1` 的 uv；
- **渲染目标是左上角为原点的像素坐标：`gl_FragCoord.y` 越大越靠下**（与画布一致，主程序不会再翻）。
  要按"画面上方"判断（上下不对称的效果）就自己翻一下 y：
  `vec2 p = (gl_FragCoord.xy - 0.5 * res) / (0.5 * min(res.x, res.y)); p.y = -p.y;`
  —— 光圈示例就是这么做的（正上方 = 最低频，写反了频率轴会上下颠倒）；
- 输出写在 `layout(location = 0) out`；
- 完整效果参考 `plugins/example_js_playing_bg/shader/shaders/bg.frag`（晶格化：Worley 噪声切块 +
  4 色双线性混合 + 夜间压暗 + 轻微暗角）与同目录的 `ring.frag`（光圈：从正上方开始顺时针把
  64 个频带摆成一圈，每个点向内外凸出尖角、相邻点与内外沿之间用直线连成蛛网，不画基线圆，
  振幅直接用主程序给的频带值 + 中心光源；**亮背景（浅色主题 / 亮封面）下线条与光晕自动改用墨色压暗**，
  否则白线会糊在浅色背景里）。

**写法限制（一份源码要过所有后端）**：源码先被 `impellerc` 编成 SPIR-V，运行时再按后端翻译一次
（GLES 后端翻译成 **GLSL ES 1.00**，也就是 `#version 100`）：

- 内置函数**只用浮点版**：GLSL ES 1.00 里 `clamp` / `min` / `max` / `abs` 只有浮点重载，
  对 `int` 用它们会翻译出编不过的代码 —— 表现为选中后报「渲染失败」、连续失败 3 次被停用
  （`clamp(int(index), 0, 15)` 就是踩过的坑，见 `ring.frag` 的 `binRaw`）。
  整型的钳制用 `if`（或三元表达式）自己写；要 `int` 下标时先在浮点上钳制再 `int(...)`；
- **不能用运行时的下标取 uniform**：只允许常量下标或循环下标。要把一帧的 256 个频点
  （`spectrum.bins.*`，见 §7.2）用到圆上时，组号是逐像素算出来的，只能把 64 组逐个比较
  （『光圈』示例的 `binGroup` 就是机械展开的 64 条 `if`）—— 循环里读一整组、逐分量算
  （例如"最低 16 个频点取平均"）仍然可以用循环下标；
- 整型的比较、加减与 `float(i)` / `int(f)` 转换都是安全的；
- 循环上界写常量（`for (int i = 0; i < 5; ++i)`）；
- 拿不准的写法就在本地验证一次：`impellerc --runtime-stage-gles --gles-language-version=100
  --input=<片元> --input-type=frag --spirv=<临时>.spv --sl=<临时>.glsl` 会把后端要编的那份
  GLSL 打出来（整型 `clamp` 这种问题直接在输出里就能看到）。

---

## 7. 参数（`args`）：值表达式

着色器的每个 `vec4` 成员都在 `args` 里声明，**只有一种写法**：`args` 是一个对象，
键是 uniform 成员名（字母或下划线开头，最长 32 字符），值是一条**值表达式**：
`{"kind": "<节点类型>", ...参数}`。表达式可以像 widget 一样互相嵌套，主程序**每帧**从根节点
求值一次，结果直接写进 uniform —— 插件不用在着色器里重复写时间/滤波逻辑。

**名字空间与解析顺序**（与页面/`AnimatedBuilder` 里用的是同一套值系统）：

- **`musicxx.*` 是框架/应用保留的**：派生来源（主题色 / 封面取色 / 频谱 / 环境量）与官方广播频道
  （`musicxx.ui.*` / `musicxx.theme.*` 这类，只能读）；插件不要用这个前缀声明自己的频道；
- **其它名字（允许含 `.`）是自定义的**：`AnimatedBuilder.values` 里的局部通道，或广播频道的当前值
  （应用登记的官方频道、插件自己声明并发布的 `plugin.<插件id>.<名>`）；名字里的 `.` 不再用来判别"系统还是自定义"；
- `source` 取值的顺序：**局部作用域（最近一层 `AnimatedBuilder`）→ 广播频道当前值 → 派生来源**；
  都取不到就用 `fallback` / 零值，并记一条日志；
- 需要绕开顺序时写 `scope`：`"auto"`（缺省，按上面的顺序）、`"local"`（只看局部）、
  `"var"`（跳过局部，只看广播当前值与派生来源，历史名字保留）。

**值的类型**：只有两类 —— `vec4`（数值 / 颜色 / 向量；标量位置取 `.x`）与**文本**；
开关用 `vec4` 的 0/1（非 0 即真）；尺寸位置可用 `unit: "percent"`（或不带 `kind` 的
`{"percent":40}`，两者等价）。

**同一套值系统也用在页面块上**（界面描述层）：尺寸 / 数值字段（`SizedBox` 宽高、`Gap.size`、
`Progress.value`、过渡块的 `value`/`from`/`to`…）、**文本字段**（`Text.text`、KV / Table / Tree 的文本…）
与公共字段 **`visible`** 都能写值表达式；文本字段求值失败时用字面量作为回退，`visible` 求值为假时这一块不渲染。
页面侧逐帧驱动与过渡块见 `plugin-ui.md` §2.3.2。

```jsonc
"args": {
  // 来源：取不到时的回退值写在节点里（fallback / fallbackNight）
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
| `smooth` | `of`(必填)、`ms` 或 `attackMs` + `releaseMs`（缺省 150 / 150） | 一阶过渡：上升用 `attackMs`、回落用 `releaseMs`（"快起慢落"）；文本原样返回 |
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
| `musicxx.spectrum.bands64.0` .. `musicxx.spectrum.bands64.15` | 当前音频的 **64 个频带**（低频在前，0~1；每项 4 个连续频带写在 xyzw）——画面上要摆很多点时用这一档 |
| `musicxx.spectrum.bins.0` .. `musicxx.spectrum.bins.63` | 当前音频的**全部 256 个频点**（低频在前，0~1；每项 4 个连续频点写在 xyzw） |
| `musicxx.env.night` / `musicxx.env.hasPalette` / `musicxx.env.hasSpectrum` / `musicxx.env.hasCover` | 环境量：是否夜间 / 有没有封面配色 / 有没有频谱数据 / 有没有封面图（0 或 1，写在 4 个分量）；配 `mix` 就能写出"按环境切换"的值 |
| **其它任意名字（可含 `.`）** | 广播频道的当前值（`read`）：应用登记的官方广播频道、插件自己声明并发布的 `plugin.<插件id>.<名>`（发布前要 `cache: "latest"` 才有当前值）；值是数字 / 布尔 / 文本，主程序按字段期望的类型转换 |
| **局部通道名（`AnimatedBuilder.values` 的键）** | 优先于上面两类；只在那一层作用域里可见（页面里的用法见 `plugin-ui.md`） |

**频谱来源（`musicxx.spectrum.*`）** 读的是**插件提供的实现**提取的数据（内置『音乐动效』插件是
应用默认登记的那一份，注册了能力 `musicxx.media.spectrum.extract` 的插件可以替代它；每 100 ms 一帧）：

- 有三档粒度（都是一帧 256 个频点再按频率分组，低频在前）：`bands.0..3`（16 个频带）、
  `bands64.0..15`（64 个频带）、`bins.0..63`（256 个频点，等于原始频点）。画面上要摆几十个点时
  用 `bands64.*`（『光圈』示例就是 64 个点对 64 个频带）；只有十几个控制点时用 `bands.*`
  （`example_native` 的晶格背景用它驱动几个亮度/边缘项）；
- **频点按频率分段，不是等宽的**：0 ~ 3840 Hz 每 30 Hz 一段（128 段），3.84 ~ 12 kHz 每段是前一段的
  1.01491 倍（约每倍频程 47 段），12 ~ 20 kHz 每段是前一段的 1.01007 倍（约每倍频程 69 段），低频在前。
  频带（`bands.*` / `bands64.*`）是把频点再按频率分组后的值：低频一列一段、高频一列按等倍频程加宽，
  每列取它覆盖的频点里**最大的那一段**（取平均会把窄的强成分稀释掉）。所以靠高频的一列覆盖的频率
  更宽：16 个频带时第 0 个是 0 ~ 30 Hz、最后一个 13 ~ 20 kHz；64 个频带时前 35 个各 30 Hz
  （0 ~ 1050 Hz）、最后一个 17.9 ~ 20 kHz；
- **别用 `bins.*` 做逐像素查表**：GLES 后端（GLSL ES 1.00）不允许用运行时下标取 uniform，
  按组号取一组的实现只能是"64 个成员逐个比较"（见 §6 的写法限制），每个像素都要走一遍 ——
  实测 1280x720 单帧渲染因此多花 90 ms 以上（同一台机器上晶格背景只要 4 ms 左右），
  播放页会明显卡。需要很多点又要便宜就用 `bands64.*`（16 个成员，实测 64 个点约 30 ms 量级），
  或者用循环下标一次算一整组（例如"最低 16 个频点取平均"）；
- **数据本身的过渡由主程序自动做**：按播放位置在两帧频谱之间插值（就是内置动效"自动插入过渡值"
  的做法），所以 10 帧/秒的数据在几十帧/秒的渲染里也是连续的；只有逐帧数据（结果里的
  `source` 是 `live`）时没有下一帧可用，这时用 `smooth` 节点做过渡；
- 没有数据时**写全 0（静音），不看 `fallback`**：要区分"静音 / 没启用 / 正在加载"就看
  `musicxx.env.hasSpectrum`（或着色器里的 `uEnv.z`）；
- **数值是"画图用"的**：频点与频带与内置『音乐动效』画出来的高度是同一套数值 —— 每一帧按它自己的
  最高频点归一（最高点画到满刻度的九成，比它低 50 dB 的位置落到 0），再把挤在画布中下部的高低差
  拉开（比中线高的往上、低的往下，顶部与底部不动），最后按 gamma 1.2 拉一档；极轻的帧（最高点
  不到满刻度的四分之一）不做归一，静音保持全 0。这是为了让长年处在低位的数值也能占满画面
  （实测密集段落柱间起伏 +44%、逐帧移动 +60%，拉开高低差之后这两项再各涨四成与五成）。代价是它
  **不是绝对声压/dBFS**：要绝对电平做分析用 MCP 工具 `GetAudioSpectrum`（整曲提取的原始电平），
  不要拿着色器里的数值当测量值；
- **别再自己放大**：数值里的高低差已经拉开过，着色器直接用就行。再叠一层"抬增益 + 减底噪
  + 取指数"（例如 `pow(v, 2.6)`）会把大多数频带压到 0，画面只剩几根孤零零的尖角；
- **确实要改对比度时，改完要拉回 0~1**：uniform 里的数值约定是 0~1，自己加的曲线必须把
  结果留在这个区间里（否则画面会溢出或整片发黑）。记 `v` = 一个频带值，`m` = **这一帧**
  的最大频带值（`v / m` 就是这一帧归一后的 0~1）：
  - **放大差异**（更"有棱角"）：**先取指数再除回数值范围** —— 除的是本帧峰值的幂
    `pow(m, k)`，所以最高点仍然回到 1，其余频带被拉开：`float p = pow(v / m, k);`（`k` > 1
    越陡，2 ~ 3 之间比较常用）；
  - **缩小差异**（更"平"）：**先取对数再乘回数值范围** —— 乘的是本帧峰值 `m`，结果仍在
    0~1：`float q = m * log(1.0 + a * v / m) / log(1.0 + a);`（`a` > 0 越大越平）；
  - 两条曲线的参数（`k` / `a`）建议做成插件自己的参数（着色器常量或 `args` 声明），让用户
    能调；示例插件『光圈』现在**直接用数值**（不套任何曲线），要改时照上面两条公式改就行；
- 一帧有 **256 个频点**，频带是这些频点再按频率分组后的 16 个值（与能力的 `GetAudioSpectrum`
  一致，频带 0 最低、频带 15 最高，每组取覆盖范围内最大的那一段）；想要别的规则或整曲数据用
  `musicxx.media.spectrum` 能力自己算；
- 能取到数据的条件：正在播放**本地/缓存**的音频（网络流要先有本地缓存），时长不超过
  15 分钟，且有插件提供频谱提取实现（内置『音乐动效』，或注册了实现
  `musicxx.media.spectrum.extract` 的插件）。

### 7.3 时间与状态

- `tween` / `lfo` 用的是**动画时间**：与着色器里的 `t = uParams.z * uParams.w` 一致，
  插件设置页里的"动画速率"会一起带动它们；`animate: false` 时时间恒为 0（动画停在起点）；
- `smooth` 的状态（上一帧输出）跟着**参数表**走：插件重新声明**同一份参数**时沿用旧状态
  （只是改了 `speed` 这类字段不会打断正在跑的过渡）；声明变了、换 bundle 或换样式时从新声明
  开始 —— 第一次求值直接取目标值，不会从 0 慢慢爬上来；
- 首帧、长时间没出帧（不可见恢复、卡顿）之后的那一步不做过渡（不会"一步跳完"）。

### 7.4 上限与降级

- 一份 `args` 最多 **80 个成员**（频谱的全部 256 个频点就要 64 个成员，再加上颜色、
  响度等常用参数，所以上限比"通用表达式声明"的 16 个宽；见 §7.2 —— 但成员多不代表渲染便宜，
  64 个成员逐像素挑一次会把画面拖卡）；
  单个成员最多 **32 个节点**、**8 层**嵌套；
  `durationMs` / `periodMs` 会钳制在 16 ms ~ 3600000 ms；
- 写错的项**被忽略并记一条日志**（未知 `kind`、缺必填参数、成员名非法、超过上限…），
  不影响同一份声明里的其它成员，也不会让整个样式不可用；
- 一个着色器最多声明 **4 张纹理**（`args` 里 `kind: "texture"` 的成员，见 §5.1）；一张纹理的信息成员
  也算在 80 个成员里；
- 播放页背景**完全不写 `args`** 时只有自动成员（`uParams` / `uEnv`）：不再隐式给 4 个绘制色，
  要颜色自己声明来源（内置背景用的那 4 色是 `musicxx.icon.themeMapping.0..3`）；
- 旧的 `data.colors` 字段与旧的扁平 `args` 写法（数组 + `source` / `value` / `valueNight` /
  `convert` 字段）**都已移除**：还写着的插件不会解析 `args`（当作"没有参数"），
  主程序日志里会给一条迁移提示。

---

## 8. 字段表（块 `musicxx.Shader` 与渲染项）

块 `musicxx.Shader`（两个背景的 `view` 里、插件页面里、以及歌曲图 / 动效的 `shader` 形态都能用）：

| 字段 | 默认 | 范围 | 说明 |
|---|---|---|---|
| `bundle` | 必填 | 插件目录内的相对路径 | 着色器包（§4） |
| `args` | 空 | ≤ 80 个成员 | 参数（成员名 → 值表达式，见 §7）**与纹理声明**（`kind: "texture"`，见 §5.1）写在同一个对象里 |
| `speed` | 1 | 0..20 | 时间推进速度，写进 `uParams.w`（着色器要自己乘，见 §5）。**由插件自己决定**，主程序不做二次缩放 |
| `maxFps` | 16 | ≥ 1（不设上限） | 帧率上限（插件自己定，想要 60 / 120 就写上去） |
| `resolutionScale` | 1.0 | 0.25..1.0 | 降采样后由主程序放大（省 GPU） |
| `animate` | true | — | false = 只渲染一帧（时间恒为 0） |
| `visible` | 由字段推断 | — | 公共块字段：求值为假时不画（可以做显隐动画） |

UI 项（`musicxx.ui.playing.background` / `page.background`）自己是块树的容器，字段只有：

| 字段 | 默认 | 说明 |
|---|---|---|
| `title` | 必填 | 设置列表里的样式名（普通页面背景只用于调试信息与插件详情） |
| `depict` | `""` | 副标题（写 `subtitle` 也可以，且优先） |
| `routes` | 不声明 | **只对普通页面背景**：适用的页面路由 id；不写或空数组 = 所有普通页面 |
| `view` | 必填 | 块对象 / 块数组 / 视图 id（视图由插件同名能力给出） |

渲染目标最长边由主程序限制在 **1280 px**
（超出的部分按比例缩小，再交给 Flutter 放大）；bundle 体积、路径长度与 UI 项数量都不设上限。

字段写错只会退化成默认表现（数值会被 clamp），不会让整项不可用；不可用的原因只来自
`view` 取不到内容 / 解析失败，或者运行期连续渲染失败。

### 8.1 播放页前景（`musicxx.ui.playing.foreground.set`）

主程序播放页的**文本、歌词、进度条与块状背景只用三种颜色**。插件的背景样式被选中之后，可以调能力
一次指定"用遮罩外观 + 这三色"，让自己的背景配上合适的文字颜色。**写在能力参数里，不再挂在 UI 项上**：

```js
// 只有"当前生效的播放页背景插件项"能调（不是它 → not_active；它没被选中 → not_selected）
await musicxx.capability.call("musicxx.ui.playing.foreground.set", {
    style: "mask",              // mask（浅色遮罩，默认）/ neumorphism（常规轻拟物）
    colors: {
        main:  "#ffe8f0ff",     // 普通色：主要文字与图形
        cross: "#80e8f0ff",     // 减淡色：次要内容
        block: "#1f5a7bb8"      // block 背景色：块状背景
    }
});

// 交还主题默认（例如本插件没有取色结果时）
await musicxx.capability.call("musicxx.ui.playing.foreground.set", { clear: true });
```

| 键 | 用在哪里 |
|---|---|
| `main` 普通色 | 歌曲名、高亮歌词行、进度条已播部分、按钮图标、列表里选中的那一首 |
| `cross` 减淡色 | 歌手名、提示文字、总时长、未高亮的歌词行 |
| `block` block 背景色 | 歌曲列表的选中底色、按钮底、信息块与面板、进度条槽底 |

- **只写其中一部分也可以**：没写的色用主程序当前的默认色补齐（白昼、夜间各一套），
  所以"本插件的背景是深色，只要换文字颜色"时只写 `main`；
- **给了 `colors` 就一定用遮罩外观**（`style` 写 `neumorphism` 也一样）；只给 `style` 就只换外观；
- **颜色一个都解析不出来** = 这次不指定（保持现状，不报错）；
- **什么时候清空**：生效项换人（换插件 / 回内置样式）、插件停用 / 卸载 / 关闭『拟声++』时应用自动清空；
  **同一个插件里切歌、切昼夜、视图重建都不清** —— 需要时插件再调一次覆盖；
- 插件怎么知道"现在是我"：`musicxx.state.uiSlots["player.background"]` 的 `itemId` / `plugin`，
  或能力 `musicxx.ui.slot.current`；`foreground.set` 的返回码是最终确认；
- 三色进的是播放页色板，不是全局主题：主题本身的配置不会被插件改掉。

---

## 9. 画一块着色器（块 `musicxx.Shader`）

**插件唯一的着色器入口就是这个块**：两个背景的 `view` 里、插件页面里、以及歌曲图 / 动效的
`shader` 形态都用它（字段见 §8）。背景里它就是整页画布：

```jsonc
// 播放页背景 / 普通页面背景的 view（根块直接写着色器就铺满整页）
{"kind": "musicxx.Shader",
    "bundle": "shader/bg.shaderbundle",
    "args": {
      "uCover":  {"kind": "texture", "source": "musicxx.cover", "size": 512, "blur": 16},
      "uColor1": {"kind": "source", "name": "musicxx.theme.primary"},
      "uColor2": {"kind": "source", "name": "musicxx.icon.main", "convert": true, "fallback": "#8899aa"}
    },
    "speed": 1, "maxFps": 16}
```

页面里的块（`ext://<插件id>/<视图id>` 的视图描述）：**尺寸由父块决定**：

```jsonc
{"kind": "SizedBox", "height": 300, "children": [{
    "kind": "musicxx.Shader",
    "bundle": "shader/ring.shaderbundle",
    "args": { ... }
}]}
```

- **尺寸由父块决定**：不写尺寸就用父块给的空间，所以页面里通常要像上面这样用 `SizedBox`
  （或 `Expanded`、`Row` 里的 `Expanded`）给它确定的高度；父块给不出确定尺寸时这一块**留空**
  并记一条日志（不崩、也不画占位）。背景的**根块就是画布**：直接写一块 `musicxx.Shader`（或 `Stack`）
  就铺满整页，不用再包 `Expanded`（包了也行，根上的 `Expanded` 会被解开）；
- `bundle` 只能是**本插件目录内**的相对路径；
- 背景挂载点让一次失败不至于让整页空掉：那里的着色器连续失败 3 次才停用（页面里的块是 1 次就停用，
  留空 + 日志）；
- 页面不可见（被路由遮挡、进后台）时自动停帧；
- 完整示例：`plugins/example_js_playing_bg/`（六种背景样式都是 `view` 里的这一块）与它的
  `settings` 页（页面内联块 + 一键切换六种背景）。

---

## 10. 运行期：查询、切换与状态广播

槽位用 `slot` 参数区分（省略 = 默认槽位 `player.background`）：`player.background`（播放页背景）、
`player.icon`（播放页歌曲图，§5.3）与 `player.visualizer`（播放页音频动效，§5.4）各有自己的候选列表与选择，
互不影响。**普通页面背景（`page.background`，§5.5）不在这三条能力里**：它没有用户选择，
按页面路由自动生效。

```js
// 有哪些可选样式（含内置项）
const r = await musicxx.ui.slot.list({ slot: "player.background" });
// r.slots[0] = { slot, title, selectedId, items: [
//     { id, title, depict?, source:"builtin"|"plugin", plugin?, available, reason?, selected } ] }

// 一键使用（也可以切到内置样式）
await musicxx.capability.call("musicxx.ui.slot.set", { slot: "player.background", id: "plugin.my_plugin.bg" });
await musicxx.ui.slot.set("musicxx.auto");     // 省略 slot 用默认槽位
// 成功 → { ok:true, slot, id }；不可用 / 未知槽位 → { ok:false, error:"<原因>" }

// 现在是谁在画（同步读状态广播）
const slot = (musicxx.broadcast.read("musicxx.state.uiSlots") || {})["player.background"];
```

- 内置候选 id 是 `musicxx.<内置样式名>`，例如 `musicxx.auto`（内置换算出来的列表见
  `musicxx.ui.slot.list` 的返回）；三个槽位统一这套前缀（歌曲图用短名，例如 `musicxx.circle`）；
- 两个动作 id 不进候选列表：`musicxx.previous` = 交还控制权（恢复 `musicxx.default` 重置掉的那个值，
  没有记录时不动、重复调用安全），`musicxx.default` = 写回该设置项的默认值（背景 `musicxx.auto`、
  动效 `musicxx.none`、歌曲图 `musicxx.circle`），只在确实想重置用户设置时用；
- 命令行 / agent 不用写插件也能切：`musicxx-cli ui-slot list` / `ui-slot current` /
  `ui-slot set plugin.my_plugin.bg`（见 [plugin-agent-cli.md](plugin-agent-cli.md) §5.5）；
- 广播 `musicxx.state.uiSlots` 每个槽位一个条目，字段：

  | 字段 | 含义 |
  |---|---|
  | `itemId` | 现在由哪个**插件项**在画（为空 = 没有插件项在画） |
  | `plugin` | 生效项所属插件 |
  | `selectedId` | 用户选中的是谁（三个槽位的内置项都是 `musicxx.*`） |
  | `visible` | 主程序当前是否在渲染（播放页被遮挡 / 切后台 / 播放页不在页面上 → false） |
  | `night` | 是否夜间表现 |
  | `width` / `height` | 当前渲染的像素尺寸（不可见时为 0） |

  `animate` / `maxFps` 已经不在广播里：它们是**每个着色器块自己的字段**（§8），不同块可以不一样。

- 槽位条目**一直存在**：用户切回内置样式时 `itemId` 为空而 `selectedId` 是内置项 id
  （三个槽位都是 `musicxx.*`），
  插件据此就能说清「内置背景」而不是猜「没有条目意味着什么」；只有插件框架停止（关闭『拟声++』）
  才清空；
- 需要更细的颜色数据用 `musicxx.media.palette`（分析结果 + 主程序 4 色），需要封面像素用
  `musicxx.media.cover`（`size` 16..1024，`format` = `jpeg`/`png`/`rgba`，`data` 是 base64）；
- 播放页前景用 §8.1 的能力改，不要等"应用来问"。

**当前音频频谱**（由插件提供的实现提取：内置『音乐动效』或注册了提取实现的插件）有两条读法，
着色器与 JS 各用一条：

```js
// 1) 着色器参数（每帧现读，零成本）：见 §7 的 musicxx.spectrum.* 来源
// 2) JS 侧读一帧（异步调用）：当前这一帧的响度与频带
const s = await musicxx.media.spectrum({ bandCount: 16, unit: "normalized" });
// s = { ok:true, status:"ready"|"loading"|"none"|"off"|"unavailable",
//       available, loading, reason, source:"extracted"|"live",
//       srcKey, name, artist, durationMs, positionMs,
//       frameHz:10, frameIndex, frames, binCount:256, bandCount, unit,
//       level, bands:[...], frameData? }        // frameData 需要 includeBins:true

// 3) 同步读状态广播（播放中约 10 Hz 推送，适合插件自己的绘制循环）
const live = musicxx.broadcast.read("musicxx.state.spectrum");
// { status, available, loading, reason, source, srcKey,
//   frameHz, frameIndex, frames, bandCount, level, bands:[16] }
```

- 没有数据（没有任何插件提供提取实现 / 正在提取 / 来源不支持）都回 `ok: true`：
  `status` = `off` / `loading` / `none` / `unavailable`，`available` 为假、`bands` 与 `level` 是 0；
- `unit` 可选 `normalized`（0~1，缺省）/ `db`（频点 -80~0 dB，响度 -60~+20 dB）/ `raw`（0~255）；
  想自己分析整曲频谱用能力 `musicxx.media.spectrum.extract`（MCP 工具 `GetAudioSpectrum`）。

---

## 11. 动画速率由插件自己提供

主程序用的时间推进速度就是块的 `speed`（写进 `uParams.w`），不做二次缩放。要允许用户调速率，
就把它做成**插件自己设置页里的一个设置项**（值存插件目录的 `config.json`）：

```js
var BG_BASE_SPEED = 1;                 // 本插件的基准速度（= 1×）
var BG_RATE_OPTIONS = [0.5, 1, 2];     // 可选倍率

function bgView(rate) {
    // 整个背景就是一栖块：不用包 Expanded / blocks（主程序会给紧约束铺满）
    return {
        kind: "musicxx.Shader",
        bundle: "shader/bg.shaderbundle",
        args: { /* 成员名 → 值表达式，见 §7 */ },
        speed: BG_BASE_SPEED * rate,   // 声明给主程序的块速度
        maxFps: 16
    };
}

function bgData(rate) {
    return { title: "流光背景", view: bgView(rate) };
}

musicxx.ui.slot.register({ name: "bg", type: "playing.background", data: bgData(1) });

function applyRate(rate) {
    // 改参数就是改声明：用 register 覆盖同名项，整份 data 重给（位置不变）
    musicxx.ui.slot.register({ name: "bg", type: "playing.background", data: bgData(rate) });
    musicxx.storage.setConfig("bgRate", rate);
}

function refreshBgView() {
    // 声明不变、只想重画：让主程序重新取一次视图（这里是 `data.view` 的视图 id 形态才有意义）
    musicxx.ui.slot.update("bg");
}
```

- 运行期改 `speed` 会推 `musicxx.ui.changed`，主程序刷新候选后**正在使用的背景立即用新速度**
  （不用重新选中：`speed` 每帧现算，着色器里乘上 `uParams.w` 就见效）；
- `maxFps` / `resolutionScale` / `animate` 这类要改帧调度或分辨率创建的字段，运行期改完需要
  **重新选中该项**才完全生效；
- 示例插件 `example_js_playing_bg` 就是这么做的：它的设置页有「背景动画速率」（0.5× / 1× / 2×），
  基准速度定为 1（速率 0.5× / 1× / 2× 就是块的 `speed` 0.5 / 1 / 2）；它注册了六种背景样式，
  改速率时**六项各 register 覆盖一次**（声明整份重给）；
  `example_native` 的 C++ 版声明 `speed: 1`。

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
| 选中后回到内置背景 | 加载或渲染报错被停用（连续失败 3 次才停用，其间保留最后一帧）；看主程序日志与「外部插件 → 调试」里的背景段落 |
| 动画不动 | `animate: false`、`speed: 0`，或着色器没有把 `uParams.z` 乘上 `uParams.w`；播放页被遮挡 / 切后台时本来就不渲染（`visible: false`） |
| `musicxx.spectrum.*` 一直是 0（画面不跟着音乐动） | `uEnv.z` 为 0 = 现在没有频谱数据：没有任何插件提供频谱提取实现（没装『音乐动效』，也没有注册 `musicxx.media.spectrum.extract` 的插件）、还在提取、歌曲不是本地/缓存来源（网络流要先有本地缓存）、或时长超过 15 分钟。要用 `uEnv.z` 判断，别把 0 当成"音乐静音" |
| 封面没画出来（宣告了封面纹理） | 按顺序看：① `uCoverInfo.w` 是不是 0（当前歌曲没有封面 / 封面还没加载好 / 封面源解析失败，主程序日志里有"插件封面纹理准备失败"）；② 着色器里有没有 `uniform sampler2D <args 成员名>;`（名字不一致时主程序会记"没有声明这个纹理 uniform，已跳过绑定"）；③ 采样坐标要对：渲染目标与纹理都是 y 向下，把 `p.y` 翻过的着色器要翻回去；④ 采样到的是 1×1 透明占位纹理时颜色全黑，别用它判断"有没有封面" |
| 用了封面纹理后崩溃 / 画面异常 | 着色器里的 `sampler2D` 名字与 `args` 成员名不一致，或**`args` 里压根没声明这条纹理**：那种 sampler 从未被绑定，采样未绑定的纹理在部分后端上行为未定义（软件后端实测会崩）。名字必须两边一致；当前没有封面时主程序会绑 1×1 透明占位纹理，所以"声明了但没封面"是安全的 |
| 图片纹理没画出来 | ① 插件有没有先 `musicxx.media.bindImage`（或 `cover` 的 `bind`）——返回里看 `ok` 与 `bindError`；② `args` 里的 `source` 是不是 `musicxx.image.<绑定的 key>`；③ `args` 成员名与着色器里的 `sampler2D` 名是否一致；④ `uXxxInfo.w` 是不是 0（还没绑定 / 已解绑） |
| 绑定图片失败 | 键不合法（1..64 位字母数字与 `_ - .`）、`format: "rgba"` 缺 `width` / `height` 或字节数与尺寸不匹配 —— 原因都在调用返回的 `bindError` / `error` 里（张数与尺寸已不再限制） |
| 封面上下颠倒 | 纹理坐标要把"向上为正"的坐标翻回去（`vec2(x, -y) * 0.5 + 0.5`），见 §5.1 |
| 画面比预期快/慢 | `uParams.z` 是真实秒数、`uParams.w` 是插件声明的速度：主程序的基准速度就是插件给的值（示例把 1 当 1×） |