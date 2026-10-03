/// 外部插件多目标打包支持 (实现; 约定见 host_target.h)

#include "host_target.h"

#include "pluginxx/host/manifest.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <cctype>
#include <system_error>

namespace musicxx {
namespace extern_plugin {

namespace {

namespace fs = std::filesystem;

/// 名字规范化: 转小写并去掉分隔符
///
/// 这样 `windows-x64` / `windows_x64` / `Windows.X64` 会被当成同一个标签; 标签里
/// 的分隔符只影响书写习惯, 不参与匹配。
std::string foldName(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) {
    if (ch == '-' || ch == '_' || ch == '.' || ch == ' ' || ch == '\t') {
      continue;
    }
    out.push_back(static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch))));
  }
  return out;
}

/// 别名 → 规范名 (alias 已按 foldName 写成小写无分隔符形式)
struct NameAlias {
  const char *alias;
  const char *canon;
};

/// 系统别名: 覆盖各生态里的常见写法 (win32 / osx / darwin / harmonyos ...)
constexpr NameAlias kOsAliases[] = {
    {"windows", "windows"},   {"win", "windows"},       {"win32", "windows"},
    {"win64", "windows"},     {"windowsnt", "windows"}, {"msvc", "windows"},
    {"linux", "linux"},       {"gnu", "linux"},         {"gnulinux", "linux"},
    {"macos", "macos"},       {"mac", "macos"},         {"macosx", "macos"},
    {"osx", "macos"},         {"darwin", "macos"},      {"apple", "macos"},
    {"android", "android"},   {"androideabi", "android"},
    {"ios", "ios"},           {"iphoneos", "ios"},      {"ipados", "ios"},
    {"ohos", "ohos"},         {"harmonyos", "ohos"},    {"harmony", "ohos"},
    {"fuchsia", "fuchsia"},
};

/// 架构别名: x64 / x86 / arm64 / armv7 / riscv64 / loongarch64
///
/// 说明: `arm` 与 `arm32` 按 32 位 ARM (armv7) 处理, 因为 64 位 ARM 有独立的
/// `arm64` / `aarch64` 写法。
constexpr NameAlias kArchAliases[] = {
    {"x64", "x64"},         {"x8664", "x64"},        {"amd64", "x64"},
    {"intel64", "x64"},     {"x86", "x86"},          {"i386", "x86"},
    {"i486", "x86"},        {"i586", "x86"},         {"i686", "x86"},
    {"ia32", "x86"},        {"x8632", "x86"},        {"386", "x86"},
    {"486", "x86"},         {"586", "x86"},          {"686", "x86"},
    {"arm64", "arm64"},     {"aarch64", "arm64"},    {"arm64v8", "arm64"},
    {"arm64v8a", "arm64"},  {"armv8", "arm64"},      {"armv8a", "arm64"},
    {"arm64e", "arm64"},    {"armv8l", "arm64"},
    {"arm", "armv7"},       {"arm32", "armv7"},      {"armv7", "armv7"},
    {"armv7a", "armv7"},    {"armv7l", "armv7"},     {"armeabi", "armv7"},
    {"armeabiv7", "armv7"}, {"armeabiv7a", "armv7"},
    {"riscv64", "riscv64"}, {"rv64", "riscv64"},     {"riscv", "riscv64"},
    {"loongarch64", "loongarch64"},
    {"loong64", "loongarch64"},
    {"loongarch", "loongarch64"},
};

/// 通用分支别名 (任何系统 / 架构都能用)
constexpr const char *kUniversalAliases[] = {
    "universal", "any", "all", "noarch", "generic", "common",
};

/// 在别名表里查规范名 (未命中返回空串)
template <size_t N>
std::string lookupAlias(const NameAlias (&table)[N], std::string_view folded) {
  if (folded.empty()) {
    return {};
  }
  for (const NameAlias &item : table) {
    if (folded == item.alias) {
      return item.canon;
    }
  }
  return {};
}

bool isUniversalAlias(std::string_view folded) {
  for (const char *alias : kUniversalAliases) {
    if (folded == alias) {
      return true;
    }
  }
  return false;
}

/// 库文件扩展名 (插件动态库只有这三种; 大小写不敏感比较)
bool isLibraryFileName(std::string_view name) {
  const auto lowerEndsWith = [&name](std::string_view suffix) {
    if (name.size() <= suffix.size()) {
      return false;
    }
    const size_t offset = name.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
      const char a = static_cast<char>(std::tolower(
          static_cast<unsigned char>(name[offset + i])));
      if (a != suffix[i]) {
        return false;
      }
    }
    return true;
  };
  // 注意: `libfoo.so.1` 这类带版本号的库不算 (内核与宿主都按 `*.so` 找)
  return lowerEndsWith(".dll") || lowerEndsWith(".so") || lowerEndsWith(".dylib");
}

/// 在某个目录里找插件动态库 (顺序见 host_target.h 顶部说明)
std::string findLibraryInDir(const fs::path &dir, const std::string &entry,
                             const std::string &pluginName) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    return {};
  }
  // 1) 清单 entry: 交给内核的解析规则 (平台修正扩展名 + 配置子目录回退),
  //    再退一步按清单原文找一次 (entry 里带子目录时用得上)
  if (!entry.empty()) {
    const std::string resolved = pluginxx::resolvePluginEntryPath(dir, entry);
    if (!resolved.empty() && fs::is_regular_file(resolved, ec)) {
      return resolved;
    }
    const fs::path plain = dir / entry;
    if (fs::is_regular_file(plain, ec)) {
      return plain.string();
    }
  }
  // 2) 平台默认库名 (lib<名>.so / <名>.dll / lib<名>.dylib)
  if (!pluginName.empty()) {
    const fs::path fallback = dir / defaultPluginLibraryName(pluginName);
    if (fs::is_regular_file(fallback, ec)) {
      return fallback.string();
    }
  }
  // 3) 目录里只有一个动态库文件时直接用它 (插件作者不用关心文件叫什么;
  //    有多个说明分不清哪个是入口, 交给清单 entry 明确指定)
  std::string only;
  for (const auto &item : fs::directory_iterator(dir, ec)) {
    if (ec) {
      break;
    }
    std::error_code itemEc;
    if (!item.is_regular_file(itemEc)) {
      continue;
    }
    const std::string fileName = item.path().filename().string();
    if (fileName.empty() || fileName.front() == '.') {
      continue;
    }
    if (!isLibraryFileName(fileName)) {
      continue;
    }
    if (!only.empty()) {
      return {}; ///< 多于一个: 不猜
    }
    only = item.path().string();
  }
  return only;
}

/// 相对插件目录的路径文本 (根目录时为空串; 统一用 `/` 便于跨平台展示)
std::string relativeText(const fs::path &pluginDir, const fs::path &path) {
  if (path.empty()) {
    return {};
  }
  std::error_code ec;
  const fs::path rel = fs::relative(path, pluginDir, ec);
  if (ec || rel.empty()) {
    return path.generic_string();
  }
  return rel.generic_string();
}

} // namespace

std::string normalizeTargetOs(std::string_view name) {
  return lookupAlias(kOsAliases, foldName(name));
}

std::string normalizeTargetArch(std::string_view name) {
  return lookupAlias(kArchAliases, foldName(name));
}

std::string currentTargetOs() {
#if XX_IS_WIN_D
  return "windows";
#elif XX_IS_ANDROID_D
  return "android";
#elif XX_IS_IOS_D
  return "ios";
#elif XX_IS_MACOS_D
  return "macos";
#elif XX_IS_LINUX_D
  return "linux";
#else
  return "unknown";
#endif
}

std::string currentTargetArch() {
  // 与宿主上报给插件的 hostArch() 一致 (x64 / arm64 / x86)
#if defined(_M_ARM64) || defined(__aarch64__)
  return "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
  return "x64";
#elif defined(_M_IX86) || defined(__i386__)
  return "x86";
#else
  return "unknown";
#endif
}

MusicxxTargetTag parseTargetTag(std::string_view tag) {
  MusicxxTargetTag out;
  const std::string folded = foldName(tag);
  if (folded.empty()) {
    return out;
  }
  // 整体就是一个名字: 通用 / 系统 / 架构
  if (isUniversalAlias(folded)) {
    out.recognized = true;
    out.universal = true;
    return out;
  }
  if (const std::string os = lookupAlias(kOsAliases, folded); !os.empty()) {
    out.recognized = true;
    out.os = os;
    return out;
  }
  if (const std::string arch = lookupAlias(kArchAliases, folded);
      !arch.empty()) {
    out.recognized = true;
    out.arch = arch;
    return out;
  }
  // 组合标签: 在折名后的文本上找一个切分点, 要求两边分别是"系统"与"架构"
  // (两种顺序都接受: windows-x64 / x64-windows)
  for (size_t split = 1; split < folded.size(); ++split) {
    const std::string_view left{folded.data(), split};
    const std::string_view right{folded.data() + split, folded.size() - split};
    const std::string leftOs = lookupAlias(kOsAliases, left);
    const std::string rightArch = lookupAlias(kArchAliases, right);
    if (!leftOs.empty() && !rightArch.empty()) {
      out.recognized = true;
      out.os = leftOs;
      out.arch = rightArch;
      return out;
    }
    const std::string leftArch = lookupAlias(kArchAliases, left);
    const std::string rightOs = lookupAlias(kOsAliases, right);
    if (!leftArch.empty() && !rightOs.empty()) {
      out.recognized = true;
      out.os = rightOs;
      out.arch = leftArch;
      return out;
    }
  }
  return out;
}

std::string defaultPluginLibraryName(std::string_view pluginName) {
  // 与内核 defaultPluginLibraryPath 同一规则: 只差目录前缀
#if XX_IS_WIN_D
  return std::string{pluginName} + ".dll";
#elif XX_IS_MACOS_D || XX_IS_IOS_D
  return "lib" + std::string{pluginName} + ".dylib";
#else
  return "lib" + std::string{pluginName} + ".so";
#endif
}

MusicxxPluginTargets
resolvePluginTargets(const fs::path &pluginDir, const std::string &targetsDir,
                     const std::string &entry, const std::string &pluginName,
                     const std::string &os, const std::string &arch,
                     bool scanRootFallback) {
  MusicxxPluginTargets result;
  result.dirName = targetsDir;

  const fs::path rootDir = pluginDir;
  const std::string currentOs = os.empty() ? currentTargetOs() : os;
  const std::string currentArch = arch.empty() ? currentTargetArch() : arch;

  // 1) 收集分支目录 (只认能解析出来的标签; 其它目录当普通资源目录忽略)
  if (!targetsDir.empty()) {
    std::error_code ec;
    const fs::path branchRoot = pluginDir / targetsDir;
    if (fs::is_directory(branchRoot, ec)) {
      for (const auto &item : fs::directory_iterator(branchRoot, ec)) {
        if (ec) {
          break;
        }
        std::error_code itemEc;
        if (!item.is_directory(itemEc)) {
          continue;
        }
        const std::string tagName = item.path().filename().string();
        const MusicxxTargetTag tag = parseTargetTag(tagName);
        if (!tag.recognized) {
          continue;
        }
        MusicxxPluginTarget target;
        target.tag = tagName;
        target.dir = targetsDir + "/" + tagName;
        target.os = tag.os;
        target.arch = tag.arch;
        target.universal = tag.universal;
        // 匹配等级: 声明的系统/架构都必须与当前环境一致, 否则不可用
        const bool osOk = tag.os.empty() || tag.os == currentOs;
        const bool archOk = tag.arch.empty() || tag.arch == currentArch;
        if (!osOk || !archOk) {
          target.matchLevel = -1;
        } else if (!tag.os.empty() && !tag.arch.empty()) {
          target.matchLevel = 3;
        } else if (!tag.os.empty()) {
          target.matchLevel = 2;
        } else if (!tag.arch.empty()) {
          target.matchLevel = 1;
        } else {
          target.matchLevel = 0;
        }
        const std::string lib =
            findLibraryInDir(item.path(), entry, pluginName);
        if (!lib.empty()) {
          target.libPath = lib;
          target.lib = relativeText(pluginDir, lib);
        }
        result.all.push_back(std::move(target));
      }
    }
  }
  result.hasBranches = !result.all.empty();

  // 2) 排序: 匹配等级高的在前; 同级按标签名排序 (同名标签只可能是大小写差异)
  std::sort(result.all.begin(), result.all.end(),
            [](const MusicxxPluginTarget &a, const MusicxxPluginTarget &b) {
              if (a.matchLevel != b.matchLevel) {
                return a.matchLevel > b.matchLevel;
              }
              return a.tag < b.tag;
            });

  // 3) 选分支: 先选"匹配且有库文件"的; 都没有库文件时保留最高优先级的匹配分支,
  //    好让调用方给出准确原因 (分支在, 但里面没库文件)
  for (auto &target : result.all) {
    if (target.matchLevel < 0 || target.libPath.empty()) {
      continue;
    }
    target.selected = true;
    result.selectedTag = target.tag;
    result.selectedLib = target.lib;
    result.selectedLibPath = target.libPath;
    return result;
  }
  for (auto &target : result.all) {
    if (target.matchLevel < 0) {
      continue;
    }
    target.selected = true;
    result.selectedTag = target.tag;
    result.note = "分支 `" + target.dir + "` 里没有可加载的库文件";
    return result;
  }

  // 4) 没有匹配分支: 回退插件根目录 (旧布局 / 隐式通用分支)
  if (scanRootFallback) {
    const std::string rootLib = findLibraryInDir(rootDir, entry, pluginName);
    if (!rootLib.empty()) {
      result.selectedLib = relativeText(pluginDir, rootLib);
      result.selectedLibPath = rootLib;
      if (result.hasBranches) {
        result.note = "包内没有匹配当前系统/架构的分支, 已回退插件根目录";
        XX_LOGW("[musicxx_ext] 插件 `{}` 的分支都不匹配当前环境 ({}/{}), "
                "回退插件根目录; 包内分支: {}",
                pluginName, currentOs, currentArch,
                describePluginTargets(result));
      }
      return result;
    }
  }
  if (result.hasBranches) {
    result.note = "包内没有匹配当前系统/架构的分支 (当前 " + currentOs + "/" +
                  currentArch + ", 包内分支: " + describePluginTargets(result) +
                  ")";
  }
  return result;
}

std::string describePluginTargets(const MusicxxPluginTargets &targets) {
  std::string text;
  for (const MusicxxPluginTarget &target : targets.all) {
    if (!text.empty()) {
      text += "、";
    }
    text += target.tag;
  }
  return text;
}

} // namespace extern_plugin
} // namespace musicxx
