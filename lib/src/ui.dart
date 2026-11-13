/// 声明式 UI 扩展模型
///
/// 插件**不写 Flutter 代码**：它只注册"UI 项"，用官方类型 + JSON 内容描述
/// "长什么样、点了做什么"；渲染由 musicxx 主程序负责
/// （见 `lib/pages/plugins/externPlugin/` 与 `lib/plugin/externPlugin/ExternPluginView.dart`）。
///
/// 生命周期与来源：
/// - 注册表在原生插件框架里（`musicxx.ui` 表），插件框架按插件实例维护，卸载/禁用自动摘除；
/// - Dart 侧用 [MusicxxPluginManager.uiSnapshot] 拉全量快照，用
///   [MusicxxPluginEventType.uiChanged] 事件整批替换某个插件的项（事件载荷带 `items`）。
library;

import 'package:pluginxx_ui/pluginxx_ui.dart';

/// UI 项类型（官方；与 `MUSICXX_PLUGIN_UI_TYPE_*` 一一对应）
///
/// 入口项与菜单项的 `icon` 写**插件自己带的 svg 路径**：一条相对插件目录的路径
/// （`"resource/svg/logo.svg"` 或省掉后缀的 `"resource/svg/logo"`；用 `/` 分隔，允许子目录）。
/// **`resource/svg/` 只是推荐位置**，插件可以按自己的目录结构放图标。插件框架不接受自己的图标名，
/// 路径不合法或文件缺失时按"没有图标"处理（入口/菜单退回默认图标，页面里的 `Icon` 块退回
/// `glyph`）—— 详见包内 `docs/plugin-ui.md` §1.5。
abstract final class MusicxxPluginUIType {
  /// 功能主页入口项：`data = {title, subtitle?, icon?, action?}`
  static const String homeEntry = 'musicxx.ui.home.entry';

  /// 歌曲菜单项：`data = {title, icon?, action?}`
  static const String songAction = 'musicxx.ui.song.action';

  /// 歌单菜单项：`data = {title, icon?, action?}`
  static const String playlistAction = 'musicxx.ui.playlist.action';

  /// 「添加歌曲」弹窗里的来源项：`data = {title, subtitle?, icon?, action?}`
  ///
  /// 位置：歌曲列表页 [+] → 「添加歌曲」弹窗，排在内置来源之后、“从其他歌单导入”这类工具项之前。
  /// 点击时主程序把当前歌单的上下文（`pid` / `name` / `songNum`）合进动作参数。
  /// 典型写法是把动作指向 `musicxx.ui.overlay.showSheet`（打开一层插件视图浮层），
  /// 或者指向自己的能力，返回一个视图描述（主程序会把它当成插件页面打开）。
  static const String addSongEntry = 'musicxx.ui.addsong.entry';

  /// 播放页背景样式（渲染槽位 `player.background`）：
  /// `data = {title, depict?, enabled?, shader:{bundle, vertex?, fragment?},
  ///          args?（成员名 → 值表达式）, cover?（封面纹理，见 docs/plugin-shader-bundle.md §5.1）,
  ///          speed?, resolutionScale?, maxFps?, animate?, scrim?, foregroundStyle?}`
  ///
  /// 着色器参数只有 `args` 一种写法（旧的 `colors` 已移除）；不写 `args` 时插件框架默认给内置背景的
  /// 4 个绘制色（`icon.themeMapping.0..3`）。`cover` 由主程序解码 + 缩放（可选预模糊）后上传成
  /// GPU 纹理、每帧绑定给着色器，插件不搬字节。详见 `docs/plugin-shader-bundle.md`。
  static const String playingBackground = 'musicxx.ui.playing.background';

  /// 播放页歌曲图（渲染槽位 `player.icon`）：接管播放页中间那张歌曲图 / 视频的显示
  ///
  /// `data = {title, depict?, enabled?, mode?: "none"|"shader"|"view", keepSpace?, fill?,
  ///          shader?: {bundle, vertex?, fragment?}, view?: "<视图id>" | {视图对象},
  ///          cover?, image?, args?, speed?, resolutionScale?, maxFps?, animate?}`
  ///
  /// 三种接管方式（不写 `mode` 时按字段推断：有 `shader.bundle` → `shader`，有 `view` → `view`，
  /// 都没有 → `none`）：
  /// - `shader`：插件着色器绘制（渲染输入与播放页背景**完全一致**，见
  ///   docs/plugin-shader-bundle.md：`cover` / `image` / `args` / `speed` / `maxFps` /
  ///   `resolutionScale` / `animate`）；
  /// - `view`：插件用声明式界面组合（`view` 写视图 id 或直接给视图对象），
  ///   渲染与动作（`dispatch` / `command` / `route`）和插件页面同一套；
  /// - `none`：不显示内容；`keepSpace`（缺省 true）决定是否保留原来的占位尺寸。
  ///
  /// 接管后原来的"点击歌曲图播放 / 暂停"不再生效（整个显示被替换）：需要这个交互就在自己的
  /// 界面里放按钮，动作用 `command` 调官方能力 `musicxx.player.toggle`。
  ///
  /// 设置入口是「设置 → 主题 → 播放页面歌曲图」，内置显示按形状分成三条候选
  /// `musicxx.circle` / `musicxx.square` / `musicxx.adaptive` 与插件项在同一份列表里。
  /// `musicxx.previous` 是"交还控制权"（恢复上一个形状，没有记录时不动设置项，重复调用安全）；
  /// `musicxx.default` 是"重置成默认形状"（写之前会记下原来的值）。两个动作都不进列表，
  /// 背景 / 歌曲图 / 音频动效三个槽位都支持它们（内置项 id 统一是 `musicxx.<名字>`）。
  static const String playingIcon = 'musicxx.ui.playing.icon';

  /// 播放页音频可视化样式（渲染槽位 `player.visualizer`）：与内置『音乐动效』的四种样式在同一个
  /// 列表里选，**没安装内置插件时插件样式照样可用**
  ///
  /// `data = {title, depict?, enabled?, mode?: "none"|"shader"|"view",
  ///          shader?: {bundle, vertex?, fragment?}, view?: "<视图id>" | {视图对象},
  ///          cover?, image?, args?, speed?, resolutionScale?, maxFps?, animate?}`
  ///
  /// 字段与 [playingIcon] 完全一致：`shader` 是插件着色器绘制（渲染输入与播放页背景共用；
  /// 频谱用 `musicxx.spectrum.*` 来源现读），`view` 是插件声明的界面组合（可以配
  /// `musicxx.AnimatedBuilder` 做逐帧动效），`none` 不显示内容。
  ///
  /// 渲染区域是播放页的动效区（封面下方那一块），**不接手势**：原来的封面点击、拖动切歌、
  /// 迷你歌词要继续可用，所以插件不要假设自己能收到点击。
  static const String playingVisualizer = 'musicxx.ui.playing.visualizer';

  /// 播放页底部控制按钮栏：**整栏**由插件画（替换，不是插入）
  ///
  /// `data = {view: "<视图id>" | {视图对象} | [块数组], enabled?}`
  ///
  /// 有插件声明这个类型时，内置的五个按钮（菜单 / 上一曲 / 播放暂停 / 下一曲 / 收藏）
  /// 不再显示，整栏都由这个视图负责：按钮、图标、点击动作都在视图里写
  /// （动作用 `command` 调官方能力，见 `docs/plugin-api.md` §3.2）。
  ///
  /// **同一个位置只认一项**：`order` 小的优先，同 `order` 按注册顺序；
  /// 先声明整栏的插件生效时，下面五个按钮级类型不再参与渲染。
  /// 取不到内容（能力没实现 / 返回的界面解析不出来）时回到内置界面，
  /// 插件停用 / 卸载后也一样。见 `docs/plugin-ui.md` §1.7。
  static const String playingControlBar = 'musicxx.ui.playing.controlBar';

  /// 播放页控制按钮栏的「菜单」按钮：只替换这一个按钮的显示（内置栏布局不变）
  ///
  /// `data = {view: "<视图id>" | {视图对象} | [块数组], enabled?}`
  ///
  /// 内置按钮的行为是打开「更多」菜单，插件可以用官方能力
  /// `musicxx.ui.playing.openMenu` 做同一件事。
  static const String playingControlBarMenu =
      'musicxx.ui.playing.controlBar.menu';

  /// 播放页控制按钮栏的「上一曲」按钮（只替换显示）
  ///
  /// 对应的官方能力是 `musicxx.player.prev`。
  static const String playingControlBarPrevious =
      'musicxx.ui.playing.controlBar.previous';

  /// 播放页控制按钮栏的「播放 / 暂停」按钮（只替换显示）
  ///
  /// 对应的官方能力是 `musicxx.player.toggle`（与内置按钮同一行为）。
  /// 视图参数里有 `playing`（当前是否在播放），播放状态变了主程序会重新取一次视图。
  static const String playingControlBarPlayOrPause =
      'musicxx.ui.playing.controlBar.playOrPause';

  /// 播放页控制按钮栏的「下一曲」按钮（只替换显示）
  ///
  /// 对应的官方能力是 `musicxx.player.next`。
  static const String playingControlBarNext =
      'musicxx.ui.playing.controlBar.next';

  /// 播放页控制按钮栏的「收藏」按钮（只替换显示）
  ///
  /// 对应的官方能力是 `musicxx.playlist.song.favorite`（收藏 / 取消收藏当前歌曲）。
  /// 视图参数里有 `favorite`（当前歌曲是否已收藏），收藏状态变了主程序会重新取一次视图。
  static const String playingControlBarFavorite =
      'musicxx.ui.playing.controlBar.favorite';

  /// 播放页子页面的插入块：**十个位置各一个类型，类型本身就是位置**（没有 `slot` 字段）
  ///
  /// `data = {view: "<视图id>" | {视图对象} | [块数组], enabled?}`
  ///
  /// 页面固定区域（不随列表滚动）：
  /// - [playingLyricTop] / [playingLyricBottom]：歌词页顶部 / 底部；
  /// - [playingSonglistTop] / [playingSonglistBottom]：播放列表页顶部 / 底部。
  ///
  /// 列表滚动内容里（`header` / `footer`）：
  /// - [playingLyricHeader] / [playingLyricFooter]：歌词列表的 header / footer；
  /// - [playingSonglistHeader] / [playingSonglistFooter]：播放列表的 header / footer；
  /// - [playingMoreInfoHeader] / [playingMoreInfoFooter]：更多信息页内容开头 / 末尾（整页滚动）。
  ///
  /// 同一位置可以注册多项，按 UI 项的 `order` 与注册顺序从上到下排列；这些类型都不需要
  /// `title`（它们不画在任何列表里）。插入块的页面已经给过页面边距，块只画自己的内容；
  /// 取不到内容时只是不渲染，不影响页面与同一位置的其它项。
  static const String playingLyricTop = 'musicxx.ui.playing.lyric.top';

  /// 歌词页底部（列表之下，不随内容滚动）
  static const String playingLyricBottom = 'musicxx.ui.playing.lyric.bottom';

  /// 歌词列表内顶部（列表的 header，跟随内容滚动）
  static const String playingLyricHeader = 'musicxx.ui.playing.lyric.header';

  /// 歌词列表内底部（列表的 footer，跟随内容滚动）
  static const String playingLyricFooter = 'musicxx.ui.playing.lyric.footer';

  /// 更多信息页内容开头（整页滚动，这一块在内容的最前面）
  static const String playingMoreInfoHeader =
      'musicxx.ui.playing.moreInfo.header';

  /// 更多信息页内容末尾（整页滚动，这一块在内容的最后面）
  static const String playingMoreInfoFooter =
      'musicxx.ui.playing.moreInfo.footer';

  /// 播放列表页顶部（列表之上，不随内容滚动）
  static const String playingSonglistTop = 'musicxx.ui.playing.songlist.top';

  /// 播放列表页底部（列表之下，不随内容滚动）
  static const String playingSonglistBottom =
      'musicxx.ui.playing.songlist.bottom';

  /// 播放列表内顶部（列表的 header，跟随内容滚动）
  static const String playingSonglistHeader =
      'musicxx.ui.playing.songlist.header';

  /// 播放列表内底部（列表的 footer，跟随内容滚动）
  static const String playingSonglistFooter =
      'musicxx.ui.playing.songlist.footer';
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

  /// 调用主程序官方能力：`{kind:"command", name:"musicxx.<域>.<能力>", args:{...}}`
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

  /// 全名 `plugin.<插件id>.<短名>`（插件框架保证在插件命名空间内）
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

  /// 图标路径（可选；**相对插件目录**的 svg 路径，可省 `.svg` 后缀，缺失时渲染方退回默认图标）
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

  /// `route` 动作的目标（任意插件的 `ext://<插件id>/<视图id>`；不是 route 或地址不合法时返回 `null`）
  ///
  /// `route` **允许指向任意插件**的页面（含自己）：页面由目标插件自己绘制（内容来自它的能力），
  /// 发起方拿不到对方的数据。例如 `ext://other_plugin/card` → 插件 `other_plugin`、视图 `card`；
  /// 视图 id 是第一个斜杠之后的全部内容（里面可以再带斜杠），两端空白会被去掉。
  ///
  /// 主程序侧有一份规则相同的实现（`ExternPluginUIRoutes.parsePluginViewRoute`，供
  /// `musicxx.ui.openRoute` 动作与页面里的动作使用）；**改这里的判定要同步改那一份**。
  ({String plugin, String view})? _routeOf() {
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
    final String target = rest.substring(0, split).trim();
    final String view = rest.substring(split + 1).trim();
    if (target.isEmpty || view.isEmpty) {
      return null;
    }
    return (plugin: target, view: view);
  }

  /// 声明式插件页面的**目标插件 id**（`route` 动作；地址不合法时返回 `null`）
  ///
  /// 与 [viewId] 配对使用；目标可以是**任意插件**（见 [_routeOf]）。
  String? get routePlugin => _routeOf()?.plugin;

  /// 声明式插件页面的**视图 id**（`route` 动作；地址不合法时返回 `null`）
  ///
  /// 视图 id 与目标插件的**能力短名**一致（主程序打开页面时调用同名能力取视图描述）。
  String? get viewId => _routeOf()?.view;

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

  /// 官方能力全名（`command` 动作）
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

  /// 解析插件框架快照 / 事件载荷里的 UI 项数组（未知字段忽略，非法项跳过）
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

/// UI 项集合的工具（主程序 Store 用它维护"当前全部项"）
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
}
