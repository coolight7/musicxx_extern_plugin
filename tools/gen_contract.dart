/// 能力 / 广播契约生成器
///
/// 用法（在包目录下）：
/// ```
/// dart run tools/gen_contract.dart            # 生成
/// dart run tools/gen_contract.dart --check    # 只校验（生成物与定义一致时退出码 0）
/// ```
///
/// 声明源只有一处：`tools/capabilities.def.json`（`capabilities` 与 `broadcasts` 两段）。
///
/// 产物：
/// | 产物 | 用途 |
/// |---|---|
/// | `lib/src/capability_ids.g.dart` | Dart 侧 id 常量 + 声明 + 阶段 / 合并策略枚举 |
/// | `src/sdk/include/musicxx/plugin/api/capability_ids.g.h` | C++ 侧 id 常量 + 声明表（插件框架据此拒绝未知 id、判阶段与合并策略） |
/// | `docs/plugin-capabilities.md` | 能力（请求 → 结果）作者文档 |
/// | `docs/plugin-broadcasts.md` | 广播（通知 / 快照）作者文档 |
///
/// 槽位与变量清单由 `tools/gen_ui_contract.dart` 生成（同样是提交物）。
///
/// 为什么只有一处：同一个 id 同时出现在"派发、命令面、MCP、链接入口、自省、作者文档"几处，
/// 靠手写常量对齐迟早会漂。原来还有一份 `tools/hooks.def.json`（只写派发层用到的模式 / 合并策略 /
/// 阶段 / 是否接入），它与这里的声明逐条重复，已删除：
/// - 模式（通知型 / 裁决型）= 这个 id 是不是广播频道（`broadcasts` 里有就是通知型）；
/// - 合并策略 = 声明里的 `merge`；阶段 = `phases`；是否接入 = `implemented`。
///
/// 生成物是提交物：改完定义文件必须重新生成并提交。
library;

import 'dart:io';

import 'gen_capabilities.dart';
import 'gen_ui_contract.dart';

/// 去掉空白与"收尾逗号"（`dart format` 只会动这两类字符）
String _canonical(String text) => text
    .replaceAll(RegExp(r'\s+'), '')
    .replaceAll(RegExp(r',(?=[)\]};])'), '')
    .replaceAll(RegExp(r',$'), '');

/// 两份文本第一处不同的说明（用于 `--check` 报出"是哪一条"）
///
/// 按行比：报第一处不同的行，两边都给出去掉首尾空白后的内容（过长截断）。
/// 只差空白或收尾逗号时单独说一句，避免把格式差异当成内容差异。
String _firstDifference(String existing, String wanted) {
  final List<String> before = existing.split('\n');
  final List<String> after = wanted.split('\n');
  final int shared = before.length < after.length
      ? before.length
      : after.length;
  int index = 0;
  while (index < shared && before[index] == after[index]) {
    index += 1;
  }
  if (index == shared && before.length == after.length) {
    return '   （逐行相同，仅空白或收尾逗号不同）';
  }
  String clip(String text) {
    final String flat = text.trim();
    return flat.length <= 120 ? flat : '${flat.substring(0, 120)}…';
  }

  final StringBuffer sb = StringBuffer()..writeln('   第一处不同在第 ${index + 1} 行:');
  if (index < before.length) {
    sb.writeln('   - ${clip(before[index])}');
  }
  if (index < after.length) {
    sb.writeln('   + ${clip(after[index])}');
  }
  return sb.toString().trimRight();
}

void main(List<String> args) {
  final bool check = args.contains('--check');
  final Map<String, String> files = <String, String>{
    ...buildCapabilityContract(),
    ...buildUiContract(),
  };
  stdout.writeln(
    '${check ? '校验' : '生成'}能力 / 广播 / 槽位 / 变量约定: '
    '来源 $capabilitiesDefPath、tools/ui_slots.def.json、tools/ui_variables.def.json',
  );

  final List<String> drift = <String>[];
  final List<String> details = <String>[];
  files.forEach((String path, String content) {
    final File file = File(path);
    if (check) {
      final String existing = file.existsSync() ? file.readAsStringSync() : '';
      // Dart 生成物提交前可能被 `dart format` 重排，那是格式差异不是内容差异
      final bool same = path.endsWith('.g.dart')
          ? _canonical(existing) == _canonical(content)
          : existing == content;
      if (!same) {
        drift.add(path);
        details.add(_firstDifference(existing, content));
      }
      return;
    }
    file.parent.createSync(recursive: true);
    file.writeAsStringSync(content);
    stdout.writeln('  生成: $path');
  });

  if (check && drift.isNotEmpty) {
    stderr.writeln('生成物与定义不一致（请运行 dart run tools/gen_contract.dart 后提交）:');
    for (int i = 0; i < drift.length; i += 1) {
      stderr.writeln('  ${drift[i]}');
      stderr.writeln(details[i]);
    }
    exit(1);
  }
  if (check) {
    stdout.writeln('生成物与定义一致');
  }
}
