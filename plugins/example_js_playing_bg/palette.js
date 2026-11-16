/// example_js_playing_bg 的取色脚本: 封面取色钩子 + 自动调整播放页前景色
///
/// 两件事都围绕"当前歌曲封面的颜色"：
/// 1. **封面取色**（钩子 `musicxx.media.palette.analyse`）：把封面缩到 64×64 读 RGBA，
///    按 4 bit/通道量化统计出现次数最多的 4 个色点，按亮度映射成
///    main / light / lightMuted / dark / darkMuted，替代内置的封面颜色分析。
///    『沉浸』的渐变底与『黑胶』的底色用的就是这套颜色。
/// 2. **自动调整播放页前景色**（设置页开关，默认开）：按封面取色的主色推一套播放页三色
///    （普通色 / 减淡色 / 块状背景色），保留主色的色相与彩色程度，明度按亮暗主题给倾向，
///    对比度按**当前样式估出的背景亮度**算。三色用能力
///    `musicxx.ui.playing.foreground.set` 交给应用（本插件的背景样式被选中时才接受）。
///
/// 关键约束（沿用原示例的实测结论）：取色钩子的处理器**必须同步返回**。
/// 命中缓存就当场给颜色，没命中返回 null（应用用它自己的快速取色），真正的计算与写回
/// （`musicxx.media.palette.set`）放到后台做 —— 处理器里 `await` 主程序动作会让两边
/// 互相等，表现是切歌 / 换背景样式卡几秒。

/* ==================== 颜色工具 ==================== */

/// 错误对象 → 文本（钩子、定时器里的失败只记日志）
function errText(err) {
    return (err && err.message) ? err.message : String(err);
}

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

/// 色点 → `#rrggbb`
function hexOf(rgb) {
    function part(value) {
        const text = Math.max(0, Math.min(255, value)).toString(16);
        return text.length < 2 ? "0" + text : text;
    }
    return "#" + part(rgb.r) + part(rgb.g) + part(rgb.b);
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

/// 一个色点的亮度（0..1，人眼权重）
function luminanceOf(r, g, b) {
    return (r * 0.299 + g * 0.587 + b * 0.114) / 255;
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

/* ==================== 自动调整播放页前景色 ==================== */
//
// 播放页的文本、歌词、进度条与块状背景只用三种颜色（普通色 / 减淡色 / block 背景色）。
// 本插件的背景都是封面 / 封面主色铺出来的，主题默认的那三色不一定压得住画面，
// 所以这里按封面取色的**主色**再结合亮暗主题推一套，用能力
// `musicxx.ui.playing.foreground.set` 交给应用（应用只在本插件的背景样式被选中时接受）；
// 算不出三色时调 `{clear: true}` 交还主题默认。
//
// 三色怎么定：
//   1. 色相与彩色程度取自封面的**主色**（[accentColorOf]：出现最多的色点，它是灰的就看第二个），
//      换成 HSL 后一路保留色度 —— 这样文字颜色看得出是画面的主色；
//   2. 明度朝主题倾向的一侧调（白昼偏深、夜间偏浅），**从主色自己的明度出发**逐档移动，
//      取第一个够清楚的明度：离主色最近又看得清；
//   3. 对比度按**当前样式估出的背景亮度**算；要求按 [FG_PICK_RULES] 逐条放宽：
//      先"正文级对比度 + 明显的颜色"，不够就依次让一点对比度（不低于 [FG_CONTRAST_FLOOR]），
//      最后才让颜色 —— 保色优先，只有给不出颜色时才退回黑 / 白；
//   4. 减淡色 = 普通色 30% 透明；块状背景色取"与普通色相反的一侧"的低饱和中间色。
//
// 昼夜从哪来：快照频道里的 `musicxx.state.app.isNight` 只在启动时推过一次，运行期切主题
// 它不会更新，所以它**只用来当初始值**；之后的判断一律以 `musicxx.media.palette()` 返回的
// `night` 与取色钩子载荷里的 `night` 为准。

/// 自动前景色的对比度要求（见 [pickForegroundMain]）
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
/// 色度低于它就算"近灰"：挑前景色相时会跳过这样的色点
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
/// 黑胶盘面的近黑色（与 vinyl.frag 的 kDiscBase 同一档）
const FG_DISC_BASE = { r: 12, g: 13, b: 15 };
/// 查一次"当前昼夜 + 封面主色"的间隔：本插件的背景在画时勤查，没在画时几乎不用查
const FG_REFRESH_ACTIVE_MS = 5 * 1000;
const FG_REFRESH_IDLE_MS = 30 * 1000;

/// 当前昼夜（true = 夜间）；null = 还不知道（这时不声明前景三色，播放页用主题默认色）
let isNightNow = null;

/// 封面取色的主色（`#rrggbb`）；空串 = 还没有可用的取色结果
let foregroundBase = "";

/// 前景色的**色相**来源（`#rrggbb`）：封面里"出现最多、又看得出颜色"的那个色点
let foregroundAccent = "";

/// 这条主色是哪首歌的（srcKey）：同一首歌封面一时读不到时用它判断"要不要先留着"
let foregroundBaseKey = "";

/// 封面平均色（取色时顺便算出，只用来估背景亮度）；null = 还没有
let coverAverage = null;

/// 最近一次推给应用的前景（`<样式>|<三色>`）：没变就不重复调用能力
let lastForegroundSpec = "";

/// 已经排了一次"稍后重推前景三色"（钩子处理器里不直接调主程序动作）
let foregroundApplyScheduled = false;

/// 最近一次查"昼夜 + 主色"的时间
let foregroundCheckedAt = 0;

/// 各样式估出的背景亮度：只用来判断"文字压在什么颜色上"，取主要铺满画面的那个颜色
///
/// - 『模糊』：整屏就是（预模糊的）封面本身，夜间着色器再统一压到 0.74；
/// - 『沉浸』：大部分是原图（夜间不压暗），整幅再叠一层主题遮罩（白昼提亮、夜间压暗）；
/// - 『黑胶』：文字大多压在近黑的盘面上，底色只在四周，所以按盘面算；
/// - 『晶格』：模糊封面铺底再叠一层晶格，比原图再压暗一点；
/// - 『光圈』：4 色渐变底（与『模糊』同一套封面配色），与『模糊』同档；
/// - 『插件纹理铺底』：纹理内容插件自己定，按封面平均色估，没有取色结果时用深色兜底。
function backgroundEstimate(style) {
    const night = (true === isNightNow);
    const avg = coverAverage || parseColorText(foregroundBase) ||
        { r: 34, g: 38, b: 46 };
    if (STYLE_VINYL === style) {
        return mixRgb(FG_DISC_BASE, avg, 0.12);
    }
    if (STYLE_CARD === style) {
        return night
            ? mixRgb(avg, { r: 0, g: 0, b: 0 }, 0.34)
            : mixRgb(avg, { r: 255, g: 255, b: 255 }, 0.22);
    }
    if (STYLE_LATTICE === style) {
        return night ? scaleRgb(avg, 0.62) : mixRgb(avg, { r: 0, g: 0, b: 0 }, 0.18);
    }
    if (STYLE_RING === style) {
        return night ? scaleRgb(avg, 0.74) : avg;
    }
    if (STYLE_BOUND === style) {
        return night ? scaleRgb(avg, 0.7) : mixRgb(avg, { r: 0, g: 0, b: 0 }, 0.12);
    }
    return night ? scaleRgb(avg, 0.74) : avg;
}

/// 从一个明度出发、朝一个方向逐档调整，找"第一个够清楚"的明度
///
/// 起点是**主色自己的明度**（夹进 [FG_LIGHTNESS_MIN]..[FG_LIGHTNESS_MAX]）：原色就够清楚时
/// 直接用它，否则每档 [FG_LIGHTNESS_STEP] 往指定方向走一步 —— 得到的永远是"离主色最近、又
/// 满足 [minContrast]"的那一档；这个方向达不到时返回该方向上对比度最高的一个。
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
/// 逐档移动，取**第一个**满足要求的明度 —— 所以文字通常是"主色的加深 / 变亮版"，
/// 而不是一律黑或白。要求按 [FG_PICK_RULES] 逐条放宽。
///
/// 返回 `{rgb, dark, ratio}`：`dark` 表示普通色偏暗（块状背景色要反过来配），
/// `ratio` 是它与背景的对比度。
function pickForegroundMain(hsl, style) {
    const night = (true === isNightNow);
    const bg = backgroundEstimate(style);
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

/// 算一个样式的前景：普通色 / 减淡色 / 块状背景色，外加挑色时的中间结果
///
/// [hsl] 是色相来源（封面的主色）的 HSL；`colors` 就是交给应用的那三色，
/// `dark` / `ratio` 是状态行要报的"深色字还是浅色字 / 对比度多少"。
function computeForegroundOf(hsl, style) {
    const picked = pickForegroundMain(hsl, style);
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

/// 六个样式各自的前景三色；返回 null 表示"这次不指定"
///
/// null 的两种情况：开关关着、昼夜还不知道、或者还没有取色结果 —— 这时插件框架用它自己的
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
    const specs = {};
    const styles = [
        STYLE_BLUR, STYLE_CARD, STYLE_VINYL,
        STYLE_LATTICE, STYLE_RING, STYLE_BOUND,
    ];
    for (let i = 0; i < styles.length; ++i) {
        specs[styles[i]] = computeForegroundOf(hsl, styles[i]);
    }
    foregroundCacheKey = key;
    foregroundCache = specs;
    return specs;
}

/// 某个样式的前景三色（没算出来就返回 null）
function foregroundColorsSpec(style) {
    const specs = foregroundSpecs();
    if (!specs) {
        return null;
    }
    const of = specs[style];
    return of ? of.colors : null;
}

/// 当前歌曲的身份（换歌判断用；镜像里没有就是空串）
function currentSongKey() {
    const song = musicxx.broadcast.read("musicxx.state.song");
    if (song && typeof song.srcKey === "string") {
        return song.srcKey;
    }
    return "";
}

/// 记下封面取色的主色、平均色与"色相来源"（取色算完 / 当前歌已有分析结果时调用）
///
/// [key] 是这套颜色属于哪首歌（srcKey）：报上来的歌和快照频道里正在放的不是同一首、
/// 或者同一首歌的结果晚到了（切歌了），都不写进来 —— 颜色错配比"晚一点上色"更难受。
/// 只改脚本里的状态，重推排到本次之后（见 [applyForegroundLater]）：这个函数会从
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

/// 把当前样式的前景三色推给应用（`musicxx.ui.playing.foreground.set`）
///
/// 应用只接受“当前生效的播放页背景插件项”的调用，所以没在画时一概不调（调了会回 `not_active`）。
/// 算不出三色（开关关着 / 还不知道昼夜 / 没有取色结果）时交还主题默认（`clear`）。
/// 同一个结果只推一次（[lastForegroundSpec]）。
function applyForegroundIfChanged() {
    const style = styleOfItem(slotItemIdOf(BACKGROUND_SLOT));
    if (!style) {
        // 现在不是本插件的背景在画：不碰前景（应用在生效项换人时自己清空）
        lastForegroundSpec = "";
        return false;
    }
    const colors = foregroundColorsSpec(style);
    const key = style + "|" + (colors ? JSON.stringify(colors) : "default");
    if (key === lastForegroundSpec) {
        return false;
    }
    lastForegroundSpec = key;
    musicxx.capability.call(
        "musicxx.ui.playing.foreground.set",
        colors ? { style: "mask", colors: colors } : { clear: true }
    ).then(function (result) {
        if (result && result.ok === true) {
            return;
        }
        // 失败（例如应用侧的生效项还没同步过来）：清掉"已推过"标记，下一次检查重试
        lastForegroundSpec = "";
        musicxx.host.log(3, "设置播放页前景失败: " + JSON.stringify(result || {}));
    }, function (err) {
        lastForegroundSpec = "";
        musicxx.host.log(3, "设置播放页前景失败: " + errText(err));
    });
    return true;
}

/// 把"推前景"排到本轮之后再执行
///
/// 取色钩子是主程序等着回答的调用点（处理器里直接调能力会打断调用方），
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

/// 快照频道里的渲染槽位状态（同步读，不产生动作往返）
function slotMirrorOf(slotId) {
    const slots = musicxx.broadcast.read("musicxx.state.uiSlots") || {};
    return slots[slotId] || null;
}

/// 从快照频道里读昼夜（主程序启动时推过一次；读不到返回 null）
///
/// 只在还没有值时用它当初始值：运行期切了主题镜像不会更新（主程序只在启动时推这个键），
/// 拿它当"当前值"会和当场查询的结果打架，配色就会隔一会儿跳一次。
function isNightFromMirror() {
    const app = musicxx.broadcast.read("musicxx.state.app");
    if (app && typeof app.isNight === "boolean") {
        return app.isNight;
    }
    const slot = slotMirrorOf(BACKGROUND_SLOT);
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
/// [force] 写成 `true != force` 判断（不是 `false == force`）：定时器回调不带参数，
/// 那时的 force 是 undefined，用 `false == force` 会把"没传参数"当成"强制刷新"。
function refreshForegroundIfDue(force) {
    // 样式可能刚被切走 / 切回本插件：每次检查都按当前状态推一次（同一结果只推一次）
    if (false == configuredAutoForeground()) {
        applyForegroundIfChanged();
        return;
    }
    if (null === isNightNow) {
        const seed = isNightFromMirror();
        if (null !== seed) {
            isNightNow = seed;
            applyForegroundLater();
        }
    }
    applyForegroundIfChanged();
    const now = Date.now();
    const interval = backgroundIsMine() ? FG_REFRESH_ACTIVE_MS : FG_REFRESH_IDLE_MS;
    if (true != force && now - foregroundCheckedAt < interval) {
        return;
    }
    refreshForegroundState();
}

/// 自动前景色的状态文本（设置页 / 说明页的状态行）
///
/// 顺便报出当前是深色字还是浅色字、与估出的背景对比度多少 —— 换歌、切主题、换样式后
/// 颜色为什么变，看这一行就够了。
function foregroundStateText() {
    if (false == configuredAutoForeground()) {
        return "已关闭";
    }
    if (null === isNightNow) {
        return "读取中";
    }
    if (false == musicxx.capability.has("musicxx.media.palette.analyse")) {
        return "取色未注册";
    }
    const specs = foregroundSpecs();
    if (!specs) {
        return "等待封面取色";
    }
    const style = currentStyle();
    const useStyle = (STYLE_OTHER === style || STYLE_BUILTIN === style) ? STYLE_BLUR : style;
    const of = specs[useStyle];
    return (true === isNightNow ? "夜间" : "白昼") + " · " +
        (of.dark ? "深色字" : "浅色字") + " · 对比度 " +
        (Math.round(of.ratio * 10) / 10) + " · " + of.colors.main;
}

/* ==================== 封面取色（钩子 musicxx.media.palette.analyse） ==================== */

/// base64 字符表（QuickJS 里没有 atob，自己解码：4 个字符 → 3 字节）
const BASE64_CHARS =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/// 字符码 → 6 bit 值（非法字符与空白返回 -1）
const BASE64_LOOKUP = (function () {
    const table = [];
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

/// 取色尺寸与"取几个色点"
const PALETTE_COVER_SIZE = 64;
const PALETTE_TOP_COLORS = 4;
/// 取色缓存最多留这么多首歌
///
/// 它是"最近放过的歌"的缓存，不是长期存储：一首歌一条（5 个颜色文本），不设上限时
/// 长时间播放会一直涨。满了就丢最早的一条（字符串键按插入顺序排列），
/// 要立刻清空用设置页的「清空取色缓存」。
const PALETTE_CACHE_LIMIT = 64;

/// 统计 RGBA 字节里出现次数最多的色点（简单算法：4 bit/通道量化 + 逐像素计数）
///
/// 每个通道取高 4 位当桶号（4096 个桶），桶里累加真实通道值，取计数最多的前
/// [PALETTE_TOP_COLORS] 个桶，代表色取桶内平均 —— 比"每像素聚类"便宜得多。
/// 顺便算出**整幅的平均色**（`average`）：自动前景色要用它估背景亮度。
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
/// 跟"画面主色"没什么关系；而"出现最多但近灰"的色点（白底封面）也不该当色相来源。
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
/// 返回 `{colors, average, dominant}`：`colors` 是写给插件框架的那 5 个字段，
/// `average` 是整幅平均色（自动前景色估背景用），`dominant` 是前景色的色相来源。
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

/// 正在计算的取色任务（srcKey → Promise）：同一首歌不重复计算
let paletteTasks = {};

/// 取色自检计数（设置页显示）
let paletteCalls = 0;
let paletteHits = 0;
let paletteReady = 0;

/// 写进取色缓存，超出上限时丢掉最早的一条
function cachePalette(key, entry) {
    paletteCache[key] = entry;
    const keys = Object.keys(paletteCache);
    while (keys.length > PALETTE_CACHE_LIMIT) {
        delete paletteCache[keys.shift()];
    }
}

/// 异步算好封面配色，算完**主动写回**（`musicxx.media.palette.set`）
///
/// 为什么要这样绕一圈：取色钩子是"应用要颜色"的调用点，处理器里 `await
/// musicxx.media.cover(...)` 会让应用一直等到超时 —— 用户看到的是**切歌 / 切换背景样式
/// 卡几秒**。所以钩子处理器只读缓存（命中就当场给），没命中就返回 null（应用用它自己的
/// 快速取色），真正的计算与写回都在这里异步做。
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

/// 取色钩子的处理器内容：应用要为当前歌曲封面取色时问一次
///
/// - 命中缓存 → **同步返回**这套颜色（应用当场用上，跳过它自己的分析）；
/// - 没命中 → 返回 null（应用走它自己的快速取色），同时后台算好、算完用
///   `musicxx.media.palette.set` 写回（颜色稍后会被插件的算法替换）。
///
/// **处理器绝不返回 Promise**：见 [warmPalette] 的说明。
function paletteAnalyseVerdict(payload) {
    // 载荷里的 night 是当场算的：主题切了快照频道不会更新，这里顺手校正一次
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
}

/// 封面变化（观察型）：换歌时先把颜色算起来，等应用来问时通常已经命中
function paletteOnCoverChanged(payload) {
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
}

/// 取色状态文本
function paletteStateText() {
    if (false == musicxx.capability.has("musicxx.media.palette.analyse")) {
        return "未注册";
    }
    return "已注册 · 缓存 " + Object.keys(paletteCache).length + "/" +
        PALETTE_CACHE_LIMIT + " 首";
}

/// 取色的详细计数（写在说明里：它是会变的数字，放在右侧状态位会把行撑长）
function paletteDetailText() {
    if (false == musicxx.capability.has("musicxx.media.palette.analyse")) {
        return "未注册（应用走它自己的封面取色）";
    }
    return "问过 " + paletteCalls + " 次（命中 " + paletteHits + " 次）、写回 " +
        paletteReady + " 次";
}
