import 'dart:async';
import 'dart:convert';
import 'dart:ffi';

import 'bindings_generated.dart';
import 'events.dart';
import 'native_strings.dart';
import 'runtime.dart';

/// 宿主动作处理器：接收插件参数，返回结果（JSON 可序列化）
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

  /// 延长本次请求的超时（长任务用；宿主下限 1 s）—— 需由宿主确认，v1 仅记录
  final void Function(Duration extra) extendDeadline;

  @override
  String toString() =>
      'MusicxxPluginActionInvocation($plugin → $action #$requestId)';
}

/// 宿主动作注册表
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

  /// 直接执行一个已注册的宿主动作（**应用侧 UI 触发**；不经插件 op 通道）
  ///
  /// 用途：插件用声明式 UI 声明 `{"kind":"action","name":"musicxx.player.toggle"}`，
  /// 用户点击后由应用直接调用该动作实现（与插件发起动作走同一份实现与权限规则）。
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
      final Object? result = handler(invocation);
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
    final Object? rawArgs = event.payload['args'];
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
      final Object? result = handler(invocation);
      if (result is Future) {
        // 异步处理器：先说明"已受理"，完成后再由处理器自行 respond（或返回未来值）
        result.then<void>(
          (Object? value) => respond(requestId, result: value),
          onError: (Object error) =>
              respond(requestId, status: -99, error: error.toString()),
        );
        return;
      }
      respond(requestId, result: result);
    } catch (error) {
      respond(requestId, status: -99, error: error.toString());
    }
  }

  /// `musicxx.action.cancel`（插件取消 / 宿主超时）：通知已注册的处理器
  void handleActionCancel(MusicxxPluginEvent event) {
    // v1：仅记录；处理器可订阅事件流自行收尾（S4 接入取消令牌）
    _runtime.log(
      3,
      '动作请求被取消: #${event.intOf('requestId')} (${event.stringOf('reason') ?? ''})',
    );
  }

  static String _encode(Object? value) => jsonEncode(value);

  static String _errorJson(String message) =>
      jsonEncode(<String, Object?>{'error': message});
}

/// 已注册动作的默认实现（音乐应用侧在 S4 接入 Store；这里给出可用骨架）
abstract final class MusicxxPluginActionNames {
  static const String playerPlay = 'musicxx.player.play';
  static const String playerPause = 'musicxx.player.pause';
  static const String playerToggle = 'musicxx.player.toggle';
  static const String playerStop = 'musicxx.player.stop';
  static const String playerNext = 'musicxx.player.next';
  static const String playerPrev = 'musicxx.player.prev';
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
  static const String libraryAddSong = 'musicxx.library.addSong';
  static const String libraryRemoveSong = 'musicxx.library.removeSong';
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
  static const String uiOpenRoute = 'musicxx.ui.openRoute';
  static const String uiSetEntryBadge = 'musicxx.ui.setEntryBadge';

  static const String storageGet = 'musicxx.storage.get';
  static const String storageSet = 'musicxx.storage.set';
  static const String storageDelete = 'musicxx.storage.delete';
  static const String storageList = 'musicxx.storage.list';

  static const String netFetch = 'musicxx.net.fetch';
  static const String netDownload = 'musicxx.net.download';

  static const String hostOpenUrl = 'musicxx.host.openUrl';
  static const String hostShareText = 'musicxx.host.shareText';
  static const String hostClipboard = 'musicxx.host.clipboard';
  static const String hostGetPath = 'musicxx.host.getPath';

  static const String statsReportMemory = 'musicxx.stats.reportMemory';
  static const String statsReportMetric = 'musicxx.stats.reportMetric';
}
