import 'dart:convert';
import 'dart:ffi';

import 'bindings_generated.dart';
import 'native_strings.dart';
import 'runtime.dart';

/// 广播频道发布（应用 → 插件）
///
/// 语义（与插件框架的两张表对应，见 `docs/plugin-broadcasts.md`）：
/// - **事件频道**（`cache: none`，例如 `musicxx.song.changed`）用 [publish] 发布：
///   订阅方在插件框架线程上收到一条，之后谁也不留当前值；
/// - **快照频道**（`cache: latest`，例如 `musicxx.state.song`）走
///   [MusicxxPluginState.update]：它既写快照（插件可以同步 `read`），也通知订阅方；
/// - 频道必须是已声明的（生成物 `MusicxxCapabilityCatalogue.broadcasts`，原生宿主启动时
///   会把应用那一份声明好）；未声明或命名空间不对（不是 `musicxx.*`）会被插件框架拒绝并记日志；
/// - 投递不等待：调用方立刻返回，接收方慢不会拖住应用。
class MusicxxPluginBroadcasts {
  MusicxxPluginBroadcasts.internal(this._runtime);

  final MusicxxPluginRuntime _runtime;

  /// 发布一条事件；返回是否受理
  bool publish(String id, Object? value) {
    if (false == _runtime.isRunning) {
      return false;
    }
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final String json = value == null ? '{}' : jsonEncode(value);
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings.musicxx_extern_plugin_event_publish(
        _runtime.host,
        arena.view(id),
        arena.view(json),
        log,
      );
      if (rc != 0) {
        _runtime.log(
          3,
          'event_publish($id) 失败(code=$rc): ${takeOutString(log, _runtime.bindings)}',
        );
        return false;
      }
      return true;
    } finally {
      arena.dispose();
    }
  }
}
