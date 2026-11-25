///
/// 覆盖：插件用**短名**声明自己的槽位（`musicxx.ui.slot.define({id: "toolbar"})`）
///
/// 插件文档（`docs/plugin-ui.md` §1.10、`docs/ui-slots.md`）与配套的两个示例写的都是短名，
/// 名字落在 `plugin.<自己>.ui.<名字>`；插件框架按前缀判断槽位归属，所以 JS 这一层必须把
/// 短名补成自己的命名空间。原样把短名发给框架会被当成越权，往这个槽位注册内容也会因为
/// "槽位未知"被拒 —— 表现是插件加载后状态上挂着一条"界面操作被拒绝"。
///
/// 前置：先跑 `tools/build_examples.ps1`（Windows）或 `tools/build_examples.sh`（Linux/macOS）。
/// 找不到原生库或示例插件时跳过而不是失败。
library;

import 'dart:async';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:musicxx_extern_plugin/musicxx_extern_plugin.dart';

import 'ui_content_probe.dart';

const String _providerId = 'example_js_slot_provider';
const String _consumerId = 'example_js_slot_consumer';
const String _slotId = 'plugin.example_js_slot_provider.ui.toolbar';

void main() {
  final _Environment? env = _Environment.detect();

  test('插件用短名声明自己的槽位（provider / consumer 示例）', () async {
    if (env == null) {
      markTestSkipped('未找到原生插件框架库，跳过（先跑 tools/build_examples.ps1）');
      return;
    }
    if (!env.hasPlugin(_providerId) || !env.hasPlugin(_consumerId)) {
      markTestSkipped('没有构建 $_providerId / $_consumerId，跳过');
      return;
    }

    final MusicxxPluginRuntime runtime = MusicxxPluginRuntime.instance;
    addTearDown(runtime.dispose);
    final List<MusicxxPluginEvent> events = <MusicxxPluginEvent>[];
    final StreamSubscription<MusicxxPluginEvent> sub = runtime.events.listen(
      events.add,
    );
    addTearDown(sub.cancel);

    await runtime.init(
      config: MusicxxPluginRuntimeConfig(
        appVersion: '0.0.0-test',
        platform: MusicxxPluginRuntime.currentPlatform,
        language: 'zh-cn',
        userPluginDir: env.pluginRoot,
        observeEvents: true,
      ),
      libraryPath: env.libraryPath,
    );
    await _pumpUntil(
      () => events.any(
        (MusicxxPluginEvent event) =>
            event.type == MusicxxPluginEventType.hostReady,
      ),
    );

    await runtime.plugins.load(_providerId);
    await runtime.plugins.load(_consumerId);

    // 1) 槽位声明成功：短名被补成 `plugin.<自己>.ui.<名字>`，归属是本插件
    final List<MusicxxPluginUISlot> own = (await runtime.plugins.uiSlots())
        .where((MusicxxPluginUISlot slot) => slot.id == _slotId)
        .toList();
    expect(own, hasLength(1), reason: '槽位 $_slotId 没有声明成功');
    expect(own.single.kind, 'insert');
    expect(own.single.owner, _providerId);
    expect(own.single.isPluginDeclared, isTrue);

    // 2) 提供方的内容挂在它自己的槽位上
    final UiContentProbe probe = UiContentProbe(runtime, events);
    final List<MusicxxPluginUIContent> contents = await probe.registrationsOf(
      _providerId,
      slot: _slotId,
    );
    expect(contents, hasLength(1), reason: '提供方的 hint 没有注册到自己的槽位上');
    expect(contents.single.id, 'plugin.$_providerId.hint');

    // 3) 内容生成得出来（块树里有文本与按钮），外层传的参数也确实传进去了
    final Map<String, Object?> hintJson = await probe.jsonOf(
      'plugin.$_providerId.hint',
    );
    expect(hintJson['blocks'], isA<List<Object?>>());
    final Map<String, Object?> compactJson = await probe.jsonOf(
      'plugin.$_providerId.hint',
      context: <String, Object?>{
        'scope:musicxx.slot.args': <String, Object?>{'compact': true},
      },
    );
    expect(
      _textsOf(compactJson).join('|'),
      contains('紧凑模式'),
      reason: 'scope:musicxx.slot.args 没有传给提供方的内容',
    );

    // 4) 使用方的页面把提供方的槽位嵌了进去
    final Map<String, Object?> cardJson = await probe.jsonOf(
      'plugin.$_consumerId.card',
    );
    expect(
      _slotRefsOf(cardJson),
      contains(_slotId),
      reason: '使用方的说明页里没有嵌 $_slotId',
    );

    // 5) 插件自检：声明确实生效（短名声明失败时这两项都是 0）
    final Object? rawProbe = await runtime.plugins.call(
      _providerId,
      'plugin.$_providerId.probe',
      const <String, Object?>{},
    );
    expect(rawProbe, isA<Map<String, Object?>>());
    final Map<String, Object?> flags = rawProbe! as Map<String, Object?>;
    expect(flags['declared'], 1, reason: '插件自己看不到声明的槽位');
    expect(flags['contents'], 1, reason: '插件自己看不到注册的内容');

    await runtime.plugins.unload(_consumerId);
    await runtime.plugins.unload(_providerId);
  });
}

Future<void> _pumpUntil(
  FutureOr<bool> Function() condition, {
  Duration timeout = const Duration(seconds: 5),
}) async {
  final DateTime deadline = DateTime.now().add(timeout);
  final MusicxxPluginRuntime runtime = MusicxxPluginRuntime.instance;
  while (!await condition()) {
    if (DateTime.now().isAfter(deadline)) {
      return;
    }
    runtime.pumpEvents();
    await Future<void>.delayed(const Duration(milliseconds: 20));
  }
}

/// 递归收集块树里的文本（断言"这一页里有没有某段文案"用）
List<String> _textsOf(Object? node) {
  final List<String> out = <String>[];
  void walk(Object? value) {
    if (value is String) {
      out.add(value);
      return;
    }
    if (value is Map) {
      for (final Object? item in value.values) {
        walk(item);
      }
      return;
    }
    if (value is Iterable) {
      for (final Object? item in value) {
        walk(item);
      }
    }
  }

  walk(node);
  return out;
}

/// 块树里所有 `UISlot` 块指向的槽位 id
List<String> _slotRefsOf(Object? node) {
  final List<String> out = <String>[];
  void walk(Object? value) {
    if (value is Map) {
      if (value['kind'] == 'UISlot' && value['slot'] is String) {
        out.add(value['slot']! as String);
      }
      for (final Object? item in value.values) {
        walk(item);
      }
      return;
    }
    if (value is Iterable) {
      for (final Object? item in value) {
        walk(item);
      }
    }
  }

  walk(node);
  return out;
}

/// 原生库与示例插件的产物目录（与 host_test.dart 同一套探测规则）
class _Environment {
  const _Environment(this.libraryPath, this.pluginRoot);

  final String libraryPath;
  final String pluginRoot;

  bool hasPlugin(String id) => Directory('$pluginRoot/$id').existsSync();

  static _Environment? detect() {
    String? library;
    for (final String candidate
        in MusicxxPluginNativeLibrary.defaultCandidates()) {
      if (File(candidate).existsSync()) {
        library = candidate;
        break;
      }
    }
    if (library == null) {
      return null;
    }
    final Directory binDir = File(library).parent;
    for (final String root in <String>[
      '${binDir.parent.path}/plugins',
      '${binDir.path}/plugins',
    ]) {
      if (Directory(root).existsSync()) {
        return _Environment(library, root);
      }
    }
    return null;
  }
}
