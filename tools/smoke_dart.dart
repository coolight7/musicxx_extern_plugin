/// Dart 侧冒烟脚本（纯 Dart VM，不依赖 Flutter；用于定位 FFI 卡点）
///
/// 用法：
/// ```
/// dart run tools/smoke_dart.dart [<原生库路径>]
/// ```
/// 与 `test/host_smoke_test.dart` 走同一条链路，但每一步都打印进度，便于排查
/// "某个 FFI 调用不返回"这类问题（此时 Dart isolate 被阻塞，测试框架的计时器也不会触发）。
library;

import 'dart:io';

import 'package:musicxx_extern_plugin/musicxx_extern_plugin.dart';

void step(String message) {
  stdout.writeln('[smoke] $message');
}

Future<void> main(List<String> args) async {
  final String? library = args.isNotEmpty ? args.first : _detectLibrary();
  if (library == null) {
    stderr.writeln('未找到原生库；先运行 pwsh tools/build_native.ps1');
    exitCode = 2;
    return;
  }
  final String pluginRoot = '${File(library).parent.parent.path}/plugins';
  step('库: $library');
  step('插件目录: $pluginRoot (存在=${Directory(pluginRoot).existsSync()})');

  final MusicxxPluginRuntime runtime = MusicxxPluginRuntime.instance;
  step('1 init…');
  runtime.init(
    config: MusicxxPluginRuntimeConfig(
      appVersion: '0.0.0-smoke',
      platform: MusicxxPluginRuntime.currentPlatform,
      language: 'zh-cn',
      userPluginDir: pluginRoot,
      observeEvents: true,
    ),
    libraryPath: library,
  );
  step('1 init 完成 (running=${runtime.isRunning})');

  step('2 pumpEvents…');
  final int pumped = runtime.pumpEvents();
  step('2 pumpEvents 完成: $pumped 条; recent=${runtime.recentEvents.length}');
  for (final MusicxxPluginEvent event in runtime.recentEvents) {
    step('    事件: ${event.type} ${event.plugin}');
  }

  step('3 scan…');
  final List<MusicxxPluginInfo> found = runtime.plugins.scan();
  step(
    '3 scan 完成: ${found.length} 项 ${found.map((MusicxxPluginInfo e) => '${e.id}/${e.statusText}').toList()}',
  );

  step('4 load…');
  runtime.plugins.load('example_native');
  step('4 load 完成: loaded=${runtime.plugins.findLoaded('example_native')}');

  step('5 hook_count…');
  final int count = runtime.hooks.refreshNativeHandlerCount(
    MusicxxPluginHookId.playerBeforePlaySong,
  );
  step('5 hook_count 完成: $count');

  step('6 state.update…');
  runtime.state.update(MusicxxPluginState.keySong, <String, Object?>{
    'sid': 's-smoke',
    'name': '冒烟歌曲',
  });
  step('6 state.update 完成');

  step('7 plugin_call…');
  final probe = runtime.plugins.call(
    'example_native',
    'plugin.example_native.probe',
    const <String, Object?>{},
  );
  step('7 plugin_call 完成: $probe');

  step('8 decide…');
  final Map<String, Object?>? verdict = runtime.hooks.decide(
    MusicxxPluginHookId.playerBeforePlaySong,
    <String, Object?>{
      'sid': 's-ad',
      'song': <String, Object?>{'name': '广告插曲 - 测试'},
    },
  );
  step('8 decide 完成: $verdict');

  step('9 disable/enable…');
  runtime.plugins.disable('example_native');
  step(
    '9.1 disable 完成 (count=${runtime.hooks.refreshNativeHandlerCount(MusicxxPluginHookId.playerBeforePlaySong)})',
  );
  runtime.plugins.enable('example_native');
  step(
    '9.2 enable 完成 (count=${runtime.hooks.refreshNativeHandlerCount(MusicxxPluginHookId.playerBeforePlaySong)})',
  );

  step('10 unload…');
  runtime.plugins.unload('example_native');
  step('10 unload 完成');

  step('11 stats…');
  final Map<String, Object?> stats = runtime.plugins.stats();
  step('11 stats 完成: host=${stats['host'] is Map ? 'ok' : '-'}');

  step('12 dispose…');
  runtime.dispose();
  step('12 dispose 完成');
}

String? _detectLibrary() {
  for (final String candidate
      in MusicxxPluginNativeLibrary.defaultCandidates()) {
    if (File(candidate).existsSync()) {
      return candidate;
    }
  }
  return null;
}
