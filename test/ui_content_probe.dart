// 取插件内容的测试辅助
//
// 主程序里内容由 `ContentHub` 缓存并按需向插件要（包的 `MusicxxUiContentTransport`）；
// 测试没有那层缓存，所以直接向插件要一次，再从 `musicxx.ui.content` 事件里取生成结果。
//
// 用法：先订阅 `runtime.events`（没人听时框架送来的事件会被丢掉），
// 再用同一份事件列表构造本对象。
import 'package:flutter_test/flutter_test.dart';
import 'package:musicxx_extern_plugin/musicxx_extern_plugin.dart';

/// 一份内容的生成结果（`musicxx.ui.content` 事件的载荷）
class UiContentResult {
  const UiContentResult(this.payload);

  /// 原始载荷：`{slot, id, shortId, plugin, rev, json?, data?, error?}`
  final Map<String, Object?> payload;

  /// 内容 JSON（块树 / 文档）
  Map<String, Object?> get json {
    final Object? value = payload['json'];
    expect(
      value,
      isA<Map<String, Object?>>(),
      reason: '内容 $id 没有 json: $payload',
    );
    return value! as Map<String, Object?>;
  }

  /// 内容的 `data` 段（生成函数没给时为空）
  Map<String, Object?> get data {
    final Object? value = payload['data'];
    if (value is! Map) {
      return const <String, Object?>{};
    }
    return value.cast<String, Object?>();
  }

  /// 内容全名 `plugin.<插件id>.<短 id>`
  String get id => '${payload['id']}';

  /// 生成失败的原因（空 = 成功）
  String get error =>
      payload['error'] is String ? payload['error']! as String : '';

  @override
  String toString() => 'UiContentResult($id, error=$error)';
}

/// 向插件要内容并取回生成结果
class UiContentProbe {
  UiContentProbe(this.runtime, this.events);

  final MusicxxPluginRuntime runtime;

  /// 事件列表（调用方持有：`runtime.events.listen(events.add)`）
  final List<MusicxxPluginEvent> events;

  /// 要一份内容。[contentId] 是全名 `plugin.<插件id>.<短 id>`
  ///
  /// 生成是同步受理的（原生插件当场生成，JS 插件随后生成），结果经事件泵送到，
  /// 所以这里一边泵事件一边等。
  Future<UiContentResult> request(
    String contentId, {
    Map<String, Object?> context = const <String, Object?>{},
    Duration timeout = const Duration(seconds: 5),
  }) async {
    final int start = events.length;
    await runtime.plugins.requestUiContent(contentId, context);
    final DateTime deadline = DateTime.now().add(timeout);
    while (true) {
      for (int i = start; i < events.length; ++i) {
        final MusicxxPluginEvent event = events[i];
        if (event.type != MusicxxPluginEventType.uiContent ||
            event.stringOf('id') != contentId) {
          continue;
        }
        // 同一份内容也会推出"注册信息变了"这类不带生成结果的载荷：只认生成结果
        final Map<String, Object?> payload = event.payload;
        if (!payload.containsKey('json') &&
            !payload.containsKey('data') &&
            !payload.containsKey('error')) {
          continue;
        }
        final UiContentResult result = UiContentResult(payload);
        expect(result.error, isEmpty, reason: '内容 $contentId 生成失败');
        return result;
      }
      if (DateTime.now().isAfter(deadline)) {
        fail('等待内容 $contentId 超时（已泵 $timeout）');
      }
      runtime.pumpEvents();
      await Future<void>.delayed(const Duration(milliseconds: 20));
    }
  }

  /// 要一份内容的 JSON（块树 / 文档）
  Future<Map<String, Object?>> jsonOf(
    String contentId, {
    Map<String, Object?> context = const <String, Object?>{},
  }) async => (await request(contentId, context: context)).json;

  /// 内容注册表快照里属于 [plugin] 的内容（可按槽位过滤）
  Future<List<MusicxxPluginUIContent>> registrationsOf(
    String plugin, {
    String? slot,
  }) async {
    return <MusicxxPluginUIContent>[
      for (final MusicxxPluginUIContent item
          in await runtime.plugins.uiContents())
        if (item.plugin == plugin && (slot == null || item.slot == slot)) item,
    ];
  }
}
