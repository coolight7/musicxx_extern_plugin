/// 整套流程测试：示例插件（JS）的**配置读写**在真实原生插件框架上的验证
///
/// 覆盖：
/// - 主程序动作 `musicxx.storage.get/set` 的约定：`get` 的应答就是**值本身**
///   （键不存在时为空应答）—— JS 便捷封装 `musicxx.storage.getConfig(key, 默认值)`
///   直接把应答当值使用，包一层对象会让插件读不到自己写的内容；
/// - 设置页能力读到的值与 config.json 一致；
/// - 改设置 → 写回 config.json → 重新加载（等价于重启）后仍然生效；
/// - 运行期改插件自己的背景动画速率设置能反映到 UI 项快照里
///   （`example_js_playing_bg`：播放页背景合集：六种样式 + 速率 + 取色 + 前景色）。
/// - 播放页背景的状态文字（`example_js_playing_bg` 的说明页）跟着快照频道走：
///   选中本插件但播放页没打开时也不能报成"内置背景"，切回内置后要更新（不能停在"已请求"）。
/// - 频谱页（`example_js_playing_bg` 的 `spectrumProbe`）必须**同步返回**：镜像当场可用、动作结果
///   异步回读 —— 处理器里等动作会与同步进插件框架的调用方互锁，只能等到 5 秒超时（这条用例守它）。
/// - 字体切换页（`example_js_font`）的按钮同样必须**立刻返回**：动作在后台执行（回读）
///   ，处理器里 await 字体动作会锁到动作超时（用户看到"卡几秒 + 切换失败但字体已变"）。
///
/// 前置：先跑 `tools/build_examples.ps1`（Windows）或 `tools/build_examples.sh`（Linux/macOS）（产出 `.native/output/<平台>-<架构>-<配置>/`，含示例插件）。
/// 若找不到原生库或示例插件，测试会跳过而不是失败。
library;

import 'dart:async';
import 'dart:convert' as convert;
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:musicxx_extern_plugin/musicxx_extern_plugin.dart';
import 'package:path/path.dart' as path;

import 'ui_content_probe.dart';

void main() {
  final _Environment? env = _Environment.detect();

  test('example_js：设置写入文件，重新加载后仍然生效', () async {
    if (env == null || !env.hasPlugin('example_js')) {
      markTestSkipped(
        '未找到原生插件框架库或示例插件，跳过（先跑 tools/build_examples.ps1（Windows）或 tools/build_examples.sh（Linux/macOS）产出原生库与示例插件）',
      );
      return;
    }

    final Directory work = Directory.systemTemp.createTempSync(
      'musicxx_plugin_config_test',
    );
    addTearDown(() {
      if (work.existsSync()) {
        work.deleteSync(recursive: true);
      }
    });

    // 复制一份示例插件：插件目录必须可写（设置就写在插件目录的 config.json 里）
    final Directory pluginRoot = _copyPlugin(env, work, 'example_js');

    final MusicxxPluginRuntime runtime = MusicxxPluginRuntime.create();
    addTearDown(runtime.dispose);
    _registerHostActions(runtime, pluginRoot.path);
    final List<MusicxxPluginEvent> seen = <MusicxxPluginEvent>[];
    final StreamSubscription<MusicxxPluginEvent> sub = runtime.events.listen(
      seen.add,
    );
    addTearDown(sub.cancel);

    await runtime.init(
      config: MusicxxPluginRuntimeConfig(
        appVersion: '0.0.0-test',
        platform: MusicxxPluginRuntime.currentPlatform,
        language: 'zh-cn',
        userPluginDir: pluginRoot.path,
        observeEvents: true,
      ),
      libraryPath: env.libraryPath,
    );
    await runtime.plugins.scan();
    await runtime.plugins.load('example_js');
    expect(runtime.plugins.findLoaded('example_js'), isNotNull);
    final UiContentProbe probe = UiContentProbe(runtime, seen);

    // 等脚本顶层的配置读取（异步动作往返）走完：探针里的配置缓存被填上才算读完
    await _pumpUntil(
      () async => (await _probe(runtime))['configSkipAds'] != null,
      runtime: runtime,
    );
    // 行的标题就是查找键（页面是 `musicxx.ui.page` 上的内容，列表块已移除，行由布局块组合，见 `_rowOf`）
    expect(
      _rowOf(
        await probe.jsonOf('plugin.example_js.settings'),
        '跳过广告曲目',
      )?['right'],
      '已开启',
    );

    final File config = File(
      path.join(pluginRoot.path, 'example_js', 'config.json'),
    );
    // 首次加载还没写过配置：默认值来自脚本（getConfig 的第二个参数）
    expect((await _probe(runtime))['configSkipAds'], true);
    expect(config.existsSync(), false, reason: '没改过设置就不该写配置文件');

    // 改设置：与用户在设置页点按钮等价（动作只改状态，页面靠 `slot.update` 重生成）
    await _callAction(runtime, 'toggleSkipAds');
    expect(
      _rowOf(
        await probe.jsonOf('plugin.example_js.settings'),
        '跳过广告曲目',
      )?['right'],
      '已关闭',
    );
    await _pumpUntil(() => config.existsSync(), runtime: runtime);
    expect(_readConfig(config)['skipAds'], false);

    // 再改另一项（同一个 config.json 的另一个键）：写新键不能丢掉已有的键
    await _callAction(runtime, 'cycleHeartbeat');
    await _pumpUntil(
      () => _readConfig(config)['heartbeatMs'] == 60000,
      runtime: runtime,
    );
    expect(_readConfig(config)['heartbeatMs'], 60000, reason: '30 秒 → 60 秒');
    expect(_readConfig(config)['skipAds'], false, reason: '写新键不能丢掉已有的键');

    // 重新加载（等价于"重启应用后再加载插件"）：设置必须还在
    await runtime.plugins.unload('example_js');
    await runtime.plugins.load('example_js');
    await _pumpUntil(
      () async => (await _probe(runtime))['configSkipAds'] == false,
      runtime: runtime,
    );
    expect((await _probe(runtime))['configSkipAds'], false, reason: '设置必须持久化');
    expect(
      _rowOf(
        await probe.jsonOf('plugin.example_js.settings'),
        '跳过广告曲目',
      )?['right'],
      '已关闭',
    );
    expect(
      _rowOf(
        await probe.jsonOf('plugin.example_js.settings'),
        '心跳间隔(毫秒)',
      )?['right'],
      '60000',
    );
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('example_js_playing_bg：背景动画速率写入磁盘，重新加载后仍然生效', () async {
    if (env == null || !env.hasPlugin('example_js_playing_bg')) {
      markTestSkipped(
        '未找到原生插件框架库或背景示例插件，跳过（先跑 tools/build_examples.ps1（Windows）或 tools/build_examples.sh（Linux/macOS）产出原生库与示例插件）',
      );
      return;
    }

    final Directory work = Directory.systemTemp.createTempSync(
      'musicxx_plugin_playing_bg_config_test',
    );
    addTearDown(() {
      if (work.existsSync()) {
        work.deleteSync(recursive: true);
      }
    });

    const String pluginId = 'example_js_playing_bg';
    final Directory pluginRoot = _copyPlugin(env, work, pluginId);

    final MusicxxPluginRuntime runtime = MusicxxPluginRuntime.create();
    addTearDown(runtime.dispose);
    _registerHostActions(runtime, pluginRoot.path);
    final List<MusicxxPluginEvent> seen = <MusicxxPluginEvent>[];
    final StreamSubscription<MusicxxPluginEvent> sub = runtime.events.listen(
      seen.add,
    );
    addTearDown(sub.cancel);

    await runtime.init(
      config: MusicxxPluginRuntimeConfig(
        appVersion: '0.0.0-test',
        platform: MusicxxPluginRuntime.currentPlatform,
        language: 'zh-cn',
        userPluginDir: pluginRoot.path,
        observeEvents: true,
      ),
      libraryPath: env.libraryPath,
    );
    await runtime.plugins.scan();
    await runtime.plugins.load(pluginId);
    expect(runtime.plugins.findLoaded(pluginId), isNotNull);

    // 背景样式是 `musicxx.ui.playing.background` 上的**内容**：候选列表读注册（名字 / 说明 /
    // 顺序），画面读生成出来的块树。所以这里既看注册表，也向插件要一份内容。
    //
    // 第一个样式是『模糊』；动画速率只影响三个图案样式，所以速度读『晶格』。
    final UiContentProbe probe = UiContentProbe(runtime, seen);
    final List<MusicxxPluginUIContent> backgrounds = await probe
        .registrationsOf(pluginId, slot: 'musicxx.ui.playing.background');
    expect(backgrounds.length, 6, reason: '六种背景样式各注册一份内容: $backgrounds');
    expect(backgrounds.first.title, isNotEmpty, reason: '候选列表在选中之前就要有名字');
    expect(
      _shaderBlockOf(await probe.jsonOf('plugin.$pluginId.bg'))?['bundle'],
      'shader/heat.shaderbundle',
      reason: 'bundle 用插件目录内的相对路径（第一个样式是『模糊』）',
    );
    expect(await _backgroundSpeed(probe, pluginId), 1);

    // 第二种样式『光圈』：圆上 64 个点、一个点一个频带，所以数据要用 64 个频带
    // （`spectrum.bands64.0..15`，每项 4 个连续频带），少一项圆上就会缺一段
    final Map<String, Object?>? ring = _shaderBlockOf(
      await probe.jsonOf('plugin.$pluginId.ring'),
    );
    expect(ring, isNotNull, reason: '插件应注册『光圈』样式');
    expect(ring!['bundle'], 'shader/ring.shaderbundle');
    final Map<String, Set<String>> ringSources = _argSources(ring);
    expect(ringSources.containsKey('uLine'), true, reason: 'uLine 也要声明');
    expect(ringSources['uLine'], isEmpty, reason: '线条色是 const 节点（不读任何来源）');
    expect(ringSources['uLevel'], contains('musicxx.spectrum.level'));
    for (int i = 0; i < 16; ++i) {
      expect(
        ringSources['uBands$i'],
        contains('musicxx.spectrum.bands64.$i'),
        reason: '第 $i 组频带（频带 ${i * 4}..${i * 4 + 3}）必须声明',
      );
    }
    expect(
      ringSources.length,
      23,
      reason:
          '4 个绘制色 + 线条色 + 响度 + 16 组频带（64 个频带）+ 圆心封面的纹理声明；'
          '一帧 256 个频点那种写法有 64 个成员，逐像素按组挑一次会很卡，别改回去',
    );
    // 频谱必须包一层过渡（10 帧/秒的数据直接取会有台阶感）
    final Object? levelNode = ring['args'] is Map
        ? (ring['args'] as Map)['uLevel']
        : null;
    expect(
      levelNode is Map ? levelNode['kind'] : null,
      'smooth',
      reason: '频谱来源要声明过渡',
    );

    final File config = File(
      path.join(pluginRoot.path, pluginId, 'config.json'),
    );
    expect(config.existsSync(), false, reason: '没改过设置就不该写配置文件');
    expect(
      _rowOf(
        await probe.jsonOf('plugin.$pluginId.settings'),
        '背景动画速率',
      )?['right'],
      '1x',
    );

    // 切一档速率（等价于用户在设置页点按钮）：内容重新注册 + 写回 config.json
    //
    // 两个条件都要等：内容刷新走插件框架线程，写配置走 Dart 侧动作请求，
    // 只看其中一个会让"另一半还没被处理"时就开始断言。
    await _callAction(runtime, 'cycleBackgroundRate', plugin: pluginId);
    await _pumpUntil(
      () async =>
          await _backgroundSpeed(probe, pluginId) == 2 &&
          _readConfig(config)['bgRate'] == 2,
      runtime: runtime,
    );
    expect(
      await _backgroundSpeed(probe, pluginId),
      2,
      reason: '1x 的基准速度是 1，切到 2x 后声明 2',
    );
    expect(_readConfig(config)['bgRate'], 2);

    // 重新加载：速率必须还在，设置页显示的也是新值
    await runtime.plugins.unload(pluginId);
    await runtime.plugins.load(pluginId);
    await _pumpUntil(
      () async =>
          _rowOf(
            await probe.jsonOf('plugin.$pluginId.settings'),
            '背景动画速率',
          )?['right'] ==
          '2x',
      runtime: runtime,
    );
    expect(await _backgroundSpeed(probe, pluginId), 2, reason: '速率必须持久化');
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('example_js_playing_bg：背景状态文字跟着快照频道更新', () async {
    if (env == null || !env.hasPlugin('example_js_playing_bg')) {
      markTestSkipped(
        '未找到原生插件框架库或背景示例插件，跳过（先跑 tools/build_examples.ps1（Windows）或 tools/build_examples.sh（Linux/macOS）产出原生库与示例插件）',
      );
      return;
    }

    final Directory work = Directory.systemTemp.createTempSync(
      'musicxx_plugin_playing_bg_state_test',
    );
    addTearDown(() {
      if (work.existsSync()) {
        work.deleteSync(recursive: true);
      }
    });

    const String pluginId = 'example_js_playing_bg';
    const String itemId = 'plugin.example_js_playing_bg.lattice';
    final Directory pluginRoot = _copyPlugin(env, work, pluginId);

    final MusicxxPluginRuntime runtime = MusicxxPluginRuntime.create();
    addTearDown(runtime.dispose);
    _registerHostActions(runtime, pluginRoot.path);
    final List<MusicxxPluginEvent> seen = <MusicxxPluginEvent>[];
    final StreamSubscription<MusicxxPluginEvent> sub = runtime.events.listen(
      seen.add,
    );
    addTearDown(sub.cancel);
    await runtime.init(
      config: MusicxxPluginRuntimeConfig(
        appVersion: '0.0.0-test',
        platform: MusicxxPluginRuntime.currentPlatform,
        language: 'zh-cn',
        userPluginDir: pluginRoot.path,
      ),
      libraryPath: env.libraryPath,
    );
    await runtime.plugins.scan();
    await runtime.plugins.load(pluginId);
    expect(runtime.plugins.findLoaded(pluginId), isNotNull);
    final UiContentProbe probe = UiContentProbe(runtime, seen);

    /// 推一条播放页背景槽位的状态（主程序侧就是 `musicxx.state.uiSlots`）
    ///
    /// 规则：`itemId` = 现在由哪个插件项在画（空 = 没有插件项在画），
    /// `selectedId` = 用户选中的是谁（可能是 `musicxx.*`）。
    /// `animate` / `maxFps` 不在槽位状态里（它们是每个着色器块自己的字段）。
    Future<void> pushSlot({
      required String itemId,
      required String selectedId,
      required bool visible,
      int width = 0,
      int height = 0,
    }) async {
      await runtime.broadcasts.publish(
        'musicxx.state.uiSlots',
        <String, Object?>{
          'player.background': <String, Object?>{
            'itemId': itemId,
            'plugin': itemId.isEmpty ? '' : pluginId,
            'selectedId': selectedId,
            'night': false,
            'visible': visible,
            'width': width,
            'height': height,
          },
        },
      );
    }

    Future<String?> backgroundText() async =>
        _rowOf(await probe.jsonOf('plugin.$pluginId.card'), '播放页背景')?['right']
            as String?;

    Future<String?> renderText() async =>
        _rowOf(await probe.jsonOf('plugin.$pluginId.card'), '渲染状态')?['right']
            as String?;

    // 主程序还没推过这个槽位：只能说"没有状态"，不能猜成"内置背景"
    expect(await backgroundText(), '无状态');

    // 用户选中的是内置样式（条目仍在，itemId 为空）
    await pushSlot(itemId: '', selectedId: 'musicxx.auto', visible: false);
    expect(await backgroundText(), '内置背景');
    expect(await renderText(), '未生效');

    // 一键使用：动作还在往返，页面当场先显示"已请求"
    // （动作只改状态、让页面重出一份内容；这里就是要一份新内容看当下的反馈）
    await _callAction(
      runtime,
      'useBackground',
      plugin: pluginId,
      args: <String, Object?>{'id': itemId, 'view': 'card'},
    );
    expect(
      _rowOf(await probe.jsonOf('plugin.$pluginId.card'), '播放页背景')?['right'],
      '已请求',
      reason: '点击后当场要有反馈（镜像还没反映这次请求）',
    );

    // 镜像反映这次请求：选中本插件、但播放页没打开（没有挂载点 → 没有渲染）
    await pushSlot(itemId: itemId, selectedId: itemId, visible: false);
    expect(await backgroundText(), '生效中');
    expect(await renderText(), '未在渲染');

    // 播放页在前台：带上渲染尺寸与是否动态
    await pushSlot(
      itemId: itemId,
      selectedId: itemId,
      visible: true,
      width: 1280,
      height: 720,
    );
    expect(await renderText(), '渲染中, 1280x720');

    // 切回内置：条目还在（itemId 为空），状态文字要更新，不能一直停在"已请求"
    await pushSlot(itemId: '', selectedId: 'musicxx.auto', visible: false);
    await _pumpUntil(
      () async => await backgroundText() == '内置背景',
      runtime: runtime,
    );
    expect(
      await backgroundText(),
      '内置背景',
      reason: '切回内置样式后状态文字必须更新（否则页面上看着像没生效）',
    );
    expect(await renderText(), '未生效');
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('example_js_playing_bg：频谱页同步返回（镜像当场可用，动作异步回读）', () async {
    if (env == null || !env.hasPlugin('example_js_playing_bg')) {
      markTestSkipped(
        '未找到原生插件框架库或背景示例插件，跳过（先跑 tools/build_examples.ps1（Windows）或 tools/build_examples.sh（Linux/macOS）产出原生库与示例插件）',
      );
      return;
    }

    final Directory work = Directory.systemTemp.createTempSync(
      'musicxx_plugin_playing_bg_spectrum_test',
    );
    addTearDown(() {
      if (work.existsSync()) {
        work.deleteSync(recursive: true);
      }
    });

    const String pluginId = 'example_js_playing_bg';
    final Directory pluginRoot = _copyPlugin(env, work, pluginId);

    final MusicxxPluginRuntime runtime = MusicxxPluginRuntime.create();
    addTearDown(runtime.dispose);
    _registerHostActions(runtime, pluginRoot.path);
    final List<MusicxxPluginEvent> seen = <MusicxxPluginEvent>[];
    final StreamSubscription<MusicxxPluginEvent> sub = runtime.events.listen(
      seen.add,
    );
    addTearDown(sub.cancel);

    // 频谱动作用一条固定结果回答（真实实现读播放器提取的频谱，这里只验证调用能走通）
    int spectrumCalls = 0;
    runtime.appCapabilities.register(MusicxxCapabilityId.mediaSpectrum, (
      invocation,
    ) {
      ++spectrumCalls;
      return <String, Object?>{
        'ok': true,
        'status': 'ready',
        'available': true,
        'loading': false,
        'reason': '',
        'unit': 'normalized',
        'bandCount': 8,
        'level': 0.5,
        'bands': <double>[0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8],
        'positionMs': 1000,
        'durationMs': 200000,
      };
    });

    await runtime.init(
      config: MusicxxPluginRuntimeConfig(
        appVersion: '0.0.0-test',
        platform: MusicxxPluginRuntime.currentPlatform,
        language: 'zh-cn',
        userPluginDir: pluginRoot.path,
      ),
      libraryPath: env.libraryPath,
    );
    await runtime.plugins.scan();
    await runtime.plugins.load(pluginId);
    expect(runtime.plugins.findLoaded(pluginId), isNotNull);
    final UiContentProbe probe = UiContentProbe(runtime, seen);

    // 插件加载时就发起过一次动作读取（脚本顶层的 refreshSpectrumAction）
    await _pumpUntil(() => spectrumCalls >= 1, runtime: runtime);
    expect(spectrumCalls, greaterThanOrEqualTo(1), reason: '插件加载后应异步读一次频谱动作');

    // 生成函数必须**同步返回**：在生成里等动作会一直卡到插件框架的时限
    // （主程序侧表现是整机卡住几秒 + 请求超时）。
    final Stopwatch watch = Stopwatch()..start();
    Map<String, Object?> page = await probe.jsonOf(
      'plugin.$pluginId.spectrumProbe',
    );
    watch.stop();
    expect(
      watch.elapsedMilliseconds,
      lessThan(1000),
      reason: '生成函数不能等动作：这一页必须立刻生成出来（等动作会卡到超时）',
    );
    // 镜像还没推过：镜像那一行只能说"无状态"（动作结果照旧异步读，不影响这一页当场画出来）
    expect(_rowOf(page, '快照频道')?['right'], '无状态');

    // 推一条"有数据"的镜像：镜像那一行按镜像说真话（同步、实时）
    await runtime.broadcasts
        .publish('musicxx.state.spectrum', <String, Object?>{
          'status': 'ready',
          'available': true,
          'loading': false,
          'reason': '',
          'level': 0.42,
          'bands': List<double>.filled(16, 0.1),
          'frameHz': 10,
        });
    page = await probe.jsonOf('plugin.$pluginId.spectrumProbe');
    expect(_rowOf(page, '快照频道')?['right'], '有数据（响度 42%）');

    // 动作结果由页面回读：重新要一份内容就会再发起一次读取，所以这里反复要，
    // 直到某一次读到的就是异步读回的结果（生成函数同步返回，每次都是立即的）
    await _pumpUntil(() async {
      page = await probe.jsonOf('plugin.$pluginId.spectrumProbe');
      return _textStartingWith(page, '动作结果: ').contains('bandCount = 8');
    }, runtime: runtime);
    final String detail = _textStartingWith(page, '动作结果: ');
    expect(detail, contains('status = ready'));
    expect(detail, contains('bandCount = 8'));
    expect(detail, contains('频带(%) = 10 / 20 / 30 / 40 / 50 / 60 / 70 / 80'));
    expect(detail, contains('位置 1s / 200s'));
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('example_js_font：字体切换立刻返回（动作后台执行，不再互锁到超时）', () async {
    if (env == null || !env.hasPlugin('example_js_font')) {
      markTestSkipped(
        '未找到原生插件框架库或字体示例插件，跳过（先跑 tools/build_examples.ps1（Windows）或 tools/build_examples.sh（Linux/macOS）产出原生库与示例插件）',
      );
      return;
    }

    final Directory work = Directory.systemTemp.createTempSync(
      'musicxx_plugin_font_test',
    );
    addTearDown(() {
      if (work.existsSync()) {
        work.deleteSync(recursive: true);
      }
    });

    const String pluginId = 'example_js_font';
    final Directory pluginRoot = _copyPlugin(env, work, pluginId);

    final MusicxxPluginRuntime runtime = MusicxxPluginRuntime.create();
    addTearDown(runtime.dispose);
    _registerHostActions(runtime, pluginRoot.path);
    final List<MusicxxPluginEvent> seen = <MusicxxPluginEvent>[];
    final StreamSubscription<MusicxxPluginEvent> sub = runtime.events.listen(
      seen.add,
    );
    addTearDown(sub.cancel);

    // 字体动作：故意做慢（400ms 同步阻塞，模拟主程序"换字体重建整棵界面"）
    //
    // 这一步是关键：能力处理器里 `await` 这个动作时，动作要回到调用线程处理，而调用线程
    // 正阻塞在这次能力调用里 —— 两边互等到动作超时（5 秒）。正确写法是"发出去就返回"。
    final List<String> fontCalls = <String>[];
    const Duration fontWork = Duration(milliseconds: 400);
    runtime.appCapabilities.register(MusicxxCapabilityId.uiSetFontFamily, (
      invocation,
    ) {
      final String family = '${invocation.args['fontFamily']}';
      fontCalls.add(family);
      sleep(fontWork);
      return <String, Object?>{
        'ok': true,
        'fontFamily': family,
        'isCustom': family != 'default',
      };
    });
    runtime.appCapabilities.register(MusicxxCapabilityId.uiLoadFont, (
      invocation,
    ) {
      final String family = '${invocation.args['family']}';
      fontCalls.add('file:$family');
      sleep(fontWork);
      return <String, Object?>{
        'ok': true,
        'family': family,
        'applied': true,
        'fontFamily': family,
      };
    });
    runtime.appCapabilities.register(
      MusicxxCapabilityId.uiToast,
      (invocation) => <String, Object?>{'ok': true},
    );

    await runtime.init(
      config: MusicxxPluginRuntimeConfig(
        appVersion: '0.0.0-test',
        platform: MusicxxPluginRuntime.currentPlatform,
        language: 'zh-cn',
        userPluginDir: pluginRoot.path,
        observeEvents: true,
      ),
      libraryPath: env.libraryPath,
    );
    // 官方频道：插件顶层会订阅它（主程序侧就是 MusicxxValueBroadcasts 的 musicxx.ui.fontFamily）
    await runtime.broadcasts.publish('musicxx.ui.fontFamily', 'default');
    await runtime.plugins.scan();
    await runtime.plugins.load(pluginId);
    expect(runtime.plugins.findLoaded(pluginId), isNotNull);
    final UiContentProbe probe = UiContentProbe(runtime, seen);

    // 页面：默认模式 + "主程序生效字体"（同步读变量缓存）
    Map<String, Object?> view = await probe.jsonOf('plugin.$pluginId.card');
    expect(_rowOf(view, '选择的模式')?['right'], contains('拟声默认'));

    // 点『微软雅黑』：能力必须**立刻**返回
    final Stopwatch watch = Stopwatch()..start();
    await _callAction(runtime, 'selectYahei', plugin: pluginId);
    watch.stop();
    expect(
      watch.elapsedMilliseconds,
      lessThan(1000),
      reason:
          '按钮能力必须立刻返回：处理器里 await 字体动作会与调用方互锁，'
          '一直卡到动作超时（主程序侧表现是整机卡几秒 + 报 action_timeout，但字体其实切成功了）',
    );
    // 当场给反馈：页面重出一份就显示新模式（这时动作还没被处理）
    expect(
      _rowOf(await probe.jsonOf('plugin.$pluginId.card'), '选择的模式')?['right'],
      contains('微软雅黑'),
    );
    // 说明：主程序调插件现在是**异步受理**（结果经事件回填），等结果期间事件循环照跑，
    // 所以"调用返回时后台动作还没被处理"不再是可靠判据 —— 事件泵可能已经把动作跑了。
    // 这里能判的是"调用没有被动作拖住"：旧写法（处理器里 await 动作）会与调用方互锁，
    // 一直卡到动作超时，根本走不到下面这行。
    expect(
      watch.elapsedMilliseconds,
      lessThan(1000),
      reason: '调用没有被后台动作拖住（旧写法会互锁到动作超时）',
    );

    // 后台动作随后到达（慢动作 400ms）并写入磁盘
    final File config = File(
      path.join(pluginRoot.path, pluginId, 'config.json'),
    );
    await _pumpUntil(
      () => fontCalls.length == 1 && _readConfig(config)['fontMode'] == 'yahei',
      runtime: runtime,
    );
    expect(fontCalls, <String>['Microsoft YaHei']);
    expect(_readConfig(config)['fontMode'], 'yahei');

    // 主程序推一次官方频道（真实主程序在字体变化时推）：页面用同步 read 读到它
    await runtime.broadcasts.publish(
      'musicxx.ui.fontFamily',
      'Microsoft YaHei',
    );
    await _pumpUntil(
      () async =>
          _rowOf(
            await probe.jsonOf('plugin.$pluginId.card'),
            '主程序生效字体',
          )?['right'] ==
          'Microsoft YaHei',
      runtime: runtime,
    );
    expect(
      _rowOf(await probe.jsonOf('plugin.$pluginId.card'), '主程序生效字体')?['right'],
      'Microsoft YaHei',
      reason: '页面用 peek 同步读变量缓存（不做 FFI 往返，也不阻塞能力调用）',
    );

    // 动作成功后再取页面：不再显示"切换中"
    view = await probe.jsonOf('plugin.$pluginId.card');
    expect(_rowOf(view, '选择的模式')?['right'], contains('微软雅黑'));
    expect(_rowOf(view, '选择的模式')?['right'], isNot(contains('切换中')));

    // 『字体文件示例』：同样的写法走 loadFont 分支
    final Stopwatch fileWatch = Stopwatch()..start();
    await _callAction(runtime, 'selectFile', plugin: pluginId);
    fileWatch.stop();
    expect(fileWatch.elapsedMilliseconds, lessThan(1000));
    expect(
      _rowOf(await probe.jsonOf('plugin.$pluginId.card'), '选择的模式')?['right'],
      contains('字体文件示例'),
    );
    await _pumpUntil(
      () => fontCalls.length == 2 && _readConfig(config)['fontMode'] == 'file',
      runtime: runtime,
    );
    expect(fontCalls.last, 'file:ExampleFont');
    expect(_readConfig(config)['fontMode'], 'file');

    // 『拟声默认』：恢复默认字体
    await _callAction(runtime, 'selectDefault', plugin: pluginId);
    await _pumpUntil(
      () =>
          fontCalls.length == 3 && _readConfig(config)['fontMode'] == 'default',
      runtime: runtime,
    );
    expect(fontCalls.last, 'default');
  }, timeout: const Timeout(Duration(seconds: 60)));
}

/// 调一个"只改状态"的插件能力
///
/// 动作不再返回视图：改完状态由插件调 `musicxx.ui.slot.update`，主程序让那一页重出一份内容。
/// 这里只确认动作受理并按约定返回 `{ok: true}`；要看结果就再用 [UiContentProbe] 要一份内容。
Future<void> _callAction(
  MusicxxPluginRuntime runtime,
  String method, {
  String plugin = 'example_js',
  Map<String, Object?> args = const <String, Object?>{},
}) async {
  final Object? raw = await runtime.plugins.call(
    plugin,
    'plugin.$plugin.$method',
    args,
  );
  expect(raw, <String, Object?>{'ok': true}, reason: '$method 应返回 ok');
}

/// 取视图里某一行的排版结果（按行标题找）
///
/// 列表块已移除：行由 `Column` + `Row` + `Expanded` + `Column` + `Text` 组合出来，
/// 这里按"行内第一个文本 = 标题、行内最后一个文本 = 右侧状态"还原成 `{id, title, right}`，
/// 调用方只关心右侧状态文字（原先是 list 条目里的 `right` 字段）。
Map<String, Object?>? _rowOf(Map<String, Object?> view, String title) {
  final Object? blocks = view['blocks'];
  if (blocks is! List) {
    return null;
  }
  for (final Object? block in blocks) {
    for (final Object? row in _rowsIn(block)) {
      final List<String> texts = <String>[];
      _collectTexts(row, texts);
      if (texts.isNotEmpty && texts.first == title) {
        return <String, Object?>{
          'id': title,
          'title': title,
          'right': texts.length > 1 ? texts.last : '',
        };
      }
    }
  }
  return null;
}

/// 一个块里的所有行（行被 `Block`（内容块）或 `Column`（带行间距）包着）
Iterable<Object?> _rowsIn(Object? block) sync* {
  if (block is! Map) {
    return;
  }
  if (block['kind'] == 'Row') {
    yield block;
    return;
  }
  final Object? child = block['child'];
  if (null != child) {
    yield* _rowsIn(child);
  }
  final Object? children = block['children'];
  if (children is List) {
    for (final Object? item in children) {
      yield* _rowsIn(item);
    }
  }
}

/// 按布局块顺序收集文本（`child` / `children` 都算）
void _collectTexts(Object? block, List<String> out) {
  if (block is! Map) {
    return;
  }
  if (block['kind'] == 'Text') {
    final Object? text = block['text'];
    if (text is String && text.isNotEmpty) {
      out.add(text);
    }
    return;
  }
  for (final String key in <String>['child', 'children']) {
    final Object? value = block[key];
    if (value is List) {
      for (final Object? item in value) {
        _collectTexts(item, out);
      }
    } else if (null != value) {
      _collectTexts(value, out);
    }
  }
}

/// 视图里第一个以 [prefix] 开头的文本（没有就返回空串）
String _textStartingWith(Map<String, Object?> view, String prefix) {
  final List<String> texts = <String>[];
  final Object? blocks = view['blocks'];
  if (blocks is List) {
    for (final Object? block in blocks) {
      _collectTexts(block, texts);
    }
  }
  for (final String text in texts) {
    if (text.startsWith(prefix)) {
      return text;
    }
  }
  return '';
}

/// 插件探针（能力 `probe`，仅 example_js 有）
Future<Map<String, Object?>> _probe(MusicxxPluginRuntime runtime) async {
  final Object? raw = await runtime.plugins.call(
    'example_js',
    'plugin.example_js.probe',
    const <String, Object?>{},
  );
  expect(raw, isA<Map<String, Object?>>());
  return raw! as Map<String, Object?>;
}

/// 块树里的着色器块
///
/// 背景的根块是 `Expanded`（不包的话着色器拿不到确定高度），着色器块在它的 `child`
/// （或 `children` 的第一项）里。
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

/// 着色器块 `args`（成员名 → 值表达式）里每个成员用到的**具名来源**
///
/// 值表达式可以嵌套（`smooth` / `mul` / `mix` …），要把树走一遍才知道成员实际读的是哪些来源；
/// 测试只关心"声明里连的是哪个来源"（没有来源的成员给空集合）。
Map<String, Set<String>> _argSources(Map<String, Object?>? shader) {
  final Map<String, Set<String>> out = <String, Set<String>>{};
  final Object? args = shader?['args'];
  if (args is Map) {
    for (final MapEntry<Object?, Object?> entry in args.entries) {
      final Set<String> names = <String>{};
      _collectSourceNames(entry.value, names);
      out['${entry.key}'] = names;
    }
  }
  return out;
}

/// 走一遍值表达式，收集 `source` 节点的名字
void _collectSourceNames(Object? node, Set<String> out) {
  if (node is! Map) {
    return;
  }
  if (node['kind'] == 'source') {
    final Object? name = node['name'];
    if (name is String && name.isNotEmpty) {
      out.add(name);
    }
  }
  for (final String key in <String>['of', 'a', 'b', 't']) {
    final Object? child = node[key];
    if (child is List) {
      for (final Object? item in child) {
        _collectSourceNames(item, out);
      }
    } else {
      _collectSourceNames(child, out);
    }
  }
}

/// 背景动画速度（着色器块的 `speed`；动画速率只影响图案样式，所以读『晶格』）
Future<num?> _backgroundSpeed(UiContentProbe probe, String pluginId) async {
  final Map<String, Object?> json = await probe.jsonOf(
    'plugin.$pluginId.lattice',
  );
  final Object? value = _shaderBlockOf(json)?['speed'];
  return value is num ? value : null;
}

Map<String, Object?> _readConfig(File file) {
  if (false == file.existsSync()) {
    return <String, Object?>{};
  }
  final Object? decoded = convert.jsonDecode(file.readAsStringSync());
  if (decoded is! Map) {
    return <String, Object?>{};
  }
  return decoded.cast<String, Object?>();
}

/// 把示例插件复制到临时目录（返回临时插件根目录）
///
/// 插件目录必须可写：设置就写在插件目录的 `config.json` 里，
/// 直接用安装目录会把测试的写入留在产物里。
Directory _copyPlugin(_Environment env, Directory work, String pluginId) {
  final Directory pluginRoot = Directory(path.join(work.path, 'plugins'))
    ..createSync(recursive: true);
  _copyDirectory(
    Directory(path.join(env.pluginRoot, pluginId)),
    Directory(path.join(pluginRoot.path, pluginId)),
  );
  return pluginRoot;
}

/// 注册主程序动作（与音乐主程序侧的语义一致：`storage.get` 返回**值本身**）
///
/// 这里只实现本测试需要的那几个动作，行为与主程序接入层一致：
/// - `namespace: "config"` 读写插件目录的 `config.json`；
/// - 其它命名空间读写内存里的私有 KV（真实应用是 `data/kv.json`）；
/// - `musicxx.ui.toast` 直接成功（不看界面）。
void _registerHostActions(MusicxxPluginRuntime runtime, String pluginRoot) {
  final Map<String, Object?> kv = <String, Object?>{};

  String pluginIdOf(MusicxxPluginAppCapabilityInvocation invocation) =>
      invocation.plugin.startsWith('js:')
      ? invocation.plugin.substring(3)
      : invocation.plugin;

  File configOf(String pluginId) =>
      File(path.join(pluginRoot, pluginId, 'config.json'));

  Map<String, Object?> readConfig(String pluginId) =>
      _readConfig(configOf(pluginId));

  String writeConfig(String pluginId, Map<String, Object?> data) {
    try {
      final File file = configOf(pluginId);
      file.parent.createSync(recursive: true);
      file.writeAsStringSync(
        const convert.JsonEncoder.withIndent('  ').convert(data),
      );
      return '';
    } catch (error) {
      return '$error';
    }
  }

  runtime.appCapabilities.register(MusicxxCapabilityId.storageGet, (
    invocation,
  ) {
    final String key = '${invocation.args['key']}';
    final String id = pluginIdOf(invocation);
    if (invocation.args['namespace'] == 'config') {
      // 约定：返回**值本身**（缺失即 null → 空应答 → 插件用自己给的默认值）
      return readConfig(id)[key];
    }
    return kv['$id:$key'];
  });
  runtime.appCapabilities.register(MusicxxCapabilityId.storageSet, (
    invocation,
  ) {
    final String key = '${invocation.args['key']}';
    final String id = pluginIdOf(invocation);
    if (invocation.args['namespace'] == 'config') {
      final Map<String, Object?> data = readConfig(id);
      data[key] = invocation.args['value'] ?? '';
      final String problem = writeConfig(id, data);
      return problem.isEmpty
          ? <String, Object?>{'ok': true}
          : <String, Object?>{'ok': false, 'error': problem};
    }
    kv['$id:$key'] = invocation.args['value'] ?? '';
    return <String, Object?>{'ok': true};
  });
  runtime.appCapabilities.register(MusicxxCapabilityId.storageDelete, (
    invocation,
  ) {
    final String key = '${invocation.args['key']}';
    final String id = pluginIdOf(invocation);
    if (invocation.args['namespace'] == 'config') {
      final Map<String, Object?> data = readConfig(id)..remove(key);
      writeConfig(id, data);
      return <String, Object?>{'ok': true};
    }
    kv.remove('$id:$key');
    return <String, Object?>{'ok': true};
  });
  runtime.appCapabilities.register(MusicxxCapabilityId.storageList, (
    invocation,
  ) {
    final String id = pluginIdOf(invocation);
    if (invocation.args['namespace'] == 'config') {
      return <String, Object?>{'keys': readConfig(id).keys.toList()..sort()};
    }
    return <String, Object?>{
      'keys': <String>[
        for (final String key in kv.keys)
          if (key.startsWith('$id:')) key.substring(id.length + 1),
      ],
    };
  });
  // 界面提示在测试环境没有界面：直接成功
  runtime.appCapabilities.register(
    MusicxxCapabilityId.uiToast,
    (invocation) => <String, Object?>{'ok': true},
  );
}

/// 等待条件成立（最多 5 秒；每 20 ms 泵一次事件并让出事件循环）
Future<void> _pumpUntil(
  FutureOr<bool> Function() condition, {
  required MusicxxPluginRuntime runtime,
  Duration timeout = const Duration(seconds: 5),
}) async {
  final DateTime deadline = DateTime.now().add(timeout);
  while (!await condition()) {
    if (DateTime.now().isAfter(deadline)) {
      return;
    }
    runtime.pumpEvents();
    await Future<void>.delayed(const Duration(milliseconds: 20));
  }
}

void _copyDirectory(Directory from, Directory to) {
  to.createSync(recursive: true);
  for (final FileSystemEntity entity in from.listSync(recursive: true)) {
    final String relative = path.relative(entity.path, from: from.path);
    final String target = path.join(to.path, relative);
    if (entity is Directory) {
      Directory(target).createSync(recursive: true);
    } else if (entity is File) {
      File(target)
        ..parent.createSync(recursive: true)
        ..writeAsBytesSync(entity.readAsBytesSync());
    }
  }
}

/// 原生库 + 示例插件目录探测（找不到就跳过需要它们的用例）
class _Environment {
  const _Environment(this.libraryPath, this.pluginRoot);

  final String libraryPath;
  final String pluginRoot;

  /// 该插件是否随产物一起安装（只有一个示例插件时也能跳过另一个用例）
  bool hasPlugin(String id) =>
      Directory(path.join(pluginRoot, id)).existsSync();

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
