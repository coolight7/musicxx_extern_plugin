/// musicxx 外部插件: playing_bg_image —— 播放页背景『热浪封面』+ 接管歌曲图 + 封面取色
///
/// 三件事（都由『拟声++』与用户在设置里的选择门控，未选中时零成本）：
///
/// 1. **播放页背景『热浪封面』**（渲染槽位 `player.background`）：
///    取当前歌曲封面（渲染项 `cover`：宿主解码 + 缩到 512 + 预模糊 34），
///    在着色器里**放大 2 倍**铺满屏幕、缓慢四处漂移，局部偶尔轻微扭曲 ——
///    类似夏天地面温度高、空气被折射时看到的轻微晃动。没有封面时回退成暗底 + 光斑。
/// 2. **接管播放页歌曲图**（渲染槽位 `player.icon`）：声明 `mode: "none"` + `keepSpace: true`
///    —— 不显示内容、只保留占位（热浪封面已经铺满画面，原来的封面圆盘与它重复）。
///    启用『热浪封面』时自动接管（每秒读一次状态镜像 `musicxx.state.renderSlots`，
///    判断背景槽位是不是自己在画），背景换回别的样式时自动交还。
/// 3. **封面取色**（钩子 `musicxx.media.palette.provide`）：自己算封面配色 ——
///    把封面缩到 64×64 读 RGBA，按 4 bit/通道量化统计，取出现次数最多的 4 个色点，
///    按亮度映射成 main / light / lightMuted / dark / darkMuted，替代内置的颜色分析。
///
/// 排查入口：宿主日志（`musicxx.host.log`）、设置页（`ext://playing_bg_image/settings`）、
/// 以及状态镜像里的 `player.background` / `player.icon` 两个槽位。
const kit = pluginxx.ui.kit;

const PLUGIN_ID = musicxx.pluginId;
const BG_ITEM_NAME = "bg";
const ICON_ITEM_NAME = "icon";
const HOME_ITEM_NAME = "card";
const BG_ITEM_ID = "plugin." + PLUGIN_ID + "." + BG_ITEM_NAME;
const ICON_ITEM_ID = "plugin." + PLUGIN_ID + "." + ICON_ITEM_NAME;
const BACKGROUND_SLOT = "player.background";
const ICON_SLOT = "player.icon";
/// 两个槽位的内置项 id：切回内置背景 / 交还歌曲图用
const BUILTIN_BG_ID = "builtin:Auto";
const BUILTIN_ICON_ID = "builtin:default";

const BG_TITLE = "热浪封面";
const ICON_TITLE = "不显示歌曲图（热浪封面配套）";

/// 封面纹理参数：宿主解码 → 缩放 → 可选预模糊（着色器只做放大、漂移与扭曲）
///
/// `blur` 是**纹理上的** sigma：纹理铺满屏幕后再放大 2 倍，屏幕上看到的模糊 ≈
/// `blur × max(屏幕宽/纹理宽, 屏幕高/纹理高) × 2` —— 512 的纹理在 1280 宽的窗口里
/// 大约放大 5 倍，所以 34 会糊成一片渐变（只剩色带），14 才能看出封面的大致内容。
/// 档位由设置页的「背景模糊程度」切换（存在 config.json）。
const COVER_TEXTURE = "uCover";
const COVER_INFO = "uCoverInfo";
const COVER_SIZE = 512;

/// 预模糊档位（清晰 / 标准 / 柔和）与默认档
const BLUR_OPTIONS = [6, 14, 28];
const BLUR_DEFAULT = 14;

/// 画面放大倍率档位与默认档
///
/// 倍率是"纹理在屏幕上的总体放大倍率"（含铺满所需的那部分）：3 = 纹理按 3 倍铺在屏幕上、
/// 超出裁掉，能看到封面约 1/3 的画面；数值越小看到的内容越完整（1 = 只铺满不放大）。
const ZOOM_OPTIONS = [2, 3, 5];
const ZOOM_DEFAULT = 3;

/// 取色用的封面尺寸与取几个色点
const PALETTE_COVER_SIZE = 64;
const PALETTE_TOP_COLORS = 4;

/* ==================== 插件配置（config.json，设置页按钮改它） ==================== */

/// 配置里的模糊档位（null = 还没读到，用默认档）
let configBlur = null;

/// 当前生效的模糊档位（配置没读到 / 值非法时用默认档）
function configuredBlur() {
    const value = configBlur;
    return (typeof value === "number" && BLUR_OPTIONS.indexOf(value) >= 0)
        ? value
        : BLUR_DEFAULT;
}

/// 模糊档位的显示名
function blurText(value) {
    if (value <= BLUR_OPTIONS[0]) {
        return "清晰（" + value + "）";
    }
    if (value <= BLUR_OPTIONS[1]) {
        return "标准（" + value + "）";
    }
    return "柔和（" + value + "）";
}

/// 应用一个模糊档位：重新登记背景样式（`updateEntry` 是整体替换），正在用的背景立即换图
function applyBlur(value) {
    configBlur = value;
    musicxx.ui.updateEntry(BG_ITEM_NAME, backgroundData());
    musicxx.host.log(2, "背景模糊程度 → " + blurText(value));
}

/// 配置里的放大倍率档位（null = 还没读到，用默认档）
let configZoom = null;

/// 当前生效的放大倍率档位（配置没读到 / 值非法时用默认档）
function configuredZoom() {
    const value = configZoom;
    return (typeof value === "number" && ZOOM_OPTIONS.indexOf(value) >= 0)
        ? value
        : ZOOM_DEFAULT;
}

/// 放大倍率的显示名
function zoomText(value) {
    return value + "×";
}

/// 应用一个放大倍率档位（同样通过重新登记背景样式生效）
function applyZoom(value) {
    configZoom = value;
    musicxx.ui.updateEntry(BG_ITEM_NAME, backgroundData());
    musicxx.host.log(2, "背景放大倍率 → " + zoomText(value));
}

/// 异步读配置（默认值由脚本给；读到之后再换成实际档位）
function refreshConfig() {
    musicxx.storage.getConfig("blur", BLUR_DEFAULT).then(function (value) {
        // 读取是异步的，结果可能后于用户操作才到：已经改过就不再覆盖
        if (configBlur !== null) {
            return;
        }
        const parsed = Number(value);
        const blur = (BLUR_OPTIONS.indexOf(parsed) >= 0) ? parsed : BLUR_DEFAULT;
        configBlur = blur;
        applyBlur(blur);
    }, function () { return null; });
    musicxx.storage.getConfig("zoom", ZOOM_DEFAULT).then(function (value) {
        if (configZoom !== null) {
            return;
        }
        const parsed = Number(value);
        const zoom = (ZOOM_OPTIONS.indexOf(parsed) >= 0) ? parsed : ZOOM_DEFAULT;
        configZoom = zoom;
        applyZoom(zoom);
    }, function () { return null; });
}
// 注册回放完成后再读配置（顶层同步注册阶段先不投递动作）
musicxx.timer.setTimeout(refreshConfig, 200);

/// 写配置项：先更新脚本里记下的值，再异步写 config.json（失败只记日志）
function saveConfig(key, value) {
    if (key === "blur") {
        configBlur = value;
    } else if (key === "zoom") {
        configZoom = value;
    }
    musicxx.storage.setConfig(key, value).then(function () {
        return null;
    }, function (err) {
        musicxx.host.log(3, "写配置失败: " + errText(err));
    });
}

/* ==================== UI 项声明 ==================== */

/// 背景样式的 UI 项内容
///
/// `musicxx.ui.updateEntry` 是整体替换，所以每次都要从这里取完整 data。
function backgroundData() {
    return {
        title: BG_TITLE,
        depict: "封面预模糊后放大 2 倍缓慢漂移，局部偶尔轻微扭曲；启用时会接管歌曲图（不显示）",
        shader: { bundle: "shader/heat.shaderbundle" },
        // 封面纹理：宿主缩到 512 并按当前档位预模糊，每帧绑定到 uCover；
        // (纹理宽, 纹理高, 原图宽高比, 是否有封面) 写进 uCoverInfo
        cover: {
            texture: COVER_TEXTURE,
            info: COVER_INFO,
            size: COVER_SIZE,
            blur: configuredBlur(),
        },
        // 两个绘制色跟着封面配色走（取色钩子提供的算法）；没有分析结果时用固定色
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
        },
        speed: 1,
        maxFps: 24,
        // 模糊画面不需要满分辨率：降采样后由宿主放大，省一半以上的像素
        resolutionScale: 0.6,
        animate: true,
    };
}

/// 接管歌曲图的 UI 项内容：不显示内容、保留占位
function iconData() {
    return {
        title: ICON_TITLE,
        depict: "接管播放页中间的歌曲图：保留占位、不画内容；背景换回其它样式时自动交还",
        mode: "none",
        keepSpace: true,
    };
}

musicxx.ui.registerEntry({
    name: BG_ITEM_NAME,
    type: "playing.background",
    order: 40,
    data: backgroundData(),
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
        title: "热浪封面背景",
        subtitle: "playing_bg_image：封面模糊放大 + 热浪扭曲，自带封面取色",
        // 图标名放在 data 里（`registerEntry` 只在没有 data 时才把顶层字段并进去）
        icon: "addition",
        action: { kind: "route", route: "ext://" + PLUGIN_ID + "/card" },
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

/// 取到的一组色点 → 宿主封面取色的 5 个字段（按亮度从亮到暗映射）
///
/// 数量不足时按已有的复用（只求"这套颜色来自封面"）。这里同时做一点调整：
/// 最亮的提亮一点、最暗的压暗一点，让内置背景与插件背景都有明暗层次可用。
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

/* ==================== 跟随背景自动接管歌曲图 ==================== */

/// 是否跟随『热浪封面』自动接管 / 交还歌曲图（设置页可以关掉）
let followBackground = true;

/// 手动接管 / 交还之后暂停跟随的时长（免得把用户刚做的选择立刻覆盖回去）
const FOLLOW_PAUSE_MS = 10 * 60 * 1000;

/// 暂停跟随到这个时刻（毫秒时间戳；0 = 没暂停）
let followPausedUntil = 0;

/// 最近一次槽位请求的目标与时间（同一个目标 30 秒内不重复请求：动作失败时不要每秒重试）
let lastRequestTarget = "";
let lastRequestAt = 0;

/// 用户手动接管 / 交还后的说明文本（设置页显示）
let lastLinkNote = "";

/// 读一个槽位现在是谁在画
function slotItemIdOf(slotId) {
    const slots = musicxx.state.get("musicxx.state.renderSlots") || {};
    const slot = slots[slotId] || null;
    return (slot && typeof slot.itemId === "string") ? slot.itemId : "";
}

/// 当前是不是本插件在画该槽位
function isSlotMine(slotId, itemId) {
    return slotItemIdOf(slotId) === itemId;
}

/// 请求切换一个槽位（异步动作，不等待结果）
///
/// [manual] 为真表示这是用户主动点的（设置页按钮）：之后暂停自动跟随一会儿，
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

/// 每秒检查一次：背景是『热浪封面』就接管歌曲图，背景不是了就交还
///
/// 用状态镜像判断（同步读，不产生动作往返）：容器里没有"槽位变化事件"，
/// 轮询 1 秒足够 —— 一次读 + 一次字符串比较。用户手动操作后 10 分钟内不自动跟随；
/// 同一个目标 30 秒内不重复请求（失败时不要每秒重试）。
function followTick() {
    if (!followBackground) {
        return;
    }
    const now = Date.now();
    if (now < followPausedUntil) {
        return;
    }
    const bgMine = isSlotMine(BACKGROUND_SLOT, BG_ITEM_ID);
    const iconMine = isSlotMine(ICON_SLOT, ICON_ITEM_ID);
    let target = "";
    let note = "";
    if (bgMine && !iconMine) {
        target = ICON_ITEM_ID;
        note = "跟随背景接管歌曲图";
    } else if (!bgMine && iconMine) {
        target = BUILTIN_ICON_ID;
        note = "背景已切走，交还歌曲图";
    }
    if (!target) {
        return;
    }
    if (target === lastRequestTarget && now - lastRequestAt < 30000) {
        return;
    }
    requestSlotSelect(ICON_SLOT, target, note, false);
}

musicxx.timer.setInterval(followTick, 1000);

/* ==================== 页面（说明页 + 设置页） ==================== */

function errText(err) {
    return (err && err.message) ? err.message : String(err);
}

/// 客户端能力段（宿主取页面时会放进参数）
function viewEnv(args) {
    if (args && typeof args.ui === "object" && args.ui !== null) {
        return args.ui;
    }
    return null;
}

/// 一个槽位的状态文本
function slotStateText(slotId, itemId) {
    const slots = musicxx.state.get("musicxx.state.renderSlots") || {};
    const slot = slots[slotId] || null;
    if (!slot) {
        return "无状态";
    }
    if (slot.itemId === itemId) {
        return (slot.visible === true) ? "生效中" : "生效中（当前不在画）";
    }
    if (!slot.selectedId) {
        return "未生效";
    }
    if (slot.selectedId === itemId && slot.itemId !== itemId) {
        return "已选中但未生效（检查中）";
    }
    return "未生效（当前：" + slot.selectedId + "）";
}

/// 取色状态文本
function paletteStateText() {
    const bound = musicxx.hooks.has("musicxx.media.palette.provide");
    const cachedKeys = Object.keys(paletteCache).length;
    return (bound ? "已注册" : "未注册") +
        " · 问过 " + paletteCalls + " 次（命中 " + paletteHits + "）" +
        " · 已写回 " + paletteReady + " 次" +
        " · 缓存 " + cachedKeys + " 首";
}

/// 设置页 / 说明页共用的状态区
function statusBlocks(env) {
    return [
        kit.card({
            children: [
                kit.listRow({
                    title: "播放页背景",
                    subtitle: "渲染槽位 player.background（状态镜像同步读）",
                    trailing: slotStateText(BACKGROUND_SLOT, BG_ITEM_ID),
                }, env),
                kit.listRow({
                    title: "播放页歌曲图",
                    subtitle: "渲染槽位 player.icon：接管后保留占位、不画内容",
                    trailing: slotStateText(ICON_SLOT, ICON_ITEM_ID),
                }, env),
                kit.settingRow({
                    title: "封面取色",
                    depict: "钩子 musicxx.media.palette.provide：统计封面里最多的 4 个色点",
                    value: paletteStateText(),
                }, env),
                kit.settingRow({
                    title: "背景模糊程度",
                    depict: "越小越看得清封面内容，越大越柔和（改完立即生效）",
                    value: blurText(configuredBlur()),
                }, env),
                kit.settingRow({
                    title: "背景放大倍率",
                    depict: "纹理在屏幕上的总体放大倍率：越小看到的内容越完整（3× 约看到封面 1/3）",
                    value: zoomText(configuredZoom()),
                }, env),
                kit.settingRow({
                    title: "跟随背景接管歌曲图",
                    depict: "开启时：背景是『" + BG_TITLE + "』就自动接管，背景切走就交还",
                    value: (!followBackground)
                        ? "关闭"
                        : ((Date.now() < followPausedUntil)
                            ? "暂停中（手动操作后 10 分钟）"
                            : "开启"),
                }, env),
                kit.settingRow({
                    title: "最近一次联动",
                    depict: "按钮或跟随逻辑的结果（异步动作，稍后刷新可见）",
                    value: lastLinkNote || "(无)",
                }, env),
            ],
        }, env),
    ];
}

/// 说明页（主页入口）
function cardView(args) {
    const env = viewEnv(args);
    return {
        title: "热浪封面背景",
        subtitle: "playing_bg_image：封面模糊放大 + 热浪扭曲，自带封面取色",
        blocks: [
            kit.hint({
                text: "• 把本插件的『" + BG_TITLE + "』设为播放页背景后，当前歌曲封面会被预模糊、放大 2 倍铺满屏幕，" +
                    "缓慢四处漂移，局部偶尔出现轻微扭曲（夏天地面热浪折射的感觉）；没有封面时回退成暗底 + 光斑。",
            }, env),
            kit.hint({
                text: "• 同时它会接管播放页中间的歌曲图（渲染槽位 player.icon，声明 mode: \"none\"）：" +
                    "保留原来的占位尺寸、不显示内容 —— 封面圆盘与铺满画面的热浪封面重复，留着会显得杂乱。" +
                    "背景换回其它样式后会自动交还（也可以在设置页手动接管 / 交还）。",
            }, env),
            kit.hint({
                text: "• 封面取色由本插件提供（钩子 musicxx.media.palette.provide）：把封面缩到 " +
                    PALETTE_COVER_SIZE + "×" + PALETTE_COVER_SIZE + " 读 RGBA，按 4 bit/通道统计出现次数最多的 " +
                    PALETTE_TOP_COLORS + " 个色点，按亮度映射成 main / light / lightMuted / dark / darkMuted。" +
                    "它替代内置的封面颜色分析，内置背景与插件背景都会用上这套颜色。",
            }, env),
            kit.divider({}, env),
            ...statusBlocks(env),
            kit.button({
                label: "使用『" + BG_TITLE + "』背景",
                variant: "primary",
                action: { kind: "dispatch", name: "useBackground", args: { view: "card" } },
            }, env),
            kit.button({
                label: "切回内置背景",
                action: { kind: "dispatch", name: "useBuiltinBackground", args: { view: "card" } },
            }, env),
            kit.button({
                label: "打开插件设置页",
                action: { kind: "route", route: "ext://" + PLUGIN_ID + "/settings" },
            }, env),
        ],
    };
}

/// 设置页：状态 + 全部开关与按钮
function settingsView(args) {
    const env = viewEnv(args);
    return {
        title: "热浪封面 · 设置",
        subtitle: "选择背景样式、接管歌曲图、查看取色状态",
        blocks: [
            kit.hint({
                text: "• 背景样式与内置样式占同一个槽位（player.background），设置里同时只能选中一个；" +
                    "本插件注册的样式默认不生效，需要在这里（或『设置 → 主题 → 播放页面背景』）选中。",
            }, env),
            ...statusBlocks(env),
            kit.button({
                label: "使用『" + BG_TITLE + "』背景",
                variant: "primary",
                action: { kind: "dispatch", name: "useBackground", args: { view: "settings" } },
            }, env),
            kit.button({
                label: "切回内置背景",
                action: { kind: "dispatch", name: "useBuiltinBackground", args: { view: "settings" } },
            }, env),
            kit.button({
                label: "切换背景模糊程度（清晰 / 标准 / 柔和）",
                action: { kind: "dispatch", name: "cycleBlur", args: { view: "settings" } },
            }, env),
            kit.button({
                label: "切换背景放大倍率（2× / 3× / 5×）",
                action: { kind: "dispatch", name: "cycleZoom", args: { view: "settings" } },
            }, env),
            kit.button({
                label: "立即接管歌曲图（不显示内容）",
                action: { kind: "dispatch", name: "takeIcon", args: { view: "settings" } },
            }, env),
            kit.button({
                label: "交还歌曲图（显示内置）",
                action: { kind: "dispatch", name: "releaseIcon", args: { view: "settings" } },
            }, env),
            kit.button({
                label: followBackground ? "关闭自动跟随（当前：开启）" : "开启自动跟随（当前：关闭）",
                action: { kind: "dispatch", name: "toggleFollow", args: { view: "settings" } },
            }, env),
            kit.button({
                label: "清空取色缓存",
                action: { kind: "dispatch", name: "clearPaletteCache", args: { view: "settings" } },
            }, env),
            kit.hint({
                text: "• 说明：接管歌曲图后，原来的\"点击歌曲图播放 / 暂停\"不再生效（整个显示都被替换了）；" +
                    "封面取色的结果缓存在插件内（按歌曲身份），换歌自动重算。",
            }, env),
        ],
    };
}

musicxx.capability.register("card", function (args) {
    return { view: cardView(args) };
});

musicxx.capability.register("settings", function (args) {
    return { view: settingsView(args) };
});

musicxx.capability.register("useBackground", function (args) {
    requestSlotSelect(
        BACKGROUND_SLOT, BG_ITEM_ID, "使用『" + BG_TITLE + "』背景", false);
    return {
        view: (args && args.view === "card") ? cardView(args) : settingsView(args),
    };
});

musicxx.capability.register("useBuiltinBackground", function (args) {
    requestSlotSelect(BACKGROUND_SLOT, BUILTIN_BG_ID, "切回内置背景", false);
    return {
        view: (args && args.view === "card") ? cardView(args) : settingsView(args),
    };
});

musicxx.capability.register("takeIcon", function (args) {
    requestSlotSelect(ICON_SLOT, ICON_ITEM_ID, "接管歌曲图", true);
    return { view: settingsView(args) };
});

musicxx.capability.register("releaseIcon", function (args) {
    requestSlotSelect(ICON_SLOT, BUILTIN_ICON_ID, "交还歌曲图", true);
    return { view: settingsView(args) };
});

musicxx.capability.register("toggleFollow", function (args) {
    followBackground = !followBackground;
    lastLinkNote = followBackground ? "自动跟随已开启" : "自动跟随已关闭";
    return { view: settingsView(args) };
});

musicxx.capability.register("clearPaletteCache", function (args) {
    paletteCache = {};
    paletteTasks = {};
    lastLinkNote = "取色缓存已清空";
    return { view: settingsView(args) };
});

/// 设置页按钮：循环切换背景模糊程度（清晰 → 标准 → 柔和 → 清晰）
///
/// 改完用 `musicxx.ui.updateEntry` 重新登记同一个背景样式（整体替换）：
/// 正在用的背景会换用新的预模糊纹理（宿主按新参数重新解码一次）。
musicxx.capability.register("cycleBlur", function (args) {
    const current = configuredBlur();
    let index = BLUR_OPTIONS.indexOf(current);
    if (index < 0) {
        index = BLUR_OPTIONS.indexOf(BLUR_DEFAULT);
    }
    const next = BLUR_OPTIONS[(index + 1) % BLUR_OPTIONS.length];
    saveConfig("blur", next);
    applyBlur(next);
    return { view: settingsView(args) };
});

/// 设置页按钮：循环切换背景放大倍率（2× → 3× → 5× → 2×）
///
/// 倍率是"纹理在屏幕上的总体放大倍率"：2× 看到的内容最完整，5× 最局部（接近早先固定
/// 写死 2.0 时的观感 —— 那个叠上铺满倍率后屏幕上其实约 5 倍，只剩封面中心一小块）。
musicxx.capability.register("cycleZoom", function (args) {
    const current = configuredZoom();
    let index = ZOOM_OPTIONS.indexOf(current);
    if (index < 0) {
        index = ZOOM_OPTIONS.indexOf(ZOOM_DEFAULT);
    }
    const next = ZOOM_OPTIONS[(index + 1) % ZOOM_OPTIONS.length];
    saveConfig("zoom", next);
    applyZoom(next);
    return { view: settingsView(args) };
});

musicxx.host.log(2, "playing_bg_image 已装载: 背景样式『" + BG_TITLE +
    "』+ 歌曲图接管项 + 封面取色钩子");
