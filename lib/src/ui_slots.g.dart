// 本文件由 tools/gen_contract.dart 生成，请勿手工修改。
// 定义来源：tools/ui_slots.def.json
// 槽位（渲染位置）与声明项：id、kind、排序位置与说明。

/// 槽位的使用方式（主程序据它决定怎么用内容，插件据它知道该注册什么）
enum MusicxxUiSlotKind {
  /// 声明项：出现在主程序自己的列表里，不含界面树
  item,

  /// 插入：一个位置可以有多份内容，按 order 排
  insert,

  /// 替换：只认一份内容，没有内容时画内置界面
  replace,

  /// 可选样式：候选声明 + 用户选择
  selectable,

  /// 普通页面背景：按路由自动生效
  pageBackground,

  /// 插件页面：插件自己发起
  page,

  /// 插件浮层：插件自己发起
  overlay,
}

/// 一个槽位 / 声明项的元信息
class MusicxxUiSlotSpec_c {
  const MusicxxUiSlotSpec_c({
    required this.id,
    required this.kind,
    required this.label,
    required this.page,
    required this.position,
    required this.inList,
    required this.doc,
  });

  /// 全名（`musicxx.ui.*`）
  final String id;
  final MusicxxUiSlotKind kind;

  /// 中文叫法（错误提示与文档用）
  final String label;

  /// 所在页面
  final String page;

  /// 页面内的位置
  final String position;

  /// 内容是否画在列表里（在列表里时页面已经给过边距）
  final bool inList;

  /// 一句话说明
  final String doc;

  /// 简称（去掉 `musicxx.ui.` 前缀）
  String get shortName => id.startsWith(kMusicxxUiSlotPrefix)
      ? id.substring(kMusicxxUiSlotPrefix.length)
      : id;

  @override
  String toString() => 'MusicxxUiSlotSpec_c($id)';
}

/// 槽位 id 的统一前缀
const String kMusicxxUiSlotPrefix = 'musicxx.ui.';

/// 主程序预留的全部槽位与声明项（顺序与定义文件一致）
const List<MusicxxUiSlotSpec_c> kMusicxxUiSlots = <MusicxxUiSlotSpec_c>[
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.home.entry',
    kind: MusicxxUiSlotKind.item,
    label: '主页入口',
    page: '功能主页',
    position: '入口列表',
    inList: true,
    doc: '功能主页上的一个入口。data = {title, subtitle?, icon?, action?}；点它时派发 action。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.song.action',
    kind: MusicxxUiSlotKind.item,
    label: '歌曲菜单项',
    page: '各处歌曲菜单',
    position: '菜单项',
    inList: true,
    doc: '歌曲菜单里的一项。data = {title, icon?, action?}；点它时派发 action。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.playlist.action',
    kind: MusicxxUiSlotKind.item,
    label: '歌单菜单项',
    page: '歌单菜单',
    position: '菜单项',
    inList: true,
    doc: '歌单菜单里的一项。data = {title, icon?, action?}；点它时派发 action，参数里带当前歌单。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.addsong.entry',
    kind: MusicxxUiSlotKind.item,
    label: '添加歌曲来源项',
    page: '「添加歌曲」弹窗',
    position: '来源列表',
    inList: true,
    doc:
        '「添加歌曲」弹窗里的一个来源。data = {title, subtitle?, icon?, action?}；点击时参数里带当前歌单（pid / name / songNum）。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.audioEffect',
    kind: MusicxxUiSlotKind.item,
    label: '音效',
    page: '音效页面',
    position: '音效列表',
    inList: true,
    doc:
        '一段音频滤镜。data = {title, subtitle?, filter, pitch?, cpuCost?}；pitch 夹在 0.25~2.0，cpuCost 夹在 1~5（取整）。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.playing.background',
    kind: MusicxxUiSlotKind.selectable,
    label: '播放页背景',
    page: '播放页',
    position: '整页背景（用户选择生效）',
    inList: true,
    doc: '候选样式，靠 content 给出界面。声明项 data = {title, depict?}；界面内容注册在同一个槽位上。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.playing.icon',
    kind: MusicxxUiSlotKind.selectable,
    label: '播放页歌曲图',
    page: '播放页',
    position: '歌曲图 / 视频（用户选择生效）',
    inList: true,
    doc: '候选样式，靠 content 给出界面。声明项 data = {title, depict?}；界面内容注册在同一个槽位上。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.playing.visualizer',
    kind: MusicxxUiSlotKind.selectable,
    label: '播放页音频动效',
    page: '播放页',
    position: '动效区（用户选择生效）',
    inList: true,
    doc: '候选样式，靠 content 给出界面。声明项 data = {title, depict?}；界面内容注册在同一个槽位上。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.page.background',
    kind: MusicxxUiSlotKind.pageBackground,
    label: '普通页面背景',
    page: '所有普通页面',
    position: '整页背景（按路由自动生效）',
    inList: true,
    doc: '声明项 data = {title, depict?, routes?}；界面内容注册在同一个槽位上，按 routes 与注册顺序生效。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.playing.controlBar',
    kind: MusicxxUiSlotKind.replace,
    label: '播放页控制栏',
    page: '播放页',
    position: '整栏（替换内置界面）',
    inList: false,
    doc: '整栏由插件画。同一个槽位只认 order 最小的一份内容，没有内容时画内置控制栏。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.playing.controlBar.menu',
    kind: MusicxxUiSlotKind.replace,
    label: '播放页控制栏 · 菜单按钮',
    page: '播放页',
    position: '控制栏「菜单」按钮（替换显示）',
    inList: false,
    doc: '只替换这一个按钮的显示；内置按钮的行为是打开「更多」菜单（musicxx.ui.playing.openMenu）。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.playing.controlBar.previous',
    kind: MusicxxUiSlotKind.replace,
    label: '播放页控制栏 · 上一曲按钮',
    page: '播放页',
    position: '控制栏「上一曲」按钮（替换显示）',
    inList: false,
    doc: '只替换这一个按钮的显示；对应能力 musicxx.player.prev。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.playing.controlBar.playOrPause',
    kind: MusicxxUiSlotKind.replace,
    label: '播放页控制栏 · 播放暂停按钮',
    page: '播放页',
    position: '控制栏「播放 / 暂停」按钮（替换显示）',
    inList: false,
    doc:
        '只替换这一个按钮的显示；对应能力 musicxx.player.toggle；画面要跟播放状态走就用变量 musicxx.player.playing。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.playing.controlBar.next',
    kind: MusicxxUiSlotKind.replace,
    label: '播放页控制栏 · 下一曲按钮',
    page: '播放页',
    position: '控制栏「下一曲」按钮（替换显示）',
    inList: false,
    doc: '只替换这一个按钮的显示；对应能力 musicxx.player.next。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.playing.controlBar.favorite',
    kind: MusicxxUiSlotKind.replace,
    label: '播放页控制栏 · 收藏按钮',
    page: '播放页',
    position: '控制栏「收藏」按钮（替换显示）',
    inList: false,
    doc:
        '只替换这一个按钮的显示；对应能力 musicxx.songlist.favorite；收藏状态用变量 musicxx.song.favorite。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.playing.lyric.top',
    kind: MusicxxUiSlotKind.insert,
    label: '歌词页顶部',
    page: '播放页 · 歌词页',
    position: '列表之上（不随内容滚动）',
    inList: false,
    doc: '同一位置可以有多份内容，按 order 从上到下排列。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.playing.lyric.bottom',
    kind: MusicxxUiSlotKind.insert,
    label: '歌词页底部',
    page: '播放页 · 歌词页',
    position: '列表之下（不随内容滚动）',
    inList: false,
    doc: '同一位置可以有多份内容，按 order 从上到下排列。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.playing.lyric.header',
    kind: MusicxxUiSlotKind.insert,
    label: '歌词列表内顶部',
    page: '播放页 · 歌词页',
    position: '列表的 header（跟随内容滚动）',
    inList: true,
    doc: '同一位置可以有多份内容，按 order 从上到下排列。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.playing.lyric.footer',
    kind: MusicxxUiSlotKind.insert,
    label: '歌词列表内底部',
    page: '播放页 · 歌词页',
    position: '列表的 footer（跟随内容滚动）',
    inList: true,
    doc: '同一位置可以有多份内容，按 order 从上到下排列。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.playing.moreInfo.header',
    kind: MusicxxUiSlotKind.insert,
    label: '更多信息页内容开头',
    page: '播放页 · 更多信息页',
    position: '整页滚动内容的最前面',
    inList: true,
    doc: '同一位置可以有多份内容，按 order 从上到下排列。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.playing.moreInfo.footer',
    kind: MusicxxUiSlotKind.insert,
    label: '更多信息页内容末尾',
    page: '播放页 · 更多信息页',
    position: '整页滚动内容的最后面',
    inList: true,
    doc: '同一位置可以有多份内容，按 order 从上到下排列。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.playing.songlist.top',
    kind: MusicxxUiSlotKind.insert,
    label: '播放列表页顶部',
    page: '播放页 · 播放列表页',
    position: '列表之上（不随内容滚动）',
    inList: false,
    doc: '同一位置可以有多份内容，按 order 从上到下排列。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.playing.songlist.bottom',
    kind: MusicxxUiSlotKind.insert,
    label: '播放列表页底部',
    page: '播放页 · 播放列表页',
    position: '列表之下（不随内容滚动）',
    inList: false,
    doc: '同一位置可以有多份内容，按 order 从上到下排列。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.playing.songlist.header',
    kind: MusicxxUiSlotKind.insert,
    label: '播放列表内顶部',
    page: '播放页 · 播放列表页',
    position: '列表的 header（跟随内容滚动）',
    inList: true,
    doc: '同一位置可以有多份内容，按 order 从上到下排列。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.playing.songlist.footer',
    kind: MusicxxUiSlotKind.insert,
    label: '播放列表内底部',
    page: '播放页 · 播放列表页',
    position: '列表的 footer（跟随内容滚动）',
    inList: true,
    doc: '同一位置可以有多份内容，按 order 从上到下排列。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.page',
    kind: MusicxxUiSlotKind.page,
    label: '插件页面',
    page: '插件页面 ext://<插件id>/<内容 id>',
    position: '整页（插件自己发起）',
    inList: false,
    doc: '内容 id 就是页面地址里的那一段；作用域变量 scope:musicxx.page.*。',
  ),
  MusicxxUiSlotSpec_c(
    id: 'musicxx.ui.overlay',
    kind: MusicxxUiSlotKind.overlay,
    label: '插件浮层',
    page: '浮层（musicxx.ui.overlay.show / showSheet）',
    position: '整层（插件自己发起）',
    inList: false,
    doc: '内容 id 就是能力参数里的 id；作用域变量 scope:musicxx.overlay.*。',
  ),
];

/// 槽位 id → 元信息
final Map<String, MusicxxUiSlotSpec_c> kMusicxxUiSlotIndex =
    <String, MusicxxUiSlotSpec_c>{
      for (final MusicxxUiSlotSpec_c slot in kMusicxxUiSlots) slot.id: slot,
    };

/// 简称 → 元信息
final Map<String, MusicxxUiSlotSpec_c> kMusicxxUiSlotShortIndex =
    <String, MusicxxUiSlotSpec_c>{
      for (final MusicxxUiSlotSpec_c slot in kMusicxxUiSlots)
        slot.shortName: slot,
    };

/// 主程序预留的槽位常量（插件与主程序都写这些名字，不要手打字符串）
abstract final class MusicxxUiSlots_c {
  /// 主页入口
  static const String homeEntry = 'musicxx.ui.home.entry';

  /// 歌曲菜单项
  static const String songAction = 'musicxx.ui.song.action';

  /// 歌单菜单项
  static const String playlistAction = 'musicxx.ui.playlist.action';

  /// 添加歌曲来源项
  static const String addsongEntry = 'musicxx.ui.addsong.entry';

  /// 音效
  static const String audioEffect = 'musicxx.ui.audioEffect';

  /// 播放页背景
  static const String playingBackground = 'musicxx.ui.playing.background';

  /// 播放页歌曲图
  static const String playingIcon = 'musicxx.ui.playing.icon';

  /// 播放页音频动效
  static const String playingVisualizer = 'musicxx.ui.playing.visualizer';

  /// 普通页面背景
  static const String pageBackground = 'musicxx.ui.page.background';

  /// 播放页控制栏
  static const String playingControlBar = 'musicxx.ui.playing.controlBar';

  /// 播放页控制栏 · 菜单按钮
  static const String playingControlBarMenu =
      'musicxx.ui.playing.controlBar.menu';

  /// 播放页控制栏 · 上一曲按钮
  static const String playingControlBarPrevious =
      'musicxx.ui.playing.controlBar.previous';

  /// 播放页控制栏 · 播放暂停按钮
  static const String playingControlBarPlayOrPause =
      'musicxx.ui.playing.controlBar.playOrPause';

  /// 播放页控制栏 · 下一曲按钮
  static const String playingControlBarNext =
      'musicxx.ui.playing.controlBar.next';

  /// 播放页控制栏 · 收藏按钮
  static const String playingControlBarFavorite =
      'musicxx.ui.playing.controlBar.favorite';

  /// 歌词页顶部
  static const String playingLyricTop = 'musicxx.ui.playing.lyric.top';

  /// 歌词页底部
  static const String playingLyricBottom = 'musicxx.ui.playing.lyric.bottom';

  /// 歌词列表内顶部
  static const String playingLyricHeader = 'musicxx.ui.playing.lyric.header';

  /// 歌词列表内底部
  static const String playingLyricFooter = 'musicxx.ui.playing.lyric.footer';

  /// 更多信息页内容开头
  static const String playingMoreInfoHeader =
      'musicxx.ui.playing.moreInfo.header';

  /// 更多信息页内容末尾
  static const String playingMoreInfoFooter =
      'musicxx.ui.playing.moreInfo.footer';

  /// 播放列表页顶部
  static const String playingSonglistTop = 'musicxx.ui.playing.songlist.top';

  /// 播放列表页底部
  static const String playingSonglistBottom =
      'musicxx.ui.playing.songlist.bottom';

  /// 播放列表内顶部
  static const String playingSonglistHeader =
      'musicxx.ui.playing.songlist.header';

  /// 播放列表内底部
  static const String playingSonglistFooter =
      'musicxx.ui.playing.songlist.footer';

  /// 插件页面
  static const String page = 'musicxx.ui.page';

  /// 插件浮层
  static const String overlay = 'musicxx.ui.overlay';
}

/// 把简称补成全名（已经是全名、或不是主程序的槽位时原样返回）
String musicxxUiSlotFullName(String id) {
  if (id.isEmpty || id.startsWith(kMusicxxUiSlotPrefix)) {
    return id;
  }
  if (kMusicxxUiSlotShortIndex.containsKey(id)) {
    return kMusicxxUiSlotPrefix + id;
  }
  return id;
}

/// 查一个槽位（接受简称或全名）；不是主程序的槽位返回 null
MusicxxUiSlotSpec_c? musicxxUiSlotOf(String id) {
  final String full = musicxxUiSlotFullName(id);
  return kMusicxxUiSlotIndex[full];
}
