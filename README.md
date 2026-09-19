# musicxx_extern_plugin

musicxx 的**外部插件框架**（原生宿主库 + 插件 SDK + 示例插件 + 原生测试）。
设计文档：`<musicxx 仓库>/resource/history/extern-plugin-impl/plan.md`（§3 仓库与包结构、§4 原生宿主、§6 C ABI、§11 构建打包）。

## 目录

```
src/CMakeLists.txt   顶层 superbuild：XX_IS_* 平台/编译器宏 → BoostConfig.cmake → ExternalProject_Add 构建依赖 → 构建宿主工程
src/host/            宿主工程（嵌套构建，CMakeLists.txt 里用 find_package 取依赖）
  include/           Dart ⇄ 原生 C ABI v1（ffigen 入口，唯一导出契约）
  sdk/include/       插件作者 SDK（musicxx/plugin/api/*；插件只依赖头文件）
  tests/             原生测试（不依赖 Dart）
  third_party/       依赖子模块（cxx_pluginxx / cxx_utilxx_base / fmt / yaml-cpp / simdjson / libiconv-native / uchardet / quickjs）
example/example_native/ 示例插件（C++，演示钩子/状态镜像/日志）
tools/               build_native.ps1（环境准备 + 调 cmake）、check_submodules.ps1（子模块检查）、cmake/BoostConfig.cmake.in
.native/             本地构建产物（构建目录 / 安装前缀 / 便携输出 / Boost 缓存，**全部可重建，不入版本库**）
```

## 依赖管理

依赖来源只有两处，**不需要任何预编译库**：

1. **git submodule**（`src/third_party/*`；每个子模块的 commit 由外层仓库的 gitlink 记录，不需要额外的版本清单文件）

   | 子模块 | 作用 |
   |---|---|
   | `cxx_pluginxx` | 插件框架内核（装载/清单/生命周期/通用接口表/SDK） |
   | `cxx_utilxx_base` | 日志/JSON/字符串/取消令牌/系统探测 |
   | `fmt`、`yaml-cpp`、`simdjson` | 格式化、`plugin.yaml` 解析、JSON 后端 |
   | `libiconv-native`、`uchardet` | 字符编码转换与探测（`cxx_utilxx_base` 的 `string_util` **无条件**使用 `<iconv.h>` 与 uchardet，不能靠开关关掉） |
   | `quickjs` | 已登记版本，供 M3（JS 插件运行时）使用，当前不编译 |

   ```powershell
   git submodule update --init --recursive             # 首次拉取
   pwsh -NoProfile -File tools/check_submodules.ps1     # 检查是否都已初始化 (commit 由 git 锁定)
   ```

2. **Boost 头文件**（只用 Boost.Asio 头，不链接任何 Boost 编译库）
   - 本机已有 `src/third_party/boost/include`（含 `boost/`，约 151 MB，不入版本库）时脚本直接复用；
   - 缺失时按 `tools/build_native.ps1` 顶部锁定的 **版本 + URL + SHA256** 下载官方发布包，
     只解出 `boost/` 头文件子树到 `.native/boost/`；
   - `<安装前缀>/lib/cmake/Boost-<版本>/BoostConfig.cmake` 由顶层 CMake 依 `tools/cmake/BoostConfig.cmake.in`
     生成（只暴露头文件根 `Boost_INCLUDE_DIRS` 与 `Boost::headers`），不依赖 b2 生成的整套配置；
   - 可用 `-BoostRoot <含 boost/ 的目录>` 或 `-BoostArchive <发布包>` 覆盖（离线/自备包）。

## 构建

结构仿照 agentxx（`agent/CMakeLists.txt` + `agent/script/windows_*_build.bat`）：

```text
tools/build_native.ps1            环境预检 (cmake/git/VS) + UTF-8 控制台/关闭 vcpkg/MSVC 英文输出 + Boost 头文件准备
  └─ cmake -S src -B .native/build/<平台>-<配置> -G <生成器> -A <平台> \
           -DXX_IS_RELEASE_D=<0|1> -DCMAKE_BUILD_TYPE=<配置> -DCMAKE_CONFIGURATION_TYPES=<配置> \
           -DMUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE=<含 boost/ 的目录>
       └─ src/CMakeLists.txt (superbuild)
            XX_IS_* 宏 + BoostConfig.cmake + _COMMON_CMAKE_ARGS / _COMMON_CMAKE_CACHE_ARGS
            ExternalProject_Add: fmt → yaml-cpp → simdjson → libiconv-native → uchardet
                                 → cxx_utilxx_base → cxx_pluginxx
            ExternalProject_Add: musicxx_extern_plugin_host_repo (src/host, 内部 find_package 取依赖)
```

```powershell
# Release 全量（依赖 + 宿主），产物装到 <构建目录>/musicxx-extern-plugin-install 并复制到 .native/output/<平台>-<架构>-<配置>/
pwsh -NoProfile -File tools/build_native.ps1

# 常用开关
pwsh -NoProfile -File tools/build_native.ps1 -Config Debug       # Debug (独立构建目录)
pwsh -NoProfile -File tools/build_native.ps1 -DepsOnly           # 只构建依赖库 (target cxx_pluginxx_repo)
pwsh -NoProfile -File tools/build_native.ps1 -ConfigureOnly      # 只做预检/Boost/configure
pwsh -NoProfile -File tools/build_native.ps1 -Clean              # 先清空构建目录与输出目录
pwsh -NoProfile -File tools/build_native.ps1 -RunTests           # 构建后跑原生测试
pwsh -NoProfile -File tools/build_native.ps1 -Jobs 8             # 指定并行度
```

要点：

- **安装前缀**（`<构建目录>/musicxx-extern-plugin-install`）是唯一的依赖来源，里面同时有
  `lib/cmake/*` 供 `find_package` 使用、`bin/` 宿主产物、`plugins/` 示例插件；宿主工程找不到依赖时
  会直接提示"先跑 `tools/build_native.ps1`"。
- **一个构建目录一个配置**：Release/Debug 分别用 `.native/build/windows-release`、`.native/build/windows-debug`，
  安装前缀在构建目录内，因此不会混入其它配置的产物；脚本会按"配置 + 生成器 + 平台 + Boost 位置"签名，
  变化时自动重置构建目录（避免 CMake 缓存残留上一次的参数）。
- **依赖构建目录用短路径**（`<构建目录>/e/<依赖短名>`）：Windows 的 `MAX_PATH=260`，
  ExternalProject 默认的 `<项目名>-prefix/src/<项目名>-build` 叠加 `CMakeFiles/CMakeScratch/TryCompile-*/…/*.tlog`
  会超限并触发 MSBuild `FTK1011`。
- **嵌套构建不传 `CMAKE_CONFIGURATION_TYPES`**：嵌套 cmake 的编译器探测固定用 `Debug` 配置，
  收窄可用配置会让探测失败；构建配置由 ExternalProject 自动带上 `--config <顶层配置>`。
- **uchardet** 必须 `-DBUILD_BINARY=OFF`（其命令行工具依赖 Windows 没有的 `getopt.h`）。
- **MSVC 必须带 `/utf-8`**（源码头文件含中文注释，否则按代码页 936 解析会破坏换行）；CMake 已在各目标上设置。

产物（`-Config Release`）：

```
<构建目录>/musicxx-extern-plugin-install/bin/musicxx_extern_plugin.dll        宿主库（只导出 musicxx_extern_plugin_*）
<构建目录>/musicxx-extern-plugin-install/bin/musicxx_extern_plugin_test.exe   原生测试
<构建目录>/musicxx-extern-plugin-install/plugins/example_native/              示例插件（库文件 + plugin.yaml）
.native/output/windows-x64-release/{bin,plugins}/                            上述产物的稳定复制（供打包/手工取用）
```

校验导出面与运行期依赖：

```powershell
dumpbin /exports   .native/output/windows-x64-release/bin/musicxx_extern_plugin.dll   # 仅 musicxx_extern_plugin_*
dumpbin /dependents .native/output/windows-x64-release/bin/musicxx_extern_plugin.dll  # 仅系统 DLL（无第三方 DLL）
```

## 原生测试

宿主按「父目录下的每个子目录 = 一个插件」扫描，因此测试参数要传**插件目录的父目录**：

```powershell
pwsh -NoProfile -File tools/build_native.ps1 -RunTests      # 自动用 <安装前缀>/plugins 作为插件目录
```

手工运行：

```powershell
<构建目录>/musicxx-extern-plugin-install/bin/musicxx_extern_plugin_test.exe `
    <构建目录>/musicxx-extern-plugin-install/plugins
```

## 当前状态

见 musicxx 仓库 `resource/history/extern-plugin-impl/work.md`：
构建链路（从 `src/third_party` 源码到宿主库）已跑通；原生测试中「插件装载 + `host_stop` 收尾」仍待修（work.md §4.3）。
