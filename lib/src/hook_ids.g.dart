// 自动生成（tools/gen_contract.dart ← tools/hooks.def.json）—— 请勿手改。
//
// 钩子 id 是跨边界稳定契约：字符串值一旦发布不得修改，
// 只能新增或标记废弃。字段含义见 tools/hooks.def.json。
// ignore_for_file: type=lint, constant_identifier_names

/// 钩子模式（与 C++ `MUSICXX_PLUGIN_HOOK_MODE_*` 一致）
enum MusicxxPluginHookMode {
  /// 观察型：不等待、不裁决（入队即返回）
  observe(0),

  /// 裁决型：等待处理器链结果（有等待预算）
  decision(1);

  const MusicxxPluginHookMode(this.code);

  final int code;
}

/// 多处理器裁决合并策略
enum MusicxxPluginDecisionPolicy {
  /// 首个非空裁决生效，后续处理器不再询问
  firstNonNull(0),

  /// 任一处理器 cancel/deny 即生效（最保守），patch 按优先级合并
  anyCancel(1),

  /// 所有 patch 依次深合并
  allMerge(2),

  /// 最后一个非空裁决生效
  lastWrite(3);

  const MusicxxPluginDecisionPolicy(this.code);

  final int code;
}

/// 派发方式
enum MusicxxPluginHookDispatch {
  /// 调用点就地等待裁决（`MusicxxPluginHooks.decide`；占用调用线程，有等待预算）
  sync(0),

  /// 不占用调用线程：观察型 = 入队即返回；裁决型 = 异步派发（`decideAsync`，
  /// 结果经 `musicxx.hook.decision.result` 事件回传）
  async(1);

  const MusicxxPluginHookDispatch(this.code);

  final int code;
}

/// 埋点可用性阶段
enum MusicxxPluginHookPhase { P0, P1, P2 }

/// 钩子 id 与元信息（由 `tools/hooks.def.json` 生成）
enum MusicxxPluginHookId {
  appStart(
    'musicxx.app.start',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P0,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  appReady(
    'musicxx.app.ready',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P0,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  appBackground(
    'musicxx.app.background',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  appForeground(
    'musicxx.app.foreground',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  appExit(
    'musicxx.app.exit',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.anyCancel,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.sync,
    30,
    100,
  ),
  appDeepLink(
    'musicxx.app.deepLink',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.anyCancel,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.sync,
    30,
    100,
  ),
  appUpgrade(
    'musicxx.app.upgrade',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),

  /// 调用点本身是 Future, 用异步派发避免卡住切歌 (切歌期间有新的请求时旧裁决丢弃)
  playerBeforePlaySong(
    'musicxx.player.beforePlaySong',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.anyCancel,
    MusicxxPluginHookPhase.P0,
    MusicxxPluginHookDispatch.async,
    30,
    100,
  ),
  playerSourceBeforeParse(
    'musicxx.player.source.beforeParse',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.firstNonNull,
    MusicxxPluginHookPhase.P0,
    MusicxxPluginHookDispatch.async,
    50,
    200,
  ),
  playerSourceResolved(
    'musicxx.player.source.resolved',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P0,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  playerBeforeOpen(
    'musicxx.player.beforeOpen',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.firstNonNull,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    50,
    200,
  ),
  playerState(
    'musicxx.player.state',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P0,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  playerPosition(
    'musicxx.player.position',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  playerSeek(
    'musicxx.player.seek',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.anyCancel,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.sync,
    30,
    100,
  ),
  playerVolume(
    'musicxx.player.volume',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.anyCancel,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.sync,
    30,
    100,
  ),
  playerSpeed(
    'musicxx.player.speed',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.anyCancel,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.sync,
    30,
    100,
  ),
  playerPitch(
    'musicxx.player.pitch',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.anyCancel,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.sync,
    30,
    100,
  ),
  playerError(
    'musicxx.player.error',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.anyCancel,
    MusicxxPluginHookPhase.P0,
    MusicxxPluginHookDispatch.sync,
    30,
    100,
  ),
  playerCompleted(
    'musicxx.player.completed',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P0,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  playerQualityChanged(
    'musicxx.player.qualityChanged',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  mediaNotification(
    'musicxx.media.notification',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.sync,
    30,
    100,
  ),
  songChanged(
    'musicxx.song.changed',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P0,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  songInfoAnalyse(
    'musicxx.song.info.analyse',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.firstNonNull,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.sync,
    30,
    100,
  ),
  songMetaWriteback(
    'musicxx.song.meta.writeback',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  songBeforeAdd(
    'musicxx.song.beforeAdd',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.anyCancel,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.sync,
    30,
    100,
  ),
  songRemoved(
    'musicxx.song.removed',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  playlistLoaded(
    'musicxx.playlist.loaded',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  playlistFilter(
    'musicxx.playlist.filter',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.allMerge,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    50,
    200,
  ),
  songlistFilter(
    'musicxx.songlist.filter',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.allMerge,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    50,
    200,
  ),
  historyRecord(
    'musicxx.history.record',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  localScanFile(
    'musicxx.local.scan.file',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.anyCancel,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.sync,
    30,
    100,
  ),
  localScanFinished(
    'musicxx.local.scan.finished',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  lyricLoadBefore(
    'musicxx.lyric.load.before',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.firstNonNull,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    50,
    200,
  ),
  lyricLoaded(
    'musicxx.lyric.loaded',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  lyricTransform(
    'musicxx.lyric.transform',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.allMerge,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    50,
    200,
  ),
  lyricCurrent(
    'musicxx.lyric.current',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  lyricSearchStr(
    'musicxx.lyric.searchStr',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.firstNonNull,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.sync,
    30,
    100,
  ),
  lyricProvider(
    'musicxx.lyric.provider',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.firstNonNull,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    50,
    200,
  ),
  iconRequest(
    'musicxx.icon.request',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.firstNonNull,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    50,
    200,
  ),
  iconGenerated(
    'musicxx.icon.generated',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  mediaInfoRequest(
    'musicxx.media.info.request',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.firstNonNull,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    50,
    200,
  ),
  mediaWaveReady(
    'musicxx.media.wave.ready',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  mediaChorusAnalysed(
    'musicxx.media.chorus.analysed',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  netRequestBefore(
    'musicxx.net.request.before',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.anyCancel,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    50,
    200,
  ),
  netResponseAfter(
    'musicxx.net.response.after',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  netServerRoute(
    'musicxx.net.server.route',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.firstNonNull,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    100,
    500,
  ),
  netMcpTools(
    'musicxx.net.mcp.tools',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.allMerge,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.sync,
    50,
    200,
  ),
  netLanEvent(
    'musicxx.net.lan.event',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  downloadBefore(
    'musicxx.download.before',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.anyCancel,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    50,
    200,
  ),
  downloadCompleted(
    'musicxx.download.completed',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  cacheBeforeTrim(
    'musicxx.cache.beforeTrim',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.anyCancel,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.sync,
    30,
    100,
  ),
  cachePathRequest(
    'musicxx.cache.pathRequest',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.firstNonNull,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.sync,
    30,
    100,
  ),
  uiHomeEntries(
    'musicxx.ui.home.entries',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.allMerge,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.sync,
    30,
    100,
  ),
  uiSongActions(
    'musicxx.ui.song.actions',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.allMerge,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.sync,
    30,
    100,
  ),
  uiPlaylistActions(
    'musicxx.ui.playlist.actions',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.allMerge,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.sync,
    30,
    100,
  ),
  uiRouteResolve(
    'musicxx.ui.route.resolve',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.firstNonNull,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.sync,
    50,
    200,
  ),
  uiPageEnter(
    'musicxx.ui.page.enter',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  uiPageLeave(
    'musicxx.ui.page.leave',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  uiThemeChanged(
    'musicxx.ui.theme.changed',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  uiNotify(
    'musicxx.ui.notify',
    MusicxxPluginHookMode.decision,
    MusicxxPluginDecisionPolicy.anyCancel,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.sync,
    30,
    100,
  ),
  uiUserAction(
    'musicxx.ui.user.action',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  scriptBound(
    'musicxx.script.bound',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  scriptDisposed(
    'musicxx.script.disposed',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  scriptActionExecuted(
    'musicxx.script.action.executed',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  clockingTick(
    'musicxx.clocking.tick',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  ),
  listenTogetherEvent(
    'musicxx.listenTogether.event',
    MusicxxPluginHookMode.observe,
    MusicxxPluginDecisionPolicy.lastWrite,
    MusicxxPluginHookPhase.P1,
    MusicxxPluginHookDispatch.async,
    0,
    0,
  );

  const MusicxxPluginHookId(
    this.id,
    this.mode,
    this.policy,
    this.phase,
    this.dispatch,
    this.budgetMs,
    this.hardMs,
  );

  /// 跨边界稳定 id（`musicxx.<域>.<名>`）
  final String id;

  /// 观察型 / 裁决型
  final MusicxxPluginHookMode mode;

  /// 裁决合并策略（观察型无意义）
  final MusicxxPluginDecisionPolicy policy;

  /// 埋点阶段
  final MusicxxPluginHookPhase phase;

  /// 派发方式（是否占用调用线程；见 `tools/hooks.def.json`）
  final MusicxxPluginHookDispatch dispatch;

  /// 整链软等待预算（毫秒；0 = 用宿主默认）
  final int budgetMs;

  /// 整链硬等待预算（毫秒；超过只记统计，不打断插件）
  final int hardMs;

  /// 是否裁决型（观察型不参与合并）
  bool get isDecision => mode == MusicxxPluginHookMode.decision;

  /// 是否为异步派发（观察型恒为 true；裁决型表示调用点用 `decideAsync`）
  bool get isAsyncDispatch => dispatch == MusicxxPluginHookDispatch.async;

  /// 整链等待预算上限（毫秒；异步派发的兜底等待按此计算）
  int get budgetLimitMs => budgetMs + hardMs;

  /// 按 id 反查（未知 id 返回 null；插件注册未知钩子会被宿主拒绝）
  static MusicxxPluginHookId? tryFromId(String id) => _byId[id];

  static final Map<String, MusicxxPluginHookId> _byId = {
    for (final MusicxxPluginHookId hook in values) hook.id: hook,
  };
}
