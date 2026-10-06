/// 声明式 UI 扩展模型
///
/// 插件**不写 Flutter 代码**：它只注册"UI 项"，用官方类型 + JSON 内容描述
/// "长什么样、点了做什么"；渲染由 musicxx 应用侧负责
/// （见 `lib/pages/plugins/externPlugin/` 与 `lib/plugin/externPlugin/ExternPluginView.dart`）。
///
/// 生命周期与来源：
/// - 注册表在原生宿主里（`musicxx.ui` 表），宿主按插件实例维护，卸载/禁用自动摘除；
/// - Dart 侧用 [MusicxxPluginManager.uiSnapshot] 拉全量快照，用
///   [MusicxxPluginEventType.uiChanged] 事件整批替换某个插件的项（事件载荷带 `items`）。
library;

import 'package:pluginxx_ui/pluginxx_ui.dart';

/// UI 项类型（官方；与 `MUSICXX_PLUGIN_UI_TYPE_*` 一一对应）
abstract final class MusicxxPluginUIType {
  /// 功能主页入口项：`data = {title, subtitle?, icon?, action?}`
  static const String homeEntry = 'musicxx.ui.home.entry';

  /// 歌曲菜单项：`data = {title, icon?, action?}`
  static const String songAction = 'musicxx.ui.song.action';

  /// 歌单菜单项：`data = {title, icon?, action?}`
  static const String playlistAction = 'musicxx.ui.playlist.action';

  /// 播放页背景样式（渲染槽位 `player.background`）：
  /// `data = {title, depict?, enabled?, shader:{bundle, vertex?, fragment?},
  ///          args?（成员名 → 值表达式）, cover?（封面纹理，见 docs/plugin-shader-bundle.md §5.1）,
  ///          speed?, resolutionScale?, maxFps?, animate?, scrim?, foregroundStyle?}`
  ///
  /// 着色器参数只有 `args` 一种写法（旧的 `colors` 已移除）；不写 `args` 时宿主默认给内置背景的
  /// 4 个绘制色（`icon.themeMapping.0..3`）。`cover` 由宿主解码 + 缩放（可选预模糊）后上传成
  /// GPU 纹理、每帧绑定给着色器，插件不搬字节。详见 `docs/plugin-shader-bundle.md`。
  static const String playingBackground = 'musicxx.ui.playing.background';
}

/// UI 项动作：与界面描述层的动作写法**完全一致**（同一套解析器）
///
/// - 字符串短写 = `dispatch`（调用本插件的能力）；
/// - `{kind: dispatch, name, args}` / `{kind: route, route}` / `{kind: command, name, args}` / `{kind: none}`。
///
/// 历史名称（`capability` / `action`）在本次界面重构里已废弃，不再解析。
abstract final class MusicxxPluginUIActionKind {
  /// 打开声明式插件页面：`{kind:"route", route:"ext://<插件id>/<viewId>"}`
  static const String route = 'route';

  /// 调用插件自己的能力：`{kind:"dispatch", name:"<短名>", args:{...}}`（或字符串短写）
  static const String dispatch = 'dispatch';

  /// 调用宿主官方动作：`{kind:"command", name:"musicxx.<域>.<动作>", args:{...}}`
  static const String command = 'command';

  /// 无动作（纯展示项）
  static const String none = 'none';
}

/// 一个插件注册的 UI 项
class MusicxxPluginUIItem {
  const MusicxxPluginUIItem({
    required this.id,
    required this.plugin,
    required this.type,
    this.order = 0,
    this.data = const <String, Object?>{},
  });

  /// 全名 `plugin.<插件id>.<短名>`（宿主保证在插件命名空间内）
  final String id;

  /// 所属插件 id
  final String plugin;

  /// 官方 UI 项类型（见 [MusicxxPluginUIType]）
  final String type;

  /// 排序权重（小者靠前；同权重按注册顺序）
  final int order;

  /// 声明式内容（结构由 [type] 决定）
  final Map<String, Object?> data;

  /// 短名（去掉 `plugin.<插件id>.` 前缀）
  String get name {
    final String prefix = 'plugin.$plugin.';
    return id.startsWith(prefix) ? id.substring(prefix.length) : id;
  }

  /// 标题（入口项/菜单项必然有；缺失返回空串）
  String get title => _stringOf(data['title']) ?? '';

  /// 副标题（可选）
  String? get subtitle => _stringOf(data['subtitle']);

  /// 图标名（可选；由宿主侧图标表解析，解析不到时用默认图标）
  String? get icon => _stringOf(data['icon']);

  /// 动作描述（缺省或 `kind: none` 时为 `null`）
  ///
  /// 解析走界面描述层的 `parseAction`：UI 项的动作与插件页面里的动作是同一套写法，
  /// 错误写法（未知 kind / 缺字段）会被忽略，不会抛异常。
  UiAction? get action {
    final UiAction? parsed = parseAction(data['action']);
    return (null == parsed || parsed.isEmpty) ? null : parsed;
  }

  /// 动作类型（见 [MusicxxPluginUIActionKind]；无动作返回 [MusicxxPluginUIActionKind.none]）
  String get actionKind {
    switch (action?.kind) {
      case ActionKind.route:
        return MusicxxPluginUIActionKind.route;
      case ActionKind.dispatch:
        return MusicxxPluginUIActionKind.dispatch;
      case ActionKind.command:
        return MusicxxPluginUIActionKind.command;
      default:
        return MusicxxPluginUIActionKind.none;
    }
  }

  /// 声明式插件页面的视图 id（`route` 动作；地址不属于本插件时返回 `null`）
  String? get viewId {
    final UiAction? value = action;
    if (null == value || value.kind != ActionKind.route) {
      return null;
    }
    final String prefix = 'ext://$plugin/';
    if (value.route.startsWith(prefix)) {
      final String view = value.route.substring(prefix.length);
      return view.isEmpty ? null : view;
    }
    return null;
  }

  /// 动作参数（`dispatch` / `command` 动作）
  Map<String, Object?> get actionArgs {
    final Object? args = action?.args;
    return args is Map
        ? args.cast<String, Object?>()
        : const <String, Object?>{};
  }

  /// 能力短名（`dispatch` 动作）
  String? get capabilityName => actionKind == MusicxxPluginUIActionKind.dispatch
      ? _nonEmpty(action?.name)
      : null;

  /// 官方动作全名（`command` 动作）
  String? get hostActionName => actionKind == MusicxxPluginUIActionKind.command
      ? _nonEmpty(action?.name)
      : null;

  factory MusicxxPluginUIItem.fromJson(Map<String, Object?> json) {
    final data = json['data'];
    return MusicxxPluginUIItem(
      id: json['id'] as String? ?? '',
      plugin: json['plugin'] as String? ?? '',
      type: json['type'] as String? ?? '',
      order: (json['order'] as num?)?.toInt() ?? 0,
      data: data is Map
          ? data.cast<String, Object?>()
          : const <String, Object?>{},
    );
  }

  /// 解析宿主快照 / 事件载荷里的 UI 项数组（未知字段忽略，非法项跳过）
  static List<MusicxxPluginUIItem> parseList(Object? json) {
    if (json is! List) {
      return const <MusicxxPluginUIItem>[];
    }
    final List<MusicxxPluginUIItem> result = <MusicxxPluginUIItem>[];
    for (final item in json) {
      if (item is! Map) {
        continue;
      }
      final MusicxxPluginUIItem parsed = MusicxxPluginUIItem.fromJson(
        item.cast<String, Object?>(),
      );
      if (parsed.id.isEmpty || parsed.type.isEmpty) {
        continue;
      }
      result.add(parsed);
    }
    result.sort(_compare);
    return result;
  }

  /// 排序：类型 → 权重 → 注册顺序（与宿主快照一致）
  static int _compare(MusicxxPluginUIItem a, MusicxxPluginUIItem b) {
    final int byType = a.type.compareTo(b.type);
    if (0 != byType) {
      return byType;
    }
    return a.order.compareTo(b.order);
  }

  static String? _stringOf(Object? value) {
    if (value is String && value.isNotEmpty) {
      return value;
    }
    return null;
  }

  static String? _nonEmpty(String? value) =>
      (null == value || value.isEmpty) ? null : value;

  @override
  String toString() => 'MusicxxPluginUIItem($type $id)';
}

/// UI 项集合的工具（应用侧 Store 用它维护"当前全部项"）
abstract final class MusicxxPluginUIItems {
  /// 按插件 id 整批替换（`musicxx.ui.changed` 事件的语义：载荷带该插件的全部项）
  static List<MusicxxPluginUIItem> replacePlugin(
    List<MusicxxPluginUIItem> current,
    String pluginId,
    List<MusicxxPluginUIItem> next,
  ) {
    final List<MusicxxPluginUIItem> result = <MusicxxPluginUIItem>[
      for (final MusicxxPluginUIItem item in current)
        if (item.plugin != pluginId) item,
      ...next,
    ];
    result.sort(MusicxxPluginUIItem._compare);
    return result;
  }

  /// 取某个类型的项（顺序与宿主快照一致）
  static List<MusicxxPluginUIItem> byType(
    List<MusicxxPluginUIItem> items,
    String type,
  ) => <MusicxxPluginUIItem>[
    for (final MusicxxPluginUIItem item in items)
      if (item.type == type) item,
  ];
}
