/// musicxx 外部插件: playing_bg_image —— 「示例封面背景」
///
/// 三件事（都由『拟声++』与用户在设置里的选择门控，未选中时零成本）：
///
/// 1. **播放页背景的三种模式**（渲染槽位 `player.background`，同一个槽位里的三个候选样式）：
///
///    · 『示例封面背景 · 模糊』（`shader/heat.shaderbundle`）：当前歌曲封面预模糊后放大
///      铺满屏幕、缓慢四处漂移，画面上分布着密而小的热浪斑块、斑块里的内容微微扭曲 ——
///      类似夏天地面温度高、空气被折射时看到的晃动。模糊程度（0~30）与放大倍数（1~3）
///      在设置页里调，扭曲波纹可以关掉；没有封面时回退成暗底 + 光斑。
///
///    · 『示例封面背景 · 沉浸』（`shader/card.shaderbundle`）：封面主色做渐变底，
///      清晰封面贴着屏幕边缘铺一片 —— 竖屏贴上、左、右三条边（下边缘模糊渐隐），横屏贴左、
///      上、下三条边（右边缘模糊渐隐）；放大倍数（1~3）只改取景（越大越局部），
///      最后按主题叠一层亮暗遮罩（遮罩色就是主题背景色，浅色主题提亮、深色主题压暗）。
///
///    · 『示例封面背景 · 黑胶』（`shader/vinyl.shaderbundle`）：封面主色铺底，画面中间一张
///      黑胶唱片 —— 盘面是细密的音轨纹路加一道来自左上方的柔光与外缘倒角高光，盘心圆裁
///      当前歌曲封面、像唱片一样缓慢旋转；盘沿上方一支白色唱臂（转轴圆钮 + **折线**臂管
///      （肘部圆角过渡，像手臂那样折一下）+ 贴着盘沿的一小段唱头壳，周围一层很软的淡阴影，
///      轻拟物），唱头压在盘面的外圈上。
///      转速档位（0.5× / 1× / 2×）在设置页里调，没有封面时盘心画一个两色渐变的标签。
///
/// 2. **接管播放页歌曲图**（渲染槽位 `player.icon`）：声明 `mode: "none"` + `keepSpace: true`
///    —— 不显示内容、只保留占位（封面已经铺满画面 / 已经在盘心，原来的封面圆盘与它重复）。
///    设置页的「隐藏歌曲图」**默认关**（多数用户希望播放页保留歌曲图）；打开后，背景是本插件
///    的三种模式之一时自动接管：每秒读一次状态镜像 `musicxx.state.renderSlots` 判断，
///    背景切走（或关掉开关）就自动交还。
///
/// 3. **封面取色**（钩子 `musicxx.media.palette.provide`）：自己算封面配色 ——
///    把封面缩到 64×64 读 RGBA，按 4 bit/通道量化统计，取出现次数最多的 4 个色点，
///    按亮度映射成 main / light / lightMuted / dark / darkMuted，替代内置的颜色分析。
///    『沉浸』的渐变底用的就是这套颜色（`musicxx.icon.main` / `musicxx.icon.dark`），
///    『黑胶』的底色也从它来（`musicxx.icon.themeMapping.0` / `.1`；唱臂是固定的近白色）。
///
/// 4. **自动调整播放页前景色**（设置页的「自动调整前景色」，**默认开**）：本插件的三种背景
///    都是封面自己的颜色铺出来的，主题自带的前景色（浅色主题的深灰文字 / 深色主题的白字）
///    不一定压得住画面。打开后按封面取色的主色（[foregroundColorsSpec]）推一套页面前景：
///    **保留主色的色相与彩色程度**，只把明度朝主题倾向的一侧调（白昼偏深、夜间偏浅），
///    调到刚好够清楚为止 —— 文字看着就是主色的深浅版。对比度按**当前模式估出的背景亮度**
///    算（[backgroundEstimate]），要求逐条放宽（见 [FG_PICK_RULES]）：先争取高对比 + 明显的
///    颜色，不够再依次让一点对比度（不低于大字号标准），最后才让颜色 —— 颜色能保住就保住，
///    只有怎么调都给不出颜色时才退回黑 / 白。另外配出减淡色（同色 30% 透明）与块状背景色
///    （与普通色相反的一侧、低饱和、15% 透明）。三色用 UI 项的 `foregroundColors` 交给插件框架 ——
///    播放页的文字、歌词、进度条与块状背景都用它；换歌 / 换模式 / 主题切换（白昼 ↔ 夜间）后
///    重新推，关掉开关就交回主题默认色。
///
/// 设置（7 项，存在插件目录的 config.json）：模糊程度、放大倍数、图片占比、唱片转速、
/// 热浪扭曲波纹、隐藏歌曲图、自动调整前景色。
///
/// 排查入口：主程序日志（`musicxx.host.log`）、设置页（`ext://playing_bg_image/settings`）、
/// 说明页（`ext://playing_bg_image/card`）、状态镜像里的 `player.background` / `player.icon`
/// 两个槽位。
///
/// 约束：脚本顶层必须**同步**完成注册（顶层不能用 await）；异步逻辑放到钩子或定时器里。
const kit = pluginxx.ui.kit;

/// 插件 id 与显示名
const PLUGIN_ID = musicxx.pluginId;
const PLUGIN_TITLE = "示例封面背景";

/// 三种模式（同一个槽位里的三个候选样式：用户在『设置 → 主题 → 播放页面背景』里选一个）
const MODE_BLUR = "blur";
const MODE_CARD = "card";
const MODE_VINYL = "vinyl";
/// 模式选择里的"用内置背景"（不是本插件的模式，只是让三个样式都不生效）
const MODE_BUILTIN = "builtin";

/// 模式的显示名与副标题（设置列表里的样式名 = 这里的标题）
const MODE_TITLES = {};
MODE_TITLES[MODE_BLUR] = PLUGIN_TITLE + " · 模糊";
MODE_TITLES[MODE_CARD] = PLUGIN_TITLE + " · 沉浸";
MODE_TITLES[MODE_VINYL] = PLUGIN_TITLE + " · 黑胶";
/// 模式的短名（状态行里用；完整标题太长，窄窗口里放不下）
const MODE_SHORT = {};
MODE_SHORT[MODE_BLUR] = "模糊";
MODE_SHORT[MODE_CARD] = "沉浸";
MODE_SHORT[MODE_VINYL] = "黑胶";
const MODE_DEPICTS = {};
MODE_DEPICTS[MODE_BLUR] =
    "封面高斯模糊后放大铺满屏幕、缓慢四处漂移；模糊程度 0~30、放大倍数 1~3 可调";
MODE_DEPICTS[MODE_CARD] =
    "封面主色做渐变底、清晰封面贴着屏幕边缘铺一片（自由边模糊、图片占比可调），按主题叠亮暗遮罩";
MODE_DEPICTS[MODE_VINYL] =
    "封面主色铺底，中间一张黑胶唱片（音轨纹路 + 左上柔光），盘心是缓慢旋转的歌曲封面，" +
    "盘沿上方一支白色唱臂（转轴 + 折线臂管 + 一小段唱头壳，轻拟物淡阴影）压在盘面外圈；转速档位可调";

/// 三种模式各自的 UI 项短名与完整 id
///
/// 模式一沿用原来的短名 `bg`：已经选中它的用户不会因为这次重构而失效。
const BG_ITEM_NAME = "bg";
const BG_ITEM_ID = "plugin." + PLUGIN_ID + "." + BG_ITEM_NAME;
const CARD_ITEM_NAME = "bgCard";
const CARD_ITEM_ID = "plugin." + PLUGIN_ID + "." + CARD_ITEM_NAME;
const VINYL_ITEM_NAME = "bgVinyl";
const VINYL_ITEM_ID = "plugin." + PLUGIN_ID + "." + VINYL_ITEM_NAME;

/// 插件项 id → 模式名（判断"背景现在是不是本插件在画"、状态行里显示哪一个模式都用它）
const MODE_OF_ITEM = {};
MODE_OF_ITEM[BG_ITEM_ID] = MODE_BLUR;
MODE_OF_ITEM[CARD_ITEM_ID] = MODE_CARD;
MODE_OF_ITEM[VINYL_ITEM_ID] = MODE_VINYL;

/// 模式名 → 插件项 id（模式选择按钮切槽位时用它）
const ITEM_OF_MODE = {};
ITEM_OF_MODE[MODE_BLUR] = BG_ITEM_ID;
ITEM_OF_MODE[MODE_CARD] = CARD_ITEM_ID;
ITEM_OF_MODE[MODE_VINYL] = VINYL_ITEM_ID;

/// 歌曲图接管项的短名与完整 id
const ICON_ITEM_NAME = "icon";
const ICON_ITEM_ID = "plugin." + PLUGIN_ID + "." + ICON_ITEM_NAME;

/// 主页入口与两个页面（说明页 / 设置页）的视图 id
const HOME_ITEM_NAME = "home";
const HOME_VIEW_ID = "card";
const SETTINGS_VIEW_ID = "settings";

/// 两个槽位与它们的内置项 id（切回内置背景 / 交还歌曲图用）
const BACKGROUND_SLOT = "player.background";
const ICON_SLOT = "player.icon";
const BUILTIN_BG_ID = "builtin:Auto";
const BUILTIN_ICON_ID = "builtin:default";

/// 封面纹理参数：主程序解码 → 缩放 → 可选预模糊（着色器只做放大、漂移、扭曲）
///
/// `cover.blur` 是**纹理上的** sigma：纹理铺满屏幕后再放大，屏幕上看到的模糊 ≈
/// `blur × 纹理→屏幕的放大倍数` —— 512 的纹理在 1280 宽的窗口里大约放大 5 倍，
/// 所以 30 会糊成一片渐变（只剩色带），14 才能看出封面的大致内容。
/// 两个模式各用一份纹理：『模糊』按设置预模糊，『沉浸』要清晰（blur = 0）。
const COVER_TEXTURE = "uCover";
const COVER_INFO = "uCoverInfo";
const BLUR_COVER_SIZE = 512;
const CARD_COVER_SIZE = 768;
/// 『黑胶』盘心是圆裁的封面：纹理声明成正方形（`square: true`），圆内正好是它的内切圆，
/// 尺寸给得比另两种大一点（盘心在屏幕上的直径可能有六七百像素）
const VINYL_COVER_SIZE = 768;

/// 设置的取值范围与默认值
const BLUR_MIN = 0;
const BLUR_MAX = 30;
const BLUR_DEFAULT = 14;
const ZOOM_MIN = 1;
const ZOOM_MAX = 3;
const ZOOM_DEFAULT = 2;
/// 图片占比（只影响『沉浸』）：图片至少占屏幕这个比例，越大图片越多、渐变底越少
const SHARE_MIN = 0.3;
const SHARE_MAX = 1;
const SHARE_DEFAULT = 0.75;
/// 唱片转速（只影响『黑胶』）：写进 UI 项的 `speed`，着色器按它推进封面转角
const SPIN_MIN = 0.25;
const SPIN_MAX = 4;
const SPIN_DEFAULT = 1;
/// 『黑胶』的基准转速：speed = 1 时盘心封面转一圈的秒数（与 vinyl.frag 的 kSpinSeconds 一致）
const VINYL_SPIN_SECONDS = 30;
/// 热浪扭曲波纹：默认开
const RIPPLE_DEFAULT = true;
/// 隐藏歌曲图：默认关（保持播放页原来的歌曲图；想只要背景时再打开）
const HIDE_ICON_DEFAULT = false;
/// 自动调整前景色：默认开
///
/// 三种背景都是封面自己的颜色铺出来的，主题默认的前景色不一定压得住画面，
/// 所以默认按封面主色 + 亮暗主题推一套页面前景（见 [foregroundColorsSpec]）。
const AUTO_FOREGROUND_DEFAULT = true;

/// 自动前景色的取值（见 [pickForegroundMain]）
///
/// 做法是"**从主色本身出发**"：保留它的色相与彩色程度（色度），只把明度朝主题倾向的一侧
/// 调到刚好够清楚为止 —— 所以文字通常是"主色的加深 / 变亮版"，而不是一律黑或白。
const FG_CONTRAST_MIN = 4.5;
/// 挑普通色时的"要求表"：从上到下逐条放宽
///
/// 每条 = `{对比度下限, 要求的彩色程度}`。先争取"高对比（AAA 级）+ 明显的颜色"，
/// 达不到就依次让一点对比度 —— 宁可少一点对比度（仍然不低于大字号标准的
/// [FG_CONTRAST_FLOOR]），也尽量把颜色保住；只有全都给不出颜色时才会退回高对比的黑 / 白。
const FG_PICK_RULES = [
    { contrast: 7.0, chroma: 0.6 },
    { contrast: FG_CONTRAST_MIN, chroma: 0.6 },
    { contrast: 4.2, chroma: 0.45 },
    { contrast: 3.8, chroma: 0.3 },
    { contrast: 3.4, chroma: 0.15 },
    { contrast: 3.0, chroma: 0 },
];
/// 彩色程度低于它就不算"看得出颜色"（[FG_PICK_RULES] 里最后两条的判据）
const FG_CONTRAST_FLOOR = 3.0;
/// 彩色程度档位（相对主色的色度，从"完整的画面主色"到"接近灰"）
const FG_CHROMA_LEVELS = [1, 0.55, 0.25, 0];
/// 前景色的色度上限（HSL 里的 max - min；主色越鲜艳越接近它）
const FG_CHROMA_MAX = 0.6;
/// 主色的色度放大一点：文字面积小，颜色要比背景更"实"才看得出
const FG_CHROMA_BOOST = 1.3;
/// 色度低于它就算"近灰"：挑前景色相时会跳过这样的色点（见 [accentColorOf]）
const FG_ACCENT_MIN_CHROMA = 0.1;
/// 明度搜索的步长与上下限
const FG_LIGHTNESS_STEP = 0.02;
const FG_LIGHTNESS_MIN = 0.04;
const FG_LIGHTNESS_MAX = 0.96;
/// 减淡色与块状背景色的透明度（与主题默认前景的 30% / 15% 一致）
const FG_CROSS_ALPHA = 0.3;
const FG_BLOCK_ALPHA = 0.15;
/// 块状背景色取"与普通色相反的一侧"（普通色偏暗 → 块状背景偏亮），低饱和
const FG_BLOCK_DARK = 0.45;
const FG_BLOCK_LIGHT = 0.72;
const FG_BLOCK_MAX_SATURATION = 0.3;
/// 查一次"当前昼夜 + 封面主色"的间隔
///
/// 本插件的背景在画时才需要勤查（用户切了主题，几秒内就跟着换色）；没在画时几乎不用查。
const FG_REFRESH_ACTIVE_MS = 5 * 1000;
const FG_REFRESH_IDLE_MS = 30 * 1000;

/// 设置页上的一排候选项（精调走输入框）
const BLUR_CHOICES = [0, 8, 16, 24, 30];
const ZOOM_CHOICES = [1, 1.5, 2, 2.5, 3];
const SHARE_CHOICES = [0.45, 0.6, 0.75, 0.9];
const SPIN_CHOICES = [0.5, 1, 2];

/// 错误对象 → 文本（钩子、定时器里的失败只记日志）
function errText(err) {
    return (err && err.message) ? err.message : String(err);
}

/* ==================== 插件配置（config.json，设置页改它） ==================== */

/// 各配置项的默认值：读不到时用它（框架不保存默认值）
const CONFIG_DEFAULTS = {
    blur: BLUR_DEFAULT,
    zoom: ZOOM_DEFAULT,
    share: SHARE_DEFAULT,
    spin: SPIN_DEFAULT,
    ripple: RIPPLE_DEFAULT,
    hideIcon: HIDE_ICON_DEFAULT,
    autoForeground: AUTO_FOREGROUND_DEFAULT,
};

/// 读到 / 用户改过的值（没读到时用 [CONFIG_DEFAULTS]）
let configValues = {};

/// 该键是否已经确定（读过或用户改过）
///
/// 异步读的结果可能晚于用户操作才到：已经确定的键不要再被读回来的旧值覆盖
const configSettled = {};

/// 数值配置：解析失败用默认值，超出范围夹到边界，最多两位小数
function configNumber(value, min, max, def) {
    const parsed = Number(value);
    if (false == isFinite(parsed)) {
        return def;
    }
    if (parsed < min) {
        return min;
    }
    if (parsed > max) {
        return max;
    }
    return Math.round(parsed * 100) / 100;
}

/// 开关配置：接受布尔与 "true"/"1" 这类写法（手改 config.json 时写字符串也能用）
function configBool(value, def) {
    if (typeof value === "boolean") {
        return value;
    }
    if (value === 1 || value === "1" || value === "true") {
        return true;
    }
    if (value === 0 || value === "0" || value === "false") {
        return false;
    }
    return def;
}

/// 把配置值归一成这个键允许的形态
function normalizeConfig(key, value) {
    switch (key) {
        case "blur":
            return Math.round(configNumber(value, BLUR_MIN, BLUR_MAX, BLUR_DEFAULT));
        case "zoom":
            return configNumber(value, ZOOM_MIN, ZOOM_MAX, ZOOM_DEFAULT);
        case "share":
            return configNumber(value, SHARE_MIN, SHARE_MAX, SHARE_DEFAULT);
        case "spin":
            return configNumber(value, SPIN_MIN, SPIN_MAX, SPIN_DEFAULT);
        case "ripple":
            return configBool(value, RIPPLE_DEFAULT);
        case "hideIcon":
            return configBool(value, HIDE_ICON_DEFAULT);
        case "autoForeground":
            return configBool(value, AUTO_FOREGROUND_DEFAULT);
        default:
            return value;
    }
}

/// 当前配置值（键不认识时返回 undefined）
function configValue(key) {
    return (key in configValues) ? configValues[key] : CONFIG_DEFAULTS[key];
}

/// 记下一个已确定的配置值
function setConfigValue(key, value) {
    configValues[key] = normalizeConfig(key, value);
    configSettled[key] = true;
}

/// 『模糊』的预模糊强度（0~30）
function configuredBlur() {
    return configValue("blur");
}

/// 放大倍数（1~3，两种模式共用）
function configuredZoom() {
    return configValue("zoom");
}

/// 图片占比（只影响『沉浸』：图片至少占屏幕这个比例）
function configuredShare() {
    return configValue("share");
}

/// 唱片转速（只影响『黑胶』：写进 UI 项的 `speed`，1 = 着色器里的基准转速）
function configuredSpin() {
    return configValue("spin");
}

/// 热浪扭曲波纹开关
function configuredRipple() {
    return configValue("ripple");
}

/// 隐藏歌曲图开关
function configuredHideIcon() {
    return configValue("hideIcon");
}

/// 自动调整前景色开关（按封面主色 + 亮暗主题推播放页的三色）
function configuredAutoForeground() {
    return configValue("autoForeground");
}

/// 把设置应用到三个背景样式
///
/// `musicxx.ui.updateEntry` 是**整体替换**（每次都取完整的 data），插件框架收到后会刷新候选；
/// 正在使用的样式立即换用新参数（封面纹理按新参数重新解码一次，『模糊』改模糊程度时
/// 就能当场看到变化；『黑胶』改转速只换 `speed`，正在画的封面下一帧就按新转速转）。
/// 自动前景色也一起推（[foregroundColorsSpec]）；它没变时由 [applyForegroundIfChanged] 拦住，
/// 不会因为"只改了模糊程度"而多推一次。
function applySettings() {
    lastForegroundSpec = foregroundSpecsSignature();
    musicxx.ui.updateEntry(BG_ITEM_NAME, blurBackgroundData());
    musicxx.ui.updateEntry(CARD_ITEM_NAME, cardBackgroundData());
    musicxx.ui.updateEntry(VINYL_ITEM_NAME, vinylBackgroundData());
}

/// 异步读配置（默认值由脚本给；所有键都读完 / 失败后再换用实际值）
///
/// 放在定时器里读：顶层同步注册阶段不能投递动作（插件框架这时还在回放注册）。
function refreshConfig() {
    const keys = Object.keys(CONFIG_DEFAULTS);
    let pending = keys.length;
    function done() {
        --pending;
        if (0 === pending) {
            // 先用状态镜像里的昼夜（主程序启动时推过一次），异步查到的值会再校正一次；
            // 顺带问一次"当前昼夜 + 封面主色"，"恢复上次的歌"时启动完就能上色
            if (null === isNightNow) {
                isNightNow = isNightFromMirror();
            }
            applySettings();
            refreshForegroundState();
        }
    }
    keys.forEach(function (key) {
        musicxx.storage.getConfig(key, CONFIG_DEFAULTS[key]).then(function (value) {
            // 用户还没改过（configSettled 里还没有这个键）时才用读回来的值。
            // 这里不能写 `false == configSettled[key]`：键不存在时它是 undefined，
            // `false == undefined` 是假，那样读回来的值永远用不上（等于只认默认值）。
            if (true != configSettled[key]) {
                setConfigValue(key, value);
            }
            done();
        }, function () {
            done();
        });
    });
}
musicxx.timer.setTimeout(refreshConfig, 200);

/// 写一个配置项：先更新脚本里记下的值（页面立刻显示新值），再异步写 config.json
function saveConfig(key, value) {
    setConfigValue(key, value);
    applySettings();
    musicxx.storage.setConfig(key, configValue(key)).then(function () {
        return null;
    }, function (err) {
        musicxx.host.log(3, "写配置失败(" + key + "): " + errText(err));
    });
}

/* ==================== 自动调整播放页前景色 ==================== */
//
// 播放页的文本、歌词、进度条与块状背景只用三种颜色（普通色 / 减淡色 / block 背景色）。
// 本插件的三种背景都是封面自己的颜色铺出来的，主题默认的那三色（浅色主题深灰、深色主题白）
// 不一定压得住画面，所以这里按封面取色的**主色**再结合亮暗主题推一套，用 UI 项的
// `foregroundColors` 交给插件框架（插件框架只在本插件的背景样式被选中时用它）。
//
// 三色怎么定：
//   1. 色相与彩色程度取自封面的**主色**（[accentColorOf]：出现最多的色点，它是灰的就看第二个），
//      换成 HSL 后一路保留色度 —— 这样文字颜色看得出是画面的主色；
//   2. 明度朝主题倾向的一侧调（白昼偏深、夜间偏浅），**从主色自己的明度出发**逐档移动，
//      取第一个够清楚的明度（[searchForegroundLightness]）：离主色最近又看得清；
//   3. 对比度按**当前模式估出的背景亮度**算（[backgroundEstimate]）；要求按 [FG_PICK_RULES]
//      逐条放宽：先"正文级对比度 + 明显的颜色"，不够就依次让一点对比度（不低于
//      [FG_CONTRAST_FLOOR]），最后才让颜色 —— 保色优先，只有给不出颜色时才退回黑 / 白；
//   4. 减淡色 = 普通色 30% 透明；块状背景色取"与普通色相反的一侧"的低饱和中间色。
//
// 昼夜从哪来：状态镜像里的 `musicxx.state.app.isNight` 只在启动时推过一次，运行期切主题
// 它不会更新，所以它**只用来当初始值**；之后的判断一律以 `musicxx.media.palette()` 返回的
// `night`（主程序当场算的）与取色钩子载荷里的 `night` 为准。两个来源混用会让配色每隔一段
// 时间来回跳（镜像里还是上一个主题的值）。

/// 当前昼夜（true = 夜间）；null = 还不知道（这时不声明前景三色，播放页用主题默认色）
let isNightNow = null;

/// 封面取色的主色（`#rrggbb`）；空串 = 还没有可用的取色结果
let foregroundBase = "";

/// 前景色的**色相**来源（`#rrggbb`）：封面里"出现最多、又看得出颜色"的那个色点
/// （见 [accentColorOf]）；空串时退回 [foregroundBase]。
let foregroundAccent = "";

/// 这条主色是哪首歌的（srcKey）：同一首歌封面一时读不到时用它判断"要不要先留着"
let foregroundBaseKey = "";

/// 封面平均色（取色时顺便算出来，只用来估背景亮度）；null = 还没有
let coverAverage = null;

/// 最近一次推给插件框架的前景三色（三个模式一起序列化）：没变就不再重登记
/// （重登记会让插件框架重新同步候选，能省则省）
let lastForegroundSpec = "";

/// 已经排了一次"稍后重推前景三色"（钩子处理器里不直接调主程序动作）
let foregroundApplyScheduled = false;

/// 最近一次查"昼夜 + 主色"的时间
let foregroundCheckedAt = 0;

/// 颜色文本 → `{r, g, b}`（支持 `#rrggbb` 与 `#aarrggbb`；解析不出来返回 null）
function parseColorText(text) {
    if (typeof text !== "string") {
        return null;
    }
    let value = text.trim();
    if (value.charAt(0) === "#") {
        value = value.slice(1);
    }
    if (8 === value.length) {
        value = value.slice(2); // 去掉透明度：前景色只关心色相与明暗
    }
    if (6 !== value.length || false == /^[0-9a-fA-F]{6}$/.test(value)) {
        return null;
    }
    return {
        r: parseInt(value.slice(0, 2), 16),
        g: parseInt(value.slice(2, 4), 16),
        b: parseInt(value.slice(4, 6), 16),
    };
}

/// 归一成 `#rrggbb`（解析不出来返回空串）
function normalizeHexColor(text) {
    const rgb = parseColorText(text);
    return rgb ? hexOf(rgb) : "";
}

/// `{r, g, b}` → `{h(0..360), s(0..1), l(0..1)}`
function rgbToHsl(rgb) {
    const r = rgb.r / 255;
    const g = rgb.g / 255;
    const b = rgb.b / 255;
    const max = Math.max(r, g, b);
    const min = Math.min(r, g, b);
    const l = (max + min) * 0.5;
    const span = max - min;
    if (0 === span) {
        return { h: 0, s: 0, l: l };
    }
    const s = (l > 0.5) ? (span / (2 - max - min)) : (span / (max + min));
    let h;
    if (max === r) {
        h = (g - b) / span + (g < b ? 6 : 0);
    } else if (max === g) {
        h = (b - r) / span + 2;
    } else {
        h = (r - g) / span + 4;
    }
    return { h: h * 60, s: s, l: l };
}

/// `{h, s, l}` → `{r, g, b}`（s / l 超出 0..1 时夹到边界）
function hslToRgb(hsl) {
    const l = Math.max(0, Math.min(1, hsl.l));
    const s = Math.max(0, Math.min(1, hsl.s));
    if (0 === s) {
        const gray = Math.round(l * 255);
        return { r: gray, g: gray, b: gray };
    }
    let h = hsl.h / 360;
    h = h - Math.floor(h);
    const q = (l < 0.5) ? (l * (1 + s)) : (l + s - l * s);
    const p = 2 * l - q;
    function channel(t) {
        let x = t;
        if (x < 0) {
            x += 1;
        }
        if (x > 1) {
            x -= 1;
        }
        if (x < 1 / 6) {
            return p + (q - p) * 6 * x;
        }
        if (x < 1 / 2) {
            return q;
        }
        if (x < 2 / 3) {
            return p + (q - p) * (2 / 3 - x) * 6;
        }
        return p;
    }
    return {
        r: Math.round(channel(h + 1 / 3) * 255),
        g: Math.round(channel(h) * 255),
        b: Math.round(channel(h - 1 / 3) * 255),
    };
}

/// 色点 + 透明度 → `#aarrggbb`（插件框架接受 `#rrggbb` 与 `#aarrggbb` 两种写法）
function hexWithAlpha(rgb, alpha) {
    const a = Math.round(Math.max(0, Math.min(1, alpha)) * 255);
    const text = a.toString(16);
    return "#" + (text.length < 2 ? "0" + text : text) + hexOf(rgb).slice(1);
}

/// 两个色点按 t 混合（0 = 全 a，1 = 全 b）
function mixRgb(a, b, t) {
    return {
        r: Math.round(a.r + (b.r - a.r) * t),
        g: Math.round(a.g + (b.g - a.g) * t),
        b: Math.round(a.b + (b.b - a.b) * t),
    };
}

/// 整体乘一个系数（变亮 / 变暗，超过 255 的部分夹住）
function scaleRgb(rgb, k) {
    function clamp(value) {
        return Math.max(0, Math.min(255, Math.round(value)));
    }
    return { r: clamp(rgb.r * k), g: clamp(rgb.g * k), b: clamp(rgb.b * k) };
}

/// 相对亮度（WCAG：sRGB 通道先线性化再加权），用来算对比度
function relativeLuminance(rgb) {
    function channel(value) {
        const x = value / 255;
        return (x <= 0.03928) ? (x / 12.92) : Math.pow((x + 0.055) / 1.055, 2.4);
    }
    return 0.2126 * channel(rgb.r) + 0.7152 * channel(rgb.g) + 0.0722 * channel(rgb.b);
}

/// 两色的对比度（1..21，越大越清楚）
function contrastRatio(a, b) {
    const la = relativeLuminance(a);
    const lb = relativeLuminance(b);
    const hi = Math.max(la, lb);
    const lo = Math.min(la, lb);
    return (hi + 0.05) / (lo + 0.05);
}

/// 黑胶盘面的近黑色（与 vinyl.frag 的 kDiscBase 同一档）
const FG_DISC_BASE = { r: 12, g: 13, b: 15 };

/// 各模式的背景估色：只用来判断"文字压在什么颜色上"，所以取主要铺满画面的那个颜色
///
/// - 『模糊』：整屏就是（预模糊的）封面本身，夜间着色器再统一压到 0.74；
/// - 『沉浸』：大部分是原图（夜间不压暗），整幅再叠一层主题遮罩（白昼提亮、夜间压暗）；
/// - 『黑胶』：文字大多压在近黑的盘面上，底色只在四周，所以按盘面算。
function backgroundEstimate(mode) {
    const night = (true === isNightNow);
    const avg = coverAverage || parseColorText(foregroundBase) ||
        { r: 34, g: 38, b: 46 };
    if (MODE_VINYL === mode) {
        return mixRgb(FG_DISC_BASE, avg, 0.12);
    }
    if (MODE_CARD === mode) {
        return night
            ? mixRgb(avg, { r: 0, g: 0, b: 0 }, 0.34)
            : mixRgb(avg, { r: 255, g: 255, b: 255 }, 0.22);
    }
    return night ? scaleRgb(avg, 0.74) : avg;
}

/// 一个色点的"彩色程度"（HSL 里的 max - min，0 = 灰）
///
/// 用它而不是 HSL 的 `s`：`s` 会随着明度一起变，判断"像不像原来的主色"要看色度。
function chromaOf(hsl) {
    return (1 - Math.abs(2 * hsl.l - 1)) * hsl.s;
}

/// 用色相 + 色度 + 明度构造颜色：HSL 的 `s` 按明度换算，**色度保持不变**
///
/// 明度往两端走时同样的 `s` 会越来越没颜色，这里反过来算 —— 保住"像主色"的那个量。
/// 明度太极端时 s 会顶到 1（再往上没有空间了），颜色也就不可避免地变淡。
function hslWithChroma(hue, chromaValue, lightness) {
    const l = Math.max(0.02, Math.min(0.98, lightness));
    const span = 1 - Math.abs(2 * l - 1);
    const s = (span <= 0.0001) ? 0 : Math.max(0, Math.min(1, chromaValue / span));
    return hslToRgb({ h: hue, s: s, l: l });
}

/// 从一个明度出发、朝一个方向逐档调整，找"第一个够清楚"的明度
///
/// 起点是**主色自己的明度**（夹进 [FG_LIGHTNESS_MIN]..[FG_LIGHTNESS_MAX]）：原色就够清楚时
/// 直接用它，否则每档 [FG_LIGHTNESS_STEP] 往指定方向走一步 —— 得到的永远是"离主色最近、又
/// 满足 [minContrast]"的那一档；这个方向达不到时返回该方向上对比度最高的一个（可能仍不达标，
/// 由调用方决定要不要用）。
function searchForegroundLightness(hue, chromaValue, sourceLightness, dir, bg, minContrast) {
    const start = Math.max(FG_LIGHTNESS_MIN,
        Math.min(FG_LIGHTNESS_MAX, sourceLightness));
    const maxSteps = Math.ceil(
        (FG_LIGHTNESS_MAX - FG_LIGHTNESS_MIN) / FG_LIGHTNESS_STEP) + 1;
    let best = null;
    for (let step = 0; step <= maxSteps; ++step) {
        const lightness = start + dir * step * FG_LIGHTNESS_STEP;
        if (lightness < FG_LIGHTNESS_MIN || lightness > FG_LIGHTNESS_MAX) {
            break;
        }
        const rgb = hslWithChroma(hue, chromaValue, lightness);
        const ratio = contrastRatio(rgb, bg);
        const candidate = { rgb: rgb, ratio: ratio, lightness: lightness };
        if (null === best || ratio > best.ratio) {
            best = candidate;
        }
        if (ratio >= minContrast) {
            return candidate;
        }
    }
    return best;
}

/// 按封面主色 + 亮暗主题挑普通色
///
/// 做法是"**从主色本身出发**"：保留它的色相与彩色程度（色度），只把明度朝主题倾向的一侧
/// 逐档移动，取**第一个**满足要求的明度（见 [searchForegroundLightness]）—— 所以文字通常是
/// "主色的加深/变亮版"，而不是一律黑或白。
///
/// 要求按 [FG_PICK_RULES] 逐条放宽（先争取高对比 + 明显的颜色，再让对比度、最后让颜色），
/// 每条要求里先试主题倾向的方向、再从高到低试 [FG_CHROMA_LEVELS]；全都给不出颜色时才退回
/// 高对比的黑 / 白（表里最后一条）。
///
/// 返回 `{rgb, dark, ratio}`：`dark` 表示普通色偏暗（块状背景色要反过来配），
/// `ratio` 是它与背景的对比度。
function pickForegroundMain(hsl, mode) {
    const night = (true === isNightNow);
    const bg = backgroundEstimate(mode);
    const chromaTarget = Math.min(FG_CHROMA_MAX, chromaOf(hsl) * FG_CHROMA_BOOST);
    // 方向按主题倾向排：白昼先往暗、夜间先往亮；第二个是"保色"不成时的另一侧
    const directions = night ? [1, -1] : [-1, 1];
    let best = null;
    for (let r = 0; r < FG_PICK_RULES.length; ++r) {
        const rule = FG_PICK_RULES[r];
        for (let d = 0; d < directions.length; ++d) {
            for (let c = 0; c < FG_CHROMA_LEVELS.length; ++c) {
                const level = FG_CHROMA_LEVELS[c];
                if (level < rule.chroma) {
                    continue;
                }
                const found = searchForegroundLightness(
                    hsl.h, chromaTarget * level, hsl.l, directions[d], bg, rule.contrast);
                if (null === found) {
                    continue;
                }
                const candidate = {
                    rgb: found.rgb,
                    dark: found.lightness < 0.5,
                    ratio: found.ratio,
                };
                if (found.ratio >= rule.contrast) {
                    return candidate; // 这一条要求满足了：它就是最合适的
                }
                if (null === best || candidate.ratio > best.ratio) {
                    best = candidate; // 兜底：所有要求都不满足时取对比度最高的
                }
            }
        }
    }
    return best;
}

/// 算一个模式的前景：普通色 / 减淡色 / 块状背景色，外加挑色时的中间结果
///
/// [hsl] 是色相来源（封面的主色）的 HSL；`colors` 就是交给插件框架的那三色，
/// `dark` / `ratio` 是状态行要报的"深色字还是浅色字 / 对比度多少"。
function computeForegroundOf(hsl, mode) {
    const picked = pickForegroundMain(hsl, mode);
    const block = hslToRgb({
        h: hsl.h,
        s: Math.min(hsl.s, FG_BLOCK_MAX_SATURATION),
        l: picked.dark ? FG_BLOCK_LIGHT : FG_BLOCK_DARK,
    });
    return {
        colors: {
            main: hexWithAlpha(picked.rgb, 1),
            cross: hexWithAlpha(picked.rgb, FG_CROSS_ALPHA),
            block: hexWithAlpha(block, FG_BLOCK_ALPHA),
        },
        dark: picked.dark,
        ratio: picked.ratio,
    };
}

/// 已经算好的一组前景（按输入缓存）
///
/// 一次重算要按"方向 × 彩色程度档 × 明度档"逐档试，是这里最费时的东西；而每次重推样式、
/// 每次刷新页面、状态行都要问一遍 —— 输入（昼夜、开关、色相来源、平均色）没变就直接复用。
let foregroundCacheKey = "";
let foregroundCache = null;

/// 缓存键：昼夜 + 开关 + 色相来源 + 平均色（平均色取整到 4，免得一点点抖动就重算）
function foregroundCacheKeyOf(night) {
    const avg = coverAverage;
    const avgText = avg
        ? (Math.round(avg.r / 4) + "," + Math.round(avg.g / 4) + "," + Math.round(avg.b / 4))
        : "-";
    return (night ? "夜" : "昼") + "|" + (configuredAutoForeground() ? "开" : "关") + "|" +
        (foregroundAccent || foregroundBase) + "|" + avgText;
}

/// 当前的三组前景（blur / card / vinyl）；返回 null 表示"这次不指定"
///
/// null 的三种情况：开关关着、昼夜还不知道、或者还没有取色结果 —— 这时插件框架用它自己的
/// 默认前景色。
function foregroundSpecs() {
    if (false == configuredAutoForeground() || null === isNightNow) {
        return null;
    }
    const accent = parseColorText(foregroundAccent) || parseColorText(foregroundBase);
    if (!accent) {
        return null;
    }
    const key = foregroundCacheKeyOf(true === isNightNow);
    if (key === foregroundCacheKey && null !== foregroundCache) {
        return foregroundCache;
    }
    const hsl = rgbToHsl(accent);
    const specs = {
        blur: computeForegroundOf(hsl, MODE_BLUR),
        card: computeForegroundOf(hsl, MODE_CARD),
        vinyl: computeForegroundOf(hsl, MODE_VINYL),
    };
    foregroundCacheKey = key;
    foregroundCache = specs;
    return specs;
}

/// 某个模式的前景三色（没有就返回 null）
function foregroundColorsSpec(mode) {
    const specs = foregroundSpecs();
    if (!specs) {
        return null;
    }
    if (MODE_CARD === mode) {
        return specs.card.colors;
    }
    if (MODE_VINYL === mode) {
        return specs.vinyl.colors;
    }
    return specs.blur.colors;
}

/// 三个模式的前景三色一起序列化（判断"要不要重推"用）
function foregroundSpecsSignature() {
    const specs = foregroundSpecs();
    if (!specs) {
        return "null";
    }
    return JSON.stringify([
        specs.blur.colors,
        specs.card.colors,
        specs.vinyl.colors,
    ]);
}

/// 当前歌曲的身份（换歌判断用；镜像里没有就是空串）
function currentSongKey() {
    const song = musicxx.state.get("musicxx.state.song");
    if (song && typeof song.srcKey === "string") {
        return song.srcKey;
    }
    return "";
}

/// 记下封面取色的主色、平均色与"色相来源"（取色算完 / 当前歌已有分析结果时调用）
///
/// [key] 是这套颜色属于哪首歌（srcKey）：报上来的歌和状态镜像里正在放的不是同一首、
/// 或者同一首歌的结果晚到了（切歌了），都不写进来 —— 颜色错配比"晚一点上色"更难受。
/// [accent] 是前景色的色相来源（见 [accentColorOf]），空串表示没有（退回主色）。
/// 只改脚本里的状态，重推排到本次之后（见 [applyForegroundLater]）—— 这个函数会从
/// 取色钩子的处理器里被调用，那里不能做重活。
function setForegroundBase(text, key, average, accent) {
    if (key) {
        const now = currentSongKey();
        if (now && now !== key) {
            return false; // 上一首的取色结果：丢掉
        }
    }
    let changed = false;
    if (average && isFinite(average.r) && isFinite(average.g) && isFinite(average.b)) {
        coverAverage = average;
        changed = true;
    }
    const accentText = normalizeHexColor(accent);
    if (accentText !== foregroundAccent) {
        foregroundAccent = accentText;
        changed = true;
    }
    const normalized = normalizeHexColor(text);
    if (normalized && normalized !== foregroundBase) {
        foregroundBase = normalized;
        foregroundBaseKey = key || foregroundBaseKey;
        changed = true;
    }
    if (changed) {
        applyForegroundLater();
    }
    return changed;
}

/// 当前歌曲没有封面时的处理
///
/// 是**另一首歌**（或不知道是哪首）就丢掉记下的颜色 —— 不然会用上一首的颜色调前景；
/// 还是同一首歌就先留着：封面可能只是还没准备好，等它到了再换。
function onCoverMissing(key) {
    if ("" === foregroundBase && "" === foregroundAccent) {
        return false;
    }
    const sameSong = key && foregroundBaseKey && key === foregroundBaseKey;
    if (sameSong) {
        return false;
    }
    foregroundBase = "";
    foregroundAccent = "";
    foregroundBaseKey = "";
    coverAverage = null;
    applyForegroundLater();
    return true;
}

/// 前景三色有变化时重推三个背景样式（没变就不重登记）
function applyForegroundIfChanged() {
    if (foregroundSpecsSignature() === lastForegroundSpec) {
        return false;
    }
    applySettings();
    return true;
}

/// 把"重推前景三色"排到本轮之后再执行
///
/// 取色钩子是主程序等着回答的调用点（处理器里直接调 `musicxx.ui.updateEntry` 会打断调用方），
/// 所以统一延后到下一次定时器回调里做。
function applyForegroundLater() {
    if (foregroundApplyScheduled) {
        return;
    }
    foregroundApplyScheduled = true;
    musicxx.timer.setTimeout(function () {
        foregroundApplyScheduled = false;
        applyForegroundIfChanged();
    }, 0);
}

/// 从状态镜像里读昼夜（主程序启动时推过一次；读不到返回 null）
///
/// 只在还没有值时用它当初始值：运行期切了主题镜像不会更新（主程序只在启动时推这个键），
/// 拿它当"当前值"会和当场查询的结果打架，配色就会隔一会儿跳一次。
function isNightFromMirror() {
    const app = musicxx.state.get("musicxx.state.app");
    if (app && typeof app.isNight === "boolean") {
        return app.isNight;
    }
    const slot = slotOf(BACKGROUND_SLOT);
    if (slot && typeof slot.night === "boolean") {
        return slot.night;
    }
    return null;
}

/// 记下当场问到的昼夜；变了就排一次重推
function applyNightState(night) {
    if (typeof night !== "boolean" || night === isNightNow) {
        return false;
    }
    isNightNow = night;
    applyForegroundLater();
    return true;
}

/// 查一次"当前昼夜 + 封面主色"（主程序当场算的值；不强制刷新时只是读现成的分析结果）
///
/// 主色优先用本插件自己的取色结果（[setForegroundBase] 已经写过）；还没有、或者已经换成
/// 另一首歌时用主程序现有分析里的主色，这样"插件刚启用、主程序已经分析过封面"时也能立刻上色。
function refreshForegroundState() {
    foregroundCheckedAt = Date.now();
    return musicxx.media.palette().then(function (result) {
        if (!result || result.ok !== true) {
            return false;
        }
        applyNightState(result.night);
        const raw = (result.raw && typeof result.raw === "object") ? result.raw : {};
        const key = (typeof result.srcKey === "string") ? result.srcKey : "";
        // 主程序分析里的"主色候选"（`dominant[0]` 是它选出的主色）：当色相来源用
        const dominantList = (raw.dominant instanceof Array) ? raw.dominant : [];
        const accent = (dominantList.length > 0) ? dominantList[0] : "";
        if (result.hasCover === true) {
            // 还没有底色，或者这条底色是上一首的（主程序已经在放另一首了）：用主程序现成的分析结果
            if ("" === foregroundBase || (key && key !== foregroundBaseKey)) {
                setForegroundBase(raw.main, key, null, accent);
            }
        }
        applyForegroundLater();
        return true;
    }, function (err) {
        musicxx.host.log(3, "读取封面配色失败: " + errText(err));
        return false;
    });
}

/// 到点就查一次（每秒的跟随检查里顺带做）
///
/// 没有值时先用状态镜像里的昼夜兜一下（主程序启动时推的那份就是当时的真实值），
/// 之后一律以当场查询为准 —— 见 [isNightFromMirror] 的说明。
///
/// [force] 写成 `true != force` 判断（不是 `false == force`）：定时器回调不带参数，
/// 那时的 force 是 undefined，用 `false == force` 会把"没传参数"当成"强制刷新"。
function refreshForegroundIfDue(force) {
    if (false == configuredAutoForeground()) {
        return;
    }
    if (null === isNightNow) {
        const seed = isNightFromMirror();
        if (null !== seed) {
            isNightNow = seed;
            applyForegroundLater();
        }
    }
    const now = Date.now();
    const interval = backgroundIsMine() ? FG_REFRESH_ACTIVE_MS : FG_REFRESH_IDLE_MS;
    if (true != force && now - foregroundCheckedAt < interval) {
        return;
    }
    refreshForegroundState();
}

/// 自动前景色的状态文本（设置页 / 说明页的状态行）
///
/// 顺便报出当前是深色字还是浅色字、与估出的背景对比度多少 —— 换歌、切主题、换模式后
/// 颜色为什么变，看这一行就够了。
function foregroundStateText() {
    if (false == configuredAutoForeground()) {
        return "已关闭";
    }
    if (null === isNightNow) {
        return "读取中";
    }
    if (false == musicxx.hooks.has("musicxx.media.palette.provide")) {
        return "取色未注册";
    }
    const mode = currentMode();
    const useMode = (MODE_BLUR === mode || MODE_CARD === mode || MODE_VINYL === mode)
        ? mode
        : MODE_BLUR;
    const specs = foregroundSpecs();
    if (!specs) {
        return "等待封面取色";
    }
    const of = (MODE_CARD === useMode) ? specs.card
        : ((MODE_VINYL === useMode) ? specs.vinyl : specs.blur);
    return (true === isNightNow ? "夜间" : "白昼") + " · " +
        (of.dark ? "深色字" : "浅色字") + " · 对比度 " +
        (Math.round(of.ratio * 10) / 10) + " · " + of.colors.main;
}

/* ==================== UI 项声明 ==================== */

/// 『模糊』的完整声明
///
/// 封面纹理每帧绑定到 `uCover`，`(纹理宽, 纹理高, 原图宽高比, 是否有封面)` 写进 `uCoverInfo`；
/// 两个绘制色跟着封面配色走（取色钩子提供的算法），没有分析结果时用固定色。
function blurBackgroundData() {
    return {
        title: MODE_TITLES[MODE_BLUR],
        depict: MODE_DEPICTS[MODE_BLUR],
        shader: { bundle: "shader/heat.shaderbundle" },
        cover: {
            texture: COVER_TEXTURE,
            info: COVER_INFO,
            size: BLUR_COVER_SIZE,
            blur: configuredBlur(),
        },
        args: {
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
        },
        // 自动调整前景色：按封面主色 + 亮暗主题推的播放页三色，按这个模式估背景后挑明暗
        // （开关关着 / 还不知道昼夜 / 没有取色结果时不声明，插件框架用主题默认色）
        foregroundColors: foregroundColorsSpec(MODE_BLUR) || undefined,
        speed: 1,
        maxFps: 24,
        // 模糊画面不需要满分辨率：降采样后由插件框架放大，省一半以上的像素
        resolutionScale: 0.6,
        animate: true,
    };
}

/// 『沉浸』的完整声明
///
/// 与『模糊』的区别：清晰封面（`blur: 0`，尺寸给大一点，卡片里是原图）+ 封面主色做的
/// 渐变底 + 按主题叠的亮暗遮罩；图片占比（`uShare`）决定图片至少占屏幕多少（越大图片越多、
/// 渐变底越少），取景与卡片贴边都由着色器按屏幕方向决定。
function cardBackgroundData() {
    return {
        title: MODE_TITLES[MODE_CARD],
        depict: MODE_DEPICTS[MODE_CARD],
        shader: { bundle: "shader/card.shaderbundle" },
        cover: {
            texture: COVER_TEXTURE,
            info: COVER_INFO,
            size: CARD_COVER_SIZE,
            blur: 0,
        },
        args: {
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
            // 图片占比（只在『沉浸』里用：图片至少占屏幕的这个比例）
            uShare: { kind: "const", value: configuredShare() },
            uRipple: { kind: "const", value: configuredRipple() ? 1 : 0 },
        },
        // 自动调整前景色：按封面主色 + 亮暗主题推的播放页三色（见 [foregroundColorsSpec]）
        foregroundColors: foregroundColorsSpec(MODE_CARD) || undefined,
        speed: 1,
        // 这一种基本是静态画面（只有热浪波纹在动），帧率给低一档就够
        maxFps: 16,
        // 卡片里是清晰封面：不降采样
        resolutionScale: 1,
        animate: true,
    };
}

/// 『黑胶』的完整声明
///
/// 与另两种的区别：封面**正方形圆裁**（`square: true`，盘心圆内正好是它的内切圆）+
/// 盘心旋转 —— 转速由 `speed` 决定（写进 `uParams.w`，着色器按它推进封面转角，
/// 所以改转速不用重新选中这个样式）；底色与唱臂色都从封面配色来（取色钩子提供的算法）。
function vinylBackgroundData() {
    return {
        title: MODE_TITLES[MODE_VINYL],
        depict: MODE_DEPICTS[MODE_VINYL],
        shader: { bundle: "shader/vinyl.shaderbundle" },
        cover: {
            texture: COVER_TEXTURE,
            info: COVER_INFO,
            size: VINYL_COVER_SIZE,
            blur: 0,
            // 盘心是圆裁的封面：正方形纹理的内切圆正好铺满它
            square: true,
        },
        args: {
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
        },
        // 自动调整前景色：按封面主色 + 亮暗主题推的播放页三色
        // （黑胶的盘面近黑，估出来的背景偏暗，所以这里通常给浅色字）
        foregroundColors: foregroundColorsSpec(MODE_VINYL) || undefined,
        // 转速：着色器里的基准转速（30 秒一圈）再乘这个倍率
        speed: configuredSpin(),
        maxFps: 24,
        // 盘心的封面要清晰：不降采样
        resolutionScale: 1,
        animate: true,
    };
}

/// 接管歌曲图的 UI 项内容：不显示内容、保留占位
function iconData() {
    return {
        title: "隐藏歌曲图（" + PLUGIN_TITLE + " 配套）",
        depict: "接管播放页中间的歌曲图：保留占位、不画内容；背景换回其它样式时自动交还",
        mode: "none",
        keepSpace: true,
    };
}

/// 注册三个背景样式（同一个槽位的三个候选，注册顺序决定设置列表里的先后）
musicxx.ui.registerEntry({
    name: BG_ITEM_NAME,
    type: "playing.background",
    order: 40,
    data: blurBackgroundData(),
});

musicxx.ui.registerEntry({
    name: CARD_ITEM_NAME,
    type: "playing.background",
    order: 41,
    data: cardBackgroundData(),
});

musicxx.ui.registerEntry({
    name: VINYL_ITEM_NAME,
    type: "playing.background",
    order: 42,
    data: vinylBackgroundData(),
});

musicxx.ui.registerEntry({
    name: ICON_ITEM_NAME,
    type: "playing.icon",
    order: 40,
    data: iconData(),
});

musicxx.ui.registerEntry({
    name: HOME_ITEM_NAME,
    type: "home.entry",
    order: 130,
    data: {
        title: PLUGIN_TITLE,
        subtitle: "playing_bg_image：三种播放页背景模式 + 封面取色 + 自动前景色",
        // 图标名放在 data 里（`registerEntry` 只在没有 data 时才把顶层字段并进去）；
        // 图标是插件自己带的 svg：相对插件目录的路径（resource/svg/ 只是推荐位置，
        // 插件框架不接受自己的图标名，见 docs/plugin-ui.md §1.5）
        icon: "resource/svg/vinyl.svg",
        action: { kind: "route", route: "ext://" + PLUGIN_ID + "/" + HOME_VIEW_ID },
    },
});

/* ==================== 封面取色（钩子 musicxx.media.palette.provide） ==================== */

/// base64 字符表（QuickJS 里没有 atob，自己解码：4 个字符 → 3 字节）
const BASE64_CHARS =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// 字符码 → 6 bit 值（非法字符与空白返回 -1）
const BASE64_LOOKUP = (function () {
    let table = [];
    for (let i = 0; i < 128; ++i) {
        table[i] = -1;
    }
    for (let i = 0; i < BASE64_CHARS.length; ++i) {
        table[BASE64_CHARS.charCodeAt(i)] = i;
    }
    return table;
})();

/// base64 → 字节数组（普通数组：只有 16 KiB 量级，够用）
function base64ToBytes(text) {
    const bytes = [];
    let buffer = 0;
    let bits = 0;
    for (let i = 0; i < text.length; ++i) {
        const code = text.charCodeAt(i);
        if (code === 61) {
            break; // '='：数据结束
        }
        const value = code < 128 ? BASE64_LOOKUP[code] : -1;
        if (value < 0) {
            continue; // 换行 / 空白
        }
        buffer = (buffer << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            bytes.push((buffer >> bits) & 0xff);
        }
    }
    return bytes;
}

/// 一个色点的亮度（0..1，人眼权重）
function luminanceOf(r, g, b) {
    return (r * 0.299 + g * 0.587 + b * 0.114) / 255;
}

/// 与白色混合（amount = 0 不变，1 = 全白）
function lighten(rgb, amount) {
    return {
        r: Math.round(rgb.r + (255 - rgb.r) * amount),
        g: Math.round(rgb.g + (255 - rgb.g) * amount),
        b: Math.round(rgb.b + (255 - rgb.b) * amount),
    };
}

/// 与黑色混合（amount = 0 不变，1 = 全黑）
function darken(rgb, amount) {
    return {
        r: Math.round(rgb.r * (1 - amount)),
        g: Math.round(rgb.g * (1 - amount)),
        b: Math.round(rgb.b * (1 - amount)),
    };
}

/// 色点 → `#rrggbb`
function hexOf(rgb) {
    function part(value) {
        const text = Math.max(0, Math.min(255, value)).toString(16);
        return text.length < 2 ? "0" + text : text;
    }
    return "#" + part(rgb.r) + part(rgb.g) + part(rgb.b);
}

/// 统计 RGBA 字节里出现次数最多的色点（简单算法：4 bit/通道量化 + 逐像素计数）
///
/// 最直接的做法：每个通道取高 4 位当桶号（4096 个桶），桶里累加真实通道值，
/// 取计数最多的前 [PALETTE_TOP_COLORS] 个桶，代表色取桶内平均 —— 比"每像素聚类"便宜得多，
/// 对"取主要配色"这个用途足够了。
///
/// 顺便算出**整幅的平均色**（`average`）：自动前景色要用它估背景亮度 —— 封面本身
/// 就是画面时（『模糊』），平均色比"最常见的那几个色点"更接近眼睛看到的底。
function topColorsOfRgba(bytes) {
    const bucketCount = 4096;
    const counts = [];
    const sumR = [];
    const sumG = [];
    const sumB = [];
    for (let i = 0; i < bucketCount; ++i) {
        counts[i] = 0;
        sumR[i] = 0;
        sumG[i] = 0;
        sumB[i] = 0;
    }
    let totalCount = 0;
    let totalR = 0;
    let totalG = 0;
    let totalB = 0;
    for (let i = 0; i + 3 < bytes.length; i += 4) {
        const a = bytes[i + 3];
        if (a < 40) {
            continue; // 基本透明的不统计
        }
        const r = bytes[i];
        const g = bytes[i + 1];
        const b = bytes[i + 2];
        const key = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
        counts[key] += 1;
        sumR[key] += r;
        sumG[key] += g;
        sumB[key] += b;
        totalCount += 1;
        totalR += r;
        totalG += g;
        totalB += b;
    }
    const picked = [];
    for (let n = 0; n < PALETTE_TOP_COLORS; ++n) {
        let best = -1;
        let bestCount = 0;
        for (let key = 0; key < bucketCount; ++key) {
            if (counts[key] > bestCount) {
                bestCount = counts[key];
                best = key;
            }
        }
        if (best < 0 || bestCount === 0) {
            break;
        }
        picked.push({
            r: Math.round(sumR[best] / bestCount),
            g: Math.round(sumG[best] / bestCount),
            b: Math.round(sumB[best] / bestCount),
        });
        counts[best] = 0; // 选过的桶清零：不重复取同一个色
    }
    const average = (totalCount > 0)
        ? {
            r: Math.round(totalR / totalCount),
            g: Math.round(totalG / totalCount),
            b: Math.round(totalB / totalCount),
        }
        : null;
    return { colors: picked, average: average };
}

/// 取到的色点数量、取色尺寸与"取几个色点"
const PALETTE_COVER_SIZE = 64;
const PALETTE_TOP_COLORS = 4;

/// 取到的一组色点 → 插件框架封面取色的 5 个字段（按亮度从亮到暗映射）
///
/// 数量不足时按已有的复用（只求"这套颜色来自封面"）。这里同时做一点调整：
/// 最亮的提亮一点、最暗的压暗一点，让内置背景与插件背景都有明暗层次可用 ——
/// 『沉浸』的渐变底（`musicxx.icon.main` → `musicxx.icon.dark`）用的就是这两个。
function paletteFromTopColors(picked) {
    if (!picked || picked.length === 0) {
        return null;
    }
    const sorted = picked.slice().sort(function (a, b) {
        return luminanceOf(b.r, b.g, b.b) - luminanceOf(a.r, a.g, a.b);
    });
    function at(index) {
        return sorted[Math.min(index, sorted.length - 1)];
    }
    const light = at(0);
    const second = at(1);
    const third = at(Math.max(0, sorted.length - 2));
    const dark = at(sorted.length - 1);
    return {
        light: hexOf(lighten(light, 0.16)),
        lightMuted: hexOf(light),
        main: hexOf(second),
        dark: hexOf(third),
        darkMuted: hexOf(darken(dark, 0.24)),
    };
}

/// 前景色的色相从哪来：**出现最多的色点**（`picked[0]`），它是灰的就看第二个
///
/// 为什么不用 [paletteFromTopColors] 里的 `main`：那个是"按亮度排序后的第二个色点"，
/// 跟"画面主色"没什么关系；而"出现最多但近灰"的色点（白底封面）也不该当色相来源 ——
/// 这时看第二个色点，彩色图案（出现次数通常排第二）才有机会被用上。
function accentColorOf(picked) {
    if (!picked || picked.length === 0) {
        return "";
    }
    const first = picked[0];
    const second = (picked.length > 1) ? picked[1] : null;
    if (chromaOf(rgbToHsl(first)) >= FG_ACCENT_MIN_CHROMA) {
        return hexOf(first);
    }
    if (second && chromaOf(rgbToHsl(second)) >= FG_ACCENT_MIN_CHROMA * 2) {
        return hexOf(second);
    }
    return hexOf(first);
}

/// 取当前封面的主要配色（读 `musicxx.media.cover` 的 RGBA 字节）
///
/// 返回 `{colors, average, dominant}`：`colors` 是写给插件框架的那 5 个字段，`average` 是整幅
/// 平均色（自动前景色估背景用），`dominant` 是前景色的色相来源（见 [accentColorOf]）。
function paletteFromCurrentCover() {
    return musicxx.media.cover({ size: PALETTE_COVER_SIZE, format: "rgba" })
        .then(function (result) {
            if (!result || result.ok !== true || typeof result.data !== "string") {
                return null;
            }
            const stats = topColorsOfRgba(base64ToBytes(result.data));
            const colors = paletteFromTopColors(stats.colors);
            if (!colors) {
                return null;
            }
            return {
                colors: colors,
                average: stats.average,
                dominant: accentColorOf(stats.colors),
            };
        }, function (err) {
            musicxx.host.log(3, "读取封面失败: " + errText(err));
            return null;
        });
}

/// 取色缓存（srcKey → `{colors, average, dominant}`）：同一首歌只算一次
let paletteCache = {};

/// 取色缓存最多留这么多首歌
///
/// 它是"最近放过的歌"的缓存，不是长期存储：一首歌一条（5 个颜色文本），不设上限时
/// 长时间播放会一直涨。满了就丢最早的一条（字符串键按插入顺序排列），要立刻清空用
/// 设置页的「清空取色缓存」。
const PALETTE_CACHE_LIMIT = 64;

/// 写进取色缓存，超出上限时丢掉最早的一条
function cachePalette(key, entry) {
    paletteCache[key] = entry;
    const keys = Object.keys(paletteCache);
    while (keys.length > PALETTE_CACHE_LIMIT) {
        delete paletteCache[keys.shift()];
    }
}

/// 正在计算的取色任务（srcKey → Promise）：同一首歌不重复计算
let paletteTasks = {};

let paletteCalls = 0;
let paletteHits = 0;
let paletteReady = 0;

/// 异步算好封面配色，算完**主动写回**（`musicxx.media.palette.set`）
///
/// 为什么要这样绕一圈：取色钩子是"应用要颜色"的调用点，处理器里 `await
/// musicxx.media.cover(...)` 会让应用一直等到超时 —— 用户看到的是**切歌 / 切换背景样式
/// 卡几秒**（实测：启用本插件后，用别的插件的背景样式也卡，禁用插件就正常）。
/// 所以钩子处理器只读缓存（命中就当场给），没命中就返回 null（应用用它自己的快速取色），
/// 真正的计算与写回都在这里异步做。
function warmPalette(key) {
    const cacheKey = key || "(no-key)";
    if (paletteTasks[cacheKey]) {
        return paletteTasks[cacheKey];
    }
    const task = paletteFromCurrentCover().then(function (result) {
        paletteTasks[cacheKey] = null;
        if (!result) {
            return null;
        }
        const colors = result.colors;
        if (key) {
            cachePalette(key, result);
        }
        // 顺便按这套颜色更新"自动调整前景色"（开关关着 / 还不知道昼夜时它什么都不做）
        setForegroundBase(colors.main, key, result.average, result.dominant);
        ++paletteReady;
        return musicxx.media.setPalette(colors).then(function () {
            musicxx.host.log(2, "封面取色已写回: " + JSON.stringify(colors));
            return colors;
        }, function (err) {
            musicxx.host.log(3, "写回封面取色失败: " + errText(err));
            return colors;
        });
    }, function (err) {
        paletteTasks[cacheKey] = null;
        musicxx.host.log(3, "封面取色失败: " + errText(err));
        return null;
    });
    paletteTasks[cacheKey] = task;
    return task;
}

/// 取色钩子：应用要为当前歌曲封面取色时问一次
///
/// - 命中缓存 → **同步返回**这套颜色（应用当场用上，跳过它自己的分析）；
/// - 没命中 → 返回 null（应用走它自己的快速取色），同时后台算好、算完用
///   `musicxx.media.palette.set` 写回（颜色稍后会被插件的算法替换）。
///
/// **处理器绝不返回 Promise**：见 [warmPalette] 的说明。
musicxx.hooks.register("musicxx.media.palette.provide", function (payload) {
    // 载荷里的 night 是当场算的：主题切了状态镜像不会更新，这里顺手校正一次
    // （处理器里不直接调主程序动作，排到本次之后重推前景三色）
    if (payload) {
        applyNightState(payload.night);
    }
    const key = (payload && typeof payload.srcKey === "string") ? payload.srcKey : "";
    if (!payload || payload.hasCover !== true) {
        onCoverMissing(key); // 另一首歌没封面：前景色交回主题默认（别用上一首的颜色）
        return null; // 没有封面时没必要算
    }
    ++paletteCalls;
    if (key && paletteCache[key]) {
        ++paletteHits;
        // 命中缓存：顺便用它校正自动前景色（换回放过的歌时不用重算）
        const entry = paletteCache[key];
        setForegroundBase(entry.colors.main, key, entry.average, entry.dominant);
        return { colors: entry.colors };
    }
    warmPalette(key);
    return null;
});

/// 封面变化（观察型）：换歌时先把颜色算起来，等应用来问时通常已经命中
musicxx.hooks.register("musicxx.media.cover.changed", { mode: "observe" }, function (payload) {
    const key = (payload && typeof payload.srcKey === "string") ? payload.srcKey : "";
    if (!payload || payload.hasCover !== true) {
        onCoverMissing(key);
        return;
    }
    if (key && paletteCache[key]) {
        // 颜色已经算过：直接用它更新自动前景色（算过的歌不再重复算）
        const entry = paletteCache[key];
        setForegroundBase(entry.colors.main, key, entry.average, entry.dominant);
        return;
    }
    warmPalette(key);
});

/// 启动后预热一次：应用启动时可能已经有一首歌在放
musicxx.timer.setTimeout(function () {
    warmPalette("");
}, 2000);

/* ==================== 跟随背景隐藏歌曲图 ==================== */

/// 手动接管 / 交还之后暂停自动跟随的时长（免得把用户刚做的选择立刻覆盖回去）
const FOLLOW_PAUSE_MS = 10 * 60 * 1000;

/// 暂停跟随到这个时刻（毫秒时间戳；0 = 没暂停）
let followPausedUntil = 0;

/// 最近一次槽位请求的目标与时间（同一个目标 30 秒内不重复请求：动作失败时不要每秒重试）
let lastRequestTarget = "";
let lastRequestAt = 0;

/// 最近一次自动接管 / 操作的说明文本（页面显示）
let lastLinkNote = "";

/// 读一个槽位的状态（同步读，不产生动作往返）
function slotOf(slotId) {
    const slots = musicxx.state.get("musicxx.state.renderSlots") || {};
    return slots[slotId] || null;
}

/// 现在由哪个**插件项**在画该槽位（空 = 没有插件项在画）
function slotItemIdOf(slotId) {
    const slot = slotOf(slotId);
    return (slot && typeof slot.itemId === "string") ? slot.itemId : "";
}

/// 用户选中了谁（可能是内置样式 `builtin:*`）
function slotSelectedIdOf(slotId) {
    const slot = slotOf(slotId);
    return (slot && typeof slot.selectedId === "string") ? slot.selectedId : "";
}

/// 插件项 id 对应的模式名（不是本插件的项时返回空串）
function modeOfItemId(itemId) {
    return MODE_OF_ITEM[itemId] || "";
}

/// 背景槽位现在是不是本插件的样式在画
function backgroundIsMine() {
    return "" !== modeOfItemId(slotItemIdOf(BACKGROUND_SLOT));
}

/// 请求切换一个槽位（异步动作，不等待结果）
///
/// [manual] 为真表示这是用户主动点的（页面上的按钮）：之后暂停自动跟随一会儿，
/// 免得跟随逻辑把用户刚做的选择又改回去。
function requestSlotSelect(slotId, itemId, note, manual) {
    musicxx.host.log(2, "收到槽位切换请求: " + slotId + " → " + itemId);
    lastRequestTarget = itemId;
    lastRequestAt = Date.now();
    if (manual) {
        followPausedUntil = Date.now() + FOLLOW_PAUSE_MS;
    }
    lastLinkNote = note;
    musicxx.render.select(itemId, { slot: slotId }).then(function (result) {
        if (result && result.ok === true) {
            lastLinkNote = note + "（已生效）";
            musicxx.host.log(2, "槽位切换成功: " + slotId + " → " + itemId);
        } else {
            lastLinkNote = note + "失败：" + ((result && result.error) || "未知原因");
            musicxx.host.log(3, "槽位切换失败: " + JSON.stringify(result || {}));
        }
    }, function (err) {
        lastLinkNote = note + "失败：" + errText(err);
        musicxx.host.log(3, "槽位切换失败: " + errText(err));
    });
}

/// 每秒检查一次：打开「隐藏歌曲图」且背景是本插件的两种模式之一时接管，否则交还
///
/// 用状态镜像判断（同步读，不产生动作往返）：容器里没有"槽位变化事件"，轮询 1 秒足够 ——
/// 一次读 + 一次字符串比较。用户手动操作后 10 分钟内不自动跟随；同一个目标 30 秒内不重复
/// 请求（失败时不要每秒重试）。
///
/// 顺带查一次"昼夜 + 封面主色"（自动调整前景色用；见 [refreshForegroundIfDue]）。
///
/// [force] 为真时跳过暂停与去重（设置页刚改完开关，希望当场看到结果）。
/// 判断写成 `true != force`：定时器回调不带参数（force = undefined），
/// 用 `false == force` 会让"没传参数"被当成"强制"，暂停与去重就都失效了。
function followTick(force) {
    const now = Date.now();
    refreshForegroundIfDue(force);
    if (true != force && now < followPausedUntil) {
        return;
    }
    const bgMine = backgroundIsMine();
    const iconMine = slotItemIdOf(ICON_SLOT) === ICON_ITEM_ID;
    let target = "";
    let note = "";
    if (configuredHideIcon() && bgMine && false == iconMine) {
        target = ICON_ITEM_ID;
        note = "已隐藏歌曲图";
    } else if ((false == configuredHideIcon() || false == bgMine) && iconMine) {
        target = BUILTIN_ICON_ID;
        note = configuredHideIcon() ? "背景已切走，交还" : "已关闭开关，交还";
    }
    if (!target) {
        return;
    }
    if (true != force && target === lastRequestTarget && now - lastRequestAt < 30000) {
        return;
    }
    requestSlotSelect(ICON_SLOT, target, note, false);
}

musicxx.timer.setInterval(followTick, 1000);

/* ==================== 页面（说明页 + 设置页） ==================== */

/// 客户端能力段（插件框架取页面时会放进参数）
function viewEnv(args) {
    if (args && typeof args.ui === "object" && args.ui !== null) {
        return args.ui;
    }
    return null;
}

/// 现在生效的是哪种模式：`blur` / `card` / `vinyl` / `builtin`（都不是 = 别人的样式或没选中）
///
/// 已经在画的看 `itemId`（现在由谁在画），还没生效的看 `selectedId`（用户选中了谁）：
/// 切换要经过一次动作往返，选中后到生效之间用后者，按钮才算"点得动、有反应"。
function currentMode() {
    const itemMode = modeOfItemId(slotItemIdOf(BACKGROUND_SLOT));
    if (itemMode) {
        return itemMode;
    }
    const selectedMode = modeOfItemId(slotSelectedIdOf(BACKGROUND_SLOT));
    if (selectedMode) {
        return selectedMode;
    }
    const selected = slotSelectedIdOf(BACKGROUND_SLOT);
    if (selected === "" || selected.indexOf("builtin:") === 0) {
        return MODE_BUILTIN;
    }
    return "other";
}

/// 背景槽位的状态文本（哪种模式在画 / 内置 / 别人的样式）
function backgroundStateText() {
    const slot = slotOf(BACKGROUND_SLOT);
    if (!slot) {
        return "无状态";
    }
    const itemMode = modeOfItemId(slotItemIdOf(BACKGROUND_SLOT));
    if (itemMode) {
        return MODE_SHORT[itemMode] + (slot.visible === true ? " · 生效中" : " · 不在画");
    }
    const selected = slotSelectedIdOf(BACKGROUND_SLOT);
    if (modeOfItemId(selected)) {
        return "检查中";
    }
    if (selected === "" || selected.indexOf("builtin:") === 0) {
        return "内置背景";
    }
    return "其它样式";
}

/// 歌曲图槽位的状态文本
function iconStateText() {
    const slot = slotOf(ICON_SLOT);
    if (!slot) {
        return "无状态";
    }
    if (slot.itemId === ICON_ITEM_ID) {
        return (slot.visible === true) ? "已隐藏" : "已隐藏 · 未渲染";
    }
    if (!slot.selectedId) {
        return "显示中";
    }
    if (slot.selectedId === ICON_ITEM_ID && slot.itemId !== ICON_ITEM_ID) {
        return "检查中";
    }
    return "别的样式";
}

/// 渲染状态文本（尺寸 / 帧率 / 是否在画）
function renderStateText() {
    const slot = slotOf(BACKGROUND_SLOT);
    if (!slot || !backgroundIsMine()) {
        return "未生效";
    }
    if (slot.visible !== true) {
        return "未渲染";
    }
    return (slot.width || 0) + "×" + (slot.height || 0) + " · " +
        ((slot.animate === false) ? "静态" : ((slot.maxFps || 0) + " fps"));
}

/// 取色状态文本
function paletteStateText() {
    if (false == musicxx.hooks.has("musicxx.media.palette.provide")) {
        return "未注册";
    }
    return "已注册 · 缓存 " + Object.keys(paletteCache).length + "/" +
        PALETTE_CACHE_LIMIT + " 首";
}

/// 取色的详细计数（写在说明里：它是会变的数字，放在右侧状态位会把行撑长）
function paletteDetailText() {
    if (false == musicxx.hooks.has("musicxx.media.palette.provide")) {
        return "未注册（应用走它自己的封面取色）";
    }
    return "问过 " + paletteCalls + " 次（命中 " + paletteHits + "）、写回 " +
        paletteReady + " 次";
}

/// 一行「标题 + 说明 + 右侧状态」
///
/// 不用 kit 的 `listRow` / `settingRow`：它们的右侧文本没有宽度约束，状态文字一长
/// （例如"示例封面背景 · 模糊（生效中 · 当前不在画）"）在窄窗口里会把整行撑出可见区域
/// （报 `RenderFlex overflow`），左侧说明还会被挤成一字一行、整页被拉得很长。
/// 这里两侧都放进 `Expanded`（左 3 右 2），文字各自在给定宽度里换行 —— 不论状态文字多长都不会溢出。
function infoRow(title, depict, value) {
    const left = {
        kind: "Expanded",
        flex: 3,
        children: [{
            kind: "Column",
            gap: 2,
            children: [
                { kind: "Text", text: title },
                { kind: "Text", text: depict, type: "caption", tone: "hint" },
            ],
        }],
    };
    const children = [left];
    if (value) {
        children.push({
            kind: "Expanded",
            flex: 2,
            children: [{
                kind: "Text",
                text: value,
                type: "caption",
                tone: "hint",
                align: "end",
            }],
        });
    }
    return {
        kind: "Block",
        variant: "plain",
        padding: { horizontal: 10, vertical: 6 },
        children: [{
            kind: "Row",
            gap: 10,
            cross: "center",
            children: children,
        }],
    };
}

/// 两个页面共用的状态区（槽位 / 取色 / 渲染 / 最近一次操作）
function statusRows() {
    return [
        infoRow(
            "播放页背景",
            "渲染槽位 player.background（状态镜像同步读）",
            backgroundStateText()),
        infoRow(
            "播放页歌曲图",
            "渲染槽位 player.icon：接管后保留占位、不画内容",
            iconStateText()),
        infoRow(
            "封面取色",
            "钩子 musicxx.media.palette.provide：统计封面里出现最多的 4 个色点（" +
            paletteDetailText() + "）；『沉浸』的渐变底与『黑胶』的底色用的就是它写的颜色",
            paletteStateText()),
        infoRow(
            "自动前景色",
            "按封面主色 + 亮暗主题推播放页的三色（普通色 / 减淡色 / 块状背景色），" +
            "保留主色色相、按当前模式估背景亮度保证对比度；本插件的背景被选中时生效，" +
            "换歌 / 换模式 / 主题切换后重新推",
            foregroundStateText()),
        infoRow(
            "渲染状态",
            "插件据此知道自己的画面在不在画、有多大、跑多少帧",
            renderStateText()),
        infoRow(
            "最近一次操作",
            lastLinkNote || "（无）",
            ""),
    ];
}

/// 一行数值输入控件（值变化即派发 `setOption`，参数里带 `id` 与当前值）
function numberControl(id, label, help, value, min, max) {
    return {
        kind: "Block",
        variant: "plain",
        padding: { horizontal: 10, vertical: 6 },
        children: [{
            kind: "Control",
            control: "number",
            id: id,
            label: label,
            help: help,
            value: value,
            integer: true,
            min: min,
            max: max,
            action: { kind: "dispatch", name: "setOption", args: { view: SETTINGS_VIEW_ID } },
        }],
    };
}

/// 开关行：标题 + 说明在左（可换行），开关在右
///
/// 为什么不用 kit 的 `switchRow`：它把 `Control` 当成外层 `Row` 的右端元素，而"行内控件"在
/// 宽度无约束时会踩渲染层的 `RenderFlex children have non-zero flex but incoming width
/// constraints are unbounded`（`Row` 给非弹性子元素的就是无限宽，`Expanded` 不能这么放）。
/// 这里给控件一个**固定宽度**的盒子：控件在盒子里定宽绘制，界面表现一致、也不会断言。
/// 宽度取 `XXSwitchBool` 自己的尺寸（190×100 设计像素，是定宽控件）。
const SWITCH_BOX_WIDTH = 190;

function switchControl(id, title, depict, value) {
    return {
        kind: "Block",
        variant: "plain",
        padding: { horizontal: 10, vertical: 6 },
        children: [{
            kind: "Row",
            gap: 12,
            cross: "center",
            children: [
                {
                    kind: "Expanded",
                    flex: 3,
                    children: [{
                        kind: "Column",
                        gap: 2,
                        children: [
                            { kind: "Text", text: title },
                            { kind: "Text", text: depict, type: "caption", tone: "hint" },
                        ],
                    }],
                },
                {
                    kind: "SizedBox",
                    width: SWITCH_BOX_WIDTH,
                    children: [{
                        kind: "Control",
                        control: "switch",
                        id: id,
                        value: value,
                        action: { kind: "dispatch", name: "setOption" },
                    }],
                },
            ],
        }],
    };
}

/// 互斥的几选一：标题 + 说明 + 一排按钮（当前项高亮）
///
/// 用 `Button` 块（各自声明 `{id, value}` 参数）而不是 `Control(buttons)`：按钮块撑满可用宽度
/// （`width: double.infinity`），窄窗口里也放得下；候选项按钮控件按字符数估宽度，中文标签会估少、
/// 按钮里的字顶破按钮（旧的渲染层一律如此）。
function choiceControl(title, help, id, current, viewId, options) {
    return {
        kind: "Block",
        variant: "plain",
        padding: { horizontal: 10, vertical: 6 },
        children: [{
            kind: "Column",
            gap: 6,
            children: [
                { kind: "Text", text: title },
                { kind: "Text", text: help, type: "caption", tone: "hint" },
                {
                    kind: "Row",
                    gap: 12,
                    children: options.map(function (option) {
                        return {
                            kind: "Expanded",
                            children: [{
                                kind: "Button",
                                label: option.label,
                                // 当前项用主按钮的样子（与候选项按钮控件的选中态一致）
                                variant: (option.value === current) ? "primary" : "secondary",
                                action: {
                                    kind: "dispatch",
                                    name: "setOption",
                                    args: {
                                        id: id,
                                        value: option.value,
                                        view: viewId,
                                    },
                                },
                            }],
                        };
                    }),
                },
            ],
        }],
    };
}

/// 模式选择那一行（说明页与设置页共用：[viewId] 决定动作返回哪一页去刷新）
function modeControl(viewId) {
    return choiceControl("", "",
        "mode", currentMode(), viewId, [
        { value: MODE_BLUR, label: "模糊" },
        { value: MODE_CARD, label: "沉浸" },
        { value: MODE_VINYL, label: "黑胶" },
        { value: MODE_BUILTIN, label: "内置背景" },
    ]);
}

/// 设置页：模式、参数、开关与状态
function settingsView(args) {
    const env = viewEnv(args);
    return {
        title: PLUGIN_TITLE + " · 设置",
        subtitle: "三种背景模式的参数与开关；值存在插件目录的 config.json",
        blocks: [
            kit.hint({
                text: "• 本插件注册了三种播放页背景：『" + MODE_TITLES[MODE_BLUR] + "』、『" +
                    MODE_TITLES[MODE_CARD] + "』与『" + MODE_TITLES[MODE_VINYL] + "』。" +
                    "它们占同一个槽位 player.background，" +
                    "「设置 → 主题 → 播放页面背景」里同时只能选中一个。",
            }, env),
            kit.card({ children: statusRows() }, env),
            kit.card({
                title: "模式",
                children: [modeControl(SETTINGS_VIEW_ID)],
            }, env),
            kit.card({
                title: "参数",
                children: [
                    choiceControl("模糊档位", "",
                        "blur", configuredBlur(), SETTINGS_VIEW_ID,
                        BLUR_CHOICES.map(function (value) {
                            return { value: value, label: String(value) };
                        })),
                    choiceControl("背景放大倍数（" + ZOOM_MIN + "~" + ZOOM_MAX + "）",
                        "『模糊』与『沉浸』共用：数值越大画面越局部" +
                        "（1 = 封面铺满、内容最完整）",
                        "zoom", configuredZoom(), SETTINGS_VIEW_ID,
                        ZOOM_CHOICES.map(function (value) {
                            return { value: value, label: value + "×" };
                        })),
                    choiceControl("图片占比（『沉浸』模式）",
                        "图片至少占屏幕的比例：越大图片越多、渐变底越少" +
                        "（图片按卡片铺满、超出部分裁掉）",
                        "share", configuredShare(), SETTINGS_VIEW_ID,
                        SHARE_CHOICES.map(function (value) {
                            return { value: value, label: Math.round(value * 100) + "%" };
                        })),
                    choiceControl("唱片转速（『黑胶』模式）",
                        "盘心的封面转一圈的快慢：1× 是 " + VINYL_SPIN_SECONDS + " 秒一圈",
                        "spin", configuredSpin(), SETTINGS_VIEW_ID,
                        SPIN_CHOICES.map(function (value) {
                            return { value: value, label: value + "×" };
                        })),
                ],
            }, env),
            kit.card({
                title: "开关",
                children: [
                    switchControl("ripple", "热浪扭曲波纹",
                        "关掉后『模糊』只剩缓慢漂移、『沉浸』完全静止",
                        configuredRipple()),
                    switchControl("hideIcon", "隐藏歌曲图",
                        "默认关。打开后，背景是本插件的三种模式之一时接管播放页中间的歌曲图" +
                        "（保留占位、不显示内容）",
                        configuredHideIcon()),
                    switchControl("autoForeground", "自动调整前景色",
                        "默认开。按封面取色的主色 + 亮暗主题推一套播放页前景" +
                        "（白昼偏深、夜间偏浅，保留主色的色相，按当前模式估出的背景亮度" +
                        "保证对比度）；关掉就用主题默认前景色",
                        configuredAutoForeground()),
                ],
            }, env),
            kit.card({
                title: "操作",
                children: [
                    kit.button({
                        label: "清空取色缓存",
                        action: {
                            kind: "dispatch",
                            name: "clearPaletteCache",
                            args: { view: SETTINGS_VIEW_ID },
                        },
                    }, env),
                    kit.button({
                        label: "打开说明页",
                        action: {
                            kind: "route",
                            route: "ext://" + PLUGIN_ID + "/" + HOME_VIEW_ID,
                        },
                    }, env),
                ],
            }, env),
        ],
    };
}

/// 说明页（主页入口）：这个插件做了什么、三种模式各是什么样
function cardView(args) {
    const env = viewEnv(args);
    return {
        title: PLUGIN_TITLE,
        subtitle: "playing_bg_image：三种播放页背景模式 + 歌曲图接管 + 封面取色 + 自动前景色",
        blocks: [
            kit.hint({
                text: "• 『" + MODE_TITLES[MODE_BLUR] + "』：" + MODE_DEPICTS[MODE_BLUR] +
                    "。封面由主程序预模糊（sigma = 设置里的模糊程度），着色器只做放大、漂移、" +
                    "扭曲与压暗；没有封面时回退成暗底 + 光斑。",
            }, env),
            kit.hint({
                text: "• 『" + MODE_TITLES[MODE_CARD] + "』：竖屏时封面贴上、左、右三条边" +
                    "（下边缘模糊并渐隐到渐变底），横屏时贴左、上、下三条边（右边缘同样处理）；" +
                    "图片占比（45%~90%）决定图片至少占屏幕多少，放大倍数只改取景（越大越局部），" +
                    "最后按主题叠一层亮暗遮罩 —— 遮罩色就是主题背景色，浅色主题提亮、深色主题压暗。",
            }, env),
            kit.hint({
                text: "• 『" + MODE_TITLES[MODE_VINYL] + "』：底色是封面主色，中间一张黑胶唱片 ——" +
                    "盘面有细密的音轨纹路，一道来自左上方的柔光与外缘倒角高光；盘心圆裁当前封面" +
                    "并缓慢旋转（转速档位 " + SPIN_CHOICES.join("× / ") + "×，1× 是 " +
                    VINYL_SPIN_SECONDS + " 秒一圈），盘沿上方一支白色唱臂（转轴圆钮 + 折线臂管" +
                    "（肘部圆角过渡，像手臂那样折一下）+ 贴着盘沿的一小段唱头壳，周围一圈很软的" +
                    "淡阴影）压在盘面外圈。没有封面时盘心画一个两色渐变的标签。",
            }, env),
            kit.hint({
                text: "• 『模糊』与『沉浸』都能开关『热浪扭曲波纹』（热浪场的轻微折射感，" +
                    "场里有疏密不同的斑块在游走，波纹关掉后『模糊』只剩缓慢漂移、" +
                    "『沉浸』完全静止）；三种模式都能开关『隐藏歌曲图』（**默认关**：打开后，" +
                    "背景是本插件的样式时接管播放页中间的歌曲图 —— 保留占位、不画内容；" +
                    "封面已经铺满画面 / 已经在盘心，原来的封面圆盘与它重复）。",
            }, env),
            kit.hint({
                text: "• 『自动调整前景色』（**默认开**）按封面取色的主色推一套播放页前景：" +
                    "**保留主色的色相与彩色程度**（文字看着就是主色的深浅版），明度按亮暗主题给" +
                    "倾向（白昼偏深、夜间偏浅）；对比度按**当前模式估出的背景亮度**算，要求逐条" +
                    "放宽 —— 先争取高对比 + 明显的颜色，不够就依次让一点对比度（不低于大字号" +
                    "标准），最后才让颜色，只有怎么调都给不出颜色时才退回黑 / 白。再配出减淡色" +
                    "（同色 30% 透明）与块状背景色（与普通色相反的一侧、低饱和、15% 透明），" +
                    "交给插件框架用在播放页的文字、歌词、进度条与块状背景上。换歌、换模式与主题切换" +
                    "后重新推，关掉就用主题默认前景色；设置页的状态行会显示当前是深色字还是" +
                    "浅色字、对比度多少。",
            }, env),
            kit.hint({
                text: "• 封面取色由本插件提供（钩子 musicxx.media.palette.provide）：把封面缩到 " +
                    PALETTE_COVER_SIZE + "×" + PALETTE_COVER_SIZE + " 读 RGBA，按 4 bit/通道" +
                    "统计出现次数最多的 " + PALETTE_TOP_COLORS + " 个色点，按亮度映射成 " +
                    "main / light / lightMuted / dark / darkMuted。它替代内置的封面颜色分析，" +
                    "内置背景与插件背景都会用上这套颜色，『自动调整前景色』也拿它的主色当底色。",
            }, env),
            kit.card({ children: statusRows() }, env),
            kit.card({
                title: "选择模式",
                children: [modeControl(HOME_VIEW_ID)],
            }, env),
            kit.button({
                label: "打开设置页",
                variant: "primary",
                action: {
                    kind: "route",
                    route: "ext://" + PLUGIN_ID + "/" + SETTINGS_VIEW_ID,
                },
            }, env),
        ],
    };
}

/// 动作从哪个页面发起就返回哪个页面（插件框架用返回值直接刷新当前页）
function pageView(args) {
    return (args && args.view === HOME_VIEW_ID) ? cardView(args) : settingsView(args);
}

musicxx.capability.register("card", function (args) {
    return { view: cardView(args) };
});

musicxx.capability.register("settings", function (args) {
    return { view: settingsView(args) };
});

/// 控件值变化统一入口：控件把自己的 `id` 与当前值带进参数
///
/// 模式选择走槽位动作（`musicxx.render.select`，异步）；其余几项写 config.json 并重新登记
/// 三个背景样式（`updateEntry` 是整体替换）；处理器**同步返回**页面（动作结果稍后由页面
/// 自己从状态镜像读到，见 [currentMode]）。
musicxx.capability.register("setOption", function (args) {
    const id = (args && typeof args.id === "string") ? args.id : "";
    const value = args ? args.value : null;
    if (id === "mode") {
        if (value === MODE_BLUR || value === MODE_CARD || value === MODE_VINYL) {
            requestSlotSelect(
                BACKGROUND_SLOT,
                ITEM_OF_MODE[value],
                "切到" + MODE_SHORT[value],
                false);
        } else if (value === MODE_BUILTIN) {
            requestSlotSelect(BACKGROUND_SLOT, BUILTIN_BG_ID, "切回内置背景", false);
        }
        return { view: pageView(args) };
    }
    if (id === "blur" || id === "zoom" || id === "share" || id === "spin" ||
        id === "ripple" || id === "hideIcon" || id === "autoForeground") {
        saveConfig(id, value);
        // 关掉「隐藏歌曲图」时立刻交还，不用等下一秒的跟随检查
        if (id === "hideIcon") {
            followTick(true);
        }
        // 打开「自动调整前景色」时立刻查一次封面主色与昼夜，不用等下一次定期检查
        if (id === "autoForeground") {
            refreshForegroundState();
        }
        return { view: pageView(args) };
    }
    musicxx.host.log(3, "未知的设置项: " + id);
    return { view: pageView(args) };
});

/// 立即接管歌曲图（手动；之后 10 分钟不自动跟随，免得被跟随逻辑改回去）
musicxx.capability.register("takeIcon", function (args) {
    requestSlotSelect(ICON_SLOT, ICON_ITEM_ID, "手动隐藏", true);
    return { view: pageView(args) };
});

/// 交还歌曲图（手动，显示内置的歌曲图）
musicxx.capability.register("releaseIcon", function (args) {
    requestSlotSelect(ICON_SLOT, BUILTIN_ICON_ID, "手动交还", true);
    return { view: pageView(args) };
});

/// 清空取色缓存（下次换歌 / 换背景样式时重新按封面算一遍）
///
/// 缓存本身最多留 PALETTE_CACHE_LIMIT 首（超出自动丢最早的），这里只是立刻清空。
musicxx.capability.register("clearPaletteCache", function (args) {
    paletteCache = {};
    paletteTasks = {};
    lastLinkNote = "取色缓存已清空";
    return { view: pageView(args) };
});

musicxx.host.log(2, "playing_bg_image（" + PLUGIN_TITLE + "）已加载：背景样式『" +
    MODE_TITLES[MODE_BLUR] + "』/『" + MODE_TITLES[MODE_CARD] + "』/『" +
    MODE_TITLES[MODE_VINYL] + "』+ 歌曲图接管项 + 封面取色钩子 + 自动调整前景色");
