import 'dart:async';
import 'dart:convert';
import 'dart:ffi';

import 'bindings_generated.dart';
import 'events.dart';
import 'native_strings.dart';
import 'plugin_info.dart';
import 'plugin_inspect.dart';
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
/// 这里只做有界等待；耗时的准备工作（例如解压安装包）由调用方放到单独的 isolate 里完成。
class MusicxxPluginManager {
  MusicxxPluginManager.internal(this._runtime);

  final MusicxxPluginRuntime _runtime;

  /// 已加载插件缓存（事件到达时重新查询；以宿主快照为准）
  List<MusicxxPluginInfo> _cache = const <MusicxxPluginInfo>[];
  final Map<String, MusicxxPluginInfo> _byId = <String, MusicxxPluginInfo>{};

  /// 最近一次扫描结果
  List<MusicxxPluginInfo> _scanned = const <MusicxxPluginInfo>[];

  /// 等待结果的异步能力调用（`requestId` → 完成器）
  ///
  /// 受理成功 ⇒ 一定有终局：正常结果、执行失败、插件卸载、宿主停机四条路都会把它清掉；
  /// 调用方自己的超时也会清（晚到的结果按"已经有终局了"丢弃并记日志）。
  final Map<int, Completer<Object?>> _pendingCalls =
      <int, Completer<Object?>>{};

  /// 宿主库有没有 `plugin_call_async` 符号（探测一次就够；false = 库太旧）
  bool? _asyncCallAvailable;

  /// 宿主库是否支持异步能力调用（false = 库太旧，能力调用会直接失败）
  bool get supportsAsyncCall => _asyncCallAvailable ?? true;

  /// 现在有多少次能力调用还在等结果（调试页/自省用）
  int get pendingCallCount => _pendingCalls.length;

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

  /// 检查一个插件目录（**只读**，不装载、不改动任何状态）
  ///
  /// 与 [scan] 同源：原生侧是同一份 `inspectPluginDir`，因此"预检通过、装载却用另一个
  /// 分支"这类分歧不会出现。用途是应用侧「从压缩包安装」的预检：把解包出来的临时目录
  /// 交给宿主判定，装之前就知道"当前系统/架构能不能用、会用哪个分支、库文件在不在"。
  ///
  /// - [dir] 插件目录（绝对路径；压缩包解包后的临时目录同样可以）；
  /// - [os] / [arch] 要判定的目标环境（留空 = 宿主自己的平台与架构）；
  /// - 目录不存在 / 清单非法时返回的对象里 `valid == false`（不抛异常，看 [error]）；
  /// - 不看运行期开关（JS 运行时是否可用、安全模式、禁用动态库）——那些由 [scan] 叠加。
  MusicxxPluginInspect_c inspect(
    String dir, {
    String os = '',
    String arch = '',
  }) {
    _requireRunning('plugin_inspect');
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> out = arena.outString();
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings.musicxx_extern_plugin_plugin_inspect(
        _runtime.host,
        arena.view(dir),
        arena.view(os),
        arena.view(arch),
        out,
        log,
      );
      _runtime.checkOrThrow(rc, log, 'plugin_inspect');
      return MusicxxPluginInspect_c.fromJson(
        decodeJsonObject(takeOutString(out, _runtime.bindings)),
      );
    } finally {
      arena.dispose();
    }
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
  String configPath(String id) {    _requireRunning('plugin_get_config_path');
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

  /// 调用插件能力（Dart → 插件）：**异步，不占调用线程**
  ///
  /// - native/JS 插件统一用 `plugin.<插件id>.<能力名>`（短名由宿主补齐；
  ///   能力必须在插件 `start` 事务里声明，归属校验由宿主完成）；
  /// - 受理成功 ⇒ 一定有终局：结果、执行失败、插件卸载（`plugin_unloaded`）、
  ///   宿主停机（`host_stopped`）都会给这个 `Future` 一个交代，不会悬挂；
  /// - [timeout] 只是"我不等了"：到点抛出超时错误，晚到的结果**丢弃**并记一条日志
  ///   （不是取消，目标插件照它自己的节奏跑完）；`null` = 不设（一直等到宿主给出终局）；
  /// - 宿主库没有 `plugin_call_async` 符号 → 立即失败（"宿主库太旧"），**不退回同步**；
  /// - 结果形态与插件给的 JSON 一致（对象按 Map 解码，其余按值）。
  Future<Object?> call(
    String id,
    String method, [
    Map<String, Object?> args = const <String, Object?>{},
    Duration? timeout = const Duration(seconds: 30),
  ]) async {
    _requireRunning('plugin_call');
    if (false == _asyncCallAvailable) {
      throw MusicxxPluginApiException(
        -2,
        '宿主库太旧，没有 plugin_call_async（请重新构建宿主库：tools/build_native.ps1）',
        operation: 'plugin_call',
      );
    }
    int requestId = 0;
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final Pointer<Int64> outRequestId = arena.mallocInt64();
      final int rc;
      try {
        rc = _runtime.bindings.musicxx_extern_plugin_plugin_call_async(
          _runtime.host,
          arena.view(id),
          arena.view(method),
          arena.view(_optionsJson(args)),
          outRequestId,
          log,
        );
      } catch (error) {
        // 旧宿主库没有这个符号：探测一次就够，之后一律按"库太旧"处理
        _asyncCallAvailable = false;
        throw MusicxxPluginApiException(
          -2,
          '宿主库太旧，没有 plugin_call_async（请重新构建宿主库：tools/build_native.ps1）',
          operation: 'plugin_call',
        );
      }
      _asyncCallAvailable = true;
      _runtime.checkOrThrow(rc, log, 'plugin_call');
      requestId = outRequestId.value;
    } finally {
      arena.dispose();
    }
    if (requestId <= 0) {
      throw MusicxxPluginApiException(
        -2,
        '宿主没有给出 requestId（无法配对结果）',
        operation: 'plugin_call',
      );
    }

    final Completer<Object?> completer = Completer<Object?>();
    _pendingCalls[requestId] = completer;
    Timer? timer;
    if (null != timeout) {
      timer = Timer(timeout, () {
        final Completer<Object?>? pending = _pendingCalls.remove(requestId);
        if (null == pending || pending.isCompleted) {
          return;
        }
        // 到点只是"我不等了"：目标插件照它自己的节奏跑完，结果丢弃（不是取消）
        _runtime.log(
          3,
          '能力调用超时，结果丢弃: $id.$method #$requestId',
        );
        pending.completeError(
          MusicxxPluginApiException(
            -5,
            '调用插件能力超时: $id.$method',
            operation: 'plugin_call',
          ),
        );
      });
    }
    try {
      return await completer.future;
    } finally {
      timer?.cancel();
    }
  }

  /// 资源与耗时统计快照（各阶段耗时/钩子耗时/注册项/计数器；只观测不限制）
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
    // 宿主已经不在：还没回来的能力调用不可能再有结果，按"宿主已停止"收尾
    // （不留悬挂的 Future；正常路径下宿主停机时会先推一条 host_stopped 结果事件）
    final List<Completer<Object?>> pending = List<Completer<Object?>>.of(
      _pendingCalls.values,
    );
    _pendingCalls.clear();
    for (final Completer<Object?> completer in pending) {
      if (!completer.isCompleted) {
        completer.completeError(
          MusicxxPluginApiException(
            -2,
            '宿主已停止，能力调用未完成',
            operation: 'plugin_call',
          ),
        );
      }
    }
    _asyncCallAvailable = null;
    _cache = const <MusicxxPluginInfo>[];
    _byId.clear();
    _scanned = const <MusicxxPluginInfo>[];
  }

  /// `musicxx.plugin.call.result`：按 `requestId` 给等待中的调用回填结果
  ///
  /// 找不到等待方有两种情况，都按"丢弃"处理：调用方已经超时（它自己不等了），
  /// 或者宿主重启后 `requestId` 空间从头开始（残留的结果不能串到新的调用上）。
  void handleCallResultEvent(MusicxxPluginEvent event) {
    final int? requestId = event.intOf('requestId');
    if (null == requestId) {
      return;
    }
    final Completer<Object?>? completer = _pendingCalls.remove(requestId);
    if (null == completer || completer.isCompleted) {
      _runtime.log(3, '收到没有等待方的能力结果 #$requestId（已超时或宿主重启），丢弃');
      return;
    }
    final bool ok = event.boolOf('ok') ?? false;
    if (ok) {
      completer.complete(event.payload['result']);
      return;
    }
    final String reason = event.stringOf('error') ?? '插件能力执行失败';
    completer.completeError(
      MusicxxPluginApiException(-99, reason, operation: 'plugin_call'),
    );
  }

  /// 生命周期事件：只重新查询一次快照（避免事件顺序与快照不一致导致状态错乱）
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
