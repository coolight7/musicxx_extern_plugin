/// `example_ftp_source` 的集成测试（真实原生库 + 真实 JS 运行时 + 本地假 FTP 服务器）
///
/// 覆盖：
/// - 账号：表单字段 → 保存到插件 KV → 摘要视图不回显密码；
/// - 浏览：列目录（MLSD）、目录优先排序、"只看音频"过滤、按名称筛选、进入子目录；
/// - 加歌：`browseAdd` 发给 `musicxx.playlist.addSong` 的 `songs` 载荷（type / src / fileName / size）；
/// - 播放：`musicxx.player.source.resolve` 返回本机 http 地址 + `kind: url`，同一文件复用同一个地址；
/// - 转流：GET / HEAD / 单区间 Range（206 / 416）/ 服务器不支持 REST、SIZE 时的退化行为；
/// - 换账号后旧地址失效、插件卸载后端口释放。
///
/// 假 FTP 服务器只实现这个示例用到的命令（`USER` / `PASS` / `OPTS` / `FEAT` / `TYPE` / `EPSV` /
/// `PASV` / `MLSD` / `LIST` / `SIZE` / `REST` / `RETR` / `QUIT`）。
/// 前置：先跑 `tools/build_examples.ps1`（Windows）或 `tools/build_examples.sh`（Linux/macOS），
/// 产出原生库与 `.native/output/<平台>/plugins/` 里的示例插件；找不到时跳过。
library;

import 'dart:async';
import 'dart:convert' as convert;
import 'dart:io';
import 'dart:math' as math;
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:musicxx_extern_plugin/musicxx_extern_plugin.dart';
import 'package:path/path.dart' as path;

const String pluginId = 'example_ftp_source';
const String ftpType = 'plugin.$pluginId.audio';

/// 假服务器上的文件（用可校验的内容：每字节 = 位置 % 251）
final Uint8List fileA = _bytesOf(64 * 1024 + 123);
final Uint8List fileB = _bytesOf(8 * 1024);
final Uint8List fileC = _bytesOf(3 * 1024);

Uint8List _bytesOf(int length) =>
    Uint8List.fromList(List<int>.generate(length, (int i) => i % 251));

void main() {
  final _Environment? env = _Environment.detect();

  /// 每个用例一套环境（插件状态不跨用例：账号、浏览状态、token 都在内存里）
  Future<_Harness?> start() async {
    if (env == null || !env.hasPlugin(pluginId)) {
      return null;
    }
    final _Harness harness = _Harness(env);
    addTearDown(harness.dispose);
    await harness.start();
    return harness;
  }

  bool unsupported() {
    if (env == null || !env.hasPlugin(pluginId)) {
      markTestSkipped(
        '未找到原生插件框架库或示例插件，跳过（先跑 tools/build_examples.ps1（Windows）'
        '或 tools/build_examples.sh（Linux/macOS）产出原生库与示例插件）',
      );
      return true;
    }
    return false;
  }

  test('账号：填表 → 保存 → 摘要不回显密码', () async {
    if (unsupported()) {
      return;
    }
    final _Harness harness = (await start())!;

    // 第一次进账号页：没配置 → 直接是表单，字段是缺省值
    final Map<String, Object?> form = await harness.accounts(const <String, Object?>{});
    expect(_controlValue(form, 'host'), '');
    expect(_controlValue(form, 'port'), 21);
    expect(_controlValue(form, 'encoding'), isNull,
        reason: '编码是一排按钮（Button 块），不是 Control');
    expect(_controlValue(form, 'timeoutSec'), 10);
    expect(_controlValue(form, 'audioOnly'), isTrue);

    // 逐个字段填（控件的 action 只报 id 与 value，插件记在内存草稿里）
    await harness.setField('host', '127.0.0.1');
    await harness.setField('port', harness.ftp.port);
    await harness.setField('user', 'test');
    await harness.setField('password', '123456');
    await harness.setField('rootPath', '/Music');
    await harness.setField('encoding', 'utf-8');

    final Map<String, Object?> saved = await harness.accounts(
      const <String, Object?>{'op': 'save'},
    );
    final List<String> texts = _textsOf(saved);
    expect(
      texts.any((String text) => text.contains('已保存到本机')),
      isTrue,
      reason: '保存后应有一句提示: $texts',
    );

    final Map<String, Object?> probe = await harness.probe();
    expect(probe['configured'], isTrue);
    expect('${probe['fingerprint']}', matches(RegExp(r'^[0-9a-f]{8}$')));
    expect((probe['account']! as Map)['rootPath'], '/Music');

    // 账号存进了插件 KV（密码只在存储与内存里）
    final Map<Object?, Object?> stored =
        (harness.kv['account']! as Map).cast<Object?, Object?>();
    expect(stored['password'], '123456');
    expect(stored['port'], harness.ftp.port);

    // 摘要页：密码不回显（只有"已保存（不回显）"这一句）
    final List<String> summaryTexts = _textsOf(saved);
    expect(summaryTexts.any((String text) => text == '已保存（不回显）'), isTrue,
        reason: '摘要里密码一栏只写"已保存（不回显）": $summaryTexts');
    expect(summaryTexts.any((String text) => text.contains('123456')), isFalse,
        reason: '密码不能出现在页面文本里');

    // 编辑：密码输入框从空开始（留空 = 不修改）
    final Map<String, Object?> edit = await harness.accounts(
      const <String, Object?>{'op': 'edit'},
    );
    expect(_controlValue(edit, 'password'), '');
    expect(_controlValue(edit, 'host'), '127.0.0.1');

    // 测试连接：真的去登录 + 读根路径
    final Map<String, Object?> tested = await harness.accounts(
      const <String, Object?>{'op': 'test'},
    );
    final List<String> testedTexts = _textsOf(tested);
    expect(
      testedTexts.any((String text) => text.contains('连接成功')),
      isTrue,
      reason: '测试连接应成功: $testedTexts',
    );
    expect(harness.ftp.loginCount, greaterThan(0));
  }, timeout: const Timeout(Duration(seconds: 90)));

  test('账号：没配置时点「取消」也回到表单（空状态不报错）', () async {
    if (unsupported()) {
      return;
    }
    final _Harness harness = (await start())!;

    final Map<String, Object?> form = await harness.accounts(
      const <String, Object?>{'op': 'view'},
    );
    expect(_controlValue(form, 'host'), '', reason: '没配置时直接给表单');

    // 取消：草稿清掉之后仍然要有可显示的内容（摘要页没有账号会没有可列的项）
    final Map<String, Object?> cancelled = await harness.accounts(
      const <String, Object?>{'op': 'cancel'},
    );
    expect(_controlValue(cancelled, 'host'), isNotNull,
        reason: '取消后应回到表单，而不是一张空摘要');
    expect(_controlValue(cancelled, 'port'), 21);
  }, timeout: const Timeout(Duration(seconds: 60)));

  test('浏览：列目录、目录优先、只看音频、筛选、进入子目录', () async {
    if (unsupported()) {
      return;
    }
    final _Harness harness = (await start())!;
    await harness.configureAccount();

    // 打开浮层时主程序会把当前歌单的 pid / name 合进参数
    final Map<String, Object?> view = await harness.browse(<String, Object?>{
      'pid': 7,
      'name': '测试歌单',
      'songNum': 3,
    });

    expect(_viewOf(view)['title'], contains('测试歌单'));
    final List<String> names = _entryNamesOf(view);
    expect(names, contains('sub'), reason: '文件夹要列出来: $names');
    expect(names, contains('a.mp3'));
    expect(names, contains('b.flac'));
    expect(names, isNot(contains('readme.txt')),
        reason: '账号缺省的"只看音频"应当隐藏 .txt');
    expect(names.first, 'sub', reason: '文件夹排在前面');

    final Map<String, Object?> probe = await harness.probe();
    expect((probe['browse']! as Map)['pid'], 7);
    expect((probe['browse']! as Map)['path'], '/Music');

    // 进入子目录
    final Map<String, Object?> sub = await harness.browse(
      const <String, Object?>{'op': 'enter', 'path': '/Music/sub'},
    );
    expect(_entryNamesOf(sub), containsAll(<String>['c.ogg']));
    expect(((await harness.probe())['browse']! as Map)['path'], '/Music/sub');

    // 上一级
    final Map<String, Object?> up = await harness.browse(
      const <String, Object?>{'op': 'up'},
    );
    expect(_entryNamesOf(up), contains('a.mp3'));

    // 关掉"只看音频"：文本文件也出现
    final Map<String, Object?> all = await harness.browse(
      const <String, Object?>{'op': 'audioOnly', 'value': false},
    );
    expect(_entryNamesOf(all), contains('readme.txt'));

    // 按名称筛选（控件失焦后才派发，参数里带 id 与 value）
    final Map<String, Object?> filtered = await harness.browse(
      const <String, Object?>{'op': 'keyword', 'id': 'keyword', 'value': 'FLAC'},
    );
    final List<String> filteredNames = _entryNamesOf(filtered);
    expect(filteredNames, <String>['b.flac'], reason: '筛选不区分大小写: $filteredNames');

    // 刷新（清掉筛选）
    final Map<String, Object?> refreshed = await harness.browse(
      const <String, Object?>{'op': 'keyword', 'value': ''},
    );
    expect(_entryNamesOf(refreshed), containsAll(<String>['a.mp3', 'b.flac']));
  }, timeout: const Timeout(Duration(seconds: 90)));

  test('加歌：songs 载荷的类型、路径、文件名与大小', () async {
    if (unsupported()) {
      return;
    }
    final _Harness harness = (await start())!;
    await harness.configureAccount();
    await harness.browse(<String, Object?>{'pid': 12, 'name': '我的歌单'});

    // 勾选两个文件（控件与整行点击都走 select / toggle）
    await harness.browse(
      const <String, Object?>{'op': 'select', 'path': '/Music/a.mp3', 'value': true},
    );
    // 子目录里那个文件要先列一次，插件才知道它有多大（大小只用于显示与缓存提示，可以不填）
    await harness.browse(const <String, Object?>{'op': 'enter', 'path': '/Music/sub'});
    final Map<String, Object?> view = await harness.browse(
      const <String, Object?>{'op': 'toggle', 'path': '/Music/sub/c.ogg'},
    );
    expect(_textsOf(view).any((String text) => text.contains('已选 2 首')), isTrue,
        reason: '页面要显示已选数量');

    final Map<String, Object?> added = await harness.browseAdd();
    expect(_textsOf(added).any((String text) => text.contains('正在处理')), isTrue);

    // 插件不 await 主程序动作：结果在事件泵里回来
    await harness.pumpUntil(() => harness.addedSongs.isNotEmpty);
    final List<Map<String, Object?>> songs = harness.addedSongs;
    expect(songs, hasLength(2));
    final String fingerprint = '${(await harness.probe())['fingerprint']}';
    for (final Map<String, Object?> song in songs) {
      expect(song['type'], ftpType);
      expect('${song['src']}', startsWith('$fingerprint/'));
      expect(song['name'], isNull, reason: '歌名与歌手留给主程序按文件名识别');
      expect('${song['fileName']}', isNotEmpty);
    }
    expect(songs[0]['src'], '$fingerprint/Music/a.mp3');
    expect(songs[0]['fileName'], 'a.mp3');
    expect(songs[0]['size'], fileA.length);
    expect(songs[1]['src'], '$fingerprint/Music/sub/c.ogg');
    expect(songs[1]['size'], fileC.length);
    expect(harness.addSongPid, 12);

    // 成功后提示 + 关掉浮层
    await harness.pumpUntil(() => harness.overlayCloseCalls > 0);
    expect(harness.lastToast, contains('已添加 2 首'));
  }, timeout: const Timeout(Duration(seconds: 90)));

  test('播放：解析出本机地址，GET / HEAD / Range 都能读到原字节', () async {
    if (unsupported()) {
      return;
    }
    final _Harness harness = (await start())!;
    await harness.configureAccount();
    final String fingerprint = '${(await harness.probe())['fingerprint']}';

    // 目录里先读一次，让插件知道文件大小（真实流程也是这样：先浏览再加歌）
    await harness.browse(<String, Object?>{'pid': 1, 'name': 'x'});

    final Map<String, Object?>? verdict = await harness.resolve(
      '$fingerprint/Music/a.mp3',
    );
    expect(verdict, isNotNull, reason: '本插件自己的音源类型应当被接管');
    final String url = '${verdict!['path']}';
    expect(verdict['kind'], 'url');
    expect(url, startsWith('http://127.0.0.1:'));
    expect(url, matches(RegExp(r'/s/[0-9a-f]{32}$')));

    // 同一个 (账号, 路径) 再解析：地址不变（播放器 / 缓存流会拿它做缓存键）
    final Map<String, Object?>? again = await harness.resolve(
      '$fingerprint/Music/a.mp3',
    );
    expect('${again!['path']}', url);

    final HttpClient client = HttpClient();
    addTearDown(client.close);

    final _HttpResult full = await harness.httpGet(client, url);
    expect(full.status, 200);
    expect(full.contentLength, fileA.length);
    expect(full.acceptRanges, 'bytes');
    expect(full.contentType, 'audio/mpeg');
    expect(full.body.length, fileA.length);
    expect(full.body.sublist(0, 64), fileA.sublist(0, 64));
    expect(full.body.sublist(fileA.length - 64), fileA.sublist(fileA.length - 64));

    final _HttpResult head = await harness.httpGet(client, url, method: 'HEAD');
    expect(head.status, 200);
    expect(head.contentLength, fileA.length);
    expect(head.body, isEmpty);

    final _HttpResult partial = await harness.httpGet(
      client,
      url,
      headers: <String, String>{'Range': 'bytes=1024-2047'},
    );
    expect(partial.status, 206);
    // 示例只支持「单区间」，结束位置一律做到文件末尾（播放器按 Content-Range 读自己需要的那段）；
    // 这样不需要在 JS 里截断数据片，也不会让 Content-Length 与正文对不上。
    expect(partial.contentRange, 'bytes 1024-${fileA.length - 1}/${fileA.length}');
    expect(partial.contentLength, fileA.length - 1024);
    expect(partial.body, fileA.sublist(1024));

    final _HttpResult suffix = await harness.httpGet(
      client,
      url,
      headers: <String, String>{'Range': 'bytes=-100'},
    );
    expect(suffix.status, 206);
    expect(suffix.body, fileA.sublist(fileA.length - 100));

    final _HttpResult open = await harness.httpGet(
      client,
      url,
      headers: <String, String>{'Range': 'bytes=${fileA.length - 10}-'},
    );
    expect(open.status, 206);
    expect(open.body, fileA.sublist(fileA.length - 10));

    final _HttpResult beyond = await harness.httpGet(
      client,
      url,
      headers: <String, String>{'Range': 'bytes=${fileA.length + 100}-'},
    );
    expect(beyond.status, 416);

    // 不认识的 token → 404（服务端按 token 查表，不解析路径）
    final _HttpResult missing = await harness.httpGet(
      client,
      url.replaceFirst(RegExp(r'/s/[0-9a-f]{32}$'), '/s/deadbeef'),
    );
    expect(missing.status, 404);

    final Map<String, Object?> probe = await harness.probe();
    final Map<String, Object?> proxy = (probe['proxy']! as Map).cast<String, Object?>();
    expect(proxy['running'], isTrue);
    expect(proxy['port'], greaterThan(0));
    expect(proxy['requests'], greaterThanOrEqualTo(7));
    expect(proxy['tokens'], greaterThanOrEqualTo(1));
    expect((probe['counts']! as Map)['resolveOk'], 2);
  }, timeout: const Timeout(Duration(seconds: 120)));

  test('转流：服务器不支持 REST 时退回整段 200', () async {
    if (unsupported()) {
      return;
    }
    final _Harness harness = (await start())!;
    await harness.configureAccount();
    final String fingerprint = '${(await harness.probe())['fingerprint']}';
    await harness.browse(<String, Object?>{'pid': 1, 'name': 'x'});

    harness.ftp.supportRest = false;
    final Map<String, Object?>? verdict = await harness.resolve(
      '$fingerprint/Music/b.flac',
    );
    final String url = '${verdict!['path']}';

    final HttpClient client = HttpClient();
    addTearDown(client.close);
    final _HttpResult ranged = await harness.httpGet(
      client,
      url,
      headers: <String, String>{'Range': 'bytes=100-199'},
    );
    expect(ranged.status, 200,
        reason: '服务器不支持 REST 时从头发，响应码变回 200');
    expect(ranged.contentLength, fileB.length);
    expect(ranged.body, fileB);
  }, timeout: const Timeout(Duration(seconds: 120)));

  test('转流：服务器不支持 SIZE 时不写 Content-Length，靠关连接结束', () async {
    if (unsupported()) {
      return;
    }
    final _Harness harness = (await start())!;
    await harness.configureAccount();
    final String fingerprint = '${(await harness.probe())['fingerprint']}';
    await harness.browse(<String, Object?>{'pid': 1, 'name': 'x'});

    harness.ftp.supportSize = false;
    final Map<String, Object?>? verdict = await harness.resolve(
      '$fingerprint/Music/b.flac',
    );
    final String url = '${verdict!['path']}';

    final HttpClient client = HttpClient();
    addTearDown(client.close);
    // 有 Range 也要从头读（不知道大小就算不出区间）
    final _HttpResult result = await harness.httpGet(
      client,
      url,
      headers: <String, String>{'Range': 'bytes=100-199'},
    );
    expect(result.status, 200);
    expect(result.contentLength, -1, reason: '大小未知时不写 Content-Length');
    expect(result.body, fileB);
  }, timeout: const Timeout(Duration(seconds: 120)));

  test('换账号：旧地址失效（token 全部作废）', () async {
    if (unsupported()) {
      return;
    }
    final _Harness harness = (await start())!;
    await harness.configureAccount();
    final String fingerprint = '${(await harness.probe())['fingerprint']}';

    final Map<String, Object?>? verdict = await harness.resolve(
      '$fingerprint/Music/a.mp3',
    );
    final String url = '${verdict!['path']}';
    final HttpClient client = HttpClient();
    addTearDown(client.close);
    expect((await harness.httpGet(client, url)).status, 200);

    // 换用户名 = 换账号指纹（旧歌的 src 里还是老指纹）
    await harness.setField('user', 'other');
    await harness.accounts(const <String, Object?>{'op': 'save'});
    final Map<String, Object?> probe = await harness.probe();
    expect('${probe['fingerprint']}', isNot(fingerprint));

    final _HttpResult old = await harness.httpGet(client, url);
    expect(old.status, 404, reason: '换账号后旧 token 已经作废');

    // 旧指纹的歌也不再被接管（主程序会提示"没有可用的解析实现"）
    expect(await harness.resolve('$fingerprint/Music/a.mp3'), isNull);
  }, timeout: const Timeout(Duration(seconds: 120)));

  test('自检与卸载：probe 反映状态，卸载后端口释放', () async {
    if (unsupported()) {
      return;
    }
    final _Harness harness = (await start())!;
    await harness.configureAccount();
    final String fingerprint = '${(await harness.probe())['fingerprint']}';

    // 加载时就声明了自己的音源类型
    expect(harness.definedSources, contains(ftpType));

    final Map<String, Object?> before = await harness.probe();
    expect(before['netOk'], isTrue);
    expect(before['hasCodec'], isTrue, reason: '插件框架应带 textEncode/textDecode');
    expect((before['proxy']! as Map)['running'], isFalse,
        reason: '还没人播放时不该起监听');

    final Map<String, Object?>? verdict = await harness.resolve(
      '$fingerprint/Music/a.mp3',
    );
    final String url = '${verdict!['path']}';
    final Map<String, Object?> after = await harness.probe();
    expect((after['proxy']! as Map)['running'], isTrue);
    expect((after['counts']! as Map)['resolveOk'], 1);

    // 卸载：框架关掉插件的全部 socket，回环端口随即释放
    harness.runtime.plugins.unload(pluginId);
    await harness.pumpFor(const Duration(milliseconds: 200));
    final HttpClient client = HttpClient();
    addTearDown(client.close);
    await expectLater(
      harness.httpGet(client, url),
      throwsA(anyOf(isA<SocketException>(), isA<HttpException>())),
      reason: '插件卸载后本机转流服务应当已关闭',
    );
  }, timeout: const Timeout(Duration(seconds: 120)));
}

/* ==================== 测试环境 ==================== */

class _Harness {
  _Harness(this.env);

  final _Environment env;
  final MusicxxPluginRuntime runtime = MusicxxPluginRuntime.create();
  late final Directory work;
  late final Directory pluginRoot;
  late final _FakeFtpServer ftp;

  /// 插件 KV（`musicxx.storage` 的假实现）
  final Map<String, Object?> kv = <String, Object?>{};

  final List<String> definedSources = <String>[];
  final List<Map<String, Object?>> addedSongs = <Map<String, Object?>>[];
  int addSongPid = 0;
  int overlayCloseCalls = 0;
  int toastCount = 0;
  String lastToast = '';
  Map<String, Object?>? lastPlaySrc;

  Future<void> start() async {
    work = Directory.systemTemp.createTempSync('musicxx_ftp_source_test');
    pluginRoot = Directory(path.join(work.path, 'plugins'))
      ..createSync(recursive: true);
    ftp = await _FakeFtpServer.start(
      files: <String, Uint8List>{
        '/Music/a.mp3': fileA,
        '/Music/b.flac': fileB,
        '/Music/sub/c.ogg': fileC,
      },
      textFiles: <String, Uint8List>{
        '/Music/readme.txt': Uint8List.fromList(convert.utf8.encode('hello')),
      },
    );
    addTearDown(ftp.close);

    _copyPlugin();
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
    // 账号读取是顶层发起的异步动作：泵一会儿让它落地
    await pumpFor(const Duration(milliseconds: 100));
  }

  void _copyPlugin() {
    final Directory from = Directory(path.join(env.pluginRoot, pluginId));
    final Directory to = Directory(path.join(pluginRoot.path, pluginId));
    for (final FileSystemEntity entity in from.listSync(recursive: true)) {
      final String relative = path.relative(entity.path, from: from.path);
      final String target = path.join(to.path, relative);
      if (entity is Directory) {
        Directory(target).createSync(recursive: true);
      } else if (entity is File) {
        Directory(path.dirname(target)).createSync(recursive: true);
        entity.copySync(target);
      }
    }
  }

  void _registerHostActions() {
    runtime.actions.register(MusicxxPluginActionNames.storageGet, (invocation) {
      final String key = '${invocation.args['key']}';
      if (invocation.args['namespace'] == 'config') {
        return null;
      }
      return kv[key];
    });
    runtime.actions.register(MusicxxPluginActionNames.storageSet, (invocation) {
      kv['${invocation.args['key']}'] = invocation.args['value'];
      return <String, Object?>{'ok': true};
    });
    runtime.actions.register(MusicxxPluginActionNames.storageDelete, (
      invocation,
    ) {
      kv.remove('${invocation.args['key']}');
      return <String, Object?>{'ok': true};
    });
    runtime.actions.register(MusicxxPluginActionNames.storageList, (_) {
      return <String, Object?>{'keys': kv.keys.toList()..sort()};
    });
    runtime.actions.register(MusicxxPluginActionNames.uiToast, (invocation) {
      ++toastCount;
      lastToast = '${invocation.args['text']}';
      return <String, Object?>{'ok': true};
    });
    runtime.actions.register(MusicxxPluginActionNames.uiNotify, (_) {
      return <String, Object?>{'ok': true};
    });
    runtime.actions.register(MusicxxPluginActionNames.uiOverlayClose, (_) {
      ++overlayCloseCalls;
      return <String, Object?>{'ok': true, 'closed': true};
    });
    runtime.actions.register(MusicxxPluginActionNames.playlistAddSong, (
      invocation,
    ) {
      addSongPid = int.tryParse('${invocation.args['pid']}') ?? 0;
      final Object? songs = invocation.args['songs'];
      if (songs is List) {
        for (final Object? item in songs) {
          if (item is Map) {
            addedSongs.add(item.cast<String, Object?>());
          }
        }
      }
      return <String, Object?>{
        'ok': true,
        'pid': addSongPid,
        'name': '测试歌单',
        'total': addedSongs.length,
        'added': addedSongs.length,
        'skipped': 0,
      };
    });
    runtime.actions.register(MusicxxPluginActionNames.playerPlaySrc, (
      invocation,
    ) {
      lastPlaySrc = invocation.args.cast<String, Object?>();
      return <String, Object?>{'ok': true};
    });
    runtime.actions.register(MusicxxPluginActionNames.sourceDefine, (
      invocation,
    ) {
      final Object? id = invocation.args['id'];
      if (id is String && !definedSources.contains(id)) {
        definedSources.add(id);
      }
      return <String, Object?>{'ok': true, 'id': id};
    });
    runtime.actions.register(MusicxxPluginActionNames.sourceUndefine, (_) {
      return <String, Object?>{'ok': true};
    });
  }

  void dispose() {
    runtime.dispose();
    try {
      if (work.existsSync()) {
        work.deleteSync(recursive: true);
      }
    } catch (_) {
      // Windows 上插件框架可能还持有文件句柄：留着临时目录也不影响测试结论
    }
  }

  /// 调一个能力并泵事件直到结果回来
  Future<Map<String, Object?>> call(
    String capability,
    Map<String, Object?> args,
  ) async {
    final Object? raw = await _pumpFuture(
      runtime,
      runtime.plugins.call(pluginId, 'plugin.$pluginId.$capability', args),
    );
    expect(raw, isA<Map<String, Object?>>(),
        reason: '$capability 应返回对象，实际：$raw');
    return raw! as Map<String, Object?>;
  }

  Future<Map<String, Object?>> accounts(Map<String, Object?> args) =>
      call('accounts', args);

  Future<Map<String, Object?>> browse(Map<String, Object?> args) =>
      call('browse', <String, Object?>{'view': 'browse', ...args});

  Future<Map<String, Object?>> browseAdd() => call('browseAdd', const <String, Object?>{});

  Future<Map<String, Object?>> probe() => call('probe', const <String, Object?>{});

  /// 设一个表单字段（等价于控件派发：只有 id 与 value）
  Future<void> setField(String id, Object? value) async {
    await accounts(<String, Object?>{'op': 'field', 'id': id, 'value': value});
  }

  /// 填好账号并保存（假服务器的地址与端口）
  Future<void> configureAccount() async {
    await setField('host', '127.0.0.1');
    await setField('port', ftp.port);
    await setField('user', 'test');
    await setField('password', '123456');
    await setField('rootPath', '/Music');
    await accounts(const <String, Object?>{'op': 'save'});
    final Map<String, Object?> probe = await this.probe();
    expect(probe['configured'], isTrue, reason: '账号应当保存成功：$probe');
  }

  /// 派发一次音源解析（插件自己的音源类型），返回合并后的裁决
  Future<Map<String, Object?>?> resolve(String src) async {
    final Map<String, Object?>? verdict = await _pumpFuture(
      runtime,
      runtime.hooks.decideAsync(
        MusicxxPluginHookId.playerSourceResolve,
        <String, Object?>{
          'srcKey': 'test-$src',
          'type': '',
          'src': src,
          'crudeType': '',
          'crudeSrc': src,
          'fileName': 'a.mp3',
          'durationMs': 213000,
          'quality': 'upper',
          'isMediaSrc': true,
          'pluginType': ftpType,
          'caller': null,
        },
      ),
    );
    if (verdict == null) {
      return null;
    }
    final Object? merged = verdict['src'];
    if (merged is Map) {
      return merged.cast<String, Object?>();
    }
    return verdict;
  }

  /// 发一次 HTTP 请求（过程中持续泵事件：转流是 JS 在处理 socket 事件）
  Future<_HttpResult> httpGet(
    HttpClient client,
    String url, {
    Map<String, String>? headers,
    String method = 'GET',
  }) {
    final Future<_HttpResult> future = () async {
      final HttpClientRequest request = await client.openUrl(
        method,
        Uri.parse(url),
      );
      headers?.forEach(request.headers.set);
      final HttpClientResponse response = await request.close();
      final List<int> body = method == 'HEAD'
          ? <int>[]
          : await response.fold<List<int>>(
              <int>[],
              (List<int> acc, List<int> chunk) => acc..addAll(chunk),
            );
      return _HttpResult(response.statusCode, response.headers, body);
    }();
    return _pumpFuture(runtime, future);
  }

  Future<void> pumpFor(Duration duration) async {
    final DateTime deadline = DateTime.now().add(duration);
    while (DateTime.now().isBefore(deadline)) {
      runtime.pumpEvents();
      await Future<void>.delayed(const Duration(milliseconds: 5));
    }
  }

  Future<void> pumpUntil(
    bool Function() condition, {
    Duration timeout = const Duration(seconds: 10),
  }) async {
    final DateTime deadline = DateTime.now().add(timeout);
    while (!condition()) {
      if (DateTime.now().isAfter(deadline)) {
        return;
      }
      runtime.pumpEvents();
      await Future<void>.delayed(const Duration(milliseconds: 10));
    }
  }
}

/* ==================== 一次 HTTP 请求的结果 ==================== */

class _HttpResult {
  _HttpResult(this.status, this.headers, this.body);

  final int status;
  final HttpHeaders headers;
  final List<int> body;

  int get contentLength =>
      int.tryParse(headers.value(HttpHeaders.contentLengthHeader) ?? '') ?? -1;

  String? get contentType => headers.contentType?.mimeType;

  String? get acceptRanges => headers.value('accept-ranges');

  String? get contentRange => headers.value('content-range');
}

/* ==================== 视图取值小工具 ==================== */

/// 取能力返回里的视图（插件返回 `{view: {...}}`）
Map<String, Object?> _viewOf(Map<String, Object?> result) {
  final Object? raw = result['view'];
  if (raw is Map) {
    return raw.cast<String, Object?>();
  }
  return result;
}

/// 取某个控件（`kind: Control`）的 `value`
Object? _controlValue(Map<String, Object?> view, String id) {
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
List<String> _textsOf(Map<String, Object?> view) {
  final List<String> out = <String>[];
  void visit(Object? node) {
    if (node is String) {
      out.add(node);
      return;
    }
    if (node is Map) {
      for (final Object? value in node.values) {
        visit(value);
      }
      return;
    }
    if (node is List) {
      for (final Object? item in node) {
        visit(item);
      }
    }
  }

  visit(view);
  return out;
}

/// 浏览页里列出的条目名（只取"行首那个 Text"：文件行与目录行的第一段文字就是名字）
List<String> _entryNamesOf(Map<String, Object?> view) {
  final Object? raw = view['view'];
  final Map<String, Object?> body =
      (raw is Map) ? raw.cast<String, Object?>() : view;
  final List<String> names = <String>[];
  final Object? blocks = body['blocks'];
  if (blocks is! List) {
    return names;
  }
  for (final Object? block in blocks) {
    if (block is! Map) {
      continue;
    }
    if (block['kind'] != 'Block' || block['action'] is! Map) {
      continue;
    }
    final Map<Object?, Object?> action = (block['action']! as Map).cast<Object?, Object?>();
    if (action['name'] != 'browse') {
      continue;
    }
    final Object? args = action['args'];
    final String? key = (args is Map) ? '${args['op']}' : null;
    if (key != 'enter' && key != 'toggle') {
      continue;
    }
    final Object? children = block['children'];
    if (children is! List) {
      continue;
    }
    // 行结构：Row → [SizedBox(勾选框)?, Expanded(Column(Text(名字), …))]（目录行没有勾选框）
    for (final Object? child in children) {
      final String name = _firstTextOf(child) ?? '';
      if (name.isNotEmpty) {
        names.add(name);
      }
    }
  }
  return names;
}

/// 深度找第一段 `Text.text`
String? _firstTextOf(Object? node) {
  if (node is Map) {
    if (node['kind'] == 'Text' && node['text'] is String) {
      return node['text']! as String;
    }
    for (final Object? value in node.values) {
      final String? found = _firstTextOf(value);
      if (found != null) {
        return found;
      }
    }
    return null;
  }
  if (node is List) {
    for (final Object? item in node) {
      final String? found = _firstTextOf(item);
      if (found != null) {
        return found;
      }
    }
  }
  return null;
}

/* ==================== 假 FTP 服务器 ==================== */

/// 只实现这个示例用到的命令；每个请求都能切换"支持 / 不支持"，用来覆盖退化路径
class _FakeFtpServer {
  _FakeFtpServer._(this._server, this.files);

  final ServerSocket _server;
  final Map<String, Uint8List> files;
  final Set<String> dirs = <String>{'/'};
  final List<Socket> _clients = <Socket>[];

  String user = 'test';
  String password = '123456';

  /// 统计：登录次数、RETR 次数（断言插件真的连上来过）
  int loginCount = 0;
  int retrCount = 0;

  /// 服务器不支持这两条命令时的退化行为（插件应当还能播）
  bool supportRest = true;
  bool supportSize = true;
  bool supportMlsd = true;

  int get port => _server.port;

  static Future<_FakeFtpServer> start({
    required Map<String, Uint8List> files,
    Map<String, Uint8List> textFiles = const <String, Uint8List>{},
  }) async {
    final ServerSocket server = await ServerSocket.bind(
      InternetAddress.loopbackIPv4,
      0,
    );
    final Map<String, Uint8List> all = <String, Uint8List>{...files, ...textFiles};
    final _FakeFtpServer result = _FakeFtpServer._(server, all);
    for (final String file in all.keys) {
      String parent = path.posix.dirname(file);
      while (parent.isNotEmpty && parent != '/') {
        result.dirs.add(parent);
        parent = path.posix.dirname(parent);
      }
    }
    server.listen((Socket socket) {
      result._clients.add(socket);
      _FtpSession(socket, result);
    });
    return result;
  }

  /// 某个目录下的条目名（排序：目录在前，再按名字）
  List<String> childrenOf(String dir) {
    final String base = (dir == '/' ? '' : dir);
    final Set<String> names = <String>{};
    for (final String file in files.keys) {
      final String parent = path.posix.dirname(file);
      if (parent == (base.isEmpty ? '/' : base)) {
        names.add(path.posix.basename(file));
      }
    }
    for (final String folder in dirs) {
      if (folder == '/') {
        continue;
      }
      if (path.posix.dirname(folder) == (base.isEmpty ? '/' : base)) {
        names.add(path.posix.basename(folder));
      }
    }
    final List<String> result = names.toList();
    result.sort((String left, String right) {
      final bool leftDir = dirs.contains(path.posix.join(base.isEmpty ? '/' : base, left));
      final bool rightDir = dirs.contains(path.posix.join(base.isEmpty ? '/' : base, right));
      if (leftDir != rightDir) {
        return leftDir ? -1 : 1;
      }
      return left.compareTo(right);
    });
    return result;
  }

  bool isDir(String absolute) => dirs.contains(absolute);

  Uint8List? fileAt(String absolute) => files[absolute];

  Future<void> close() async {
    for (final Socket socket in _clients) {
      try {
        socket.destroy();
      } catch (_) {
        // 已经关了
      }
    }
    _clients.clear();
    try {
      await _server.close();
    } catch (_) {
      // 已经关了
    }
  }
}

class _FtpSession {
  _FtpSession(this._socket, this._server) {
    _socket.listen(
      _onData,
      onDone: _dispose,
      onError: (Object error) => _dispose(),
      cancelOnError: true,
    );
    // 控制连接建好后先打招呼（插件要收到 220 才开始登录）
    _write('220 Fake FTP server ready.\r\n');
  }

  final Socket _socket;
  final _FakeFtpServer _server;

  String _buffer = '';
  ServerSocket? _passive;
  Completer<Socket>? _dataReady;
  bool _closed = false;
  int _restOffset = 0;

  void _write(String text) {
    if (_closed) {
      return;
    }
    try {
      _socket.write(text);
    } catch (_) {
      _dispose();
    }
  }

  void _onData(Uint8List bytes) {
    _buffer += convert.utf8.decode(bytes, allowMalformed: true);
    for (;;) {
      final int at = _buffer.indexOf('\r\n');
      if (at < 0) {
        return;
      }
      final String line = _buffer.substring(0, at);
      _buffer = _buffer.substring(at + 2);
      _handle(line);
    }
  }

  void _handle(String line) {
    final int space = line.indexOf(' ');
    final String cmd = (space < 0 ? line : line.substring(0, space)).toUpperCase();
    final String arg = space < 0 ? '' : line.substring(space + 1);
    switch (cmd) {
      case 'USER':
        _write('331 Password required for $arg\r\n');
        break;
      case 'PASS':
        if (arg == _server.password) {
          ++_server.loginCount;
          _write('230 Login successful.\r\n');
        } else {
          _write('530 Login incorrect.\r\n');
        }
        break;
      case 'OPTS':
        _write('200 Always in UTF8 mode.\r\n');
        break;
      case 'FEAT':
        final List<String> lines = <String>['211-Features:'];
        if (_server.supportMlsd) {
          lines.add(' MLSD');
        }
        if (_server.supportSize) {
          lines.add(' SIZE');
        }
        if (_server.supportRest) {
          lines.add(' REST STREAM');
        }
        lines.add(' UTF8');
        lines.add('211 End');
        _write('${lines.join('\r\n')}\r\n');
        break;
      case 'TYPE':
        _write('200 Type set to I.\r\n');
        break;
      case 'EPSV':
        unawaited(_enterPassive(extended: true));
        break;
      case 'PASV':
        unawaited(_enterPassive(extended: false));
        break;
      case 'MLSD':
        unawaited(_sendListing(arg, mlsd: true));
        break;
      case 'LIST':
        unawaited(_sendListing(arg, mlsd: false));
        break;
      case 'SIZE':
        if (!_server.supportSize) {
          _write('550 SIZE not allowed in ASCII mode.\r\n');
          break;
        }
        final Uint8List? file = _server.fileAt(_normalize(arg));
        if (file == null) {
          _write('550 Could not get file size.\r\n');
        } else {
          _write('213 ${file.length}\r\n');
        }
        break;
      case 'REST':
        if (!_server.supportRest) {
          _write('502 Command not implemented.\r\n');
          break;
        }
        _restOffset = int.tryParse(arg) ?? 0;
        _write('350 Restarting at $arg. Send STORE or RETRIEVE.\r\n');
        break;
      case 'RETR':
        unawaited(_sendFile(arg));
        break;
      case 'QUIT':
        _write('221 Goodbye.\r\n');
        _dispose();
        break;
      default:
        _write('502 Command not implemented.\r\n');
    }
  }

  Future<void> _enterPassive({required bool extended}) async {
    await _closePassive();
    final ServerSocket passive = await ServerSocket.bind(
      InternetAddress.loopbackIPv4,
      0,
    );
    _passive = passive;
    _dataReady = Completer<Socket>();
    passive.listen((Socket client) {
      final Completer<Socket>? ready = _dataReady;
      if (ready != null && !ready.isCompleted) {
        ready.complete(client);
      }
    });
    final int port = passive.port;
    if (extended) {
      _write('229 Entering Extended Passive Mode (|||$port|)\r\n');
    } else {
      _write(
        '227 Entering Passive Mode (127,0,0,1,${port ~/ 256},${port % 256})\r\n',
      );
    }
  }

  Future<Socket?> _awaitData() async {
    final Completer<Socket>? ready = _dataReady;
    if (ready == null) {
      return null;
    }
    try {
      return await ready.future.timeout(const Duration(seconds: 5));
    } on TimeoutException {
      return null;
    }
  }

  Future<void> _sendListing(String rawPath, {required bool mlsd}) async {
    if (mlsd && !_server.supportMlsd) {
      _write('500 Unknown command.\r\n');
      return;
    }
    final String dir = _normalize(rawPath);
    final Socket? data = await _awaitData();
    if (data == null) {
      _write('425 Use PASV or EPSV first.\r\n');
      return;
    }
    if (!_server.isDir(dir)) {
      _write('550 No such directory.\r\n');
      await _closeData(data);
      return;
    }
    _write('150 Here comes the directory listing.\r\n');
    final List<String> lines = <String>[];
    for (final String name in _server.childrenOf(dir)) {
      final String child = dir == '/' ? '/$name' : '$dir/$name';
      if (mlsd) {
        // MLSD 的事实全部写在第一个空格之前（事实之间不加空格）
        lines.add(_server.isDir(child)
            ? 'type=dir;modify=20260101120000; $name'
            : 'type=file;size=${_server.fileAt(child)?.length ?? 0};'
                'modify=20260101120000; $name');
      } else {
        lines.add(_server.isDir(child)
            ? 'drwxr-xr-x 1 ftp ftp 0 Jan 01 12:00 $name'
            : '-rw-r--r-- 1 ftp ftp ${_server.fileAt(child)?.length ?? 0} '
                'Jan 01 12:00 $name');
      }
    }
    final String body = lines.isEmpty ? '' : '${lines.join('\r\n')}\r\n';
    await _writeData(data, convert.utf8.encode(body));
    await _closeData(data);
    await _closePassive();
    _write('226 Directory send OK.\r\n');
  }

  Future<void> _sendFile(String rawPath) async {
    final String target = _normalize(rawPath);
    final Uint8List? file = _server.fileAt(target);
    final Socket? data = await _awaitData();
    if (file == null) {
      _write('550 No such file or directory.\r\n');
      if (data != null) {
        await _closeData(data);
      }
      return;
    }
    if (data == null) {
      _write('425 Use PASV or EPSV first.\r\n');
      return;
    }
    _write('150 Opening BINARY mode data connection.\r\n');
    ++_server.retrCount;
    final int offset = _server.supportRest ? math.min(_restOffset, file.length) : 0;
    const int chunk = 16 * 1024;
    // 插件在拿到它要的数据后会主动断开（拖动 / 只看开头几 KB）：写失败是很常见的事，
    // 这里不能把异常漏出去（异步函数里的未捕获异常会直接把用例判失败）。
    for (int at = offset; at < file.length; at += chunk) {
      final int end = math.min(at + chunk, file.length);
      if (!await _writeData(data, file.sublist(at, end))) {
        await _closePassive();
        return;
      }
    }
    await _closeData(data);
    await _closePassive();
    _write('226 Transfer complete.\r\n');
  }

  /// 往数据连接写一段；对方已经关掉连接时返回 false（不抛异常）
  Future<bool> _writeData(Socket data, List<int> bytes) async {
    try {
      data.add(bytes);
      await data.flush();
      return true;
    } catch (_) {
      return false;
    }
  }

  /// 关数据连接（可能已经被对方关掉）
  Future<void> _closeData(Socket data) async {
    try {
      await data.close();
    } catch (_) {
      // 已经关了
    }
  }

  String _normalize(String raw) {
    String text = raw.trim();
    if (text.isEmpty) {
      return '/';
    }
    if (!text.startsWith('/')) {
      text = '/$text';
    }
    while (text.length > 1 && text.endsWith('/')) {
      text = text.substring(0, text.length - 1);
    }
    return text;
  }

  Future<void> _closePassive() async {
    final ServerSocket? passive = _passive;
    _passive = null;
    if (passive != null) {
      try {
        await passive.close();
      } catch (_) {
        // 已经关了
      }
    }
  }

  void _dispose() {
    if (_closed) {
      return;
    }
    _closed = true;
    try {
      _socket.destroy();
    } catch (_) {
      // 已经关了
    }
    unawaited(_closePassive());
  }
}

/* ==================== 环境探测 ==================== */

/// 等一个 Future 完成，同时持续泵事件（插件的能力 / 钩子结果是经事件回来的）
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
  final DateTime deadline = DateTime.now().add(const Duration(seconds: 30));
  while (!done) {
    if (DateTime.now().isAfter(deadline)) {
      throw TimeoutException('等待插件返回超时（事件泵 30 s 内没有结果）');
    }
    runtime.pumpEvents();
    await Future<void>.delayed(const Duration(milliseconds: 5));
  }
  if (error != null) {
    throw error!;
  }
  return value as T;
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
