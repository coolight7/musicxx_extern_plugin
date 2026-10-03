/// 多目标打包（仿 APK 的 `lib/<系统>-<架构>/`）：Dart 侧只保留数据模型与环境取值
///
/// 说明：**分支选择规则只在原生宿主**（`src/host/host_target.{h,cpp}`），Dart 侧不再复制
/// 一份实现。规则本身由原生用例 `test_host.cpp::testPluginTargetLayout` 覆盖；对真实宿主
/// 的调用（`plugin_inspect` 与 `plugin_scan` 结论一致）见 `test/host_test.dart`。
///
/// 运行：`flutter test test/plugin_target_test.dart`
library;

import 'dart:ffi';

import 'package:flutter_test/flutter_test.dart';
import 'package:musicxx_extern_plugin/musicxx_extern_plugin.dart';

void main() {
  group('MusicxxPluginEnv（环境取值）', () {
    test('系统名/架构名与宿主的规范名对得上', () {
      final String os = MusicxxPluginEnv.currentOs();
      final String arch = MusicxxPluginEnv.currentArch();
      expect(<String>[
        'windows',
        'linux',
        'macos',
        'android',
        'ios',
        'ohos',
        'fuchsia',
      ], contains(os));
      expect(<String>[
        'x64',
        'x86',
        'arm64',
        'armv7',
        'riscv64',
      ], contains(arch));
    });

    test('32 位 ARM 报 armv7（Android armeabi-v7a / Linux armhf）', () {
      // 与宿主 `currentTargetArch()` 同一套取值：清单 `arch`、分支标签都用这些名字
      expect(MusicxxPluginEnv.currentArch(Abi.androidArm), 'armv7');
      expect(MusicxxPluginEnv.currentArch(Abi.linuxArm), 'armv7');
      // 64 位 ARM 不能被 32 位的判断吃掉
      expect(MusicxxPluginEnv.currentArch(Abi.androidArm64), 'arm64');
      expect(MusicxxPluginEnv.currentArch(Abi.linuxArm64), 'arm64');
      expect(MusicxxPluginEnv.currentArch(Abi.androidX64), 'x64');
      expect(MusicxxPluginEnv.currentArch(Abi.androidIA32), 'x86');
      expect(MusicxxPluginEnv.currentArch(Abi.linuxRiscv64), 'riscv64');
    });

    test('平台默认库名与宿主同一规则', () {
      switch (MusicxxPluginEnv.currentOs()) {
        case 'windows':
          expect(MusicxxPluginEnv.defaultLibraryName('demo'), 'demo.dll');
        case 'macos':
        case 'ios':
          expect(MusicxxPluginEnv.defaultLibraryName('demo'), 'libdemo.dylib');
        default:
          expect(MusicxxPluginEnv.defaultLibraryName('demo'), 'libdemo.so');
      }
    });

    test('分支目录缺省名与宿主、清单 targets_dir 缺省一致', () {
      expect(MusicxxPluginEnv.defaultTargetsDirName, 'lib');
    });
  });

  group('宿主判定结果的解析（plugin_scan / plugin_inspect 的字段）', () {
    test('MusicxxPluginInspect_c：分支、选中项与展示文本', () {
      final MusicxxPluginInspect_c inspect = MusicxxPluginInspect_c.fromJson(
        <String, Object?>{
          'valid': true,
          'id': 'demo',
          'kind': 'native',
          'entry': 'demo.so',
          'targetsDir': 'lib',
          'target': 'linux-x64',
          'targetEntry': 'lib/linux-x64/demo.so',
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
          'supported': true,
          'reason': '',
        },
      );
      expect(inspect.valid, isTrue);
      expect(inspect.hasBranches, isTrue);
      expect(inspect.selected?.tag, 'linux-x64');
      expect(inspect.targetText, 'linux-x64（lib/linux-x64/demo.so）');
      expect(inspect.tagsText, 'linux-x64、windows-x64');
      expect(inspect.describe(), contains('包内分支：linux-x64、windows-x64'));
      expect(inspect.targets.last.matchLevel, -1);
      expect(inspect.targets.last.depict, 'windows-x64（windows/x64）');
    });

    test('插件根目录的库文件（旧布局）展示为"插件根目录"', () {
      final MusicxxPluginInspect_c inspect = MusicxxPluginInspect_c.fromJson(
        <String, Object?>{
          'valid': true,
          'id': 'demo',
          'kind': 'native',
          'target': '',
          'targetEntry': 'demo.so',
          'targets': <Object?>[],
          'supported': true,
        },
      );
      expect(inspect.hasBranches, isFalse);
      expect(inspect.targetText, '插件根目录（demo.so）');
      expect(inspect.describe(), '库文件：插件根目录（demo.so）');
    });

    test('不可用原因与清单解析失败都走同一个模型', () {
      final MusicxxPluginInspect_c unsupported =
          MusicxxPluginInspect_c.fromJson(<String, Object?>{
            'valid': true,
            'id': 'demo',
            'kind': 'native',
            'targets': <Object?>[],
            'supported': false,
            'reason': '包内没有匹配当前系统/架构的分支 (当前 macos/arm64, 包内分支: windows-x64)',
          });
      expect(unsupported.supported, isFalse);
      expect(unsupported.describe(), contains('没有匹配当前系统/架构的分支'));

      final MusicxxPluginInspect_c invalid =
          MusicxxPluginInspect_c.fromJson(<String, Object?>{
            'valid': false,
            'error': '插件目录不存在',
          });
      expect(invalid.valid, isFalse);
      expect(invalid.error, '插件目录不存在');
      expect(invalid.hasBranches, isFalse);
    });

    test('扫描项的 targets 字段与 inspect 共用同一个分支模型', () {
      final MusicxxPluginInfo info = MusicxxPluginInfo.fromJson(
        <String, Object?>{
          'id': 'demo',
          'kind': 'native',
          'target': 'windows-x64',
          'targetEntry': 'lib/windows-x64/demo.dll',
          'targetsDir': 'lib',
          'targets': <Object?>[
            <String, Object?>{
              'tag': 'windows-x64',
              'dir': 'lib/windows-x64',
              'lib': 'lib/windows-x64/demo.dll',
              'os': 'windows',
              'arch': 'x64',
              'universal': false,
              'match': 3,
              'selected': true,
            },
          ],
        },
      );
      expect(info.target, 'windows-x64');
      expect(info.targets.single.selected, isTrue);
      expect(info.targets.single.matchLevel, 3);
    });
  });
}
