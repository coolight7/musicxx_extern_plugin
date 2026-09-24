import 'dart:async';
import 'dart:convert';
import 'dart:ffi';

import 'bindings_generated.dart';
import 'events.dart';
import 'hook_ids.g.dart';
import 'native_strings.dart';
import 'runtime.dart';

/// Dart 侧钩子处理器：返回 `null` 表示"不裁决"，交给下一个处理器
typedef MusicxxPluginDartHandler =
    Map<String, Object?>? Function(MusicxxPluginHookContext ctx);

/// 派发给 Dart 处理器的上下文（不可变；载荷已解码为 JSON 对象）
class MusicxxPluginHookContext {
  const MusicxxPluginHookContext({
    required this.id,
    required this.payload,
    required this.sid,
  });

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
/// 执行顺序：
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

  /// 在途的异步裁决（callId → 完成器）：结果由 `musicxx.hook.decision.result` 事件回填
  final Map<int, Completer<Map<String, Object?>?>> _pendingDecisions =
      <int, Completer<Map<String, Object?>?>>{};

  /// 节流视图缓存（key = `钩子 id|间隔毫秒`）：同一组合复用同一个实例，
  /// 节流状态才能跨调用累积
  final Map<String, MusicxxPluginThrottle> _throttles =
      <String, MusicxxPluginThrottle>{};

  /// 异步裁决的兜底定时器（事件丢失时不至于永久悬挂）
  final Map<int, Timer> _pendingDecisionTimers = <int, Timer>{};

  /// 异步裁决的兜底余量（毫秒）：宿主预算之外给事件往返留的余量
  static const int _asyncDecisionSlackMs = 600;

  int _asyncDecisionCalls = 0;
  int _asyncDecisionTimeouts = 0;

  /// 是否有任何启用中的处理器（Dart 或原生）；埋点快速路径用
  bool hasHandlers(MusicxxPluginHookId id) =>
      hasDartHandler(id) || nativeHandlerCount(id) > 0;

  /// 是否有 Dart 处理器
  bool hasDartHandler(MusicxxPluginHookId id) =>
      (_dartHandlers[id]?.isNotEmpty) ?? false;

  /// 原生/JS 处理器数量（缓存值；宿主启动后由事件与首次查询填充）
  int nativeHandlerCount(MusicxxPluginHookId id) => _nativeCounts[id.id] ?? 0;

  /// 是否有任何钩子存在处理器（全局快速开关：false 时埋点零成本）
  bool get active =>
      _dartHandlers.isNotEmpty || _nativeCounts.values.any((int v) => v > 0);

  /// 注册 Dart 处理器（同一钩子多个处理器按 priority 升序执行）
  void on(
    MusicxxPluginHookId id,
    MusicxxPluginDartHandler handler, {
    int priority = 0,
  }) {
    final List<_DartHandlerEntry> list = _dartHandlers.putIfAbsent(
      id,
      () => <_DartHandlerEntry>[],
    );
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

  /// 观察型：不等待插件，入队即返回
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
    final int budget =
        timeout?.inMilliseconds ??
        (id.budgetMs > 0 ? id.budgetMs + (id.hardMs > 0 ? id.hardMs : 0) : 0);
    final Map<String, Object?>? nativeResult = _emit(
      id,
      payloadMap,
      sync: true,
      timeoutMs: budget,
    );
    final Map<String, Object?>? mergedResult = _merge(id, merged, nativeResult);
    return (mergedResult == null || mergedResult.isEmpty) ? null : mergedResult;
  }

  /// 高频钩子的节流包装（进度/歌词行按最小间隔丢弃）
  ///
  /// 同一个 `(钩子, 间隔)` 返回**同一个实例**：节流状态（上次派发时刻）保存在节流
  /// 对象上，调用点每次新建一个等于没有节流，所以这里统一缓存复用。
  MusicxxPluginThrottle throttle(
    MusicxxPluginHookId id, {
    int minIntervalMs = 1000,
  }) {
    final String key = '${id.id}|$minIntervalMs';
    return _throttles.putIfAbsent(
      key,
      () => MusicxxPluginThrottle._(this, id, minIntervalMs),
    );
  }

  // ==================== 异步裁决 ====================

  /// 裁决型（异步）：调用点本身是 `Future` 时使用
  ///
  /// 与 [decide] 的区别只有"谁在等"：Dart 处理器照常就地执行；原生/JS 处理器链由宿主在
  /// 自己的线程上跑（**不占用调用线程**），结果经 `musicxx.hook.decision.result` 事件回来
  /// 后再按 [MusicxxPluginHookId.policy] 合并。
  ///
  /// 超时/事件丢失按"无裁决"处理（返回 `null`，调用点走原逻辑）；调用点仍需自行校验
  /// `sid` 等身份字段（切歌/切列表后到达的结果必须丢弃）。
  Future<Map<String, Object?>?> decideAsync(
    MusicxxPluginHookId id, [
    Object? payload,
    Duration? timeout,
  ]) async {
    if (!id.isDecision) {
      throw ArgumentError('decideAsync 只能用于裁决型钩子: ${id.id}');
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
    final int budget = timeout?.inMilliseconds ?? _defaultBudgetMs(id);
    final int? callId = _emitAsyncDecision(id, payloadMap, budget);
    if (callId == null) {
      return merged;
    }
    final Map<String, Object?>? nativeResult = await _awaitAsyncDecision(
      callId,
      budget,
    );
    final Map<String, Object?>? mergedResult = _merge(id, merged, nativeResult);
    return (mergedResult == null || mergedResult.isEmpty) ? null : mergedResult;
  }

  /// 在途异步裁决数量（调试页展示；只读）
  int get pendingAsyncDecisions => _pendingDecisions.length;

  /// 已完成的异步裁决次数（含超时）
  int get asyncDecisionCalls => _asyncDecisionCalls;

  /// 异步裁决超时/事件丢失次数（按"无裁决"收尾）
  int get asyncDecisionTimeouts => _asyncDecisionTimeouts;

  /// `musicxx.hook.decision.result`：把结果交给等待中的调用点
  void handleDecisionResultEvent(MusicxxPluginEvent event) {
    final int? callId = event.intOf('callId');
    if (callId == null) {
      return;
    }
    final Completer<Map<String, Object?>?>? pending = _pendingDecisions.remove(
      callId,
    );
    _pendingDecisionTimers.remove(callId)?.cancel();
    if (pending == null || pending.isCompleted) {
      return; // 已超时收尾或不属于本处理器链：直接忽略
    }
    ++_asyncDecisionCalls;
    pending.complete(_verdictOf(event.payload));
  }

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
      final int rc = _runtime.bindings.musicxx_extern_plugin_hook_stats(
        _runtime.host,
        out,
        log,
      );
      _runtime.checkOrThrow(rc, log, 'hook_stats');
      return decodeJsonObject(takeOutString(out, _runtime.bindings));
    } finally {
      arena.dispose();
    }
  }

  // ==================== 内部 ====================

  void handleDisposed() {
    _nativeCounts.clear();
    _throttles.clear();
    // 宿主已停：在途异步裁决一律按"无裁决"收尾，避免调用点永久悬挂
    for (final MapEntry<int, Timer> entry in _pendingDecisionTimers.entries) {
      entry.value.cancel();
    }
    _pendingDecisionTimers.clear();
    for (final Completer<Map<String, Object?>?> completer
        in _pendingDecisions.values) {
      if (!completer.isCompleted) {
        completer.complete(null);
      }
    }
    _pendingDecisions.clear();
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
    final hooks = event.payload['hooks'];
    if (hooks is List) {
      for (final hook in hooks) {
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
  Map<String, Object?>? _runDartHandlers(
    MusicxxPluginHookId id,
    Map<String, Object?> payload,
  ) {
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
    final Map<String, Object?>? ack = _emitAck(
      id,
      payload,
      sync: sync,
      timeoutMs: timeoutMs,
    );
    if (!sync || ack == null) {
      return null;
    }
    return _verdictOf(ack);
  }

  /// 发起一次异步裁决派发，返回配对的 `callId`（无处理器/失败返回 `null`）
  int? _emitAsyncDecision(
    MusicxxPluginHookId id,
    Map<String, Object?> payload,
    int budgetMs,
  ) {
    final Map<String, Object?>? ack = _emitAck(
      id,
      payload,
      sync: false,
      timeoutMs: budgetMs,
    );
    if (ack == null || ack['handled'] == false) {
      return null;
    }
    final callId = ack['callId'];
    return callId is num ? callId.toInt() : null; // 未返回 callId（旧库）时按"无裁决"处理
  }

  /// 等待异步裁决结果（结果事件优先；超时/事件丢失按"无裁决"收尾）
  Future<Map<String, Object?>?> _awaitAsyncDecision(int callId, int budgetMs) {
    final Completer<Map<String, Object?>?> completer =
        Completer<Map<String, Object?>?>();
    _pendingDecisions[callId] = completer;
    final int waitMs = (budgetMs > 0 ? budgetMs : 100) + _asyncDecisionSlackMs;
    _pendingDecisionTimers[callId] = Timer(Duration(milliseconds: waitMs), () {
      final Completer<Map<String, Object?>?>? pending = _pendingDecisions
          .remove(callId);
      _pendingDecisionTimers.remove(callId);
      if (pending != null && !pending.isCompleted) {
        ++_asyncDecisionTimeouts;
        pending.complete(null);
      }
    });
    return completer.future;
  }

  /// 整链默认等待预算：软预算 + 硬预算（与同步派发传给宿主的取值一致）
  static int _defaultBudgetMs(MusicxxPluginHookId id) =>
      id.budgetMs > 0 ? id.budgetLimitMs : 0;

  /// 从宿主回执里取出裁决对象（`{"handled":..,"result":{...}}`）
  static Map<String, Object?>? _verdictOf(Map<String, Object?> ack) {
    final verdict = ack['result'];
    if (verdict is Map<String, Object?>) {
      return verdict;
    }
    if (verdict is Map<Object?, Object?>) {
      return verdict.cast<String, Object?>();
    }
    return null;
  }

  /// 调用原生/JS 处理器链并返回宿主回执（一次 FFI 调用）
  Map<String, Object?>? _emitAck(
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
        _runtime.log(
          3,
          'hook_emit ${id.id} 失败: ${takeOutString(log, _runtime.bindings)}',
        );
        return null;
      }
      return decodeJsonObject(takeOutString(out, _runtime.bindings));
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
        final action = result['action'];
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
      final current = merged['action'];
      final incoming = later['action'];
      if ((current == 'cancel' || current == 'skip') &&
          incoming == 'continue') {
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

  static void _mergePatch(
    Map<String, Object?> target,
    Map<String, Object?> source,
  ) {
    final patch = source['patch'];
    if (patch is! Map) {
      return;
    }
    final Map<String, Object?> existing = switch (target['patch']) {
      final Map<String, Object?> map => Map<String, Object?>.of(map),
      final Map<Object?, Object?> map => map.cast<String, Object?>(),
      _ => <String, Object?>{},
    };
    for (final MapEntry<Object?, Object?> entry in patch.entries) {
      final key = entry.key;
      if (key is String) {
        existing[key] = entry.value;
      }
    }
    target['patch'] = existing;
  }
}

/// 节流视图（高频钩子：进度/歌词行）
///
/// 由 [MusicxxPluginHooks.throttle] 按 `(钩子, 间隔)` 缓存复用：节流状态在实例上，
/// 每次新建实例会让节流失效（调用点不要自己 `new`，直接调 `throttle()` 即可）。
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
