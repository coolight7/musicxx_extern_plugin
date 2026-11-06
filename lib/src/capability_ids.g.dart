// 自动生成（tools/gen_contract.dart ← tools/capabilities.def.json）—— 请勿手改。
//
// 能力 = 调用方发一次请求、等一个结果；广播 = 属主改了东西、通知关心的人。
// id 是跨边界稳定的约定：字符串值一旦发布不得修改，只能新增或标记废弃。
// ignore_for_file: type=lint, constant_identifier_names

/// 能力 id（调用方发一次请求、等一个结果）
abstract final class MusicxxCapabilityId {
  /// musicxx.app.exit
  static const String appExit = 'musicxx.app.exit';
  /// musicxx.app.deepLink
  static const String appDeepLink = 'musicxx.app.deepLink';
  /// 播放前
  static const String playerBeforePlaySong = 'musicxx.player.beforePlaySong';
  /// 音源解析前
  static const String playerSourceBeforeParse = 'musicxx.player.source.beforeParse';
  /// 音源解析
  static const String playerSourceResolve = 'musicxx.player.source.resolve';
  /// 跳转播放位置
  static const String playerSeek = 'musicxx.player.seek';
  /// 调整音量
  static const String playerVolume = 'musicxx.player.volume';
  /// 调整播放速度
  static const String playerSpeed = 'musicxx.player.speed';
  /// 调整音调
  static const String playerPitch = 'musicxx.player.pitch';
  /// 播放失败
  static const String playerError = 'musicxx.player.error';
  /// musicxx.media.notification
  static const String mediaNotification = 'musicxx.media.notification';
  /// musicxx.media.info.analyse
  static const String mediaInfoAnalyse = 'musicxx.media.info.analyse';
  /// musicxx.song.beforeAdd
  static const String songBeforeAdd = 'musicxx.song.beforeAdd';
  /// musicxx.playlist.load
  static const String playlistLoad = 'musicxx.playlist.load';
  /// musicxx.local.scan.file
  static const String localScanFile = 'musicxx.local.scan.file';
  /// 歌词加载
  static const String lyricLoad = 'musicxx.lyric.load';
  /// 歌词加载 · 拟声云歌词
  static const String lyricLoadMusicxxServer = 'musicxx.lyric.load.musicxx_server';
  /// 歌词加载 · 歌词缓存
  static const String lyricLoadCache = 'musicxx.lyric.load.cache';
  /// 歌词加载 · 内置歌词
  static const String lyricLoadBuiltin = 'musicxx.lyric.load.builtin';
  /// 歌词加载 · 外置歌词
  static const String lyricLoadBuiltout = 'musicxx.lyric.load.builtout';
  /// 歌词加载 · 自动匹配
  static const String lyricLoadMatch = 'musicxx.lyric.load.match';
  /// musicxx.media.spectrum.extract
  static const String mediaSpectrumExtract = 'musicxx.media.spectrum.extract';
  /// musicxx.media.chorus.analyse
  static const String mediaChorusAnalyse = 'musicxx.media.chorus.analyse';
  /// musicxx.media.palette.provide
  static const String mediaPaletteProvide = 'musicxx.media.palette.provide';
  /// musicxx.app.net.http.request
  static const String appNetHttpRequest = 'musicxx.app.net.http.request';
  /// musicxx.net.server.route
  static const String netServerRoute = 'musicxx.net.server.route';
  /// musicxx.net.mcp.tools
  static const String netMcpTools = 'musicxx.net.mcp.tools';
  /// musicxx.download
  static const String download = 'musicxx.download';
  /// musicxx.cache.beforeTrim
  static const String cacheBeforeTrim = 'musicxx.cache.beforeTrim';
  /// musicxx.cache.pathRequest
  static const String cachePathRequest = 'musicxx.cache.pathRequest';
  /// musicxx.ui.route.resolve
  static const String uiRouteResolve = 'musicxx.ui.route.resolve';
  /// musicxx.feature.point.call
  static const String featurePointCall = 'musicxx.feature.point.call';
  /// musicxx.app.foreground
  static const String appForeground = 'musicxx.app.foreground';
  /// musicxx.app.background
  static const String appBackground = 'musicxx.app.background';
  /// musicxx.player.play
  static const String playerPlay = 'musicxx.player.play';
  /// musicxx.player.pause
  static const String playerPause = 'musicxx.player.pause';
  /// musicxx.player.toggle
  static const String playerToggle = 'musicxx.player.toggle';
  /// musicxx.player.stop
  static const String playerStop = 'musicxx.player.stop';
  /// musicxx.player.next
  static const String playerNext = 'musicxx.player.next';
  /// musicxx.player.prev
  static const String playerPrev = 'musicxx.player.prev';
  /// musicxx.player.playSrc
  static const String playerPlaySrc = 'musicxx.player.playSrc';
  /// musicxx.player.setVolume
  static const String playerSetVolume = 'musicxx.player.setVolume';
  /// musicxx.player.setSpeed
  static const String playerSetSpeed = 'musicxx.player.setSpeed';
  /// musicxx.player.setPitch
  static const String playerSetPitch = 'musicxx.player.setPitch';
  /// musicxx.player.setQuality
  static const String playerSetQuality = 'musicxx.player.setQuality';
  /// musicxx.player.setLoopMode
  static const String playerSetLoopMode = 'musicxx.player.setLoopMode';
  /// musicxx.player.setMediaType
  static const String playerSetMediaType = 'musicxx.player.setMediaType';
  /// musicxx.playlist.queryLists
  static const String playlistQueryLists = 'musicxx.playlist.queryLists';
  /// musicxx.playlist.querySongs
  static const String playlistQuerySongs = 'musicxx.playlist.querySongs';
  /// musicxx.playlist.play
  static const String playlistPlay = 'musicxx.playlist.play';
  /// musicxx.playlist.addSong
  static const String playlistAddSong = 'musicxx.playlist.addSong';
  /// musicxx.playlist.removeSong
  static const String playlistRemoveSong = 'musicxx.playlist.removeSong';
  /// musicxx.playlist.create
  static const String playlistCreate = 'musicxx.playlist.create';
  /// musicxx.playlist.updateSong
  static const String playlistUpdateSong = 'musicxx.playlist.updateSong';
  /// musicxx.playlist.syncSongs.sync
  static const String playlistSyncSongsSync = 'musicxx.playlist.syncSongs.sync';
  /// musicxx.songlist.querySongs
  static const String songlistQuerySongs = 'musicxx.songlist.querySongs';
  /// musicxx.songlist.playSong
  static const String songlistPlaySong = 'musicxx.songlist.playSong';
  /// musicxx.songlist.addSong
  static const String songlistAddSong = 'musicxx.songlist.addSong';
  /// musicxx.songlist.removeSong
  static const String songlistRemoveSong = 'musicxx.songlist.removeSong';
  /// musicxx.songlist.updateSong
  static const String songlistUpdateSong = 'musicxx.songlist.updateSong';
  /// musicxx.library.querySonglists
  static const String libraryQuerySonglists = 'musicxx.library.querySonglists';
  /// musicxx.library.querySongs
  static const String libraryQuerySongs = 'musicxx.library.querySongs';
  /// musicxx.library.querySonglist
  static const String libraryQuerySonglist = 'musicxx.library.querySonglist';
  /// musicxx.library.playSong
  static const String libraryPlaySong = 'musicxx.library.playSong';
  /// musicxx.library.playSonglist
  static const String libraryPlaySonglist = 'musicxx.library.playSonglist';
  /// musicxx.library.removeSong
  static const String libraryRemoveSong = 'musicxx.library.removeSong';
  /// musicxx.library.createSonglist
  static const String libraryCreateSonglist = 'musicxx.library.createSonglist';
  /// musicxx.library.setSongInfo
  static const String librarySetSongInfo = 'musicxx.library.setSongInfo';
  /// musicxx.library.search
  static const String librarySearch = 'musicxx.library.search';
  /// musicxx.lyric.current.get
  static const String lyricCurrentGet = 'musicxx.lyric.current.get';
  /// musicxx.lyric.current.update
  static const String lyricCurrentUpdate = 'musicxx.lyric.current.update';
  /// musicxx.lyric.current.updateContent
  static const String lyricCurrentUpdateContent = 'musicxx.lyric.current.updateContent';
  /// musicxx.lyric.getBySrc
  static const String lyricGetBySrc = 'musicxx.lyric.getBySrc';
  /// musicxx.lyric.search
  static const String lyricSearch = 'musicxx.lyric.search';
  /// musicxx.ui.toast
  static const String uiToast = 'musicxx.ui.toast';
  /// musicxx.ui.dialog
  static const String uiDialog = 'musicxx.ui.dialog';
  /// musicxx.ui.overlay.show
  static const String uiOverlayShow = 'musicxx.ui.overlay.show';
  /// musicxx.ui.overlay.close
  static const String uiOverlayClose = 'musicxx.ui.overlay.close';
  /// musicxx.ui.openOverlaySheet
  static const String uiOpenOverlaySheet = 'musicxx.ui.openOverlaySheet';
  /// musicxx.ui.openRoute
  static const String uiOpenRoute = 'musicxx.ui.openRoute';
  /// musicxx.ui.pickSongs
  static const String uiPickSongs = 'musicxx.ui.pickSongs';
  /// musicxx.ui.pickDir
  static const String uiPickDir = 'musicxx.ui.pickDir';
  /// musicxx.ui.browseDrive
  static const String uiBrowseDrive = 'musicxx.ui.browseDrive';
  /// musicxx.ui.setEntryBadge
  static const String uiSetEntryBadge = 'musicxx.ui.setEntryBadge';
  /// musicxx.ui.setFontFamily
  static const String uiSetFontFamily = 'musicxx.ui.setFontFamily';
  /// musicxx.ui.getFontFamily
  static const String uiGetFontFamily = 'musicxx.ui.getFontFamily';
  /// musicxx.ui.loadFont
  static const String uiLoadFont = 'musicxx.ui.loadFont';
  /// musicxx.storage.get
  static const String storageGet = 'musicxx.storage.get';
  /// musicxx.storage.set
  static const String storageSet = 'musicxx.storage.set';
  /// musicxx.storage.delete
  static const String storageDelete = 'musicxx.storage.delete';
  /// musicxx.storage.list
  static const String storageList = 'musicxx.storage.list';
  /// musicxx.net.fetch
  static const String netFetch = 'musicxx.net.fetch';
  /// musicxx.net.download
  static const String netDownload = 'musicxx.net.download';
  /// musicxx.render.list
  static const String renderList = 'musicxx.render.list';
  /// musicxx.render.current
  static const String renderCurrent = 'musicxx.render.current';
  /// musicxx.render.select
  static const String renderSelect = 'musicxx.render.select';
  /// musicxx.media.palette
  static const String mediaPalette = 'musicxx.media.palette';
  /// musicxx.media.cover
  static const String mediaCover = 'musicxx.media.cover';
  /// musicxx.media.palette.set
  static const String mediaPaletteSet = 'musicxx.media.palette.set';
  /// musicxx.media.bindImage
  static const String mediaBindImage = 'musicxx.media.bindImage';
  /// musicxx.media.unbindImage
  static const String mediaUnbindImage = 'musicxx.media.unbindImage';
  /// musicxx.media.spectrum
  static const String mediaSpectrum = 'musicxx.media.spectrum';
  /// musicxx.media.kernel
  static const String mediaKernel = 'musicxx.media.kernel';
  /// musicxx.feature.list
  static const String featureList = 'musicxx.feature.list';
  /// musicxx.feature.call
  static const String featureCall = 'musicxx.feature.call';
  /// musicxx.feature.point.list
  static const String featurePointList = 'musicxx.feature.point.list';
  /// musicxx.feature.point.define
  static const String featurePointDefine = 'musicxx.feature.point.define';
  /// musicxx.feature.point.undefine
  static const String featurePointUndefine = 'musicxx.feature.point.undefine';
  /// musicxx.source.define
  static const String sourceDefine = 'musicxx.source.define';
  /// musicxx.source.undefine
  static const String sourceUndefine = 'musicxx.source.undefine';
  /// musicxx.source.list
  static const String sourceList = 'musicxx.source.list';
  /// musicxx.source.browse
  static const String sourceBrowse = 'musicxx.source.browse';
  /// musicxx.hook.handlers
  static const String hookHandlers = 'musicxx.hook.handlers';
  /// musicxx.host.openUrl
  static const String hostOpenUrl = 'musicxx.host.openUrl';
  /// musicxx.host.shareText
  static const String hostShareText = 'musicxx.host.shareText';
  /// musicxx.host.clipboard
  static const String hostClipboard = 'musicxx.host.clipboard';
  /// musicxx.host.getPath
  static const String hostGetPath = 'musicxx.host.getPath';
  /// musicxx.stats.reportMemory
  static const String statsReportMemory = 'musicxx.stats.reportMemory';
  /// musicxx.stats.reportMetric
  static const String statsReportMetric = 'musicxx.stats.reportMetric';

  /// 全部能力 id（按声明顺序）
  static const List<String> all = <String>[
    appExit,
    appDeepLink,
    playerBeforePlaySong,
    playerSourceBeforeParse,
    playerSourceResolve,
    playerSeek,
    playerVolume,
    playerSpeed,
    playerPitch,
    playerError,
    mediaNotification,
    mediaInfoAnalyse,
    songBeforeAdd,
    playlistLoad,
    localScanFile,
    lyricLoad,
    lyricLoadMusicxxServer,
    lyricLoadCache,
    lyricLoadBuiltin,
    lyricLoadBuiltout,
    lyricLoadMatch,
    mediaSpectrumExtract,
    mediaChorusAnalyse,
    mediaPaletteProvide,
    appNetHttpRequest,
    netServerRoute,
    netMcpTools,
    download,
    cacheBeforeTrim,
    cachePathRequest,
    uiRouteResolve,
    featurePointCall,
    appForeground,
    appBackground,
    playerPlay,
    playerPause,
    playerToggle,
    playerStop,
    playerNext,
    playerPrev,
    playerPlaySrc,
    playerSetVolume,
    playerSetSpeed,
    playerSetPitch,
    playerSetQuality,
    playerSetLoopMode,
    playerSetMediaType,
    playlistQueryLists,
    playlistQuerySongs,
    playlistPlay,
    playlistAddSong,
    playlistRemoveSong,
    playlistCreate,
    playlistUpdateSong,
    playlistSyncSongsSync,
    songlistQuerySongs,
    songlistPlaySong,
    songlistAddSong,
    songlistRemoveSong,
    songlistUpdateSong,
    libraryQuerySonglists,
    libraryQuerySongs,
    libraryQuerySonglist,
    libraryPlaySong,
    libraryPlaySonglist,
    libraryRemoveSong,
    libraryCreateSonglist,
    librarySetSongInfo,
    librarySearch,
    lyricCurrentGet,
    lyricCurrentUpdate,
    lyricCurrentUpdateContent,
    lyricGetBySrc,
    lyricSearch,
    uiToast,
    uiDialog,
    uiOverlayShow,
    uiOverlayClose,
    uiOpenOverlaySheet,
    uiOpenRoute,
    uiPickSongs,
    uiPickDir,
    uiBrowseDrive,
    uiSetEntryBadge,
    uiSetFontFamily,
    uiGetFontFamily,
    uiLoadFont,
    storageGet,
    storageSet,
    storageDelete,
    storageList,
    netFetch,
    netDownload,
    renderList,
    renderCurrent,
    renderSelect,
    mediaPalette,
    mediaCover,
    mediaPaletteSet,
    mediaBindImage,
    mediaUnbindImage,
    mediaSpectrum,
    mediaKernel,
    featureList,
    featureCall,
    featurePointList,
    featurePointDefine,
    featurePointUndefine,
    sourceDefine,
    sourceUndefine,
    sourceList,
    sourceBrowse,
    hookHandlers,
    hostOpenUrl,
    hostShareText,
    hostClipboard,
    hostGetPath,
    statsReportMemory,
    statsReportMetric,
  ];
}

/// 广播 id（属主改了东西、通知关心的人）
abstract final class MusicxxBroadcastId {
  /// 应用启动
  static const String appStart = 'musicxx.app.start';
  /// 应用就绪
  static const String appReady = 'musicxx.app.ready';
  /// 应用进入后台
  static const String appBackground = 'musicxx.app.background';
  /// 应用回到前台
  static const String appForeground = 'musicxx.app.foreground';
  /// musicxx.app.upgrade
  static const String appUpgrade = 'musicxx.app.upgrade';
  /// 音源解析完成
  static const String playerSourceResolved = 'musicxx.player.source.resolved';
  /// 播放状态变化
  static const String playerState = 'musicxx.player.state';
  /// 播放进度
  static const String playerPosition = 'musicxx.player.position';
  /// 一曲播放完成
  static const String playerCompleted = 'musicxx.player.completed';
  /// musicxx.player.qualityChanged
  static const String playerQualityChanged = 'musicxx.player.qualityChanged';
  /// 切歌
  static const String songChanged = 'musicxx.song.changed';
  /// musicxx.song.meta.writeback
  static const String songMetaWriteback = 'musicxx.song.meta.writeback';
  /// musicxx.song.removed
  static const String songRemoved = 'musicxx.song.removed';
  /// musicxx.history.record
  static const String historyRecord = 'musicxx.history.record';
  /// musicxx.local.scan.finished
  static const String localScanFinished = 'musicxx.local.scan.finished';
  /// musicxx.lyric.current
  static const String lyricCurrent = 'musicxx.lyric.current';
  /// 封面变化
  static const String mediaCoverChanged = 'musicxx.media.cover.changed';
  /// musicxx.net.lan.event
  static const String netLanEvent = 'musicxx.net.lan.event';
  /// musicxx.ui.page.enter
  static const String uiPageEnter = 'musicxx.ui.page.enter';
  /// musicxx.ui.page.leave
  static const String uiPageLeave = 'musicxx.ui.page.leave';
  /// musicxx.ui.theme.changed
  static const String uiThemeChanged = 'musicxx.ui.theme.changed';
  /// musicxx.ui.user.action
  static const String uiUserAction = 'musicxx.ui.user.action';
  /// 应用状态
  static const String stateApp = 'musicxx.state.app';
  /// 播放器状态
  static const String statePlayer = 'musicxx.state.player';
  /// 当前歌曲
  static const String stateSong = 'musicxx.state.song';
  /// 当前播放列表
  static const String statePlaylist = 'musicxx.state.playlist';
  /// 当前歌词
  static const String stateLyric = 'musicxx.state.lyric';
  /// 歌单列表快照
  static const String statePlaylists = 'musicxx.state.playlists';
  /// 运行环境
  static const String stateEnv = 'musicxx.state.env';
  /// 渲染槽位状态
  static const String stateRenderSlots = 'musicxx.state.renderSlots';
  /// 当前频谱
  static const String stateSpectrum = 'musicxx.state.spectrum';
  /// 状态镜像变化通知
  static const String stateChanged = 'musicxx.state.changed';
  /// 事件发布观测
  static const String eventPublished = 'musicxx.event.published';

  /// 全部广播 id（按声明顺序）
  static const List<String> all = <String>[
    appStart,
    appReady,
    appBackground,
    appForeground,
    appUpgrade,
    playerSourceResolved,
    playerState,
    playerPosition,
    playerCompleted,
    playerQualityChanged,
    songChanged,
    songMetaWriteback,
    songRemoved,
    historyRecord,
    localScanFinished,
    lyricCurrent,
    mediaCoverChanged,
    netLanEvent,
    uiPageEnter,
    uiPageLeave,
    uiThemeChanged,
    uiUserAction,
    stateApp,
    statePlayer,
    stateSong,
    statePlaylist,
    stateLyric,
    statePlaylists,
    stateEnv,
    stateRenderSlots,
    stateSpectrum,
    stateChanged,
    eventPublished,
  ];
}

/// 一条能力的声明
///
/// 字段含义见 `tools/capabilities.def.json` 与 `docs/plugin-capabilities.md`。
final class MusicxxCapabilityDecl {
  const MusicxxCapabilityDecl({
    required this.id,
    required this.impls,
    required this.merge,
    required this.phases,
    required this.dispatch,
    required this.timeoutMs,
    required this.cache,
    required this.expose,
    required this.consent,
    required this.usage,
    required this.implemented,
    required this.title,
    required this.depict,
  });

  final String id;

  /// `one`（后登记覆盖先登记）/ `many`（多实现共存）
  final String impls;

  /// `first` / `veto`；不适用时为空串
  final String merge;

  /// `before` / `impl` / `after` 的子集，按固定次序
  final List<String> phases;

  /// `sync` / `async`
  final String dispatch;

  /// 等实现的上限（毫秒；0 = 不设上限）
  final int timeoutMs;

  /// `none` / `byIdentity`
  final String cache;

  /// `cli` / `mcp` / `link` / `plugin` / `ui` / `page` / `drive` 的子集
  final List<String> expose;

  /// `none` / `ask`（链接触发时是否先问用户）
  final String consent;

  /// `point` / `command` / `service` 的子集
  final List<String> usage;

  /// 主程序是否已经接入（未接入的条目注册不会报错，也不会触发）
  final bool implemented;

  /// 一句话标题（自省与文档用）
  final String title;

  /// 一句话说明（自省与文档用）
  final String depict;

  /// 实现是否开放（`phases` 含 `impl`）；false 时插件只能注册 `before` / `after`
  bool get openForImpl => phases.contains('impl');

  /// 多个实现共存
  bool get many => impls == 'many';

  /// 只作功能埋点（界面上叫"功能埋点"，代码里叫能力）
  bool get isPoint => usage.contains('point');

  /// 能被外部当命令调
  bool get isCommand => usage.contains('command');

  /// 按请求身份缓存结果
  bool get cachedByIdentity => cache == 'byIdentity';
}

/// 一条广播的声明
final class MusicxxBroadcastDecl {
  const MusicxxBroadcastDecl({
    required this.id,
    required this.cache,
    required this.writable,
    required this.pushOnSubscribe,
    required this.coalesce,
    required this.scope,
    required this.implemented,
    required this.title,
    required this.depict,
  });

  final String id;

  /// `none`（事件）/ `latest`（快照）
  final String cache;

  /// `owner` / `anyone`
  final String writable;

  /// 订阅时是否立刻收到当前缓存值（只有 `cache=latest` 能是 true）
  final bool pushOnSubscribe;

  /// 允许合并（允许丢中间值）
  final bool coalesce;

  /// `app` / `system`
  final String scope;

  final bool implemented;

  final String title;

  final String depict;

  /// 能同步读当前值
  bool get cached => cache == 'latest';

  /// 任何人都能写（变量）
  bool get writableByAnyone => writable == 'anyone';
}

/// 一类动态广播（id 由属主在运行期决定，例如变量）
final class MusicxxDynamicBroadcastDecl {
  const MusicxxDynamicBroadcastDecl({
    required this.idPattern,
    required this.cache,
    required this.writable,
    required this.pushOnSubscribe,
    required this.coalesce,
    required this.scope,
    required this.title,
    required this.depict,
  });

  /// `musicxx.var.<属主>.<键>` 这类写法，用 <占位名> 表达属主决定的部分
  final String idPattern;
  final String cache;
  final String writable;
  final bool pushOnSubscribe;
  final bool coalesce;
  final String scope;
  final String title;
  final String depict;
}

/// 声明清单（与 `tools/capabilities.def.json` 一一对应）
abstract final class MusicxxCapabilityCatalogue {
  /// 全部能力声明（按声明顺序）
  static const List<MusicxxCapabilityDecl> capabilities = <MusicxxCapabilityDecl>[
    MusicxxCapabilityDecl(
      id: 'musicxx.app.exit',
      impls: 'many',
      merge: 'veto',
      phases: <String>['impl'],
      dispatch: 'sync',
      timeoutMs: 0,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.app.deepLink',
      impls: 'many',
      merge: 'veto',
      phases: <String>['impl'],
      dispatch: 'sync',
      timeoutMs: 0,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.beforePlaySong',
      impls: 'many',
      merge: 'veto',
      phases: <String>['impl', 'after'],
      dispatch: 'async',
      timeoutMs: 0,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: true,
      title: '播放前',
      depict: '播放某一曲之前：可以跳过本曲（skip）；换音源/改音质当前版本未实现',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.source.beforeParse',
      impls: 'many',
      merge: 'first',
      phases: <String>['impl', 'after'],
      dispatch: 'async',
      timeoutMs: 0,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: true,
      title: '音源解析前',
      depict: '解析某个音源之前：可以跳过它（skip）或替换本轮使用的音源（patch.src）',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.source.resolve',
      impls: 'many',
      merge: 'first',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'byIdentity',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: true,
      title: '音源解析',
      depict: '把一个音源解析成播放器内核能打开的地址与请求头（本地还是网络写在结果里）',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.seek',
      impls: 'many',
      merge: 'veto',
      phases: <String>['impl', 'after'],
      dispatch: 'async',
      timeoutMs: 500,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['point', 'command'],
      implemented: true,
      title: '跳转播放位置',
      depict: '要把播放位置改到新目标之前：可以改目标位置（patch.to）或取消这次跳转（cancel）',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.volume',
      impls: 'many',
      merge: 'veto',
      phases: <String>['impl', 'after'],
      dispatch: 'async',
      timeoutMs: 500,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: true,
      title: '调整音量',
      depict: '要把音量改成新值之前：可以改新值（patch.to）或这次不改（cancel）',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.speed',
      impls: 'many',
      merge: 'veto',
      phases: <String>['impl', 'after'],
      dispatch: 'async',
      timeoutMs: 500,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: true,
      title: '调整播放速度',
      depict: '要把播放速度改成新值之前：可以改新值（patch.to）或这次不改（cancel）',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.pitch',
      impls: 'many',
      merge: 'veto',
      phases: <String>['impl', 'after'],
      dispatch: 'async',
      timeoutMs: 500,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: true,
      title: '调整音调',
      depict: '要把音调改成新值之前：可以改新值（patch.to）或这次不改（cancel）',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.error',
      impls: 'many',
      merge: 'veto',
      phases: <String>['impl', 'after'],
      dispatch: 'async',
      timeoutMs: 0,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: true,
      title: '播放失败',
      depict: '播放失败：可以停止播放（stop）、跳过当前曲目（skip）或不再尝试当前源（patch.tryNextSrc）',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.media.notification',
      impls: 'many',
      merge: 'first',
      phases: <String>['impl'],
      dispatch: 'sync',
      timeoutMs: 0,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.media.info.analyse',
      impls: 'many',
      merge: 'first',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 0,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.song.beforeAdd',
      impls: 'many',
      merge: 'veto',
      phases: <String>['impl'],
      dispatch: 'sync',
      timeoutMs: 0,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.playlist.load',
      impls: 'one',
      merge: '',
      phases: <String>['before', 'after'],
      dispatch: 'async',
      timeoutMs: 0,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.local.scan.file',
      impls: 'many',
      merge: 'veto',
      phases: <String>['impl'],
      dispatch: 'sync',
      timeoutMs: 0,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.lyric.load',
      impls: 'many',
      merge: 'first',
      phases: <String>['before', 'impl', 'after'],
      dispatch: 'async',
      timeoutMs: 8000,
      cache: 'byIdentity',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: true,
      title: '歌词加载',
      depict: '为一首歌加载歌词（整条链：拟声云歌词 → 歌词缓存 → 内置 → 外置 → 自动匹配）',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.lyric.load.musicxx_server',
      impls: 'many',
      merge: 'first',
      phases: <String>['before', 'impl', 'after'],
      dispatch: 'async',
      timeoutMs: 8000,
      cache: 'byIdentity',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: true,
      title: '歌词加载 · 拟声云歌词',
      depict: '读拟声云歌词（用户指定 / 歌曲自带的歌词来源）或绑定的本地歌词',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.lyric.load.cache',
      impls: 'many',
      merge: 'first',
      phases: <String>['before', 'impl', 'after'],
      dispatch: 'async',
      timeoutMs: 8000,
      cache: 'byIdentity',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: true,
      title: '歌词加载 · 歌词缓存',
      depict: '读歌词缓存（按音源身份、md5、sha1、大小与时长命中）',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.lyric.load.builtin',
      impls: 'many',
      merge: 'first',
      phases: <String>['before', 'impl', 'after'],
      dispatch: 'async',
      timeoutMs: 8000,
      cache: 'byIdentity',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: true,
      title: '歌词加载 · 内置歌词',
      depict: '读内置歌词：播放器元数据标签，以及媒体解码器从音频文件 / 缓存流里读出的标签歌词',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.lyric.load.builtout',
      impls: 'many',
      merge: 'first',
      phases: <String>['before', 'impl', 'after'],
      dispatch: 'async',
      timeoutMs: 8000,
      cache: 'byIdentity',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: true,
      title: '歌词加载 · 外置歌词',
      depict: '读外置歌词（同名 .lrc、云盘与媒体库的歌词接口）',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.lyric.load.match',
      impls: 'many',
      merge: 'first',
      phases: <String>['before', 'impl', 'after'],
      dispatch: 'async',
      timeoutMs: 8000,
      cache: 'byIdentity',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: true,
      title: '歌词加载 · 自动匹配',
      depict: '自动匹配歌词（按歌名 / 歌手 / hash / 大小 / 时长要推荐歌词）',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.media.spectrum.extract',
      impls: 'many',
      merge: 'first',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 30000,
      cache: 'byIdentity',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.media.chorus.analyse',
      impls: 'many',
      merge: 'first',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 20000,
      cache: 'byIdentity',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.media.palette.provide',
      impls: 'many',
      merge: 'first',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 800,
      cache: 'byIdentity',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.app.net.http.request',
      impls: 'one',
      merge: '',
      phases: <String>['before', 'after'],
      dispatch: 'async',
      timeoutMs: 0,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.net.server.route',
      impls: 'many',
      merge: 'first',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 0,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.net.mcp.tools',
      impls: 'many',
      merge: 'first',
      phases: <String>['impl'],
      dispatch: 'sync',
      timeoutMs: 0,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.download',
      impls: 'one',
      merge: '',
      phases: <String>['before', 'after'],
      dispatch: 'async',
      timeoutMs: 0,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.cache.beforeTrim',
      impls: 'many',
      merge: 'veto',
      phases: <String>['impl'],
      dispatch: 'sync',
      timeoutMs: 0,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.cache.pathRequest',
      impls: 'many',
      merge: 'first',
      phases: <String>['impl'],
      dispatch: 'sync',
      timeoutMs: 0,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.ui.route.resolve',
      impls: 'many',
      merge: 'first',
      phases: <String>['impl'],
      dispatch: 'sync',
      timeoutMs: 0,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.feature.point.call',
      impls: 'many',
      merge: 'first',
      phases: <String>['before', 'impl', 'after'],
      dispatch: 'async',
      timeoutMs: 0,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['point', 'command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.app.foreground',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 0,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.app.background',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 0,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.play',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.pause',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.toggle',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.stop',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.next',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.prev',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.playSrc',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.setVolume',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.setSpeed',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.setPitch',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.setQuality',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.setLoopMode',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.setMediaType',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.playlist.queryLists',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.playlist.querySongs',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.playlist.play',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.playlist.addSong',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.playlist.removeSong',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.playlist.create',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.playlist.updateSong',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.playlist.syncSongs.sync',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.songlist.querySongs',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.songlist.playSong',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.songlist.addSong',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.songlist.removeSong',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.songlist.updateSong',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.library.querySonglists',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.library.querySongs',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.library.querySonglist',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.library.playSong',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.library.playSonglist',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.library.removeSong',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.library.createSonglist',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.library.setSongInfo',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.library.search',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.lyric.current.get',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.lyric.current.update',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.lyric.current.updateContent',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.lyric.getBySrc',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.lyric.search',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.ui.toast',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.ui.dialog',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.ui.overlay.show',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.ui.overlay.close',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.ui.openOverlaySheet',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.ui.openRoute',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.ui.pickSongs',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.ui.pickDir',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.ui.browseDrive',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.ui.setEntryBadge',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.ui.setFontFamily',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.ui.getFontFamily',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.ui.loadFont',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.storage.get',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.storage.set',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.storage.delete',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.storage.list',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.net.fetch',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.net.download',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.render.list',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.render.current',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.render.select',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.media.palette',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.media.cover',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.media.palette.set',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.media.bindImage',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.media.unbindImage',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.media.spectrum',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.media.kernel',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.feature.list',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.feature.call',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.feature.point.list',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.feature.point.define',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.feature.point.undefine',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.source.define',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.source.undefine',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.source.list',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.source.browse',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.hook.handlers',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.host.openUrl',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.host.shareText',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.host.clipboard',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.host.getPath',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.stats.reportMemory',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.stats.reportMetric',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '',
      depict: '',
    ),
  ];

  /// 全部广播声明（按声明顺序）
  static const List<MusicxxBroadcastDecl> broadcasts = <MusicxxBroadcastDecl>[
    MusicxxBroadcastDecl(
      id: 'musicxx.app.start',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      implemented: true,
      title: '应用启动',
      depict: '主程序启动阶段（业务 Store 与插件框架都就绪之后）派发一次',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.app.ready',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      implemented: true,
      title: '应用就绪',
      depict: '应用初始化收尾（首屏与数据都就绪）派发一次，比应用启动晚',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.app.background',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      implemented: true,
      title: '应用进入后台',
      depict: '应用进入后台（窗口隐藏 / 最小化，或移动到后台）时派发；观察型，不等待',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.app.foreground',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      implemented: true,
      title: '应用回到前台',
      depict: '应用回到前台时派发，带这次在后台待了多久；观察型，不等待',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.app.upgrade',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.player.source.resolved',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      implemented: true,
      title: '音源解析完成',
      depict: '某个音源解析完成（拿到真正播放用的路径与请求头）；载荷里没有直链与 token',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.player.state',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      implemented: true,
      title: '播放状态变化',
      depict: '播放状态变化（播放 / 暂停 / 停止 / 结束）时派发；播放进度不走这个埋点',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.player.position',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      implemented: true,
      title: '播放进度',
      depict: '播放进度（1 秒节流）；只报位置，不报“是否在播放”',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.player.completed',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      implemented: true,
      title: '一曲播放完成',
      depict: '一曲播放完成（正常结束或按完成处理）之后派发；不代表已经切歌',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.player.qualityChanged',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.song.changed',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      implemented: true,
      title: '切歌',
      depict: '切歌时派发（派发前调用方已刷新状态镜像与封面）',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.song.meta.writeback',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.song.removed',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.history.record',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.local.scan.finished',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.lyric.current',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.media.cover.changed',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      implemented: true,
      title: '封面变化',
      depict: '当前歌曲的封面发生变化（换歌、换封面图、封面被清空）；载荷里没有直链与图片字节',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.net.lan.event',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.ui.page.enter',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.ui.page.leave',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.ui.theme.changed',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.ui.user.action',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.state.app',
      cache: 'latest',
      writable: 'owner',
      pushOnSubscribe: true,
      coalesce: true,
      scope: 'app',
      implemented: true,
      title: '应用状态',
      depict: '状态镜像：属主推送，插件订阅即拿到当前值，也可以同步读缓存',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.state.player',
      cache: 'latest',
      writable: 'owner',
      pushOnSubscribe: true,
      coalesce: true,
      scope: 'app',
      implemented: true,
      title: '播放器状态',
      depict: '状态镜像：属主推送，插件订阅即拿到当前值，也可以同步读缓存',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.state.song',
      cache: 'latest',
      writable: 'owner',
      pushOnSubscribe: true,
      coalesce: true,
      scope: 'app',
      implemented: true,
      title: '当前歌曲',
      depict: '状态镜像：属主推送，插件订阅即拿到当前值，也可以同步读缓存',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.state.playlist',
      cache: 'latest',
      writable: 'owner',
      pushOnSubscribe: true,
      coalesce: true,
      scope: 'app',
      implemented: true,
      title: '当前播放列表',
      depict: '状态镜像：属主推送，插件订阅即拿到当前值，也可以同步读缓存',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.state.lyric',
      cache: 'latest',
      writable: 'owner',
      pushOnSubscribe: true,
      coalesce: true,
      scope: 'app',
      implemented: true,
      title: '当前歌词',
      depict: '状态镜像：属主推送，插件订阅即拿到当前值，也可以同步读缓存',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.state.playlists',
      cache: 'latest',
      writable: 'owner',
      pushOnSubscribe: true,
      coalesce: true,
      scope: 'app',
      implemented: true,
      title: '歌单列表快照',
      depict: '状态镜像：属主推送，插件订阅即拿到当前值，也可以同步读缓存',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.state.env',
      cache: 'latest',
      writable: 'owner',
      pushOnSubscribe: true,
      coalesce: true,
      scope: 'app',
      implemented: true,
      title: '运行环境',
      depict: '状态镜像：属主推送，插件订阅即拿到当前值，也可以同步读缓存',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.state.renderSlots',
      cache: 'latest',
      writable: 'owner',
      pushOnSubscribe: true,
      coalesce: true,
      scope: 'app',
      implemented: true,
      title: '渲染槽位状态',
      depict: '状态镜像：属主推送，插件订阅即拿到当前值，也可以同步读缓存',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.state.spectrum',
      cache: 'latest',
      writable: 'owner',
      pushOnSubscribe: true,
      coalesce: true,
      scope: 'app',
      implemented: true,
      title: '当前频谱',
      depict: '状态镜像：属主推送，插件订阅即拿到当前值，也可以同步读缓存',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.state.changed',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'system',
      implemented: true,
      title: '状态镜像变化通知',
      depict: '任意状态键变化时推一次，载荷 `{key}`；要用当前值请直接读那个键',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.event.published',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'system',
      implemented: true,
      title: '事件发布观测',
      depict: '调试用：插件发布事件时推一份原始载荷',
    ),
  ];

  /// 动态广播声明（id 在运行期由属主决定）
  static const List<MusicxxDynamicBroadcastDecl> dynamicBroadcasts =
      <MusicxxDynamicBroadcastDecl>[
    MusicxxDynamicBroadcastDecl(
      idPattern: 'musicxx.var.<属主>.<键>',
      cache: 'latest',
      writable: 'anyone',
      pushOnSubscribe: true,
      coalesce: false,
      scope: 'app',
      title: '变量',
      depict: '有属主的小值（配置 / 小状态）；写要回执，订阅时立刻收到当前值',
    ),
    MusicxxDynamicBroadcastDecl(
      idPattern: 'plugin.<插件 id>.net',
      cache: 'none',
      writable: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      title: '插件网络事件',
      depict: '插件自己的网络请求进度 / 完成事件',
    ),
  ];

  static final Map<String, MusicxxCapabilityDecl> _capabilityIndex =
      <String, MusicxxCapabilityDecl>{
    for (final MusicxxCapabilityDecl decl in capabilities) decl.id: decl,
  };

  static final Map<String, MusicxxBroadcastDecl> _broadcastIndex =
      <String, MusicxxBroadcastDecl>{
    for (final MusicxxBroadcastDecl decl in broadcasts) decl.id: decl,
  };

  /// 按 id 找能力声明（没有时返回 null）
  static MusicxxCapabilityDecl? capabilityOf(String id) =>
      _capabilityIndex[id];

  /// 按 id 找广播声明（没有时返回 null）
  static MusicxxBroadcastDecl? broadcastOf(String id) =>
      _broadcastIndex[id];

  /// 这个 id 是不是声明过的能力
  static bool declaresCapability(String id) =>
      _capabilityIndex.containsKey(id);

  /// 这个 id 是不是声明过的广播
  static bool declaresBroadcast(String id) =>
      _broadcastIndex.containsKey(id);
}
