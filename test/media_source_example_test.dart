/// `example_override_media_source` 的集成测试（真实原生库 + 真实 JS 运行时）
///
/// 覆盖：
/// - 功能 1：`musicxx.player.source.resolve` 的 Bili 分支（BV 号 → 直链 + 请求头）；
/// - 多 P 视频不接管（载荷里没有分 P 序号，硬接管会把第 2 P 播成第 1 P）；
/// - 开关关闭后不接管；
/// - 设置页能力与 `probe` 自检。
///
/// B 站接口用一个本地 HTTP 服务回答：插件真的走插件框架的网络能力，但不联网、结果可复现
/// （测试会把复制出去的插件的接口域名改成本地服务地址）。
/// 前置：先跑 `tools/build_examples.ps1`（Windows）或 `tools/build_examples.sh`（Linux/macOS），
/// 产出原生库与 `.native/output/<平台>/plugins/` 里的示例插件；找不到时跳过。
library;

import 'dart:async';
import 'dart:convert' as convert;
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:musicxx_extern_plugin/musicxx_extern_plugin.dart';
import 'package:path/path.dart' as path;

const String pluginId = 'example_override_media_source';
const String biliType = 'plugin.$pluginId.plugin_bili';
const String proxyType = 'plugin.$pluginId.plugin_proxy_local';

/// 示例用的单 P 视频（真实存在的 BV 号；测试里接口是假的）
const String singlePageBvid = 'BV1GJ411x7h7';

/// 假接口返回"多 P"的 BV 号（与上面不同，避免命中插件缓存）
const String multiPageBvid = 'BV1xx411c7mD';

/// 假接口给出的音频直链（插件会拼上自己选中的那一路的 id）
const String fakeAudioUrl =
    'https://cdn.example.test/audio.m4s?deadline=9999999999';

/// 匿名请求 B 站只能拿到三路音频里最高 192k 的那一路（30280）
const String fakeBestAudioUrl = '$fakeAudioUrl&id=30280';

void main() {
  final _Environment? env = _Environment.detect();

  test('功能 1：Bili 分支用 BV 号换直链与请求头', () async {
    if (env == null || !env.hasPlugin(pluginId)) {
      markTestSkipped(
        '未找到原生插件框架库或示例插件，跳过（先跑 tools/build_examples.ps1（Windows）或 tools/build_examples.sh（Linux/macOS）产出原生库与示例插件）',
      );
      return;
    }

    final _Harness harness = _Harness(env);
    addTearDown(harness.dispose);
    await harness.start();

    // 配置读完（三个 getConfig 动作往返）
    await harness.pumpUntil(
      () async => (await harness.probe())['configLoaded'] == 3,
    );

    final Map<String, Object?>? verdict = await harness.resolveBili(
      crudeSrc: singlePageBvid,
      src: 'https://cdn.example.test/already-resolved.m4s',
      quality: 'upper',
    );
    expect(verdict, isNotNull, reason: '单 P 视频应当由插件接管');
    final Map<String, Object?> src = _srcOf(verdict!);
    expect(src['path'], fakeBestAudioUrl,
        reason: '返回的应是插件自己解析出来的直链（按音质偏好选中 30280）');
    expect(src['kind'], 'url');
    final Map<Object?, Object?> headers =
        (src['headers'] as Map).cast<Object?, Object?>();
    expect(headers['Referer'], 'https://www.bilibili.com');
    expect(headers['Origin'], 'https://www.bilibili.com');
    expect('${headers['User-Agent']}', contains('Mozilla/5.0'));
    expect(src['durationMs'], 213000, reason: '分 P 时长来自 view 接口（秒 → 毫秒）');
    expect(src['size'], greaterThan(0), reason: '按带宽与时长估算的文件大小');

    // 同一个 BV 再问一次：命中插件内存缓存，不再打接口
    final int fetchesBefore = harness.fetchCalls;
    final Map<String, Object?>? again = await harness.resolveBili(
      crudeSrc: singlePageBvid,
      src: 'https://cdn.example.test/already-resolved.m4s',
      quality: 'upper',
    );
    expect(_srcOf(again!)['path'], fakeBestAudioUrl);
    expect(harness.fetchCalls, fetchesBefore, reason: '同一个 BV + 音质不应重复请求接口');

    // probe 的计数（Bili 被问 2 次、成功 2 次）
    final Map<String, Object?> probe = await harness.probe();
    final Map<String, Object?> stats =
        (probe['stats'] as Map).cast<String, Object?>();
    expect(stats['biliCalls'], 2);
    expect(stats['biliOk'], 2);
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('功能 1：多 P 视频不接管（交回主程序）', () async {
    if (env == null || !env.hasPlugin(pluginId)) {
      markTestSkipped(
        '未找到原生插件框架库或示例插件，跳过（先跑 tools/build_examples.ps1（Windows）或 tools/build_examples.sh（Linux/macOS）产出原生库与示例插件）',
      );
      return;
    }
    final _Harness harness = _Harness(env);
    addTearDown(harness.dispose);
    await harness.start();
    await harness.pumpUntil(
      () async => (await harness.probe())['configLoaded'] == 3,
    );

    final Map<String, Object?>? verdict = await harness.resolveBili(
      crudeSrc: multiPageBvid,
      src: 'https://cdn.example.test/multi.m4s',
      quality: 'upper',
    );
    expect(verdict, isNull, reason: '多 P 视频必须返回 null，让主程序已经算好的那一份继续生效');

    final Map<String, Object?> probe = await harness.probe();
    final Map<String, Object?> stats =
        (probe['stats'] as Map).cast<String, Object?>();
    expect(stats['biliCalls'], 1);
    expect(stats['biliOk'], 0);
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('功能 2：插件音源 plugin_bili 用 BV 号换直链', () async {
    if (env == null || !env.hasPlugin(pluginId)) {
      markTestSkipped(
        '未找到原生插件框架库或示例插件，跳过（先跑 tools/build_examples.ps1（Windows）或 tools/build_examples.sh（Linux/macOS）产出原生库与示例插件）',
      );
      return;
    }
    final _Harness harness = _Harness(env);
    addTearDown(harness.dispose);
    await harness.start();
    await harness.pumpUntil(
      () async => (await harness.probe())['configLoaded'] == 3,
    );

    // 加载时声明了自己的音源类型（自省用；能不能播看处理器）
    expect(harness.definedSources, contains(biliType));

    // 插件音源：类型串在音源信息里（`pluginType`），`src` 就是插件自己的资源 id
    //（内置类型没有值时 type 是空串）
    final Map<String, Object?>? verdict = await harness.resolveSrc(
      type: '',
      src: singlePageBvid,
      pluginType: biliType,
      quality: 'hq',
    );
    expect(verdict, isNotNull, reason: '插件音源应当由插件接管');
    final Map<String, Object?> src = _srcOf(verdict!);
    expect(src['path'], fakeBestAudioUrl);
    expect(src['kind'], 'url');
    expect((src['headers'] as Map)['Referer'], 'https://www.bilibili.com');

    final Map<String, Object?> probe = await harness.probe();
    final Map<String, Object?> stats =
        (probe['stats'] as Map).cast<String, Object?>();
    expect(stats['pluginBiliCalls'], 1);
    expect(stats['biliCalls'], 0, reason: '内置 Bili 开关这条路没被问到');
    expect((probe['sourceTypes'] as Map)['bili'], biliType);
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('开关：关掉 pluginBiliEnabled 后插件音源不接管', () async {
    if (env == null || !env.hasPlugin(pluginId)) {
      markTestSkipped(
        '未找到原生插件框架库或示例插件，跳过（先跑 tools/build_examples.ps1（Windows）或 tools/build_examples.sh（Linux/macOS）产出原生库与示例插件）',
      );
      return;
    }
    final _Harness harness = _Harness(env);
    addTearDown(harness.dispose);
    await harness.start();
    await harness.pumpUntil(
      () async => (await harness.probe())['configLoaded'] == 3,
    );

    await harness.runtime.plugins.call(
      pluginId,
      'plugin.$pluginId.setSwitch',
      <String, Object?>{'id': 'pluginBiliEnabled', 'value': false},
    );

    final Map<String, Object?>? verdict = await harness.resolveSrc(
      type: '',
      src: singlePageBvid,
      pluginType: biliType,
      quality: 'hq',
    );
    expect(verdict, isNull, reason: '开关关闭时不接管（该来源会解析失败）');
    expect(harness.fetchCalls, 0);
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('开关：关掉 overrideBili 后不再接管', () async {
    if (env == null || !env.hasPlugin(pluginId)) {
      markTestSkipped(
        '未找到原生插件框架库或示例插件，跳过（先跑 tools/build_examples.ps1（Windows）或 tools/build_examples.sh（Linux/macOS）产出原生库与示例插件）',
      );
      return;
    }
    final _Harness harness = _Harness(env);
    addTearDown(harness.dispose);
    await harness.start();
    await harness.pumpUntil(
      () async => (await harness.probe())['configLoaded'] == 3,
    );

    // 与用户在设置页拨开关等价：能力返回刷新后的页面
    final Object? raw = await harness.runtime.plugins.call(
      pluginId,
      'plugin.$pluginId.setSwitch',
      <String, Object?>{'id': 'overrideBili', 'value': false},
    );
    expect(raw, isA<Map<String, Object?>>());
    expect(
      (raw! as Map<String, Object?>)['view'],
      isA<Map<String, Object?>>(),
      reason: '改开关要当场返回新页面',
    );

    final Map<String, Object?>? verdict = await harness.resolveBili(
      crudeSrc: singlePageBvid,
      src: 'https://cdn.example.test/already-resolved.m4s',
      quality: 'upper',
    );
    expect(verdict, isNull, reason: '开关关闭时不接管（主程序原有解析照旧）');
    expect(harness.fetchCalls, 0, reason: '开关关闭时不应再请求 B 站接口');

    final Map<String, Object?> probe = await harness.probe();
    expect((probe['config'] as Map)['overrideBili'], false);
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('功能 3：本地文件走插件 HTTP 服务（GET / Range / HEAD）', () async {
    if (env == null || !env.hasPlugin(pluginId)) {
      markTestSkipped(
        '未找到原生插件框架库或示例插件，跳过（先跑 tools/build_examples.ps1（Windows）或 tools/build_examples.sh（Linux/macOS）产出原生库与示例插件）',
      );
      return;
    }
    final _Harness harness = _Harness(env);
    addTearDown(harness.dispose);
    await harness.start();
    await harness.pumpUntil(
      () async => (await harness.probe())['configLoaded'] == 3,
    );

    // 造一个 200 KB 的文件（内容可校验）
    final File media = File(path.join(harness.work.path, 'sample.bin'));
    final List<int> content = List<int>.generate(200 * 1024, (int i) => i % 251);
    media.writeAsBytesSync(content);

    final Map<String, Object?>? verdict = await harness.resolveSrc(
      type: '',
      src: media.path,
      pluginType: proxyType,
      quality: 'hq',
    );
    expect(verdict, isNotNull, reason: '本地代理音源应当由插件接管');
    final Map<String, Object?> src = _srcOf(verdict!);
    expect(src['kind'], 'url');
    final String url = src['path']! as String;
    expect(url, startsWith('http://127.0.0.1:'));
    expect(src['size'], content.length);

    // 整段 GET
    final HttpClient client = HttpClient();
    addTearDown(client.close);
    final _HttpResult full = await _httpGet(client, url);
    expect(full.status, 200);
    expect(full.contentLength, content.length);
    expect(full.acceptRanges, 'bytes');
    expect(full.body.length, content.length);
    expect(full.body.sublist(0, 16), content.sublist(0, 16));

    // 单段 Range：拖动进度时播放器就是这么请求的
    final _HttpResult partial = await _httpGet(
      client,
      url,
      headers: <String, String>{'Range': 'bytes=100-199'},
    );
    expect(partial.status, 206);
    expect(partial.contentRange, 'bytes 100-199/${content.length}');
    expect(partial.body, content.sublist(100, 200));

    // 后缀 Range
    final _HttpResult suffix = await _httpGet(
      client,
      url,
      headers: <String, String>{'Range': 'bytes=-64'},
    );
    expect(suffix.status, 206);
    expect(suffix.body, content.sublist(content.length - 64));

    // 越界 Range → 416
    final _HttpResult beyond = await _httpGet(
      client,
      url,
      headers: <String, String>{'Range': 'bytes=${content.length + 10}-'},
    );
    expect(beyond.status, 416);

    // HEAD：只要长度
    final _HttpResult head = await _httpGet(client, url, method: 'HEAD');
    expect(head.status, 200);
    expect(head.contentLength, content.length);
    expect(head.body, isEmpty);

    // 错误的 token → 404（服务端只按 token 查表，不解析路径）
    final _HttpResult missing = await _httpGet(
      client,
      url.replaceFirst(RegExp(r'/f/[0-9a-f]+'), '/f/deadbeef'),
    );
    expect(missing.status, 404);

    // 计数与状态
    final Map<String, Object?> probe = await harness.probe();
    expect((probe['stats'] as Map)['proxyCalls'], 1);
    final Map<String, Object?> server =
        (probe['server'] as Map).cast<String, Object?>();
    expect(server['running'], isTrue);
    expect(server['files'], greaterThanOrEqualTo(1));
    expect(server['requests'], greaterThanOrEqualTo(2));
    expect(server['bytesServed'], greaterThanOrEqualTo(content.length));

    // 同一个文件再解析一次：命中登记（URL 不变），不会多一条映射
    final Map<String, Object?>? again = await harness.resolveSrc(
      type: '',
      src: media.path,
      pluginType: proxyType,
      quality: 'hq',
    );
    expect(_srcOf(again!)['path'], url);

    // 卸载插件 → 服务跟着关掉，端口不再接受连接
    harness.runtime.plugins.unload(pluginId);
    await expectLater(
      _httpGet(client, url),
      throwsA(isA<SocketException>()),
      reason: '插件卸载后本地服务应当已关闭',
    );
  }, timeout: const Timeout(Duration(seconds: 90)));

  test('功能 3：serveBytes 登记一段内容', () async {
    if (env == null || !env.hasPlugin(pluginId)) {
      markTestSkipped(
        '未找到原生插件框架库或示例插件，跳过（先跑 tools/build_examples.ps1（Windows）或 tools/build_examples.sh（Linux/macOS）产出原生库与示例插件）',
      );
      return;
    }
    final _Harness harness = _Harness(env);
    addTearDown(harness.dispose);
    await harness.start();
    await harness.pumpUntil(
      () async => (await harness.probe())['configLoaded'] == 3,
    );

    const String text = '#EXTM3U\n#EXT-X-VERSION:3\nseg0.ts\n';
    final Object? raw = await harness.runtime.plugins.call(
      pluginId,
      'plugin.$pluginId.serveText',
      <String, Object?>{'text': text},
    );
    expect(raw, isA<Map<String, Object?>>());
    final Map<String, Object?> served = raw! as Map<String, Object?>;
    expect(served['ok'], isTrue, reason: 'serveBytes 应当成功: $served');

    final HttpClient client = HttpClient();
    addTearDown(client.close);
    final _HttpResult result = await _httpGet(client, served['url']! as String);
    expect(result.status, 200);
    expect(result.contentType, contains('text/plain'));
    expect(convert.utf8.decode(result.body), text);
  }, timeout: const Timeout(Duration(seconds: 90)));

  test('开关：关掉 proxyLocalEnabled 后本地代理不接管', () async {
    if (env == null || !env.hasPlugin(pluginId)) {
      markTestSkipped(
        '未找到原生插件框架库或示例插件，跳过（先跑 tools/build_examples.ps1（Windows）或 tools/build_examples.sh（Linux/macOS）产出原生库与示例插件）',
      );
      return;
    }
    final _Harness harness = _Harness(env);
    addTearDown(harness.dispose);
    await harness.start();
    await harness.pumpUntil(
      () async => (await harness.probe())['configLoaded'] == 3,
    );

    await harness.runtime.plugins.call(
      pluginId,
      'plugin.$pluginId.setSwitch',
      <String, Object?>{'id': 'proxyLocalEnabled', 'value': false},
    );

    final Map<String, Object?>? verdict = await harness.resolveSrc(
      type: '',
      src: 'D:/no/such/file.mp3',
      pluginType: proxyType,
      quality: 'hq',
    );
    expect(verdict, isNull, reason: '开关关闭时不接管（该来源会解析失败）');
    final Map<String, Object?> probe = await harness.probe();
    expect((probe['stats'] as Map)['proxyCalls'], 0);
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('设置页与 probe：初始值、页面内容', () async {
    if (env == null || !env.hasPlugin(pluginId)) {
      markTestSkipped(
        '未找到原生插件框架库或示例插件，跳过（先跑 tools/build_examples.ps1（Windows）或 tools/build_examples.sh（Linux/macOS）产出原生库与示例插件）',
      );
      return;
    }
    final _Harness harness = _Harness(env);
    addTearDown(harness.dispose);
    await harness.start();
    await harness.pumpUntil(
      () async => (await harness.probe())['configLoaded'] == 3,
    );

    // 主页入口必须指向设置页（只声明入口不实现能力时，页面打开会失败）
    final List<MusicxxPluginUIItem> entries = MusicxxPluginUIItems.byType(
      harness.runtime.plugins.uiSnapshot(),
      MusicxxPluginUIType.homeEntry,
    ).where((MusicxxPluginUIItem item) => item.plugin == pluginId).toList();
    expect(entries, hasLength(1));
    expect(entries.first.viewId, 'settings');

    final Object? raw = await harness.runtime.plugins.call(
      pluginId,
      'plugin.$pluginId.settings',
      const <String, Object?>{},
    );
    expect(raw, isA<Map<String, Object?>>());
    final Map<String, Object?> view =
        (raw! as Map<String, Object?>)['view']! as Map<String, Object?>;
    expect(view['title'], isNotEmpty);
    final List<String> texts = <String>[];
    _collectText(view, texts);
    expect(texts.any((String text) => text.contains('覆盖内置 Bili 解析')), isTrue,
        reason: '设置页要有功能 1 的开关: $texts');
    expect(texts.any((String text) => text.contains('自定义音源 plugin_bili')), isTrue,
        reason: '设置页要有功能 2 的开关: $texts');
    expect(texts.any((String text) => text.contains('播放示例视频')), isTrue,
        reason: '设置页要有演示按钮: $texts');
    expect(texts.any((String text) => text.contains('播放自定义音源')), isTrue,
        reason: '设置页要有自定义音源的演示按钮: $texts');
    expect(texts.any((String text) => text.contains('本地文件走 HTTP')), isTrue,
        reason: '设置页要有功能 3 的开关: $texts');
    expect(texts.any((String text) => text.contains('本地 HTTP 服务')), isTrue,
        reason: '设置页要有本地服务状态: $texts');
    expect(_switchValue(view, 'overrideBili'), isTrue, reason: '默认开启');
    expect(_switchValue(view, 'pluginBiliEnabled'), isTrue, reason: '默认开启');
    expect(_switchValue(view, 'proxyLocalEnabled'), isTrue, reason: '默认开启');
  }, timeout: const Timeout(Duration(seconds: 60)));
}

/// 一个测试用的小环境：运行时 + 假动作 + 插件加载
class _Harness {
  _Harness(this.env);

  final _Environment env;
  final MusicxxPluginRuntime runtime = MusicxxPluginRuntime.create();
  late final Directory work;
  late final Directory pluginRoot;

  /// 假 B 站接口的应答构造（在 _registerHostActions 里赋值）
  late String Function(String url) fakeBiliBody;

  /// `musicxx.net.fetch` 被调用的次数（验证缓存/开关时用）
  int fetchCalls = 0;

  /// 插件声明的音源类型 id（`musicxx.source.define` 的参数）
  final List<String> definedSources = <String>[];

  /// 假的 config.json 内容（`musicxx.storage.get/set` 用）
  final Map<String, Object?> configFile = <String, Object?>{};

  Future<void> start() async {
    work = Directory.systemTemp.createTempSync('musicxx_media_source_test');
    pluginRoot = Directory(path.join(work.path, 'plugins'))
      ..createSync(recursive: true);
    // 先起本地假接口，再把插件复制到临时目录（复制时把接口域名改成它）
    await _startFakeBiliApi();
    _copyPluginWithFakeApi();
    _registerHostActions();

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
    expect(runtime.plugins.findLoaded(pluginId), isNotNull, reason: '插件应能加载');
    expect(
      runtime.hooks.refreshNativeHandlerCount(
        MusicxxPluginHookId.playerSourceResolve,
      ),
      1,
      reason: '音源解析钩子应登记成功',
    );
  }

  /// 本地假 B 站接口的地址（复制插件时写进去）
  String get apiBase => 'http://127.0.0.1:${_server!.port}';

  HttpServer? _server;

  /// 起一个只监听回环的 HTTP 服务回答 `/x/web-interface/view` 与 `/x/player/playurl`
  Future<void> _startFakeBiliApi() async {
    _server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    _server!.listen((HttpRequest request) async {
      ++fetchCalls;
      final String body = fakeBiliBody(request.uri.toString());
      request.response.headers.contentType = ContentType.json;
      request.response.write(body);
      await request.response.close();
    });
  }

  /// 复制插件目录；`plugin.js` 里的 B 站接口域名换成本地服务
  void _copyPluginWithFakeApi() {
    final Directory from = Directory(path.join(env.pluginRoot, pluginId));
    final Directory to = Directory(path.join(pluginRoot.path, pluginId));
    for (final FileSystemEntity entity in from.listSync(recursive: true)) {
      final String relative = path.relative(entity.path, from: from.path);
      final String target = path.join(to.path, relative);
      if (entity is Directory) {
        Directory(target).createSync(recursive: true);
        continue;
      }
      if (entity is! File) {
        continue;
      }
      Directory(path.dirname(target)).createSync(recursive: true);
      if (path.basename(entity.path) == 'plugin.js') {
        final String source = entity.readAsStringSync();
        final String patched = source.replaceAll(
          'https://api.bilibili.com',
          apiBase,
        );
        expect(
          patched,
          isNot(equals(source)),
          reason: '示例插件里应能找到 B 站接口域名（否则测试无法指向本地服务）',
        );
        File(target).writeAsStringSync(patched);
      } else {
        entity.copySync(target);
      }
    }
  }

  void dispose() {
    runtime.dispose();
    try {
      _server?.close(force: true);
    } catch (_) {
      // 服务可能已经被关掉：不影响测试结论
    }
    try {
      if (work.existsSync()) {
        work.deleteSync(recursive: true);
      }
    } catch (_) {
      // Windows 上插件框架可能还持有文件句柄：留着临时目录也不影响测试结论
    }
  }

  Future<Map<String, Object?>> probe() async {
    final Object? raw = await runtime.plugins.call(
      pluginId,
      'plugin.$pluginId.probe',
      const <String, Object?>{},
    );
    expect(raw, isA<Map<String, Object?>>(), reason: 'probe 应返回对象');
    return raw! as Map<String, Object?>;
  }

  /// 派发一次音源解析，并泵事件直到裁决返回
  Future<Map<String, Object?>?> resolveBili({
    required String crudeSrc,
    required String src,
    required String quality,
  }) {
    return resolveSrc(
      type: 'bili',
      crudeSrc: crudeSrc,
      src: src,
      quality: quality,
    );
  }

  /// 派发一次音源解析（任意来源类型），并泵事件直到裁决返回
  Future<Map<String, Object?>?> resolveSrc({
    required String type,
    required String src,
    required String quality,
    String? crudeSrc,
    String? pluginType,
  }) {
    final Future<Map<String, Object?>?> future = runtime.hooks.decideAsync(
      MusicxxPluginHookId.playerSourceResolve,
      <String, Object?>{
        'srcKey': 'test-src-key',
        'type': type,
        'src': src,
        'crudeType': type,
        'crudeSrc': crudeSrc ?? src,
        'fileName': null,
        'durationMs': 213000,
        'quality': quality,
        'isMediaSrc': true,
        'pluginType': pluginType,
        'caller': null,
      },
    );
    return _pumpFuture(runtime, future);
  }

  Future<void> pumpUntil(FutureOr<bool> Function() condition) =>
      _pumpUntil(runtime, condition);

  void _registerHostActions() {
    runtime.actions.register(MusicxxPluginActionNames.storageGet, (invocation) {
      final String key = '${invocation.args['key']}';
      if (invocation.args['namespace'] == 'config') {
        // 约定：返回**值本身**；键不存在返回 null（插件用自己给的默认值）
        return configFile[key];
      }
      return null;
    });
    runtime.actions.register(MusicxxPluginActionNames.storageSet, (invocation) {
      final String key = '${invocation.args['key']}';
      if (invocation.args['namespace'] == 'config') {
        configFile[key] = invocation.args['value'] ?? '';
        return <String, Object?>{'ok': true};
      }
      return <String, Object?>{'ok': true};
    });
    runtime.actions.register(MusicxxPluginActionNames.storageDelete, (
      invocation,
    ) {
      configFile.remove('${invocation.args['key']}');
      return <String, Object?>{'ok': true};
    });
    runtime.actions.register(MusicxxPluginActionNames.storageList, (invocation) {
      return <String, Object?>{'keys': configFile.keys.toList()..sort()};
    });

    // B 站接口的固定应答（由本地假服务按地址回答，见 _startFakeBiliApi）
    fakeBiliBody = (String url) {
      final String body;
      if (url.contains('/x/web-interface/view')) {
        final bool multi = url.contains(multiPageBvid);
        final List<Map<String, Object?>> pages = <Map<String, Object?>>[
          <String, Object?>{
            'cid': 137649199,
            'page': 1,
            'part': '第 1 P',
            'duration': 213,
          },
          if (multi)
            <String, Object?>{
              'cid': 137649200,
              'page': 2,
              'part': '第 2 P',
              'duration': 95,
            },
        ];
        body = convert.jsonEncode(<String, Object?>{
          'code': 0,
          'message': '0',
          'data': <String, Object?>{
            'bvid': multi ? multiPageBvid : singlePageBvid,
            'cid': 137649199,
            'pages': pages,
          },
        });
      } else if (url.contains('/x/player/playurl')) {
        body = convert.jsonEncode(<String, Object?>{
          'code': 0,
          'message': '0',
          'data': <String, Object?>{
            'timelength': 213000,
            'dash': <String, Object?>{
              'audio': <Map<String, Object?>>[
                <String, Object?>{
                  'id': 30216,
                  'bandwidth': 43962,
                  'baseUrl': '$fakeAudioUrl&id=30216',
                },
                <String, Object?>{
                  'id': 30232,
                  'bandwidth': 102931,
                  'baseUrl': '$fakeAudioUrl&id=30232',
                },
                <String, Object?>{
                  'id': 30280,
                  'bandwidth': 203786,
                  'baseUrl': '$fakeAudioUrl&id=30280',
                },
              ],
            },
          },
        });
      } else {
        body = convert.jsonEncode(<String, Object?>{'code': -404, 'message': 'unexpected url'});
      }
      return body;
    };

    // 声明音源类型（功能 2 用；这里记录参数，断言清单是否正确）
    runtime.actions.register(
      MusicxxPluginActionNames.sourceDefine,
      (invocation) {
        final Object? id = invocation.args['id'];
        if (id is String && !definedSources.contains(id)) {
          definedSources.add(id);
        }
        return <String, Object?>{'ok': true, 'id': id};
      },
    );
  }
}

/// 取裁决里的 `src` 对象（插件框架会把 `patch` 合并到顶层，见 docs/plugin-hooks.md）
Map<String, Object?> _srcOf(Map<String, Object?> verdict) {
  final Object? src = verdict['src'];
  expect(src, isA<Map<String, Object?>>(), reason: '裁决里应有 src: $verdict');
  return src! as Map<String, Object?>;
}

/// 一次 HTTP 请求的结果（只取测试关心的几个头）
class _HttpResult {
  _HttpResult(this.status, this.headers, this.body);

  final int status;
  final HttpHeaders headers;
  final List<int> body;

  int get contentLength =>
      int.tryParse(headers.value(HttpHeaders.contentLengthHeader) ?? '') ?? -1;

  String? get contentType => headers.contentType?.toString();

  String? get acceptRanges => headers.value('accept-ranges');

  String? get contentRange => headers.value('content-range');
}

/// 发一次 HTTP 请求（`method: 'HEAD'` 时不读正文）
Future<_HttpResult> _httpGet(
  HttpClient client,
  String url, {
  Map<String, String>? headers,
  String method = 'GET',
}) async {
  final HttpClientRequest request =
      await client.openUrl(method, Uri.parse(url));
  headers?.forEach(request.headers.set);
  final HttpClientResponse response = await request.close();
  final List<int> body = method == 'HEAD'
      ? <int>[]
      : await response.fold<List<int>>(
          <int>[],
          (List<int> acc, List<int> chunk) => acc..addAll(chunk),
        );
  return _HttpResult(response.statusCode, response.headers, body);
}

/// 取设置页里某个开关的 `value`（按控件 id 找）
Object? _switchValue(Map<String, Object?> view, String id) {
  Object? found;
  bool visit(Object? node) {
    if (node is Map) {
      if (node['kind'] == 'Control' && node['id'] == id) {
        found = node['value'];
        return true;
      }
      for (final Object? value in node.values) {
        if (visit(value)) {
          return true;
        }
      }
    } else if (node is List) {
      for (final Object? item in node) {
        if (visit(item)) {
          return true;
        }
      }
    }
    return false;
  }
  visit(view);
  return found;
}

/// 递归收集视图里的文本（断言文案用）
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
    for (final Object? item in node) {
      _collectText(item, out);
    }
  }
}

/// 等一个 Future 完成，同时持续泵事件（异步裁决结果经事件回来）
Future<T> _pumpFuture<T>(MusicxxPluginRuntime runtime, Future<T> future) async {
  T? value;
  Object? error;
  bool done = false;
  future.then((T result) {
    value = result;
    done = true;
  }, onError: (Object e) {
    error = e;
    done = true;
  });
  final DateTime deadline = DateTime.now().add(const Duration(seconds: 10));
  while (!done) {
    if (DateTime.now().isAfter(deadline)) {
      throw TimeoutException('等待裁决超时（事件泵 10 s 内没有结果）');
    }
    runtime.pumpEvents();
    await Future<void>.delayed(const Duration(milliseconds: 5));
  }
  if (error != null) {
    throw error!;
  }
  return value as T;
}

/// 等待条件成立（最多 5 秒；每 20 ms 泵一次事件）
Future<void> _pumpUntil(
  MusicxxPluginRuntime runtime,
  FutureOr<bool> Function() condition, {
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

/// 原生库 + 示例插件目录探测（找不到就跳过需要它们的用例）
class _Environment {
  const _Environment(this.libraryPath, this.pluginRoot);

  final String libraryPath;
  final String pluginRoot;

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
