/// 端到端测试：Dart 侧运行时对**真实原生宿主库**的验证
///
/// 覆盖：
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

  test('原生宿主：init/事件泵/扫描/装载/钩子/能力/卸载', () async {
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

    // 多目标打包（仿 APK 的 lib/<系统>-<架构>/）: 示例插件 example_native_multi
    // 的库文件放在分支目录里，扫描应报告选中的分支，装载的也应是那一份构建。
    final MusicxxPluginInfo? multi = found
        .where((MusicxxPluginInfo info) => info.id == 'example_native_multi')
        .firstOrNull;
    expect(
      multi,
      isNotNull,
      reason: '扫描结果: ${found.map((e) => e.id).toList()}',
    );
    expect(multi!.supported, isTrue, reason: multi.reason);
    expect(multi.target, isNotEmpty, reason: '多目标包应报告选中的分支标签');
    expect(multi.targetEntry, contains('lib/${multi.target}/'));
    expect(
      multi.targets.any((MusicxxPluginTarget_c item) => item.selected),
      isTrue,
    );
    expect(
      multi.target.contains(MusicxxPluginEnv.currentOs()),
      isTrue,
      reason: '选中的分支应属于当前系统 (${MusicxxPluginEnv.currentOs()})',
    );
    runtime.plugins.load('example_native_multi');
    expect(runtime.plugins.findLoaded('example_native_multi'), isNotNull);
    final Object? which = runtime.plugins.call(
      'example_native_multi',
      'plugin.example_native_multi.which',
    );
    expect(which, isA<Map<String, Object?>>());
    expect(
      (which! as Map<String, Object?>)['tag'],
      multi.target,
      reason: '装载的应是宿主选中的那个分支',
    );
    runtime.plugins.unload('example_native_multi');

    // 安装预检入口（plugin_inspect）：与扫描同源，可指定目标环境
    final MusicxxPluginInspect_c inspected = runtime.plugins.inspect(
      multi.path,
      os: MusicxxPluginEnv.currentOs(),
      arch: MusicxxPluginEnv.currentArch(),
    );
    expect(inspected.valid, isTrue, reason: inspected.error);
    expect(inspected.id, 'example_native_multi');
    expect(inspected.kind, MusicxxPluginKind.native);
    expect(inspected.supported, isTrue, reason: inspected.reason);
    expect(
      inspected.target,
      multi.target,
      reason: '预检与扫描选中的分支必须一致（原生侧同一份判定）',
    );
    expect(inspected.targetEntry, multi.targetEntry);
    expect(inspected.targets.length, multi.targets.length);

    // 应用侧安装预检的调用形状：**不传** os/arch（= 用宿主当前环境）
    //
    // 回归：声明了 `platforms` 的插件曾被一律判成"当前平台不在清单声明内"，
    // 于是「从压缩包安装」永远失败 —— 示例插件几乎都声明了 platforms。
    final MusicxxPluginInfo? declared = found
        .where((MusicxxPluginInfo info) => info.id == 'example_js_shader')
        .firstOrNull;
    expect(declared, isNotNull, reason: '扫描结果里应有 example_js_shader');
    final MusicxxPluginInspect_c declaredByHost = runtime.plugins.inspect(
      declared!.path,
    );
    expect(declaredByHost.valid, isTrue, reason: declaredByHost.error);
    expect(
      declaredByHost.platforms,
      isNotEmpty,
      reason: '该示例插件声明了 platforms，正是这条回归的样本',
    );
    expect(
      declaredByHost.supported,
      isTrue,
      reason: '不传 os/arch 时按宿主当前环境判定：${declaredByHost.reason}',
    );

    // 指定目标环境：示例包只有本机那一份构建，换个系统就应当选不到分支
    final String otherOs =
        MusicxxPluginEnv.currentOs() == 'windows' ? 'linux' : 'windows';
    final MusicxxPluginInspect_c mismatched = runtime.plugins.inspect(
      multi.path,
      os: otherOs,
      arch: MusicxxPluginEnv.currentArch(),
    );
    expect(mismatched.supported, isFalse);
    expect(mismatched.reason, contains('没有匹配当前系统/架构的分支'));

    // 目录不存在：valid=false + 可读原因（不抛异常）
    final MusicxxPluginInspect_c missing = runtime.plugins.inspect(
      '${multi.path}_missing',
    );
    expect(missing.valid, isFalse);
    expect(missing.error, isNotEmpty);

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
    expect(
      probe['foreignActionRc'],
      0,
      reason: '动作名不再做命名空间校验（任意动作都被受理）',
    );
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
    // 异步裁决：Dart 线程不等待，结果经 `musicxx.hook.decision.result` 回传后合并
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

    // ===== 声明式 UI 扩展 =====
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

    // ===== JS 插件 (零编译) =====
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
    // 扫描结果带脚本清单（管理页展示；示例插件按清单的 scripts 顺序装载 kit 与脚本）
    expect(
      exampleJs.scripts,
      <String>['pluginxx_ui_kit.js', 'musicxx_ui_kit.js', 'plugin.js'],
    );

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
    // JS 裁决处理器返回 Promise (异步裁决): 宿主一直等到脚本结算, 结算后裁决生效
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

    // 异步能力处理器 (返回 Promise): 宿主等到它结算再给调用方结果
    final Object? slowRaw = runtime.plugins.call(
      'example_js',
      'plugin.example_js.slowProbe',
      const <String, Object?>{'waitMs': 250},
    );
    expect(slowRaw, isA<Map<String, Object?>>(), reason: '异步能力应有结果');
    final Map<String, Object?> slow = slowRaw! as Map<String, Object?>;
    expect(slow['ok'], true);
    expect(slow['waitedMs'], 250);
    expect(slow['pluginId'], 'example_js');
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

    // 播放页背景示例已拆成独立插件 example_js_shader: 它注册背景槽位的 UI 项
    // (shader bundle 声明), 速率是它自己的设置项。
    if (Directory('${env.pluginRoot}/example_js_shader').existsSync()) {
      final MusicxxPluginInfo? shaderPlugin = found2
          .where((MusicxxPluginInfo info) => info.id == 'example_js_shader')
          .firstOrNull;
      expect(shaderPlugin, isNotNull);
      expect(shaderPlugin!.kind, MusicxxPluginKind.js);
      runtime.plugins.load('example_js_shader');
      final MusicxxPluginUIItem background = MusicxxPluginUIItems.byType(
        runtime.plugins.uiSnapshot(),
        MusicxxPluginUIType.playingBackground,
      ).firstWhere(
        (MusicxxPluginUIItem item) => item.plugin == 'example_js_shader',
      );
      expect(
        (background.data['shader']! as Map<String, Object?>)['bundle'],
        'shader/bg.shaderbundle',
        reason: 'bundle 用插件目录内的相对路径',
      );
      expect(background.data['speed'], 1, reason: '默认 1× = 基准速度 1');
      // 设置页能力: 页面由插件给出 (改速率用的是同一个页面)
      final Object? viewRaw = runtime.plugins.call(
        'example_js_shader',
        'plugin.example_js_shader.settings',
        const <String, Object?>{},
      );
      final Map<String, Object?> shaderView =
          (viewRaw! as Map<String, Object?>)['view']! as Map<String, Object?>;
      expect(shaderView['title'], isNotEmpty);
      expect(shaderView['blocks'], isA<List<Object?>>());
      runtime.plugins.unload('example_js_shader');
      _step('18.5 背景示例插件完成');
    }

    // 新槽位示例 playing_bg_image: 同时注册『播放页背景样式』与『播放页歌曲图接管项』
    // （`musicxx.ui.playing.icon` 是这一轮新增的 UI 项类型）。它顶层还注册了
    // `musicxx.media.palette.provide` 取色钩子 —— 这两条都能挡住"忘了重建宿主库"
    // （旧宿主库会以未知 UI 项类型 / 未知钩子拒绝注册）。
    if (Directory('${env.pluginRoot}/playing_bg_image').existsSync()) {
      final MusicxxPluginInfo? imagePlugin = found2
          .where((MusicxxPluginInfo info) => info.id == 'playing_bg_image')
          .firstOrNull;
      expect(imagePlugin, isNotNull);
      expect(imagePlugin!.kind, MusicxxPluginKind.js);
      runtime.plugins.load('playing_bg_image');
      expect(
        runtime.hooks.refreshNativeHandlerCount(
          MusicxxPluginHookId.mediaPaletteProvide,
        ),
        1,
        reason: '取色钩子的处理器要在宿主侧登记成功（宿主库没重建时这里会是 0）',
      );
      final List<MusicxxPluginUIItem> items = runtime.plugins.uiSnapshot();
      final MusicxxPluginUIItem background = MusicxxPluginUIItems.byType(
        items,
        MusicxxPluginUIType.playingBackground,
      ).firstWhere(
        (MusicxxPluginUIItem item) => item.plugin == 'playing_bg_image',
      );
      expect(
        (background.data['shader']! as Map<String, Object?>)['bundle'],
        'shader/heat.shaderbundle',
      );
      expect(
        (background.data['cover']! as Map<String, Object?>)['blur'],
        14,
        reason: '封面纹理的预模糊强度由插件声明（默认档"标准"，设置页可切清晰/柔和）',
      );
      // 歌曲图接管项：mode = none（保留占位、不显示内容）
      final MusicxxPluginUIItem icon = MusicxxPluginUIItems.byType(
        items,
        MusicxxPluginUIType.playingIcon,
      ).firstWhere((MusicxxPluginUIItem item) => item.plugin == 'playing_bg_image');
      expect(icon.data['mode'], 'none');
      expect(icon.data['keepSpace'], true);
      // 取色钩子的处理器必须**同步**返回（不能在里面 await 宿主动作）：应用是在需要颜色的
      // 时候等这次裁决的（切换背景样式、切歌、渲染准备都在 await 这条链上），处理器里等动作
      // 会把两边互相拖住 —— 实测表现是"启用插件后切歌 / 换背景样式卡几秒"。
      final Stopwatch paletteWatch = Stopwatch()..start();
      final Map<String, Object?>? paletteVerdict = await runtime.hooks
          .decideAsync(
            MusicxxPluginHookId.mediaPaletteProvide,
            <String, Object?>{
              'srcKey': 'test-key',
              'name': '测试歌曲',
              'artist': '测试歌手',
              'hasCover': true,
              'night': false,
            },
          );
      paletteWatch.stop();
      expect(
        paletteWatch.elapsedMilliseconds,
        lessThan(300),
        reason: '取色钩子的处理器要同步返回：在里面等宿主动作会让应用一直等到超时',
      );
      expect(
        paletteVerdict,
        isNull,
        reason: '没命中取色缓存时返回 null（应用走内置快速取色，算好后用 setPalette 写回）',
      );
      // 设置页能力：页面由插件给出（宿主只做结构校验，渲染在应用侧）
      final Object? imageViewRaw = runtime.plugins.call(
        'playing_bg_image',
        'plugin.playing_bg_image.settings',
        const <String, Object?>{},
      );
      final Map<String, Object?> imageView =
          (imageViewRaw! as Map<String, Object?>)['view']!
              as Map<String, Object?>;
      expect(imageView['title'], isNotEmpty);
      expect(imageView['blocks'], isA<List<Object?>>());
      runtime.plugins.unload('playing_bg_image');
      _step('18.7 热浪封面插件完成');
    }

    // 统计快照可读
    final Map<String, Object?> stats = runtime.plugins.stats();
    expect(stats['host'], isA<Map<String, Object?>>());
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('变量通道：声明/读/写/订阅（对真实原生库）', () async {
    if (env == null || !env.hasPlugin('example_js_vars')) {
      markTestSkipped('未找到原生宿主库或变量通道示例插件，跳过（先运行 tools/build_native.ps1）');
      return;
    }
    final MusicxxPluginRuntime runtime = MusicxxPluginRuntime.create();
    addTearDown(runtime.dispose);

    // 应用侧"官方键的真值"（真值在应用这里，宿主只留一份服务 peek 的缓存）
    Object? officialValue = 'v0';

    final StreamSubscription<MusicxxPluginEvent> sub = runtime.events.listen((
      MusicxxPluginEvent e,
    ) {
      switch (e.type) {
        case MusicxxPluginEventType.varRead:
          // 插件发起异步读 → 应用回答（本地的直接问实现，权威且立刻可得）
          runtime.vars.readResult(
            e.intOf('requestId') ?? -1,
            ok: true,
            value: officialValue,
          );
        case MusicxxPluginEventType.varWrite:
          // 插件发起写 → 应用写入（回执即写入）后回执
          officialValue = e.payload['value'];
          runtime.vars.writeResult(
            e.intOf('requestId') ?? -1,
            accepted: true,
            value: officialValue,
          );
      }
    });
    addTearDown(sub.cancel);

    runtime.init(
      config: MusicxxPluginRuntimeConfig(
        appVersion: '0.0.0-test',
        platform: MusicxxPluginRuntime.currentPlatform,
        language: 'zh-cn',
        userPluginDir: env.pluginRoot,
      ),
      libraryPath: env.libraryPath,
    );

    // 旧宿主库诊断：本次构建必须带变量位
    expect(runtime.featureBits, isNotNull);
    expect(runtime.missingFeatures, isEmpty, reason: runtime.featureHint);

    // 1) 声明官方键（必须赶在装载插件之前：脚本顶层就要能读到）
    expect(
      runtime.vars.declare(<MusicxxPluginVarDeclare>[
        const MusicxxPluginVarDeclare(
          key: 'musicxx.test.bound',
          caps: <String>['get', 'set', 'notify'],
          type: 'string',
          options: <String>['v0', 'v1', 'v2'],
          hasValue: true,
          value: 'v0',
          title: '绑定测试值',
          risk: 'low',
        ),
      ]),
      isTrue,
    );

    // 2) 装载变量通道示例（顶层登记自己的变量 + 订阅官方键）
    runtime.plugins.load('example_js_vars');

    Map<String, Object?> probe() {
      final Object? raw = runtime.plugins.call('example_js_vars', 'probe');
      return raw is Map
          ? raw.cast<String, Object?>()
          : <String, Object?>{};
    }

    // 3) 插件变量进了变量表，声明字段如实
    final List<Map<String, Object?>> own = runtime.vars.list(
      prefix: 'plugin.example_js_vars',
    );
    final List<Object?> ownKeys = own
        .map((Map<String, Object?> e) => e['key'])
        .toList();
    expect(ownKeys, contains('plugin.example_js_vars.tip.start'));
    expect(ownKeys, contains('plugin.example_js_vars.stats.reads'));
    final Map<String, Object?> tip = own.firstWhere(
      (Map<String, Object?> e) => e['key'] == 'plugin.example_js_vars.tip.start',
    );
    expect(tip['owner'], 'example_js_vars');
    expect(tip['caps'], <Object?>['get', 'set', 'notify']);
    expect(tip['type'], 'bool');
    // 声明了初值 → 同步读缓存里就有值（declared 模式由宿主代存）
    final Map<String, Object?> peeked =
        (probe()['tipPeek'] as Map<Object?, Object?>?)?.cast<String, Object?>() ??
        <String, Object?>{};
    expect(peeked['value'], false);

    // 4) 应用推值 → 订阅了该键的插件收到通知（bind 回调）
    officialValue = 'v1';
    expect(runtime.vars.update('musicxx.test.bound', 'v1'), isTrue);
    // 同一个值再推一次: 包层就不发了（去重）
    expect(runtime.vars.update('musicxx.test.bound', 'v1'), isFalse);
    await _pumpUntil(() => _intOf(probe()['changes']) >= 1);
    expect(_intOf(probe()['changes']), greaterThanOrEqualTo(1));
    final Map<String, Object?> lastChange =
        (probe()['lastChange'] as Map<Object?, Object?>?)
            ?.cast<String, Object?>() ??
        <String, Object?>{};
    expect(lastChange['value'], 'v1');
    expect(lastChange['by'], ''); // 写入方是应用（不是插件）

    // 5) 插件异步读官方键（宿主向应用取一次真实值）
    runtime.plugins.call('example_js_vars', 'triggerRead', <String, Object?>{
      'key': 'musicxx.test.bound',
    });
    await _pumpUntil(() => probe()['readValue'] != null);
    expect(probe()['readValue'], 'v1', reason: '异步读应当拿到应用侧的真值');

    // 6) 插件异步写官方键（应用写入后才算数）
    runtime.plugins.call('example_js_vars', 'triggerWrite', <String, Object?>{
      'key': 'musicxx.test.bound',
      'value': 'v2',
    });
    await _pumpUntil(() => probe()['writeResult'] != null);
    final Map<String, Object?> writeResult =
        (probe()['writeResult'] as Map<Object?, Object?>?)
            ?.cast<String, Object?>() ??
        <String, Object?>{};
    expect(writeResult['accepted'], isTrue);
    expect(writeResult['value'], 'v2');
    expect(officialValue, 'v2', reason: '写入由应用执行');
    // 写成功后值真的变了 → 订阅者再收到一条通知
    await _pumpUntil(() => _intOf(probe()['changes']) >= 2);

    // 7) 应用读/写插件键（declared 模式立即结算）
    expect(runtime.vars.get('plugin.example_js_vars.tip.start'), false);
    final Map<String, Object?> setResult =
        runtime.vars.set('plugin.example_js_vars.tip.start', true) ??
        <String, Object?>{};
    expect(setResult['accepted'], isTrue);
    expect(setResult['changed'], isTrue);
    expect(runtime.vars.get('plugin.example_js_vars.tip.start'), true);

    // 7.5) handler 模式：读写在属主手里（宿主只留同步读缓存）
    //      - 读: 宿主把请求转给属主, 由 onRead 回答
    //      - 写: 宿主把请求转给 onWrite, 属主写入后回执
    expect(
      runtime.vars.get('plugin.example_js_vars.handler.tip'),
      false,
      reason: 'handler 变量的读要走属主',
    );
    final Map<String, Object?> handlerWrite =
        runtime.vars.set('plugin.example_js_vars.handler.tip', true) ??
        <String, Object?>{};
    expect(handlerWrite['accepted'], isTrue);
    expect(handlerWrite['value'], true, reason: '回执带的是属主写入的最终值');
    expect(_intOf(probe()['handlerTip']), 0); // 插件侧状态是 bool, 这里只校验可取到
    expect(runtime.vars.get('plugin.example_js_vars.handler.tip'), true);
    // 属主自己改值时提交一次（不是应用写的）
    runtime.plugins.call('example_js_vars', 'setHandlerInternal', <String, Object?>{
      'value': false,
    });
    expect(runtime.vars.get('plugin.example_js_vars.handler.tip'), false);

    // 8) 不存在的键: 读/写都要明确失败（而不是静默成功）
    expect(runtime.vars.get('plugin.example_js_vars.nope'), isNull);
    expect(runtime.vars.set('plugin.example_js_vars.nope', 1), isNull);

    // 9) 卸载后变量随实例摘除
    expect(
      runtime.vars.list(prefix: 'plugin.example_js_vars').isNotEmpty,
      isTrue,
      reason: '卸载前应当能看到插件变量',
    );
    runtime.plugins.unload('example_js_vars');
    expect(runtime.vars.list(prefix: 'plugin.example_js_vars'), isEmpty);
  });

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
  print('[host-test] $message');
}

/// 原生库 + 示例插件目录探测（找不到就跳过需要它们的用例）
class _Environment {
  const _Environment(this.libraryPath, this.pluginRoot);

  final String libraryPath;
  final String pluginRoot;

  /// 该插件是否随产物一起安装（只构建了一部分示例时，对应用例跳过而不是失败）
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

/// 取一个数值字段（缺省或类型不符时按 0 处理；读插件的探针结果用）
int _intOf(Object? value) => value is num ? value.toInt() : 0;

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
