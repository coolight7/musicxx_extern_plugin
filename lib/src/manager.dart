import 'dart:convert';
import 'dart:ffi';

import 'bindings_generated.dart';
import 'events.dart';
import 'native_strings.dart';
import 'plugin_info.dart';
import 'ui.dart';
import 'runtime.dart';

/// 原生调用出参读取器（统一的 arena/错误串/JSON 处理）
typedef _OutCall =
    int Function(
      MusicxxExternPluginBindings bindings,
      Pointer<MusicxxExternPluginString> out,
      Pointer<MusicxxExternPluginString> log,
    );

/// 插件管理（Dart → 原生宿主的插件生命周期与查询）
///
/// 全部方法都是**同步 FFI 调用**（有等待上界）：装载/卸载在宿主线程异步完成，
/// 这里只做有界等待；长时间操作（安装压缩包等）留给 S4 的隔离 isolate。
class MusicxxPluginManager {
  MusicxxPluginManager.internal(this._runtime);

  final MusicxxPluginRuntime _runtime;

  /// 已加载插件缓存（由事件维护为"标脏 + 重查"，权威值来自宿主快照）
  List<MusicxxPluginInfo> _cache = const <MusicxxPluginInfo>[];
  final Map<String, MusicxxPluginInfo> _byId = <String, MusicxxPluginInfo>{};

  /// 最近一次扫描结果
  List<MusicxxPluginInfo> _scanned = const <MusicxxPluginInfo>[];

  /// 是否处于安全模式（本次运行不加载外部插件）
  bool get safeMode => _runtime.config?.safeMode ?? false;

  /// 已加载插件（缓存快照；权威结果请调用 [list]）
  List<MusicxxPluginInfo> get loaded =>
      List<MusicxxPluginInfo>.unmodifiable(_cache);

  /// 最近一次扫描结果
  List<MusicxxPluginInfo> get discovered =>
      List<MusicxxPluginInfo>.unmodifiable(_scanned);

  /// 扫描插件目录（用户目录 + 随包目录）
  ///
  /// 返回全部发现的清单（含 `valid/supported/reason`，管理页据此标注"为什么不能用"）。
  List<MusicxxPluginInfo> scan() {
    if (!_runtime.isRunning) {
      return const <MusicxxPluginInfo>[];
    }
    final List<MusicxxPluginInfo> result = _readList(
      'plugin_scan',
      (
        MusicxxExternPluginBindings b,
        Pointer<MusicxxExternPluginString> out,
        Pointer<MusicxxExternPluginString> log,
      ) => b.musicxx_extern_plugin_plugin_scan(_runtime.host, out, log),
    );
    _scanned = result;
    return result;
  }

  /// 当前已加载插件状态快照
  List<MusicxxPluginInfo> list() {
    if (!_runtime.isRunning) {
      return const <MusicxxPluginInfo>[];
    }
    final List<MusicxxPluginInfo> result = _readList(
      'plugin_list',
      (
        MusicxxExternPluginBindings b,
        Pointer<MusicxxExternPluginString> out,
        Pointer<MusicxxExternPluginString> log,
      ) => b.musicxx_extern_plugin_plugin_list(_runtime.host, out, log),
    );
    _cache = result;
    _byId
      ..clear()
      ..addEntries(
        result.map(
          (MusicxxPluginInfo info) =>
              MapEntry<String, MusicxxPluginInfo>(info.id, info),
        ),
      );
    return result;
  }

  /// 按 id 查已加载插件（缓存）
  MusicxxPluginInfo? findLoaded(String id) => _byId[id];

  /// 装载插件（同步等待，带超时）
  ///
  /// - [idOrPath] 传插件 id（扫描结果的 `id`）或插件目录/库文件路径；
  /// - [args] 作为插件参数下发（插件经 `pluginxx.config` 的 `get_plugin_args` 读取）；
  /// - 结果事件：`musicxx.plugin.loaded` / `musicxx.plugin.error`。
  void load(
    String idOrPath, {
    Map<String, Object?>? args,
    Duration timeout = const Duration(seconds: 10),
  }) {
    _requireRunning('plugin_load_sync');
    final String options = _optionsJson(args);
    _withArena((
      MusicxxPluginArena arena,
      Pointer<MusicxxExternPluginString> log,
    ) {
      final int rc = _runtime.bindings.musicxx_extern_plugin_plugin_load_sync(
        _runtime.host,
        arena.view(idOrPath),
        arena.view(options),
        timeout.inMilliseconds,
        log,
      );
      _runtime.checkOrThrow(rc, log, 'plugin_load_sync');
    });
    list();
  }

  /// 异步装载（结果只经事件回报；适合批量启动时避免逐个等待）
  void loadAsync(String idOrPath, {Map<String, Object?>? args}) {
    _requireRunning('plugin_load');
    final String options = _optionsJson(args);
    _withArena((
      MusicxxPluginArena arena,
      Pointer<MusicxxExternPluginString> log,
    ) {
      final int rc = _runtime.bindings.musicxx_extern_plugin_plugin_load(
        _runtime.host,
        arena.view(idOrPath),
        arena.view(options),
        log,
      );
      _runtime.checkOrThrow(rc, log, 'plugin_load');
    });
  }

  /// 启用插件（重新执行插件 `start` 事务，插件借此重新注册钩子/能力/订阅）
  void enable(String id) {
    _requireRunning('plugin_enable');
    _withArena((
      MusicxxPluginArena arena,
      Pointer<MusicxxExternPluginString> log,
    ) {
      final int rc = _runtime.bindings.musicxx_extern_plugin_plugin_enable(
        _runtime.host,
        arena.view(id),
        log,
      );
      _runtime.checkOrThrow(rc, log, 'plugin_enable');
    });
    list();
  }

  /// 禁用插件（摘除注册并调用插件 `stop`；实例保留在内存，可再 [enable]）
  void disable(String id) {
    _requireRunning('plugin_disable');
    _withArena((
      MusicxxPluginArena arena,
      Pointer<MusicxxExternPluginString> log,
    ) {
      final int rc = _runtime.bindings.musicxx_extern_plugin_plugin_disable(
        _runtime.host,
        arena.view(id),
        log,
      );
      _runtime.checkOrThrow(rc, log, 'plugin_disable');
    });
    list();
  }

  /// 卸载插件（释放实例与动态库；宿主侧卸载是幂等的）
  void unload(String id) {
    _requireRunning('plugin_unload');
    _withArena((
      MusicxxPluginArena arena,
      Pointer<MusicxxExternPluginString> log,
    ) {
      final int rc = _runtime.bindings.musicxx_extern_plugin_plugin_unload(
        _runtime.host,
        arena.view(id),
        log,
      );
      _runtime.checkOrThrow(rc, log, 'plugin_unload');
    });
    list();
  }

  /// 卸载后按当前配置重新装载
  void reload(String id) {
    _requireRunning('plugin_reload');
    _withArena((
      MusicxxPluginArena arena,
      Pointer<MusicxxExternPluginString> log,
    ) {
      final int rc = _runtime.bindings.musicxx_extern_plugin_plugin_reload(
        _runtime.host,
        arena.view(id),
        log,
      );
      _runtime.checkOrThrow(rc, log, 'plugin_reload');
    });
    list();
  }

  /// 更新插件参数（下次 `reload` 生效；由调用方决定何时重载）
  void setArgs(String id, Map<String, Object?> args) {
    _requireRunning('plugin_set_args');
    final String json = _optionsJson(args);
    _withArena((
      MusicxxPluginArena arena,
      Pointer<MusicxxExternPluginString> log,
    ) {
      final int rc = _runtime.bindings.musicxx_extern_plugin_plugin_set_args(
        _runtime.host,
        arena.view(id),
        arena.view(json),
        log,
      );
      _runtime.checkOrThrow(rc, log, 'plugin_set_args');
    });
  }

  /// 插件配置文件路径（管理页/插件自读配置用）
  String configPath(String id) {
    _requireRunning('plugin_get_config_path');
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> out = arena.outString();
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings
          .musicxx_extern_plugin_plugin_get_config_path(
            _runtime.host,
            arena.view(id),
            out,
            log,
          );
      _runtime.checkOrThrow(rc, log, 'plugin_get_config_path');
      return takeOutString(out, _runtime.bindings);
    } finally {
      arena.dispose();
    }
  }

  /// 调用插件能力（Dart → 插件）：native/JS 统一用 `plugin.<插件id>.<能力名>`
  ///
  /// - 能力必须在插件 `start` 事务里声明（`pluginxx.capabilities` 表）；
  /// - 归属校验由宿主完成（不能调用他人命名空间）；
  /// - 超时抛 [MusicxxPluginApiException]（code = -5）。
  Object? call(
    String id,
    String method, [
    Map<String, Object?> args = const <String, Object?>{},
    Duration timeout = const Duration(seconds: 5),
  ]) {
    _requireRunning('plugin_call');
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> out = arena.outString();
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings.musicxx_extern_plugin_plugin_call(
        _runtime.host,
        arena.view(id),
        arena.view(method),
        arena.view(_optionsJson(args)),
        timeout.inMilliseconds,
        out,
        log,
      );
      _runtime.checkOrThrow(rc, log, 'plugin_call');
      return tryDecodeJson(takeOutString(out, _runtime.bindings));
    } finally {
      arena.dispose();
    }
  }

  /// 资源与耗时统计快照（阶段耗时/钩子耗时/注册项/计数器；只观测不限制）
  Map<String, Object?> stats({Map<String, Object?>? scope}) {
    _requireRunning('stats');
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginStringView> scopeView = scope == null
          ? nullptr
          : arena.view(_optionsJson(scope));
      final Pointer<MusicxxExternPluginString> out = arena.outString();
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings.musicxx_extern_plugin_stats(
        _runtime.host,
        scopeView,
        out,
        log,
      );
      _runtime.checkOrThrow(rc, log, 'stats');
      return decodeJsonObject(takeOutString(out, _runtime.bindings));
    } finally {
      arena.dispose();
    }
  }

  /// 插件贡献的 UI 项全量快照（声明式 UI 扩展）
  ///
  /// 变更会额外推送 `musicxx.ui.changed` 事件（载荷带该插件的全部项），
  /// 应用侧按插件整批替换即可（见 [MusicxxPluginUIItems.replacePlugin]）。
  List<MusicxxPluginUIItem> uiSnapshot() {
    _requireRunning('ui_snapshot');
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> out = arena.outString();
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings.musicxx_extern_plugin_ui_snapshot(
        _runtime.host,
        out,
        log,
      );
      _runtime.checkOrThrow(rc, log, 'ui_snapshot');
      final String json = takeOutString(out, _runtime.bindings);
      if (json.isEmpty) {
        return const <MusicxxPluginUIItem>[];
      }
      final decoded = tryDecodeJson(json);
      return MusicxxPluginUIItem.parseList(decoded);
    } finally {
      arena.dispose();
    }
  }

  /// 统计采集配置（`{"enable":false}` 可整体关闭采集）
  void statsConfig(Map<String, Object?> cfg) {
    _requireRunning('stats_config');
    final String json = _optionsJson(cfg);
    _withArena((
      MusicxxPluginArena arena,
      Pointer<MusicxxExternPluginString> log,
    ) {
      final int rc = _runtime.bindings.musicxx_extern_plugin_stats_config(
        _runtime.host,
        arena.view(json),
        log,
      );
      _runtime.checkOrThrow(rc, log, 'stats_config');
    });
  }

  /// 某插件的日志流（来自 `musicxx.plugin.log` 事件）
  Stream<MusicxxPluginLog> logs(String id) => _runtime.events
      .where(
        (MusicxxPluginEvent event) =>
            event.type == MusicxxPluginEventType.pluginLog &&
            event.plugin == id,
      )
      .map(MusicxxPluginLog.fromEvent);

  // ==================== 内部 ====================

  void handleDisposed() {
    _cache = const <MusicxxPluginInfo>[];
    _byId.clear();
    _scanned = const <MusicxxPluginInfo>[];
  }

  /// 生命周期事件：只做"标脏 + 重查"（避免事件顺序与快照不一致导致状态错乱）
  void handleLifecycleEvent(MusicxxPluginEvent event) {
    if (!_runtime.isRunning) {
      return;
    }
    final String id = event.plugin;
    if (id.isNotEmpty &&
        event.type == MusicxxPluginEventType.pluginUnloaded &&
        _byId.containsKey(id)) {
      _byId.remove(id);
      _cache = _cache
          .where((MusicxxPluginInfo info) => info.id != id)
          .toList(growable: false);
    }
    list();
  }

  void _requireRunning(String operation) {
    if (!_runtime.isRunning) {
      throw MusicxxPluginApiException(-2, '宿主未启动', operation: operation);
    }
  }

  static String _optionsJson(Map<String, Object?>? value) {
    if (value == null || value.isEmpty) {
      return '{}';
    }
    return jsonEncode(value);
  }

  void _withArena(
    void Function(
      MusicxxPluginArena arena,
      Pointer<MusicxxExternPluginString> log,
    )
    body,
  ) {
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      body(arena, log);
    } finally {
      arena.dispose();
    }
  }

  /// 读取 JSON 数组出参（失败抛异常）
  List<MusicxxPluginInfo> _readList(String operation, _OutCall call) {
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> out = arena.outString();
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      _runtime.checkOrThrow(call(_runtime.bindings, out, log), log, operation);
      return decodeJsonArray(
        takeOutString(out, _runtime.bindings),
      ).map(MusicxxPluginInfo.fromJson).toList(growable: false);
    } finally {
      arena.dispose();
    }
  }
}
