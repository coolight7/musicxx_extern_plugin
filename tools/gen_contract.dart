/// 契约生成器：把 `tools/hooks.def.json` 生成三份产物
///
/// 用法（在包目录下）：
/// ```
/// dart run tools/gen_contract.dart            # 生成
/// dart run tools/gen_contract.dart --check    # 只校验（生成物与定义一致时退出码 0）
/// ```
///
/// 产物：
/// | 产物 | 用途 |
/// |---|---|
/// | `lib/src/hook_ids.g.dart` | Dart 侧 `MusicxxPluginHookId` 枚举 + 元信息 + id 查找 |
/// | `src/sdk/include/musicxx/plugin/api/hook_ids.g.h` | C++ 侧钩子 id 常量 + 已知钩子表（宿主与插件共用） |
/// | `docs/plugin-hooks.md` | 插件作者文档（钩子总表） |
///
/// 为什么是这三处：Dart 侧埋点与派发要用枚举、原生宿主要用表拒绝未知钩子、
/// 插件作者要有一份可查的清单。定义只有一处，避免常量漂移。
///
/// 说明：C++ 头放在 **SDK 目录** 而不是 `src/host/include/`，
/// 这样宿主（`find_package` 后拿到的 SDK 头）与插件作者（`musicxx_extern_plugin_sdk`）
/// 都能直接包含同一份生成物，不需要维护两份。
library;

import 'dart:convert';
import 'dart:io';

const String _defPath = 'tools/hooks.def.json';
const String _dartOut = 'lib/src/hook_ids.g.dart';
const String _cppOut = 'src/sdk/include/musicxx/plugin/api/hook_ids.g.h';
const String _docOut = 'docs/plugin-hooks.md';

/// 合并策略名 → C++/Dart 两侧共用的整数编码（与宿主 `dispatchHook` 的判据一致）
const Map<String, int> _policyCodes = <String, int>{
  'firstNonNull': 0,
  'anyCancel': 1,
  'allMerge': 2,
  'lastWrite': 3,
};

/// 裁决派发方式：`sync` = 调用点就地等待（`decide`）；`async` = 事件回传（`decideAsync`）
const Map<String, int> _dispatchCodes = <String, int>{'sync': 0, 'async': 1};

/// C++ 常量名后缀：`musicxx.player.beforePlaySong` → `PLAYER_BEFORE_PLAY_SONG`
String _cppConstant(String id) {
  final String body = id.startsWith('musicxx.')
      ? id.substring('musicxx.'.length)
      : id;
  final StringBuffer sb = StringBuffer('MUSICXX_PLUGIN_HOOK_');
  for (int i = 0; i < body.length; i++) {
    final String ch = body[i];
    final bool upper = ch.toUpperCase() == ch && ch.toLowerCase() != ch;
    if (ch == '.') {
      sb.write('_');
    } else if (upper && i > 0) {
      // 驼峰边界 → 下划线 + 大写（如 beforePlaySong → BEFORE_PLAY_SONG）
      sb.write('_${ch.toUpperCase()}');
    } else {
      sb.write(ch.toUpperCase());
    }
  }
  return sb.toString();
}

/// Dart 枚举名：定义文件里的 `dart` 字段
String _dartEnumName(Map<String, Object?> hook) => hook['dart']! as String;

int _modeCode(String mode) => mode == 'decision' ? 1 : 0;

String _generateDart(List<Map<String, Object?>> hooks) {
  final StringBuffer sb = StringBuffer()
    ..writeln('// 自动生成（tools/gen_contract.dart ← tools/hooks.def.json）—— 请勿手改。')
    ..writeln('//')
    ..writeln('// 钩子 id 是跨边界稳定契约：字符串值一旦发布不得修改，')
    ..writeln('// 只能新增或标记废弃。字段含义见 tools/hooks.def.json。')
    ..writeln('// ignore_for_file: type=lint, constant_identifier_names')
    ..writeln()
    ..writeln('/// 钩子模式（与 C++ `MUSICXX_PLUGIN_HOOK_MODE_*` 一致）')
    ..writeln('enum MusicxxPluginHookMode {')
    ..writeln("  /// 观察型：不等待、不裁决（入队即返回）")
    ..writeln('  observe(0),')
    ..writeln()
    ..writeln("  /// 裁决型：等待处理器链结果（有等待预算）")
    ..writeln('  decision(1);')
    ..writeln()
    ..writeln('  const MusicxxPluginHookMode(this.code);')
    ..writeln()
    ..writeln('  final int code;')
    ..writeln('}')
    ..writeln()
    ..writeln('/// 多处理器裁决合并策略')
    ..writeln('enum MusicxxPluginDecisionPolicy {')
    ..writeln('  /// 首个非空裁决生效，后续处理器不再询问')
    ..writeln('  firstNonNull(0),')
    ..writeln()
    ..writeln('  /// 任一处理器 cancel/deny 即生效（最保守），patch 按优先级合并')
    ..writeln('  anyCancel(1),')
    ..writeln()
    ..writeln('  /// 所有 patch 依次深合并')
    ..writeln('  allMerge(2),')
    ..writeln()
    ..writeln('  /// 最后一个非空裁决生效')
    ..writeln('  lastWrite(3);')
    ..writeln()
    ..writeln('  const MusicxxPluginDecisionPolicy(this.code);')
    ..writeln()
    ..writeln('  final int code;')
    ..writeln('}')
    ..writeln()
    ..writeln('/// 派发方式')
    ..writeln('enum MusicxxPluginHookDispatch {')
    ..writeln('  /// 调用点就地等待裁决（`MusicxxPluginHooks.decide`；占用调用线程，有等待预算）')
    ..writeln('  sync(0),')
    ..writeln()
    ..writeln('  /// 不占用调用线程：观察型 = 入队即返回；裁决型 = 异步派发（`decideAsync`，')
    ..writeln('  /// 结果经 `musicxx.hook.decision.result` 事件回传）')
    ..writeln('  async(1);')
    ..writeln()
    ..writeln('  const MusicxxPluginHookDispatch(this.code);')
    ..writeln()
    ..writeln('  final int code;')
    ..writeln('}')
    ..writeln()
    ..writeln('/// 钩子 id 与元信息（由 `tools/hooks.def.json` 生成）')
    ..writeln('enum MusicxxPluginHookId {');
  for (final Map<String, Object?> hook in hooks) {
    final String doc = (hook['doc'] as String?) ?? '';
    if (doc.isNotEmpty) {
      sb.writeln('  /// $doc');
    }
    final String mode = hook['mode']! as String;
    final String policy = hook['policy']! as String;
    final bool wired = hook['wired'] == true;
    final String dispatch = (hook['dispatch'] as String?) ?? 'sync';
    final int budget = (hook['budgetMs'] as num).toInt();
    final int hard = (hook['hardMs'] as num).toInt();
    sb.writeln(
      "  ${_dartEnumName(hook)}('${hook['id']}', MusicxxPluginHookMode.$mode, "
      'MusicxxPluginDecisionPolicy.$policy, $wired, '
      'MusicxxPluginHookDispatch.$dispatch, $budget, $hard),',
    );
  }
  sb
    ..writeln('  ;')
    ..writeln()
    ..writeln('  const MusicxxPluginHookId(')
    ..writeln('    this.id,')
    ..writeln('    this.mode,')
    ..writeln('    this.policy,')
    ..writeln('    this.wired,')
    ..writeln('    this.dispatch,')
    ..writeln('    this.budgetMs,')
    ..writeln('    this.hardMs,')
    ..writeln('  );')
    ..writeln()
    ..writeln('  /// 跨边界稳定 id（`musicxx.<域>.<名>`）')
    ..writeln('  final String id;')
    ..writeln()
    ..writeln('  /// 观察型 / 裁决型')
    ..writeln('  final MusicxxPluginHookMode mode;')
    ..writeln()
    ..writeln('  /// 裁决合并策略（观察型无意义）')
    ..writeln('  final MusicxxPluginDecisionPolicy policy;')
    ..writeln()
    ..writeln('  /// 应用侧是否已经埋点：true = 当前版本会派发；')
    ..writeln('  /// false = 契约已冻结但尚未埋点（注册不会报错，当前版本也不会触发）')
    ..writeln('  final bool wired;')
    ..writeln()
    ..writeln('  /// 派发方式（是否占用调用线程；见 `tools/hooks.def.json`）')
    ..writeln('  final MusicxxPluginHookDispatch dispatch;')
    ..writeln()
    ..writeln('  /// 整链软等待预算（毫秒；0 = 用宿主默认）')
    ..writeln('  final int budgetMs;')
    ..writeln()
    ..writeln('  /// 整链硬等待预算（毫秒；超过只记统计，不打断插件）')
    ..writeln('  final int hardMs;')
    ..writeln()
    ..writeln('  /// 是否裁决型（观察型不参与合并）')
    ..writeln(
      '  bool get isDecision => mode == MusicxxPluginHookMode.decision;',
    )
    ..writeln()
    ..writeln('  /// 是否为异步派发（观察型恒为 true；裁决型表示调用点用 `decideAsync`）')
    ..writeln(
      '  bool get isAsyncDispatch => dispatch == MusicxxPluginHookDispatch.async;',
    )
    ..writeln()
    ..writeln('  /// 整链等待预算上限（毫秒；异步派发时用来算等待上限）')
    ..writeln('  int get budgetLimitMs => budgetMs + hardMs;')
    ..writeln()
    ..writeln('  /// 按 id 反查（未知 id 返回 null；插件注册未知钩子会被宿主拒绝）')
    ..writeln(
      '  static MusicxxPluginHookId? tryFromId(String id) => _byId[id];',
    )
    ..writeln();
  // 反查表（懒构造，避免每次线性扫描 66 项）
  sb
    ..writeln('  static final Map<String, MusicxxPluginHookId> _byId = {')
    ..writeln(
      '    for (final MusicxxPluginHookId hook in values) hook.id: hook,',
    )
    ..writeln('  };')
    ..writeln('}')
    ..writeln();
  return sb.toString();
}

String _generateCpp(List<Map<String, Object?>> hooks) {
  final StringBuffer sb = StringBuffer()
    ..writeln(
      '/// 自动生成（tools/gen_contract.dart ← tools/hooks.def.json）—— 请勿手改。',
    )
    ..writeln('///')
    ..writeln('/// C++ 侧钩子契约：id 常量 + 已知钩子表（宿主据此拒绝未知钩子、按声明的')
    ..writeln('/// 模式/策略/预算派发；插件按常量注册，避免手写字符串打错）。')
    ..writeln('#ifndef MUSICXX_PLUGIN_HOOK_IDS_G_H')
    ..writeln('#define MUSICXX_PLUGIN_HOOK_IDS_G_H')
    ..writeln()
    ..writeln('#include <stddef.h>')
    ..writeln('#include <stdint.h>')
    ..writeln()
    ..writeln(
      '/* ==================== 合并策略编码 (与 Dart 侧 MusicxxPluginDecisionPolicy 一致) ==================== */',
    )
    ..writeln()
    ..writeln('#define MUSICXX_PLUGIN_HOOK_POLICY_FIRST_NON_NULL 0')
    ..writeln('#define MUSICXX_PLUGIN_HOOK_POLICY_ANY_CANCEL     1')
    ..writeln('#define MUSICXX_PLUGIN_HOOK_POLICY_ALL_MERGE      2')
    ..writeln('#define MUSICXX_PLUGIN_HOOK_POLICY_LAST_WRITE     3')
    ..writeln()
    ..writeln('/* ==================== 钩子 id 常量 ==================== */')
    ..writeln();
  final Set<String> used = <String>{};
  for (final Map<String, Object?> hook in hooks) {
    final String name = _cppConstant(hook['id']! as String);
    if (!used.add(name)) {
      throw StateError('钩子常量名冲突: $name');
    }
    sb.writeln('#define $name "${hook['id']}"');
  }
  sb
    ..writeln()
    ..writeln('#ifdef __cplusplus')
    ..writeln()
    ..writeln('namespace musicxx {')
    ..writeln('namespace plugin {')
    ..writeln()
    ..writeln('/// 已知钩子条目: id / 模式 / 合并策略 / 软硬等待预算 (毫秒; 0 = 宿主默认)')
    ..writeln('struct MusicxxPluginHookMeta {')
    ..writeln('    const char* id;')
    ..writeln(
      '    int32_t     mode;     ///< 0 观察型 / 1 裁决型 (MUSICXX_PLUGIN_HOOK_MODE_*)',
    )
    ..writeln('    int32_t     policy;   ///< MUSICXX_PLUGIN_HOOK_POLICY_*')
    ..writeln('    int32_t     budgetMs; ///< 整链软等待预算')
    ..writeln('    int32_t     hardMs;   ///< 整链硬等待预算 (超过只记统计)')
    ..writeln('};')
    ..writeln()
    ..writeln('/// 已知钩子表 (宿主拒绝表外钩子: 插件不得制造宿主不认识的钩子点)')
    ..writeln(
      'inline constexpr MusicxxPluginHookMeta musicxxPluginKnownHooks[] = {',
    );
  for (final Map<String, Object?> hook in hooks) {
    final int mode = _modeCode(hook['mode']! as String);
    final int policy = _policyCodes[hook['policy']]!;
    final int budget = (hook['budgetMs'] as num).toInt();
    final int hard = (hook['hardMs'] as num).toInt();
    sb.writeln("    {\"${hook['id']}\", $mode, $policy, $budget, $hard},");
  }
  sb
    ..writeln('};')
    ..writeln()
    ..writeln('/// 已知钩子条目数')
    ..writeln(
      'inline constexpr size_t musicxxPluginKnownHookCount'
      ' = sizeof(musicxxPluginKnownHooks) / sizeof(musicxxPluginKnownHooks[0]);',
    )
    ..writeln()
    ..writeln('} // namespace plugin')
    ..writeln('} // namespace musicxx')
    ..writeln()
    ..writeln('#endif /* __cplusplus */')
    ..writeln()
    ..writeln('#endif /* MUSICXX_PLUGIN_HOOK_IDS_G_H */');
  return sb.toString();
}

String _generateDoc(List<Map<String, Object?>> hooks) {
  final StringBuffer sb = StringBuffer()
    ..writeln('# 钩子总表（插件作者文档）')
    ..writeln()
    ..writeln(
      '> 本文件由 `tools/gen_contract.dart` 从 `tools/hooks.def.json` 生成，请勿手改。',
    )
    ..writeln('>')
    ..writeln('> 注册钩子时必须写全名（官方 `musicxx.*`）；插件自定义事件/能力/UI 项用')
    ..writeln('> `plugin.<pluginId>.*`。宿主会拒绝未知钩子与未知前缀。')
    ..writeln()
    ..writeln('## 怎么读这张表')
    ..writeln()
    ..writeln(
      '- **是否已埋点**：`已埋点` = 应用侧已经在调用点接上这个钩子（当前版本会派发，插件注册后会被调用）；'
      '`未埋点` = 钩子契约已冻结、应用侧**还没有接**（注册不会报错，但当前版本不会触发）。'
      '想知道某个钩子此刻有没有处理器，看管理页「外部插件 → 调试」分页的钩子统计（`hooks` 段），'
      '或在插件里调 `musicxx.hooks.has(id)`（只反映自己注册没注册）。',
    )
    ..writeln(
      '- **模式**：`observe` = 只通知（返回值忽略、不等待、没有预算）；`decision` = 可裁决，'
      '返回 `null` 表示这次不表态。',
    )
    ..writeln(
      '- **派发**：`sync` = 调用点就地等待（占用调用线程）；`async` = 不占用调用线程，'
      '裁决结果经 `musicxx.hook.decision.result` 事件回传（调用点用 `decideAsync` 时）。',
    )
    ..writeln(
      '- **合并策略**：多个处理器给出裁决时怎么合并（`firstNonNull` 首个非空生效并停止询问、'
      '`anyCancel` 任一 cancel/skip 即生效、`allMerge` 全部合并、`lastWrite` 最后一个生效）。',
    )
    ..writeln(
      '- **软/硬预算**：软预算 = 整条处理器链最多等多久（超时按“无裁决”继续，不打断插件）；'
      '硬预算 = 单个处理器耗时超过它只记一条 `musicxx.plugin.warn` 与统计。'
      'JS 插件的处理器链还有一层固定上限：最多等 100 ms（Promise 超预算按不裁决处理）。',
    )
    ..writeln()
    ..writeln('| 钩子 id | 模式 | 派发 | 合并策略 | 软/硬预算 (ms) | 是否已埋点 |')
    ..writeln('|---|---|---|---|---|---|');
  for (final Map<String, Object?> hook in hooks) {
    final String budget =
        '${(hook['budgetMs'] as num).toInt()} / ${(hook['hardMs'] as num).toInt()}';
    final String dispatch = (hook['dispatch'] as String?) ?? 'sync';
    final String wiredText = hook['wired'] == true ? '已埋点' : '未埋点';
    sb.writeln(
      "| `${hook['id']}` | ${hook['mode']} | $dispatch | ${hook['policy']} | $budget | $wiredText |",
    );
  }
  sb
    ..writeln()
    ..writeln('## 已埋点钩子的载荷与裁决')
    ..writeln()
    ..writeln(
      '下面是应用侧**已经埋点**的钩子：处理器拿到的载荷字段与裁决语义都在这里。'
      '尚未埋点的钩子只冻结了 id / 模式 / 派发 / 合并策略，载荷字段在应用侧接入时补齐'
      '（接入后会写进 `tools/hooks.def.json` 的 `doc` 字段并重新生成本文件）。',
    )
    ..writeln()
    ..writeln('> 载荷统一是 JSON 对象；不裁决时返回 `null`（JS）或把出参留空（C++）。')
    ..writeln(
      '> 载荷里不放音频直链与 token：需要地址时请用 `musicxx.net` / 宿主动作自行获取。',
    );
  for (final Map<String, Object?> hook in hooks) {
    final String doc = (hook['doc'] as String?) ?? '';
    if (doc.isEmpty) {
      continue;
    }
    final String dispatch = (hook['dispatch'] as String?) ?? 'sync';
    final String budget =
        '${(hook['budgetMs'] as num).toInt()} / ${(hook['hardMs'] as num).toInt()}';
    sb
      ..writeln()
      ..writeln('### `${hook['id']}`')
      ..writeln()
      ..writeln(
        '- 模式：`${hook['mode']}`；派发：`$dispatch`；'
        '合并策略：`${hook['policy']}`；软/硬预算：`$budget` ms',
      )
      ..writeln('- $doc');
  }
  sb
    ..writeln()
    ..writeln('## 派发方式')
    ..writeln()
    ..writeln('| 派发 | 含义 | 调用点写法 |')
    ..writeln('|---|---|---|')
    ..writeln('| `sync` | 调用点就地等待裁决（占用调用线程，有等待预算） | `decide(...)` |')
    ..writeln(
      '| `async` | 调用点本身是 Future：异步派发，结果经事件回传 | `await decideAsync(...)` |',
    )
    ..writeln()
    ..writeln(
      '两种方式对插件处理器是**透明的**：处理器照常返回裁决对象即可；区别只在宿主侧'
      '（同步派发阻塞调用线程，异步派发不阻塞、结果经 `musicxx.hook.decision.result` 回传）。',
    )
    ..writeln(
      '异步派发的调用点在拿到结果前可能已经切歌/切列表，**插件要自己在载荷里带上身份字段'
      '（`sid` 等）并在调用点校验**，宿主只负责丢弃过期结果。',
    )
    ..writeln()
    ..writeln('## 裁决对象通用外壳')
    ..writeln()
    ..writeln('```jsonc')
    ..writeln('{ "action": "continue" | "skip" | "cancel" | "replace",')
    ..writeln('  "patch": { /* 钩子专属字段 */ },')
    ..writeln('  "error": "可选说明" }')
    ..writeln('```')
    ..writeln()
    ..writeln('- 返回 `null` / 空对象表示“不裁决”，交给下一个处理器；')
    ..writeln('- `action` 的宿主语义由各调用点决定（例如 `beforePlaySong` 的 `skip` 表示跳过本曲）；')
    ..writeln('- 处理器必须尽快返回：宿主对整链有等待预算，超时按“无裁决”继续（不打断插件）。')
    ..writeln()
    ..writeln('## 处理器失败与暂停派发')
    ..writeln()
    ..writeln(
      '- 处理器抛异常 / 返回失败只记日志与统计，**不影响其它处理器与插件**；',
    )
    ..writeln(
      '- 同一个处理器**连续 3 次失败**会被宿主临时暂停派发 60 秒（只暂停这一个处理器，'
      '同插件的其它处理器照常工作，插件也不会被卸载）；暂停状态与管理页里的剩余时间见 '
      '`hook_stats()` 的 `paused` / `pausedRemainMs`；',
    )
    ..writeln(
      '- 超过硬预算只是慢，不计失败（记 `musicxx.plugin.warn`，`code = handler_slow`）；',
    )
    ..writeln('- 派发期间注册/注销钩子不会破坏本轮遍历：宿主在派发前对处理器列表做快照。');
  return sb.toString();
}

/// 读定义文件并做基本校验
List<Map<String, Object?>> _loadHooks() {
  final File file = File(_defPath);
  if (!file.existsSync()) {
    stderr.writeln('找不到定义文件: $_defPath');
    exit(2);
  }
  final Map<String, Object?> def =
      jsonDecode(file.readAsStringSync()) as Map<String, Object?>;
  final List<Object?> raw = def['hooks']! as List<Object?>;
  final List<Map<String, Object?>> hooks = <Map<String, Object?>>[];
  final Set<String> ids = <String>{};
  final Set<String> dartNames = <String>{};
  for (final item in raw) {
    final Map<String, Object?> hook = item! as Map<String, Object?>;
    final String id = hook['id']! as String;
    if (!id.startsWith('musicxx.')) {
      throw StateError('钩子 id 必须以 musicxx. 开头: $id');
    }
    if (!ids.add(id)) {
      throw StateError('钩子 id 重复: $id');
    }
    if (!dartNames.add(hook['dart']! as String)) {
      throw StateError('Dart 枚举名重复: ${hook['dart']}');
    }
    final String mode = hook['mode']! as String;
    if (mode != 'observe' && mode != 'decision') {
      throw StateError('未知模式: $id -> $mode');
    }
    final String policy = hook['policy']! as String;
    if (!_policyCodes.containsKey(policy)) {
      throw StateError('未知合并策略: $id -> $policy');
    }
    final String dispatch = (hook['dispatch'] as String?) ?? 'sync';
    if (!_dispatchCodes.containsKey(dispatch)) {
      throw StateError('未知派发方式: $id -> $dispatch');
    }
    if (hook['wired'] is! bool) {
      throw StateError('wired 必须是布尔值（应用侧是否已经埋点）: $id');
    }
    if (mode == 'observe' && dispatch != 'async') {
      throw StateError('观察型钩子必须是异步派发（入队即返回）: $id -> $dispatch');
    }
    hooks.add(hook);
  }
  return hooks;
}

/// 写入（或校验）单个生成物
///
/// [ignoreFormatting] 给 Dart 生成物用：Dart 文件在提交前通常会被 `dart format`
/// 重排（换行/缩进/尾逗号），那是格式差异而不是内容差异，校验时按"去空白 + 去收尾逗号"
/// 比较；其它生成物（C++ 头、文档）按字节比较。
void _emit(
  String path,
  String content, {
  required bool check,
  required List<String> drift,
  bool ignoreFormatting = false,
}) {
  final File file = File(path);
  if (check) {
    final String existing = file.existsSync() ? file.readAsStringSync() : '';
    final bool same = ignoreFormatting
        ? _canonical(existing) == _canonical(content)
        : existing == content;
    if (!same) {
      drift.add(path);
    }
    return;
  }
  file.parent.createSync(recursive: true);
  file.writeAsStringSync(content);
  stdout.writeln('  生成: $path');
}

/// 去掉空白与"收尾逗号"（`dart format` 只会动这两类字符）
String _canonical(String text) => text
    .replaceAll(RegExp(r'\s+'), '')
    .replaceAll(RegExp(r',(?=[)\]};])'), '')
    .replaceAll(RegExp(r',$'), '');

void main(List<String> args) {
  final bool check = args.contains('--check');
  final List<Map<String, Object?>> hooks = _loadHooks();
  stdout.writeln(
    '${check ? '校验' : '生成'}契约: ${hooks.length} 个钩子 (来源 $_defPath)',
  );

  final List<String> drift = <String>[];
  _emit(
    _dartOut,
    _generateDart(hooks),
    check: check,
    drift: drift,
    ignoreFormatting: true,
  );
  _emit(_cppOut, _generateCpp(hooks), check: check, drift: drift);
  _emit(_docOut, _generateDoc(hooks), check: check, drift: drift);

  if (check && drift.isNotEmpty) {
    stderr.writeln('生成物与定义不一致（请运行 dart run tools/gen_contract.dart 后提交）:');
    for (final String path in drift) {
      stderr.writeln('  $path');
    }
    exit(1);
  }
  if (check) {
    stdout.writeln('生成物与定义一致');
  }
}
