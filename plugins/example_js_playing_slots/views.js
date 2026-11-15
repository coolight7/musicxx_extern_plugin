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
    return configLyricTip() ? "显示" : "已隐藏（注册还在，enabled=false）";
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
                        children: [{
                            kind: "Text",
                            text: "当前播放：" + currentSongTitle(args) + "（来自插件的列表 header）",
                            type: "caption",
                            tone: "hint",
                        }],
                    },
                    {
                        kind: "Button",
                        label: "打开更多菜单",
                        variant: "ghost",
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
            {
                kind: "Row",
                gap: 10,
                children: [{
                    kind: "Expanded",
                    children: [{
                        kind: "Button",
                        label: "收藏当前歌曲",
                        variant: "primary",
                        action: { kind: "command", name: "musicxx.songlist.favorite" },
                    }],
                }],
            },
        ],
    };
}

/// 控制栏「收藏」按钮的替换视图（按 `args.favorite` 换显示）
///
/// 主程序在收藏状态变化时会带着新的参数再取一次视图，所以状态跟着走。
function favoriteButtonView(args) {
    const favorite = !!(args && args.favorite === true);
    return {
        blocks: [{
            kind: "Button",
            label: favorite ? "♥ 已收藏" : "♡ 收藏",
            variant: "ghost",
            action: { kind: "command", name: "musicxx.songlist.favorite" },
        }],
    };
}

/// 一条控制栏图标按钮（图标是插件自己带的 svg，动作调官方能力）
function controlBarButton(icon, command, label) {
    return {
        kind: "Button",
        label: label || "",
        variant: "ghost",
        icon: icon,
        action: { kind: "command", name: command },
    };
}

/// 整条控制栏的替换视图：五个按钮，动作都调官方能力
///
/// **替换，不是插入**：整栏生效时内置的五个按钮与按钮级替换项都不再参与渲染，
/// 所以两个按钮级项虽然还注册着，页面上的提示会说明这一点。
/// 主程序给视图参数 `playing` / `favorite` / `hasSong` / `song` / `colors`：
/// 播放 / 暂停、切歌、收藏、换主题时会带新参数再取一次。
function controlBarView(args) {
    const playing = !!(args && args.playing === true);
    const favorite = !!(args && args.favorite === true);
    const hasSong = !(args && args.hasSong === false);
    const colors = (args && typeof args.colors === "object" && args.colors) || null;
    const buttons = [
        controlBarButton("resource/svg/menu.svg", "musicxx.ui.playing.openMenu", "菜单"),
        controlBarButton("resource/svg/prev.svg", "musicxx.player.prev", "上一曲"),
        controlBarButton(
            playing ? "resource/svg/pause.svg" : "resource/svg/play.svg",
            "musicxx.player.toggle",
            playing ? "暂停" : "播放"),
        controlBarButton("resource/svg/next.svg", "musicxx.player.next", "下一曲"),
        {
            kind: "Button",
            label: favorite ? "♥" : "♡",
            variant: "ghost",
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
            subtitle: "渲染槽位：与内置动效在同一份列表里选（mirror musicxx.state.renderSlots）",
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
            kit.card({ children: statusRows() }, env),
            kit.button({ label: "刷新本页", variant: "primary", action: "card" }, env),
            kit.button({
                label: "打开设置页",
                variant: "primary",
                action: { kind: "route", route: "ext://" + PLUGIN_ID + "/" + SETTINGS_VIEW_ID },
            }, env),
            kit.hint({
                text: "• 这些块都在播放页 / 列表页上：点播放页左下角的封面进入播放页，再切到歌词、列表、更多信息页就能看到。",
            }, env),
        ],
    };
}

/// 设置页：两个开关 + 动效选择
function settingsView(args) {
    const env = viewEnv(args);
    return {
        title: PLUGIN_TITLE + " · 设置",
        subtitle: "位置注册的开关；值保存在插件目录的 config.json",
        blocks: [
            kit.hint({
                text: "• 整栏替换开关：打开后注册整条控制按钮栏（按钮级项保留，主程序会忽略它们），" +
                    "关掉就注销整栏；歌词页顶部提示用 UI 项的 enabled 开关，注册一直保留。",
            }, env),
            kit.card({ children: statusRows() }, env),
            kit.listRow({
                title: "整条控制按钮栏",
                subtitle: "打开 = 五个按钮都由本插件画；关掉 = 只替换菜单 / 收藏两个按钮",
                trailing: configWholeBar() ? "已开启" : "已关闭",
            }, env),
            kit.button({
                label: configWholeBar() ? "关闭『整栏替换』" : "开启『整栏替换』",
                variant: "primary",
                action: { kind: "dispatch", name: "toggleWholeBar", args: { view: "settings" } },
            }, env),
            kit.listRow({
                title: "歌词页顶部提示",
                subtitle: "用 musicxx.ui.updateEntry 改 UI 项的 enabled（注册还在，只是不渲染）",
                trailing: configLyricTip() ? "显示" : "已隐藏",
            }, env),
            kit.button({
                label: configLyricTip() ? "隐藏歌词页顶部提示" : "显示歌词页顶部提示",
                action: { kind: "dispatch", name: "toggleLyricTip", args: { view: "settings" } },
            }, env),
            kit.card({
                title: "音频动效",
                children: [
                    kit.hint({
                        text: "选中后动效区由插件的着色器绘制；「切回不显示」等价于在设置里选『不显示』。",
                    }, env),
                    kit.button({
                        label: "使用本插件的音频动效",
                        variant: "primary",
                        action: { kind: "dispatch", name: "useVisualizer", args: { view: "settings" } },
                    }, env),
                    kit.button({
                        label: "切回不显示",
                        action: { kind: "dispatch", name: "clearVisualizer", args: { view: "settings" } },
                    }, env),
                ],
            }, env),
            kit.button({
                label: "打开说明页",
                action: { kind: "route", route: "ext://" + PLUGIN_ID + "/" + HOME_VIEW_ID },
            }, env),
        ],
    };
}

/// 动作从哪个页面发起就返回哪个页面（插件框架用返回值直接刷新当前页）
function pageView(args) {
    return (args && args.view === HOME_VIEW_ID) ? cardView(args) : settingsView(args);
}
