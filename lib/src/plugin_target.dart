/// 外部插件多目标打包（仿 APK 的 `lib/<系统>-<架构>/`）：Dart 侧只保留**数据模型与环境取值**
///
/// 分支选择规则**只在原生宿主**一处实现（`src/host/host_target.{h,cpp}`）：
/// 扫描（`plugin_scan`）、装载（`loadNativeDirAsync`）与安装预检（`plugin_inspect`）都走
/// 同一份判定。Dart 侧不再复制别名表与目录遍历，因此不会出现"两边规则漂移"。
///
/// 这里保留两类东西：
/// 1. [MusicxxPluginTarget_c]：宿主报出来的分支（`plugin_scan` / `plugin_inspect` 的
///    `targets[]`）；
/// 2. [MusicxxPluginEnv]：应用与包自己要用的环境取值（当前系统 / CPU 架构 / 平台默认
///    库文件名 / 分支目录缺省名）。
library;

import 'dart:ffi' show Abi;
import 'dart:io';

/// 包内的一个目标分支（宿主 `targets[]` 的一项）
class MusicxxPluginTarget_c {
  const MusicxxPluginTarget_c({
    required this.tag,
    required this.dir,
    required this.lib,
    required this.os,
    required this.arch,
    required this.universal,
    required this.matchLevel,
    required this.selected,
  });

  /// 分支目录名（标签）；空串表示"插件根目录"这一个隐式通用分支
  final String tag;

  /// 分支目录（相对插件目录；根目录时为空串）
  final String dir;

  /// 分支里的库文件（相对插件目录；找不到时为空串）
  final String lib;

  /// 分支声明的系统 / 架构（空 = 不限）
  final String os;
  final String arch;

  /// 通用分支（`universal` / `any` / `noarch` …）
  final bool universal;

  /// 与目标环境的匹配等级：3 系统+架构 / 2 系统 / 1 架构 / 0 通用 / -1 不匹配
  final int matchLevel;

  /// 是否是最终选中要加载的分支
  final bool selected;

  factory MusicxxPluginTarget_c.fromJson(Map<String, Object?> json) =>
      MusicxxPluginTarget_c(
        tag: json['tag'] as String? ?? '',
        dir: json['dir'] as String? ?? '',
        lib: json['lib'] as String? ?? '',
        os: json['os'] as String? ?? '',
        arch: json['arch'] as String? ?? '',
        universal: json['universal'] as bool? ?? false,
        matchLevel: (json['match'] as num?)?.toInt() ?? -1,
        selected: json['selected'] as bool? ?? false,
      );

  /// 从宿主 JSON 的 `targets[]` 解析（非列表时返回空列表）
  static List<MusicxxPluginTarget_c> parseList(Object? value) {
    if (value is! List) {
      return const <MusicxxPluginTarget_c>[];
    }
    return value
        .whereType<Map<Object?, Object?>>()
        .map(
          (Map<Object?, Object?> item) =>
              MusicxxPluginTarget_c.fromJson(item.cast<String, Object?>()),
        )
        .toList(growable: false);
  }

  /// 分支的说明文本（管理页用）：标签 + 声明的系统/架构
  String get depict {
    if (universal) {
      return '$tag（通用）';
    }
    final List<String> parts = <String>[
      if (os.isNotEmpty) os,
      if (arch.isNotEmpty) arch,
    ];
    return parts.isEmpty ? tag : '$tag（${parts.join('/')}）';
  }

  @override
  String toString() =>
      'MusicxxPluginTarget_c($tag, lib=$lib, match=$matchLevel)';
}

/// 应用侧要用的环境取值（**不含任何分支选择规则**）
abstract final class MusicxxPluginEnv {
  /// 分支根目录名缺省值（与宿主、清单 `targets_dir` 缺省一致）
  static const String defaultTargetsDirName = 'lib';

  /// 当前进程的规范系统名（与宿主上报的 `platform` 取值一致）
  static String currentOs() {
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

  /// 当前进程的规范架构名（与宿主 `hostArch()` 同一口径：x64 / arm64 / x86 …）
  static String currentArch() {
    final String abi = Abi.current().toString().toLowerCase();
    if (abi.contains('arm64') || abi.contains('aarch64')) {
      return 'arm64';
    }
    if (abi.contains('ia32') || abi.contains('x86_32') || abi.contains('i686')) {
      return 'x86';
    }
    if (abi.contains('arm')) {
      return 'armv7';
    }
    if (abi.contains('riscv64')) {
      return 'riscv64';
    }
    return 'x64';
  }

  /// 平台默认库文件名（与宿主 `defaultPluginLibraryName` 同一规则）：
  /// windows → `<名>.dll`，macos/ios → `lib<名>.dylib`，其余 → `lib<名>.so`
  static String defaultLibraryName(String pluginName) {
    switch (currentOs()) {
      case 'windows':
        return '$pluginName.dll';
      case 'macos':
      case 'ios':
        return 'lib$pluginName.dylib';
      default:
        return 'lib$pluginName.so';
    }
  }
}
