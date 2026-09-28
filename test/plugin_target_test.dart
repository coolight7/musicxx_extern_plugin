/// 多目标打包（仿 APK 的 `lib/<系统>-<架构>/`）：Dart 侧规则单测
///
/// 这里验证的是**安装预检与管理页用的那一套规则**（`lib/src/plugin_target.dart`）。
/// 权威判定在原生宿主（`src/host/host_target.cpp`），两侧规则必须一致：宿主的
/// 分支选择与装载行为由包内 `test/host_test.dart` 与应用侧 `extern_plugin` 模块覆盖。
///
/// 运行：`flutter test test/plugin_target_test.dart`
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:musicxx_extern_plugin/musicxx_extern_plugin.dart';
import 'package:path/path.dart' as path;

void main() {
  group('分支标签解析', () {
    test('系统 + 架构（两种顺序、多种分隔符与别名都能认）', () {
      for (final String tag in <String>[
        'windows-x64',
        'windows_x64',
        'Windows.X64',
        'x64-windows',
        'win-amd64',
        'linux-arm64',
        'gnu-aarch64',
        'macos-arm64',
        'darwin-arm64',
        'osx-x64',
        'android-arm64-v8a',
        'android-armeabi-v7a',
        'android-x86_64',
      ]) {
        final MusicxxTargetTag_c parsed = MusicxxPluginTargets.parseTag(tag);
        expect(parsed.recognized, true, reason: tag);
        expect(parsed.os.isNotEmpty, true, reason: tag);
        expect(parsed.arch.isNotEmpty, true, reason: tag);
      }
    });

    test('具体取值', () {
      expect(MusicxxPluginTargets.parseTag('windows-x64').os, 'windows');
      expect(MusicxxPluginTargets.parseTag('windows-x64').arch, 'x64');
      // ABI 写法（APK 的 lib/<abi>/）与规范架构名等价
      expect(MusicxxPluginTargets.parseTag('android-arm64-v8a').arch, 'arm64');
      expect(MusicxxPluginTargets.parseTag('android-arm64-v8a').os, 'android');
      expect(MusicxxPluginTargets.parseTag('android-armeabi-v7a').arch, 'armv7');
      expect(MusicxxPluginTargets.parseTag('android-x86_64').arch, 'x64');
      expect(MusicxxPluginTargets.parseTag('armeabi-v7a').arch, 'armv7');
      expect(MusicxxPluginTargets.parseTag('arm64-v8a').arch, 'arm64');
      // win32 是系统别名（不是架构）
      expect(MusicxxPluginTargets.parseTag('win32').os, 'windows');
      expect(MusicxxPluginTargets.parseTag('win32').arch, '');
    });

    test('只系统 / 只架构', () {
      final MusicxxTargetTag_c osOnly =
          MusicxxPluginTargets.parseTag('windows');
      expect(osOnly.recognized, true);
      expect(osOnly.os, 'windows');
      expect(osOnly.arch, '');

      final MusicxxTargetTag_c archOnly = MusicxxPluginTargets.parseTag('x64');
      expect(archOnly.recognized, true);
      expect(archOnly.os, '');
      expect(archOnly.arch, 'x64');
    });

    test('通用分支别名', () {
      for (final String tag in <String>[
        'universal',
        'any',
        'all',
        'noarch',
        'NoArch',
      ]) {
        final MusicxxTargetTag_c parsed = MusicxxPluginTargets.parseTag(tag);
        expect(parsed.recognized, true, reason: tag);
        expect(parsed.universal, true, reason: tag);
        expect(parsed.os.isEmpty && parsed.arch.isEmpty, true, reason: tag);
      }
    });

    test('不认识的名字不算分支（普通资源目录被忽略）', () {
      for (final String tag in <String>[
        'utils',
        'shader',
        'libs',
        'windows-x64-debug',
        'x64_arm64',
        '',
      ]) {
        expect(
          MusicxxPluginTargets.parseTag(tag).recognized,
          false,
          reason: tag,
        );
      }
    });

    test('匹配等级：系统+架构 > 只系统 > 只架构 > 通用；系统或架构不符即不匹配', () {
      final MusicxxTargetTag_c exact =
          MusicxxPluginTargets.parseTag('linux-arm64');
      expect(exact.matchLevelOf('linux', 'arm64'), 3);
      expect(exact.matchLevelOf('linux', 'x64'), -1);
      // 系统不符就是不可用（跨系统的库文件加载不了）
      expect(exact.matchLevelOf('windows', 'arm64'), -1);

      expect(
        MusicxxPluginTargets.parseTag('linux').matchLevelOf('linux', 'x64'),
        2,
      );
      expect(
        MusicxxPluginTargets.parseTag('arm64').matchLevelOf('linux', 'arm64'),
        1,
      );
      expect(
        MusicxxPluginTargets.parseTag('universal').matchLevelOf('linux', 'x64'),
        0,
      );
    });
  });

  group('选择规则（临时目录）', () {
    late Directory root;

    setUp(() {
      root = Directory.systemTemp.createTempSync('musicxx_target_test_');
    });

    tearDown(() {
      if (root.existsSync()) {
        root.deleteSync(recursive: true);
      }
    });

    /// 建插件目录：写清单与相对路径文件（文件内容无所谓）
    String makePackage(
      List<String> relativeFiles, {
      String name = 'demo',
      String entry = 'demo.so',
      String targetsDir = 'lib',
    }) {
      final Directory dir = Directory(path.join(root.path, name));
      dir.createSync(recursive: true);
      File(path.join(dir.path, 'plugin.yaml')).writeAsStringSync(
        'name: $name\nentry: $entry\nkind: native\ntargets_dir: $targetsDir\n',
      );
      for (final String relative in relativeFiles) {
        final File file = File(path.join(dir.path, relative));
        file.parent.createSync(recursive: true);
        file.writeAsStringSync('not a real library');
      }
      return dir.path;
    }

    test('优先级：系统+架构 → 只系统 → 只架构 → 通用 → 根目录', () {
      final String dir = makePackage(<String>[
        'lib/windows-x64/other.dll',
        'lib/universal/other.so',
        'lib/arm64/other.so',
        'lib/linux/other.so',
        'lib/linux-arm64/demo.so',
        'demo.so',
      ]);

      // 精确匹配（系统+架构）
      MusicxxPluginTargets_c targets = MusicxxPluginTargets.resolve(
        pluginDir: dir,
        targetsDir: 'lib',
        entry: 'demo.so',
        pluginName: 'demo',
        os: 'linux',
        arch: 'arm64',
      );
      expect(targets.selectedTag, 'linux-arm64');
      expect(targets.selectedLib, 'lib/linux-arm64/demo.so');
      expect(targets.hasBranches, true);
      expect(targets.selected?.matchLevel, 3);
      expect(
        targets.all.last.matchLevel,
        -1,
        reason: '跨系统的分支排在最后且标为不匹配',
      );

      // 去掉精确分支后：只系统优先于只架构
      Directory(
        path.join(dir, 'lib', 'linux-arm64'),
      ).deleteSync(recursive: true);
      targets = MusicxxPluginTargets.resolve(
        pluginDir: dir,
        targetsDir: 'lib',
        entry: 'demo.so',
        pluginName: 'demo',
        os: 'linux',
        arch: 'arm64',
      );
      expect(targets.selectedTag, 'linux');
      expect(targets.selected?.matchLevel, 2);

      // 再去掉只系统分支：选只架构（`arm64` 目录里的库用"唯一动态库"规则认出）
      Directory(path.join(dir, 'lib', 'linux')).deleteSync(recursive: true);
      targets = MusicxxPluginTargets.resolve(
        pluginDir: dir,
        targetsDir: 'lib',
        entry: 'demo.so',
        pluginName: 'demo',
        os: 'linux',
        arch: 'arm64',
      );
      expect(targets.selectedTag, 'arm64');
      expect(targets.selected?.matchLevel, 1);
      expect(targets.selectedLib, 'lib/arm64/other.so');

      // 再去掉只架构分支：选通用分支
      Directory(path.join(dir, 'lib', 'arm64')).deleteSync(recursive: true);
      targets = MusicxxPluginTargets.resolve(
        pluginDir: dir,
        targetsDir: 'lib',
        entry: 'demo.so',
        pluginName: 'demo',
        os: 'linux',
        arch: 'arm64',
      );
      expect(targets.selectedTag, 'universal');
      expect(targets.selected?.matchLevel, 0);

      // 通用分支也没有时：回退插件根目录
      Directory(path.join(dir, 'lib', 'universal')).deleteSync(recursive: true);
      targets = MusicxxPluginTargets.resolve(
        pluginDir: dir,
        targetsDir: 'lib',
        entry: 'demo.so',
        pluginName: 'demo',
        os: 'linux',
        arch: 'arm64',
      );
      expect(targets.selectedTag, '');
      expect(targets.selectedLib, 'demo.so');
      expect(targets.note.contains('已回退插件根目录'), true);
    });

    test('没有匹配分支时回退插件根目录；根目录也没有就没得用', () {
      final String dir = makePackage(<String>['lib/windows-x64/other.dll']);
      MusicxxPluginTargets_c targets = MusicxxPluginTargets.resolve(
        pluginDir: dir,
        targetsDir: 'lib',
        entry: 'demo.so',
        pluginName: 'demo',
        os: 'linux',
        arch: 'arm64',
      );
      expect(targets.selectedTag, '');
      expect(targets.selectedLib, '');
      expect(targets.note.contains('没有匹配当前系统/架构的分支'), true);
      expect(targets.note.contains('windows-x64'), true);
      expect(
        targets.all.single.matchLevel,
        -1,
        reason: '不匹配的分支照常列出（管理页要能看到包内分支）',
      );

      File(path.join(dir, 'demo.so')).writeAsStringSync('root lib');
      targets = MusicxxPluginTargets.resolve(
        pluginDir: dir,
        targetsDir: 'lib',
        entry: 'demo.so',
        pluginName: 'demo',
        os: 'linux',
        arch: 'arm64',
      );
      expect(targets.selectedTag, '');
      expect(targets.selectedLib, 'demo.so');
      expect(targets.note.contains('已回退插件根目录'), true);
    });

    test('旧布局（没有分支目录）不产生分支信息', () {
      final String dir = makePackage(<String>['demo.so']);
      final MusicxxPluginTargets_c targets = MusicxxPluginTargets.resolve(
        pluginDir: dir,
        targetsDir: 'lib',
        entry: 'demo.so',
        pluginName: 'demo',
        os: 'linux',
        arch: 'arm64',
      );
      expect(targets.hasBranches, false);
      expect(targets.all, isEmpty);
      expect(targets.selectedLib, 'demo.so');
      expect(targets.note, '');
    });

    test('平台修正扩展名：Windows/macOS 上按 entry 的 Linux 写法找 .dll/.dylib', () {
      final String dir = makePackage(<String>['lib/universal/demo.so']);
      // 当前平台是 Windows 时 entry `demo.so` 会解析成 `demo.dll`
      final String expected = MusicxxPluginTargets.platformLibraryName(
        'demo.so',
        'demo',
      );
      final MusicxxPluginTargets_c targets = MusicxxPluginTargets.resolve(
        pluginDir: dir,
        targetsDir: 'lib',
        entry: expected,
        pluginName: 'demo',
        os: 'linux',
        arch: 'x64',
      );
      expect(targets.selectedTag, 'universal');
      expect(targets.selectedLib, 'lib/universal/demo.so');
    });

    test('分支里唯一的动态库会被自动识别（名字不必与 entry 相同）', () {
      final String dir = makePackage(<String>['lib/universal/whatever.so']);
      final MusicxxPluginTargets_c targets = MusicxxPluginTargets.resolve(
        pluginDir: dir,
        targetsDir: 'lib',
        entry: 'demo.so',
        pluginName: 'demo',
        os: 'linux',
        arch: 'x64',
      );
      expect(targets.selectedTag, 'universal');
      expect(targets.selectedLib, 'lib/universal/whatever.so');
    });

    test('分支里有多个库文件且名字对不上时，不猜（判为空）', () {
      final String dir = makePackage(<String>[
        'lib/universal/a.so',
        'lib/universal/b.so',
      ]);
      final MusicxxPluginTargets_c targets = MusicxxPluginTargets.resolve(
        pluginDir: dir,
        targetsDir: 'lib',
        entry: 'demo.so',
        pluginName: 'demo',
        os: 'linux',
        arch: 'x64',
      );
      expect(targets.selectedTag, 'universal');
      expect(targets.selectedLib, '');
      expect(targets.note.contains('没有可加载的库文件'), true);
    });

    test('targets_dir 改名：只扫清单指定的目录', () {
      final String dir = makePackage(
        <String>['targets/linux-x64/demo.so', 'lib/linux-x64/demo.so'],
        targetsDir: 'targets',
      );
      final MusicxxPluginTargets_c targets = MusicxxPluginTargets.resolve(
        pluginDir: dir,
        targetsDir: 'targets',
        entry: 'demo.so',
        pluginName: 'demo',
        os: 'linux',
        arch: 'x64',
      );
      expect(targets.dirName, 'targets');
      expect(targets.selectedLib, 'targets/linux-x64/demo.so');
      expect(targets.all.single.tag, 'linux-x64');
    });

    test('不认识的分支目录不算分支', () {
      final String dir = makePackage(<String>[
        'lib/linux-x64/demo.so',
        'lib/utils/demo.so',
        'lib/shader/demo.so',
      ]);
      final MusicxxPluginTargets_c targets = MusicxxPluginTargets.resolve(
        pluginDir: dir,
        targetsDir: 'lib',
        entry: 'demo.so',
        pluginName: 'demo',
        os: 'linux',
        arch: 'x64',
      );
      expect(targets.all.length, 1);
      expect(targets.all.single.tag, 'linux-x64');
    });

    test('分支情况说明文本（安装确认与详情页）', () {
      final String dir = makePackage(<String>[
        'lib/linux-x64/demo.so',
        'lib/windows-x64/demo.dll',
      ]);
      final MusicxxPluginTargets_c targets = MusicxxPluginTargets.resolve(
        pluginDir: dir,
        targetsDir: 'lib',
        entry: 'demo.so',
        pluginName: 'demo',
        os: 'linux',
        arch: 'x64',
      );
      final String text = MusicxxPluginTargets.describe(targets);
      expect(text.contains('已选分支：linux-x64'), true);
      expect(text.contains('lib/linux-x64/demo.so'), true);
      expect(text.contains('包内分支：linux-x64、windows-x64'), true);
    });
  });

  group('当前环境取值', () {
    test('系统名/架构名是宿主认得的规范名', () {
      final String os = MusicxxPluginTargets.currentOs();
      final String arch = MusicxxPluginTargets.currentArch();
      expect(MusicxxPluginTargets.normalizeOs(os), os);
      expect(MusicxxPluginTargets.normalizeArch(arch), arch);
    });

    test('平台默认库名与扩展名修正', () {
      final String os = MusicxxPluginTargets.currentOs();
      if (os == 'windows') {
        expect(MusicxxPluginTargets.defaultLibraryName('demo'), 'demo.dll');
        expect(MusicxxPluginTargets.platformLibraryName('demo.so', 'demo'),
            'demo.dll');
      } else if (os == 'macos' || os == 'ios') {
        expect(
          MusicxxPluginTargets.defaultLibraryName('demo'),
          'libdemo.dylib',
        );
        expect(MusicxxPluginTargets.platformLibraryName('demo.so', 'demo'),
            'demo.dylib');
      } else {
        expect(MusicxxPluginTargets.defaultLibraryName('demo'), 'libdemo.so');
        expect(MusicxxPluginTargets.platformLibraryName('demo.so', 'demo'),
            'demo.so');
      }
      expect(MusicxxPluginTargets.isLibraryFileName('a.DLL'), true);
      expect(MusicxxPluginTargets.isLibraryFileName('liba.so.1'), false);
      expect(MusicxxPluginTargets.isLibraryFileName('plugin.yaml'), false);
    });
  });

  group('宿主扫描字段解析', () {
    test('MusicxxPluginInfo 带上分支信息', () {
      final MusicxxPluginInfo info = MusicxxPluginInfo.fromJson(
        <String, Object?>{
          'id': 'demo',
          'kind': 'native',
          'target': 'linux-x64',
          'targetEntry': 'lib/linux-x64/demo.so',
          'targetsDir': 'lib',
          'targets': <Object?>[
            <String, Object?>{
              'tag': 'linux-x64',
              'dir': 'lib/linux-x64',
              'lib': 'lib/linux-x64/demo.so',
              'os': 'linux',
              'arch': 'x64',
              'universal': false,
              'match': 3,
              'selected': true,
            },
            <String, Object?>{
              'tag': 'windows-x64',
              'dir': 'lib/windows-x64',
              'lib': 'lib/windows-x64/demo.dll',
              'os': 'windows',
              'arch': 'x64',
              'universal': false,
              'match': -1,
              'selected': false,
            },
          ],
        },
      );
      expect(info.target, 'linux-x64');
      expect(info.targetEntry, 'lib/linux-x64/demo.so');
      expect(info.targetsDir, 'lib');
      expect(info.targets.length, 2);
      expect(info.targets.first.selected, true);
      expect(info.targets.first.matchLevel, 3);
      expect(info.targets.last.matchLevel, -1);
      expect(info.targets.last.depict, 'windows-x64（windows/x64）');
    });

    test('缺少分支字段时按旧布局处理（不抛异常）', () {
      final MusicxxPluginInfo info = MusicxxPluginInfo.fromJson(
        <String, Object?>{'id': 'demo', 'kind': 'native'},
      );
      expect(info.target, '');
      expect(info.targetEntry, '');
      expect(info.targets, isEmpty);
    });
  });
}
