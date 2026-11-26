/// 整套流程测试：Dart 侧运行时对**真实原生插件框架库**的验证
///
/// 覆盖：
/// - 库加载与 C ABI 版本校验、插件框架 init/dispose 幂等；
/// - 事件泵（host.ready / plugin.loaded 等按序到达）；
/// - 插件扫描/加载/能力调用/卸载；
/// - 钩子派发：Dart 处理器链 + 原生处理器链 + 合并，以及"无处理器"快速路径；
/// - 快照频道推送（插件侧同步可读）。
///
/// 前置：先跑 `tools/build_examples.ps1`（Windows）或 `tools/build_examples.sh`（Linux/macOS）（产出 `.native/output/<平台>-<架构>-<配置>/`，含示例插件）。
/// 若找不到原生库或示例插件，测试会跳过而不是失败（CI 里可以先构建再跑）。
library;

import 'dart:async';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:musicxx_extern_plugin/musicxx_extern_plugin.dart';

import 'ui_content_probe.dart';

void main() {
  final _Environment? env = _Environment.detect();

  test('原生插件框架：init/事件泵/扫描/加载/钩子/能力/卸载', () async {
    if (env == null) {
      markTestSkipped(
        '未找到原生插件框架库或示例插件，跳过（先跑 tools/build_examples.ps1（Windows）或 tools/build_examples.sh（Linux/macOS）产出原生库与示例插件）',
      );
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
    _step('1 init 完成');
    final UiContentProbe contentProbe = UiContentProbe(runtime, seen);
    expect(runtime.isRunning, isTrue);
    expect(runtime.libraryPath, env.libraryPath);
    // 库自报的版本要与 Dart 侧声明一致：不一致说明加载到的插件框架库不是这次构建的产物
    // （改了 C 头宏没重建库时，主程序页面上的版本与插件读到的版本会不是一个值）
    expect(
      runtime.libraryVersion,
      MusicxxPluginVersion.version,
      reason: '原生库自报的版本与 lib/src/version.dart 不一致，重新构建插件框架库',
    );

    // 事件泵：host.ready 一定在队列里（init 里已 pump 过一次）
    await _pumpUntil(
      () => seen.any(
        (MusicxxPluginEvent e) => e.type == MusicxxPluginEventType.hostReady,
      ),
    );
    final MusicxxPluginEvent ready = seen.firstWhere(
      (MusicxxPluginEvent e) => e.type == MusicxxPluginEventType.hostReady,
    );
    expect(ready.payload['apiVersion'], MusicxxPluginVersion.apiVersion);
    expect(ready.payload['platform'], MusicxxPluginRuntime.currentPlatform);

    _step('2 host.ready 已收到');
    // 扫描：应发现示例插件
    final List<MusicxxPluginInfo> found = await runtime.plugins.scan();
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
    // 的库文件放在分支目录里，扫描应报告选中的分支，加载的也应是那一份构建。
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
    await runtime.plugins.load('example_native_multi');
    expect(runtime.plugins.findLoaded('example_native_multi'), isNotNull);
    final Object? which = await runtime.plugins.call(
      'example_native_multi',
      'plugin.example_native_multi.which',
    );
    expect(which, isA<Map<String, Object?>>());
    expect(
      (which! as Map<String, Object?>)['tag'],
      multi.target,
      reason: '加载的应是插件框架选中的那个分支',
    );
    await runtime.plugins.unload('example_native_multi');

    // 安装预检入口（plugin_inspect）：与扫描同源，可指定目标环境
    final MusicxxPluginInspect_c inspected = await runtime.plugins.inspect(
      multi.path,
      os: MusicxxPluginEnv.currentOs(),
      arch: MusicxxPluginEnv.currentArch(),
    );
    expect(inspected.valid, isTrue, reason: inspected.error);
    expect(inspected.id, 'example_native_multi');
    expect(inspected.kind, MusicxxPluginKind.native);
    expect(inspected.supported, isTrue, reason: inspected.reason);
    expect(inspected.target, multi.target, reason: '预检与扫描选中的分支必须一致（原生侧同一份判定）');
    expect(inspected.targetEntry, multi.targetEntry);
    expect(inspected.targets.length, multi.targets.length);

    // 主程序安装预检的调用形状：**不传** os/arch（= 用插件框架当前环境）
    //
    // 回归：声明了 `platforms` 的插件曾被一律判成"当前平台不在清单声明内"，
    // 于是「从压缩包安装」永远失败 —— 示例插件几乎都声明了 platforms。
    final MusicxxPluginInfo? declared = found
        .where((MusicxxPluginInfo info) => info.id == 'example_js_net')
        .firstOrNull;
    expect(declared, isNotNull, reason: '扫描结果里应有 example_js_net');
    final MusicxxPluginInspect_c declaredByHost = await runtime.plugins.inspect(
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
      reason: '不传 os/arch 时按插件框架当前环境判定：${declaredByHost.reason}',
    );

    // 指定目标环境：示例包只有本机那一份构建，换个系统就应当选不到分支
    final String otherOs = MusicxxPluginEnv.currentOs() == 'windows'
        ? 'linux'
        : 'windows';
    final MusicxxPluginInspect_c mismatched = await runtime.plugins.inspect(
      multi.path,
      os: otherOs,
      arch: MusicxxPluginEnv.currentArch(),
    );
    expect(mismatched.supported, isFalse);
    expect(mismatched.reason, contains('没有匹配当前系统/架构的分支'));

    // 目录不存在：valid=false + 可读原因（不抛异常）
    final MusicxxPluginInspect_c missing = await runtime.plugins.inspect(
      '${multi.path}_missing',
    );
    expect(missing.valid, isFalse);
    expect(missing.error, isNotEmpty);

    _step('3 scan 完成: ${found.length} 项');
    // 加载：同步等待 + 事件回报
    await runtime.plugins.load('example_native');
    expect(runtime.plugins.findLoaded('example_native'), isNotNull);
    await _pumpUntil(
      () => seen.any(
        (MusicxxPluginEvent e) => e.type == MusicxxPluginEventType.pluginLoaded,
      ),
    );

    _step('4 load 完成');
    // 原生处理器位图：插件在 start 里注册了 3 个钩子
    expect(
      (await runtime.capabilities.refreshNativeHandlers(
        MusicxxCapabilityId.playerBeforePlaySong,
      ))!.length,
      1,
    );
    expect(
      runtime.capabilities.hasHandlers(
        MusicxxCapabilityId.playerBeforePlaySong,
      ),
      isTrue,
    );
    expect(
      runtime.capabilities.hasHandlers(MusicxxBroadcastId.playerPosition),
      isFalse,
    );

    _step('5 hook_count 完成');
    // 快照频道：Dart 发布 → 插件同步可读（探针里回报 stateLen）
    await runtime.broadcasts.publish(
      MusicxxBroadcastId.stateSong,
      <String, Object?>{'sid': 's-dart', 'name': 'Dart 推送的歌曲'},
    );

    _step('6 state 推送完成');
    // 能力调用：读取插件自检信息（线程归属 + 命名空间校验结果）
    final probeRaw = await runtime.plugins.call(
      'example_native',
      'plugin.example_native.probe',
      const <String, Object?>{},
    );
    expect(probeRaw, isA<Map<String, Object?>>());
    final Map<String, Object?> probe = probeRaw! as Map<String, Object?>;
    expect(
      probe['startThread'],
      equals(probe['callThread']),
      reason: '插件代码必须只在插件框架线程执行',
    );
    expect(
      probe['unknownCapabilityRc'],
      -200,
      reason: '应用没声明的官方能力必须被拒绝 (能力表返回 not_declared = -200)',
    );
    expect(
      probe['foreignActionRc'],
      -200,
      reason: '没声明过的动作名（能力）必须被拒绝 (not_declared)',
    );
    expect(probe['stateLen'], greaterThan(0), reason: '插件应能同步读到 Dart 推送的快照频道');

    _step('7 能力调用完成: $probeRaw');
    // 钩子派发（裁决型）：广告曲目 → cancel
    final Map<String, Object?>? verdict = await runtime.capabilities.dispatch(
      MusicxxCapabilityId.playerBeforePlaySong,
      <String, Object?>{
        'sid': 's-ad',
        'song': <String, Object?>{'name': '广告插曲 - 测试'},
      },
    );
    expect(verdict, isNotNull);
    expect(verdict!['action'], 'skip');
    // 普通曲目 → 无裁决
    expect(
      await runtime.capabilities.dispatch(
        MusicxxCapabilityId.playerBeforePlaySong,
        <String, Object?>{
          'sid': 's-normal',
          'song': <String, Object?>{'name': '普通歌曲'},
        },
      ),
      isNull,
    );

    _step('8 decide 完成');
    // 应用侧没有实现链: 裁决完全由插件的实现链给出（Dart 只提交请求、等结果）。
    // 同一条能力再来两次仍各有独立回执：一条命中广告、一条普通曲目。
    final Map<String, Object?>? repeatVerdict = await runtime.capabilities
        .dispatch(MusicxxCapabilityId.playerBeforePlaySong, <String, Object?>{
          'sid': 's-again-ad',
          'song': <String, Object?>{'name': '广告曲目 2'},
        });
    expect(repeatVerdict!['action'], 'skip', reason: '重复派发仍给出同一条裁决');
    expect(
      await runtime.capabilities.dispatch(
        MusicxxCapabilityId.playerBeforePlaySong,
        <String, Object?>{
          'sid': 's-again-normal',
          'song': <String, Object?>{'name': '普通歌曲'},
        },
      ),
      isNull,
    );

    // 广播型 id：只跑处理器、不回结果（要顺序就 await，这条确认不影响播放）
    await runtime.capabilities.notify(
      MusicxxBroadcastId.songChanged,
      <String, Object?>{
        'sid': 's-ad',
        'song': <String, Object?>{'name': '普通歌曲'},
      },
    );
    await _pumpUntil(() => runtime.recentEvents.isNotEmpty);

    _step('9 裁决与通知完成');
    // 禁用 → 原生处理器位图清零；启用 → 恢复
    await runtime.plugins.disable('example_native');
    expect(
      (await runtime.capabilities.refreshNativeHandlers(
        MusicxxCapabilityId.playerBeforePlaySong,
      ))!.length,
      0,
    );
    await runtime.plugins.enable('example_native');
    expect(
      (await runtime.capabilities.refreshNativeHandlers(
        MusicxxCapabilityId.playerBeforePlaySong,
      ))!.length,
      1,
    );

    _step('10 enable/disable 完成');
    // 卸载：幂等（第二次仍成功），且能力调用失败
    await runtime.plugins.unload('example_native');
    await runtime.plugins.unload('example_native');
    expect(runtime.plugins.findLoaded('example_native'), isNull);
    expect(
      () async => await runtime.plugins.call(
        'example_native',
        'plugin.example_native.probe',
      ),
      throwsA(isA<MusicxxPluginApiException>()),
    );

    _step('11 unload 完成');

    // ===== 声明式 UI 扩展 =====
    // 重新加载原生示例插件, 读取 UI 项快照 (入口项 / 菜单项 / 顺序 / 动作描述)
    await runtime.plugins.load('example_native');
    final List<MusicxxPluginUIItem> uiItems = await runtime.plugins
        .uiSnapshot();
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
    expect(card.contentId, 'card');
    expect(
      songActions.any(
        (MusicxxPluginUIItem item) => item.plugin == 'example_native',
      ),
      isTrue,
      reason: '快照: $songActions',
    );

    _step('11.5 UI 项快照完成: ${uiItems.length} 项');
    // 页面是 `musicxx.ui.page` 上的内容：入口只声明"打开哪个内容"，页面本身由插件按需生成。
    // 这里断言"声明了 route 入口的插件确实注册了同名内容"——只声明入口不给内容时，
    // 主程序打开页面只能拿到一份空内容。
    final String? nativePageId = card.contentId;
    expect(nativePageId, isNotNull);
    final Map<String, Object?> nativeView = await contentProbe.jsonOf(
      'plugin.example_native.$nativePageId',
    );
    expect(nativeView['title'], isNotEmpty);
    expect(nativeView['blocks'], isA<List<Object?>>());
    expect((nativeView['blocks']! as List<Object?>).length, greaterThan(1));

    _step('11.6 原生示例的页面内容完成');
    // 卸载后 UI 项被摘除 (插件框架随实例摘除, Dart 侧不再渲染)
    await runtime.plugins.unload('example_native');
    expect(
      (await runtime.plugins.uiSnapshot()).any(
        (MusicxxPluginUIItem item) => item.plugin == 'example_native',
      ),
      isFalse,
      reason: '卸载后不应残留该插件的 UI 项',
    );

    _step('11.7 卸载后 UI 项摘除完成');

    // ===== 卸载 = 释放动态库：插件目录要能删 =====
    //
    // 管理页的「彻底卸载」= 卸载实例 → 删除插件目录；插件框架没关掉动态库句柄时目录一直
    // 被占着，删除只会拿到"拒绝访问"。这里用一份可自由删除的插件副本走同一条路。
    {
      final Directory copyRoot = Directory.systemTemp.createTempSync(
        'mx-unload-',
      );
      final Directory copyDir = Directory('${copyRoot.path}/example_native');
      addTearDown(() {
        try {
          copyRoot.deleteSync(recursive: true);
        } catch (_) {}
      });
      _copyDirectory(Directory('${env.pluginRoot}/example_native'), copyDir);
      await runtime.plugins.load(copyDir.path);
      expect(runtime.plugins.findLoaded('example_native'), isNotNull);
      await runtime.plugins.unload('example_native');
      expect(runtime.plugins.findLoaded('example_native'), isNull);
      copyDir.deleteSync(recursive: true);
      expect(copyDir.existsSync(), isFalse, reason: '卸载后插件目录应能删除');
    }

    _step('11.8 卸载后插件目录可删');

    // ===== JS 插件 (零编译) =====
    // 扫描结果里应有 JS 示例插件
    final List<MusicxxPluginInfo> found2 = await runtime.plugins.scan();
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
    // 扫描结果带脚本清单（管理页展示；示例插件按清单的 scripts 顺序加载 kit 与脚本）
    expect(exampleJs.scripts, <String>[
      'pluginxx_ui_kit.js',
      'musicxx_ui_kit.js',
      'plugin.js',
    ]);

    _step('12 scan 发现 JS 插件');
    await runtime.plugins.load('example_js');
    expect(
      runtime.plugins.findLoaded('example_js'),
      isNotNull,
      reason: 'JS 插件实例应已加载',
    );
    expect(
      (await runtime.capabilities.refreshNativeHandlers(
        MusicxxCapabilityId.playerBeforePlaySong,
      ))!.length,
      1,
    );

    _step('13 JS 插件加载完成');
    // JS插件的 UI 项 (顶层注册 → 插件框架线程回放) 同样进快照
    final List<MusicxxPluginUIItem> uiItemsJs = await runtime.plugins
        .uiSnapshot();
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
    final Map<String, Object?>? jsVerdict = await runtime.capabilities.dispatch(
      MusicxxCapabilityId.playerBeforePlaySong,
      <String, Object?>{
        'sid': 's-js-ad',
        'song': <String, Object?>{'name': '广告插曲 - JS'},
      },
    );
    expect(jsVerdict, isNotNull);
    expect(jsVerdict!['action'], 'skip');
    expect(
      await runtime.capabilities.dispatch(
        MusicxxCapabilityId.playerBeforePlaySong,
        <String, Object?>{
          'sid': 's-js-normal',
          'song': <String, Object?>{'name': '普通歌曲 JS'},
        },
      ),
      isNull,
    );

    _step('14 JS decide 完成');
    // 异步派发同样适用于 JS 处理器（插件框架在自己的线程上等脚本，Dart 线程不阻塞）
    final Map<String, Object?>? jsAsyncVerdict = await runtime.capabilities
        .dispatch(MusicxxCapabilityId.playerBeforePlaySong, <String, Object?>{
          'sid': 's-js-async-ad',
          'song': <String, Object?>{'name': '广告插曲 - JS 异步'},
        });
    expect(jsAsyncVerdict, isNotNull);
    expect(jsAsyncVerdict!['action'], 'skip');

    _step('14.5 JS 异步裁决完成');
    // JS 前链处理器返回 Promise: 插件框架一直等到脚本结算, 结算后改写生效
    // （“改播放值”的四个能力实现固定, 插件只能注册前链 / 后链）
    final Map<String, Object?>? jsPromiseVerdict = await runtime.capabilities
        .dispatch(MusicxxCapabilityId.playerSpeed, <String, Object?>{
          'sid': 's-js-speed',
          'from': 1.0,
          'to': 8.0,
        }, MusicxxCapabilityPhase.before);
    expect(jsPromiseVerdict, isNotNull, reason: 'example_js 的异步裁决应生效');
    // 前链的 patch 合进结果对象里（`result.patch.to`）
    final Map<String, Object?> jsPromisePatch =
        (jsPromiseVerdict!['patch'] as Map<Object?, Object?>?)
            ?.cast<String, Object?>() ??
        const <String, Object?>{};
    expect((jsPromisePatch['to'] as num?)?.toDouble(), 3.0);
    // 同一个处理器在"不需要裁决"的分支上同步返回 null → 没有裁决
    expect(
      await runtime.capabilities.dispatch(
        MusicxxCapabilityId.playerSpeed,
        <String, Object?>{'sid': 's-js-speed-ok', 'from': 1.0, 'to': 1.5},
        MusicxxCapabilityPhase.before,
      ),
      isNull,
    );

    _step('14.6 JS Promise 裁决完成');
    // 观察型通知：现在是广播频道，应用发布、插件订阅（框架 API 2）
    await runtime.broadcasts.publish('musicxx.song.changed', <String, Object?>{
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

    _step('15 JS 广播订阅执行完成');
    // 能力探针: JS 侧同步读快照频道 + 插件框架信息
    final jsProbeRaw = await runtime.plugins.call(
      'example_js',
      'plugin.example_js.probe',
      const <String, Object?>{},
    );
    expect(jsProbeRaw, isA<Map<String, Object?>>());
    final Map<String, Object?> jsProbe = jsProbeRaw! as Map<String, Object?>;
    expect(jsProbe['pluginId'], 'example_js');
    expect(jsProbe['hostPlatform'], MusicxxPluginRuntime.currentPlatform);

    // 异步能力处理器 (返回 Promise): 插件框架等到它结算再给调用方结果
    final Object? slowRaw = await runtime.plugins.call(
      'example_js',
      'plugin.example_js.slowProbe',
      const <String, Object?>{'waitMs': 250},
    );
    expect(slowRaw, isA<Map<String, Object?>>(), reason: '异步能力应有结果');
    final Map<String, Object?> slow = slowRaw! as Map<String, Object?>;
    expect(slow['ok'], true);
    expect(slow['waitedMs'], 250);
    expect(slow['pluginId'], 'example_js');
    expect(jsProbe['currentSongName'], 'Dart 推送的歌曲', reason: 'JS 应能同步读到快照频道');
    expect(jsProbe['songChangedCount'], greaterThan(0));

    _step('16 JS 能力调用完成');
    // JS 示例的主页入口指向 ext://example_js/card：页面是 `musicxx.ui.page` 上的内容，
    // 入口只声明"打开哪个内容"。这里断言这条路径真的通（只声明入口不给内容时，页面是空的）。
    final List<MusicxxPluginUIItem> jsHomeEntries = MusicxxPluginUIItems.byType(
      uiItemsJs,
      MusicxxPluginUIType.homeEntry,
    );
    final MusicxxPluginUIItem jsCard = jsHomeEntries.firstWhere(
      (MusicxxPluginUIItem item) => item.plugin == 'example_js',
    );
    expect(jsCard.contentId, isNotNull);
    final Map<String, Object?> jsView = await contentProbe.jsonOf(
      'plugin.example_js.${jsCard.contentId}',
    );
    expect(jsView['title'], isNotEmpty);
    expect(jsView['blocks'], isA<List<Object?>>());
    expect((jsView['blocks']! as List<Object?>).length, greaterThan(1));

    _step('16.5 JS 示例的页面内容完成');
    // 禁用/启用: JS 脚本随 stop/start 重跑
    await runtime.plugins.disable('example_js');
    expect(
      (await runtime.capabilities.refreshNativeHandlers(
        MusicxxCapabilityId.playerBeforePlaySong,
      ))!.length,
      0,
    );
    await runtime.plugins.enable('example_js');
    expect(
      (await runtime.capabilities.refreshNativeHandlers(
        MusicxxCapabilityId.playerBeforePlaySong,
      ))!.length,
      1,
    );

    _step('17 JS enable/disable 完成');
    // 卸载: JS 实例与注册全部摘除, 槽位保留在插件框架内置表里
    await runtime.plugins.unload('example_js');
    expect(runtime.plugins.findLoaded('example_js'), isNull);
    expect(
      () async =>
          await runtime.plugins.call('example_js', 'plugin.example_js.probe'),
      throwsA(isA<MusicxxPluginApiException>()),
    );

    _step('18 JS unload 完成');

    // 播放页背景示例 example_js_playing_bg（六种背景）+ 播放页歌曲图接管项
    // （`musicxx.ui.playing.icon`）。它顶层还注册了 `musicxx.media.palette.analyse`
    // 取色钩子 —— 这几条都能挡住"忘了重建插件框架库"（旧插件框架库会以未知槽位 /
    // 未知钩子拒绝注册）。
    if (Directory('${env.pluginRoot}/example_js_playing_bg').existsSync()) {
      final MusicxxPluginInfo? imagePlugin = found2
          .where((MusicxxPluginInfo info) => info.id == 'example_js_playing_bg')
          .firstOrNull;
      expect(imagePlugin, isNotNull);
      expect(imagePlugin!.kind, MusicxxPluginKind.js);
      await runtime.plugins.load('example_js_playing_bg');
      expect(
        (await runtime.capabilities.refreshNativeHandlers(
          MusicxxCapabilityId.mediaPaletteAnalyse,
        ))!.length,
        1,
        reason: '取色钩子的处理器要在插件框架侧登记成功（插件框架库没重建时这里会是 0）',
      );

      // 六种样式 = 同一个槽位里的六份内容：候选列表读注册（名字 / 说明 / 顺序），
      // 画面读生成出来的块树。注册顺序决定设置列表里的先后：封面三种在前，图案三种在后。
      final List<MusicxxPluginUIContent> backgrounds = await contentProbe
          .registrationsOf(
            'example_js_playing_bg',
            slot: 'musicxx.ui.playing.background',
          );
      expect(backgrounds.length, 6, reason: '六种背景样式各注册一份内容: $backgrounds');
      expect(
        backgrounds.map((MusicxxPluginUIContent item) => item.shortId).toList(),
        <String>['bg', 'bgCard', 'bgVinyl', 'lattice', 'ring', 'bound'],
        reason: '注册顺序：封面三种在前，图案三种在后',
      );
      expect(
        backgrounds.first.title,
        isNotEmpty,
        reason: '用户要在选中之前就看到候选名字（注册时就给）',
      );

      /// 样式内容里的着色器块（背景的根块是 `Expanded`，着色器在它的 child 里）
      Future<Map<String, Object?>?> shaderOf(String shortId) async =>
          _shaderBlockOf(
            await contentProbe.jsonOf('plugin.example_js_playing_bg.$shortId'),
          );

      /// 着色器块里的一个成员（纹理声明 / 值表达式）
      Future<Map<String, Object?>> memberOf(
        String shortId,
        String member,
      ) async {
        final Map<String, Object?>? shader = await shaderOf(shortId);
        final Object? args = shader?['args'];
        expect(args, isA<Map<String, Object?>>(), reason: '$shortId 缺少 args');
        final Object? value = (args! as Map<String, Object?>)[member];
        expect(
          value,
          isA<Map<String, Object?>>(),
          reason: '$shortId.$member 缺失（成员名 = 采样器名）',
        );
        return value! as Map<String, Object?>;
      }

      expect(
        (await shaderOf('bg'))?['bundle'],
        'shader/heat.shaderbundle',
        reason: '第一个是『模糊』',
      );
      expect(
        (await memberOf('bg', 'uCover'))['blur'],
        14,
        reason: '封面纹理的预模糊强度由插件声明（默认 14，设置页可在 0~30 之间调）',
      );
      expect(
        (await shaderOf('bgCard'))?['bundle'],
        'shader/card.shaderbundle',
        reason: '第二个是『沉浸』（清晰封面 + 渐变底 + 亮暗遮罩）',
      );
      expect(
        (await memberOf('bgCard', 'uCover')).containsKey('blur'),
        isFalse,
        reason: '『沉浸』的封面卡片要是原图，不做预模糊（类声明不写 blur）',
      );
      expect(
        (await shaderOf('bgVinyl'))?['bundle'],
        'shader/vinyl.shaderbundle',
        reason: '第三个是『黑胶』（唱盘 + 圆裁封面 + 唱臂）',
      );
      expect(
        (await memberOf('bgVinyl', 'uCover'))['square'],
        true,
        reason: '『黑胶』的盘心是圆裁的封面：纹理要中心裁剪成正方形',
      );
      expect(
        (await shaderOf('bgVinyl'))?['speed'],
        1,
        reason: '『黑胶』的转速由块的 speed 决定（默认 1×；改转速不用重新选中该样式）',
      );
      expect(
        (await shaderOf('lattice'))?['bundle'],
        'shader/bg.shaderbundle',
        reason: '第四个是『晶格』（模糊封面铺底 + 晶格图案）',
      );
      expect(
        (await shaderOf('lattice'))?['speed'],
        1,
        reason: '图案样式默认 1× = 基准速度 1',
      );
      expect(
        (await shaderOf('ring'))?['bundle'],
        'shader/ring.shaderbundle',
        reason: '第五个是『光圈』（频谱圆环）',
      );
      expect(
        (await memberOf('bound', 'uCoverBack'))['source'],
        'musicxx.image.coverArt',
        reason: '第六个是『插件纹理铺底』：底层用插件绑定的图片（musicxx.image.<key>）',
      );

      // 歌曲图接管项也是内容：不画东西、保留占位（`data.keepSpace`）
      final UiContentResult icon = await contentProbe.request(
        'plugin.example_js_playing_bg.icon',
      );
      expect(
        icon.data['keepSpace'],
        true,
        reason: '接管歌曲图 = 只留位置不画内容: ${icon.payload}',
      );
      expect(icon.json['blocks'], isEmpty, reason: '不画内容：块树是空的');

      // 取色钩子的处理器必须**同步**返回（不能在里面 await 主程序动作）：主程序是在需要颜色的
      // 时候等这次裁决的（切换背景样式、切歌、渲染准备都在 await 这条链上），处理器里等动作
      // 会把两边互相拖住 —— 实测表现是"启用插件后切歌 / 换背景样式卡几秒"。
      final Stopwatch paletteWatch = Stopwatch()..start();
      final Map<String, Object?>? paletteVerdict = await runtime.capabilities
          .dispatch(MusicxxCapabilityId.mediaPaletteAnalyse, <String, Object?>{
            'srcKey': 'test-key',
            'name': '测试歌曲',
            'artist': '测试歌手',
            'hasCover': true,
            'night': false,
          });
      paletteWatch.stop();
      expect(
        paletteWatch.elapsedMilliseconds,
        lessThan(300),
        reason: '取色钩子的处理器要同步返回：在里面等主程序动作会让主程序一直等到超时',
      );
      expect(
        paletteVerdict,
        isNull,
        reason: '没命中取色缓存时返回 null（应用走内置快速取色，算好后用 setPalette 写回）',
      );

      // 设置页是 `musicxx.ui.page` 上的内容（生成函数同步给出块树，渲染在主程序侧）
      final Map<String, Object?> imageView = await contentProbe.jsonOf(
        'plugin.example_js_playing_bg.settings',
      );
      expect(imageView['title'], isNotEmpty);
      expect(imageView['blocks'], isA<List<Object?>>());
      // 设置页要有「自动调整前景色」的开关与状态行（插件按封面主色 + 亮暗主题推前景三色）
      final List<String> settingsText = <String>[];
      _collectText(imageView, settingsText);
      expect(
        settingsText.any((String text) => text.contains('自动调整前景色')),
        isTrue,
        reason: '设置页缺少自动前景色的开关/状态行: $settingsText',
      );
      // 状态行报的是"已关闭 / 读取中 / 等待封面取色"其中之一（测试环境里没有封面与昼夜）
      expect(
        settingsText.any(
          (String text) =>
              text.contains('已关闭') ||
              text.contains('读取中') ||
              text.contains('等待封面取色'),
        ),
        isTrue,
        reason: '设置页没有自动前景色的状态文本: $settingsText',
      );
      // 说明页（主页入口指向的页面）同样是内容
      final Map<String, Object?> imageCard = await contentProbe.jsonOf(
        'plugin.example_js_playing_bg.card',
      );
      expect(imageCard['title'], isNotEmpty);
      expect(imageCard['blocks'], isA<List<Object?>>());

      // 设置项：控件把自己的 id 与值交回来（`args.id` / `args.value`），插件写 config.json
      // 并让背景内容重出一份。动作只改状态（不返回视图），生成函数也必须同步返回。
      await _callAction(
        runtime,
        'example_js_playing_bg',
        'setOption',
        <String, Object?>{'id': 'zoom', 'value': 2.5, 'view': 'settings'},
      );
      expect(
        (await contentProbe.jsonOf(
          'plugin.example_js_playing_bg.settings',
        ))['blocks'],
        isA<List<Object?>>(),
        reason: '改设置后页面能重新生成（不能在一次请求里等异步动作）',
      );
      // 改回默认值，别把测试写下的配置留给下一次运行
      await _callAction(
        runtime,
        'example_js_playing_bg',
        'setOption',
        <String, Object?>{'id': 'zoom', 'value': 2},
      );
      // 『黑胶』的唱片转速：同一个入口，改完让内容重出一份 —— 新速度立刻生效
      // （转速走块的 `speed`，运行期不用重新选中该样式）
      await _callAction(
        runtime,
        'example_js_playing_bg',
        'setOption',
        <String, Object?>{'id': 'spin', 'value': 2, 'view': 'settings'},
      );
      expect(
        (await shaderOf('bgVinyl'))?['speed'],
        2,
        reason: '改「唱片转速」要让内容重出一份（把新速度写进块的 speed）',
      );
      await _callAction(
        runtime,
        'example_js_playing_bg',
        'setOption',
        <String, Object?>{'id': 'spin', 'value': 1},
      );
      // 图案样式的动画速率是独立开关（bgRate）：改完写进『晶格』的 speed
      await _callAction(
        runtime,
        'example_js_playing_bg',
        'setOption',
        <String, Object?>{'id': 'bgRate', 'value': 2, 'view': 'settings'},
      );
      expect(
        (await shaderOf('lattice'))?['speed'],
        2,
        reason: '背景动画速率 2x = 基准速度 1 × 2',
      );
      await _callAction(
        runtime,
        'example_js_playing_bg',
        'setOption',
        <String, Object?>{'id': 'bgRate', 'value': 1},
      );
      await runtime.plugins.unload('example_js_playing_bg');
      _step('18.7 示例播放页背景插件完成');
    }

    // 播放页插槽示例 example_js_playing_slots：位置上的界面全是内容
    // （插入块 / 控制栏 / 音频动效 / 普通页面背景），声明项只剩主页入口。
    if (Directory('${env.pluginRoot}/example_js_playing_slots').existsSync()) {
      await runtime.plugins.load('example_js_playing_slots');
      final Object? slotsProbe = await runtime.plugins.call(
        'example_js_playing_slots',
        'plugin.example_js_playing_slots.probe',
        const <String, Object?>{},
      );
      expect(slotsProbe, isA<Map<String, Object?>>());
      final Map<String, Object?> slotsInfo =
          slotsProbe! as Map<String, Object?>;
      expect(
        _intOf(slotsInfo['uiEntries']),
        1,
        reason: '声明项只剩主页入口（位置上的界面全是内容）',
      );
      expect(
        _intOf(slotsInfo['uiContents']),
        9,
        reason: '内容 9 份 = 3 个插入 + 2 个按钮 + 1 个动效 + 1 个页面背景 + 2 个页面',
      );
      expect(_intOf(slotsInfo['hasControlBar']), 1);
      expect(_intOf(slotsInfo['visualizer']), 1);
      expect(_intOf(slotsInfo['pageBackground']), 1);

      // 位置上的界面只能由内容填充：插件用 `item.register` 往这些槽位注册声明项会被拒绝，
      // 所以声明项快照里没有它们（主程序也不按声明项画这些位置）。
      final List<MusicxxPluginUIItem> slotItems =
          (await runtime.plugins.uiSnapshot())
              .where(
                (MusicxxPluginUIItem item) =>
                    item.plugin == 'example_js_playing_slots',
              )
              .toList();
      for (final String type in <String>[
        'musicxx.ui.playing.lyric.top',
        'musicxx.ui.playing.songlist.header',
        'musicxx.ui.playing.moreInfo.footer',
        'musicxx.ui.playing.controlBar',
        'musicxx.ui.playing.controlBar.menu',
        'musicxx.ui.playing.controlBar.favorite',
        'musicxx.ui.playing.visualizer',
        'musicxx.ui.page.background',
      ]) {
        expect(
          slotItems.any((MusicxxPluginUIItem item) => item.type == type),
          isFalse,
          reason: '$type 只能由内容填充（声明项不进快照）',
        );
      }

      // 内容注册表：三个插入位置 / 两个按钮 / 动效 / 页面背景 / 两个页面
      final List<MusicxxPluginUIContent> registrations = await contentProbe
          .registrationsOf('example_js_playing_slots');
      final Set<String> registeredSlots = <String>{
        for (final MusicxxPluginUIContent item in registrations) item.slot,
      };
      expect(
        registeredSlots,
        containsAll(<String>[
          'musicxx.ui.playing.lyric.top',
          'musicxx.ui.playing.songlist.header',
          'musicxx.ui.playing.moreInfo.footer',
          'musicxx.ui.playing.controlBar.menu',
          'musicxx.ui.playing.controlBar.favorite',
          'musicxx.ui.playing.visualizer',
          'musicxx.ui.page.background',
          'musicxx.ui.page',
        ]),
        reason: '注册表里的槽位: $registeredSlots',
      );
      final MusicxxPluginUIContent pageBackground = registrations.firstWhere(
        (MusicxxPluginUIContent item) =>
            item.slot == 'musicxx.ui.page.background',
      );
      expect(
        pageBackground.routes,
        contains('/songs'),
        reason: '页面背景的适用页面只能注册时给（内容还没生成）',
      );

      // 整栏替换：开启后多出一份 `wholeBar` 内容（按钮级内容保留，主程序会忽略它们）
      await _callAction(
        runtime,
        'example_js_playing_slots',
        'toggleWholeBar',
        <String, Object?>{'view': 'settings'},
      );
      expect(
        (await contentProbe.registrationsOf(
          'example_js_playing_slots',
          slot: 'musicxx.ui.playing.controlBar',
        )).map((MusicxxPluginUIContent item) => item.shortId),
        contains('wholeBar'),
        reason: '整栏替换是一份内容（开启后进注册表）',
      );

      // 整栏内容：主程序带着依赖变量来要，生成函数同步给出块描述
      final Map<String, Object?> barView = await contentProbe.jsonOf(
        'plugin.example_js_playing_slots.wholeBar',
        context: <String, Object?>{
          'musicxx.player.playing': true,
          'musicxx.song.hasSong': true,
          'musicxx.song.favorite': true,
        },
      );
      expect(barView['blocks'], isA<List<Object?>>());
      final List<String> barTexts = <String>[];
      _collectText(barView, barTexts);
      expect(barTexts, isNotEmpty);

      // 关回去（config.json 恢复到默认值）
      await _callAction(
        runtime,
        'example_js_playing_slots',
        'toggleWholeBar',
        <String, Object?>{'view': 'settings'},
      );
      await runtime.plugins.unload('example_js_playing_slots');
      _step('18.8 播放页插槽示例完成');
    }

    // 统计快照可读
    final Map<String, Object?> stats = await runtime.plugins.stats();
    expect(stats['host'], isA<Map<String, Object?>>());
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('能力调用：异步受理、超时丢弃、停机收尾（不悬挂）', () async {
    if (env == null || !env.hasPlugin('example_js')) {
      markTestSkipped(
        '未找到原生插件框架库或示例插件，跳过（先跑 tools/build_examples.ps1（Windows）或 tools/build_examples.sh（Linux/macOS）产出原生库与示例插件）',
      );
      return;
    }
    final MusicxxPluginRuntime runtime = MusicxxPluginRuntime.create();
    addTearDown(runtime.dispose);
    await runtime.init(
      config: MusicxxPluginRuntimeConfig(
        appVersion: '0.0.0-test',
        platform: MusicxxPluginRuntime.currentPlatform,
        language: 'zh-cn',
        userPluginDir: env.pluginRoot,
      ),
      libraryPath: env.libraryPath,
    );
    await runtime.plugins.scan();
    await runtime.plugins.load('example_js');

    // 现在只有一种调用形态: 提交 + 一条结果事件; 受理本身是同步的判定
    expect(runtime.requests.pendingCount, 0);

    // 1) 正常路径：异步完成的处理器（返回 Promise）也能等到结果
    final Object? slow = await runtime.plugins.call(
      'example_js',
      'plugin.example_js.slowProbe',
      const <String, Object?>{'waitMs': 120},
    );
    expect(slow, isA<Map<String, Object?>>());
    expect((slow! as Map<String, Object?>)['waitedMs'], 120);
    expect(runtime.requests.pendingCount, 0, reason: '结果回来后不再有等待中的调用');

    // 2) 调用方自己的超时：到点抛超时错误，晚到的结果丢弃（不悬挂）
    await expectLater(
      runtime.plugins.call(
        'example_js',
        'plugin.example_js.slowProbe',
        const <String, Object?>{'waitMs': 800},
        const Duration(milliseconds: 100),
      ),
      throwsA(isA<MusicxxPluginApiException>()),
    );
    expect(runtime.requests.pendingCount, 0, reason: '超时后清掉等待中的调用');
    // 晚到的结果到达时没有等待方：按丢弃处理（不抛、不串台）
    await _pumpUntil(() => true, timeout: const Duration(seconds: 1));

    // 3) 受理失败：未加载的插件直接报未找到
    await expectLater(
      runtime.plugins.call('no_such_plugin', 'probe'),
      throwsA(isA<MusicxxPluginApiException>()),
    );

    // 4) 插件框架停机：还没回来的调用不能被悬挂（由 handleDisposed 收尾）
    final Future<Object?> pending = runtime.plugins.call(
      'example_js',
      'plugin.example_js.slowProbe',
      const <String, Object?>{'waitMs': 5000},
    );
    expect(runtime.requests.pendingCount, 1);
    runtime.dispose();
    // 断言结果码而不是文案：原生消息是英文的框架级诊断，文案会跟着语言改，码不会
    await expectLater(
      pending,
      throwsA(
        isA<MusicxxPluginApiException>().having(
          (MusicxxPluginApiException e) => e.code,
          'code',
          -2,
        ),
      ),
    );
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('广播：发布 / 读当前值 / 列频道 / 订阅数（对真实原生库）', () async {
    if (env == null || !env.hasPlugin('example_js_broadcast')) {
      markTestSkipped(
        '未找到原生插件框架库或广播示例插件，跳过（先跑 tools/build_examples.ps1（Windows）或 tools/build_examples.sh（Linux/macOS）产出原生库与示例插件）',
      );
      return;
    }
    final MusicxxPluginRuntime runtime = MusicxxPluginRuntime.create();
    addTearDown(runtime.dispose);

    // 订阅数回调（事件驱动）：应用据此决定"还要不要继续发这个频道"
    final Map<String, int> subscriberCounts = <String, int>{};
    runtime.broadcasts.onSubscribers = (String id, int count) {
      subscriberCounts[id] = count;
    };

    await runtime.init(
      config: MusicxxPluginRuntimeConfig(
        appVersion: '0.0.0-test',
        platform: MusicxxPluginRuntime.currentPlatform,
        language: 'zh-cn',
        userPluginDir: env.pluginRoot,
      ),
      libraryPath: env.libraryPath,
    );

    // 旧插件框架库诊断：本次构建必须带广播位
    expect(runtime.featureBits, isNotNull);
    expect(runtime.missingFeatures, isEmpty, reason: runtime.featureHint);

    // 1) 加载广播示例（顶层声明自己的频道 + 订阅官方频道）
    await runtime.plugins.load('example_js_broadcast');

    Map<String, Object?> probeCache = <String, Object?>{};
    Future<void> refreshProbe() async {
      final Object? raw = await runtime.plugins.call(
        'example_js_broadcast',
        'probe',
      );
      probeCache = raw is Map
          ? raw.cast<String, Object?>()
          : <String, Object?>{};
    }

    Map<String, Object?> probe() => probeCache;

    // 2) 插件看到的接口代次按它自己的形态报：JS 插件看到 JS 接口代次（当前 3）
    await refreshProbe();
    expect(probe()['apiVersion'], 3);

    // 3) 插件声明的频道进了广播表，声明字段如实
    final List<Map<String, Object?>> own = await runtime.broadcasts.list(
      prefix: 'plugin.example_js_broadcast',
    );
    final List<Object?> ownIds = own
        .map((Map<String, Object?> e) => e['id'])
        .toList();
    expect(ownIds, contains('plugin.example_js_broadcast.tip.start'));
    expect(ownIds, contains('plugin.example_js_broadcast.stats.reads'));
    final Map<String, Object?> tip = own.firstWhere(
      (Map<String, Object?> e) =>
          e['id'] == 'plugin.example_js_broadcast.tip.start',
    );
    expect(tip['cache'], 'latest');
    expect(tip['publish'], 'owner');
    expect(tip['owner'], 'example_js_broadcast');
    expect(tip['hasValue'], isTrue);

    // 声明时就发布过一次初始值：主程序同步读得到
    expect(
      await runtime.broadcasts.read('plugin.example_js_broadcast.tip.start'),
      false,
    );

    // 4) 插件自己改值（publish）→ 主程序读到的当前值跟着变
    await runtime.plugins.call(
      'example_js_broadcast',
      'publishTip',
      <String, Object?>{'value': true},
    );
    expect(
      await runtime.broadcasts.read('plugin.example_js_broadcast.tip.start'),
      true,
    );

    // 5) 插件订阅了官方频道：主程序发布后被叫到（订阅真的收到了）
    expect(
      await runtime.broadcasts.publish('musicxx.ui.animatedLevel', 3),
      isTrue,
    );
    // 同一个值再发一次：包层去重，不再发
    expect(
      await runtime.broadcasts.publish('musicxx.ui.animatedLevel', 3),
      isFalse,
    );
    await _pumpUntil(() async {
      await refreshProbe();
      return _intOf(probe()['changes']) >= 1;
    });
    final Map<String, Object?> lastChange =
        (probe()['lastChange'] as Map<Object?, Object?>?)
            ?.cast<String, Object?>() ??
        <String, Object?>{};
    expect(lastChange['value'], 3);

    // 6) 插件订阅官方频道时，应用收到订阅数变化（这里同时验证事件载荷）
    await _pumpUntil(() async {
      runtime.pumpEvents();
      return subscriberCounts.containsKey('musicxx.ui.animatedLevel');
    });
    expect(
      subscriberCounts['musicxx.ui.animatedLevel'],
      greaterThanOrEqualTo(1),
      reason: '插件订阅后应用应当知道这个频道有人听',
    );

    // 7) 高频频道按订阅方声明的频率到达（示例给的是 500 ms / delay）
    expect(
      await runtime.broadcasts.publish(
        'musicxx.player.position',
        <String, Object?>{'positionMs': 1000, 'durationMs': 200000},
        skipIfUnchanged: false,
      ),
      isTrue,
    );
    await _pumpUntil(() async {
      await refreshProbe();
      return _intOf(probe()['positionNotices']) >= 1;
    });
    expect(_intOf(probe()['positionNotices']), greaterThanOrEqualTo(1));

    // 8) 不存在的频道：读不到值（不抛异常，也不返回旧值）
    expect(await runtime.broadcasts.read('musicxx.test.nope'), isNull);

    // 9) 卸载后插件频道随实例摘除
    expect(
      (await runtime.broadcasts.list(
        prefix: 'plugin.example_js_broadcast',
      )).isNotEmpty,
      isTrue,
      reason: '卸载前应当能看到插件频道',
    );
    await runtime.plugins.unload('example_js_broadcast');
    expect(
      await runtime.broadcasts.list(prefix: 'plugin.example_js_broadcast'),
      isEmpty,
    );
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

/// 块树里的着色器块
///
/// 背景的根块是 `Expanded`（不包的话着色器拿不到确定高度），着色器块在它的 `child` 里；
/// 以后换成 `Column` 套 `Expanded` 也能沿这条链找下去。
Map<String, Object?>? _shaderBlockOf(Object? node) {
  for (int depth = 0; depth < 4 && node is Map; ++depth) {
    if (node['kind'] == 'musicxx.Shader') {
      return node.cast<String, Object?>();
    }
    final Object? child = node['child'];
    if (child is Map) {
      node = child;
      continue;
    }
    final Object? children = node['children'];
    if (children is! List || children.isEmpty) {
      return null;
    }
    node = children.first;
  }
  return null;
}

/// 调一个"只改状态"的插件能力
///
/// 动作不再返回视图：改完状态由插件调 `musicxx.ui.slot.update`，主程序让那一份内容重出一份。
/// 这里只确认动作受理并按约定返回 `{ok: true}`；要看结果再用 [UiContentProbe] 要一份内容。
Future<void> _callAction(
  MusicxxPluginRuntime runtime,
  String plugin,
  String method, [
  Map<String, Object?> args = const <String, Object?>{},
]) async {
  final Object? raw = await runtime.plugins.call(
    plugin,
    'plugin.$plugin.$method',
    args,
  );
  expect(raw, <String, Object?>{'ok': true}, reason: '$method 应返回 ok');
}

/// 递归收集界面描述里的文本（断言"这一页里有没有某段文案"用）
void _collectText(Object? node, List<String> out) {
  if (node is String) {
    out.add(node);
    return;
  }
  if (node is Map) {
    for (final Object? value in node.values) {
      _collectText(value, out);
    }
    return;
  }
  if (node is List) {
    for (final Object? value in node) {
      _collectText(value, out);
    }
  }
}

/// 递归复制目录（用例里用来做一份可以随便删的插件副本）
void _copyDirectory(Directory from, Directory to) {
  to.createSync(recursive: true);
  for (final FileSystemEntity item in from.listSync()) {
    final String name = item.uri.pathSegments
        .where((String segment) => segment.isNotEmpty)
        .last;
    if (item is Directory) {
      _copyDirectory(item, Directory('${to.path}/$name'));
    } else if (item is File) {
      item.copySync('${to.path}/$name');
    }
  }
}

/// 等待条件成立（最多 5 秒；每 20 ms 泵一次事件并让出事件循环）
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
