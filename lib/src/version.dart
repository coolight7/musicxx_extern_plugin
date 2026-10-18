/// 插件框架版本与出处（一个版本号只在这里声明一次）
///
/// 版本号格式 `<年>.<月日>.<小版本号>`，例 `2026.1011.1` = 2026 年 10 月 11 日的第 1 版：
/// 年 4 位；月日 4 位（`MMDD`，1—9 月补 0）；小版本号从 1 开始，同一天再发一次就 +1。
/// 插件清单里的 `version` 用同一套格式（见 `plugins/README.md`）。
///
/// 主程序的管理页、命令行与 MCP 显示的就是这里的值（跳转 GitHub 也用这里的仓库地址）。
///
/// 改版本号时：`src/include/musicxx_extern_plugin_api.h` 的 `MUSICXX_EXTERN_PLUGIN_VERSION`
/// 和 `pubspec.yaml` 要一起改（包内 `test/version_test.dart` 校验三处一致）；
/// 提交号的补记流程与 musicxx 版本对照表见 `docs/version-history.md`。
library;

/// 插件框架版本（Dart 侧声明；C 侧同名宏 `MUSICXX_EXTERN_PLUGIN_VERSION`）
abstract final class MusicxxPluginVersion {
  /// 版本号（`<年>.<月日>.<小版本号>`）
  static const String version = '2026.1019.1';

  /// 本版本号首次出现的提交（本包仓库的短提交号；空串 = 还没补记，见
  /// `docs/version-history.md` 的补记流程）
  ///
  /// 提交号写不进它自己所在的提交，所以声明该版本号的那次提交里这里是空的，
  /// 紧接着的那次提交把它补上。
  static const String commit = '';

  /// 本版本号的日期（`YYYY-MM-DD`，与 [version] 里的年月日一致）
  static const String date = '2026-10-19';

  /// 本包仓库地址（主程序页面上的跳转用它）
  static const String repositoryUrl =
      'https://github.com/coolight7/musicxx_extern_plugin';

  /// 本版本提交在 GitHub 上的地址（[commit] 为空时退回仓库地址）
  static String get commitUrl =>
      commit.isEmpty ? repositoryUrl : '$repositoryUrl/commit/$commit';

  /// 一行说明（例 `2026.1011.1（2026-10-11 · commit fc9c89a）`）
  static String get describe =>
      commit.isEmpty ? '$version（$date）' : '$version（$date · commit $commit）';
}
