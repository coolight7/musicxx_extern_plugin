/// 插件清单（`plugin.yaml`）读取：本宿主关心的字段只有一处实现
///
/// 分工：
/// - **内核**（`pluginxx::parsePluginManifest`）负责 `name` / `entry` / `depends` /
///   `optional_depends` 的权威解析（扫描、装载、依赖检查都用它）；
/// - 本模块负责宿主自己的扩展字段（kind / version / scripts / permissions /
///   platforms / arch / api_version / targets_dir 等），供扫描、装载与安装预检共用。
///
/// 之所以单独成文件：这三条路径（`scanDir`、`plugin_inspect`、原生装载）必须看到**同一份**
/// 字段值，否则会出现"扫描说支持、装载却按另一个分支"这类分歧。
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace musicxx {
namespace extern_plugin {

/// `plugin.yaml` 里的扩展字段（缺省值与清单缺省一致）
struct MusicxxManifestFields {
  /// YAML 能否解析（false = 调用方按"清单非法"处理；缺文件同样为 false）
  bool yamlOk = false;

  /// native / js / builtin（清单没写时为空，由调用方按 entry 推导）
  std::string kind;
  std::string version;
  std::string description;
  std::string author;
  std::string homepage;

  /// 兼容的插件 API 版本（宿主拒绝高于自己版本的插件）
  int32_t apiVersion = 1;

  /// JS 插件按顺序执行的脚本（相对插件目录；空 = 用 entry / plugin.js）
  std::vector<std::string> scripts;
  std::vector<std::string> permissions;
  std::vector<std::string> platforms;
  std::vector<std::string> arch;

  /// 多目标打包的分支目录名（缺省 `lib`；清单里显式写空串 = 关闭分支扫描）
  std::string targetsDir = "lib";
};

/// 读插件目录的清单扩展字段
///
/// - 目录里没有 `plugin.yaml`、文件为空、YAML 非法：返回 `yamlOk = false`
///   的结构（字段为缺省值；调用方据此给出"清单无效"结论）；
/// - 解析失败只记日志，不抛异常。
MusicxxManifestFields readManifestFields(const std::filesystem::path &pluginDir);

/// 读清单的依赖声明（`depends` / `optional_depends`；支持标量与列表两种写法）
///
/// JS 插件单独用：内核的按名依赖检查用实例名查表，而 JS 实例名是 `js:<id>`，
/// 因此 JS 插件由宿主按**插件 id** 自行校验一遍（语义与动态库插件一致）。
void readManifestDepends(const std::filesystem::path &pluginDir,
                         std::vector<std::string> &depends,
                         std::vector<std::string> &optionalDepends);

} // namespace extern_plugin
} // namespace musicxx
