/// 插件目录的**静态判定**（清单 + 多目标分支选择 + 库文件是否存在）
///
/// 一个入口给三条路径用：
/// - `plugin_scan`（宿主扫描）：叠加运行期开关后作为扫描结果；
/// - `plugin_inspect`（安装预检）：应用把解包出来的临时目录交给宿主判定，
///   "这个包在当前系统/架构下能不能用、会用哪个分支"装之前就有答案；
/// - 原生装载：按同一份判定选分支（`loadNativeDirAsync` 复用 `resolvePluginTargets`）。
///
/// 三者共用同一套规则（`host_target.h` 的分支选择 + `host_manifest.h` 的清单字段），
/// 因此不会出现"扫描/预检说一套、装载做另一套"。
#pragma once

#include "host_target.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace musicxx {
namespace extern_plugin {

/// 插件目录的静态判定结果（不装载、不看运行期开关）
struct MusicxxPluginInspect {
  /// 清单是否可解析（能读到内核要求的 `name`）
  bool valid = false;

  /// 不可解析时的原因（用户可见）
  std::string error;

  /// 插件目录（绝对路径）与目录名
  std::string dir;
  std::string dirName;

  /// 插件 id（清单 `name`）与形态（native / js）
  std::string id;
  std::string kind;

  /// 清单字段
  std::string version;
  std::string description;
  std::string author;
  std::string homepage;
  std::string entry;
  int32_t apiVersion = 1;
  std::vector<std::string> depends;
  std::vector<std::string> optionalDepends;
  std::vector<std::string> permissions;
  std::vector<std::string> platforms;
  std::vector<std::string> arch;
  std::vector<std::string> scripts;

  /// 多目标打包的分支解析结果（只有动态库插件会填）
  MusicxxPluginTargets targets;

  /// 当前系统/架构/API 版本/库文件是否可用（不含运行期开关）
  bool supported = true;

  /// 不可用原因（用户可见；`supported=true` 时为空）
  std::string reason;
};

/// 检查一个插件目录（**只读**）：清单字段 + 多目标分支选择 + 库文件判定
///
/// - `os` / `arch` 为空时用宿主当前平台/架构（别名都认，见 host_target.h）；
/// - 目录不存在、缺 `plugin.yaml`、YAML 非法、缺 `name` → `valid=false` + `error`；
/// - 不读取运行期开关（JS 运行时是否可用、安全模式、禁用动态库等由调用方叠加）。
MusicxxPluginInspect inspectPluginDir(const std::filesystem::path &pluginDir,
                                      const std::string &os = {},
                                      const std::string &arch = {});

/// 判定结果 → JSON（`plugin_inspect` 的出参；也是扫描项 JSON 的字段来源）
///
/// 形状：
/// ```json
/// {"valid":true,"error":"","dir":"...","dirName":"...","id":"demo","kind":"native",
///  "version":"1.0.0","description":"","author":"","homepage":"","entry":"demo.so",
///  "apiVersion":1,"depends":[],"optionalDepends":[],"permissions":[],"platforms":[],
///  "arch":[],"scripts":[],"targetsDir":"lib","target":"windows-x64",
///  "targetEntry":"lib/windows-x64/demo.dll","targets":[...],"supported":true,"reason":""}
/// ```
std::string inspectToJson(const MusicxxPluginInspect &info);

} // namespace extern_plugin
} // namespace musicxx
