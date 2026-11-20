/// 声明式界面：声明项、内容、槽位
///
/// 插件**不写 Flutter 代码**，只做三件事：
/// - **声明项**（[MusicxxPluginUIItem]）：入口 / 菜单项 / 音效 / 可选样式的元信息，
///   只有数据、没有界面树；
/// - **内容**（[MusicxxPluginUIContent]）：挂在某个槽位上的界面 JSON，由生成函数按需给出；
/// - **槽位**（[MusicxxPluginUISlot]）：应用预留的渲染位置，或插件自己声明的位置。
///
/// 槽位与变量清单由 `tools/ui_slots.def.json`、`tools/ui_variables.def.json` 生成
/// （见 `ui_slots.g.dart` / `ui_variables.g.dart`），应用与插件写的是同一份名字。
///
/// 生命周期与来源：
/// - 注册表在原生插件框架里（`musicxx.ui` 表），按插件实例维护，卸载 / 禁用自动摘除；
/// - Dart 侧用 [MusicxxPluginManager.uiSnapshot] / `uiContents()` / `uiSlots()` 拉快照，
///   用事件（`musicxx.ui.changed` / `.contents` / `.content` / `.slots`）增量更新。
library;

import 'package:pluginxx_ui/pluginxx_ui.dart';

/// 声明项的类型（与生成的槽位表同一份声明）
///
/// 会出现在用户列表里的声明项（入口 / 菜单 / 可选样式）必须有 `data.title`；
/// 音效的 `pitch` / `cpuCost` 越界时按边界值保存。
abstract final class MusicxxPluginUIType {
  /// 主页入口
  static const String homeEntry = 'musicxx.ui.home.entry';

  /// 歌曲菜单项
  static const String songAction = 'musicxx.ui.song.action';

  /// 歌单菜单项
  static const String playlistAction = 'musicxx.ui.playlist.action';

  /// 「添加歌曲」弹窗里的来源项
  static const String addSongEntry = 'musicxx.ui.addsong.entry';

  /// 音效
  static const String audioEffect = 'musicxx.ui.audioEffect';

  /// 播放页背景（可选样式，用户选择生效）
  static const String playingBackground = 'musicxx.ui.playing.background';

  /// 播放页歌曲图（可选样式，用户选择生效）
  static const String playingIcon = 'musicxx.ui.playing.icon';

  /// 播放页音频动效（可选样式，用户选择生效）
  static const String playingVisualizer = 'musicxx.ui.playing.visualizer';

  /// 普通页面背景（按路由自动生效，没有用户选择）
  static const String pageBackground = 'musicxx.ui.page.background';
}

/// 声明项的动作：与界面描述层的动作写法完全一致
///
/// - 字符串短写 = `call`（调用本插件的能力）；
/// - `{kind: call, name, args, timeoutMs?}` / `{kind: route, route}` / `{kind: none}`；
/// - `call.name` 按前缀决定谁来处理：短名回自己、`plugin.<别的插件>.` 跨插件、
///   `musicxx.*` 调应用。
abstract final class MusicxxPluginUIActionKind {
  /// 打开声明式插件页面：`{kind:"route", route:"ext://<插件id>/<视图id>"}`
  static const String route = 'route';

  /// 调用能力（短名 / 其它插件 / 应用）或字符串短写
  static const String call = 'call';

  /// 无动作（纯展示项）
  static const String none = 'none';
}

/// 一个插件注册的声明项
class MusicxxPluginUIItem {
  const MusicxxPluginUIItem({
    required this.id,
    required this.plugin,
    required this.type,
    this.order = 0,
    this.rev = 0,
    this.data = const <String, Object?>{},
  });

  /// 全名 `plugin.<插件id>.<短名>`（插件框架保证在插件命名空间内）
  final String id;

  /// 所属插件 id
  final String plugin;

  /// 声明项类型（见 [MusicxxPluginUIType]）
  final String type;

  /// 排序权重（小者靠前；同权重按注册顺序）
  final int order;

  /// 声明项的修订号（重新注册时由插件框架递增，供渲染方判断"声明变了"）
  final int rev;

  /// 声明式数据（结构由 [type] 决定）
  final Map<String, Object?> data;

  /// 短名（去掉 `plugin.<插件id>.` 前缀）
  String get name {
    final String prefix = 'plugin.$plugin.';
    return id.startsWith(prefix) ? id.substring(prefix.length) : id;
  }

  /// 标题（入口项 / 菜单项 / 可选样式必然有；缺失返回空串）
  String get title => _stringOf(data['title']) ?? '';

  /// 副标题（可选）
  String? get subtitle => _stringOf(data['subtitle']);

  /// 图标路径（可选；**相对插件目录**的 svg 路径，可省 `.svg` 后缀）
  String? get icon => _stringOf(data['icon']);

  /// 动作描述（缺省或 `kind: none` 时为 `null`）
  UiAction? get action {
    final UiAction? parsed = parseAction(data['action']);
    return (null == parsed || parsed.isEmpty) ? null : parsed;
  }

  /// 动作类型（见 [MusicxxPluginUIActionKind]；无动作返回 [MusicxxPluginUIActionKind.none]）
  String get actionKind {
    switch (action?.kind) {
      case ActionKind.route:
        return MusicxxPluginUIActionKind.route;
      case ActionKind.call:
        return MusicxxPluginUIActionKind.call;
      default:
        return MusicxxPluginUIActionKind.none;
    }
  }

  /// 这次调用是不是给应用的能力（`musicxx.*`）
  bool get callsHost => action?.isHostCall ?? false;

  /// 这次调用是不是指向别的插件（`plugin.<别的插件>.`）
  bool callsOtherPlugin(String selfPluginId) =>
      action?.callsOtherPlugin(selfPluginId) ?? false;

  /// 能力名（`call` 动作；服务端已保证是短名 / `plugin.*` / `musicxx.*`）
  String? get capabilityName =>
      actionKind == MusicxxPluginUIActionKind.call
      ? _nonEmpty(action?.name)
      : null;

  /// 动作参数（`call` 动作）
  Map<String, Object?> get actionArgs {
    final Object? args = action?.args;
    return args is Map
        ? args.cast<String, Object?>()
        : const <String, Object?>{};
  }

  /// 插件页面的目标插件 id（`route` 动作；地址不合法时返回 `null`）
  ({String plugin, String content})? get routeTarget {
    final UiAction? value = action;
    if (null == value || value.kind != ActionKind.route) {
      return null;
    }
    const String prefix = 'ext://';
    final String route = value.route.trim();
    if (false == route.startsWith(prefix)) {
      return null;
    }
    final String rest = route.substring(prefix.length);
    final int split = rest.indexOf('/');
    if (split <= 0 || split >= rest.length - 1) {
      return null;
    }
    final String plugin = rest.substring(0, split).trim();
    final String content = rest.substring(split + 1).trim();
    if (plugin.isEmpty || content.isEmpty) {
      return null;
    }
    return (plugin: plugin, content: content);
  }

  /// 声明式插件页面的目标插件 id（`route` 动作）
  String? get routePlugin => routeTarget?.plugin;

  /// 声明式插件页面的内容 id（`route` 动作）
  String? get contentId => routeTarget?.content;

  factory MusicxxPluginUIItem.fromJson(Map<String, Object?> json) {
    final data = json['data'];
    return MusicxxPluginUIItem(
      id: json['id'] as String? ?? '',
      plugin: json['plugin'] as String? ?? '',
      type: json['type'] as String? ?? '',
      order: (json['order'] as num?)?.toInt() ?? 0,
      rev: (json['rev'] as num?)?.toInt() ?? 0,
      data: data is Map
          ? data.cast<String, Object?>()
          : const <String, Object?>{},
    );
  }

  /// 解析插件框架快照 / 事件载荷里的声明项数组（未知字段忽略，非法项跳过）
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

  /// 排序：类型 → 权重 → 注册顺序（与插件框架快照一致）
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

/// 一份内容的注册信息（插件挂在某个槽位上的界面 JSON 的生成器）
///
/// 内容本身由主程序按需向插件要（`musicxx.ui.content` 事件带 `json` / `data`），
/// 这里只有"谁挂在哪个槽位、依赖哪些变量"。
class MusicxxPluginUIContent {
  const MusicxxPluginUIContent({
    required this.id,
    required this.shortId,
    required this.plugin,
    required this.slot,
    this.title = '',
    this.depict = '',
    this.order = 0,
    this.minIntervalMs = 0,
    this.deps = const <String>[],
    this.rev = 0,
  });

  /// 全名 `plugin.<插件id>.<短 id>`
  final String id;

  /// 插件自己起的短 id（同一插件同一槽位内唯一）
  final String shortId;

  /// 所属插件 id
  final String plugin;

  /// 槽位 id（应用预留的，或插件声明的）
  final String slot;

  /// 展示名（可选样式槽位的候选列表用；空 = 用 [shortId]）
  ///
  /// 注册时就给了：用户要在**选中之前**就看到名字，那时还没有内容。
  final String title;

  /// 一句话说明（可选样式槽位的副标题；可为空）
  final String depict;

  /// 排序权重（同一槽位内小者靠前；同权重按注册顺序）
  final int order;

  /// 插件要求的额外时间间隔（毫秒；0 = 不按时间节流）
  final int minIntervalMs;

  /// 依赖的变量 id（插件按这些变量决定结构）
  final List<String> deps;

  /// 最近一次生成结果的版本（成功生成一次 +1）
  final int rev;

  factory MusicxxPluginUIContent.fromJson(Map<String, Object?> json) {
    final Object? deps = json['deps'];
    return MusicxxPluginUIContent(
      id: json['id'] as String? ?? '',
      shortId: json['shortId'] as String? ?? '',
      plugin: json['plugin'] as String? ?? '',
      slot: json['slot'] as String? ?? '',
      title: json['title'] as String? ?? '',
      depict: json['depict'] as String? ?? '',
      order: (json['order'] as num?)?.toInt() ?? 0,
      minIntervalMs: (json['minIntervalMs'] as num?)?.toInt() ?? 0,
      deps: deps is List
          ? <String>[
              for (final item in deps)
                if (item is String) item,
            ]
          : const <String>[],
      rev: (json['rev'] as num?)?.toInt() ?? 0,
    );
  }

  /// 解析内容注册表快照（非法项跳过）
  static List<MusicxxPluginUIContent> parseList(Object? json) {
    if (json is! List) {
      return const <MusicxxPluginUIContent>[];
    }
    final List<MusicxxPluginUIContent> result = <MusicxxPluginUIContent>[];
    for (final item in json) {
      if (item is! Map) {
        continue;
      }
      final MusicxxPluginUIContent parsed = MusicxxPluginUIContent.fromJson(
        item.cast<String, Object?>(),
      );
      if (parsed.id.isEmpty || parsed.slot.isEmpty) {
        continue;
      }
      result.add(parsed);
    }
    return result;
  }

  @override
  String toString() => 'MusicxxPluginUIContent($slot $id)';
}

/// 一个槽位：应用预留的（`owner` 为空）或插件声明的（`owner` = 那个插件）
class MusicxxPluginUISlot {
  const MusicxxPluginUISlot({
    required this.id,
    required this.kind,
    this.owner = '',
  });

  /// 槽位 id（`musicxx.ui.*` 或 `plugin.<插件id>.ui.<名字>`）
  final String id;

  /// 槽位形态：`item` / `selectable` / `pageBackground` / `replace` / `insert` /
  /// `page` / `overlay`（与 `pluginxx_ui` 的 `SlotKind` 同名）
  final String kind;

  /// 声明这个槽位的插件 id（应用预留的槽位为空串）
  final String owner;

  /// 是不是插件自己声明的槽位
  bool get isPluginDeclared => owner.isNotEmpty;

  factory MusicxxPluginUISlot.fromJson(Map<String, Object?> json) =>
      MusicxxPluginUISlot(
        id: json['id'] as String? ?? '',
        kind: json['kind'] as String? ?? '',
        owner: json['owner'] as String? ?? '',
      );

  /// 解析槽位表快照（非法项跳过）
  static List<MusicxxPluginUISlot> parseList(Object? json) {
    if (json is! List) {
      return const <MusicxxPluginUISlot>[];
    }
    final List<MusicxxPluginUISlot> result = <MusicxxPluginUISlot>[];
    for (final item in json) {
      if (item is! Map) {
        continue;
      }
      final MusicxxPluginUISlot parsed = MusicxxPluginUISlot.fromJson(
        item.cast<String, Object?>(),
      );
      if (parsed.id.isEmpty) {
        continue;
      }
      result.add(parsed);
    }
    return result;
  }

  @override
  String toString() => 'MusicxxPluginUISlot($kind $id)';
}

/// 声明项集合的工具（主程序 Store 用它维护"当前全部项"）
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

  /// 取某个类型的项（顺序与插件框架快照一致）
  static List<MusicxxPluginUIItem> byType(
    List<MusicxxPluginUIItem> items,
    String type,
  ) => <MusicxxPluginUIItem>[
    for (final MusicxxPluginUIItem item in items)
      if (item.type == type) item,
  ];

  /// 取某个槽位上的内容（按 `order` 与注册顺序，保持插件框架给的顺序）
  static List<MusicxxPluginUIContent> contentsOf(
    List<MusicxxPluginUIContent> contents,
    String slot,
  ) => <MusicxxPluginUIContent>[
    for (final MusicxxPluginUIContent content in contents)
      if (content.slot == slot) content,
  ];
}
