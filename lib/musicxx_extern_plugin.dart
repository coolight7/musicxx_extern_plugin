/// musicxx 外部插件框架：Dart 侧公共 API
///
/// 应用侧只需要接触这一层：
/// ```dart
/// final runtime = MusicxxPluginRuntime.instance;
/// runtime.init(config: MusicxxPluginRuntimeConfig(appVersion: '0.87.0',
///     platform: MusicxxPluginRuntime.currentPlatform, userPluginDir: ...));
/// runtime.events.listen((e) => ...);
/// final found = runtime.plugins.scan();
/// runtime.plugins.load('example_native');
/// final verdict = runtime.hooks.decide(MusicxxPluginHookId.playerBeforePlaySong, payload);
/// runtime.dispose();
/// ```
///
/// 线程与 isolate 约束：动态库插件代码一律在原生宿主线程执行；
/// Dart 处理器与事件泵必须在 **UI isolate**；阻塞式 API（同步装载/能力调用）在
/// UI isolate 调用时是"有界等待"（不发起长任务）。
library;

export 'src/actions.dart';
export 'src/bindings_generated.dart'
    show
        MusicxxExternPluginBindings,
        MusicxxExternPluginHost,
        MusicxxExternPluginHostConfig,
        MusicxxExternPluginString,
        MusicxxExternPluginStringView;
export 'src/events.dart';
export 'src/hook_ids.g.dart';
export 'src/hooks.dart'
    show
        MusicxxPluginDartHandler,
        MusicxxPluginHookContext,
        MusicxxPluginHooks,
        MusicxxPluginThrottle;
export 'src/manager.dart' show MusicxxPluginManager;
export 'src/native_library.dart'
    show
        MusicxxPluginApiVersionException,
        MusicxxPluginLibraryException,
        MusicxxPluginNativeLibrary;
export 'src/native_strings.dart'
    show MusicxxPluginArena, decodeJsonArray, decodeJsonObject, tryDecodeJson;
export 'src/plugin_info.dart';
export 'src/runtime.dart'
    show
        MusicxxPluginApiException,
        MusicxxPluginRuntime,
        MusicxxPluginRuntimeConfig;
export 'src/state_mirror.dart' show MusicxxPluginState;
export 'src/ui.dart'
    show
        MusicxxPluginUIItem,
        MusicxxPluginUIItems,
        MusicxxPluginUIType,
        MusicxxPluginUIActionKind;
