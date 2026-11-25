import 'dart:async';
import 'dart:convert';
import 'dart:ffi';

import 'bindings_generated.dart';
import 'events.dart';
import 'native_strings.dart';
import 'requests.dart';
import 'runtime.dart';

/// 广播（应用 → 插件）：发布、读当前值、列频道
///
/// 语义（与插件框架的广播表对应，见 `docs/plugin-broadcasts.md`）：
/// - 广播只有四件事：**声明**（宿主按生成表补）、**发布**、**读当前值**、**订阅**（插件侧）；
/// - **任何频道都走 [publish]**：带当前值的频道（`cache: latest`，例如 `musicxx.state.song`）既写
///   当前值（插件可以同步读）、也通知订阅方；没有当前值的频道（`cache: none`，例如
///   `musicxx.song.changed`）只通知，之后谁也不留当前值；
/// - 频道未声明时插件框架按生成表补一份默认声明，所以应用只要发布即可；
///   声明源在生成物 `MusicxxCapabilityCatalogue.broadcasts`；
/// - **默认同值不推**（[publish] 的 `skipIfUnchanged`）：按频道记住上次发布的 JSON，
///   值没变就跳过，省一次提交与唤醒；要"重复也通知"就传 `false`；
/// - 节流不在这里做：调用方自己决定频率，订阅方用 `intervalMs` / `mode` 声明自己的通知频率。
///
/// **读值分两条路**：[valueOf] 读 Dart 侧缓存（每帧 / 每次取值走这条，不过 FFI），
/// 缓存由 `musicxx.broadcast.changed`（含订阅时补发的当前值）维护；[read] 才是冷读。
class MusicxxPluginBroadcasts {
  MusicxxPluginBroadcasts.internal(this._runtime) {
    _eventSub = _runtime.events.listen(_onEvent);
  }

  final MusicxxPluginRuntime _runtime;

  /// 各频道上次发布成功的 JSON（用于"同值不推"）
  final Map<String, String> _lastPushed = <String, String>{};

  /// 各频道最近一次见到的当前值（`musicxx.broadcast.changed` 维护）
  final Map<String, Object?> _values = <String, Object?>{};

  StreamSubscription<MusicxxPluginEvent>? _eventSub;

  /// 订阅数变化回调：`(频道 id, 订阅方数量)`
  ///
  /// 由事件 `musicxx.broadcast.subscribers` 驱动（订阅 / 退订 / 插件卸载 / 删频道都会来一条）。
  /// 应用用它决定"还要不要继续发这个频道"，尤其是 `cache: none` 的高频事件。
  void Function(String id, int subscribers)? onSubscribers;

  /// 发布一条广播（回一条确认，需要顺序时可 `await`；不关心就 `.ignore()`）
  ///
  /// 返回是否受理（同值跳过时返回 `false`）。失败只记日志，不抛异常。
  Future<bool> publish(
    String id,
    Object? value, {
    bool skipIfUnchanged = true,
  }) async {
    if (false == _runtime.isRunning) {
      return false;
    }
    final String json = value == null ? '{}' : jsonEncode(value);
    if (skipIfUnchanged && _lastPushed[id] == json) {
      return false;
    }
    final MusicxxPluginRequestResult result = await _runtime.requests.submit(
      'broadcast.publish',
      (
        MusicxxPluginArena arena,
        Pointer<Int64> outId,
        Pointer<MusicxxExternPluginString> log,
      ) => _runtime.bindings.musicxx_extern_plugin_broadcast_publish(
        _runtime.host,
        arena.view(id),
        arena.view(json),
        outId,
        log,
      ),
    );
    if (!result.ok) {
      _runtime.log(
        3,
        'broadcast_publish($id) 失败: ${result.error?.message ?? ''}',
      ).ignore();
      return false;
    }
    _lastPushed[id] = json;
    return true;
  }

  /// 读一条频道的当前值（Dart 侧缓存；没有当前值返回 `null`）
  ///
  /// 这是**热路径**用法（每帧 / 每次取值）：不碰 FFI。缓存由
  /// `musicxx.broadcast.changed` 维护 —— 订阅（[MusicxxPluginManager.watchBroadcast]）
  /// 成功时插件框架会先补发一条当前值。
  Object? valueOf(String id) => _values[id];

  /// 冷读一条频道的当前值（等插件框架答复）
  ///
  /// - 频道没声明过 / 没有当前值（`cache: none` 或还没发布过）都返回 `null`；
  /// - 这一步只查插件框架里的当前值，不会去问属主。
  Future<Object?> read(String id) async {
    if (!_runtime.isRunning) {
      return null;
    }
    final MusicxxPluginRequestResult result = await _runtime.requests.submit(
      'broadcast.read',
      (
        MusicxxPluginArena arena,
        Pointer<Int64> outId,
        Pointer<MusicxxExternPluginString> log,
      ) => _runtime.bindings.musicxx_extern_plugin_broadcast_read(
        _runtime.host,
        arena.view(id),
        outId,
        log,
      ),
    );
    if (!result.ok) {
      return null;
    }
    final Object? value = result.data;
    _values[id] = value;
    return value;
  }

  /// 列频道（声明 + 当前值 + 订阅数）；`prefix` 为空 = 全部
  ///
  /// 每项形如
  /// `{id, cache, publish, scope, owner, subscribers, hasValue, value?}`。
  Future<List<Map<String, Object?>>> list({String prefix = ''}) async {
    if (!_runtime.isRunning) {
      return const <Map<String, Object?>>[];
    }
    final MusicxxPluginRequestResult result = await _runtime.requests.submit(
      'broadcast.list',
      (
        MusicxxPluginArena arena,
        Pointer<Int64> outId,
        Pointer<MusicxxExternPluginString> log,
      ) => _runtime.bindings.musicxx_extern_plugin_broadcast_list(
        _runtime.host,
        arena.view(prefix),
        outId,
        log,
      ),
    );
    if (!result.ok) {
      _runtime.log(
        3,
        'broadcast_list 失败: ${result.error?.message ?? ''}',
      ).ignore();
      return const <Map<String, Object?>>[];
    }
    final Object? items = result.dataObject['broadcasts'];
    if (items is! List) {
      return const <Map<String, Object?>>[];
    }
    return items
        .whereType<Map<Object?, Object?>>()
        .map((Map<Object?, Object?> item) => item.cast<String, Object?>())
        .toList(growable: false);
  }

  /// 清空"已发布"与"当前值"缓存（例如插件框架重启后强制全量重推）
  void invalidateCache() {
    _lastPushed.clear();
    _values.clear();
  }

  /// 释放事件订阅（随运行时 dispose）
  void dispose() {
    _eventSub?.cancel();
    _eventSub = null;
    onSubscribers = null;
    _values.clear();
    _lastPushed.clear();
  }

  void _onEvent(MusicxxPluginEvent event) {
    switch (event.type) {
      case MusicxxPluginEventType.broadcastChanged:
        final String id = event.stringOf('id') ?? '';
        if (id.isEmpty) {
          return;
        }
        _values[id] = event.payload['value'];
        return;
      case MusicxxPluginEventType.broadcastSubscribers:
        final void Function(String, int)? handler = onSubscribers;
        if (handler == null) {
          return;
        }
        final String id = event.stringOf('id') ?? '';
        if (id.isEmpty) {
          return;
        }
        handler(id, event.intOf('subscribers') ?? 0);
        return;
      default:
        return;
    }
  }
}
