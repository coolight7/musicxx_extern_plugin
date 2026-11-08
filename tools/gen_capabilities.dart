/// 能力与广播清单生成器（由 `tools/gen_contract.dart` 调用）
///
/// 定义源：`tools/capabilities.def.json`
///
/// 产物：
/// | 产物 | 用途 |
/// |---|---|
/// | `lib/src/capability_ids.g.dart` | Dart 侧 id 常量 + 声明（impls / merge / phases / dispatch / timeout / cache / expose / usage / implemented） |
/// | `src/sdk/include/musicxx/plugin/api/capability_ids.g.h` | C++ 侧 id 常量 + 已知声明表（插件框架据此拒绝未知 id、判阶段与合并策略） |
/// | `docs/plugin-capabilities.md` | 能力（请求 → 结果）作者文档 |
/// | `docs/plugin-broadcasts.md` | 广播（通知 / 快照）作者文档 |
///
/// 为什么只有一个定义源：同一个 id 现在同时出现在"派发、命令面、MCP、链接入口、自省、
/// 作者文档"几处，靠手写常量加断言对齐迟早会漂；生成物把 id 与声明字段一起带出去。
///
/// - 校验规则见 [loadCapabilityContract]：id 命名空间、`impls` 与 `phases` 的组合、
///   `merge` 的适用范围、`usage` 非空、广播"订阅即推值"只能在 `cache=latest` 时声明。
/// - 生成物是提交物：改完定义文件必须跑 `dart run tools/gen_contract.dart` 并提交。
library;

import 'dart:convert';
import 'dart:io';

/// 定义文件（相对包根目录）
const String capabilitiesDefPath = 'tools/capabilities.def.json';

/// Dart 生成物
const String capabilityDartOut = 'lib/src/capability_ids.g.dart';

/// C++ 生成物
const String capabilityCppOut =
    'src/sdk/include/musicxx/plugin/api/capability_ids.g.h';

/// 能力作者文档
const String capabilityDocOut = 'docs/plugin-capabilities.md';

/// 广播作者文档
const String broadcastDocOut = 'docs/plugin-broadcasts.md';

/* ==================== 编码（Dart 与 C++ 两侧共用） ==================== */

/// 实现槽位：`one`（后登记覆盖先登记）/ `many`（多实现共存）
const Map<String, int> _implsCodes = <String, int>{'one': 0, 'many': 1};

/// 合并策略：`first`（第一个合法结果生效）/ `veto`（任一实现能否）
const Map<String, int> _mergeCodes = <String, int>{'first': 1, 'veto': 2};

/// 阶段编号（与 `MUSICXX_PLUGIN_HOOK_PHASE_*` 一致：impl 0 / before 1 / after 2）
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

/// 广播的缓存：`none`（事件）/ `latest`（快照，订阅即拿到当前值）
const Map<String, int> _broadcastCacheCodes = <String, int>{
  'none': 0,
  'latest': 1,
};

/// 广播的写权限：`owner`（只有属主能写）/ `anyone`（变量）
const Map<String, int> _writableCodes = <String, int>{'owner': 0, 'anyone': 1};

/// 广播的作用域：`app`（业务频道）/ `system`（调试与自省频道，插件不能写）
const Map<String, int> _scopeCodes = <String, int>{'app': 0, 'system': 1};

/// 链接触发时是否先问用户
const Map<String, int> _consentCodes = <String, int>{'none': 0, 'ask': 1};

/// 用途位掩码：功能埋点 / 命令 / 插件服务
const Map<String, int> _usageCodes = <String, int>{
  'point': 1,
  'command': 2,
  'service': 4,
};

/// 用途的固定次序（生成的数组与文档列都按它写）
const List<String> _usageOrder = <String>['point', 'command', 'service'];

/// 外部入口位掩码
const Map<String, int> _exposeCodes = <String, int>{
  'cli': 1,
  'mcp': 2,
  'link': 4,
  'plugin': 8,
  'ui': 16,
  'page': 32,
  'drive': 64,
};

/// 外部入口的固定次序
const List<String> _exposeOrder = <String>[
  'cli',
  'mcp',
  'link',
  'plugin',
  'ui',
  'page',
  'drive',
];

/// 用途的中文说法（文档用）
const Map<String, String> _usageText = <String, String>{
  'point': '功能埋点',
  'command': '命令',
  'service': '插件服务',
};

/* ==================== 声明模型 ==================== */

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
    required this.expose,
    required this.consent,
    required this.usage,
    required this.implemented,
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

  /// `cli` / `mcp` / `link` / `plugin` / `ui` / `page` / `drive`，按固定次序
  final List<String> expose;

  /// `none` / `ask`
  final String consent;

  /// `point` / `command` / `service`，按固定次序
  final List<String> usage;

  /// 主程序是否已经接入
  final bool implemented;

  /// 载荷与回答的说明（未接入的条目为空或只是草稿）
  final String doc;

  /// 实现是否开放（`phases` 含 `impl`）
  bool get openForImpl => phases.contains('impl');

  /// 阶段位掩码
  int get phaseMask => phases.fold<int>(
    0,
    (int mask, String phase) => mask | (1 << _phaseCodes[phase]!),
  );

  /// 用途位掩码
  int get usageMask =>
      usage.fold<int>(0, (int mask, String item) => mask | _usageCodes[item]!);

  /// 外部入口位掩码
  int get exposeMask => expose.fold<int>(
    0,
    (int mask, String item) => mask | _exposeCodes[item]!,
  );
}

/// 一条广播声明
final class BroadcastDeclaration {
  BroadcastDeclaration({
    required this.id,
    required this.dart,
    required this.title,
    required this.depict,
    required this.cache,
    required this.writable,
    required this.pushOnSubscribe,
    required this.coalesce,
    required this.scope,
    required this.implemented,
    required this.doc,
  });

  final String id;
  final String dart;
  final String title;
  final String depict;

  /// `none` / `latest`
  final String cache;

  /// `owner` / `anyone`
  final String writable;
  final bool pushOnSubscribe;
  final bool coalesce;

  /// `app` / `system`
  final String scope;
  final bool implemented;
  final String doc;

  /// 有没有"当前值"可读
  bool get cached => cache == 'latest';
}

/// 一类动态广播（id 由属主在运行期决定，例如变量）
final class DynamicBroadcastDeclaration {
  DynamicBroadcastDeclaration({
    required this.idPattern,
    required this.title,
    required this.depict,
    required this.cache,
    required this.writable,
    required this.pushOnSubscribe,
    required this.coalesce,
    required this.scope,
    required this.doc,
  });

  final String idPattern;
  final String title;
  final String depict;
  final String cache;
  final String writable;
  final bool pushOnSubscribe;
  final bool coalesce;
  final String scope;
  final String doc;
}

/// 定义文件的全部内容
final class CapabilityContract {
  CapabilityContract({
    required this.capabilities,
    required this.broadcasts,
    required this.dynamicBroadcasts,
  });

  final List<CapabilityDeclaration> capabilities;
  final List<BroadcastDeclaration> broadcasts;
  final List<DynamicBroadcastDeclaration> dynamicBroadcasts;
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

/// 读取定义文件并做全部校验
///
/// 校验的是"设计语义"，不是格式：
/// - id 命名空间只有 `musicxx.*` 与 `plugin.*` 两个；
/// - `phases` 不含 `impl` 时只能是 `impls: one`（实现固定由应用提供，插件只能注册 before / after）；
/// - `merge` 只在 `impls: many` 且 `phases` 含 `impl` 时写，其余情况写了就是声明错了；
/// - `usage` 至少一项（否则这条能力没有任何调用方）；
/// - 广播的 `pushOnSubscribe` 只能在 `cache: latest` 时声明（没有当前值就没什么可推的）。
CapabilityContract loadCapabilityContract({String path = capabilitiesDefPath}) {
  final File file = File(path);
  if (!file.existsSync()) {
    throw StateError('找不到定义文件: $path');
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
    final List<String> usage = _stringList(
      json,
      'usage',
      where,
      allowed: _usageCodes.keys.toList(growable: false),
      order: _usageOrder,
      allowEmpty: false,
    );
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
        expose: _stringList(
          json,
          'expose',
          where,
          allowed: _exposeCodes.keys.toList(growable: false),
          order: _exposeOrder,
        ),
        consent: _requireEnum(json, 'consent', where, _consentCodes),
        usage: usage,
        implemented: _optionalBool(json, 'implemented', where),
        doc: _optionalString(json, 'doc'),
      ),
    );
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
    broadcasts.add(
      BroadcastDeclaration(
        id: id,
        dart: dart,
        title: _optionalString(json, 'title'),
        depict: _optionalString(json, 'depict'),
        cache: cache,
        writable: _requireEnum(json, 'writable', where, _writableCodes),
        pushOnSubscribe: pushOnSubscribe,
        coalesce: _optionalBool(json, 'coalesce', where),
        scope: _requireEnum(json, 'scope', where, _scopeCodes),
        implemented: _optionalBool(json, 'implemented', where),
        doc: _optionalString(json, 'doc'),
      ),
    );
  }

  final List<DynamicBroadcastDeclaration> dynamicBroadcasts =
      <DynamicBroadcastDeclaration>[];
  final Object? rawDynamic = def['dynamicBroadcasts'];
  if (rawDynamic is List) {
    final Set<String> patterns = <String>{};
    for (final Object? item in rawDynamic) {
      final Map<String, Object?> json = item! as Map<String, Object?>;
      final String pattern = _requireString(json, 'idPattern', '动态广播');
      final String where = '动态广播 $pattern';
      if (!pattern.contains('<')) {
        throw StateError('$where: idPattern 里要用 <占位名> 表达属主决定的部分');
      }
      if (!patterns.add(pattern)) {
        throw StateError('动态广播 idPattern 重复: $pattern');
      }
      final String cache = _requireEnum(
        json,
        'cache',
        where,
        _broadcastCacheCodes,
      );
      final bool pushOnSubscribe = _optionalBool(
        json,
        'pushOnSubscribe',
        where,
      );
      if (pushOnSubscribe && cache != 'latest') {
        throw StateError('$where: 只有 cache=latest 才能在订阅时立刻推当前值');
      }
      dynamicBroadcasts.add(
        DynamicBroadcastDeclaration(
          idPattern: pattern,
          title: _optionalString(json, 'title'),
          depict: _optionalString(json, 'depict'),
          cache: cache,
          writable: _requireEnum(json, 'writable', where, _writableCodes),
          pushOnSubscribe: pushOnSubscribe,
          coalesce: _optionalBool(json, 'coalesce', where),
          scope: _requireEnum(json, 'scope', where, _scopeCodes),
          doc: _optionalString(json, 'doc'),
        ),
      );
    }
  }

  return CapabilityContract(
    capabilities: capabilities,
    broadcasts: broadcasts,
    dynamicBroadcasts: dynamicBroadcasts,
  );
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

/// 用途的中文说法（多个用顿号连起来）
String _usageTextOf(List<String> usage) =>
    usage.map((String item) => _usageText[item] ?? item).join('、');

/// 阶段的中文说法（没有 impl 时写清"只能 before / after"）
String _phaseTextOf(CapabilityDeclaration decl) {
  if (!decl.openForImpl) {
    return '只能 ${decl.phases.join(" / ")}';
  }
  return decl.phases.join(' / ');
}

/// Dart 侧列表字面量：`<String>['a', 'b']`
String _dartStringList(List<String> values) =>
    '<String>[${values.map(_dartLiteral).join(', ')}]';

/* ==================== Dart 生成物 ==================== */

String _generateDart(CapabilityContract contract) {
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
    ..writeln('final class MusicxxCapabilityDecl {')
    ..writeln('  const MusicxxCapabilityDecl({')
    ..writeln('    required this.id,')
    ..writeln('    required this.impls,')
    ..writeln('    required this.merge,')
    ..writeln('    required this.phases,')
    ..writeln('    required this.dispatch,')
    ..writeln('    required this.timeoutMs,')
    ..writeln('    required this.cache,')
    ..writeln('    required this.expose,')
    ..writeln('    required this.consent,')
    ..writeln('    required this.usage,')
    ..writeln('    required this.implemented,')
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
    ..writeln(
      '  /// `cli` / `mcp` / `link` / `plugin` / `ui` / `page` / `drive` 的子集',
    )
    ..writeln('  final List<String> expose;')
    ..writeln()
    ..writeln('  /// `none` / `ask`（链接触发时是否先问用户）')
    ..writeln('  final String consent;')
    ..writeln()
    ..writeln('  /// `point` / `command` / `service` 的子集')
    ..writeln('  final List<String> usage;')
    ..writeln()
    ..writeln('  /// 主程序是否已经接入（未接入的条目注册不会报错，也不会触发）')
    ..writeln('  final bool implemented;')
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
    ..writeln('  /// 只作功能埋点（界面上叫"功能埋点"，代码里叫能力）')
    ..writeln("  bool get isPoint => usage.contains('point');")
    ..writeln()
    ..writeln('  /// 能被外部当命令调')
    ..writeln("  bool get isCommand => usage.contains('command');")
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
    ..writeln('    required this.writable,')
    ..writeln('    required this.pushOnSubscribe,')
    ..writeln('    required this.coalesce,')
    ..writeln('    required this.scope,')
    ..writeln('    required this.implemented,')
    ..writeln('    required this.title,')
    ..writeln('    required this.depict,')
    ..writeln('  });')
    ..writeln()
    ..writeln('  final String id;')
    ..writeln()
    ..writeln('  /// `none`（事件）/ `latest`（快照）')
    ..writeln('  final String cache;')
    ..writeln()
    ..writeln('  /// `owner` / `anyone`')
    ..writeln('  final String writable;')
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
    ..writeln('  final bool implemented;')
    ..writeln()
    ..writeln('  final String title;')
    ..writeln()
    ..writeln('  final String depict;')
    ..writeln()
    ..writeln('  /// 能同步读当前值')
    ..writeln("  bool get cached => cache == 'latest';")
    ..writeln()
    ..writeln('  /// 任何人都能写（变量）')
    ..writeln("  bool get writableByAnyone => writable == 'anyone';")
    ..writeln('}')
    ..writeln()
    ..writeln('/// 一类动态广播（id 由属主在运行期决定，例如变量）')
    ..writeln('final class MusicxxDynamicBroadcastDecl {')
    ..writeln('  const MusicxxDynamicBroadcastDecl({')
    ..writeln('    required this.idPattern,')
    ..writeln('    required this.cache,')
    ..writeln('    required this.writable,')
    ..writeln('    required this.pushOnSubscribe,')
    ..writeln('    required this.coalesce,')
    ..writeln('    required this.scope,')
    ..writeln('    required this.title,')
    ..writeln('    required this.depict,')
    ..writeln('  });')
    ..writeln()
    ..writeln('  /// `musicxx.var.<属主>.<键>` 这类写法，用 <占位名> 表达属主决定的部分')
    ..writeln('  final String idPattern;')
    ..writeln('  final String cache;')
    ..writeln('  final String writable;')
    ..writeln('  final bool pushOnSubscribe;')
    ..writeln('  final bool coalesce;')
    ..writeln('  final String scope;')
    ..writeln('  final String title;')
    ..writeln('  final String depict;')
    ..writeln()
    ..writeln('  /// 这个 id 是不是命中这类写法')
    ..writeln('  ///')
    ..writeln('  /// - `<占位>` 每段匹配一个非空片段；')
    ..writeln('  /// - 最后一段是占位时吃掉剩下的全部（`musicxx.point.<名字>` 允许名字里再带点）。')
    ..writeln('  bool matches(String id) {')
    ..writeln('    final List<String> patternParts = idPattern.split(".");')
    ..writeln('    final List<String> idParts = id.split(".");')
    ..writeln('    if (idParts.length < patternParts.length) {')
    ..writeln('      return false;')
    ..writeln('    }')
    ..writeln('    for (int i = 0; i < patternParts.length; i++) {')
    ..writeln('      final String part = patternParts[i];')
    ..writeln(
      '      final bool placeholder = part.startsWith("<") && part.endsWith(">");',
    )
    ..writeln('      if (placeholder) {')
    ..writeln('        if (i == patternParts.length - 1) {')
    ..writeln('          // 最后一段吃掉剩下的部分：至少要有一段')
    ..writeln('          return idParts.length >= patternParts.length;')
    ..writeln('        }')
    ..writeln('        if (idParts[i].isEmpty) {')
    ..writeln('          return false;')
    ..writeln('        }')
    ..writeln('        continue;')
    ..writeln('      }')
    ..writeln('      if (part != idParts[i]) {')
    ..writeln('        return false;')
    ..writeln('      }')
    ..writeln('    }')
    ..writeln('    return idParts.length == patternParts.length;')
    ..writeln('  }')
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
      ..writeln('      expose: ${_dartStringList(decl.expose)},')
      ..writeln('      consent: ${_dartLiteral(decl.consent)},')
      ..writeln('      usage: ${_dartStringList(decl.usage)},')
      ..writeln('      implemented: ${decl.implemented},')
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
      ..writeln('      writable: ${_dartLiteral(decl.writable)},')
      ..writeln('      pushOnSubscribe: ${decl.pushOnSubscribe},')
      ..writeln('      coalesce: ${decl.coalesce},')
      ..writeln('      scope: ${_dartLiteral(decl.scope)},')
      ..writeln('      implemented: ${decl.implemented},')
      ..writeln('      title: ${_dartLiteral(decl.title)},')
      ..writeln('      depict: ${_dartLiteral(decl.depict)},')
      ..writeln('    ),');
  }

  sb
    ..writeln('  ];')
    ..writeln()
    ..writeln('  /// 动态广播声明（id 在运行期由属主决定）')
    ..writeln(
      '  static const List<MusicxxDynamicBroadcastDecl> dynamicBroadcasts =',
    )
    ..writeln('      <MusicxxDynamicBroadcastDecl>[');

  for (final DynamicBroadcastDeclaration decl in contract.dynamicBroadcasts) {
    sb
      ..writeln('    MusicxxDynamicBroadcastDecl(')
      ..writeln('      idPattern: ${_dartLiteral(decl.idPattern)},')
      ..writeln('      cache: ${_dartLiteral(decl.cache)},')
      ..writeln('      writable: ${_dartLiteral(decl.writable)},')
      ..writeln('      pushOnSubscribe: ${decl.pushOnSubscribe},')
      ..writeln('      coalesce: ${decl.coalesce},')
      ..writeln('      scope: ${_dartLiteral(decl.scope)},')
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
    ..writeln()
    ..writeln('  /// 命中哪一类动态写法（没有时返回 null）')
    ..writeln('  ///')
    ..writeln('  /// 按声明顺序匹配，同一条 id 只落在一类写法里（`plugin.<id>.net` 这种具体名字')
    ..writeln('  /// 也走通用写法）。')
    ..writeln(
      '  static MusicxxDynamicBroadcastDecl? dynamicBroadcastOf(String id) {',
    )
    ..writeln(
      '    for (final MusicxxDynamicBroadcastDecl decl in dynamicBroadcasts) {',
    )
    ..writeln('      if (decl.matches(id)) {')
    ..writeln('        return decl;')
    ..writeln('      }')
    ..writeln('    }')
    ..writeln('    return null;')
    ..writeln('  }')
    ..writeln()
    ..writeln('  /// 按 id 取广播声明：静态优先，其次按动态写法合成一份')
    ..writeln('  ///')
    ..writeln('  /// 合成出来的是运行期频道的声明（`id` 就是传进来的那个）；缓存 / 写权限 /')
    ..writeln('  /// 合并 / 作用域都来自那一类写法。')
    ..writeln(
      '  static MusicxxBroadcastDecl? broadcastDeclarationOf(String id) {',
    )
    ..writeln('    final MusicxxBroadcastDecl? declared = _broadcastIndex[id];')
    ..writeln('    if (null != declared) {')
    ..writeln('      return declared;')
    ..writeln('    }')
    ..writeln(
      '    final MusicxxDynamicBroadcastDecl? dynamic = dynamicBroadcastOf(id);',
    )
    ..writeln('    if (null == dynamic) {')
    ..writeln('      return null;')
    ..writeln('    }')
    ..writeln('    return MusicxxBroadcastDecl(')
    ..writeln('      id: id,')
    ..writeln('      cache: dynamic.cache,')
    ..writeln('      writable: dynamic.writable,')
    ..writeln('      pushOnSubscribe: dynamic.pushOnSubscribe,')
    ..writeln('      coalesce: dynamic.coalesce,')
    ..writeln('      scope: dynamic.scope,')
    ..writeln('      implemented: true,')
    ..writeln('      title: dynamic.title,')
    ..writeln('      depict: dynamic.depict,')
    ..writeln('    );')
    ..writeln('  }')
    ..writeln('}');
  return sb.toString();
}

/* ==================== C++ 生成物 ==================== */

String _generateCpp(CapabilityContract contract) {
  final StringBuffer sb = StringBuffer()
    ..writeln(
      '/// 自动生成（tools/gen_contract.dart ← tools/capabilities.def.json）—— 请勿手改。',
    )
    ..writeln('///')
    ..writeln('/// C++ 侧能力与广播约定：id 常量 + 已知声明表（插件框架据此拒绝未知 id、')
    ..writeln('/// 判实现槽位 / 合并策略 / 阶段 / 超时 / 缓存与写权限）。')
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
    ..writeln('/* ==================== 用途位掩码 ==================== */')
    ..writeln()
    ..writeln('#define MUSICXX_CAPABILITY_USAGE_POINT   0x1u')
    ..writeln('#define MUSICXX_CAPABILITY_USAGE_COMMAND 0x2u')
    ..writeln('#define MUSICXX_CAPABILITY_USAGE_SERVICE 0x4u')
    ..writeln()
    ..writeln('/* ==================== 外部入口位掩码 ==================== */')
    ..writeln()
    ..writeln('#define MUSICXX_CAPABILITY_EXPOSE_CLI    0x01u')
    ..writeln('#define MUSICXX_CAPABILITY_EXPOSE_MCP    0x02u')
    ..writeln('#define MUSICXX_CAPABILITY_EXPOSE_LINK   0x04u')
    ..writeln('#define MUSICXX_CAPABILITY_EXPOSE_PLUGIN 0x08u')
    ..writeln('#define MUSICXX_CAPABILITY_EXPOSE_UI     0x10u')
    ..writeln('#define MUSICXX_CAPABILITY_EXPOSE_PAGE   0x20u')
    ..writeln('#define MUSICXX_CAPABILITY_EXPOSE_DRIVE  0x40u')
    ..writeln()
    ..writeln('/* ==================== 链接触发时是否先问用户 ==================== */')
    ..writeln()
    ..writeln('#define MUSICXX_CAPABILITY_CONSENT_NONE 0')
    ..writeln('#define MUSICXX_CAPABILITY_CONSENT_ASK  1')
    ..writeln()
    ..writeln('/* ==================== 广播的缓存与写权限 ==================== */')
    ..writeln()
    ..writeln('/// 事件：没有当前值')
    ..writeln('#define MUSICXX_BROADCAST_CACHE_NONE   0')
    ..writeln('/// 快照：订阅即拿到当前值，也能同步读')
    ..writeln('#define MUSICXX_BROADCAST_CACHE_LATEST 1')
    ..writeln()
    ..writeln('/// 只有属主能写')
    ..writeln('#define MUSICXX_BROADCAST_WRITABLE_OWNER  0')
    ..writeln('/// 任何人都能写（变量）')
    ..writeln('#define MUSICXX_BROADCAST_WRITABLE_ANYONE 1')
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
    ..writeln('    uint32_t    usage_mask;  ///< MUSICXX_CAPABILITY_USAGE_*')
    ..writeln('    uint32_t    expose_mask; ///< MUSICXX_CAPABILITY_EXPOSE_*')
    ..writeln('    int32_t     consent;     ///< MUSICXX_CAPABILITY_CONSENT_*')
    ..writeln('    int32_t     implemented; ///< 主程序是否已经接入')
    ..writeln('} MusicxxCapabilityDecl;')
    ..writeln()
    ..writeln('/// 一条广播的声明')
    ..writeln('typedef struct MusicxxBroadcastDecl {')
    ..writeln('    const char* id;')
    ..writeln(
      '    int32_t     cache;             ///< MUSICXX_BROADCAST_CACHE_*',
    )
    ..writeln(
      '    int32_t     writable;          ///< MUSICXX_BROADCAST_WRITABLE_*',
    )
    ..writeln('    int32_t     push_on_subscribe; ///< 订阅时立刻推当前值')
    ..writeln('    int32_t     coalesce;          ///< 允许合并（允许丢中间值）')
    ..writeln(
      '    int32_t     scope;             ///< MUSICXX_BROADCAST_SCOPE_*',
    )
    ..writeln('    int32_t     implemented;       ///< 主程序是否已经接入')
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
      '${_cacheCodes[decl.cache]!}, ${decl.usageMask}u, ${decl.exposeMask}u, '
      '${_consentCodes[decl.consent]!}, ${decl.implemented ? 1 : 0}},',
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
      '${_writableCodes[decl.writable]!}, ${decl.pushOnSubscribe ? 1 : 0}, '
      '${decl.coalesce ? 1 : 0}, ${_scopeCodes[decl.scope]!}, '
      '${decl.implemented ? 1 : 0}},',
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

/* ==================== 作者文档 ==================== */

String _generateCapabilityDoc(CapabilityContract contract) {
  final StringBuffer sb = StringBuffer()
    ..writeln('# 能力总表（插件作者文档）')
    ..writeln()
    ..writeln(
      '> 本文件由 `tools/gen_contract.dart` 从 `tools/capabilities.def.json` 生成，请勿手改。',
    )
    ..writeln()
    ..writeln(
      '> 两张表：**能力**（前链改输入 / 实现链给结果 / 后链改输出）就是本文里的这些，'
      '**广播**（只通知、不回答）在 [plugin-broadcasts.md](plugin-broadcasts.md)。'
      '写法（注册、阶段顺序、裁决对象）见 [plugin-api.md](plugin-api.md) 第 8 节。',
    )
    ..writeln()
    ..writeln('**能力** = 调用方发一次请求、等一个结果。同一个 id 只有一份声明，声明里写清：')
    ..writeln()
    ..writeln('| 字段 | 含义 |')
    ..writeln('|---|---|')
    ..writeln(
      '| `impls` | `one` = 一个槽位（后登记的覆盖先登记的，跨插件也覆盖）；'
      '`many` = 多实现共存，按 (层, priority, 登记顺序) 组成链 |',
    )
    ..writeln(
      '| `merge` | 多实现的结果怎么合并：`first` = 第一个合法结果生效；'
      '`veto` = 任一实现能否决 |',
    )
    ..writeln(
      '| `phases` | 能参与哪些阶段：`before`（正序改输入）/ `impl`（给结果）/ '
      '`after`（逆序改输出）。**不含 `impl` = 实现固定由应用提供，插件只能注册 `before` / `after`** |',
    )
    ..writeln('| `dispatch` | `sync` = 调用方等结果（插件侧只能同步处理器）；`async` = 不占用调用线程 |')
    ..writeln('| `timeoutMs` | 等实现的上限，超时按"没给"处理；`0` = 不设上限 |')
    ..writeln('| `cache` | `byIdentity` = 同一个请求身份不重复算；`none` = 每次都问实现 |')
    ..writeln(
      '| `usage` | 这条能力用来干什么。**功能埋点** = 只给应用自己当埋点调；'
      '**命令** = 外部（命令行 / MCP / 链接）能调；**插件服务** = 插件之间互相调 |',
    )
    ..writeln(
      '| `expose` | 谁能调：`cli` / `mcp` / `link` / `plugin` / `ui` / `page` / `drive` |',
    )
    ..writeln('| `consent` | 链接触发时是否先问用户（`ask` = 问，`none` = 不问） |')
    ..writeln()
    ..writeln('## 怎么读这张表')
    ..writeln()
    ..writeln(
      '- **用途**列就是界面与文档里的说法：`功能埋点` 对应代码里的"能力"'
      '（插件接管它的实现就是替代应用自己的这一段）；',
    )
    ..writeln(
      '- **可注册阶段**列写清插件能参与哪一步；只写 `before / after` 的能力**不能**注册实现'
      '（注册会被拒并说明原因）；',
    )
    ..writeln(
      '- **状态**列 `已接入` = 主程序已经在调用点接上（注册后会被调用）；'
      '`未接入` = 约定已定、主程序还没接（注册不报错，也不会触发）；',
    )
    ..writeln(
      '- 注册时写全名：官方 `musicxx.*`；实现 / 声明自己的能力时写名字'
      '（宿主补成 `plugin.<本插件 id>.<名字>`，名字可以含点，例如 `drive.list`）。',
    )
    ..writeln()
    ..writeln('## 能力清单')
    ..writeln()
    ..writeln('| 能力 id | 用途 | 可注册阶段 | 合并 | 派发 | 超时 | 缓存 | 外部入口 | 状态 |')
    ..writeln('|---|---|---|---|---|---|---|---|---|');

  for (final CapabilityDeclaration decl in contract.capabilities) {
    final String timeout = decl.timeoutMs == 0 ? '-' : '${decl.timeoutMs} ms';
    final String cache = decl.cache == 'byIdentity' ? '按身份' : '-';
    final String expose = decl.expose.isEmpty ? '-' : decl.expose.join('/');
    sb.writeln(
      '| `${decl.id}` | ${_usageTextOf(decl.usage)} | ${_phaseTextOf(decl)} | '
      '${decl.merge.isEmpty ? "-" : decl.merge} | ${decl.dispatch} | $timeout | '
      '$cache | $expose | ${decl.implemented ? "已接入" : "未接入"} |',
    );
  }

  sb
    ..writeln()
    ..writeln('## 载荷与回答（已接入的能力）')
    ..writeln()
    ..writeln('未接入的能力只占位：注册不会报错，也不会触发。已接入的每一条下面写清载荷字段与回答格式。');

  for (final CapabilityDeclaration decl in contract.capabilities) {
    if (!decl.implemented || decl.doc.trim().isEmpty) {
      continue;
    }
    final String heading = decl.title.isEmpty ? '' : ' ${decl.title}';
    sb
      ..writeln()
      ..writeln('### `${decl.id}`$heading')
      ..writeln()
      ..writeln(decl.doc.trim());
  }
  return sb.toString();
}

String _generateBroadcastDoc(CapabilityContract contract) {
  final StringBuffer sb = StringBuffer()
    ..writeln('# 广播总表（插件作者文档）')
    ..writeln()
    ..writeln(
      '> 本文件由 `tools/gen_contract.dart` 从 `tools/capabilities.def.json` 生成，请勿手改。',
    )
    ..writeln()
    ..writeln(
      '> 两张表：**广播频道**（只通知、不回答、不参与合并）就是本文里的这些，'
      '**能力**（能改输入、给结果、改输出）在 [plugin-capabilities.md](plugin-capabilities.md)。'
      '订阅 / 发布的写法见 [plugin-api.md](plugin-api.md) 第 8 节。',
    )
    ..writeln()
    ..writeln('**广播** = 属主改了东西、通知所有关心的人。它没有返回值、不等待、允许丢中间值；')
    ..writeln('要不要"当前值"由声明决定：`cache: latest` 的频道订阅时就能拿到当前值，也能同步读；')
    ..writeln('`cache: none` 的是事件，只能等下一次通知。')
    ..writeln()
    ..writeln('| 字段 | 含义 |')
    ..writeln('|---|---|')
    ..writeln('| `cache` | `latest` = 有当前值（快照，只保留最后一份）；`none` = 只有事件 |')
    ..writeln('| `writable` | `owner` = 只有属主能写；`anyone` = 任何人都能写（变量） |')
    ..writeln('| `pushOnSubscribe` | 订阅时立刻收到当前缓存值（仅 `cache: latest`） |')
    ..writeln('| `coalesce` | 允许合并（允许丢中间值）；变量类是 `false`（写要回执，不能丢） |')
    ..writeln('| `scope` | `app` = 业务频道；`system` = 调试与自省频道（插件不能写） |')
    ..writeln()
    ..writeln('## 频道清单')
    ..writeln()
    ..writeln('| 广播 id | 缓存 | 写权限 | 订阅即推 | 合并 | 作用域 | 状态 |')
    ..writeln('|---|---|---|---|---|---|---|');

  for (final BroadcastDeclaration decl in contract.broadcasts) {
    sb.writeln(
      '| `${decl.id}` | ${decl.cache} | ${decl.writable} | '
      '${decl.pushOnSubscribe ? "是" : "否"} | '
      '${decl.coalesce ? "允许" : "不合并"} | '
      '${decl.scope} | ${decl.implemented ? "已接入" : "未接入"} |',
    );
  }

  if (contract.dynamicBroadcasts.isNotEmpty) {
    sb
      ..writeln()
      ..writeln('## 动态频道（id 由属主在运行期决定）')
      ..writeln()
      ..writeln(
        '这几类的**形状**由声明表决定，**具体 id** 在运行期由属主给出（插件发布时生成，'
        '应用自己登记的点频道由应用登记）：缓存 / 写权限 / 作用域都跟着这一类写法，'
        '不需要逐个 id 上报。',
      )
      ..writeln()
      ..writeln('| id 写法 | 缓存 | 写权限 | 订阅即推 | 合并 | 作用域 | 说明 |')
      ..writeln('|---|---|---|---|---|---|---|');
    for (final DynamicBroadcastDeclaration decl in contract.dynamicBroadcasts) {
      sb.writeln(
        '| `${decl.idPattern}` | ${decl.cache} | ${decl.writable} | '
        '${decl.pushOnSubscribe ? "是" : "否"} | '
        '${decl.coalesce ? "允许" : "不合并"} | '
        '${decl.scope} | ${decl.depict} |',
      );
    }
    for (final DynamicBroadcastDeclaration decl in contract.dynamicBroadcasts) {
      if (decl.doc.trim().isEmpty) {
        continue;
      }
      sb
        ..writeln()
        ..writeln('**`${decl.idPattern}`**：${decl.doc.trim()}');
    }
  }

  final List<BroadcastDeclaration> documented = <BroadcastDeclaration>[
    for (final BroadcastDeclaration decl in contract.broadcasts)
      if (decl.implemented && decl.doc.trim().isNotEmpty) decl,
  ];
  if (documented.isNotEmpty) {
    sb
      ..writeln()
      ..writeln('## 载荷（已接入的频道）');
    for (final BroadcastDeclaration decl in documented) {
      final String heading = decl.title.isEmpty ? '' : ' ${decl.title}';
      sb
        ..writeln()
        ..writeln('### `${decl.id}`$heading')
        ..writeln()
        ..writeln(decl.doc.trim());
    }
  }
  return sb.toString();
}

/* ==================== 对外入口 ==================== */

/// 生成全部产物（path → 内容），并做完整校验
Map<String, String> buildCapabilityContract({
  String defPath = capabilitiesDefPath,
}) {
  final CapabilityContract contract = loadCapabilityContract(path: defPath);
  return <String, String>{
    capabilityDartOut: _generateDart(contract),
    capabilityCppOut: _generateCpp(contract),
    capabilityDocOut: _generateCapabilityDoc(contract),
    broadcastDocOut: _generateBroadcastDoc(contract),
  };
}
