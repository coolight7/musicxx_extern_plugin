/// 自动生成（tools/gen_contract.dart ← tools/hooks.def.json）—— 请勿手改。
///
/// C++ 侧钩子约定：id 常量 + 已知钩子表（插件框架据此拒绝未知钩子、按声明的
/// 模式/策略派发；插件按常量注册，避免手写字符串打错）。
#ifndef MUSICXX_PLUGIN_HOOK_IDS_G_H
#define MUSICXX_PLUGIN_HOOK_IDS_G_H

#include <stddef.h>
#include <stdint.h>

/* ==================== 合并策略编码 (与 Dart 侧 MusicxxPluginDecisionPolicy 一致) ==================== */

#define MUSICXX_PLUGIN_HOOK_POLICY_FIRST_NON_NULL 0
#define MUSICXX_PLUGIN_HOOK_POLICY_ANY_CANCEL     1
#define MUSICXX_PLUGIN_HOOK_POLICY_ALL_MERGE      2
#define MUSICXX_PLUGIN_HOOK_POLICY_LAST_WRITE     3

/* ==================== 阶段 (与 Dart 侧 MusicxxPluginHookPhase 一致) ==================== */

/* 阶段编号：注册时用它表达参与方式 */
#define MUSICXX_PLUGIN_HOOK_PHASE_IMPL   0
#define MUSICXX_PLUGIN_HOOK_PHASE_BEFORE 1
#define MUSICXX_PLUGIN_HOOK_PHASE_AFTER  2

/* 阶段位掩码：钩子元信息的 phases 字段用它 (1 << 阶段编号) */
#define MUSICXX_PLUGIN_HOOK_PHASE_MASK_IMPL   (1u << 0)
#define MUSICXX_PLUGIN_HOOK_PHASE_MASK_BEFORE (1u << 1)
#define MUSICXX_PLUGIN_HOOK_PHASE_MASK_AFTER  (1u << 2)

/* 阶段编号 → 位掩码 (非法编号返回 0) */
static inline uint32_t musicxxPluginHookPhaseMask(int32_t phase) {
    return (phase >= 0 && phase <= 2) ? (1u << phase) : 0u;
}

/* ==================== 钩子 id 常量 ==================== */

#define MUSICXX_PLUGIN_HOOK_APP_START "musicxx.app.start"
#define MUSICXX_PLUGIN_HOOK_APP_READY "musicxx.app.ready"
#define MUSICXX_PLUGIN_HOOK_APP_BACKGROUND "musicxx.app.background"
#define MUSICXX_PLUGIN_HOOK_APP_FOREGROUND "musicxx.app.foreground"
#define MUSICXX_PLUGIN_HOOK_APP_EXIT "musicxx.app.exit"
#define MUSICXX_PLUGIN_HOOK_APP_DEEP_LINK "musicxx.app.deepLink"
#define MUSICXX_PLUGIN_HOOK_APP_UPGRADE "musicxx.app.upgrade"
#define MUSICXX_PLUGIN_HOOK_PLAYER_BEFORE_PLAY_SONG "musicxx.player.beforePlaySong"
#define MUSICXX_PLUGIN_HOOK_PLAYER_SOURCE_BEFORE_PARSE "musicxx.player.source.beforeParse"
#define MUSICXX_PLUGIN_HOOK_PLAYER_SOURCE_RESOLVED "musicxx.player.source.resolved"
#define MUSICXX_PLUGIN_HOOK_PLAYER_SOURCE_RESOLVE "musicxx.player.source.resolve"
#define MUSICXX_PLUGIN_HOOK_PLAYER_BEFORE_OPEN "musicxx.player.beforeOpen"
#define MUSICXX_PLUGIN_HOOK_PLAYER_STATE "musicxx.player.state"
#define MUSICXX_PLUGIN_HOOK_PLAYER_POSITION "musicxx.player.position"
#define MUSICXX_PLUGIN_HOOK_PLAYER_SEEK "musicxx.player.seek"
#define MUSICXX_PLUGIN_HOOK_PLAYER_VOLUME "musicxx.player.volume"
#define MUSICXX_PLUGIN_HOOK_PLAYER_SPEED "musicxx.player.speed"
#define MUSICXX_PLUGIN_HOOK_PLAYER_PITCH "musicxx.player.pitch"
#define MUSICXX_PLUGIN_HOOK_PLAYER_ERROR "musicxx.player.error"
#define MUSICXX_PLUGIN_HOOK_PLAYER_COMPLETED "musicxx.player.completed"
#define MUSICXX_PLUGIN_HOOK_PLAYER_QUALITY_CHANGED "musicxx.player.qualityChanged"
#define MUSICXX_PLUGIN_HOOK_MEDIA_NOTIFICATION "musicxx.media.notification"
#define MUSICXX_PLUGIN_HOOK_SONG_CHANGED "musicxx.song.changed"
#define MUSICXX_PLUGIN_HOOK_SONG_INFO_ANALYSE "musicxx.song.info.analyse"
#define MUSICXX_PLUGIN_HOOK_SONG_META_WRITEBACK "musicxx.song.meta.writeback"
#define MUSICXX_PLUGIN_HOOK_SONG_BEFORE_ADD "musicxx.song.beforeAdd"
#define MUSICXX_PLUGIN_HOOK_SONG_REMOVED "musicxx.song.removed"
#define MUSICXX_PLUGIN_HOOK_PLAYLIST_LOADED "musicxx.playlist.loaded"
#define MUSICXX_PLUGIN_HOOK_PLAYLIST_FILTER "musicxx.playlist.filter"
#define MUSICXX_PLUGIN_HOOK_SONGLIST_FILTER "musicxx.songlist.filter"
#define MUSICXX_PLUGIN_HOOK_HISTORY_RECORD "musicxx.history.record"
#define MUSICXX_PLUGIN_HOOK_LOCAL_SCAN_FILE "musicxx.local.scan.file"
#define MUSICXX_PLUGIN_HOOK_LOCAL_SCAN_FINISHED "musicxx.local.scan.finished"
#define MUSICXX_PLUGIN_HOOK_LYRIC_LOAD_BEFORE "musicxx.lyric.load.before"
#define MUSICXX_PLUGIN_HOOK_LYRIC_LOADED "musicxx.lyric.loaded"
#define MUSICXX_PLUGIN_HOOK_LYRIC_TRANSFORM "musicxx.lyric.transform"
#define MUSICXX_PLUGIN_HOOK_LYRIC_CURRENT "musicxx.lyric.current"
#define MUSICXX_PLUGIN_HOOK_LYRIC_SEARCH_STR "musicxx.lyric.searchStr"
#define MUSICXX_PLUGIN_HOOK_LYRIC_PROVIDER "musicxx.lyric.provider"
#define MUSICXX_PLUGIN_HOOK_ICON_REQUEST "musicxx.icon.request"
#define MUSICXX_PLUGIN_HOOK_ICON_GENERATED "musicxx.icon.generated"
#define MUSICXX_PLUGIN_HOOK_MEDIA_INFO_REQUEST "musicxx.media.info.request"
#define MUSICXX_PLUGIN_HOOK_MEDIA_WAVE_READY "musicxx.media.wave.ready"
#define MUSICXX_PLUGIN_HOOK_MEDIA_CHORUS_ANALYSED "musicxx.media.chorus.analysed"
#define MUSICXX_PLUGIN_HOOK_MEDIA_SPECTRUM_EXTRACT "musicxx.media.spectrum.extract"
#define MUSICXX_PLUGIN_HOOK_MEDIA_CHORUS_ANALYSE "musicxx.media.chorus.analyse"
#define MUSICXX_PLUGIN_HOOK_MEDIA_PALETTE_PROVIDE "musicxx.media.palette.provide"
#define MUSICXX_PLUGIN_HOOK_MEDIA_COVER_CHANGED "musicxx.media.cover.changed"
#define MUSICXX_PLUGIN_HOOK_NET_REQUEST_BEFORE "musicxx.net.request.before"
#define MUSICXX_PLUGIN_HOOK_NET_RESPONSE_AFTER "musicxx.net.response.after"
#define MUSICXX_PLUGIN_HOOK_NET_SERVER_ROUTE "musicxx.net.server.route"
#define MUSICXX_PLUGIN_HOOK_NET_MCP_TOOLS "musicxx.net.mcp.tools"
#define MUSICXX_PLUGIN_HOOK_NET_LAN_EVENT "musicxx.net.lan.event"
#define MUSICXX_PLUGIN_HOOK_DOWNLOAD_BEFORE "musicxx.download.before"
#define MUSICXX_PLUGIN_HOOK_DOWNLOAD_COMPLETED "musicxx.download.completed"
#define MUSICXX_PLUGIN_HOOK_CACHE_BEFORE_TRIM "musicxx.cache.beforeTrim"
#define MUSICXX_PLUGIN_HOOK_CACHE_PATH_REQUEST "musicxx.cache.pathRequest"
#define MUSICXX_PLUGIN_HOOK_UI_HOME_ENTRIES "musicxx.ui.home.entries"
#define MUSICXX_PLUGIN_HOOK_UI_SONG_ACTIONS "musicxx.ui.song.actions"
#define MUSICXX_PLUGIN_HOOK_UI_PLAYLIST_ACTIONS "musicxx.ui.playlist.actions"
#define MUSICXX_PLUGIN_HOOK_UI_ROUTE_RESOLVE "musicxx.ui.route.resolve"
#define MUSICXX_PLUGIN_HOOK_UI_PAGE_ENTER "musicxx.ui.page.enter"
#define MUSICXX_PLUGIN_HOOK_UI_PAGE_LEAVE "musicxx.ui.page.leave"
#define MUSICXX_PLUGIN_HOOK_UI_THEME_CHANGED "musicxx.ui.theme.changed"
#define MUSICXX_PLUGIN_HOOK_UI_NOTIFY "musicxx.ui.notify"
#define MUSICXX_PLUGIN_HOOK_UI_USER_ACTION "musicxx.ui.user.action"
#define MUSICXX_PLUGIN_HOOK_SCRIPT_BOUND "musicxx.script.bound"
#define MUSICXX_PLUGIN_HOOK_SCRIPT_DISPOSED "musicxx.script.disposed"
#define MUSICXX_PLUGIN_HOOK_SCRIPT_ACTION_EXECUTED "musicxx.script.action.executed"
#define MUSICXX_PLUGIN_HOOK_CLOCKING_TICK "musicxx.clocking.tick"
#define MUSICXX_PLUGIN_HOOK_LISTEN_TOGETHER_EVENT "musicxx.listenTogether.event"
#define MUSICXX_PLUGIN_HOOK_FEATURE_POINT_CALL "musicxx.feature.point.call"

#ifdef __cplusplus

namespace musicxx {
namespace plugin {

/// 已知钩子条目: id / 模式 / 合并策略 / 支持的阶段
struct MusicxxPluginHookMeta {
    const char* id;
    int32_t     mode;     ///< 0 观察型 / 1 裁决型 (MUSICXX_PLUGIN_HOOK_MODE_*)
    int32_t     policy;   ///< MUSICXX_PLUGIN_HOOK_POLICY_*
    uint32_t    phases;   ///< MUSICXX_PLUGIN_HOOK_PHASE_MASK_* (支持哪几个阶段)
};

/// 已知钩子表 (插件框架拒绝表外钩子: 插件不得制造插件框架不认识的钩子点)
inline constexpr MusicxxPluginHookMeta musicxxPluginKnownHooks[] = {
    {"musicxx.app.start", 0, 3, 1u},
    {"musicxx.app.ready", 0, 3, 1u},
    {"musicxx.app.background", 0, 3, 1u},
    {"musicxx.app.foreground", 0, 3, 1u},
    {"musicxx.app.exit", 1, 1, 1u},
    {"musicxx.app.deepLink", 1, 1, 1u},
    {"musicxx.app.upgrade", 0, 3, 1u},
    {"musicxx.player.beforePlaySong", 1, 1, 1u},
    {"musicxx.player.source.beforeParse", 1, 0, 1u},
    {"musicxx.player.source.resolved", 0, 3, 1u},
    {"musicxx.player.source.resolve", 1, 0, 1u},
    {"musicxx.player.beforeOpen", 1, 0, 1u},
    {"musicxx.player.state", 0, 3, 1u},
    {"musicxx.player.position", 0, 3, 1u},
    {"musicxx.player.seek", 1, 1, 1u},
    {"musicxx.player.volume", 1, 1, 1u},
    {"musicxx.player.speed", 1, 1, 1u},
    {"musicxx.player.pitch", 1, 1, 1u},
    {"musicxx.player.error", 1, 1, 1u},
    {"musicxx.player.completed", 0, 3, 1u},
    {"musicxx.player.qualityChanged", 0, 3, 1u},
    {"musicxx.media.notification", 1, 3, 1u},
    {"musicxx.song.changed", 0, 3, 1u},
    {"musicxx.song.info.analyse", 1, 0, 1u},
    {"musicxx.song.meta.writeback", 0, 3, 1u},
    {"musicxx.song.beforeAdd", 1, 1, 1u},
    {"musicxx.song.removed", 0, 3, 1u},
    {"musicxx.playlist.loaded", 0, 3, 1u},
    {"musicxx.playlist.filter", 1, 2, 1u},
    {"musicxx.songlist.filter", 1, 2, 1u},
    {"musicxx.history.record", 0, 3, 1u},
    {"musicxx.local.scan.file", 1, 1, 1u},
    {"musicxx.local.scan.finished", 0, 3, 1u},
    {"musicxx.lyric.load.before", 1, 0, 1u},
    {"musicxx.lyric.loaded", 0, 3, 1u},
    {"musicxx.lyric.transform", 1, 2, 1u},
    {"musicxx.lyric.current", 0, 3, 1u},
    {"musicxx.lyric.searchStr", 1, 0, 1u},
    {"musicxx.lyric.provider", 1, 0, 1u},
    {"musicxx.icon.request", 1, 0, 1u},
    {"musicxx.icon.generated", 0, 3, 1u},
    {"musicxx.media.info.request", 1, 0, 1u},
    {"musicxx.media.wave.ready", 0, 3, 1u},
    {"musicxx.media.chorus.analysed", 0, 3, 1u},
    {"musicxx.media.spectrum.extract", 1, 0, 1u},
    {"musicxx.media.chorus.analyse", 1, 0, 1u},
    {"musicxx.media.palette.provide", 1, 0, 1u},
    {"musicxx.media.cover.changed", 0, 3, 1u},
    {"musicxx.net.request.before", 1, 1, 1u},
    {"musicxx.net.response.after", 0, 3, 1u},
    {"musicxx.net.server.route", 1, 0, 1u},
    {"musicxx.net.mcp.tools", 1, 2, 1u},
    {"musicxx.net.lan.event", 0, 3, 1u},
    {"musicxx.download.before", 1, 1, 1u},
    {"musicxx.download.completed", 0, 3, 1u},
    {"musicxx.cache.beforeTrim", 1, 1, 1u},
    {"musicxx.cache.pathRequest", 1, 0, 1u},
    {"musicxx.ui.home.entries", 1, 2, 1u},
    {"musicxx.ui.song.actions", 1, 2, 1u},
    {"musicxx.ui.playlist.actions", 1, 2, 1u},
    {"musicxx.ui.route.resolve", 1, 0, 1u},
    {"musicxx.ui.page.enter", 0, 3, 1u},
    {"musicxx.ui.page.leave", 0, 3, 1u},
    {"musicxx.ui.theme.changed", 0, 3, 1u},
    {"musicxx.ui.notify", 1, 1, 1u},
    {"musicxx.ui.user.action", 0, 3, 1u},
    {"musicxx.script.bound", 0, 3, 1u},
    {"musicxx.script.disposed", 0, 3, 1u},
    {"musicxx.script.action.executed", 0, 3, 1u},
    {"musicxx.clocking.tick", 0, 3, 1u},
    {"musicxx.listenTogether.event", 0, 3, 1u},
    {"musicxx.feature.point.call", 1, 0, 1u},
};

/// 已知钩子条目数
inline constexpr size_t musicxxPluginKnownHookCount = sizeof(musicxxPluginKnownHooks) / sizeof(musicxxPluginKnownHooks[0]);

} // namespace plugin
} // namespace musicxx

#endif /* __cplusplus */

#endif /* MUSICXX_PLUGIN_HOOK_IDS_G_H */
