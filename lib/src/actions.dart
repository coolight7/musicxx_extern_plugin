import 'dart:async';
import 'dart:convert';
import 'dart:ffi';

import 'bindings_generated.dart';
import 'events.dart';
import 'native_strings.dart';
import 'runtime.dart';

/// 主程序动作处理器：接收插件参数，返回结果（JSON 可序列化）
typedef MusicxxPluginActionHandler =
    Object? Function(MusicxxPluginActionInvocation invocation);

/// 一次动作请求（插件 → Dart）
class MusicxxPluginActionInvocation {
  const MusicxxPluginActionInvocation({
    required this.requestId,
    required this.plugin,
    required this.action,
    required this.args,
    required this.extendDeadline,
  });

  /// 请求 id（回复时原样带回）
  final int requestId;

  /// 发起插件（可能是 `js:<id>` 的 id 形式）
  final String plugin;

  /// 动作全名（`musicxx.<域>.<动作>`）
  final String action;

  /// 参数
  final Map<String, Object?> args;

  /// 延长本次请求的超时（长任务用；主程序下限 1 s）—— 需由主程序确认，v1 仅记录
  final void Function(Duration extra) extendDeadline;

  @override
  String toString() =>
      'MusicxxPluginActionInvocation($plugin → $action #$requestId)';
}

/// 主程序动作注册表
///
/// 插件经 `musicxx.host.request_action` 发起动作；Dart 侧在 [register] 注册实现。
/// **未注册的动作会立即收到"未找到"**，避免插件 op 一直挂到超时。
class MusicxxPluginActions {
  MusicxxPluginActions.internal(this._runtime);

  final MusicxxPluginRuntime _runtime;

  final Map<String, MusicxxPluginActionHandler> _handlers =
      <String, MusicxxPluginActionHandler>{};

  /// 已注册动作名
  Set<String> get registeredActions => Set<String>.unmodifiable(_handlers.keys);

  /// 还没回复的异步动作数量（异步处理器返回 Future 时记一笔）
  ///
  /// 给"兜底轮询要不要跑"用：这些回复还在等结果，事件回传不能漏（见 runtime 的定时器）。
  int get pendingResponses => _pendingResponses;
  int _pendingResponses = 0;

  /// 注册动作实现（同名覆盖）
  void register(String action, MusicxxPluginActionHandler handler) {
    _handlers[action] = handler;
  }

  /// 注销动作
  void unregister(String action) {
    _handlers.remove(action);
  }

  /// 主动回复某个请求（一般由内部调用；长任务可先记录 requestId 再异步回复）
  void respond(int requestId, {Object? result, int status = 0, String? error}) {
    if (!_runtime.isRunning) {
      return;
    }
    final String payload = result == null
        ? (error == null ? '' : _errorJson(error))
        : _encode(result);
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings.musicxx_extern_plugin_action_respond(
        _runtime.host,
        requestId,
        status,
        arena.view(payload),
        log,
      );
      if (rc != 0) {
        _runtime.log(3, 'action_respond(#$requestId) 失败(code=$rc)');
      }
    } finally {
      arena.dispose();
    }
  }

  /// 直接执行一个已注册的主程序动作（**主程序 UI 触发**；不经插件 op 通道）
  ///
  /// 用途：插件用声明式 UI 声明 `{"kind":"action","name":"musicxx.player.toggle"}`，
  /// 用户点击后由主程序直接调用该动作实现（与插件发起动作走同一份实现与权限规则）。
  /// - `pluginId`：调用上下文（权限判定与插件私有存储定位用；可为空）；
  /// - 返回动作结果；动作未注册返回 `null`（调用方可据此提示）。
  Future<Object?> invoke(
    String action, {
    String pluginId = '',
    Map<String, Object?> args = const <String, Object?>{},
  }) async {
    final MusicxxPluginActionHandler? handler = _handlers[action];
    if (handler == null) {
      return null;
    }
    final MusicxxPluginActionInvocation invocation =
        MusicxxPluginActionInvocation(
          requestId: 0,
          plugin: pluginId,
          action: action,
          args: args,
          extendDeadline: (Duration _) {},
        );
    try {
      final result = handler(invocation);
      if (result is Future) {
        return await result;
      }
      return result;
    } catch (error) {
      return <String, Object?>{'ok': false, 'error': error.toString()};
    }
  }

  // ==================== 内部 ====================

  /// `musicxx.action.request` 事件 → 同步调用 Dart 处理器并回复
  ///
  /// 说明：v1 在事件泵里同步执行处理器（处理器应快速返回；耗时工作请自行异步化并在完成后
  /// 调用 [respond]）。这样插件侧 op 能在第一时间拿到结果，而不是等超时。
  void handleActionRequest(MusicxxPluginEvent event) {
    final int requestId = event.intOf('requestId') ?? 0;
    if (requestId == 0) {
      return;
    }
    final String action = event.stringOf('action') ?? '';
    final rawArgs = event.payload['args'];
    final Map<String, Object?> args = rawArgs is Map<String, Object?>
        ? rawArgs
        : (rawArgs is Map<Object?, Object?>
              ? rawArgs.cast<String, Object?>()
              : const <String, Object?>{});
    final MusicxxPluginActionHandler? handler = _handlers[action];
    if (handler == null) {
      respond(requestId, status: -4, error: 'action_not_registered: $action');
      return;
    }
    final MusicxxPluginActionInvocation invocation =
        MusicxxPluginActionInvocation(
          requestId: requestId,
          plugin: event.plugin,
          action: action,
          args: args,
          extendDeadline: (Duration _) {},
        );
    try {
      final result = handler(invocation);
      if (result is Future) {
        // 异步处理器：先说明"已受理"，完成后再由处理器自行 respond（或返回未来值）
        ++_pendingResponses;
        result.then<void>(
          (Object? value) {
            --_pendingResponses;
            respond(requestId, result: value);
          },
          onError: (Object error) {
            --_pendingResponses;
            respond(requestId, status: -99, error: error.toString());
          },
        );
        return;
      }
      respond(requestId, result: result);
    } catch (error) {
      respond(requestId, status: -99, error: error.toString());
    }
  }

  /// `musicxx.action.cancel`（插件取消 / 主程序超时）：通知已注册的处理器
  void handleActionCancel(MusicxxPluginEvent event) {
    // v1 只记一条日志；需要中断的处理器的实现可以订阅事件流自行收尾（取消令牌尚未接入）
    _runtime.log(
      3,
      '动作请求被取消: #${event.intOf('requestId')} (${event.stringOf('reason') ?? ''})',
    );
  }

  /// 主程序收尾：没回复完的异步动作不再有人接，计数归零
  void handleDisposed() {
    _pendingResponses = 0;
  }

  static String _encode(Object? value) => jsonEncode(value);

  static String _errorJson(String message) =>
      jsonEncode(<String, Object?>{'error': message});
}

/// 动作名常量（主程序在 Store 里注册对应实现；这里只列名字）
abstract final class MusicxxPluginActionNames {
  /// 切到前台（把主窗口显示出来）
  ///
  /// 与命令行 `window.foreground` 是同一份实现（`GlobalStore.appForeground`）。
  /// 切完会派发通知型钩子 `musicxx.app.foreground`，并刷新状态镜像 `musicxx.state.app`。
  static const String appForeground = 'musicxx.app.foreground';

  /// 切到后台（隐藏主窗口，播放不受影响）
  ///
  /// 与命令行 `window.background` 是同一份实现（`GlobalStore.appBackground`）。
  /// 切完会派发通知型钩子 `musicxx.app.background`，并刷新状态镜像 `musicxx.state.app`。
  static const String appBackground = 'musicxx.app.background';

  static const String playerPlay = 'musicxx.player.play';
  static const String playerPause = 'musicxx.player.pause';
  static const String playerToggle = 'musicxx.player.toggle';
  static const String playerStop = 'musicxx.player.stop';
  static const String playerNext = 'musicxx.player.next';
  static const String playerPrev = 'musicxx.player.prev';
  /// 按来源播放（本地路径 / `content://` / 文件链接 / 拟声音源字符串）。
  /// 和 applink（`musicxx://play/`）、启动参数、命令行、MCP 是同一个入口，
  /// 参数名也一致：`src` / `sources` / `type` / `name`。
  static const String playerPlaySrc = 'musicxx.player.playSrc';
  static const String playerSeek = 'musicxx.player.seek';
  static const String playerSetVolume = 'musicxx.player.setVolume';
  static const String playerSetSpeed = 'musicxx.player.setSpeed';
  static const String playerSetPitch = 'musicxx.player.setPitch';
  static const String playerSetQuality = 'musicxx.player.setQuality';
  static const String playerSetLoopMode = 'musicxx.player.setLoopMode';
  static const String playerSetMediaType = 'musicxx.player.setMediaType';

  static const String libraryQuerySongs = 'musicxx.library.querySongs';
  static const String libraryQuerySonglists = 'musicxx.library.querySonglists';
  static const String libraryQuerySonglist = 'musicxx.library.querySonglist';
  static const String libraryPlaySong = 'musicxx.library.playSong';
  static const String libraryPlaySonglist = 'musicxx.library.playSonglist';
  static const String libraryRemoveSong = 'musicxx.library.removeSong';

  /// 把歌曲加进**一张歌单**（`playlist` = 单张歌单，不是播放队列）
  ///
  /// 参数：`pid`（必填）、`dedup`（缺省 true）、`songs`（1 ~ 200 首，或单首写成 `song`）。
  /// 每首歌：`type`（插件自己的音源类型 `plugin.<插件id>.<类型名>`）、`src`、`name?` /
  /// `artist?`（空则按 `fileName` 交给主程序识别）、`fileName?`、`size?`、`durationMs?`、
  /// `cover?`（本地路径或 `http(s)` 地址）。
  ///
  /// 返回：`{ok, pid, name, total, added, skipped}`；失败是 `{ok:false, error}`。
  /// 与 `musicxx.library.*` 的区别：那一组是“读歌单 / 读歌曲 / 播歌”，这一条是“改歌单内容”，
  /// 所以用 `playlist`（单张歌单）。播放队列在文档里一律叫 `songlist`。
  static const String playlistAddSong = 'musicxx.playlist.addSong';
  static const String libraryCreateSonglist = 'musicxx.library.createSonglist';
  static const String librarySetSongInfo = 'musicxx.library.setSongInfo';
  static const String librarySearch = 'musicxx.library.search';

  static const String lyricGetCurrent = 'musicxx.lyrics.getCurrent';
  static const String lyricGetBySrc = 'musicxx.lyrics.getBySrc';
  static const String lyricSet = 'musicxx.lyrics.set';
  static const String lyricSync = 'musicxx.lyrics.sync';
  static const String lyricSearch = 'musicxx.lyrics.search';

  static const String uiNotify = 'musicxx.ui.notify';
  static const String uiToast = 'musicxx.ui.toast';
  static const String uiDialog = 'musicxx.ui.dialog';

  /// 打开一层浮层（sheet），等用户关掉后返回结果
  ///
  /// 参数：`preset`（`custom` 缺省 / `picker`）、`id`、`title`、`depict`、`blocks`（`custom` 的内容）、
  /// `options`（`picker` 的候选项）、`value`（`picker` 的当前值）、`textConfirm` / `textCancel`、
  /// `size`（尺寸预设）、`dismissible`（缺省 true）。
  ///
  /// 返回：`{ok, reason, value?}`（`reason` 是 `confirmed` / `cancelled` / `closed`）；
  /// `preset: "picker"` 时是 `{ok, selected}` 或 `{ok, cancelled: true}`。
  static const String uiOverlayShow = 'musicxx.ui.overlay.show';

  /// 收起这个插件当前那一层浮层（参数 `id` 可选：只关名字对得上的那一层）
  ///
  /// 返回 `{ok, closed}`；没有开着的浮层时 `closed` 为 false。
  static const String uiOverlayClose = 'musicxx.ui.overlay.close';

  /// 打开一层“插件视图”浮层（底部弹层），等用户关掉后返回
  ///
  /// 与 `musicxx.ui.overlay.show` 的区别：`show` 的内容是调用方给的 `blocks`（静态描述），
  /// 这一条的内容是**插件自己的视图**，所以能在浮层里翻目录 / 改选择（浮层里的动作返回
  /// `{"view": …}` 就刷新内容）。
  ///
  /// 参数：`route`（必填，`ext://<插件id>/<视图id>`）、`title?`、`arguments?`（原样交给插件能力；
  /// 不给时把除 `route` / `title` / `size` / `dismissible` / `textConfirm` / `textCancel`
  /// 之外的键当参数）。
  /// 返回：`{ok, reason, title?}`（`reason` 是 `confirmed` / `cancelled` / `closed`）。
  ///
  /// 动作**在浮层关掉之后才完成**：调用方正好能在关闭后刷新自己的列表。
  /// 插件在能力处理器里调它时**不要 await**（会与主程序动作互相等）。
  static const String uiOpenOverlaySheet = 'musicxx.ui.openOverlaySheet';
  static const String uiOpenRoute = 'musicxx.ui.openRoute';

  /// 让主程序打开「选择歌曲」弹窗（挑自己这个音源里的歌，选完直接加进歌单）
  ///
  /// 前提是先声明 `musicxx.source.browse` 并实现 `drive.list` 能力。参数：`type`（必填，
  /// 自己声明的音源类型 id）、`title?`、`path?`（起始目录，缺省用声明里的 `root`）、
  /// `pid?`（目标歌单，不给就是当前播放列表）、`multiple?`（默认 true）、
  /// `allowSelectDir?`（默认 true）、`search?`、`pageSize?`。
  /// 返回：`{ok, added, skipped, cancelled}`。
  ///
  /// 动作**在弹窗关掉之后才完成**：插件在能力处理器里调它时**不要 await**
  /// （会与主程序动作互相等，与 `musicxx.ui.openOverlaySheet` 同理）。
  static const String uiPickSongs = 'musicxx.ui.pickSongs';

  /// 让主程序打开「选择文件夹」弹窗（上传目标、保存位置这类场景）
  ///
  /// 参数与 [uiPickSongs] 基本相同，`multiple` 缺省 false。返回 `{ok, paths, cancelled}`。
  static const String uiPickDir = 'musicxx.ui.pickDir';

  /// 打开某个浏览来源的**整页列表**（与内置网盘页同一套界面）
  ///
  /// 参数：`type`（必填）、`path?`（起始目录）、`pid?`（目标歌单，缺省当前播放列表）。
  /// 返回 `{ok, reason, added}`；`reason` 是 `closed` / `confirmed`。
  static const String uiBrowseDrive = 'musicxx.ui.browseDrive';
  static const String uiSetEntryBadge = 'musicxx.ui.setEntryBadge';
  static const String uiSetFontFamily = 'musicxx.ui.setFontFamily';
  static const String uiGetFontFamily = 'musicxx.ui.getFontFamily';
  static const String uiLoadFont = 'musicxx.ui.loadFont';

  static const String storageGet = 'musicxx.storage.get';
  static const String storageSet = 'musicxx.storage.set';
  static const String storageDelete = 'musicxx.storage.delete';
  static const String storageList = 'musicxx.storage.list';

  static const String netFetch = 'musicxx.net.fetch';
  static const String netDownload = 'musicxx.net.download';

  static const String renderList = 'musicxx.render.list';
  static const String renderCurrent = 'musicxx.render.current';
  static const String renderSelect = 'musicxx.render.select';

  static const String mediaPalette = 'musicxx.media.palette';
  static const String mediaCover = 'musicxx.media.cover';

  /// 写封面取色（与内置分析写入同一批字段，之后 `musicxx.media.palette` 读到的就是它）
  ///
  /// 参数：`main` / `light` / `lightMuted` / `dark` / `darkMuted`（`#rrggbb` 或 `#aarrggbb`），
  /// 可选 `dominant`（数组）。没给的键保持原样。
  static const String mediaPaletteSet = 'musicxx.media.palette.set';

  /// 把插件自己准备的图片绑定成纹理（渲染项用 `image` 声明引用）
  ///
  /// 参数：`key`（本地名字，渲染项里引用它）、`data`（base64）、
  /// `format`（`png` / `jpeg` / `rgba`；`rgba` 需要 `width` / `height`）。
  static const String mediaBindImage = 'musicxx.media.bindImage';

  /// 释放之前绑定的图片纹理（参数 `key`）
  static const String mediaUnbindImage = 'musicxx.media.unbindImage';

  /// 当前音频频谱（内置『音乐动效』插件提取的数据）
  ///
  /// 参数：`bandCount`（1~64，必须整除 256，缺省 16）、`unit`（`normalized` 缺省 /
  /// `db` / `raw`）、`includeBins`（附带当前帧的 256 个频点）。
  /// 没有数据不是错误：结果里用 `status` / `available` / `loading` / `reason` 表达。
  static const String mediaSpectrum = 'musicxx.media.spectrum';

  /// 应用能力导出（见 manager/featureExport/）：
  /// - `list` 列出全部能力的声明（id/标题/说明/参数/输出形态）；
  /// - `call` 调一个能力（`{id, args}`），与 MCP 工具共用同一份实现。
  static const String featureList = 'musicxx.feature.list';
  static const String featureCall = 'musicxx.feature.call';

  /// 列出功能埋点与各自的实现（谁在提供这个功能、排第几、谁把功能关掉了）
  ///
  /// 与命令行 `featurePoint.list`、MCP 工具 `GetFeatureProviders` 共用同一份自省数据。
  /// 参数：`id`（只看一个埋点）、`provided`（只看有实现的埋点）。
  static const String featurePointList = 'musicxx.feature.point.list';

  /// 调一个功能点并拿到结果（应用留出的埋点，或插件自己声明的功能点）
  ///
  /// 参数：
  /// - `id`：功能点 id（应用埋点如 `musicxx.media.spectrum.extract`，
  ///   插件点如 `plugin.<插件id>.<名字>`）；
  /// - `args`：给这个功能点的参数（对象，可省；每个功能点的参数见 `list` 里的 `callDoc`）；
  /// - `refresh`：布尔，可省。true = 跳过值仓重新算一遍。
  ///
  /// 返回 `{ok, id, value, by, identity, fromCache, ms}`；失败是
  /// `{ok:false, error, message}`，`error` 取 `not_callable` / `bad_args` / `no_impl` /
  /// `disabled` / `busy` / `failed`。
  ///
  /// **调用零副作用**：只读值仓、不写值仓、不改应用状态 —— 插件调一次封面取色
  /// 不会换掉用户看到的背景色（要改观感请看 `musicxx.media.palette.set`）。
  /// 同一功能点 + 同一调用方的上一次调用没结束时返回 `busy`（不排队）。
  static const String featurePointCall = 'musicxx.feature.point.call';

  /// 声明一个**自己的**功能点（id 必须形如 `plugin.<自己的插件 id>.<名字>`）
  ///
  /// 插件声明的功能点共用**一条**通用通道钩子（`musicxx.feature.point.call`），而不是每个点
  /// 各占一个钩子 id（插件框架只认固定的钩子表）。声明只描述"我提供什么功能"：
  /// 参数 `id`（必填）、`title`（展示名）、`depict`（一句话说明）、`args`（参数说明）、
  /// `result`（结果说明）。同一 id 重复声明 = 覆盖（最后一次生效）。
  /// 声明跟着插件生命周期走（禁用 / 卸载自动撤销），不持久化；重新启用时脚本会再声明一遍。
  /// 声明之后，别的插件与应用都能用 `musicxx.feature.point.call` 调它（`id` 就是这个点）。
  static const String featurePointDefine = 'musicxx.feature.point.define';

  /// 收回自己声明的功能点（参数 `id`）
  ///
  /// 一般不用调：插件被禁用 / 卸载时插件框架会撤销它的全部声明。
  static const String featurePointUndefine = 'musicxx.feature.point.undefine';

  /// 声明一个**自己的音源类型**（id 必须形如 `plugin.<自己的插件 id>.<类型名>`）
  ///
  /// 音源地址写成 `plugin.<自己的插件 id>.<类型名>://<自己的标识>`：主程序内置实现不解析这种
  /// 地址，播放时问钩子 `musicxx.player.source.resolve` 的处理器（注册处理器就是"提供解析"）。
  /// 参数 `id`（必填，与地址的 scheme 一致）、`title`（展示名）、`depict`（一句话说明）、
  /// `srcDoc`（地址里写什么）。声明只用于自省与更清楚的报错：**解析能不能成看有没有注册解析
  /// 处理器**。同一 id 重复声明 = 覆盖（最后一次生效）；声明跟着插件生命周期走
  /// （禁用 / 卸载自动撤销），不持久化。
  static const String sourceDefine = 'musicxx.source.define';

  /// 收回自己声明的音源类型（参数 `id`）
  ///
  /// 一般不用调：插件被禁用 / 卸载时插件框架会撤销它的全部声明。
  static const String sourceUndefine = 'musicxx.source.undefine';

  /// 列出插件音源类型与音源解析链（自省）
  ///
  /// 返回 `{sources, resolveChain}`：`sources` = 各插件声明的音源类型，
  /// `resolveChain` = 音源解析链现在有谁在提供（与 `musicxx.feature.point.list`
  /// 看 `musicxx.player.source.resolve` 是同一份数据）。
  static const String sourceList = 'musicxx.source.list';

  /// 声明**这个音源类型可以浏览并挑歌**（目录列表、搜索、写操作都走这里）
  ///
  /// 与 `musicxx.source.define` 分开：`define` 是"这个类型能解析播放"，`browse` 是"这个类型
  /// 能浏览并挑歌"。一个类型可以只有前者（例如只做播放代理），也可以两者都有。
  ///
  /// 参数：`id`（必填，必须是自己命名空间下的音源类型 id）、`title`（菜单标题）、
  /// `root`（根目录，缺省 `/`）、`icon?`（插件目录里的 svg 路径）、`write?`（是否允许写）、
  /// `search?`（是否支持搜索）、`capabilities?`（覆盖默认能力名，键是 `list` /
  /// `resolveSongs` / `search` / `mkdir` / `rename` / `remove` / `info` / `open`）。
  /// 声明跟着插件生命周期走（禁用 / 卸载自动撤销），不持久化。
  ///
  /// 声明后插件自动出现在「添加歌曲」菜单里（一行，点开就是内置风格的挑歌弹窗），
  /// 数据由 `drive.list` 能力提供。
  static const String sourceBrowse = 'musicxx.source.browse';

  /// 读某个钩子当前的处理器列表（自省：谁注册了、什么形态、排第几）
  ///
  /// 参数：`hook`（钩子 id，如 `musicxx.media.spectrum.extract`）。
  /// 返回 `{hook, count, handlers, listAvailable}`；`listAvailable=false` 表示插件框架库太旧，
  /// 只能报处理器数量（此时看 `count`）。
  static const String hookHandlers = 'musicxx.hook.handlers';

  static const String hostOpenUrl = 'musicxx.host.openUrl';
  static const String hostShareText = 'musicxx.host.shareText';
  static const String hostClipboard = 'musicxx.host.clipboard';
  static const String hostGetPath = 'musicxx.host.getPath';

  static const String statsReportMemory = 'musicxx.stats.reportMemory';
  static const String statsReportMetric = 'musicxx.stats.reportMetric';
}
