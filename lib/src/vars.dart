import 'dart:convert';
import 'dart:ffi';

import 'bindings_generated.dart';
import 'native_strings.dart';
import 'runtime.dart';

/// 应用侧声明一条变量（对应 `musicxx_extern_plugin_var_declare` 的一项）
class MusicxxPluginVarDeclare {
  const MusicxxPluginVarDeclare({
    required this.key,
    this.caps = const <String>['get'],
    this.type = '',
    this.options = const <String>[],
    this.hasValue = false,
    this.value,
    this.title = '',
    this.depict = '',
    this.risk = '',
    this.throttleMs = 0,
    this.notifyThrottleMs = 0,
  });

  /// 变量键（官方键 `musicxx.<域>.<名>`）
  final String key;

  /// 能力位：`get` / `set` / `notify` 的任意非空子集（三者不要求都实现）
  final List<String> caps;

  /// 值类型提示：bool / number / string / json（只做展示与写入粗校验）
  final String type;

  /// `type == "string"` 时的候选值（稳定字符串 id；展示与写入粗校验）
  final List<String> options;

  /// 是否带上初始值（没带时 `peek` 在别人读过之前会是空）
  final bool hasValue;

  /// 初始值
  final Object? value;

  /// 展示名
  final String title;

  /// 说明
  final String depict;

  /// 风险等级：low / medium / high（管理页排序用）
  final String risk;

  /// 属主侧的推送节流设置（毫秒；0 = 不节流）
  ///
  /// 只影响应用侧"要不要合并后再推"：高频值必须声明（见设计文档 §3.11）。
  final int throttleMs;

  /// 宿主侧通知合并窗口（毫秒；0 = 不合并）
  final int notifyThrottleMs;

  Map<String, Object?> toJson() => <String, Object?>{
    'key': key,
    'caps': caps,
    if (type.isNotEmpty) 'type': type,
    if (options.isNotEmpty) 'options': options,
    if (hasValue) 'value': value,
    if (title.isNotEmpty) 'title': title,
    if (depict.isNotEmpty) 'depict': depict,
    if (risk.isNotEmpty) 'risk': risk,
    if (throttleMs > 0) 'throttleMs': throttleMs,
    if (notifyThrottleMs > 0) 'notifyThrottleMs': notifyThrottleMs,
  };
}

/// 变量通道（`runtime.vars`）：有属主、可读、可写、可订阅变动的小值
///
/// 语义要点：
/// - **应用侧是官方键的真值所在**：值由应用推（[update]），写入由应用执行
///   （[readResult] / [writeResult]）；
/// - `get` / `peek` / `list` 是只读查询：插件键可以读，官方键只能由应用自己持有；
/// - 没有真值存在宿主里：宿主只保留一份服务 `peek` 的同步读缓存。
class MusicxxPluginVars {
  MusicxxPluginVars.internal(this._runtime);

  final MusicxxPluginRuntime _runtime;

  /// 能力位常量（与原生 `MUSICXX_PLUGIN_VAR_CAP_*` 一致）
  static const int capGet = 0x1;
  static const int capSet = 0x2;
  static const int capNotify = 0x4;

  /// 宿主能力位：变量通道（`musicxx_extern_plugin_feature_bits`）
  static const int featureVars = 0x10;

  final Map<String, String> _lastPushed = <String, String>{};

  /// 声明一批变量（宿主启动后调用一次；必须在装载插件之前）
  ///
  /// 逐项失败只会记一条日志（返回值仍是 0），不影响其它项与启动流程。
  bool declare(List<MusicxxPluginVarDeclare> items) {
    if (!_runtime.isRunning || items.isEmpty) {
      return false;
    }
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final String json = _encode(
        items.map((MusicxxPluginVarDeclare item) => item.toJson()).toList(),
      );
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings.musicxx_extern_plugin_var_declare(
        _runtime.host,
        arena.view(json),
        log,
      );
      final String message = takeOutString(log, _runtime.bindings);
      if (rc != 0) {
        _runtime.log(3, 'var_declare 失败(code=$rc): $message');
        return false;
      }
      if (message.isNotEmpty) {
        _runtime.log(3, 'var_declare: $message');
      }
      return true;
    } finally {
      arena.dispose();
    }
  }

  /// 推一个官方键的值（值没变不上报，减少 FFI 与唤醒）
  bool update(String key, Object? value) {
    if (!_runtime.isRunning) {
      return false;
    }
    final String json = _encode(value);
    if (_lastPushed[key] == json) {
      return false;
    }
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings.musicxx_extern_plugin_var_update(
        _runtime.host,
        arena.view(key),
        arena.view(json),
        log,
      );
      if (rc != 0) {
        _runtime.log(
          3,
          'var_update($key) 失败(code=$rc): ${takeOutString(log, _runtime.bindings)}',
        );
        return false;
      }
      _lastPushed[key] = json;
      return true;
    } finally {
      arena.dispose();
    }
  }

  /// 批量推值（同一次变化里多个键合并成一次 FFI 调用）
  bool updateBatch(Map<String, Object?> items) {
    if (!_runtime.isRunning || items.isEmpty) {
      return false;
    }
    final List<Map<String, Object?>> payload = <Map<String, Object?>>[];
    for (final MapEntry<String, Object?> entry in items.entries) {
      final String json = _encode(entry.value);
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
      final int rc = _runtime.bindings
          .musicxx_extern_plugin_var_update_batch(
            _runtime.host,
            arena.view(_encode(payload)),
            log,
          );
      if (rc != 0) {
        _runtime.log(
          3,
          'var_update_batch 失败(code=$rc): ${takeOutString(log, _runtime.bindings)}',
        );
        return false;
      }
      for (final Map<String, Object?> item in payload) {
        final Object? key = item['key'];
        if (key is String) {
          _lastPushed[key] = _encode(item['value']);
        }
      }
      return true;
    } finally {
      arena.dispose();
    }
  }

  /// 回执插件对官方键的写请求（**回执即写入**：accepted 且带值时宿主机写入值并广播）
  bool writeResult(
    int requestId, {
    required bool accepted,
    Object? value,
    String? error,
  }) {
    if (!_runtime.isRunning) {
      return false;
    }
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings.musicxx_extern_plugin_var_write_result(
        _runtime.host,
        requestId,
        accepted ? 1 : 0,
        arena.view(value == null ? 'null' : _encode(value)),
        arena.view(error ?? ''),
        log,
      );
      if (rc != 0) {
        _runtime.log(
          3,
          'var_write_result($requestId) 失败(code=$rc): '
          '${takeOutString(log, _runtime.bindings)}',
        );
        return false;
      }
      return true;
    } finally {
      arena.dispose();
    }
  }

  /// 回答插件对官方键的读请求（`musicxx.var.read` → 这条）
  bool readResult(
    int requestId, {
    required bool ok,
    Object? value,
    String? error,
  }) {
    if (!_runtime.isRunning) {
      return false;
    }
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings.musicxx_extern_plugin_var_read_result(
        _runtime.host,
        requestId,
        ok ? 1 : 0,
        arena.view(value == null ? 'null' : _encode(value)),
        arena.view(error ?? ''),
        log,
      );
      if (rc != 0) {
        _runtime.log(
          3,
          'var_read_result($requestId) 失败(code=$rc): '
          '${takeOutString(log, _runtime.bindings)}',
        );
        return false;
      }
      return true;
    } finally {
      arena.dispose();
    }
  }

  /// 读插件键（应用侧用；官方键不从这里读）。有界等待宿主与属主。
  ///
  /// 失败返回 `null`（原因写日志）。
  Object? get(String key) {
    if (!_runtime.isRunning) {
      return null;
    }
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> out = arena.outString();
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings.musicxx_extern_plugin_var_get(
        _runtime.host,
        arena.view(key),
        out,
        log,
      );
      if (rc != 0) {
        _runtime.log(
          3,
          'var_get($key) 失败(code=$rc): ${takeOutString(log, _runtime.bindings)}',
        );
        return null;
      }
      final Map<String, Object?> result = decodeJsonObject(
        takeOutString(out, _runtime.bindings),
      );
      return result['value'];
    } finally {
      arena.dispose();
    }
  }

  /// 一次读多个键（宿主把同一批合成一次往返；单项失败为 null）
  Map<String, Object?> getMany(List<String> keys) {
    final Map<String, Object?> result = <String, Object?>{};
    for (final String key in keys) {
      result[key] = get(key);
    }
    return result;
  }

  /// 写插件键（应用也是"其他一方"，受插件声明的 `write` 限制）
  ///
  /// 返回宿主给的结算对象：`{accepted, value, changed, revision}` 或
  /// `{accepted:false, error}`；`declared` 立即结算，`handler` 等属主。
  Map<String, Object?>? set(String key, Object? value) {
    if (!_runtime.isRunning) {
      return null;
    }
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> out = arena.outString();
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings.musicxx_extern_plugin_var_set(
        _runtime.host,
        arena.view(key),
        arena.view(_encode(value)),
        out,
        log,
      );
      if (rc != 0) {
        _runtime.log(
          3,
          'var_set($key) 失败(code=$rc): ${takeOutString(log, _runtime.bindings)}',
        );
        return null;
      }
      return decodeJsonObject(takeOutString(out, _runtime.bindings));
    } finally {
      arena.dispose();
    }
  }

  /// 列出变量（`prefix` 为空 = 全部；值字段是缓存值，带 revision/ageMs/stale）
  List<Map<String, Object?>> list({String prefix = ''}) {
    if (!_runtime.isRunning) {
      return const <Map<String, Object?>>[];
    }
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> out = arena.outString();
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings.musicxx_extern_plugin_var_list(
        _runtime.host,
        arena.view(prefix),
        out,
        log,
      );
      if (rc != 0) {
        _runtime.log(
          3,
          'var_list 失败(code=$rc): ${takeOutString(log, _runtime.bindings)}',
        );
        return const <Map<String, Object?>>[];
      }
      return decodeJsonArray(takeOutString(out, _runtime.bindings));
    } finally {
      arena.dispose();
    }
  }

  /// 订阅插件键的变动（`keysOrPrefixes` 支持前缀：`plugin.` / `plugin.<id>.`）
  ///
  /// 订阅了某个键，它变化时才会推 `musicxx.var.changed` 事件。
  bool subscribe(List<String> keysOrPrefixes) =>
      _sub(keysOrPrefixes, subscribe: true);

  /// 退订
  bool unsubscribe(List<String> keysOrPrefixes) =>
      _sub(keysOrPrefixes, subscribe: false);

  bool _sub(List<String> keysOrPrefixes, {required bool subscribe}) {
    if (!_runtime.isRunning || keysOrPrefixes.isEmpty) {
      return false;
    }
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = subscribe
          ? _runtime.bindings.musicxx_extern_plugin_var_subscribe(
              _runtime.host,
              arena.view(_encode(keysOrPrefixes)),
              log,
            )
          : _runtime.bindings.musicxx_extern_plugin_var_unsubscribe(
              _runtime.host,
              arena.view(_encode(keysOrPrefixes)),
              log,
            );
      if (rc != 0) {
        _runtime.log(
          3,
          'var_${subscribe ? 'subscribe' : 'unsubscribe'} 失败(code=$rc): '
          '${takeOutString(log, _runtime.bindings)}',
        );
        return false;
      }
      return true;
    } finally {
      arena.dispose();
    }
  }

  /// 清空本地"已推送"缓存（宿主重启后强制全量重推）
  void invalidateCache() {
    _lastPushed.clear();
  }

  static String _encode(Object? value) => jsonEncode(value);
}
