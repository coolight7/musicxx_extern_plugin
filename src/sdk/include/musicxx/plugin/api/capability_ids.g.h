/// 自动生成（tools/gen_contract.dart ← tools/capabilities.def.json）—— 请勿手改。
///
/// C++ 侧能力与广播约定：id 常量 + 预设声明表（表里的 id 带实现槽位 /
/// 合并策略 / 阶段 / 超时；表外的 id 一律按默认属性处理）。
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

/* ==================== 外部通道位掩码 ==================== */

/// 命令面能调（命令行 / /cli / 链接）
#define MUSICXX_CAPABILITY_CHANNEL_CLI    0x01u
/// 进 MCP 工具表
#define MUSICXX_CAPABILITY_CHANNEL_MCP    0x02u
/// 插件可以在 musicxx.capability.call 里调
#define MUSICXX_CAPABILITY_CHANNEL_PLUGIN 0x04u

/* ==================== 广播的缓存与发布权限 ==================== */

/// 没有当前值：每次发布都通知
#define MUSICXX_BROADCAST_CACHE_NONE   0
/// 有当前值：订阅即拿到，也能同步读
#define MUSICXX_BROADCAST_CACHE_LATEST 1

/// 只有属主能发布
#define MUSICXX_BROADCAST_PUBLISH_OWNER  0
/// 任何人都能发布
#define MUSICXX_BROADCAST_PUBLISH_ANYONE 1

/// 业务频道
#define MUSICXX_BROADCAST_SCOPE_APP    0
/// 调试与自省频道（插件不能写）
#define MUSICXX_BROADCAST_SCOPE_SYSTEM 1

/* ==================== 能力 / 广播 id 常量 ==================== */

#define MUSICXX_CAPABILITY_PLAYER_BEFORE_PLAY_SONG "musicxx.player.beforePlaySong"
#define MUSICXX_CAPABILITY_PLAYER_SOURCE_BEFORE_PARSE "musicxx.player.source.beforeParse"
#define MUSICXX_CAPABILITY_PLAYER_SOURCE_RESOLVE "musicxx.player.source.resolve"
#define MUSICXX_CAPABILITY_PLAYER_SEEK "musicxx.player.seek"
#define MUSICXX_CAPABILITY_PLAYER_VOLUME "musicxx.player.volume"
#define MUSICXX_CAPABILITY_PLAYER_SPEED "musicxx.player.speed"
#define MUSICXX_CAPABILITY_PLAYER_PITCH "musicxx.player.pitch"
#define MUSICXX_CAPABILITY_PLAYER_ERROR "musicxx.player.error"
#define MUSICXX_CAPABILITY_LYRIC_LOAD "musicxx.lyric.load"
#define MUSICXX_CAPABILITY_LYRIC_LOAD_MUSICXX_SERVER "musicxx.lyric.load.musicxx_server"
#define MUSICXX_CAPABILITY_LYRIC_LOAD_CACHE "musicxx.lyric.load.cache"
#define MUSICXX_CAPABILITY_LYRIC_LOAD_BUILTIN "musicxx.lyric.load.builtin"
#define MUSICXX_CAPABILITY_LYRIC_LOAD_BUILTOUT "musicxx.lyric.load.builtout"
#define MUSICXX_CAPABILITY_LYRIC_LOAD_MATCH "musicxx.lyric.load.match"
#define MUSICXX_CAPABILITY_MEDIA_SPECTRUM_EXTRACT "musicxx.media.spectrum.extract"
#define MUSICXX_CAPABILITY_MEDIA_CHORUS_ANALYSE "musicxx.media.chorus.analyse"
#define MUSICXX_CAPABILITY_MEDIA_PALETTE_ANALYSE "musicxx.media.palette.analyse"
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
#define MUSICXX_CAPABILITY_PLAYER_MPV_GET "musicxx.player.mpv.get"
#define MUSICXX_CAPABILITY_PLAYER_MPV_SET "musicxx.player.mpv.set"
#define MUSICXX_CAPABILITY_PLAYER_MPV_COMMAND "musicxx.player.mpv.command"
#define MUSICXX_CAPABILITY_PLAYER_MPV_FILTER "musicxx.player.mpv.filter"
#define MUSICXX_CAPABILITY_SONGLIST_LIST "musicxx.songlist.list"
#define MUSICXX_CAPABILITY_SONGLIST_LIST_SONGS "musicxx.songlist.listSongs"
#define MUSICXX_CAPABILITY_SONGLIST_PLAY "musicxx.songlist.play"
#define MUSICXX_CAPABILITY_SONGLIST_ADD_SONG "musicxx.songlist.addSong"
#define MUSICXX_CAPABILITY_SONGLIST_REMOVE_SONG "musicxx.songlist.removeSong"
#define MUSICXX_CAPABILITY_SONGLIST_CREATE "musicxx.songlist.create"
#define MUSICXX_CAPABILITY_SONGLIST_UPDATE_SONG "musicxx.songlist.updateSong"
#define MUSICXX_CAPABILITY_SONGLIST_FAVORITE "musicxx.songlist.favorite"
#define MUSICXX_CAPABILITY_SONGLIST_SYNC "musicxx.songlist.sync"
#define MUSICXX_CAPABILITY_PLAYLIST_LIST_SONGS "musicxx.playlist.listSongs"
#define MUSICXX_CAPABILITY_PLAYLIST_PLAY_SONG "musicxx.playlist.playSong"
#define MUSICXX_CAPABILITY_PLAYLIST_ADD_SONG "musicxx.playlist.addSong"
#define MUSICXX_CAPABILITY_PLAYLIST_REMOVE_SONG "musicxx.playlist.removeSong"
#define MUSICXX_CAPABILITY_PLAYLIST_UPDATE_SONG "musicxx.playlist.updateSong"
#define MUSICXX_CAPABILITY_LYRIC_CURRENT_GET "musicxx.lyric.current.get"
#define MUSICXX_CAPABILITY_LYRIC_CURRENT_UPDATE "musicxx.lyric.current.update"
#define MUSICXX_CAPABILITY_LYRIC_CURRENT_UPDATE_CONTENT "musicxx.lyric.current.updateContent"
#define MUSICXX_CAPABILITY_LYRIC_GET_BY_SRC "musicxx.lyric.getBySrc"
#define MUSICXX_CAPABILITY_LYRIC_SEARCH "musicxx.lyric.search"
#define MUSICXX_CAPABILITY_UI_TOAST "musicxx.ui.toast"
#define MUSICXX_CAPABILITY_UI_DIALOG "musicxx.ui.dialog"
#define MUSICXX_CAPABILITY_UI_OVERLAY_SHOW "musicxx.ui.overlay.show"
#define MUSICXX_CAPABILITY_UI_OVERLAY_CLOSE "musicxx.ui.overlay.close"
#define MUSICXX_CAPABILITY_UI_OVERLAY_SHOW_SHEET "musicxx.ui.overlay.showSheet"
#define MUSICXX_CAPABILITY_UI_OPEN_ROUTE "musicxx.ui.openRoute"
#define MUSICXX_CAPABILITY_UI_PLAYING_OPEN_MENU "musicxx.ui.playing.openMenu"
#define MUSICXX_CAPABILITY_UI_DRIVE_PICK_SONGS "musicxx.ui.drive.pickSongs"
#define MUSICXX_CAPABILITY_UI_DRIVE_PICK_DIR "musicxx.ui.drive.pickDir"
#define MUSICXX_CAPABILITY_UI_DRIVE_BROWSE "musicxx.ui.drive.browse"
#define MUSICXX_CAPABILITY_UI_SET_FONT_FAMILY "musicxx.ui.setFontFamily"
#define MUSICXX_CAPABILITY_UI_GET_FONT_FAMILY "musicxx.ui.getFontFamily"
#define MUSICXX_CAPABILITY_UI_SET_ANIMATED_LEVEL "musicxx.ui.setAnimatedLevel"
#define MUSICXX_CAPABILITY_UI_SET_PARTICLE_ANIMATE "musicxx.ui.setParticleAnimate"
#define MUSICXX_CAPABILITY_UI_SET_SONG_ICON_ROTATE "musicxx.ui.setSongIconRotate"
#define MUSICXX_CAPABILITY_UI_SET_SONG_ICON_WAVE "musicxx.ui.setSongIconWave"
#define MUSICXX_CAPABILITY_THEME_SET_CURRENT "musicxx.theme.setCurrent"
#define MUSICXX_CAPABILITY_THEME_SET_MODE "musicxx.theme.setMode"
#define MUSICXX_CAPABILITY_THEME_SET_TEMP_MODE "musicxx.theme.setTempMode"
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
#define MUSICXX_CAPABILITY_PLAYER_AUDIO_EFFECT_LIST "musicxx.player.audioEffect.list"
#define MUSICXX_CAPABILITY_PLAYER_AUDIO_EFFECT_GET "musicxx.player.audioEffect.get"
#define MUSICXX_CAPABILITY_PLAYER_AUDIO_EFFECT_SET "musicxx.player.audioEffect.set"
#define MUSICXX_CAPABILITY_MEDIA_PALETTE_GET "musicxx.media.palette.get"
#define MUSICXX_CAPABILITY_MEDIA_COVER "musicxx.media.cover"
#define MUSICXX_CAPABILITY_MEDIA_COVER_SET "musicxx.media.cover.set"
#define MUSICXX_CAPABILITY_MEDIA_COVER_RESOLVE "musicxx.media.cover.resolve"
#define MUSICXX_CAPABILITY_MEDIA_PALETTE_SET "musicxx.media.palette.set"
#define MUSICXX_CAPABILITY_MEDIA_BIND_IMAGE "musicxx.media.bindImage"
#define MUSICXX_CAPABILITY_MEDIA_UNBIND_IMAGE "musicxx.media.unbindImage"
#define MUSICXX_CAPABILITY_MEDIA_SPECTRUM "musicxx.media.spectrum"
#define MUSICXX_CAPABILITY_MEDIA_KERNEL "musicxx.media.kernel"
#define MUSICXX_CAPABILITY_FEATURE_POINT_LIST "musicxx.feature.point.list"
#define MUSICXX_CAPABILITY_FEATURE_POINT_DEFINE "musicxx.feature.point.define"
#define MUSICXX_CAPABILITY_FEATURE_POINT_UNDEFINE "musicxx.feature.point.undefine"
#define MUSICXX_CAPABILITY_SOURCE_DEFINE "musicxx.source.define"
#define MUSICXX_CAPABILITY_SOURCE_UNDEFINE "musicxx.source.undefine"
#define MUSICXX_CAPABILITY_SOURCE_LIST "musicxx.source.list"
#define MUSICXX_CAPABILITY_SOURCE_BROWSE "musicxx.source.browse"
#define MUSICXX_CAPABILITY_CAPABILITY_SUBSCRIBERS "musicxx.capability.subscribers"
#define MUSICXX_CAPABILITY_HOST_OPEN_URL "musicxx.host.openUrl"
#define MUSICXX_CAPABILITY_HOST_CLIPBOARD "musicxx.host.clipboard"
#define MUSICXX_CAPABILITY_HOST_GET_PATH "musicxx.host.getPath"
#define MUSICXX_CAPABILITY_STATS_REPORT_MEMORY "musicxx.stats.reportMemory"
#define MUSICXX_CAPABILITY_STATS_REPORT_METRIC "musicxx.stats.reportMetric"

#define MUSICXX_BROADCAST_APP_STARTED "musicxx.app.started"
#define MUSICXX_BROADCAST_APP_READY "musicxx.app.ready"
#define MUSICXX_BROADCAST_APP_LIFECYCLE "musicxx.app.lifecycle"
#define MUSICXX_BROADCAST_PLAYER_SOURCE_RESOLVED "musicxx.player.source.resolved"
#define MUSICXX_BROADCAST_PLAYER_STATE_CHANGED "musicxx.player.stateChanged"
#define MUSICXX_BROADCAST_PLAYER_POSITION "musicxx.player.position"
#define MUSICXX_BROADCAST_PLAYER_COMPLETED "musicxx.player.completed"
#define MUSICXX_BROADCAST_SONG_CHANGED "musicxx.song.changed"
#define MUSICXX_BROADCAST_MEDIA_COVER_CHANGED "musicxx.media.cover.changed"
#define MUSICXX_BROADCAST_UI_ANIMATED_LEVEL "musicxx.ui.animatedLevel"
#define MUSICXX_BROADCAST_UI_PARTICLE_ANIMATE "musicxx.ui.particleAnimate"
#define MUSICXX_BROADCAST_UI_SONG_ICON_ROTATE "musicxx.ui.songIconRotate"
#define MUSICXX_BROADCAST_UI_SONG_ICON_WAVE "musicxx.ui.songIconWave"
#define MUSICXX_BROADCAST_UI_FONT_FAMILY "musicxx.ui.fontFamily"
#define MUSICXX_BROADCAST_THEME_AVAILABLE "musicxx.theme.available"
#define MUSICXX_BROADCAST_THEME_CURRENT "musicxx.theme.current"
#define MUSICXX_BROADCAST_THEME_MODE "musicxx.theme.mode"
#define MUSICXX_BROADCAST_STATE_APP "musicxx.state.app"
#define MUSICXX_BROADCAST_STATE_PLAYER "musicxx.state.player"
#define MUSICXX_BROADCAST_STATE_SONG "musicxx.state.song"
#define MUSICXX_BROADCAST_STATE_PLAYLIST "musicxx.state.playlist"
#define MUSICXX_BROADCAST_STATE_LYRIC "musicxx.state.lyric"
#define MUSICXX_BROADCAST_STATE_SONGLISTS "musicxx.state.songlists"
#define MUSICXX_BROADCAST_STATE_ENV "musicxx.state.env"
#define MUSICXX_BROADCAST_STATE_RENDER_SLOTS "musicxx.state.renderSlots"
#define MUSICXX_BROADCAST_STATE_SPECTRUM "musicxx.state.spectrum"

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
    uint32_t    channels;    ///< MUSICXX_CAPABILITY_CHANNEL_*
} MusicxxCapabilityDecl;

/// 一条广播的声明
typedef struct MusicxxBroadcastDecl {
    const char* id;
    int32_t     cache;             ///< MUSICXX_BROADCAST_CACHE_*
    int32_t     publish;           ///< MUSICXX_BROADCAST_PUBLISH_*
    int32_t     push_on_subscribe; ///< 订阅时立刻推当前值
    int32_t     coalesce;          ///< 允许合并（允许丢中间值）
    int32_t     scope;             ///< MUSICXX_BROADCAST_SCOPE_*
} MusicxxBroadcastDecl;

static const MusicxxCapabilityDecl musicxxCapabilityDecls[] = {
    {"musicxx.player.beforePlaySong", 1, 2, 5u, 1, 0, 0, 0u},
    {"musicxx.player.source.beforeParse", 1, 1, 5u, 1, 0, 0, 0u},
    {"musicxx.player.source.resolve", 1, 1, 1u, 1, 5000, 1, 0u},
    {"musicxx.player.seek", 0, 0, 6u, 1, 500, 0, 5u},
    {"musicxx.player.volume", 0, 0, 6u, 1, 500, 0, 0u},
    {"musicxx.player.speed", 0, 0, 6u, 1, 500, 0, 0u},
    {"musicxx.player.pitch", 0, 0, 6u, 1, 500, 0, 0u},
    {"musicxx.player.error", 1, 2, 5u, 1, 0, 0, 0u},
    {"musicxx.lyric.load", 1, 1, 7u, 1, 8000, 1, 0u},
    {"musicxx.lyric.load.musicxx_server", 1, 1, 7u, 1, 8000, 1, 0u},
    {"musicxx.lyric.load.cache", 1, 1, 7u, 1, 8000, 1, 0u},
    {"musicxx.lyric.load.builtin", 1, 1, 7u, 1, 8000, 1, 0u},
    {"musicxx.lyric.load.builtout", 1, 1, 7u, 1, 8000, 1, 0u},
    {"musicxx.lyric.load.match", 1, 1, 7u, 1, 8000, 1, 0u},
    {"musicxx.media.spectrum.extract", 1, 1, 1u, 1, 30000, 1, 2u},
    {"musicxx.media.chorus.analyse", 1, 1, 1u, 1, 20000, 1, 2u},
    {"musicxx.media.palette.analyse", 0, 0, 1u, 1, 800, 1, 2u},
    {"musicxx.feature.point.call", 1, 1, 7u, 1, 0, 0, 5u},
    {"musicxx.app.foreground", 0, 0, 1u, 1, 0, 0, 5u},
    {"musicxx.app.background", 0, 0, 1u, 1, 0, 0, 5u},
    {"musicxx.player.play", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.player.pause", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.player.toggle", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.player.stop", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.player.next", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.player.prev", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.player.playSrc", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.player.setVolume", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.player.setSpeed", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.player.setPitch", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.player.setQuality", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.player.setLoopMode", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.player.mpv.get", 0, 0, 1u, 1, 5000, 0, 7u},
    {"musicxx.player.mpv.set", 0, 0, 1u, 1, 5000, 0, 7u},
    {"musicxx.player.mpv.command", 0, 0, 1u, 1, 5000, 0, 7u},
    {"musicxx.player.mpv.filter", 0, 0, 1u, 1, 5000, 0, 7u},
    {"musicxx.songlist.list", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.songlist.listSongs", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.songlist.play", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.songlist.addSong", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.songlist.removeSong", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.songlist.create", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.songlist.updateSong", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.songlist.favorite", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.songlist.sync", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.playlist.listSongs", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.playlist.playSong", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.playlist.addSong", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.playlist.removeSong", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.playlist.updateSong", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.lyric.current.get", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.lyric.current.update", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.lyric.current.updateContent", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.lyric.getBySrc", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.lyric.search", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.ui.toast", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.ui.dialog", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.ui.overlay.show", 0, 0, 1u, 1, 5000, 0, 4u},
    {"musicxx.ui.overlay.close", 0, 0, 1u, 1, 5000, 0, 4u},
    {"musicxx.ui.overlay.showSheet", 0, 0, 1u, 1, 5000, 0, 4u},
    {"musicxx.ui.openRoute", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.ui.playing.openMenu", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.ui.drive.pickSongs", 0, 0, 1u, 1, 5000, 0, 4u},
    {"musicxx.ui.drive.pickDir", 0, 0, 1u, 1, 5000, 0, 4u},
    {"musicxx.ui.drive.browse", 0, 0, 1u, 1, 5000, 0, 4u},
    {"musicxx.ui.setFontFamily", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.ui.getFontFamily", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.ui.setAnimatedLevel", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.ui.setParticleAnimate", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.ui.setSongIconRotate", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.ui.setSongIconWave", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.theme.setCurrent", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.theme.setMode", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.theme.setTempMode", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.ui.loadFont", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.storage.get", 0, 0, 1u, 1, 5000, 0, 4u},
    {"musicxx.storage.set", 0, 0, 1u, 1, 5000, 0, 4u},
    {"musicxx.storage.delete", 0, 0, 1u, 1, 5000, 0, 4u},
    {"musicxx.storage.list", 0, 0, 1u, 1, 5000, 0, 4u},
    {"musicxx.net.fetch", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.net.download", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.render.list", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.render.current", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.render.select", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.player.audioEffect.list", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.player.audioEffect.get", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.player.audioEffect.set", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.media.palette.get", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.media.cover", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.media.cover.set", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.media.cover.resolve", 1, 1, 7u, 1, 3000, 0, 0u},
    {"musicxx.media.palette.set", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.media.bindImage", 0, 0, 1u, 1, 5000, 0, 4u},
    {"musicxx.media.unbindImage", 0, 0, 1u, 1, 5000, 0, 4u},
    {"musicxx.media.spectrum", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.media.kernel", 0, 0, 1u, 1, 5000, 0, 7u},
    {"musicxx.feature.point.list", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.feature.point.define", 0, 0, 1u, 1, 5000, 0, 4u},
    {"musicxx.feature.point.undefine", 0, 0, 1u, 1, 5000, 0, 4u},
    {"musicxx.source.define", 0, 0, 1u, 1, 5000, 0, 4u},
    {"musicxx.source.undefine", 0, 0, 1u, 1, 5000, 0, 4u},
    {"musicxx.source.list", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.source.browse", 0, 0, 1u, 1, 5000, 0, 4u},
    {"musicxx.capability.subscribers", 0, 0, 1u, 1, 5000, 0, 4u},
    {"musicxx.host.openUrl", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.host.clipboard", 0, 0, 1u, 1, 5000, 0, 5u},
    {"musicxx.host.getPath", 0, 0, 1u, 1, 5000, 0, 4u},
    {"musicxx.stats.reportMemory", 0, 0, 1u, 1, 5000, 0, 4u},
    {"musicxx.stats.reportMetric", 0, 0, 1u, 1, 5000, 0, 4u},
};

/// 已知能力条目数
static const size_t musicxxCapabilityDeclCount = sizeof(musicxxCapabilityDecls) / sizeof(musicxxCapabilityDecls[0]);

static const MusicxxBroadcastDecl musicxxBroadcastDecls[] = {
    {"musicxx.app.started", 0, 0, 0, 1, 0},
    {"musicxx.app.ready", 0, 0, 0, 1, 0},
    {"musicxx.app.lifecycle", 1, 0, 1, 0, 0},
    {"musicxx.player.source.resolved", 0, 0, 0, 1, 0},
    {"musicxx.player.stateChanged", 0, 0, 0, 1, 0},
    {"musicxx.player.position", 1, 0, 1, 0, 0},
    {"musicxx.player.completed", 0, 0, 0, 1, 0},
    {"musicxx.song.changed", 0, 0, 0, 1, 0},
    {"musicxx.media.cover.changed", 0, 0, 0, 1, 0},
    {"musicxx.ui.animatedLevel", 1, 0, 1, 0, 0},
    {"musicxx.ui.particleAnimate", 1, 0, 1, 0, 0},
    {"musicxx.ui.songIconRotate", 1, 0, 1, 0, 0},
    {"musicxx.ui.songIconWave", 1, 0, 1, 0, 0},
    {"musicxx.ui.fontFamily", 1, 0, 1, 0, 0},
    {"musicxx.theme.available", 1, 0, 1, 0, 0},
    {"musicxx.theme.current", 1, 0, 1, 0, 0},
    {"musicxx.theme.mode", 1, 0, 1, 0, 0},
    {"musicxx.state.app", 1, 0, 1, 1, 0},
    {"musicxx.state.player", 1, 0, 1, 1, 0},
    {"musicxx.state.song", 1, 0, 1, 1, 0},
    {"musicxx.state.playlist", 1, 0, 1, 1, 0},
    {"musicxx.state.lyric", 1, 0, 1, 1, 0},
    {"musicxx.state.songlists", 1, 0, 1, 1, 0},
    {"musicxx.state.env", 1, 0, 1, 1, 0},
    {"musicxx.state.renderSlots", 1, 0, 1, 1, 0},
    {"musicxx.state.spectrum", 1, 0, 1, 1, 0},
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
