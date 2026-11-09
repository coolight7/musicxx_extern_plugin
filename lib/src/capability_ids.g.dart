// 自动生成（tools/gen_contract.dart ← tools/capabilities.def.json）—— 请勿手改。
//
// 能力 = 调用方发一次请求、等一个结果；广播 = 属主改了东西、通知关心的人。
// id 是跨边界稳定的约定：字符串值一旦发布不得修改，只能新增或标记废弃。
// ignore_for_file: type=lint, constant_identifier_names

/// 能力阶段（参与方式）：同一条能力上的三段
///
/// 能力 id 表示**一次调用**；参与方式由注册时的阶段表达：
/// - [before]：进入调用之前，串行、正序，只能改输入或 `stop`；
/// - [impl]：实现链，给出结果（多数能力只有这一段）；
/// - [after]：拿到结果之后，串行、逆序，只能改输出或 `drop`。
enum MusicxxCapabilityPhase {
  impl(0),

  before(1),

  after(2);

  const MusicxxCapabilityPhase(this.code);

  /// 阶段编号（与 C 侧 `MUSICXX_CAPABILITY_PHASE_*` 一致）
  final int code;

  /// 位掩码（`1 << code`，与 C 侧 `MUSICXX_CAPABILITY_PHASE_MASK_*` 一致）
  int get mask => 1 << code;

  /// 对外字符串（事件载荷与自省用它）
  String get wireName {
    switch (this) {
      case MusicxxCapabilityPhase.before:
        return "before";
      case MusicxxCapabilityPhase.impl:
        return "impl";
      case MusicxxCapabilityPhase.after:
        return "after";
    }
  }

  /// 按对外字符串反查（未知名字返回 null）
  static MusicxxCapabilityPhase? tryFromName(String name) {
    switch (name) {
      case "impl":
        return MusicxxCapabilityPhase.impl;
      case "before":
        return MusicxxCapabilityPhase.before;
      case "after":
        return MusicxxCapabilityPhase.after;
    }
    return null;
  }
}

/// 合并策略：多个实现都给出结果时怎么合
enum MusicxxCapabilityMerge {
  /// 第一个合法结果生效，后面的实现不再问
  first('first'),

  /// 任一实现能否决（最保守的裁决不被后面的实现覆盖）
  veto('veto');

  const MusicxxCapabilityMerge(this.wireName);

  /// 声明里的写法（`first` / `veto`）
  final String wireName;

  /// 按声明里的写法反查（空串与未知值都返回 null：包装点没有实现链，不需要策略）
  static MusicxxCapabilityMerge? tryFromName(String name) {
    switch (name) {
      case "first":
        return MusicxxCapabilityMerge.first;
      case "veto":
        return MusicxxCapabilityMerge.veto;
    }
    return null;
  }
}

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
  /// 调用功能点
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
  /// 按来源播放
  static const String playerPlaySrc = 'musicxx.player.playSrc';
  /// 设置应用内音量
  static const String playerSetVolume = 'musicxx.player.setVolume';
  /// 设置播放速度
  static const String playerSetSpeed = 'musicxx.player.setSpeed';
  /// 设置音调
  static const String playerSetPitch = 'musicxx.player.setPitch';
  /// 设置音质
  static const String playerSetQuality = 'musicxx.player.setQuality';
  /// 设置播放模式
  static const String playerSetLoopMode = 'musicxx.player.setLoopMode';
  /// musicxx.player.setMediaType
  static const String playerSetMediaType = 'musicxx.player.setMediaType';
  /// musicxx.playlist.queryLists
  static const String playlistQueryLists = 'musicxx.playlist.queryLists';
  /// 读歌单歌曲
  static const String playlistQuerySongs = 'musicxx.playlist.querySongs';
  /// 播放歌单
  static const String playlistPlay = 'musicxx.playlist.play';
  /// musicxx.playlist.addSong
  static const String playlistAddSong = 'musicxx.playlist.addSong';
  /// 从歌单移除歌曲
  static const String playlistRemoveSong = 'musicxx.playlist.removeSong';
  /// 新建歌单
  static const String playlistCreate = 'musicxx.playlist.create';
  /// musicxx.playlist.updateSong
  static const String playlistUpdateSong = 'musicxx.playlist.updateSong';
  /// 同步歌单音源
  static const String playlistSyncSongsSync = 'musicxx.playlist.syncSongs.sync';
  /// 读播放列表歌曲
  static const String songlistQuerySongs = 'musicxx.songlist.querySongs';
  /// 播播放列表里的歌
  static const String songlistPlaySong = 'musicxx.songlist.playSong';
  /// musicxx.songlist.addSong
  static const String songlistAddSong = 'musicxx.songlist.addSong';
  /// 从播放列表移除歌曲
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
  /// 当前歌词
  static const String lyricCurrentGet = 'musicxx.lyric.current.get';
  /// 换当前歌词
  static const String lyricCurrentUpdate = 'musicxx.lyric.current.update';
  /// 换显示歌词
  static const String lyricCurrentUpdateContent = 'musicxx.lyric.current.updateContent';
  /// 按来源取歌词
  static const String lyricGetBySrc = 'musicxx.lyric.getBySrc';
  /// 搜索歌词
  static const String lyricSearch = 'musicxx.lyric.search';
  /// 轻提示
  static const String uiToast = 'musicxx.ui.toast';
  /// 确认弹窗
  static const String uiDialog = 'musicxx.ui.dialog';
  /// musicxx.ui.overlay.show
  static const String uiOverlayShow = 'musicxx.ui.overlay.show';
  /// 关闭浮层
  static const String uiOverlayClose = 'musicxx.ui.overlay.close';
  /// musicxx.ui.openOverlaySheet
  static const String uiOpenOverlaySheet = 'musicxx.ui.openOverlaySheet';
  /// 打开页面
  static const String uiOpenRoute = 'musicxx.ui.openRoute';
  /// musicxx.ui.pickSongs
  static const String uiPickSongs = 'musicxx.ui.pickSongs';
  /// musicxx.ui.pickDir
  static const String uiPickDir = 'musicxx.ui.pickDir';
  /// 浏览插件网盘
  static const String uiBrowseDrive = 'musicxx.ui.browseDrive';
  /// musicxx.ui.setEntryBadge
  static const String uiSetEntryBadge = 'musicxx.ui.setEntryBadge';
  /// 切换字体
  static const String uiSetFontFamily = 'musicxx.ui.setFontFamily';
  /// musicxx.ui.getFontFamily
  static const String uiGetFontFamily = 'musicxx.ui.getFontFamily';
  /// 设置动画等级
  static const String uiSetAnimatedLevel = 'musicxx.ui.setAnimatedLevel';
  /// 设置粒子动画
  static const String uiSetParticleAnimate = 'musicxx.ui.setParticleAnimate';
  /// 设置歌曲图旋转
  static const String uiSetSongIconRotate = 'musicxx.ui.setSongIconRotate';
  /// 设置歌曲图波浪
  static const String uiSetSongIconWave = 'musicxx.ui.setSongIconWave';
  /// 切换当前主题
  static const String themeSetCurrent = 'musicxx.theme.setCurrent';
  /// 设置主题模式
  static const String themeSetMode = 'musicxx.theme.setMode';
  /// 临时切换主题
  static const String themeTempSwitch = 'musicxx.theme.tempSwitch';
  /// 加载字体
  static const String uiLoadFont = 'musicxx.ui.loadFont';
  /// 读插件存储
  static const String storageGet = 'musicxx.storage.get';
  /// 写插件存储
  static const String storageSet = 'musicxx.storage.set';
  /// 删插件存储
  static const String storageDelete = 'musicxx.storage.delete';
  /// 列出插件存储
  static const String storageList = 'musicxx.storage.list';
  /// musicxx.net.fetch
  static const String netFetch = 'musicxx.net.fetch';
  /// musicxx.net.download
  static const String netDownload = 'musicxx.net.download';
  /// 列出渲染槽位
  static const String renderList = 'musicxx.render.list';
  /// 当前渲染项
  static const String renderCurrent = 'musicxx.render.current';
  /// 选择渲染项
  static const String renderSelect = 'musicxx.render.select';
  /// 封面取色
  static const String mediaPalette = 'musicxx.media.palette';
  /// musicxx.media.cover
  static const String mediaCover = 'musicxx.media.cover';
  /// musicxx.media.palette.set
  static const String mediaPaletteSet = 'musicxx.media.palette.set';
  /// 登记插件图片
  static const String mediaBindImage = 'musicxx.media.bindImage';
  /// 撤销插件图片
  static const String mediaUnbindImage = 'musicxx.media.unbindImage';
  /// musicxx.media.spectrum
  static const String mediaSpectrum = 'musicxx.media.spectrum';
  /// 媒体内核信息
  static const String mediaKernel = 'musicxx.media.kernel';
  /// musicxx.feature.list
  static const String featureList = 'musicxx.feature.list';
  /// 调用能力
  static const String featureCall = 'musicxx.feature.call';
  /// 功能点清单
  static const String featurePointList = 'musicxx.feature.point.list';
  /// 声明功能点
  static const String featurePointDefine = 'musicxx.feature.point.define';
  /// 收回功能点
  static const String featurePointUndefine = 'musicxx.feature.point.undefine';
  /// 声明音源类型
  static const String sourceDefine = 'musicxx.source.define';
  /// 收回音源类型
  static const String sourceUndefine = 'musicxx.source.undefine';
  /// musicxx.source.list
  static const String sourceList = 'musicxx.source.list';
  /// 声明可浏览来源
  static const String sourceBrowse = 'musicxx.source.browse';
  /// 能力订阅 / 实现行
  static const String capabilitySubscribers = 'musicxx.capability.subscribers';
  /// 打开链接
  static const String hostOpenUrl = 'musicxx.host.openUrl';
  /// musicxx.host.shareText
  static const String hostShareText = 'musicxx.host.shareText';
  /// 写剪贴板
  static const String hostClipboard = 'musicxx.host.clipboard';
  /// musicxx.host.getPath
  static const String hostGetPath = 'musicxx.host.getPath';
  /// 自报内存占用
  static const String statsReportMemory = 'musicxx.stats.reportMemory';
  /// 自报自定义指标
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
    uiSetAnimatedLevel,
    uiSetParticleAnimate,
    uiSetSongIconRotate,
    uiSetSongIconWave,
    themeSetCurrent,
    themeSetMode,
    themeTempSwitch,
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
    capabilitySubscribers,
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
  /// 前后台状态
  static const String appLifecycle = 'musicxx.app.lifecycle';
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
  /// 动画等级
  static const String uiAnimatedLevel = 'musicxx.ui.animatedLevel';
  /// 粒子动画
  static const String uiParticleAnimate = 'musicxx.ui.particleAnimate';
  /// 歌曲图旋转
  static const String uiSongIconRotate = 'musicxx.ui.songIconRotate';
  /// 歌曲图波浪
  static const String uiSongIconWave = 'musicxx.ui.songIconWave';
  /// 全局字体
  static const String uiFontFamily = 'musicxx.ui.fontFamily';
  /// 可用主题
  static const String themeAvailable = 'musicxx.theme.available';
  /// 当前主题
  static const String themeCurrent = 'musicxx.theme.current';
  /// 主题切换模式
  static const String themeMode = 'musicxx.theme.mode';
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
  /// 事件发布观测
  static const String eventPublished = 'musicxx.event.published';

  /// 全部广播 id（按声明顺序）
  static const List<String> all = <String>[
    appStart,
    appReady,
    appBackground,
    appForeground,
    appUpgrade,
    appLifecycle,
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
    uiAnimatedLevel,
    uiParticleAnimate,
    uiSongIconRotate,
    uiSongIconWave,
    uiFontFamily,
    themeAvailable,
    themeCurrent,
    themeMode,
    stateApp,
    statePlayer,
    stateSong,
    statePlaylist,
    stateLyric,
    statePlaylists,
    stateEnv,
    stateRenderSlots,
    stateSpectrum,
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

  /// 合并策略对象（`merge` 为空 = 不适用，例如包装点没有实现链）
  MusicxxCapabilityMerge? get mergeKind =>
      MusicxxCapabilityMerge.tryFromName(merge);

  /// 这条 id 是不是广播频道（通知型：只通知、不回答）
  bool get isNotification => null != MusicxxCapabilityCatalogue.broadcastOf(id);
}

/// 一条广播的声明
final class MusicxxBroadcastDecl {
  const MusicxxBroadcastDecl({
    required this.id,
    required this.cache,
    required this.publish,
    required this.pushOnSubscribe,
    required this.coalesce,
    required this.scope,
    required this.type,
    required this.options,
    required this.implemented,
    required this.title,
    required this.depict,
  });

  final String id;

  /// `none`（事件）/ `latest`（快照）
  final String cache;

  /// `owner` / `anyone`：谁能往这个频道发布
  final String publish;

  /// 订阅时是否立刻收到当前缓存值（只有 `cache=latest` 能是 true）
  final bool pushOnSubscribe;

  /// 允许合并（允许丢中间值）
  final bool coalesce;

  /// `app` / `system`
  final String scope;

  /// 值的类型提示（`bool` / `number` / `string` / `json`；空串 = 没写）
  final String type;

  /// 枚举取值域（稳定 id；只用于文档与自省）
  final List<String> options;

  final bool implemented;

  final String title;

  final String depict;

  /// 能同步读当前值
  bool get cached => cache == 'latest';

  /// 任何人都能往它发布
  bool get publishByAnyone => publish == 'anyone';
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
      impls: 'one',
      merge: '',
      phases: <String>['before', 'after'],
      dispatch: 'async',
      timeoutMs: 500,
      cache: 'none',
      expose: <String>['cli', 'mcp', 'link', 'plugin'],
      consent: 'none',
      usage: <String>['point', 'command'],
      implemented: true,
      title: '跳转播放位置',
      depict: '要把播放位置改到新目标之前：可以在前链改目标位置（patch.to）或拦下这次跳转（cancel），在后链改最终位置或作废这次的值（drop）',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.volume',
      impls: 'one',
      merge: '',
      phases: <String>['before', 'after'],
      dispatch: 'async',
      timeoutMs: 500,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: true,
      title: '调整音量',
      depict: '要把音量改成新值之前：可以在前链改新值（patch.to）或这次不改（cancel），在后链改最终值或作废这次的值（drop）',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.speed',
      impls: 'one',
      merge: '',
      phases: <String>['before', 'after'],
      dispatch: 'async',
      timeoutMs: 500,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: true,
      title: '调整播放速度',
      depict: '要把播放速度改成新值之前：可以在前链改新值（patch.to）或这次不改（cancel），在后链改最终值或作废这次的值（drop）',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.player.pitch',
      impls: 'one',
      merge: '',
      phases: <String>['before', 'after'],
      dispatch: 'async',
      timeoutMs: 500,
      cache: 'none',
      expose: <String>[],
      consent: 'none',
      usage: <String>['point'],
      implemented: true,
      title: '调整音调',
      depict: '要把音调改成新值之前：可以在前链改新值（patch.to）或这次不改（cancel），在后链改最终值或作废这次的值（drop）',
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
      title: '调用功能点',
      depict: '调用一个功能点（应用留出的或插件自己声明的）；只读值缓存，不改应用状态',
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
      title: '按来源播放',
      depict: '播放一个或多个音源（文件 / 链接 / 音源字符串；多个作为临时歌单）',
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
      title: '设置应用内音量',
      depict: '把应用内音量改到 0~1（大于 1 按百分比处理），不影响系统音量',
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
      title: '设置播放速度',
      depict: '把播放速度改到 0.25~8 倍（1 = 原速）',
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
      title: '设置音调',
      depict: '把音调改到 0.25~2 倍（1 = 原调），不影响播放速度',
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
      title: '设置音质',
      depict: '选在线音源播放时想要的音质（AudioQuality_e 的名字）',
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
      title: '设置播放模式',
      depict: '改播放模式（Order / RealRand / Only / SimulateRand / SimulateRandReflush）',
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
      title: '读歌单歌曲',
      depict: '读一张歌单（或当前播放列表）里的歌曲，可截断与要完整字段',
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
      title: '播放歌单',
      depict: '打开一张歌单并从指定位置开始播',
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
      title: '从歌单移除歌曲',
      depict: '把歌从保存下来的歌单里删掉（sid 或 sids，可多个）',
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
      title: '新建歌单',
      depict: '新建一张本地或在线歌单，可以顺手把歌加进去',
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
      title: '同步歌单音源',
      depict: '把歌单绑定的音源重读一遍，新增的歌加进歌单',
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
      title: '读播放列表歌曲',
      depict: '读当前播放列表里的歌曲，可截断与要完整字段',
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
      title: '播播放列表里的歌',
      depict: '按 sid 播放当前播放列表里的某一首',
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
      title: '从播放列表移除歌曲',
      depict: '只改当前播放队列（sid 或 sids，可多个）',
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
      title: '当前歌词',
      depict: '读当前歌曲的歌词内容与逐行信息（limit 截断、word 带逐字）',
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
      title: '换当前歌词',
      depict: '改当前歌曲的拟声云歌词 id 或这首歌的歌词偏移',
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
      title: '换显示歌词',
      depict: '直接换显示内容（不写缓存、不改歌曲绑定）',
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
      title: '按来源取歌词',
      depict: '按音源或云歌词 id 读歌词，不换当前歌词',
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
      title: '搜索歌词',
      depict: '按关键词或歌名歌手搜拟声云歌词',
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
      title: '轻提示',
      depict: '弹一条轻提示（kind 为 error 时用错误样式）',
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
      title: '确认弹窗',
      depict: '弹一个确认框（正文或用 blocks 写富内容），等用户点确定',
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
      title: '关闭浮层',
      depict: '关掉自己在显示的浮层（不给 id 时关当前那个）',
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
      title: '打开页面',
      depict: '跳到官方页面或插件自己的页面（ext://<插件id>/<视图id>）',
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
      title: '浏览插件网盘',
      depict: '打开某个插件音源的浏览器（可指定分类与起始路径）',
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
      title: '切换字体',
      depict: '把界面字体换成已注册的字体（空串 = 恢复默认）',
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
      id: 'musicxx.ui.setAnimatedLevel',
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
      title: '设置动画等级',
      depict: '把界面动画等级改成指定值（越高动画越多，越低越省电）',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.ui.setParticleAnimate',
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
      title: '设置粒子动画',
      depict: '开关点击与拖动时的粒子效果',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.ui.setSongIconRotate',
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
      title: '设置歌曲图旋转',
      depict: '开关播放时圆形歌曲图的旋转',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.ui.setSongIconWave',
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
      title: '设置歌曲图波浪',
      depict: '开关播放时歌曲图随节拍起伏',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.theme.setCurrent',
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
      title: '切换当前主题',
      depict: '只切显示的主题（不改主题模式、不写用户的模式设置、不暂停跟随）',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.theme.setMode',
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
      title: '设置主题模式',
      depict: '改主题模式（跟随系统 / 跟随时间 / 跟随屏幕亮度 / 固定白昼 / 固定夜间）并立即生效',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.theme.tempSwitch',
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
      title: '临时切换主题',
      depict: '临时切一次主题（toggle = 明暗互切）；会暂停主题跟随',
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
      title: '加载字体',
      depict: '从文件或 base64 注册一个字体（apply = 加载后立即使用）',
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
      title: '读插件存储',
      depict: '读插件私有存储里的一个键（命名空间可省）',
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
      title: '写插件存储',
      depict: '把值写进插件私有存储（值可以是任意 JSON）',
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
      title: '删插件存储',
      depict: '删掉插件私有存储里的一个键',
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
      title: '列出插件存储',
      depict: '列出插件私有存储里的键（命名空间可省）',
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
      title: '列出渲染槽位',
      depict: '列出渲染槽位与候选（可以只看一个槽位）',
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
      title: '当前渲染项',
      depict: '读某个槽位当前选中的项',
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
      title: '选择渲染项',
      depict: '把某个槽位换成指定候选（插件项与内置项同一份列表）',
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
      title: '封面取色',
      depict: '取当前封面的主色调（force 时跳缓存重算）',
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
      title: '登记插件图片',
      depict: '把一张图片（base64）按 key 登记给主程序的图片槽位',
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
      title: '撤销插件图片',
      depict: '按 key 撤销之前登记的图片',
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
      title: '媒体内核信息',
      depict: '读播放内核与解码信息（part 选段，refresh 重新探测）',
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
      title: '调用能力',
      depict: '调一个应用能力（见 `musicxx.feature.list`），返回文本或 JSON',
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
      title: '功能点清单',
      depict: '列出功能点与实现行（谁在提供、哪个阶段、排第几、有没有被关掉）',
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
      title: '声明功能点',
      depict: '声明一个自己的功能点（`plugin.<自己的插件 id>.<名字>`），实现是那条通用通道的处理器',
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
      title: '收回功能点',
      depict: '收回自己声明的功能点（停用 / 卸载会自动撤）',
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
      title: '声明音源类型',
      depict: '声明一个自己的音源类型（`plugin.<自己的插件 id>.<类型名>`），播放地址由音源解析给出',
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
      title: '收回音源类型',
      depict: '收回自己声明的音源类型；给了 `view` 时只撤那一个浏览分类',
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
      title: '声明可浏览来源',
      depict: '声明这个音源类型可以浏览挑歌（一个类型可以有多个分类，界面复用内置网盘那一套）',
    ),
    MusicxxCapabilityDecl(
      id: 'musicxx.capability.subscribers',
      impls: 'one',
      merge: '',
      phases: <String>['impl'],
      dispatch: 'async',
      timeoutMs: 5000,
      cache: 'none',
      expose: <String>['plugin'],
      consent: 'none',
      usage: <String>['command'],
      implemented: true,
      title: '能力订阅 / 实现行',
      depict: '读某条能力当前的实现行（谁提供、什么形态、在哪个阶段、排第几）',
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
      title: '打开链接',
      depict: '把地址交给系统打开（协议不限）',
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
      title: '写剪贴板',
      depict: '把文本写进系统剪贴板',
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
      title: '自报内存占用',
      depict: '插件自报内存占用（字节；只观测不限制）',
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
      title: '自报自定义指标',
      depict: '插件自报一个自定义指标（名字 + 任意 JSON 值；只观测）',
    ),
  ];

  /// 全部广播声明（按声明顺序）
  static const List<MusicxxBroadcastDecl> broadcasts = <MusicxxBroadcastDecl>[
    MusicxxBroadcastDecl(
      id: 'musicxx.app.start',
      cache: 'none',
      publish: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: true,
      title: '应用启动',
      depict: '主程序启动阶段（业务 Store 与插件框架都就绪之后）派发一次',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.app.ready',
      cache: 'none',
      publish: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: true,
      title: '应用就绪',
      depict: '应用初始化收尾（首屏与数据都就绪）派发一次，比应用启动晚',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.app.background',
      cache: 'none',
      publish: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: true,
      title: '应用进入后台',
      depict: '应用进入后台（窗口隐藏 / 最小化，或移动到后台）时派发；观察型，不等待',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.app.foreground',
      cache: 'none',
      publish: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: true,
      title: '应用回到前台',
      depict: '应用回到前台时派发，带这次在后台待了多久；观察型，不等待',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.app.upgrade',
      cache: 'none',
      publish: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.app.lifecycle',
      cache: 'latest',
      publish: 'owner',
      pushOnSubscribe: true,
      coalesce: false,
      scope: 'app',
      type: 'string',
      options: <String>['foreground', 'background'],
      implemented: true,
      title: '前后台状态',
      depict: '应用现在在前台还是后台（变化时发布，订阅即拿到当前值）',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.player.source.resolved',
      cache: 'none',
      publish: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: true,
      title: '音源解析完成',
      depict: '某个音源解析完成（拿到真正播放用的路径与请求头）；载荷里没有直链与 token',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.player.state',
      cache: 'none',
      publish: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: true,
      title: '播放状态变化',
      depict: '播放状态变化（播放 / 暂停 / 停止 / 结束）时派发；播放进度不走这个埋点',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.player.position',
      cache: 'latest',
      publish: 'owner',
      pushOnSubscribe: true,
      coalesce: false,
      scope: 'app',
      type: 'json',
      options: <String>[],
      implemented: true,
      title: '播放进度',
      depict: '播放进度（不限频，按播放内核上报的节奏发布）；只读，改位置调用能力 musicxx.player.seek',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.player.completed',
      cache: 'none',
      publish: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: true,
      title: '一曲播放完成',
      depict: '一曲播放完成（正常结束或按完成处理）之后派发；不代表已经切歌',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.player.qualityChanged',
      cache: 'none',
      publish: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.song.changed',
      cache: 'none',
      publish: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: true,
      title: '切歌',
      depict: '切歌时派发（派发前调用方已刷新快照频道与封面）',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.song.meta.writeback',
      cache: 'none',
      publish: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.song.removed',
      cache: 'none',
      publish: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.history.record',
      cache: 'none',
      publish: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.local.scan.finished',
      cache: 'none',
      publish: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.lyric.current',
      cache: 'none',
      publish: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.media.cover.changed',
      cache: 'none',
      publish: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: true,
      title: '封面变化',
      depict: '当前歌曲的封面发生变化（换歌、换封面图、封面被清空）；载荷里没有直链与图片字节',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.net.lan.event',
      cache: 'none',
      publish: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.ui.page.enter',
      cache: 'none',
      publish: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.ui.page.leave',
      cache: 'none',
      publish: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.ui.theme.changed',
      cache: 'none',
      publish: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.ui.user.action',
      cache: 'none',
      publish: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: false,
      title: '',
      depict: '',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.ui.animatedLevel',
      cache: 'latest',
      publish: 'owner',
      pushOnSubscribe: true,
      coalesce: false,
      scope: 'app',
      type: 'string',
      options: <String>['undefined', 'disable', 'low', 'medium', 'high', 'max'],
      implemented: true,
      title: '动画等级',
      depict: '界面动画等级（越高动画越多）；只读，改等级调用能力 musicxx.ui.setAnimatedLevel',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.ui.particleAnimate',
      cache: 'latest',
      publish: 'owner',
      pushOnSubscribe: true,
      coalesce: false,
      scope: 'app',
      type: 'bool',
      options: <String>[],
      implemented: true,
      title: '粒子动画',
      depict: '点击与拖动时的粒子效果开关；只读，改开关调用能力 musicxx.ui.setParticleAnimate',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.ui.songIconRotate',
      cache: 'latest',
      publish: 'owner',
      pushOnSubscribe: true,
      coalesce: false,
      scope: 'app',
      type: 'bool',
      options: <String>[],
      implemented: true,
      title: '歌曲图旋转',
      depict: '播放时圆形歌曲图旋转的开关；只读，改开关调用能力 musicxx.ui.setSongIconRotate',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.ui.songIconWave',
      cache: 'latest',
      publish: 'owner',
      pushOnSubscribe: true,
      coalesce: false,
      scope: 'app',
      type: 'bool',
      options: <String>[],
      implemented: true,
      title: '歌曲图波浪',
      depict: '播放时歌曲图随节拍起伏的开关；只读，改开关调用能力 musicxx.ui.setSongIconWave',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.ui.fontFamily',
      cache: 'latest',
      publish: 'owner',
      pushOnSubscribe: true,
      coalesce: false,
      scope: 'app',
      type: 'string',
      options: <String>[],
      implemented: true,
      title: '全局字体',
      depict: '界面现在用的字体名（默认字体时是 default）；只读，切换字体调用能力 musicxx.ui.setFontFamily',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.theme.available',
      cache: 'latest',
      publish: 'owner',
      pushOnSubscribe: true,
      coalesce: false,
      scope: 'app',
      type: 'json',
      options: <String>[],
      implemented: true,
      title: '可用主题',
      depict: '主题目录（静态列表：id / 标题 / 明暗）',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.theme.current',
      cache: 'latest',
      publish: 'owner',
      pushOnSubscribe: true,
      coalesce: false,
      scope: 'app',
      type: 'string',
      options: <String>['mimicryLight', 'mimicryNight'],
      implemented: true,
      title: '当前主题',
      depict: '当前显示的主题 id；只读，切主题调用能力 musicxx.theme.setCurrent / musicxx.theme.tempSwitch',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.theme.mode',
      cache: 'latest',
      publish: 'owner',
      pushOnSubscribe: true,
      coalesce: false,
      scope: 'app',
      type: 'string',
      options: <String>['light', 'night', 'followSystem', 'followTime', 'followScreenBrightness'],
      implemented: true,
      title: '主题切换模式',
      depict: '跟随系统 / 跟随时间 / 跟随屏幕亮度，或固定白昼、夜间；只读，改模式调用能力 musicxx.theme.setMode',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.state.app',
      cache: 'latest',
      publish: 'owner',
      pushOnSubscribe: true,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: true,
      title: '应用状态',
      depict: '快照频道：属主发布，插件订阅即拿到当前值，也可以同步读缓存',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.state.player',
      cache: 'latest',
      publish: 'owner',
      pushOnSubscribe: true,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: true,
      title: '播放器状态',
      depict: '快照频道：属主发布，插件订阅即拿到当前值，也可以同步读缓存',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.state.song',
      cache: 'latest',
      publish: 'owner',
      pushOnSubscribe: true,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: true,
      title: '当前歌曲',
      depict: '快照频道：属主发布，插件订阅即拿到当前值，也可以同步读缓存',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.state.playlist',
      cache: 'latest',
      publish: 'owner',
      pushOnSubscribe: true,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: true,
      title: '当前播放列表',
      depict: '快照频道：属主发布，插件订阅即拿到当前值，也可以同步读缓存',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.state.lyric',
      cache: 'latest',
      publish: 'owner',
      pushOnSubscribe: true,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: true,
      title: '当前歌词',
      depict: '快照频道：属主发布，插件订阅即拿到当前值，也可以同步读缓存',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.state.playlists',
      cache: 'latest',
      publish: 'owner',
      pushOnSubscribe: true,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: true,
      title: '歌单列表快照',
      depict: '快照频道：属主发布，插件订阅即拿到当前值，也可以同步读缓存',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.state.env',
      cache: 'latest',
      publish: 'owner',
      pushOnSubscribe: true,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: true,
      title: '运行环境',
      depict: '快照频道：属主发布，插件订阅即拿到当前值，也可以同步读缓存',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.state.renderSlots',
      cache: 'latest',
      publish: 'owner',
      pushOnSubscribe: true,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: true,
      title: '渲染槽位状态',
      depict: '快照频道：属主发布，插件订阅即拿到当前值，也可以同步读缓存',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.state.spectrum',
      cache: 'latest',
      publish: 'owner',
      pushOnSubscribe: true,
      coalesce: true,
      scope: 'app',
      type: '',
      options: <String>[],
      implemented: true,
      title: '当前频谱',
      depict: '快照频道：属主发布，插件订阅即拿到当前值，也可以同步读缓存',
    ),
    MusicxxBroadcastDecl(
      id: 'musicxx.event.published',
      cache: 'none',
      publish: 'owner',
      pushOnSubscribe: false,
      coalesce: true,
      scope: 'system',
      type: '',
      options: <String>[],
      implemented: true,
      title: '事件发布观测',
      depict: '调试用：插件发布事件时推一份原始载荷',
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
