/// example_js_playing_bg 的插件脚本: 配置、槽位跟随、能力入口与顶层注册
///
/// 这个插件演示"播放页背景"这一整条路：
/// - 六种播放页背景样式（封面三种 + 图案三种），见 `backgrounds.js`；
/// - 歌曲图内容的三种效果（只换图片，手势与形状裁剪仍由主程序画），见 `icons.js`；
/// - 封面取色钩子与自动调整播放页前景色，见 `palette.js`；
/// - 说明页 / 设置页 / 频谱页，见 `views.js`；
/// - 本文件：读写 config.json、跟随背景接管歌曲图、能力入口，以及**全部顶层注册**。
///
/// 为什么注册都放在最后：六个脚本在同一个 JS 上下文里按清单 `scripts` 的顺序执行，
/// 前面的文件只声明常量与函数；等到这里再统一注册，就不会出现"注册时用到的常量还没初始化"。
/// 顶层注册必须**同步**完成（顶层不能用 await），异步逻辑放到钩子或定时器里。
///
/// 设置（8 项，存在插件目录的 config.json）：模糊档位、放大倍数、图片占比、唱片转速、
/// 背景动画速率、热浪扭曲波纹、隐藏歌曲图、自动调整前景色。

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
const AUTO_FOREGROUND_DEFAULT = true;

/// 设置页上的一排候选项（精调可以手改 config.json）
const BLUR_CHOICES = [0, 8, 16, 24, 30];
const ZOOM_CHOICES = [1, 1.5, 2, 2.5, 3];
const SHARE_CHOICES = [0.45, 0.6, 0.75, 0.9];
const SPIN_CHOICES = [0.5, 1, 2];

/* ==================== 配置（config.json，设置页改它） ==================== */

/// 各配置项的默认值：读不到时用它（插件框架不保存默认值）
const CONFIG_DEFAULTS = {
    blur: BLUR_DEFAULT,
    zoom: ZOOM_DEFAULT,
    share: SHARE_DEFAULT,
    spin: SPIN_DEFAULT,
    bgRate: DEFAULT_BG_RATE,
    ripple: RIPPLE_DEFAULT,
    hideIcon: HIDE_ICON_DEFAULT,
    autoForeground: AUTO_FOREGROUND_DEFAULT,
};

/// 读到 / 用户改过的值（没读到时用 [CONFIG_DEFAULTS]）
let configValues = {};

/// 该键是否已经确定（读过或用户改过）
///
/// 异步读的结果可能晚于用户操作才到：已经确定的键不要再被读回来的旧值覆盖
/// （否则用户刚切到的 2x 会被读回来的默认值改回去）。
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
        case "bgRate":
            return normalizeBgRate(value);
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

function configuredBlur() {
    return configValue("blur");
}

/// 放大倍数（1~3，『模糊』与『沉浸』共用）
function configuredZoom() {
    return configValue("zoom");
}

/// 图片占比（只影响『沉浸』）
function configuredShare() {
    return configValue("share");
}

/// 唱片转速（只影响『黑胶』）
function configuredSpin() {
    return configValue("spin");
}

/// 背景动画速率（只影响三个图案样式）
function configuredBgRate() {
    return configValue("bgRate");
}

/// 热浪扭曲波纹开关
function configuredRipple() {
    return configValue("ripple");
}

/// 隐藏歌曲图开关
function configuredHideIcon() {
    return configValue("hideIcon");
}

/// 自动调整前景色开关
function configuredAutoForeground() {
    return configValue("autoForeground");
}

/// 异步读配置（默认值由脚本给；所有键都读完 / 失败后，再用实际值重新声明样式）
///
/// 放在定时器里读：顶层同步注册阶段不能投递动作（插件框架这时还在回放注册）。
function refreshConfig() {
    const keys = Object.keys(CONFIG_DEFAULTS);
    let pending = keys.length;
    function done() {
        --pending;
        if (0 === pending) {
            // 先用快照频道里的昼夜（主程序启动时推过一次），异步查到的值会再校正一次；
            // 顺带问一次"当前昼夜 + 封面主色"，"恢复上次的歌"时启动完就能上色
            if (null === isNightNow) {
                isNightNow = isNightFromMirror();
            }
            refreshBackgroundEntries();
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

/// 写一个配置项：先更新脚本里记下的值（页面立刻显示新值），再异步写 config.json
///
/// 六个背景样式立刻重新声明（改参数就是改声明：`register` 覆盖同名项，位置不变）：
/// 正在使用的样式马上换用新参数。
function saveConfig(key, value) {
    setConfigValue(key, value);
    refreshBackgroundEntries();
    musicxx.storage.setConfig(key, configValue(key)).then(function () {
        return null;
    }, function (err) {
        musicxx.host.log(3, "写配置失败(" + key + "): " + errText(err));
    });
}

/* ==================== 插件纹理（image 声明用的图片） ==================== */

/// 插件纹理的绑定状态（页面回读；动作是异步的，能力处理器里不能等它）
let boundImageState = "未绑定";

/// 把当前歌曲封面绑成纹理（异步发起，不等待）
///
/// `cover` 动作的 `bind` 参数：插件框架复用这一次解码出来的像素上传成 GPU 纹理，不重复解码。
/// 绑定后『插件纹理铺底』的 `musicxx.image.<key>` 纹理声明就能采样到它（图像表版本变化，背景下一帧用上新图）。
/// 绑定是临时资源：插件停用 / 卸载、关闭『拟声++』时插件框架自动摘掉。
///
/// [view] 是发起这次绑定的页面内容 id：结果要几秒后才到，到的时候再刷新那一页，
/// 不然页面上会一直停在"绑定中…"（下次点别的才变）。
function bindCoverImage(view) {
    boundImageState = "绑定中…";
    musicxx.media.cover({ size: 128, format: "rgba", bind: BOUND_IMAGE_KEY }).then(function (r) {
        if (r && r.ok === true && r.bound === true) {
            boundImageState = "已绑定 " + r.width + "x" + r.height;
            musicxx.host.log(2, "已把封面绑成纹理: " + boundImageState);
        } else {
            boundImageState = "失败: " + ((r && (r.bindError || r.error)) || "未知原因");
            musicxx.host.log(3, "绑定封面纹理失败: " + boundImageState);
        }
        refreshPageAfter(view);
    }, function (err) {
        boundImageState = "失败: " + errText(err);
        musicxx.host.log(3, "绑定封面纹理失败: " + boundImageState);
        refreshPageAfter(view);
    });
}

/// 释放绑定的纹理（异步发起，不等待；[view] 同 [bindCoverImage]）
function unbindCoverImage(view) {
    musicxx.media.unbindImage({ key: BOUND_IMAGE_KEY }).then(function (r) {
        boundImageState = (r && r.ok === true) ? "未绑定" : "解绑失败";
        musicxx.host.log(2, "释放插件纹理: " + boundImageState);
        refreshPageAfter(view);
    }, function (err) {
        boundImageState = "解绑失败: " + errText(err);
        musicxx.host.log(3, "释放插件纹理失败: " + boundImageState);
        refreshPageAfter(view);
    });
}

/* ==================== 封面变化处理器 ==================== */

/// 最近一次"封面变化"通知（演示"注册了就收到、随时可以移除"）
let lastCoverNotice = "(未收到)";

/// 封面变化处理器当前有没有注册（演示"运行期退订 / 重新订阅"）
let coverHookBound = true;

/// 封面变化处理器：插件框架在换歌 / 同一首歌换了封面图时派发一次
///
/// 载荷 = `{srcKey, name, artist, hasCover, kind}`（`kind` = local/cache/content/asset/network），
/// **没有直链也没有图片字节** —— 要图片自己调 `musicxx.media.cover`，要在着色器里采样就在
/// 着色器块的 `args` 里声明纹理（`musicxx.cover` / `musicxx.image.<key>`）。观察型：返回值忽略，处理器出错也不会影响播放。
/// 这里同时用它预热取色（见 [paletteOnCoverChanged]）。
function onCoverChanged(payload) {
    const name = (payload && payload.name) ? payload.name : "未知歌曲";
    const kind = (payload && payload.hasCover === true) ? (payload.kind || "有封面") : "无封面";
    lastCoverNotice = name + " · " + kind;
    musicxx.host.log(2, "封面变化: " + JSON.stringify(payload || {}));
    paletteOnCoverChanged(payload);
}

/// 绑定 / 解除封面变化处理器
///
/// 解除（`unsubscribe`）后插件框架不再派发，取色也不会提前预热（应用来问时才算）；
/// 重新注册即可恢复。插件停用 / 卸载时插件框架也会自动摘掉。
function setCoverHookBound(bound) {
    if (bound === coverHookBound) {
        return;
    }
    if (bound) {
        musicxx.broadcast.subscribe("musicxx.media.cover.changed", onCoverChanged);
    } else {
        musicxx.broadcast.unsubscribe("musicxx.media.cover.changed");
    }
    coverHookBound = bound;
    musicxx.host.log(2, "封面变化处理器: " + (bound ? "已绑定" : "已移除"));
}

/* ==================== 槽位切换与跟随 ==================== */

/// 手动接管 / 交还之后暂停自动跟随的时长（免得把用户刚做的选择立刻覆盖回去）
const FOLLOW_PAUSE_MS = 10 * 60 * 1000;

/// 暂停跟随到这个时刻（毫秒时间戳；0 = 没暂停）
let followPausedUntil = 0;

/// 最近一次槽位请求的目标与时间（同一个目标 30 秒内不重复请求：动作失败时不要每秒重试）
let lastRequestTarget = "";
let lastRequestAt = 0;

/// 最近一次操作 / 跟随的说明文本（页面显示）
let lastLinkNote = "";

/// 最近一次"一键使用"请求的样式 id
///
/// 切换要经过一次动作往返（`musicxx.ui.slot.set`），页面却是先于动作结果画出来的：
/// 这里先记住请求，让页面当场就能看到"点了有反应"；镜像一反映这次请求就把它清掉。
/// 动作结束后还会再刷新一次页面（见 [requestSlotSelect] 的 `view` 参数），
/// 那次画到的就是更新过的镜像。
let lastBackgroundRequest = "";

/// 现在由哪个**插件项**在画该槽位（空 = 没有插件项在画）
function slotItemIdOf(slotId) {
    const slot = slotMirrorOf(slotId);
    return (slot && typeof slot.itemId === "string") ? slot.itemId : "";
}

/// 用户选中了谁（可能是内置项 id：两个槽位的内置项都是 `musicxx.*`）
function slotSelectedIdOf(slotId) {
    const slot = slotMirrorOf(slotId);
    return (slot && typeof slot.selectedId === "string") ? slot.selectedId : "";
}

/// 背景槽位现在是不是本插件的样式在画
function backgroundIsMine() {
    return "" !== styleOfItem(slotItemIdOf(BACKGROUND_SLOT));
}

/// 请求切换一个槽位（异步动作，不等待结果）
///
/// [manual] 为真表示这是用户主动点的（页面上的按钮）：之后暂停自动跟随一会儿，
/// 免得跟随逻辑把用户刚做的选择又改回去。
///
/// [view] 是发起这次切换的页面内容 id（空 = 没有页面，定时跟随就是这种）。
/// 动作结束（成功或失败）后会再刷新那一页：镜像 `musicxx.state.uiSlots` 要等这次往返
/// 走完才更新，处理器里紧接着的那次刷新画到的还是旧的 `selectedId` / `itemId`，
/// 页面上的选中高亮会停在上一项，直到用户再点一次才会显示成新的。
function requestSlotSelect(slotId, itemId, note, manual, view) {
    musicxx.host.log(2, "收到槽位切换请求: " + slotId + " → " + itemId);
    lastRequestTarget = itemId;
    lastRequestAt = Date.now();
    if (manual) {
        followPausedUntil = Date.now() + FOLLOW_PAUSE_MS;
    }
    if (BACKGROUND_SLOT === slotId) {
        lastBackgroundRequest = itemId;
    }
    lastLinkNote = note;
    musicxx.ui.slot.set(itemId, { slot: slotId }).then(function (result) {
        if (result && result.ok === true) {
            lastLinkNote = note + "（已生效）";
            musicxx.host.log(2, "槽位切换成功: " + slotId + " → " + itemId);
        } else {
            lastLinkNote = note + "失败：" + ((result && result.error) || "未知原因");
            musicxx.host.log(3, "槽位切换失败: " + JSON.stringify(result || {}));
        }
        // 镜像同步更新过了：不再需要"已请求"提示
        lastBackgroundRequest = "";
        refreshPageAfter(view);
    }, function (err) {
        lastLinkNote = note + "失败：" + errText(err);
        lastBackgroundRequest = "";
        musicxx.host.log(3, "槽位切换失败: " + errText(err));
        refreshPageAfter(view);
    });
}

/// 每秒检查一次：打开「隐藏歌曲图」且背景是本插件的样式时才接管，否则交还
///
/// 用快照频道判断（同步读，不产生动作往返）：没有"槽位变化事件"，轮询 1 秒足够 ——
/// 一次读 + 一次字符串比较。用户手动操作后 10 分钟内不自动跟随；同一个目标 30 秒内不重复
/// 请求（失败时不要每秒重试）。
///
/// 前景色不在这里处理：它由 `installForegroundSubscriptions` 订阅变化驱动。
///
/// [force] 为真时跳过暂停与去重（设置页刚改完开关，希望当场看到结果）。
/// 判断写成 `true != force`：定时器回调不带参数（force = undefined），
/// 用 `false == force` 会让"没传参数"被当成"强制"，暂停与去重就都失效了。
///
/// [view] 是发起这次检查的页面内容 id：用户点了开关时由设置页传进来，
/// 切换结果要回到那一页上（定时器调用时不传，没有页面要刷新）。
function followTick(force, view) {
    const now = Date.now();
    if (true != force && now < followPausedUntil) {
        return;
    }
    const bgMine = backgroundIsMine();
    const iconMine = slotItemIdOf(ICON_SLOT) === ITEM_ID_ICON;
    let target = "";
    let note = "";
    if (configuredHideIcon() && bgMine && false == iconMine) {
        target = ITEM_ID_ICON;
        note = "已隐藏歌曲图";
    } else if ((false == configuredHideIcon() || false == bgMine) && iconMine) {
        target = PREVIOUS_ICON_ID;
        note = configuredHideIcon() ? "背景已切走，交还" : "已关闭开关，交还";
    }
    if (!target) {
        return;
    }
    if (true != force && target === lastRequestTarget && now - lastRequestAt < 30000) {
        return;
    }
    requestSlotSelect(ICON_SLOT, target, note, false, view);
}

/* ==================== 页面内容 ==================== */

/// 三个页面都是 `musicxx.ui.page` 上的内容（地址 `ext://<插件 id>/<内容 id>`）
///
/// 频读页的动作会把结果记到脚本里，生成函数只负责把上一次的结果画出来。
function registerPages() {
    musicxx.ui.content.register({
        id: HOME_VIEW_ID,
        slot: "musicxx.ui.page",
        title: PLUGIN_TITLE,
        depict: "六种背景样式与歌曲图接管的说明",
    }, function () {
        return cardView({ view: HOME_VIEW_ID });
    });
    musicxx.ui.content.register({
        id: SETTINGS_VIEW_ID,
        slot: "musicxx.ui.page",
        title: PLUGIN_TITLE + " · 设置",
    }, function () {
        return settingsView({ view: SETTINGS_VIEW_ID });
    });
    musicxx.ui.content.register({
        id: "spectrumProbe",
        slot: "musicxx.ui.page",
        title: PLUGIN_TITLE + " · 频谱读取",
    }, function () {
        return spectrumProbeView({ view: "spectrum" });
    });
}

/// 兼容旧客户端的取视图入口（主程序新版不调它，页面读 `musicxx.ui.page` 上的内容）
///
/// 删除时间：主程序删掉 `callPluginView` 的那一步。
musicxx.capability.register("card", function (args) {
    return { view: cardView(args) };
});

musicxx.capability.register("settings", function (args) {
    return { view: settingsView(args) };
});
/// 动作改完状态后让发起的那一页重出一份内容
function refreshPage(args) {
    const view = (args && typeof args.view === "string" && args.view)
        ? args.view : SETTINGS_VIEW_ID;
    musicxx.ui.slot.update("musicxx.ui.page", [view]);
    return { ok: true };
}

/// 异步动作结束后刷新发起它的那一页（[view] 为空则什么都不做）
///
/// 能力的回答是异步的：处理器里立刻刷新，画到的是动作还没落地时的状态（槽位镜像里的
/// 选中项、纹理绑定状态都还是旧的），要等下一次刷新才会变。所以处理器发起动作时记下
/// 自己的页面内容 id，等结果回来（成功或失败）再让那一页重出一份。
/// 定时器里发起的动作（跟随背景接管歌曲图）没有页面，不传就行。
function refreshPageAfter(view) {
    if (typeof view !== "string" || "" === view) {
        return;
    }
    refreshPage({ view: view });
}

/* ==================== 能力入口 ==================== */

/// 注册本插件的能力（页面 / 按钮都走这里，处理器一律同步返回）
function installPluginCapabilities() {
    /// 自检：确认七个脚本都执行过（顶层不报错、样式、歌曲图内容与页面都在）
    ///
    /// 七个脚本共用一个 JS 上下文：有一个脚本没跑完（报错 / 清单漏了），这里的计数就对不上。
    musicxx.capability.register("probe", function () {
        return {
            pluginId: PLUGIN_ID,
            styles: Object.keys(ITEM_OF_STYLE).length,
            // 歌曲图内容（只换图片的那个槽位）的候选数
            iconImageStyles: ICON_IMAGE_ENTRIES.length,
            uiEntries: musicxx.ui.item.entries().length,
            uiContents: musicxx.ui.content.entries().length,
            // 数字（1 / 0）：原生测试按整数字段读（布尔会被当成 0）
            paletteHook: musicxx.capability.has("musicxx.media.palette.analyse") ? 1 : 0,
            hasSettingsView: (typeof settingsView === "function") ? 1 : 0,
            hasSpectrumView: (typeof spectrumProbeView === "function") ? 1 : 0,
            bgRate: configuredBgRate(),
        };
    });

    /// 说明页 / 设置页不再注册能力：它们已经是 `musicxx.ui.page` 上的内容（见 [registerPages]）

    /// 频谱页：先发起新的读取，再让那一页重出一份
    ///
    /// 顺序很重要：反过来的话页面读到的 `pending` 永远是自己刚设的那一次，
    /// 只能一直显示"读取中"。
    musicxx.capability.register("spectrumProbe", function (args) {
        // 顺序：先按上一次的结果取视图，再发起新的读取（反过来画到的永远是"读取中"）
        const view = spectrumProbeView(args);
        refreshSpectrumAction();
        refreshPage((args && typeof args.view === "string")
            ? args : { view: "spectrumProbe" });
        // 兼容旧客户端：返回值里也带视图
        return { view: view };
    });

    /// 一键使用某个样式（也可以切到内置样式：id 用 `musicxx.auto`）
    ///
    /// `musicxx.ui.slot.set` 是主程序官方动作：只允许选中**可用**项，选中后立即生效，
    /// 用户在设置里随时能改回来。处理器必须同步返回，所以这里只说明"已请求"，
    /// 实际结果看页面刷新后的状态（或 mirror 里的 `player.background`）。
    musicxx.capability.register("useBackground", function (args) {
        const id = (args && args.id) ? String(args.id) : ITEM_ID_BLUR;
        const style = styleOfItem(id);
        requestSlotSelect(
            BACKGROUND_SLOT,
            id,
            style ? ("切到" + STYLE_SHORT[style]) : "切换播放页背景",
            false,
            (args && typeof args.view === "string") ? args.view : "");
        return refreshPage(args);
    });

    /// 循环切换背景动画速率（0.5x → 1x → 2x → 0.5x），改完立即生效
    musicxx.capability.register("cycleBackgroundRate", function (args) {
        const current = configuredBgRateValue();
        let index = 0;
        for (let i = 0; i < BG_RATE_OPTIONS.length; ++i) {
            if (BG_RATE_OPTIONS[i] === current) {
                index = i;
            }
        }
        const next = BG_RATE_OPTIONS[(index + 1) % BG_RATE_OPTIONS.length];
        saveConfig("bgRate", next);
        return refreshPage(args);
    });

    /// 控件值变化统一入口：控件把自己的 `id` 与当前值带进参数
    ///
    /// 样式选择走槽位动作（`musicxx.ui.slot.set`，异步）；其余几项写 config.json 并让
    /// 背景内容重出一份（`musicxx.ui.slot.update`），否则画面还停在旧参数上。
    /// 异步的那几项（切样式 / 跟随歌曲图 / 查封面配色）在动作返回后再刷一次页，
    /// 处理器里那次刷新早于结果，画到的还是旧状态。
    musicxx.capability.register("setOption", function (args) {
        const id = (args && typeof args.id === "string") ? args.id : "";
        const value = args ? args.value : null;
        const view = (args && typeof args.view === "string") ? args.view : "";
        if (id === "mode") {
            const style = String(value || "");
            if (ITEM_OF_STYLE[style]) {
                requestSlotSelect(BACKGROUND_SLOT, ITEM_OF_STYLE[style], "切到" + STYLE_SHORT[style], false, view);
            } else if (style === STYLE_BUILTIN) {
                requestSlotSelect(BACKGROUND_SLOT, BUILTIN_BG_ID, "切回内置背景", false, view);
            }
            return refreshPage(args);
        }
        if (id === "iconImage") {
            // 歌曲图内容：只换歌曲图里的图片，手势与形状裁剪仍由主程序画
            const style = String(value || "");
            if (ITEM_OF_ICON_IMAGE[style]) {
                requestSlotSelect(ICON_IMAGE_SLOT, ITEM_OF_ICON_IMAGE[style],
                    "歌曲图内容切到" + ICON_IMAGE_SHORT[style], false, view);
            } else if (style === ICON_IMAGE_BUILTIN) {
                requestSlotSelect(ICON_IMAGE_SLOT, ICON_IMAGE_ORIGINAL_ID,
                    "歌曲图内容回到原图", false, view);
            }
            return refreshPage(args);
        }
        if (id === "blur" || id === "zoom" || id === "share" || id === "spin" ||
            id === "bgRate" || id === "ripple" || id === "hideIcon" || id === "autoForeground") {
            saveConfig(id, value);
            // 关掉「隐藏歌曲图」时立刻交还，不用等下一秒的跟随检查
            if (id === "hideIcon") {
                followTick(true, view);
            }
            // 打开「自动调整前景色」时立刻查一次封面主色与昼夜，不用等下一次定期检查
            if (id === "autoForeground") {
                refreshForegroundState().then(function () {
                    refreshPageAfter(view);
                });
            }
            refreshBackgroundEntries();
            return refreshPage(args);
        }
        musicxx.host.log(3, "未知的设置项: " + id);
        return refreshPage(args);
    });

    /// 把当前封面绑成纹理（异步发起，处理器同步返回）
    musicxx.capability.register("bindCoverTexture", function (args) {
        bindCoverImage((args && typeof args.view === "string") ? args.view : "");
        return refreshPage(args);
    });

    /// 释放绑定的纹理
    musicxx.capability.register("unbindCoverTexture", function (args) {
        unbindCoverImage((args && typeof args.view === "string") ? args.view : "");
        return refreshPage(args);
    });

    /// 绑定 / 解除封面变化处理器（演示运行期退订与重新订阅）
    musicxx.capability.register("toggleCoverHook", function (args) {
        setCoverHookBound(!coverHookBound);
        return refreshPage(args);
    });

    /// 立即接管歌曲图（手动；之后 10 分钟不自动跟随，免得被跟随逻辑改回去）
    musicxx.capability.register("takeIcon", function (args) {
        requestSlotSelect(ICON_SLOT, ITEM_ID_ICON, "手动隐藏", true,
            (args && typeof args.view === "string") ? args.view : "");
        return refreshPage(args);
    });

    /// 交还歌曲图（手动，恢复主程序记下的上一个形状；没有记录时保持用户现在的形状）
    musicxx.capability.register("releaseIcon", function (args) {
        requestSlotSelect(ICON_SLOT, PREVIOUS_ICON_ID, "手动交还", true,
            (args && typeof args.view === "string") ? args.view : "");
        return refreshPage(args);
    });

    /// 清空取色缓存（下次换歌 / 换背景样式时重新按封面算一遍）
    musicxx.capability.register("clearPaletteCache", function (args) {
        paletteCache = {};
        paletteTasks = {};
        lastLinkNote = "取色缓存已清空";
        return refreshPage(args);
    });
}

/* ==================== 顶层注册（必须是最后一步，且同步完成） ==================== */

/// 六种背景样式 + 三种歌曲图内容 + 歌曲图接管 + 主页入口
registerPluginEntries();

/// 三种歌曲图内容（渲染槽位 player.icon.image）
registerIconImageEntries();

/// 三个插件页面（`musicxx.ui.page` 上的内容）
registerPages();

/// 能力：说明页 / 设置页 / 频谱页 / 一键切换 / 设置项 / 纹理与处理器 / 歌曲图
installPluginCapabilities();

/// 封面取色钩子：应用要为当前歌曲封面取色时问一次（处理器同步返回，见 palette.js）
musicxx.capability.implement("musicxx.media.palette.analyse", paletteAnalyseVerdict);

/// 封面变化（观察型）：换歌时先把颜色算起来，等应用来问时通常已经命中
musicxx.broadcast.subscribe("musicxx.media.cover.changed", onCoverChanged);

/// 自动前景色：订阅样式 / 昼夜 / 封面三类变化，变化时推一次三色（不轮询）
installForegroundSubscriptions();

/// 读配置：默认值先按声明用，读到 config.json 后再重新声明一遍
musicxx.timer.setTimeout(refreshConfig, 200);

/// 每秒检查一次：跟随背景接管 / 交还歌曲图（前景色不靠这里的轮询）
musicxx.timer.setInterval(followTick, 1000);

/// 启动后预热一次取色：应用启动时可能已经有一首歌在放
musicxx.timer.setTimeout(function () {
    warmPalette("");
}, 2000);

/// 频谱页的动作结果先读一次：第一次打开这一页时通常已经就位
refreshSpectrumAction();

musicxx.host.log(2, PLUGIN_TITLE + "（" + PLUGIN_ID + "）已加载：六种播放页背景（" +
    STYLE_TITLES[STYLE_BLUR] + " / " + STYLE_TITLES[STYLE_CARD] + " / " +
    STYLE_TITLES[STYLE_VINYL] + " / " + STYLE_TITLES[STYLE_LATTICE] + " / " +
    STYLE_TITLES[STYLE_RING] + " / " + STYLE_TITLES[STYLE_BOUND] +
    "）+ 歌曲图接管 + 歌曲图内容（热浪 / 油画 / 像素）+ 封面取色钩子 + 自动调整前景色");
