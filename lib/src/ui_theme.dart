/// musicxx 的界面样式作用域规格：`ThemeScope.data` 的字段定义与解析
///
/// 描述层的 `ThemeScope` 只规定"`data` 是一个对象、`child` 是一块内容"，含义由客户端定义；
/// 本文件就是 musicxx 的那一份定义：`data` 里按组写颜色，客户端把解析结果映射到自己的色板，
/// 子树里的文字 / 按钮 / 卡片 / 遮罩三色按新色板取色。
///
/// ```jsonc
/// {
///   "kind": "ThemeScope",
///   "data": {
///     "text":    { "main": "#ff202124", "cross": "#99202124" },
///     "surface": { "primary": "#ff3a7bd5", "background": "#ffffffff" },
///     "button":  { "content": "#ff202124", "selectContent": "#ffffffff" },
///     "mask":    { "main": "#ffffffff" },
///     "shimmer": { "base": "#80000000" }
///   },
///   "child": { "kind": "Column", "children": [ … ] }
/// }
/// ```
///
/// 规则：
/// - 颜色串是 `#rrggbb` / `#aarrggbb`（与 `musicxx.ui.playing.foreground.set` 一致），
///   不带 `#` 也认；一个可用颜色都没有时当成"没有声明"；
/// - **只覆盖写了的角色**，没写的从父作用域（外层 `ThemeScope` 或客户端主题）继承；
/// - 不认识的键、不是对象的组、解析不出来的颜色都跳过（可由 [warn] 收提示），不会让整块失效；
/// - 只管颜色：圆角、阴影、渐变、字号、间距这些结构样式不开放。
library;

/// 颜色角色名与分组
///
/// 角色名是 `组.键`（例如 `text.main`），`data` 里的嵌套结构就是它按 `.` 拆开的样子。
abstract final class MusicxxThemeScopeRole_c {
  static const String groupText = 'text';
  static const String groupSurface = 'surface';
  static const String groupButton = 'button';
  static const String groupMask = 'mask';
  static const String groupShimmer = 'shimmer';

  /// 组名 -> 这一组允许的键（顺序与文档一致）
  static const Map<String, List<String>> groups = <String, List<String>>{
    groupText: <String>['main', 'cross', 'title', 'titleBackground'],
    groupSurface: <String>[
      'primary',
      'background',
      'backgroundCross',
      'error',
      'disable',
      'dialog',
      'dialogBarrier',
    ],
    groupButton: <String>[
      'content',
      'simpleContent',
      'selectContent',
      'disableContent',
    ],
    groupMask: <String>['main', 'cross', 'block'],
    groupShimmer: <String>['base', 'highlight', 'decoration'],
  };

  /// 组名 + 键 → 角色名（`text.main`）
  static String of(String group, String key) => '$group.$key';

  /// 全部角色名（按组声明顺序）
  static List<String> get all => <String>[
    for (final MapEntry<String, List<String>> group in groups.entries)
      for (final String key in group.value) of(group.key, key),
  ];
}

/// `ThemeScope.data` 的解析结果
///
/// 只保存"写了哪些颜色"，按角色名（`text.main`）取 ARGB 整数；客户端自己把它映射到色板。
class MusicxxThemeScopeData_c {
  const MusicxxThemeScopeData_c(this.colors);

  /// 角色名 -> ARGB（`0xAARRGGBB`）
  final Map<String, int> colors;

  /// 一个可用颜色都没有
  bool get isEmpty => colors.isEmpty;
  bool get isNotEmpty => !isEmpty;

  /// 某个角色的颜色（没写返回 null）
  int? colorOf(String role) => colors[role];

  /// 取某一组的全部颜色（角色名 → ARGB，只含写了的）
  Map<String, int> groupOf(String group) => <String, int>{
    for (final MapEntry<String, int> entry in colors.entries)
      if (entry.key.startsWith('$group.')) entry.key: entry.value,
  };

  /// 写回 `data` 的形状（只含写了的角色）
  Map<String, Object?> toJson() {
    final Map<String, Object?> out = <String, Object?>{};
    for (final MapEntry<String, List<String>> group
        in MusicxxThemeScopeRole_c.groups.entries) {
      final Map<String, Object?> keys = <String, Object?>{};
      for (final String key in group.value) {
        final int? color = colors[MusicxxThemeScopeRole_c.of(group.key, key)];
        if (null != color) {
          keys[key] = formatColor(color);
        }
      }
      if (keys.isNotEmpty) {
        out[group.key] = keys;
      }
    }
    return out;
  }

  /// 解析 `data`
  ///
  /// 不是对象、或一个可用颜色都没有时返回 null（调用方按"没有声明样式"处理）。
  /// [warn] 收容错提示（每个问题一条，文案已经带上组名 / 键名）。
  static MusicxxThemeScopeData_c? tryParse(
    Object? json, {
    void Function(String message)? warn,
  }) {
    if (json is! Map) {
      warn?.call('data is not an object (expected {group: {key: color}})');
      return null;
    }
    final Map<String, int> colors = <String, int>{};
    for (final MapEntry<Object?, Object?> group in json.entries) {
      final String groupName = group.key is String ? group.key! as String : '';
      final List<String>? keys = MusicxxThemeScopeRole_c.groups[groupName];
      if (null == keys) {
        warn?.call(
          'unknown group `$groupName` in data (known groups: '
          '${MusicxxThemeScopeRole_c.groups.keys.join(' / ')})',
        );
        continue;
      }
      final Object? value = group.value;
      if (value is! Map) {
        warn?.call('data.$groupName must be an object ({key: color})');
        continue;
      }
      for (final MapEntry<Object?, Object?> entry in value.entries) {
        final String key = entry.key is String ? entry.key! as String : '';
        if (!keys.contains(key)) {
          warn?.call(
            'data.$groupName has no color `$key` (known keys: '
            '${keys.join(' / ')})',
          );
          continue;
        }
        final int? color = parseColor(entry.value);
        if (null == color) {
          warn?.call(
            'cannot parse the color `${entry.value}` of data.$groupName.$key'
            ' (expected #rrggbb or #aarrggbb)',
          );
          continue;
        }
        colors[MusicxxThemeScopeRole_c.of(groupName, key)] = color;
      }
    }
    return colors.isEmpty ? null : MusicxxThemeScopeData_c(colors);
  }

  /// `#rrggbb` / `#aarrggbb` → ARGB 整数（与 `musicxx.ui.playing.foreground.set` 同一套写法）
  ///
  /// 不带 `#` 也认（`rrggbb` / `aarrggbb`）；位数不对、含非十六进制字符时返回 null。
  static int? parseColor(Object? raw) {
    if (raw is! String) {
      return null;
    }
    String text = raw.trim();
    if (text.startsWith('#')) {
      text = text.substring(1);
    }
    if (6 != text.length && 8 != text.length) {
      return null;
    }
    final int? value = int.tryParse(text, radix: 16);
    if (null == value) {
      return null;
    }
    return 6 == text.length ? (0xff000000 | value) : value;
  }

  /// ARGB 整数 → `#aarrggbb`（写回 `data` 用）
  static String formatColor(int argb) =>
      '#${(argb & 0xffffffff).toRadixString(16).padLeft(8, '0')}';
}
