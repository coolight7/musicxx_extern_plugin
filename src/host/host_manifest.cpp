/// 插件清单（`plugin.yaml`）读取实现（约定见 host_manifest.h）

#include "host_manifest.h"

#include "utilxx_base/log.h"
#include "yaml-cpp/yaml.h"

#include <fstream>
#include <sstream>

namespace musicxx {
namespace extern_plugin {

namespace {

std::string readFileText(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

/// YAML 标量 → 字符串（非标量/缺失时保持原值）
void readScalar(const YAML::Node &node, const char *key, std::string &dst) {
  if (node[key] && node[key].IsScalar()) {
    dst = node[key].as<std::string>();
  }
}

/// YAML 列表（或单个标量）→ 字符串数组
void readList(const YAML::Node &node, const char *key,
              std::vector<std::string> &dst) {
  if (!node[key]) {
    return;
  }
  if (node[key].IsSequence()) {
    for (const auto &item : node[key]) {
      if (item.IsScalar()) {
        const std::string value = item.as<std::string>();
        if (!value.empty()) {
          dst.push_back(value);
        }
      }
    }
  } else if (node[key].IsScalar()) {
    const std::string value = node[key].as<std::string>();
    if (!value.empty()) {
      dst.push_back(value);
    }
  }
}

} // namespace

MusicxxManifestFields readManifestFields(const std::filesystem::path &pluginDir) {
  MusicxxManifestFields fields;
  const std::string text = readFileText(pluginDir / "plugin.yaml");
  if (text.empty()) {
    return fields; ///< yamlOk 保持 false（缺文件或空文件）
  }
  try {
    const YAML::Node node = YAML::Load(text);
    if (!node || !node.IsMap()) {
      return fields;
    }
    readScalar(node, "kind", fields.kind);
    readScalar(node, "version", fields.version);
    readScalar(node, "description", fields.description);
    readScalar(node, "author", fields.author);
    readScalar(node, "homepage", fields.homepage);
    if (node["api_version"] && node["api_version"].IsScalar()) {
      fields.apiVersion = node["api_version"].as<int32_t>();
    }
    readList(node, "scripts", fields.scripts);
    readList(node, "permissions", fields.permissions);
    readList(node, "platforms", fields.platforms);
    readList(node, "arch", fields.arch);
    if (node["targets_dir"]) {
      if (node["targets_dir"].IsScalar()) {
        // 显式写空串 = 关闭分支扫描（插件自己管目录布局）
        fields.targetsDir = node["targets_dir"].as<std::string>();
      } else if (node["targets_dir"].IsNull()) {
        fields.targetsDir.clear();
      }
    }
    fields.yamlOk = true;
  } catch (const std::exception &e) {
    XX_LOGW("[musicxx_ext] 读取插件 `{}` 的清单失败: {}",
            pluginDir.string(), e.what());
  }
  return fields;
}

void readManifestDepends(const std::filesystem::path &pluginDir,
                         std::vector<std::string> &depends,
                         std::vector<std::string> &optionalDepends) {
  const std::string text = readFileText(pluginDir / "plugin.yaml");
  if (text.empty()) {
    return;
  }
  try {
    const YAML::Node node = YAML::Load(text);
    if (!node || !node.IsMap()) {
      return;
    }
    readList(node, "depends", depends);
    readList(node, "optional_depends", optionalDepends);
  } catch (const std::exception &e) {
    XX_LOGW("[musicxx_ext] 读取插件 `{}` 的依赖声明失败: {}",
            pluginDir.string(), e.what());
  }
}

} // namespace extern_plugin
} // namespace musicxx
