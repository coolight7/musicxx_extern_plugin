/// 版本号声明的校验：格式、三处一致（Dart 声明 / C 头宏 / pubspec）、示例插件清单
///
/// 为什么要有这个用例：插件框架的版本号写在两个语言里 —— Dart 侧
/// `MusicxxPluginVersion`（`lib/src/version.dart`）与 C 头里的 `MUSICXX_EXTERN_PLUGIN_VERSION`
/// （`src/include/musicxx_extern_plugin_api.h`）。主程序管理页显示的是 Dart 侧那份，插件在
/// `host.info().hostVersion` / `musicxx.version` 读到的是原生库自报的那份，两边不一致时表现为
/// "页面显示的版本和库里自报的版本不是一个值"。`pubspec.yaml` 与示例插件清单用同一套格式，
/// 一并守着。
///
/// 改版本号的流程（含补记提交号）见 `docs/version-history.md`。
///
/// 运行：`flutter test test/version_test.dart`
library;

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:musicxx_extern_plugin/musicxx_extern_plugin.dart';

/// 版本号格式 `<年>.<月日>.<小版本号>`（例 `2026.1011.1`；月日是 4 位 `MMDD`，1—9 月补 0）
final RegExp _versionFormat = RegExp(
  r'^\d{4}\.(0[1-9]|1[0-2])(0[1-9]|[12]\d|3[01])\.\d+$',
);

/// 读文件内容（测试在包目录下跑，路径相对包根）
String _read(String relativePath) {
  final File file = File(relativePath);
  expect(file.existsSync(), isTrue, reason: '找不到 $relativePath（请在包目录下运行测试）');
  return file.readAsStringSync();
}

/// 取清单里的 `version`（顶层键；读不到返回空串）
String _manifestVersion(String text) {
  final Match? match = RegExp(
    r'^\s*version\s*:\s*(.+?)\s*$',
    multiLine: true,
  ).firstMatch(text);
  if (null == match) {
    return '';
  }
  return match.group(1)!.trim().replaceAll(RegExp(r'''^["']|["']$'''), '');
}

void main() {
  group('插件框架版本声明', () {
    test('版本号格式合法（年.月日.小版本号）', () {
      expect(
        _versionFormat.hasMatch(MusicxxPluginVersion.version),
        isTrue,
        reason: '版本号 `${MusicxxPluginVersion.version}` 不符合 `<年>.<月日>.<小版本号>`',
      );
    });

    test('日期字段与版本号里的年月日一致', () {
      final List<String> version = MusicxxPluginVersion.version.split('.');
      final List<String> date = MusicxxPluginVersion.date.split('-');
      expect(date.length, 3, reason: '日期要写成 `YYYY-MM-DD`');
      expect(version[0], date[0], reason: '版本号的年份与日期不一致');
      expect(version[1], '${date[1]}${date[2]}', reason: '版本号的月日与日期不一致');
    });

    test('Dart 声明与 C 头宏一致（原生库自报的就是那个宏）', () {
      final String header = _read('src/include/musicxx_extern_plugin_api.h');
      final Match? match = RegExp(
        r'#define\s+MUSICXX_EXTERN_PLUGIN_VERSION\s+"([^"]+)"',
      ).firstMatch(header);
      expect(
        match,
        isNotNull,
        reason: 'C 头里必须有 MUSICXX_EXTERN_PLUGIN_VERSION 宏',
      );
      expect(
        match!.group(1),
        MusicxxPluginVersion.version,
        reason: 'C 头宏与 lib/src/version.dart 的版本号必须一致',
      );
      expect(
        _versionFormat.hasMatch(match.group(1)!),
        isTrue,
        reason: 'C 头宏的版本号也要用 `<年>.<月日>.<小版本号>` 格式',
      );
    });

    test('pubspec 的 version 与声明一致', () {
      final String pubspec = _read('pubspec.yaml');
      final Match? match = RegExp(
        r'^version:\s*(\S+)\s*$',
        multiLine: true,
      ).firstMatch(pubspec);
      expect(match, isNotNull, reason: 'pubspec.yaml 里没有 version');
      expect(
        match!.group(1),
        MusicxxPluginVersion.version,
        reason: 'pubspec.yaml 的 version 要与版本号声明一致',
      );
    });

    test('提交号：留空或 7 位以上十六进制（补记流程见 docs/version-history.md）', () {
      const String commit = MusicxxPluginVersion.commit;
      if (commit.isEmpty) {
        return;
      }
      expect(
        RegExp(r'^[0-9a-f]{7,40}$').hasMatch(commit),
        isTrue,
        reason: '提交号写成 7 位以上的十六进制短号（当前 `$commit`）',
      );
    });

    test('GitHub 跳转地址：给了提交号就指向那个提交，没给就指向仓库', () {
      final String expected = MusicxxPluginVersion.commit.isEmpty
          ? MusicxxPluginVersion.repositoryUrl
          : '${MusicxxPluginVersion.repositoryUrl}/commit/${MusicxxPluginVersion.commit}';
      expect(MusicxxPluginVersion.commitUrl, expected);
      expect(
        MusicxxPluginVersion.repositoryUrl.startsWith('https://github.com/'),
        isTrue,
      );
      expect(MusicxxPluginVersion.describe, contains(MusicxxPluginVersion.version));
    });
  });

  group('示例插件清单的版本号', () {
    test('每个示例插件都声明了版本号，且用同一套格式', () {
      final Directory plugins = Directory('plugins');
      expect(plugins.existsSync(), isTrue, reason: '找不到 plugins/ 目录');
      final List<Directory> dirs = plugins
          .listSync()
          .whereType<Directory>()
          .toList()
        ..sort((Directory a, Directory b) => a.path.compareTo(b.path));
      expect(dirs, isNotEmpty);
      for (final Directory dir in dirs) {
        final String manifestPath = '${dir.path}/plugin.yaml';
        if (false == File(manifestPath).existsSync()) {
          continue;
        }
        final String version = _manifestVersion(
          File(manifestPath).readAsStringSync(),
        );
        expect(
          version,
          isNotEmpty,
          reason: '$manifestPath 没有声明 version',
        );
        expect(
          _versionFormat.hasMatch(version),
          isTrue,
          reason: '$manifestPath 的 version `$version` 不符合 `<年>.<月日>.<小版本号>`',
        );
      }
    });
  });
}
