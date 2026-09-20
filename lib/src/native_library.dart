import 'dart:ffi';
import 'dart:io';

import 'bindings_generated.dart';

/// 原生库定位/加载失败（诊断信息包含所有尝试过的路径）
class MusicxxPluginLibraryException implements Exception {
  MusicxxPluginLibraryException(this.message, {this.attempted = const <String>[], this.causes = const <String>[]});

  final String message;

  /// 依次尝试过的候选路径/库名
  final List<String> attempted;

  /// 每个候选的失败原因（与 [attempted] 一一对应）
  final List<String> causes;

  @override
  String toString() {
    final StringBuffer sb = StringBuffer(message);
    for (int i = 0; i < attempted.length; i++) {
      sb.write('\n  - ${attempted[i]}');
      if (i < causes.length && causes[i].isNotEmpty) {
        sb.write(' → ${causes[i]}');
      }
    }
    return sb.toString();
  }
}

/// Dart 与原生库的 C ABI 版本不匹配（两侧版本必须一致）
class MusicxxPluginApiVersionException implements Exception {
  MusicxxPluginApiVersionException(this.nativeVersion, this.expectedVersion);

  final int nativeVersion;
  final int expectedVersion;

  @override
  String toString() =>
      'musicxx_extern_plugin C ABI 版本不匹配: 原生库=$nativeVersion, Dart 侧期望=$expectedVersion';
}

/// 已加载的原生宿主库（`DynamicLibrary` + 生成的绑定）
class MusicxxPluginNativeLibrary {
  MusicxxPluginNativeLibrary._(this.library, this.bindings, this.path);

  final DynamicLibrary library;
  final MusicxxExternPluginBindings bindings;

  /// 实际加载到的路径/库名（诊断用）
  final String path;

  /// 库文件基名（按平台）
  static String get libraryFileName {
    if (Platform.isWindows) {
      return 'musicxx_extern_plugin.dll';
    }
    if (Platform.isMacOS || Platform.isIOS) {
      return 'libmusicxx_extern_plugin.dylib';
    }
    return 'libmusicxx_extern_plugin.so';
  }

  /// 默认候选路径（按优先级）：
  /// 1. 环境变量 `MUSICXX_EXTERN_PLUGIN_LIBRARY`（显式路径，便于打包/调试，plan §5.2）；
  /// 2. `<包目录>/.native/output/*/bin/<库名>`（本包构建脚本的稳定输出目录）；
  /// 3. `<包目录>/.native/build/*/musicxx-extern-plugin-install/bin/<库名>`（安装前缀）；
  /// 4. 可执行文件旁边与其 `lib/` 子目录（随应用分发的宿主库，见 `windows|linux/CMakeLists.txt`；
  ///    Linux 桌面应用的打包结果是 `<bundle>/musicxx` + `<bundle>/lib/*.so`，因此要一并覆盖）；
  /// 5. 纯库名（交给系统搜索路径，如随应用包分发时）。
  ///
  /// 说明：开发机的 `.native/` 产物排在"可执行文件旁边"之前，因为它每次构建都会刷新，
  /// 而随应用分发的那一份要等下一次 Flutter 构建才会更新（避免"改了原生代码却还是老行为"）。
  static List<String> defaultCandidates({String? packageRoot}) {
    final String? env = Platform.environment['MUSICXX_EXTERN_PLUGIN_LIBRARY'];
    final List<String> candidates = <String>[
      if (env != null && env.isNotEmpty) env,
    ];
    final String root = packageRoot ?? Directory.current.path;
    candidates.addAll(_globLibraryFiles('$root/.native/output', libraryFileName, 'bin'));
    candidates.addAll(
      _globLibraryFiles('$root/.native/build', libraryFileName, 'musicxx-extern-plugin-install/bin'),
    );
    try {
      final String exeDir = File(Platform.resolvedExecutable).parent.path;
      candidates.add('$exeDir/$libraryFileName');
      candidates.add('$exeDir/lib/$libraryFileName');
    } catch (_) {
      // 某些平台上 `resolvedExecutable` 可能不可用：忽略这些候选即可
    }
    candidates.add(libraryFileName);
    return candidates;
  }

  /// 在 [base]/*/[suffix] 下查找库文件（一层通配，够用且无额外依赖）
  ///
  /// 排序（决定优先级）：Release 优先于 Debug，其次按文件修改时间从新到旧。
  /// 原因：同一台开发机上常有多个构建目录（release/debug），若按目录名字母序挑选会
  /// 拿到**过期**或不匹配配置的库（表现为"代码明明改过却还是老行为"，很难排查）。
  static List<String> _globLibraryFiles(String base, String fileName, String suffix) {
    final Directory dir = Directory(base);
    if (!dir.existsSync()) {
      return const <String>[];
    }
    final List<({String path, int score, int mtime})> found = <({String path, int score, int mtime})>[];
    try {
      for (final FileSystemEntity entity in dir.listSync()) {
        if (entity is! Directory) {
          continue;
        }
        final String candidate = '${entity.path}/$suffix/$fileName';
        final File file = File(candidate);
        if (!file.existsSync()) {
          continue;
        }
        final String lower = entity.path.toLowerCase();
        final int score = lower.contains('release') ? 0 : (lower.contains('debug') ? 1 : 2);
        found.add((path: candidate, score: score, mtime: file.lastModifiedSync().millisecondsSinceEpoch));
      }
    } on FileSystemException {
      return const <String>[];
    }
    found.sort((({String path, int score, int mtime}) a, ({String path, int score, int mtime}) b) {
      final int byScore = a.score.compareTo(b.score);
      return byScore != 0 ? byScore : b.mtime.compareTo(a.mtime);
    });
    return found.map((({String path, int score, int mtime}) e) => e.path).toList(growable: false);
  }

  /// 加载原生库并校验 C ABI 版本
  ///
  /// - [path] 非空时只尝试该路径（不命中直接抛异常，不做静默回退）；
  /// - 未指定时按 [defaultCandidates] 顺序尝试；
  /// - 加载成功但版本不匹配时抛 [MusicxxPluginApiVersionException]（避免"能加载但行为错乱"）。
  static MusicxxPluginNativeLibrary open({
    String? path,
    int expectedApiVersion = 1,
    String? packageRoot,
  }) {
    final List<String> candidates =
        path != null ? <String>[path] : defaultCandidates(packageRoot: packageRoot);
    final List<String> attempted = <String>[];
    final List<String> causes = <String>[];
    for (final String candidate in candidates) {
      attempted.add(candidate);
      try {
        final DynamicLibrary library = DynamicLibrary.open(candidate);
        final MusicxxExternPluginBindings bindings = MusicxxExternPluginBindings(library);
        final int nativeVersion = bindings.musicxx_extern_plugin_api_version();
        if (nativeVersion != expectedApiVersion) {
          throw MusicxxPluginApiVersionException(nativeVersion, expectedApiVersion);
        }
        return MusicxxPluginNativeLibrary._(library, bindings, candidate);
      } on MusicxxPluginApiVersionException {
        rethrow;
      } catch (error) {
        causes.add(error.toString());
      }
    }
    throw MusicxxPluginLibraryException(
      '无法加载 musicxx_extern_plugin 原生宿主库（请先运行 tools/build_native.ps1 构建，'
      '或用 MUSICXX_EXTERN_PLUGIN_LIBRARY 指定路径）:',
      attempted: attempted,
      causes: causes,
    );
  }

  @override
  String toString() => 'MusicxxPluginNativeLibrary($path)';
}
