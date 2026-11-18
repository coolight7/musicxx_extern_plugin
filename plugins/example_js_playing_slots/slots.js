/// example_js_playing_slots 的位置声明: 页面插入块 + 控制按钮栏 + 普通页面背景
///
/// 这个插件只演示"插件在哪些位置能放自己的界面"，分四类：
/// 1. **插入块**（`musicxx.ui.playing.<页面>.<位置>`）：在页面固定位置或列表 header/footer
///    插一块自己的界面，同一位置可以摆多项；
/// 2. **替换位置**（`musicxx.ui.playing.controlBar[.<按钮>]`）：换掉整条控制按钮栏，
///    或者只换其中一个按钮；
/// 3. **渲染槽位**（`musicxx.ui.playing.visualizer`）：成为音频动效的一个候选，见 visualizer.js；
/// 4. **普通页面背景**（`musicxx.ui.page.background`）：给某个路由的页面铺一层背景，
///    按路由自动生效、没有用户选择。这里给歌曲页面（`/songs`）画这张歌单的封面。
///
/// 位置就是 UI 类型：写错页面或位置名时插件框架直接拒绝注册（不会静默不显示）。
/// 本文件只写"声明"与注册函数，页面内容在 views.js，顶层调用在 plugin.js。

/// 插件 id 与显示名
const PLUGIN_ID = musicxx.pluginId;
const PLUGIN_TITLE = "示例播放页插槽";

/// 用到的位置（短名；完整名 = `musicxx.ui.` + 短名）
const SLOT_LYRIC_TOP = "playing.lyric.top";
const SLOT_SONGLIST_HEADER = "playing.songlist.header";
const SLOT_MORE_INFO_FOOTER = "playing.moreInfo.footer";
const SLOT_CONTROL_BAR = "playing.controlBar";
const SLOT_CONTROL_BAR_MENU = "playing.controlBar.menu";
const SLOT_CONTROL_BAR_FAVORITE = "playing.controlBar.favorite";
const SLOT_VISUALIZER = "playing.visualizer";
const SLOT_PAGE_BACKGROUND = "page.background";

/// 渲染槽位 id（`musicxx.ui.slot.set` 的 slot 参数）与内置的"不显示"项
const VISUALIZER_SLOT_ID = "player.visualizer";
const NONE_VISUALIZER_ID = "musicxx.none";

/// UI 项短名（完整 id = `plugin.<插件 id>.<短名>`）
const SLOT_NAME_LYRIC_TOP = "lyricTop";
const SLOT_NAME_SONGLIST_HEADER = "songlistHeader";
const SLOT_NAME_MORE_INFO_FOOTER = "moreInfoFooter";
const SLOT_NAME_MENU = "menuButton";
const SLOT_NAME_FAVORITE = "favoriteButton";
const SLOT_NAME_WHOLE_BAR = "wholeBar";
const SLOT_NAME_VISUALIZER = "visualizer";
const SLOT_NAME_PAGE_BACKGROUND = "pageCover";
const SLOT_NAME_HOME = "home";

/// 主页入口与两个页面的视图 id
const HOME_VIEW_ID = "card";
const SETTINGS_VIEW_ID = "settings";

/// 本插件的动效项 id（选中它 / 切回"不显示"时用）
const VISUALIZER_ITEM_ID = "plugin." + PLUGIN_ID + "." + SLOT_NAME_VISUALIZER;

/// 『歌词页顶部』的插入块（内联视图：内容不变，直接写在这里）
///
/// 为什么用内联：内容固定、不需要按状态变。`top` / `bottom` 是页面固定区域（不随列表滚动），
/// 放一两行就够；长内容要放 `header` / `footer`。
function lyricTopEntryData() {
    return {
        view: {
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
                                text: "歌词页顶部这块来自插件（example_js_playing_slots）",
                                type: "caption",
                                tone: "hint",
                            }],
                        },
                        {
                            kind: "Button",
                            label: "说明",
                            variant: "simple",
                            action: { kind: "route", route: "ext://" + PLUGIN_ID + "/" + HOME_VIEW_ID },
                        },
                    ],
                },
            ],
        },
    };
}

/// 歌词页顶部那块现在有没有注册（注销一个没注册的名字会报错，所以先记着）
let lyricTipRegistered = false;

/// 显示 / 隐藏歌词页顶部那块：显示 = 注册，隐藏 = 注销
///
/// UI 项没有 `enabled` 字段了：不想显示就注销这一项，重新显示再注册回来。
/// 注销后再注册是新注册（排到最后）；这里只有一个位置项，先后无所谓。
function applyLyricTip(enabled) {
    const want = true === enabled;
    if (want === lyricTipRegistered) {
        return;
    }
    if (want) {
        musicxx.ui.slot.register({
            name: SLOT_NAME_LYRIC_TOP,
            type: SLOT_LYRIC_TOP,
            order: 10,
            data: lyricTopEntryData(),
        });
    } else {
        musicxx.ui.slot.unregister(SLOT_NAME_LYRIC_TOP);
    }
    lyricTipRegistered = want;
}

/// 注册三个插入块（固定区域 · 列表 header · 另一页的 footer）
///
/// 同一位置可以注册多项、按 `order` 从上到下排；歌词页顶部那块的开关由
/// [applyLyricTip] 管（开 = 注册，关 = 注销）。
function registerInsertSlots(lyricTipEnabled) {
    applyLyricTip(lyricTipEnabled);
    musicxx.ui.slot.register({
        name: SLOT_NAME_SONGLIST_HEADER,
        type: SLOT_SONGLIST_HEADER,
        order: 10,
        // 视图 id：主程序调本插件的能力 `songlistHeader` 取内容（能按状态返回不同块）
        data: { view: "songlistHeader" },
    });
    musicxx.ui.slot.register({
        name: SLOT_NAME_MORE_INFO_FOOTER,
        type: SLOT_MORE_INFO_FOOTER,
        order: 10,
        data: { view: "moreInfoFooter" },
    });
}

/// 注册按钮级替换（菜单按钮与收藏按钮）
///
/// - 菜单按钮用内联视图：只有一个图标 + 官方动作 `musicxx.ui.playing.openMenu`；
/// - 收藏按钮用视图 id：要按 `args.favorite` 换显示（"状态变了会重新取一次视图"）。
///
/// 两个都用 `variant: "simple"`：**无底色**，只有图标（或文字），跟内置控制栏按钮长得一样；
/// 换成 `normal` 会多出一块方块底。
function registerButtonSlots() {
    musicxx.ui.slot.register({
        name: SLOT_NAME_MENU,
        type: SLOT_CONTROL_BAR_MENU,
        order: 10,
        data: {
            view: {
                blocks: [{
                    kind: "Button",
                    label: "",
                    variant: "simple",
                    icon: "resource/svg/menu.svg",
                    action: { kind: "command", name: "musicxx.ui.playing.openMenu" },
                }],
            },
        },
    });
    musicxx.ui.slot.register({
        name: SLOT_NAME_FAVORITE,
        type: SLOT_CONTROL_BAR_FAVORITE,
        order: 10,
        data: { view: "favoriteButton" },
    });
}

/// 整栏替换现在有没有注册（注销一个没注册的名字会报错，所以先记着）
let wholeBarRegistered = false;

/// 整条控制按钮栏的替换项（设置页开关控制注册 / 注销）
///
/// 同一个位置**只认一项**：整栏生效时，按钮级的两个项仍然注册着、但不再参与渲染
/// （页面上会提示这一点，免得以为注销失败）。关掉开关就注销整栏，按钮级替换重新生效。
///
/// 两个方向都按 [wholeBarRegistered] 判断：读配置时会拿默认值与实际值对一次，
/// 这时"已经是关的"也会走到这里，不判断就会去注销一个从没注册过的项。
function registerWholeBar() {
    if (wholeBarRegistered) {
        return;
    }
    musicxx.ui.slot.register({
        name: SLOT_NAME_WHOLE_BAR,
        type: SLOT_CONTROL_BAR,
        order: 10,
        data: { view: "controlBar" },
    });
    wholeBarRegistered = true;
}

function unregisterWholeBar() {
    if (!wholeBarRegistered) {
        return;
    }
    musicxx.ui.slot.unregister(SLOT_NAME_WHOLE_BAR);
    wholeBarRegistered = false;
}

/// 歌曲页面（`/songs`）的页面背景：把这张歌单的封面贴在屏幕边缘占 70%，余下渐变到主色
///
/// 普通页面背景（`musicxx.ui.page.background`）与播放页背景同一份块写法，区别是：
/// - **没有用户选择**：`routes` 声明适用页面，写了这个路由的项优先于“所有页面”的项，
///   同一优先级里**注册顺序最靠后的**生效（别处也注册了 `/songs` 时可能被顶掉）；
/// - 纹理来源 `musicxx.songlist.cover` **只在歌单页面内可用**（当前打开的歌单由主程序的
///   页面子树给出），别的页面拿不到封面，画面只剩渐变底。
///
/// `uTint` 是同一张封面的缩小 + 重度模糊版：模糊到位后基本是纯色，采样一次就相当于
/// 这张图的主色调 —— 插件拿不到像素数据，这是最省的做法，也不需要主程序再提供取色接口。
///
/// **配色由插件自己保证可读**：页面上的标题与歌曲名用主程序的文字色（夜间浅、白昼深），
/// 所以着色器把整幅画面的亮度收进“与当前主题文字够对比”的区间（封面与渐变一起，区间内不动）。
function pageCoverEntryData() {
    return {
        title: PLUGIN_TITLE + " · 歌曲页面封面背景",
        depict: "歌单封面贴着屏幕边缘占 70%，其余渐变到这张图的主色调" +
            "（横屏贴左/上/下，竖屏贴上/左/右）；亮度按封面主色与主题自动调到文字看得清",
        routes: ["/songs"],
        view: {
            kind: "musicxx.Shader",
            bundle: "shader/page_cover.shaderbundle",
            args: {
                uCover: { kind: "texture", source: "musicxx.songlist.cover", size: 768 },
                uTint: {
                    kind: "texture",
                    source: "musicxx.songlist.cover",
                    size: 128,
                    blur: 40,
                    square: true,
                },
                // 没有封面（不在歌单页面 / 这张歌单还没有封面）时的底色
                uFallback: { kind: "source", name: "musicxx.theme.background" },
                uShare: { kind: "const", value: 0.7 },
            },
            // 静态背景：进页面出一帧，换歌单 / 封面后重新准备纹理再出一帧
            animate: false,
            resolutionScale: 0.6,
        },
    };
}

/// 注册歌曲页面的页面背景（注册即生效，没有选中状态）
///
/// 不想显示就 `musicxx.ui.slot.unregister(SLOT_NAME_PAGE_BACKGROUND)`。
function registerPageBackground() {
    musicxx.ui.slot.register({
        name: SLOT_NAME_PAGE_BACKGROUND,
        type: SLOT_PAGE_BACKGROUND,
        // 同一路由命中多项时不用 order 取舍：写了这个路由的项优先，同优先级里最新的生效
        order: 10,
        data: pageCoverEntryData(),
    });
}

/// 注册主页入口（说明页 `ext://<插件 id>/card`）
function registerHomeEntry() {
    musicxx.ui.slot.register({
        name: SLOT_NAME_HOME,
        type: "home.entry",
        order: 140,
        data: {
            title: PLUGIN_TITLE,
            subtitle: "播放页插入块 / 控制按钮栏 / 音频动效三个位置各放一块",
            icon: "resource/svg/slots.svg",
            action: { kind: "route", route: "ext://" + PLUGIN_ID + "/" + HOME_VIEW_ID },
        },
    });
}
