/// 槽位与变量清单的生成器
///
/// 声明源：
/// - `tools/ui_slots.def.json`：主程序预留的渲染位置（槽位）与不含界面树的声明项；
/// - `tools/ui_variables.def.json`：可以在界面 JSON 里绑定 / 插值的变量。
///
/// 产物：
/// | 产物 | 用途 |
/// |---|---|
/// | `lib/src/ui_slots.g.dart` | Dart 侧槽位常量、类型与查询（插件框架与主程序共用） |
/// | `lib/src/ui_variables.g.dart` | Dart 侧变量表与可用性判定 |
/// | `src/sdk/include/musicxx/plugin/api/ui_slots.g.h` | C++ 侧槽位宏与已知槽位表（插件 SDK 与框架校验共用） |
/// | `src/sdk/include/musicxx/plugin/api/ui_variables.g.h` | C++ 侧变量 id 表（deps 校验用） |
/// | `docs/ui-slots.md` | 插件作者看的槽位表 |
/// | `docs/ui-variables.md` | 插件作者看的变量表 |
///
/// 为什么只有一处声明：同一个槽位 id 同时出现在 C 宏、框架校验、Dart 常量、主程序挂载点与
/// 作者文档里，手写对齐迟早会漂。新增位置只改定义文件再跑生成器。
library;

import 'dart:convert';
import 'dart:io';

/// 生成结果：路径 → 内容
Map<String, String> buildUiContract() {
  final List<Map<String, Object?>> slots = _readList('tools/ui_slots.def.json');
  final List<Map<String, Object?>> variables = _readList(
    'tools/ui_variables.def.json',
  );
  return <String, String>{
    'lib/src/ui_slots.g.dart': _dartSlots(slots),
    'lib/src/ui_variables.g.dart': _dartVariables(variables),
    'src/sdk/include/musicxx/plugin/api/ui_slots.g.h': _cHeaderSlots(slots),
    'src/sdk/include/musicxx/plugin/api/ui_variables.g.h': _cHeaderVariables(
      variables,
    ),
    'docs/ui-slots.md': _docSlots(slots),
    'docs/ui-variables.md': _docVariables(variables),
  };
}

List<Map<String, Object?>> _readList(String path) {
  final String text = File(path).readAsStringSync();
  final Object? value = jsonDecode(text);
  if (value is! List) {
    throw StateError('$path 必须是 JSON 数组');
  }
  return <Map<String, Object?>>[
    for (final Object? item in value)
      if (item is Map<String, Object?>) item,
  ];
}

String _s(Object? value) => value is String ? value : '';

bool _b(Object? value) => value is bool && value;

/// `musicxx.ui.playing.controlBar.playOrPause` → `playing.controlBar.playOrPause`
String shortNameOf(String id) =>
    id.startsWith('musicxx.ui.') ? id.substring('musicxx.ui.'.length) : id;

/// 常量名（Dart）：`playing.controlBar.playOrPause` → `playingControlBarPlayOrPause`
String dartNameOf(String id) {
  final String short = shortNameOf(id);
  final StringBuffer out = StringBuffer();
  bool upper = false;
  for (final int rune in short.runes) {
    final String ch = String.fromCharCode(rune);
    if (ch == '.') {
      upper = true;
      continue;
    }
    out.write(upper ? ch.toUpperCase() : ch);
    upper = false;
  }
  return out.toString();
}

/// C 宏名：定义文件显式给 `macro`，否则由 id 推导
String macroNameOf(Map<String, Object?> slot) {
  final String given = _s(slot['macro']);
  if (given.isNotEmpty) {
    return 'MUSICXX_PLUGIN_UI_SLOT_$given';
  }
  final String short = shortNameOf(_s(slot['id']));
  final StringBuffer out = StringBuffer();
  for (int i = 0; i < short.length; i++) {
    final String ch = short[i];
    if (ch == '.') {
      out.write('_');
      continue;
    }
    final bool upper = ch.toUpperCase() == ch && ch.toLowerCase() != ch;
    final bool prevLower =
        i > 0 &&
        short[i - 1].toUpperCase() != short[i - 1] &&
        short[i - 1].toLowerCase() == short[i - 1];
    if (upper && prevLower) {
      out.write('_');
    }
    out.write(ch.toUpperCase());
  }
  return 'MUSICXX_PLUGIN_UI_SLOT_${out.toString()}';
}

String _dartSlots(List<Map<String, Object?>> slots) {
  final StringBuffer out = StringBuffer();
  out.writeln('// 本文件由 tools/gen_contract.dart 生成，请勿手工修改。');
  out.writeln('// 定义来源：tools/ui_slots.def.json');
  out.writeln('// 槽位（渲染位置）与声明项：id、kind、排序位置与说明。');
  out.writeln();
  out.writeln('/// 槽位的使用方式（主程序据它决定怎么用内容，插件据它知道该注册什么）');
  out.writeln('enum MusicxxUiSlotKind {');
  out.writeln('  /// 声明项：出现在主程序自己的列表里，不含界面树');
  out.writeln('  item,');
  out.writeln('  /// 插入：一个位置可以有多份内容，按 order 排');
  out.writeln('  insert,');
  out.writeln('  /// 替换：只认一份内容，没有内容时画内置界面');
  out.writeln('  replace,');
  out.writeln('  /// 可选样式：候选声明 + 用户选择');
  out.writeln('  selectable,');
  out.writeln('  /// 普通页面背景：按路由自动生效');
  out.writeln('  pageBackground,');
  out.writeln('  /// 搜索类型：一份内容就是一个类型，用户在页面上现选（不落设置）');
  out.writeln('  searchType,');
  out.writeln('  /// 插件页面：插件自己发起');
  out.writeln('  page,');
  out.writeln('  /// 插件浮层：插件自己发起');
  out.writeln('  overlay,');
  out.writeln('}');
  out.writeln();
  out.writeln('/// 一个槽位 / 声明项的元信息');
  out.writeln('class MusicxxUiSlotSpec_c {');
  out.writeln('  const MusicxxUiSlotSpec_c({');
  out.writeln('    required this.id,');
  out.writeln('    required this.kind,');
  out.writeln('    required this.label,');
  out.writeln('    required this.page,');
  out.writeln('    required this.position,');
  out.writeln('    required this.inList,');
  out.writeln('    required this.doc,');
  out.writeln('  });');
  out.writeln();
  out.writeln('  /// 全名（`musicxx.ui.*`）');
  out.writeln('  final String id;');
  out.writeln('  final MusicxxUiSlotKind kind;');
  out.writeln('  /// 中文叫法（错误提示与文档用）');
  out.writeln('  final String label;');
  out.writeln('  /// 所在页面');
  out.writeln('  final String page;');
  out.writeln('  /// 页面内的位置');
  out.writeln('  final String position;');
  out.writeln('  /// 内容是否画在列表里（在列表里时页面已经给过边距）');
  out.writeln('  final bool inList;');
  out.writeln('  /// 一句话说明');
  out.writeln('  final String doc;');
  out.writeln();
  out.writeln('  /// 简称（去掉 `musicxx.ui.` 前缀）');
  out.writeln(
    '  String get shortName => id.startsWith(kMusicxxUiSlotPrefix) '
    '? id.substring(kMusicxxUiSlotPrefix.length) : id;',
  );
  out.writeln();
  out.writeln('  @override');
  out.writeln("  String toString() => 'MusicxxUiSlotSpec_c(\$id)';");
  out.writeln('}');
  out.writeln();
  out.writeln("/// 槽位 id 的统一前缀");
  out.writeln("const String kMusicxxUiSlotPrefix = 'musicxx.ui.';");
  out.writeln();
  out.writeln('/// 主程序预留的全部槽位与声明项（顺序与定义文件一致）');
  out.writeln(
    'const List<MusicxxUiSlotSpec_c> kMusicxxUiSlots = <MusicxxUiSlotSpec_c>[',
  );
  for (final Map<String, Object?> slot in slots) {
    final String id = _s(slot['id']);
    final String kind = _s(slot['kind']);
    out.writeln('  MusicxxUiSlotSpec_c(');
    out.writeln("    id: '$id',");
    out.writeln('    kind: MusicxxUiSlotKind.$kind,');
    out.writeln("    label: '${_esc(_s(slot['label']))}',");
    out.writeln("    page: '${_esc(_s(slot['page']))}',");
    out.writeln("    position: '${_esc(_s(slot['position']))}',");
    out.writeln('    inList: ${_b(slot['inList']) ? 'true' : 'false'},');
    out.writeln("    doc: '${_esc(_s(slot['doc']))}',");
    out.writeln('  ),');
  }
  out.writeln('];');
  out.writeln();
  out.writeln('/// 槽位 id → 元信息');
  out.writeln('final Map<String, MusicxxUiSlotSpec_c> kMusicxxUiSlotIndex =');
  out.writeln('    <String, MusicxxUiSlotSpec_c>{');
  out.writeln(
    '  for (final MusicxxUiSlotSpec_c slot in kMusicxxUiSlots) slot.id: slot,',
  );
  out.writeln('};');
  out.writeln();
  out.writeln('/// 简称 → 元信息');
  out.writeln(
    'final Map<String, MusicxxUiSlotSpec_c> kMusicxxUiSlotShortIndex =',
  );
  out.writeln('    <String, MusicxxUiSlotSpec_c>{');
  out.writeln(
    '  for (final MusicxxUiSlotSpec_c slot in kMusicxxUiSlots) slot.shortName: slot,',
  );
  out.writeln('};');
  out.writeln();
  out.writeln('/// 主程序预留的槽位常量（插件与主程序都写这些名字，不要手打字符串）');
  out.writeln('abstract final class MusicxxUiSlots_c {');
  for (final Map<String, Object?> slot in slots) {
    final String id = _s(slot['id']);
    final String name = dartNameOf(id);
    out.writeln('  /// ${_esc(_s(slot['label']))}');
    out.writeln("  static const String $name = '$id';");
  }
  out.writeln('}');
  out.writeln();
  out.writeln('/// 把简称补成全名（已经是全名、或不是主程序的槽位时原样返回）');
  out.writeln('String musicxxUiSlotFullName(String id) {');
  out.writeln('  if (id.isEmpty || id.startsWith(kMusicxxUiSlotPrefix)) {');
  out.writeln('    return id;');
  out.writeln('  }');
  out.writeln('  if (kMusicxxUiSlotShortIndex.containsKey(id)) {');
  out.writeln('    return kMusicxxUiSlotPrefix + id;');
  out.writeln('  }');
  out.writeln('  return id;');
  out.writeln('}');
  out.writeln();
  out.writeln('/// 查一个槽位（接受简称或全名）；不是主程序的槽位返回 null');
  out.writeln('MusicxxUiSlotSpec_c? musicxxUiSlotOf(String id) {');
  out.writeln('  final String full = musicxxUiSlotFullName(id);');
  out.writeln('  return kMusicxxUiSlotIndex[full];');
  out.writeln('}');
  return out.toString();
}

String _dartVariables(List<Map<String, Object?>> variables) {
  final StringBuffer out = StringBuffer();
  out.writeln('// 本文件由 tools/gen_contract.dart 生成，请勿手工修改。');
  out.writeln('// 定义来源：tools/ui_variables.def.json');
  out.writeln('// 界面 JSON 里可以绑定 / 插值的变量（主程序的与槽位作用域内的）。');
  out.writeln();
  out.writeln('/// 变量的取值类型（只用于文档与自省，运行期按实际值处理）');
  out.writeln('enum MusicxxUiVariableType { string, number, bool, json }');
  out.writeln();
  out.writeln('/// 一个变量的元信息');
  out.writeln('class MusicxxUiVariableSpec_c {');
  out.writeln('  const MusicxxUiVariableSpec_c({');
  out.writeln('    required this.id,');
  out.writeln('    required this.type,');
  out.writeln('    required this.depict,');
  out.writeln('    this.slots = const <String>[],');
  out.writeln('  });');
  out.writeln();
  out.writeln('  /// 变量 id（`musicxx.*` 或 `scope:musicxx.*`）');
  out.writeln('  final String id;');
  out.writeln('  final MusicxxUiVariableType type;');
  out.writeln('  /// 中文说明');
  out.writeln('  final String depict;');
  out.writeln('  /// 作用域变量只在这些槽位可用；空 = 任何槽位');
  out.writeln('  final List<String> slots;');
  out.writeln();
  out.writeln(
    '  bool get isScope => id.startsWith(kMusicxxUiVariableScopePrefix);',
  );
  out.writeln();
  out.writeln('  @override');
  out.writeln("  String toString() => 'MusicxxUiVariableSpec_c(\$id)';");
  out.writeln('}');
  out.writeln();
  out.writeln("/// 作用域变量的前缀");
  out.writeln("const String kMusicxxUiVariableScopePrefix = 'scope:';");
  out.writeln();
  out.writeln('/// 全部变量（顺序与定义文件一致）');
  out.writeln(
    'const List<MusicxxUiVariableSpec_c> kMusicxxUiVariables = '
    '<MusicxxUiVariableSpec_c>[',
  );
  for (final Map<String, Object?> item in variables) {
    final String id = _s(item['id']);
    final String type = _s(item['type']);
    final List<Object?> slots = item['slots'] is List
        ? item['slots']! as List<Object?>
        : const <Object?>[];
    out.writeln('  MusicxxUiVariableSpec_c(');
    out.writeln("    id: '$id',");
    out.writeln('    type: MusicxxUiVariableType.$type,');
    out.writeln("    depict: '${_esc(_s(item['depict']))}',");
    if (slots.isEmpty) {
      out.writeln('    slots: <String>[],');
    } else {
      out.writeln('    slots: <String>[');
      for (final Object? slot in slots) {
        out.writeln("      '${_s(slot)}',");
      }
      out.writeln('    ],');
    }
    out.writeln('  ),');
  }
  out.writeln('];');
  out.writeln();
  out.writeln('/// 变量 id → 元信息');
  out.writeln(
    'final Map<String, MusicxxUiVariableSpec_c> kMusicxxUiVariableIndex =',
  );
  out.writeln('    <String, MusicxxUiVariableSpec_c>{');
  out.writeln(
    '  for (final MusicxxUiVariableSpec_c item in kMusicxxUiVariables) item.id: item,',
  );
  out.writeln('};');
  out.writeln();
  out.writeln('/// 查一个变量；没有声明过返回 null（插件自定义变量 `plugin.*` 不属于这里）');
  out.writeln(
    'MusicxxUiVariableSpec_c? musicxxUiVariableOf(String id) => '
    'kMusicxxUiVariableIndex[id];',
  );
  out.writeln();
  out.writeln('/// 这个变量能不能在这个槽位上用');
  out.writeln('///');
  out.writeln('/// 规则：全局变量任何槽位都能用；作用域变量只在自己的槽位（以及嵌在它们里面的');
  out.writeln('/// 嵌套槽位）能用。找不到声明时按 `false` 处理。');
  out.writeln(
    'bool musicxxUiVariableAllowedOn(String id, String slot, {bool nested = false}) {',
  );
  out.writeln(
    '  final MusicxxUiVariableSpec_c? spec = kMusicxxUiVariableIndex[id];',
  );
  out.writeln('  if (null == spec) {');
  out.writeln('    return false;');
  out.writeln('  }');
  out.writeln('  if (!spec.isScope || spec.slots.isEmpty) {');
  out.writeln('    return true;');
  out.writeln('  }');
  out.writeln('  if (nested) {');
  out.writeln('    // 嵌套槽位继承外层作用域：外层的槽位变量在里层同样可读');
  out.writeln('    return true;');
  out.writeln('  }');
  out.writeln('  return spec.slots.contains(slot);');
  out.writeln('}');
  return out.toString();
}

String _cHeaderSlots(List<Map<String, Object?>> slots) {
  final StringBuffer out = StringBuffer();
  out.writeln('/* 本文件由 tools/gen_contract.dart 生成, 请勿手工修改。');
  out.writeln(' * 定义来源: tools/ui_slots.def.json');
  out.writeln(' * 槽位(渲染位置)与声明项: C 宏 + 已知槽位表。 */');
  out.writeln('#ifndef MUSICXX_PLUGIN_UI_SLOTS_G_H');
  out.writeln('#define MUSICXX_PLUGIN_UI_SLOTS_G_H');
  out.writeln();
  out.writeln('#ifdef __cplusplus');
  out.writeln('extern "C" {');
  out.writeln('#endif');
  out.writeln();
  for (final Map<String, Object?> slot in slots) {
    out.writeln(
      '/* ${_esc(_s(slot['label']))}: ${_esc(_s(slot['page']))} / '
      '${_esc(_s(slot['position']))} */',
    );
    out.writeln('#define ${macroNameOf(slot)} "${_s(slot['id'])}"');
  }
  out.writeln();
  out.writeln('#ifdef __cplusplus');
  out.writeln('}');
  out.writeln('#endif');
  out.writeln();
  out.writeln('#ifdef __cplusplus');
  out.writeln('namespace musicxx {');
  out.writeln('namespace extern_plugin {');
  out.writeln();
  out.writeln('/// 一个槽位 / 声明项的元信息 (框架校验与自省用)');
  out.writeln('struct KnownUiSlot {');
  out.writeln('  const char *id;');
  out.writeln('  const char *kind;');
  out.writeln('  const char *label;');
  out.writeln('  bool in_list;');
  out.writeln('};');
  out.writeln();
  out.writeln('inline constexpr KnownUiSlot kKnownUiSlots[] = {');
  for (final Map<String, Object?> slot in slots) {
    out.writeln(
      '    {"${_s(slot['id'])}", "${_s(slot['kind'])}", '
      '"${_esc(_s(slot['label']))}", ${_b(slot['inList']) ? 'true' : 'false'}},',
    );
  }
  out.writeln('};');
  out.writeln();
  out.writeln('} // namespace extern_plugin');
  out.writeln('} // namespace musicxx');
  out.writeln('#endif');
  out.writeln();
  out.writeln('#endif /* MUSICXX_PLUGIN_UI_SLOTS_G_H */');
  return out.toString();
}

String _cHeaderVariables(List<Map<String, Object?>> variables) {
  final StringBuffer out = StringBuffer();
  out.writeln('/* 本文件由 tools/gen_contract.dart 生成, 请勿手工修改。');
  out.writeln(' * 定义来源: tools/ui_variables.def.json');
  out.writeln(' * 界面 JSON 里可以绑定的变量 id (deps 校验用)。 */');
  out.writeln('#ifndef MUSICXX_PLUGIN_UI_VARIABLES_G_H');
  out.writeln('#define MUSICXX_PLUGIN_UI_VARIABLES_G_H');
  out.writeln();
  out.writeln('#ifdef __cplusplus');
  out.writeln('namespace musicxx {');
  out.writeln('namespace extern_plugin {');
  out.writeln();
  out.writeln('/// 一个已声明的变量');
  out.writeln('struct KnownUiVariable {');
  out.writeln('  const char *id;');
  out.writeln('  bool scope;');
  out.writeln('};');
  out.writeln();
  out.writeln('inline constexpr KnownUiVariable kKnownUiVariables[] = {');
  for (final Map<String, Object?> item in variables) {
    final String id = _s(item['id']);
    final bool scope = id.startsWith('scope:');
    out.writeln('    {"$id", ${scope ? 'true' : 'false'}},');
  }
  out.writeln('};');
  out.writeln();
  out.writeln('} // namespace extern_plugin');
  out.writeln('} // namespace musicxx');
  out.writeln('#endif');
  out.writeln();
  out.writeln('#endif /* MUSICXX_PLUGIN_UI_VARIABLES_G_H */');
  return out.toString();
}

String _docSlots(List<Map<String, Object?>> slots) {
  final StringBuffer out = StringBuffer();
  out.writeln('# 槽位（渲染位置）一览');
  out.writeln();
  out.writeln(
    '> 本文件由 `tools/gen_contract.dart` 生成，定义来源 `tools/ui_slots.def.json`。',
  );
  out.writeln();
  out.writeln('槽位是**主程序预留的渲染位置**。插件往槽位上注册内容（`musicxx.ui.content.register`），');
  out.writeln('主程序只读缓存渲染，**不会在渲染路径上等插件**。`kind` 决定内容的用法：');
  out.writeln();
  out.writeln('| kind | 含义 |');
  out.writeln('|---|---|');
  out.writeln(
    '| `item` | 声明项：出现在主程序自己的列表里（`musicxx.ui.item.register`），不含界面树 |',
  );
  out.writeln('| `insert` | 插入：一个位置可以有多份内容，按 `order` 从上到下排 |');
  out.writeln('| `replace` | 替换：只认一份内容（`order` 最小），没有内容时画内置界面 |');
  out.writeln('| `selectable` | 可选样式：插件声明候选，用户在选择列表里选 |');
  out.writeln('| `pageBackground` | 普通页面背景：按路由自动生效，不用用户选择 |');
  out.writeln(
    '| `searchType` | 搜索类型：一份内容就是一个类型（注册里的 `title` 是标签名），用户在页面上现选，不落设置 |',
  );
  out.writeln('| `page` | 插件页面：地址是 `ext://<插件id>/<内容 id>` |');
  out.writeln(
    '| `overlay` | 插件浮层：`musicxx.ui.overlay.show` / `.showSheet` 打开 |',
  );
  out.writeln();
  out.writeln('## 清单');
  out.writeln();
  out.writeln('| 槽位 id | kind | 简称 | 位置 | 说明 |');
  out.writeln('|---|---|---|---|---|');
  for (final Map<String, Object?> slot in slots) {
    final String id = _s(slot['id']);
    out.writeln(
      '| `$id` | `${_s(slot['kind'])}` | `${shortNameOf(id)}` | '
      '${_esc(_s(slot['page']))} · ${_esc(_s(slot['position']))} | ${_esc(_s(slot['doc']))} |',
    );
  }
  out.writeln();
  out.writeln('## 写法');
  out.writeln();
  out.writeln('```js');
  out.writeln('musicxx.ui.content.register({');
  out.writeln('    id: "row1",');
  out.writeln('    slot: "playing.songlist.top",       // 简称或全名');
  out.writeln(
    '    title: "示例行",                    // 给人看的名字, 可选样式槽位的候选列表用它',
  );
  out.writeln('    depict: "插在播放列表顶部",          // 一句话说明 (可省)');
  out.writeln('    deps: ["musicxx.song.name"],        // 只订阅用到的变量');
  out.writeln('    order: 10,');
  out.writeln('}, function (ctx) {');
  out.writeln('    return { json: { blocks: [ /* … */ ] } };');
  out.writeln('});');
  out.writeln('```');
  out.writeln();
  out.writeln('插件也可以声明自己的槽位（`musicxx.ui.slot.define`，`kind` 只允许 `insert` /');
  out.writeln('`replace`），再在界面里用 `UISlot` 块嵌进去，让别的插件往那里填内容。');
  return out.toString();
}

String _docVariables(List<Map<String, Object?>> variables) {
  final StringBuffer out = StringBuffer();
  out.writeln('# 变量一览');
  out.writeln();
  out.writeln(
    '> 本文件由 `tools/gen_contract.dart` 生成，定义来源 `tools/ui_variables.def.json`。',
  );
  out.writeln();
  out.writeln('变量可以写在界面的任何值位置：`{"var": "音乐歌曲名变量"}` 或字符串里的 `{变量 id}`。');
  out.writeln('内容注册时在 `deps` 里声明用到的变量，主程序只订阅这些：变化时先重新绑定（立刻看到新值），');
  out.writeln('再让插件重新生成一次结构（串行，只保留最新一次）。');
  out.writeln();
  out.writeln('三种前缀：');
  out.writeln();
  out.writeln('| 前缀 | 来源 | 可用范围 |');
  out.writeln('|---|---|---|');
  out.writeln('| `musicxx.` | 主程序状态 | 任何槽位 |');
  out.writeln('| `scope:musicxx.` | 主程序状态 | 只在自己的槽位（以及嵌在里面的嵌套槽位） |');
  out.writeln('| `plugin.` | 插件广播频道 | 任何槽位（可以跨插件） |');
  out.writeln();
  out.writeln('## 全局变量');
  out.writeln();
  out.writeln('| 变量 id | 类型 | 说明 |');
  out.writeln('|---|---|---|');
  for (final Map<String, Object?> item in variables) {
    final String id = _s(item['id']);
    if (id.startsWith('scope:')) {
      continue;
    }
    out.writeln(
      '| `$id` | `${_s(item['type'])}` | ${_esc(_s(item['depict']))} |',
    );
  }
  out.writeln();
  out.writeln('## 作用域变量');
  out.writeln();
  out.writeln('| 变量 id | 类型 | 可用槽位 | 说明 |');
  out.writeln('|---|---|---|---|');
  for (final Map<String, Object?> item in variables) {
    final String id = _s(item['id']);
    if (!id.startsWith('scope:')) {
      continue;
    }
    final Object? rawSlots = item['slots'];
    final String slots = (rawSlots is List && rawSlots.isNotEmpty)
        ? rawSlots.map((Object? v) => '`$v`').join('<br>')
        : '任何（嵌套槽位）';
    out.writeln(
      '| `$id` | `${_s(item['type'])}` | $slots | ${_esc(_s(item['depict']))} |',
    );
  }
  out.writeln();
  out.writeln('## 求不到值时');
  out.writeln();
  out.writeln(
    '- `{"var": …}`：有 `default` 用 `default`，没有就按"这个键没写"处理（字段用自己的缺省值）；',
  );
  out.writeln('- 字符串里的 `{变量 id}`：换成空串；');
  out.writeln('- 两种情况都会在日志里记一条 `missing`（同一份内容只记一次）。');
  return out.toString();
}

String _esc(String text) => text
    .replaceAll('\\', r'\\')
    .replaceAll("'", r"\'")
    .replaceAll(r'$', r'\$')
    .replaceAll('\n', ' ');
