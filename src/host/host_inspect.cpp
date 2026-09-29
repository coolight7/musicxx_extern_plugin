/// 插件目录的静态判定（约定见 host_inspect.h）

#include "host_inspect.h"

#include "host_manifest.h"

#include "musicxx/plugin/api/plugin_api.h"
#include "pluginxx/host/manifest.h"
#include "utilxx_base/json.h"

#include <algorithm>
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

/// JS 插件要执行的脚本清单（清单 `scripts` 优先；否则用 entry 里的 .js；再否则 plugin.js）
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

bool listContains(const std::vector<std::string> &values,
                  const std::string &value) {
  return std::find(values.begin(), values.end(), value) != values.end();
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
  // JS 插件的脚本清单（按顺序执行；空 = 用 entry / plugin.js）
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
  return j.dump();
}

MusicxxPluginInspect inspectPluginDir(const fs::path &pluginDir,
                                      const std::string &os,
                                      const std::string &arch) {
  MusicxxPluginInspect info;
  info.dir = pluginDir.string();
  info.dirName = pluginDir.filename().string();

  std::error_code ec;
  if (!fs::is_directory(pluginDir, ec)) {
    info.valid = false;
    info.error = "插件目录不存在";
    return info;
  }

  // 内核解析 name/entry/depends（权威路径），本宿主补自己的扩展字段
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
  if (info.kind == "js") {
    info.scripts = scriptsOf(fields, entry);
  }

  // 多目标打包（仿 APK 的 lib/<系统>-<架构>/）：解析包内分支，选出目标环境要用的库文件
  if (info.kind == "native") {
    info.targets = resolvePluginTargets(pluginDir, fields.targetsDir, entry, name,
                                        os, arch, true);
  }

  // 静态可用性（不含运行期开关：JS 运行时是否可用、安全模式、禁用动态库由调用方叠加）
  if (!info.platforms.empty() && !listContains(info.platforms, os)) {
    info.supported = false;
    info.reason = "当前平台不在清单声明内";
  }
  if (info.supported && !info.arch.empty() && !listContains(info.arch, arch)) {
    info.supported = false;
    info.reason = "当前架构不在清单声明内";
  }
  if (info.supported && info.apiVersion < MUSICXX_PLUGINXX_MIN_API_VERSION) {
    info.supported = false;
    info.reason = "插件声明的 API 版本低于当前宿主支持的最低版本 (最低 " +
                  std::to_string(MUSICXX_PLUGINXX_MIN_API_VERSION) + ", 插件声明 " +
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
      info.reason = "JS 插件缺少脚本文件: " + missing;
    }
  }
  return info;
}

} // namespace extern_plugin
} // namespace musicxx
