/// example_js_playing_slots 的插件脚本: 配置、能力入口与顶层注册
///
/// 这个插件演示"插件在哪些位置能放自己的界面"：
/// - 插入块（歌词页顶部 / 播放列表页 header / 更多信息页 footer）与按钮级替换 → slots.js；
/// - 普通页面背景（歌曲页面的封面背景）→ slots.js 的 [pageCoverEntryData]；
/// - 音频动效（渲染槽位 `player.visualizer`）→ visualizer.js；
/// - 位置上的视图与两个页面 → views.js；
/// - 本文件：配置读写、能力入口，以及**全部顶层注册**（必须同步完成，放在最后）。
///
/// 设置（2 项 + 动效选择，存在插件目录的 config.json）：整栏替换、歌词页顶部提示。

/// 错误对象 → 文本（异步回调里的失败只记日志）
function errText(err) {
    return (err && err.message) ? err.message : String(err);
}

/// 两个配置项的默认值
const CONFIG_DEFAULTS = {
    wholeBar: false,
    lyricTip: true,
};

/// 读到 / 用户改过的值（没读到时用 [CONFIG_DEFAULTS]）
let slotConfigValues = {};

/// 该键是否已经确定（读过或用户改过）：异步读的结果不能盖掉用户刚做的选择
const slotConfigSettled = {};

/// 开关配置：接受布尔与 "true"/"1" 这类写法（手改 config.json 时写字符串也能用）
function slotConfigBool(value, def) {
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

/// 当前配置值
function slotConfigValue(key) {
    return (key in slotConfigValues) ? slotConfigValues[key] : CONFIG_DEFAULTS[key];
}

function setSlotConfigValue(key, value) {
    slotConfigValues[key] = slotConfigBool(value, CONFIG_DEFAULTS[key]);
    slotConfigSettled[key] = true;
}

/// 整条控制按钮栏是否由本插件画
function configWholeBar() {
    return slotConfigValue("wholeBar");
}

/// 歌词页顶部提示是否显示（控制这一项注册与否）
function configLyricTip() {
    return slotConfigValue("lyricTip");
}

/// 写一个配置项：先更新脚本里记下的值（页面立刻显示新值），再异步写 config.json
function saveSlotConfig(key, value) {
    setSlotConfigValue(key, value);
    musicxx.storage.setConfig(key, slotConfigValue(key)).then(function () {
        return null;
    }, function (err) {
        musicxx.host.log(3, "写配置失败(" + key + "): " + errText(err));
    });
}

/// 异步读配置（放在定时器里读：顶层注册阶段不能投递动作）
function refreshSlotConfig() {
    const keys = Object.keys(CONFIG_DEFAULTS);
    let pending = keys.length;
    function done() {
        --pending;
        if (0 !== pending) {
            return;
        }
        // 配置与顶层注册的默认值不一致时，把注册结果调整到实际值
        applyLyricTip(configLyricTip());
        if (configWholeBar()) {
            registerWholeBar();
        } else {
            unregisterWholeBar();
        }
    }
    keys.forEach(function (key) {
        musicxx.storage.getConfig(key, CONFIG_DEFAULTS[key]).then(function (value) {
            if (true != slotConfigSettled[key]) {
                setSlotConfigValue(key, value);
            }
            done();
        }, function () {
            done();
        });
    });
}

/* ==================== 能力入口 ==================== */

/// 注册本插件的能力（位置上的视图 + 两个页面 + 两个开关 + 动效选择 + 控制栏切播放模式）
function installSlotCapabilities() {
    /// 自检：确认四个业务脚本都执行过
    ///
    /// 脚本在同一个 JS 上下文里按清单顺序执行：少跑一个（报错 / 清单漏了），
    /// 这里的函数或常量就不在。
    musicxx.capability.register("probe", function () {
        return {
            pluginId: PLUGIN_ID,
            uiEntries: musicxx.ui.item.entries().length,
            // 数字（1 / 0）：原生测试按整数字段读（布尔会被当成 0）
            hasSlotViews: (typeof songlistHeaderView === "function") ? 1 : 0,
            hasControlBar: (typeof controlBarView === "function") ? 1 : 0,
            visualizer: (typeof visualizerEntryData === "function") ? 1 : 0,
            pageBackground: (typeof pageCoverEntryData === "function") ? 1 : 0,
            wholeBar: configWholeBar() ? 1 : 0,
            lyricTip: configLyricTip() ? 1 : 0,
        };
    });

    /// 说明页（主页入口指向它）
    musicxx.capability.register("card", function (args) {
        return { view: cardView(args) };
    });

    /// 设置页
    musicxx.capability.register("settings", function (args) {
        return { view: settingsView(args) };
    });

    /// 播放列表页 header 的内容（主程序取视图时调这个能力）
    musicxx.capability.register("songlistHeader", function (args) {
        return { view: songlistHeaderView(args) };
    });

    /// 更多信息页 footer 的内容
    musicxx.capability.register("moreInfoFooter", function (args) {
        return { view: moreInfoFooterView(args) };
    });

    /// 控制栏收藏按钮的内容（`args.favorite` 说明当前是否已收藏）
    musicxx.capability.register("favoriteButton", function (args) {
        return { view: favoriteButtonView(args) };
    });

    /// 整条控制栏的内容（`args` 带 playing / favorite / hasSong / song / colors）
    musicxx.capability.register("controlBar", function (args) {
        return { view: controlBarView(args) };
    });

    /// 控制栏那个"切播放模式"按钮：换模式，并把新模式画进返回的视图
    ///
    /// 两件事分开做：
    /// 1. 调主程序能力 `musicxx.player.setLoopMode`（异步，不 await —— 主程序处理能力时要回到主线程，
    ///    在这里等它会卡界面）；失败只在日志里记一条；
    /// 2. 直接返回按新模式画好的控制栏视图：主程序拿这个 `view` 换掉画面，**不需要为了这个能力
    ///    给位置多加参数**。主程序改完模式会重推 `musicxx.state.playlist`，下次取视图读到的就是真值。
    musicxx.capability.register("cycleLoopMode", function (args) {
        const bar = (args && typeof args === "object") ? args : {};
        const next = nextLoopMode(currentLoopMode());
        musicxx.capability.call("musicxx.player.setLoopMode", { mode: next }).then(
            function () {
                return null;
            },
            function (err) {
                musicxx.host.log(3, "切换播放模式失败: " + errText(err));
            });
        return { view: controlBarView(bar, next) };
    });

    /// 设置页按钮：打开 / 关闭整栏替换
    ///
    /// 关掉时注销整栏，按钮级的两个替换项立刻重新生效（它们一直注册着）。
    musicxx.capability.register("toggleWholeBar", function (args) {
        const next = !configWholeBar();
        saveSlotConfig("wholeBar", next);
        if (next) {
            registerWholeBar();
        } else {
            unregisterWholeBar();
        }
        return { view: pageView(args) };
    });

    /// 设置页按钮：显示 / 隐藏歌词页顶部那块（开 = 注册，关 = 注销）
    musicxx.capability.register("toggleLyricTip", function (args) {
        const next = !configLyricTip();
        saveSlotConfig("lyricTip", next);
        applyLyricTip(next);
        return { view: pageView(args) };
    });

    /// 设置页按钮：选中本插件的音频动效
    musicxx.capability.register("useVisualizer", function (args) {
        usePluginVisualizer();
        return { view: pageView(args) };
    });

    /// 设置页按钮：切回"不显示"
    musicxx.capability.register("clearVisualizer", function (args) {
        useNoVisualizer();
        return { view: pageView(args) };
    });
}

/* ==================== 顶层注册（必须是最后一步，且同步完成） ==================== */

/// 三个插入块（歌词页顶部按配置决定是否启用）
registerInsertSlots(configLyricTip());

/// 两个按钮级替换（菜单 / 收藏）
registerButtonSlots();

/// 歌曲页面的页面背景（普通页面背景：按 `routes` 自动生效，没有用户选择）
registerPageBackground();

/// 整栏替换：配置里开着才注册（同一个位置只认一项，整栏优先于按钮级）
if (configWholeBar()) {
    registerWholeBar();
}

/// 音频动效候选
musicxx.ui.item.register({
    name: SLOT_NAME_VISUALIZER,
    type: SLOT_VISUALIZER,
    order: 10,
    data: visualizerEntryData(),
});

/// 主页入口
registerHomeEntry();

/// 能力
installSlotCapabilities();

/// 别处改了播放模式时，自己把整条控制栏刷新一次
///
/// 动作返回的 `{view}` 只管"用户点这一个按钮"那一次；主程序不会为每个状态给位置加参数，
/// 所以插件自己订阅状态广播：读到播放模式真的变了就调 `musicxx.ui.slot.update` ——
/// 声明不变，主程序重新取一次 `controlBar` 视图（rev + 1），图标跟上真实模式。
/// 订阅时会立即收到当前值一次（那次只记下来，不刷新）。
let lastLoopModeSeen = "";
musicxx.broadcast.subscribe("musicxx.state.playlist", function (value) {
    const mode = (value && typeof value === "object" && value.loopMode)
        ? String(value.loopMode) : "";
    if (mode === lastLoopModeSeen) {
        return;
    }
    lastLoopModeSeen = mode;
    // 只有整条控制栏由本插件画时才有"播放模式"按钮；按钮级替换里没有
    if (wholeBarRegistered) {
        musicxx.ui.slot.update(SLOT_NAME_WHOLE_BAR);
    }
});

/// 读配置：默认值先按声明用，读到 config.json 后再调整注册
musicxx.timer.setTimeout(refreshSlotConfig, 200);

musicxx.host.log(2, PLUGIN_TITLE + "（" + PLUGIN_ID + "）已加载：插入块 3 处 + " +
    "控制栏按钮级替换 2 个 + 音频动效 1 项 + 歌曲页面背景 1 项");
