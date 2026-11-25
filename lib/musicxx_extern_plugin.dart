/// musicxx 外部插件框架：Dart 侧公共 API
///
/// 主程序只需要接触这一层：
/// ```dart
/// final runtime = MusicxxPluginRuntime.instance;
/// await runtime.init(config: MusicxxPluginRuntimeConfig(appVersion: '0.87.0',
///     platform: MusicxxPluginRuntime.currentPlatform, userPluginDir: ...));
/// runtime.events.listen((e) => ...);
/// final found = await runtime.plugins.scan();
/// await runtime.plugins.load('example_native');
/// final verdict = await runtime.capabilities.dispatch(
///     MusicxxCapabilityId.playerBeforePlaySong, payload);
/// await runtime.dispose();
/// ```
///
/// 线程与 isolate 约束：动态库插件代码一律在原生插件框架线程执行，JS 插件在 JS 线程；
/// Dart 处理器与事件泵必须在 **UI isolate**。所有入口都是提交式的（返回 `Future`），
/// 不在 Dart 线程上等原生 **。
library;

export 'src/app_capabilities.dart';
export 'src/bindings_generated.dart'
    show
        MusicxxExternPluginBindings,
        MusicxxExternPluginHost,
        MusicxxExternPluginHostConfig,
        MusicxxExternPluginString,
        MusicxxExternPluginStringView;
export 'src/broadcasts.dart' show MusicxxPluginBroadcasts;
export 'src/capability_ids.g.dart';
export 'src/events.dart';
export 'src/capabilities.dart'
    show
        MusicxxPluginCapabilities,
        MusicxxPluginHandlerInfo,
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
export 'src/plugin_inspect.dart';
export 'src/plugin_target.dart';
export 'src/runtime.dart'
    show
        MusicxxPluginApiException,
        MusicxxPluginRuntime,
        MusicxxPluginRuntimeConfig;
export 'src/ui.dart'
    show
        MusicxxPluginUIContent,
        MusicxxPluginUIItem,
        MusicxxPluginUIItems,
        MusicxxPluginUISlot,
        MusicxxPluginUIType,
        MusicxxPluginUIActionKind;
export 'src/ui_slots.g.dart';
export 'src/ui_theme.dart';
export 'src/ui_variables.g.dart';
export 'src/version.dart' show MusicxxPluginVersion;
