/// example_js_playing_bg 的样式脚本: 六种播放页背景样式的声明
///
/// 本插件的六种样式占同一个渲染槽位（`player.background`），用户在
/// 『设置 → 主题 → 播放页面背景』里同时只能选中一个：
/// - 封面类：『模糊』（heat）/『沉浸』（card）/『黑胶』（vinyl）—— 画面是当前歌曲封面做的；
/// - 图案类：『晶格』（lattice）/『光圈』（ring）/『插件纹理铺底』（bound）—— 画面是着色器画的，
///   其中『晶格』与『插件纹理铺底』共用一个 bundle，区别在底层纹理从哪来。
///
/// 每种样式的 `data` 只有 `title` / `depict` / `view` 三个字段：**背景是一棵块树**，
/// 着色器只是里面的一个 `musicxx.Shader` 块（`bundle` + `args` + 帧率 / 分辨率 / 速度），
/// 纹理（封面 / 插件绑定的图片）写在块的 `args` 里。播放页前景（三色 + 是否用遮罩外观）
/// 不写在 UI 项里：样式被选中后由 palette.js 调 `musicxx.ui.playing.foreground.set`。
///
/// 这个文件只写"声明"（样式 id、标题、参数与封面纹理），不做任何注册：
/// 顶层注册统一放在 plugin.js 最后执行，避免"注册时用到的函数还没定义"。
/// 六个脚本在同一个 JS 上下文里执行，顶层名字必须全局唯一（见插件目录的 README）。

/// 插件 id 与显示名
const PLUGIN_ID = musicxx.pluginId;
const PLUGIN_TITLE = "示例播放页背景";

/// 六个样式的名字（状态行与页面按钮用它，不用到处写字符串）
const STYLE_BLUR = "blur";
const STYLE_CARD = "card";
const STYLE_VINYL = "vinyl";
const STYLE_LATTICE = "lattice";
const STYLE_RING = "ring";
const STYLE_BOUND = "bound";
/// 不是本插件的样式：内置背景 / 别人的样式（页面上的"内置背景"按钮与状态判断用）
const STYLE_BUILTIN = "builtin";
const STYLE_OTHER = "other";

/// UI 项短名（完整 id = plugin.<插件 id>.<短名>，由插件框架拼）
///
/// 三个封面样式沿用原插件的短名（`bg` / `bgCard` / `bgVinyl`）；
/// 图案样式里『晶格』不能叫 `bg`（已被『模糊』占用），所以用 `lattice`。
const ITEM_NAME_BLUR = "bg";
const ITEM_NAME_CARD = "bgCard";
const ITEM_NAME_VINYL = "bgVinyl";
const ITEM_NAME_LATTICE = "lattice";
const ITEM_NAME_RING = "ring";
const ITEM_NAME_BOUND = "bound";
/// 歌曲图接管项与主页入口
const ITEM_NAME_ICON = "icon";
const ITEM_NAME_HOME = "home";

const ITEM_ID_BLUR = "plugin." + PLUGIN_ID + "." + ITEM_NAME_BLUR;
const ITEM_ID_CARD = "plugin." + PLUGIN_ID + "." + ITEM_NAME_CARD;
const ITEM_ID_VINYL = "plugin." + PLUGIN_ID + "." + ITEM_NAME_VINYL;
const ITEM_ID_LATTICE = "plugin." + PLUGIN_ID + "." + ITEM_NAME_LATTICE;
const ITEM_ID_RING = "plugin." + PLUGIN_ID + "." + ITEM_NAME_RING;
const ITEM_ID_BOUND = "plugin." + PLUGIN_ID + "." + ITEM_NAME_BOUND;
const ITEM_ID_ICON = "plugin." + PLUGIN_ID + "." + ITEM_NAME_ICON;

/// 两个渲染槽位与它们的内置项 id
const BACKGROUND_SLOT = "player.background";
const ICON_SLOT = "player.icon";
const BUILTIN_BG_ID = "musicxx.auto";
/// 交还歌曲图用 `musicxx.previous`：恢复主程序记下的"上一个形状"，没有记录时什么都不改
/// （重复调用也安全），不会把用户现在选的形状冲掉。
const PREVIOUS_ICON_ID = "musicxx.previous";
/// 歌曲图槽位的内置项都是 `musicxx.` 开头（三种形状 = 内置显示）
const BUILTIN_ICON_PREFIX = "musicxx.";
/// 两个页面（说明页 / 设置页）的视图 id：页面路由是 `ext://<插件 id>/<视图 id>`
const HOME_VIEW_ID = "card";
const SETTINGS_VIEW_ID = "settings";

/// 样式 → UI 项 id / UI 项 id → 样式 / 标题 / 短名 / 说明
const ITEM_OF_STYLE = {};
ITEM_OF_STYLE[STYLE_BLUR] = ITEM_ID_BLUR;
ITEM_OF_STYLE[STYLE_CARD] = ITEM_ID_CARD;
ITEM_OF_STYLE[STYLE_VINYL] = ITEM_ID_VINYL;
ITEM_OF_STYLE[STYLE_LATTICE] = ITEM_ID_LATTICE;
ITEM_OF_STYLE[STYLE_RING] = ITEM_ID_RING;
ITEM_OF_STYLE[STYLE_BOUND] = ITEM_ID_BOUND;

const STYLE_OF_ITEM = {};
STYLE_OF_ITEM[ITEM_ID_BLUR] = STYLE_BLUR;
STYLE_OF_ITEM[ITEM_ID_CARD] = STYLE_CARD;
STYLE_OF_ITEM[ITEM_ID_VINYL] = STYLE_VINYL;
STYLE_OF_ITEM[ITEM_ID_LATTICE] = STYLE_LATTICE;
STYLE_OF_ITEM[ITEM_ID_RING] = STYLE_RING;
STYLE_OF_ITEM[ITEM_ID_BOUND] = STYLE_BOUND;

const STYLE_TITLES = {};
STYLE_TITLES[STYLE_BLUR] = PLUGIN_TITLE + " · 模糊";
STYLE_TITLES[STYLE_CARD] = PLUGIN_TITLE + " · 沉浸";
STYLE_TITLES[STYLE_VINYL] = PLUGIN_TITLE + " · 黑胶";
STYLE_TITLES[STYLE_LATTICE] = PLUGIN_TITLE + " · 晶格";
STYLE_TITLES[STYLE_RING] = PLUGIN_TITLE + " · 光圈";
STYLE_TITLES[STYLE_BOUND] = PLUGIN_TITLE + " · 插件纹理铺底";

/// 状态行里用的短名（完整标题太长，窄窗口放不下）
const STYLE_SHORT = {};
STYLE_SHORT[STYLE_BLUR] = "模糊";
STYLE_SHORT[STYLE_CARD] = "沉浸";
STYLE_SHORT[STYLE_VINYL] = "黑胶";
STYLE_SHORT[STYLE_LATTICE] = "晶格";
STYLE_SHORT[STYLE_RING] = "光圈";
STYLE_SHORT[STYLE_BOUND] = "插件纹理";

const STYLE_DEPICTS = {};
STYLE_DEPICTS[STYLE_BLUR] =
    "封面高斯模糊后放大铺满屏幕、缓慢四处漂移；模糊程度 0~30、放大倍数 1~3 可调";
STYLE_DEPICTS[STYLE_CARD] =
    "封面主色做渐变底、清晰封面贴着屏幕边缘铺一片（自由边模糊、图片占比可调），按主题叠亮暗遮罩";
STYLE_DEPICTS[STYLE_VINYL] =
    "封面主色铺底，中间一张黑胶唱片（音轨纹路 + 左上柔光），盘心是缓慢旋转的歌曲封面，" +
    "盘沿上方一支白色唱臂（转轴 + 折线臂管 + 一小段唱头壳，轻拟物淡阴影）压在盘面外圈；转速档位可调";
STYLE_DEPICTS[STYLE_LATTICE] =
    "跟随封面配色的晶格化动态背景（固定格子大小的随机点切块 + 模糊封面铺底，只随时间缓慢漂移）";
STYLE_DEPICTS[STYLE_RING] =
    "64 个频点从正上方顺时针排成一圈，尖角跟着音乐向内/向外突出，相邻尖角用直线连成蛛网，圆心是圆形封面";
STYLE_DEPICTS[STYLE_BOUND] =
    "与『晶格』同一个 bundle，但底层纹理是插件自己绑定的图片（先在设置页把当前封面绑成纹理）";

/// 样式名（不是本插件的样式时返回空串）
function styleTitleOf(style) {
    return STYLE_TITLES[style] || "";
}

/// UI 项 id → 样式名（不是本插件的项时返回空串）
function styleOfItem(itemId) {
    return STYLE_OF_ITEM[itemId] || "";
}

/// 封面纹理参数
///
/// `blur` 是**纹理上的** sigma：纹理铺满屏幕后再放大，屏幕上看到的模糊 ≈
/// `blur × 纹理→屏幕的放大倍数` —— 512 的纹理在 1280 宽的窗口里大约放大 5 倍，
/// 所以 30 会糊成一片渐变（只剩色带），14 才能看出封面的大致内容。
const BLUR_COVER_SIZE = 512;
const CARD_COVER_SIZE = 768;
/// 『黑胶』盘心是圆裁的封面：纹理声明成正方形（`square: true`），圆内正好是它的内切圆
const VINYL_COVER_SIZE = 768;
/// 『晶格』的铺底是预模糊的封面；『光圈』圆心是清晰封面
const LATTICE_COVER_SIZE = 384;
const LATTICE_COVER_BLUR = 28;
const RING_COVER_SIZE = 512;

/// 插件图像表里的键（渲染项 `image` 与动作 `musicxx.media.bindImage` 的 `key` 用它）
const BOUND_IMAGE_KEY = "coverArt";

/// 一张**当前歌曲封面**的纹理声明（写在着色器块的 `args` 里）
///
/// `args` 的成员名就是着色器里的 `sampler2D` 名；信息 uniform（纹理宽、纹理高、
/// 原图宽高比、是否有内容）的成员名缺省是 `<成员名>Info`（本例的着色器就用这个名字）。
/// `size` / `blur` / `square` 只对封面来源有效：主程序解码、缩放、可选预模糊后上传成
/// GPU 纹理，插件不搬字节。
function coverTexture(size, blur, square) {
    const decl = { kind: "texture", source: "musicxx.cover", size: size };
    if (blur) {
        decl.blur = blur;
    }
    if (square) {
        decl.square = true;
    }
    return decl;
}

/// 一张**插件自己绑定的图片**的纹理声明（`musicxx.image.<key>`）
///
/// 图片由 `musicxx.media.bindImage`（或 `musicxx.media.cover` 的 `bind` 参数）放进图像表；
/// 插件还没绑时主程序绑 1×1 透明占位纹理，画面只剩着色器自己画的部分。
function imageTexture(key) {
    return { kind: "texture", source: "musicxx.image." + key };
}

/// 背景的根视图：一块着色器块
///
/// 背景是一棵块树，着色器就是根上的那个 `musicxx.Shader` 块。根块是着色器时渲染层
/// 会给它紧约束，所以不用再包 `Expanded`（那层包法现在是多余的，包了也能跑）。
/// [options] 是块上的其余字段：`speed`（时间推进速度，缺省 1）、`maxFps`、
/// `resolutionScale`、`animate`。
function backgroundView(bundle, args, options) {
    const opts = options || {};
    return {
        kind: "musicxx.Shader",
        bundle: bundle,
        args: args,
        speed: (opts.speed !== undefined) ? opts.speed : 1,
        maxFps: (opts.maxFps !== undefined) ? opts.maxFps : 16,
        resolutionScale: (opts.resolutionScale !== undefined) ? opts.resolutionScale : 1,
        animate: (opts.animate !== undefined) ? opts.animate : true,
    };
}

/// 基准速度与可选倍率（只影响三个图案样式：声明给插件框架的 `speed` = 基准速度 x 倍率）
///
/// 插件框架每帧直接用它推进时间（不做二次缩放），`speed` 最后会写进 uniform 的 `uParams.w`。
const BACKGROUND_BASE_SPEED = 1;
const BG_RATE_OPTIONS = [0.5, 1, 2];
const DEFAULT_BG_RATE = 1;

/// 把任意值归一到允许的倍率（非法值回退 1x）
function normalizeBgRate(value) {
    const rate = Number(value);
    for (let i = 0; i < BG_RATE_OPTIONS.length; ++i) {
        if (BG_RATE_OPTIONS[i] === rate) {
            return rate;
        }
    }
    return DEFAULT_BG_RATE;
}

/// 倍率的显示文本（1 → "1x"、0.5 → "0.5x"）
function bgRateText(rate) {
    return String(normalizeBgRate(rate)) + "x";
}

/// 倍率 → 声明给插件框架的时间推进速度
function bgSpeedOf(rate) {
    return BACKGROUND_BASE_SPEED * normalizeBgRate(rate);
}

/// 频谱来源的"快起慢落"声明
///
/// 频谱数据是 10 帧/秒，直接取会有台阶感（每 100 ms 跳一次）：`smooth` 让值在跳变时
/// 快速跟上、回落时平滑过渡（`attackMs` 小、`releaseMs` 大 = 电平表手感）。
/// 插件框架还会按播放位置在两帧频谱之间插值，两者叠加后画面才是连续的。
function smoothSpectrum(name) {
    return {
        kind: "smooth",
        attackMs: 20,
        releaseMs: 260,
        of: { kind: "source", name: name },
    };
}

/// 『光圈』的参数声明: 4 个绘制色 + 线条色 + 响度 + **64 个频带**
///
/// 圆上顺时针排 64 个点（左右不镜像，起点是正上方），一个点一个频带，所以数据要用
/// **64 个频带**（`spectrum.bands64.0..15`，每项 4 个连续频带写在 xyzw）：
/// 16 个频带太粗（一个点占不到一个值），一帧 256 个频点又要声明 64 个 vec4 成员、
/// 而且每个像素都得按算出来的组号在 64 个成员里挑一次（GLSL ES 1.00 不允许用运行时
/// 下标取 uniform，只能逐组比较）—— 实测 1280x720 单帧渲染要多花 90 ms 以上。
/// 每个频带包一层 `smooth`：频谱是 10 帧/秒的数据，直接取尖角会有台阶感。
/// 颜色从外面传进来：背景声明用映射后的 4 个绘制色，页面里的内联块用主题色 + 封面提取色。
function ringArgs(color1, color2, color3, color4) {
    const args = {
        uColor1: color1,
        uColor2: color2,
        uColor3: color3,
        uColor4: color4,
        // 线条色: 没有来源，昼夜各给一个固定值（夜间稍暗，免得抢前景文字）
        uLine: { kind: "const", value: "#f6faff", night: "#dbe7f7" },
        // 响度: 中心光源与光束的亮度
        uLevel: smoothSpectrum("musicxx.spectrum.level"),
    };
    for (let i = 0; i < 16; ++i) {
        args["uBands" + i] = smoothSpectrum("musicxx.spectrum.bands64." + i);
    }
    return args;
}

/// 四个绘制色（封面经主题映射后的颜色，内置播放页背景用的就是这 4 色）
function mappingColors() {
    return {
        uColor1: { kind: "source", name: "musicxx.icon.themeMapping.0" },
        uColor2: { kind: "source", name: "musicxx.icon.themeMapping.1" },
        uColor3: { kind: "source", name: "musicxx.icon.themeMapping.2" },
        uColor4: { kind: "source", name: "musicxx.icon.themeMapping.3" },
    };
}

/// 『模糊』的完整声明（封面预模糊铺满、放大漂移、热浪扭曲）
///
/// `uCover` 是封面纹理每帧绑定到的采样器，`(纹理宽, 纹理高, 原图宽高比, 是否有封面)`
/// 写进 `uCoverInfo`。声明了纹理才会有这份成本：没声明就不解码、不上传、不占显存。
function blurBackgroundData() {
    return {
        title: STYLE_TITLES[STYLE_BLUR],
        depict: STYLE_DEPICTS[STYLE_BLUR],
        view: backgroundView("shader/heat.shaderbundle", {
            uCover: coverTexture(BLUR_COVER_SIZE, configuredBlur()),
            uGlow: {
                kind: "source",
                name: "musicxx.icon.themeMapping.0",
                fallback: { kind: "const", value: "#3a4a66" },
            },
            uGlow2: {
                kind: "source",
                name: "musicxx.icon.themeMapping.1",
                fallback: { kind: "const", value: "#6a4a5c" },
            },
            // 画面放大倍率（着色器按它算采样步长；数值越小看到的内容越完整）
            uZoom: { kind: "const", value: configuredZoom() },
            // 热浪扭曲波纹开关（关掉后画面只剩缓慢漂移）
            uRipple: { kind: "const", value: configuredRipple() ? 1 : 0 },
        }, {
            speed: 1,
            maxFps: 24,
            // 模糊画面不需要满分辨率：降采样后由插件框架放大，省一半以上的像素
            resolutionScale: 0.6,
        }),
    };
}

/// 『沉浸』的完整声明（清晰封面卡片 + 封面主色渐变底 + 主题亮暗遮罩）
function cardBackgroundData() {
    return {
        title: STYLE_TITLES[STYLE_CARD],
        depict: STYLE_DEPICTS[STYLE_CARD],
        view: backgroundView("shader/card.shaderbundle", {
            // 封面卡片要是原图：不预模糊
            uCover: coverTexture(CARD_COVER_SIZE, 0),
            // 渐变底：封面主色（亮端）→ 封面暗色（暗端）；convert = 套主程序的昼夜转换
            uBase: {
                kind: "source",
                name: "musicxx.icon.main",
                convert: true,
                fallback: "#3d4a63",
            },
            uBase2: {
                kind: "source",
                name: "musicxx.icon.dark",
                convert: true,
                fallback: "#141a28",
            },
            // 亮暗遮罩的颜色：主题背景色（浅色主题是浅色、深色主题是深色）
            uMask: {
                kind: "source",
                name: "musicxx.theme.background",
                fallback: "#12151c",
            },
            uZoom: { kind: "const", value: configuredZoom() },
            // 图片占比（图片至少占屏幕的这个比例，越大图片越多、渐变底越少）
            uShare: { kind: "const", value: configuredShare() },
            uRipple: { kind: "const", value: configuredRipple() ? 1 : 0 },
        }, {
            speed: 1,
            // 这一种基本是静态画面（只有热浪波纹在动），帧率给低一档就够
            maxFps: 16,
            resolutionScale: 1,
        }),
    };
}

/// 『黑胶』的完整声明（正方形圆裁封面 + 盘心旋转 + 白色唱臂）
///
/// 盘心旋转由块的 `speed` 决定（写进 `uParams.w`，着色器按它推进封面转角，
/// 所以改转速不用重新选中这个样式）。
function vinylBackgroundData() {
    return {
        title: STYLE_TITLES[STYLE_VINYL],
        depict: STYLE_DEPICTS[STYLE_VINYL],
        view: backgroundView("shader/vinyl.shaderbundle", {
            // 盘心是圆裁的封面：纹理声明成正方形（圆内正好是它的内切圆）
            uCover: coverTexture(VINYL_COVER_SIZE, 0, true),
            // 底：封面主色；四周要暗下去，着色器里再往第二个绘制色靠一点
            uBg: {
                kind: "source",
                name: "musicxx.icon.themeMapping.0",
                fallback: { kind: "const", value: "#2d6c96" },
            },
            uBg2: {
                kind: "source",
                name: "musicxx.icon.themeMapping.1",
                fallback: { kind: "const", value: "#1b3d56" },
            },
            // 唱臂的颜色（近白；夜间稍微压一点，别在暗底上过曝）
            uArm: { kind: "const", value: "#f2f6fb", night: "#e2eaf4" },
        }, {
            // 转速：着色器里的基准转速（30 秒一圈）再乘这个倍率
            speed: configuredSpin(),
            maxFps: 24,
            resolutionScale: 1,
        }),
    };
}

/// 『晶格』的完整声明
///
/// 组成: 封面经中心裁剪 + 预模糊后当铺底（`uCoverBack`），上面叠晶格图案；
/// 画面只随时间缓慢漂移，不读频谱。`musicxx.ui.slot.update` 是**整体替换**，
/// 所以每次都从这一个函数取完整 data。
function latticeBackgroundData() {
    return {
        title: STYLE_TITLES[STYLE_LATTICE],
        depict: STYLE_DEPICTS[STYLE_LATTICE],
        view: backgroundView("shader/bg.shaderbundle", Object.assign(mappingColors(), {
            "uCoverBack": coverTexture(LATTICE_COVER_SIZE, LATTICE_COVER_BLUR),
        }), {
            speed: bgSpeedOf(configuredBgRate()),
            maxFps: 16,
        }),
    };
}

/// 『光圈』的完整声明（频谱圆环 + 圆心清晰封面）
///
/// 与『晶格』的区别只在 bundle 与参数: 参数见 `ringArgs`（64 个频带，圆上 64 个点），
/// 没有频谱数据时插件框架写 0，圆上的尖角长度为 0，只剩圆心光源。
function ringBackgroundData() {
    return {
        title: STYLE_TITLES[STYLE_RING],
        depict: STYLE_DEPICTS[STYLE_RING],
        view: backgroundView("shader/ring.shaderbundle", Object.assign(ringArgs(
            { kind: "source", name: "musicxx.icon.themeMapping.0" },
            { kind: "source", name: "musicxx.icon.themeMapping.1" },
            { kind: "source", name: "musicxx.icon.themeMapping.2" },
            { kind: "source", name: "musicxx.icon.themeMapping.3" }
        ), {
            // 圆心封面: 中心裁剪成正方形、缩到 512, 不模糊
            "uCover": coverTexture(RING_COVER_SIZE, 0, true),
        }), {
            speed: bgSpeedOf(configuredBgRate()),
            maxFps: 16,
        }),
    };
}

/// 『插件纹理铺底』的完整声明: 底层用**插件自己绑定的图片**
///
/// 与『晶格』同一个 bundle，只是把那张“铺底纹理”从当前歌曲封面（`musicxx.cover`）
/// 换成插件绑定的图片（`musicxx.image.<key>`）：`key` 指向图像表里的条目，
/// `args` 的成员名是着色器里的 sampler 名（与『晶格』同名，着色器不用改）。
/// 插件还没绑定时主程序绑 1×1 透明占位纹理，画面只剩晶格图案。
function boundBackgroundData() {
    return {
        title: STYLE_TITLES[STYLE_BOUND],
        depict: STYLE_DEPICTS[STYLE_BOUND],
        view: backgroundView("shader/bg.shaderbundle", Object.assign(mappingColors(), {
            "uCoverBack": imageTexture(BOUND_IMAGE_KEY),
        }), {
            speed: bgSpeedOf(configuredBgRate()),
            maxFps: 16,
        }),
    };
}

/// 歌曲图接管项的内容：不显示内容、保留占位
///
/// 三种封面样式的画面里已经有封面（铺满 / 卡片 / 盘心），播放页中间再放一个封面圆盘
/// 就重复了。打开「隐藏歌曲图」后本插件才接管，背景切走时自动交还。
function iconEntryData() {
    return {
        title: "隐藏歌曲图",
        depict: "接管播放页中间的歌曲图：保留占位、不画内容；背景换回其它样式时自动交还",
        mode: "none",
        keepSpace: true,
    };
}

/// 当前倍率（配置还没读到 / 值非法时用默认值）
function configuredBgRateValue() {
    return normalizeBgRate(configuredBgRate());
}

/// 重新声明六个背景样式（`musicxx.ui.slot.update` 是整体替换，每次都要给完整 data）
///
/// 插件框架收到后会刷新候选；正在使用的样式立即用新参数（封面纹理按新参数重新解码，
/// 『模糊』改模糊程度时就能当场看到变化；『黑胶』改转速只换块的 `speed`，下一帧就按新转速转）。
/// 前景三色不在这里：它由 [pushForeground] 单独调能力交给应用。
function refreshBackgroundEntries() {
    musicxx.ui.slot.update(ITEM_NAME_BLUR, blurBackgroundData());
    musicxx.ui.slot.update(ITEM_NAME_CARD, cardBackgroundData());
    musicxx.ui.slot.update(ITEM_NAME_VINYL, vinylBackgroundData());
    musicxx.ui.slot.update(ITEM_NAME_LATTICE, latticeBackgroundData());
    musicxx.ui.slot.update(ITEM_NAME_RING, ringBackgroundData());
    musicxx.ui.slot.update(ITEM_NAME_BOUND, boundBackgroundData());
}

/// 顶层注册六个背景样式 + 歌曲图接管项 + 主页入口（由 plugin.js 在最后调用）
///
/// 注册顺序与 `order` 决定设置列表里的先后（快照按类型 + order 排序）：
/// 封面三种在前（20~22），图案三种在后（30~32）。
function registerPluginEntries() {
    musicxx.ui.slot.register({
        name: ITEM_NAME_BLUR,
        type: "playing.background",
        order: 20,
        data: blurBackgroundData(),
    });
    musicxx.ui.slot.register({
        name: ITEM_NAME_CARD,
        type: "playing.background",
        order: 21,
        data: cardBackgroundData(),
    });
    musicxx.ui.slot.register({
        name: ITEM_NAME_VINYL,
        type: "playing.background",
        order: 22,
        data: vinylBackgroundData(),
    });
    musicxx.ui.slot.register({
        name: ITEM_NAME_LATTICE,
        type: "playing.background",
        order: 30,
        data: latticeBackgroundData(),
    });
    musicxx.ui.slot.register({
        name: ITEM_NAME_RING,
        type: "playing.background",
        order: 31,
        data: ringBackgroundData(),
    });
    musicxx.ui.slot.register({
        name: ITEM_NAME_BOUND,
        type: "playing.background",
        order: 32,
        data: boundBackgroundData(),
    });
    musicxx.ui.slot.register({
        name: ITEM_NAME_ICON,
        type: "playing.icon",
        order: 40,
        data: iconEntryData(),
    });
    musicxx.ui.slot.register({
        name: ITEM_NAME_HOME,
        type: "home.entry",
        order: 130,
        data: {
            title: PLUGIN_TITLE,
            subtitle: "六种播放页背景（封面三种 + 图案三种）+ 歌曲图接管 + 封面取色 + 自动前景色",
            // 图标放在 data 里（`musicxx.ui.slot.register` 只在没有 data 时才把顶层字段并进去）；
            // 图标是插件自己带的 svg：相对插件目录的路径（resource/svg/ 只是推荐位置，
            // 插件框架不接受自己的图标名）
            icon: "resource/svg/vinyl.svg",
            action: { kind: "route", route: "ext://" + PLUGIN_ID + "/" + HOME_VIEW_ID },
        },
    });
}
