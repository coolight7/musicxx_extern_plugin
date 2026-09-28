/// 外部插件多目标打包（仿 APK 的 `lib/<系统>-<架构>/`）：Dart 侧模型与选择规则
///
/// **权威判定在原生宿主**（`src/host/host_target.h`，扫描与装载共用同一套）；
/// 这一份是 Dart 侧的同一套规则，用途有两个：
///
/// 1. 安装前预检（`ExternPluginArchive.extract`）——压缩包里有没有当前系统/架构能用的
///    分支、库文件在不在，装之前就告诉用户；
/// 2. 管理页展示与排障——包里有哪几个分支、当前用的是哪一个。
///
/// 目录结构（一个包可以同时放多个分支，清单只有一份）：
///
/// ```text
/// my_plugin/
///   plugin.yaml                    清单（`entry` = 分支里的库文件名）
///   lib/                           分支目录（清单 `targets_dir` 可改名，缺省 lib）
///     windows-x64/  my_plugin.dll
///     linux-x64/    my_plugin.so
///     android-arm64-v8a/ my_plugin.so
///     windows/      只限系统（架构不限）
///     x64/          只限架构（系统不限）
///     universal/    通用分支（任何系统/架构都能用）
/// ```
///
/// 选择顺序（从具体到通用）：系统+架构 → 只系统 → 只架构 → 通用 → 插件根目录
/// （根目录是旧布局，也是隐式通用分支）。
library;

import 'dart:ffi' show Abi;
import 'dart:io';

import 'package:path/path.dart' as path;

/// 分支标签的解析结果（标签 = 分支目录名）
class MusicxxTargetTag_c {
  const MusicxxTargetTag_c({
    required this.recognized,
    this.os = '',
    this.arch = '',
    this.universal = false,
  });

  /// 是否是可识别的标签（系统 / 架构 / 通用）
  final bool recognized;

  /// 规范系统名（空 = 不限；见 [MusicxxPluginTargets_c.normalizeOs]）
  final String os;

  /// 规范架构名（空 = 不限；见 [MusicxxPluginTargets_c.normalizeArch]）
  final String arch;

  /// 通用分支（universal / any / noarch …）
  final bool universal;

  static const MusicxxTargetTag_c unknown = MusicxxTargetTag_c(
    recognized: false,
  );

  /// 该标签是否匹配指定的系统与架构
  bool matches(String os_, String arch_) =>
      recognized &&
      (os.isEmpty || os == os_) &&
      (arch.isEmpty || arch == arch_);

  /// 与指定环境的匹配等级：3 系统+架构 / 2 系统 / 1 架构 / 0 通用 / -1 不匹配
  int matchLevelOf(String os_, String arch_) {
    if (!matches(os_, arch_)) {
      return -1;
    }
    if (os.isNotEmpty && arch.isNotEmpty) {
      return 3;
    }
    if (os.isNotEmpty) {
      return 2;
    }
    if (arch.isNotEmpty) {
      return 1;
    }
    return 0;
  }

  @override
  String toString() =>
      'MusicxxTargetTag_c(os=$os, arch=$arch, universal=$universal)';
}

/// 包内的一个分支（含该分支里解析到的库文件）
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

  final String os;
  final String arch;
  final bool universal;

  /// 与当前环境的匹配等级：3 / 2 / 1 / 0；-1 = 不匹配
  final int matchLevel;

  /// 是否是最终选中要加载的分支
  final bool selected;

  /// 从宿主扫描结果的 JSON 项解析（`plugin_scan` 的 `targets[]`）
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
  String toString() => 'MusicxxPluginTarget_c($tag, lib=$lib, match=$matchLevel)';
}

/// 一次目标解析的结果（与宿主 `MusicxxPluginTargets` 字段一致）
class MusicxxPluginTargets_c {
  const MusicxxPluginTargets_c({
    required this.dirName,
    required this.all,
    required this.selectedTag,
    required this.selectedLib,
    required this.note,
    required this.hasBranches,
  });

  /// 分支根目录名（清单 `targets_dir`；缺省 `lib`；空串 = 清单显式关闭分支扫描）
  final String dirName;

  /// 包内识别到的分支（含不匹配当前环境的；按选择优先级排序）
  final List<MusicxxPluginTarget_c> all;

  /// 选中的分支标签（空串 = 用插件根目录）
  final String selectedTag;

  /// 选中的库文件（相对插件目录；空串 = 没找到可加载的库文件）
  final String selectedLib;

  /// 选择说明 / 不匹配原因（用户可见）
  final String note;

  /// 包内是否存在识别到的分支目录
  final bool hasBranches;

  /// 选中的分支（没有分支时返回 `null`）
  MusicxxPluginTarget_c? get selected {
    for (final MusicxxPluginTarget_c item in all) {
      if (item.selected) {
        return item;
      }
    }
    return null;
  }

  /// 分支标签清单文本（例：`windows-x64、linux-x64`；空 = "无"）
  String get tagsText =>
      all.isEmpty ? '' : all.map((MusicxxPluginTarget_c item) => item.tag).join('、');

  @override
  String toString() =>
      'MusicxxPluginTargets_c(dir=$dirName, selected=$selectedTag, lib=$selectedLib, '
      'branches=${all.length})';
}

/// 多目标打包的选择规则（与宿主 `src/host/host_target.cpp` 同一套）
abstract final class MusicxxPluginTargets {
  /// 分支根目录名缺省值（与宿主、清单 `targets_dir` 缺省一致）
  static const String defaultDirName = 'lib';

  /// 系统别名（键为去掉分隔符并转小写后的写法）
  static const Map<String, String> _osAliases = <String, String>{
    'windows': 'windows',
    'win': 'windows',
    'win32': 'windows',
    'win64': 'windows',
    'windowsnt': 'windows',
    'msvc': 'windows',
    'linux': 'linux',
    'gnu': 'linux',
    'gnulinux': 'linux',
    'macos': 'macos',
    'mac': 'macos',
    'macosx': 'macos',
    'osx': 'macos',
    'darwin': 'macos',
    'apple': 'macos',
    'android': 'android',
    'androideabi': 'android',
    'ios': 'ios',
    'iphoneos': 'ios',
    'ipados': 'ios',
    'ohos': 'ohos',
    'harmonyos': 'ohos',
    'harmony': 'ohos',
    'fuchsia': 'fuchsia',
  };

  /// 架构别名（`arm` / `arm32` 按 32 位 ARM 处理，64 位用 arm64 / aarch64）
  static const Map<String, String> _archAliases = <String, String>{
    'x64': 'x64',
    'x8664': 'x64',
    'amd64': 'x64',
    'intel64': 'x64',
    'x86': 'x86',
    'i386': 'x86',
    'i486': 'x86',
    'i586': 'x86',
    'i686': 'x86',
    'ia32': 'x86',
    'x8632': 'x86',
    '386': 'x86',
    '486': 'x86',
    '586': 'x86',
    '686': 'x86',
    'arm64': 'arm64',
    'aarch64': 'arm64',
    'arm64v8': 'arm64',
    'arm64v8a': 'arm64',
    'armv8': 'arm64',
    'armv8a': 'arm64',
    'arm64e': 'arm64',
    'armv8l': 'arm64',
    'arm': 'armv7',
    'arm32': 'armv7',
    'armv7': 'armv7',
    'armv7a': 'armv7',
    'armv7l': 'armv7',
    'armeabi': 'armv7',
    'armeabiv7': 'armv7',
    'armeabiv7a': 'armv7',
    'riscv64': 'riscv64',
    'rv64': 'riscv64',
    'riscv': 'riscv64',
    'loongarch64': 'loongarch64',
    'loong64': 'loongarch64',
    'loongarch': 'loongarch64',
  };

  /// 通用分支别名
  static const Set<String> _universalAliases = <String>{
    'universal',
    'any',
    'all',
    'noarch',
    'generic',
    'common',
  };

  /// 名字规范化：转小写并去掉分隔符（`windows-x64` / `windows_x64` 视为同一个标签）
  static String foldName(String text) {
    final StringBuffer buffer = StringBuffer();
    for (final int unit in text.toLowerCase().codeUnits) {
      final String ch = String.fromCharCode(unit);
      if (ch == '-' || ch == '_' || ch == '.' || ch == ' ' || ch == '\t') {
        continue;
      }
      buffer.write(ch);
    }
    return buffer.toString();
  }

  /// 系统名 → 规范名（未识别返回空串）
  static String normalizeOs(String name) => _osAliases[foldName(name)] ?? '';

  /// 架构名 → 规范名（未识别返回空串）
  static String normalizeArch(String name) => _archAliases[foldName(name)] ?? '';

  /// 解析分支标签（目录名）
  static MusicxxTargetTag_c parseTag(String tag) {
    final String folded = foldName(tag);
    if (folded.isEmpty) {
      return MusicxxTargetTag_c.unknown;
    }
    if (_universalAliases.contains(folded)) {
      return const MusicxxTargetTag_c(recognized: true, universal: true);
    }
    final String os = _osAliases[folded] ?? '';
    if (os.isNotEmpty) {
      return MusicxxTargetTag_c(recognized: true, os: os);
    }
    final String arch = _archAliases[folded] ?? '';
    if (arch.isNotEmpty) {
      return MusicxxTargetTag_c(recognized: true, arch: arch);
    }
    // 组合标签：找一个切分点，两边分别是"系统"与"架构"（两种顺序都接受）
    for (int split = 1; split < folded.length; ++split) {
      final String left = folded.substring(0, split);
      final String right = folded.substring(split);
      final String leftOs = _osAliases[left] ?? '';
      final String rightArch = _archAliases[right] ?? '';
      if (leftOs.isNotEmpty && rightArch.isNotEmpty) {
        return MusicxxTargetTag_c(
          recognized: true,
          os: leftOs,
          arch: rightArch,
        );
      }
      final String leftArch = _archAliases[left] ?? '';
      final String rightOs = _osAliases[right] ?? '';
      if (leftArch.isNotEmpty && rightOs.isNotEmpty) {
        return MusicxxTargetTag_c(
          recognized: true,
          os: rightOs,
          arch: leftArch,
        );
      }
    }
    return MusicxxTargetTag_c.unknown;
  }

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

  /// 当前进程的规范架构名（与宿主 `hostArch()` 同一口径：x64 / arm64 / x86）
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

  /// 平台默认库文件名（与宿主 `defaultPluginLibraryName` 同一规则）
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

  /// 平台修正后的动态库扩展名（清单按 Linux 写 `<名>.so`）
  static String platformLibraryName(String entry, String pluginName) {
    final String base = entry.isEmpty ? defaultLibraryName(pluginName) : entry;
    if (currentOs() == 'windows' && base.endsWith('.so')) {
      return '${base.substring(0, base.length - 3)}.dll';
    }
    if ((currentOs() == 'macos' || currentOs() == 'ios') &&
        base.endsWith('.so')) {
      return '${base.substring(0, base.length - 3)}.dylib';
    }
    return base;
  }

  /// 判断一个文件名是否像插件动态库（`*.dll` / `*.so` / `*.dylib`）
  static bool isLibraryFileName(String name) {
    final String lower = name.toLowerCase();
    return lower.endsWith('.dll') ||
        lower.endsWith('.so') ||
        lower.endsWith('.dylib');
  }

  /// 解析插件目录里的分支并选出当前系统/架构要用的库文件
  ///
  /// - [pluginDir] 插件目录（解包后的临时目录或已安装目录）；
  /// - [targetsDir] 分支根目录名（清单 `targets_dir`；空串 = 关闭分支扫描）；
  /// - [entry] 清单 `entry`（分支里的库文件名，按 Linux 写法）；
  /// - [pluginName] 清单 `name`（用来拼平台默认库名）；
  /// - [os] / [arch] 目标环境（缺省用当前进程的）；
  /// - [scanRootFallback] = true 时，没有匹配分支会回退插件根目录的库文件（旧布局）。
  static MusicxxPluginTargets_c resolve({
    required String pluginDir,
    String targetsDir = defaultDirName,
    String entry = '',
    String pluginName = '',
    String os = '',
    String arch = '',
    bool scanRootFallback = true,
  }) {
    final String currentOs = os.isEmpty ? MusicxxPluginTargets.currentOs() : os;
    final String currentArch =
        arch.isEmpty ? MusicxxPluginTargets.currentArch() : arch;

    final List<MusicxxPluginTarget_c> branches = <MusicxxPluginTarget_c>[];
    if (targetsDir.isNotEmpty) {
      final Directory branchRoot = Directory(path.join(pluginDir, targetsDir));
      if (branchRoot.existsSync()) {
        for (final FileSystemEntity entity in branchRoot.listSync()) {
          if (entity is! Directory) {
            continue;
          }
          final String tagName = path.basename(entity.path);
          final MusicxxTargetTag_c tag = parseTag(tagName);
          if (false == tag.recognized) {
            continue;
          }
          final String lib = _findLibraryInDir(
            entity.path,
            entry: entry,
            pluginName: pluginName,
          );
          branches.add(
            MusicxxPluginTarget_c(
              tag: tagName,
              dir: '$targetsDir/$tagName',
              lib: lib.isEmpty
                  ? ''
                  : path.relative(lib, from: pluginDir).replaceAll(r'\', '/'),
              os: tag.os,
              arch: tag.arch,
              universal: tag.universal,
              matchLevel: tag.matchLevelOf(currentOs, currentArch),
              selected: false,
            ),
          );
        }
      }
    }

    // 匹配等级高的在前；同级按标签名排序（同名标签只可能是大小写差异）
    branches.sort((MusicxxPluginTarget_c a, MusicxxPluginTarget_c b) {
      if (a.matchLevel != b.matchLevel) {
        return b.matchLevel.compareTo(a.matchLevel);
      }
      return a.tag.compareTo(b.tag);
    });

    // 选分支：先选"匹配且有库文件"的；都没有库文件时保留最高优先级的匹配分支，
    // 好给出准确原因（分支在，但里面没库文件）
    for (int i = 0; i < branches.length; ++i) {
      final MusicxxPluginTarget_c item = branches[i];
      if (item.matchLevel < 0 || item.lib.isEmpty) {
        continue;
      }
      branches[i] = _selectedCopy(item);
      return MusicxxPluginTargets_c(
        dirName: targetsDir,
        all: List<MusicxxPluginTarget_c>.unmodifiable(branches),
        selectedTag: item.tag,
        selectedLib: item.lib,
        note: '',
        hasBranches: true,
      );
    }
    for (int i = 0; i < branches.length; ++i) {
      final MusicxxPluginTarget_c item = branches[i];
      if (item.matchLevel < 0) {
        continue;
      }
      branches[i] = _selectedCopy(item);
      return MusicxxPluginTargets_c(
        dirName: targetsDir,
        all: List<MusicxxPluginTarget_c>.unmodifiable(branches),
        selectedTag: item.tag,
        selectedLib: '',
        note: '分支 `${item.dir}` 里没有可加载的库文件',
        hasBranches: true,
      );
    }

    // 没有匹配分支：回退插件根目录（旧布局 / 隐式通用分支）
    if (scanRootFallback) {
      final String rootLib = _findLibraryInDir(
        pluginDir,
        entry: entry,
        pluginName: pluginName,
      );
      if (rootLib.isNotEmpty) {
        return MusicxxPluginTargets_c(
          dirName: targetsDir,
          all: List<MusicxxPluginTarget_c>.unmodifiable(branches),
          selectedTag: '',
          selectedLib: path
              .relative(rootLib, from: pluginDir)
              .replaceAll(r'\', '/'),
          note: branches.isEmpty ? '' : '包内没有匹配当前系统/架构的分支, 已回退插件根目录',
          hasBranches: branches.isNotEmpty,
        );
      }
    }
    return MusicxxPluginTargets_c(
      dirName: targetsDir,
      all: List<MusicxxPluginTarget_c>.unmodifiable(branches),
      selectedTag: '',
      selectedLib: '',
      note: branches.isEmpty
          ? ''
          : '包内没有匹配当前系统/架构的分支 (当前 $currentOs/$currentArch, '
                '包内分支: ${branches.map((MusicxxPluginTarget_c item) => item.tag).join('、')})',
      hasBranches: branches.isNotEmpty,
    );
  }

  static MusicxxPluginTarget_c _selectedCopy(MusicxxPluginTarget_c item) =>
      MusicxxPluginTarget_c(
        tag: item.tag,
        dir: item.dir,
        lib: item.lib,
        os: item.os,
        arch: item.arch,
        universal: item.universal,
        matchLevel: item.matchLevel,
        selected: true,
      );

  /// 在某个目录里找插件动态库：清单 entry（平台修正扩展名）→ 平台默认库名 →
  /// 目录里唯一的动态库文件；找不到返回空串
  static String _findLibraryInDir(
    String dir, {
    required String entry,
    required String pluginName,
  }) {
    if (false == Directory(dir).existsSync()) {
      return '';
    }
    // 1) 清单 entry（平台修正扩展名；entry 里带子目录时按原样找一次）
    if (entry.isNotEmpty) {
      for (final String candidate in <String>[
        platformLibraryName(entry, pluginName),
        entry,
      ]) {
        final String full = path.join(dir, candidate);
        if (File(full).existsSync()) {
          return full;
        }
      }
    }
    // 2) 平台默认库名
    if (pluginName.isNotEmpty) {
      final String fallback = path.join(dir, defaultLibraryName(pluginName));
      if (File(fallback).existsSync()) {
        return fallback;
      }
    }
    // 3) 目录里只有一个动态库文件时直接用它
    String only = '';
    for (final FileSystemEntity entity in Directory(dir).listSync()) {
      if (entity is! File) {
        continue;
      }
      final String name = path.basename(entity.path);
      if (name.isEmpty || name.startsWith('.') || false == isLibraryFileName(name)) {
        continue;
      }
      if (only.isNotEmpty) {
        return '';
      }
      only = entity.path;
    }
    return only;
  }

  /// 生成"分支情况"的一行说明（安装确认弹窗与详情页用）
  ///
  /// 例：`已选分支 windows-x64（库文件 lib/windows-x64/my_plugin.dll）；包内分支：…`
  static String describe(MusicxxPluginTargets_c targets) {
    final StringBuffer buffer = StringBuffer();
    if (targets.selectedLib.isNotEmpty) {
      buffer.write(
        targets.selectedTag.isEmpty
            ? '库文件：${targets.selectedLib}（插件根目录）'
            : '已选分支：${targets.selectedTag}（${targets.selectedLib}）',
      );
    }
    if (targets.hasBranches) {
      if (buffer.isNotEmpty) {
        buffer.write('；');
      }
      buffer.write('包内分支：${targets.tagsText}');
    }
    if (targets.note.isNotEmpty) {
      if (buffer.isNotEmpty) {
        buffer.write('；');
      }
      buffer.write(targets.note);
    }
    return buffer.toString();
  }
}
