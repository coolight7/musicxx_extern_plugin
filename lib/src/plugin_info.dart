/// 插件信息与日志模型（与原生宿主 JSON 出参对应）
library;

import 'events.dart';

/// 插件形态
abstract final class MusicxxPluginKind {
  static const String native = 'native';
  static const String js = 'js';
  static const String builtin = 'builtin';
}

/// 扫描/列表返回的插件条目（`plugin_scan` / `plugin_list`）
class MusicxxPluginInfo {
  const MusicxxPluginInfo({
    required this.id,
    required this.instance,
    required this.kind,
    required this.version,
    required this.description,
    required this.author,
    required this.homepage,
    required this.path,
    required this.dir,
    required this.entry,
    required this.source,
    required this.valid,
    required this.supported,
    required this.reason,
    required this.loaded,
    required this.permissions,
    required this.platforms,
    required this.arch,
    required this.apiVersion,
    required this.depends,
    required this.optionalDepends,
    required this.configPath,
    required this.enabled,
    required this.counters,
  });

  /// 插件 id（清单 `name`；同名视为同一插件）
  final String id;

  /// 宿主实例名（JS 插件为 `js:<id>`）
  final String instance;

  /// native / js / builtin
  final String kind;

  final String version;
  final String description;
  final String author;
  final String homepage;

  /// 插件目录（扫描项）/ 装载路径（已加载项）
  final String path;

  /// 目录名（仅提示用；id 以清单为准）
  final String dir;

  /// 动态库插件的库文件名
  final String entry;

  /// 来源：user / builtin
  final String source;

  /// 清单是否可解析
  final bool valid;

  /// 当前平台/架构/版本是否支持加载
  final bool supported;

  /// 不支持时的原因（用户可见）
  final String reason;

  /// 是否已加载
  final bool loaded;

  /// 清单声明的权限
  final List<String> permissions;
  final List<String> platforms;
  final List<String> arch;

  /// 清单 `api_version`（必须 ≤ 宿主插件 API 版本）
  final int apiVersion;

  /// 必选依赖 / 可选依赖
  final List<String> depends;
  final List<String> optionalDepends;

  /// 插件配置路径（已加载时由宿主推导）
  final String configPath;

  /// 启用状态（宿主侧）
  final bool enabled;

  /// 计数器（hookCalls/actionRequests/...）
  final Map<String, Object?> counters;

  static List<String> _stringList(Object? value) {
    if (value is! List) {
      return const <String>[];
    }
    return value.whereType<String>().toList(growable: false);
  }

  factory MusicxxPluginInfo.fromJson(Map<String, Object?> json) {
    final Object? counters = json['counters'];
    return MusicxxPluginInfo(
      id: json['id'] as String? ?? '',
      instance: json['instance'] as String? ?? (json['id'] as String? ?? ''),
      kind: json['kind'] as String? ?? MusicxxPluginKind.native,
      version: json['version'] as String? ?? '',
      description: json['description'] as String? ?? '',
      author: json['author'] as String? ?? '',
      homepage: json['homepage'] as String? ?? '',
      path: json['path'] as String? ?? '',
      dir: json['dir'] as String? ?? '',
      entry: json['entry'] as String? ?? '',
      source: json['source'] as String? ?? '',
      valid: json['valid'] as bool? ?? true,
      supported: json['supported'] as bool? ?? true,
      reason: json['reason'] as String? ?? '',
      loaded: json['loaded'] as bool? ?? false,
      permissions: _stringList(json['permissions']),
      platforms: _stringList(json['platforms']),
      arch: _stringList(json['arch']),
      apiVersion: (json['apiVersion'] as num?)?.toInt() ?? 0,
      depends: _stringList(json['depends']),
      optionalDepends: _stringList(json['optionalDepends']),
      configPath: json['configPath'] as String? ?? '',
      enabled: json['enabled'] as bool? ?? false,
      counters: counters is Map<String, Object?>
          ? counters
          : const <String, Object?>{},
    );
  }

  /// 用户可读的一行状态（管理页用）
  String get statusText {
    if (!valid) {
      return '清单无效';
    }
    if (!supported) {
      return reason.isEmpty ? '当前环境不支持' : reason;
    }
    return loaded ? (enabled ? '已启用' : '已禁用') : '未加载';
  }

  @override
  String toString() => 'MusicxxPluginInfo($id, $kind, $statusText)';
}

/// 插件日志条目（`musicxx.plugin.log` / `musicxx.js.console`）
class MusicxxPluginLog {
  const MusicxxPluginLog({
    required this.plugin,
    required this.level,
    required this.message,
    required this.ts,
  });

  final String plugin;

  /// 0 trace / 1 debug / 2 info / 3 warn / 4 error
  final int level;
  final String message;
  final int ts;

  factory MusicxxPluginLog.fromEvent(MusicxxPluginEvent event) =>
      MusicxxPluginLog(
        plugin: event.plugin,
        level: event.intOf('level') ?? 2,
        message: event.stringOf('message') ?? '',
        ts: event.ts,
      );

  /// 日志级别名
  String get levelName {
    switch (level) {
      case 0:
        return 'trace';
      case 1:
        return 'debug';
      case 2:
        return 'info';
      case 3:
        return 'warn';
      default:
        return 'error';
    }
  }

  @override
  String toString() => '[$levelName] $plugin: $message';
}
