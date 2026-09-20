import 'dart:async';
import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';

import 'actions.dart';
import 'bindings_generated.dart';
import 'events.dart';
import 'hooks.dart';
import 'manager.dart';
import 'native_library.dart';
import 'native_strings.dart';
import 'state_mirror.dart';

/// 宿主配置（对应 C ABI `MusicxxExternPluginHostConfig`）
class MusicxxPluginRuntimeConfig {
  const MusicxxPluginRuntimeConfig({
    required this.appVersion,
    required this.platform,
    this.language = 'zh-cn',
    this.userPluginDir = '',
    this.builtinPluginDir = '',
    this.dataDir = '',
    this.logDir = '',
    this.logLevel = 2,
    this.safeMode = false,
    this.enableNative = true,
    this.enableJs = true,
    this.observeEvents = false,
    this.eventQueueCapacity = 0,
  });

  /// 应用版本（插件可读；用于 `min_app_version` 过滤的展示）
  final String appVersion;

  /// 平台标识（windows/linux/macos/android/ios/ohos）
  final String platform;

  /// 语言代码
  final String language;

  /// 用户插件目录（绝对路径）
  final String userPluginDir;

  /// 随包插件目录（绝对路径）
  final String builtinPluginDir;

  /// 插件私有数据根目录
  final String dataDir;

  /// 日志目录
  final String logDir;

  /// 0 trace .. 4 error
  final int logLevel;

  /// 安全模式：本次运行不加载任何外部插件（崩溃后自检用，plan §12.3）
  final bool safeMode;

  /// 允许原生（DSO）插件
  final bool enableNative;

  /// 允许 JS 插件
  final bool enableJs;

  /// 是否向 Dart 回传观测事件（observe 钩子/事件镜像；默认关，零开销）
  final bool observeEvents;

  /// 事件队列容量（0 = 宿主默认 16384）
  final int eventQueueCapacity;

  /// 组装 C ABI 的 flags 位
  int get flags {
    int value = 0;
    if (safeMode) {
      value |= _flagSafeMode;
    }
    if (!enableNative) {
      value |= _flagNoNative;
    }
    if (!enableJs) {
      value |= _flagNoJs;
    }
    if (observeEvents) {
      value |= _flagDebugObserveEvents;
    }
    return value;
  }
}

const int _flagSafeMode = 0x0001;
const int _flagNoNative = 0x0002;
const int _flagNoJs = 0x0004;
const int _flagDebugObserveEvents = 0x0008;

/// 一次原生调用失败（承载宿主返回的错误码与可读原因）
class MusicxxPluginApiException implements Exception {
  MusicxxPluginApiException(this.code, this.message, {this.operation = ''});

  /// C ABI 错误码（0 成功 / -1 参数 / -2 状态 / -3 JSON / -4 未找到 / -5 超时 / -6 权限 …）
  final int code;
  final String message;
  final String operation;

  @override
  String toString() => 'musicxx_extern_plugin ${operation.isEmpty ? '' : '$operation '}'
      '失败 (code=$code): $message';
}

/// 外部插件宿主运行时（进程单例）
///
/// 生命周期（plan §5.1）：[init] → 用 [plugins]/[hooks]/[state]/[actions] →
/// [dispose]。Dart 侧只做三件事：加载原生库、泵事件、把钩子/动作接到业务上。
///
/// 线程模型（plan §2.3）：宿主内所有插件代码运行在**原生宿主线程**上，Dart 线程只在
/// 同步 FFI 调用（钩子派发、能力调用、状态推送）期间参与，且有等待上界。
class MusicxxPluginRuntime {
  MusicxxPluginRuntime._();

  static final MusicxxPluginRuntime instance = MusicxxPluginRuntime._();

  MusicxxPluginNativeLibrary? _library;
  Pointer<MusicxxExternPluginHost> _host = nullptr;
  MusicxxPluginRuntimeConfig? _config;

  final StreamController<MusicxxPluginEvent> _events =
      StreamController<MusicxxPluginEvent>.broadcast();
  final List<MusicxxPluginEvent> _eventLog = <MusicxxPluginEvent>[];

  late final MusicxxPluginManager plugins = MusicxxPluginManager.internal(this);
  late final MusicxxPluginHooks hooks = MusicxxPluginHooks.internal(this);
  late final MusicxxPluginState state = MusicxxPluginState.internal(this);
  late final MusicxxPluginActions actions = MusicxxPluginActions.internal(this);

  /// 原生 → Dart 唤醒回调
  ///
  /// 说明：`NativeCallable.listener` 只支持 void 返回，而 C ABI 的回调签名是
  /// `int32_t (*)(void*)`。宿主**忽略**该返回值（只用来触发一次轮询安排），
  /// 因此这里返回 void 是安全的；不要依赖它向原生回传信息。
  NativeCallable<Void Function(Pointer<Void>)>? _wakeCallable;
  Timer? _pollFallback;
  bool _pollScheduled = false;
  bool _running = false;
  bool _disposed = false;
  int _droppedEvents = 0;
  int? _lastSeq;

  /// 事件流（宿主事件按序推送；未知类型同样转发，见 [MusicxxPluginEventType]）
  Stream<MusicxxPluginEvent> get events => _events.stream;

  /// 是否已启动（`host_start` 成功）
  bool get isRunning => _running;

  /// 原生库是否已加载
  bool get isLoaded => _library != null;

  /// 最近若干条事件（调试页/排障用；容量固定，不参与派发）
  List<MusicxxPluginEvent> get recentEvents => List<MusicxxPluginEvent>.unmodifiable(_eventLog);

  /// 宿主丢事件计数（队列溢出时由宿主补发 `musicxx.host.error` 并累加）
  int get droppedEvents => _droppedEvents;

  /// 原生库诊断信息（路径/库版本）
  String get libraryPath => _library?.path ?? '(未加载)';

  /// 初始化宿主（幂等：重复调用直接返回）
  ///
  /// 步骤：加载原生库 → 校验 C ABI 版本 → `host_create` → 注册唤醒回调 →
  /// `host_start` → 启动事件泵与 200 ms 兜底轮询。
  void init({
    required MusicxxPluginRuntimeConfig config,
    String? libraryPath,
    String? packageRoot,
  }) {
    if (_running) {
      return;
    }
    if (_disposed) {
      throw StateError('MusicxxPluginRuntime 已 dispose，不能再次 init');
    }
    _library = MusicxxPluginNativeLibrary.open(path: libraryPath, packageRoot: packageRoot);
    _config = config;
    final MusicxxExternPluginBindings bindings = _library!.bindings;

    final Pointer<MusicxxExternPluginHostConfig> cfg =
        malloc<MusicxxExternPluginHostConfig>();
    try {
      cfg.ref
        ..struct_size = sizeOf<MusicxxExternPluginHostConfig>()
        ..log_level = config.logLevel
        ..flags = config.flags
        ..event_queue_capacity = config.eventQueueCapacity;
      final MusicxxPluginArena arena = MusicxxPluginArena();
      try {
        cfg.ref
          ..app_version = arena.view(config.appVersion).ref
          ..platform = arena.view(config.platform).ref
          ..language = arena.view(config.language).ref
          ..user_plugin_dir = arena.view(config.userPluginDir).ref
          ..builtin_plugin_dir = arena.view(config.builtinPluginDir).ref
          ..data_dir = arena.view(config.dataDir).ref
          ..log_dir = arena.view(config.logDir).ref;
        final Pointer<MusicxxExternPluginString> log = arena.outString();
        _host = bindings.musicxx_extern_plugin_host_create(cfg, log);
        if (_host == nullptr) {
          throw MusicxxPluginApiException(-99, takeOutString(log, bindings), operation: 'host_create');
        }
        try {
          _check(bindings.musicxx_extern_plugin_host_start(_host, log), log, 'host_start');
        } on MusicxxPluginApiException {
          bindings.musicxx_extern_plugin_host_destroy(_host);
          _host = nullptr;
          rethrow;
        }
      } finally {
        arena.dispose();
      }
    } finally {
      malloc.free(cfg);
    }

    // 唤醒回调：原生事件入队时立即通知 Dart（失败则退回定时轮询）
    //
    // 注意 ABI 细节：C ABI 的唤醒回调返回 int32_t（宿主忽略其值，只用来触发安排），
    // 而 `NativeCallable.listener` 只支持 void 返回，因此这里做一次函数指针类型转换。
    try {
      _wakeCallable = NativeCallable<Void Function(Pointer<Void>)>.listener(_onWake);
      final int rc = bindings.musicxx_extern_plugin_set_wake_callback(
        _host,
        _wakeCallable!.nativeFunction
            .cast<NativeFunction<MusicxxExternPluginWakeFnFunction>>(),
        nullptr,
      );
      if (rc != 0) {
        _wakeCallable!.close();
        _wakeCallable = null;
      }
    } catch (_) {
      _wakeCallable?.close();
      _wakeCallable = null;
    }
    _pollFallback = Timer.periodic(const Duration(milliseconds: 200), (_) => pumpEvents());
    _running = true;
    // 首轮立刻取一次（host.ready 已在队列里）
    pumpEvents();
  }

  /// 停止宿主并释放原生资源（幂等）
  void dispose() {
    if (_disposed) {
      return;
    }
    _disposed = true;
    _pollFallback?.cancel();
    _pollFallback = null;
    _running = false;
    final MusicxxPluginNativeLibrary? library = _library;
    if (library != null && _host != nullptr) {
      final MusicxxPluginArena arena = MusicxxPluginArena();
      try {
        final Pointer<MusicxxExternPluginString> log = arena.outString();
        library.bindings.musicxx_extern_plugin_host_stop(_host, 5000, log);
        library.bindings.musicxx_extern_plugin_host_destroy(_host);
      } finally {
        arena.outString();
        arena.dispose();
      }
      _host = nullptr;
    }
    _wakeCallable?.close();
    _wakeCallable = null;
    if (!_events.isClosed) {
      _events.close();
    }
    plugins.handleDisposed();
    hooks.handleDisposed();
  }

  // ==================== 内部：绑定/宿主访问 ====================

  MusicxxExternPluginBindings get bindings {
    final MusicxxPluginNativeLibrary? library = _library;
    if (library == null) {
      throw StateError('MusicxxPluginRuntime 尚未 init');
    }
    return library.bindings;
  }

  Pointer<MusicxxExternPluginHost> get host {
    if (_host == nullptr) {
      throw StateError('MusicxxPluginRuntime 尚未 init 或已 dispose');
    }
    return _host;
  }

  MusicxxPluginRuntimeConfig? get config => _config;

  /// 运行一次原生调用：构造 arena → 读错误串 → 释放 → 失败时抛异常
  T _call<T>(
    String operation,
    T Function(MusicxxPluginArena arena, Pointer<MusicxxExternPluginString> log) body,
  ) {
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final T result = body(arena, log);
      return result;
    } finally {
      arena.dispose();
    }
  }

  /// 校验返回码；失败时读取 `log` 出参作为原因
  void _check(
    int rc,
    Pointer<MusicxxExternPluginString> log,
    String operation,
  ) {
    if (rc == 0) {
      return;
    }
    final String message = takeOutString(log, bindings);
    throw MusicxxPluginApiException(rc, message.isEmpty ? '宿主未给出原因' : message, operation: operation);
  }

  // ==================== 事件泵 ====================

  /// 原生线程 → Dart：只做"安排一次轮询"，绝不在这里做重活（plan §4.8 无死锁不变式）
  void _onWake(Pointer<Void> _) {
    _schedulePump();
  }

  void _schedulePump() {
    if (_pollScheduled || _disposed) {
      return;
    }
    _pollScheduled = true;
    scheduleMicrotask(() {
      _pollScheduled = false;
      pumpEvents();
    });
  }

  /// 批量取事件并分发（默认每次最多 200 条，避免长时间占用 UI 线程）
  ///
  /// 返回本次取出的事件条数；无事件时返回 0。
  int pumpEvents({int maxCount = 200, int maxRounds = 8}) {
    if (_host == nullptr) {
      return 0;
    }
    int total = 0;
    for (int round = 0; round < maxRounds; round++) {
      final List<MusicxxPluginEvent> batch = _takeBatch(maxCount);
      if (batch.isEmpty) {
        break;
      }
      total += batch.length;
      for (final MusicxxPluginEvent event in batch) {
        _dispatchEvent(event);
      }
      if (batch.length < maxCount) {
        break;
      }
    }
    return total;
  }

  List<MusicxxPluginEvent> _takeBatch(int maxCount) {
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> out = arena.outString();
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = bindings.musicxx_extern_plugin_poll_events(_host, maxCount, out, log);
      if (rc == -5) {
        return const <MusicxxPluginEvent>[]; // 无事件：正常路径
      }
      if (rc != 0) {
        final String message = takeOutString(log, bindings);
        throw MusicxxPluginApiException(rc, message, operation: 'poll_events');
      }
      final String json = takeOutString(out, bindings);
      return decodeJsonArray(json).map(MusicxxPluginEvent.fromJson).toList(growable: false);
    } finally {
      arena.dispose();
    }
  }

  void _dispatchEvent(MusicxxPluginEvent event) {
    // 序号连续性：宿主队列溢出会补发 host.error，这里记录可观察的丢事件数
    if (_lastSeq != null && event.seq > _lastSeq! + 1) {
      _droppedEvents += event.seq - _lastSeq! - 1;
    }
    _lastSeq = event.seq;

    _eventLog.add(event);
    if (_eventLog.length > 512) {
      _eventLog.removeRange(0, _eventLog.length - 512);
    }

    // 内部订阅者（宿主维护的位图/动作分发）先处理，异常隔离后继续广播
    try {
      switch (event.type) {
        case MusicxxPluginEventType.hookChanged:
          hooks.handleHookChangedEvent(event);
        case MusicxxPluginEventType.hookDecisionResult:
          hooks.handleDecisionResultEvent(event);
        case MusicxxPluginEventType.pluginLoaded:
        case MusicxxPluginEventType.pluginEnabled:
        case MusicxxPluginEventType.pluginDisabled:
        case MusicxxPluginEventType.pluginUnloaded:
          plugins.handleLifecycleEvent(event);
          hooks.handlePluginLifecycleEvent(event);
        case MusicxxPluginEventType.actionRequest:
          actions.handleActionRequest(event);
        case MusicxxPluginEventType.actionCancel:
          actions.handleActionCancel(event);
        case MusicxxPluginEventType.hostError:
          final String? code = event.stringOf('code');
          if (code == 'event_queue_overflow') {
            _droppedEvents += event.intOf('dropped') ?? 0;
          }
      }
    } catch (_) {
      // 内部处理失败不能影响事件广播与宿主泵
    }

    if (!_events.isClosed) {
      _events.add(event);
    }
  }

  // ==================== 通用原生调用（供 manager/hooks/state/actions 使用） ====================

  /// 读取 `log` 出参并在失败时抛异常的通用包装（供子对象复用）
  void checkOrThrow(int rc, Pointer<MusicxxExternPluginString> log, String operation) =>
      _check(rc, log, operation);

  /// 写 Dart → 原生日志（统一落盘/转发策略由原生侧配置）
  void log(int level, String message) {
    if (_host == nullptr) {
      return;
    }
    _call<int>('log', (MusicxxPluginArena arena, Pointer<MusicxxExternPluginString> logOut) {
      bindings.musicxx_extern_plugin_log(_host, level, arena.view(message), logOut);
      return 0;
    });
  }

  /// 调试信息（版本/平台/目录/装载计数；支持页复制用）
  Map<String, Object?> debugInfo() {
    if (_host == nullptr) {
      return const <String, Object?>{};
    }
    return _call<Map<String, Object?>>('debug_info', (arena, log) {
      final Pointer<MusicxxExternPluginString> out = arena.outString();
      _check(bindings.musicxx_extern_plugin_debug_info(_host, out, log), log, 'debug_info');
      return decodeJsonObject(takeOutString(out, bindings));
    });
  }

  /// 解析平台标识（与宿主 `platform` 字段取值一致）
  static String get currentPlatform {
    if (Platform.isWindows) {
      return 'windows';
    }
    if (Platform.isMacOS) {
      return 'macos';
    }
    if (Platform.isLinux) {
      return 'linux';
    }
    if (Platform.isAndroid) {
      return 'android';
    }
    if (Platform.isIOS) {
      return 'ios';
    }
    if (Platform.isFuchsia) {
      return 'fuchsia';
    }
    return Platform.operatingSystem;
  }
}
