/// 插件目录的**静态判定**结果（宿主 `plugin_inspect` 的 Dart 侧模型）
///
/// 用途：应用侧「从压缩包安装」的预检 —— 把解包出来的临时目录交给宿主判定，
/// "这个包在当前系统/架构下能不能用、会用哪个分支、库文件或脚本在不在"装之前就有答案。
/// 判定与扫描、装载在原生侧同源（`inspectPluginDir`），因此不会出现
/// "预检通过、装载却用另一个分支"。
///
/// 注意：这里**只有数据**，没有分支选择规则（规则在宿主 `src/host/host_target.h`）。
library;

import 'plugin_target.dart';

/// 插件目录的静态判定（只读；不装载、不看运行期开关）
class MusicxxPluginInspect_c {
  const MusicxxPluginInspect_c({
    required this.valid,
    required this.error,
    required this.path,
    required this.dirName,
    required this.id,
    required this.kind,
    required this.version,
    required this.description,
    required this.author,
    required this.homepage,
    required this.entry,
    required this.apiVersion,
    required this.depends,
    required this.optionalDepends,
    required this.permissions,
    required this.platforms,
    required this.arch,
    required this.scripts,
    required this.targetsDir,
    required this.target,
    required this.targetEntry,
    required this.targets,
    required this.supported,
    required this.reason,
  });

  /// 清单是否可解析（能读到 `name`）；false 时看 [error]
  final bool valid;
  final String error;

  /// 插件目录（绝对路径）与目录名
  final String path;
  final String dirName;

  /// 插件 id（清单 `name`）与形态（native / js）
  final String id;
  final String kind;

  /// 清单字段
  final String version;
  final String description;
  final String author;
  final String homepage;
  final String entry;
  final int apiVersion;
  final List<String> depends;
  final List<String> optionalDepends;
  final List<String> permissions;
  final List<String> platforms;
  final List<String> arch;

  /// JS 插件按顺序执行的脚本（动态库插件为空）
  final List<String> scripts;

  /// 动态库插件的多目标分支信息：分支根目录名 / 选中的标签 / 选中的库文件 / 包内分支
  final String targetsDir;
  final String target;
  final String targetEntry;
  final List<MusicxxPluginTarget_c> targets;

  /// 静态可用性（不含运行期开关：JS 运行时是否可用、安全模式、禁用动态库由扫描叠加）
  final bool supported;
  final String reason;

  static List<String> _stringList(Object? value) {
    if (value is! List) {
      return const <String>[];
    }
    return value.whereType<String>().toList(growable: false);
  }

  factory MusicxxPluginInspect_c.fromJson(Map<String, Object?> json) =>
      MusicxxPluginInspect_c(
        valid: json['valid'] as bool? ?? false,
        error: json['error'] as String? ?? '',
        path: json['path'] as String? ?? '',
        dirName: json['dirName'] as String? ?? '',
        id: json['id'] as String? ?? '',
        kind: json['kind'] as String? ?? '',
        version: json['version'] as String? ?? '',
        description: json['description'] as String? ?? '',
        author: json['author'] as String? ?? '',
        homepage: json['homepage'] as String? ?? '',
        entry: json['entry'] as String? ?? '',
        apiVersion: (json['apiVersion'] as num?)?.toInt() ?? 0,
        depends: _stringList(json['depends']),
        optionalDepends: _stringList(json['optionalDepends']),
        permissions: _stringList(json['permissions']),
        platforms: _stringList(json['platforms']),
        arch: _stringList(json['arch']),
        scripts: _stringList(json['scripts']),
        targetsDir: json['targetsDir'] as String? ?? '',
        target: json['target'] as String? ?? '',
        targetEntry: json['targetEntry'] as String? ?? '',
        targets: MusicxxPluginTarget_c.parseList(json['targets']),
        supported: json['supported'] as bool? ?? false,
        reason: json['reason'] as String? ?? '',
      );

  /// 包内是否存在分支目录（多目标包）
  bool get hasBranches => targets.isNotEmpty;

  /// 选中的分支（没有分支时返回 `null`）
  MusicxxPluginTarget_c? get selected {
    for (final MusicxxPluginTarget_c item in targets) {
      if (item.selected) {
        return item;
      }
    }
    return null;
  }

  /// 分支标签清单文本（例：`windows-x64、linux-x64`；空 = 没有分支）
  String get tagsText =>
      targets.map((MusicxxPluginTarget_c item) => item.tag).join('、');

  /// 选中目标的展示文本（例：`windows-x64（lib/windows-x64/demo.dll）` / `插件根目录`）
  String get targetText {
    if (targetEntry.isEmpty) {
      return target.isEmpty ? '未选中分支' : target;
    }
    return target.isEmpty ? '插件根目录（$targetEntry）' : '$target（$targetEntry）';
  }

  /// 安装确认弹窗/详情页用的一行说明（分支 + 库文件 + 包内分支 + 不可用原因）
  String describe() {
    final StringBuffer buffer = StringBuffer();
    if (kind == 'native') {
      buffer.write('库文件：$targetText');
    }
    if (hasBranches) {
      if (buffer.isNotEmpty) {
        buffer.write('；');
      }
      buffer.write('包内分支：$tagsText');
    }
    if (reason.isNotEmpty) {
      if (buffer.isNotEmpty) {
        buffer.write('；');
      }
      buffer.write(reason);
    }
    return buffer.toString();
  }

  @override
  String toString() =>
      'MusicxxPluginInspect_c($id, kind=$kind, valid=$valid, supported=$supported, '
      'target=$target)';
}
