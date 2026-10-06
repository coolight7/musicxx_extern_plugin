/// musicxx 外部插件: playing_bg_image —— 「示例封面背景」
///
/// 三件事（都由『拟声++』与用户在设置里的选择门控，未选中时零成本）：
///
/// 1. **播放页背景的两种模式**（渲染槽位 `player.background`，同一个槽位里的两个候选样式）：
///
///    · 『示例封面背景 · 模糊热浪』（`shader/heat.shaderbundle`）：当前歌曲封面预模糊后放大
///      铺满屏幕、缓慢四处漂移，局部偶尔轻微扭曲 —— 类似夏天地面温度高、空气被折射时看到的
///      轻微晃动。模糊程度（0~30）与放大倍数（1~3）在设置页里调，扭曲波纹可以关掉；
///      没有封面时回退成暗底 + 光斑。
///
///    · 『示例封面背景 · 渐变贴边』（`shader/card.shaderbundle`）：封面主色做渐变底，
///      清晰封面贴着屏幕边缘铺一片 —— 竖屏贴上、左、右三条边（下边缘模糊渐隐），横屏贴左、
///      上、下三条边（右边缘模糊渐隐）；放大倍数（1~3）只改取景（越大越局部），
///      最后按主题叠一层亮暗遮罩（遮罩色就是主题背景色，浅色主题提亮、深色主题压暗）。
///
/// 2. **接管播放页歌曲图**（渲染槽位 `player.icon`）：声明 `mode: "none"` + `keepSpace: true`
///    —— 不显示内容、只保留占位（封面已经铺满画面，原来的封面圆盘与它重复）。
///    设置页的「隐藏歌曲图」打开后，背景是本插件的两种模式之一时自动接管：每秒读一次状态
///    镜像 `musicxx.state.renderSlots` 判断，背景切走（或关掉开关）就自动交还。
///
/// 3. **封面取色**（钩子 `musicxx.media.palette.provide`）：自己算封面配色 ——
///    把封面缩到 64×64 读 RGBA，按 4 bit/通道量化统计，取出现次数最多的 4 个色点，
///    按亮度映射成 main / light / lightMuted / dark / darkMuted，替代内置的颜色分析。
///    『渐变贴边』的渐变底用的就是这套颜色（`musicxx.icon.main` / `musicxx.icon.dark`）。
///
/// 设置（5 项，存在插件目录的 config.json）：模糊程度、放大倍数、图片占比、热浪扭曲波纹、隐藏歌曲图。
///
/// 排查入口：宿主日志（`musicxx.host.log`）、设置页（`ext://playing_bg_image/settings`）、
/// 说明页（`ext://playing_bg_image/card`）、状态镜像里的 `player.background` / `player.icon`
/// 两个槽位。
///
/// 约束：脚本顶层必须**同步**完成注册（顶层不能用 await）；异步逻辑放到钩子或定时器里。
const kit = pluginxx.ui.kit;

/// 插件 id 与显示名
const PLUGIN_ID = musicxx.pluginId;
const PLUGIN_TITLE = "示例封面背景";

/// 两种模式（同一个槽位里的两个候选样式：用户在『设置 → 主题 → 播放页面背景』里选一个）
const MODE_BLUR = "blur";
const MODE_CARD = "card";
/// 模式选择里的"用内置背景"（不是本插件的模式，只是让两个样式都不生效）
const MODE_BUILTIN = "builtin";

/// 模式的显示名与副标题（设置列表里的样式名 = 这里的标题）
const MODE_TITLES = {};
MODE_TITLES[MODE_BLUR] = PLUGIN_TITLE + " · 模糊热浪";
MODE_TITLES[MODE_CARD] = PLUGIN_TITLE + " · 渐变贴边";
/// 模式的短名（状态行里用；完整标题太长，窄窗口里放不下）
const MODE_SHORT = {};
MODE_SHORT[MODE_BLUR] = "模糊热浪";
MODE_SHORT[MODE_CARD] = "渐变贴边";
const MODE_DEPICTS = {};
MODE_DEPICTS[MODE_BLUR] =
    "封面高斯模糊后放大铺满屏幕、缓慢四处漂移；模糊程度 0~30、放大倍数 1~3 可调";
MODE_DEPICTS[MODE_CARD] =
    "封面主色做渐变底、清晰封面贴着屏幕边缘铺一片（自由边模糊、图片占比可调），按主题叠亮暗遮罩";

/// 两种模式各自的 UI 项短名与完整 id
///
/// 模式一沿用原来的短名 `bg`：已经选中它的用户不会因为这次重构而失效。
const BG_ITEM_NAME = "bg";
const BG_ITEM_ID = "plugin." + PLUGIN_ID + "." + BG_ITEM_NAME;
const CARD_ITEM_NAME = "bgCard";
const CARD_ITEM_ID = "plugin." + PLUGIN_ID + "." + CARD_ITEM_NAME;

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

/// 封面纹理参数：宿主解码 → 缩放 → 可选预模糊（着色器只做放大、漂移、扭曲）
///
/// `cover.blur` 是**纹理上的** sigma：纹理铺满屏幕后再放大，屏幕上看到的模糊 ≈
/// `blur × 纹理→屏幕的放大倍数` —— 512 的纹理在 1280 宽的窗口里大约放大 5 倍，
/// 所以 30 会糊成一片渐变（只剩色带），14 才能看出封面的大致内容。
/// 两个模式各用一份纹理：『模糊热浪』按设置预模糊，『渐变贴边』要清晰（blur = 0）。
const COVER_TEXTURE = "uCover";
const COVER_INFO = "uCoverInfo";
const BLUR_COVER_SIZE = 512;
const CARD_COVER_SIZE = 768;

/// 设置的取值范围与默认值
const BLUR_MIN = 0;
const BLUR_MAX = 30;
const BLUR_DEFAULT = 14;
const ZOOM_MIN = 1;
const ZOOM_MAX = 3;
const ZOOM_DEFAULT = 2;
/// 图片占比（只影响『渐变贴边』）：图片至少占屏幕这个比例，越大图片越多、渐变底越少
const SHARE_MIN = 0.3;
const SHARE_MAX = 1;
const SHARE_DEFAULT = 0.75;
const RIPPLE_DEFAULT = true;
const HIDE_ICON_DEFAULT = true;

/// 设置页上的一排候选项（精调走输入框）
const BLUR_CHOICES = [0, 8, 16, 24, 30];
const ZOOM_CHOICES = [1, 1.5, 2, 2.5, 3];
const SHARE_CHOICES = [0.45, 0.6, 0.75, 0.9];

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
    ripple: RIPPLE_DEFAULT,
    hideIcon: HIDE_ICON_DEFAULT,
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
        case "ripple":
            return configBool(value, RIPPLE_DEFAULT);
        case "hideIcon":
            return configBool(value, HIDE_ICON_DEFAULT);
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

/// 『模糊热浪』的预模糊强度（0~30）
function configuredBlur() {
    return configValue("blur");
}

/// 放大倍数（1~3，两种模式共用）
function configuredZoom() {
    return configValue("zoom");
}

/// 图片占比（只影响『渐变贴边』：图片至少占屏幕这个比例）
function configuredShare() {
    return configValue("share");
}

/// 热浪扭曲波纹开关
function configuredRipple() {
    return configValue("ripple");
}

/// 隐藏歌曲图开关
function configuredHideIcon() {
    return configValue("hideIcon");
}

/// 把设置应用到两个背景样式
///
/// `musicxx.ui.updateEntry` 是**整体替换**（每次都取完整的 data），宿主收到后会刷新候选；
/// 正在使用的样式立即换用新参数（封面纹理按新参数重新解码一次，『模糊热浪』改模糊程度时
/// 就能当场看到变化）。
function applySettings() {
    musicxx.ui.updateEntry(BG_ITEM_NAME, blurBackgroundData());
    musicxx.ui.updateEntry(CARD_ITEM_NAME, cardBackgroundData());
}

/// 异步读配置（默认值由脚本给；四个键都读完 / 失败后再换用实际值）
///
/// 放在定时器里读：顶层同步注册阶段不能投递动作（宿主这时还在回放注册）。
function refreshConfig() {
    const keys = Object.keys(CONFIG_DEFAULTS);
    let pending = keys.length;
    function done() {
        --pending;
        if (0 === pending) {
            applySettings();
        }
    }
    keys.forEach(function (key) {
        musicxx.storage.getConfig(key, CONFIG_DEFAULTS[key]).then(function (value) {
            if (false == configSettled[key]) {
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

/* ==================== UI 项声明 ==================== */

/// 『模糊热浪』的完整声明
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
        speed: 1,
        maxFps: 24,
        // 模糊画面不需要满分辨率：降采样后由宿主放大，省一半以上的像素
        resolutionScale: 0.6,
        animate: true,
    };
}

/// 『渐变贴边』的完整声明
///
/// 与『模糊热浪』的区别：清晰封面（`blur: 0`，尺寸给大一点，卡片里是原图）+ 封面主色做的
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
            // 渐变底：封面主色（亮端）→ 封面暗色（暗端）；convert = 套宿主的昼夜转换
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
            // 图片占比（只在『渐变贴边』里用：图片至少占屏幕的这个比例）
            uShare: { kind: "const", value: configuredShare() },
            uRipple: { kind: "const", value: configuredRipple() ? 1 : 0 },
        },
        speed: 1,
        // 这一种基本是静态画面（只有热浪波纹在动），帧率给低一档就够
        maxFps: 16,
        // 卡片里是清晰封面：不降采样
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

/// 注册两个背景样式（同一个槽位的两个候选，注册顺序决定设置列表里的先后）
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
        subtitle: "playing_bg_image：两种播放页背景模式 + 封面取色",
        // 图标名放在 data 里（`registerEntry` 只在没有 data 时才把顶层字段并进去）
        icon: "addition",
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
    return picked;
}

/// 取到的色点数量、取色尺寸与"取几个色点"
const PALETTE_COVER_SIZE = 64;
const PALETTE_TOP_COLORS = 4;

/// 取到的一组色点 → 宿主封面取色的 5 个字段（按亮度从亮到暗映射）
///
/// 数量不足时按已有的复用（只求"这套颜色来自封面"）。这里同时做一点调整：
/// 最亮的提亮一点、最暗的压暗一点，让内置背景与插件背景都有明暗层次可用 ——
/// 『渐变贴边』的渐变底（`musicxx.icon.main` → `musicxx.icon.dark`）用的就是这两个。
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

/// 取当前封面的主要配色（读 `musicxx.media.cover` 的 RGBA 字节）
function paletteFromCurrentCover() {
    return musicxx.media.cover({ size: PALETTE_COVER_SIZE, format: "rgba" })
        .then(function (result) {
            if (!result || result.ok !== true || typeof result.data !== "string") {
                return null;
            }
            return paletteFromTopColors(topColorsOfRgba(base64ToBytes(result.data)));
        }, function (err) {
            musicxx.host.log(3, "读取封面失败: " + errText(err));
            return null;
        });
}

/// 取色缓存（srcKey → 颜色表）：同一首歌只算一次
let paletteCache = {};

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
    const task = paletteFromCurrentCover().then(function (colors) {
        paletteTasks[cacheKey] = null;
        if (!colors) {
            return null;
        }
        if (key) {
            paletteCache[key] = colors;
        }
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
    if (!payload || payload.hasCover !== true) {
        return null; // 没有封面时没必要算
    }
    ++paletteCalls;
    const key = (typeof payload.srcKey === "string") ? payload.srcKey : "";
    if (key && paletteCache[key]) {
        ++paletteHits;
        return { colors: paletteCache[key] };
    }
    warmPalette(key);
    return null;
});

/// 封面变化（观察型）：换歌时先把颜色算起来，等应用来问时通常已经命中
musicxx.hooks.register("musicxx.media.cover.changed", { mode: "observe" }, function (payload) {
    if (!payload || payload.hasCover !== true) {
        return;
    }
    const key = (typeof payload.srcKey === "string") ? payload.srcKey : "";
    if (key && paletteCache[key]) {
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

/// 最近一次联动 / 操作的说明文本（页面显示）
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

/// 背景槽位现在是不是本插件的样式在画
function backgroundIsMine() {
    const itemId = slotItemIdOf(BACKGROUND_SLOT);
    return itemId === BG_ITEM_ID || itemId === CARD_ITEM_ID;
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
/// [force] 为真时跳过暂停与去重（设置页刚改完开关，希望当场看到结果）。
function followTick(force) {
    const now = Date.now();
    if (false == force && now < followPausedUntil) {
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
    if (false == force && target === lastRequestTarget && now - lastRequestAt < 30000) {
        return;
    }
    requestSlotSelect(ICON_SLOT, target, note, false);
}

musicxx.timer.setInterval(followTick, 1000);

/* ==================== 页面（说明页 + 设置页） ==================== */

/// 客户端能力段（宿主取页面时会放进参数）
function viewEnv(args) {
    if (args && typeof args.ui === "object" && args.ui !== null) {
        return args.ui;
    }
    return null;
}

/// 现在生效的是哪种模式：`blur` / `card` / `builtin`（都不是 = 别人的样式或没选中）
///
/// 已经在画的看 `itemId`（现在由谁在画），还没生效的看 `selectedId`（用户选中了谁）：
/// 切换要经过一次动作往返，选中后到生效之间用后者，按钮才算"点得动、有反应"。
function currentMode() {
    const itemId = slotItemIdOf(BACKGROUND_SLOT);
    if (itemId === BG_ITEM_ID) {
        return MODE_BLUR;
    }
    if (itemId === CARD_ITEM_ID) {
        return MODE_CARD;
    }
    const selected = slotSelectedIdOf(BACKGROUND_SLOT);
    if (selected === BG_ITEM_ID) {
        return MODE_BLUR;
    }
    if (selected === CARD_ITEM_ID) {
        return MODE_CARD;
    }
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
    const itemId = slotItemIdOf(BACKGROUND_SLOT);
    if (itemId === BG_ITEM_ID || itemId === CARD_ITEM_ID) {
        const which = (itemId === BG_ITEM_ID) ? MODE_SHORT[MODE_BLUR] : MODE_SHORT[MODE_CARD];
        return which + (slot.visible === true ? " · 生效中" : " · 不在画");
    }
    const selected = slotSelectedIdOf(BACKGROUND_SLOT);
    if (selected === BG_ITEM_ID || selected === CARD_ITEM_ID) {
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
    return "已注册 · 缓存 " + Object.keys(paletteCache).length + " 首";
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
/// （例如"示例封面背景 · 模糊热浪（生效中 · 当前不在画）"）在窄窗口里会把整行撑出可见区域
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
            paletteDetailText() + "）；『渐变贴边』的渐变底用的就是它写的颜色",
            paletteStateText()),
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
    return choiceControl("选择模式", "",
        "mode", currentMode(), viewId, [
        { value: MODE_BLUR, label: "模糊热浪" },
        { value: MODE_CARD, label: "渐变贴边" },
        { value: MODE_BUILTIN, label: "内置背景" },
    ]);
}

/// 设置页：模式、参数、开关与状态
function settingsView(args) {
    const env = viewEnv(args);
    return {
        title: PLUGIN_TITLE + " · 设置",
        subtitle: "两种背景模式的参数与开关；值存在插件目录的 config.json",
        blocks: [
            kit.hint({
                text: "• 本插件注册了两种播放页背景：『" + MODE_TITLES[MODE_BLUR] + "』与『" +
                    MODE_TITLES[MODE_CARD] + "』。它们占同一个槽位 player.background，" +
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
                    choiceControl("常用模糊档位", "点一下就用这个值（当前档位高亮）",
                        "blur", configuredBlur(), SETTINGS_VIEW_ID,
                        BLUR_CHOICES.map(function (value) {
                            return { value: value, label: String(value) };
                        })),
                    choiceControl("背景放大倍数（" + ZOOM_MIN + "~" + ZOOM_MAX + "）",
                        "两种模式共用：数值越大画面越局部（1 = 封面铺满、内容最完整）",
                        "zoom", configuredZoom(), SETTINGS_VIEW_ID,
                        ZOOM_CHOICES.map(function (value) {
                            return { value: value, label: value + "×" };
                        })),
                    choiceControl("图片占比（只影响『渐变贴边』）",
                        "图片至少占屏幕的比例：越大图片越多、渐变底越少" +
                        "（图片按卡片铺满、超出部分裁掉）",
                        "share", configuredShare(), SETTINGS_VIEW_ID,
                        SHARE_CHOICES.map(function (value) {
                            return { value: value, label: Math.round(value * 100) + "%" };
                        })),
                ],
            }, env),
            kit.card({
                title: "开关",
                children: [
                    switchControl("ripple", "热浪扭曲波纹",
                        "关掉后『模糊热浪』只剩缓慢漂移、『渐变贴边』完全静止",
                        configuredRipple()),
                    switchControl("hideIcon", "隐藏歌曲图",
                        "背景是本插件的两种模式之一时，接管播放页中间的歌曲图" +
                        "（保留占位、不显示内容）",
                        configuredHideIcon()),
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

/// 说明页（主页入口）：这个插件做了什么、两种模式各是什么样
function cardView(args) {
    const env = viewEnv(args);
    return {
        title: PLUGIN_TITLE,
        subtitle: "playing_bg_image：两种播放页背景模式 + 歌曲图接管 + 封面取色",
        blocks: [
            kit.hint({
                text: "• 『" + MODE_TITLES[MODE_BLUR] + "』：" + MODE_DEPICTS[MODE_BLUR] +
                    "。封面由宿主预模糊（sigma = 设置里的模糊程度），着色器只做放大、漂移、" +
                    "扭曲与压暗；没有封面时回退成暗底 + 光斑。",
            }, env),
            kit.hint({
                text: "• 『" + MODE_TITLES[MODE_CARD] + "』：竖屏时封面贴上、左、右三条边" +
                    "（下边缘模糊并渐隐到渐变底），横屏时贴左、上、下三条边（右边缘同样处理）；" +
                    "图片占比（45%~90%）决定图片至少占屏幕多少，放大倍数只改取景（越大越局部），" +
                    "最后按主题叠一层亮暗遮罩 —— 遮罩色就是主题背景色，浅色主题提亮、深色主题压暗。",
            }, env),
            kit.hint({
                text: "• 两种模式都能开关『热浪扭曲波纹』（热浪场的轻微折射感）与『隐藏歌曲图』" +
                    "（背景是本插件的样式时接管播放页中间的歌曲图：保留占位、不画内容 —— " +
                    "封面已经铺满画面，原来的封面圆盘与它重复）。",
            }, env),
            kit.hint({
                text: "• 封面取色由本插件提供（钩子 musicxx.media.palette.provide）：把封面缩到 " +
                    PALETTE_COVER_SIZE + "×" + PALETTE_COVER_SIZE + " 读 RGBA，按 4 bit/通道" +
                    "统计出现次数最多的 " + PALETTE_TOP_COLORS + " 个色点，按亮度映射成 " +
                    "main / light / lightMuted / dark / darkMuted。它替代内置的封面颜色分析，" +
                    "内置背景与插件背景都会用上这套颜色。",
            }, env),
            kit.card({ children: statusRows() }, env),
            kit.card({
                title: "模式",
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

/// 动作从哪个页面发起就返回哪个页面（宿主用返回值直接刷新当前页）
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
/// 模式选择走槽位动作（`musicxx.render.select`，异步）；其余四项写 config.json 并重新登记
/// 两个背景样式（`updateEntry` 是整体替换）；处理器**同步返回**页面（动作结果稍后由页面
/// 自己从状态镜像读到，见 [currentMode]）。
musicxx.capability.register("setOption", function (args) {
    const id = (args && typeof args.id === "string") ? args.id : "";
    const value = args ? args.value : null;
    if (id === "mode") {
        if (value === MODE_BLUR || value === MODE_CARD) {
            requestSlotSelect(
                BACKGROUND_SLOT,
                (value === MODE_BLUR) ? BG_ITEM_ID : CARD_ITEM_ID,
                "切到" + MODE_SHORT[value],
                false);
        } else if (value === MODE_BUILTIN) {
            requestSlotSelect(BACKGROUND_SLOT, BUILTIN_BG_ID, "切回内置背景", false);
        }
        return { view: pageView(args) };
    }
    if (id === "blur" || id === "zoom" || id === "share" ||
        id === "ripple" || id === "hideIcon") {
        saveConfig(id, value);
        // 关掉「隐藏歌曲图」时立刻交还，不用等下一秒的跟随检查
        if (id === "hideIcon") {
            followTick(true);
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
musicxx.capability.register("clearPaletteCache", function (args) {
    paletteCache = {};
    paletteTasks = {};
    lastLinkNote = "取色缓存已清空";
    return { view: pageView(args) };
});

musicxx.host.log(2, "playing_bg_image（" + PLUGIN_TITLE + "）已装载：背景样式『" +
    MODE_TITLES[MODE_BLUR] + "』/『" + MODE_TITLES[MODE_CARD] +
    "』+ 歌曲图接管项 + 封面取色钩子");
