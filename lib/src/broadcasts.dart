import 'dart:convert';
import 'dart:ffi';

import 'bindings_generated.dart';
import 'native_strings.dart';
import 'runtime.dart';

/// 广播频道发布（应用 → 插件）
///
/// 语义（与插件框架的两张表对应，见 `docs/plugin-broadcasts.md`）：
/// - **任何频道都走 [publish]**：快照频道（`cache: latest`，例如 `musicxx.state.song`）既写
///   当前值（插件可以同步 `read`）、也通知订阅方；事件频道（`cache: none`，例如
///   `musicxx.song.changed`）只通知，之后谁也不留当前值；
/// - 频道未声明时插件框架按 id 前缀补一份默认声明（`musicxx.state.*` 是快照频道，其它按事件），
///   所以应用只要发布即可；声明源在生成物 `MusicxxCapabilityCatalogue.broadcasts`；
/// - **默认同值不推**（[publish] 的 `skipIfUnchanged`）：按频道记住上次发布的 JSON，
///   值没变就跳过，省一次 FFI 与唤醒；要"重复也通知"就传 `false`；
/// - 节流不在这里做：调用方按业务决定频率（`musicxx.player.position` 本身就是 1 Hz）；
/// - 投递不等待：调用方立刻返回，接收方慢不会拖住应用。
class MusicxxPluginBroadcasts {
  MusicxxPluginBroadcasts.internal(this._runtime);

  final MusicxxPluginRuntime _runtime;

  /// 各频道上次发布成功的 JSON（用于"同值不推"）
  final Map<String, String> _lastPushed = <String, String>{};

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

  /// 清空"已发布"缓存（例如插件框架重启后强制全量重推）
  void invalidateCache() => _lastPushed.clear();
}
