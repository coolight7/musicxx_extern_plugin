import 'dart:convert';
import 'dart:ffi';

import 'bindings_generated.dart';
import 'capability_ids.g.dart';
import 'events.dart';
import 'native_strings.dart';
import 'requests.dart';
import 'runtime.dart';

/// 一个外部插件的能力处理器（自省用：谁实现了哪条能力、在哪个阶段、排第几）
///
/// 数据来自插件框架的 `musicxx.capability.changed` 事件（载荷里的 `handlers` 列表）与
/// `capability.handlers` 提交，字段与插件框架给出的完全一致：
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

/// 能力派发（原生 / JS 实现链）
///
/// 执行顺序：
/// ```
/// 快速路径（本阶段没有处理器 → 直接返回，0 次 FFI、0 次 JSON 构造）
///   → 一次提交（`capability.dispatch`）
///   → 插件框架在插件执行线程上按阶段与 priority 逐个问处理器
///   → 按声明的 merge 合并后回一条结果事件
/// ```
///
/// 一条 id 是能力还是广播由声明决定（`capability_ids.g.dart`）：广播只通知、没有结果，
/// 所以 [notify] 走广播、[dispatch] 走能力，用错会抛 [ArgumentError]。
///
/// **处理器数量与列表走 Dart 缓存**（由 `musicxx.capability.changed` 与
/// [refreshNativeHandlers] 维持）：频繁执行的路径不为了问一句"有没有处理器"去等一次往返。
class MusicxxPluginCapabilities {
  MusicxxPluginCapabilities.internal(this._runtime);

  final MusicxxPluginRuntime _runtime;

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

  /// 节流视图缓存（key = `能力 id|间隔毫秒`）：同一组合复用同一个实例，
  /// 节流状态才能跨调用累积
  final Map<String, MusicxxPluginThrottle> _throttles =
      <String, MusicxxPluginThrottle>{};

  int _dispatchCalls = 0;

  /// 某个能力现在有没有实现（调用快速路径用）
  bool hasHandlers(
    String id, {
    MusicxxCapabilityPhase phase = MusicxxCapabilityPhase.impl,
  }) => nativeHandlerCount(id, phase: phase) > 0;

  /// 原生/JS 处理器数量（缓存值；事件与回查填充）
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

  /// 回查插件框架的处理器列表并刷新缓存
  ///
  /// 用途：插件框架重启、怀疑丢了一条 `musicxx.capability.changed` 之后手动对齐。
  /// 返回 `null` 表示这次没拿到列表（插件框架没在跑、或插件框架不认识这条 id），
  /// 此时缓存保持原样。
  Future<List<MusicxxPluginHandlerInfo>?> refreshNativeHandlers(
    String id, {
    MusicxxCapabilityPhase? phase,
  }) async {
    if (!_runtime.isRunning) {
      _clearNative(id, phase: phase);
      return const <MusicxxPluginHandlerInfo>[];
    }
    final MusicxxPluginRequestResult result = null == phase
        ? await _runtime.requests.submit(
            'capability.handlers',
            (
              MusicxxPluginArena arena,
              Pointer<Int64> outId,
              Pointer<MusicxxExternPluginString> log,
            ) => _runtime.bindings.musicxx_extern_plugin_capability_handlers(
              _runtime.host,
              arena.view(id),
              outId,
              log,
            ),
          )
        : await _runtime.requests.submit(
            'capability.handlers',
            (
              MusicxxPluginArena arena,
              Pointer<Int64> outId,
              Pointer<MusicxxExternPluginString> log,
            ) => _runtime.bindings
                .musicxx_extern_plugin_capability_handlers_phase(
                  _runtime.host,
                  arena.view(id),
                  phase.code,
                  outId,
                  log,
                ),
          );
    if (!result.ok) {
      return null;
    }

    /// 插件框架的回答是 `{capability, count, handlers}`；只给数组时也能用
    final Object? raw = result.data;
    final Object? listRaw = raw is Map ? raw['handlers'] : raw;
    final List<MusicxxPluginHandlerInfo> list = <MusicxxPluginHandlerInfo>[];
    if (listRaw is List) {
      for (final Object? item in listRaw) {
        if (item is Map<Object?, Object?>) {
          list.add(
            MusicxxPluginHandlerInfo.fromJson(item.cast<String, Object?>()),
          );
        }
      }
    }
    _applyHandlerList(id, list, phase: phase);
    return List<MusicxxPluginHandlerInfo>.unmodifiable(list);
  }

  /// 是否有任何能力存在实现（全局快速开关：false 时调用零成本）
  bool get active =>
      _nativeCounts.values.any((int v) => v > 0) || _nativeHandlers.isNotEmpty;

  /// 广播：所有者改了东西，通知所有关心的人（回一条确认，需要顺序时可以 `await`）
  ///
  /// 广播型 id 由插件框架按声明处理：只跑处理器、不做结果合并。
  /// 不关心什么时候跑完就 `.ignore()`。
  Future<void> notify(String id, [Object? payload]) async {
    if (null == MusicxxCapabilityCatalogue.broadcastOf(id)) {
      throw ArgumentError('notify is only valid for broadcasts: $id');
    }
    if (nativeHandlerCount(id) == 0 || !_runtime.isRunning) {
      return;
    }
    await dispatch(id, payload);
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

  /// 调用一条能力（要结果）
  ///
  /// - 返回合并后的裁决（插件给的对象，例如 `{patch: …, action: …}`）；`null` = 没有实现
  ///   给出结果，调用点走原逻辑；
  /// - 原生/JS 实现链由插件框架在**插件执行线程**上跑，不占用调用线程；
  /// - 派发失败（插件框架没跑、参数错、排队满）按"没有结果"返回，不抛异常；
  /// - 调用点仍需自行校验 `sid` 等身份字段（切歌/切列表后到达的结果必须丢弃）。
  Future<Map<String, Object?>?> dispatch(
    String id, [
    Object? payload,
    MusicxxCapabilityPhase phase = MusicxxCapabilityPhase.impl,
  ]) async {
    if (null != MusicxxCapabilityCatalogue.broadcastOf(id)) {
      throw ArgumentError(
        'dispatch cannot be used for a broadcast: $id (use notify)',
      );
    }
    if (!active || !_runtime.isRunning) {
      return null;
    }
    if (nativeHandlerCount(id, phase: phase) == 0) {
      return null;
    }
    final String body = jsonEncode(_asPayload(payload));
    final MusicxxPluginRequestResult result =
        phase == MusicxxCapabilityPhase.impl
        ? await _runtime.requests.submit(
            'capability.dispatch',
            (
              MusicxxPluginArena arena,
              Pointer<Int64> outId,
              Pointer<MusicxxExternPluginString> log,
            ) => _runtime.bindings.musicxx_extern_plugin_capability_dispatch(
              _runtime.host,
              arena.view(id),
              arena.view(body),
              outId,
              log,
            ),
          )
        : await _runtime.requests.submit(
            'capability.dispatch',
            (
              MusicxxPluginArena arena,
              Pointer<Int64> outId,
              Pointer<MusicxxExternPluginString> log,
            ) => _runtime.bindings
                .musicxx_extern_plugin_capability_dispatch_phase(
                  _runtime.host,
                  arena.view(id),
                  phase.code,
                  arena.view(body),
                  outId,
                  log,
                ),
          );
    if (!result.ok) {
      return null;
    }
    ++_dispatchCalls;
    final Object? merged = result.dataObject['result'];
    if (merged is Map<Object?, Object?>) {
      return merged.cast<String, Object?>();
    }
    return null;
  }

  /// 已完成的派发次数（自省/调试用）
  int get dispatchCalls => _dispatchCalls;

  /// 资源与耗时统计快照（各阶段耗时 / 能力耗时 / 注册项 / 计数器；只统计不限制）
  Future<Map<String, Object?>> stats() async {
    if (!_runtime.isRunning) {
      return const <String, Object?>{};
    }
    final MusicxxPluginRequestResult result = await _runtime.requests.submit(
      'capability.stats',
      (
        MusicxxPluginArena arena,
        Pointer<Int64> outId,
        Pointer<MusicxxExternPluginString> log,
      ) => _runtime.bindings.musicxx_extern_plugin_capability_stats(
        _runtime.host,
        outId,
        log,
      ),
    );
    return result.dataObject;
  }

  /// `musicxx.capability.changed`：按载荷重建**这一条能力**的缓存
  ///
  /// 一条事件只带一条能力的全部处理器（注册/注销是低频操作），所以直接整条替换。
  void handleCapabilityChangedEvent(MusicxxPluginEvent event) {
    final String capability = event.stringOf('capability') ?? '';
    if (capability.isEmpty) {
      return;
    }
    _clearNative(capability);
    final Object? handlers = event.payload['handlers'];
    if (handlers is! List) {
      return;
    }
    for (final Object? item in handlers) {
      if (item is! Map<Object?, Object?>) {
        continue;
      }
      final MusicxxPluginHandlerInfo info = MusicxxPluginHandlerInfo.fromJson(
        item.cast<String, Object?>(),
      );
      final MusicxxCapabilityPhase phase =
          MusicxxCapabilityPhase.tryFromName(info.phase) ??
          MusicxxCapabilityPhase.impl;
      final String key = _phaseKey(capability, phase);
      _nativeCounts[key] = (_nativeCounts[key] ?? 0) + 1;
      _nativeHandlers
          .putIfAbsent(key, () => <MusicxxPluginHandlerInfo>[])
          .add(info);
    }
  }

  /// 插件卸载 / 停用：把它的处理器从缓存里摘掉
  ///
  /// 多数情况下插件框架随后会补一条 `musicxx.capability.changed`（见
  /// [handleCapabilityChangedEvent]）；这条兜底是为了"插件已经不在、缓存里还留着它"。
  void handlePluginLifecycleEvent(MusicxxPluginEvent event) {
    final bool removing =
        event.type == MusicxxPluginEventType.pluginUnloaded ||
        event.type == MusicxxPluginEventType.pluginDisabled ||
        event.type == MusicxxPluginEventType.pluginError;
    if (!removing) {
      return;
    }
    final String plugin = event.plugin;
    if (plugin.isEmpty) {
      return;
    }
    for (final String key in _nativeHandlers.keys.toList(growable: false)) {
      final List<MusicxxPluginHandlerInfo> list = _nativeHandlers[key]!;
      final List<MusicxxPluginHandlerInfo> kept = list
          .where((MusicxxPluginHandlerInfo info) => info.plugin != plugin)
          .toList(growable: false);
      if (kept.length == list.length) {
        continue;
      }
      if (kept.isEmpty) {
        _nativeHandlers.remove(key);
        _nativeCounts.remove(key);
      } else {
        _nativeHandlers[key] = kept;
        _nativeCounts[key] = kept.length;
      }
    }
  }

  void handleDisposed() {
    _nativeCounts.clear();
    _nativeHandlers.clear();
    _throttles.clear();
    _dispatchCalls = 0;
  }

  void _applyHandlerList(
    String id,
    List<MusicxxPluginHandlerInfo> list, {
    MusicxxCapabilityPhase? phase,
  }) {
    if (null != phase) {
      final String key = _phaseKey(id, phase);
      if (list.isEmpty) {
        _nativeHandlers.remove(key);
        _nativeCounts.remove(key);
      } else {
        _nativeHandlers[key] = list;
        _nativeCounts[key] = list.length;
      }
      return;
    }
    _clearNative(id);
    for (final MusicxxPluginHandlerInfo info in list) {
      final MusicxxCapabilityPhase item =
          MusicxxCapabilityPhase.tryFromName(info.phase) ??
          MusicxxCapabilityPhase.impl;
      final String key = _phaseKey(id, item);
      _nativeCounts[key] = (_nativeCounts[key] ?? 0) + 1;
      _nativeHandlers
          .putIfAbsent(key, () => <MusicxxPluginHandlerInfo>[])
          .add(info);
    }
  }

  void _clearNative(String id, {MusicxxCapabilityPhase? phase}) {
    if (null != phase) {
      final String key = _phaseKey(id, phase);
      _nativeHandlers.remove(key);
      _nativeCounts.remove(key);
      return;
    }
    for (final MusicxxCapabilityPhase item in MusicxxCapabilityPhase.values) {
      final String key = _phaseKey(id, item);
      _nativeHandlers.remove(key);
      _nativeCounts.remove(key);
    }
  }

  static Map<String, Object?> _asPayload(Object? payload) {
    if (payload == null) {
      return <String, Object?>{};
    }
    if (payload is Map<String, Object?>) {
      return payload;
    }
    if (payload is Map<Object?, Object?>) {
      return payload.cast<String, Object?>();
    }
    return <String, Object?>{'value': payload};
  }
}

/// 高频能力的节流包装（[MusicxxPluginCapabilities.throttle] 返回）
class MusicxxPluginThrottle {
  MusicxxPluginThrottle._(this._capabilities, this._id, this._minIntervalMs);

  final MusicxxPluginCapabilities _capabilities;
  final String _id;
  final int _minIntervalMs;
  int _lastMs = 0;

  /// 按最小间隔派发一次（太频繁时直接跳过）
  ///
  /// 返回 `true` = 这次真的发出去了，`false` = 被节流丢掉。
  /// 这是通知型用法：不等结果，失败只影响日志。
  bool notify([Object? payload]) {
    final int now = DateTime.now().millisecondsSinceEpoch;
    if (now - _lastMs < _minIntervalMs) {
      return false;
    }
    _lastMs = now;
    _capabilities.notify(_id, payload).ignore();
    return true;
  }
}
