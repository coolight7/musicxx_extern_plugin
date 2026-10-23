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
///   → 原生/JS 处理器链（`hook_emit`，插件框架线程执行）
///   → 按 policy 合并
/// ```
/// 一个外部插件的钩子处理器（自省用：谁注册了哪个钩子、排第几）
///
/// 数据来自插件框架的 `musicxx.hook.changed` 事件（载荷里的 `handlers` 列表）与
/// `musicxx_extern_plugin_hook_handlers` 查询，字段与插件框架给出的完全一致：
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

  /// 注册时的 owner 标记（同一插件对同一钩子注册多项时用来区分）
  final String ownerTag;

  /// 参与方式：`impl` / `before` / `after`
  ///
  /// 钩子 id 表示一次调用，同一个处理器只属于一个阶段；旧插件框架库不带这个字段，
  /// 那时一律按 `impl`（见 [MusicxxPluginHooks.supportsHookPhases]）。
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

class MusicxxPluginHooks {
  MusicxxPluginHooks.internal(this._runtime);

  final MusicxxPluginRuntime _runtime;

  /// Dart 处理器：钩子 → 处理器列表（按 priority 升序，稳定）
  final Map<MusicxxPluginHookId, List<_DartHandlerEntry>> _dartHandlers =
      <MusicxxPluginHookId, List<_DartHandlerEntry>>{};

  /// 原生/JS 处理器数量快照（`钩子 id|阶段` → 数量；由 `musicxx.hook.changed` 事件维护）
  ///
  /// 判定一律按**阶段**：同一个钩子上外置插件可能只注册了 `after`，这时 `impl` 阶段是 0 ——
  /// 拿总数去判"要不要派发 impl"会让派发空跑一趟。
  final Map<String, int> _nativeCounts = <String, int>{};

  /// 原生/JS 处理器列表快照（`钩子 id|阶段` → 处理器，按调用顺序）
  ///
  /// 由 `musicxx.hook.changed` 事件维护（插件框架注册/注销时会带上列表），插件生命周期事件到达时
  /// 用 [refreshNativeHandlerCount] 向插件框架对齐一遍；旧插件框架库没有列表（事件不带 `handlers`
  /// 字段、也没有查询接口），这时这里留空、只有 [_nativeCounts] 可用（自省退化成"有几个处理器"）。
  final Map<String, List<MusicxxPluginHandlerInfo>> _nativeHandlers =
      <String, List<MusicxxPluginHandlerInfo>>{};

  /// 阶段位图是否可用（插件框架库有没有分阶段接口）
  ///
  /// 旧插件框架库没有 `hook_emit_phase` / `hook_count_phase` / `hook_handlers_phase`：
  /// 那时非 `impl` 阶段的处理器一律当作不存在（**不退回"把阶段处理器当 impl 派发"**，
  /// 那会让一个 before 处理器收到 impl 载荷，比不派发更糟）。
  bool get supportsHookPhases => true == _phasesAbiAvailable;
  bool? _phasesAbiAvailable;

  /// 计数/列表缓存的键（`钩子 id|阶段`）
  static String _phaseKey(MusicxxPluginHookId id, MusicxxPluginHookPhase phase) =>
      "${id.id}|${phase.wireName}";

  /// 异步裁决的等待方（callId → 完成器）：结果由 `musicxx.hook.decision.result` 事件回填
  final Map<int, Completer<Map<String, Object?>?>> _pendingDecisions =
      <int, Completer<Map<String, Object?>?>>{};

  /// 节流视图缓存（key = `钩子 id|间隔毫秒`）：同一组合复用同一个实例，
  /// 节流状态才能跨调用累积
  final Map<String, MusicxxPluginThrottle> _throttles =
      <String, MusicxxPluginThrottle>{};

  int _asyncDecisionCalls = 0;

  /// 某个阶段现在是否有处理器（Dart 或原生）；调用快速路径用
  ///
  /// Dart 处理器只登记在实现链（`impl`）上，所以非 `impl` 阶段只看原生/JS。
  bool hasHandlers(
    MusicxxPluginHookId id, {
    MusicxxPluginHookPhase phase = MusicxxPluginHookPhase.impl,
  }) =>
      (phase == MusicxxPluginHookPhase.impl && hasDartHandler(id)) ||
      nativeHandlerCount(id, phase: phase) > 0;

  /// 是否有 Dart 处理器
  bool hasDartHandler(MusicxxPluginHookId id) =>
      (_dartHandlers[id]?.isNotEmpty) ?? false;

  /// 原生/JS 处理器数量（缓存值；插件框架启动后由事件与首次查询填充）
  ///
  /// [phase] 缺省 = `impl`（实现链）。旧插件框架库只会把总数记在 `impl` 上。
  int nativeHandlerCount(
    MusicxxPluginHookId id, {
    MusicxxPluginHookPhase phase = MusicxxPluginHookPhase.impl,
  }) =>
      _nativeCounts[_phaseKey(id, phase)] ?? 0;

  /// 原生/JS 处理器列表（按调用顺序：priority 升序、同优先级按注册顺序）
  ///
  /// [phase] 为空 = 全部阶段（按 before → impl → after 拼接，同一阶段内仍是调用顺序）。
  /// 空列表有两种可能：这个钩子没有处理器，或者插件框架/事件没给列表（旧插件框架库）——
  /// 后者要用 [nativeHandlerCount] 判断更可靠。列表是只读快照，改它不影响派发。
  List<MusicxxPluginHandlerInfo> handlersOf(
    MusicxxPluginHookId id, {
    MusicxxPluginHookPhase? phase,
  }) {
    if (null != phase) {
      return List<MusicxxPluginHandlerInfo>.unmodifiable(
        _nativeHandlers[_phaseKey(id, phase)] ??
            const <MusicxxPluginHandlerInfo>[],
      );
    }
    final List<MusicxxPluginHandlerInfo> all = <MusicxxPluginHandlerInfo>[];
    for (final MusicxxPluginHookPhase item in const <MusicxxPluginHookPhase>[
      MusicxxPluginHookPhase.before,
      MusicxxPluginHookPhase.impl,
      MusicxxPluginHookPhase.after,
    ]) {
      all.addAll(
        _nativeHandlers[_phaseKey(id, item)] ??
            const <MusicxxPluginHandlerInfo>[],
      );
    }
    return List<MusicxxPluginHandlerInfo>.unmodifiable(all);
  }

  /// 有没有拿到过处理器列表（false 时自省只能报数量，见 [handlersOf]）
  bool get hasHandlerList => _nativeHandlers.isNotEmpty;

  /// 插件框架库有没有"查处理器列表"的接口（`musicxx_extern_plugin_hook_handlers`）
  ///
  /// 旧插件框架库没有这个符号：第一次探测失败后就固定为 false，自省与插件动作降级成
  /// "只报处理器数量"（与"未返回 callId 按无裁决处理"是同一套思路）。
  bool get supportsHandlerList => true == _handlersAbiAvailable;
  bool? _handlersAbiAvailable;

  /// 回查插件框架的处理器列表并刷新缓存（插件框架重启、丢了一条 `musicxx.hook.changed` 后手动对齐）
  ///
  /// 返回 `null` 表示这次没拿到列表（插件框架库太旧、插件框架没在跑、或钩子 id 插件框架不认识）；
  /// 此时缓存保持原样，调用方用 [nativeHandlerCount] 判断有没有处理器更可靠。
  List<MusicxxPluginHandlerInfo>? refreshNativeHandlers(
    MusicxxPluginHookId id, {
    MusicxxPluginHookPhase? phase,
  }) {
    if (!_runtime.isRunning) {
      if (null == phase) {
        for (final MusicxxPluginHookPhase item in MusicxxPluginHookPhase.values) {
          _nativeCounts[_phaseKey(id, item)] = 0;
          _nativeHandlers.remove(_phaseKey(id, item));
        }
      } else {
        _nativeCounts[_phaseKey(id, phase)] = 0;
        _nativeHandlers.remove(_phaseKey(id, phase));
      }
      return const <MusicxxPluginHandlerInfo>[];
    }
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      return _queryHandlers(id, arena, phase: phase);
    } finally {
      arena.dispose();
    }
  }

  /// 是否有任何钩子存在处理器（全局快速开关：false 时调用零成本）
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
    // 通知型只有实现链（它本来就是链的末端，没有前后链）
    if (nativeHandlerCount(id) == 0 || !_runtime.isRunning) {
      return;
    }
    _emitAsync(id, payloadMap);
  }

  /// 裁决型（同步）：Dart 处理器 → 原生/JS 处理器 → 按策略合并
  ///
  /// 返回 `null` 表示"无裁决"（调用点走原逻辑）。
  ///
  /// 插件框架不设等待超时：处理器链返回之前，本调用会一直占用调用线程（热路径上调用
  /// 就要自己判断能不能接受这一点）。
  Map<String, Object?>? decide(
    MusicxxPluginHookId id, [
    Object? payload,
  ]) {
    return decideAtPhase(id, MusicxxPluginHookPhase.impl, payload);
  }

  /// 某个阶段的裁决型派发（同步；占用调用线程）
  ///
  /// 只有 `impl` 会跑 Dart 侧处理器（`on()` 登记的都在实现链上）；`before` / `after`
  /// 是外置插件专用的阶段，而且必须是同步处理器。
  Map<String, Object?>? decideAtPhase(
    MusicxxPluginHookId id,
    MusicxxPluginHookPhase phase, [
    Object? payload,
  ]) {
    if (!id.isDecision) {
      throw ArgumentError('decide 不能用于观察型钩子: ${id.id}');
    }
    if (!active) {
      return null;
    }
    final Map<String, Object?> payloadMap = _asPayload(payload);
    Map<String, Object?>? merged;
    final Map<String, Object?>? dartResult =
        phase == MusicxxPluginHookPhase.impl
            ? _runDartHandlers(id, payloadMap)
            : null;
    if (dartResult != null) {
      merged = dartResult;
      if (id.policy == MusicxxPluginDecisionPolicy.firstNonNull) {
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
        : _verdictOf(ack);
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
  /// 与 [decide] 的区别只有"谁在等"：Dart 处理器照常就地执行；原生/JS 处理器链由插件框架在
  /// 自己的线程上跑（**不占用调用线程**），结果经 `musicxx.hook.decision.result` 事件回来
  /// 后再按 [MusicxxPluginHookId.policy] 合并。
  ///
  /// 调用点仍需自行校验 `sid` 等身份字段（切歌/切列表后到达的结果必须丢弃）。
  Future<Map<String, Object?>?> decideAsync(
    MusicxxPluginHookId id, [
    Object? payload,
  ]) {
    return decideAsyncAtPhase(id, MusicxxPluginHookPhase.impl, payload);
  }

  /// 某个阶段的裁决型派发（异步；不占用调用线程）
  ///
  /// 只有 `impl` 会跑 Dart 侧处理器；`before` / `after` 只问原生/JS 处理器。
  /// 阶段要的是"拿到这一步的结果再走下一步"，所以插件框架侧只接受同步处理器。
  Future<Map<String, Object?>?> decideAsyncAtPhase(
    MusicxxPluginHookId id,
    MusicxxPluginHookPhase phase, [
    Object? payload,
  ]) async {
    if (!id.isDecision) {
      throw ArgumentError('decideAsync 只能用于裁决型钩子: ${id.id}');
    }
    if (!active) {
      return null;
    }
    final Map<String, Object?> payloadMap = _asPayload(payload);
    Map<String, Object?>? merged;
    final Map<String, Object?>? dartResult =
        phase == MusicxxPluginHookPhase.impl
            ? _runDartHandlers(id, payloadMap)
            : null;
    if (dartResult != null) {
      merged = dartResult;
      if (id.policy == MusicxxPluginDecisionPolicy.firstNonNull) {
        return merged;
      }
    }
    if (nativeHandlerCount(id, phase: phase) == 0 || !_runtime.isRunning) {
      return merged;
    }
    final int? callId = _emitAsyncDecision(id, payloadMap, phase: phase);
    if (callId == null) {
      return merged;
    }
    final Map<String, Object?>? nativeResult = await _awaitAsyncDecision(callId);
    final Map<String, Object?>? mergedResult = _merge(id, merged, nativeResult);
    return (mergedResult == null || mergedResult.isEmpty) ? null : mergedResult;
  }

  /// 等结果回传的异步裁决数量（调试页展示；只读）
  int get pendingAsyncDecisions => _pendingDecisions.length;

  /// 已完成的异步裁决次数
  int get asyncDecisionCalls => _asyncDecisionCalls;

  /// `musicxx.hook.decision.result`：把结果交给等待中的调用点
  void handleDecisionResultEvent(MusicxxPluginEvent event) {
    final int? callId = event.intOf('callId');
    if (callId == null) {
      return;
    }
    final Completer<Map<String, Object?>?>? pending = _pendingDecisions.remove(
      callId,
    );
    if (pending == null || pending.isCompleted) {
      return; // 不属于本处理器链：直接忽略
    }
    ++_asyncDecisionCalls;
    pending.complete(_verdictOf(event.payload));
  }

  /// 刷新某钩子的原生处理器数量与列表（事件丢包/插件框架重启后手动重新查询）
  ///
  /// 计数一直走 `hook_count`（热路径判定用它）；插件框架库支持列表查询时顺带把列表对齐一遍
  /// —— 列表只服务自省，不影响派发。
  int refreshNativeHandlerCount(
    MusicxxPluginHookId id, {
    MusicxxPluginHookPhase phase = MusicxxPluginHookPhase.impl,
  }) {
    final String key = _phaseKey(id, phase);
    if (!_runtime.isRunning) {
      _nativeCounts[key] = 0;
      _nativeHandlers.remove(key);
      return 0;
    }
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final Pointer<Int32> count = arena.mallocInt();
      // 分阶段计数: 有新接口就用它; 旧插件框架库只有总数 (全部阶段), 记到 impl 上
      final bool perPhase = phase != MusicxxPluginHookPhase.impl;
      int rc;
      if (perPhase) {
        rc = _countPhase(id, phase, arena, count, log);
      } else {
        rc = _runtime.bindings.musicxx_extern_plugin_hook_count(
          _runtime.host,
          arena.view(id.id),
          count,
          log,
        );
      }
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

  /// 全部钩子的派发统计（次数/失败/平均耗时/最大耗时）
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

  /// 查询某个钩子的处理器列表并刷新缓存；插件框架库没有这个接口时返回 `null`
  ///
  /// `arena` 由调用方给出并负责释放（入参视图与出参槽都在它上面）。
  List<MusicxxPluginHandlerInfo>? _queryHandlers(
    MusicxxPluginHookId id,
    MusicxxPluginArena arena, {
    MusicxxPluginHookPhase? phase,
  }) {
    if (false == _handlersAbiAvailable) {
      return null;
    }
    final Pointer<MusicxxExternPluginString> out = arena.outString();
    final Pointer<MusicxxExternPluginString> log = arena.outString();
    int rc;
    try {
      if (null == phase) {
        rc = _runtime.bindings.musicxx_extern_plugin_hook_handlers(
          _runtime.host,
          arena.view(id.id),
          out,
          log,
        );
      } else {
        rc = _handlersPhase(
          id,
          phase,
          arena,
          out,
          log,
        );
      }
    } catch (_) {
      // 旧插件框架库没有这个符号：探测一次就够，之后一律按"只有计数"处理
      _handlersAbiAvailable = false;
      return null;
    }
    _handlersAbiAvailable = true;
    final Map<String, Object?> data = decodeJsonObject(
      takeOutString(out, _runtime.bindings),
    );
    if (rc != 0) {
      // 钩子 id 插件框架不认识等：保持缓存原样（不把"查不到"当成"没有处理器"）
      return null;
    }
    return _applyHandlerList(id, data, phase: phase);
  }

  /// `hook_handlers_phase`（旧插件框架库没有这个符号时返回 `-4`）
  int _handlersPhase(
    MusicxxPluginHookId id,
    MusicxxPluginHookPhase phase,
    MusicxxPluginArena arena,
    Pointer<MusicxxExternPluginString> out,
    Pointer<MusicxxExternPluginString> log,
  ) {
    if (false == _phasesAbiAvailable) {
      return -4;
    }
    try {
      return _runtime.bindings.musicxx_extern_plugin_hook_handlers_phase(
        _runtime.host,
        arena.view(id.id),
        phase.code,
        out,
        log,
      );
    } catch (_) {
      _phasesAbiAvailable = false;
      return -4;
    }
  }

  /// `hook_count_phase`（旧插件框架库没有这个符号时返回 `-4`）
  int _countPhase(
    MusicxxPluginHookId id,
    MusicxxPluginHookPhase phase,
    MusicxxPluginArena arena,
    Pointer<Int32> count,
    Pointer<MusicxxExternPluginString> log,
  ) {
    if (false == _phasesAbiAvailable) {
      return -4;
    }
    try {
      return _runtime.bindings.musicxx_extern_plugin_hook_count_phase(
        _runtime.host,
        arena.view(id.id),
        phase.code,
        count,
        log,
      );
    } catch (_) {
      _phasesAbiAvailable = false;
      return -4;
    }
  }

  /// 把插件框架给出的 `{hook, count, handlers}` 写进缓存，返回解析出的列表
  List<MusicxxPluginHandlerInfo> _applyHandlerList(
    MusicxxPluginHookId id,
    Map<String, Object?> data, {
    MusicxxPluginHookPhase? phase,
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
    // 全部阶段: 按每项自报的 phase 分组写回（旧插件框架库没有 phase 字段 → 全算 impl）
    _writePhaseGroups(id, list);
    return List<MusicxxPluginHandlerInfo>.unmodifiable(list);
  }

  /// 把一份"全部阶段"的处理器列表按阶段分组写进缓存
  void _writePhaseGroups(
    MusicxxPluginHookId id,
    List<MusicxxPluginHandlerInfo> list, {
    Map<String, Object?>? phaseCount,
  }) {
    for (final MusicxxPluginHookPhase phase in MusicxxPluginHookPhase.values) {
      _nativeHandlers.remove(_phaseKey(id, phase));
    }
    final Map<String, List<MusicxxPluginHandlerInfo>> groups =
        <String, List<MusicxxPluginHandlerInfo>>{};
    for (final MusicxxPluginHandlerInfo handler in list) {
      groups
          .putIfAbsent(
            handler.phase,
            () => <MusicxxPluginHandlerInfo>[],
          )
          .add(handler);
    }
    for (final MusicxxPluginHookPhase phase in MusicxxPluginHookPhase.values) {
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
    _phasesAbiAvailable = null;
    _throttles.clear();
    // 插件框架已停：还没结算的异步裁决一律按"无裁决"收尾，避免调用点永久悬挂
    for (final Completer<Map<String, Object?>?> completer
        in _pendingDecisions.values) {
      if (!completer.isCompleted) {
        completer.complete(null);
      }
    }
    _pendingDecisions.clear();
  }

  /// `musicxx.hook.changed`：维护"该钩子是否有原生处理器"的位图与处理器列表
  void handleHookChangedEvent(MusicxxPluginEvent event) {
    final String? hook = event.stringOf('hook');
    if (hook == null || hook.isEmpty) {
      return;
    }
    final MusicxxPluginHookId? id = MusicxxPluginHookId.tryFromId(hook);
    if (null == id) {
      return;
    }
    // 计数一律按阶段: 事件带 `phaseCount` 就用它（旧插件框架库没有这个字段 → 总数记到 impl）
    final Object? rawPhaseCount = event.payload['phaseCount'];
    final Map<String, Object?>? phaseCount = rawPhaseCount is Map
        ? rawPhaseCount.cast<String, Object?>()
        : null;
    // 插件框架注册/注销时一并给出处理器列表（谁注册的、排第几、在哪个阶段）；
    // 旧插件框架库没有这个字段，那时只对齐计数，自省退化成"只知道有几个处理器"
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
      _writePhaseGroups(id, list, phaseCount: phaseCount);
      return;
    }
    final int count = event.intOf('count') ?? 0;
    if (null != phaseCount) {
      for (final MusicxxPluginHookPhase phase in MusicxxPluginHookPhase.values) {
        final Object? declared = phaseCount[phase.wireName];
        _nativeCounts[_phaseKey(id, phase)] = declared is num
            ? (declared.toInt() < 0 ? 0 : declared.toInt())
            : 0;
      }
      return;
    }
    // 没有分阶段信息: 总数只能算在 impl 上（阶段派发在旧库上本来也不支持）
    _nativeCounts[_phaseKey(id, MusicxxPluginHookPhase.impl)] = count < 0
        ? 0
        : count;
    for (final MusicxxPluginHookPhase phase in MusicxxPluginHookPhase.values) {
      if (phase != MusicxxPluginHookPhase.impl) {
        _nativeCounts[_phaseKey(id, phase)] = 0;
      }
    }
  }

  /// 插件生命周期事件：加载/启用/卸载会批量改变处理器数量 → 用插件框架的权威值对齐一遍
  ///
  /// 计数平时靠 `musicxx.hook.changed` 维护（每次注册/注销一条）；它一旦丢一条就会停在
  /// 旧值——"有处理器却被当成没有"会让该钩子静默失效，所以每次插件生命周期变化都用插件框架
  /// 里的真实数量覆盖一次：载荷带 `hooks` 时只查这些钩子（新插件框架），否则把主程序已接入的
  /// 钩子全部查一遍（旧插件框架库，行为与以前一致）。
  void handlePluginLifecycleEvent(MusicxxPluginEvent event) {
    if (!_runtime.isRunning) {
      return;
    }
    final List<MusicxxPluginHookId> targets = <MusicxxPluginHookId>[];
    final hooks = event.payload['hooks'];
    if (hooks is List && hooks.isNotEmpty) {
      for (final hook in hooks) {
        if (hook is String) {
          final MusicxxPluginHookId? id = MusicxxPluginHookId.tryFromId(hook);
          if (null != id) {
            targets.add(id);
          }
        }
      }
    } else {
      for (final MusicxxPluginHookId id in MusicxxPluginHookId.values) {
        if (id.wired) {
          targets.add(id);
        }
      }
    }
    for (final MusicxxPluginHookId id in targets) {
      // 每个阶段都对齐一次：只对齐 impl 会让"只注册了 after"的钩子被当成没有处理器
      for (final MusicxxPluginHookPhase phase
          in MusicxxPluginHookPhase.values) {
        refreshNativeHandlerCount(id, phase: phase);
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

  /// 观察型派发（不做回执解析）
  ///
  /// 插件框架收到的是"入队即返回"（`sync = 0`）：这里不等结果，回执内容也没有读者，
  /// 所以只把出参内存释放掉，不做 JSON 解析（观察型在热路径上，能省一次解析）。
  void _emitAsync(MusicxxPluginHookId id, Map<String, Object?> payload) {
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> out = arena.outString();
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings.musicxx_extern_plugin_hook_emit(
        _runtime.host,
        arena.view(id.id),
        arena.view(jsonEncode(payload)),
        0,
        0,
        out,
        log,
      );
      if (rc != 0) {
        // 派发失败不阻断业务（按"无插件"处理，只记录）
        _runtime.log(
          3,
          'hook_emit ${id.id} 失败: ${takeOutString(log, _runtime.bindings)}',
        );
      }
      discardOutString(out, _runtime.bindings);
    } finally {
      arena.dispose();
    }
  }

  /// 发起一次异步裁决派发，返回配对的 `callId`（无处理器/失败返回 `null`）
  int? _emitAsyncDecision(
    MusicxxPluginHookId id,
    Map<String, Object?> payload, {
    MusicxxPluginHookPhase phase = MusicxxPluginHookPhase.impl,
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
    return callId is num ? callId.toInt() : null; // 未返回 callId（旧库）时按"无裁决"处理
  }

  /// 等待异步裁决结果（由 `musicxx.hook.decision.result` 事件回填；没有等待超时）
  Future<Map<String, Object?>?> _awaitAsyncDecision(int callId) {
    final Completer<Map<String, Object?>?> completer =
        Completer<Map<String, Object?>?>();
    _pendingDecisions[callId] = completer;
    return completer.future;
  }

  /// 从插件框架回执里取出裁决对象（`{"handled":..,"result":{...}}`）
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

  /// 调用原生/JS 处理器链并返回插件框架回执（一次 FFI 调用）
  Map<String, Object?>? _emitAck(
    MusicxxPluginHookId id,
    Map<String, Object?> payload, {
    required bool sync,
    required int timeoutMs,
    MusicxxPluginHookPhase phase = MusicxxPluginHookPhase.impl,
  }) {
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> out = arena.outString();
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      int rc;
      if (phase == MusicxxPluginHookPhase.impl) {
        rc = _runtime.bindings.musicxx_extern_plugin_hook_emit(
          _runtime.host,
          arena.view(id.id),
          arena.view(jsonEncode(payload)),
          sync ? 1 : 0,
          timeoutMs,
          out,
          log,
        );
      } else {
        rc = _emitPhase(id, phase, payload, arena, sync, timeoutMs, out, log);
      }
      if (rc != 0) {
        // 派发失败不阻断业务（按"无裁决"处理，只记录）
        _runtime.log(
          3,
          'hook_emit ${id.id}${phase == MusicxxPluginHookPhase.impl ? "" : " (${phase.wireName})"} '
          '失败: ${takeOutString(log, _runtime.bindings)}',
        );
        return null;
      }
      return decodeJsonObject(takeOutString(out, _runtime.bindings));
    } finally {
      arena.dispose();
    }
  }

  /// `hook_emit_phase`（旧插件框架库没有这个符号时返回 `-4`）
  int _emitPhase(
    MusicxxPluginHookId id,
    MusicxxPluginHookPhase phase,
    Map<String, Object?> payload,
    MusicxxPluginArena arena,
    bool sync,
    int timeoutMs,
    Pointer<MusicxxExternPluginString> out,
    Pointer<MusicxxExternPluginString> log,
  ) {
    if (false == _phasesAbiAvailable) {
      return -4;
    }
    try {
      return _runtime.bindings.musicxx_extern_plugin_hook_emit_phase(
        _runtime.host,
        arena.view(id.id),
        phase.code,
        arena.view(jsonEncode(payload)),
        sync ? 1 : 0,
        timeoutMs,
        out,
        log,
      );
    } catch (_) {
      _phasesAbiAvailable = false;
      return -4;
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
