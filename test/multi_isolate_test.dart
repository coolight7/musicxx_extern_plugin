/// 多 isolate 并发调用用例
///
/// 约定：
/// - 插件框架是**进程单例**：只在一个 isolate（主程序的 UI isolate）里 `init`，其它 isolate
///   通过**同一个插件框架句柄**调用管理/派发/状态类 API；本用例把句柄地址经 SendPort 传给 worker；
/// - 在第二个 isolate 里再次 `init` 必须被拒绝（`host_create` 的进程单例校验），
///   否则同一进程会出现两套插件实例与两条事件队列；
/// - 事件队列是**进程级共享**的：worker 只提交、把 `requestId` 报回来，结果统一由主 isolate
///   从队列里取 —— 两边都去取会互相抢走对方的结果。
///
/// 覆盖：两个 worker 并发提交「读处理器列表 / 裁决型能力 / 快照频道发布」，主 isolate 同时
/// 派发裁决；断言不崩溃、`requestId` 不重复、每条请求恰好一条结果、裁决不串台、
/// 事件 `seq` 严格递增、多 isolate 写入的快照频道可见。
///
/// 前置：先跑 `tools/build_examples.ps1`（Windows）或 `tools/build_examples.sh`（Linux/macOS）（产出 `.native/output/<平台>-<架构>-<配置>/`，含示例插件）。
/// 找不到原生库或示例插件时跳过而不是失败（CI 里可以先构建再跑）。
library;

import 'dart:async';
import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';

import 'package:flutter_test/flutter_test.dart';
import 'package:musicxx_extern_plugin/musicxx_extern_plugin.dart';

/// 快照频道载荷的固定长度（worker 写入后由插件探针回读长度，证明写入真的生效）
const int _statePayloadBytes = 200;

/// worker 轮数（每个 worker 的调用次数）
const int _rounds = 40;

void main() {
  final _Environment? env = _Environment.detect();

  test('多 isolate 并发调用：不崩溃、seq 单调、requestId 不重复、裁决不串台', () async {
    if (env == null) {
      markTestSkipped(
        '未找到原生插件框架库或示例插件，跳过（先跑 tools/build_examples.ps1（Windows）或 tools/build_examples.sh（Linux/macOS）产出原生库与示例插件）',
      );
      return;
    }

    final MusicxxPluginRuntime runtime = MusicxxPluginRuntime.instance;
    addTearDown(runtime.dispose);

    final List<MusicxxPluginEvent> seen = <MusicxxPluginEvent>[];
    final StreamSubscription<MusicxxPluginEvent> sub = runtime.events.listen(
      seen.add,
    );
    addTearDown(sub.cancel);

    await runtime.init(
      config: MusicxxPluginRuntimeConfig(
        appVersion: '0.0.0-isolate',
        platform: MusicxxPluginRuntime.currentPlatform,
        language: 'zh-cn',
        userPluginDir: env.pluginRoot,
      ),
      libraryPath: env.libraryPath,
    );
    await runtime.plugins.load('example_native');
    expect(
      (await runtime.capabilities.refreshNativeHandlers(
        MusicxxCapabilityId.playerBeforePlaySong,
      ))!.length,
      greaterThan(0),
    );

    // 1) 第二个 isolate 再次 init：必须被拒绝（插件框架进程单例）
    final ReceivePort guardPort = ReceivePort();
    await Isolate.spawn(_initGuardWorker, <Object?>[
      guardPort.sendPort,
      env.libraryPath,
      env.pluginRoot,
    ]);
    final guardResult = await guardPort.first.timeout(
      const Duration(seconds: 30),
    );
    guardPort.close();
    _step('第二个 isolate 的 init 结果: $guardResult');
    expect(guardResult, isNot('ok'), reason: '同一进程不允许存在第二个插件框架');
    expect(
      guardResult.toString(),
      contains('process singleton'),
      reason: '拒绝原因必须可读: $guardResult',
    );

    // 2) 两个 worker 并发提交
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

    // 3) 主 isolate 同时派发（与 worker 的提交并发进出同一条队列）
    final List<Map<String, Object?>> verdicts = <Map<String, Object?>>[];
    for (int i = 0; i < 15; ++i) {
      final Map<String, Object?>? verdict = await runtime.capabilities.dispatch(
        MusicxxCapabilityId.playerBeforePlaySong,
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
    expect(verdicts.length, 15, reason: '并发期间派发必须照常拿到裁决');

    // 4) worker 报告：每条请求的 requestId 与它提交的是哪一类
    final List<Object?> reports = await Future.wait<Object?>(<Future<Object?>>[
      portA.first,
      portB.first,
    ]);
    portA.close();
    portB.close();
    final List<Map<String, Object?>> workerReports = <Map<String, Object?>>[
      for (final item in reports)
        (item! as Map<Object?, Object?>).cast<String, Object?>(),
    ];
    final Map<int, String> requests = <int, String>{};
    for (final Map<String, Object?> report in workerReports) {
      expect(report['error'], isNull, reason: 'worker 报错: ${report['error']}');
      expect(
        report['submitted'],
        _rounds * 3,
        reason: '${report['name']}: 每轮三条请求都要受理',
      );
      final Map<Object?, Object?> ids =
          (report['requests']! as Map<Object?, Object?>);
      for (final MapEntry<Object?, Object?> entry in ids.entries) {
        requests[entry.key! as int] = entry.value! as String;
      }
    }
    expect(
      requests.length,
      _rounds * 3 * 2,
      reason: 'requestId 必须唯一（插件框架分配器是进程级原子计数）',
    );
    _step('worker 报告: 已受理 ${requests.length} 条请求');

    // 5) 每条请求都恰好收到一条结果，且结果与提交的类型对得上
    final Map<int, Map<String, Object?>> results =
        <int, Map<String, Object?>>{};
    await _pumpUntil(() {
      for (final MusicxxPluginEvent event in seen) {
        if (event.type != MusicxxPluginEventType.requestResult) {
          continue;
        }
        final Object? id = event.payload['requestId'];
        if (id is int) {
          results[id] = event.payload;
        }
      }
      return requests.keys.every(results.containsKey);
    }, timeout: const Duration(seconds: 30));
    for (final MapEntry<int, String> entry in requests.entries) {
      final Map<String, Object?>? payload = results[entry.key];
      expect(payload, isNotNull, reason: 'requestId ${entry.key} 没有结果事件');
      expect(payload!['op'], entry.value, reason: '结果事件的 op 要对得上');
      expect(payload['ok'], isTrue, reason: '请求 ${entry.key} 应当成功: $payload');
      final Object? data = payload['data'];
      if (entry.value == 'capability.dispatch') {
        final Map<String, Object?> result = (data! as Map<Object?, Object?>)
            .cast<String, Object?>();
        final Map<String, Object?> verdict =
            (result['result']! as Map<Object?, Object?>)
                .cast<String, Object?>();
        expect(verdict['action'], 'skip', reason: '裁决结果不能串台: $result');
      }
    }

    // 6) 事件 seq 严格递增（跳号 = 丢事件；乱序 = 入队/取队有问题）
    int lastSeq = -1;
    for (final MusicxxPluginEvent event in seen) {
      expect(event.seq, greaterThan(lastSeq), reason: '事件 seq 必须严格递增');
      lastSeq = event.seq;
    }

    // 7) 多 isolate 写入的快照频道确实生效（插件侧同步读到的字节数）
    int stateLen = -1;
    final DateTime deadline = DateTime.now().add(const Duration(seconds: 5));
    while (DateTime.now().isBefore(deadline)) {
      final probeRaw = await runtime.plugins.call(
        'example_native',
        'plugin.example_native.probe',
        const <String, Object?>{},
      );
      final Map<String, Object?> probe = (probeRaw! as Map<Object?, Object?>)
          .cast<String, Object?>();
      stateLen = (probe['stateLen'] as num?)?.toInt() ?? -1;
      if (stateLen == _statePayloadBytes) {
        break;
      }
      runtime.pumpEvents();
      await Future<void>.delayed(const Duration(milliseconds: 20));
    }
    expect(stateLen, _statePayloadBytes, reason: 'worker 写入的快照频道必须可见');
    _step(
      '事件 ${seen.length} 条 (seq 单调)、结果事件 ${results.length} 条、快照频道 ${stateLen}B',
    );

    // 8) 并发风暴之后插件框架仍然可用（插件没被卸载、处理器还在）
    expect(
      (await runtime.capabilities.refreshNativeHandlers(
        MusicxxCapabilityId.playerBeforePlaySong,
      ))!.length,
      greaterThan(0),
    );
    expect(runtime.isRunning, isTrue);
  }, timeout: const Timeout(Duration(seconds: 120)));
}

/// 测试进度输出（定位卡死步骤用）
void _step(String message) {
  // ignore: avoid_print
  print('[isolate] $message');
}

/// worker isolate：只用插件框架句柄提交请求，结果由主 isolate 统一取
Future<void> _apiWorker(List<Object?> args) async {
  final SendPort port = args[0]! as SendPort;
  final int hostAddress = args[1]! as int;
  final String libraryPath = args[2]! as String;
  final String name = args[3]! as String;
  final int rounds = args[4]! as int;

  final Map<String, Object?> report = <String, Object?>{'name': name};
  try {
    final MusicxxPluginNativeLibrary library = MusicxxPluginNativeLibrary.open(
      path: libraryPath,
    );
    final MusicxxExternPluginBindings bindings = library.bindings;
    final Pointer<MusicxxExternPluginHost> host =
        Pointer<MusicxxExternPluginHost>.fromAddress(hostAddress);

    final Map<int, String> requests = <int, String>{};
    int submitted = 0;

    /// 提交一次并记下 requestId（结果留给主 isolate）
    bool submit(
      String op,
      int Function(Pointer<Int64> outId, Pointer<MusicxxExternPluginString> log)
      call,
    ) {
      final MusicxxPluginArena arena = MusicxxPluginArena();
      try {
        final Pointer<Int64> outId = arena.mallocInt64();
        final Pointer<MusicxxExternPluginString> log = arena.outString();
        final int rc = call(outId, log);
        if (rc != 0) {
          return false;
        }
        requests[outId.value] = op;
        ++submitted;
        return true;
      } finally {
        arena.dispose();
      }
    }

    for (int i = 0; i < rounds; ++i) {
      // 1) 读处理器列表（只读查询也算一次请求）
      final MusicxxPluginArena countArena = MusicxxPluginArena();
      try {
        submit(
          'capability.handlers',
          (Pointer<Int64> outId, Pointer<MusicxxExternPluginString> log) =>
              bindings.musicxx_extern_plugin_capability_handlers(
                host,
                countArena.view('musicxx.player.beforePlaySong'),
                outId,
                log,
              ),
        );
      } finally {
        countArena.dispose();
      }

      // 2) 裁决型能力：结果里的 action 用于检查不串台
      final MusicxxPluginArena emitArena = MusicxxPluginArena();
      try {
        submit(
          'capability.dispatch',
          (Pointer<Int64> outId, Pointer<MusicxxExternPluginString> log) =>
              bindings.musicxx_extern_plugin_capability_dispatch(
                host,
                emitArena.view('musicxx.player.beforePlaySong'),
                emitArena.view(
                  '{"sid":"$name-$i","song":{"name":"广告 - $name"}}',
                ),
                outId,
                log,
              ),
        );
      } finally {
        emitArena.dispose();
      }

      // 3) 快照频道写入（值定长，便于主 isolate 回读校验）
      final MusicxxPluginArena stateArena = MusicxxPluginArena();
      try {
        submit(
          'broadcast.publish',
          (Pointer<Int64> outId, Pointer<MusicxxExternPluginString> log) =>
              bindings.musicxx_extern_plugin_broadcast_publish(
                host,
                stateArena.view(MusicxxBroadcastId.stateSong),
                stateArena.view(_statePayload(name, i)),
                outId,
                log,
              ),
        );
      } finally {
        stateArena.dispose();
      }

      await Future<void>.delayed(const Duration(milliseconds: 3));
    }
    report['submitted'] = submitted;
    report['requests'] = requests;
  } catch (error, stack) {
    report['error'] = '$error\n$stack';
  }
  Isolate.exit(port, report);
}

/// worker isolate：尝试在第二个 isolate 里 init（应被插件框架以"进程单例"拒绝）
Future<void> _initGuardWorker(List<Object?> args) async {
  final SendPort port = args[0]! as SendPort;
  final String libraryPath = args[1]! as String;
  final String pluginRoot = args[2]! as String;
  String result;
  try {
    await MusicxxPluginRuntime.instance.init(
      config: MusicxxPluginRuntimeConfig(
        appVersion: '0.0.0-isolate',
        platform: MusicxxPluginRuntime.currentPlatform,
        userPluginDir: pluginRoot,
      ),
      libraryPath: libraryPath,
    );
    result = 'ok';
    await MusicxxPluginRuntime.instance.dispose();
  } catch (error) {
    result = '$error';
  }
  Isolate.exit(port, result);
}

/// 定长快照频道载荷（ASCII，utf8 字节数 == [_statePayloadBytes]）
String _statePayload(String name, int index) {
  final String head = '{"sid":"$name-$index","pad":"';
  const String tail = '"}';
  final int padLength = _statePayloadBytes - head.length - tail.length;
  return head + 'x' * (padLength > 0 ? padLength : 0) + tail;
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
