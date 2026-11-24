// 本文件由 tools/gen_contract.dart 生成，请勿手工修改。
// 定义来源：tools/ui_variables.def.json
// 界面 JSON 里可以绑定 / 插值的变量（主程序的与槽位作用域内的）。

/// 变量的取值类型（只用于文档与自省，运行期按实际值处理）
enum MusicxxUiVariableType { string, number, bool, json }

/// 一个变量的元信息
class MusicxxUiVariableSpec_c {
  const MusicxxUiVariableSpec_c({
    required this.id,
    required this.type,
    required this.depict,
    this.slots = const <String>[],
  });

  /// 变量 id（`musicxx.*` 或 `scope:musicxx.*`）
  final String id;
  final MusicxxUiVariableType type;
  /// 中文说明
  final String depict;
  /// 作用域变量只在这些槽位可用；空 = 任何槽位
  final List<String> slots;

  bool get isScope => id.startsWith(kMusicxxUiVariableScopePrefix);

  @override
  String toString() => 'MusicxxUiVariableSpec_c($id)';
}

/// 作用域变量的前缀
const String kMusicxxUiVariableScopePrefix = 'scope:';

/// 全部变量（顺序与定义文件一致）
const List<MusicxxUiVariableSpec_c> kMusicxxUiVariables = <MusicxxUiVariableSpec_c>[
  MusicxxUiVariableSpec_c(
    id: 'musicxx.app.foreground',
    type: MusicxxUiVariableType.bool,
    depict: '应用是否在前台',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.app.lifecycle',
    type: MusicxxUiVariableType.string,
    depict: '应用生命周期状态（ground / background 等）',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.app.version',
    type: MusicxxUiVariableType.string,
    depict: '应用版本号',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.app.lang',
    type: MusicxxUiVariableType.string,
    depict: '界面语言（zh_Hans / en_US …）',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.app.platform',
    type: MusicxxUiVariableType.string,
    depict: '运行平台（windows / android / ios / linux / macos）',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.theme.night',
    type: MusicxxUiVariableType.bool,
    depict: '是否夜间主题',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.theme.mode',
    type: MusicxxUiVariableType.string,
    depict: '主题模式（跟随系统 / 亮 / 暗）',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.theme.current',
    type: MusicxxUiVariableType.string,
    depict: '当前主题名',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.ui.animatedLevel',
    type: MusicxxUiVariableType.string,
    depict: '界面动画等级',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.ui.songIconRotate',
    type: MusicxxUiVariableType.bool,
    depict: '歌曲图是否旋转',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.ui.songIconWave',
    type: MusicxxUiVariableType.bool,
    depict: '歌曲图是否随音频起伏',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.ui.fontFamily',
    type: MusicxxUiVariableType.string,
    depict: '界面字体',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.player.state',
    type: MusicxxUiVariableType.string,
    depict: '播放状态（idle / playing / paused / completed …）',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.player.playing',
    type: MusicxxUiVariableType.bool,
    depict: '现在是否在播放',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.player.positionMs',
    type: MusicxxUiVariableType.number,
    depict: '当前播放位置（毫秒）',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.player.durationMs',
    type: MusicxxUiVariableType.number,
    depict: '当前歌曲总时长（毫秒）',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.player.volume',
    type: MusicxxUiVariableType.number,
    depict: '音量（0~100）',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.player.speed',
    type: MusicxxUiVariableType.number,
    depict: '播放速度',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.player.pitch',
    type: MusicxxUiVariableType.number,
    depict: '音调',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.song.hasSong',
    type: MusicxxUiVariableType.bool,
    depict: '有没有当前歌曲',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.song.sid',
    type: MusicxxUiVariableType.number,
    depict: '当前歌曲的会话 id（每次切歌都会变）',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.song.name',
    type: MusicxxUiVariableType.string,
    depict: '当前歌曲名',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.song.artist',
    type: MusicxxUiVariableType.string,
    depict: '当前歌曲歌手',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.song.album',
    type: MusicxxUiVariableType.string,
    depict: '当前歌曲专辑',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.song.subtitle',
    type: MusicxxUiVariableType.string,
    depict: '当前歌曲副标题',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.song.durationMs',
    type: MusicxxUiVariableType.number,
    depict: '当前歌曲时长（毫秒）',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.song.srcKey',
    type: MusicxxUiVariableType.string,
    depict: '当前歌曲的音源身份',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.song.favorite',
    type: MusicxxUiVariableType.bool,
    depict: '当前歌曲是否已收藏',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.song.hasCover',
    type: MusicxxUiVariableType.bool,
    depict: '当前歌曲有没有封面',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.playlist.length',
    type: MusicxxUiVariableType.number,
    depict: '当前播放队列的长度',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.playlist.index',
    type: MusicxxUiVariableType.number,
    depict: '当前歌曲在播放队列里的位置（从 0 起）',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.playlist.loopMode',
    type: MusicxxUiVariableType.string,
    depict: '循环模式',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.playlist.autoPlayMode',
    type: MusicxxUiVariableType.string,
    depict: '自动播放策略',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.lyric.hasLyric',
    type: MusicxxUiVariableType.bool,
    depict: '当前歌曲有没有歌词',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.lyric.offsetMs',
    type: MusicxxUiVariableType.number,
    depict: '歌词偏移（毫秒）',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.lyric.line',
    type: MusicxxUiVariableType.number,
    depict: '当前高亮的歌词行下标（没有歌词时为 -1）',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.env.windowWidth',
    type: MusicxxUiVariableType.number,
    depict: '窗口逻辑宽度',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.env.windowHeight',
    type: MusicxxUiVariableType.number,
    depict: '窗口逻辑高度',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.env.dpi',
    type: MusicxxUiVariableType.number,
    depict: '窗口所在屏幕的像素比',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.env.isDesktop',
    type: MusicxxUiVariableType.bool,
    depict: '是不是桌面平台',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.env.isMobile',
    type: MusicxxUiVariableType.bool,
    depict: '是不是移动平台',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.spectrum.available',
    type: MusicxxUiVariableType.bool,
    depict: '现在有没有频谱数据',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'musicxx.spectrum.level',
    type: MusicxxUiVariableType.number,
    depict: '频谱响度（0~1）',
    slots: <String>[],
  ),
  MusicxxUiVariableSpec_c(
    id: 'scope:musicxx.page.route',
    type: MusicxxUiVariableType.string,
    depict: '当前插件页面的地址（ext://<插件id>/<内容 id>）',
    slots: <String>[
      'musicxx.ui.page',
      'musicxx.ui.page.background',
    ],
  ),
  MusicxxUiVariableSpec_c(
    id: 'scope:musicxx.page.args',
    type: MusicxxUiVariableType.json,
    depict: '打开插件页面时带的参数',
    slots: <String>[
      'musicxx.ui.page',
    ],
  ),
  MusicxxUiVariableSpec_c(
    id: 'scope:musicxx.overlay.id',
    type: MusicxxUiVariableType.string,
    depict: '浮层的内容 id',
    slots: <String>[
      'musicxx.ui.overlay',
    ],
  ),
  MusicxxUiVariableSpec_c(
    id: 'scope:musicxx.overlay.args',
    type: MusicxxUiVariableType.json,
    depict: '打开浮层时带的参数',
    slots: <String>[
      'musicxx.ui.overlay',
    ],
  ),
  MusicxxUiVariableSpec_c(
    id: 'scope:musicxx.search.keyword',
    type: MusicxxUiVariableType.string,
    depict: '搜索页里已提交的搜索词（没搜过是空串；用户在输入框打字不会改它）',
    slots: <String>[
      'musicxx.ui.search.type',
    ],
  ),
  MusicxxUiVariableSpec_c(
    id: 'scope:musicxx.search.request',
    type: MusicxxUiVariableType.number,
    depict: '搜索请求序号，每次点搜索 +1（同一个搜索词再点一次也会变）',
    slots: <String>[
      'musicxx.ui.search.type',
    ],
  ),
  MusicxxUiVariableSpec_c(
    id: 'scope:musicxx.search.type',
    type: MusicxxUiVariableType.string,
    depict: '当前搜索类型的内容短 id（一个插件注册多个搜索类型时用来分辨）',
    slots: <String>[
      'musicxx.ui.search.type',
    ],
  ),
  MusicxxUiVariableSpec_c(
    id: 'scope:musicxx.songlist.pid',
    type: MusicxxUiVariableType.string,
    depict: '歌单内容页打开的歌单 id',
    slots: <String>[
      'musicxx.ui.page.background',
    ],
  ),
  MusicxxUiVariableSpec_c(
    id: 'scope:musicxx.songlist.name',
    type: MusicxxUiVariableType.string,
    depict: '歌单内容页打开的歌单名',
    slots: <String>[
      'musicxx.ui.page.background',
    ],
  ),
  MusicxxUiVariableSpec_c(
    id: 'scope:musicxx.songlist.songNum',
    type: MusicxxUiVariableType.number,
    depict: '歌单的歌曲数',
    slots: <String>[
      'musicxx.ui.page.background',
    ],
  ),
  MusicxxUiVariableSpec_c(
    id: 'scope:musicxx.songlist.sort',
    type: MusicxxUiVariableType.string,
    depict: '歌单的排序方式',
    slots: <String>[
      'musicxx.ui.page.background',
    ],
  ),
  MusicxxUiVariableSpec_c(
    id: 'scope:musicxx.songlist.isLocal',
    type: MusicxxUiVariableType.bool,
    depict: '歌单是不是本地歌单',
    slots: <String>[
      'musicxx.ui.page.background',
    ],
  ),
  MusicxxUiVariableSpec_c(
    id: 'scope:musicxx.songlist.cover',
    type: MusicxxUiVariableType.string,
    depict: '歌单封面路径（没有封面时为空串）',
    slots: <String>[
      'musicxx.ui.page.background',
    ],
  ),
  MusicxxUiVariableSpec_c(
    id: 'scope:musicxx.songlist.loading',
    type: MusicxxUiVariableType.bool,
    depict: '歌单是否还在加载',
    slots: <String>[
      'musicxx.ui.page.background',
    ],
  ),
  MusicxxUiVariableSpec_c(
    id: 'scope:musicxx.slot.colorMain',
    type: MusicxxUiVariableType.string,
    depict: '播放页前景普通色（#aarrggbb）',
    slots: <String>[
      'musicxx.ui.playing.controlBar',
      'musicxx.ui.playing.controlBar.menu',
      'musicxx.ui.playing.controlBar.previous',
      'musicxx.ui.playing.controlBar.playOrPause',
      'musicxx.ui.playing.controlBar.next',
      'musicxx.ui.playing.controlBar.favorite',
    ],
  ),
  MusicxxUiVariableSpec_c(
    id: 'scope:musicxx.slot.colorCross',
    type: MusicxxUiVariableType.string,
    depict: '播放页前景减淡色（#aarrggbb）',
    slots: <String>[
      'musicxx.ui.playing.controlBar',
      'musicxx.ui.playing.controlBar.menu',
      'musicxx.ui.playing.controlBar.previous',
      'musicxx.ui.playing.controlBar.playOrPause',
      'musicxx.ui.playing.controlBar.next',
      'musicxx.ui.playing.controlBar.favorite',
    ],
  ),
  MusicxxUiVariableSpec_c(
    id: 'scope:musicxx.slot.colorBlock',
    type: MusicxxUiVariableType.string,
    depict: '播放页前景的块底色（#aarrggbb）',
    slots: <String>[
      'musicxx.ui.playing.controlBar',
      'musicxx.ui.playing.controlBar.menu',
      'musicxx.ui.playing.controlBar.previous',
      'musicxx.ui.playing.controlBar.playOrPause',
      'musicxx.ui.playing.controlBar.next',
      'musicxx.ui.playing.controlBar.favorite',
    ],
  ),
  MusicxxUiVariableSpec_c(
    id: 'scope:musicxx.slot.args',
    type: MusicxxUiVariableType.json,
    depict: '外层内容通过 UISlot.args 传给这个嵌套槽位的参数',
    slots: <String>[],
  ),
];

/// 变量 id → 元信息
final Map<String, MusicxxUiVariableSpec_c> kMusicxxUiVariableIndex =
    <String, MusicxxUiVariableSpec_c>{
  for (final MusicxxUiVariableSpec_c item in kMusicxxUiVariables) item.id: item,
};

/// 查一个变量；没有声明过返回 null（插件自定义变量 `plugin.*` 不属于这里）
MusicxxUiVariableSpec_c? musicxxUiVariableOf(String id) => kMusicxxUiVariableIndex[id];

/// 这个变量能不能在这个槽位上用
///
/// 规则：全局变量任何槽位都能用；作用域变量只在自己的槽位（以及嵌在它们里面的
/// 嵌套槽位）能用。找不到声明时按 `false` 处理。
bool musicxxUiVariableAllowedOn(String id, String slot, {bool nested = false}) {
  final MusicxxUiVariableSpec_c? spec = kMusicxxUiVariableIndex[id];
  if (null == spec) {
    return false;
  }
  if (!spec.isScope || spec.slots.isEmpty) {
    return true;
  }
  if (nested) {
    // 嵌套槽位继承外层作用域：外层的槽位变量在里层同样可读
    return true;
  }
  return spec.slots.contains(slot);
}
