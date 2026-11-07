/// 自动生成（tools/gen_contract.dart ← tools/capabilities.def.json）—— 请勿手改。
///
/// C++ 侧能力与广播约定：id 常量 + 已知声明表（插件框架据此拒绝未知 id、
/// 判实现槽位 / 合并策略 / 阶段 / 超时 / 缓存与写权限）。
#ifndef MUSICXX_CAPABILITY_IDS_G_H
#define MUSICXX_CAPABILITY_IDS_G_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ==================== 实现槽位 ==================== */

/// 一个槽位：后登记的覆盖先登记的
#define MUSICXX_CAPABILITY_IMPLS_ONE  0
/// 多个槽位：多实现按 (层, priority, 登记顺序) 共存
#define MUSICXX_CAPABILITY_IMPLS_MANY 1

/* ==================== 合并策略 ==================== */

/// 第一个给出合法结果的实现生效
#define MUSICXX_CAPABILITY_MERGE_FIRST 1
/// 任一实现能否决（最保守），patch 由后链改
#define MUSICXX_CAPABILITY_MERGE_VETO  2

/* ==================== 阶段位掩码 ==================== */

/* 阶段编号：注册时用它表达参与方式 */
#define MUSICXX_CAPABILITY_PHASE_IMPL   0
#define MUSICXX_CAPABILITY_PHASE_BEFORE 1
#define MUSICXX_CAPABILITY_PHASE_AFTER  2

/* 阶段位掩码：声明里的 phase_mask 字段用它 (1 << 阶段编号) */
#define MUSICXX_CAPABILITY_PHASE_MASK_IMPL   (1u << 0)
#define MUSICXX_CAPABILITY_PHASE_MASK_BEFORE (1u << 1)
#define MUSICXX_CAPABILITY_PHASE_MASK_AFTER  (1u << 2)

/* ==================== 派发方式 ==================== */

/// 调用方等结果
#define MUSICXX_CAPABILITY_DISPATCH_SYNC  0
/// 不占用调用线程
#define MUSICXX_CAPABILITY_DISPATCH_ASYNC 1

/* ==================== 值缓存 ==================== */

#define MUSICXX_CAPABILITY_CACHE_NONE        0
#define MUSICXX_CAPABILITY_CACHE_BY_IDENTITY 1

/* ==================== 用途位掩码 ==================== */

#define MUSICXX_CAPABILITY_USAGE_POINT   0x1u
#define MUSICXX_CAPABILITY_USAGE_COMMAND 0x2u
#define MUSICXX_CAPABILITY_USAGE_SERVICE 0x4u

/* ==================== 外部入口位掩码 ==================== */

#define MUSICXX_CAPABILITY_EXPOSE_CLI    0x01u
#define MUSICXX_CAPABILITY_EXPOSE_MCP    0x02u
#define MUSICXX_CAPABILITY_EXPOSE_LINK   0x04u
#define MUSICXX_CAPABILITY_EXPOSE_PLUGIN 0x08u
#define MUSICXX_CAPABILITY_EXPOSE_UI     0x10u
#define MUSICXX_CAPABILITY_EXPOSE_PAGE   0x20u
#define MUSICXX_CAPABILITY_EXPOSE_DRIVE  0x40u

/* ==================== 链接触发时是否先问用户 ==================== */

#define MUSICXX_CAPABILITY_CONSENT_NONE 0
#define MUSICXX_CAPABILITY_CONSENT_ASK  1

/* ==================== 广播的缓存与写权限 ==================== */

/// 事件：没有当前值
#define MUSICXX_BROADCAST_CACHE_NONE   0
/// 快照：订阅即拿到当前值，也能同步读
#define MUSICXX_BROADCAST_CACHE_LATEST 1

/// 只有属主能写
#define MUSICXX_BROADCAST_WRITABLE_OWNER  0
/// 任何人都能写（变量）
#define MUSICXX_BROADCAST_WRITABLE_ANYONE 1

/// 业务频道
#define MUSICXX_BROADCAST_SCOPE_APP    0
/// 调试与自省频道（插件不能写）
#define MUSICXX_BROADCAST_SCOPE_SYSTEM 1

/* ==================== 能力 / 广播 id 常量 ==================== */

#define MUSICXX_CAPABILITY_APP_EXIT "musicxx.app.exit"
#define MUSICXX_CAPABILITY_APP_DEEP_LINK "musicxx.app.deepLink"
#define MUSICXX_CAPABILITY_PLAYER_BEFORE_PLAY_SONG "musicxx.player.beforePlaySong"
#define MUSICXX_CAPABILITY_PLAYER_SOURCE_BEFORE_PARSE "musicxx.player.source.beforeParse"
#define MUSICXX_CAPABILITY_PLAYER_SOURCE_RESOLVE "musicxx.player.source.resolve"
#define MUSICXX_CAPABILITY_PLAYER_SEEK "musicxx.player.seek"
#define MUSICXX_CAPABILITY_PLAYER_VOLUME "musicxx.player.volume"
#define MUSICXX_CAPABILITY_PLAYER_SPEED "musicxx.player.speed"
#define MUSICXX_CAPABILITY_PLAYER_PITCH "musicxx.player.pitch"
#define MUSICXX_CAPABILITY_PLAYER_ERROR "musicxx.player.error"
#define MUSICXX_CAPABILITY_MEDIA_NOTIFICATION "musicxx.media.notification"
#define MUSICXX_CAPABILITY_MEDIA_INFO_ANALYSE "musicxx.media.info.analyse"
#define MUSICXX_CAPABILITY_SONG_BEFORE_ADD "musicxx.song.beforeAdd"
#define MUSICXX_CAPABILITY_PLAYLIST_LOAD "musicxx.playlist.load"
#define MUSICXX_CAPABILITY_LOCAL_SCAN_FILE "musicxx.local.scan.file"
#define MUSICXX_CAPABILITY_LYRIC_LOAD "musicxx.lyric.load"
#define MUSICXX_CAPABILITY_LYRIC_LOAD_MUSICXX_SERVER "musicxx.lyric.load.musicxx_server"
#define MUSICXX_CAPABILITY_LYRIC_LOAD_CACHE "musicxx.lyric.load.cache"
#define MUSICXX_CAPABILITY_LYRIC_LOAD_BUILTIN "musicxx.lyric.load.builtin"
#define MUSICXX_CAPABILITY_LYRIC_LOAD_BUILTOUT "musicxx.lyric.load.builtout"
#define MUSICXX_CAPABILITY_LYRIC_LOAD_MATCH "musicxx.lyric.load.match"
#define MUSICXX_CAPABILITY_MEDIA_SPECTRUM_EXTRACT "musicxx.media.spectrum.extract"
#define MUSICXX_CAPABILITY_MEDIA_CHORUS_ANALYSE "musicxx.media.chorus.analyse"
#define MUSICXX_CAPABILITY_MEDIA_PALETTE_PROVIDE "musicxx.media.palette.provide"
#define MUSICXX_CAPABILITY_APP_NET_HTTP_REQUEST "musicxx.app.net.http.request"
#define MUSICXX_CAPABILITY_NET_SERVER_ROUTE "musicxx.net.server.route"
#define MUSICXX_CAPABILITY_NET_MCP_TOOLS "musicxx.net.mcp.tools"
#define MUSICXX_CAPABILITY_DOWNLOAD "musicxx.download"
#define MUSICXX_CAPABILITY_CACHE_BEFORE_TRIM "musicxx.cache.beforeTrim"
#define MUSICXX_CAPABILITY_CACHE_PATH_REQUEST "musicxx.cache.pathRequest"
#define MUSICXX_CAPABILITY_UI_ROUTE_RESOLVE "musicxx.ui.route.resolve"
#define MUSICXX_CAPABILITY_FEATURE_POINT_CALL "musicxx.feature.point.call"
#define MUSICXX_CAPABILITY_APP_FOREGROUND "musicxx.app.foreground"
#define MUSICXX_CAPABILITY_APP_BACKGROUND "musicxx.app.background"
#define MUSICXX_CAPABILITY_PLAYER_PLAY "musicxx.player.play"
#define MUSICXX_CAPABILITY_PLAYER_PAUSE "musicxx.player.pause"
#define MUSICXX_CAPABILITY_PLAYER_TOGGLE "musicxx.player.toggle"
#define MUSICXX_CAPABILITY_PLAYER_STOP "musicxx.player.stop"
#define MUSICXX_CAPABILITY_PLAYER_NEXT "musicxx.player.next"
#define MUSICXX_CAPABILITY_PLAYER_PREV "musicxx.player.prev"
#define MUSICXX_CAPABILITY_PLAYER_PLAY_SRC "musicxx.player.playSrc"
#define MUSICXX_CAPABILITY_PLAYER_SET_VOLUME "musicxx.player.setVolume"
#define MUSICXX_CAPABILITY_PLAYER_SET_SPEED "musicxx.player.setSpeed"
#define MUSICXX_CAPABILITY_PLAYER_SET_PITCH "musicxx.player.setPitch"
#define MUSICXX_CAPABILITY_PLAYER_SET_QUALITY "musicxx.player.setQuality"
#define MUSICXX_CAPABILITY_PLAYER_SET_LOOP_MODE "musicxx.player.setLoopMode"
#define MUSICXX_CAPABILITY_PLAYER_SET_MEDIA_TYPE "musicxx.player.setMediaType"
#define MUSICXX_CAPABILITY_PLAYLIST_QUERY_LISTS "musicxx.playlist.queryLists"
#define MUSICXX_CAPABILITY_PLAYLIST_QUERY_SONGS "musicxx.playlist.querySongs"
#define MUSICXX_CAPABILITY_PLAYLIST_PLAY "musicxx.playlist.play"
#define MUSICXX_CAPABILITY_PLAYLIST_ADD_SONG "musicxx.playlist.addSong"
#define MUSICXX_CAPABILITY_PLAYLIST_REMOVE_SONG "musicxx.playlist.removeSong"
#define MUSICXX_CAPABILITY_PLAYLIST_CREATE "musicxx.playlist.create"
#define MUSICXX_CAPABILITY_PLAYLIST_UPDATE_SONG "musicxx.playlist.updateSong"
#define MUSICXX_CAPABILITY_PLAYLIST_SYNC_SONGS_SYNC "musicxx.playlist.syncSongs.sync"
#define MUSICXX_CAPABILITY_SONGLIST_QUERY_SONGS "musicxx.songlist.querySongs"
#define MUSICXX_CAPABILITY_SONGLIST_PLAY_SONG "musicxx.songlist.playSong"
#define MUSICXX_CAPABILITY_SONGLIST_ADD_SONG "musicxx.songlist.addSong"
#define MUSICXX_CAPABILITY_SONGLIST_REMOVE_SONG "musicxx.songlist.removeSong"
#define MUSICXX_CAPABILITY_SONGLIST_UPDATE_SONG "musicxx.songlist.updateSong"
#define MUSICXX_CAPABILITY_LIBRARY_QUERY_SONGLISTS "musicxx.library.querySonglists"
#define MUSICXX_CAPABILITY_LIBRARY_QUERY_SONGS "musicxx.library.querySongs"
#define MUSICXX_CAPABILITY_LIBRARY_QUERY_SONGLIST "musicxx.library.querySonglist"
#define MUSICXX_CAPABILITY_LIBRARY_PLAY_SONG "musicxx.library.playSong"
#define MUSICXX_CAPABILITY_LIBRARY_PLAY_SONGLIST "musicxx.library.playSonglist"
#define MUSICXX_CAPABILITY_LIBRARY_REMOVE_SONG "musicxx.library.removeSong"
#define MUSICXX_CAPABILITY_LIBRARY_CREATE_SONGLIST "musicxx.library.createSonglist"
#define MUSICXX_CAPABILITY_LIBRARY_SET_SONG_INFO "musicxx.library.setSongInfo"
#define MUSICXX_CAPABILITY_LIBRARY_SEARCH "musicxx.library.search"
#define MUSICXX_CAPABILITY_LYRIC_CURRENT_GET "musicxx.lyric.current.get"
#define MUSICXX_CAPABILITY_LYRIC_CURRENT_UPDATE "musicxx.lyric.current.update"
#define MUSICXX_CAPABILITY_LYRIC_CURRENT_UPDATE_CONTENT "musicxx.lyric.current.updateContent"
#define MUSICXX_CAPABILITY_LYRIC_GET_BY_SRC "musicxx.lyric.getBySrc"
#define MUSICXX_CAPABILITY_LYRIC_SEARCH "musicxx.lyric.search"
#define MUSICXX_CAPABILITY_UI_TOAST "musicxx.ui.toast"
#define MUSICXX_CAPABILITY_UI_DIALOG "musicxx.ui.dialog"
#define MUSICXX_CAPABILITY_UI_OVERLAY_SHOW "musicxx.ui.overlay.show"
#define MUSICXX_CAPABILITY_UI_OVERLAY_CLOSE "musicxx.ui.overlay.close"
#define MUSICXX_CAPABILITY_UI_OPEN_OVERLAY_SHEET "musicxx.ui.openOverlaySheet"
#define MUSICXX_CAPABILITY_UI_OPEN_ROUTE "musicxx.ui.openRoute"
#define MUSICXX_CAPABILITY_UI_PICK_SONGS "musicxx.ui.pickSongs"
#define MUSICXX_CAPABILITY_UI_PICK_DIR "musicxx.ui.pickDir"
#define MUSICXX_CAPABILITY_UI_BROWSE_DRIVE "musicxx.ui.browseDrive"
#define MUSICXX_CAPABILITY_UI_SET_ENTRY_BADGE "musicxx.ui.setEntryBadge"
#define MUSICXX_CAPABILITY_UI_SET_FONT_FAMILY "musicxx.ui.setFontFamily"
#define MUSICXX_CAPABILITY_UI_GET_FONT_FAMILY "musicxx.ui.getFontFamily"
#define MUSICXX_CAPABILITY_UI_LOAD_FONT "musicxx.ui.loadFont"
#define MUSICXX_CAPABILITY_STORAGE_GET "musicxx.storage.get"
#define MUSICXX_CAPABILITY_STORAGE_SET "musicxx.storage.set"
#define MUSICXX_CAPABILITY_STORAGE_DELETE "musicxx.storage.delete"
#define MUSICXX_CAPABILITY_STORAGE_LIST "musicxx.storage.list"
#define MUSICXX_CAPABILITY_NET_FETCH "musicxx.net.fetch"
#define MUSICXX_CAPABILITY_NET_DOWNLOAD "musicxx.net.download"
#define MUSICXX_CAPABILITY_RENDER_LIST "musicxx.render.list"
#define MUSICXX_CAPABILITY_RENDER_CURRENT "musicxx.render.current"
#define MUSICXX_CAPABILITY_RENDER_SELECT "musicxx.render.select"
#define MUSICXX_CAPABILITY_MEDIA_PALETTE "musicxx.media.palette"
#define MUSICXX_CAPABILITY_MEDIA_COVER "musicxx.media.cover"
#define MUSICXX_CAPABILITY_MEDIA_PALETTE_SET "musicxx.media.palette.set"
#define MUSICXX_CAPABILITY_MEDIA_BIND_IMAGE "musicxx.media.bindImage"
#define MUSICXX_CAPABILITY_MEDIA_UNBIND_IMAGE "musicxx.media.unbindImage"
#define MUSICXX_CAPABILITY_MEDIA_SPECTRUM "musicxx.media.spectrum"
#define MUSICXX_CAPABILITY_MEDIA_KERNEL "musicxx.media.kernel"
#define MUSICXX_CAPABILITY_FEATURE_LIST "musicxx.feature.list"
#define MUSICXX_CAPABILITY_FEATURE_CALL "musicxx.feature.call"
#define MUSICXX_CAPABILITY_FEATURE_POINT_LIST "musicxx.feature.point.list"
#define MUSICXX_CAPABILITY_FEATURE_POINT_DEFINE "musicxx.feature.point.define"
#define MUSICXX_CAPABILITY_FEATURE_POINT_UNDEFINE "musicxx.feature.point.undefine"
#define MUSICXX_CAPABILITY_SOURCE_DEFINE "musicxx.source.define"
#define MUSICXX_CAPABILITY_SOURCE_UNDEFINE "musicxx.source.undefine"
#define MUSICXX_CAPABILITY_SOURCE_LIST "musicxx.source.list"
#define MUSICXX_CAPABILITY_SOURCE_BROWSE "musicxx.source.browse"
#define MUSICXX_CAPABILITY_CAPABILITY_SUBSCRIBERS "musicxx.capability.subscribers"
#define MUSICXX_CAPABILITY_HOST_OPEN_URL "musicxx.host.openUrl"
#define MUSICXX_CAPABILITY_HOST_SHARE_TEXT "musicxx.host.shareText"
#define MUSICXX_CAPABILITY_HOST_CLIPBOARD "musicxx.host.clipboard"
#define MUSICXX_CAPABILITY_HOST_GET_PATH "musicxx.host.getPath"
#define MUSICXX_CAPABILITY_STATS_REPORT_MEMORY "musicxx.stats.reportMemory"
#define MUSICXX_CAPABILITY_STATS_REPORT_METRIC "musicxx.stats.reportMetric"

#define MUSICXX_BROADCAST_APP_START "musicxx.app.start"
#define MUSICXX_BROADCAST_APP_READY "musicxx.app.ready"
#define MUSICXX_BROADCAST_APP_BACKGROUND "musicxx.app.background"
#define MUSICXX_BROADCAST_APP_FOREGROUND "musicxx.app.foreground"
#define MUSICXX_BROADCAST_APP_UPGRADE "musicxx.app.upgrade"
#define MUSICXX_BROADCAST_PLAYER_SOURCE_RESOLVED "musicxx.player.source.resolved"
#define MUSICXX_BROADCAST_PLAYER_STATE "musicxx.player.state"
#define MUSICXX_BROADCAST_PLAYER_POSITION "musicxx.player.position"
#define MUSICXX_BROADCAST_PLAYER_COMPLETED "musicxx.player.completed"
#define MUSICXX_BROADCAST_PLAYER_QUALITY_CHANGED "musicxx.player.qualityChanged"
#define MUSICXX_BROADCAST_SONG_CHANGED "musicxx.song.changed"
#define MUSICXX_BROADCAST_SONG_META_WRITEBACK "musicxx.song.meta.writeback"
#define MUSICXX_BROADCAST_SONG_REMOVED "musicxx.song.removed"
#define MUSICXX_BROADCAST_HISTORY_RECORD "musicxx.history.record"
#define MUSICXX_BROADCAST_LOCAL_SCAN_FINISHED "musicxx.local.scan.finished"
#define MUSICXX_BROADCAST_LYRIC_CURRENT "musicxx.lyric.current"
#define MUSICXX_BROADCAST_MEDIA_COVER_CHANGED "musicxx.media.cover.changed"
#define MUSICXX_BROADCAST_NET_LAN_EVENT "musicxx.net.lan.event"
#define MUSICXX_BROADCAST_UI_PAGE_ENTER "musicxx.ui.page.enter"
#define MUSICXX_BROADCAST_UI_PAGE_LEAVE "musicxx.ui.page.leave"
#define MUSICXX_BROADCAST_UI_THEME_CHANGED "musicxx.ui.theme.changed"
#define MUSICXX_BROADCAST_UI_USER_ACTION "musicxx.ui.user.action"
#define MUSICXX_BROADCAST_STATE_APP "musicxx.state.app"
#define MUSICXX_BROADCAST_STATE_PLAYER "musicxx.state.player"
#define MUSICXX_BROADCAST_STATE_SONG "musicxx.state.song"
#define MUSICXX_BROADCAST_STATE_PLAYLIST "musicxx.state.playlist"
#define MUSICXX_BROADCAST_STATE_LYRIC "musicxx.state.lyric"
#define MUSICXX_BROADCAST_STATE_PLAYLISTS "musicxx.state.playlists"
#define MUSICXX_BROADCAST_STATE_ENV "musicxx.state.env"
#define MUSICXX_BROADCAST_STATE_RENDER_SLOTS "musicxx.state.renderSlots"
#define MUSICXX_BROADCAST_STATE_SPECTRUM "musicxx.state.spectrum"
#define MUSICXX_BROADCAST_STATE_CHANGED "musicxx.state.changed"
#define MUSICXX_BROADCAST_EVENT_PUBLISHED "musicxx.event.published"

/* ==================== 已知声明表 ==================== */

/// 一条能力的声明（字段编码见上面的宏）
typedef struct MusicxxCapabilityDecl {
    const char* id;
    int32_t     impls;       ///< MUSICXX_CAPABILITY_IMPLS_*
    int32_t     merge;       ///< MUSICXX_CAPABILITY_MERGE_*（0 = 不适用）
    uint32_t    phase_mask;  ///< MUSICXX_CAPABILITY_PHASE_MASK_*
    int32_t     dispatch;    ///< MUSICXX_CAPABILITY_DISPATCH_*
    int32_t     timeout_ms;  ///< 等实现的上限（0 = 不设上限）
    int32_t     cache;       ///< MUSICXX_CAPABILITY_CACHE_*
    uint32_t    usage_mask;  ///< MUSICXX_CAPABILITY_USAGE_*
    uint32_t    expose_mask; ///< MUSICXX_CAPABILITY_EXPOSE_*
    int32_t     consent;     ///< MUSICXX_CAPABILITY_CONSENT_*
    int32_t     implemented; ///< 主程序是否已经接入
} MusicxxCapabilityDecl;

/// 一条广播的声明
typedef struct MusicxxBroadcastDecl {
    const char* id;
    int32_t     cache;             ///< MUSICXX_BROADCAST_CACHE_*
    int32_t     writable;          ///< MUSICXX_BROADCAST_WRITABLE_*
    int32_t     push_on_subscribe; ///< 订阅时立刻推当前值
    int32_t     coalesce;          ///< 允许合并（允许丢中间值）
    int32_t     scope;             ///< MUSICXX_BROADCAST_SCOPE_*
    int32_t     implemented;       ///< 主程序是否已经接入
} MusicxxBroadcastDecl;

static const MusicxxCapabilityDecl musicxxCapabilityDecls[] = {
    {"musicxx.app.exit", 1, 2, 1u, 0, 0, 0, 1u, 0u, 0, 0},
    {"musicxx.app.deepLink", 1, 2, 1u, 0, 0, 0, 1u, 0u, 0, 0},
    {"musicxx.player.beforePlaySong", 1, 2, 5u, 1, 0, 0, 1u, 0u, 0, 1},
    {"musicxx.player.source.beforeParse", 1, 1, 5u, 1, 0, 0, 1u, 0u, 0, 1},
    {"musicxx.player.source.resolve", 1, 1, 1u, 1, 5000, 1, 1u, 0u, 0, 1},
    {"musicxx.player.seek", 1, 2, 5u, 1, 500, 0, 3u, 15u, 0, 1},
    {"musicxx.player.volume", 1, 2, 5u, 1, 500, 0, 1u, 0u, 0, 1},
    {"musicxx.player.speed", 1, 2, 5u, 1, 500, 0, 1u, 0u, 0, 1},
    {"musicxx.player.pitch", 1, 2, 5u, 1, 500, 0, 1u, 0u, 0, 1},
    {"musicxx.player.error", 1, 2, 5u, 1, 0, 0, 1u, 0u, 0, 1},
    {"musicxx.media.notification", 1, 1, 1u, 0, 0, 0, 1u, 0u, 0, 0},
    {"musicxx.media.info.analyse", 1, 1, 1u, 1, 0, 0, 1u, 0u, 0, 0},
    {"musicxx.song.beforeAdd", 1, 2, 1u, 0, 0, 0, 1u, 0u, 0, 0},
    {"musicxx.playlist.load", 0, 0, 6u, 1, 0, 0, 1u, 0u, 0, 0},
    {"musicxx.local.scan.file", 1, 2, 1u, 0, 0, 0, 1u, 0u, 0, 0},
    {"musicxx.lyric.load", 1, 1, 7u, 1, 8000, 1, 1u, 0u, 0, 1},
    {"musicxx.lyric.load.musicxx_server", 1, 1, 7u, 1, 8000, 1, 1u, 0u, 0, 1},
    {"musicxx.lyric.load.cache", 1, 1, 7u, 1, 8000, 1, 1u, 0u, 0, 1},
    {"musicxx.lyric.load.builtin", 1, 1, 7u, 1, 8000, 1, 1u, 0u, 0, 1},
    {"musicxx.lyric.load.builtout", 1, 1, 7u, 1, 8000, 1, 1u, 0u, 0, 1},
    {"musicxx.lyric.load.match", 1, 1, 7u, 1, 8000, 1, 1u, 0u, 0, 1},
    {"musicxx.media.spectrum.extract", 1, 1, 1u, 1, 30000, 1, 1u, 0u, 0, 1},
    {"musicxx.media.chorus.analyse", 1, 1, 1u, 1, 20000, 1, 1u, 0u, 0, 1},
    {"musicxx.media.palette.provide", 1, 1, 1u, 1, 800, 1, 1u, 0u, 0, 1},
    {"musicxx.app.net.http.request", 0, 0, 6u, 1, 0, 0, 1u, 0u, 0, 0},
    {"musicxx.net.server.route", 1, 1, 1u, 1, 0, 0, 1u, 0u, 0, 0},
    {"musicxx.net.mcp.tools", 1, 1, 1u, 0, 0, 0, 1u, 0u, 0, 0},
    {"musicxx.download", 0, 0, 6u, 1, 0, 0, 1u, 0u, 0, 0},
    {"musicxx.cache.beforeTrim", 1, 2, 1u, 0, 0, 0, 1u, 0u, 0, 0},
    {"musicxx.cache.pathRequest", 1, 1, 1u, 0, 0, 0, 1u, 0u, 0, 0},
    {"musicxx.ui.route.resolve", 1, 1, 1u, 0, 0, 0, 1u, 0u, 0, 0},
    {"musicxx.feature.point.call", 1, 1, 7u, 1, 0, 0, 3u, 15u, 0, 1},
    {"musicxx.app.foreground", 0, 0, 1u, 1, 0, 0, 2u, 15u, 0, 1},
    {"musicxx.app.background", 0, 0, 1u, 1, 0, 0, 2u, 15u, 0, 1},
    {"musicxx.player.play", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.player.pause", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.player.toggle", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.player.stop", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.player.next", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.player.prev", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.player.playSrc", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.player.setVolume", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.player.setSpeed", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.player.setPitch", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.player.setQuality", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.player.setLoopMode", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.player.setMediaType", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 0},
    {"musicxx.playlist.queryLists", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.playlist.querySongs", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.playlist.play", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.playlist.addSong", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.playlist.removeSong", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.playlist.create", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.playlist.updateSong", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.playlist.syncSongs.sync", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.songlist.querySongs", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.songlist.playSong", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.songlist.addSong", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.songlist.removeSong", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.songlist.updateSong", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.library.querySonglists", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 0},
    {"musicxx.library.querySongs", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 0},
    {"musicxx.library.querySonglist", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 0},
    {"musicxx.library.playSong", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 0},
    {"musicxx.library.playSonglist", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 0},
    {"musicxx.library.removeSong", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 0},
    {"musicxx.library.createSonglist", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 0},
    {"musicxx.library.setSongInfo", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 0},
    {"musicxx.library.search", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 0},
    {"musicxx.lyric.current.get", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.lyric.current.update", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.lyric.current.updateContent", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.lyric.getBySrc", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.lyric.search", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.ui.toast", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.ui.dialog", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.ui.overlay.show", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.ui.overlay.close", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.ui.openOverlaySheet", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.ui.openRoute", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.ui.pickSongs", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.ui.pickDir", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.ui.browseDrive", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.ui.setEntryBadge", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 0},
    {"musicxx.ui.setFontFamily", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.ui.getFontFamily", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.ui.loadFont", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.storage.get", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.storage.set", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.storage.delete", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.storage.list", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.net.fetch", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.net.download", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.render.list", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.render.current", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.render.select", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.media.palette", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.media.cover", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.media.palette.set", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.media.bindImage", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.media.unbindImage", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.media.spectrum", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.media.kernel", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.feature.list", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.feature.call", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.feature.point.list", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.feature.point.define", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.feature.point.undefine", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.source.define", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.source.undefine", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.source.list", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.source.browse", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.capability.subscribers", 0, 0, 1u, 1, 5000, 0, 2u, 8u, 0, 1},
    {"musicxx.host.openUrl", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.host.shareText", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 0},
    {"musicxx.host.clipboard", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.host.getPath", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.stats.reportMemory", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
    {"musicxx.stats.reportMetric", 0, 0, 1u, 1, 5000, 0, 2u, 15u, 0, 1},
};

/// 已知能力条目数
static const size_t musicxxCapabilityDeclCount = sizeof(musicxxCapabilityDecls) / sizeof(musicxxCapabilityDecls[0]);

static const MusicxxBroadcastDecl musicxxBroadcastDecls[] = {
    {"musicxx.app.start", 0, 0, 0, 1, 0, 1},
    {"musicxx.app.ready", 0, 0, 0, 1, 0, 1},
    {"musicxx.app.background", 0, 0, 0, 1, 0, 1},
    {"musicxx.app.foreground", 0, 0, 0, 1, 0, 1},
    {"musicxx.app.upgrade", 0, 0, 0, 1, 0, 0},
    {"musicxx.player.source.resolved", 0, 0, 0, 1, 0, 1},
    {"musicxx.player.state", 0, 0, 0, 1, 0, 1},
    {"musicxx.player.position", 0, 0, 0, 1, 0, 1},
    {"musicxx.player.completed", 0, 0, 0, 1, 0, 1},
    {"musicxx.player.qualityChanged", 0, 0, 0, 1, 0, 0},
    {"musicxx.song.changed", 0, 0, 0, 1, 0, 1},
    {"musicxx.song.meta.writeback", 0, 0, 0, 1, 0, 0},
    {"musicxx.song.removed", 0, 0, 0, 1, 0, 0},
    {"musicxx.history.record", 0, 0, 0, 1, 0, 0},
    {"musicxx.local.scan.finished", 0, 0, 0, 1, 0, 0},
    {"musicxx.lyric.current", 0, 0, 0, 1, 0, 0},
    {"musicxx.media.cover.changed", 0, 0, 0, 1, 0, 1},
    {"musicxx.net.lan.event", 0, 0, 0, 1, 0, 0},
    {"musicxx.ui.page.enter", 0, 0, 0, 1, 0, 0},
    {"musicxx.ui.page.leave", 0, 0, 0, 1, 0, 0},
    {"musicxx.ui.theme.changed", 0, 0, 0, 1, 0, 0},
    {"musicxx.ui.user.action", 0, 0, 0, 1, 0, 0},
    {"musicxx.state.app", 1, 0, 1, 1, 0, 1},
    {"musicxx.state.player", 1, 0, 1, 1, 0, 1},
    {"musicxx.state.song", 1, 0, 1, 1, 0, 1},
    {"musicxx.state.playlist", 1, 0, 1, 1, 0, 1},
    {"musicxx.state.lyric", 1, 0, 1, 1, 0, 1},
    {"musicxx.state.playlists", 1, 0, 1, 1, 0, 1},
    {"musicxx.state.env", 1, 0, 1, 1, 0, 1},
    {"musicxx.state.renderSlots", 1, 0, 1, 1, 0, 1},
    {"musicxx.state.spectrum", 1, 0, 1, 1, 0, 1},
    {"musicxx.state.changed", 0, 0, 0, 1, 1, 1},
    {"musicxx.event.published", 0, 0, 0, 1, 1, 1},
};

/// 已知广播条目数
static const size_t musicxxBroadcastDeclCount = sizeof(musicxxBroadcastDecls) / sizeof(musicxxBroadcastDecls[0]);

/// 按 id 查能力声明（没有返回 NULL）
static inline const MusicxxCapabilityDecl* musicxxCapabilityDeclOf(const char* id) {
    if (!id) {
        return NULL;
    }
    for (size_t i = 0; i < musicxxCapabilityDeclCount; ++i) {
        if (strcmp(musicxxCapabilityDecls[i].id, id) == 0) {
            return &musicxxCapabilityDecls[i];
        }
    }
    return NULL;
}

/// 按 id 查广播声明（没有返回 NULL）
static inline const MusicxxBroadcastDecl* musicxxBroadcastDeclOf(const char* id) {
    if (!id) {
        return NULL;
    }
    for (size_t i = 0; i < musicxxBroadcastDeclCount; ++i) {
        if (strcmp(musicxxBroadcastDecls[i].id, id) == 0) {
            return &musicxxBroadcastDecls[i];
        }
    }
    return NULL;
}

#endif /* MUSICXX_CAPABILITY_IDS_G_H */
