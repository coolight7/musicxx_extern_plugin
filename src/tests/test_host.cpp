/// musicxx 外部插件宿主原生测试 (不依赖 Dart)
///
/// 覆盖:
/// - test_host_lifecycle: create/start/stop/destroy 幂等;
/// - test_plugin_load: 扫描 + 装载示例插件 + 钩子注册生效;
/// - test_hooks: decision 钩子裁决 (skip) / 观察钩子 / 未命中时零处理器;
/// - test_namespace: 未知钩子注册被拒 (由宿主实现保证, 这里校验已注册集合);
/// - 卸载后钩子注册全部摘除 (无残留)。
///
/// 运行: musicxx_extern_plugin_test <example_native 插件目录>

#include "musicxx_extern_plugin_api.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failed = 0;
int g_checks = 0;

void check(bool ok, const char *what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("  [FAIL] %s\n", what);
  } else {
    std::printf("  [ ok ] %s\n", what);
  }
}

MusicxxExternPluginStringView view(const std::string &s) {
  MusicxxExternPluginStringView v{};
  v.data = s.data();
  v.size = s.size();
  return v;
}

/// 指针版视图 (C ABI 入参形态)
///
/// 注意: 槽位是滚动复用的静态数组, 因此**一次调用里的视图个数必须 ≤ 槽位数**
/// (C++ 不规定实参求值顺序, 槽位太少会让后写入的值覆盖先写入的视图)。
MusicxxExternPluginStringView *viewP(const std::string &s) {
  static MusicxxExternPluginStringView slots[8];
  static int next = 0;
  auto &v = slots[next++ % 8];
  v.data = s.data();
  v.size = s.size();
  return &v;
}

MusicxxExternPluginStringView viewC(const char *s) {
  MusicxxExternPluginStringView v{};
  v.data = s;
  v.size = std::strlen(s);
  return v;
}

MusicxxExternPluginStringView *viewCP(const char *s) {
  static MusicxxExternPluginStringView slots[8];
  static int next = 0;
  auto &v = slots[next++ % 8];
  v.data = s;
  v.size = std::strlen(s);
  return &v;
}

void freeStr(MusicxxExternPluginString &s) {
  musicxx_extern_plugin_string_free(&s);
}

std::string take(MusicxxExternPluginString &s) {
  std::string out;
  if (s.data) {
    out.assign(s.data, static_cast<size_t>(s.size));
  }
  freeStr(s);
  return out;
}

/// 从扫描结果 JSON (扁平数组) 中取出指定插件 id 的片段 (到下一个条目的 "id"
/// 之前)
///
/// 用途: 断言某个插件的 `valid`/`supported` 字段 ——
/// 入口文件按平台解析后的结果是否正确 (清单按 Linux 写 entry, Windows/macOS
/// 由内核修正扩展名)。
std::string scanItemOf(const std::string &scanJson, const std::string &id) {
  const std::string key = "\"id\":\"" + id + "\"";
  const auto pos = scanJson.find(key);
  if (pos == std::string::npos) {
    return {};
  }
  const auto next = scanJson.find("\"id\":\"", pos + key.size());
  return scanJson.substr(pos, next == std::string::npos ? std::string::npos
                                                        : next - pos);
}

/// 取 JSON 字符串字段的裸值 (测试专用极简解析; 不支持转义)
std::string jsonStringField(const std::string &json, const std::string &key) {
  const std::string needle = "\"" + key + "\":\"";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) {
    return {};
  }
  const auto begin = pos + needle.size();
  const auto end = json.find('"', begin);
  if (end == std::string::npos) {
    return {};
  }
  return json.substr(begin, end - begin);
}

/// 取 JSON 整数字段的裸文本 (测试专用)
std::string jsonIntField(const std::string &json, const std::string &key) {
  const std::string needle = "\"" + key + "\":";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) {
    return {};
  }
  auto begin = pos + needle.size();
  auto end = begin;
  while (end < json.size() &&
         (std::isdigit(static_cast<unsigned char>(json[end])) ||
          json[end] == '-')) {
    ++end;
  }
  return json.substr(begin, end - begin);
}

/// 取视图里第一个形如 `1x` / `0.5x` 的文本 (测试专用)
///
/// 设置页里「背景动画速率」那一行的右侧状态就是这个形态。列表块移除后，行由
/// `Block`(内容块) 与布局块组合出来，没有 `right` 字段可读，因此按值的形态取
/// （与 Dart 侧端到端用例 `_rowOf` 同一个目的：读到那一行显示的状态文字）。
std::string jsonFirstRateText(const std::string &json) {
  const std::string needle = "\"text\":\"";
  size_t pos = 0;
  while ((pos = json.find(needle, pos)) != std::string::npos) {
    const size_t begin = pos + needle.size();
    const size_t end = json.find('"', begin);
    if (end == std::string::npos) {
      return {};
    }
    const std::string value = json.substr(begin, end - begin);
    pos = end;
    size_t i = 0;
    bool digits = false;
    while (i < value.size() &&
           std::isdigit(static_cast<unsigned char>(value[i]))) {
      ++i;
      digits = true;
    }
    if (digits && i < value.size() && value[i] == '.') {
      ++i;
      while (i < value.size() &&
             std::isdigit(static_cast<unsigned char>(value[i]))) {
        ++i;
      }
    }
    if (digits && i + 1 == value.size() && value[i] == 'x') {
      return value;
    }
  }
  return {};
}

/* ==================== 多目标打包（分支选择）测试辅助 ==================== */

/// 与 `check` 同义，但允许拼出带变量的说明文本
void checkText(bool ok, const std::string &what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("  [FAIL] %s\n", what.c_str());
  } else {
    std::printf("  [ ok ] %s\n", what.c_str());
  }
}

/// 当前编译目标的规范系统名（与宿主上报的平台标识同规则）
std::string testPlatform() {
#if defined(__ANDROID__)
  return "android";
#elif defined(_WIN32)
  return "windows";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__linux__)
  return "linux";
#else
  return "unknown";
#endif
}

/// 当前编译目标的规范架构名（与宿主 `currentTargetArch()` / `hostArch()` 同规则）
std::string testArch() {
#if defined(_M_ARM64) || defined(__aarch64__)
  return "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
  return "x64";
#elif defined(_M_ARM) || defined(__arm__)
  return "armv7"; ///< 32 位 ARM (Android armeabi-v7a / Linux armhf)
#elif defined(_M_IX86) || defined(__i386__)
  return "x86";
#elif defined(__riscv) && (__riscv_xlen == 64)
  return "riscv64";
#elif defined(__loongarch64)
  return "loongarch64";
#else
  return "unknown";
#endif
}

/// 当前系统的别名写法（清单 `platforms` 里写别名与写规范名必须等效）
std::string testOsAlias() {
  const std::string os = testPlatform();
  if (os == "windows") {
    return "win32";
  }
  if (os == "linux") {
    return "gnu";
  }
  if (os == "macos") {
    return "osx";
  }
  return os;
}

/// 当前架构的别名写法（清单 `arch` 里写别名与写规范名必须等效）
std::string testArchAlias() {
  const std::string arch = testArch();
  if (arch == "x64") {
    return "amd64";
  }
  if (arch == "arm64") {
    return "aarch64";
  }
  if (arch == "x86") {
    return "i686";
  }
  return arch;
}

/// 平台化后的库文件名（清单按 Linux 写 `<名>.so`，宿主按平台修正扩展名）
std::string testLibFileName(const std::string &base) {
#if defined(_WIN32)
  return base + ".dll";
#elif defined(__APPLE__)
  return "lib" + base + ".dylib";
#else
  return base + ".so";
#endif
}

void writeTextFile(const std::filesystem::path &path, const std::string &text) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary);
  out << text;
}

void removePath(const std::filesystem::path &path) {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
}

/// 建一个临时"插件根目录"：`<根>/<id>/plugin.yaml` + 参数里给的相对路径文件
///
/// 文件内容随便给（扫描只看文件在不在）；返回插件根目录（扫描入口）。
std::filesystem::path makeTempPluginRoot(
    const std::string &id, const std::string &manifest,
    const std::vector<std::string> &files) {
  static int counter = 0;
  std::error_code ec;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path root =
      std::filesystem::temp_directory_path(ec) /
      ("musicxx_ext_pkg_" + std::to_string(stamp) + "_" +
       std::to_string(++counter));
  const std::filesystem::path dir = root / id;
  std::filesystem::create_directories(dir, ec);
  writeTextFile(dir / "plugin.yaml", manifest);
  for (const std::string &relative : files) {
    writeTextFile(dir / relative, "not a real library");
  }
  return root;
}

/// JSON 字符串转义（路径里有反斜杠时也能安全放进 JSON 文本）
std::string jsonEscape(const std::string &text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char ch : text) {
    if (ch == '\\' || ch == '"') {
      out.push_back('\\');
    }
    out.push_back(ch);
  }
  return out;
}

/// 切换宿主的用户插件目录（扫描入口）并返回扫描结果里某个插件的片段
std::string scanPluginItem(MusicxxExternPluginHost *host,
                           const std::string &pluginRoot,
                           const std::string &id) {
  const std::string cfg = "{\"userPluginDir\":\"" + jsonEscape(pluginRoot) +
                          "\"}";
  MusicxxExternPluginString cfgLog{};
  musicxx_extern_plugin_set_config(host, viewP(cfg), &cfgLog);
  freeStr(cfgLog);
  MusicxxExternPluginString out{};
  MusicxxExternPluginString scanLog{};
  const auto rc = musicxx_extern_plugin_plugin_scan(host, &out, &scanLog);
  freeStr(scanLog);
  const std::string json = take(out);
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    return {};
  }
  return scanItemOf(json, id);
}

/// 取选中分支的标签（`"target":"..."`）
std::string targetOf(const std::string &item) {
  return jsonStringField(item, "target");
}

/// 多目标打包（仿 APK 的 `lib/<系统>-<架构>/`）：分支选择与装载
///
/// 用例覆盖：选择优先级（系统+架构 → 系统 → 架构 → 通用 → 根目录）、
/// 不匹配分支的报错、未知标签目录被忽略、`targets_dir` 改名、
/// "分支里唯一的动态库文件"自动识别、真实装载走分支。
void testPluginTargetLayout(MusicxxExternPluginHost *host,
                            const std::string &pluginsRoot) {
  const std::string os = testPlatform();
  const std::string arch = testArch();
  const std::string otherOs = (os == "windows") ? "linux" : "windows";
  const std::string exactTag = os + "-" + arch;
  const std::string osTag = os;
  const std::string archTag = arch;
  const std::string otherTag = otherOs + "-" + arch;
  const std::string libName = testLibFileName("tgt_multi");
  const std::string manifest =
      "name: tgt_multi\n"
      "entry: tgt_multi.so\n"
      "kind: native\n"
      "version: 0.1.0\n";

  // 1) 选择优先级：包内同时放 不匹配 / 系统+架构 / 只系统 / 只架构 / 通用，
  //    以及一个不是分支的目录与根目录库文件
  const std::filesystem::path root = makeTempPluginRoot(
      "tgt_multi", manifest,
      {"lib/" + otherTag + "/" + libName, "lib/" + exactTag + "/" + libName,
       "lib/" + osTag + "/" + libName, "lib/" + archTag + "/" + libName,
       "lib/universal/" + libName, "lib/notatag/" + libName,
       testLibFileName("tgt_multi")});
  const std::filesystem::path pkg = root / "tgt_multi";
  {
    const std::string item = scanPluginItem(host, root.string(), "tgt_multi");
    checkText(!item.empty(), "多目标包被扫描到");
    checkText(item.find("\"supported\":true") != std::string::npos,
              "多目标包（有匹配分支）判定为可用");
    checkText(targetOf(item) == exactTag,
              "优先选系统+架构分支 (" + exactTag + ")");
    checkText(item.find("\"targetEntry\":\"lib/" + exactTag + "/" + libName +
                            "\"") != std::string::npos,
              "targetEntry 指向分支内的库文件");
    checkText(item.find("\"targets\":[") != std::string::npos &&
                  item.find("\"notatag\"") == std::string::npos,
              "unknown 标签目录不算分支");
    checkText(item.find("\"" + otherTag + "\"") != std::string::npos &&
                  item.find("\"match\":-1") != std::string::npos,
              "不匹配当前环境的分支照常列出（match=-1）");
  }
  // 2) 逐个删掉更高优先级的分支，验证剩下的按 只系统 → 只架构 → 通用 →
  //    根目录 的顺序接管
  removePath(pkg / "lib" / exactTag);
  {
    const std::string item = scanPluginItem(host, root.string(), "tgt_multi");
    checkText(targetOf(item) == osTag, "去掉系统+架构分支后选只系统分支");
  }
  removePath(pkg / "lib" / osTag);
  {
    const std::string item = scanPluginItem(host, root.string(), "tgt_multi");
    checkText(targetOf(item) == archTag, "再去掉只系统分支后选只架构分支");
  }
  removePath(pkg / "lib" / archTag);
  {
    const std::string item = scanPluginItem(host, root.string(), "tgt_multi");
    checkText(targetOf(item) == "universal", "再去掉只架构分支后选通用分支");
  }
  removePath(pkg / "lib" / "universal");
  {
    const std::string item = scanPluginItem(host, root.string(), "tgt_multi");
    checkText(targetOf(item).empty() && item.find("\"supported\":true") !=
                                            std::string::npos,
              "分支都不匹配时回退插件根目录的库文件");
  }
  removePath(pkg / testLibFileName("tgt_multi"));
  {
    const std::string item = scanPluginItem(host, root.string(), "tgt_multi");
    checkText(item.find("\"supported\":false") != std::string::npos,
              "包内没有匹配分支且根目录也没有库文件时判为不可用");
    checkText(item.find("包内没有匹配当前系统/架构的分支") !=
                  std::string::npos,
              "不可用原因写明没有匹配的分支, 并列出包内分支");
  }
  removePath(root);

  // 3) 清单 targets_dir 改名：lib/ 里的分支不参与，改用 targets/
  {
    const std::filesystem::path customRoot = makeTempPluginRoot(
        "tgt_custom",
        "name: tgt_custom\nentry: tgt_custom.so\nkind: native\n"
        "targets_dir: targets\n",
        {"targets/" + exactTag + "/" + libName,
         "lib/" + otherTag + "/" + libName});
    const std::string item =
        scanPluginItem(host, customRoot.string(), "tgt_custom");
    checkText(targetOf(item) == exactTag,
              "targets_dir 指定的分支目录按清单生效");
    checkText(item.find("\"targetEntry\":\"targets/" + exactTag + "/") !=
                  std::string::npos,
              "targetEntry 指向 targets_dir 里的库文件");
    removePath(customRoot);
  }

  // 4) 分支里的库文件名与清单 entry 不同：目录里只有一个动态库文件时直接用它
  {
    const std::string customLib = testLibFileName("whatever_name");
    const std::filesystem::path uniRoot = makeTempPluginRoot(
        "tgt_uni",
        "name: tgt_uni\nentry: tgt_uni.so\nkind: native\n",
        {"lib/universal/" + customLib});
    const std::string item = scanPluginItem(host, uniRoot.string(), "tgt_uni");
    checkText(targetOf(item) == "universal", "只有通用分支时选通用分支");
    checkText(item.find("\"targetEntry\":\"lib/universal/" + customLib + "\"") !=
                  std::string::npos,
              "分支里唯一的动态库文件被自动识别（名字不必与 entry 相同）");
    removePath(uniRoot);
  }

  // 5) 分支目录存在但没有库文件：报"分支里没有库文件"
  {
    const std::filesystem::path emptyRoot = makeTempPluginRoot(
        "tgt_empty", "name: tgt_empty\nentry: tgt_empty.so\nkind: native\n",
        {"lib/" + exactTag + "/readme.txt"});
    const std::string item =
        scanPluginItem(host, emptyRoot.string(), "tgt_empty");
    checkText(item.find("\"supported\":false") != std::string::npos,
              "分支里没有库文件时判为不可用");
    checkText(item.find("没有可加载的库文件") != std::string::npos,
              "不可用原因说明分支里没有库文件");
    removePath(emptyRoot);
  }

  // 6) 真实装载：把示例动态库插件复制到分支目录里，验证装载走的也是分支
  //    （装载后立即卸载，避免与后面的常规装载重名）
  {
    const std::filesystem::path nativeDir =
        std::filesystem::path(pluginsRoot) / "example_native";
    const std::filesystem::path nativeLib =
        nativeDir / testLibFileName("example_native");
    const std::filesystem::path nativeManifest = nativeDir / "plugin.yaml";
    if (!std::filesystem::exists(nativeLib) ||
        !std::filesystem::exists(nativeManifest)) {
      checkText(false, "示例动态库插件缺失 (跳过真实装载用例)");
      return;
    }
    // 清单原样复制（entry 仍是按 Linux 写的 example_native.so），库文件放进分支目录
    std::error_code ec;
    std::ifstream in(nativeManifest, std::ios::binary);
    std::ostringstream oss;
    oss << in.rdbuf();
    const std::filesystem::path loadRoot =
        makeTempPluginRoot("example_native", oss.str(), {});
    const std::filesystem::path branchDir =
        loadRoot / "example_native" / "lib" / exactTag;
    std::filesystem::create_directories(branchDir, ec);
    std::filesystem::copy_file(nativeLib, branchDir / nativeLib.filename(), ec);

    const std::string item =
        scanPluginItem(host, loadRoot.string(), "example_native");
    checkText(targetOf(item) == exactTag, "示例插件放进分支目录后按分支选中");

    MusicxxExternPluginString loadLog{};
    const auto loadRc = musicxx_extern_plugin_plugin_load_sync(
        host, viewCP("example_native"), viewCP("{}"), 10000, &loadLog);
    if (loadRc != MUSICXX_EXTERN_PLUGIN_OK) {
      std::printf("  [info] 分支装载失败 log=%s\n", take(loadLog).c_str());
      freeStr(loadLog);
    } else {
      freeStr(loadLog);
    }
    checkText(loadRc == MUSICXX_EXTERN_PLUGIN_OK, "分支里的库文件能真正装载");
    {
      MusicxxExternPluginString listOut{};
      MusicxxExternPluginString listLog{};
      musicxx_extern_plugin_plugin_list(host, &listOut, &listLog);
      freeStr(listLog);
      const std::string loadedItem =
          scanItemOf(take(listOut), "example_native");
      checkText(targetOf(loadedItem) == exactTag,
                "已加载插件报告自己用的是哪个分支");
      checkText(loadedItem.find("\"targetEntry\":\"lib/" + exactTag + "/") !=
                    std::string::npos,
                "已加载插件报告分支里的库文件");
    }
    {
      MusicxxExternPluginString unloadLog{};
      musicxx_extern_plugin_plugin_unload(host, viewCP("example_native"),
                                          &unloadLog);
      freeStr(unloadLog);
    }
    removePath(loadRoot);
    // 还原插件目录（后面的用例还要用常规目录里的插件）
    scanPluginItem(host, pluginsRoot, "example_native");
  }
}

/// 插件目录静态判定（`plugin_inspect`）：安装预检入口，与扫描/装载同源
///
/// 用例覆盖：清单字段、按当前或**指定**目标环境选分支、没有匹配分支的原因、
/// 清单 `platforms` / `arch` 的声明（含别名与 32 位 ARM）、JS 插件的脚本清单、
/// 目录不存在，以及"inspect 与 scan 对同一目录结论一致"。
void testPluginInspect(MusicxxExternPluginHost *host,
                       const std::string &pluginsRoot) {
  auto inspect = [&](const std::string &dir, const std::string &os,
                     const std::string &arch) -> std::string {
    MusicxxExternPluginString out{};
    MusicxxExternPluginString log{};
    const auto rc = musicxx_extern_plugin_plugin_inspect(
        host, viewP(dir), viewP(os), viewP(arch), &out, &log);
    freeStr(log);
    const std::string json = take(out);
    return rc == MUSICXX_EXTERN_PLUGIN_OK ? json : std::string{};
  };

  const std::string os = testPlatform();
  const std::string arch = testArch();
  const std::string exactTag = os + "-" + arch;
  const std::string libName = testLibFileName("tgt_inspect");
  const std::string manifest =
      "name: tgt_inspect\n"
      "entry: tgt_inspect.so\n"
      "kind: native\n"
      "version: 0.2.0\n"
      "author: \"probe\"\n";
  const std::filesystem::path root = makeTempPluginRoot(
      "tgt_inspect", manifest,
      {"lib/" + exactTag + "/" + libName, "lib/linux-arm64/" + libName});
  const std::filesystem::path pkg = root / "tgt_inspect";

  // 1) 用宿主当前环境判定（os/arch 传空）
  const std::string info = inspect(pkg.string(), "", "");
  checkText(!info.empty(), "plugin_inspect 有出参");
  checkText(info.find("\"valid\":true") != std::string::npos,
            "plugin_inspect 判定清单可解析");
  checkText(jsonStringField(info, "id") == "tgt_inspect",
            "plugin_inspect 给出插件 id");
  checkText(jsonStringField(info, "kind") == "native",
            "plugin_inspect 给出形态");
  checkText(jsonStringField(info, "version") == "0.2.0",
            "plugin_inspect 给出清单字段");
  checkText(info.find("\"supported\":true") != std::string::npos,
            "plugin_inspect 判定为可用");
  checkText(jsonStringField(info, "target") == exactTag,
            "plugin_inspect 按当前系统/架构选中分支");

  // 2) 与扫描同源：同一个目录，两边报的分支与库文件必须一致
  {
    const std::string scanItem =
        scanPluginItem(host, root.string(), "tgt_inspect");
    checkText(jsonStringField(scanItem, "target") ==
                  jsonStringField(info, "target"),
              "plugin_inspect 与扫描选中的分支一致");
    checkText(jsonStringField(scanItem, "targetEntry") ==
                  jsonStringField(info, "targetEntry"),
              "plugin_inspect 与扫描的库文件一致");
    checkText(jsonStringField(scanItem, "reason") ==
                  jsonStringField(info, "reason"),
              "plugin_inspect 与扫描的可用性原因一致（均没有原因）");
  }

  // 3) 指定目标环境：按传进来的 os/arch 选分支（不需要在那种机器上）
  checkText(jsonStringField(inspect(pkg.string(), "linux", "arm64"), "target") ==
                "linux-arm64",
            "plugin_inspect 按指定目标环境选分支");

  // 4) 目标环境没有匹配分支：标明不可用并给出原因
  {
    const std::string mac = inspect(pkg.string(), "macos", "arm64");
    checkText(mac.find("\"supported\":false") != std::string::npos,
              "plugin_inspect 对没有匹配分支的环境判为不可用");
    checkText(mac.find("没有匹配当前系统/架构的分支") != std::string::npos,
              "plugin_inspect 给出没有匹配分支的原因");
  }
  removePath(root);

  // 5) 清单声明了 platforms / arch：按目标环境比较（传空 = 宿主当前环境）
  //
  // 回归：应用侧安装预检（`host.plugins.inspect(dir)`）**不传** os/arch，早期实现
  // 拿空串去比对清单里的 platforms，于是声明了 platforms 的插件一律被判成
  // "当前平台不在清单声明内" —— 这类插件永远装不上（示例插件几乎都声明了 platforms）。
  {
    const std::string libName2 = testLibFileName("tgt_declared");
    const std::filesystem::path declaredRoot = makeTempPluginRoot(
        "tgt_declared",
        "name: tgt_declared\nentry: tgt_declared.so\nkind: native\n"
        "platforms: [" + testOsAlias() + "]\narch: [" + testArchAlias() + "]\n",
        {libName2});
    const std::string pkgDir = (declaredRoot / "tgt_declared").string();

    const std::string hostEnv = inspect(pkgDir, "", "");
    checkText(hostEnv.find("\"supported\":true") != std::string::npos,
              "清单声明了 platforms / arch 时，按宿主当前环境（传空）判定为可用");
    checkText(jsonStringField(hostEnv, "reason").empty(),
              "按宿主当前环境判定时没有不可用原因");

    const std::string scanItem =
        scanPluginItem(host, declaredRoot.string(), "tgt_declared");
    checkText(scanItem.find("\"supported\":true") != std::string::npos,
              "扫描对声明了别名平台/架构的插件同样判为可用");

    // 换个平台：明确报"当前平台不在清单声明内"，并带上当前环境与声明内容
    const std::string otherOs = (testPlatform() == "windows") ? "linux" : "windows";
    const std::string mismatch = inspect(pkgDir, otherOs, testArch());
    checkText(mismatch.find("\"supported\":false") != std::string::npos,
              "换成别的平台后判为不可用");
    checkText(mismatch.find("当前平台不在清单声明内") != std::string::npos &&
                  mismatch.find(otherOs) != std::string::npos,
              "不可用原因写明平台不在声明内，并给出目标环境");
    removePath(declaredRoot);
  }

  // 6) 32 位 ARM：`android-armeabi-v7a` 分支与清单里的别名写法（arm / armv7 / armeabi-v7a）
  //
  // 本用例在 x64 机上跑：目标环境由参数注入（`plugin_inspect` 的 os/arch 参数就是为这种
  // 验证准备的）。编译目标本身的架构判定（32 位 ARM 上报 armv7）由 `debug_info` 的
  // `arch` 断言覆盖（见 main 里的"宿主上报的架构"）。
  {
    const std::string armLib = testLibFileName("tgt_armv7");
    const std::filesystem::path armRoot = makeTempPluginRoot(
        "tgt_armv7",
        "name: tgt_armv7\nentry: tgt_armv7.so\nkind: native\n"
        "platforms: [android, linux]\narch: [arm]\n",
        {"lib/android-armeabi-v7a/" + armLib, "lib/android-arm64-v8a/" + armLib,
         "lib/linux-armv7/" + armLib});
    const std::string armPkg = (armRoot / "tgt_armv7").string();

    const std::string androidArm = inspect(armPkg, "android", "armv7");
    checkText(androidArm.find("\"supported\":true") != std::string::npos,
              "32 位 ARM（armv7）判定为可用");
    checkText(jsonStringField(androidArm, "target") == "android-armeabi-v7a",
              "选中 32 位 ARM 分支 android-armeabi-v7a");
    checkText(androidArm.find("\"arch\":[\"arm\"]") != std::string::npos,
              "清单 arch 写别名 arm 照常解析");

    // Linux armhf：同一份包按 linux-armv7 分支
    const std::string linuxArm = inspect(armPkg, "linux", "armv7");
    checkText(jsonStringField(linuxArm, "target") == "linux-armv7",
              "Linux 32 位 ARM 选 linux-armv7 分支");

    // 换个架构（arm64）：清单 arch 只声明了 32 位 ARM，应判为不可用并说明原因
    const std::string arm64Info = inspect(armPkg, "android", "arm64");
    checkText(arm64Info.find("\"supported\":false") != std::string::npos,
              "只声明 32 位 ARM 的插件在 arm64 环境判为不可用");
    checkText(arm64Info.find("当前架构不在清单声明内") != std::string::npos &&
                  arm64Info.find("当前 arm64") != std::string::npos,
              "架构不匹配的原因写明当前架构与清单声明");
    removePath(armRoot);
  }

  // 7) JS 插件：形态与脚本清单；动态库分支字段不出现
  {
    const std::filesystem::path jsDir =
        std::filesystem::path(pluginsRoot) / "example_js";
    const std::string jsInfo = inspect(jsDir.string(), "", "");
    checkText(jsonStringField(jsInfo, "kind") == "js",
              "plugin_inspect 识别 JS 插件");
    checkText(jsInfo.find("\"scripts\":[\"") != std::string::npos,
              "plugin_inspect 给出 JS 插件的脚本清单");
    checkText(jsInfo.find("\"targetsDir\"") == std::string::npos &&
                  jsInfo.find("\"targetEntry\"") == std::string::npos,
              "JS 插件不出现动态库分支字段");
    // 示例插件都声明了 platforms：按宿主当前环境判定必须是可用（安装预检的调用形状）
    checkText(jsInfo.find("\"supported\":true") != std::string::npos,
              "声明了 platforms 的 JS 示例插件按宿主当前环境判定为可用");
  }

  // 8) 目录不存在：valid=false + 可读原因（调用本身仍然成功）
  {
    const std::filesystem::path missing =
        std::filesystem::path(pluginsRoot) / "definitely_missing_plugin";
    const std::string missingInfo = inspect(missing.string(), "", "");
    checkText(!missingInfo.empty(), "plugin_inspect 对不存在的目录也有出参");
    checkText(missingInfo.find("\"valid\":false") != std::string::npos,
              "plugin_inspect 对不存在的目录判 valid=false");
    checkText(missingInfo.find("插件目录不存在") != std::string::npos,
              "plugin_inspect 说明目录不存在");
  }

  // 还原插件目录（本用例中途把扫描入口指向了临时包，后面的用例要用常规目录里的插件）
  scanPluginItem(host, pluginsRoot, "example_native");
}

} // namespace

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0); // debug: unbuffered stdout
  const std::string pluginDir = (argc > 1) ? argv[1] : "";
  std::printf("musicxx_extern_plugin native test (pluginDir=%s)\n",
              pluginDir.c_str());

  check(musicxx_extern_plugin_api_version() ==
            MUSICXX_EXTERN_PLUGIN_API_VERSION,
        "api_version 匹配");

  MusicxxExternPluginString log{};
  MusicxxExternPluginString version{};
  check(musicxx_extern_plugin_version(&version) == MUSICXX_EXTERN_PLUGIN_OK &&
            version.size > 0,
        "version 可读");
  freeStr(version);

  MusicxxExternPluginHostConfig cfg{};
  cfg.struct_size = sizeof(MusicxxExternPluginHostConfig);
  const std::string appV = "0.0.0-test";
  /// 平台标识与编译目标一致（宿主按它选多目标分支，见 testPluginTargetLayout）
  const std::string plat = testPlatform();
  const std::string lang = "zh-cn";
  /// 客户端界面能力段 (正常情况下由界面描述层库按客户端实现生成)
  const std::string uiCaps =
      R"({"apiVersion":1,"kind":"gui","blocks":["Text","Row","Column"],)"
      R"("controls":["checkbox"],"gap":12})";
  cfg.app_version = view(appV);
  cfg.platform = view(plat);
  cfg.language = view(lang);
  cfg.user_plugin_dir = view(pluginDir);
  cfg.ui_capabilities = view(uiCaps);
  cfg.log_level = 2;
  cfg.flags = MUSICXX_EXTERN_PLUGIN_FLAG_DEBUG_OBSERVE_EVENTS;

  auto *host = musicxx_extern_plugin_host_create(&cfg, &log);
  if (!host) {
    std::printf("host_create failed: %s\n", take(log).c_str());
    return 2;
  }
  check(host != nullptr, "host_create");

  check(musicxx_extern_plugin_host_start(host, &log) ==
            MUSICXX_EXTERN_PLUGIN_OK,
        "host_start");

  // 扫描
  MusicxxExternPluginString scan{};
  const auto scanRc = musicxx_extern_plugin_plugin_scan(host, &scan, &log);
  const std::string scanJson = take(scan);
  check(scanRc == MUSICXX_EXTERN_PLUGIN_OK, "plugin_scan 成功");
  std::printf("  [info] scanJson=%s\n", scanJson.substr(0, 400).c_str());
  // 扫描应同时发现原生与 JS 示例插件 (JS 插件零编译)
  check(scanJson.find("example_native") != std::string::npos,
        "扫描发现 example_native");
  check(scanJson.find("example_js") != std::string::npos,
        "扫描发现 example_js (JS 插件)");
  check(scanJson.find("example_js_shader") != std::string::npos,
        "扫描发现 example_js_shader (JS 插件)");
  check(scanJson.find("\"kind\":\"js\"") != std::string::npos,
        "JS 插件的 kind 为 js");
  check(scanJson.find("本次运行未启用 JS 运行时") == std::string::npos,
        "JS 运行时可用 (未报未启用)");

  // 动态库插件的入口解析: 清单按 Linux 写 entry (`example_native.so`),
  // Windows/macOS 上由内核
  // 修正扩展名。扫描阶段的校验必须与加载阶段用同一套解析,
  // 否则会出现"扫描判缺库文件、 加载却能命中"的不一致
  // (本用例就是那次缺陷的回归防护)。
  {
    const std::string nativeItem = scanItemOf(scanJson, "example_native");
    check(!nativeItem.empty(), "扫描结果含 example_native 条目");
    check(nativeItem.find("\"supported\":true") != std::string::npos,
          "动态库插件按平台解析 entry 后仍为可用 (未被判库文件缺失)");
    check(nativeItem.find("库文件缺失") == std::string::npos,
          "动态库插件未被判为库文件缺失");
  }

  // 多目标打包（仿 APK 的 lib/<系统>-<架构>/）：分支选择与装载
  testPluginTargetLayout(host, pluginDir);

  // 插件目录静态判定（plugin_inspect）：安装预检用的入口，与扫描同源
  testPluginInspect(host, pluginDir);

  // 装载 (同步)
  MusicxxExternPluginString empty{};
  const auto loadRc = musicxx_extern_plugin_plugin_load_sync(
      host, viewCP("example_native"), viewCP(R"({"enabled":true})"), 10000,
      &log);
  if (loadRc != MUSICXX_EXTERN_PLUGIN_OK) {
    std::printf("  [info] load rc=%d log=%s\n", loadRc, take(log).c_str());
  }
  check(loadRc == MUSICXX_EXTERN_PLUGIN_OK, "plugin_load_sync 成功");

  // 钩子处理器数量
  int32_t count = 0;
  check(musicxx_extern_plugin_hook_count(
            host, viewCP("musicxx.player.beforePlaySong"), &count, &log) ==
                MUSICXX_EXTERN_PLUGIN_OK &&
            count == 1,
        "beforePlaySong 处理器数为 1");
  check(musicxx_extern_plugin_hook_count(
            host, viewCP("musicxx.player.completed"), &count, &log) ==
                MUSICXX_EXTERN_PLUGIN_OK &&
            count == 0,
        "未注册钩子的处理器数为 0");

  // 事件泵: 应能取到 host.ready / plugin.loaded / hook.changed
  {
    MusicxxExternPluginString events{};
    const auto rc = musicxx_extern_plugin_poll_events(host, 200, &events, &log);
    const std::string eventsJson = take(events);
    check(rc == MUSICXX_EXTERN_PLUGIN_OK ||
              rc == MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT,
          "poll_events 可调用");
    check(eventsJson.find("musicxx.plugin.loaded") != std::string::npos ||
              eventsJson.find("musicxx.host.ready") != std::string::npos,
          "事件包含 host.ready / plugin.loaded");
    // UI 项注册/更新会推送 musicxx.ui.changed; 通知走动作请求通道
    // (musicxx.ui.notify)
    check(eventsJson.find("musicxx.ui.changed") != std::string::npos,
          "UI 项变化推送 musicxx.ui.changed 事件");
    check(eventsJson.find("musicxx.ui.notify") != std::string::npos,
          "通知走动作请求通道 (musicxx.ui.notify)");
  }

  // decision 钩子: 广告曲目 → skip
  {
    MusicxxExternPluginString out{};
    const std::string payload =
        R"({"sid":"s1","song":{"name":"广告插播 - 测试","artist":"x"},"mode":"normal"})";
    const auto rc = musicxx_extern_plugin_hook_emit(
        host, viewCP("musicxx.player.beforePlaySong"), viewP(payload),
        MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 200, &out, &log);
    const std::string result = take(out);
    check(rc == MUSICXX_EXTERN_PLUGIN_OK, "hook_emit(sync) 返回成功");
    check(result.find("\"skip\"") != std::string::npos,
          "广告曲目被裁决为 skip");
  }

  // decision 钩子: 普通曲目 → 无裁决
  {
    MusicxxExternPluginString out{};
    const std::string payload =
        R"({"sid":"s2","song":{"name":"普通歌曲","artist":"y"}})";
    const auto rc = musicxx_extern_plugin_hook_emit(
        host, viewCP("musicxx.player.beforePlaySong"), viewP(payload),
        MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 200, &out, &log);
    const std::string result = take(out);
    check(rc == MUSICXX_EXTERN_PLUGIN_OK, "hook_emit(sync) 普通曲目成功");
    check(result.find("\"skip\"") == std::string::npos, "普通曲目无 skip 裁决");
  }

  // 观察型钩子 (异步派发)
  {
    MusicxxExternPluginString out{};
    const std::string payload = R"({"sid":"s3","song":{"name":"观察目标"}})";
    const auto rc = musicxx_extern_plugin_hook_emit(
        host, viewCP("musicxx.song.changed"), viewP(payload),
        MUSICXX_EXTERN_PLUGIN_HOOK_ASYNC, 0, &out, &log);
    freeStr(out);
    check(rc == MUSICXX_EXTERN_PLUGIN_OK, "hook_emit(async) 入队成功");
  }

  // 裁决型钩子的异步派发: 立即拿到 callId, 结果经事件回传
  {
    MusicxxExternPluginString out{};
    const std::string payload =
        R"({"sid":"s4","song":{"name":"广告插播 - 异步","artist":"z"}})";
    const auto rc = musicxx_extern_plugin_hook_emit(
        host, viewCP("musicxx.player.beforePlaySong"), viewP(payload),
        MUSICXX_EXTERN_PLUGIN_HOOK_ASYNC, 200, &out, &log);
    const std::string ack = take(out);
    check(rc == MUSICXX_EXTERN_PLUGIN_OK, "裁决型钩子异步派发返回成功");
    check(ack.find("\"async\":true") != std::string::npos,
          "异步派发确认 async=true");
    const std::string callId = jsonIntField(ack, "callId");
    check(!callId.empty(), "异步派发返回 callId");

    // 结果经 `musicxx.hook.decision.result` 事件回传 (等一会儿让宿主线程跑完)
    std::string eventsJson;
    bool seen = false;
    for (int i = 0; i < 50 && !seen; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds{20});
      MusicxxExternPluginString events{};
      const auto rcEv =
          musicxx_extern_plugin_poll_events(host, 500, &events, &log);
      const std::string batch = take(events);
      if (rcEv == MUSICXX_EXTERN_PLUGIN_OK) {
        eventsJson += batch;
      }
      seen =
          eventsJson.find("musicxx.hook.decision.result") != std::string::npos;
    }
    check(seen, "异步裁决结果经事件回传");
    const size_t at = eventsJson.find("musicxx.hook.decision.result");
    if (at != std::string::npos) {
      const std::string tail = eventsJson.substr(at);
      check(tail.find("\"callId\":" + callId) != std::string::npos,
            "结果事件带同一个 callId");
      check(tail.find("\"skip\"") != std::string::npos, "异步裁决结果为 skip");
      check(tail.find("\"hook\":\"musicxx.player.beforePlaySong\"") !=
                std::string::npos,
            "结果事件带钩子 id");
    }

    // 异步派发不占用 Dart 线程: 派发调用立即返回 ack (由上面的 ack 证明)
    MusicxxExternPluginString out2{};
    const auto rc2 = musicxx_extern_plugin_hook_emit(
        host, viewCP("musicxx.player.beforePlaySong"),
        viewCP(R"({"sid":"s5","song":{"name":"普通歌曲"}})"),
        MUSICXX_EXTERN_PLUGIN_HOOK_ASYNC, 200, &out2, &log);
    const std::string ack2 = take(out2);
    check(rc2 == MUSICXX_EXTERN_PLUGIN_OK, "异步派发普通曲目成功");
    check(ack2.find("\"callId\"") != std::string::npos,
          "异步派发每次都分配 callId");
  }

  // 未知钩子: 处理器数为 0 且派发不报错
  {
    MusicxxExternPluginString out{};
    const auto rc = musicxx_extern_plugin_hook_emit(
        host, viewCP("musicxx.plugin.unknownHook"), viewCP("{}"),
        MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 50, &out, &log);
    const std::string result = take(out);
    check(rc == MUSICXX_EXTERN_PLUGIN_OK, "未知钩子派发不报错");
    check(result.find("\"handled\":false") != std::string::npos,
          "未知钩子无处理器");
  }

  // 状态镜像
  {
    const std::string key = "musicxx.state.song";
    const std::string val = R"({"sid":"s1","name":"镜像歌曲"})";
    check(musicxx_extern_plugin_state_update(host, viewP(key), viewP(val),
                                             &log) == MUSICXX_EXTERN_PLUGIN_OK,
          "state_update 成功");
    MusicxxExternPluginString listed{};
    check(musicxx_extern_plugin_plugin_list(host, &listed, &log) ==
              MUSICXX_EXTERN_PLUGIN_OK,
          "plugin_list 成功");
    const std::string listJson = take(listed);
    check(listJson.find("example_native") != std::string::npos,
          "plugin_list 含已装载插件");
  }

  // 统计与调试信息
  {
    MusicxxExternPluginString stats{};
    check(musicxx_extern_plugin_stats(host, nullptr, &stats, &log) ==
              MUSICXX_EXTERN_PLUGIN_OK,
          "stats 可读");
    const std::string statsJson = take(stats);
    check(statsJson.find("example_native") != std::string::npos,
          "stats 含插件条目");

    // 事件计数与速率: 插件发布的 plugin.<id>.* 事件归属到该插件
    // (example_native 的 start 事务里发布过一次 plugin.example_native.hello)
    const auto at = statsJson.find(R"("id":"example_native")");
    const std::string entry =
        (at == std::string::npos) ? std::string{} : statsJson.substr(at, 600);
    const std::string eventsText = jsonIntField(entry, "events");
    check(!eventsText.empty() && std::stoll(eventsText) >= 1,
          "插件发布的事件计入统计 (plugin.<id>.* 归属)");
    check(entry.find("\"eventsPerSec\":") != std::string::npos,
          "stats 含事件速率字段");
    check(entry.find("\"eventsAvgPerSec\":") != std::string::npos,
          "stats 含平均事件速率字段");

    // 钩子统计按处理器给出调用次数/失败次数/耗时 (只观测不限制: 没有"暂停派发"这个概念)
    MusicxxExternPluginString hookStats{};
    check(musicxx_extern_plugin_hook_stats(host, &hookStats, &log) ==
              MUSICXX_EXTERN_PLUGIN_OK,
          "hook_stats 可读");
    const std::string hookStatsJson = take(hookStats);
    check(hookStatsJson.find("\"failures\":") != std::string::npos,
          "hook_stats 含失败次数");
    check(hookStatsJson.find("\"paused\"") == std::string::npos,
          "hook_stats 不再含暂停派发标记");
    check(hookStatsJson.find("\"timeouts\"") == std::string::npos,
          "hook_stats 不再含超时统计");

    MusicxxExternPluginString info{};
    const auto infoRc = musicxx_extern_plugin_debug_info(host, &info, &log);
    const std::string infoJson = take(info);
    check(infoRc == MUSICXX_EXTERN_PLUGIN_OK, "debug_info 可读");

    // 架构标识: 上报给插件的 `arch` 必须是规范名, 而且与"选分支/判清单 arch 用的架构"
    // 是同一个名字 (32 位 ARM 上曾经报 unknown, 于是 armeabi-v7a 分支与 `arch: [arm]`
    // 声明谁都匹配不上)
    const std::string reportedArch = jsonStringField(infoJson, "arch");
    checkText(reportedArch != "unknown",
              "宿主上报的架构不是 unknown (32 位 ARM 应报 armv7)");
    checkText(reportedArch == testArch(),
              "宿主上报的架构与用例期望一致 (" + reportedArch + ")");
  }

  // 插件能力调用 (Dart → 插件) + 线程模型 / 命名空间自检
  {
    // 先让宿主发布一个测试主题 (插件已订阅), 再读探针 —— 两者都投递到宿主线程,
    // 因此顺序确定 (先进先出), 探针一定看到已处理的事件
    MusicxxExternPluginString pubLog{};
    const auto pubRc = musicxx_extern_plugin_event_publish(
        host, viewCP("musicxx.test.ping"), viewCP(R"({"n":1})"), &pubLog);
    check(pubRc == MUSICXX_EXTERN_PLUGIN_OK, "event_publish 合法主题成功");
    freeStr(pubLog);

    MusicxxExternPluginString badPubLog{};
    const auto badPubRc = musicxx_extern_plugin_event_publish(
        host, viewCP("foo.bar"), viewCP("{}"), &badPubLog);
    check(badPubRc == MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION,
          "event_publish 非法主题被拒绝");
    freeStr(badPubLog);

    MusicxxExternPluginString out{};
    MusicxxExternPluginString callLog{};
    const auto rc = musicxx_extern_plugin_plugin_call(
        host, viewCP("example_native"), viewCP("plugin.example_native.probe"),
        viewCP("{}"), 3000, &out, &callLog);
    const std::string probe = take(out);
    const std::string callErr = take(callLog);
    check(rc == MUSICXX_EXTERN_PLUGIN_OK, "plugin_call(probe) 成功");
    std::printf("  [info] plugin_call rc=%d log=%s\n", rc, callErr.c_str());
    std::printf("  [info] probe=%s\n", probe.c_str());

    // ==================== 声明式 UI 扩展 (动态库插件)
    // ====================
    //
    // 插件只做声明 (类型 + JSON 内容), 宿主负责渲染;
    // 这里校验注册/更新/拒绝/快照/事件。
    {
      check(jsonIntField(probe, "uiHomeRc") == "0",
            "注册主页入口项成功 (rc=0)");
      check(jsonIntField(probe, "uiSongRc") == "0",
            "注册歌曲菜单项成功 (rc=0)");
      check(jsonIntField(probe, "uiForeignRc") == "-6",
            "他人命名空间的项被拒绝 (-6)");
      check(jsonIntField(probe, "uiBadTypeRc") == "-4",
            "未知 UI 项类型被拒绝 (-4)");
      check(jsonIntField(probe, "uiBadDataRc") == "-1",
            "缺少 title 的声明被拒绝 (-1)");
      check(jsonIntField(probe, "uiUpdateRc") == "0",
            "更新自己的项成功 (rc=0)");
      check(jsonIntField(probe, "uiNotifyRc") == "0",
            "通知受理成功 (fire-and-forget)");

      MusicxxExternPluginString snap{};
      const auto snapRc = musicxx_extern_plugin_ui_snapshot(host, &snap, &log);
      const std::string snapshot = take(snap);
      check(snapRc == MUSICXX_EXTERN_PLUGIN_OK, "ui_snapshot 成功");
      check(snapshot.find("plugin.example_native.card") != std::string::npos,
            "快照含主页入口项");
      check(snapshot.find("plugin.example_native.songInfo") !=
                std::string::npos,
            "快照含歌曲菜单项");
      check(snapshot.find("plugin.example_native.playingBg") !=
                    std::string::npos &&
                snapshot.find("musicxx.ui.playing.background") !=
                    std::string::npos &&
                snapshot.find("shader/bg.shaderbundle") != std::string::npos,
            "快照含播放页背景样式 (shader bundle 声明)");
      check(snapshot.find("musicxx.ui.home.entry") != std::string::npos,
            "快照含官方 UI 项类型");
      check(snapshot.find("plugin") != std::string::npos,
            "快照项带所属插件 id");
      check(snapshot.find("示例插件") != std::string::npos,
            "更新后的声明内容生效");
      check(snapshot.find("plugin.other_plugin.card") == std::string::npos,
            "他人命名空间的项不在快照里");
      check(snapshot.find("musicxx.ui.unknown") == std::string::npos,
            "未知类型不在快照里");
      check(snapshot.find("nodata") == std::string::npos, "非法声明不在快照里");
    }

    // 单宿主线程模型: start 事务与能力处理器必须在同一条线程上执行
    const std::string startThread = jsonStringField(probe, "startThread");
    const std::string callThread = jsonStringField(probe, "callThread");
    check(!startThread.empty() && startThread == callThread,
          "插件代码全部在宿主线程执行");

    // 钩子 / 事件命名空间校验 (动作名不再校验: 插件的动作请求一律受理)
    check(jsonIntField(probe, "unknownHookRc") == "-4",
          "未知前缀钩子注册被拒绝");
    check(jsonIntField(probe, "foreignActionRc") == "0",
          "任意动作名都被受理 (不再做命名空间校验)");
    check(jsonIntField(probe, "dupHookRc") == "0",
          "同一钩子覆盖式重复注册成功");
    // 事件命名空间与订阅
    check(jsonIntField(probe, "badTopicSubscribeRc") == "-1",
          "非法事件主题订阅被拒绝");
    check(jsonIntField(probe, "ownPublishRc") == "0",
          "本插件命名空间事件可发布");
    check(jsonIntField(probe, "foreignPublishRc") != "0",
          "他人命名空间事件发布被拒绝");
    check(jsonIntField(probe, "pingEvents") == "1", "插件收到宿主发布的事件");
    check(jsonIntField(probe, "stateEvents") != "0",
          "状态镜像变化通知到订阅插件");
    // 状态镜像可被插件同步读到 (上一段推送过 musicxx.state.song)
    check(jsonIntField(probe, "stateLen") != "0", "插件可同步读状态镜像");
  }

  // 动作请求超时保护: 插件请求 → Dart 不回复 → 宿主到点终结并推
  // cancel 事件
  {
    MusicxxExternPluginString out{};
    const auto rc = musicxx_extern_plugin_plugin_call(
        host, viewCP("example_native"), viewCP("probe"),
        viewCP(R"({"requestAction":"musicxx.test.neverRespond"})"), 3000, &out,
        &log);
    freeStr(out);
    check(rc == MUSICXX_EXTERN_PLUGIN_OK, "probe 触发插件动作请求成功");

    // 动作超时: 插件给的时间就是生效的超时 (不再钳制区间); 等它过期后取事件与探针
    std::this_thread::sleep_for(std::chrono::milliseconds{1600});

    MusicxxExternPluginString events{};
    const auto rcEv =
        musicxx_extern_plugin_poll_events(host, 500, &events, &log);
    const std::string eventsJson = take(events);
    check(rcEv == MUSICXX_EXTERN_PLUGIN_OK ||
              rcEv == MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT,
          "poll_events 可调用");
    check(eventsJson.find("musicxx.action.request") != std::string::npos,
          "Dart 侧收到动作请求事件");
    check(eventsJson.find("musicxx.action.cancel") != std::string::npos,
          "动作超时后收到取消事件");

    MusicxxExternPluginString probeOut{};
    const auto rcProbe = musicxx_extern_plugin_plugin_call(
        host, viewCP("example_native"), viewCP("probe"), viewCP("{}"), 3000,
        &probeOut, &log);
    const std::string probe2 = take(probeOut);
    check(rcProbe == MUSICXX_EXTERN_PLUGIN_OK, "超时后探针可读");
    // PLUGINXX_OPERATOR_CANCELLED == 1: 超时按取消终结,
    // 插件侧收到恰好一次完成通知
    check(jsonIntField(probe2, "lastActionStatus") == "1",
          "动作超时按取消终结并回调插件");
  }

  // 能力调用错误路径: 未加载插件 / 未声明能力
  {
    MusicxxExternPluginString out{};
    const auto rc = musicxx_extern_plugin_plugin_call(
        host, viewCP("no_such_plugin"), viewCP("probe"), viewCP("{}"), 1000,
        &out, &log);
    freeStr(out);
    check(rc == MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND,
          "plugin_call 未加载插件返回未找到");

    MusicxxExternPluginString out2{};
    const auto rc2 = musicxx_extern_plugin_plugin_call(
        host, viewCP("example_native"), viewCP("noSuchCapability"),
        viewCP("{}"), 1000, &out2, &log);
    freeStr(out2);
    check(rc2 == MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND,
          "plugin_call 未声明能力返回未找到");
  }

  // 装载失败安全降级: 不崩溃、明确失败、宿主继续可用
  {
    MusicxxExternPluginString loadLog{};
    const auto rc = musicxx_extern_plugin_plugin_load_sync(
        host, viewCP("definitely_missing_plugin"), viewCP("{}"), 3000,
        &loadLog);
    const std::string loadErr = take(loadLog);
    check(rc != MUSICXX_EXTERN_PLUGIN_OK, "装载不存在的插件返回失败");
    check(rc != MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT,
          "失败是明确失败而非挂住超时");
    check(!loadErr.empty(), "失败原因有可读文本");
    std::printf("  [info] load-fail rc=%d log=%s\n", rc, loadErr.c_str());

    MusicxxExternPluginString again{};
    const auto rc2 = musicxx_extern_plugin_plugin_load_sync(
        host, viewCP("definitely_missing_plugin"), viewCP("{}"), 3000, &again);
    take(again);
    check(rc2 != MUSICXX_EXTERN_PLUGIN_OK, "失败后不会静默重试成功");

    // 宿主仍可正常工作
    MusicxxExternPluginString listed{};
    check(musicxx_extern_plugin_plugin_list(host, &listed, &log) ==
                  MUSICXX_EXTERN_PLUGIN_OK &&
              take(listed).find("example_native") != std::string::npos,
          "装载失败后宿主仍可用");
  }

  // 禁用 → 钩子摘除, 启用 → 重新注册
  {
    check(musicxx_extern_plugin_plugin_disable(
              host, viewCP("example_native"), &log) == MUSICXX_EXTERN_PLUGIN_OK,
          "plugin_disable 成功");
    int32_t afterDisable = 0;
    musicxx_extern_plugin_hook_count(
        host, viewCP("musicxx.player.beforePlaySong"), &afterDisable, &log);
    check(afterDisable == 0, "禁用后钩子处理器被摘除");
  }

  // 卸载
  {
    check(musicxx_extern_plugin_plugin_unload(host, viewCP("example_native"),
                                              &log) == MUSICXX_EXTERN_PLUGIN_OK,
          "plugin_unload 成功");
    int32_t after = 0;
    musicxx_extern_plugin_hook_count(
        host, viewCP("musicxx.player.beforePlaySong"), &after, &log);
    check(after == 0, "卸载后无钩子残留");
    // 幂等: 重复卸载视为已达目标状态 (Dart 侧状态不同步/重复点击都会走到这里)
    check(musicxx_extern_plugin_plugin_unload(host, viewCP("example_native"),
                                              &log) == MUSICXX_EXTERN_PLUGIN_OK,
          "重复卸载幂等成功");
    // 卸载后能力调用应返回未找到 (注册已随实例摘除)
    MusicxxExternPluginString out{};
    const auto rc = musicxx_extern_plugin_plugin_call(
        host, viewCP("example_native"), viewCP("probe"), viewCP("{}"), 1000,
        &out, &log);
    freeStr(out);
    check(rc != MUSICXX_EXTERN_PLUGIN_OK, "卸载后能力调用失败");
  }

  // ==================== JS 插件 (零编译) ====================
  //
  // 同一套用例在 native/js 两条链路上跑: decision 裁决、observe
  // 通知、能力探针、 状态镜像读取、事件订阅、定时器、禁用/卸载摘除。
  {
    MusicxxExternPluginString loadLog{};
    const auto jsLoadRc = musicxx_extern_plugin_plugin_load_sync(
        host, viewCP("example_js"), viewCP(R"({"enabled":true})"), 15000,
        &loadLog);
    if (jsLoadRc != MUSICXX_EXTERN_PLUGIN_OK) {
      std::printf("  [info] js load rc=%d log=%s\n", jsLoadRc,
                  take(loadLog).c_str());
    } else {
      freeStr(loadLog);
    }
    check(jsLoadRc == MUSICXX_EXTERN_PLUGIN_OK, "JS 插件装载成功 (example_js)");

    int32_t jsHookCount = 0;
    check(musicxx_extern_plugin_hook_count(
              host, viewCP("musicxx.player.beforePlaySong"), &jsHookCount,
              &log) == MUSICXX_EXTERN_PLUGIN_OK &&
              jsHookCount == 1,
          "JS 钩子处理器已注册 (beforePlaySong = 1)");

    // decision 钩子: 广告曲目 → skip
    {
      MusicxxExternPluginString out{};
      const std::string payload =
          R"({"sid":"js1","song":{"name":"广告插播 - JS 测试","artist":"x"}})";
      const auto rc = musicxx_extern_plugin_hook_emit(
          host, viewCP("musicxx.player.beforePlaySong"), viewP(payload),
          MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 300, &out, &log);
      const std::string result = take(out);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "JS 钩子派发返回成功");
      check(result.find("\"skip\"") != std::string::npos,
            "JS 裁决: 广告曲目 skip");
    }

    // decision 钩子: 普通曲目 → 无裁决
    {
      MusicxxExternPluginString out{};
      const std::string payload =
          R"({"sid":"js2","song":{"name":"普通歌曲 JS"}})";
      const auto rc = musicxx_extern_plugin_hook_emit(
          host, viewCP("musicxx.player.beforePlaySong"), viewP(payload),
          MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 300, &out, &log);
      const std::string result = take(out);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "JS 钩子 (普通曲目) 派发成功");
      check(result.find("\"skip\"") == std::string::npos,
            "JS 裁决: 普通曲目无 skip");
    }

    // decision 钩子: 播放错误 → patch.tryNextSrc
    {
      MusicxxExternPluginString out{};
      const std::string payload =
          R"({"sid":"js3","srcKey":"k3","errorCode":-1})";
      const auto rc = musicxx_extern_plugin_hook_emit(
          host, viewCP("musicxx.player.error"), viewP(payload),
          MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 300, &out, &log);
      const std::string result = take(out);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "JS 错误钩子派发成功");
      check(result.find("tryNextSrc") != std::string::npos,
            "JS 裁决: 首个错误建议换源");
    }

    // observe 钩子 (异步): 切歌通知 + 脚本日志
    {
      MusicxxExternPluginString out{};
      const std::string payload =
          R"({"sid":"js4","song":{"name":"观察目标 JS"}})";
      const auto rc = musicxx_extern_plugin_hook_emit(
          host, viewCP("musicxx.song.changed"), viewP(payload),
          MUSICXX_EXTERN_PLUGIN_HOOK_ASYNC, 0, &out, &log);
      freeStr(out);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "JS 观察钩子入队成功");

      // 等 JS 线程执行完 (含脚本定时器 50ms)
      std::this_thread::sleep_for(std::chrono::milliseconds{500});
      MusicxxExternPluginString events{};
      const std::string eventsJson = [&] {
        musicxx_extern_plugin_poll_events(host, 500, &events, &log);
        return take(events);
      }();
      check(eventsJson.find("musicxx.plugin.log") != std::string::npos,
            "JS 日志经事件回传");
      check(eventsJson.find("切歌") != std::string::npos,
            "JS 观察钩子已执行 (切歌通知)");
    }

    // 能力探针 (Dart → JS): 自检信息 + 状态镜像同步读
    {
      MusicxxExternPluginString out{};
      const auto rc = musicxx_extern_plugin_plugin_call(
          host, viewCP("example_js"), viewCP("probe"), viewCP("{}"), 5000, &out,
          &log);
      const std::string probe = take(out);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "JS 能力调用成功 (probe)");
      check(jsonStringField(probe, "pluginId") == "example_js",
            "JS 能力读到插件 id");
      check(jsonIntField(probe, "hookCount") == "4", "JS 注册了 4 个钩子");
      check(jsonIntField(probe, "songChangedCount") == "1",
            "JS 观察钩子计数为 1 (钩子真的执行了)");
      check(jsonStringField(probe, "currentSongName") == "镜像歌曲",
            "JS 同步读状态镜像 (musicxx.state.song)");
      check(jsonStringField(probe, "hostPlatform") == "windows",
            "JS 读到宿主信息 (平台)");
      // JS 侧的声明式 UI 项 (顶层注册 → 宿主线程回放)
      // 2 项 = 主页入口 + 歌曲菜单 (播放页背景样式已拆到 example_js_shader;
      // 设置界面是插件自己的页面, 不是 UI 项)
      check(jsonIntField(probe, "uiEntries") == "2",
            "JS 注册了 2 个 UI 项 (脚本侧登记)");
      check(jsonIntField(probe, "selfStatsHooks") == "4",
            "JS 能读自己的统计 (stats.getSelf 的钩子计数)");
      // 客户端界面能力段: 宿主配置里给的 ui 段原样出现在 host.info() 里
      check(jsonStringField(probe, "hostUiKind") == "gui",
            "JS 从 host.info().ui 读到客户端能力段 (渲染类型)");
      check(jsonIntField(probe, "hostUiBlocks") == "3",
            "JS 从 host.info().ui 读到客户端支持的组件数");
    }

    // JS 插件的 UI 项进入宿主快照 (与动态库插件同一注册表)
    {
      MusicxxExternPluginString snap{};
      const auto rc = musicxx_extern_plugin_ui_snapshot(host, &snap, &log);
      const std::string snapshot = take(snap);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "ui_snapshot 可读 (含 JS 项)");
      check(snapshot.find("plugin.example_js.card") != std::string::npos,
            "JS 插件的主页入口项进入快照");
      check(snapshot.find("plugin.example_js.songInfo") != std::string::npos,
            "JS 插件的菜单项进入快照");
      // 框架没有"插件设置页"类型: 设置界面改成插件自己的页面 (同名能力),
      // 不再注册 UI 项, 快照里也不该出现 settings.page
      check(snapshot.find("musicxx.ui.settings.page") == std::string::npos,
            "快照里没有设置页类型 (框架不管理插件设置入口)");
      check(snapshot.find("plugin.example_js.settings") == std::string::npos,
            "JS 插件不再注册设置页入口项");
      check(snapshot.find("plugin.example_js.bg") == std::string::npos,
            "播放页背景示例已拆成独立插件 (example_js 不再注册背景项)");
    }

    // 插件自己的设置界面: 就是插件页面的同名能力
    // (框架不管理设置入口、也不渲染设置控件, 页面内容全部由插件给)
    {
      MusicxxExternPluginString out{};
      const auto rc = musicxx_extern_plugin_plugin_call(
          host, viewCP("example_js"), viewCP("settings"),
          viewCP(R"({"view":"settings"})"), 5000, &out, &log);
      const std::string view = take(out);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK,
            "设置界面能力调用成功 (插件自绘页面)");
      check(view.find("\"blocks\"") != std::string::npos,
            "设置界面返回声明式块 (由插件给出结构)");
      check(view.find("config.json") != std::string::npos,
            "设置界面内容由插件给出 (说明自己的 config.json 用法)");
    }

    // 播放页背景示例已拆成独立插件 `example_js_shader`: 它声明背景样式,
    // 速率 (0.5× / 1× / 2×) 是它自己的设置项, 改完重新声明 UI 项。
    {
      MusicxxExternPluginString loadLog{};
      const auto shaderLoadRc = musicxx_extern_plugin_plugin_load_sync(
          host, viewCP("example_js_shader"), viewCP(R"({"enabled":true})"),
          8000, &loadLog);
      if (shaderLoadRc != MUSICXX_EXTERN_PLUGIN_OK) {
        std::printf("  [info] shader js load rc=%d log=%s\n", shaderLoadRc,
                    take(loadLog).c_str());
      } else {
        freeStr(loadLog);
      }
      check(shaderLoadRc == MUSICXX_EXTERN_PLUGIN_OK,
            "背景示例插件装载成功 (example_js_shader)");

      MusicxxExternPluginString snap{};
      const auto snapRc = musicxx_extern_plugin_ui_snapshot(host, &snap, &log);
      const std::string snapshot = take(snap);
      check(snapRc == MUSICXX_EXTERN_PLUGIN_OK &&
                snapshot.find("plugin.example_js_shader.bg") !=
                    std::string::npos &&
                snapshot.find("musicxx.ui.playing.background") !=
                    std::string::npos &&
                snapshot.find("shader/bg.shaderbundle") != std::string::npos,
            "背景示例的播放页背景样式进入快照 (含 shader bundle)");
      check(snapshot.find("plugin.example_js_shader.card") != std::string::npos,
            "背景示例的主页入口项进入快照");

      // 速率设置: 页面里能读到, 切一档后显示的值跟着变
      // (设置页的行由布局块组合, 右侧状态按值的形态读: 见 jsonFirstRateText)
      MusicxxExternPluginString before{};
      const auto beforeRc = musicxx_extern_plugin_plugin_call(
          host, viewCP("example_js_shader"), viewCP("settings"), viewCP("{}"),
          5000, &before, &log);
      const std::string beforeView = take(before);
      check(beforeRc == MUSICXX_EXTERN_PLUGIN_OK &&
                beforeView.find("背景动画速率") != std::string::npos,
            "背景示例的设置页含『背景动画速率』设置项");
      const std::string beforeRate = jsonFirstRateText(beforeView);

      MusicxxExternPluginString cycle{};
      const auto cycleRc = musicxx_extern_plugin_plugin_call(
          host, viewCP("example_js_shader"), viewCP("cycleBackgroundRate"),
          viewCP(R"({"view":"settings"})"), 5000, &cycle, &log);
      const std::string cycleView = take(cycle);
      const std::string afterRate = jsonFirstRateText(cycleView);
      check(cycleRc == MUSICXX_EXTERN_PLUGIN_OK,
            "切换背景动画速率的能力可调用");
      check(!beforeRate.empty() && !afterRate.empty() &&
                beforeRate != afterRate,
            "切换后设置页显示的速率随之变化");

      // 卸载 → UI 项摘除
      check(
          musicxx_extern_plugin_plugin_unload(host, viewCP("example_js_shader"),
                                              &log) == MUSICXX_EXTERN_PLUGIN_OK,
          "卸载背景示例插件");
      MusicxxExternPluginString after{};
      const auto afterRc =
          musicxx_extern_plugin_ui_snapshot(host, &after, &log);
      const std::string afterSnapshot = take(after);
      check(afterRc == MUSICXX_EXTERN_PLUGIN_OK &&
                afterSnapshot.find("plugin.example_js_shader.bg") ==
                    std::string::npos,
            "卸载后背景示例的 UI 项无残留");
    }

    // ============ 清单 scripts: 多脚本按顺序装载 (JS 插件) ============
    //
    // `scripts: [kit.js, plugin.js]` 里的脚本在**同一个 JS 上下文**里依次执行:
    // 前一个脚本定义的全局量在后一个里可见 (kit 随插件目录分发的用法)。
    {
      MusicxxExternPluginString loadLog{};
      const auto loadRc = musicxx_extern_plugin_plugin_load_sync(
          host, viewCP("example_js_multi_script"), viewCP("{}"), 8000, &loadLog);
      if (loadRc != MUSICXX_EXTERN_PLUGIN_OK) {
        std::printf("  [info] example_js_multi_script load rc=%d log=%s\n", loadRc,
                    take(loadLog).c_str());
      } else {
        freeStr(loadLog);
      }
      check(loadRc == MUSICXX_EXTERN_PLUGIN_OK, "多脚本插件装载成功");

      MusicxxExternPluginString out{};
      const auto rc = musicxx_extern_plugin_plugin_call(
          host, viewCP("example_js_multi_script"), viewCP("probe"), viewCP("{}"), 5000,
          &out, &log);
      const std::string probe = take(out);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "多脚本插件的能力可调用");
      check(jsonStringField(probe, "marker") == "kit-loaded",
            "脚本按 scripts 顺序执行 (前一个脚本的全局量在后一个里可见)");
      check(jsonStringField(probe, "rowKind") == "Text",
            "后一个脚本能调用前一个脚本定义的函数");
      check(jsonStringField(probe, "hostUiKind") == "gui",
            "多脚本插件同样读到 host.info().ui");

      check(musicxx_extern_plugin_plugin_unload(host,
                                                viewCP("example_js_multi_script"),
                                                &log) == MUSICXX_EXTERN_PLUGIN_OK,
            "多脚本插件卸载成功");
    }

    // ============ 跨插件能力调用 (capability.call) ============
    //
    // JS 插件调用其它插件的能力: JS 目标同线程直接调用;
    // 原生目标投递到宿主线程执行, 脚本侧不等待 (返回
    // Promise)。这里用"最后一次结果"记录, 由 probe 能力回读。
    {
      // 原生示例插件在前面的用例里被卸载了, 这里临时再装一次作为被调用方
      MusicxxExternPluginString reloadLog{};
      const auto reloadRc = musicxx_extern_plugin_plugin_load_sync(
          host, viewCP("example_native"), viewCP("{}"), 8000, &reloadLog);
      check(reloadRc == MUSICXX_EXTERN_PLUGIN_OK,
            "临时装载动态库插件 (跨插件调用被调用方)");

      MusicxxExternPluginString out{};
      const auto callRc = musicxx_extern_plugin_plugin_call(
          host, viewCP("example_js"), viewCP("crossCall"),
          viewCP(R"({"target":"example_native","method":"probe"})"), 5000, &out,
          &log);
      const std::string callJson = take(out);
      check(callRc == MUSICXX_EXTERN_PLUGIN_OK, "触发跨插件调用 (JS → 原生)");
      check(jsonIntField(callJson, "accepted") == "1", "跨插件调用被受理");

      // 等回执回到 JS 线程 (宿主线程执行 + 投递回 JS 线程)
      std::this_thread::sleep_for(std::chrono::milliseconds{800});
      MusicxxExternPluginString probe3{};
      const auto probe3Rc = musicxx_extern_plugin_plugin_call(
          host, viewCP("example_js"), viewCP("probe"), viewCP("{}"), 3000,
          &probe3, &log);
      const std::string probe3Json = take(probe3);
      check(probe3Rc == MUSICXX_EXTERN_PLUGIN_OK, "回读跨插件调用结果");
      check(jsonIntField(probe3Json, "crossPending") == "0",
            "跨插件调用已结束 (无挂起)");
      check(jsonIntField(probe3Json, "crossOk") == "1",
            "跨插件调用成功 (JS → 原生能力)");
      check(jsonIntField(probe3Json, "crossKeys") != "0",
            "跨插件调用拿到结果内容");

      check(musicxx_extern_plugin_plugin_unload(host, viewCP("example_native"),
                                                &log) ==
                MUSICXX_EXTERN_PLUGIN_OK,
            "卸载临时装载的动态库插件");
    }

    // ============ JS 执行上限已移除 ============
    //
    // 早期版本有一个可选的 `jsExecGuardMs`（超时中断超长脚本）。现在宿主不限制脚本执行
    // 时长、也不提供中断开关：死循环脚本会一直占住共享 JS 线程（对照示例 example_js_spin
    // 因此不再装载 —— 它会真的把线程占死，连卸载都做不了）。
    {
      check(musicxx_extern_plugin_set_config(host,
                                            viewP(R"({"jsExecGuardMs":400})"),
                                            &log) == MUSICXX_EXTERN_PLUGIN_OK,
            "执行上限配置项被忽略 (不再报错)");
      MusicxxExternPluginString stats{};
      const auto statsRc =
          musicxx_extern_plugin_stats(host, nullptr, &stats, &log);
      const std::string statsJson = take(stats);
      check(statsRc == MUSICXX_EXTERN_PLUGIN_OK, "stats 可读 (无执行上限)");
      check(statsJson.find("execGuardHits") == std::string::npos,
            "统计不再含执行上限中断计数");
      check(statsJson.find("execGuardMs") == std::string::npos,
            "统计不再含执行上限配置");
    }

    // 禁用 → 钩子摘除, 启用 → 重新注册 (脚本重跑)
    {
      check(musicxx_extern_plugin_plugin_disable(
                host, viewCP("example_js"), &log) == MUSICXX_EXTERN_PLUGIN_OK,
            "JS 插件禁用成功");
      int32_t afterDisable = 0;
      musicxx_extern_plugin_hook_count(
          host, viewCP("musicxx.player.beforePlaySong"), &afterDisable, &log);
      check(afterDisable == 0, "JS 插件禁用后钩子被摘除");

      check(musicxx_extern_plugin_plugin_enable(
                host, viewCP("example_js"), &log) == MUSICXX_EXTERN_PLUGIN_OK,
            "JS 插件重新启用成功");
      std::this_thread::sleep_for(std::chrono::milliseconds{300});
      int32_t afterEnable = 0;
      musicxx_extern_plugin_hook_count(
          host, viewCP("musicxx.player.beforePlaySong"), &afterEnable, &log);
      check(afterEnable == 1, "JS 插件启用后钩子重新注册");
    }

    // 调试信息含 JS 运行时状态
    {
      MusicxxExternPluginString debug{};
      const auto rc = musicxx_extern_plugin_debug_info(host, &debug, &log);
      const std::string debugJson = take(debug);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "debug_info 可读");
      check(debugJson.find("\"available\":true") != std::string::npos,
            "调试信息含 JS 运行时");
      check(debugJson.find("example_js") != std::string::npos,
            "调试信息含 JS 插件");
      // 共享 JS 线程的排队观测 (只观测不限制)
      check(debugJson.find("\"queueDepth\":") != std::string::npos,
            "调试信息含 JS 任务队列深度");
      check(debugJson.find("\"queueWaitMaxMs\":") != std::string::npos,
            "调试信息含 JS 排队等待时长");

      MusicxxExternPluginString stats{};
      check(musicxx_extern_plugin_stats(host, nullptr, &stats, &log) ==
                MUSICXX_EXTERN_PLUGIN_OK,
            "stats 可读 (含 JS 段)");
      const std::string statsJson = take(stats);
      check(statsJson.find("\"queueDepth\":") != std::string::npos,
            "stats 含 JS 任务队列深度");
      check(statsJson.find("\"queueWaitLastMs\":") != std::string::npos,
            "stats 含 JS 最近一次排队等待时长");
    }

    // 卸载 → 注册全部摘除
    {
      check(musicxx_extern_plugin_plugin_unload(
                host, viewCP("example_js"), &log) == MUSICXX_EXTERN_PLUGIN_OK,
            "JS 插件卸载成功");
      int32_t after = 0;
      musicxx_extern_plugin_hook_count(
          host, viewCP("musicxx.player.beforePlaySong"), &after, &log);
      check(after == 0, "JS 插件卸载后无钩子残留");
      MusicxxExternPluginString out{};
      const auto rc = musicxx_extern_plugin_plugin_call(
          host, viewCP("example_js"), viewCP("probe"), viewCP("{}"), 1000, &out,
          &log);
      freeStr(out);
      check(rc != MUSICXX_EXTERN_PLUGIN_OK, "JS 插件卸载后能力调用失败");
    }
  }

  // ==================== JavaScript 异步裁决 (裁决处理器返回 Promise)
  // ====================
  //
  // 语义 (无等待超时):
  // - 处理器返回 Promise 时, 宿主一直等到它结算, 裁决照常生效;
  // - 没有"等太久就按无裁决继续"这回事, 也没有迟到丢弃。
  {
    MusicxxExternPluginString loadLog{};
    const auto jsAsyncRc = musicxx_extern_plugin_plugin_load_sync(
        host, viewCP("example_js_async"), viewCP(R"({"enabled":true})"), 8000,
        &loadLog);
    check(jsAsyncRc == MUSICXX_EXTERN_PLUGIN_OK,
          "异步裁决对照示例装载成功 (example_js_async)");
    if (jsAsyncRc != MUSICXX_EXTERN_PLUGIN_OK) {
      std::printf("  [info] example_js_async rc=%d log=%s\n", jsAsyncRc,
                  take(loadLog).c_str());
    } else {
      freeStr(loadLog);
    }

    int32_t seekHandlers = 0;
    musicxx_extern_plugin_hook_count(host, viewCP("musicxx.player.seek"),
                                     &seekHandlers, &log);
    check(seekHandlers == 1, "异步裁决处理器已注册 (player.seek = 1)");

    // 1) Promise 30 ms 后结算 → patch 生效
    {
      MusicxxExternPluginString out{};
      const auto rc = musicxx_extern_plugin_hook_emit(
          host, viewCP("musicxx.player.seek"),
          viewP(R"({"sid":"async1","fromMs":1000,"toMs":2000})"),
          MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 300, &out, &log);
      const std::string result = take(out);
      std::printf("  [info] async seek result=%s\n", result.c_str());
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "异步裁决钩子派发成功");
      check(jsonIntField(result, "toMs") == "12345",
            "Promise 结算的 patch 生效 (toMs)");
    }

    // 2) Promise 1.5 s 后才结算 → 宿主一直等, 裁决照样生效 (没有等待超时)
    {
      MusicxxExternPluginString out{};
      const auto t0 = std::chrono::steady_clock::now();
      const auto rc = musicxx_extern_plugin_hook_emit(
          host, viewCP("musicxx.player.volume"),
          viewP(R"({"sid":"async2","from":0.5,"to":0.9})"),
          MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 300, &out, &log);
      const std::string result = take(out);
      const auto elapsedMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0)
              .count();
      std::printf("  [info] async volume result=%s (%lld ms)\n", result.c_str(),
                  static_cast<long long>(elapsedMs));
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "长 Promise 的派发返回成功");
      check(result.find("\"cancel\"") != std::string::npos,
            "宿主等到 Promise 结算, 裁决生效 (cancel)");
      check(elapsedMs >= 1200, "宿主等满了 Promise 的等待时间");
    }

    // 3) 结算之后可以继续派发 (处理器没有被跳过)
    {
      MusicxxExternPluginString out{};
      const auto rc = musicxx_extern_plugin_hook_emit(
          host, viewCP("musicxx.player.volume"),
          viewP(R"({"sid":"async3","from":0.1,"to":0.2})"),
          MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 300, &out, &log);
      freeStr(out);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "再次派发成功 (处理器未被跳过)");
    }

    // 4) 统计: 三次派发的 Promise 都结算 (没有超时/迟到这两个概念)
    {
      MusicxxExternPluginString out{};
      const auto rc = musicxx_extern_plugin_plugin_call(
          host, viewCP("example_js_async"), viewCP("probe"), viewCP("{}"), 5000, &out,
          &log);
      const std::string probe = take(out);
      std::printf("  [info] async probe=%s\n", probe.c_str());
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "异步裁决对照示例能力调用成功 (probe)");
      check(jsonIntField(probe, "asyncHookSettled") == "3",
            "统计: 三次派发都在结算后拿到结果");
      check(jsonIntField(probe, "runs") == "3",
            "处理器每次都被调用 (未被跳过)");
      check(probe.find("asyncHookTimeouts") == std::string::npos,
            "统计不再有超时字段");
      check(probe.find("asyncHookLateDrops") == std::string::npos,
            "统计不再有迟到丢弃字段");
    }

    // 5) 钩子统计里没有失败 (慢不等于失败)
    {
      MusicxxExternPluginString stats{};
      const auto rc = musicxx_extern_plugin_hook_stats(host, &stats, &log);
      const std::string statsJson = take(stats);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "hook_stats 可读");
      const auto at = statsJson.find("musicxx.player.volume");
      const std::string entry =
          (at == std::string::npos)
              ? std::string{}
              : statsJson.substr(at, statsJson.find(']', at) - at);
      check(entry.find("\"failures\":0") != std::string::npos,
            "慢的异步裁决不计处理器失败");
    }

    // 6) 卸载 → 无残留 (含等待条目)
    {
      check(musicxx_extern_plugin_plugin_unload(
                host, viewCP("example_js_async"), &log) == MUSICXX_EXTERN_PLUGIN_OK,
            "异步裁决对照示例卸载成功");
      int32_t seekAfter = 0;
      int32_t volumeAfter = 0;
      musicxx_extern_plugin_hook_count(host, viewCP("musicxx.player.seek"),
                                       &seekAfter, &log);
      musicxx_extern_plugin_hook_count(host, viewCP("musicxx.player.volume"),
                                       &volumeAfter, &log);
      check(seekAfter == 0 && volumeAfter == 0, "异步裁决对照示例卸载后无钩子残留");
    }
  }

  // 脚本错误安全降级: 非法脚本的插件不应装载成功, 宿主继续可用
  {
    MusicxxExternPluginString loadLog{};
    const auto rc = musicxx_extern_plugin_plugin_load_sync(
        host, viewCP("example_js_broken"), viewCP("{}"), 5000, &loadLog);
    const std::string errText = take(loadLog);
    check(rc != MUSICXX_EXTERN_PLUGIN_OK, "脚本错误的 JS 插件装载失败");
    check(rc != MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT,
          "脚本错误是明确失败而非超时");
    std::printf("  [info] example_js_broken rc=%d log=%s\n", rc, errText.c_str());
    MusicxxExternPluginString listed{};
    check(musicxx_extern_plugin_plugin_list(host, &listed, &log) ==
              MUSICXX_EXTERN_PLUGIN_OK,
          "脚本错误后宿主仍可用");
    freeStr(listed);
  }

  // 插件全部卸载后: UI 项应全部摘除 (无残留)
  {
    MusicxxExternPluginString snap{};
    const auto snapRc = musicxx_extern_plugin_ui_snapshot(host, &snap, &log);
    const std::string snapshot = take(snap);
    check(snapRc == MUSICXX_EXTERN_PLUGIN_OK, "ui_snapshot 可读 (全部卸载后)");
    check(snapshot.find("plugin.example_native.card") == std::string::npos,
          "动态库插件卸载后 UI 项无残留");
    check(snapshot.find("plugin.example_js.card") == std::string::npos,
          "JS 插件卸载后 UI 项无残留");
  }

  // ==================== 入口符号约定 (对应用例 test_entry_symbols)
  // ====================
  //
  // 对照示例 example_native_bad_entry 的库文件导出了 get_info/create/destroy, 但**没有**
  // start/stop。 约定要求 start/stop 成对存在 (create 只构造, start
  // 才是注册事务), 宿主必须在 "查找入口符号"阶段就拒绝装载: 明确失败
  // (不是超时)、有可读原因、不留注册残留。
  {
    MusicxxExternPluginString loadLog{};
    const auto rc = musicxx_extern_plugin_plugin_load_sync(
        host, viewCP("example_native_bad_entry"), viewCP("{}"), 5000, &loadLog);
    const std::string errText = take(loadLog);
    check(rc != MUSICXX_EXTERN_PLUGIN_OK, "缺 start/stop 入口的库被拒绝装载");
    check(rc != MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT,
          "拒绝是明确失败而非挂住超时");
    check(errText.find("入口符号") != std::string::npos,
          "失败原因指向缺失的入口符号");
    std::printf("  [info] bad-entry rc=%d log=%s\n", rc, errText.c_str());

    // 未装载 + 未注册任何钩子 (此时其它插件都已卸载)
    MusicxxExternPluginString listed{};
    const std::string listJson = [&] {
      musicxx_extern_plugin_plugin_list(host, &listed, &log);
      return take(listed);
    }();
    check(listJson.find("example_native_bad_entry") == std::string::npos,
          "缺入口的库不出现在已装载列表");
    int32_t badEntryHooks = -1;
    musicxx_extern_plugin_hook_count(host, viewCP("musicxx.song.changed"),
                                     &badEntryHooks, &log);
    check(badEntryHooks == 0, "缺入口的库没有注册任何钩子");
  }

  // ==================== 多目标打包示例 (lib/<系统>-<架构>/)
  // ====================
  //
  // 示例插件 example_native_multi 的库文件放在 `lib/<构建目标标签>/` 里（SDK 助手按
  // TARGET_TAG auto 摆放）。这里验证：扫描/装载都按当前系统与架构选中分支，并且
  // 装载的确实是那一份构建（插件自报的构建标签与宿主选中的分支一致）。
  {
    const std::string multiItem = scanItemOf(scanJson, "example_native_multi");
    check(!multiItem.empty(), "扫描发现 example_native_multi (多目标包)");
    check(multiItem.find("\"supported\":true") != std::string::npos,
          "多目标包按当前系统/架构选中分支后判定为可用");
    const std::string selectedTag = jsonStringField(multiItem, "target");
    check(!selectedTag.empty(), "多目标包报告了选中的分支标签");
    check(multiItem.find("\"targetEntry\":\"lib/") != std::string::npos,
          "多目标包的 targetEntry 指向分支目录里的库文件");

    MusicxxExternPluginString loadLog{};
    const auto loadRc = musicxx_extern_plugin_plugin_load_sync(
        host, viewCP("example_native_multi"), viewCP("{}"), 10000, &loadLog);
    if (loadRc != MUSICXX_EXTERN_PLUGIN_OK) {
      std::printf("  [info] example_native_multi load rc=%d log=%s\n", loadRc,
                  take(loadLog).c_str());
    } else {
      freeStr(loadLog);
    }
    check(loadRc == MUSICXX_EXTERN_PLUGIN_OK, "多目标包按分支装载成功");

    MusicxxExternPluginString capOut{};
    MusicxxExternPluginString capLog{};
    const auto capRc = musicxx_extern_plugin_plugin_call(
        host, viewCP("example_native_multi"),
        viewCP("plugin.example_native_multi.which"), viewCP("{}"), 5000, &capOut,
        &capLog);
    freeStr(capLog);
    const std::string cap = take(capOut);
    check(capRc == MUSICXX_EXTERN_PLUGIN_OK, "多目标包的能力可调用");
    check(jsonStringField(cap, "tag") == selectedTag && !selectedTag.empty(),
          "装载的正是宿主选中的分支 (插件自报构建标签与分支标签一致)");

    MusicxxExternPluginString unloadLog{};
    musicxx_extern_plugin_plugin_unload(host, viewCP("example_native_multi"),
                                        &unloadLog);
    freeStr(unloadLog);
  }

  // ==================== 处理器失败只记统计 (不再暂停派发)
  // ====================
  //
  // 对照示例 example_native_fail 注册两个处理器:
  //   musicxx.song.changed     → 每次失败 (返回非 0)
  //   musicxx.player.completed → 每次成功
  // 期望: 失败只累计到统计 (hook_stats 的 failures), 处理器每次都照常被调用;
  // 没有"连续失败暂停派发"这回事, 插件也不会被卸载。
  {
    MusicxxExternPluginString loadLog{};
    const auto loadRc = musicxx_extern_plugin_plugin_load_sync(
        host, viewCP("example_native_fail"), viewCP("{}"), 8000, &loadLog);
    if (loadRc != MUSICXX_EXTERN_PLUGIN_OK) {
      std::printf("  [info] example_native_fail load rc=%d log=%s\n", loadRc,
                  take(loadLog).c_str());
    } else {
      freeStr(loadLog);
    }
    check(loadRc == MUSICXX_EXTERN_PLUGIN_OK, "失败对照示例装载成功 (example_native_fail)");

    int32_t fixtureHooks = -1;
    musicxx_extern_plugin_hook_count(host, viewCP("musicxx.song.changed"),
                                     &fixtureHooks, &log);
    check(fixtureHooks == 1, "失败处理器的处理器数为 1 (其余插件已卸载)");

    // 连续 5 次派发: 每次都真的调用了处理器 (called=1), 不做任何跳过
    for (int i = 1; i <= 5; ++i) {
      MusicxxExternPluginString out{};
      const auto rc = musicxx_extern_plugin_hook_emit(
          host, viewCP("musicxx.song.changed"),
          viewP(R"({"sid":"brk","song":{"name":"失败目标"}})"),
          MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 200, &out, &log);
      const std::string result = take(out);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "失败处理器派发可调用");
      check(jsonIntField(result, "called") == "1",
            "失败处理器每次都被调用 (不跳过)");
    }

    // 处理器级明细来自钩子统计接口 `hook_stats`: 只有次数与失败次数
    {
      MusicxxExternPluginString stats{};
      const auto statsRc = musicxx_extern_plugin_hook_stats(host, &stats, &log);
      const std::string statsJson = take(stats);
      check(statsRc == MUSICXX_EXTERN_PLUGIN_OK, "hook_stats 可读 (失败统计)");
      check(statsJson.find("\"calls\":5") != std::string::npos,
            "失败处理器被调用 5 次 (每次都派发)");
      check(statsJson.find("\"failures\":5") != std::string::npos,
            "失败次数累计为 5");
      check(statsJson.find("\"paused\"") == std::string::npos,
            "hook_stats 不再有暂停派发标记");
      check(statsJson.find("\"failures\":0") != std::string::npos,
            "同插件的正常处理器无失败记录");
    }

    // 聚合统计 (宿主/插件维度) 仍然可读: 记录观测数据, 不做任何惩罚
    {
      MusicxxExternPluginString stats{};
      const auto statsRc =
          musicxx_extern_plugin_stats(host, nullptr, &stats, &log);
      const std::string statsJson = take(stats);
      check(statsRc == MUSICXX_EXTERN_PLUGIN_OK, "stats 可读 (失败统计)");
      check(statsJson.find("example_native_fail") != std::string::npos,
            "聚合统计含失败对照示例插件");
      check(statsJson.find("\"calls\":") != std::string::npos,
            "聚合统计含钩子调用次数");
    }

    // 同一插件的另一个处理器照常工作
    {
      MusicxxExternPluginString out{};
      const auto rc = musicxx_extern_plugin_hook_emit(
          host, viewCP("musicxx.player.completed"),
          viewP(R"({"sid":"brk","playedMs":1000})"),
          MUSICXX_EXTERN_PLUGIN_HOOK_SYNC, 200, &out, &log);
      const std::string result = take(out);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "同插件的另一个处理器派发成功");
      check(jsonIntField(result, "called") == "1", "另一个处理器不受影响");
    }

    // 插件本身仍在装载状态
    {
      MusicxxExternPluginString probe{};
      const auto probeRc = musicxx_extern_plugin_plugin_call(
          host, viewCP("example_native_fail"), viewCP("plugin.example_native_fail.probe"),
          viewCP("{}"), 3000, &probe, &log);
      const std::string probeJson = take(probe);
      check(probeRc == MUSICXX_EXTERN_PLUGIN_OK,
            "失败之后插件仍可响应能力调用 (未被卸载)");
      check(jsonIntField(probeJson, "failingCalls") == "5",
            "失败处理器被调用 5 次 (全部派发)");
      check(jsonIntField(probeJson, "goodCalls") == "1",
            "正常处理器按预期被调用 1 次");
    }

    // 卸载后无残留
    {
      check(musicxx_extern_plugin_plugin_unload(
                host, viewCP("example_native_fail"), &log) == MUSICXX_EXTERN_PLUGIN_OK,
            "失败对照示例卸载成功");
      int32_t after = -1;
      musicxx_extern_plugin_hook_count(host, viewCP("musicxx.song.changed"),
                                       &after, &log);
      check(after == 0, "失败对照示例卸载后无钩子残留");
    }
  }

  // ==================== 变量表 (musicxx.vars) ====================
  {
    // 能力位: 本次构建必须带变量通道
    {
      const int32_t bits = musicxx_extern_plugin_feature_bits();
      check((bits & MUSICXX_EXTERN_PLUGIN_FEATURE_VARS) != 0,
            "feature_bits 含变量通道");
      check((bits & MUSICXX_EXTERN_PLUGIN_FEATURE_HOOKS) != 0,
            "feature_bits 含钩子通道");
    }

    // 声明之前的官方键: 列出为空
    {
      MusicxxExternPluginString out{};
      const int32_t rc = musicxx_extern_plugin_var_list(
          host, viewCP("musicxx."), &out, &log);
      const std::string json = take(out);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "var_list 可读 (声明之前)");
      check(json == "[]", "声明之前没有官方变量");
    }

    // 未声明的键: 读/写都要明确报"未找到"
    {
      MusicxxExternPluginString out{};
      const int32_t readRc = musicxx_extern_plugin_var_get(
          host, viewCP("musicxx.test.missing"), &out, &log);
      take(out);
      check(readRc == MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND,
            "读未声明的官方键返回未找到 (插件据此知道要先声明)");
      MusicxxExternPluginString setOut{};
      const int32_t writeRc = musicxx_extern_plugin_var_set(
          host, viewCP("musicxx.test.missing"), viewCP("1"), &setOut, &log);
      take(setOut);
      check(writeRc == MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND,
            "写未声明的官方键返回未找到");
    }

    // 应用声明两个变量 (一个读写通知齐全, 一个只写)
    {
      const std::string items = R"([
        {"key":"musicxx.test.value","caps":["get","set","notify"],"type":"string",
         "options":["a","b"],"value":"a","title":"测试值","depict":"测试用","risk":"low"},
        {"key":"musicxx.test.onlyWrite","caps":["set"],"type":"string"}
      ])";
      MusicxxExternPluginString message{};
      const int32_t rc = musicxx_extern_plugin_var_declare(host, viewP(items),
                                                          &message);
      const std::string text = take(message);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "var_declare 受理");
      check(text.empty(), "合法声明不产生拒绝日志");
    }

    // 列出来: 两条都在, caps/type/options 如实
    {
      MusicxxExternPluginString out{};
      musicxx_extern_plugin_var_list(host, viewCP("musicxx.test."), &out, &log);
      const std::string json = take(out);
      check(json.find("musicxx.test.value") != std::string::npos,
            "var_list 列出读写的官方键");
      check(json.find("musicxx.test.onlyWrite") != std::string::npos,
            "var_list 列出只写的官方键");
      check(json.find("\"notify\"") != std::string::npos,
            "var_list 如实给出能力位");
      check(json.find("\"options\"") != std::string::npos,
            "var_list 带上候选值");
      check(json.find("\"hasValue\":true") != std::string::npos,
            "var_list 如实标记有没有缓存值");
      check(json.find("\"valueMs\"") != std::string::npos,
            "var_list 带上缓存时刻 (ageMs/stale 由它算)");
    }

    // 声明里不合法的项: 只记日志 (整体仍然返回 0, 不影响启动)
    {
      const std::string bad = R"([
        {"key":"plugin.other.foo","caps":["get"]},
        {"key":"musicxx.test.noCaps","caps":[]},
        {"caps":["get"]}
      ])";
      MusicxxExternPluginString message{};
      const int32_t rc =
          musicxx_extern_plugin_var_declare(host, viewP(bad), &message);
      const std::string text = take(message);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "非法声明不导致整体失败");
      check(!text.empty(), "非法声明在日志里说明原因");
      MusicxxExternPluginString out{};
      musicxx_extern_plugin_var_list(host, viewCP("musicxx.test.noCaps"), &out,
                                     &log);
      check(take(out) == "[]", "非法声明没有建出变量");
    }

    // 推值: 值变了才通知, 值没变不产生事件
    {
      MusicxxExternPluginString out{};
      const int32_t rc = musicxx_extern_plugin_var_update(
          host, viewCP("musicxx.test.value"), viewCP(R"("b")"), &log);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "var_update 受理");
      MusicxxExternPluginString listed{};
      musicxx_extern_plugin_var_list(host, viewCP("musicxx.test.value"), &listed,
                                     &log);
      const std::string json = take(listed);
      check(json.find("\"value\":\"b\"") != std::string::npos,
            "推值后列表里是缓存的最新值");
      check(json.find("\"revision\":1") != std::string::npos,
            "推值让修订号前进一步");

      // 值没变: 再来一次, revision 不动 (不产生通知)
      MusicxxExternPluginString again{};
      musicxx_extern_plugin_var_update(host, viewCP("musicxx.test.value"),
                                       viewCP(R"("b")"), &log);
      musicxx_extern_plugin_var_list(host, viewCP("musicxx.test.value"), &again,
                                     &log);
      check(take(again).find("\"revision\":1") != std::string::npos,
            "值没变时不涨修订号 (也就不通知)");
    }

    // 非法值: 拒绝 (不写入值、不通知)
    {
      MusicxxExternPluginString message{};
      const int32_t rc = musicxx_extern_plugin_var_update(
          host, viewCP("musicxx.test.value"), viewCP("{不是 JSON"), &message);
      check(rc != MUSICXX_EXTERN_PLUGIN_OK, "非法 JSON 值被拒绝");
    }

    // 批量推值
    {
      const std::string items = R"([
        {"key":"musicxx.test.value","value":"a"},
        {"key":"musicxx.test.onlyWrite","value":"x"}
      ])";
      const int32_t rc = musicxx_extern_plugin_var_update_batch(
          host, viewP(items), &log);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "var_update_batch 受理");
      MusicxxExternPluginString listed{};
      musicxx_extern_plugin_var_list(host, viewCP("musicxx.test.value"), &listed,
                                     &log);
      check(take(listed).find("\"value\":\"a\"") != std::string::npos,
            "批量推值逐个生效");
    }

    // 关心数回传: 声明/订阅变化时会推给应用 (没人关心时也会推 0)
    {
      MusicxxExternPluginString events{};
      musicxx_extern_plugin_poll_events(host, 500, &events, &log);
      const std::string json = take(events);
      check(json.find("musicxx.var.subscriptions") != std::string::npos,
            "关心数变化回传给应用 (键与数量)");
    }

    // 应用订阅插件键: 订阅一个还不存在的键也能受理 (键出现时自动挂)
    {
      const std::string keys = R"(["plugin.nobody.tip","plugin."])";
      const int32_t rc = musicxx_extern_plugin_var_subscribe(host, viewP(keys),
                                                              &log);
      check(rc == MUSICXX_EXTERN_PLUGIN_OK, "var_subscribe 受理 (含前缀写法)");
      const int32_t unRc = musicxx_extern_plugin_var_unsubscribe(
          host, viewP(R"(["plugin.nobody.tip"])"), &log);
      check(unRc == MUSICXX_EXTERN_PLUGIN_OK, "var_unsubscribe 受理");
    }

    // 调试信息里能看到变量表规模 (排障用)
    {
      MusicxxExternPluginString debug{};
      musicxx_extern_plugin_debug_info(host, &debug, &log);
      const std::string json = take(debug);
      check(json.find("\"vars\"") != std::string::npos,
            "调试信息含变量表");
      check(json.find("\"appVars\"") != std::string::npos,
            "调试信息区分官方键与插件键");
    }
  }

  check(musicxx_extern_plugin_host_stop(host, 5000, &log) ==
            MUSICXX_EXTERN_PLUGIN_OK,
        "host_stop");
  check(musicxx_extern_plugin_host_stop(host, 1000, &log) ==
            MUSICXX_EXTERN_PLUGIN_OK,
        "host_stop 幂等");
  musicxx_extern_plugin_host_destroy(host);

  std::printf("\nchecks=%d failed=%d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
