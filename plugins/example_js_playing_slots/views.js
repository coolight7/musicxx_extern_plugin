/// example_js_playing_slots 的页面与视图
///
/// 两类内容：
/// 1. **位置上的视图**：控制栏与插入块取的视图（`songlistHeader` / `moreInfoFooter` /
///    `favoriteButton` / `controlBar`）。它们是主程序按状态来取的，能力处理器里**同步返回**；
/// 2. **插件页面**：说明页 `card` 与设置页 `settings`（主页入口指向说明页）。
///
/// 插件不写界面代码：页面是"块描述"，由客户端按自己的能力渲染（`kit` 随插件分发）。

/// 界面 kit（基础 kit + musicxx 扩展 kit）
const kit = pluginxx.ui.kit;

/// 卡片现在只收一块内容：多行先包一层 Column（`kit.card` 的 `child` 是单个块）
function columnOf(children) {
    return { kind: "Column", children: children };
}

/// 客户端能力段（插件框架取页面时会放进参数）
function viewEnv(args) {
    if (args && typeof args.ui === "object" && args.ui !== null) {
        return args.ui;
    }
    return null;
}

/* ==================== 状态文字 ==================== */

/// 当前歌曲名（视图参数里有就用，没有就读快照频道）
function currentSongTitle(args) {
    if (args && args.song && typeof args.song.name === "string" && args.song.name) {
        return args.song.name;
    }
    const song = musicxx.broadcast.read("musicxx.state.song");
    if (song && typeof song.name === "string" && song.name) {
        return song.name;
    }
    return "(当前没有歌曲)";
}

/// 整栏替换的状态文本
function wholeBarStateText() {
    return configWholeBar() ? "整栏由本插件画" : "只替换菜单 / 收藏两个按钮";
}

/// 歌词页顶部提示的状态文本
function lyricTipStateText() {
    return configLyricTip() ? "显示" : "已隐藏（已注销注册）";
}

/* ==================== 位置上的视图 ==================== */

/// 播放列表页的列表内 header：当前歌曲 + 「打开更多菜单」
///
/// `header` 在列表的滚动内容里（`top` / `bottom` 才是固定区域），所以这里可以放一行内容。
function songlistHeaderView(args) {
    const env = viewEnv(args);
    return {
        blocks: [
            {
                kind: "Row",
                gap: 10,
                cross: "center",
                children: [
                    {
                        kind: "Expanded",
                        child: {
                            kind: "Text",
                            text: "当前播放：" + currentSongTitle(args) + "（来自插件的列表 header）",
                            type: "caption",
                            color: "hint",
                        },
                    },
                    {
                        kind: "Button",
                        label: "打开更多菜单",
                        variant: "simple",
                        action: { kind: "command", name: "musicxx.ui.playing.openMenu" },
                    },
                ],
            },
        ],
    };
}

/// 更多信息页的列表内 footer：一句说明 + 「收藏当前歌曲」
///
/// 收藏走官方能力 `musicxx.songlist.favorite`：与控制栏收藏按钮、锁屏音乐是同一份实现。
function moreInfoFooterView(args) {
    const env = viewEnv(args);
    return {
        blocks: [
            kit.hint({
                text: "这块在「更多信息页」的末尾，随页面一起滚动；按钮调官方能力 musicxx.songlist.favorite。",
            }, env),
            kit.button({
                label: "收藏当前歌曲",
                variant: "full",
                action: { kind: "command", name: "musicxx.songlist.favorite" },
            }, env),
        ],
    };
}

/// 收藏按钮的两个图标（实心 = 已收藏，空心 = 未收藏）
const FAVORITE_ICON_ON = "resource/svg/heart.svg";
const FAVORITE_ICON_OFF = "resource/svg/heartOutline.svg";

/// 控制栏「收藏」按钮的替换视图（按 `args.favorite` 换图标）
///
/// 控制栏里的按钮**只放图标**（文字会把整排按钮挤变形，内置那一排也只有图标）：
/// 已收藏用实心、未收藏用空心。主程序在收藏状态变化时会带着新的参数再取一次视图。
function favoriteButtonView(args) {
    const favorite = !!(args && args.favorite === true);
    return {
        blocks: [{
            kind: "Button",
            label: "",
            variant: "simple",
            icon: favorite ? FAVORITE_ICON_ON : FAVORITE_ICON_OFF,
            action: { kind: "command", name: "musicxx.songlist.favorite" },
        }],
    };
}

/// 一条控制栏图标按钮（图标是插件自己带的 svg，动作调官方能力）
///
/// 控制栏按钮**只放图标、不写文字**：内置那一排也是这样，写文字会把整排挤变形。
/// `simple` = 无底色：只有图标，与内置控制栏按钮一致（`normal` 会多出一块方块底）。
/// [size] 是按钮高度（u）：给了就写进描述，不给用客户端默认大小。
function controlBarButton(icon, command, size) {
    const button = {
        kind: "Button",
        label: "",
        variant: "simple",
        icon: icon,
        action: { kind: "command", name: command },
    };
    if (null != size) {
        button.size = size;
    }
    return button;
}

/// 控制栏中间那个「播放 / 暂停」按钮的高度（u）
///
/// 不写 `size` 时按钮高 128u（客户端默认值，见插件文档的 `Button` 行），这里取它的两倍。
const PLAY_PAUSE_BUTTON_SIZE = 256;

/// 本插件的"切播放模式"按钮只在这三个模式之间循环
///
/// 顺序与主程序自己的切换顺序一致：单曲循环 → 顺序播放 → 真随机播放 → 单曲循环。
const LOOP_MODES = ["Only", "Order", "RealRand"];

/// 每个模式的图标（插件自带的 svg）
const LOOP_MODE_VIEW = {
    Only: { svg: "resource/svg/playOnly.svg" },
    Order: { svg: "resource/svg/playOrder.svg" },
    RealRand: { svg: "resource/svg/playRand.svg" },
};

/// 当前的播放模式（本插件认识的那三个之一）
///
/// 读的是状态广播 `musicxx.state.playlist` 里的 `loopMode`（同步读，没有 IO）：主程序在模式变化后
/// 会重推这份快照，所以这里总是当前值；不是那三个（伪随机的两种）按"顺序播放"算。
function currentLoopMode() {
    const snapshot = musicxx.broadcast.read("musicxx.state.playlist") || {};
    const mode = snapshot.loopMode || "";
    return LOOP_MODE_VIEW[mode] ? mode : "Order";
}

/// 点一下切到哪个模式（Only → Order → RealRand → Only）
function nextLoopMode(mode) {
    const index = LOOP_MODES.indexOf(mode);
    return LOOP_MODES[(index + 1) % LOOP_MODES.length];
}

/// 控制栏的"切播放模式"按钮：图标反映当前模式，点一下切到下一个（只画图标）
///
/// 动作用 `dispatch` 交给本插件的能力 `cycleLoopMode`：它调主程序能力换模式，
/// 并把新模式直接画进返回的视图 —— **不用等主程序重取**，也不需要主程序为控制栏多给一个参数。
/// [mode] 只在"刚点完、要立刻画新模式"时传，平时读广播快照。
function loopModeButton(args, mode) {
    const useMode = LOOP_MODE_VIEW[mode] ? mode : currentLoopMode();
    const view = LOOP_MODE_VIEW[useMode];
    return {
        kind: "Button",
        label: "",
        variant: "simple",
        icon: view.svg,
        action: {
            kind: "dispatch",
            name: "cycleLoopMode",
            // 把控制栏的视图参数原样交给能力：它返回的控制栏视图要按同一份上下文画
            args: args,
        },
    };
}

/// 整条控制栏的替换视图：五个按钮（切播放模式 / 上一曲 / 播放暂停 / 下一曲 / 收藏）
///
/// **替换，不是插入**：整栏生效时内置的五个按钮与按钮级替换项都不再参与渲染，
/// 所以两个按钮级项虽然还注册着，页面上的提示会说明这一点。
/// 主程序给视图参数 `playing` / `favorite` / `hasSong` / `song` / `colors`：
/// 播放 / 暂停、切歌、收藏、换主题时会带新参数再取一次；播放模式不靠参数 ——
/// 读广播快照 `musicxx.state.playlist`（见 [currentLoopMode]）。
/// **按钮的颜色不用插件管**：没写 `color` 的按钮在播放页上按页面前景三色画（见插件文档 §1.7）。
/// [mode] 是刚切完的新模式（只有点的瞬间传，平时为空）。
function controlBarView(args, mode) {
    const playing = !!(args && args.playing === true);
    const favorite = !!(args && args.favorite === true);
    const hasSong = !(args && args.hasSong === false);
    const buttons = [
        loopModeButton(args, mode),
        controlBarButton("resource/svg/prev.svg", "musicxx.player.prev"),
        controlBarButton(
            playing ? "resource/svg/pause.svg" : "resource/svg/play.svg",
            "musicxx.player.toggle", PLAY_PAUSE_BUTTON_SIZE),
        controlBarButton("resource/svg/next.svg", "musicxx.player.next"),
        {
            kind: "Button",
            label: "",
            variant: "simple",
            icon: favorite ? FAVORITE_ICON_ON : FAVORITE_ICON_OFF,
            disabled: false == hasSong,
            action: { kind: "command", name: "musicxx.songlist.favorite" },
        },
    ];
    return {
        blocks: [{
            kind: "Row",
            main: "spaceBetween",
            cross: "center",
            gap: 8,
            children: buttons,
        }],
    };
}

/* ==================== 插件页面 ==================== */

/// 两个页面共用的状态区
function statusRows() {
    return [
        kit.listRow({
            title: "歌词页顶部 (lyric.top)",
            subtitle: "插入块：页面固定区域，不随列表滚动",
            trailing: lyricTipStateText(),
        }, viewEnv(null)),
        kit.listRow({
            title: "播放列表页 header",
            subtitle: "插入块：列表的 header，跟随内容滚动",
            trailing: "已注册",
        }, viewEnv(null)),
        kit.listRow({
            title: "更多信息页 footer",
            subtitle: "插入块：另一页的列表末尾",
            trailing: "已注册",
        }, viewEnv(null)),
        kit.listRow({
            title: "控制按钮栏",
            subtitle: "替换位置：整栏或单个按钮，同一个位置只认一项",
            trailing: wholeBarStateText(),
        }, viewEnv(null)),
        kit.listRow({
            title: "音频动效 (visualizer)",
            subtitle: "渲染槽位：与内置动效在同一份列表里选（mirror musicxx.state.uiSlots）",
            trailing: visualizerStateText(),
        }, viewEnv(null)),
    ];
}

/// 说明页：每个位置在哪、怎么验证、整栏替换的规则
function cardView(args) {
    const env = viewEnv(args);
    return {
        title: PLUGIN_TITLE,
        subtitle: "播放页插入块（3 处）+ 控制按钮栏（按钮级 / 整栏）+ 音频动效",
        blocks: [
            kit.hint({
                text: "• 插入块：类型名就是位置，同一个位置可以注册多项（按 order 从上到下）。" +
                    "『top』『bottom』是页面固定区域（不随列表滚动），『header』『footer』在列表的滚动内容里。" +
                    "本插件在歌词页顶部（固定区域）、播放列表页 header、更多信息页 footer 各放了一块。",
            }, env),
            kit.hint({
                text: "• 控制按钮栏是**替换**位置：整栏（musicxx.ui.playing.controlBar）生效时，" +
                    "五个按钮全由插件摆，按钮级的两项（menu / favorite）虽然还注册着但不再参与渲染；" +
                    "关掉整栏开关就回到按钮级替换。同一个位置只认一项（order 小的优先）。" +
                    "取不到内容时主程序画回内置按钮，用户不会因此失去播放控制。",
            }, env),
            kit.hint({
                text: "• 音频动效（musicxx.ui.playing.visualizer）是一个候选：用户在「设置 → 主题 → " +
                    "播放页音频动效」里选中后才渲染。动效区不接手势（封面点击、拖动切歌要继续可用）。" +
                    "没安装『音乐动效』时插件项仍可选，但频谱全是 0。",
            }, env),
            kit.card({ child: columnOf(statusRows()) }, env),
            kit.button({ label: "刷新本页", variant: "full", action: "card" }, env),
            kit.button({
                label: "打开设置页",
                variant: "full",
                action: { kind: "route", route: "ext://" + PLUGIN_ID + "/" + SETTINGS_VIEW_ID },
            }, env),
            kit.hint({
                text: "• 这些块都在播放页 / 列表页上：点播放页左下角的封面进入播放页，再切到歌词、列表、更多信息页就能看到。",
            }, env),
        ],
    };
}

/// 设置页：两个开关 + 动效选择
///
/// 按钮都用 `variant: "full"`：大号按钮、宽度默认铺满父块（主程序用 `XXFullBtn` 画，
/// 默认是非选中态，不带主色底），适合设置页里"一行一个"的按钮。
function settingsView(args) {
    const env = viewEnv(args);
    return {
        title: PLUGIN_TITLE + " · 设置",
        subtitle: "位置注册的开关；值保存在插件目录的 config.json",
        blocks: [
            kit.hint({
                text: "• 整栏替换开关：打开后注册整条控制按钮栏（按钮级项保留，主程序会忽略它们），" +
                    "关掉就注销整栏；歌词页顶部提示也是注册 / 注销（开 = 注册，关 = 注销，" +
                    "没有 enabled 字段）。",
            }, env),
            kit.card({ child: columnOf(statusRows()) }, env),
            kit.listRow({
                title: "整条控制按钮栏",
                subtitle: "打开 = 五个按钮都由本插件画；关掉 = 只替换菜单 / 收藏两个按钮",
                trailing: configWholeBar() ? "已开启" : "已关闭",
            }, env),
            kit.button({
                label: configWholeBar() ? "关闭『整栏替换』" : "开启『整栏替换』",
                variant: "full",
                action: { kind: "dispatch", name: "toggleWholeBar", args: { view: "settings" } },
            }, env),
            kit.listRow({
                title: "歌词页顶部提示",
                subtitle: "开 = 注册这一项，关 = musicxx.ui.slot.unregister",
                trailing: configLyricTip() ? "显示" : "已隐藏",
            }, env),
            kit.button({
                label: configLyricTip() ? "隐藏歌词页顶部提示" : "显示歌词页顶部提示",
                variant: "full",
                action: { kind: "dispatch", name: "toggleLyricTip", args: { view: "settings" } },
            }, env),
            kit.card({
                title: "音频动效",
                child: columnOf([
                    kit.hint({
                        text: "选中后动效区由插件的着色器绘制；「切回不显示」等价于在设置里选『不显示』。",
                    }, env),
                    kit.button({
                        label: "使用本插件的音频动效",
                        variant: "full",
                        action: { kind: "dispatch", name: "useVisualizer", args: { view: "settings" } },
                    }, env),
                    kit.button({
                        label: "切回不显示",
                        variant: "full",
                        action: { kind: "dispatch", name: "clearVisualizer", args: { view: "settings" } },
                    }, env),
                ]),
            }, env),
            kit.button({
                label: "打开说明页",
                variant: "full",
                action: { kind: "route", route: "ext://" + PLUGIN_ID + "/" + HOME_VIEW_ID },
            }, env),
        ],
    };
}

/// 动作从哪个页面发起就返回哪个页面（插件框架用返回值直接刷新当前页）
function pageView(args) {
    return (args && args.view === HOME_VIEW_ID) ? cardView(args) : settingsView(args);
}
