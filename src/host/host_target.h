/// 外部插件多目标打包支持 (仿 APK 的原生库目录结构)
///
/// 一个插件包 (目录或压缩包) 里可以同时放多个 **系统 / CPU 架构分支**, 宿主在扫描与
/// 装载时按当前环境选一个分支用:
///
/// ```text
/// my_plugin/
///   plugin.yaml                 清单 (一份; entry 是"分支里的库文件名")
///   lib/                        分支根目录 (清单 targets_dir 可改名, 缺省 lib)
///     windows-x64/  my_plugin.dll
///     linux-x64/    my_plugin.so
///     linux-arm64/  my_plugin.so
///     android-arm64-v8a/ my_plugin.so
///     windows/      只限系统 (架构不限)
///     x64/          只限架构 (系统不限)
///     universal/    通用分支 (任何系统 / 架构都能用)
///   shader/                     与分支无关的共享资源 (不在分支目录里)
/// ```
///
/// 选择规则 (从具体到通用, 先命中先用):
///   1. 系统 + 架构都匹配 (windows-x64)
///   2. 只声明系统 (windows)
///   3. 只声明架构 (x64)
///   4. 通用分支 (universal)
///   5. 插件根目录下的库文件 (旧布局 / 隐式通用, 见 resolvePluginTargets)
///
/// 分支目录里找库文件的顺序: 清单 `entry` (按平台修正扩展名) → 平台默认库名
/// (lib<名>.so / <名>.dll / lib<名>.dylib) → 目录里唯一的 *.dll/*.so/*.dylib。
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace musicxx {
namespace extern_plugin {

/// 分支标签的解析结果 (标签 = 分支目录名)
struct MusicxxTargetTag {
  bool recognized = false; ///< 是否是可识别的标签 (系统 / 架构 / 通用)
  std::string os;          ///< 规范系统名 (空 = 不限; 见 normalizeTargetOs)
  std::string arch;        ///< 规范架构名 (空 = 不限; 见 normalizeTargetArch)
  bool universal = false;  ///< 通用分支 (universal / any / noarch ...)
};

/// 包内的一个分支 (含该分支里解析到的库文件)
struct MusicxxPluginTarget {
  /// 分支目录名 (标签); 空串表示"插件根目录"这一个隐式通用分支
  std::string tag;

  /// 分支目录 (相对插件目录的路径; 根目录时为空串)
  std::string dir;

  /// 分支内的库文件 (相对插件目录的路径; 找不到时为空串)
  std::string lib;

  /// 库文件绝对路径 (装载用; 找不到时为空串)
  std::string libPath;

  /// 声明的系统 / 架构 (空 = 不限) 与是否通用分支
  std::string os;
  std::string arch;
  bool universal = false;

  /// 与当前环境的匹配等级: 3 系统+架构 / 2 系统 / 1 架构 / 0 通用 / -1 不匹配
  int matchLevel = -1;

  /// 是否是最终选中要加载的分支
  bool selected = false;
};

/// 一次目标解析的结果 (扫描与装载共用同一份判定)
struct MusicxxPluginTargets {
  /// 分支根目录名 (清单 `targets_dir`; 缺省 lib; 空串 = 清单显式关闭分支扫描)
  std::string dirName = "lib";

  /// 包内识别到的分支目录 (含不匹配当前环境的; 按选择优先级排序)
  std::vector<MusicxxPluginTarget> all;

  /// 选中的分支标签 (空串 = 用插件根目录, 即旧布局或通用回退)
  std::string selectedTag;

  /// 选中的库文件 (相对插件目录; 空串 = 没找到可加载的库文件)
  std::string selectedLib;

  /// 选中的库文件绝对路径 (装载用; 空串 = 没找到可加载的库文件)
  std::string selectedLibPath;

  /// 选择的说明 / 不匹配原因 (用户可见; 正常选中时可能为空)
  std::string note;

  /// 包内是否存在识别到的分支目录
  bool hasBranches = false;
};

/// 解析分支标签 (目录名): 支持 `windows-x64` / `linux_arm64` / `android-arm64-v8a` /
/// `x64` / `windows` / `universal` 等写法 (别名见 normalizeTargetOs/normalizeTargetArch)
MusicxxTargetTag parseTargetTag(std::string_view tag);

/// 系统名 → 规范名 (windows / linux / macos / android / ios / ohos ...);
/// 未识别返回空串
std::string normalizeTargetOs(std::string_view name);

/// 架构名 → 规范名 (x64 / x86 / arm64 / armv7 / riscv64 / loongarch64 ...);
/// 未识别返回空串
std::string normalizeTargetArch(std::string_view name);

/// 当前宿主的规范系统名 / 架构名 (与宿主上报给插件的取值一致)
std::string currentTargetOs();
std::string currentTargetArch();

/// 读清单 `targets_dir` (缺省 lib; 清单里显式写空串 = 关闭分支扫描)
std::string readManifestTargetsDir(const std::filesystem::path &pluginDir);

/// 平台默认库文件名 (与内核 defaultPluginLibraryPath 同一规则):
/// windows → `<名>.dll` / `lib<名>.dll`, macos → `lib<名>.dylib`, 其余 → `lib<名>.so`
std::string defaultPluginLibraryName(std::string_view pluginName);

/// 解析插件目录里的分支, 并选出当前系统/架构要加载的库文件
///
/// - `os` / `arch` 为外部传入的规范名 (宿主: 平台标识与 hostArch; 便于测试注入);
/// - `scanRootFallback` = true 时, 没有匹配分支会回退到插件根目录的库文件 (旧布局);
/// - 结果里的 `note` 在"没有可用库文件"时给出用户可读原因。
MusicxxPluginTargets
resolvePluginTargets(const std::filesystem::path &pluginDir,
                     const std::string &targetsDir, const std::string &entry,
                     const std::string &pluginName, const std::string &os,
                     const std::string &arch, bool scanRootFallback = true);

/// 把分支列表拼成用户可读文本 (例: `windows-x64、linux-x64`; 空 = "无")
std::string describePluginTargets(const MusicxxPluginTargets &targets);

} // namespace extern_plugin
} // namespace musicxx
