/// 插件目录的静态判定（约定见 host_inspect.h）

#include "host_inspect.h"

#include "host_manifest.h"
#include "host_naming.h"

#include "musicxx/plugin/api/plugin_api.h"
#include "pluginxx/host/manifest.h"
#include "utilxx_base/json.h"

#include <system_error>

namespace musicxx {
namespace extern_plugin {

namespace {

namespace fs = std::filesystem;

/// 清单没写 `kind` 时按 entry 推导（有库文件名 = 动态库插件，否则按脚本插件）
std::string kindOf(const MusicxxManifestFields &fields,
                   const std::string &entry) {
  if (!fields.kind.empty()) {
    return fields.kind;
  }
  return entry.empty() ? "js" : "native";
}

/// JS插件要执行的脚本清单（清单 `scripts` 优先；否则用 entry 里的 .js；再否则 plugin.js）
std::vector<std::string> scriptsOf(const MusicxxManifestFields &fields,
                                   const std::string &entry) {
  if (!fields.scripts.empty()) {
    return fields.scripts;
  }
  if (entry.size() > 3 && entry.compare(entry.size() - 3, 3, ".js") == 0) {
    return {entry};
  }
  return {"plugin.js"};
}

/// 名字归一化函数的类型（系统与架构各一张别名表，见 host_target.h）
using NameNormalizer = std::string (*)(std::string_view);

/// 目标系统 / 架构名：调用方传空时用插件框架当前环境
///
/// 别名（`win32` / `osx` / `amd64` …）归一成规范名，认不出来的取值保持原文
/// （按原样比较，不做猜测）。
std::string resolveTargetOs(const std::string &os) {
  if (os.empty()) {
    return currentTargetOs();
  }
  const std::string normalized = normalizeTargetOs(os);
  return normalized.empty() ? os : normalized;
}

std::string resolveTargetArch(const std::string &arch) {
  if (arch.empty()) {
    return currentTargetArch();
  }
  const std::string normalized = normalizeTargetArch(arch);
  return normalized.empty() ? arch : normalized;
}

/// 清单里的取值（`platforms` / `arch`）是否声明了目标环境
///
/// 与多目标分支标签共用同一张别名表：`win32` / `osx` / `amd64` 这类写法都认，
/// 否则会出现"分支目录认别名、清单里的 platforms 不认"的分歧。
bool declaresTarget(const std::vector<std::string> &values,
                    const std::string &target, NameNormalizer normalize) {
  for (const std::string &value : values) {
    if (value == target) {
      return true;
    }
    const std::string normalized = normalize(value);
    if (!normalized.empty() && normalized == target) {
      return true;
    }
  }
  return false;
}

/// 字符串数组拼成用户可读文本（例：`windows、linux`；空 = "无"）
std::string joinText(const std::vector<std::string> &values) {
  std::string text;
  for (const std::string &value : values) {
    if (!text.empty()) {
      text += "、";
    }
    text += value;
  }
  return text;
}

/// 字符串数组 → JSON 数组
utilxx_base::Json toJsonArray(const std::vector<std::string> &values) {
  utilxx_base::Json array = utilxx_base::Json::array();
  for (const std::string &value : values) {
    array.push_back(value);
  }
  return array;
}

} // namespace

std::string inspectToJson(const MusicxxPluginInspect &info) {
  using utilxx_base::Json;
  Json j;
  j["valid"] = info.valid;
  j["error"] = info.error;
  j["path"] = info.dir;
  j["dirName"] = info.dirName;
  j["id"] = info.id;
  j["kind"] = info.kind;
  j["version"] = info.version;
  j["description"] = info.description;
  j["author"] = info.author;
  j["homepage"] = info.homepage;
  j["entry"] = info.entry;
  j["apiVersion"] = info.apiVersion;
  j["depends"] = toJsonArray(info.depends);
  j["optionalDepends"] = toJsonArray(info.optionalDepends);
  j["permissions"] = toJsonArray(info.permissions);
  j["platforms"] = toJsonArray(info.platforms);
  j["arch"] = toJsonArray(info.arch);
  // JS插件的脚本清单（按顺序执行；空 = 用 entry / plugin.js）
  if (info.kind == "js") {
    j["scripts"] = toJsonArray(info.scripts);
  }
  // 多目标布局信息（见 host_target.h）：target 为空串 = 用插件根目录的库文件；
  // targets 含未匹配目标环境的分支
  if (info.kind == "native") {
    j["targetsDir"] = info.targets.dirName;
    j["target"] = info.targets.selectedTag;
    j["targetEntry"] = info.targets.selectedLib;
    Json branches = Json::array();
    for (const auto &branch : info.targets.all) {
      Json node;
      node["tag"] = branch.tag;
      node["dir"] = branch.dir;
      node["lib"] = branch.lib;
      node["os"] = branch.os;
      node["arch"] = branch.arch;
      node["universal"] = branch.universal;
      /// 与目标环境的匹配等级: 3 系统+架构 / 2 系统 / 1 架构 / 0 通用 / -1 不匹配
      node["match"] = branch.matchLevel;
      node["selected"] = branch.selected;
      branches.push_back(std::move(node));
    }
    j["targets"] = std::move(branches);
  }
  // 静态可用性（不含运行期开关：JS 运行时是否可用、安全模式、禁用动态库由调用方叠加）
  j["supported"] = info.supported;
  j["reason"] = info.reason;
  // 形态（kind）的事实：由谁提供、要不要按 C ABI 判 api_version、提供者要不校验
  j["kindKnown"] = info.kindKnown;
  j["kindRequiresPluginAbi"] = info.kindRequiresPluginAbi;
  // 这个形态的接口代次对：当前代次（上报给插件）/ 兼容下限（判 api_version 用）
  j["kindInterfaceVersion"] = info.kindInterfaceVersion;
  j["kindMinInterfaceVersion"] = info.kindMinInterfaceVersion;
  j["kindProvider"] = info.kindProvider;
  j["kindProviderId"] = info.kindProviderId;
  j["kindValidateCapability"] = info.kindValidateCapability;
  return j.dump();
}

MusicxxPluginInspect inspectPluginDir(const fs::path &pluginDir,
                                      const std::string &os,
                                      const std::string &arch,
                                      const MusicxxPluginKindRegistry *kinds) {
  MusicxxPluginInspect info;
  info.dir = pluginDir.string();
  info.dirName = pluginDir.filename().string();
  // 目标环境：`os` / `arch` 传空 = 用插件框架当前环境（C ABI `plugin_inspect` 的约定，
  // 主程序安装预检就不传）。**这一步不能省**：清单声明了 `platforms` 的插件一旦
  // 拿空串去比对，会被判成"当前平台不在清单声明内"，表现为"这类插件永远装不上"。
  const std::string targetOs = resolveTargetOs(os);
  const std::string targetArch = resolveTargetArch(arch);

  std::error_code ec;
  if (!fs::is_directory(pluginDir, ec)) {
    info.valid = false;
    info.error = "插件目录不存在";
    return info;
  }

  // 插件框架解析 name/entry/depends（权威路径），本框架补自己的扩展字段
  std::string name;
  std::string entry;
  std::vector<std::string> depends;
  std::vector<std::string> optionalDepends;
  const bool manifestOk = pluginxx::parsePluginManifest(
      pluginDir, name, entry, depends, optionalDepends, nullptr, nullptr);
  if (!manifestOk || name.empty()) {
    info.valid = false;
    info.error = "plugin.yaml 解析失败 (缺少 name 或 YAML 非法)";
    return info;
  }
  const MusicxxManifestFields fields = readManifestFields(pluginDir);
  info.valid = true;
  info.id = name;
  info.entry = entry;
  info.kind = kindOf(fields, entry);
  info.version = fields.version;
  info.description = fields.description;
  info.author = fields.author;
  info.homepage = fields.homepage;
  info.apiVersion = fields.apiVersion;
  info.depends = depends;
  info.optionalDepends = optionalDepends;
  info.permissions = fields.permissions;
  info.platforms = fields.platforms;
  info.arch = fields.arch;

  // 形态（kind）查注册表：内置 native / js 由插件框架自带，其余由运行器插件登记。
  // - 清单既没写 kind 又没写 entry 时 kind 为空：按原有的 native 路径报
  //   “入口缺失”，不要报成“没有运行器支持空形态”；
  // - 未登记 → 现在跑不了（原因直接告诉用户要先装运行器）；
  // - 只有要求 C ABI 的形态才检查 api_version（解释执行的插件不调 C 接口表）；
  // - 多目标分支只对要求 C ABI 的形态（动态库）有意义。
  const std::string kindForRegistry =
      info.kind.empty() ? std::string{"native"} : info.kind;
  const MusicxxPluginKindSpec *kindSpec =
      kinds ? kinds->find(kindForRegistry) : nullptr;
  const bool kindKnown = (kinds == nullptr) || (kindSpec != nullptr);
  const bool requiresCAbi =
      kindSpec ? kindSpec->requiresPluginAbi : true;
  // 清单 `api_version` 的判定线由**这个形态**自己给：native 是 C 接口下限（3），
  // js 是 JS 接口下限（1），运行器登记的形态写多少就多少（0 = 不判）。
  const int32_t minInterfaceVersion =
      kindSpec ? kindSpec->minInterfaceVersion
               : MUSICXX_PLUGINXX_MIN_API_VERSION;
  // 形态事实一起报给调用方：主程序据此展示“这个形态的接口代次”与
  // “要不要请提供者校验”，不在自己的代码里写死形态名。
  info.kindKnown = kindKnown;
  info.kindRequiresPluginAbi = requiresCAbi;
  info.kindMinInterfaceVersion = minInterfaceVersion;
  info.kindInterfaceVersion =
      (kindSpec && kindSpec->interfaceVersion > 0) ? kindSpec->interfaceVersion
                                                  : MUSICXX_PLUGIN_API_VERSION;
  if (kindSpec && !kindSpec->frameworkOwned()) {
    info.kindProvider = kindSpec->provider;
    info.kindProviderId = naming::pluginIdOfInstance(kindSpec->provider);
  }
  info.kindValidateCapability =
      kindSpec ? kindSpec->validateCapability : std::string{};

  // JS 形态的脚本清单（空 = 用 entry / plugin.js）——管理页据此展示；
  // 其它解释执行的形态（例如以后的 Python）脚本怎么找由运行器自己定义。
  if (info.kind == "js") {
    info.scripts = scriptsOf(fields, entry);
  }

  // 多目标打包（仿 APK 的 lib/<系统>-<架构>/）：解析包内分支，选出目标环境要用的库文件
  if (requiresCAbi) {
    info.targets = resolvePluginTargets(pluginDir, fields.targetsDir, entry, name,
                                        targetOs, targetArch, true);
  }

  if (!kindKnown) {
    info.supported = false;
    info.reason = "没有运行器声明支持插件形态 `" + info.kind +
                  "` (先安装并启用提供这个形态的插件)";
  }

  // 静态可用性（不含运行期开关：JS 运行时是否可用、安全模式、禁用动态库由调用方叠加）
  if (info.supported && !info.platforms.empty() &&
      !declaresTarget(info.platforms, targetOs, normalizeTargetOs)) {
    info.supported = false;
    info.reason = "当前平台不在清单声明内 (当前 " + targetOs +
                  "，清单声明: " + joinText(info.platforms) + ")";
  }
  if (info.supported && !info.arch.empty() &&
      !declaresTarget(info.arch, targetArch, normalizeTargetArch)) {
    info.supported = false;
    info.reason = "当前架构不在清单声明内 (当前 " + targetArch +
                  "，清单声明: " + joinText(info.arch) + ")";
  }
  if (info.supported && kindKnown && minInterfaceVersion > 0 &&
      info.apiVersion < minInterfaceVersion) {
    info.supported = false;
    info.reason = "插件声明的 API 版本低于这个形态支持的最低版本 (最低 " +
                  std::to_string(minInterfaceVersion) + ", 插件声明 " +
                  std::to_string(info.apiVersion) + ")";
  }
  if (info.supported && info.kind == "native" &&
      info.targets.selectedLib.empty()) {
    info.supported = false;
    info.reason = info.targets.note.empty()
                      ? ("动态库插件库文件缺失 (entry=" + entry + ")")
                      : info.targets.note;
  }
  if (info.supported && info.kind == "js") {
    std::string missing;
    for (const std::string &script : info.scripts) {
      if (!fs::is_regular_file(pluginDir / script, ec)) {
        missing = script;
        break;
      }
    }
    if (!missing.empty()) {
      info.supported = false;
      info.reason = "JS插件缺少脚本文件: " + missing;
    }
  }
  return info;
}

} // namespace extern_plugin
} // namespace musicxx
