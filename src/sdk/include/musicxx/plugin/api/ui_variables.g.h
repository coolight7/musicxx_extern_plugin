/* 本文件由 tools/gen_contract.dart 生成, 请勿手工修改。
 * 定义来源: tools/ui_variables.def.json
 * 界面 JSON 里可以绑定的变量 id (deps 校验用)。 */
#ifndef MUSICXX_PLUGIN_UI_VARIABLES_G_H
#define MUSICXX_PLUGIN_UI_VARIABLES_G_H

#ifdef __cplusplus
namespace musicxx {
namespace extern_plugin {

/// 一个已声明的变量
struct KnownUiVariable {
  const char *id;
  bool scope;
};

inline constexpr KnownUiVariable kKnownUiVariables[] = {
    {"musicxx.app.foreground", false},
    {"musicxx.app.lifecycle", false},
    {"musicxx.app.version", false},
    {"musicxx.app.lang", false},
    {"musicxx.app.platform", false},
    {"musicxx.theme.night", false},
    {"musicxx.theme.mode", false},
    {"musicxx.theme.current", false},
    {"musicxx.ui.animatedLevel", false},
    {"musicxx.ui.songIconRotate", false},
    {"musicxx.ui.songIconWave", false},
    {"musicxx.ui.fontFamily", false},
    {"musicxx.player.state", false},
    {"musicxx.player.playing", false},
    {"musicxx.player.positionMs", false},
    {"musicxx.player.durationMs", false},
    {"musicxx.player.volume", false},
    {"musicxx.player.speed", false},
    {"musicxx.player.pitch", false},
    {"musicxx.song.hasSong", false},
    {"musicxx.song.sid", false},
    {"musicxx.song.name", false},
    {"musicxx.song.artist", false},
    {"musicxx.song.album", false},
    {"musicxx.song.subtitle", false},
    {"musicxx.song.durationMs", false},
    {"musicxx.song.srcKey", false},
    {"musicxx.song.favorite", false},
    {"musicxx.song.hasCover", false},
    {"musicxx.playlist.length", false},
    {"musicxx.playlist.index", false},
    {"musicxx.playlist.loopMode", false},
    {"musicxx.playlist.autoPlayMode", false},
    {"musicxx.lyric.hasLyric", false},
    {"musicxx.lyric.offsetMs", false},
    {"musicxx.lyric.line", false},
    {"musicxx.env.windowWidth", false},
    {"musicxx.env.windowHeight", false},
    {"musicxx.env.dpi", false},
    {"musicxx.env.isDesktop", false},
    {"musicxx.env.isMobile", false},
    {"musicxx.spectrum.available", false},
    {"musicxx.spectrum.level", false},
    {"scope:musicxx.page.route", true},
    {"scope:musicxx.page.args", true},
    {"scope:musicxx.overlay.id", true},
    {"scope:musicxx.overlay.args", true},
    {"scope:musicxx.songlist.pid", true},
    {"scope:musicxx.songlist.name", true},
    {"scope:musicxx.songlist.songNum", true},
    {"scope:musicxx.songlist.sort", true},
    {"scope:musicxx.songlist.isLocal", true},
    {"scope:musicxx.songlist.cover", true},
    {"scope:musicxx.songlist.loading", true},
    {"scope:musicxx.slot.colorMain", true},
    {"scope:musicxx.slot.colorCross", true},
    {"scope:musicxx.slot.colorBlock", true},
    {"scope:musicxx.slot.args", true},
};

} // namespace extern_plugin
} // namespace musicxx
#endif

#endif /* MUSICXX_PLUGIN_UI_VARIABLES_G_H */
