# example_js_playing_bg —— 播放页背景示例插件

一个示例只教一件事：**怎样往播放页背景槽位（`player.background`）里放自己的样式**。
六种样式做在一个插件里，正好是两类做法：

| 类别 | 样式 | 画面来自 | bundle |
|---|---|---|---|
| 封面类 | 『模糊』 | 当前歌曲封面（预模糊后放大铺满、缓慢漂移，可加轻微扭曲） | `shader/heat.shaderbundle` |
| 封面类 | 『沉浸』 | 封面主色渐变底 + 贴边铺开的清晰封面（自由边模糊） | `shader/card.shaderbundle` |
| 封面类 | 『黑胶』 | 封面主色铺底 + 黑胶唱片与唱臂，盘心是旋转的封面 | `shader/vinyl.shaderbundle` |
| 图案类 | 『晶格』 | 只有着色器画（Worley 随机点切块 + 4 个绘制色混合） | `shader/bg.shaderbundle` |
| 图案类 | 『光圈』 | 只有着色器画（64 个频带尖角连成一圈，跟随频谱律动） | `shader/ring.shaderbundle` |
| 图案类 | 『插件纹理铺底』 | 同『晶格』的着色器，底层纹理换成插件自己绑定的图 | `shader/bg.shaderbundle`（共用） |

另有**歌曲图内容**三种（槽位 `player.icon.image`：只换歌曲图里那张图，手势 / 形状裁剪 /
描边 / 加载提示仍由应用画）：

| 名称 | 画面 | bundle |
|---|---|---|
| 『热浪』 | 当前歌曲图 + 向上升起的热气流扰动与轻微色散 | `shader/icon_heat.shaderbundle` |
| 『油画』 | 四个方向取色彩最平滑的一块（Kuwahara 式）+ 提彩色 + 压色阶 | `shader/icon_oil.shaderbundle` |
| 『像素』 | 按短边切成小方块、最近邻取样 + 压色阶 | `shader/icon_pixel.shaderbundle` |

顺带演示同一套代码里的另外四件事：

- **播放页歌曲图槽位**（`player.icon`）：背景是自己时才接管歌曲图，别的样式在画就交还
  `musicxx.previous`（每秒读一次 `musicxx.state.uiSlots`）；
- **播放页歌曲图内容槽位**（`player.icon.image`）：只换歌曲图里那张图，三种效果——
  『热浪』（热气流扰动 + 色散）、『油画』（Kuwahara 式色块 + 压色阶）、『像素』（小方块 + 压色阶）；
  手势、形状裁剪、描边与加载提示仍是应用画；
- **封面取色能力**（`musicxx.media.palette` 的 `implement`）：命中缓存同步返回，未命中返回 null 并
  在后台算好、用 `musicxx.media.palette.set` 写回（**不要在处理器里 await 应用实现的能力**）；
- **同一份描述的复用**：说明页 / 设置页 / 频谱页都是声明式视图，用随插件分发的界面 kit 拼装。

---

## 1. 装与跑

```powershell
# 在插件包目录里构建（产物在 .native/output/<平台>-<架构>-<配置>/plugins/）
pwsh -NoProfile -File tools/build_examples.ps1

# 装进应用（也可以用管理页「从压缩包安装」）
musicxx-cli plugin install "<绝对路径>/example_js_playing_bg-2026_1121_1.zip"

# 自检：六种样式、三种歌曲图内容、四个脚本都执行过、页面函数都在
musicxx-cli call probe --plugin example_js_playing_bg
```

装好后：**设置 → 主题 → 播放页面背景**里会多出六项（标题都以『示例播放页背景 · 』开头），
选一项就生效；插件自己的说明页从**音乐主页 → 示例播放页背景**进入。

每个样式项的 `data` 只有三个字段，背景是一棵块树：

```js
{
    title: "示例播放页背景 · 模糊",
    depict: "封面高斯模糊后放大铺满屏幕…",
    view: {
        kind: "musicxx.Shader",               // 根块直接写着色器就铺满整页（不用包 Expanded）
        bundle: "shader/heat.shaderbundle",
        args: {
            uCover: { kind: "texture", source: "musicxx.cover", size: 512, blur: 14 },
            uZoom: { kind: "const", value: 2 },
        },
        speed: 1, maxFps: 24, resolutionScale: 0.6,
    },
}
```

播放页前景（三色 + 是否用遮罩外观）**不在项里**：样式被选中后由 `palette.js` 调
`musicxx.ui.playing.foreground.set`（应用只接受当前生效的播放页背景插件项）。
`palette.js` 靠广播驱动（订阅 `musicxx.state.uiSlots` / `musicxx.theme.current` /
`musicxx.media.cover.changed`），不轮询。

## 2. 文件怎么分工

六个脚本在同一个 JS 上下文里按 `plugin.yaml` 的 `scripts` 顺序执行，
所以**顶层 `const` / `let` / `function` 名字必须全局唯一**，顶层注册与定时器统一放在最后一个文件：

| 文件 | 只做什么 |
|---|---|
| `pluginxx_ui_kit.js` / `musicxx_ui_kit.js` | 界面 kit（`pwsh tools/sync_ui_kit.ps1` 复制，不要手改） |
| `backgrounds.js` | 六个样式的声明数据（`title` / `depict` / `view` 块树，块里的 `args` 带封面纹理与块速度）、样式与 UI 项 id 的对照表、注册函数 |
| `icons.js` | 歌曲图内容的三种效果（`iconHeat` / `iconOil` / `iconPixel`）的声明与注册函数、设置页用的状态文字 |
| `palette.js` | 封面取色：六种样式各自的颜色估算、前景色挑选、base64 与图片工具；算好的三色用能力 `musicxx.ui.playing.foreground.set` 交给应用（算不出来就 `clear`） |
| `views.js` | 说明页 / 设置页 / 频谱页的视图（含「歌曲图内容」那一栏），以及频谱动作的异步状态 |
| `plugin.js` | 配置读写、封面纹理绑定、槽位跟随、能力入口、全部顶层注册 |

`shader/` 下八份片段着色器共用一份全屏顶点着色器 `shaders/full.vert`，描述文件是 `bundle_<名字>.json`，
`build_bundle.ps1` 逐个编成 `.shaderbundle`（产物随插件分发）。改了着色器就重跑：

```powershell
pwsh -NoProfile -File plugins/example_js_playing_bg/shader/build_bundle.ps1
```

歌曲图内容的三份 bundle（`icon_heat` / `icon_oil` / `icon_pixel`）由 `icons.js` 声明引用，
纹理都是 `musicxx.cover`（当前歌曲图，方形裁剪）；没有封面时它们输出透明像素，
应用画的默认图会透出来。

## 3. 设置项（存插件目录的 `config.json`）

| 键 | 含义 | 取值 | 默认 |
|---|---|---|---|
| `blur` | 『模糊』的模糊程度 | 0 ~ 30 | 14 |
| `zoom` | 『模糊』的放大倍数 | 1 ~ 3 | 2 |
| `share` | 『沉浸』里图片占比 | 0.3 ~ 1 | 0.75 |
| `spin` | 『黑胶』转速 | 0.25 ~ 4（档位 0.5× / 1× / 2×） | 1 |
| `bgRate` | 图案三种的动画速率 | 0.5 / 1 / 2 | 1 |
| `ripple` | 『模糊』的热浪扭曲 | 布尔 | `true` |
| `hideIcon` | 用背景时隐藏歌曲图 | 布尔 | `false` |
| `autoForeground` | 按封面自动调整播放页前景色 | 布尔 | `true` |

- 改完的值要重新声明受影响的样式项（用 `musicxx.ui.slot.register` 同名覆盖，声明整份重给，六个各一次）；
  只改 `speed` 这类每帧现算的字段，改动立即生效，不用重新选中。
- 值调到边界外会被夹到范围内，读到非法值用默认值；异步读回来的旧值不会盖掉用户刚做的选择。
- `1×` 就是 `speed` 1（基准速度 1），没有"声明 4 当基准"那种偏移。
- 「歌曲图内容」**不是**配置项：设置页上那一栏直接改主程序的渲染槽位选择
  （`musicxx.ui.slot.set`，槽位 `player.icon.image`），卸载插件后自动回到原图。

## 4. 用到的能力与槽位

| 能力 | 作用 |
|---|---|
| `probe` | 自检：样式数 / UI 项数 / 三个业务脚本是否执行过 |
| `card` / `settings` / `spectrumProbe` | 说明页 / 设置页 / 频谱页（页面路由是 `ext://<插件 id>/<视图>`） |
| `useBackground` | 从插件页面里切某一种样式（内部调 `musicxx.ui.slot.set`） |
| `cycleBackgroundRate` | 切动画速率（设置页那一行按钮） |
| `setOption` | 改上面任意一项数值 / 布尔设置 |
| `bindCoverTexture` / `unbindCoverTexture` | 把当前封面绑成『插件纹理铺底』的底层纹理 |
| `toggleCoverHook` | 开关封面取色钩子 |
| `takeIcon` / `releaseIcon` | 接管 / 交还播放页歌曲图 |
| `clearPaletteCache` | 清掉插件自己缓存的取色结果 |

- 样式项类型：`musicxx.ui.playing.background`（六项）、歌曲图：`musicxx.ui.playing.icon`、
  歌曲图内容：`musicxx.ui.playing.icon.image`（三项，只换图片）、主页入口：`home.entry`；
- 槽位 id：`player.background` / `player.icon` / `player.icon.image`；选择存主程序的渲染槽位设置，
  插件停用 / 卸载后自动回退内置样式；
- 频谱读的是主程序已有的数据（`musicxx.state.spectrum` 广播 + `musicxx.media.spectrum` 能力），
  插件不另做分析。

## 5. 容易踩的坑

- **采样步长要逐分量相除**（`res / (texSize * 缩放)`）：写反了在屏幕与纹理宽高比不同时画面会拉伸变形，
  叠加的漂移还会被 `clamp` 吃掉（看起来"根本没动"）。五份着色器都按这个写法；
- **只认浮点版内置函数**：GLES 后端翻译成 GLSL ES 1.00，`clamp(int, …)` 这类会编不过；
  频带下标用 `if` 夹到范围内（见 `ring.frag` 的 `clampBandIndex`）；
- **`uCoverInfo.w` 才是"有没有封面"**，不要去采样纹理颜色猜；
- **取色处理器必须同步返回**：命中缓存给结果、没命中先回 null 再异步算；
- **不要在能力处理器里 `await` 应用实现的能力**（会互相等，表现为整个应用卡几秒）；
- **异步动作完成后要再刷一次页面**：切换样式 / 绑定纹理这类动作里的能力调用是异步的，
  处理器里那次 `slot.update` 早于结果，画到的是旧的槽位镜像 —— 页面上会出现“样式已经生效，
  但选中高亮要下次点击才变”。`plugin.js` 里的 `refreshPageAfter(view)` 就是干这件事，
  新增异步动作时把发起页面的内容 id 传进去就行（定时器里发起的动作没有页面，不用传）。
