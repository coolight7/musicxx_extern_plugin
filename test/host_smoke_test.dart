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
    final StreamSubscription<MusicxxPluginEvent> sub = runtime.events.listen(
      seen.add,
    );
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
    await _pumpUntil(
      () => seen.any(
        (MusicxxPluginEvent e) => e.type == MusicxxPluginEventType.hostReady,
      ),
    );
    final MusicxxPluginEvent ready = seen.firstWhere(
      (MusicxxPluginEvent e) => e.type == MusicxxPluginEventType.hostReady,
    );
    expect(ready.payload['apiVersion'], 1);
    expect(ready.payload['platform'], MusicxxPluginRuntime.currentPlatform);

    _step('2 host.ready 已收到');
    // 扫描：应发现示例插件
    final List<MusicxxPluginInfo> found = runtime.plugins.scan();
    final MusicxxPluginInfo? example = found
        .where((MusicxxPluginInfo info) => info.id == 'example_native')
        .firstOrNull;
    expect(
      example,
      isNotNull,
      reason: '扫描结果: ${found.map((e) => e.id).toList()}',
    );
    expect(example!.valid, isTrue);
    expect(example.supported, isTrue, reason: example.reason);
    expect(example.kind, MusicxxPluginKind.native);

    _step('3 scan 完成: ${found.length} 项');
    // 装载：同步等待 + 事件回报
    runtime.plugins.load('example_native');
    expect(runtime.plugins.findLoaded('example_native'), isNotNull);
    await _pumpUntil(
      () => seen.any(
        (MusicxxPluginEvent e) => e.type == MusicxxPluginEventType.pluginLoaded,
      ),
    );

    _step('4 load 完成');
    // 原生处理器位图：插件在 start 里注册了 3 个钩子
    expect(
      runtime.hooks.refreshNativeHandlerCount(
        MusicxxPluginHookId.playerBeforePlaySong,
      ),
      1,
    );
    expect(
      runtime.hooks.hasHandlers(MusicxxPluginHookId.playerBeforePlaySong),
      isTrue,
    );
    expect(
      runtime.hooks.hasHandlers(MusicxxPluginHookId.playerPosition),
      isFalse,
    );

    _step('5 hook_count 完成');
    // 状态镜像：Dart 推送 → 插件同步可读（探针里回报 stateLen）
    runtime.state.update(MusicxxPluginState.keySong, <String, Object?>{
      'sid': 's-dart',
      'name': 'Dart 推送的歌曲',
    });

    _step('6 state 推送完成');
    // 能力调用：读取插件自检信息（线程归属 + 命名空间校验结果）
    final probeRaw = runtime.plugins.call(
      'example_native',
      'plugin.example_native.probe',
      const <String, Object?>{},
    );
    expect(probeRaw, isA<Map<String, Object?>>());
    final Map<String, Object?> probe = probeRaw! as Map<String, Object?>;
    expect(
      probe['startThread'],
      equals(probe['callThread']),
      reason: '插件代码必须只在宿主线程执行',
    );
    expect(probe['unknownHookRc'], -4, reason: '未知前缀钩子必须被拒绝');
    expect(probe['foreignActionRc'], -6, reason: '他人命名空间动作必须被拒绝');
    expect(probe['stateLen'], greaterThan(0), reason: '插件应能同步读到 Dart 推送的状态镜像');

    _step('7 能力调用完成: $probeRaw');
    // 钩子派发（裁决型）：广告曲目 → skip
    final Map<String, Object?>? verdict = runtime.hooks.decide(
      MusicxxPluginHookId.playerBeforePlaySong,
      <String, Object?>{
        'sid': 's-ad',
        'song': <String, Object?>{'name': '广告插曲 - 测试'},
      },
    );
    expect(verdict, isNotNull);
    expect(verdict!['action'], 'skip');
    // 普通曲目 → 无裁决
    expect(
      runtime.hooks.decide(
        MusicxxPluginHookId.playerBeforePlaySong,
        <String, Object?>{
          'sid': 's-normal',
          'song': <String, Object?>{'name': '普通歌曲'},
        },
      ),
      isNull,
    );

    _step('8 decide 完成');
    // Dart 处理器链 + 合并：Dart 侧给 patch，原生侧给 skip（anyCancel 最保守 → 保留 skip）
    Map<String, Object?> dartHandler(MusicxxPluginHookContext context) =>
        <String, Object?>{
          'action': 'continue',
          'patch': <String, Object?>{'quality': 'hires'},
        };
    runtime.hooks.on(MusicxxPluginHookId.playerBeforePlaySong, dartHandler);
    expect(
      runtime.hooks.hasDartHandler(MusicxxPluginHookId.playerBeforePlaySong),
      isTrue,
    );
    final Map<String, Object?>? merged = runtime.hooks.decide(
      MusicxxPluginHookId.playerBeforePlaySong,
      <String, Object?>{
        'sid': 's-ad2',
        'song': <String, Object?>{'name': '广告曲目 2'},
      },
    );
    expect(merged!['action'], 'skip', reason: 'anyCancel：cancel/skip 不被后续覆盖');
    runtime.hooks.off(MusicxxPluginHookId.playerBeforePlaySong, dartHandler);
    expect(
      runtime.hooks.hasDartHandler(MusicxxPluginHookId.playerBeforePlaySong),
      isFalse,
    );

    // 观察型钩子：入队即返回（不等待）
    runtime.hooks.observe(MusicxxPluginHookId.songChanged, <String, Object?>{
      'sid': 's-ad',
      'song': <String, Object?>{'name': '普通歌曲'},
    });
    await _pumpUntil(() => runtime.recentEvents.isNotEmpty);

    _step('9 Dart 处理器链完成');
    // 异步裁决（plan §5.3）：Dart 线程不等待，结果经 `musicxx.hook.decision.result` 回传后合并
    final Map<String, Object?>? asyncVerdict = await runtime.hooks.decideAsync(
      MusicxxPluginHookId.playerBeforePlaySong,
      <String, Object?>{
        'sid': 's-async-ad',
        'song': <String, Object?>{'name': '广告插曲 - 异步'},
      },
    );
    expect(asyncVerdict, isNotNull);
    expect(asyncVerdict!['action'], 'skip', reason: '异步裁决结果应与同步一致');
    expect(
      await runtime.hooks.decideAsync(
        MusicxxPluginHookId.playerBeforePlaySong,
        <String, Object?>{
          'sid': 's-async-normal',
          'song': <String, Object?>{'name': '普通歌曲'},
        },
      ),
      isNull,
    );
    expect(runtime.hooks.pendingAsyncDecisions, 0, reason: '异步裁决不应残留等待中的调用');
    expect(runtime.hooks.asyncDecisionCalls, greaterThan(0));
    expect(runtime.hooks.asyncDecisionTimeouts, 0, reason: '正常路径不应超时');

    _step('9.5 异步裁决完成');
    // 禁用 → 原生处理器位图清零；启用 → 恢复
    runtime.plugins.disable('example_native');
    expect(
      runtime.hooks.refreshNativeHandlerCount(
        MusicxxPluginHookId.playerBeforePlaySong,
      ),
      0,
    );
    runtime.plugins.enable('example_native');
    expect(
      runtime.hooks.refreshNativeHandlerCount(
        MusicxxPluginHookId.playerBeforePlaySong,
      ),
      1,
    );

    _step('10 enable/disable 完成');
    // 卸载：幂等（第二次仍成功），且能力调用失败
    runtime.plugins.unload('example_native');
    runtime.plugins.unload('example_native');
    expect(runtime.plugins.findLoaded('example_native'), isNull);
    expect(
      () =>
          runtime.plugins.call('example_native', 'plugin.example_native.probe'),
      throwsA(isA<MusicxxPluginApiException>()),
    );

    _step('11 unload 完成');

    // ===== 声明式 UI 扩展 (plan §5.6) =====
    // 重新装载原生示例插件, 读取 UI 项快照 (入口项 / 菜单项 / 顺序 / 动作描述)
    runtime.plugins.load('example_native');
    final List<MusicxxPluginUIItem> uiItems = runtime.plugins.uiSnapshot();
    final List<MusicxxPluginUIItem> homeEntries = MusicxxPluginUIItems.byType(
      uiItems,
      MusicxxPluginUIType.homeEntry,
    );
    final List<MusicxxPluginUIItem> songActions = MusicxxPluginUIItems.byType(
      uiItems,
      MusicxxPluginUIType.songAction,
    );
    expect(
      homeEntries.any(
        (MusicxxPluginUIItem item) => item.plugin == 'example_native',
      ),
      isTrue,
      reason: '快照: $uiItems',
    );
    final MusicxxPluginUIItem card = homeEntries.firstWhere(
      (MusicxxPluginUIItem item) => item.plugin == 'example_native',
    );
    expect(card.name, 'card');
    expect(card.title, isNotEmpty);
    expect(card.actionKind, MusicxxPluginUIActionKind.route);
    expect(card.viewId, 'card');
    expect(
      songActions.any(
        (MusicxxPluginUIItem item) => item.plugin == 'example_native',
      ),
      isTrue,
      reason: '快照: $songActions',
    );

    _step('11.5 UI 项快照完成: ${uiItems.length} 项');
    // 声明式页面：UI 项只声明"打开哪个页面"，页面本身由插件的**同名能力**绘制。
    // 这里断言"声明了 route 入口的插件确实提供了同名能力" —— 只声明入口不实现能力时，
    // 宿主打开页面只会得到"插件未声明该能力"。
    final String? nativeViewId = card.viewId;
    expect(nativeViewId, isNotNull);
    final Object? nativeCardRaw = runtime.plugins.call(
      'example_native',
      'plugin.example_native.$nativeViewId',
      <String, Object?>{'view': nativeViewId},
    );
    expect(nativeCardRaw, isA<Map<String, Object?>>(), reason: '原生示例应返回页面视图');
    final Map<String, Object?> nativeView =
        (nativeCardRaw! as Map<String, Object?>)['view']! as Map<String, Object?>;
    expect(nativeView['title'], isNotEmpty);
    expect(nativeView['blocks'], isA<List<Object?>>());
    expect((nativeView['blocks']! as List<Object?>).length, greaterThan(1));

    _step('11.6 原生示例的页面能力完成');
    // 卸载后 UI 项被摘除 (宿主随实例摘除, Dart 侧不再渲染)
    runtime.plugins.unload('example_native');
    expect(
      runtime.plugins.uiSnapshot().any(
        (MusicxxPluginUIItem item) => item.plugin == 'example_native',
      ),
      isFalse,
      reason: '卸载后不应残留该插件的 UI 项',
    );

    _step('11.7 卸载后 UI 项摘除完成');

    // ===== JS 插件 (零编译, plan §4.4/§4.6) =====
    // 扫描结果里应有 JS 示例插件
    final List<MusicxxPluginInfo> found2 = runtime.plugins.scan();
    final MusicxxPluginInfo? exampleJs = found2
        .where((MusicxxPluginInfo info) => info.id == 'example_js')
        .firstOrNull;
    expect(
      exampleJs,
      isNotNull,
      reason: '扫描结果: ${found2.map((e) => e.id).toList()}',
    );
    expect(exampleJs!.kind, MusicxxPluginKind.js);
    expect(exampleJs.supported, isTrue, reason: exampleJs.reason);

    _step('12 scan 发现 JS 插件');
    runtime.plugins.load('example_js');
    expect(
      runtime.plugins.findLoaded('example_js'),
      isNotNull,
      reason: 'JS 插件实例应已加载',
    );
    expect(
      runtime.hooks.refreshNativeHandlerCount(
        MusicxxPluginHookId.playerBeforePlaySong,
      ),
      1,
    );

    _step('13 JS 插件装载完成');
    // JS 插件的 UI 项 (顶层注册 → 宿主线程回放) 同样进快照
    final List<MusicxxPluginUIItem> uiItemsJs = runtime.plugins.uiSnapshot();
    expect(
      uiItemsJs.any(
        (MusicxxPluginUIItem item) =>
            item.plugin == 'example_js' &&
            item.type == MusicxxPluginUIType.homeEntry,
      ),
      isTrue,
      reason: '快照: $uiItemsJs',
    );
    // 裁决型钩子 (JS 侧处理器)
    final Map<String, Object?>? jsVerdict = runtime.hooks.decide(
      MusicxxPluginHookId.playerBeforePlaySong,
      <String, Object?>{
        'sid': 's-js-ad',
        'song': <String, Object?>{'name': '广告插曲 - JS'},
      },
    );
    expect(jsVerdict, isNotNull);
    expect(jsVerdict!['action'], 'skip');
    expect(
      runtime.hooks.decide(
        MusicxxPluginHookId.playerBeforePlaySong,
        <String, Object?>{
          'sid': 's-js-normal',
          'song': <String, Object?>{'name': '普通歌曲 JS'},
        },
      ),
      isNull,
    );

    _step('14 JS decide 完成');
    // 异步派发同样适用于 JS 处理器（宿主在自己的线程上等脚本，Dart 线程不阻塞）
    final Map<String, Object?>? jsAsyncVerdict = await runtime.hooks
        .decideAsync(
          MusicxxPluginHookId.playerBeforePlaySong,
          <String, Object?>{
            'sid': 's-js-async-ad',
            'song': <String, Object?>{'name': '广告插曲 - JS 异步'},
          },
        );
    expect(jsAsyncVerdict, isNotNull);
    expect(jsAsyncVerdict!['action'], 'skip');

    _step('14.5 JS 异步裁决完成');
    // JS 裁决处理器返回 Promise (异步裁决): 宿主在等待预算内等脚本结算, 结算后裁决生效
    final Map<String, Object?>? jsPromiseVerdict = runtime.hooks.decide(
      MusicxxPluginHookId.playerSpeed,
      <String, Object?>{'sid': 's-js-speed', 'from': 1.0, 'to': 8.0},
    );
    expect(jsPromiseVerdict, isNotNull, reason: 'example_js 的异步裁决应生效');
    // 宿主把处理器返回的 patch 合并进结果对象（顶层键，与其它裁决钩子一致）
    expect((jsPromiseVerdict!['to'] as num?)?.toDouble(), 3.0);
    // 同一个处理器在"不需要裁决"的分支上同步返回 null → 没有裁决
    expect(
      runtime.hooks.decide(MusicxxPluginHookId.playerSpeed, <String, Object?>{
        'sid': 's-js-speed-ok',
        'from': 1.0,
        'to': 1.5,
      }),
      isNull,
    );

    _step('14.6 JS Promise 裁决完成');
    // 观察型钩子 (异步) + JS 侧日志
    runtime.hooks.observe(MusicxxPluginHookId.songChanged, <String, Object?>{
      'sid': 's-js',
      'song': <String, Object?>{'name': 'JS 观察目标'},
    });
    await _pumpUntil(
      () => seen.any(
        (MusicxxPluginEvent e) =>
            e.type == MusicxxPluginEventType.pluginLog &&
            e.stringOf('message')?.contains('切歌') == true,
      ),
    );

    _step('15 JS 观察钩子执行完成');
    // 能力探针: JS 侧同步读状态镜像 + 宿主信息
    final jsProbeRaw = runtime.plugins.call(
      'example_js',
      'plugin.example_js.probe',
      const <String, Object?>{},
    );
    expect(jsProbeRaw, isA<Map<String, Object?>>());
    final Map<String, Object?> jsProbe = jsProbeRaw! as Map<String, Object?>;
    expect(jsProbe['pluginId'], 'example_js');
    expect(jsProbe['hostPlatform'], MusicxxPluginRuntime.currentPlatform);
    expect(jsProbe['currentSongName'], 'Dart 推送的歌曲', reason: 'JS 应能同步读到状态镜像');
    expect(jsProbe['songChangedCount'], greaterThan(0));

    _step('16 JS 能力调用完成');
    // JS 示例的主页入口与播放页附加信息块都指向 ext://example_js/card:
    // 页面由同名能力 `card` 绘制, 这里断言这条链路真的通 (只声明入口不实现能力时,
    // 宿主打开页面只会得到"插件未声明该能力")
    final List<MusicxxPluginUIItem> jsHomeEntries = MusicxxPluginUIItems.byType(
      uiItemsJs,
      MusicxxPluginUIType.homeEntry,
    );
    final MusicxxPluginUIItem jsCard = jsHomeEntries.firstWhere(
      (MusicxxPluginUIItem item) => item.plugin == 'example_js',
    );
    expect(jsCard.viewId, isNotNull);
    final Object? jsCardRaw = runtime.plugins.call(
      'example_js',
      'plugin.example_js.${jsCard.viewId}',
      <String, Object?>{'view': jsCard.viewId},
    );
    expect(jsCardRaw, isA<Map<String, Object?>>(), reason: 'JS 示例应返回页面视图');
    final Map<String, Object?> jsView =
        (jsCardRaw! as Map<String, Object?>)['view']! as Map<String, Object?>;
    expect(jsView['title'], isNotEmpty);
    expect(jsView['blocks'], isA<List<Object?>>());
    expect((jsView['blocks']! as List<Object?>).length, greaterThan(1));

    _step('16.5 JS 示例的页面能力完成');
    // 禁用/启用: JS 脚本随 stop/start 重跑
    runtime.plugins.disable('example_js');
    expect(
      runtime.hooks.refreshNativeHandlerCount(
        MusicxxPluginHookId.playerBeforePlaySong,
      ),
      0,
    );
    runtime.plugins.enable('example_js');
    expect(
      runtime.hooks.refreshNativeHandlerCount(
        MusicxxPluginHookId.playerBeforePlaySong,
      ),
      1,
    );

    _step('17 JS enable/disable 完成');
    // 卸载: JS 实例与注册全部摘除, 槽位保留在宿主内置表里
    runtime.plugins.unload('example_js');
    expect(runtime.plugins.findLoaded('example_js'), isNull);
    expect(
      () => runtime.plugins.call('example_js', 'plugin.example_js.probe'),
      throwsA(isA<MusicxxPluginApiException>()),
    );

    _step('18 JS unload 完成');

    // 统计快照可读
    final Map<String, Object?> stats = runtime.plugins.stats();
    expect(stats['host'], isA<Map<String, Object?>>());
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('库缺失时抛出可诊断异常', () {
    expect(
      () => MusicxxPluginNativeLibrary.open(
        path: 'definitely/missing/musicxx.dll',
      ),
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

/// 等待条件成立（最多 5 秒；每 20 ms 泵一次事件并让出事件循环）
Future<void> _pumpUntil(
  bool Function() condition, {
  Duration timeout = const Duration(seconds: 5),
}) async {
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
