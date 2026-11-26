/// 能力与广播清单生成器（由 `tools/gen_contract.dart` 调用）
///
/// 定义源：`tools/capabilities.def.json`
///
/// 产物：
/// | 产物 | 用途 |
/// |---|---|
/// | `lib/src/capability_ids.g.dart` | Dart 侧 id 常量 + 声明（impls / merge / phases / dispatch / timeout / cache / 三个通道块） |
/// | `src/sdk/include/musicxx/plugin/api/capability_ids.g.h` | C++ 侧 id 常量 + 已知声明表（插件框架据此拒绝未知 id、判阶段与合并策略） |
/// | `docs/plugin-capabilities.md` | 能力（请求 → 结果）插件开发文档 |
/// | `docs/plugin-broadcasts.md` | 广播（只通知，不回答）插件开发文档 |
///
/// 为什么只有一个定义源：同一个 id 现在同时出现在"派发、命令面、MCP、链接入口、自省、
/// 插件开发文档"几处，靠手写常量加断言对齐迟早会漂；生成物把 id 与声明字段一起带出去。
///
/// 这份文件只做装配：产物路径、定义文件位置，以及把生成核心
/// （`contract/contract_core.dart`）与文档措辞（`contract/musicxx_docs.dart`）拼起来。
/// 校验规则见 [loadContract]。
///
/// - 生成物是提交物：改完定义文件必须跑 `dart run tools/gen_contract.dart` 并提交。
library;

import 'contract/contract_core.dart';
import 'contract/musicxx_docs.dart';

export 'contract/contract_core.dart';

/// 定义文件（相对包根目录）
const String capabilitiesDefPath = 'tools/capabilities.def.json';

/// Dart 生成物
const String capabilityDartOut = 'lib/src/capability_ids.g.dart';

/// C++ 生成物
const String capabilityCppOut =
    'src/sdk/include/musicxx/plugin/api/capability_ids.g.h';

/// 能力总表（插件开发文档）
const String capabilityDocOut = 'docs/plugin-capabilities.md';

/// 广播总表（插件开发文档）
const String broadcastDocOut = 'docs/plugin-broadcasts.md';

/* ==================== 对外入口 ==================== */

/// 生成全部产物（path → 内容），并做完整校验
Map<String, String> buildCapabilityContract({
  String defPath = capabilitiesDefPath,
}) {
  final CapabilityContract contract = loadContract(defPath);
  return <String, String>{
    capabilityDartOut: generateDartIds(contract),
    capabilityCppOut: generateCppIds(contract),
    capabilityDocOut: generateCapabilityDoc(contract),
    broadcastDocOut: generateBroadcastDoc(contract),
  };
}
