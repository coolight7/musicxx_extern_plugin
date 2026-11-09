import 'dart:async';
import 'dart:convert';
import 'dart:ffi';

import 'bindings_generated.dart';
import 'events.dart';
import 'native_strings.dart';
import 'runtime.dart';

/// 广播（应用 → 插件）：发布、读当前值、列频道
///
/// 语义（与插件框架的广播表对应，见 `docs/plugin-broadcasts.md`）：
/// - 广播只有四件事：**声明**（宿主按生成表补）、**发布**、**读当前值**、**订阅**（插件侧）；
/// - **任何频道都走 [publish]**：带当前值的频道（`cache: latest`，例如 `musicxx.state.song`）既写
///   当前值（插件可以同步 [read]）、也通知订阅方；没有当前值的频道（`cache: none`，例如
///   `musicxx.song.changed`）只通知，之后谁也不留当前值；
/// - 频道未声明时插件框架按生成表补一份默认声明，所以应用只要发布即可；
///   声明源在生成物 `MusicxxCapabilityCatalogue.broadcasts`；
/// - **默认同值不推**（[publish] 的 `skipIfUnchanged`）：按频道记住上次发布的 JSON，
///   值没变就跳过，省一次 FFI 与唤醒；要"重复也通知"就传 `false`；
/// - 节流不在这里做：调用方自己决定频率，订阅方用 `intervalMs` / `mode` 声明自己的通知频率；
/// - 投递不等待：调用方立刻返回，接收方慢不会拖住应用。
class MusicxxPluginBroadcasts {
  MusicxxPluginBroadcasts.internal(this._runtime) {
    _eventSub = _runtime.events.listen(_onEvent);
  }

  final MusicxxPluginRuntime _runtime;

  /// 各频道上次发布成功的 JSON（用于"同值不推"）
  final Map<String, String> _lastPushed = <String, String>{};

  StreamSubscription<MusicxxPluginEvent>? _eventSub;

  /// 订阅数变化回调：`(频道 id, 订阅方数量)`
  ///
  /// 由事件 `musicxx.broadcast.subscribers` 驱动（订阅 / 退订 / 插件卸载 / 删频道都会来一条）。
  /// 应用用它决定"还要不要继续发这个频道"，尤其是 `cache: none` 的高频事件。
  void Function(String id, int subscribers)? onSubscribers;

  /// 发布一条广播；返回是否受理（同值跳过时返回 false）
  bool publish(String id, Object? value, {bool skipIfUnchanged = true}) {
    if (false == _runtime.isRunning) {
      return false;
    }
    final String json = value == null ? '{}' : jsonEncode(value);
    if (skipIfUnchanged && _lastPushed[id] == json) {
      return false;
    }
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings.musicxx_extern_plugin_broadcast_publish(
        _runtime.host,
        arena.view(id),
        arena.view(json),
        log,
      );
      if (rc != 0) {
        _runtime.log(
          3,
          'broadcast_publish($id) 失败(code=$rc): ${takeOutString(log, _runtime.bindings)}',
        );
        return false;
      }
      _lastPushed[id] = json;
      return true;
    } finally {
      arena.dispose();
    }
  }

  /// 读一条频道的当前值（同步；没有当前值返回 `null`）
  ///
  /// - 频道没声明过 / 没有当前值（`cache: none` 或还没发布过）都返回 `null`；
  /// - 这一步不等待：读的是插件框架里的当前值，不会去问属主。
  Object? read(String id) {
    if (!_runtime.isRunning) {
      return null;
    }
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> out = arena.outString();
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings.musicxx_extern_plugin_broadcast_read(
        _runtime.host,
        arena.view(id),
        out,
        log,
      );
      if (rc != 0) {
        return null;
      }
      return tryDecodeJson(takeOutString(out, _runtime.bindings));
    } finally {
      arena.dispose();
    }
  }

  /// 列频道（声明 + 当前值 + 订阅数）；`prefix` 为空 = 全部
  ///
  /// 每项形如
  /// `{id, cache, publish, scope, owner, subscribers, hasValue, value?}`。
  List<Map<String, Object?>> list({String prefix = ''}) {
    if (!_runtime.isRunning) {
      return const <Map<String, Object?>>[];
    }
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> out = arena.outString();
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings.musicxx_extern_plugin_broadcast_list(
        _runtime.host,
        arena.view(prefix),
        out,
        log,
      );
      if (rc != 0) {
        _runtime.log(
          3,
          'broadcast_list 失败(code=$rc): ${takeOutString(log, _runtime.bindings)}',
        );
        return const <Map<String, Object?>>[];
      }
      final Map<String, Object?> result = decodeJsonObject(
        takeOutString(out, _runtime.bindings),
      );
      final Object? items = result['broadcasts'];
      if (items is! List) {
        return const <Map<String, Object?>>[];
      }
      return items
          .whereType<Map<Object?, Object?>>()
          .map((Map<Object?, Object?> item) => item.cast<String, Object?>())
          .toList(growable: false);
    } finally {
      arena.dispose();
    }
  }

  /// 清空"已发布"缓存（例如插件框架重启后强制全量重推）
  void invalidateCache() => _lastPushed.clear();

  /// 释放事件订阅（随运行时 dispose）
  void dispose() {
    _eventSub?.cancel();
    _eventSub = null;
    onSubscribers = null;
  }

  void _onEvent(MusicxxPluginEvent event) {
    if (event.type != MusicxxPluginEventType.broadcastSubscribers) {
      return;
    }
    final void Function(String, int)? handler = onSubscribers;
    if (handler == null) {
      return;
    }
    final String id = event.stringOf('id') ?? '';
    if (id.isEmpty) {
      return;
    }
    handler(id, event.intOf('subscribers') ?? 0);
  }
}
