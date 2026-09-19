import 'dart:convert';
import 'dart:ffi';

import 'bindings_generated.dart';
import 'events.dart';
import 'hook_ids.g.dart';
import 'native_strings.dart';
import 'runtime.dart';

/// Dart 侧钩子处理器：返回 `null` 表示"不裁决"，交给下一个处理器
typedef MusicxxPluginDartHandler = Map<String, Object?>? Function(MusicxxPluginHookContext ctx);

/// 派发给 Dart 处理器的上下文（不可变；载荷已解码为 JSON 对象）
class MusicxxPluginHookContext {
  const MusicxxPluginHookContext({required this.id, required this.payload, required this.sid});

  /// 当前钩子
  final MusicxxPluginHookId id;

  /// 载荷（业务侧构造的强类型对象序列化结果）
  final Map<String, Object?> payload;

  /// 身份字段（`sid`/`srcKey`/`pid`；缺失为 `null`）—— 异步结果回填时用它防串台
  final String? sid;

  /// 读取载荷字段
  Object? operator [](String key) => payload[key];
}

/// 钩子注册与派发（Dart 侧处理器链 + 原生/JS 处理器链）
///
/// 执行顺序（plan §5.4）：
/// ```
/// 快速路径（无任何处理器 → 直接返回，0 次 FFI、0 次 JSON 构造）
///   → Dart 处理器链（按 priority 升序，本线程就地执行）
///   → 原生/JS 处理器链（`hook_emit`，宿主线程执行，有等待预算）
///   → 按 policy 合并
/// ```
class MusicxxPluginHooks {
  MusicxxPluginHooks.internal(this._runtime);

  final MusicxxPluginRuntime _runtime;

  /// Dart 处理器：钩子 → 处理器列表（按 priority 升序，稳定）
  final Map<MusicxxPluginHookId, List<_DartHandlerEntry>> _dartHandlers =
      <MusicxxPluginHookId, List<_DartHandlerEntry>>{};

  /// 原生/JS 处理器数量快照（由 `musicxx.hook.changed` 事件维护；O(1) 判定）
  final Map<String, int> _nativeCounts = <String, int>{};

  /// 是否有任何启用中的处理器（Dart 或原生）；埋点快速路径用
  bool hasHandlers(MusicxxPluginHookId id) =>
      hasDartHandler(id) || nativeHandlerCount(id) > 0;

  /// 是否有 Dart 处理器
  bool hasDartHandler(MusicxxPluginHookId id) => (_dartHandlers[id]?.isNotEmpty) ?? false;

  /// 原生/JS 处理器数量（缓存值；宿主启动后由事件与首次查询填充）
  int nativeHandlerCount(MusicxxPluginHookId id) => _nativeCounts[id.id] ?? 0;

  /// 是否有任何钩子存在处理器（全局快速开关：false 时埋点零成本）
  bool get active => _dartHandlers.isNotEmpty || _nativeCounts.values.any((int v) => v > 0);

  /// 注册 Dart 处理器（同一钩子多个处理器按 priority 升序执行）
  void on(
    MusicxxPluginHookId id,
    MusicxxPluginDartHandler handler, {
    int priority = 0,
  }) {
    final List<_DartHandlerEntry> list =
        _dartHandlers.putIfAbsent(id, () => <_DartHandlerEntry>[]);
    list.add(_DartHandlerEntry(handler, priority, list.length));
    _sort(list);
  }

  /// 注销 Dart 处理器（同钩子同 handler 移除）
  void off(MusicxxPluginHookId id, MusicxxPluginDartHandler handler) {
    final List<_DartHandlerEntry>? list = _dartHandlers[id];
    if (list == null) {
      return;
    }
    list.removeWhere((_DartHandlerEntry entry) => entry.handler == handler);
    if (list.isEmpty) {
      _dartHandlers.remove(id);
    }
  }

  /// 观察型：不等待插件，入队即返回（plan §8.1）
  void observe(MusicxxPluginHookId id, [Object? payload]) {
    if (id.isDecision) {
      throw ArgumentError('observe 不能用于裁决型钩子: ${id.id}');
    }
    final Map<String, Object?> payloadMap = _asPayload(payload);
    // Dart 处理器就地执行（异常隔离），原生/JS 处理器异步通知
    _runDartHandlers(id, payloadMap);
    if (nativeHandlerCount(id) == 0 || !_runtime.isRunning) {
      return;
    }
    _emit(id, payloadMap, sync: false, timeoutMs: 0);
  }

  /// 裁决型（同步）：Dart 处理器 → 原生/JS 处理器 → 按策略合并
  ///
  /// 返回 `null` 表示"无裁决"（调用点走原逻辑）；超时/熔断同样按"无裁决"处理。
  Map<String, Object?>? decide(
    MusicxxPluginHookId id, [
    Object? payload,
    Duration? timeout,
  ]) {
    if (!id.isDecision) {
      throw ArgumentError('decide 不能用于观察型钩子: ${id.id}');
    }
    if (!active) {
      return null;
    }
    final Map<String, Object?> payloadMap = _asPayload(payload);
    Map<String, Object?>? merged;
    final Map<String, Object?>? dartResult = _runDartHandlers(id, payloadMap);
    if (dartResult != null) {
      merged = dartResult;
      if (id.policy == MusicxxPluginDecisionPolicy.firstNonNull) {
        return merged;
      }
    }
    if (nativeHandlerCount(id) == 0 || !_runtime.isRunning) {
      return merged;
    }
    final int budget = timeout?.inMilliseconds ??
        (id.budgetMs > 0 ? id.budgetMs + (id.hardMs > 0 ? id.hardMs : 0) : 0);
    final Map<String, Object?>? nativeResult = _emit(id, payloadMap, sync: true, timeoutMs: budget);
    final Map<String, Object?>? mergedResult = _merge(id, merged, nativeResult);
    return (mergedResult == null || mergedResult.isEmpty) ? null : mergedResult;
  }

  /// 高频钩子的节流包装（进度/歌词行按最小间隔丢弃）
  MusicxxPluginThrottle throttle(MusicxxPluginHookId id, {int minIntervalMs = 1000}) =>
      MusicxxPluginThrottle._(this, id, minIntervalMs);

  /// 刷新某钩子的原生处理器数量（事件丢包/宿主重启后手动兜底）
  int refreshNativeHandlerCount(MusicxxPluginHookId id) {
    if (!_runtime.isRunning) {
      _nativeCounts[id.id] = 0;
      return 0;
    }
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final Pointer<Int32> count = arena.mallocInt();
      final int rc = _runtime.bindings.musicxx_extern_plugin_hook_count(
        _runtime.host,
        arena.view(id.id),
        count,
        log,
      );
      if (rc != 0) {
        return _nativeCounts[id.id] ?? 0;
      }
      final int value = count.value;
      _nativeCounts[id.id] = value;
      return value;
    } finally {
      arena.dispose();
    }
  }

  /// 全部钩子的派发统计（次数/平均耗时/最大耗时/超时）
  Map<String, Object?> stats() {
    if (!_runtime.isRunning) {
      return const <String, Object?>{};
    }
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> out = arena.outString();
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc =
          _runtime.bindings.musicxx_extern_plugin_hook_stats(_runtime.host, out, log);
      _runtime.checkOrThrow(rc, log, 'hook_stats');
      return decodeJsonObject(takeOutString(out, _runtime.bindings));
    } finally {
      arena.dispose();
    }
  }

  // ==================== 内部 ====================

  void handleDisposed() {
    _nativeCounts.clear();
  }

  /// `musicxx.hook.changed`：维护"该钩子是否有原生处理器"的位图
  void handleHookChangedEvent(MusicxxPluginEvent event) {
    final String? hook = event.stringOf('hook');
    if (hook == null || hook.isEmpty) {
      return;
    }
    final int count = event.intOf('count') ?? 0;
    _nativeCounts[hook] = count < 0 ? 0 : count;
  }

  /// 插件生命周期事件：加载/启用/卸载会批量改变处理器数量 → 触发一次全量重查
  void handlePluginLifecycleEvent(MusicxxPluginEvent event) {
    if (!_runtime.isRunning) {
      return;
    }
    // 事件里带 hooks 列表时直接用它更新；否则留待下次派发时按需刷新
    final Object? hooks = event.payload['hooks'];
    if (hooks is List) {
      for (final Object? hook in hooks) {
        if (hook is String) {
          final MusicxxPluginHookId? id = MusicxxPluginHookId.tryFromId(hook);
          if (id != null) {
            refreshNativeHandlerCount(id);
          }
        }
      }
    }
  }

  static void _sort(List<_DartHandlerEntry> list) {
    list.sort((_DartHandlerEntry a, _DartHandlerEntry b) {
      final int byPriority = a.priority.compareTo(b.priority);
      return byPriority != 0 ? byPriority : a.order.compareTo(b.order);
    });
  }

  static Map<String, Object?> _asPayload(Object? payload) {
    if (payload == null) {
      return const <String, Object?>{};
    }
    if (payload is Map<String, Object?>) {
      return payload;
    }
    if (payload is Map<Object?, Object?>) {
      return payload.cast<String, Object?>();
    }
    return <String, Object?>{'value': payload};
  }

  /// 执行 Dart 处理器链并合并（异常隔离：单个处理器失败不影响其它处理器）
  Map<String, Object?>? _runDartHandlers(MusicxxPluginHookId id, Map<String, Object?> payload) {
    final List<_DartHandlerEntry>? list = _dartHandlers[id];
    if (list == null || list.isEmpty) {
      return null;
    }
    final MusicxxPluginHookContext context = MusicxxPluginHookContext(
      id: id,
      payload: payload,
      sid: payload['sid'] as String?,
    );
    Map<String, Object?>? merged;
    for (final _DartHandlerEntry entry in List<_DartHandlerEntry>.of(list)) {
      Map<String, Object?>? result;
      try {
        result = entry.handler(context);
      } catch (_) {
        continue; // 处理器异常按"无裁决"跳过
      }
      if (result == null || result.isEmpty) {
        continue;
      }
      if (!id.isDecision) {
        continue; // 观察型忽略返回值
      }
      if (_isTerminal(id, result)) {
        return result;
      }
      if (merged == null) {
        merged = result;
      } else {
        merged = _merge(id, merged, result);
      }
    }
    return merged;
  }

  /// 调用原生/JS 处理器链（同步等待或入队）
  Map<String, Object?>? _emit(
    MusicxxPluginHookId id,
    Map<String, Object?> payload, {
    required bool sync,
    required int timeoutMs,
  }) {
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> out = arena.outString();
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings.musicxx_extern_plugin_hook_emit(
        _runtime.host,
        arena.view(id.id),
        arena.view(jsonEncode(payload)),
        sync ? 1 : 0,
        timeoutMs,
        out,
        log,
      );
      if (rc != 0) {
        // 派发失败不阻断业务（按"无裁决"处理，只记录）
        _runtime.log(3, 'hook_emit ${id.id} 失败: ${takeOutString(log, _runtime.bindings)}');
        return null;
      }
      final Map<String, Object?> result = decodeJsonObject(takeOutString(out, _runtime.bindings));
      if (!sync) {
        return null;
      }
      final Object? verdict = result['result'];
      if (verdict is Map<String, Object?>) {
        return verdict;
      }
      if (verdict is Map<Object?, Object?>) {
        return verdict.cast<String, Object?>();
      }
      return null;
    } finally {
      arena.dispose();
    }
  }

  /// 判定"无需再问后续处理器"（firstNonNull / anyCancel 的提前终止条件）
  static bool _isTerminal(MusicxxPluginHookId id, Map<String, Object?> result) {
    switch (id.policy) {
      case MusicxxPluginDecisionPolicy.firstNonNull:
        return true;
      case MusicxxPluginDecisionPolicy.anyCancel:
        final Object? action = result['action'];
        return action == 'cancel' || action == 'skip';
      default:
        return false;
    }
  }

  /// 合并两份裁决（按声明的 policy；later 覆盖 earlier 的标量，patch 深合并）
  ///
  /// 空裁决（null 或空映射）视为"没有意见"，因此两侧都空时返回 `null`
  /// —— 调用点据此走原逻辑（不能返回空映射，否则会被当成"有裁决但没内容"）。
  static Map<String, Object?>? _merge(
    MusicxxPluginHookId id,
    Map<String, Object?>? earlier,
    Map<String, Object?>? later,
  ) {
    final bool hasEarlier = earlier != null && earlier.isNotEmpty;
    final bool hasLater = later != null && later.isNotEmpty;
    if (!hasEarlier && !hasLater) {
      return null;
    }
    if (!hasEarlier) {
      return later;
    }
    if (!hasLater) {
      return earlier;
    }
    final Map<String, Object?> merged = Map<String, Object?>.of(earlier);
    if (id.policy == MusicxxPluginDecisionPolicy.anyCancel) {
      // 最保守：已出现的 cancel/skip 不被后续的 continue 覆盖
      final Object? current = merged['action'];
      final Object? incoming = later['action'];
      if ((current == 'cancel' || current == 'skip') && incoming == 'continue') {
        // 保留保守裁决
      } else if (incoming != null) {
        merged['action'] = incoming;
      }
      _mergePatch(merged, later);
      return merged;
    }
    for (final MapEntry<String, Object?> entry in later.entries) {
      if (entry.key == 'patch') {
        _mergePatch(merged, later);
      } else {
        merged[entry.key] = entry.value;
      }
    }
    return merged;
  }

  static void _mergePatch(Map<String, Object?> target, Map<String, Object?> source) {
    final Object? patch = source['patch'];
    if (patch is! Map) {
      return;
    }
    final Map<String, Object?> existing = switch (target['patch']) {
      final Map<String, Object?> map => Map<String, Object?>.of(map),
      final Map<Object?, Object?> map => map.cast<String, Object?>(),
      _ => <String, Object?>{},
    };
    for (final MapEntry<Object?, Object?> entry in patch.entries) {
      final Object? key = entry.key;
      if (key is String) {
        existing[key] = entry.value;
      }
    }
    target['patch'] = existing;
  }
}

/// 节流视图（高频钩子：进度/歌词行）
class MusicxxPluginThrottle {
  MusicxxPluginThrottle._(this._hooks, this._id, this._minIntervalMs);

  final MusicxxPluginHooks _hooks;
  final MusicxxPluginHookId _id;
  final int _minIntervalMs;
  int _lastMs = 0;

  /// 距上次通过不足间隔时丢弃；返回是否真正派发
  bool observe([Object? payload]) {
    final int now = DateTime.now().millisecondsSinceEpoch;
    if (now - _lastMs < _minIntervalMs) {
      return false;
    }
    _lastMs = now;
    _hooks.observe(_id, payload);
    return true;
  }
}

class _DartHandlerEntry {
  const _DartHandlerEntry(this.handler, this.priority, this.order);

  final MusicxxPluginDartHandler handler;
  final int priority;
  final int order;
}
