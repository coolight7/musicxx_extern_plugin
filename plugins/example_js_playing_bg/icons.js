/// example_js_playing_bg 的歌曲图内容脚本：播放页歌曲图里的三种效果
///
/// 这个槽位（渲染槽位 `player.icon.image`，UI 类型 `musicxx.ui.playing.icon.image`）与
/// 『隐藏歌曲图』用的那个（`player.icon`）不是一回事：
/// - `player.icon`：整块接管，盒子、手势、形状都由插件画；
/// - `player.icon.image`：只换歌曲图**里面那张图**，盒子尺寸、形状裁剪（圆形 / 圆角 / 自适应）、
///   描边、加载提示、点击播放 / 暂停与拖动切歌都还是主程序画。
///
/// 三种效果都是一块 `musicxx.Shader`：纹理声明成 `musicxx.cover`（当前歌曲图，方形裁剪），
/// 没有封面时着色器输出透明，主程序画的默认图照常显示。
///
/// 这个文件只声明常量与注册函数，顶层注册仍放在 plugin.js 最后执行（见插件目录的 README）。

/// 渲染槽位 id（能力与广播里的键）与内容槽位的 UI 类型
const ICON_IMAGE_SLOT = "player.icon.image";
const ICON_IMAGE_SLOT_TYPE = "musicxx.ui.playing.icon.image";

/// 内置候选『原图』：选中它就是清空这个槽位的插件选择
const ICON_IMAGE_ORIGINAL_ID = "musicxx.original";

/// 三种效果的名字（设置页按钮值与状态判断都用它，不用到处写字符串）
const ICON_IMAGE_HEAT = "heat";
const ICON_IMAGE_OIL = "oil";
const ICON_IMAGE_PIXEL = "pixel";
/// 不是本插件的效果：原图（内置）/ 别人的样式
const ICON_IMAGE_BUILTIN = "builtin";
const ICON_IMAGE_OTHER = "other";

/// 内容短名（完整 id = plugin.<插件 id>.<短名>，由插件框架拼）
const ITEM_NAME_ICON_HEAT = "iconHeat";
const ITEM_NAME_ICON_OIL = "iconOil";
const ITEM_NAME_ICON_PIXEL = "iconPixel";

const ITEM_ID_ICON_HEAT = "plugin." + PLUGIN_ID + "." + ITEM_NAME_ICON_HEAT;
const ITEM_ID_ICON_OIL = "plugin." + PLUGIN_ID + "." + ITEM_NAME_ICON_OIL;
const ITEM_ID_ICON_PIXEL = "plugin." + PLUGIN_ID + "." + ITEM_NAME_ICON_PIXEL;

/// 效果 → 内容 id / 内容 id → 效果
const ITEM_OF_ICON_IMAGE = {};
ITEM_OF_ICON_IMAGE[ICON_IMAGE_HEAT] = ITEM_ID_ICON_HEAT;
ITEM_OF_ICON_IMAGE[ICON_IMAGE_OIL] = ITEM_ID_ICON_OIL;
ITEM_OF_ICON_IMAGE[ICON_IMAGE_PIXEL] = ITEM_ID_ICON_PIXEL;

const ICON_IMAGE_OF_ITEM = {};
ICON_IMAGE_OF_ITEM[ITEM_ID_ICON_HEAT] = ICON_IMAGE_HEAT;
ICON_IMAGE_OF_ITEM[ITEM_ID_ICON_OIL] = ICON_IMAGE_OIL;
ICON_IMAGE_OF_ITEM[ITEM_ID_ICON_PIXEL] = ICON_IMAGE_PIXEL;

const ICON_IMAGE_TITLES = {};
ICON_IMAGE_TITLES[ICON_IMAGE_HEAT] = PLUGIN_TITLE + " · 歌曲图热浪";
ICON_IMAGE_TITLES[ICON_IMAGE_OIL] = PLUGIN_TITLE + " · 歌曲图油画";
ICON_IMAGE_TITLES[ICON_IMAGE_PIXEL] = PLUGIN_TITLE + " · 歌曲图像素";

const ICON_IMAGE_SHORT = {};
ICON_IMAGE_SHORT[ICON_IMAGE_HEAT] = "热浪";
ICON_IMAGE_SHORT[ICON_IMAGE_OIL] = "油画";
ICON_IMAGE_SHORT[ICON_IMAGE_PIXEL] = "像素";

const ICON_IMAGE_DEPICTS = {};
ICON_IMAGE_DEPICTS[ICON_IMAGE_HEAT] =
    "当前歌曲图上叠一层向上升起的热气流扰动与轻微色散（着色器画，幅度可调）";
ICON_IMAGE_DEPICTS[ICON_IMAGE_OIL] =
    "按四个方向取局部色块最平滑的一块（Kuwahara 式），再提彩色、压色阶，像一层层颜料";
ICON_IMAGE_DEPICTS[ICON_IMAGE_PIXEL] =
    "按短边切成小方块、每块只取一次颜色并压到几档色阶，像老掌机的像素画";

/// 歌曲图纹理的最长边：图标盒子通常只有几百像素见方，512 够用
const ICON_IMAGE_COVER_SIZE = 512;

/// 一张**当前歌曲图**的纹理声明（方形裁剪成与图标盒子同形）
///
/// `smooth: false` 时用最近邻取样：像素风要硬边，线性过滤会把方块糊成马赛克过渡。
function iconImageCoverTexture(smooth) {
    const decl = coverTexture(ICON_IMAGE_COVER_SIZE, 0, true);
    if (false === smooth) {
        decl.smooth = false;
    }
    return decl;
}

/// 歌曲图内容『热浪』的声明
///
/// `uStrength` 是波动幅度倍率（1 = 基准、0 = 静止）：改它不用重做着色器。
/// 帧率给 24：扰动是缓慢的，但色散边缘要跟手，帧率太低会一顿一顿。
function iconHeatData() {
    return {
        title: ICON_IMAGE_TITLES[ICON_IMAGE_HEAT],
        depict: ICON_IMAGE_DEPICTS[ICON_IMAGE_HEAT],
        view: backgroundView("shader/icon_heat.shaderbundle", {
            uCover: iconImageCoverTexture(true),
            uStrength: { kind: "const", value: 1 },
        }, {
            speed: 1,
            maxFps: 24,
            resolutionScale: 1,
        }),
    };
}

/// 歌曲图内容『油画』的声明（静态画面：只要图片不变就不用重画）
function iconOilData() {
    return {
        title: ICON_IMAGE_TITLES[ICON_IMAGE_OIL],
        depict: ICON_IMAGE_DEPICTS[ICON_IMAGE_OIL],
        view: backgroundView("shader/icon_oil.shaderbundle", {
            uCover: iconImageCoverTexture(true),
        }, {
            speed: 1,
            // 16 次取样的滤镜：静态画面不需要每帧都算
            maxFps: 12,
            animate: false,
            resolutionScale: 1,
        }),
    };
}

/// 歌曲图内容『像素』的声明
///
/// `uPixel` 是短边切多少块（4~128）：数越小块越大、越"像素"。
function iconPixelData() {
    return {
        title: ICON_IMAGE_TITLES[ICON_IMAGE_PIXEL],
        depict: ICON_IMAGE_DEPICTS[ICON_IMAGE_PIXEL],
        view: backgroundView("shader/icon_pixel.shaderbundle", {
            // 像素风要硬边：最近邻取样
            uCover: iconImageCoverTexture(false),
            uPixel: { kind: "const", value: 40 },
        }, {
            speed: 1,
            maxFps: 12,
            animate: false,
            resolutionScale: 1,
        }),
    };
}

/// 三种效果的声明表：短名 + 排序 + 取声明的函数
const ICON_IMAGE_ENTRIES = [
    { name: ITEM_NAME_ICON_HEAT, style: ICON_IMAGE_HEAT, order: 50, data: iconHeatData },
    { name: ITEM_NAME_ICON_OIL, style: ICON_IMAGE_OIL, order: 51, data: iconOilData },
    { name: ITEM_NAME_ICON_PIXEL, style: ICON_IMAGE_PIXEL, order: 52, data: iconPixelData },
];

/// 注册一个歌曲图内容（名字与说明在注册时给：用户选中之前候选列表就要显示它们）
function registerIconImageContent(entry) {
    const data = entry.data();
    musicxx.ui.content.register({
        id: entry.name,
        slot: ICON_IMAGE_SLOT_TYPE,
        title: data.title,
        depict: data.depict,
        order: entry.order,
    }, function () {
        return entry.data().view;
    });
}

/// 顶层注册三种歌曲图内容（由 plugin.js 在最后调用）
function registerIconImageEntries() {
    for (let i = 0; i < ICON_IMAGE_ENTRIES.length; ++i) {
        registerIconImageContent(ICON_IMAGE_ENTRIES[i]);
    }
}

/// 当前歌曲图内容：`heat` / `oil` / `pixel` 之一，或者 `builtin` / `other`
///
/// 已经在画的看 `itemId`（现在由谁在画），还没生效的看 `selectedId`（用户选中了谁）：
/// 切换要经过一次动作往返，先看后者，按钮才算"点得动、有反应"。
function currentIconImageStyle() {
    const slot = slotMirrorOf(ICON_IMAGE_SLOT) || {};
    const itemStyle = ICON_IMAGE_OF_ITEM[slot.itemId || ""];
    if (itemStyle) {
        return itemStyle;
    }
    const selectedStyle = ICON_IMAGE_OF_ITEM[slot.selectedId || ""];
    if (selectedStyle) {
        return selectedStyle;
    }
    const selected = slot.selectedId || "";
    if (selected === "" || selected === ICON_IMAGE_ORIGINAL_ID ||
        selected.indexOf("musicxx.") === 0) {
        return ICON_IMAGE_BUILTIN;
    }
    return ICON_IMAGE_OTHER;
}

/// 歌曲图内容槽位的状态文本（设置页 / 说明页的状态行）
function iconImageStateText() {
    const slot = slotMirrorOf(ICON_IMAGE_SLOT) || {};
    if (!slot || Object.keys(slot).length === 0) {
        return "无状态";
    }
    const current = ICON_IMAGE_OF_ITEM[slot.itemId || ""];
    if (current) {
        return (slot.visible === true) ? "生效中" : "已选中 · 未渲染";
    }
    const selected = ICON_IMAGE_OF_ITEM[slot.selectedId || ""];
    if (selected) {
        return "已选中";
    }
    if (!slot.selectedId || slot.selectedId.indexOf("musicxx.") === 0) {
        return "原图";
    }
    return "别的样式";
}
