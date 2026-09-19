# musicxx_extern_plugin

musicxx 的**外部插件框架**（原生宿主库 + 插件 SDK + 示例插件 + 原生测试）。
设计文档：`<musicxx 仓库>/resource/history/extern-plugin-impl/plan.md`（§3 仓库与包结构、§4 原生宿主、§6 C ABI、§11 构建打包）。

## 目录

```
src/include/     Dart ⇄ 原生 C ABI v1（ffigen 入口，唯一导出契约）
src/host/        宿主实现：宿主线程/事件队列/状态镜像/动作路由/领域表(musicxx.hooks、musicxx.host)/统计
src/sdk/include/ 插件作者 SDK（musicxx/plugin/api/*；插件只依赖头文件）
src/tests/       原生测试（不依赖 Dart）
src/third_party/ 依赖子模块（cxx_pluginxx / cxx_utilxx_base / fmt / yaml-cpp / simdjson / libiconv-native / uchardet / quickjs）
example/example_native/ 示例插件（C++，演示钩子/状态镜像/日志）
tools/           build_native.ps1（依赖与宿主构建）、check_submodules.ps1（依赖版本校验）、cmake/（Boost 配置模板）
.native/         本地构建产物：deps/<平台>-<架构> 依赖前缀、build/ 构建目录、downloads/ Boost 发布包缓存（**全部可由脚本重建，不入版本库**）
```

## 依赖管理

依赖来源只有两处，**不需要任何预编译库**：

1. **git submodule**（版本锁定记录在 `third_party_versions.json`：URL + commit）

   | 子模块 | 作用 |
   |---|---|
   | `cxx_pluginxx` | 插件框架内核（装载/清单/生命周期/通用接口表/SDK） |
   | `cxx_utilxx_base` | 日志/JSON/字符串/取消令牌/系统探测 |
   | `fmt`、`yaml-cpp`、`simdjson` | 格式化、`plugin.yaml` 解析、JSON 后端 |
   | `libiconv-native`、`uchardet` | 字符编码转换与探测（`cxx_utilxx_base` 的 `string_util` **无条件**使用 `<iconv.h>` 与 uchardet，不能靠开关关掉） |
   | `quickjs` | 已登记版本，供 M3（JS 插件运行时）使用，当前不编译 |

   ```powershell
   git submodule update --init --recursive          # 首次拉取
   pwsh -NoProfile -File tools/check_submodules.ps1 # 校验 URL 与 commit 是否与记录一致
   ```

2. **Boost 头文件**（只用 Boost.Asio 头，不链接任何 Boost 编译库）
   - 本机已有 `src/third_party/boost/include`（含 `boost/`，约 151 MB，不入版本库）时脚本直接复用；
   - 缺失时按 `third_party_versions.json` 里锁定的 `version` / `source_url` / `archive_sha256`
     下载官方发布包并**只解出 `boost/` 头文件子树**到 `.native/boost/boost_<下划线版本>/`；
   - `<依赖前缀>/lib/cmake/Boost-<版本>/BoostConfig.cmake` 由脚本依据 `tools/cmake/BoostConfig.cmake.in`
     生成（只暴露头文件根 `Boost_INCLUDE_DIRS` 与 `Boost::headers`），因此不依赖 b2 生成的整套配置；
   - 可用 `-BoostRoot <含 boost/ 的目录>` 或 `-BoostArchive <发布包>` 覆盖（离线/自备包）。

## 构建

```powershell
# 阶段一（依赖）+ 阶段二（宿主/示例/测试）
pwsh -NoProfile -File tools/build_native.ps1

# 常用开关
pwsh -NoProfile -File tools/build_native.ps1 -DepsOnly      # 只构建依赖
pwsh -NoProfile -File tools/build_native.ps1 -HostOnly      # 只构建宿主（依赖已在先前缀）
pwsh -NoProfile -File tools/build_native.ps1 -PrepareOnly   # 只做子模块预检与 Boost 准备
pwsh -NoProfile -File tools/build_native.ps1 -Clean         # 先清空依赖前缀与构建目录（从零验证）
pwsh -NoProfile -File tools/build_native.ps1 -RunTests      # 构建后跑原生测试
pwsh -NoProfile -File tools/build_native.ps1 -Jobs 8        # 指定并行度
```

依赖按顺序构建并安装到 `.native/deps/<平台>-<架构>`：

```text
fmt → yaml-cpp → simdjson → libiconv-native → uchardet → cxx_utilxx_base → cxx_pluginxx
```

要点：

- **依赖前缀**（`{include,lib,cmake}` 布局）是唯一依赖来源；`src/CMakeLists.txt` 默认取
  `.native/deps/<平台>-<架构>`，找不到依赖时直接报错并提示先运行本脚本。
- **构建目录带签名**：前缀/Boost 位置/生成器变化时脚本会自动重置对应构建目录，
  避免 CMake 缓存里的 `<包>_DIR` 继续指向旧前缀（手工改前缀而不清缓存是常见的踩坑点）。
- **uchardet** 必须 `-DBUILD_BINARY=OFF`（其命令行工具依赖 Windows 没有的 `getopt.h`）。
- **MSVC 必须带 `/utf-8`**（源码头文件含中文注释，否则按代码页 936 解析会破坏换行）；CMake 已在各目标上设置。

产物：

```
.native/build/host/Release/musicxx_extern_plugin.dll       宿主库（只导出 musicxx_extern_plugin_*）
.native/build/host/example_native/Release/example_native.dll  示例插件（只导出 musicxx_plugin_*）
.native/build/host/Release/musicxx_extern_plugin_test.exe  原生测试
```

校验导出面与运行期依赖：

```powershell
dumpbin /exports   .native/build/host/Release/musicxx_extern_plugin.dll   # 仅 musicxx_extern_plugin_*
dumpbin /dependents .native/build/host/Release/musicxx_extern_plugin.dll  # 仅系统 DLL（无第三方 DLL）
```

## 原生测试

宿主按「父目录下的每个子目录 = 一个插件」扫描，因此测试参数要传**插件目录的父目录**：

```powershell
pwsh -NoProfile -File tools/build_native.ps1 -RunTests      # 自动准备插件目录并运行
```

手工运行：

```powershell
mkdir .native/build/host/test-plugins/example_native
cp .native/build/host/example_native/Release/example_native.dll .native/build/host/test-plugins/example_native/
cp example/example_native/plugin.yaml                            .native/build/host/test-plugins/example_native/
.native/build/host/Release/musicxx_extern_plugin_test.exe .native/build/host/test-plugins
```

## 当前状态

见 musicxx 仓库 `resource/history/extern-plugin-impl/work.md`：
构建链路（从 `src/third_party` 源码到宿主库）已跑通；原生测试中「插件装载 + `host_stop` 收尾」仍待修（work.md §4.3）。
