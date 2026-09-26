import 'dart:convert';
import 'dart:ffi';

import 'bindings_generated.dart';
import 'native_strings.dart';
import 'runtime.dart';

/// 状态镜像推送（Dart → 原生，供插件**同步**读取）
///
/// 语义：
/// - 值是 JSON；单键上限 512 KiB，超限由宿主**直接拒绝写入**（[update] 返回 false 并记日志）；
/// - 建议使用 [standardKeys] 里的标准键，插件按同一批键读取；
/// - 高频键（播放进度）请用 [updateThrottled] 或自行 1 Hz 节流；
/// - 变化会异步通知声明关心的插件（`musicxx.state.changed`）。
class MusicxxPluginState {
  MusicxxPluginState.internal(this._runtime);

  final MusicxxPluginRuntime _runtime;

  /// 官方状态镜像键（与 `musicxx.state.*` 常量一致）
  static const String keyApp = 'musicxx.state.app';
  static const String keyPlayer = 'musicxx.state.player';
  static const String keySong = 'musicxx.state.song';
  static const String keyPlaylist = 'musicxx.state.playlist';
  static const String keyLyric = 'musicxx.state.lyric';
  static const String keyLibrary = 'musicxx.state.library';
  static const String keyEnv = 'musicxx.state.env';

  /// 渲染槽位状态（哪个插件样式正在画、是否可见、尺寸等）
  ///
  /// 不属于 [standardKeys]：宿主在生效/可见性/昼夜/尺寸变化时按需推送。
  static const String keyRenderSlots = 'musicxx.state.renderSlots';

  /// 全部标准键（启动时一次推送）
  static const List<String> standardKeys = <String>[
    keyApp,
    keyPlayer,
    keySong,
    keyPlaylist,
    keyLyric,
    keyLibrary,
    keyEnv,
  ];

  final Map<String, String> _lastPushed = <String, String>{};
  final Map<String, int> _lastPushedMs = <String, int>{};

  /// 推送单个键（值会被序列化为 JSON；与上次相同则跳过，减少 FFI 与唤醒）
  ///
  /// 返回是否真的推送了。
  bool update(String key, Object? value, {bool skipIfUnchanged = true}) {
    if (!_runtime.isRunning) {
      return false;
    }
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final String json = value == null ? 'null' : _encode(value);
      if (skipIfUnchanged && _lastPushed[key] == json) {
        return false;
      }
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings.musicxx_extern_plugin_state_update(
        _runtime.host,
        arena.view(key),
        arena.view(json),
        log,
      );
      if (rc != 0) {
        _runtime.log(
          3,
          'state_update($key) 失败(code=$rc): ${takeOutString(log, _runtime.bindings)}',
        );
        return false;
      }
      _lastPushed[key] = json;
      _lastPushedMs[key] = DateTime.now().millisecondsSinceEpoch;
      return true;
    } finally {
      arena.dispose();
    }
  }

  /// 节流推送（高频键：同一键在 [minIntervalMs] 内只推最后一次）
  bool updateThrottled(String key, Object? value, {int minIntervalMs = 1000}) {
    final int now = DateTime.now().millisecondsSinceEpoch;
    final int? last = _lastPushedMs[key];
    if (last != null && now - last < minIntervalMs) {
      return false;
    }
    return update(key, value, skipIfUnchanged: true);
  }

  /// 批量推送（同帧多键合并成一次 FFI 调用）
  ///
  /// 返回是否成功（某个键超限时只会被宿主拒绝写入，不影响其它键）。
  bool updateBatch(Map<String, Object?> items) {
    if (!_runtime.isRunning || items.isEmpty) {
      return false;
    }
    final List<Map<String, Object?>> payload = <Map<String, Object?>>[];
    for (final MapEntry<String, Object?> entry in items.entries) {
      final String json = entry.value == null ? 'null' : _encode(entry.value);
      if (_lastPushed[entry.key] == json) {
        continue;
      }
      payload.add(<String, Object?>{'key': entry.key, 'value': entry.value});
    }
    if (payload.isEmpty) {
      return false;
    }
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings.musicxx_extern_plugin_state_update_batch(
        _runtime.host,
        arena.view(_encode(payload)),
        log,
      );
      if (rc != 0) {
        _runtime.log(
          3,
          'state_update_batch 失败(code=$rc): ${takeOutString(log, _runtime.bindings)}',
        );
        return false;
      }
      for (final Map<String, Object?> item in payload) {
        final key = item['key'];
        if (key is String) {
          _lastPushed[key] = _encode(item['value']);
          _lastPushedMs[key] = DateTime.now().millisecondsSinceEpoch;
        }
      }
      return true;
    } finally {
      arena.dispose();
    }
  }

  /// 清空本地"已推送"缓存（例如宿主重启后强制全量重推）
  void invalidateCache() {
    _lastPushed.clear();
    _lastPushedMs.clear();
  }

  static String _encode(Object? value) => jsonEncode(value);
}
