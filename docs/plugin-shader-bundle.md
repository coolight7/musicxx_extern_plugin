# 插件渲染：shader bundle 打包与 uniform 约定

插件可以把**打包期编译好的 shader bundle** 注册成一种宿主渲染样式（当前有两个渲染槽位：
「播放页背景」`player.background` 与「播放页歌曲图」`player.icon`，后者见 §5.3），由用户在
「设置 → 主题」里选中后生效（命令行 / agent 用
`musicxx-cli render list|select` 切，见 [plugin-agent-cli.md](plugin-agent-cli.md) §5.5）；
同一个 bundle 也能画在插件自己的页面里（`Shader` 块）。

插件不写 Dart/Flutter 代码，只交出 bundle + 一份参数声明。相关文档：界面与页面块类型见
[plugin-ui.md](plugin-ui.md)，JS 与 C++ 的写法见 [plugin-js-api.md](plugin-js-api.md) /
[plugin-native-api.md](plugin-native-api.md)，整体流程与注意事项见 [plugin-guide.md](plugin-guide.md)。

> 现成可编译的例子：`plugins/example_js_shader/shader/` 与 `plugins/example_native/shader/`
> —— 「晶格化」背景（随机点最近邻切块，配色用宿主的 4 个绘制色）。`example_js_shader` 里还有
> 第二个 bundle：**「光圈」背景**（`shaders/ring.frag`：从正上方开始顺时针排 64 个点、一个点一个
> 频带，尖角向内外凸出、相邻点之间用直线连成一张"蛛网"，不画基线圆），照抄同一份打包脚本即可编译。
> 基线版本 3.47.5 的 Flutter SDK
> 编译出来的 bundle 可以直接用（`format_version = 2`）。
> 另一个现成的例子是 `plugins/playing_bg_image/shader/`（插件名「示例封面背景」，**两种背景模式
> 各自一个 bundle**）：**「模糊热浪」**（`shaders/heat.frag`：封面预模糊后放大铺满屏幕、缓慢四处
> 漂移、局部偶尔轻微扭曲，模糊程度 0~30 与放大倍数 1~3 由插件的设置页决定）与**「渐变贴边」**
> （`shaders/card.frag`：封面主色做渐变底、清晰封面贴着屏幕边缘铺一片（图片占比可调，越大图片越多）、自由边模糊渐隐，
> 再按主题叠一层亮暗遮罩；同一个 `player.background` 槽位里注册两项，用户选哪一项就是哪种模式）。
> 它同时演示了歌曲图槽位（§5.3）与封面取色钩子 —— 见 `plugins/playing_bg_image/plugin.js`。

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
（`plugins/playing_bg_image/` 是另一种布局：两种背景模式的两个 bundle 共用一份顶点着色器
`shaders/full.vert`，描述文件是 `bundle.json` → `heat.shaderbundle` 与
`bundle_card.json` → `card.shaderbundle`，同一个 `build_bundle.ps1` 逐个编译。）

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

## 5. uniform 约定

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
  vec4 uCoverInfo; // 只在声明了 cover 时由宿主自动写：见 §5.1
} render_info;
```

规则：

- **结构体名与成员名必须完全一致**（宿主按名字寻址写入）；成员偏移必须 16 字节对齐；
- 结构体大小必须是 **16 的倍数且 ≥ 16**，否则该样式判为不可用并给出原因；
- `uParams` / `uEnv` 由宿主自动写（不用在 `args` 里声明）；声明了 `cover` 时，封面信息成员
  （缺省名 `uCoverInfo`，见 §5.1）同样由宿主自动写，也不要在 `args` 里声明；
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

## 5.1 封面纹理（`cover`）：把当前歌曲封面交给着色器采样

播放页背景经常要用当前歌曲封面（模糊铺底、圆心盘、扭曲、像素化…）。**不要把封面字节读进插件再想办法画**
—— 动作 `musicxx.media.cover` 那套是给"读数据"用的（插件脚本拿到的是 base64 字节，进不了 GPU）。
渲染项里声明 `cover`，剩下的事由宿主做：解码 → 缩放（可选中心裁剪成正方形）→ 可选**预模糊**
（引擎的图像滤镜，GPU 一次）→ 上传成 GPU 纹理 → 每帧绑定到你指定的 `sampler2D`。

```jsonc
// 渲染项（musicxx.ui.playing.background 的 data）
{
  "shader": {"bundle": "shader/bg.shaderbundle"},
  "cover": {"texture": "uCover", "info": "uCoverInfo", "size": 512, "blur": 0, "square": false}
}
```

```glsl
uniform sampler2D uCover;            // 名字与 cover.texture 一致（缺省 uCover）

uniform MusicxxRenderInfo {
  vec4 uCoverInfo;                   // 名字与 cover.info 一致（缺省 uCoverInfo；不用在 args 里声明）
} render_info;

void main() {
  vec2 uv = gl_FragCoord.xy / max(render_info.uParams.xy, vec2(1.0));
  // 没有封面时 uCoverInfo.w 是 0：这时不要用采样结果（宿主绑的是 1×1 透明占位纹理）
  float coverOn = render_info.uCoverInfo.w > 0.5 ? 1.0 : 0.0;
  vec3 coverColor = texture(uCover, uv).rgb;
  ...
}
```

| 字段 | 默认 | 范围 | 说明 |
|---|---|---|---|
| `texture` | `uCover` | GLSL 标识符 | 着色器里的 `sampler2D` 名 |
| `info` | `uCoverInfo` | GLSL 标识符 | 封面信息写到哪个结构体成员（与 `texture` 同名时退回缺省名） |
| `size` | 512 | 128..1024 | 纹理最长边（px）：等比缩放、**不放大** |
| `blur` | 0 | 0..64 | 预模糊强度（sigma，px）；0 = 原图。**注意这是"纹理上的" sigma**：纹理会铺满屏幕（还可能被插件再放大），屏幕上看到的模糊 ≈ `blur × 纹理→屏幕的放大倍数` —— 512 的纹理铺在 1280 宽的窗口里放大 5 倍左右，`blur: 34` 会糊成一片渐变，`blur: 10~16` 才看得出封面内容 |
| `square` | false | — | true = 中心裁剪成正方形（`size` × `size`），false = 保持原图比例 |
| `smooth` | true | — | 采样过滤：true = 线性（缩放平滑），false = 最近邻（像素风） |

`cover` 也可以写成 `true`（全用默认值）、一个名字字符串（只改纹理名），或者**数组**（多张纹理，
例如"原图 + 预模糊"各一张，每张用自己的 `texture` / `info` 名字）。

`uCoverInfo = (纹理宽, 纹理高, 原图宽高比, 是否有封面)`：

- 纹理是**等比缩放**的（`square: false` 时宽高比就是原图），拿 `uCoverInfo.xy` 可以算自己的 uv
  —— 例如"铺满画面、超出裁掉"（cover 裁切）的写法见
  `plugins/example_js_shader/shader/shaders/bg.frag`；
- `uCoverInfo.w = 0` 表示这一帧**没有封面**（还没加载好 / 这首歌没有封面）：宿主仍然会绑定纹理，
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
  `plugins/playing_bg_image/shader/shaders/heat.frag`（铺满全屏）与同目录的 `card.frag`
  （只在卡片里铺满、还要居中裁剪）以及 `plugins/example_js_shader/shader/shaders/bg.frag`；
- 想让画面只显示封面的一部分（"放大"背景），**用一个自己声明的 `args` 成员传倍率**，
  在着色器里按"总体放大倍率"算采样步长：`coverScale = max(fillScale, uZoom.x)`、
  `uvStep = res / (texSize * coverScale)`（`fillScale = max(res.x/texSize.x, res.y/texSize.y)`
  是铺满所需的最小倍率）—— `plugins/playing_bg_image` 就是这么做的（设置页可切 1× / 1.5× / 2× /
  2.5× / 3×，真机自检里有"倍率越大画面越局部"的回归用例；`card.frag` 是同一个套路的第二种用法：
  卡片尺寸固定、只把纹理在卡片里放大，居中取景）；
- 没声明 `cover` 时**零成本**：不解码、不上传、不占显存（一张 512×512 的纹理约 1 MiB）；
  连参数里都没问 `musicxx.env.hasCover` 时，渲染每帧也不会去读"现在有没有封面"；
- **不保留解码结果**：纹理只活在"正在用它的那个渲染视图"里 —— 视图销毁、用户切回内置样式、
  插件停用或卸载、关闭『拟声++』时都会立即释放（丢引用并释放包装用的图像）；没有跨视图缓存，
  也不会留一份"以后可能用到"的解码图；解码用的（独立命名空间的）图片缓存按
  （provider + 解码尺寸）键控、**不在每次准备前清空**（每清一次就会让切歌时反复解码、
  网络封面还要重新下载，表现为"切歌卡几秒"）；纹理准备还有 120ms 防抖（切歌时封面源
  常连着变几次）；
- 封面变化（换歌、同一首歌换封面）时宿主重新准备一次（几毫秒），**不是每帧上传**；模糊也是一次性
  预生成，不是每帧做 —— 想在着色器里自己模糊当然也行（多采样几次，代价自己算）；
- 同一张封面可以被多个渲染项各自声明（背景与页面里的 `Shader` 块各拿一份纹理，各自释放，互不影响）；
- 封面可能带透明（PNG）：采样到的 `a` 就是原图的 alpha，透明处 `rgb` 是 0，需要不透明底就自己 `mix`；
- **着色器里声明了 `sampler2D`，就一定要在渲染项里有同名的 `cover` 声明**：宿主只绑定
  `cover.texture` 里写出的名字；只声明了 `cover` 而着色器里没有对应 uniform 时宿主会记一条日志并
  跳过（安全），反过来（着色器有 sampler、渲染项没声明）就是**采样一张从未绑定的纹理**，
  在部分后端上行为未定义（软件后端实测会崩）。声明了 `cover` 而当前没有封面时宿主绑的是
  1×1 透明占位纹理，采样是安全的 —— 用 `uCoverInfo.w` 判断要不要用它；
- 声明了名字但着色器里没有对应的 `uniform sampler2D` 时，宿主记一条日志并跳过这一张，
  不影响这一帧的其它内容（不会让整个样式失效）；
- 页面里的 `Shader` 块（§9）目前还没有 `cover` 字段（描述层字段表里没有它），要用封面纹理
  先做成背景样式；`musicxx.state.renderSlots` / `musicxx.media.palette` 之类的状态读法与以前一样。

---

## 5.2 插件绑定的图片（`image`）：把插件自己的图交给着色器

`cover` 只能画"当前歌曲封面"。要让着色器画**插件自己准备的图**（自己下载的素材、脚本合成的图片、
处理过的封面），就先把图片交给宿主绑定，再在渲染项里引用它：

```js
// 1) 把图片交给宿主（宿主解码 + 上传成 GPU 纹理，登记进这个插件的图像表）
await musicxx.call("musicxx.media.bindImage", {
    key: "mini",            // 本地名字，渲染项里用它引用
    data: base64,           // 图片字节（png / jpeg / rgba）
    format: "png",          // png / jpeg / rgba（rgba 还要 width / height）
});

// 或者"读封面顺便绑"：复用同一次解码的像素，不重复解码
await musicxx.call("musicxx.media.cover", { size: 96, format: "rgba", bind: "mini" });

// 不再需要时释放（不释放也会随插件停用/卸载、宿主停止自动摘掉）
await musicxx.call("musicxx.media.unbindImage", { key: "mini" });
```

```jsonc
// 2) 渲染项里引用它
{
  "shader": {"bundle": "shader/bg.shaderbundle"},
  "image": {"key": "mini", "texture": "uMini", "info": "uMiniInfo"}
}
```

```glsl
uniform sampler2D uMini;                 // 名字 = image.texture（必填）

uniform MusicxxRenderInfo {
  vec4 uMiniInfo;                        // 名字 = image.info（缺省 <texture>Info）
} render_info;

void main() {
  vec2 uv = gl_FragCoord.xy / max(render_info.uParams.xy, vec2(1.0));
  // 插件还没绑定 / 已解绑时这一位是 0（宿主绑的是 1×1 透明占位纹理）
  float hasImage = render_info.uMiniInfo.w > 0.5 ? 1.0 : 0.0;
  vec3 color = texture(uMini, uv).rgb;
  ...
}
```

| 字段 | 默认 | 说明 |
|---|---|---|
| `key` | 必填 | 插件图像表里的键（与 `musicxx.media.bindImage` 的 `key` 相同；1..64 位字母数字与 `_ - .`） |
| `texture` | 必填 | 着色器里的 `sampler2D` 名（不合法就忽略这一条并记日志） |
| `info` | `<texture>Info` | 信息成员名：`(纹理宽, 纹理高, 宽高比, 是否有内容)`；写成 `""` 或与 `texture` 同名时不写信息成员 |

约定与代价：

- **图片由插件提供，宿主只做解码与上传**（不检查内容）：单张最长边 1024（超出拒绝）、
  每个插件最多同时绑定 8 张、base64 字符串上限 6 MiB；
- **绑定是临时资源**：插件卸载 / 停用、宿主停止、关闭『拟声++』时整张表被摘掉（只丢引用，
  交给垃圾回收）；渲染视图每帧只做一次整数比较，没绑定就是零成本；
- **没声明 `image` 的渲染项**：不查表、不绑定、不占显存；声明了但还没绑定时绑 1×1 透明占位纹理；
- 插件用同一个 key 重新绑定（换成新图）时，正在画的背景会在下一帧用上新图；
- 采样坐标与 `cover` 完全一致（渲染目标与纹理都是 y 向下；把 `p.y` 翻过（向上为正）的着色器
  要翻回去，见 `ring.frag` 的圆心封面）；
- 与 `cover` 一样，**着色器里声明了 `sampler2D` 就必须在渲染项里有同名的 `image`（或 `cover`）声明**
  —— 采样一张从未绑定的纹理在部分后端上行为未定义。

---

## 5.3 播放页歌曲图槽位（`musicxx.ui.playing.icon`）：接管中间那张歌曲图

除了背景，插件还能接管沉浸式播放页**中间那张歌曲图**（内置显示是"封面图 + 播放状态按钮 / 视频"）。
它是一个独立的渲染槽位 `player.icon`：用户在「设置 → 主题 → 播放页歌曲图」里选择，或插件用
`musicxx.render.select(id, {slot: "player.icon"})` 切换（内置项 id 是 `builtin:default`）。

```jsonc
// 渲染项（musicxx.ui.playing.icon 的 data）—— 三种形态挑一种
{
  // 1) shader：插件着色器绘制（渲染输入与播放页背景**完全一致**）
  "title": "我的歌曲图", "mode": "shader",
  "shader": {"bundle": "shader/icon.shaderbundle"},
  "cover": {"texture": "uCover", "info": "uCoverInfo", "size": 512},
  "args": { "uTint": {"kind": "source", "name": "musicxx.icon.themeMapping.0"} },
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
| `cover` / `image` / `args` / `speed` / `maxFps` / `resolutionScale` / `animate` | 同背景 | 渲染输入与播放页背景**共用同一份解析与渲染宿主**，写法和含义完全一样 |

约定与代价：

- **接管是"整个显示"**：原来的"点击歌曲图播放 / 暂停"、滑动切歌（`musicxx.ui` 设置里的
  「滑动切换歌曲」）、视频播放按钮都不再出现。需要这些交互的插件自己在界面里放按钮 ——
  `view` 形态可以声明动作（`dispatch` 调自己的能力、`command` 调官方动作如
  `musicxx.player.toggle`）；
- `shader` 形态的渲染区域就是**内置歌曲图占的那块**：`uParams.xy` 是它的像素尺寸（不是整屏），
  插件按这个尺寸画；`animate: false` 时只出一帧；
- 没有额外 uniform 成员：通用成员（`uParams` / `uEnv`）够描述"画在多大的一块里"，
  颜色 / 环境量用 `args` 里的来源按需声明（与背景一致）；
- 槽位状态镜像：`musicxx.state.renderSlots["player.icon"]`（`itemId` = 现在谁在画、
  `visible` / `width` / `height` 由挂载点上报）。插件可以据此知道"是不是自己在画"，
  例如"背景是自己时才接管歌曲图"这种跟随（`plugins/playing_bg_image` 就是这么做的：它的两种
  背景模式共用同一个歌曲图接管项，靠 `itemId` 判断背景是不是自己）；
- 失效回退：bundle 预检不过、`view` 取不到内容、连续渲染失败、插件停用 / 卸载 / 关闭『拟声++』时
  自动回退内置显示，原因写在设置列表的那一行。

完整示例：`plugins/playing_bg_image/`（两种背景模式 + 歌曲图接管 + 封面取色三件事一起做的插件）。

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
  4 色双线性混合 + 夜间压暗 + 轻微暗角）与同目录的 `ring.frag`（光圈：从正上方开始顺时针把
  64 个频带摆成一圈，每个点向内外凸出尖角、相邻点与内外沿之间用直线连成蛛网，不画基线圆，
  振幅取过指数做对比 + 中心光源；**亮背景（浅色主题 / 亮封面）下线条与光晕自动改用墨色压暗**，
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
| **其它任意名字（可含 `.`）** | 变量目录里的键：应用登记的官方变量、插件登记的自定义变量（如 `plugin.<插件id>.<名>`）；值是数字 / 布尔 / 文本，宿主按字段期望的类型转换 |
| **局部通道名（`AnimatedBuilder.values` 的键）** | 优先于上面两类；只在那一层作用域里可见（页面里的用法见 `plugin-ui.md`） |

**频谱来源（`musicxx.spectrum.*`）** 读的是内置『音乐动效』插件提取的数据（每 100 ms 一帧）：

- 有三档粒度（都是一帧 256 个频点按线性分组取平均，低频在前）：`bands.0..3`（16 个频带）、
  `bands64.0..15`（64 个频带）、`bins.0..63`（256 个频点，等于原始频点）。画面上要摆几十个点时
  用 `bands64.*`（『光圈』示例就是 64 个点对 64 个频带）；只有十几个控制点时用 `bands.*`
  （`example_native` 的晶格背景用它驱动几个亮度/边缘项）；
- **别用 `bins.*` 做逐像素查表**：GLES 后端（GLSL ES 1.00）不允许用运行时下标取 uniform，
  按组号取一组的实现只能是"64 个成员逐个比较"（见 §6 的写法限制），每个像素都要走一遍 ——
  实测 1280x720 单帧渲染因此多花 90 ms 以上（同一台机器上晶格背景只要 4 ms 左右），
  播放页会明显卡。需要很多点又要便宜就用 `bands64.*`（16 个成员，实测 64 个点约 30 ms 量级），
  或者用循环下标一次算一整组（例如"最低 16 个频点取平均"）；
- **数据本身的过渡由宿主自动做**：按播放位置在两帧频谱之间插值（就是内置动效"自动插入过渡值"
  的做法），所以 10 帧/秒的数据在几十帧/秒的渲染里也是连续的；只有逐帧数据（结果里的
  `source` 是 `live`）时没有下一帧可用，这时用 `smooth` 节点做过渡；
- 没有数据时**写全 0（静音），不看 `fallback`**：要区分"静音 / 没启用 / 正在加载"就看
  `musicxx.env.hasSpectrum`（或着色器里的 `uEnv.z`）；
- 一帧有 **256 个频点**，频带是这些频点按线性分组取平均后的 16 个值（与能力的 `GetAudioSpectrum`
  一致，频带 0 最低、频带 15 最高）；想要别的规则或整曲数据用 `musicxx.media.spectrum` 动作自己算；
- 能取到数据的条件：正在播放**本地/缓存**的音频（网络流要先有本地缓存），时长不超过
  15 分钟，且内置『音乐动效』插件处于启用状态。

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
- 播放页背景**完全不写 `args`** 时，宿主默认给 `uColor1..4 ← musicxx.icon.themeMapping.0..3`
  （观感与内置背景一致）；页面里的 `Shader` 块不做这个回退（不写就没有颜色）；
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
| `args` | 空 | ≤ 80 个成员 | 着色器参数：成员名 → 值表达式（见 §7）；背景槽位不声明时默认给内置 4 色 |
| `cover` | 不声明 | 对象 / `true` / 名字 / 数组 | 封面纹理（`texture` / `info` / `size` / `blur` / `square` / `smooth`，见 §5.1）；不声明 = 零成本 |
| `image` | 不声明 | 对象 / 数组 | 插件绑定的图片纹理（`key` / `texture` / `info` / `smooth`，见 §5.2）；不声明 = 零成本 |
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

槽位用 `slot` 参数区分（省略 = 默认槽位 `player.background`）：`player.background`（播放页背景）
与 `player.icon`（播放页歌曲图，§5.3）各有自己的候选列表与选择，互不影响。

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
- 命令行 / agent 不用写插件也能切：`musicxx-cli render list` / `render current` /
  `render select plugin.my_plugin.bg`（见 [plugin-agent-cli.md](plugin-agent-cli.md) §5.5）；
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
| 封面没画出来（`cover` 声明了） | 按顺序看：① `uCoverInfo.w` 是不是 0（当前歌曲没有封面 / 封面还没加载好 / 封面源解析失败，宿主日志里有"插件封面纹理准备失败"）；② 着色器里有没有 `uniform sampler2D <cover.texture>;`（名字不一致时宿主会记"没有声明这个纹理 uniform，已跳过绑定"）；③ 采样坐标要对：渲染目标与纹理都是 y 向下，把 `p.y` 翻过的着色器要翻回去；④ 采样到的是 1×1 透明占位纹理时颜色全黑，别用它判断"有没有封面" |
| 用了封面纹理后崩溃 / 画面异常 | 着色器里的 `sampler2D` 名字与渲染项 `cover.texture` 不一致，或**渲染项压根没声明 `cover`**：那种 sampler 从未被绑定，采样未绑定的纹理在部分后端上行为未定义（软件后端实测会崩）。名字必须两边一致；当前没有封面时宿主会绑 1×1 透明占位纹理，所以"声明了但没封面"是安全的 |
| `image` 纹理没画出来 | ① 插件有没有先 `musicxx.media.bindImage`（或 `cover` 的 `bind`）——返回里看 `ok` 与 `bindError`；② 渲染项 `image.key` 与绑定的 `key` 是否一致；③ `image.texture` 与着色器里的 `sampler2D` 名是否一致；④ `uXxxInfo.w` 是不是 0（还没绑定 / 已解绑） |
| 绑定图片失败 | 键不合法（1..64 位字母数字与 `_ - .`）、base64 超过 6 MiB、单张超过 1024 像素、同一插件已绑满 8 张、`format: "rgba"` 缺 `width` / `height` 或字节数与尺寸不匹配 —— 原因都在动作返回的 `bindError` / `error` 里 |
| 封面上下颠倒 | 纹理坐标要把"向上为正"的坐标翻回去（`vec2(x, -y) * 0.5 + 0.5`），见 §5.1 |
| 画面比预期快/慢 | `uParams.z` 是真实秒数、`uParams.w` 是插件声明的速度：宿主的基准速度就是插件给的值（示例把 1 当 1×） |
