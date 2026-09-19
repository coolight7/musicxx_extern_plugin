/// 冒烟测试：Dart 侧运行时对**真实原生宿主库**的端到端验证
///
/// 覆盖（对应 plan §13.2/§13.3 的首批用例）：
/// - 库加载与 C ABI 版本校验、宿主 init/dispose 幂等；
/// - 事件泵（host.ready / plugin.loaded 等按序到达）；
/// - 插件扫描/装载/能力调用/卸载；
/// - 钩子派发：Dart 处理器链 + 原生处理器链 + 合并，以及"无处理器"快速路径；
/// - 状态镜像推送（插件侧同步可读）。
///
/// 前置：先跑 `pwsh tools/build_native.ps1`（产出 `.native/output/<平台>-<配置>/`）。
/// 若找不到原生库或示例插件，测试会跳过而不是失败（CI 里可以先构建再跑）。
library;

import 'dart:async';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:musicxx_extern_plugin/musicxx_extern_plugin.dart';

void main() {
  final _Environment? env = _Environment.detect();

  test('原生宿主冒烟：init/事件泵/扫描/装载/钩子/能力/卸载', () async {
    if (env == null) {
      markTestSkipped('未找到原生宿主库或示例插件，跳过（先运行 tools/build_native.ps1）');
      return;
    }

    _step('0 开始');
    final MusicxxPluginRuntime runtime = MusicxxPluginRuntime.instance;
    addTearDown(runtime.dispose);

    final List<MusicxxPluginEvent> seen = <MusicxxPluginEvent>[];
    final StreamSubscription<MusicxxPluginEvent> sub = runtime.events.listen(seen.add);
    addTearDown(sub.cancel);

    expect(runtime.isRunning, isFalse);
    runtime.init(
      config: MusicxxPluginRuntimeConfig(
        appVersion: '0.0.0-test',
        platform: MusicxxPluginRuntime.currentPlatform,
        language: 'zh-cn',
        userPluginDir: env.pluginRoot,
        observeEvents: true,
      ),
      libraryPath: env.libraryPath,
    );
    _step('1 init 完成');
    expect(runtime.isRunning, isTrue);
    expect(runtime.libraryPath, env.libraryPath);

    // 事件泵：host.ready 一定在队列里（init 里已 pump 过一次）
    await _pumpUntil(() => seen.any((MusicxxPluginEvent e) => e.type == MusicxxPluginEventType.hostReady));
    final MusicxxPluginEvent ready =
        seen.firstWhere((MusicxxPluginEvent e) => e.type == MusicxxPluginEventType.hostReady);
    expect(ready.payload['apiVersion'], 1);
    expect(ready.payload['platform'], MusicxxPluginRuntime.currentPlatform);

    _step('2 host.ready 已收到');
    // 扫描：应发现示例插件
    final List<MusicxxPluginInfo> found = runtime.plugins.scan();
    final MusicxxPluginInfo? example =
        found.where((MusicxxPluginInfo info) => info.id == 'example_native').firstOrNull;
    expect(example, isNotNull, reason: '扫描结果: ${found.map((e) => e.id).toList()}');
    expect(example!.valid, isTrue);
    expect(example.supported, isTrue, reason: example.reason);
    expect(example.kind, MusicxxPluginKind.native);

    _step('3 scan 完成: ${found.length} 项');
    // 装载：同步等待 + 事件回报
    runtime.plugins.load('example_native');
    expect(runtime.plugins.findLoaded('example_native'), isNotNull);
    await _pumpUntil(() => seen.any((MusicxxPluginEvent e) => e.type == MusicxxPluginEventType.pluginLoaded));

    _step('4 load 完成');
    // 原生处理器位图：插件在 start 里注册了 3 个钩子
    expect(runtime.hooks.refreshNativeHandlerCount(MusicxxPluginHookId.playerBeforePlaySong), 1);
    expect(runtime.hooks.hasHandlers(MusicxxPluginHookId.playerBeforePlaySong), isTrue);
    expect(runtime.hooks.hasHandlers(MusicxxPluginHookId.playerPosition), isFalse);

    _step('5 hook_count 完成');
    // 状态镜像：Dart 推送 → 插件同步可读（探针里回报 stateLen）
    runtime.state.update(
      MusicxxPluginState.keySong,
      <String, Object?>{'sid': 's-dart', 'name': 'Dart 推送的歌曲'},
    );

    _step('6 state 推送完成');
    // 能力调用：读取插件自检信息（线程归属 + 命名空间校验结果）
    final Object? probeRaw =
        runtime.plugins.call('example_native', 'plugin.example_native.probe', const <String, Object?>{});
    expect(probeRaw, isA<Map<String, Object?>>());
    final Map<String, Object?> probe = probeRaw! as Map<String, Object?>;
    expect(probe['startThread'], equals(probe['callThread']), reason: '插件代码必须只在宿主线程执行');
    expect(probe['unknownHookRc'], -4, reason: '未知前缀钩子必须被拒绝');
    expect(probe['foreignActionRc'], -6, reason: '他人命名空间动作必须被拒绝');
    expect(probe['stateLen'], greaterThan(0), reason: '插件应能同步读到 Dart 推送的状态镜像');

    _step('7 能力调用完成: $probeRaw');
    // 钩子派发（裁决型）：广告曲目 → skip
    final Map<String, Object?>? verdict = runtime.hooks.decide(
      MusicxxPluginHookId.playerBeforePlaySong,
      <String, Object?>{'sid': 's-ad', 'song': <String, Object?>{'name': '广告插曲 - 测试'}},
    );
    expect(verdict, isNotNull);
    expect(verdict!['action'], 'skip');
    // 普通曲目 → 无裁决
    expect(
      runtime.hooks.decide(
        MusicxxPluginHookId.playerBeforePlaySong,
        <String, Object?>{'sid': 's-normal', 'song': <String, Object?>{'name': '普通歌曲'}},
      ),
      isNull,
    );

    _step('8 decide 完成');
    // Dart 处理器链 + 合并：Dart 侧给 patch，原生侧给 skip（anyCancel 最保守 → 保留 skip）
    Map<String, Object?> dartHandler(MusicxxPluginHookContext context) =>
        <String, Object?>{'action': 'continue', 'patch': <String, Object?>{'quality': 'hires'}};
    runtime.hooks.on(MusicxxPluginHookId.playerBeforePlaySong, dartHandler);
    expect(runtime.hooks.hasDartHandler(MusicxxPluginHookId.playerBeforePlaySong), isTrue);
    final Map<String, Object?>? merged = runtime.hooks.decide(
      MusicxxPluginHookId.playerBeforePlaySong,
      <String, Object?>{'sid': 's-ad2', 'song': <String, Object?>{'name': '广告曲目 2'}},
    );
    expect(merged!['action'], 'skip', reason: 'anyCancel：cancel/skip 不被后续覆盖');
    runtime.hooks.off(MusicxxPluginHookId.playerBeforePlaySong, dartHandler);
    expect(runtime.hooks.hasDartHandler(MusicxxPluginHookId.playerBeforePlaySong), isFalse);

    // 观察型钩子：入队即返回（不等待）
    runtime.hooks.observe(
      MusicxxPluginHookId.songChanged,
      <String, Object?>{'sid': 's-ad', 'song': <String, Object?>{'name': '普通歌曲'}},
    );
    await _pumpUntil(() => runtime.recentEvents.isNotEmpty);

    _step('9 Dart 处理器链完成');
    // 禁用 → 原生处理器位图清零；启用 → 恢复
    runtime.plugins.disable('example_native');
    expect(runtime.hooks.refreshNativeHandlerCount(MusicxxPluginHookId.playerBeforePlaySong), 0);
    runtime.plugins.enable('example_native');
    expect(runtime.hooks.refreshNativeHandlerCount(MusicxxPluginHookId.playerBeforePlaySong), 1);

    _step('10 enable/disable 完成');
    // 卸载：幂等（第二次仍成功），且能力调用失败
    runtime.plugins.unload('example_native');
    runtime.plugins.unload('example_native');
    expect(runtime.plugins.findLoaded('example_native'), isNull);
    expect(
      () => runtime.plugins.call('example_native', 'plugin.example_native.probe'),
      throwsA(isA<MusicxxPluginApiException>()),
    );

    _step('11 unload 完成');
    // 统计快照可读
    final Map<String, Object?> stats = runtime.plugins.stats();
    expect(stats['host'], isA<Map<String, Object?>>());
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('库缺失时抛出可诊断异常', () {
    expect(
      () => MusicxxPluginNativeLibrary.open(path: 'definitely/missing/musicxx.dll'),
      throwsA(isA<MusicxxPluginLibraryException>()),
    );
  });
}

/// 测试进度输出（定位卡死步骤用）
void _step(String message) {
  // ignore: avoid_print
  print('[smoke] $message');
}

/// 原生库 + 示例插件目录探测（找不到就跳过需要它们的用例）
class _Environment {
  const _Environment(this.libraryPath, this.pluginRoot);

  final String libraryPath;
  final String pluginRoot;

  static _Environment? detect() {
    String? library;
    for (final String candidate in MusicxxPluginNativeLibrary.defaultCandidates()) {
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

/// 等待条件成立（最多 5 秒；每 20 ms 泵一次事件并让出事件循环）
Future<void> _pumpUntil(bool Function() condition, {Duration timeout = const Duration(seconds: 5)}) async {
  final DateTime deadline = DateTime.now().add(timeout);
  final MusicxxPluginRuntime runtime = MusicxxPluginRuntime.instance;
  while (!condition()) {
    if (DateTime.now().isAfter(deadline)) {
      return;
    }
    runtime.pumpEvents();
    await Future<void>.delayed(const Duration(milliseconds: 20));
  }
}
