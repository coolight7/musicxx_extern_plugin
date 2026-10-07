# musicxx_extern_plugin

- 本项目是 [musicxx](https://github.com/coolight7/musicxx) 的**外部插件框架**（原生宿主库 + 插件 SDK + 示例插件 + 原生测试）。
- 这是应用侧的接入说明；想直接开发插件请看下面的「文档从哪里读起」。

## 文档从哪里读起

| 你要做的事 | 读这篇 |
|---|---|
| **第一次写插件**（选形态 / 环境准备 / 第一个插件 / 注意事项 / 发布检查） | [docs/plugin-guide.md](docs/plugin-guide.md) |
| 写一个 **C++ 动态库插件**（钩子 / 能力 / 动作 / 日志） | [docs/plugin-native-api.md](docs/plugin-native-api.md) |
| 写一个 **JS 脚本插件**（零编译，一个目录即可） | [docs/plugin-js-api.md](docs/plugin-js-api.md) |
| 给 **播放页背景 / 页面内联块** 写**着色器**（shader bundle） | [docs/plugin-shader-bundle.md](docs/plugin-shader-bundle.md) |
| 给插件加**界面**（主页入口、歌曲菜单、插件页面、设置页） | [docs/plugin-ui.md](docs/plugin-ui.md) |
| 用**命令行 / MCP** 装插件、刷新、看日志（AI agent 与自动化） | [docs/plugin-agent-cli.md](docs/plugin-agent-cli.md) |
| 查**钩子** id / 模式 / 载荷 / 裁决语义 | [docs/plugin-hooks.md](docs/plugin-hooks.md) |
| 读 / 改 / 订阅**变量**（官方变量目录与规则） | [docs/plugin-vars.md](docs/plugin-vars.md) |
| 构建宿主库、把宿主库随应用分发 | 本文件「构建」「打包（随应用分发）」 |
| 在 Dart 侧接入宿主（应用开发者） | 本文件「Dart 侧（包内）」+ `lib/musicxx_extern_plugin.dart` 文件头 |

简而言之理解：

- **两种插件形态**：C++ 动态库（同进程、能自己起线程、iOS/OHOS 不可用）与 JS 脚本
  （零编译、跨平台、能力处理器必须同步返回）；两者共用同一套钩子、UI、动作约定；
- **插件不写 Flutter 代码**：界面靠声明（UI 项 + 插件页面）/ shader 由宿主渲染；
- **动态加载 shader 支持**：插件可以利用 `flutter_gpu` 编译 shader，并打包进插件安装包内，宿主可以在运行时动态加载并渲染
- **宿主是可选功能**：宿主库缺失或初始化失败时应用照常启动，只是没有外部插件。


## 目录

```
src/CMakeLists.txt   顶层 superbuild：XX_IS_* 平台/编译器宏 → BoostConfig.cmake → ExternalProject_Add 构建依赖 → 构建宿主工程
src/include/         Dart ⇄ 原生 C ABI v1（musicxx_extern_plugin_api.h，ffigen 入口，唯一导出约定）
src/host/            宿主工程（嵌套构建，CMakeLists.txt 里用 find_package 取依赖）
  js/                JS 插件运行时（QuickJS）：共享 JS 线程 + js:<pluginId> 合成内置实例 + musicxx API 面
src/sdk/include/     插件作者 SDK（musicxx/plugin/api/*；插件只依赖头文件）
                     plugin_api.h 是领域约定、plugin_kit.h 是伞头（含内核 kit）、
                     hook_ids.g.h 由 tools/gen_contract.dart 生成（钩子 id 与已知钩子表）
src/sdk/cmake/       构建助手 musicxx_plugin.cmake + find_package 配置模板
src/tests/           原生测试（test_host.cpp，不依赖 Dart；被测插件就是 `plugins/` 下的那些 example_*）
src/third_party/     依赖子模块（cxx_pluginxx / cxx_utilxx_base / fmt / yaml-cpp / simdjson / libiconv-native / uchardet / quickjs）
lib/                 Dart 侧（FFI 绑定 + 运行时/管理器/钩子/状态/动作；插件界面模型来自子模块的
                     pluginxx_ui，经 lib/pluginxx_ui.dart 转发；bindings_generated.dart 与
                     hook_ids.g.dart 为生成物）
schema/              musicxx 扩展 kit 的定义（musicxx-ui-kit.def.json；改完跑 tools/gen_ui_kit.ps1）
js/                  生成的扩展 kit（musicxx_ui_kit.js；工具会把它复制进 JS 插件目录）
test/                Dart 侧测试（ui_model_test.dart 纯模型单测；host_test.dart 对真实原生库做
                     整套流程验证；plugin_config_test.dart 覆盖示例插件的设置读写往返与重新装载后的
                     持久化；multi_isolate_test.dart 覆盖多 isolate 并发调用）
plugins/            官方插件与示例（**每个子目录一个插件**，目录名 = 插件 id = 清单 name；见该目录 README）
  example_native/   示例插件（C++，演示钩子/状态镜像/日志/能力/事件订阅/声明式 UI）
  example_native_multi/ 示例插件（C++，多目标打包：包内多个系统/架构分支，运行时按系统与架构选分支）
  example_native_fail/       对照示例（C++：处理器总是失败 → 只记统计、不暂停）
  example_native_bad_entry/  对照示例（C++：缺 start/stop 入口符号 → 拒绝装载）
  example_js/       示例插件（JS，零编译；与 native 版行为等价）
  example_js_shader/ 示例插件（JS，零编译；只演示播放页背景与动画速率设置，带三种背景样式：晶格 / 光圈 / 插件纹理铺底）
  playing_bg_image/ 示例插件（JS，零编译；插件显示名『示例封面背景』：三种播放页背景模式 —— 模糊热浪 / 渐变贴边 / 黑胶 —— + 接管播放页歌曲图 + 自定义封面取色：一个插件把背景、图标槽位与取色钩子串起来）
  example_js_async/ 对照示例（JS：裁决处理器返回 Promise 的异步裁决）
  example_js_vars/  示例插件（JS：变量通道 —— 登记插件变量 + 读写与绑定官方变量）
  example_js_multi_script/ 示例插件（JS：清单 scripts 多脚本按顺序装载）
  example_js_broken/ 对照示例（JS：脚本语法错误 → 装载失败并回滚）
docs/plugin-guide.md 插件开发指南（选形态 / 环境准备 / 第一个插件 / 注意事项 / 调试 / 发布前检查清单）
docs/plugin-hooks.md 钩子总表（生成物：id / 模式 / 派发 / 应用是否已接入 + 已接入钩子的载荷与裁决）
docs/plugin-vars.md 官方变量目录（键 / 能力位 / 取值 / 风险 + 维护约定）
docs/plugin-native-api.md 动态库插件作者指南（清单/SDK 用法/线程约定/构建/部署/排障）
docs/plugin-js-api.md JS 插件作者指南（目录结构/生命周期/`musicxx` API/硬约束/排障）
docs/plugin-ui.md 插件界面参考（UI 项类型与字段/插件页面组件/用 kit 装配内容/设置页写法）
docs/musicxx-ui-kit.md 生成的扩展 kit 说明（组件与参数；与库的 docs/kit.md 对照看）
docs/plugin-shader-bundle.md 插件渲染：shader bundle 打包、格式版本与 uniform 约定
docs/plugin-agent-cli.md 用命令行 / MCP 装插件、刷新、看日志（开发期快捷控制，给 AI agent 与自动化）
tools/               build_native.ps1（Windows：环境准备 + 调 cmake）、build_native.sh（Linux/macOS：同一套流程）、
                     gen_contract.dart（约定生成/校验）、gen_ui_kit.ps1（生成扩展 kit：C++ 头 + JS）、
                     sync_ui_kit.ps1（把 kit 复制进 JS 插件目录）、check_submodules.ps1（子模块检查）、
                     pack_plugin.ps1（把插件目录打成安装包 .zip，并列出包内分支）、
                     self_check.dart（纯 Dart 自检，定位 FFI 卡住的位置）、cmake/BoostConfig.cmake.in
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

Android 用 NDK 交叉编译（两个脚本同一套参数；每个 ABI 各构建一次，产物直接给 `android/build.gradle`
收集进 APK，见「打包（随应用分发）」的 Android 一节）：

```powershell
pwsh -NoProfile -File tools/build_native.ps1 -Android -Abi arm64-v8a          # Windows
pwsh -NoProfile -File tools/build_native.ps1 -Android -Abi arm64-v8a -Config Debug
```

```bash
./tools/build_native.sh --android --abi arm64-v8a                             # Linux / macOS
ANDROID_NDK_HOME=<ndk> ./tools/build_native.sh --android --abi arm64-v8a      # 显式指定 NDK
```

- NDK 位置：`-AndroidNdk`/`--ndk` → `$ANDROID_NDK_HOME` / `$ANDROID_NDK_ROOT`
  → `$ANDROID_HOME|$ANDROID_SDK_ROOT/ndk` 下版本号最大者；
- 固定 Ninja 生成器 + `c++_static`，最低系统版本默认 `android-24`（`-AndroidPlatform`/`--android-platform` 可改）；
- `-RunTests`/`--run-tests` 在 Android 上跳过（测试可执行文件是给设备编的），产物在
  `.native/output/android-<abi>-<配置>/{bin,plugins}`。

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
- **只导出宿主库入口**：宿主库只导出 `musicxx_extern_plugin_*` —— MSVC 靠导出宏；GNU/Clang 另加
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
nm -D --defined-only .native/output/linux-x64-release/bin/libmusicxx_extern_plugin.so   # 仅 musicxx_extern_plugin_*（数量随版本变化）
ldd .native/output/linux-x64-release/bin/libmusicxx_extern_plugin.so                     # 只有 libc/libstdc++/libgcc_s/libm
```

> 分发到 Linux 桌面时注意：宿主库的 glibc 需求取决于**构建机**（本仓实测 Ubuntu 22.04 上为 `GLIBC_2.35`），
> 且运行期会用到应用自带的 `libstdc++`（`resource/libs/linux/lib/`）。发布用的 Linux 产物建议在
> 目标发行版（或更老的发行版）上构建，并在 `flutter build linux` 后核对 `bundle/lib/libmusicxx_extern_plugin.so`。

## 打包（随应用分发）

宿主库**不在 Flutter 构建里编译**（依赖链太重），所以本包把它当作"预构建产物"来打包：

```
pubspec.yaml             flutter.plugin.platforms.{windows,linux,android}.ffiPlugin = true
windows/CMakeLists.txt   找到已构建的宿主库 → 写 musicxx_extern_plugin_bundled_libraries
linux/CMakeLists.txt     同上（库名 libmusicxx_extern_plugin.so）
                         → Flutter 放进 PLUGIN_BUNDLED_LIBRARIES → Windows 装到可执行文件旁、Linux 装到 <bundle>/lib/
android/build.gradle     按 ABI 收集 .so 到 jniLibs（每个 build type 一个目录）
                         → AGP 打进 APK 的 lib/<abi>/libmusicxx_extern_plugin.so
```

- 查找顺序（与 Dart 侧 `native_library.dart` 同一套约定）：
  `-DMUSICXX_EXTERN_PLUGIN_HOST_LIBRARY` / `-PmusicxxExternPluginLibDir` → 环境变量
  `MUSICXX_EXTERN_PLUGIN_LIBRARY`（Android 侧是 `MUSICXX_EXTERN_PLUGIN_ANDROID_LIB_DIR`）
  → `<包>/.native/output/<平台>-<架构>-<配置>/bin/` → `<包>/.native/build/<平台>-<配置>/musicxx-extern-plugin-install/bin/`
  → 仓库预置目录（`resource/libs/windows/lib/`、`resource/libs/linux/lib/`、`resource/libs/android/<abi>/`）；
- Debug 与 Release 两份产物都在时，按**当前 Flutter 构建配置**选择（`Profile` 取 Release）；
- **找不到宿主库只打警告、不中断构建**（外部插件是可选功能，缺失时应用照常启动，Dart 侧给出提示）。
  发布流水线若要求"必须打包"，请显式传 `-DMUSICXX_EXTERN_PLUGIN_HOST_LIBRARY=<路径>`，并在 CI 里自行判定失败；
- 例（Windows）：`flutter build windows --release` → `build/windows/x64/runner/Release/musicxx_extern_plugin.dll` 与 `musicxx.exe` 同目录；
- 例（Linux）：`flutter build linux --release` → `build/linux/x64/release/bundle/lib/libmusicxx_extern_plugin.so`
  （桌面应用的库都放 `bundle/lib/`，Dart 侧会去"可执行文件旁的 `lib/`"目录找它）。

### Android（已接入）

```bash
# 1) 交叉编译宿主库（每个 ABI 一次；Windows 侧同样支持）
pwsh -NoProfile -File tools/build_native.ps1 -Android -Abi arm64-v8a      # Windows
ANDROID_NDK_HOME=<ndk> tools/build_native.sh --android --abi arm64-v8a    # Linux/macOS
# 2) 正常打包：库由 android/build.gradle 收集进 jniLibs
flutter build apk --release
```

Android 与桌面端的差别（改这块之前先读 `android/README.md`）：

- 宿主库**必须随 APK 分发**：Android 7 起动态链接器只允许应用从 APK 的原生库目录（与系统目录）加载
  动态库，应用数据目录里的 `.so` 会失败（`is not accessible for the namespace "classloader-namespace"`）。
  所以 Dart 侧在 Android 上**只按库名加载**（`DynamicLibrary.open('libmusicxx_extern_plugin.so')`，
  由系统解析到 `lib/<abi>/`），桌面端那套绝对路径候选不参与；
- **用户安装的动态库插件**因此可能装载失败：宿主按"安全降级"处理（标记不可用 + 事件 + 不重试），
  应用与其它插件照常工作；JS 插件不受影响。管理页会提前说明这条限制，失败原因也会换成面向用户的说法；
- 交叉编译统一 Ninja 生成器 + `c++_static`（C++ 运行库静态链进宿主库，APK 不需要额外的 `libc++_shared.so`）；
  `-RunTests`/`--run-tests` 在 Android 上跳过（测试可执行文件是给设备编的）。

其余平台（macOS/iOS/OHOS）的打包属于后续工作：macOS 可照 Windows/Linux 的写法
（`<平台>_bundled_libraries`，另需注意 Hardened Runtime 下的 `disable-library-validation`）；
iOS/OHOS 只跑 JS 插件，需要静态库 + podspec。
平台能力（哪些平台允许动态库插件、入口是否可见）在应用侧统一判定：`lib/plugin/externPlugin/ExternPluginPlatform.dart`
（iOS/OHOS 只跑 JS 插件，不允许加载未签名动态库）。

## 写一个插件

三种形态各有完整指南（本文件只给最小示例与跳转）：

| 形态 | 目录内容 | 指南 |
|---|---|---|
| C++ 动态库 | `plugin.yaml` + 库文件 + 可选 `shader/` | [docs/plugin-native-api.md](docs/plugin-native-api.md) |
| C++ 动态库（多目标包） | `plugin.yaml` + `lib/<系统>-<架构>/库文件`（一个包支持多个系统/架构） | 同上 §1.3 |
| JS 脚本 | `plugin.yaml` + `plugin.js` | [docs/plugin-js-api.md](docs/plugin-js-api.md) |
| 着色器（两种形态都能用） | `shader/*.frag` + `bundle.json` + `*.shaderbundle` | [docs/plugin-shader-bundle.md](docs/plugin-shader-bundle.md) |

### 动态库插件（SDK 与构建模板）

插件作者只需要 SDK 头文件 + 一个 CMake 助手，不需要了解宿主工程结构：

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_plugin LANGUAGES CXX)

# SDK 前缀 = `tools/build_native.ps1` 的安装前缀（含 include/ 与 lib/cmake/）
set(musicxx_extern_plugin_DIR "<安装前缀>/lib/cmake/musicxx_extern_plugin")
find_package(musicxx_extern_plugin CONFIG REQUIRED)

musicxx_plugin_add_target(my_plugin
  SOURCES my_plugin.cpp
  MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
  ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/shader"   # 可选：随插件分发的资源
)
```

`musicxx_plugin_add_target`（`src/sdk/cmake/musicxx_plugin.cmake`）负责：

- 建 SHARED 库并链接 SDK 与内核/工具库（`cxx_pluginxx_static`/`cxx_utilxx_base_static`/`fmt::fmt` 存在就链接，插件作者不用记名字）；
- C++26 + MSVC `/utf-8` + 符号默认隐藏；
- **只导出入口符号**：非 MSVC 平台加 version script（GNU/Clang）或导出符号表（Apple），因此插件不会把内核/C++ 运行时符号暴露出去；
- 多配置生成器下把库文件与 `plugin.yaml` 放在同一层 —— 该目录可以直接作为"插件目录"使用；
- `ASSETS` 声明的目录/文件按原名复制到产物目录（shader bundle、图标等随插件分发）。

**多目标打包**（一个包同时支持多个系统 / 架构，仿 APK 的 `lib/<abi>/`）：多传
`TARGET_TAG auto`，助手就把库文件放进 `<包目录>/lib/<系统>-<架构>/`，清单与 `ASSETS` 放在包目录里：
```cmake
musicxx_plugin_add_target(my_plugin
  SOURCES my_plugin.cpp
  MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
  TARGET_TAG auto            # 按当前构建目标推标签（Android 用 ABI 名：android-arm64-v8a）
)
```

```text
<构建目录>/package/
├── plugin.yaml
└── lib/windows-x64/my_plugin.dll      # 本次构建的分支
```

每个平台/架构各构建一次、把 `lib/<标签>/` 合并进同一个包目录，用
`pwsh tools/pack_plugin.ps1 -PluginDir <包目录>` 打成安装包（它会打印包内分支与每个分支的库文件，
便于确认齐全）；宿主运行时按当前系统与 CPU 架构选分支（选择顺序、标签别名与规则见
[docs/plugin-native-api.md](docs/plugin-native-api.md) §1.3，示例插件 `plugins/example_native_multi/`）。
**分支选择规则只有宿主一处实现**：扫描、装载与应用侧安装预检（C ABI `plugin_inspect`）跑的是同一份
判定，Dart 侧只解析结果。不传 `TARGET_TAG` 时行为与以前一致（库与清单同一层，直接作为插件目录）。

### JS 插件

```text
my_plugin/
├── plugin.yaml     # name / kind: js / version / api_version / permissions ...
└── plugin.js       # 顶层同步注册钩子与能力
```

不需要 CMake、不需要编译；放进插件目录后「重新扫描」即可。完整 API、硬约束与排障见
[docs/plugin-js-api.md](docs/plugin-js-api.md)。

### 着色器（播放页背景 / 页面内联块）

```text
<插件目录>/shader/
├── bundle.json           {"MusicxxRenderVertex": {...}, "MusicxxRenderFragment": {...}}
├── shaders/bg.vert       顶点（全屏三角形）
├── shaders/bg.frag       片元（声明 uniform 结构体 MusicxxRenderInfo）
├── build_bundle.ps1      照抄示例的打包脚本
└── bg.shaderbundle       impellerc 产物（随插件分发）
```

bundle 用 Flutter SDK 自带的 `impellerc` 编译（**一个 bundle 五个后端全平台通用**）：

```bash
"$FLUTTER_ROOT/bin/cache/artifacts/engine/<平台>/impellerc" --shader-bundle="$(tr -d '\n' < bundle.json)" --sl=bg.shaderbundle
```

bundle 里的 `format_version` 必须与目标应用的 Flutter 版本一致（基线版本 3.47.5 对应 2），
编好后在 UI 项里绑定它。完整字段、uniform 约定与排障见
[docs/plugin-shader-bundle.md](docs/plugin-shader-bundle.md)。

### 装上去、改完刷新（命令行 / MCP）

应用自带命令行工具 `musicxx-cli`（与主程序同目录，macOS 在 `Musicxx.app/Contents/MacOS/`），
一条命令就能完成"安装 → 启用 → 看日志 → 改完重载 → 卸载"，也能经 `musicxx-cli mcp` 把这些
操作交给 AI 客户端：

```bash
pwsh -NoProfile -File tools/pack_plugin.ps1 -PluginDir D:/work/my_plugin   # 打包（顶层是插件目录内容）
musicxx-cli plugin install "D:/work/my_plugin.zip" --enable                # 安装并启用
musicxx-cli plugin list                                                    # 每个插件的状态与失败原因
musicxx-cli plugin logs my_plugin --limit 50                               # 最近日志（新的在前）
musicxx-cli plugin reload my_plugin                                        # 改完代码重新加载
musicxx-cli render select plugin.my_plugin.bg                              # 让播放页用上插件的背景样式
musicxx-cli plugin uninstall my_plugin                                     # 卸载（--keep-data 保留私有数据）
```

完整命令表、MCP 工具名与参数、退出码约定见
[docs/plugin-agent-cli.md](docs/plugin-agent-cli.md)。

参考实现：`plugins/example_native/`（钩子/能力/动作/事件/UI/存储/日志全演示）、`plugins/example_js/`（等价 JS 版）、
`plugins/example_js_shader/`（只演示播放页背景与动画速率；三种背景样式各一个 bundle：晶格 `bg.frag`、光圈 `ring.frag`、
插件纹理铺底复用 `bg.frag`）、
`plugins/playing_bg_image/`（显示名『示例封面背景』：三种播放页背景模式『模糊热浪』/『渐变贴边』/『黑胶』+ 接管播放页歌曲图 + 自定义封面取色：
背景、图标槽位与钩子在同一个插件里串起来，覆盖 `cover` 纹理（含圆裁用的 `square`）、同一个槽位注册多项、`playing.icon` 三种形态里的 `none`、
`musicxx.media.palette.provide`；『黑胶』还演示了线段距离场画的唱臂与按 `speed` 旋转的盘心封面）；
另外 `plugins/example_js_vars/`（变量通道）、
`plugins/example_js_multi_script/`（清单 `scripts` 多脚本），以及下面「对照示例」一组 —— 它们演示宿主在
插件写坏时的保护行为，写插件前先看一眼可以少踩坑。
钩子总表与派发方式（`sync`/`async`）见生成物 `docs/plugin-hooks.md`；界面字段见 `docs/plugin-ui.md`。

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
flutter test                                   # 整套流程验证（宿主/JS/配置读写；需要先构建原生库）
dart run tools/self_check.dart                 # 纯 Dart 自检（不依赖 Flutter，便于定位 FFI 卡住的位置）
```

Dart 侧用法（详见 `lib/musicxx_extern_plugin.dart` 文件头）：

```dart
// 一次生命周期一个运行时：宿主需要"停掉再启动"时必须新建实例（dispose 过的实例不能再次 init）
final MusicxxPluginRuntime runtime = MusicxxPluginRuntime.create();
runtime.init(
  config: MusicxxPluginRuntimeConfig(
    appVersion: '0.87.0',
    platform: MusicxxPluginRuntime.currentPlatform,
    language: 'zh-cn',
    userPluginDir: '<appData>/extern_plugin/plugins',   // 用户插件目录
    builtinPluginDir: '<appData>/extern_plugin/plugins_builtin', // 随包插件目录（可选）
    dataDir: '<appData>/extern_plugin/data',            // 插件私有数据根目录
    logDir: '<logDir>',
    // safeMode / enableNative / enableJs / observeEvents / eventQueueCapacity 都有默认值
  ),
  libraryPath: '<显式路径优先，留空则按候选顺序找>',
);
runtime.events.listen((MusicxxPluginEvent event) => ...);   // 事件流（未知类型也会转发）
runtime.plugins.scan();                                     // 扫描（用户目录 + 随包目录）
runtime.plugins.load('example_native');                      // 装载（同步等待，带超时）
runtime.plugins.enable('example_native');                    // 禁用 / 启用 / 卸载 / 重载 / 能力调用
runtime.plugins.stats();                                     // 统计快照
runtime.dispose();
```

钩子派发（Dart 侧处理器 + 原生/JS 处理器链合并）：

```dart
// 注册 Dart 处理器（同一钩子多个处理器按 priority 升序执行，本线程就地执行）
runtime.hooks.on(MusicxxPluginHookId.playerBeforePlaySong, (ctx) => null);

final Map<String, Object?>? verdict = runtime.hooks.decide(
  MusicxxPluginHookId.playerBeforePlaySong, <String, Object?>{'sid': sid, 'song': songJson});
// 异步裁决：调用点本身是 Future 时用它 —— 不占用调用线程，结果经 hook.decision.result 事件回来再合并
final Map<String, Object?>? asyncVerdict = await runtime.hooks.decideAsync(
  MusicxxPluginHookId.playerSourceBeforeParse, <String, Object?>{'sid': sid});
```

状态镜像与动作：

```dart
runtime.state.update(MusicxxPluginState.keySong, songView);          // 单键（相同值会跳过）
runtime.state.updateThrottled(MusicxxPluginState.keyPlayer, view);   // 高频键节流
runtime.state.updateBatch({...});                                    // 同帧多键合并成一次调用

// 注册宿主动作实现（插件经 request_action 发起；未注册的动作会立刻收到"未找到"）
runtime.actions.register(MusicxxPluginActionNames.playerToggle, (invocation) async {
  await player.playOrPause();
  return <String, Object?>{'ok': true};
});
```

声明式 UI 扩展（插件不写 Flutter 代码，只声明）：

```dart
// 拉到全部 UI 项（主页入口 / 歌曲菜单 / 歌单菜单 / 播放页背景）
final List<MusicxxPluginUIItem> items = runtime.plugins.uiSnapshot();
final List<MusicxxPluginUIItem> entries =
    MusicxxPluginUIItems.byType(items, MusicxxPluginUIType.homeEntry);
// 变更会推送 musicxx.ui.changed 事件（载荷带该插件的全部项）→ 整批替换即可
final List<MusicxxPluginUIItem> next =
    MusicxxPluginUIItems.replacePlugin(items, 'example_native', newItems);
```

- 原生库定位顺序：环境变量 `MUSICXX_EXTERN_PLUGIN_LIBRARY` → `.native/output/*/bin/`（Release 优先、其次按修改时间）
  → `.native/build/*/musicxx-extern-plugin-install/bin/` → 可执行文件旁（含其 `lib/`）→ 纯库名（系统搜索路径）；
  Android/iOS 只按库名加载（库随应用包分发），绝对路径候选不参与；
- 找不到库时抛 `MusicxxPluginLibraryException`（含逐个候选与原因），版本不匹配抛 `MusicxxPluginApiVersionException`；
- 包内 `test/`、`docs/plugin-hooks.md`、`lib/src/*.g.dart`、`src/sdk/include/musicxx/plugin/api/hook_ids.g.h`
  都是生成物或可重跑的用例，不要手工改生成物。

## 能力现状

宿主侧已经实现的能力（应用侧接入情况见 musicxx 仓库）：

- **领域接口表**：`musicxx.hooks` / `musicxx.host` / `musicxx.ui` 三张已实现；
  `musicxx.player` / `library` / `lyrics` / `storage` / `net` / `stats` 的 IID 已冻结、表体未实现
  （查询返回 NULL）→ 这些能力统一走动作名（`requestAction("musicxx.player.play", ...)`），由应用侧分派；
- **钩子**：约定 66 个（33 观察 / 33 裁决，其中 13 个异步派发的裁决钩子）；
  应用侧当前**已接入 9 个**（`docs/plugin-hooks.md` 的「应用是否已接入」列标「已接入」），其余注册成功但不会触发；
- **JS 运行时**：QuickJS 编入宿主库、共享一条 JS 线程、每个插件独立 `JSRuntime`、
  `js:<pluginId>` 合成实例、顶层注册统一回放、运行期注册由宿主线程执行；
  裁决处理器可以返回 Promise（宿主一直等到它结算，结算后裁决生效）；
- **声明式 UI**：UI 项（主页入口 / 歌曲菜单 / 歌单菜单 / 播放页背景）与插件自绘页面
  （`ext://<插件id>/<视图id>`）都已实现；应用侧当前渲染主页入口、歌曲菜单与插件页面，
  **歌单菜单项还没有渲染入口**（见 `docs/plugin-ui.md` §1.4）；
- **插件渲染**：`player.background` 槽位已接入（插件交付 shader bundle，宿主运行期加载渲染，
  未选中时零成本）；页面里可以内联 `Shader` 块；
- **配置与网络**：插件配置放在插件目录的 `config.json`（插件自己读写，框架不渲染配置表单）；
  `musicxx.net.fetch`/`download` 经应用统一网络栈（宿主不限定域名）；
- **观测**：钩子统计（调用 / 耗时 / 超时 / 失败 / 被暂停的处理器）、插件各阶段耗时、注册项计数、
  JS 实例统计与内存采样、内存环形事件日志；
- **平台打包**：Windows 与 Linux 已接入（宿主库随应用分发，见「打包（随应用分发）」）、
  Android 已接入（NDK 交叉编译 + `jniLibs`，见 `android/README.md`）；
  macOS/iOS/OHOS 的平台工程属于后续工作（iOS/OHOS 只跑 JS 插件）。

对照示例（故意做出问题行为的示例插件：既服务原生测试，也让插件作者看清「写坏了会怎样」。
它们随 `MUSICXX_EXTERN_PLUGIN_BUILD_TESTS` 一起构建，并安装到 `<安装前缀>/plugins/`）：

| 对照示例 | 演示 / 验证点 |
|---|---|
| `plugins/example_native_fail/` | 钩子处理器总是失败 → 宿主只记统计（`hook_stats` 里 `failures`，不暂停、不卸载），同插件的其它处理器照常工作 |
| `plugins/example_native_bad_entry/` | 库文件缺 `musicxx_plugin_start`/`stop` → 装载阶段按约定拒绝（明确失败、有可读原因、无注册残留，对应用例 `test_entry_symbols`） |
| `plugins/example_js_async/` | 裁决处理器返回 Promise → 宿主一直等到结算，裁决生效（`asyncHookSettled`） |
| `plugins/example_js_broken/` | 脚本语法错误 → 装载失败并回滚，宿主继续可用 |

插件侧可用的能力：

| 能力 | 入口 | 说明 |
|---|---|---|
| 钩子 | `musicxx.hooks.register` / `PluginBase::hook` / `observe` | 观察型与裁决型；裁决处理器可同步返回，也可返回 Promise（JS：宿主一直等到结算，结算后裁决生效）；`dispatch: async` 的钩子由宿主异步派发、不占用调用线程。约定与载荷见 `docs/plugin-hooks.md` |
| 动作 | `musicxx.call` / `PluginBase::requestAction` | 播放 / 库 / 歌词 / UI / 存储 / 网络 / 渲染 / 杂项（应用侧分派；不做权限校验） |
| 状态镜像 | `musicxx.state.get` / `PluginBase::stateJson` | 只读快照（不含临时直链/token）；键与推送情况见 `docs/plugin-js-api.md` §5 |
| 配置 | `musicxx.storage.getConfig/setConfig`（命名空间 `config`）/ `PluginBase::configPath()` | 读写插件目录的 `config.json`（默认值由插件自己给，框架不提供配置表单） |
| 网络（可选便利通道） | `musicxx.net.fetch/download` | 经宿主网络栈；宿主不限定可访问域名 |
| 声明式 UI | `musicxx.ui.registerEntry` / `PluginBase::uiRegister` | 主页入口 / 歌曲菜单 / 歌单菜单（当前版本无渲染点）/ 播放页背景；字段见 `docs/plugin-ui.md` |
| 能力与跨插件调用 | `musicxx.capability.register/call` | JS↔JS 同线程直调；JS→原生投递宿主线程、脚本不阻塞；处理器必须同步返回 |
| 插件页面 | `ext://<插件id>/<视图id>` | 宿主调用插件同名能力取视图描述（Text / Divider / Button / Block / 布局块 / Shader 块） |
| 统计自读 | `musicxx.stats.getSelf/reportMemory/reportMetric` | 只观测不限制 |

验证命令：

```powershell
# Windows（构建 + 原生测试；测试结束会打印 checks=... failed=...，失败时退出码非 0）
pwsh -NoProfile -File tools/build_native.ps1 -RunTests
```

```bash
# Linux / macOS
./tools/build_native.sh --run-tests
```

```powershell
dart run tools/gen_contract.dart --check                 # 约定生成物一致（66 个钩子；改动 hooks.def.json 后必须重新生成）
flutter analyze                                          # 期望 0 issue
flutter test                                             # 包内整套流程：宿主/JS/配置读写/多 isolate（需先构建原生库，找不到库时跳过而不是失败）
| 钩子总表（id / 模式 / 派发 / 应用是否已接入 + 已接入钩子的载荷与裁决） | `docs/plugin-hooks.md`（由 `tools/hooks.def.json` 生成） |
