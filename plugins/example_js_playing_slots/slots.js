/// example_js_playing_slots 的内容注册: 页面插入块 + 控制按钮栏 + 普通页面背景
///
/// 界面只有两层：**槽位**（一个渲染位置，清单见文档 `ui-slots.md`）与**内容**
/// （插件为某个槽位生成的一棵块树）。这个插件演示四类槽位：
/// 1. **插入位置**（`musicxx.ui.playing.<页面>.<位置>`）：在页面固定区域或列表 header/footer
///    插一块自己的界面，同一位置可以放多份内容；
/// 2. **替换位置**（`musicxx.ui.playing.controlBar[.<按钮>]`）：换掉整条控制按钮栏，
///    或者只换其中一个按钮；
/// 3. **可选样式槽位**（`musicxx.ui.playing.visualizer`）：成为音频动效的一个候选，见 visualizer.js；
/// 4. **普通页面背景**（`musicxx.ui.page.background`）：给某个路由的页面铺一层背景，
///    按路由自动生效、没有用户选择。这里给歌曲页面（`/songs`）画这张歌单的封面。
///
/// 内容在注册时只给"名字 + 生成函数"，主程序要用这份内容时才调生成函数（见本文件里
/// 每个 `musicxx.ui.content.register` 的第二个参数）。插在页面上的那一份**没有参数**，
/// 靠 `deps` 声明用到的变量：变量变化时主程序先重新绑定，再让生成函数重出一份。
///
/// 本文件只写"注册内容"与生成函数，视图在 views.js，顶层调用在 plugin.js。

/// 插件 id 与显示名
const PLUGIN_ID = musicxx.pluginId;
const PLUGIN_TITLE = "示例播放页插槽";

/// 用到的槽位（全名；写简称 `playing.lyric.top` 也可以）
const SLOT_LYRIC_TOP = "musicxx.ui.playing.lyric.top";
const SLOT_SONGLIST_HEADER = "musicxx.ui.playing.songlist.header";
const SLOT_MORE_INFO_FOOTER = "musicxx.ui.playing.moreInfo.footer";
const SLOT_CONTROL_BAR = "musicxx.ui.playing.controlBar";
const SLOT_CONTROL_BAR_MENU = "musicxx.ui.playing.controlBar.menu";
const SLOT_CONTROL_BAR_FAVORITE = "musicxx.ui.playing.controlBar.favorite";
const SLOT_VISUALIZER = "musicxx.ui.playing.visualizer";
const SLOT_PAGE_BACKGROUND = "musicxx.ui.page.background";

/// 渲染槽位 id（`musicxx.ui.slot.set` 的 slot 参数）与内置的"不显示"项
const VISUALIZER_SLOT_ID = "player.visualizer";
const NONE_VISUALIZER_ID = "musicxx.none";

/// 内容 id（全名 = `plugin.<插件 id>.<内容 id>`）
const SLOT_NAME_LYRIC_TOP = "lyricTop";
const SLOT_NAME_SONGLIST_HEADER = "songlistHeader";
const SLOT_NAME_MORE_INFO_FOOTER = "moreInfoFooter";
const SLOT_NAME_MENU = "menuButton";
const SLOT_NAME_FAVORITE = "favoriteButton";
const SLOT_NAME_WHOLE_BAR = "wholeBar";
const SLOT_NAME_VISUALIZER = "visualizer";
const SLOT_NAME_PAGE_BACKGROUND = "pageCover";
const SLOT_NAME_HOME = "home";

/// 页面内容 id：主页入口指向 `ext://<插件 id>/card`，另一个页面是 `ext://<插件 id>/settings`
const HOME_VIEW_ID = "card";
const SETTINGS_VIEW_ID = "settings";

/// 本插件的动效内容 id（选中它 / 切回"不显示"时用）
const VISUALIZER_ITEM_ID = "plugin." + PLUGIN_ID + "." + SLOT_NAME_VISUALIZER;

/// 『歌词页顶部』的界面（内容固定，生成函数直接返回这一棵块树）
///
/// `top` / `bottom` 是页面固定区域（不随列表滚动），放一两行就够；
/// 长内容要放 `header` / `footer`。
function lyricTopView() {
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
                            text: "歌词页顶部这块来自插件（example_js_playing_slots）",
                            type: "caption",
                            color: "hint",
                        },
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
    };
}

/// 歌词页顶部那块现在有没有注册（注销一份没注册过的内容会报错，所以先记着）
let lyricTipRegistered = false;

/// 显示 / 隐藏歌词页顶部那块：显示 = 注册内容，隐藏 = 注销内容
///
/// 内容没有 `enabled` 字段：不想显示就注销这一份，重新显示再注册回来。
/// 注销后再注册算新注册（排到最后）；这里只有一份内容，先后无所谓。
function applyLyricTip(enabled) {
    const want = true === enabled;
    if (want === lyricTipRegistered) {
        return;
    }
    if (want) {
        musicxx.ui.content.register({
            id: SLOT_NAME_LYRIC_TOP,
            slot: SLOT_LYRIC_TOP,
            title: "歌词页顶部提示",
            order: 10,
        }, lyricTopView);
    } else {
        musicxx.ui.content.unregister(SLOT_NAME_LYRIC_TOP);
    }
    lyricTipRegistered = want;
}

/// 注册三个插入位置的内容（固定区域 · 列表 header · 另一页的 footer）
///
/// 同一位置可以注册多份、按 `order` 从上到下排；歌词页顶部那块的开关由
/// [applyLyricTip] 管（开 = 注册，关 = 注销）。
function registerInsertSlots(lyricTipEnabled) {
    applyLyricTip(lyricTipEnabled);
    // 生成函数拿到的是声明过的依赖变量（这里的键就是 deps 里的 id）
    musicxx.ui.content.register({
        id: SLOT_NAME_SONGLIST_HEADER,
        slot: SLOT_SONGLIST_HEADER,
        title: "播放列表页顶部",
        order: 10,
        deps: ["musicxx.song.name"],
    }, function (ctx) {
        return songlistHeaderView(contentArgs(ctx));
    });
    musicxx.ui.content.register({
        id: SLOT_NAME_MORE_INFO_FOOTER,
        slot: SLOT_MORE_INFO_FOOTER,
        title: "更多信息页末尾",
        order: 10,
    }, function () {
        return moreInfoFooterView(null);
    });
}

/// 注册按钮级替换（菜单按钮与收藏按钮）
///
/// - 菜单按钮的界面是固定的：只有一个图标 + 官方能力 `musicxx.ui.playing.openMenu`；
/// - 收藏按钮要跟着"当前歌曲有没有收藏"换图标，所以声明 `musicxx.song.favorite` 作依赖，
///   这个变量一变主程序就让生成函数重出一份。
///
/// 两个都用 `variant: "simple"`：**无底色**，只有图标（或文字），跟内置控制栏按钮长得一样；
/// 换成 `normal` 会多出一块方块底。
function registerButtonSlots() {
    musicxx.ui.content.register({
        id: SLOT_NAME_MENU,
        slot: SLOT_CONTROL_BAR_MENU,
        title: "更多菜单按钮",
        order: 10,
    }, function () {
        return {
            blocks: [{
                kind: "Button",
                label: "",
                variant: "simple",
                icon: "resource/svg/menu.svg",
                action: { kind: "call", name: "musicxx.ui.playing.openMenu" },
            }],
        };
    });
    musicxx.ui.content.register({
        id: SLOT_NAME_FAVORITE,
        slot: SLOT_CONTROL_BAR_FAVORITE,
        title: "收藏按钮",
        order: 10,
        deps: ["musicxx.song.favorite"],
    }, function (ctx) {
        return favoriteButtonView(contentArgs(ctx));
    });
}

/// 整栏替换现在有没有注册（注销一份没注册过的内容会报错，所以先记着）
let wholeBarRegistered = false;

/// 整条控制按钮栏的替换内容（设置页开关控制注册 / 注销）
///
/// 同一个位置**只认一份内容**：整栏生效时，按钮级的两份仍然注册着、但不再参与渲染
/// （页面上会提示这一点，免得以为注销失败）。关掉开关就注销整栏，按钮级替换重新生效。
///
/// 两个方向都按 [wholeBarRegistered] 判断：读配置时会拿默认值与实际值对一次，
/// 这时"已经是关的"也会走到这里，不判断就会去注销一份从没注册过的内容。
function registerWholeBar() {
    if (wholeBarRegistered) {
        return;
    }
    musicxx.ui.content.register({
        id: SLOT_NAME_WHOLE_BAR,
        slot: SLOT_CONTROL_BAR,
        title: "整条控制栏",
        order: 10,
        deps: [
            "musicxx.player.playing",
            "musicxx.song.hasSong",
            "musicxx.song.favorite",
        ],
    }, function (ctx) {
        return controlBarView(contentArgs(ctx));
    });
    wholeBarRegistered = true;
}

function unregisterWholeBar() {
    if (!wholeBarRegistered) {
        return;
    }
    musicxx.ui.content.unregister(SLOT_NAME_WHOLE_BAR);
    wholeBarRegistered = false;
}

/// 歌曲页面（`/songs`）的页面背景：把这张歌单的封面贴在屏幕边缘占 70%，余下渐变到主色
///
/// 普通页面背景（`musicxx.ui.page.background`）与播放页背景同一份块写法，区别是：
/// - **没有用户选择**：`routes` 在**注册时**声明适用页面（内容这时还没生成，主程序得先按路由
///   挑"这一页用哪一份"），写了这个路由的内容优先于"所有页面"的内容，
///   同一优先级里**注册顺序最靠后的**生效；
/// - 纹理来源 `musicxx.songlist.cover` **只在歌单页面内可用**（当前打开的歌单由主程序的
///   页面子树给出），别的页面拿不到封面，画面只剩渐变底。
///
/// `uTint` 是同一张封面的缩小 + 重度模糊版：模糊到位后基本是纯色，采样一次就相当于
/// 这张图的主色调 —— 插件拿不到像素数据，这是最省的做法，也不需要主程序再提供取色接口。
///
/// **配色由插件自己保证可读**：页面上的标题与歌曲名用主程序的文字色（夜间浅、白昼深），
/// 所以着色器把整幅画面的亮度收进"与当前主题文字够对比"的区间（封面与渐变一起，区间内不动）。
function pageCoverView() {
    return {
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
    };
}

/// 注册歌曲页面的页面背景（注册即生效，没有选中状态）
///
/// 不想显示就 `musicxx.ui.content.unregister(SLOT_NAME_PAGE_BACKGROUND)`。
function registerPageBackground() {
    musicxx.ui.content.register({
        id: SLOT_NAME_PAGE_BACKGROUND,
        slot: SLOT_PAGE_BACKGROUND,
        title: PLUGIN_TITLE + " · 歌曲页面封面背景",
        depict: "歌单封面贴着屏幕边缘占 70%，其余渐变到这张图的主色调" +
            "（横屏贴左/上/下，竖屏贴上/左/右）；亮度按封面主色与主题自动调到文字看得清",
        // 适用页面只能在注册时给：内容还没生成，主程序要先按路由挑这一份
        routes: ["/songs"],
        order: 10,
    }, pageCoverView);
}

/// 注册主页入口（说明页 `ext://<插件 id>/card`）
///
/// 入口是**声明项**（`major item`），与内容不是一回事：它只有数据、没有界面树，
/// 由主程序画在主界面的入口列表里。
function registerHomeEntry() {
    musicxx.ui.item.register({
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
