/// 钩子清单生成器：把 `tools/hooks.def.json` 生成三份产物
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
/// | `src/sdk/include/musicxx/plugin/api/hook_ids.g.h` | C++ 侧钩子 id 常量 + 已知钩子表（插件框架与插件共用） |
/// | `docs/plugin-hooks.md` | 插件作者文档（钩子总表） |
///
/// 为什么是这三处：Dart 侧调用与派发要用枚举、原生插件框架要用表拒绝未知钩子、
/// 插件作者要有一份可查的清单。定义只有一处，避免常量不一致。
///
/// 说明：C++ 头放在 **SDK 目录** 而不是 `src/host/include/`，
/// 这样插件框架（`find_package` 后拿到的 SDK 头）与插件作者（`musicxx_extern_plugin_sdk`）
/// 都能直接包含同一份生成物，不需要维护两份。
library;

import 'dart:convert';
import 'dart:io';

const String _defPath = 'tools/hooks.def.json';
const String _dartOut = 'lib/src/hook_ids.g.dart';
const String _cppOut = 'src/sdk/include/musicxx/plugin/api/hook_ids.g.h';
const String _docOut = 'docs/plugin-hooks.md';

/// 合并策略名 → C++/Dart 两侧共用的整数编码（与插件框架 `dispatchHook` 的判据一致）
const Map<String, int> _policyCodes = <String, int>{
  'firstNonNull': 0,
  'anyCancel': 1,
  'allMerge': 2,
  'lastWrite': 3,
};

/// 裁决派发方式：`sync` = 调用点就地等待（`decide`）；`async` = 事件回传（`decideAsync`）
const Map<String, int> _dispatchCodes = <String, int>{'sync': 0, 'async': 1};

/// 阶段名 → 编号顺序（顺序即生成物里的固定次序：before → impl → after）
const List<String> _phaseOrder = <String>['before', 'impl', 'after'];

/// 阶段名 → 编号（`impl` 0 / `before` 1 / `after` 2；C ABI 的 `phase` 字段用它）
const Map<String, int> _phaseCodes = <String, int>{
  'impl': 0,
  'before': 1,
  'after': 2,
};

/// 阶段位掩码（`1 << 编号`；钩子元信息与 Dart 侧 `hasPhase` 用它）
int _phaseMask(String phase) => 1 << _phaseCodes[phase]!;

/// 阶段的中文说法（文档与日志用）
String _phaseText(String phase) {
  switch (phase) {
    case 'before':
      return "前链：进入调用之前改输入 / 停下";
    case 'impl':
      return "实现链：给出结果 / 裁决";
    case 'after':
      return "后链：拿到结果之后改输出 / 丢弃";
  }
  return phase;
}

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
    ..writeln('// 钩子 id 是跨边界稳定的约定：字符串值一旦发布不得修改，')
    ..writeln('// 只能新增或标记废弃。字段含义见 tools/hooks.def.json。')
    ..writeln('// ignore_for_file: type=lint, constant_identifier_names')
    ..writeln()
    ..writeln('/// 钩子模式（与 C++ `MUSICXX_PLUGIN_HOOK_MODE_*` 一致）')
    ..writeln('enum MusicxxPluginHookMode {')
    ..writeln("  /// 观察型：不等待、不裁决（入队即返回）")
    ..writeln('  observe(0),')
    ..writeln()
    ..writeln("  /// 裁决型：等待处理器链结果")
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
    ..writeln('  /// 调用点就地等待裁决（`MusicxxPluginHooks.decide`；占用调用线程）')
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
    ..writeln('/// 参与方式（同一个钩子上的三个阶段）')
    ..writeln('///')
    ..writeln('/// 钩子 id 表示**一次调用**，参与方式由注册时的阶段表达：')
    ..writeln('/// - [before]：串行、**正序**（线头 → 线尾：extern → builtin → core），只能改输入或停下，')
    ..writeln('///   回答 `{"patch": {...}}` / `{"action": "stop"}`；')
    ..writeln('/// - [impl]：实现链，第一个合法结果生效（`firstNonNull`）或按 policy 合并；')
    ..writeln('/// - [after]：串行、**逆序**（线尾 → 线头：core → builtin → extern），只能改输出或丢弃，')
    ..writeln('///   回答 `{"value": ...}` / `{"action": "drop"}`。')
    ..writeln('///')
    ..writeln('/// 后链逆序是为了保住同一个直觉：外置插件**最后**改输出，应用自己的兜底实现')
    ..writeln('/// 不能盖掉插件的改写（与实现链里“外置优先”同向）。详见 `docs/plugin-hooks.md`。')
    ..writeln('enum MusicxxPluginHookPhase {')
    ..writeln('  /// 实现链：给出结果 / 裁决（不写阶段就是它）')
    ..writeln('  impl(0),')
    ..writeln()
    ..writeln('  /// 前链：进入调用之前改输入 / 停下（这一轮不执行实现）')
    ..writeln('  before(1),')
    ..writeln()
    ..writeln('  /// 后链：拿到结果之后改输出 / 丢弃（实现已经跑完）')
    ..writeln('  after(2);')
    ..writeln()
    ..writeln('  const MusicxxPluginHookPhase(this.code);')
    ..writeln()
    ..writeln('  /// 阶段编号（与 C ABI `MUSICXX_PLUGIN_HOOK_PHASE_*` 一致）')
    ..writeln('  final int code;')
    ..writeln()
    ..writeln('  /// 位掩码（`1 << code`；与 `MusicxxPluginHookId.phases` 同一套位）')
    ..writeln('  int get mask => 1 << code;')
    ..writeln()
    ..writeln('  /// 对外字符串（事件载荷、自省与文档用它）')
    ..writeln('  String get wireName {')
    ..writeln('    switch (this) {')
    ..writeln('      case MusicxxPluginHookPhase.impl:')
    ..writeln('        return "impl";')
    ..writeln('      case MusicxxPluginHookPhase.before:')
    ..writeln('        return "before";')
    ..writeln('      case MusicxxPluginHookPhase.after:')
    ..writeln('        return "after";')
    ..writeln('    }')
    ..writeln('  }')
    ..writeln()
    ..writeln('  /// 按对外字符串反查（未知名字返回 null）')
    ..writeln('  static MusicxxPluginHookPhase? tryFromName(String name) {')
    ..writeln('    switch (name) {')
    ..writeln('      case "impl":')
    ..writeln('        return MusicxxPluginHookPhase.impl;')
    ..writeln('      case "before":')
    ..writeln('        return MusicxxPluginHookPhase.before;')
    ..writeln('      case "after":')
    ..writeln('        return MusicxxPluginHookPhase.after;')
    ..writeln('    }')
    ..writeln('    return null;')
    ..writeln('  }')
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
    final int phases = _phasesMask(hook);
    sb.writeln(
      "  ${_dartEnumName(hook)}('${hook['id']}', MusicxxPluginHookMode.$mode, "
      'MusicxxPluginDecisionPolicy.$policy, $wired, '
      'MusicxxPluginHookDispatch.$dispatch, $phases),',
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
    ..writeln('    this.phases,')
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
    ..writeln('  /// 主程序是否已经接入这个钩子：true = 当前版本会派发；')
    ..writeln('  /// false = 约定已固定，但主程序还没有接入（注册不会报错，当前版本也不会触发）')
    ..writeln('  final bool wired;')
    ..writeln()
    ..writeln('  /// 派发方式（是否占用调用线程；见 `tools/hooks.def.json`）')
    ..writeln('  final MusicxxPluginHookDispatch dispatch;')
    ..writeln()
    ..writeln('  /// 这个钩子支持的阶段（位掩码：`1 << MusicxxPluginHookPhase.code`）')
    ..writeln('  ///')
    ..writeln('  /// 注册时用 [MusicxxPluginHookPhase] 表达参与方式：同一条钩子上可以分别注册')
    ..writeln('  /// 前链（改输入）、实现链（给结果）与后链（改输出）。声明了却没派发的阶段')
    ..writeln('  /// 等于给插件一个“注册成功但永远不触发”的坑，所以只声明真的会派发的。')
    ..writeln('  final int phases;')
    ..writeln()
    ..writeln('  /// 这个钩子有没有声明某个阶段（不在声明里的阶段，注册会被插件框架拒绝）')
    ..writeln(
      '  bool hasPhase(MusicxxPluginHookPhase phase) => '
      '(phases & phase.mask) != 0;',
    )
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
    ..writeln('  /// 按 id 反查（未知 id 返回 null；插件注册未知钩子会被插件框架拒绝）')
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
    ..writeln('/// C++ 侧钩子约定：id 常量 + 已知钩子表（插件框架据此拒绝未知钩子、按声明的')
    ..writeln('/// 模式/策略派发；插件按常量注册，避免手写字符串打错）。')
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
    ..writeln(
      '/* ==================== 阶段 (与 Dart 侧 MusicxxPluginHookPhase 一致) ==================== */',
    )
    ..writeln()
    ..writeln('/* 阶段编号：注册时用它表达参与方式 */')
    ..writeln('#define MUSICXX_PLUGIN_HOOK_PHASE_IMPL   0')
    ..writeln('#define MUSICXX_PLUGIN_HOOK_PHASE_BEFORE 1')
    ..writeln('#define MUSICXX_PLUGIN_HOOK_PHASE_AFTER  2')
    ..writeln()
    ..writeln('/* 阶段位掩码：钩子元信息的 phases 字段用它 (1 << 阶段编号) */')
    ..writeln('#define MUSICXX_PLUGIN_HOOK_PHASE_MASK_IMPL   (1u << 0)')
    ..writeln('#define MUSICXX_PLUGIN_HOOK_PHASE_MASK_BEFORE (1u << 1)')
    ..writeln('#define MUSICXX_PLUGIN_HOOK_PHASE_MASK_AFTER  (1u << 2)')
    ..writeln()
    ..writeln('/* 阶段编号 → 位掩码 (非法编号返回 0) */')
    ..writeln('static inline uint32_t musicxxPluginHookPhaseMask(int32_t phase) {')
    ..writeln('    return (phase >= 0 && phase <= 2) ? (1u << phase) : 0u;')
    ..writeln('}')
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
    ..writeln('/// 已知钩子条目: id / 模式 / 合并策略 / 支持的阶段')
    ..writeln('struct MusicxxPluginHookMeta {')
    ..writeln('    const char* id;')
    ..writeln(
      '    int32_t     mode;     ///< 0 观察型 / 1 裁决型 (MUSICXX_PLUGIN_HOOK_MODE_*)',
    )
    ..writeln('    int32_t     policy;   ///< MUSICXX_PLUGIN_HOOK_POLICY_*')
    ..writeln(
      '    uint32_t    phases;   ///< MUSICXX_PLUGIN_HOOK_PHASE_MASK_* (支持哪几个阶段)',
    )
    ..writeln('};')
    ..writeln()
    ..writeln('/// 已知钩子表 (插件框架拒绝表外钩子: 插件不得制造插件框架不认识的钩子点)')
    ..writeln(
      'inline constexpr MusicxxPluginHookMeta musicxxPluginKnownHooks[] = {',
    );
  for (final Map<String, Object?> hook in hooks) {
    final int mode = _modeCode(hook['mode']! as String);
    final int policy = _policyCodes[hook['policy']]!;
    final int phases = _phasesMask(hook);
    sb.writeln("    {\"${hook['id']}\", $mode, $policy, ${phases}u},");
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
      '> **术语约定**：本文档中的**主程序**指的就是 **musicxx**（Flutter/Dart 业务层）；外置插件的 C++ 框架层称为**插件框架**；插件目前分为**动态库插件**与**JS插件**。',
    )
    ..writeln('>')
    ..writeln(
      '> 本文件由 `tools/gen_contract.dart` 从 `tools/hooks.def.json` 生成，请勿手改。',
    )
    ..writeln('>')
    ..writeln('> 注册钩子时必须写全名（官方 `musicxx.*`）；插件自定义事件/能力/UI 项用')
    ..writeln('> `plugin.<pluginId>.*`。插件框架会拒绝未知钩子与未知前缀。')
    ..writeln('>')
    ..writeln(
      '> 钩子怎么写（两种形态的注册写法、优先级、裁决对象、异步裁决、处理器里不该做的事）'
      '与未接入的钩子打算做什么，见 [plugin-api.md](plugin-api.md) §8。',
    )
    ..writeln()
    ..writeln('## 阶段：一个钩子 = 前链 + 实现链 + 后链')
    ..writeln()
    ..writeln(
      '**钩子 id 表示一次调用；参与方式（before / impl / after）由注册时的 `phase` 表达。**',
    )
    ..writeln('不再用 `.before` / `.after` 后缀造新 id（那样一条调用会变成好几条钩子）。')
    ..writeln()
    ..writeln('| 阶段 | 何时跑 | 能做什么 | 不能做什么 | 回答格式 | 结束整条链 |')
    ..writeln('|---|---|---|---|---|---|')
    ..writeln(
      '| `before` | 进入调用之前，串行、**正序** | 改输入（累加）、停下（这次不执行实现） '
      '| 给数据（那是实现链的事） | `{"patch": {...}}` | `{"action":"stop"}` |',
    )
    ..writeln(
      '| `impl` | 实现链，串行 | 给结果 / 裁决 | 改输入 / 改输出 | 与以前一样 | 按钩子声明的合并策略 |',
    )
    ..writeln(
      '| `after` | 拿到结果之后，串行、**逆序** | 改输出（累加）、丢弃（这次没有结果） '
      '| 造出本来没有的结果 | `{"value": <新结果>}` | `{"action":"drop"}` |',
    )
    ..writeln()
    ..writeln('**顺序**：一条钩子按 `(优先级升序, 注册顺序)` 排成一条链。')
    ..writeln()
    ..writeln('```text')
    ..writeln('线头（离调用方最近）                              线尾（离实现最近）')
    ..writeln('extern  ---------->  builtin  ---------->  core')
    ..writeln('   ^                                          |')
    ..writeln('   +---------------  后链（逆序） <------------+')
    ..writeln('```')
    ..writeln()
    ..writeln(
      '- **前链正序**：线头 → 线尾，每一步拿到**上一步改过**的输入；',
    )
    ..writeln(
      '- **后链逆序**：线尾 → 线头，每一步拿到**上一步改过**的输出。所以外置插件**最后**改输出，'
      '应用自己的兜底实现不能盖掉插件的改写（与实现链里“外置优先”同向）；',
    )
    ..writeln(
      '- 同一插件对同一个钩子可以分别注册三个阶段（同一个 `ownerTag` 互不覆盖）；'
      '`unregister` 按 `(hook, ownerTag)` 摘，一个 `ownerTag` 在所有阶段的登记一起摘掉；',
    )
    ..writeln(
      '- **阶段处理器必须是同步处理器**（要拿到这一步的结果才能跑下一步），注册异步处理器会被拒绝；',
    )
    ..writeln(
      '- 三种“没有结果”含义不同：实现链的 `{"disable": true}`（不再问别人）、'
      '前链的 `stop`（这次不执行实现）、后链的 `drop`（实现跑完但结果丢掉）。',
    )
    ..writeln()
    ..writeln('没有声明的阶段登记会被拒绝（日志写清“这个钩子没有 before 阶段”）。')
    ..writeln()
    ..writeln('## 怎么读这张表')
    ..writeln()
    ..writeln(
      '- **主程序是否已接入**：`已接入` = 主程序已经在调用点接上这个钩子（当前版本会派发，插件注册后会被调用）；'
      '`未接入` = 钩子约定已经定下来、主程序**还没有接**（注册不会报错，但当前版本不会触发）。'
      '想知道某个钩子此刻有没有处理器，看管理页「外部插件 → 调试」分页的钩子统计（`hooks` 段），'
      '或在插件里调 `musicxx.hooks.has(id)`（只反映自己注册没注册）。',
    )
    ..writeln(
      '- **模式**：`observe` = 只通知（返回值忽略、不等待）；`decision` = 可裁决，'
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
      '- **阶段**：这个钩子支持哪几个阶段（`impl` = 实现链；`before` = 前链，正序改输入；'
      '`after` = 后链，逆序改输出）。只列**真的会派发**的阶段 —— 注册了不派发的阶段'
      '等于“注册成功但永远不触发”。',
    )
    ..writeln(
      '- **耗时**：插件框架不设置等待超时（不因为处理器慢而中断派发、也不按耗时暂停某个处理器），'
      '处理器耗时只记进统计（管理页调试分页可看）。处理器请自己保持短小：它跑在插件框架线程上，'
      '慢就是别人一起等。',
    )
    ..writeln(
      '- **怎么写**：注册与注销、优先级与执行顺序、裁决对象外壳、异步裁决、自省，'
      '以及“处理器里不要等主程序动作”这类坑，见 `docs/plugin-api.md` §8。',
    )
    ..writeln()
    ..writeln(
      '| 钩子 id | 模式 | 阶段 | 派发 | 合并策略 | 主程序是否已接入 |',
    )
    ..writeln('|---|---|---|---|---|---|');
  for (final Map<String, Object?> hook in hooks) {
    final String dispatch = (hook['dispatch'] as String?) ?? 'sync';
    final String wiredText = hook['wired'] == true ? '已接入' : '未接入';
    sb.writeln(
      "| `${hook['id']}` | ${hook['mode']} | ${_phasesText(hook)} | $dispatch "
      '| ${hook['policy']} | $wiredText |',
    );
  }
  sb
    ..writeln()
    ..writeln('## 已接入钩子的载荷与裁决')
    ..writeln()
    ..writeln(
      '下面是主程序**已经接入**的钩子：处理器拿到的载荷字段与裁决语义都在这里。'
      '尚未接入的钩子只定下了 id / 模式 / 派发 / 合并策略，载荷字段在主程序接入时补齐'
      '（接入后会写进 `tools/hooks.def.json` 的 `doc` 字段并重新生成本文件）。',
    )
    ..writeln()
    ..writeln('> 载荷统一是 JSON 对象；不裁决时返回 `null`（JS）或把出参留空（C++）。')
    ..writeln(
      '> 载荷里不放音频直链与 token：需要地址时请用 `musicxx.net` / 主程序动作自行获取。',
    );
  for (final Map<String, Object?> hook in hooks) {
    final String doc = (hook['doc'] as String?) ?? '';
    if (doc.isEmpty) {
      continue;
    }
    final String dispatch = (hook['dispatch'] as String?) ?? 'sync';
    sb
      ..writeln()
      ..writeln('### `${hook['id']}`')
      ..writeln()
      ..writeln(
        '- 模式：`${hook['mode']}`；阶段：`${_phasesText(hook)}`；派发：`$dispatch`；'
        '合并策略：`${hook['policy']}`',
      )
      ..writeln('- $doc');
  }
  sb
    ..writeln()
    ..writeln('## 派发方式')
    ..writeln()
    ..writeln('| 派发 | 含义 | 调用点写法 |')
    ..writeln('|---|---|---|')
    ..writeln('| `sync` | 调用点就地等待裁决（占用调用线程） | `decide(...)` |')
    ..writeln(
      '| `async` | 调用点本身是 Future：异步派发，结果经事件回传 | `await decideAsync(...)` |',
    )
    ..writeln()
    ..writeln(
      '两种方式对插件处理器是**透明的**：处理器照常返回裁决对象即可；区别只在插件框架侧'
      '（同步派发阻塞调用线程，异步派发不阻塞、结果经 `musicxx.hook.decision.result` 回传）。',
    )
    ..writeln(
      '异步派发的调用点在拿到结果前可能已经切歌/切列表，**插件要自己在载荷里带上身份字段'
      '（`sid` 等）并在调用点校验**，插件框架只负责丢弃过期结果。',
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
    ..writeln('- `action` 的主程序语义由各调用点决定（例如 `beforePlaySong` 的 `skip` 表示跳过本曲）；')
    ..writeln('- 处理器必须尽快返回：它跑在插件框架线程上，慢就是业务与其它插件一起等（插件框架不设置等待超时、也不会强行中断）。')
    ..writeln()
    ..writeln('## 处理器失败')
    ..writeln()
    ..writeln(
      '- 处理器抛异常 / 返回失败只记日志与统计，**不影响其它处理器与插件**（插件框架不会因为失败次数暂停派发、也不会卸载插件）；',
    )
    ..writeln('- 处理器耗时只记统计（管理页「外部插件 → 调试」分页可看单处理器平均/最大耗时）；')
    ..writeln('- 派发期间注册/注销钩子不会破坏本轮遍历：插件框架在派发前对处理器列表做快照。');
  return sb.toString();
}

/// 一个钩子声明的阶段（已归一为 before → impl → after 的固定次序；缺省 `["impl"]`）
List<String> _phasesOf(Map<String, Object?> hook) {
  final Object? raw = hook['phases'];
  if (raw is! List) {
    return const <String>['impl'];
  }
  return raw.map((Object? item) => "$item").toList(growable: false);
}

/// 阶段位掩码（`1 << 阶段编号`）
int _phasesMask(Map<String, Object?> hook) {
  int mask = 0;
  for (final String phase in _phasesOf(hook)) {
    mask |= _phaseMask(phase);
  }
  return mask;
}

/// 阶段列表的文本写法（`impl` / `before+impl` / `before+after` …）
String _phasesText(Map<String, Object?> hook) =>
    _phasesOf(hook).join('+');

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
      throw StateError('wired 必须是布尔值（主程序是否已经接入）: $id');
    }
    if (mode == 'observe' && dispatch != 'async') {
      throw StateError('观察型钩子必须是异步派发（入队即返回）: $id -> $dispatch');
    }
    // 阶段：不写就是 `["impl"]`；写了就按声明顺序校验后归一成 before → impl → after
    hook['phases'] = _normalizePhases(hook, id, mode);
    hooks.add(hook);
  }
  return hooks;
}

/// 校验并归一 `phases`
///
/// 规则（与设计文档 §5.2 一致）：
/// - 不写 = `["impl"]`（现有条目不用一条条补）；
/// - 只能写 `before` / `impl` / `after`，不能重复；
/// - `mode: observe` 只能是 `["impl"]`（通知型是链的末端，没有前后链）；
/// - 允许没有 `impl`（包装点：实现固定是应用自己），但要显式列出来；
/// - 生成物里统一成 `["before", "impl", "after"]` 的固定次序（与声明顺序无关）。
List<String> _normalizePhases(
  Map<String, Object?> hook,
  String id,
  String mode,
) {
  final Object? raw = hook['phases'];
  final List<String> declared = <String>[];
  if (raw == null) {
    declared.add('impl');
  } else {
    if (raw is! List || raw.isEmpty) {
      throw StateError('phases 必须是非空字符串数组: $id');
    }
    for (final Object? item in raw) {
      if (item is! String || !_phaseCodes.containsKey(item)) {
        throw StateError('未知阶段: $id -> $item（只能是 before / impl / after）');
      }
      if (declared.contains(item)) {
        throw StateError('阶段重复: $id -> $item');
      }
      declared.add(item);
    }
  }
  if (mode == 'observe' &&
      (declared.length != 1 || declared.first != 'impl')) {
    throw StateError(
      '观察型钩子只能是 ["impl"]（通知型没有前后链）: $id -> [${declared.join(", ")}]',
    );
  }
  final List<String> normalized = <String>[
    for (final String phase in _phaseOrder)
      if (declared.contains(phase)) phase,
  ];
  return normalized;
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
    '${check ? '校验' : '生成'}钩子约定: ${hooks.length} 个钩子 (来源 $_defPath)',
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
