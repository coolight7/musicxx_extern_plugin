/// 能力 / 广播契约的生成核心（与具体项目无关）
///
/// 这一层只认**结构字段**：`id`、`phases`、`impls`、`merge`、`cache`、`dispatch`，
/// 以及通道块 `cli` / `mcp` / `plugin`；广播是 `cache` / `publish` / `pushOnSubscribe` /
/// `coalesce` / `scope` / `type` / `options`。`title` / `depict` / `doc` 这类展示文案
/// 会读进模型，但不参与这里的生成 —— 只有项目的文档生成器用它。
///
/// 为什么单独一层：另一个项目（agentxx）用同一套声明格式，id / 阶段 / 合并 / 缓存 /
/// 派发的生成逻辑两边一模一样，差异只在文档措辞与产物路径上。
/// 那些留在 `musicxx_docs.dart` 与 `tools/gen_capabilities.dart` 里。
library;

import 'dart:convert';
import 'dart:io';

/* ==================== 编码（Dart 与 C++ 两侧共用） ==================== */

/// 实现槽位：`one`（后登记覆盖先登记）/ `many`（多实现共存）
const Map<String, int> _implsCodes = <String, int>{'one': 0, 'many': 1};

/// 合并策略：`first`（第一个合法结果生效）/ `veto`（任一实现能否）
const Map<String, int> _mergeCodes = <String, int>{'first': 1, 'veto': 2};

/// 阶段编号（与 `MUSICXX_CAPABILITY_PHASE_*` 一致：impl 0 / before 1 / after 2）
const Map<String, int> _phaseCodes = <String, int>{
  'impl': 0,
  'before': 1,
  'after': 2,
};

/// 阶段的固定次序（生成物里统一按 before → impl → after 写）
const List<String> _phaseOrder = <String>['before', 'impl', 'after'];

/// 派发方式：`sync`（调用方等结果）/ `async`（不占用调用线程）
const Map<String, int> _dispatchCodes = <String, int>{'sync': 0, 'async': 1};

/// 能力的值缓存：`none` / `byIdentity`（同一请求身份不重复算）
const Map<String, int> _cacheCodes = <String, int>{'none': 0, 'byIdentity': 1};

/// 广播的缓存：`none`（没有当前值）/ `latest`（有当前值，订阅即拿到，也能同步读）
const Map<String, int> _broadcastCacheCodes = <String, int>{
  'none': 0,
  'latest': 1,
};

/// 广播的发布权限：`owner`（只有属主能发布）/ `anyone`（任何人都能发布）
const Map<String, int> _publishCodes = <String, int>{'owner': 0, 'anyone': 1};

/// 广播值的类型提示（可选；只用于文档与自省，不参与派发）
const List<String> _broadcastTypes = <String>[
  'bool',
  'number',
  'string',
  'json',
];

/// 广播的作用域：`app`（业务频道）/ `system`（调试与自省频道，插件不能写）
const Map<String, int> _scopeCodes = <String, int>{'app': 0, 'system': 1};

/// 外部通道位掩码：一个能力可以同时有几个
///
/// 声明里的三个可选块 `cli` / `mcp` / `plugin` 各对应一位；没有块 = 只能应用内部调
/// （纯实现 / 干预类能力，例如 `musicxx.player.source.resolve`）。
const Map<String, int> _channelCodes = <String, int>{
  'cli': 1,
  'mcp': 2,
  'plugin': 4,
};

/// MCP 工具名的字符集：协议不接受点号，统一 PascalCase
final RegExp _mcpNamePattern = RegExp(r'^[A-Z][A-Za-z0-9]*$');

/// MCP 工具说明的格式：`text`（排好版的文本）/ `json`（结构化，缺省）
const Map<String, int> _mcpFormatCodes = <String, int>{'json': 0, 'text': 1};

/* ==================== 声明模型 ==================== */

/// 按 id 生成默认的 MCP 工具名：`musicxx.player.toggle` → `PlayerToggle`
///
/// 段分隔符（`.` / `_`）各算一个词边界，其余原样保留；工具名不能带点（MCP 协议不接受）。
String defaultMcpNameOf(String id) {
  final String body = id.startsWith('musicxx.')
      ? id.substring('musicxx.'.length)
      : id;
  final StringBuffer sb = StringBuffer();
  for (final String part in body.split(RegExp(r'[._]'))) {
    if (part.isEmpty) {
      continue;
    }
    sb.write(part[0].toUpperCase());
    sb.write(part.substring(1));
  }
  return sb.toString();
}

/// 一条能力声明
final class CapabilityDeclaration {
  CapabilityDeclaration({
    required this.id,
    required this.dart,
    required this.title,
    required this.depict,
    required this.impls,
    required this.merge,
    required this.phases,
    required this.dispatch,
    required this.timeoutMs,
    required this.cache,
    required this.cli,
    required this.commandTitle,
    required this.consent,
    required this.mcp,
    required this.mcpName,
    required this.mcpFormat,
    required this.mcpTitle,
    required this.mcpDepict,
    required this.plugin,
    required this.doc,
  });

  final String id;
  final String dart;
  final String title;
  final String depict;

  /// `one` / `many`
  final String impls;

  /// `first` / `veto`；不适用时为空串
  final String merge;

  /// `before` / `impl` / `after`，按固定次序
  final List<String> phases;

  /// `sync` / `async`
  final String dispatch;

  /// 等实现的上限（毫秒；0 = 不设上限）
  final int timeoutMs;

  /// `none` / `byIdentity`
  final String cache;

  /// 命令面能调（声明里有 `cli` 块）
  final bool cli;

  /// 命令标题（`cli.title`；空串 = 用 [title]）
  final String commandTitle;

  /// 从链接触发时先问用户的原因（`cli.consent`；空串 = 不问）
  final String consent;

  /// 进 MCP 工具表（声明里有 `mcp` 块）
  final bool mcp;

  /// MCP 工具名（`mcp.name`；空串 = 按 id 生成）
  final String mcpName;

  /// `json` / `text`
  final String mcpFormat;

  /// 给模型看的标题（`mcp.title`；空串 = 用 [title]）
  final String mcpTitle;

  /// 给模型看的说明（`mcp.depict`；空串 = 用 [depict]）
  final String mcpDepict;

  /// 允许插件在 `musicxx.capability.call` 里调（声明里有 `plugin` 块）
  final bool plugin;

  /// 载荷与回答的说明
  final String doc;

  /// 实现是否开放（`phases` 含 `impl`）
  bool get openForImpl => phases.contains('impl');

  /// 阶段位掩码
  int get phaseMask => phases.fold<int>(
    0,
    (int mask, String phase) => mask | (1 << _phaseCodes[phase]!),
  );

  /// 外部通道位掩码
  int get channelMask {
    int mask = 0;
    if (cli) {
      mask |= _channelCodes['cli']!;
    }
    if (mcp) {
      mask |= _channelCodes['mcp']!;
    }
    if (plugin) {
      mask |= _channelCodes['plugin']!;
    }
    return mask;
  }

  /// 实际生效的工具名（`mcp.name` 没写时按 id 生成：`musicxx.player.toggle` → `PlayerToggle`）
  String get effectiveMcpName =>
      mcpName.isNotEmpty ? mcpName : defaultMcpNameOf(id);

  /// 实际生效的工具说明标题（没写用声明里的）
  String get effectiveMcpTitle => mcpTitle.isNotEmpty ? mcpTitle : title;

  /// 实际生效的工具说明（没写用声明里的）
  String get effectiveMcpDepict => mcpDepict.isNotEmpty ? mcpDepict : depict;

  /// 实际生效的命令标题（没写用声明里的）
  String get effectiveCommandTitle =>
      commandTitle.isNotEmpty ? commandTitle : title;
}

/// 一条广播声明
final class BroadcastDeclaration {
  BroadcastDeclaration({
    required this.id,
    required this.dart,
    required this.title,
    required this.depict,
    required this.cache,
    required this.publish,
    required this.pushOnSubscribe,
    required this.coalesce,
    required this.scope,
    required this.type,
    required this.options,
    required this.doc,
  });

  final String id;
  final String dart;
  final String title;
  final String depict;

  /// `none` / `latest`
  final String cache;

  /// `owner` / `anyone`：谁能往这个频道发布
  final String publish;
  final bool pushOnSubscribe;
  final bool coalesce;

  /// 值的类型提示（`bool` / `number` / `string` / `json`；空串 = 没写）
  final String type;

  /// 枚举取值域（稳定 id，不用本地化文案；只用于文档与自省）
  final List<String> options;

  /// `app` / `system`
  final String scope;

  final String doc;

  /// 有没有"当前值"可读
  bool get cached => cache == 'latest';
}

/// 定义文件的全部内容
final class CapabilityContract {
  CapabilityContract({required this.capabilities, required this.broadcasts});

  final List<CapabilityDeclaration> capabilities;
  final List<BroadcastDeclaration> broadcasts;
}

/* ==================== 读取与校验 ==================== */

String _requireString(Map<String, Object?> json, String key, String where) {
  final Object? raw = json[key];
  if (raw is! String || raw.isEmpty) {
    throw StateError('$where: 缺少字符串字段 `$key`');
  }
  return raw;
}

String _optionalString(Map<String, Object?> json, String key) {
  final Object? raw = json[key];
  return raw is String ? raw : '';
}

bool _optionalBool(Map<String, Object?> json, String key, String where) {
  final Object? raw = json[key];
  if (raw == null) {
    return false;
  }
  if (raw is! bool) {
    throw StateError('$where: `$key` 必须是布尔值');
  }
  return raw;
}

String _requireEnum(
  Map<String, Object?> json,
  String key,
  String where,
  Map<String, int> codes,
) {
  final String value = _requireString(json, key, where);
  if (!codes.containsKey(value)) {
    throw StateError(
      '$where: `$key` 取值非法 `$value`（可用 ${codes.keys.join(", ")}）',
    );
  }
  return value;
}

List<String> _stringList(
  Map<String, Object?> json,
  String key,
  String where, {
  required List<String> allowed,
  required List<String> order,
  bool allowEmpty = true,
}) {
  final Object? raw = json[key];
  if (raw == null) {
    return const <String>[];
  }
  if (raw is! List) {
    throw StateError('$where: `$key` 必须是字符串数组');
  }
  final List<String> declared = <String>[];
  for (final Object? item in raw) {
    final String value = "$item";
    if (!allowed.contains(value)) {
      throw StateError(
        '$where: `$key` 出现未知取值 `$value`（可用 ${allowed.join(", ")}）',
      );
    }
    if (declared.contains(value)) {
      throw StateError('$where: `$key` 里有重复取值 `$value`');
    }
    declared.add(value);
  }
  if (!allowEmpty && declared.isEmpty) {
    throw StateError('$where: `$key` 不能为空');
  }
  // 生成物统一成固定次序：定义文件里的书写顺序不影响生成结果
  return <String>[
    for (final String item in order)
      if (declared.contains(item)) item,
  ];
}

/// 原样读一个字符串数组（`options` 这类自由取值用；保持定义文件里的顺序）
List<String> _freeStringList(
  Map<String, Object?> json,
  String key,
  String where,
) {
  final Object? raw = json[key];
  if (raw == null) {
    return const <String>[];
  }
  if (raw is! List) {
    throw StateError('$where: `$key` 必须是字符串数组');
  }
  final List<String> values = <String>[];
  for (final Object? item in raw) {
    final String value = "$item";
    if (values.contains(value)) {
      throw StateError('$where: `$key` 里有重复取值 `$value`');
    }
    values.add(value);
  }
  return values;
}

/// 读一个可选的通道块（`cli` / `mcp` / `plugin`）
///
/// 没写 = 没有这个外部通道（返回 null）；写了必须是对象，字段只能是 [allowed] 里的。
Map<String, Object?>? _optionalBlock(
  Map<String, Object?> json,
  String key,
  String where, {
  required List<String> allowed,
}) {
  final Object? raw = json[key];
  if (raw == null) {
    return null;
  }
  if (raw is! Map) {
    throw StateError('$where: `$key` 必须是对象（没有字段就写 {}）');
  }
  final Map<String, Object?> block = raw.cast<String, Object?>();
  for (final String name in block.keys) {
    if (!allowed.contains(name)) {
      throw StateError(
        '$where: `$key` 里出现未知字段 `$name`（可用 ${allowed.join(", ")}）',
      );
    }
  }
  return block;
}

/// 读通道块里的字符串字段（没写返回空串）
String _blockString(Map<String, Object?>? block, String key, String where) {
  if (block == null || null == block[key]) {
    return '';
  }
  final Object? raw = block[key];
  if (raw is! String) {
    throw StateError('$where: `$key` 必须是字符串');
  }
  return raw;
}

/// 读通道块里的枚举字段（没写返回 [fallback]）
String _blockEnum(
  Map<String, Object?>? block,
  String key,
  String where,
  Map<String, int> codes,
  String fallback,
) {
  final String value = _blockString(block, key, where);
  if (value.isEmpty) {
    return fallback;
  }
  if (!codes.containsKey(value)) {
    throw StateError(
      '$where: `$key` 取值非法 `$value`（可用 ${codes.keys.join(", ")}）',
    );
  }
  return value;
}

/// 读取定义文件并做全部校验
///
/// 校验的是"设计语义"，不是格式：
/// - id 命名空间只有 `musicxx.*` 与 `plugin.*` 两个；
/// - `phases` 不含 `impl` 时只能是 `impls: one`（实现固定由应用提供，插件只能注册 before / after）；
/// - `merge` 只在 `impls: many` 且 `phases` 含 `impl` 时写，其余情况写了就是声明错了；
/// - 三个通道块 `cli` / `mcp` / `plugin` 的字段只能是用到的那几个；
/// - `mcp.name` 必须是 PascalCase 且全局不重复（协议不接受点号）；
/// - 广播的 `pushOnSubscribe` 只能在 `cache: latest` 时声明（没有当前值就没什么可推的）。
CapabilityContract loadContract(String path) {
  final File file = File(path);
  if (!file.existsSync()) {
    throw StateError('contract definition not found: $path');
  }
  final Map<String, Object?> def =
      jsonDecode(file.readAsStringSync()) as Map<String, Object?>;
  final Object? rawCaps = def['capabilities'];
  final Object? rawCasts = def['broadcasts'];
  if (rawCaps is! List) {
    throw StateError('$path: 缺少 capabilities 数组');
  }
  if (rawCasts is! List) {
    throw StateError('$path: 缺少 broadcasts 数组');
  }

  final List<CapabilityDeclaration> capabilities = <CapabilityDeclaration>[];
  final Set<String> capabilityIds = <String>{};
  final Set<String> capabilityDartNames = <String>{};
  final Map<String, String> mcpToolOwners = <String, String>{};
  for (final Object? item in rawCaps) {
    final Map<String, Object?> json = item! as Map<String, Object?>;
    final String id = _requireString(json, 'id', '能力');
    final String where = '能力 $id';
    if (!id.startsWith('musicxx.') && !id.startsWith('plugin.')) {
      throw StateError('$where: id 必须以 musicxx. 或 plugin. 开头');
    }
    if (!capabilityIds.add(id)) {
      throw StateError('能力 id 重复: $id');
    }
    final String dart = _requireString(json, 'dart', where);
    if (!RegExp(r'^[a-z][A-Za-z0-9]*$').hasMatch(dart)) {
      throw StateError('$where: dart 必须是合法的小驼峰标识符（当前 `$dart`）');
    }
    if (!capabilityDartNames.add(dart)) {
      throw StateError('能力 Dart 名重复: $dart');
    }
    final String impls = _requireEnum(json, 'impls', where, _implsCodes);
    final List<String> phases = _stringList(
      json,
      'phases',
      where,
      allowed: _phaseCodes.keys.toList(growable: false),
      order: _phaseOrder,
      allowEmpty: false,
    );
    final bool openForImpl = phases.contains('impl');
    if (!openForImpl && impls != 'one') {
      throw StateError('$where: phases 不含 impl 时 impls 只能是 one（当前 $impls）');
    }
    final String merge;
    if (impls == 'many' && openForImpl) {
      merge = _requireEnum(json, 'merge', where, _mergeCodes);
    } else {
      if (json.containsKey('merge')) {
        throw StateError('$where: merge 只在 impls=many 且 phases 含 impl 时写');
      }
      merge = '';
    }
    final String dispatch = _requireEnum(
      json,
      'dispatch',
      where,
      _dispatchCodes,
    );
    final Object? rawTimeout = json['timeoutMs'];
    int timeoutMs = 0;
    if (rawTimeout != null) {
      if (rawTimeout is! int || rawTimeout < 0) {
        throw StateError('$where: timeoutMs 必须是非负整数（0 = 不设上限）');
      }
      timeoutMs = rawTimeout;
    }
    final Map<String, Object?>? cliBlock = _optionalBlock(
      json,
      'cli',
      where,
      allowed: <String>['title', 'consent', 'params'],
    );
    final Map<String, Object?>? mcpBlock = _optionalBlock(
      json,
      'mcp',
      where,
      allowed: <String>['name', 'format', 'title', 'depict'],
    );
    final Map<String, Object?>? pluginBlock = _optionalBlock(
      json,
      'plugin',
      where,
      allowed: const <String>[],
    );
    if (null != cliBlock && null != cliBlock['params']) {
      final Object? rawParams = cliBlock['params'];
      if (rawParams is! List) {
        throw StateError('$where: `cli.params` 必须是数组');
      }
    }
    final String mcpName = _blockString(mcpBlock, 'name', '$where 的 mcp 块');
    if (mcpName.isNotEmpty && !_mcpNamePattern.hasMatch(mcpName)) {
      throw StateError('$where: `mcp.name` 必须是 PascalCase（当前 `$mcpName`）');
    }
    capabilities.add(
      CapabilityDeclaration(
        id: id,
        dart: dart,
        title: _optionalString(json, 'title'),
        depict: _optionalString(json, 'depict'),
        impls: impls,
        merge: merge,
        phases: phases,
        dispatch: dispatch,
        timeoutMs: timeoutMs,
        cache: _requireEnum(json, 'cache', where, _cacheCodes),
        cli: null != cliBlock,
        commandTitle: _blockString(cliBlock, 'title', '$where 的 cli 块'),
        consent: _blockString(cliBlock, 'consent', '$where 的 cli 块'),
        mcp: null != mcpBlock,
        mcpName: mcpName,
        mcpFormat: _blockEnum(
          mcpBlock,
          'format',
          '$where 的 mcp 块',
          _mcpFormatCodes,
          'json',
        ),
        mcpTitle: _blockString(mcpBlock, 'title', '$where 的 mcp 块'),
        mcpDepict: _blockString(mcpBlock, 'depict', '$where 的 mcp 块'),
        plugin: null != pluginBlock,
        doc: _optionalString(json, 'doc'),
      ),
    );
    final CapabilityDeclaration decl = capabilities.last;
    if (decl.mcp) {
      final String toolName = decl.effectiveMcpName;
      final String? owner = mcpToolOwners[toolName];
      if (null != owner) {
        throw StateError('MCP 工具名重复: `$toolName`（$owner 与 $id）');
      }
      mcpToolOwners[toolName] = id;
    }
  }

  final List<BroadcastDeclaration> broadcasts = <BroadcastDeclaration>[];
  final Set<String> broadcastIds = <String>{};
  final Set<String> broadcastDartNames = <String>{};
  for (final Object? item in rawCasts) {
    final Map<String, Object?> json = item! as Map<String, Object?>;
    final String id = _requireString(json, 'id', '广播');
    final String where = '广播 $id';
    if (!id.startsWith('musicxx.') && !id.startsWith('plugin.')) {
      throw StateError('$where: id 必须以 musicxx. 或 plugin. 开头');
    }
    if (!broadcastIds.add(id)) {
      throw StateError('广播 id 重复: $id');
    }
    final String dart = _requireString(json, 'dart', where);
    if (!RegExp(r'^[a-z][A-Za-z0-9]*$').hasMatch(dart)) {
      throw StateError('$where: dart 必须是合法的小驼峰标识符（当前 `$dart`）');
    }
    if (!broadcastDartNames.add(dart)) {
      throw StateError('广播 Dart 名重复: $dart');
    }
    final String cache = _requireEnum(
      json,
      'cache',
      where,
      _broadcastCacheCodes,
    );
    final bool pushOnSubscribe = _optionalBool(json, 'pushOnSubscribe', where);
    if (pushOnSubscribe && cache != 'latest') {
      throw StateError('$where: 只有 cache=latest 才能在订阅时立刻推当前值');
    }
    final String type = _optionalString(json, 'type');
    if (type.isNotEmpty && !_broadcastTypes.contains(type)) {
      throw StateError(
        '$where: `type` 取值非法 `$type`（可用 ${_broadcastTypes.join(", ")}）',
      );
    }
    final List<String> options = _freeStringList(json, 'options', where);
    if ((type.isNotEmpty || options.isNotEmpty) && cache != 'latest') {
      throw StateError('$where: `type` / `options` 只对 cache=latest 的频道有意义');
    }
    broadcasts.add(
      BroadcastDeclaration(
        id: id,
        dart: dart,
        title: _optionalString(json, 'title'),
        depict: _optionalString(json, 'depict'),
        cache: cache,
        publish: _requireEnum(json, 'publish', where, _publishCodes),
        pushOnSubscribe: pushOnSubscribe,
        coalesce: _optionalBool(json, 'coalesce', where),
        scope: _requireEnum(json, 'scope', where, _scopeCodes),
        type: type,
        options: options,
        doc: _optionalString(json, 'doc'),
      ),
    );
  }

  return CapabilityContract(capabilities: capabilities, broadcasts: broadcasts);
}

/* ==================== 小工具 ==================== */

/// Dart 单引号字符串字面量（转义反斜杠、单引号与 `$`）
String _dartLiteral(String value) {
  final String escaped = value
      .replaceAll(r'\', r'\\')
      .replaceAll("'", r"\'")
      .replaceAll(r'$', r'\$')
      .replaceAll('\n', r'\n');
  return "'$escaped'";
}

/// `///` 注释（多行压成一行：定义文件里的说明本来就是一段话）
String _docLine(String text, [String indent = '']) {
  final String flat = text.replaceAll(RegExp(r'\s+'), ' ').trim();
  if (flat.isEmpty) {
    return '';
  }
  return '$indent/// $flat\n';
}

/// C++ 常量名：`musicxx.player.beforePlaySong` → `<prefix>PLAYER_BEFORE_PLAY_SONG`
String _cppConstant(String prefix, String id) {
  final String body = id.startsWith('musicxx.')
      ? id.substring('musicxx.'.length)
      : id;
  final StringBuffer sb = StringBuffer(prefix);
  for (int i = 0; i < body.length; i++) {
    final String ch = body[i];
    final bool upper = ch.toUpperCase() == ch && ch.toLowerCase() != ch;
    if (ch == '.') {
      sb.write('_');
    } else if (upper && i > 0) {
      sb.write('_${ch.toUpperCase()}');
    } else {
      sb.write(ch.toUpperCase());
    }
  }
  return sb.toString();
}

/// Dart 侧列表字面量：`<String>['a', 'b']`
String _dartStringList(List<String> values) =>
    '<String>[${values.map(_dartLiteral).join(', ')}]';

/* ==================== Dart 生成物 ==================== */

String generateDartIds(CapabilityContract contract) {
  final StringBuffer sb = StringBuffer()
    ..writeln(
      '// 自动生成（tools/gen_contract.dart ← tools/capabilities.def.json）—— 请勿手改。',
    )
    ..writeln('//')
    ..writeln('// 能力 = 调用方发一次请求、等一个结果；广播 = 属主改了东西、通知关心的人。')
    ..writeln('// id 是跨边界稳定的约定：字符串值一旦发布不得修改，只能新增或标记废弃。')
    ..writeln('// ignore_for_file: type=lint, constant_identifier_names')
    ..writeln()
    ..writeln('/// 能力阶段（参与方式）：同一条能力上的三段')
    ..writeln('///')
    ..writeln('/// 能力 id 表示**一次调用**；参与方式由注册时的阶段表达：')
    ..writeln('/// - [before]：进入调用之前，串行、正序，只能改输入或 `stop`；')
    ..writeln('/// - [impl]：实现链，给出结果（多数能力只有这一段）；')
    ..writeln('/// - [after]：拿到结果之后，串行、逆序，只能改输出或 `drop`。')
    ..writeln('enum MusicxxCapabilityPhase {')
    ..writeln('  impl(0),')
    ..writeln()
    ..writeln('  before(1),')
    ..writeln()
    ..writeln('  after(2);')
    ..writeln()
    ..writeln('  const MusicxxCapabilityPhase(this.code);')
    ..writeln()
    ..writeln('  /// 阶段编号（与 C 侧 `MUSICXX_CAPABILITY_PHASE_*` 一致）')
    ..writeln('  final int code;')
    ..writeln()
    ..writeln(
      '  /// 位掩码（`1 << code`，与 C 侧 `MUSICXX_CAPABILITY_PHASE_MASK_*` 一致）',
    )
    ..writeln('  int get mask => 1 << code;')
    ..writeln()
    ..writeln('  /// 对外字符串（事件载荷与自省用它）')
    ..writeln('  String get wireName {')
    ..writeln('    switch (this) {')
    ..writeln('      case MusicxxCapabilityPhase.before:')
    ..writeln('        return "before";')
    ..writeln('      case MusicxxCapabilityPhase.impl:')
    ..writeln('        return "impl";')
    ..writeln('      case MusicxxCapabilityPhase.after:')
    ..writeln('        return "after";')
    ..writeln('    }')
    ..writeln('  }')
    ..writeln()
    ..writeln('  /// 按对外字符串反查（未知名字返回 null）')
    ..writeln('  static MusicxxCapabilityPhase? tryFromName(String name) {')
    ..writeln('    switch (name) {')
    ..writeln('      case "impl":')
    ..writeln('        return MusicxxCapabilityPhase.impl;')
    ..writeln('      case "before":')
    ..writeln('        return MusicxxCapabilityPhase.before;')
    ..writeln('      case "after":')
    ..writeln('        return MusicxxCapabilityPhase.after;')
    ..writeln('    }')
    ..writeln('    return null;')
    ..writeln('  }')
    ..writeln('}')
    ..writeln()
    ..writeln('/// 合并策略：多个实现都给出结果时怎么合')
    ..writeln('enum MusicxxCapabilityMerge {')
    ..writeln('  /// 第一个合法结果生效，后面的实现不再问')
    ..writeln("  first('first'),")
    ..writeln()
    ..writeln('  /// 任一实现能否决（最保守的裁决不被后面的实现覆盖）')
    ..writeln("  veto('veto');")
    ..writeln()
    ..writeln('  const MusicxxCapabilityMerge(this.wireName);')
    ..writeln()
    ..writeln('  /// 声明里的写法（`first` / `veto`）')
    ..writeln('  final String wireName;')
    ..writeln()
    ..writeln('  /// 按声明里的写法反查（空串与未知值都返回 null：包装点没有实现链，不需要策略）')
    ..writeln('  static MusicxxCapabilityMerge? tryFromName(String name) {')
    ..writeln('    switch (name) {')
    ..writeln('      case "first":')
    ..writeln('        return MusicxxCapabilityMerge.first;')
    ..writeln('      case "veto":')
    ..writeln('        return MusicxxCapabilityMerge.veto;')
    ..writeln('    }')
    ..writeln('    return null;')
    ..writeln('  }')
    ..writeln('}')
    ..writeln()
    ..writeln('/// 能力 id（调用方发一次请求、等一个结果）')
    ..writeln('abstract final class MusicxxCapabilityId {');

  for (final CapabilityDeclaration decl in contract.capabilities) {
    sb.write(_docLine(decl.title.isEmpty ? decl.id : decl.title, '  '));
    sb.writeln(
      '  static const String ${decl.dart} = ${_dartLiteral(decl.id)};',
    );
  }
  sb
    ..writeln()
    ..writeln('  /// 全部能力 id（按声明顺序）')
    ..writeln('  static const List<String> all = <String>[');
  for (final CapabilityDeclaration decl in contract.capabilities) {
    sb.writeln('    ${decl.dart},');
  }
  sb
    ..writeln('  ];')
    ..writeln('}')
    ..writeln()
    ..writeln('/// 广播 id（属主改了东西、通知关心的人）')
    ..writeln('abstract final class MusicxxBroadcastId {');

  for (final BroadcastDeclaration decl in contract.broadcasts) {
    sb.write(_docLine(decl.title.isEmpty ? decl.id : decl.title, '  '));
    sb.writeln(
      '  static const String ${decl.dart} = ${_dartLiteral(decl.id)};',
    );
  }
  sb
    ..writeln()
    ..writeln('  /// 全部广播 id（按声明顺序）')
    ..writeln('  static const List<String> all = <String>[');
  for (final BroadcastDeclaration decl in contract.broadcasts) {
    sb.writeln('    ${decl.dart},');
  }
  sb
    ..writeln('  ];')
    ..writeln('}')
    ..writeln()
    ..writeln('/// 一条能力的声明')
    ..writeln('///')
    ..writeln(
      '/// 字段含义见 `tools/capabilities.def.json` 与 `docs/plugin-capabilities.md`。',
    )
    ..writeln('///')
    ..writeln(
      '/// 按 id 生成默认的 MCP 工具名（`musicxx.player.toggle` → `PlayerToggle`）：',
    )
    ..writeln('/// 工具名不能带点（MCP 协议不接受），段分隔符各算一个词边界。')
    ..writeln('String musicxxDefaultMcpNameOf(String id) {')
    ..writeln("  const String prefix = 'musicxx.';")
    ..writeln(
      '  final String body = id.startsWith(prefix) ? id.substring(prefix.length) : id;',
    )
    ..writeln('  final StringBuffer sb = StringBuffer();')
    ..writeln("  for (final String part in body.split(RegExp(r'[._]'))) {")
    ..writeln('    if (part.isEmpty) {')
    ..writeln('      continue;')
    ..writeln('    }')
    ..writeln('    sb.write(part[0].toUpperCase());')
    ..writeln('    sb.write(part.substring(1));')
    ..writeln('  }')
    ..writeln('  return sb.toString();')
    ..writeln('}')
    ..writeln()
    ..writeln('final class MusicxxCapabilityDecl {')
    ..writeln('  const MusicxxCapabilityDecl({')
    ..writeln('    required this.id,')
    ..writeln('    required this.impls,')
    ..writeln('    required this.merge,')
    ..writeln('    required this.phases,')
    ..writeln('    required this.dispatch,')
    ..writeln('    required this.timeoutMs,')
    ..writeln('    required this.cache,')
    ..writeln('    required this.cli,')
    ..writeln('    required this.commandTitle,')
    ..writeln('    required this.consent,')
    ..writeln('    required this.mcp,')
    ..writeln('    required this.mcpName,')
    ..writeln('    required this.mcpFormat,')
    ..writeln('    required this.mcpTitle,')
    ..writeln('    required this.mcpDepict,')
    ..writeln('    required this.plugin,')
    ..writeln('    required this.title,')
    ..writeln('    required this.depict,')
    ..writeln('  });')
    ..writeln()
    ..writeln('  final String id;')
    ..writeln()
    ..writeln('  /// `one`（后登记覆盖先登记）/ `many`（多实现共存）')
    ..writeln('  final String impls;')
    ..writeln()
    ..writeln('  /// `first` / `veto`；不适用时为空串')
    ..writeln('  final String merge;')
    ..writeln()
    ..writeln('  /// `before` / `impl` / `after` 的子集，按固定次序')
    ..writeln('  final List<String> phases;')
    ..writeln()
    ..writeln('  /// `sync` / `async`')
    ..writeln('  final String dispatch;')
    ..writeln()
    ..writeln('  /// 等实现的上限（毫秒；0 = 不设上限）')
    ..writeln('  final int timeoutMs;')
    ..writeln()
    ..writeln('  /// `none` / `byIdentity`')
    ..writeln('  final String cache;')
    ..writeln()
    ..writeln('  /// `cli` / `mcp` / `plugin` 三个通道块：有哪一块，谁就能调它')
    ..writeln(
      '  /// - `cli`：命令面（命令行 / /cli / 链接）；`commandTitle` / `consent` 是它的可选字段',
    )
    ..writeln('  /// - `mcp`：进 MCP 工具表，工具名 `mcpName`（不写时按 id 生成）')
    ..writeln('  /// - `plugin`：插件可以在 `musicxx.capability.call` 里调')
    ..writeln('  final bool cli;')
    ..writeln()
    ..writeln('  /// 命令标题（`cli.title`；空串 = 用 [title]）')
    ..writeln('  final String commandTitle;')
    ..writeln()
    ..writeln('  /// 从链接触发时先问用户的原因（`cli.consent`；空串 = 不问）')
    ..writeln('  final String consent;')
    ..writeln()
    ..writeln('  /// 进 MCP 工具表（声明里有 `mcp` 块）')
    ..writeln('  final bool mcp;')
    ..writeln()
    ..writeln('  /// MCP 工具名（`mcp.name`；空串 = 按 id 生成）')
    ..writeln('  final String mcpName;')
    ..writeln()
    ..writeln('  /// `json`（结构化，缺省）/ `text`（排好版的文本）')
    ..writeln('  final String mcpFormat;')
    ..writeln()
    ..writeln('  /// 给模型看的标题（`mcp.title`；空串 = 用 [title]）')
    ..writeln('  final String mcpTitle;')
    ..writeln()
    ..writeln('  /// 给模型看的说明（`mcp.depict`；空串 = 用 [depict]）')
    ..writeln('  final String mcpDepict;')
    ..writeln()
    ..writeln('  /// 允许插件在 `musicxx.capability.call` 里调（声明里有 `plugin` 块）')
    ..writeln('  final bool plugin;')
    ..writeln()
    ..writeln('  /// 实际生效的工具名（`mcp.name` 没写时按 id 生成）')
    ..writeln(
      '  String get effectiveMcpName => mcpName.isNotEmpty ? mcpName : musicxxDefaultMcpNameOf(id);',
    )
    ..writeln()
    ..writeln('  /// 实际生效的工具标题（没写用 [title]）')
    ..writeln(
      '  String get effectiveMcpTitle => mcpTitle.isNotEmpty ? mcpTitle : title;',
    )
    ..writeln()
    ..writeln('  /// 实际生效的工具说明（没写用 [depict]）')
    ..writeln(
      '  String get effectiveMcpDepict => mcpDepict.isNotEmpty ? mcpDepict : depict;',
    )
    ..writeln()
    ..writeln('  /// 实际生效的命令标题（没写用 [title]）')
    ..writeln(
      '  String get effectiveCommandTitle => commandTitle.isNotEmpty ? commandTitle : title;',
    )
    ..writeln()
    ..writeln('  /// 一句话标题（自省与文档用）')
    ..writeln('  final String title;')
    ..writeln()
    ..writeln('  /// 一句话说明（自省与文档用）')
    ..writeln('  final String depict;')
    ..writeln()
    ..writeln(
      '  /// 实现是否开放（`phases` 含 `impl`）；false 时插件只能注册 `before` / `after`',
    )
    ..writeln("  bool get openForImpl => phases.contains('impl');")
    ..writeln()
    ..writeln('  /// 多个实现共存')
    ..writeln("  bool get many => impls == 'many';")
    ..writeln()
    ..writeln('  /// 只给模型用（不进命令表，也不对插件开放）')
    ..writeln('  bool get mcpOnly => mcp && !cli && !plugin;')
    ..writeln()
    ..writeln('  /// 只能应用内部调（三个通道块都没有）')
    ..writeln('  bool get internalOnly => !cli && !mcp && !plugin;')
    ..writeln()
    ..writeln('  /// 按请求身份缓存结果')
    ..writeln("  bool get cachedByIdentity => cache == 'byIdentity';")
    ..writeln()
    ..writeln('  /// 合并策略对象（`merge` 为空 = 不适用，例如包装点没有实现链）')
    ..writeln('  MusicxxCapabilityMerge? get mergeKind =>')
    ..writeln('      MusicxxCapabilityMerge.tryFromName(merge);')
    ..writeln()
    ..writeln('  /// 这条 id 是不是广播频道（通知型：只通知、不回答）')
    ..writeln(
      '  bool get isNotification => null != MusicxxCapabilityCatalogue.broadcastOf(id);',
    )
    ..writeln('}')
    ..writeln()
    ..writeln('/// 一条广播的声明')
    ..writeln('final class MusicxxBroadcastDecl {')
    ..writeln('  const MusicxxBroadcastDecl({')
    ..writeln('    required this.id,')
    ..writeln('    required this.cache,')
    ..writeln('    required this.publish,')
    ..writeln('    required this.pushOnSubscribe,')
    ..writeln('    required this.coalesce,')
    ..writeln('    required this.scope,')
    ..writeln('    required this.type,')
    ..writeln('    required this.options,')
    ..writeln('    required this.title,')
    ..writeln('    required this.depict,')
    ..writeln('  });')
    ..writeln()
    ..writeln('  final String id;')
    ..writeln()
    ..writeln('  /// `none`（没有当前值）/ `latest`（有当前值，订阅即拿到）')
    ..writeln('  final String cache;')
    ..writeln()
    ..writeln('  /// `owner` / `anyone`：谁能往这个频道发布')
    ..writeln('  final String publish;')
    ..writeln()
    ..writeln('  /// 订阅时是否立刻收到当前缓存值（只有 `cache=latest` 能是 true）')
    ..writeln('  final bool pushOnSubscribe;')
    ..writeln()
    ..writeln('  /// 允许合并（允许丢中间值）')
    ..writeln('  final bool coalesce;')
    ..writeln()
    ..writeln('  /// `app` / `system`')
    ..writeln('  final String scope;')
    ..writeln()
    ..writeln('  /// 值的类型提示（`bool` / `number` / `string` / `json`；空串 = 没写）')
    ..writeln('  final String type;')
    ..writeln()
    ..writeln('  /// 枚举取值域（稳定 id；只用于文档与自省）')
    ..writeln('  final List<String> options;')
    ..writeln()
    ..writeln('  final String title;')
    ..writeln()
    ..writeln('  final String depict;')
    ..writeln()
    ..writeln('  /// 能同步读当前值')
    ..writeln("  bool get cached => cache == 'latest';")
    ..writeln()
    ..writeln('  /// 任何人都能往它发布')
    ..writeln("  bool get publishByAnyone => publish == 'anyone';")
    ..writeln('}')
    ..writeln()
    ..writeln('/// 声明清单（与 `tools/capabilities.def.json` 一一对应）')
    ..writeln('abstract final class MusicxxCapabilityCatalogue {')
    ..writeln('  /// 全部能力声明（按声明顺序）')
    ..writeln(
      '  static const List<MusicxxCapabilityDecl> capabilities = '
      '<MusicxxCapabilityDecl>[',
    );

  for (final CapabilityDeclaration decl in contract.capabilities) {
    sb
      ..writeln('    MusicxxCapabilityDecl(')
      ..writeln('      id: ${_dartLiteral(decl.id)},')
      ..writeln('      impls: ${_dartLiteral(decl.impls)},')
      ..writeln('      merge: ${_dartLiteral(decl.merge)},')
      ..writeln('      phases: ${_dartStringList(decl.phases)},')
      ..writeln('      dispatch: ${_dartLiteral(decl.dispatch)},')
      ..writeln('      timeoutMs: ${decl.timeoutMs},')
      ..writeln('      cache: ${_dartLiteral(decl.cache)},')
      ..writeln('      cli: ${decl.cli},')
      ..writeln('      commandTitle: ${_dartLiteral(decl.commandTitle)},')
      ..writeln('      consent: ${_dartLiteral(decl.consent)},')
      ..writeln('      mcp: ${decl.mcp},')
      ..writeln('      mcpName: ${_dartLiteral(decl.mcpName)},')
      ..writeln('      mcpFormat: ${_dartLiteral(decl.mcpFormat)},')
      ..writeln('      mcpTitle: ${_dartLiteral(decl.mcpTitle)},')
      ..writeln('      mcpDepict: ${_dartLiteral(decl.mcpDepict)},')
      ..writeln('      plugin: ${decl.plugin},')
      ..writeln('      title: ${_dartLiteral(decl.title)},')
      ..writeln('      depict: ${_dartLiteral(decl.depict)},')
      ..writeln('    ),');
  }

  sb
    ..writeln('  ];')
    ..writeln()
    ..writeln('  /// 全部广播声明（按声明顺序）')
    ..writeln(
      '  static const List<MusicxxBroadcastDecl> broadcasts = '
      '<MusicxxBroadcastDecl>[',
    );

  for (final BroadcastDeclaration decl in contract.broadcasts) {
    sb
      ..writeln('    MusicxxBroadcastDecl(')
      ..writeln('      id: ${_dartLiteral(decl.id)},')
      ..writeln('      cache: ${_dartLiteral(decl.cache)},')
      ..writeln('      publish: ${_dartLiteral(decl.publish)},')
      ..writeln('      pushOnSubscribe: ${decl.pushOnSubscribe},')
      ..writeln('      coalesce: ${decl.coalesce},')
      ..writeln('      scope: ${_dartLiteral(decl.scope)},')
      ..writeln('      type: ${_dartLiteral(decl.type)},')
      ..writeln('      options: ${_dartStringList(decl.options)},')
      ..writeln('      title: ${_dartLiteral(decl.title)},')
      ..writeln('      depict: ${_dartLiteral(decl.depict)},')
      ..writeln('    ),');
  }

  sb
    ..writeln('  ];')
    ..writeln()
    ..writeln(
      '  static final Map<String, MusicxxCapabilityDecl> _capabilityIndex =',
    )
    ..writeln('      <String, MusicxxCapabilityDecl>{')
    ..writeln(
      '    for (final MusicxxCapabilityDecl decl in capabilities) decl.id: decl,',
    )
    ..writeln('  };')
    ..writeln()
    ..writeln(
      '  static final Map<String, MusicxxBroadcastDecl> _broadcastIndex =',
    )
    ..writeln('      <String, MusicxxBroadcastDecl>{')
    ..writeln(
      '    for (final MusicxxBroadcastDecl decl in broadcasts) decl.id: decl,',
    )
    ..writeln('  };')
    ..writeln()
    ..writeln('  /// 按 id 找能力声明（没有时返回 null）')
    ..writeln('  static MusicxxCapabilityDecl? capabilityOf(String id) =>')
    ..writeln('      _capabilityIndex[id];')
    ..writeln()
    ..writeln('  /// 按 id 找广播声明（没有时返回 null）')
    ..writeln('  static MusicxxBroadcastDecl? broadcastOf(String id) =>')
    ..writeln('      _broadcastIndex[id];')
    ..writeln()
    ..writeln('  /// 这个 id 是不是声明过的能力')
    ..writeln('  static bool declaresCapability(String id) =>')
    ..writeln('      _capabilityIndex.containsKey(id);')
    ..writeln()
    ..writeln('  /// 这个 id 是不是声明过的广播')
    ..writeln('  static bool declaresBroadcast(String id) =>')
    ..writeln('      _broadcastIndex.containsKey(id);')
    ..writeln('}');
  return sb.toString();
}

/* ==================== C++ 生成物 ==================== */

String generateCppIds(CapabilityContract contract) {
  final StringBuffer sb = StringBuffer()
    ..writeln(
      '/// 自动生成（tools/gen_contract.dart ← tools/capabilities.def.json）—— 请勿手改。',
    )
    ..writeln('///')
    ..writeln('/// C++ 侧能力与广播约定：id 常量 + 预设声明表（表里的 id 带实现槽位 /')
    ..writeln('/// 合并策略 / 阶段 / 超时；表外的 id 一律按默认属性处理）。')
    ..writeln('#ifndef MUSICXX_CAPABILITY_IDS_G_H')
    ..writeln('#define MUSICXX_CAPABILITY_IDS_G_H')
    ..writeln()
    ..writeln('#include <stddef.h>')
    ..writeln('#include <stdint.h>')
    ..writeln('#include <string.h>')
    ..writeln()
    ..writeln('/* ==================== 实现槽位 ==================== */')
    ..writeln()
    ..writeln('/// 一个槽位：后登记的覆盖先登记的')
    ..writeln('#define MUSICXX_CAPABILITY_IMPLS_ONE  0')
    ..writeln('/// 多个槽位：多实现按 (层, priority, 登记顺序) 共存')
    ..writeln('#define MUSICXX_CAPABILITY_IMPLS_MANY 1')
    ..writeln()
    ..writeln('/* ==================== 合并策略 ==================== */')
    ..writeln()
    ..writeln('/// 第一个给出合法结果的实现生效')
    ..writeln('#define MUSICXX_CAPABILITY_MERGE_FIRST 1')
    ..writeln('/// 任一实现能否决（最保守），patch 由后链改')
    ..writeln('#define MUSICXX_CAPABILITY_MERGE_VETO  2')
    ..writeln()
    ..writeln('/* ==================== 阶段位掩码 ==================== */')
    ..writeln()
    ..writeln('/* 阶段编号：注册时用它表达参与方式 */')
    ..writeln('#define MUSICXX_CAPABILITY_PHASE_IMPL   0')
    ..writeln('#define MUSICXX_CAPABILITY_PHASE_BEFORE 1')
    ..writeln('#define MUSICXX_CAPABILITY_PHASE_AFTER  2')
    ..writeln()
    ..writeln('/* 阶段位掩码：声明里的 phase_mask 字段用它 (1 << 阶段编号) */')
    ..writeln('#define MUSICXX_CAPABILITY_PHASE_MASK_IMPL   (1u << 0)')
    ..writeln('#define MUSICXX_CAPABILITY_PHASE_MASK_BEFORE (1u << 1)')
    ..writeln('#define MUSICXX_CAPABILITY_PHASE_MASK_AFTER  (1u << 2)')
    ..writeln()
    ..writeln('/* ==================== 派发方式 ==================== */')
    ..writeln()
    ..writeln('/// 调用方等结果')
    ..writeln('#define MUSICXX_CAPABILITY_DISPATCH_SYNC  0')
    ..writeln('/// 不占用调用线程')
    ..writeln('#define MUSICXX_CAPABILITY_DISPATCH_ASYNC 1')
    ..writeln()
    ..writeln('/* ==================== 值缓存 ==================== */')
    ..writeln()
    ..writeln('#define MUSICXX_CAPABILITY_CACHE_NONE        0')
    ..writeln('#define MUSICXX_CAPABILITY_CACHE_BY_IDENTITY 1')
    ..writeln()
    ..writeln('/* ==================== 外部通道位掩码 ==================== */')
    ..writeln()
    ..writeln('/// 命令面能调（命令行 / /cli / 链接）')
    ..writeln('#define MUSICXX_CAPABILITY_CHANNEL_CLI    0x01u')
    ..writeln('/// 进 MCP 工具表')
    ..writeln('#define MUSICXX_CAPABILITY_CHANNEL_MCP    0x02u')
    ..writeln('/// 插件可以在 musicxx.capability.call 里调')
    ..writeln('#define MUSICXX_CAPABILITY_CHANNEL_PLUGIN 0x04u')
    ..writeln()
    ..writeln('/* ==================== 广播的缓存与发布权限 ==================== */')
    ..writeln()
    ..writeln('/// 没有当前值：每次发布都通知')
    ..writeln('#define MUSICXX_BROADCAST_CACHE_NONE   0')
    ..writeln('/// 有当前值：订阅即拿到，也能同步读')
    ..writeln('#define MUSICXX_BROADCAST_CACHE_LATEST 1')
    ..writeln()
    ..writeln('/// 只有属主能发布')
    ..writeln('#define MUSICXX_BROADCAST_PUBLISH_OWNER  0')
    ..writeln('/// 任何人都能发布')
    ..writeln('#define MUSICXX_BROADCAST_PUBLISH_ANYONE 1')
    ..writeln()
    ..writeln('/// 业务频道')
    ..writeln('#define MUSICXX_BROADCAST_SCOPE_APP    0')
    ..writeln('/// 调试与自省频道（插件不能写）')
    ..writeln('#define MUSICXX_BROADCAST_SCOPE_SYSTEM 1')
    ..writeln()
    ..writeln('/* ==================== 能力 / 广播 id 常量 ==================== */')
    ..writeln();

  for (final CapabilityDeclaration decl in contract.capabilities) {
    sb.writeln(
      '#define ${_cppConstant("MUSICXX_CAPABILITY_", decl.id)} "${decl.id}"',
    );
  }
  sb.writeln();
  for (final BroadcastDeclaration decl in contract.broadcasts) {
    sb.writeln(
      '#define ${_cppConstant("MUSICXX_BROADCAST_", decl.id)} "${decl.id}"',
    );
  }
  sb
    ..writeln()
    ..writeln('/* ==================== 已知声明表 ==================== */')
    ..writeln()
    ..writeln('/// 一条能力的声明（字段编码见上面的宏）')
    ..writeln('typedef struct MusicxxCapabilityDecl {')
    ..writeln('    const char* id;')
    ..writeln('    int32_t     impls;       ///< MUSICXX_CAPABILITY_IMPLS_*')
    ..writeln(
      '    int32_t     merge;       ///< MUSICXX_CAPABILITY_MERGE_*（0 = 不适用）',
    )
    ..writeln(
      '    uint32_t    phase_mask;  ///< MUSICXX_CAPABILITY_PHASE_MASK_*',
    )
    ..writeln('    int32_t     dispatch;    ///< MUSICXX_CAPABILITY_DISPATCH_*')
    ..writeln('    int32_t     timeout_ms;  ///< 等实现的上限（0 = 不设上限）')
    ..writeln('    int32_t     cache;       ///< MUSICXX_CAPABILITY_CACHE_*')
    ..writeln('    uint32_t    channels;    ///< MUSICXX_CAPABILITY_CHANNEL_*')
    ..writeln('} MusicxxCapabilityDecl;')
    ..writeln()
    ..writeln('/// 一条广播的声明')
    ..writeln('typedef struct MusicxxBroadcastDecl {')
    ..writeln('    const char* id;')
    ..writeln(
      '    int32_t     cache;             ///< MUSICXX_BROADCAST_CACHE_*',
    )
    ..writeln(
      '    int32_t     publish;           ///< MUSICXX_BROADCAST_PUBLISH_*',
    )
    ..writeln('    int32_t     push_on_subscribe; ///< 订阅时立刻推当前值')
    ..writeln('    int32_t     coalesce;          ///< 允许合并（允许丢中间值）')
    ..writeln(
      '    int32_t     scope;             ///< MUSICXX_BROADCAST_SCOPE_*',
    )
    ..writeln('} MusicxxBroadcastDecl;')
    ..writeln()
    ..writeln(
      'static const MusicxxCapabilityDecl musicxxCapabilityDecls[] = {',
    );

  for (final CapabilityDeclaration decl in contract.capabilities) {
    final int impls = _implsCodes[decl.impls]!;
    final int merge = decl.merge.isEmpty ? 0 : _mergeCodes[decl.merge]!;
    sb.writeln(
      '    {"${decl.id}", $impls, $merge, ${decl.phaseMask}u, '
      '${_dispatchCodes[decl.dispatch]!}, ${decl.timeoutMs}, '
      '${_cacheCodes[decl.cache]!}, ${decl.channelMask}u},',
    );
  }

  sb
    ..writeln('};')
    ..writeln()
    ..writeln('/// 已知能力条目数')
    ..writeln(
      'static const size_t musicxxCapabilityDeclCount = '
      'sizeof(musicxxCapabilityDecls) / sizeof(musicxxCapabilityDecls[0]);',
    )
    ..writeln()
    ..writeln('static const MusicxxBroadcastDecl musicxxBroadcastDecls[] = {');

  for (final BroadcastDeclaration decl in contract.broadcasts) {
    sb.writeln(
      '    {"${decl.id}", ${_broadcastCacheCodes[decl.cache]!}, '
      '${_publishCodes[decl.publish]!}, ${decl.pushOnSubscribe ? 1 : 0}, '
      '${decl.coalesce ? 1 : 0}, ${_scopeCodes[decl.scope]!}},',
    );
  }

  sb
    ..writeln('};')
    ..writeln()
    ..writeln('/// 已知广播条目数')
    ..writeln(
      'static const size_t musicxxBroadcastDeclCount = '
      'sizeof(musicxxBroadcastDecls) / sizeof(musicxxBroadcastDecls[0]);',
    )
    ..writeln()
    ..writeln('/// 按 id 查能力声明（没有返回 NULL）')
    ..writeln(
      'static inline const MusicxxCapabilityDecl* '
      'musicxxCapabilityDeclOf(const char* id) {',
    )
    ..writeln('    if (!id) {')
    ..writeln('        return NULL;')
    ..writeln('    }')
    ..writeln('    for (size_t i = 0; i < musicxxCapabilityDeclCount; ++i) {')
    ..writeln('        if (strcmp(musicxxCapabilityDecls[i].id, id) == 0) {')
    ..writeln('            return &musicxxCapabilityDecls[i];')
    ..writeln('        }')
    ..writeln('    }')
    ..writeln('    return NULL;')
    ..writeln('}')
    ..writeln()
    ..writeln('/// 按 id 查广播声明（没有返回 NULL）')
    ..writeln(
      'static inline const MusicxxBroadcastDecl* '
      'musicxxBroadcastDeclOf(const char* id) {',
    )
    ..writeln('    if (!id) {')
    ..writeln('        return NULL;')
    ..writeln('    }')
    ..writeln('    for (size_t i = 0; i < musicxxBroadcastDeclCount; ++i) {')
    ..writeln('        if (strcmp(musicxxBroadcastDecls[i].id, id) == 0) {')
    ..writeln('            return &musicxxBroadcastDecls[i];')
    ..writeln('        }')
    ..writeln('    }')
    ..writeln('    return NULL;')
    ..writeln('}')
    ..writeln()
    ..writeln('#endif /* MUSICXX_CAPABILITY_IDS_G_H */');
  return sb.toString();
}
