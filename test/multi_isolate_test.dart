/// 多 isolate 并发调用用例（plan §5.3 的 isolate 约定 / work.md §0.2 P1-7）
///
/// 约定（与 plan §5.3 一致）：
/// - 宿主是**进程单例**：只在一个 isolate（应用的 UI isolate）里 `init`，其它 isolate
///   通过**同一个宿主句柄**调用管理/派发/状态类 API；本用例把句柄地址经 SendPort 传给 worker；
/// - 在第二个 isolate 里再次 `init` 必须被拒绝（`host_create` 的进程单例校验），
///   否则同一进程会出现两套插件实例与两条事件队列；
/// - Dart 处理器与宿主动作只允许在 UI isolate 注册：worker 只调用管理/派发/状态类 API。
///
/// 覆盖：两个 worker 并发 `hook_count` / `hook_emit(ASYNC)` / `state_update`，主 isolate
/// 同时跑同步派发；断言不崩溃、事件 `seq` 严格递增、`callId` 不重复、裁决结果不串台、
/// 多 isolate 写入的状态镜像可见。
///
/// 前置：先跑 `pwsh tools/build_native.ps1`（产出 `.native/output/<平台>-<配置>/`）。
/// 找不到原生库或示例插件时跳过而不是失败（CI 里可以先构建再跑）。
library;

import 'dart:async';
import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';

import 'package:ffi/ffi.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:musicxx_extern_plugin/musicxx_extern_plugin.dart';

/// 与 C ABI 的 `MUSICXX_EXTERN_PLUGIN_HOOK_*` 一致（ffigen 不导出宏，这里显式声明）
const int _hookAsync = 0;

/// 状态镜像载荷的固定长度（worker 写入后由插件探针回读长度，证明写入真的落地）
const int _statePayloadBytes = 200;

/// worker 轮数（每个 worker 的调用次数）
const int _rounds = 40;

void main() {
  final _Environment? env = _Environment.detect();

  test(
    '多 isolate 并发调用：不崩溃、seq 单调、callId 不重复、裁决不串台',
    () async {
      if (env == null) {
        markTestSkipped('未找到原生宿主库或示例插件，跳过（先运行 tools/build_native.ps1）');
        return;
      }

      final MusicxxPluginRuntime runtime = MusicxxPluginRuntime.instance;
      addTearDown(runtime.dispose);

      final List<MusicxxPluginEvent> seen = <MusicxxPluginEvent>[];
      final StreamSubscription<MusicxxPluginEvent> sub = runtime.events.listen(seen.add);
      addTearDown(sub.cancel);

      runtime.init(
        config: MusicxxPluginRuntimeConfig(
          appVersion: '0.0.0-isolate',
          platform: MusicxxPluginRuntime.currentPlatform,
          language: 'zh-cn',
          userPluginDir: env.pluginRoot,
        ),
        libraryPath: env.libraryPath,
      );
      runtime.plugins.load('example_native');
      expect(
        runtime.hooks.refreshNativeHandlerCount(MusicxxPluginHookId.playerBeforePlaySong),
        greaterThan(0),
      );

      // 1) 第二个 isolate 再次 init：必须被拒绝（宿主进程单例）
      final ReceivePort guardPort = ReceivePort();
      await Isolate.spawn(
        _initGuardWorker,
        <Object?>[guardPort.sendPort, env.libraryPath, env.pluginRoot],
      );
      final Object? guardResult = await guardPort.first.timeout(const Duration(seconds: 30));
      guardPort.close();
      _step('第二个 isolate 的 init 结果: $guardResult');
      expect(guardResult, isNot('ok'), reason: '同一进程不允许存在第二个宿主');
      expect(guardResult.toString(), contains('进程单例'), reason: '拒绝原因必须可读: $guardResult');

      // 2) 两个 worker 并发调用同一宿主
      final ReceivePort portA = ReceivePort();
      final ReceivePort portB = ReceivePort();
      final Isolate isolateA = await Isolate.spawn(_apiWorker, <Object?>[
        portA.sendPort,
        runtime.host.address,
        env.libraryPath,
        'A',
        _rounds,
      ]);
      final Isolate isolateB = await Isolate.spawn(_apiWorker, <Object?>[
        portB.sendPort,
        runtime.host.address,
        env.libraryPath,
        'B',
        _rounds,
      ]);
      addTearDown(() {
        isolateA.kill(priority: Isolate.immediate);
        isolateB.kill(priority: Isolate.immediate);
      });

      // 3) 主 isolate 同时做同步派发（与 worker 的异步派发并发）
      final List<Map<String, Object?>> verdicts = <Map<String, Object?>>[];
      for (int i = 0; i < 15; ++i) {
        final Map<String, Object?>? verdict = runtime.hooks.decide(
          MusicxxPluginHookId.playerBeforePlaySong,
          <String, Object?>{
            'sid': 'main-$i',
            'song': <String, Object?>{'name': '广告 - 主线程 $i'},
          },
        );
        if (verdict != null) {
          verdicts.add(verdict);
        }
        runtime.pumpEvents();
        await Future<void>.delayed(const Duration(milliseconds: 5));
      }
      expect(verdicts.length, 15, reason: '并发期间同步派发必须照常拿到裁决');

      // 4) worker 报告
      final List<Object?> reports = await Future.wait<Object?>(<Future<Object?>>[
        portA.first,
        portB.first,
      ]);
      portA.close();
      portB.close();
      final List<Map<String, Object?>> workerReports = <Map<String, Object?>>[
        for (final Object? item in reports)
          (item! as Map<Object?, Object?>).cast<String, Object?>(),
      ];
      final List<int> callIds = <int>[];
      for (final Map<String, Object?> report in workerReports) {
        expect(report['error'], isNull, reason: 'worker 报错: ${report['error']}');
        expect(report['hookCountOk'], _rounds, reason: '${report['name']}: hook_count');
        expect(report['emitOk'], _rounds, reason: '${report['name']}: hook_emit(ASYNC)');
        expect(report['stateOk'], _rounds, reason: '${report['name']}: state_update');
        callIds.addAll((report['callIds']! as List<Object?>).cast<int>());
      }
      expect(callIds.length, _rounds * 2);
      expect(
        callIds.toSet().length,
        callIds.length,
        reason: 'callId 必须唯一（宿主分配器是进程级原子计数）',
      );
      _step(
        'worker 报告: ${workerReports.map((Map<String, Object?> r) => '${r['name']}:'
            'count=${r['hookCountOk']} emit=${r['emitOk']} state=${r['stateOk']}').join(', ')}; '
        'callId=${callIds.length} 个且唯一',
      );

      // 5) 每个 callId 的结果事件都被（唯一的）事件队列回传到 UI isolate
      await _pumpUntil(
        () {
          final Set<Object?> decided = seen
              .where((MusicxxPluginEvent e) => e.type == MusicxxPluginEventType.hookDecisionResult)
              .map((MusicxxPluginEvent e) => e.payload['callId'])
              .toSet();
          return callIds.every(decided.contains);
        },
        timeout: const Duration(seconds: 15),
      );
      final Set<int> decidedIds = seen
          .where((MusicxxPluginEvent e) => e.type == MusicxxPluginEventType.hookDecisionResult)
          .map((MusicxxPluginEvent e) => e.payload['callId'])
          .whereType<int>()
          .toSet();
      for (final int callId in callIds) {
        expect(decidedIds, contains(callId), reason: '异步裁决结果必须按 callId 一一回传');
      }

      // 6) 事件 seq 严格递增（跳号 = 丢事件；乱序 = 入队/取队有问题）
      int lastSeq = -1;
      for (final MusicxxPluginEvent event in seen) {
        expect(event.seq, greaterThan(lastSeq), reason: '事件 seq 必须严格递增');
        lastSeq = event.seq;
      }

      // 7) 多 isolate 写入的状态镜像确实落地（插件侧同步读到的字节数）
      int stateLen = -1;
      final DateTime deadline = DateTime.now().add(const Duration(seconds: 5));
      while (DateTime.now().isBefore(deadline)) {
        final Object? probeRaw = runtime.plugins.call(
          'example_native',
          'plugin.example_native.probe',
          const <String, Object?>{},
        );
        final Map<String, Object?> probe = (probeRaw! as Map<Object?, Object?>).cast<String, Object?>();
        stateLen = (probe['stateLen'] as num?)?.toInt() ?? -1;
        if (stateLen == _statePayloadBytes) {
          break;
        }
        runtime.pumpEvents();
        await Future<void>.delayed(const Duration(milliseconds: 20));
      }
      expect(stateLen, _statePayloadBytes, reason: 'worker 写入的状态镜像必须可见');
      _step('事件 ${seen.length} 条 (seq 单调)、裁决结果事件 ${decidedIds.length} 条、状态镜像 ${stateLen}B');

      // 8) 并发风暴之后宿主仍然可用（插件没被卸载、处理器位图没丢）
      expect(
        runtime.hooks.refreshNativeHandlerCount(MusicxxPluginHookId.playerBeforePlaySong),
        greaterThan(0),
      );
      expect(runtime.isRunning, isTrue);
    },
    timeout: const Timeout(Duration(seconds: 120)),
  );
}

/// 测试进度输出（定位卡死步骤用）
void _step(String message) {
  // ignore: avoid_print
  print('[isolate] $message');
}

/// worker isolate：只用宿主句柄调用 API（不 init、不注册 Dart 处理器）
Future<void> _apiWorker(List<Object?> args) async {
  final SendPort port = args[0]! as SendPort;
  final int hostAddress = args[1]! as int;
  final String libraryPath = args[2]! as String;
  final String name = args[3]! as String;
  final int rounds = args[4]! as int;

  final Map<String, Object?> report = <String, Object?>{'name': name};
  try {
    final MusicxxPluginNativeLibrary library = MusicxxPluginNativeLibrary.open(path: libraryPath);
    final MusicxxExternPluginBindings bindings = library.bindings;
    final Pointer<MusicxxExternPluginHost> host =
        Pointer<MusicxxExternPluginHost>.fromAddress(hostAddress);

    final List<int> callIds = <int>[];
    int countOk = 0;
    int emitOk = 0;
    int stateOk = 0;
    for (int i = 0; i < rounds; ++i) {
      // 1) hook_count（读类 API）
      final MusicxxPluginArena countArena = MusicxxPluginArena();
      try {
        final Pointer<Int32> count = countArena.mallocInt();
        final Pointer<MusicxxExternPluginString> log = countArena.outString();
        final int rc = bindings.musicxx_extern_plugin_hook_count(
          host,
          countArena.view('musicxx.player.beforePlaySong'),
          count,
          log,
        );
        if (rc == 0 && count.value >= 1) {
          ++countOk;
        }
      } finally {
        countArena.dispose();
      }

      // 2) hook_emit(ASYNC)：裁决型钩子的异步派发（回执带 callId，结果经事件回传）
      final MusicxxPluginArena emitArena = MusicxxPluginArena();
      try {
        final Pointer<MusicxxExternPluginString> out = emitArena.outString();
        final Pointer<MusicxxExternPluginString> log = emitArena.outString();
        final int rc = bindings.musicxx_extern_plugin_hook_emit(
          host,
          emitArena.view('musicxx.player.beforePlaySong'),
          emitArena.view('{"sid":"$name-$i","song":{"name":"广告 - $name"}}'),
          _hookAsync,
          0,
          out,
          log,
        );
        if (rc == 0) {
          final Map<String, Object?> ack = decodeJsonObject(_takeString(out, bindings));
          final Object? callId = ack['callId'];
          if (callId is int) {
            callIds.add(callId);
          }
          ++emitOk;
        }
      } finally {
        emitArena.dispose();
      }

      // 3) state_update（状态镜像写入；值定长，便于主 isolate 回读校验）
      final MusicxxPluginArena stateArena = MusicxxPluginArena();
      try {
        final Pointer<MusicxxExternPluginString> log = stateArena.outString();
        final int rc = bindings.musicxx_extern_plugin_state_update(
          host,
          stateArena.view(MusicxxPluginState.keySong),
          stateArena.view(_statePayload(name, i)),
          log,
        );
        if (rc == 0) {
          ++stateOk;
        }
      } finally {
        stateArena.dispose();
      }

      await Future<void>.delayed(const Duration(milliseconds: 3));
    }
    report['hookCountOk'] = countOk;
    report['emitOk'] = emitOk;
    report['stateOk'] = stateOk;
    report['callIds'] = callIds;
  } catch (error, stack) {
    report['error'] = '$error\n$stack';
  }
  Isolate.exit(port, report);
}

/// worker isolate：尝试在第二个 isolate 里 init（应被宿主以"进程单例"拒绝）
Future<void> _initGuardWorker(List<Object?> args) async {
  final SendPort port = args[0]! as SendPort;
  final String libraryPath = args[1]! as String;
  final String pluginRoot = args[2]! as String;
  String result;
  try {
    MusicxxPluginRuntime.instance.init(
      config: MusicxxPluginRuntimeConfig(
        appVersion: '0.0.0-isolate',
        platform: MusicxxPluginRuntime.currentPlatform,
        userPluginDir: pluginRoot,
      ),
      libraryPath: libraryPath,
    );
    result = 'ok';
    MusicxxPluginRuntime.instance.dispose();
  } catch (error) {
    result = '$error';
  }
  Isolate.exit(port, result);
}

/// 定长状态镜像载荷（ASCII，utf8 字节数 == [_statePayloadBytes]）
String _statePayload(String name, int index) {
  final String head = '{"sid":"$name-$index","pad":"';
  const String tail = '"}';
  final int padLength = _statePayloadBytes - head.length - tail.length;
  return head + 'x' * (padLength > 0 ? padLength : 0) + tail;
}

/// 读取并释放宿主堆字符串（测试自带实现：包内不导出 takeOutString）
String _takeString(Pointer<MusicxxExternPluginString> out, MusicxxExternPluginBindings bindings) {
  final MusicxxExternPluginString value = out.ref;
  final String text = value.data == nullptr
      ? ''
      : value.data.cast<Utf8>().toDartString(length: value.size);
  if (value.data != nullptr) {
    bindings.musicxx_extern_plugin_string_free(out);
  }
  return text;
}

/// 等待条件成立（最多 timeout；每 20 ms 泵一次事件并让出事件循环）
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

/// 原生库 + 示例插件目录探测（找不到就跳过用例）
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
