# musicxx_extern_plugin

musicxx 的**外部插件框架**（原生宿主库 + 插件 SDK + 示例插件 + 原生测试）。
设计文档：`<musicxx 仓库>/resource/history/extern-plugin-impl/plan.md`（§3 仓库与包结构、§4 原生宿主、§6 C ABI、§11 构建打包）。

## 目录

```
src/CMakeLists.txt   顶层 superbuild：XX_IS_* 平台/编译器宏 → BoostConfig.cmake → ExternalProject_Add 构建依赖 → 构建宿主工程
src/host/            宿主工程（嵌套构建，CMakeLists.txt 里用 find_package 取依赖）
  include/           Dart ⇄ 原生 C ABI v1（ffigen 入口，唯一导出契约）
  sdk/include/       插件作者 SDK（musicxx/plugin/api/*；插件只依赖头文件）
                     其中 hook_ids.g.h 由 tools/gen_contract.dart 生成（钩子 id 与已知钩子表）
  tests/             原生测试（不依赖 Dart；`plugins/` 是 JS 夹具、`fixtures/` 是原生夹具插件）
  third_party/       依赖子模块（cxx_pluginxx / cxx_utilxx_base / fmt / yaml-cpp / simdjson / libiconv-native / uchardet / quickjs）
lib/                 Dart 侧（FFI 绑定 + 运行时/管理器/钩子/状态/动作/声明式 UI 模型；
                     bindings_generated.dart 与 hook_ids.g.dart 为生成物）
test/                Dart 侧测试（ui_model_test.dart 纯模型单测；host_smoke_test.dart 对真实原生库做端到端冒烟）
plugins/            官方插件与示例（每个子目录一个插件，插件 id 取清单 name；见该目录 README）
  example_native/   示例插件（C++，演示钩子/状态镜像/日志/能力/事件订阅/声明式 UI）
  example_js/       示例插件（JS，零编译；与 native 版行为等价）
src/host/js/            JS 插件运行时（QuickJS）：共享 JS 线程 + js:<pluginId> 合成内置实例 + musicxx API 面
docs/plugin-hooks.md 钩子总表（插件作者文档，生成物）
docs/plugin-js-api.md JS 插件作者指南（目录结构/生命周期/musicxx API/硬约束/排障/v1 边界）
docs/plugin-native-api.md 原生插件作者指南（SDK 用法/构建模板/线程纪律/权限/部署与排障）
tools/               build_native.ps1（Windows：环境准备 + 调 cmake）、build_native.sh（Linux/macOS：同一套流程）、
                     gen_contract.dart（契约生成/校验）、check_submodules.ps1（子模块检查）、
                     smoke_dart.dart（纯 Dart 冒烟，定位 FFI 卡点）、cmake/BoostConfig.cmake.in
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
   | `quickjs` | JS 插件运行时（`src/host/js/`），编入宿主库；不构建它自带的命令行工具 |

   > JS 插件是**零编译**形态：一个目录（`plugin.yaml` + `plugin.js`）即可，宿主把它装成内置实例 `js:<pluginId>`。
   > 关闭方式：`-DMUSICXX_EXTERN_PLUGIN_ENABLE_JS=OFF`（或去掉 quickjs 子模块，构建会自动跳过并打印警告）。

   ```powershell
   git submodule update --init --recursive             # 首次拉取
   pwsh -NoProfile -File tools/check_submodules.ps1     # 检查是否都已初始化 (commit 由 git 锁定)
   ```

2. **Boost 头文件**（只用 Boost.Asio 头，不链接任何 Boost 编译库）
   - 本机已有 `src/third_party/boost/include`（含 `boost/`，约 151 MB，不入版本库）时脚本直接复用；
   - 缺失时按构建脚本顶部锁定的 **版本 + URL + SHA256** 下载官方发布包
     （`tools/build_native.ps1` 与 `tools/build_native.sh` 里的常量保持一致），只解出 `boost/` 头文件子树到 `.native/boost/`；
   - `<安装前缀>/lib/cmake/Boost-<版本>/BoostConfig.cmake` 由顶层 CMake 依 `tools/cmake/BoostConfig.cmake.in`
     生成（只暴露头文件根 `Boost_INCLUDE_DIRS` 与 `Boost::headers`），不依赖 b2 生成的整套配置；
   - 可用 `-BoostRoot <含 boost/ 的目录>`/`--boost-root <目录>` 或 `-BoostArchive <发布包>`/`--boost-archive <包>` 覆盖（离线/自备包）。

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

Linux / macOS 用同一套流程的 shell 版本（选项名不区分大小写，`--deps-only` 与 `-DepsOnly` 等价）：

```bash
./tools/build_native.sh                       # Release 全量 (依赖 + 宿主)
./tools/build_native.sh --config Debug        # Debug (独立构建目录 .native/build/linux-debug)
./tools/build_native.sh --deps-only           # 只构建依赖库
./tools/build_native.sh --configure-only      # 只做预检/Boost/configure
./tools/build_native.sh --clean               # 先清空构建目录与输出目录
./tools/build_native.sh --run-tests           # 构建后跑原生测试 (失败时脚本返回非 0)
./tools/build_native.sh --jobs 8              # 指定并行度
```

要点：

- **工具链要求**（内核是 C++26）：MSVC ≥ 19.4x（VS 17.14+/VS 18）、GCC ≥ 14、Clang/NDK ≥ 18；
  Linux 上还需要 `pkg-config`（`cxx_utilxx_base` / `cxx_pluginxx` 配置阶段有 `find_package(PkgConfig REQUIRED)`）；
  shell 脚本会在预检里检查编译器大版本与 `pkg-config`，不满足时给出警告。
- **安装前缀**（`<构建目录>/musicxx-extern-plugin-install`）是唯一的依赖来源，里面同时有
  `lib/cmake/*` 供 `find_package` 使用、`bin/` 宿主产物、`plugins/` 示例插件；宿主工程找不到依赖时
  会直接提示"先跑构建脚本"。
- **一个构建目录一个配置**：Release/Debug 分别用 `.native/build/<平台>-release`、`.native/build/<平台>-debug`，
  安装前缀在构建目录内，因此不会混入其它配置的产物；脚本会按"配置 + 生成器 + 平台/架构 + Boost 位置"签名，
  变化时自动重置构建目录（避免 CMake 缓存残留上一次的参数）。
- **依赖构建目录用短路径**（`<构建目录>/e/<依赖短名>`）：Windows 的 `MAX_PATH=260`，
  ExternalProject 默认的 `<项目名>-prefix/src/<项目名>-build` 叠加 `CMakeFiles/CMakeScratch/TryCompile-*/…/*.tlog`
  会超限并触发 MSBuild `FTK1011`。
- **嵌套构建不传 `CMAKE_CONFIGURATION_TYPES`**：嵌套 cmake 的编译器探测固定用 `Debug` 配置，
  收窄可用配置会让探测失败；构建配置由 ExternalProject 自动带上 `--config <顶层配置>`。
- **uchardet** 必须 `-DBUILD_BINARY=OFF`（其命令行工具依赖 Windows 没有的 `getopt.h`）。
- **MSVC 必须带 `/utf-8`**（源码头文件含中文注释，否则按代码页 936 解析会破坏换行）；CMake 已在各目标上设置。
- **导出面收口**：宿主库只导出 `musicxx_extern_plugin_*` —— MSVC 靠导出宏；GNU/Clang 另加
  version script（`global: musicxx_extern_plugin_*; local: *;`，只靠 `-fvisibility=hidden` 挡不住静态库
  带进来的 `STB_GNU_UNIQUE` 符号，实测 Boost.Asio 的 error category 会漏出来）；Apple 由 hidden
  visibility 覆盖。

产物（Release）：

```
# Windows
<构建目录>/musicxx-extern-plugin-install/bin/musicxx_extern_plugin.dll        宿主库（只导出 musicxx_extern_plugin_*）
<构建目录>/musicxx-extern-plugin-install/bin/musicxx_extern_plugin_test.exe   原生测试
<构建目录>/musicxx-extern-plugin-install/plugins/example_native/              示例插件（库文件 + plugin.yaml）
.native/output/windows-x64-release/{bin,plugins}/                            上述产物的稳定复制（供打包/手工取用）

# Linux / macOS（库名按平台：lib*.so / lib*.dylib，可执行文件无扩展名）
<构建目录>/musicxx-extern-plugin-install/bin/libmusicxx_extern_plugin.so     宿主库
<构建目录>/musicxx-extern-plugin-install/bin/musicxx_extern_plugin_test       原生测试
.native/output/linux-x64-release/{bin,plugins}/                              上述产物的稳定复制
```

校验导出面与运行期依赖：

```powershell
# Windows
dumpbin /exports   .native/output/windows-x64-release/bin/musicxx_extern_plugin.dll   # 仅 musicxx_extern_plugin_*
dumpbin /dependents .native/output/windows-x64-release/bin/musicxx_extern_plugin.dll  # 仅系统 DLL（无第三方 DLL）
```

```bash
# Linux
nm -D --defined-only .native/output/linux-x64-release/bin/libmusicxx_extern_plugin.so   # 仅 musicxx_extern_plugin_*（37 个）
ldd .native/output/linux-x64-release/bin/libmusicxx_extern_plugin.so                     # 只有 libc/libstdc++/libgcc_s/libm
```

> 分发到 Linux 桌面时注意：宿主库的 glibc 需求取决于**构建机**（本仓实测 Ubuntu 22.04 上为 `GLIBC_2.35`），
> 且运行期会用到应用自带的 `libstdc++`（`resource/libs/linux/lib/`）。发布用的 Linux 产物建议在
> 目标发行版（或更老的发行版）上构建，并在 `flutter build linux` 后核对 `bundle/lib/libmusicxx_extern_plugin.so`。

## 打包（随应用分发）

宿主库**不在 Flutter 构建里编译**（依赖链太重，见 plan §11.2），所以本包把它当作"预构建产物"来打包：

```
pubspec.yaml             flutter.plugin.platforms.{windows,linux}.ffiPlugin = true
windows/CMakeLists.txt   找到已构建的宿主库 → 写 musicxx_extern_plugin_bundled_libraries
linux/CMakeLists.txt     同上（库名 libmusicxx_extern_plugin.so）
                         → Flutter 放进 PLUGIN_BUNDLED_LIBRARIES → Windows 装到可执行文件旁、Linux 装到 <bundle>/lib/
```

- 查找顺序（与 Dart 侧 `native_library.dart` 同一套约定）：
  `-DMUSICXX_EXTERN_PLUGIN_HOST_LIBRARY` → 环境变量 `MUSICXX_EXTERN_PLUGIN_LIBRARY`
  → `<包>/.native/output/<平台>-<架构>-<配置>/bin/` → `<包>/.native/build/<平台>-<配置>/musicxx-extern-plugin-install/bin/`
  → 仓库预置目录（`resource/libs/windows/lib/`、`resource/libs/linux/lib/`）；
- Debug 与 Release 两份产物都在时，按**当前 Flutter 构建配置**选择（`Profile` 取 Release）；
- **找不到宿主库只打警告、不中断构建**（外部插件是可选功能，缺失时应用照常启动，Dart 侧给出提示）。
  发布流水线若要求"必须打包"，请显式传 `-DMUSICXX_EXTERN_PLUGIN_HOST_LIBRARY=<路径>`，并在 CI 里自行判定失败；
- 例（Windows）：`flutter build windows --release` → `build/windows/x64/runner/Release/musicxx_extern_plugin.dll` 与 `musicxx.exe` 同目录；
- 例（Linux）：`flutter build linux --release` → `build/linux/x64/release/bundle/lib/libmusicxx_extern_plugin.so`
  （桌面应用的库都放 `bundle/lib/`，Dart 侧会去"可执行文件旁的 `lib/`"目录找它）。

其余平台（macOS/Android/iOS/OHOS）的打包属于 plan M4 的后续工作：macOS 可照 Windows/Linux 的写法
（`<平台>_bundled_libraries`，另需注意 Hardened Runtime 下的 `disable-library-validation`）；
Android 需要先用 NDK 交叉编译整套依赖（含 QuickJS），适合在 CI 里单独一条流水线。
平台能力（哪些平台允许原生插件、入口是否可见）在应用侧单点判定：`lib/plugin/externPlugin/ExternPluginPlatform.dart`
（iOS/OHOS 只跑 JS 插件，不允许加载未签名动态库）。

## 写一个原生插件（SDK 与构建模板）

插件作者只需要 SDK 头文件 + 一个 CMake 助手，不需要了解宿主工程结构：

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_plugin LANGUAGES CXX)

# SDK 前缀 = `tools/build_native.ps1` 的安装前缀（含 include/ 与 lib/cmake/）
set(musicxx_extern_plugin_DIR "<安装前缀>/lib/cmake/musicxx_extern_plugin")
find_package(musicxx_extern_plugin CONFIG REQUIRED)

musicxx_plugin_add_target(my_plugin SOURCES my_plugin.cpp MANIFEST plugin.yaml)
```

`musicxx_plugin_add_target`（`src/sdk/cmake/musicxx_plugin.cmake`）负责：

- 建 SHARED 库并链接 SDK 与内核/工具库（`cxx_pluginxx_static`/`cxx_utilxx_base_static`/`fmt::fmt` 存在就链接，插件作者不用记名字）；
- C++26 + MSVC `/utf-8` + 符号默认隐藏；
- **只导出入口符号**：非 MSVC 平台加 version script（GNU/Clang）或导出符号表（Apple），因此插件不会把内核/C++ 运行时符号暴露出去；
- 多配置生成器下把库文件与 `plugin.yaml` 放在同一层 —— 该目录可以直接作为"插件目录"使用。

参考实现：`plugins/example_native/`（钩子/能力/动作/事件/UI/存储/日志全演示）、`plugins/example_js/`（等价 JS 版）。
原生插件作者指南（完整流程 + 完整代码 + 构建/部署/排障）：`docs/plugin-native-api.md`；
钩子总表与派发方式（`sync`/`async`）见生成物 `docs/plugin-hooks.md`；JS 作者文档见 `docs/plugin-js-api.md`。

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

## Dart 侧（包内）

```powershell
flutter pub get                                # 依赖（ffi；dev: ffigen/flutter_test）
dart run tools/gen_contract.dart               # 由 tools/hooks.def.json 生成 Dart 常量 + C++ 头 + 文档
dart run tools/gen_contract.dart --check       # CI：生成物与定义不一致时退出码 1
dart run ffigen --config ffigen.yaml           # 由 src/include/musicxx_extern_plugin_api.h 生成绑定
flutter analyze
flutter test                                   # 端到端冒烟（需要先构建原生库）
dart run tools/smoke_dart.dart                 # 纯 Dart 冒烟（不依赖 Flutter，便于定位 FFI 卡点）
```

Dart 侧用法（详见 `lib/musicxx_extern_plugin.dart` 文件头）：

```dart
final MusicxxPluginRuntime runtime = MusicxxPluginRuntime.instance;
runtime.init(config: MusicxxPluginRuntimeConfig(
  appVersion: '0.87.0',
  platform: MusicxxPluginRuntime.currentPlatform,
  userPluginDir: '<appData>/plugins',
), libraryPath: '<显式路径优先，见 plan §5.2>');
runtime.events.listen((MusicxxPluginEvent event) => ...);
runtime.plugins.scan();
runtime.plugins.load('example_native');
final Map<String, Object?>? verdict = runtime.hooks.decide(
  MusicxxPluginHookId.playerBeforePlaySong, <String, Object?>{'sid': sid, 'song': songJson});
// 异步裁决：调用点本身是 Future 时用它 —— 不占用调用线程，结果经 hook.decision.result 事件回来再合并
final Map<String, Object?>? asyncVerdict = await runtime.hooks.decideAsync(
  MusicxxPluginHookId.playerSourceBeforeParse, <String, Object?>{'sid': sid});
runtime.dispose();
```

声明式 UI 扩展（插件不写 Flutter 代码，只声明；plan §5.6）：

```dart
// 拉到全部 UI 项（主页入口 / 歌曲菜单 / 设置页 / 附加信息块）
final List<MusicxxPluginUIItem> items = runtime.plugins.uiSnapshot();
final List<MusicxxPluginUIItem> entries =
    MusicxxPluginUIItems.byType(items, MusicxxPluginUIType.homeEntry);
// 变更会推送 musicxx.ui.changed 事件（载荷带该插件的全部项）→ 整批替换即可
final List<MusicxxPluginUIItem> next =
    MusicxxPluginUIItems.replacePlugin(items, 'example_native', newItems);
```

- 原生库定位顺序：环境变量 `MUSICXX_EXTERN_PLUGIN_LIBRARY` → `.native/output/*/bin/`（Release 优先、其次按修改时间）
  → `.native/build/*/musicxx-extern-plugin-install/bin/` → 纯库名（系统搜索路径）；
- 找不到库时抛 `MusicxxPluginLibraryException`（含逐个候选与原因），版本不匹配抛 `MusicxxPluginApiVersionException`；
- 包内 `test/`、`docs/`、`lib/src/*.g.dart` 都是可重新生成/可重跑的，不要手工改生成物。

## 当前状态

见 musicxx 仓库 `resource/history/extern-plugin-impl/work.md`：
- S2（原生宿主）：**已完成**（含 `musicxx.hooks` / `musicxx.host` / `musicxx.ui` 三张领域表）；
- S3（Dart 包）：主干已落地（绑定/运行时/管理器/钩子派发/状态镜像/动作分发/声明式 UI 模型/契约生成）；
- S5（JS 运行时）：已落地（QuickJS 编入宿主库、共享 JS 线程、`js:<id>` 合成实例、`musicxx` API 面）；
- S6（UI 扩展 + 观测）：声明式 UI 表与 JS/原生 API 已落地，应用侧渲染（主页入口 / 歌曲菜单 /
  插件页面 / 插件设置页）已接入；插件配置表单（清单 `settings_schema` + `config.json`）、
  `musicxx.net.fetch`/`download` 便利通道、管理页调试与统计页也已落地；
- 异步裁决：原生 ASYNC 派发 + `musicxx.hook.decision.result` 事件 + Dart `hooks.decideAsync`（应用侧 `player.source.beforeParse` 已改用）+ `musicxx.hook.observe` 观测事件；
- 平台能力单点（`ExternPluginPlatform.dart`：iOS/OHOS 只跑 JS 插件）；`overlay.widget` 附加信息块已渲染；SDK 构建模板与 `find_package` 配置已提供；
- 平台打包：**Windows 与 Linux 已接入**（宿主库随应用分发：Windows 到可执行文件旁、Linux 到 `bundle/lib/`，
  见上面「打包（随应用分发）」；`tools/build_native.sh` 覆盖 Linux/macOS 的宿主库构建）；
  macOS/Android/iOS/OHOS 的平台工程（Android 需要先用 NDK 交叉编译整套依赖）与 CI 排入后续阶段；
- JS 插件支持**异步裁决**（裁决处理器可返回 Promise，等待预算内结算生效、超时按不裁决且不计失败）；
  原生插件作者指南见 `docs/plugin-native-api.md`；
- 完整记录（每轮改了什么、验证命令与结果、偏差）见 musicxx 仓库 `resource/history/extern-plugin-impl/work.md`。

测试夹具（只服务原生测试，不是可发布插件；随测试一起安装到 `<安装前缀>/plugins/`）：

| 夹具 | 验证点 |
|---|---|
| `src/tests/fixtures/fail_native/` | 钩子处理器总是失败 → 宿主连续 3 次失败后**只暂停该处理器**（熔断，`hook_stats` 里 `failures`/`paused`）、同插件的其它处理器照常工作、插件不被卸载（plan §4.10） |
| `src/tests/fixtures/bad_entry_native/` | 库文件缺 `musicxx_plugin_start`/`stop` → 装载阶段按契约拒绝（明确失败、有可读原因、无注册残留，plan §13.1 的 `test_entry_symbols`） |
| `src/tests/plugins/spin_js/` | 观察钩子里死循环 → 可选执行上限（`jsExecGuardMs`）能中断脚本且不影响其它 JS 插件 |
| `src/tests/plugins/broken_js/` | 脚本语法错误 → 装载失败并回滚，宿主继续可用 |

插件侧可用的能力（对应 plan §4.8/§5.6/§7）：

| 能力 | 入口 | 说明 |
|---|---|---|
| 钩子 | `musicxx.hooks.register` / `pluginBase.hook` | 观察型与裁决型；裁决处理器可同步返回，也可返回 Promise（异步裁决：预算内结算生效，超时按不裁决、不计失败）；声明为 `dispatch: async` 的钩子由宿主异步派发，Dart 侧不阻塞 |
| 动作 | `musicxx.call` / `pluginBase.requestAction` | 播放/库/歌词/UI/存储/网络/杂项，逐条权限校验 |
| 状态镜像 | `musicxx.state.get` | 只读快照（不含临时直链/token） |
| 配置 | `musicxx.storage.getConfig/setConfig`（命名空间 `config`） | 读写插件目录的 `config.json`，与用户在设置页里改的是同一份 |
| 网络（可选便利通道） | `musicxx.net.fetch/download` | 经宿主网络栈；宿主不限定可访问域名 |
| 声明式 UI | `musicxx.ui.registerEntry` | 主页入口 / 歌曲菜单 / 歌单菜单 / 设置页 / 附加信息块 |
| 能力与跨插件调用 | `musicxx.capability.register/call` | JS↔JS 同线程直调；JS→原生投递宿主线程、脚本不阻塞 |
| 统计自读 | `musicxx.stats.getSelf/reportMemory/reportMetric` | 只观测不限制 |

验证命令与当前结果：

```powershell
# Windows
pwsh -NoProfile -File tools/build_native.ps1 -RunTests   # 原生测试 212 项全绿
```

```bash
# Linux / macOS
./tools/build_native.sh --run-tests                      # 原生测试 212 项全绿 (Linux 实测：WSL Ubuntu 22.04 + GCC 16)
```

```powershell
dart run tools/gen_contract.dart --check                 # 契约生成物一致（66 个钩子，12 个异步裁决）
flutter analyze                                          # 0 issue
flutter test                                             # 包内：端到端冒烟（含原生/JS 异步裁决）+ UI 模型/附加信息块单测
```
