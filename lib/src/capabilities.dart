import 'dart:async';
import 'dart:convert';
import 'dart:ffi';

import 'bindings_generated.dart';
import 'capability_ids.g.dart';
import 'events.dart';
import 'native_strings.dart';
import 'runtime.dart';

/// Dart 侧能力实现：返回 `null`（或空映射）表示"没意见"，交给下一个实现
typedef MusicxxPluginImplementation =
    Map<String, Object?>? Function(MusicxxPluginCapabilityContext ctx);

/// 派发给 Dart 实现的上下文（不可变；载荷已解码为 JSON 对象）
class MusicxxPluginCapabilityContext {
  const MusicxxPluginCapabilityContext({
    required this.id,
    required this.payload,
    required this.sid,
  });

  /// 当前能力 id
  final String id;

  /// 载荷（业务侧构造的强类型对象序列化结果）
  final Map<String, Object?> payload;

  /// 身份字段（`sid`/`srcKey`/`pid`；缺失为 `null`）—— 异步结果回填时用它防串台
  final String? sid;

  /// 读取载荷字段
  Object? operator [](String key) => payload[key];
}

/// 一个外部插件的能力处理器（自省用：谁实现了哪条能力、在哪个阶段、排第几）
///
/// 数据来自插件框架的 `musicxx.capability.changed` 事件（载荷里的 `handlers` 列表）与
/// `musicxx_extern_plugin_capability_handlers` 查询，字段与插件框架给出的完全一致：
/// 处理器按**调用顺序**排列（priority 升序，同优先级按注册顺序）。
class MusicxxPluginHandlerInfo {
  const MusicxxPluginHandlerInfo({
    required this.plugin,
    required this.instance,
    required this.kind,
    required this.priority,
    this.ownerTag = "",
    this.phase = "impl",
  });

  /// 插件 id
  final String plugin;

  /// 实例名（JS 是 `js:<id>`，动态库是插件名）
  final String instance;

  /// 形态：`js` / `native`
  final String kind;

  /// 优先级（小者先执行）
  final int priority;

  /// 注册时的 owner 标记（同一插件对同一条能力注册多项时用来区分）
  final String ownerTag;

  /// 参与方式：`impl` / `before` / `after`
  final String phase;

  /// 从插件框架给出的 JSON 项解析；字段缺失按空值处理（不抛异常）
  static MusicxxPluginHandlerInfo fromJson(Map<String, Object?> json) {
    return MusicxxPluginHandlerInfo(
      plugin: json["plugin"] is String ? json["plugin"]! as String : "",
      instance: json["instance"] is String ? json["instance"]! as String : "",
      kind: json["kind"] is String ? json["kind"]! as String : "",
      priority: json["priority"] is num
          ? (json["priority"]! as num).toInt()
          : 0,
      ownerTag: json["ownerTag"] is String ? json["ownerTag"]! as String : "",
      phase: json["phase"] is String && (json["phase"]! as String).isNotEmpty
          ? json["phase"]! as String
          : "impl",
    );
  }

  Map<String, Object?> toJson() => <String, Object?>{
    "plugin": plugin,
    "instance": instance,
    "kind": kind,
    "phase": phase,
    "priority": priority,
    "ownerTag": ownerTag,
  };

  @override
  String toString() =>
      "${plugin.isEmpty ? instance : plugin}"
      "($kind, phase=$phase, priority=$priority)";
}

/// 能力派发（Dart 侧实现链 + 原生/JS 实现链）
///
/// 执行顺序：
/// ```
/// 快速路径（无任何实现 → 直接返回，0 次 FFI、0 次 JSON 构造）
///   → Dart 实现链（按 priority 升序，本线程就地执行）
///   → 原生/JS 实现链（`capability_dispatch`，插件框架线程执行）
///   → 按声明里的 merge 合并
/// ```
///
/// 一条 id 是能力还是广播由声明决定（`capability_ids.g.dart`）：广播只通知、没有结果，
/// 所以 [notify] 走广播、[dispatch] 走能力，用错会抛 [ArgumentError]。
class MusicxxPluginCapabilities {
  MusicxxPluginCapabilities.internal(this._runtime);

  final MusicxxPluginRuntime _runtime;

  /// Dart 能力 id → 实现列表（按 priority 升序，稳定）
  ///
  /// 一条登记带阶段（缺省 `impl`）。同一条能力上可以分别登记前链 / 实现链 / 后链，
  /// 派发时只跑对应阶段的那一批。
  final Map<String, List<_DartImplementationEntry>> _dartImpls =
      <String, List<_DartImplementationEntry>>{};

  /// 原生/JS 处理器数量快照（`能力 id|阶段` → 数量；由 `musicxx.capability.changed` 事件维护）
  ///
  /// 判定一律按**阶段**：同一条能力上外置插件可能只注册了 `after`，这时 `impl` 阶段是 0 ——
  /// 拿总数去判"要不要派发 impl"会让派发空跑一趟。
  final Map<String, int> _nativeCounts = <String, int>{};

  /// 原生/JS 处理器列表快照（`能力 id|阶段` → 处理器，按调用顺序）
  final Map<String, List<MusicxxPluginHandlerInfo>> _nativeHandlers =
      <String, List<MusicxxPluginHandlerInfo>>{};

  /// 计数/列表缓存的键（`能力 id|阶段`）
  static String _phaseKey(String id, MusicxxCapabilityPhase phase) =>
      "$id|${phase.wireName}";

  /// 异步派发的等待方（callId → 完成器）：结果由 `musicxx.capability.decision.result` 事件回填
  final Map<int, Completer<Map<String, Object?>?>> _pendingDispatches =
      <int, Completer<Map<String, Object?>?>>{};

  /// 节流视图缓存（key = `能力 id|间隔毫秒`）：同一组合复用同一个实例，
  /// 节流状态才能跨调用累积
  final Map<String, MusicxxPluginThrottle> _throttles =
      <String, MusicxxPluginThrottle>{};

  int _asyncDispatchCalls = 0;

  /// 某个能力现在有没有实现（Dart 或原生/JS）；调用快速路径用
  bool hasHandlers(
    String id, {
    MusicxxCapabilityPhase phase = MusicxxCapabilityPhase.impl,
  }) =>
      hasDartImplementation(id, phase: phase) ||
      nativeHandlerCount(id, phase: phase) > 0;

  /// 某个阶段有没有 Dart 实现
  bool hasDartImplementation(
    String id, {
    MusicxxCapabilityPhase phase = MusicxxCapabilityPhase.impl,
  }) {
    final List<_DartImplementationEntry>? list = _dartImpls[id];
    if (null == list) {
      return false;
    }
    for (final _DartImplementationEntry entry in list) {
      if (entry.phase == phase) {
        return true;
      }
    }
    return false;
  }

  /// 原生/JS 处理器数量（缓存值；插件框架启动后由事件与首次查询填充）
  int nativeHandlerCount(
    String id, {
    MusicxxCapabilityPhase phase = MusicxxCapabilityPhase.impl,
  }) => _nativeCounts[_phaseKey(id, phase)] ?? 0;

  /// 原生/JS 处理器列表（按调用顺序：priority 升序、同优先级按注册顺序）
  ///
  /// [phase] 为空 = 全部阶段（按 before → impl → after 拼接，同一阶段内仍是调用顺序）。
  /// 空列表表示这条能力现在没有处理器。列表是只读快照，改它不影响派发。
  List<MusicxxPluginHandlerInfo> handlersOf(
    String id, {
    MusicxxCapabilityPhase? phase,
  }) {
    if (null != phase) {
      return List<MusicxxPluginHandlerInfo>.unmodifiable(
        _nativeHandlers[_phaseKey(id, phase)] ??
            const <MusicxxPluginHandlerInfo>[],
      );
    }
    final List<MusicxxPluginHandlerInfo> all = <MusicxxPluginHandlerInfo>[];
    for (final MusicxxCapabilityPhase item in MusicxxCapabilityPhase.values) {
      all.addAll(
        _nativeHandlers[_phaseKey(id, item)] ??
            const <MusicxxPluginHandlerInfo>[],
      );
    }
    return List<MusicxxPluginHandlerInfo>.unmodifiable(all);
  }

  /// 有没有拿到过处理器列表（false 时自省只能报数量，见 [handlersOf]）
  bool get hasHandlerList => _nativeHandlers.isNotEmpty;

  /// 回查插件框架的处理器列表并刷新缓存（插件框架重启、丢了一条 `musicxx.capability.changed` 后手动对齐）
  ///
  /// 返回 `null` 表示这次没拿到列表（插件框架没在跑、或插件框架不认识这条 id）；
  /// 此时缓存保持原样，调用方用 [nativeHandlerCount] 判断有没有处理器更可靠。
  List<MusicxxPluginHandlerInfo>? refreshNativeHandlers(
    String id, {
    MusicxxCapabilityPhase? phase,
  }) {
    if (!_runtime.isRunning) {
      _clearNative(id, phase: phase);
      return const <MusicxxPluginHandlerInfo>[];
    }
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      return _queryHandlers(id, arena, phase: phase);
    } finally {
      arena.dispose();
    }
  }

  /// 是否有任何能力存在实现（全局快速开关：false 时调用零成本）
  bool get active =>
      _dartImpls.isNotEmpty || _nativeCounts.values.any((int v) => v > 0);

  /// 登记 Dart 实现（同一能力多个实现按 priority 升序执行）
  void on(
    String id,
    MusicxxPluginImplementation implementation, {
    int priority = 0,
    MusicxxCapabilityPhase phase = MusicxxCapabilityPhase.impl,
  }) {
    final List<_DartImplementationEntry> list = _dartImpls.putIfAbsent(
      id,
      () => <_DartImplementationEntry>[],
    );
    list.add(
      _DartImplementationEntry(implementation, priority, list.length, phase),
    );
    _sort(list);
  }

  /// 注销 Dart 实现（同能力同实现移除；带 `phase` 时只摘那个阶段）
  void off(
    String id,
    MusicxxPluginImplementation implementation, {
    MusicxxCapabilityPhase? phase,
  }) {
    final List<_DartImplementationEntry>? list = _dartImpls[id];
    if (list == null) {
      return;
    }
    list.removeWhere(
      (_DartImplementationEntry entry) =>
          entry.implementation == implementation &&
          (null == phase || entry.phase == phase),
    );
    if (list.isEmpty) {
      _dartImpls.remove(id);
    }
  }

  /// 广播：属主改了东西，通知所有关心的人（不等待、没有结果）
  void notify(String id, [Object? payload]) {
    if (null == MusicxxCapabilityCatalogue.broadcastOf(id)) {
      throw ArgumentError('notify 只能用于广播: $id');
    }
    final Map<String, Object?> payloadMap = _asPayload(payload);
    // Dart 实现就地执行（异常隔离），原生/JS 实现异步通知
    _runDartImpls(id, payloadMap);
    if (nativeHandlerCount(id) == 0 || !_runtime.isRunning) {
      return;
    }
    _emitAsync(id, payloadMap);
  }

  /// 调用一条能力（同步）：Dart 实现 → 原生/JS 实现 → 按声明的 merge 合并
  ///
  /// 返回 `null` 表示"没有实现给出结果"（调用点走原逻辑）。
  ///
  /// 插件框架不设等待超时：实现链返回之前，本调用会一直占用调用线程（热路径上调用
  /// 就要自己判断能不能接受这一点）。
  Map<String, Object?>? dispatch(String id, [Object? payload]) {
    return dispatchAtPhase(id, MusicxxCapabilityPhase.impl, payload);
  }

  /// 某个阶段的同步派发（占用调用线程）
  ///
  /// 只有 `impl` 会跑 Dart 侧实现（`on()` 登记的都在实现链上）；`before` / `after`
  /// 是外置插件专用的阶段，而且必须是同步处理器。
  Map<String, Object?>? dispatchAtPhase(
    String id,
    MusicxxCapabilityPhase phase, [
    Object? payload,
  ]) {
    if (null != MusicxxCapabilityCatalogue.broadcastOf(id)) {
      throw ArgumentError('dispatch 不能用于广播: $id（广播用 notify）');
    }
    if (!active) {
      return null;
    }
    final Map<String, Object?> payloadMap = _asPayload(payload);
    Map<String, Object?>? merged;
    final Map<String, Object?>? dartResult = _runDartImpls(
      id,
      payloadMap,
      phase: phase,
    );
    if (dartResult != null) {
      merged = dartResult;
      if (_mergeKindOf(id) == MusicxxCapabilityMerge.first) {
        return merged;
      }
    }
    if (nativeHandlerCount(id, phase: phase) == 0 || !_runtime.isRunning) {
      return merged;
    }
    final Map<String, Object?>? ack = _emitAck(
      id,
      payloadMap,
      sync: true,
      timeoutMs: 0,
      phase: phase,
    );
    final Map<String, Object?>? nativeResult = null == ack
        ? null
        : _resultOf(ack);
    final Map<String, Object?>? mergedResult = _merge(id, merged, nativeResult);
    return (mergedResult == null || mergedResult.isEmpty) ? null : mergedResult;
  }

  /// 高频能力的节流包装（进度/歌词行按最小间隔丢弃）
  ///
  /// 同一个 `(能力, 间隔)` 返回**同一个实例**：节流状态（上次派发时刻）保存在节流
  /// 对象上，调用点每次新建一个等于没有节流，所以这里统一缓存复用。
  MusicxxPluginThrottle throttle(String id, {int minIntervalMs = 1000}) {
    final String key = '$id|$minIntervalMs';
    return _throttles.putIfAbsent(
      key,
      () => MusicxxPluginThrottle._(this, id, minIntervalMs),
    );
  }

  // ==================== 异步派发 ====================

  /// 调用一条能力（异步）：调用点本身是 `Future` 时使用
  ///
  /// 与 [dispatch] 的区别只有"谁在等"：Dart 实现照常就地执行；原生/JS 实现链由插件框架在
  /// 自己的线程上跑（**不占用调用线程**），结果经 `musicxx.capability.decision.result` 事件
  /// 回来后按声明的 merge 合并。
  ///
  /// 调用点仍需自行校验 `sid` 等身份字段（切歌/切列表后到达的结果必须丢弃）。
  Future<Map<String, Object?>?> dispatchAsync(String id, [Object? payload]) {
    return dispatchAsyncAtPhase(id, MusicxxCapabilityPhase.impl, payload);
  }

  /// 某个阶段的异步派发（不占用调用线程）
  Future<Map<String, Object?>?> dispatchAsyncAtPhase(
    String id,
    MusicxxCapabilityPhase phase, [
    Object? payload,
  ]) async {
    if (null != MusicxxCapabilityCatalogue.broadcastOf(id)) {
      throw ArgumentError('dispatchAsync 不能用于广播: $id（广播用 notify）');
    }
    if (!active) {
      return null;
    }
    final Map<String, Object?> payloadMap = _asPayload(payload);
    Map<String, Object?>? merged;
    final Map<String, Object?>? dartResult = _runDartImpls(
      id,
      payloadMap,
      phase: phase,
    );
    if (dartResult != null) {
      merged = dartResult;
      if (_mergeKindOf(id) == MusicxxCapabilityMerge.first) {
        return merged;
      }
    }
    if (nativeHandlerCount(id, phase: phase) == 0 || !_runtime.isRunning) {
      return merged;
    }
    final int? callId = _emitAsyncDispatch(id, payloadMap, phase: phase);
    if (callId == null) {
      return merged;
    }
    final Map<String, Object?>? nativeResult = await _awaitAsyncDispatch(
      callId,
    );
    final Map<String, Object?>? mergedResult = _merge(id, merged, nativeResult);
    return (mergedResult == null || mergedResult.isEmpty) ? null : mergedResult;
  }

  /// 等结果回传的异步派发数量（调试页展示；只读）
  int get pendingAsyncDispatches => _pendingDispatches.length;

  /// 已完成的异步派发次数
  int get asyncDispatchCalls => _asyncDispatchCalls;

  /// `musicxx.capability.decision.result`：把结果交给等待中的调用点
  void handleDecisionResultEvent(MusicxxPluginEvent event) {
    final int? callId = event.intOf('callId');
    if (callId == null) {
      return;
    }
    final Completer<Map<String, Object?>?>? pending = _pendingDispatches.remove(
      callId,
    );
    if (pending == null || pending.isCompleted) {
      return; // 不属于本处理器链：直接忽略
    }
    ++_asyncDispatchCalls;
    pending.complete(_resultOf(event.payload));
  }

  /// 刷新某能力的原生处理器数量与列表（事件丢包/插件框架重启后手动重新查询）
  int refreshNativeHandlerCount(
    String id, {
    MusicxxCapabilityPhase phase = MusicxxCapabilityPhase.impl,
  }) {
    final String key = _phaseKey(id, phase);
    if (!_runtime.isRunning) {
      _clearNative(id, phase: phase);
      return 0;
    }
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final Pointer<Int32> count = arena.mallocInt();
      final int rc = _runtime.bindings
          .musicxx_extern_plugin_capability_handler_count_phase(
            _runtime.host,
            arena.view(id),
            phase.code,
            count,
            log,
          );
      if (rc != 0) {
        return _nativeCounts[key] ?? 0;
      }
      final int value = count.value;
      _nativeCounts[key] = value;
      if (value <= 0) {
        _nativeHandlers.remove(key);
      } else {
        _queryHandlers(id, arena, phase: phase);
      }
      return value;
    } finally {
      arena.dispose();
    }
  }

  /// 全部能力的派发统计（次数/失败/平均耗时/最大耗时）
  Map<String, Object?> stats() {
    if (!_runtime.isRunning) {
      return const <String, Object?>{};
    }
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> out = arena.outString();
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings.musicxx_extern_plugin_capability_stats(
        _runtime.host,
        out,
        log,
      );
      _runtime.checkOrThrow(rc, log, 'capability_stats');
      return decodeJsonObject(takeOutString(out, _runtime.bindings));
    } finally {
      arena.dispose();
    }
  }

  // ==================== 内部 ====================

  /// 声明里的合并策略（没有声明 / 包装点 = null）
  static MusicxxCapabilityMerge? _mergeKindOf(String id) =>
      MusicxxCapabilityCatalogue.capabilityOf(id)?.mergeKind;

  void _clearNative(String id, {MusicxxCapabilityPhase? phase}) {
    if (null == phase) {
      for (final MusicxxCapabilityPhase item in MusicxxCapabilityPhase.values) {
        _nativeCounts[_phaseKey(id, item)] = 0;
        _nativeHandlers.remove(_phaseKey(id, item));
      }
      return;
    }
    _nativeCounts[_phaseKey(id, phase)] = 0;
    _nativeHandlers.remove(_phaseKey(id, phase));
  }

  /// 查询某条能力的处理器列表并刷新缓存
  ///
  /// `arena` 由调用方给出并负责释放（入参视图与出参槽都在它上面）。
  List<MusicxxPluginHandlerInfo>? _queryHandlers(
    String id,
    MusicxxPluginArena arena, {
    MusicxxCapabilityPhase? phase,
  }) {
    final Pointer<MusicxxExternPluginString> out = arena.outString();
    final Pointer<MusicxxExternPluginString> log = arena.outString();
    final int rc = null == phase
        ? _runtime.bindings.musicxx_extern_plugin_capability_handlers(
            _runtime.host,
            arena.view(id),
            out,
            log,
          )
        : _runtime.bindings.musicxx_extern_plugin_capability_handlers_phase(
            _runtime.host,
            arena.view(id),
            phase.code,
            out,
            log,
          );
    final Map<String, Object?> data = decodeJsonObject(
      takeOutString(out, _runtime.bindings),
    );
    if (rc != 0) {
      // 能力 id 插件框架不认识等：保持缓存原样（不把"查不到"当成"没有处理器"）
      return null;
    }
    return _applyHandlerList(id, data, phase: phase);
  }

  /// 把插件框架给出的 `{capability, count, handlers}` 写进缓存，返回解析出的列表
  List<MusicxxPluginHandlerInfo> _applyHandlerList(
    String id,
    Map<String, Object?> data, {
    MusicxxCapabilityPhase? phase,
  }) {
    final Object? raw = data['handlers'];
    final List<MusicxxPluginHandlerInfo> list = <MusicxxPluginHandlerInfo>[];
    if (raw is List) {
      for (final Object? item in raw) {
        if (item is Map) {
          list.add(
            MusicxxPluginHandlerInfo.fromJson(item.cast<String, Object?>()),
          );
        }
      }
    }
    if (null != phase) {
      // 只更新这一个阶段（其它阶段的缓存不动）
      _nativeCounts[_phaseKey(id, phase)] = list.length;
      if (list.isEmpty) {
        _nativeHandlers.remove(_phaseKey(id, phase));
      } else {
        _nativeHandlers[_phaseKey(id, phase)] = list;
      }
      return List<MusicxxPluginHandlerInfo>.unmodifiable(list);
    }
    // 全部阶段: 按每项自报的 phase 分组写回
    _writePhaseGroups(id, list);
    return List<MusicxxPluginHandlerInfo>.unmodifiable(list);
  }

  /// 把一份"全部阶段"的处理器列表按阶段分组写进缓存
  void _writePhaseGroups(
    String id,
    List<MusicxxPluginHandlerInfo> list, {
    Map<String, Object?>? phaseCount,
  }) {
    for (final MusicxxCapabilityPhase phase in MusicxxCapabilityPhase.values) {
      _nativeHandlers.remove(_phaseKey(id, phase));
    }
    final Map<String, List<MusicxxPluginHandlerInfo>> groups =
        <String, List<MusicxxPluginHandlerInfo>>{};
    for (final MusicxxPluginHandlerInfo handler in list) {
      groups
          .putIfAbsent(handler.phase, () => <MusicxxPluginHandlerInfo>[])
          .add(handler);
    }
    for (final MusicxxCapabilityPhase phase in MusicxxCapabilityPhase.values) {
      final Object? declared = phaseCount?[phase.wireName];
      final int count = declared is num
          ? declared.toInt()
          : (groups[phase.wireName]?.length ?? 0);
      _nativeCounts[_phaseKey(id, phase)] = count < 0 ? 0 : count;
      final List<MusicxxPluginHandlerInfo>? group = groups[phase.wireName];
      if (null != group && group.isNotEmpty) {
        _nativeHandlers[_phaseKey(id, phase)] = group;
      }
    }
  }

  void handleDisposed() {
    _nativeCounts.clear();
    _nativeHandlers.clear();
    _throttles.clear();
    // 插件框架已停：还没结算的异步派发一律按"没有结果"收尾，避免调用点永久悬挂
    for (final Completer<Map<String, Object?>?> completer
        in _pendingDispatches.values) {
      if (!completer.isCompleted) {
        completer.complete(null);
      }
    }
    _pendingDispatches.clear();
  }

  /// `musicxx.capability.changed`：维护"这条能力有没有原生处理器"的位图与处理器列表
  void handleCapabilityChangedEvent(MusicxxPluginEvent event) {
    final String? capabilityId = event.stringOf('capability');
    if (capabilityId == null || capabilityId.isEmpty) {
      return;
    }
    final Object? rawPhaseCount = event.payload['phaseCount'];
    final Map<String, Object?>? phaseCount = rawPhaseCount is Map
        ? rawPhaseCount.cast<String, Object?>()
        : null;
    final Object? raw = event.payload['handlers'];
    if (raw is List) {
      final List<MusicxxPluginHandlerInfo> list = <MusicxxPluginHandlerInfo>[];
      for (final Object? item in raw) {
        if (item is Map) {
          list.add(
            MusicxxPluginHandlerInfo.fromJson(item.cast<String, Object?>()),
          );
        }
      }
      _writePhaseGroups(capabilityId, list, phaseCount: phaseCount);
      return;
    }
    final int count = event.intOf('count') ?? 0;
    if (null != phaseCount) {
      for (final MusicxxCapabilityPhase phase
          in MusicxxCapabilityPhase.values) {
        final Object? declared = phaseCount[phase.wireName];
        _nativeCounts[_phaseKey(capabilityId, phase)] = declared is num
            ? (declared.toInt() < 0 ? 0 : declared.toInt())
            : 0;
      }
      return;
    }
    _nativeCounts[_phaseKey(capabilityId, MusicxxCapabilityPhase.impl)] =
        count < 0 ? 0 : count;
    for (final MusicxxCapabilityPhase phase in MusicxxCapabilityPhase.values) {
      if (phase != MusicxxCapabilityPhase.impl) {
        _nativeCounts[_phaseKey(capabilityId, phase)] = 0;
      }
    }
  }

  /// 插件生命周期事件：加载/启用/卸载会批量改变处理器数量 → 用插件框架的权威值对齐一遍
  ///
  /// 计数平时靠 `musicxx.capability.changed` 维护（每次注册/注销一条）；它一旦丢一条就会停在
  /// 旧值——"有处理器却被当成没有"会让该能力静默失效，所以每次插件生命周期变化都用插件框架
  /// 里的真实数量覆盖一次：载荷带 `capabilities` 时只查这些 id。
  void handlePluginLifecycleEvent(MusicxxPluginEvent event) {
    if (!_runtime.isRunning) {
      return;
    }
    final List<String> targets = <String>[];
    final capabilities = event.payload['capabilities'];
    if (capabilities is List) {
      for (final capability in capabilities) {
        if (capability is String && capability.isNotEmpty) {
          targets.add(capability);
        }
      }
    }
    for (final String id in targets) {
      // 每个阶段都对齐一次：只对齐 impl 会让"只注册了 after"的能力被当成没有处理器
      for (final MusicxxCapabilityPhase phase
          in MusicxxCapabilityPhase.values) {
        refreshNativeHandlerCount(id, phase: phase);
      }
    }
  }

  static void _sort(List<_DartImplementationEntry> list) {
    list.sort((_DartImplementationEntry a, _DartImplementationEntry b) {
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

  /// 执行 Dart 实现链并合并（异常隔离：单个实现失败不影响其它实现）
  Map<String, Object?>? _runDartImpls(
    String id,
    Map<String, Object?> payload, {
    MusicxxCapabilityPhase phase = MusicxxCapabilityPhase.impl,
  }) {
    final List<_DartImplementationEntry>? list = _dartImpls[id];
    if (list == null || list.isEmpty) {
      return null;
    }
    final MusicxxPluginCapabilityContext context =
        MusicxxPluginCapabilityContext(
          id: id,
          payload: payload,
          sid: payload['sid'] as String?,
        );
    Map<String, Object?>? merged;
    for (final _DartImplementationEntry entry
        in List<_DartImplementationEntry>.of(list)) {
      if (entry.phase != phase) {
        continue; // 只跑本阶段的实现（同一条能力上三个阶段互不干扰）
      }
      Map<String, Object?>? result;
      try {
        result = entry.implementation(context);
      } catch (_) {
        continue; // 实现异常按"没意见"跳过
      }
      if (result == null || result.isEmpty) {
        continue;
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

  /// 广播派发（不做回执解析）
  ///
  /// 插件框架收到的是"入队即返回"（`sync = 0`）：这里不等结果，回执内容也没有读者，
  /// 所以只把出参内存释放掉，不做 JSON 解析（广播在热路径上，能省一次解析）。
  void _emitAsync(String id, Map<String, Object?> payload) {
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> out = arena.outString();
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings
          .musicxx_extern_plugin_capability_dispatch(
            _runtime.host,
            arena.view(id),
            arena.view(jsonEncode(payload)),
            0,
            0,
            out,
            log,
          );
      if (rc != 0) {
        // 派发失败不阻断业务（按"没插件"处理，只记录）
        _runtime.log(
          3,
          'capability_dispatch $id 失败: '
          '${takeOutString(log, _runtime.bindings)}',
        );
      }
      discardOutString(out, _runtime.bindings);
    } finally {
      arena.dispose();
    }
  }

  /// 发起一次异步派发，返回配对的 `callId`（无处理器/失败返回 `null`）
  int? _emitAsyncDispatch(
    String id,
    Map<String, Object?> payload, {
    MusicxxCapabilityPhase phase = MusicxxCapabilityPhase.impl,
  }) {
    final Map<String, Object?>? ack = _emitAck(
      id,
      payload,
      sync: false,
      timeoutMs: 0,
      phase: phase,
    );
    if (ack == null || ack['handled'] == false) {
      return null;
    }
    final callId = ack['callId'];
    return callId is num ? callId.toInt() : null;
  }

  /// 等待异步派发结果（由 `musicxx.capability.decision.result` 事件回填；没有等待超时）
  Future<Map<String, Object?>?> _awaitAsyncDispatch(int callId) {
    final Completer<Map<String, Object?>?> completer =
        Completer<Map<String, Object?>?>();
    _pendingDispatches[callId] = completer;
    return completer.future;
  }

  /// 从插件框架回执里取出结果对象（`{"handled":..,"result":{...}}`）
  static Map<String, Object?>? _resultOf(Map<String, Object?> ack) {
    final result = ack['result'];
    if (result is Map<String, Object?>) {
      return result;
    }
    if (result is Map<Object?, Object?>) {
      return result.cast<String, Object?>();
    }
    return null;
  }

  /// 调用原生/JS 实现链并返回插件框架回执（一次 FFI 调用）
  Map<String, Object?>? _emitAck(
    String id,
    Map<String, Object?> payload, {
    required bool sync,
    required int timeoutMs,
    MusicxxCapabilityPhase phase = MusicxxCapabilityPhase.impl,
  }) {
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> out = arena.outString();
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = phase == MusicxxCapabilityPhase.impl
          ? _runtime.bindings.musicxx_extern_plugin_capability_dispatch(
              _runtime.host,
              arena.view(id),
              arena.view(jsonEncode(payload)),
              sync ? 1 : 0,
              timeoutMs,
              out,
              log,
            )
          : _runtime.bindings.musicxx_extern_plugin_capability_dispatch_phase(
              _runtime.host,
              arena.view(id),
              phase.code,
              arena.view(jsonEncode(payload)),
              sync ? 1 : 0,
              timeoutMs,
              out,
              log,
            );
      if (rc != 0) {
        // 派发失败不阻断业务（按"没有结果"处理，只记录）
        _runtime.log(
          3,
          'capability_dispatch $id${phase == MusicxxCapabilityPhase.impl ? "" : " (${phase.wireName})"} '
          '失败: ${takeOutString(log, _runtime.bindings)}',
        );
        return null;
      }
      return decodeJsonObject(takeOutString(out, _runtime.bindings));
    } finally {
      arena.dispose();
    }
  }

  /// 判定"无需再问后续实现"（first / veto 的提前终止条件）
  static bool _isTerminal(String id, Map<String, Object?> result) {
    switch (_mergeKindOf(id)) {
      case MusicxxCapabilityMerge.first:
        return true;
      case MusicxxCapabilityMerge.veto:
        final action = result['action'];
        return action == 'cancel' || action == 'skip';
      case null:
        return false;
    }
  }

  /// 合并两份结果（按声明的 merge；later 覆盖 earlier 的标量，patch 深合并）
  ///
  /// 空结果（null 或空映射）视为"没意见"，因此两侧都空时返回 `null`
  /// —— 调用点据此走原逻辑（不能返回空映射，否则会被当成"有结果但没内容"）。
  static Map<String, Object?>? _merge(
    String id,
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
    if (_mergeKindOf(id) == MusicxxCapabilityMerge.veto) {
      // 最保守：已出现的 cancel/skip 不被后续的 continue 覆盖
      final current = merged['action'];
      final incoming = later['action'];
      if ((current == 'cancel' || current == 'skip') &&
          incoming == 'continue') {
        // 保留保守结果
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

/// 节流视图（高频能力：进度/歌词行）
///
/// 由 [MusicxxPluginCapabilities.throttle] 按 `(能力, 间隔)` 缓存复用：节流状态在实例上，
/// 每次新建实例会让节流失效（调用点不要自己 `new`，直接调 `throttle()` 即可）。
class MusicxxPluginThrottle {
  MusicxxPluginThrottle._(this._capabilities, this._id, this._minIntervalMs);

  final MusicxxPluginCapabilities _capabilities;
  final String _id;
  final int _minIntervalMs;
  int _lastMs = 0;

  /// 距上次通过不足间隔时丢弃；返回是否真正派发
  bool notify([Object? payload]) {
    final int now = DateTime.now().millisecondsSinceEpoch;
    if (now - _lastMs < _minIntervalMs) {
      return false;
    }
    _lastMs = now;
    _capabilities.notify(_id, payload);
    return true;
  }
}

class _DartImplementationEntry {
  const _DartImplementationEntry(
    this.implementation,
    this.priority,
    this.order,
    this.phase,
  );

  final MusicxxPluginImplementation implementation;
  final int priority;
  final int order;

  /// 参与方式（缺省 `impl`）
  final MusicxxCapabilityPhase phase;
}
