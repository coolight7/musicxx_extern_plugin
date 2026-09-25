/// 端到端测试：示例插件（JS）的**配置读写**在真实原生宿主上的验证
///
/// 覆盖：
/// - 宿主动作 `musicxx.storage.get/set` 的契约：`get` 的应答就是**值本身**
///   （键不存在时为空应答）—— JS 便捷封装 `musicxx.storage.getConfig(key, 默认值)`
///   直接把应答当值使用，包一层对象会让插件读不到自己写的内容；
/// - 设置页能力读到的值与 config.json 一致；
/// - 改设置 → 写回 config.json → 重新装载（等价于重启）后仍然生效；
/// - 运行期改插件自己的背景动画速率设置能反映到 UI 项快照里
///   （`example_js_shader`：播放页背景与速率示例已从这个插件拆出独立实现）。
/// - 播放页背景的状态文字（`example_js_shader` 的说明页）跟着状态镜像走：
///   选中本插件但播放页没打开时也不能报成"内置背景"，切回内置后要收敛（不能停在"已请求"）。
///
/// 前置：先跑 `pwsh tools/build_native.ps1`（产出 `.native/output/<平台>-<配置>/`）。
/// 若找不到原生库或示例插件，测试会跳过而不是失败。
library;

import 'dart:async';
import 'dart:convert' as convert;
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:musicxx_extern_plugin/musicxx_extern_plugin.dart';
import 'package:path/path.dart' as path;

void main() {
  final _Environment? env = _Environment.detect();

  test('example_js：设置读写落盘，重新装载后仍然生效', () async {
    if (env == null || !env.hasPlugin('example_js')) {
      markTestSkipped('未找到原生宿主库或示例插件，跳过（先运行 tools/build_native.ps1）');
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

    runtime.init(
      config: MusicxxPluginRuntimeConfig(
        appVersion: '0.0.0-test',
        platform: MusicxxPluginRuntime.currentPlatform,
        language: 'zh-cn',
        userPluginDir: pluginRoot.path,
        observeEvents: true,
      ),
      libraryPath: env.libraryPath,
    );
    runtime.plugins.scan();
    runtime.plugins.load('example_js');
    expect(runtime.plugins.findLoaded('example_js'), isNotNull);

    // 等脚本顶层的配置读取（异步动作往返）走完：探针里的配置缓存被填上才算读完
    await _pumpUntil(
      () => _probe(runtime)['configSkipAds'] != null,
      runtime: runtime,
    );
    expect(_rowOf(_callView(runtime, 'settings'), 'skipAds')?['right'], '已开启');

    final File config = File(
      path.join(pluginRoot.path, 'example_js', 'config.json'),
    );
    // 首次装载还没写过配置：默认值来自脚本（getConfig 的第二个参数）
    expect(_probe(runtime)['configSkipAds'], true);
    expect(config.existsSync(), false, reason: '没改过设置就不该写配置文件');

    // 改设置：与用户在设置页点按钮等价
    final Map<String, Object?> toggled = _callView(runtime, 'toggleSkipAds');
    expect(_rowOf(toggled, 'skipAds')?['right'], '已关闭');
    await _pumpUntil(() => config.existsSync(), runtime: runtime);
    expect(_readConfig(config)['skipAds'], false);

    // 再改另一项（同一个 config.json 的另一个键）：写新键不能丢掉已有的键
    _callView(runtime, 'cycleHeartbeat');
    await _pumpUntil(
      () => _readConfig(config)['heartbeatMs'] == 60000,
      runtime: runtime,
    );
    expect(_readConfig(config)['heartbeatMs'], 60000, reason: '30 秒 → 60 秒');
    expect(_readConfig(config)['skipAds'], false, reason: '写新键不能丢掉已有的键');

    // 重新装载（等价于"重启应用后再加载插件"）：设置必须还在
    runtime.plugins.unload('example_js');
    runtime.plugins.load('example_js');
    await _pumpUntil(
      () => _probe(runtime)['configSkipAds'] == false,
      runtime: runtime,
    );
    expect(_probe(runtime)['configSkipAds'], false, reason: '设置必须持久化');
    expect(_rowOf(_callView(runtime, 'settings'), 'skipAds')?['right'], '已关闭');
    expect(
      _rowOf(_callView(runtime, 'settings'), 'heartbeatMs')?['right'],
      '60000',
    );
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('example_js_shader：背景动画速率落盘，重新装载后仍然生效', () async {
    if (env == null || !env.hasPlugin('example_js_shader')) {
      markTestSkipped('未找到原生宿主库或背景示例插件，跳过（先运行 tools/build_native.ps1）');
      return;
    }

    final Directory work = Directory.systemTemp.createTempSync(
      'musicxx_plugin_shader_config_test',
    );
    addTearDown(() {
      if (work.existsSync()) {
        work.deleteSync(recursive: true);
      }
    });

    const String pluginId = 'example_js_shader';
    final Directory pluginRoot = _copyPlugin(env, work, pluginId);

    final MusicxxPluginRuntime runtime = MusicxxPluginRuntime.create();
    addTearDown(runtime.dispose);
    _registerHostActions(runtime, pluginRoot.path);

    runtime.init(
      config: MusicxxPluginRuntimeConfig(
        appVersion: '0.0.0-test',
        platform: MusicxxPluginRuntime.currentPlatform,
        language: 'zh-cn',
        userPluginDir: pluginRoot.path,
        observeEvents: true,
      ),
      libraryPath: env.libraryPath,
    );
    runtime.plugins.scan();
    runtime.plugins.load(pluginId);
    expect(runtime.plugins.findLoaded(pluginId), isNotNull);

    // 背景样式声明本身：类型、bundle 路径、默认速率（1× = 基准速度 1）
    final MusicxxPluginUIItem? background = _backgroundItem(runtime, pluginId);
    expect(background, isNotNull, reason: '插件应注册播放页背景样式');
    expect(background!.type, MusicxxPluginUIType.playingBackground);
    final Object? shader = background.data['shader'];
    expect(
      shader is Map ? shader['bundle'] : null,
      'shader/bg.shaderbundle',
      reason: 'bundle 用插件目录内的相对路径',
    );
    expect(_backgroundSpeed(runtime, pluginId), 1);

    final File config = File(
      path.join(pluginRoot.path, pluginId, 'config.json'),
    );
    expect(config.existsSync(), false, reason: '没改过设置就不该写配置文件');
    expect(
      _rowOf(
        _callView(runtime, 'settings', plugin: pluginId),
        'bgRate',
      )?['right'],
      '1×',
    );

    // 切一档速率（等价于用户在设置页点按钮）：UI 项重新声明 + 写回 config.json
    //
    // 两个条件都要等：UI 项更新走宿主线程，写配置走 Dart 侧动作请求，
    // 只看其中一个会让"另一半还没被处理"时就开始断言。
    _callView(runtime, 'cycleBackgroundRate', plugin: pluginId);
    await _pumpUntil(
      () =>
          _backgroundSpeed(runtime, pluginId) == 2 &&
          _readConfig(config)['bgRate'] == 2,
      runtime: runtime,
    );
    expect(
      _backgroundSpeed(runtime, pluginId),
      2,
      reason: '1× 的基准速度是 1，切到 2× 后声明 2',
    );
    expect(_readConfig(config)['bgRate'], 2);

    // 重新装载：速率必须还在，设置页显示的也是新值
    runtime.plugins.unload(pluginId);
    runtime.plugins.load(pluginId);
    await _pumpUntil(
      () =>
          _rowOf(
            _callView(runtime, 'settings', plugin: pluginId),
            'bgRate',
          )?['right'] ==
          '2×',
      runtime: runtime,
    );
    expect(_backgroundSpeed(runtime, pluginId), 2, reason: '速率必须持久化');
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('example_js_shader：背景状态文字跟着状态镜像收敛', () async {
    if (env == null || !env.hasPlugin('example_js_shader')) {
      markTestSkipped('未找到原生宿主库或背景示例插件，跳过（先运行 tools/build_native.ps1）');
      return;
    }

    final Directory work = Directory.systemTemp.createTempSync(
      'musicxx_plugin_shader_state_test',
    );
    addTearDown(() {
      if (work.existsSync()) {
        work.deleteSync(recursive: true);
      }
    });

    const String pluginId = 'example_js_shader';
    const String itemId = 'plugin.example_js_shader.bg';
    final Directory pluginRoot = _copyPlugin(env, work, pluginId);

    final MusicxxPluginRuntime runtime = MusicxxPluginRuntime.create();
    addTearDown(runtime.dispose);
    _registerHostActions(runtime, pluginRoot.path);
    runtime.init(
      config: MusicxxPluginRuntimeConfig(
        appVersion: '0.0.0-test',
        platform: MusicxxPluginRuntime.currentPlatform,
        language: 'zh-cn',
        userPluginDir: pluginRoot.path,
      ),
      libraryPath: env.libraryPath,
    );
    runtime.plugins.scan();
    runtime.plugins.load(pluginId);
    expect(runtime.plugins.findLoaded(pluginId), isNotNull);

    /// 推一条播放页背景槽位的状态（应用侧就是 `musicxx.state.renderSlots`）
    ///
    /// 规则：`itemId` = 现在由哪个插件项在画（空 = 没有插件项在画），
    /// `selectedId` = 用户选中的是谁（可能是 `builtin:*`）。
    void pushSlot({
      required String itemId,
      required String selectedId,
      required bool visible,
      int width = 0,
      int height = 0,
    }) {
      runtime.state.update('musicxx.state.renderSlots', <String, Object?>{
        'player.background': <String, Object?>{
          'itemId': itemId,
          'plugin': itemId.isEmpty ? '' : pluginId,
          'selectedId': selectedId,
          'night': false,
          'visible': visible,
          'animate': true,
          'width': width,
          'height': height,
          'maxFps': 16,
        },
      });
    }

    String? backgroundText() =>
        _rowOf(
              _callView(runtime, 'card', plugin: pluginId),
              'background',
            )?['right']
            as String?;

    String? renderText() =>
        _rowOf(_callView(runtime, 'card', plugin: pluginId), 'render')?['right']
            as String?;

    // 宿主还没推过这个槽位：只能说"没有状态"，不能猜成"内置背景"
    expect(backgroundText(), '无状态');

    // 用户选中的是内置样式（条目仍在，itemId 为空）
    pushSlot(itemId: '', selectedId: 'builtin:Auto', visible: false);
    expect(backgroundText(), '内置背景');
    expect(renderText(), '未生效');

    // 一键使用：动作还在往返，页面当场先显示"已请求"
    final Object? requested = runtime.plugins.call(
      pluginId,
      'plugin.$pluginId.useBackground',
      <String, Object?>{'id': itemId, 'view': 'card'},
    );
    expect(requested, isA<Map<String, Object?>>());
    expect(
      _rowOf(
        (requested! as Map<String, Object?>)['view']! as Map<String, Object?>,
        'background',
      )?['right'],
      '已请求',
      reason: '点击后当场要有反馈（镜像还没反映这次请求）',
    );

    // 镜像反映这次请求：选中本插件、但播放页没打开（没有挂载点 → 没有渲染）
    pushSlot(itemId: itemId, selectedId: itemId, visible: false);
    expect(backgroundText(), '生效中');
    expect(renderText(), '未渲染');

    // 播放页在前台：带上渲染尺寸与是否动态
    pushSlot(
      itemId: itemId,
      selectedId: itemId,
      visible: true,
      width: 1280,
      height: 720,
    );
    expect(renderText(), '动画中, 1280×720');

    // 切回内置：条目还在（itemId 为空），状态文字要收敛，不能一直停在"已请求"
    pushSlot(itemId: '', selectedId: 'builtin:Auto', visible: false);
    await _pumpUntil(() => backgroundText() == '内置背景', runtime: runtime);
    expect(backgroundText(), '内置背景', reason: '切回内置样式后状态文字必须收敛（否则页面上看着像没生效）');
    expect(renderText(), '未生效');
  }, timeout: const Timeout(Duration(seconds: 60)));
}

/// 调用插件能力并取回视图描述（`{view: {...}}`）
Map<String, Object?> _callView(
  MusicxxPluginRuntime runtime,
  String method, {
  String plugin = 'example_js',
}) {
  final Object? raw = runtime.plugins.call(
    plugin,
    'plugin.$plugin.$method',
    const <String, Object?>{},
  );
  expect(raw, isA<Map<String, Object?>>(), reason: '$method 应返回视图描述');
  final Object? view = (raw! as Map<String, Object?>)['view'];
  expect(view, isA<Map<String, Object?>>(), reason: '$method 的 view 缺失');
  return view! as Map<String, Object?>;
}

/// 取视图里某个 list 块中指定 id 的行
Map<String, Object?>? _rowOf(Map<String, Object?> view, String id) {
  final Object? blocks = view['blocks'];
  if (blocks is! List) {
    return null;
  }
  for (final Object? block in blocks) {
    if (block is! Map || block['kind'] != 'list') {
      continue;
    }
    final Object? items = block['items'];
    if (items is! List) {
      continue;
    }
    for (final Object? item in items) {
      if (item is Map && item['id'] == id) {
        return item.cast<String, Object?>();
      }
    }
  }
  return null;
}

/// 插件探针（能力 `probe`，仅 example_js 有）
Map<String, Object?> _probe(MusicxxPluginRuntime runtime) {
  final Object? raw = runtime.plugins.call(
    'example_js',
    'plugin.example_js.probe',
    const <String, Object?>{},
  );
  expect(raw, isA<Map<String, Object?>>());
  return raw! as Map<String, Object?>;
}

/// UI 项快照里的播放页背景样式（`musicxx.ui.playing.background`）
MusicxxPluginUIItem? _backgroundItem(
  MusicxxPluginRuntime runtime,
  String pluginId,
) {
  for (final MusicxxPluginUIItem item in runtime.plugins.uiSnapshot()) {
    if (item.plugin == pluginId &&
        item.type == MusicxxPluginUIType.playingBackground) {
      return item;
    }
  }
  return null;
}

/// 背景样式声明的动画速度（`data.speed`）
num? _backgroundSpeed(MusicxxPluginRuntime runtime, String pluginId) {
  final MusicxxPluginUIItem? item = _backgroundItem(runtime, pluginId);
  final Object? value = item?.data['speed'];
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

/// 注册宿主动作（与音乐应用侧的语义一致：`storage.get` 返回**值本身**）
///
/// 这里只实现本测试需要的那几个动作，行为对齐接入层：
/// - `namespace: "config"` 读写插件目录的 `config.json`；
/// - 其它命名空间读写内存里的私有 KV（真实应用是 `data/kv.json`）；
/// - `musicxx.ui.notify` 直接成功（不看界面）。
void _registerHostActions(MusicxxPluginRuntime runtime, String pluginRoot) {
  final Map<String, Object?> kv = <String, Object?>{};

  String pluginIdOf(MusicxxPluginActionInvocation invocation) =>
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

  runtime.actions.register(MusicxxPluginActionNames.storageGet, (invocation) {
    final String key = '${invocation.args['key']}';
    final String id = pluginIdOf(invocation);
    if (invocation.args['namespace'] == 'config') {
      // 契约：返回**值本身**（缺失即 null → 空应答 → 插件用自己给的默认值）
      return readConfig(id)[key];
    }
    return kv['$id:$key'];
  });
  runtime.actions.register(MusicxxPluginActionNames.storageSet, (invocation) {
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
  runtime.actions.register(MusicxxPluginActionNames.storageDelete, (
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
  runtime.actions.register(MusicxxPluginActionNames.storageList, (invocation) {
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
  runtime.actions.register(
    MusicxxPluginActionNames.uiNotify,
    (invocation) => <String, Object?>{'ok': true},
  );
}

/// 等待条件成立（最多 5 秒；每 20 ms 泵一次事件并让出事件循环）
Future<void> _pumpUntil(
  bool Function() condition, {
  required MusicxxPluginRuntime runtime,
  Duration timeout = const Duration(seconds: 5),
}) async {
  final DateTime deadline = DateTime.now().add(timeout);
  while (!condition()) {
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
