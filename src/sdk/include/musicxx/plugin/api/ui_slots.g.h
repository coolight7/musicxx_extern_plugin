/* 本文件由 tools/gen_contract.dart 生成, 请勿手工修改。
 * 定义来源: tools/ui_slots.def.json
 * 槽位(渲染位置)与声明项: C 宏 + 已知槽位表。 */
#ifndef MUSICXX_PLUGIN_UI_SLOTS_G_H
#define MUSICXX_PLUGIN_UI_SLOTS_G_H

#ifdef __cplusplus
extern "C" {
#endif

/* 主页入口: 功能主页 / 入口列表 */
#define MUSICXX_PLUGIN_UI_SLOT_HOME_ENTRY "musicxx.ui.home.entry"
/* 歌曲菜单项: 各处歌曲菜单 / 菜单项 */
#define MUSICXX_PLUGIN_UI_SLOT_SONG_ACTION "musicxx.ui.song.action"
/* 歌单菜单项: 歌单菜单 / 菜单项 */
#define MUSICXX_PLUGIN_UI_SLOT_PLAYLIST_ACTION "musicxx.ui.playlist.action"
/* 添加歌曲来源项: 「添加歌曲」弹窗 / 来源列表 */
#define MUSICXX_PLUGIN_UI_SLOT_ADDSONG_ENTRY "musicxx.ui.addsong.entry"
/* 音效: 音效页面 / 音效列表 */
#define MUSICXX_PLUGIN_UI_SLOT_AUDIO_EFFECT "musicxx.ui.audioEffect"
/* 播放页背景: 播放页 / 整页背景（用户选择生效） */
#define MUSICXX_PLUGIN_UI_SLOT_PLAYING_BACKGROUND "musicxx.ui.playing.background"
/* 播放页歌曲图: 播放页 / 歌曲图 / 视频（用户选择生效） */
#define MUSICXX_PLUGIN_UI_SLOT_PLAYING_ICON "musicxx.ui.playing.icon"
/* 播放页音频动效: 播放页 / 动效区（用户选择生效） */
#define MUSICXX_PLUGIN_UI_SLOT_PLAYING_VISUALIZER "musicxx.ui.playing.visualizer"
/* 普通页面背景: 所有普通页面 / 整页背景（按路由自动生效） */
#define MUSICXX_PLUGIN_UI_SLOT_PAGE_BACKGROUND "musicxx.ui.page.background"
/* 播放页控制栏: 播放页 / 整栏（替换内置界面） */
#define MUSICXX_PLUGIN_UI_SLOT_PLAYING_CONTROL_BAR "musicxx.ui.playing.controlBar"
/* 播放页控制栏 · 菜单按钮: 播放页 / 控制栏「菜单」按钮（替换显示） */
#define MUSICXX_PLUGIN_UI_SLOT_PLAYING_CONTROL_BAR_MENU "musicxx.ui.playing.controlBar.menu"
/* 播放页控制栏 · 上一曲按钮: 播放页 / 控制栏「上一曲」按钮（替换显示） */
#define MUSICXX_PLUGIN_UI_SLOT_PLAYING_CONTROL_BAR_PREVIOUS "musicxx.ui.playing.controlBar.previous"
/* 播放页控制栏 · 播放暂停按钮: 播放页 / 控制栏「播放 / 暂停」按钮（替换显示） */
#define MUSICXX_PLUGIN_UI_SLOT_PLAYING_CONTROL_BAR_PLAY_OR_PAUSE "musicxx.ui.playing.controlBar.playOrPause"
/* 播放页控制栏 · 下一曲按钮: 播放页 / 控制栏「下一曲」按钮（替换显示） */
#define MUSICXX_PLUGIN_UI_SLOT_PLAYING_CONTROL_BAR_NEXT "musicxx.ui.playing.controlBar.next"
/* 播放页控制栏 · 收藏按钮: 播放页 / 控制栏「收藏」按钮（替换显示） */
#define MUSICXX_PLUGIN_UI_SLOT_PLAYING_CONTROL_BAR_FAVORITE "musicxx.ui.playing.controlBar.favorite"
/* 歌词页顶部: 播放页 · 歌词页 / 列表之上（不随内容滚动） */
#define MUSICXX_PLUGIN_UI_SLOT_PLAYING_LYRIC_TOP "musicxx.ui.playing.lyric.top"
/* 歌词页底部: 播放页 · 歌词页 / 列表之下（不随内容滚动） */
#define MUSICXX_PLUGIN_UI_SLOT_PLAYING_LYRIC_BOTTOM "musicxx.ui.playing.lyric.bottom"
/* 歌词列表内顶部: 播放页 · 歌词页 / 列表的 header（跟随内容滚动） */
#define MUSICXX_PLUGIN_UI_SLOT_PLAYING_LYRIC_HEADER "musicxx.ui.playing.lyric.header"
/* 歌词列表内底部: 播放页 · 歌词页 / 列表的 footer（跟随内容滚动） */
#define MUSICXX_PLUGIN_UI_SLOT_PLAYING_LYRIC_FOOTER "musicxx.ui.playing.lyric.footer"
/* 更多信息页内容开头: 播放页 · 更多信息页 / 整页滚动内容的最前面 */
#define MUSICXX_PLUGIN_UI_SLOT_PLAYING_MORE_INFO_HEADER "musicxx.ui.playing.moreInfo.header"
/* 更多信息页内容末尾: 播放页 · 更多信息页 / 整页滚动内容的最后面 */
#define MUSICXX_PLUGIN_UI_SLOT_PLAYING_MORE_INFO_FOOTER "musicxx.ui.playing.moreInfo.footer"
/* 播放列表页顶部: 播放页 · 播放列表页 / 列表之上（不随内容滚动） */
#define MUSICXX_PLUGIN_UI_SLOT_PLAYING_SONGLIST_TOP "musicxx.ui.playing.songlist.top"
/* 播放列表页底部: 播放页 · 播放列表页 / 列表之下（不随内容滚动） */
#define MUSICXX_PLUGIN_UI_SLOT_PLAYING_SONGLIST_BOTTOM "musicxx.ui.playing.songlist.bottom"
/* 播放列表内顶部: 播放页 · 播放列表页 / 列表的 header（跟随内容滚动） */
#define MUSICXX_PLUGIN_UI_SLOT_PLAYING_SONGLIST_HEADER "musicxx.ui.playing.songlist.header"
/* 播放列表内底部: 播放页 · 播放列表页 / 列表的 footer（跟随内容滚动） */
#define MUSICXX_PLUGIN_UI_SLOT_PLAYING_SONGLIST_FOOTER "musicxx.ui.playing.songlist.footer"
/* 搜索类型: 搜索页 / 类型标签栏 + 结果区（用户选中哪个就画哪个） */
#define MUSICXX_PLUGIN_UI_SLOT_SEARCH_TYPE "musicxx.ui.search.type"
/* 插件页面: 插件页面 ext://<插件id>/<内容 id> / 整页（插件自己发起） */
#define MUSICXX_PLUGIN_UI_SLOT_PAGE "musicxx.ui.page"
/* 插件浮层: 浮层（musicxx.ui.overlay.show / showSheet） / 整层（插件自己发起） */
#define MUSICXX_PLUGIN_UI_SLOT_OVERLAY "musicxx.ui.overlay"

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
namespace musicxx {
namespace extern_plugin {

/// 一个槽位 / 声明项的元信息 (框架校验与自省用)
struct KnownUiSlot {
  const char *id;
  const char *kind;
  const char *label;
  bool in_list;
};

inline constexpr KnownUiSlot kKnownUiSlots[] = {
    {"musicxx.ui.home.entry", "item", "主页入口", true},
    {"musicxx.ui.song.action", "item", "歌曲菜单项", true},
    {"musicxx.ui.playlist.action", "item", "歌单菜单项", true},
    {"musicxx.ui.addsong.entry", "item", "添加歌曲来源项", true},
    {"musicxx.ui.audioEffect", "item", "音效", true},
    {"musicxx.ui.playing.background", "selectable", "播放页背景", true},
    {"musicxx.ui.playing.icon", "selectable", "播放页歌曲图", true},
    {"musicxx.ui.playing.visualizer", "selectable", "播放页音频动效", true},
    {"musicxx.ui.page.background", "pageBackground", "普通页面背景", true},
    {"musicxx.ui.playing.controlBar", "replace", "播放页控制栏", false},
    {"musicxx.ui.playing.controlBar.menu", "replace", "播放页控制栏 · 菜单按钮", false},
    {"musicxx.ui.playing.controlBar.previous", "replace", "播放页控制栏 · 上一曲按钮", false},
    {"musicxx.ui.playing.controlBar.playOrPause", "replace", "播放页控制栏 · 播放暂停按钮", false},
    {"musicxx.ui.playing.controlBar.next", "replace", "播放页控制栏 · 下一曲按钮", false},
    {"musicxx.ui.playing.controlBar.favorite", "replace", "播放页控制栏 · 收藏按钮", false},
    {"musicxx.ui.playing.lyric.top", "insert", "歌词页顶部", false},
    {"musicxx.ui.playing.lyric.bottom", "insert", "歌词页底部", false},
    {"musicxx.ui.playing.lyric.header", "insert", "歌词列表内顶部", true},
    {"musicxx.ui.playing.lyric.footer", "insert", "歌词列表内底部", true},
    {"musicxx.ui.playing.moreInfo.header", "insert", "更多信息页内容开头", true},
    {"musicxx.ui.playing.moreInfo.footer", "insert", "更多信息页内容末尾", true},
    {"musicxx.ui.playing.songlist.top", "insert", "播放列表页顶部", false},
    {"musicxx.ui.playing.songlist.bottom", "insert", "播放列表页底部", false},
    {"musicxx.ui.playing.songlist.header", "insert", "播放列表内顶部", true},
    {"musicxx.ui.playing.songlist.footer", "insert", "播放列表内底部", true},
    {"musicxx.ui.search.type", "searchType", "搜索类型", false},
    {"musicxx.ui.page", "page", "插件页面", false},
    {"musicxx.ui.overlay", "overlay", "插件浮层", false},
};

} // namespace extern_plugin
} // namespace musicxx
#endif

#endif /* MUSICXX_PLUGIN_UI_SLOTS_G_H */
