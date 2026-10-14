# musicxx_extern_plugin

> **术语约定**：本文档中的**主程序**指的就是 **musicxx**（Flutter/Dart 业务层）；外置插件的 C++ 框架层称为**插件框架**；插件目前分为**动态库插件**与**JS插件**; `musicxx`还有早期开发的一套内置插件框架（插件直接编译打包进 musicxx，仅决定是否运行时加载模块功能，不能由用户自定义开发插件）。

- 本项目是 [musicxx](https://github.com/coolight7/musicxx) 的**外部插件框架**（插件框架原生库 + 插件 SDK + 示例插件 + 原生测试），由 [agentxx](https://github.com/coolight7/agentxx) 项目的插件框架通用化发展而来。
- 这是主程序的接入说明；想直接开发插件请看下面的「文档从哪里读起」。

## 文档从哪里读起

| 你要做的事 | 读这篇 |
|---|---|
| 给 **AI Agent / 自动化脚本** 的浓缩指南（插件框架是什么 / 框架怎么跑 / 插件怎么写 / 命令 / 排障） | [AGENTS.md](AGENTS.md) |
| **第一次写插件**（选形态 / 环境准备 / 第一个插件 / 注意事项 / 发布检查） | [docs/plugin-guide.md](docs/plugin-guide.md) |
| **查某个接口做什么、参数是什么、怎么用**（状态镜像 / 动作 / 事件 / 变量 / 能力 / 功能点 / 钩子写法，两种形态共用） | [docs/plugin-api.md](docs/plugin-api.md) |
| 写一个 **动态库插件**（清单 / SDK / 线程 / 构建 / 常用接口的 C++ 写法） | [docs/plugin-native-api.md](docs/plugin-native-api.md) |
| 写一个 **JS插件**（零编译，一个目录即可） | [docs/plugin-js-api.md](docs/plugin-js-api.md) |
| 给 **播放页背景 / 页面内联块** 写**着色器**（shader bundle） | [docs/plugin-shader-bundle.md](docs/plugin-shader-bundle.md) |
| 给插件加**界面**（主页入口、歌曲菜单、插件页面、插件自己画的设置页） | [docs/plugin-ui.md](docs/plugin-ui.md) |
| 用**命令行 / MCP** 装插件、刷新、看日志（AI agent 与自动化） | [docs/plugin-agent-cli.md](docs/plugin-agent-cli.md) |
| **musicxx 对外暴露的全部接口**（MCP 工具全表 / 命令行命令全表 / 本机 HTTP 与 WebSocket 路由 / 权限边界） | [docs/musicxx-interfaces.md](docs/musicxx-interfaces.md) |
| 查**钩子** id / 模式 / 载荷 / 裁决语义 | [docs/plugin-hooks.md](docs/plugin-hooks.md) |
| 读 / 改 / 订阅**变量**（官方变量目录与规则） | [docs/plugin-vars.md](docs/plugin-vars.md) |
| 查**版本对照**（musicxx 版本 ↔ 插件框架版本 / 提交 / 日期）、改版本号的流程 | [docs/version-history.md](docs/version-history.md) |
| 构建插件框架库、把插件框架库随主程序分发 | 本文件「构建」「打包（随应用分发）」 |
| 在 Dart 侧接入插件框架（主程序开发者） | 本文件「Dart 侧（包内）」+ `lib/musicxx_extern_plugin.dart` 文件头 |

简而言之理解：

- **两种插件形态**：
  - 动态库插件（同进程、能自己起线程；桌面端与 Android 可以加载，iOS 只允许加载应用包内、与主程序同签名身份的库，用户装的库通常会被系统拒绝）；一般使用 C/C++ 等语言开发，高性能/高权限/高自由，但开发难度高，且需要对不同系统平台适配编译动态库
  - JS插件（无需编译、跨平台、处理器可以同步返回也可以返回 Promise）
  - 两者共用同一套钩子、UI、主程序动作约定；
- **插件不写 Flutter 代码**：界面靠声明（UI 项 + 插件页面）/ shader 由主程序渲染；
- **动态加载 shader 支持**：插件可以利用 `flutter_gpu` 编译 shader，并打包进插件安装包内，主程序可以在运行时动态加载并渲染
- **插件框架是可选功能**：插件框架库缺失或初始化失败时主程序照常启动，只是没有加载外部插件。


## 目录

```
AGENTS.md            Agent 指南（插件框架 / 插件开发 / 命令 / 排障的浓缩版，给 AI 与自动化脚本）
src/CMakeLists.txt   顶层 superbuild：XX_IS_* 平台/编译器宏 → BoostConfig.cmake → ExternalProject_Add 构建依赖 → 构建插件框架工程
src/include/         Dart ⇄ 原生 C ABI v1（musicxx_extern_plugin_api.h，ffigen 入口，唯一导出约定）
src/host/            插件框架工程（嵌套构建，CMakeLists.txt 里用 find_package 取依赖）
  js/                JS 插件运行时（QuickJS）：共享 JS 线程 + js:<pluginId> 合成内置实例 + musicxx API 面
src/sdk/include/     插件作者 SDK（musicxx/plugin/api/*；插件只依赖头文件）
                     plugin_api.h 是 musicxx 专用约定、plugin_kit.h 是总头文件（含插件框架 kit）、
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
                     整套流程验证；plugin_config_test.dart 覆盖示例插件的设置读写往返与重新加载后的
                     持久化；multi_isolate_test.dart 覆盖多 isolate 并发调用）
plugins/            官方插件与示例（**每个子目录一个插件**，目录名 = 插件 id = 清单 name；见该目录 README）
  example_native/   示例插件（C++，演示钩子/状态镜像/日志/能力/事件订阅/声明式 UI）
  example_native_multi/ 示例插件（C++，多目标打包：包内多个系统/架构分支，运行时按系统与架构选分支）
  example_native_fail/       对照示例（C++：处理器总是失败 → 只记统计、不暂停）
  example_native_bad_entry/  对照示例（C++：缺 start/stop 入口符号 → 拒绝加载）
  example_js/       示例插件（JS，零编译；与 native 版行为等价）
  example_js_shader/ 示例插件（JS，零编译；只演示播放页背景与动画速率设置，带三种背景样式：晶格 / 光圈 / 插件纹理铺底）
  playing_bg_image/ 示例插件（JS，零编译；插件显示名『示例封面背景』：三种播放页背景模式 —— 模糊 / 沉浸 / 黑胶 —— + 接管播放页歌曲图 + 自定义封面取色 + 按封面主色与亮暗主题推播放页前景色（保留主色色相与彩色程度，按当前模式估出的背景亮度保证对比度）：一个插件把背景、前景色、图标槽位与取色钩子串起来）
  example_js_async/ 对照示例（JS：裁决处理器返回 Promise 的异步裁决）
  example_js_vars/  示例插件（JS：变量通道 —— 登记插件变量 + 读写与绑定官方变量）
  example_js_multi_script/ 示例插件（JS：清单 scripts 多脚本按顺序加载）
  example_js_font/  示例插件（JS：全局字体切换 —— 系统字体 / 插件自带字体文件 / 恢复默认，演示"按钮动作立刻返回 + 后台执行"）
  example_js_broken/ 对照示例（JS：脚本语法错误 → 加载失败并回滚）
docs/plugin-guide.md 插件开发指南（选形态 / 环境准备 / 第一个插件 / 注意事项 / 调试 / 发布前检查清单）
docs/plugin-api.md 两种形态共用的接口手册（状态镜像 / 动作全表 / 事件 / 变量 / 能力 / 功能点 / 钩子写法 / 约定 / 通用排障）
docs/plugin-hooks.md 钩子总表（生成物：id / 模式 / 派发 / 主程序是否已接入 + 已接入钩子的载荷与裁决）
docs/plugin-vars.md 官方变量目录（键 / 能力位 / 取值 / 风险 + 维护约定）
docs/plugin-native-api.md 动态库插件作者指南（清单/多目标打包/SDK 用法/线程约定/接口表判定/构建/部署/排障）
docs/plugin-js-api.md JS 插件作者指南（目录结构/生命周期/`musicxx` API/硬约束/排障）
docs/plugin-ui.md 插件界面参考（UI 项类型与字段/插件页面组件/用 kit 拼装内容/插件自己的设置页写法）
docs/musicxx-ui-kit.md 生成的扩展 kit 说明（组件与参数；与库的 docs/kit.md 对照看）
docs/version-history.md 版本对照（musicxx 版本 ↔ 插件框架版本 / 提交 / 日期）与改版本号的流程
docs/plugin-shader-bundle.md 插件渲染：shader bundle 打包、格式版本与 uniform 约定
docs/plugin-agent-cli.md 用命令行 / MCP 装插件、刷新、看日志（开发期快捷控制，给 AI agent 与自动化）
tools/               setup_native_env.ps1（Windows：配置开发编译环境 —— 预检 / 子模块 / Boost / NDK → 缓存）、
                     build_native.ps1（Windows：构建插件框架 = 插件框架库 + 插件 SDK）、
                     build_examples.ps1（Windows：构建并打包示例插件）、
                     native_common.ps1（上面几个 ps1 的公共库，不单独执行）、
                     build_native.sh（Linux/macOS：环境配置、框架构建与示例插件合在一个脚本里，选项同名）、
                     build_examples.sh（Linux/macOS：构建并打包示例插件）、
                     pack_plugin.sh（Linux/macOS：把插件目录打成安装包，与 pack_plugin.ps1 同一套约定）、
                     gen_contract.dart（约定生成/校验）、gen_ui_kit.ps1（生成扩展 kit：C++ 头 + JS）、
                     sync_ui_kit.ps1（把 kit 复制进 JS 插件目录）、check_submodules.ps1（子模块检查）、
                     pack_plugin.ps1（把插件目录打成安装包 `<插件id>-<清单版本号>.zip`，并列出包内分支）、
                     self_check.dart（纯 Dart 自检，定位 FFI 卡住的位置）、
                     cmake/BoostConfig.cmake.in、cmake/host_autobuild.cmake（平台工程里自动触发插件框架库构建）
.native/             本地构建产物（构建目录 / 安装前缀 / 便携输出 / Boost 下载与解包缓存 /
                     env/environment.json 环境描述缓存，**全部可重建，不入版本库**）
```

## 依赖管理

依赖来源只有两处，**不需要任何预编译库**：

1. **git submodule**（`src/third_party/*`；每个子模块的 commit 由外层仓库的 gitlink 记录，不需要额外的版本清单文件）

   | 子模块 | 作用 |
   |---|---|
   | `cxx_pluginxx` | 插件框架（加载/清单/生命周期/通用接口表/SDK） |
   | `cxx_utilxx_base` | 日志/JSON/字符串/取消令牌/系统探测 |
   | `fmt`、`yaml-cpp`、`simdjson` | 格式化、`plugin.yaml` 解析、JSON 后端 |
   | `libiconv-native`、`uchardet` | 字符编码转换与探测（`cxx_utilxx_base` 的 `string_util` **无条件**使用 `<iconv.h>` 与 uchardet，不能靠开关关掉） |
   | `quickjs` | JS 插件运行时（`src/host/js/`），编入插件框架库；不构建它自带的命令行工具 |

   > JS 插件是**零编译**形态：一个目录（`plugin.yaml` + `plugin.js`）即可，插件框架把它装成内置实例 `js:<pluginId>`。
   > 关闭方式：`-DMUSICXX_EXTERN_PLUGIN_ENABLE_JS=OFF`（或去掉 quickjs 子模块，构建会自动跳过并打印警告）。

   ```powershell
   git submodule update --init --recursive             # 首次拉取
   pwsh -NoProfile -File tools/check_submodules.ps1     # 检查是否都已初始化 (commit 由 git 锁定)
   ```

2. **Boost 头文件**（只用 Boost.Asio 头，不链接任何 Boost 编译库）
   - 本机已有 `src/third_party/boost/include`（含 `boost/`，约 151 MB，不入版本库）时脚本直接复用；
   - 缺失时按构建脚本顶部锁定的 **版本 + URL + SHA256** 下载官方发布包
     （常量在 `tools/native_common.ps1` 与 `tools/build_native.sh` 里，两边保持一致），只解出 `boost/` 头文件子树到 `.native/boost/`；
   - `<安装前缀>/lib/cmake/Boost-<版本>/BoostConfig.cmake` 由顶层 CMake 依 `tools/cmake/BoostConfig.cmake.in`
     生成（只暴露头文件根 `Boost_INCLUDE_DIRS` 与 `Boost::headers`），不依赖 b2 生成的整套配置；
   - 可用 `-BoostRoot <含 boost/ 的目录>`/`--boost-root <目录>` 或 `-BoostArchive <发布包>`/`--boost-archive <包>` 覆盖（离线/自备包）。

## 构建

三条线分开：**配环境**（一次就够，结果存缓存）、**构建插件框架**（应用与插件作者都要的东西）、
**构建示例插件**（只想跑示例/测试时才需要）。结构仿照 agentxx（`agent/CMakeLists.txt` + `agent/script/windows_*_build.bat`）：

```text
tools/setup_native_env.ps1        配置开发编译环境: 工具预检 + 子模块 + Boost 头文件 + Android NDK
  └─ 结果写进 .native/env/environment.json（缓存; 已就绪时重复执行只是读一遍缓存）
tools/build_native.ps1            构建插件框架（插件框架库 + 插件 SDK）; 先复用上面那份缓存, 没配过就现场配
  └─ cmake -S src -B .native/build/<平台>-<配置> -G <生成器> -A <平台> \
           -DXX_IS_RELEASE_D=<0|1> -DCMAKE_BUILD_TYPE=<配置> -DCMAKE_CONFIGURATION_TYPES=<配置> \
           -DMUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE=<含 boost/ 的目录> \
           -DMUSICXX_EXTERN_PLUGIN_BUILD_EXAMPLE=<0|1> -DMUSICXX_EXTERN_PLUGIN_BUILD_TESTS=<0|1>
       └─ src/CMakeLists.txt (superbuild)
            XX_IS_* 宏 + BoostConfig.cmake + _COMMON_CMAKE_ARGS / _COMMON_CMAKE_CACHE_ARGS
            ExternalProject_Add: fmt → yaml-cpp → simdjson → libiconv-native → uchardet
                                 → cxx_utilxx_base → cxx_pluginxx
            ExternalProject_Add: musicxx_extern_plugin_host_repo (src/host, 内部 find_package 取依赖)
tools/build_examples.ps1          构建示例插件（plugins/ 下的动态库插件 + JS插件）, 可选打成安装包
tools/build_examples.sh           Linux / macOS 上的同一件事（调 build_native.sh --with-examples，再调 pack_plugin.sh 打包）
```

```powershell
# 0) 配环境（可选：不跑也行，构建脚本会自己配；跑一次能提前看到环境问题）
pwsh -NoProfile -File tools/setup_native_env.ps1              # 写缓存，已就绪时秒返回
pwsh -NoProfile -File tools/setup_native_env.ps1 -Check       # 只检查并报告，不下载也不写缓存
pwsh -NoProfile -File tools/setup_native_env.ps1 -Android     # 顺带解析 Android NDK（交叉编译用）

# 1) 构建插件框架：插件框架库 + 插件 SDK（主程序要的就是这一份）
#    产物装到 <构建目录>/musicxx-extern-plugin-install 并复制到 .native/output/<平台>-<架构>-<配置>/
pwsh -NoProfile -File tools/build_native.ps1

# 2) 要示例插件/原生测试时才跑这个
pwsh -NoProfile -File tools/build_examples.ps1                # 构建示例插件并打包成 .zip
pwsh -NoProfile -File tools/build_examples.ps1 -RunTests      # 顺带跑原生测试
```

Linux / macOS 的同一套流程（示例插件单独一条脚本，构建 + 打包 + 可选跑测试）：

```bash
./tools/build_native.sh                                        # 只构建插件框架（插件框架库 + SDK）
./tools/build_examples.sh                                      # 构建示例插件并打包成 .zip
./tools/build_examples.sh --run-tests                          # 顺带跑原生测试
./tools/build_examples.sh --force                              # 不跳过，强制重建一次
./tools/pack_plugin.sh --plugin-dir <插件目录>                 # 只打某一个插件目录（产物与 ps1 版同名）
```

> 打包出来的安装包在 `.native/output/<平台>-<架构>-<配置>/packages/`，可以直接
> `musicxx-cli plugin install "<包路径>" --enable` 装进主程序试。

**主程序构建会自动触发插件框架构建**：`flutter build windows` / `flutter build linux` / `flutter build apk`
都会在配置阶段调用上面第 1 步（自动模式），因此不必先手工构建 —— 已经是最新时只花一次脚本启动的时间，
源码改过就增量重建，环境没配过就现场配（会按需下载 Boost）。关掉与调整：

```powershell
$env:MUSICXX_EXTERN_PLUGIN_AUTO_BUILD = '0'      # 关掉自动构建（全平台；Android 也可用 -PmusicxxExternPluginAutoBuild=false）
$env:MUSICXX_EXTERN_PLUGIN_CONFIG = 'Debug'      # 自动构建改用 Debug 插件框架库（默认 Release；Android 用 -PmusicxxExternPluginConfig=Debug）
# 桌面端也可以直接在 CMake 上关: cmake ... -DMUSICXX_EXTERN_PLUGIN_AUTO_BUILD=OFF
```

框架构建的常用开关：

```powershell
pwsh -NoProfile -File tools/build_native.ps1 -Config Debug       # Debug (独立构建目录)
pwsh -NoProfile -File tools/build_native.ps1 -WithExamples       # 连示例插件一起构建
pwsh -NoProfile -File tools/build_native.ps1 -WithTests          # 连原生测试一起构建（含对照示例）
pwsh -NoProfile -File tools/build_native.ps1 -RunTests           # 构建后跑原生测试（隐含示例与测试）
pwsh -NoProfile -File tools/build_native.ps1 -DepsOnly           # 只构建依赖库 (target cxx_pluginxx_repo)
pwsh -NoProfile -File tools/build_native.ps1 -ConfigureOnly      # 只做预检/Boost/configure
pwsh -NoProfile -File tools/build_native.ps1 -Clean              # 先清空构建目录与输出目录
pwsh -NoProfile -File tools/build_native.ps1 -Jobs 8             # 指定并行度
pwsh -NoProfile -File tools/build_native.ps1 -Force              # 不跳过，强制走一遍构建
pwsh -NoProfile -File tools/build_native.ps1 -Auto               # 自动模式：已是最新则秒退、失败不阻断
```

Linux / macOS 用同一套能力的 shell 版本（选项名不区分大小写，`--deps-only` 与 `-DepsOnly` 等价；
环境配置与示例插件构建同在这一个脚本里，用选项区分）：

```bash
./tools/build_native.sh                       # 插件框架（插件框架库 + SDK）
./tools/build_native.sh --config Debug        # Debug (独立构建目录 .native/build/linux-debug)
./tools/build_native.sh --with-examples       # 连示例插件一起构建（打包用 ./tools/build_examples.sh）
./tools/build_native.sh --with-tests          # 连原生测试一起构建
./tools/build_native.sh --deps-only           # 只构建依赖库
./tools/build_native.sh --configure-only      # 只做预检/Boost/configure
./tools/build_native.sh --clean               # 先清空构建目录与输出目录
./tools/build_native.sh --run-tests           # 构建后跑原生测试 (失败时脚本返回非 0)
./tools/build_native.sh --jobs 8              # 指定并行度
./tools/build_native.sh --auto                # 自动模式：已是最新则秒退、失败只警告（主程序构建用它）
./tools/build_native.sh --skip-if-fresh       # 已是最新则跳过，但要构建时失败照常报错（示例插件脚本用它）
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

- **环境缓存**：`tools/setup_native_env.ps1` 把解析结果（生成器 / 工具路径 / Boost 头文件根 / NDK）写进
  `.native/env/environment.json`，构建脚本直接复用 —— 重复构建不再重新探测工具链、不再重复解析 Boost。
  删掉这个文件（或加 `-Force`）就会重新解析一遍。
- **"已是最新"怎么判断**：每次成功构建会在构建目录写下 `.build_stamp_<framework|examples|tests>` 标记
  （`-DepsOnly` / `-ConfigureOnly` 不写），自动模式比较"标记 vs 源码最后一次改动时间"，
  因此 `flutter build` 不必每次重新走一遍 CMake；判定不成立（源码改过、产物被删、配置变了）时才重编。
- **示例插件默认不构建**（`-DMUSICXX_EXTERN_PLUGIN_BUILD_EXAMPLE=OFF`）：主程序只需要插件框架库，示例插件交给
  `tools/build_examples.ps1`（Linux/macOS 用 `tools/build_examples.sh`，底层是 `build_native.sh --with-examples` + 打包），
  装包/跑测试时再构建。
  `plugins/` 里新增插件后同样跑一次它，产物会复制到 `.native/output/<平台>-<架构>-<配置>/plugins/`
  （原生测试用的就是这个目录），安装包在 `.native/output/<平台>-<架构>-<配置>/packages/`。
- **工具链要求**（插件框架是 C++26）：MSVC ≥ 19.4x（VS 17.14+/VS 18）、GCC ≥ 14、Clang/NDK ≥ 18；
  Linux 上还需要 `pkg-config`（`cxx_utilxx_base` / `cxx_pluginxx` 配置阶段有 `find_package(PkgConfig REQUIRED)`）；
  shell 脚本会在预检里检查编译器大版本与 `pkg-config`，不满足时给出警告。
- **安装前缀**（`<构建目录>/musicxx-extern-plugin-install`）是唯一的依赖来源，里面同时有
  `lib/cmake/*` 供 `find_package` 使用、`bin/` 插件框架产物、`plugins/` 示例插件；插件框架工程找不到依赖时
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
- **只导出插件框架库入口**：插件框架库只导出 `musicxx_extern_plugin_*` —— MSVC 靠导出宏；GNU/Clang 另加
  version script（`global: musicxx_extern_plugin_*; local: *;`，只靠 `-fvisibility=hidden` 挡不住静态库
  带进来的 `STB_GNU_UNIQUE` 符号，实测 Boost.Asio 的 error category 会漏出来）；Apple 由 hidden
  visibility 覆盖。

产物（Release；示例插件与原生测试要跑过 `tools/build_examples.ps1` / `tools/build_examples.sh` 才有）：

```
# Windows
<构建目录>/musicxx-extern-plugin-install/bin/musicxx_extern_plugin.dll        插件框架库（只导出 musicxx_extern_plugin_*）
<构建目录>/musicxx-extern-plugin-install/bin/musicxx_extern_plugin_test.exe   原生测试
<构建目录>/musicxx-extern-plugin-install/plugins/example_native/              示例插件（库文件 + plugin.yaml）
.native/output/windows-x64-release/{bin,plugins}/                            上述产物的稳定复制（供打包/手工取用）

# Linux / macOS（库名按平台：lib*.so / lib*.dylib，可执行文件无扩展名）
<构建目录>/musicxx-extern-plugin-install/bin/libmusicxx_extern_plugin.so     插件框架库
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

> 分发到 Linux 桌面时注意：插件框架库的 glibc 需求取决于**构建机**（本仓实测 Ubuntu 22.04 上为 `GLIBC_2.35`），
> 且运行期会用到主程序自带的 `libstdc++`（`resource/libs/linux/lib/`）。发布用的 Linux 产物建议在
> 目标发行版（或更老的发行版）上构建，并在 `flutter build linux` 后核对 `bundle/lib/libmusicxx_extern_plugin.so`。

## 打包（随主程序分发）

插件框架库**不是普通的"插件源码编译"**（依赖链太重：fmt / yaml-cpp / simdjson / libiconv / uchardet / QuickJS），
所以本包把它当作"外部构建产物"来打包，并且在主程序构建时**按需自动构建**：

```
pubspec.yaml             flutter.plugin.platforms.{windows,linux,android}.ffiPlugin = true
windows/CMakeLists.txt   ① 缺库/过期时调包内构建脚本的自动模式（tools/cmake/host_autobuild.cmake）
linux/CMakeLists.txt        （已是最新则秒级返回；关掉见下面的开关）
                         ② 找到已构建的插件框架库 → 写 musicxx_extern_plugin_bundled_libraries
                         → Flutter 放进 PLUGIN_BUNDLED_LIBRARIES → Windows 装到可执行文件旁、Linux 装到 <bundle>/lib/
android/build.gradle     ① 每次构建都调包内构建脚本确保插件框架库最新（缺库则构建、源码改过则增量重编、
                            已是最新则秒退；每个目标 ABI 各一次）
                         ② 按 ABI 收集 .so 到 jniLibs（每个 build type 一个目录）
                         → AGP 打进 APK 的 lib/<abi>/libmusicxx_extern_plugin.so
主程序 macos/            Runner 目标的 "musicxx-extern-plugin" 构建阶段（macos/tool/copy_extern_plugin_dylib.sh）
                         ① 缺库/过期时同样调包内构建脚本的自动模式
                         ② 把 libmusicxx_extern_plugin.dylib 复制到 musicxx.app 的 Contents/Frameworks
                         （macOS 的 Flutter 插件走 CocoaPods，pod 里的脚本拿不到应用包输出目录，
                           所以这一步只能由主程序的 Runner 工程做）
主程序 ios/              Runner 目标的 "musicxx-extern-plugin" 构建阶段（ios/tool/embed_extern_plugin_dylib.sh）
                         ① 缺库/过期时调包内构建脚本的自动模式（模拟器 --ios-simulator、设备 --ios）
                         ② 把 libmusicxx_extern_plugin.dylib 复制到 Runner.app/Frameworks，
                            并用当前签名身份补签一次（iOS 只允许加载应用包内、与主程序同签名身份的库）
```

也就是说：`flutter build windows` / `flutter build linux` / `flutter build macos` / `flutter build apk`
**不需要先手工构建插件框架库**。
自动构建可以关掉（CI 里通常关掉它，改成显式构建 + `-DMUSICXX_EXTERN_PLUGIN_HOST_LIBRARY=<库路径>` 保证"必须打包"）：

```powershell
$env:MUSICXX_EXTERN_PLUGIN_AUTO_BUILD = '0'           # 全平台关掉自动构建
# 桌面端也可以在 CMake 上关: -DMUSICXX_EXTERN_PLUGIN_AUTO_BUILD=OFF
# Android 也可以: -PmusicxxExternPluginAutoBuild=false
```

- 自动构建的时机是**平台工程的配置/构建阶段**（Windows/Linux 是 CMake configure 期：查找结果要交给
  `PLUGIN_BUNDLED_LIBRARIES`，而主程序侧的 `install(FILES ...)` 在构建末尾要求文件已经在位；
  macOS 是 Runner 的构建阶段）：
- 自动构建的目标配置默认是 **Release**（Flutter 构建在 configure 阶段分辨不出 Debug/Release）；
  需要 Debug 插件框架库时用 `$env:MUSICXX_EXTERN_PLUGIN_CONFIG='Debug'`（Android 用
  `-PmusicxxExternPluginConfig=Debug`），或者手工 `tools/build_native.ps1 -Config Debug` 构建一次
  （两份都存在时，桌面端按 Flutter 的构建配置选，Profile 取 Release）；
- 自动构建失败**不会中断主程序构建**：只打警告，本次产物里不带插件框架库（外部插件功能不可用）；
- 查找顺序（与 Dart 侧 `native_library.dart` 同一套约定）：
  `-DMUSICXX_EXTERN_PLUGIN_HOST_LIBRARY` / `-PmusicxxExternPluginLibDir` → 环境变量
  `MUSICXX_EXTERN_PLUGIN_LIBRARY`（Android 侧是 `MUSICXX_EXTERN_PLUGIN_ANDROID_LIB_DIR`）
  → `<包>/.native/output/<平台>-<架构>-<配置>/bin/` → `<包>/.native/build/<平台>-<配置>/musicxx-extern-plugin-install/bin/`
  → 仓库预置目录（`resource/libs/windows/lib/`、`resource/libs/linux/lib/`、`resource/libs/macos/lib/`、
  `resource/libs/android/<abi>/`）；
- Debug 与 Release 两份产物都在时，按**当前 Flutter 构建配置**选择（`Profile` 取 Release）；
- **找不到插件框架库只打警告、不中断构建**（外部插件是可选功能，缺失时主程序照常启动，Dart 侧给出提示）。
  发布流水线若要求"必须打包"，请显式传 `-DMUSICXX_EXTERN_PLUGIN_HOST_LIBRARY=<路径>`（macOS 传
  `MUSICXX_EXTERN_PLUGIN_LIBRARY=<路径>`），并在 CI 里自行判定失败；
- 例（Windows）：`flutter build windows --release` → `build/windows/x64/runner/Release/musicxx_extern_plugin.dll` 与 `musicxx.exe` 同目录；
- 例（Linux）：`flutter build linux --release` → `build/linux/x64/release/bundle/lib/libmusicxx_extern_plugin.so`
  （桌面应用的库都放 `bundle/lib/`，Dart 侧会去"可执行文件旁的 `lib/`"目录找它）；
- 例（macOS）：`flutter build macos --release` → `build/macos/Build/Products/Release/musicxx.app/Contents/Frameworks/libmusicxx_extern_plugin.dylib`
  （Dart 侧会去应用包的 `Contents/Frameworks` 找它，见下面 macOS 一节）。

### Android（已接入）

```bash
# 1) 插件框架库由 android/build.gradle 自动保持最新（缺库则构建、C++ 改过则增量重编、已最新则秒退）
#    想先把环境与重型依赖库编好（或排查构建问题）也可以手工跑：每个 ABI 一次，Windows 侧同样支持
pwsh -NoProfile -File tools/build_native.ps1 -Android -Abi arm64-v8a      # Windows
ANDROID_NDK_HOME=<ndk> tools/build_native.sh --android --abi arm64-v8a    # Linux/macOS
# 2) 正常打包：库由 android/build.gradle 收集进 jniLibs
flutter build apk --release
```

Android 与桌面端的差别（改这块之前先读 `android/README.md`）：

- 插件框架库**必须随 APK 分发**：Android 7 起动态链接器只允许应用从 APK 的原生库目录（与系统目录）加载
  动态库，应用数据目录里的 `.so` 会失败（`is not accessible for the namespace "classloader-namespace"`）。
  所以 Dart 侧在 Android 上**只按库名加载**（`DynamicLibrary.open('libmusicxx_extern_plugin.so')`，
  由系统解析到 `lib/<abi>/`），桌面端那套绝对路径候选不参与；
- **用户安装的动态库插件**因此可能加载失败：插件框架按"安全降级"处理（标记不可用 + 事件 + 不重试），
  主程序与其它插件照常工作；JS插件不受影响。管理页会提前说明这条限制，失败原因也会换成面向用户的说法；
- 交叉编译统一 Ninja 生成器 + `c++_static`（C++ 运行库静态链进插件框架库，APK 不需要额外的 `libc++_shared.so`）；
  `-RunTests`/`--run-tests` 在 Android 上跳过（测试可执行文件是给设备编的）。

其余平台：OHOS 的打包属于后续工作（只跑 JS插件，需要静态库 + podspec）。

### iOS（已接入）

```bash
# 1) 插件框架库由主程序 Runner 的构建阶段保持最新（缺库则构建、C++ 改过则增量重编、已最新则秒退）；
#    自动模式会按当前 SDK 选目标：iphonesimulator → --ios-simulator，iphoneos → --ios
#    想先把库单独编好（或排查构建问题）也可以手工跑（只能在 macOS 上构建）：
./tools/build_native.sh --ios-simulator        # → .native/output/ios-simulator-<架构>-release/bin/libmusicxx_extern_plugin.dylib
./tools/build_native.sh --ios                  # 设备（arm64）→ .native/output/ios-arm64-release/bin/...
./tools/build_native.sh --ios --run-tests      # 设备目标跳过测试（产物是给设备编的）
./tools/build_native.sh --ios-simulator --run-tests   # 模拟器目标能直接在开发机上跑（自动补 DYLD_ROOT_PATH）
# 2) 之后正常构建主程序即可（Runner 的 musicxx-extern-plugin 构建阶段会把库复制进包）
flutter build ios --simulator        # → build/ios/iphonesimulator/Runner.app/Frameworks/libmusicxx_extern_plugin.dylib
flutter build ios                    # 设备构建 → Runner.app/Frameworks/...，并用当前签名身份补签
```

iOS 与桌面端/Android 的差别（改这块之前先读这一节，以及主程序 `ios/tool/embed_extern_plugin_dylib.sh`）：

- **插件框架库必须随 `.app` 分发**：iOS 只允许加载应用包内、且与主程序同一签名身份的动态库；
  Runner 的构建阶段把它复制到 `Runner.app/Frameworks/` 后用 `EXPANDED_CODE_SIGN_IDENTITY` 补签一次。
  关掉签名的构建（`--no-codesign`、模拟器构建）本来就允许加载未签名的库，脚本会跳过签名；
- **用户安装的动态库插件在设备上会被系统拒绝**（库在应用数据目录里、没有与主程序一致的签名），
  插件框架按"安全降级"处理（标记不可用 + 事件 + 不重试），JS插件不受影响；
  管理页会提前说明这条限制，dyld 的 `code signature invalid` / `Library Validation` / `Team ID` 报错
  会换成面向用户的说法（主程序 `ExternPluginPlatform_c.isSystemBlockedNativeReason`）；
  模拟器上没这条限制（库校验宽松），因此模拟器里能加载的插件在真机上不一定能加载；
- **设备与模拟器分别是两份产物**（平台段 `ios` / `ios-simulator`，架构：设备固定 arm64、模拟器用开发机架构），
  各自 build/output 目录独立；Dart 侧在 iOS 上只挑 `ios-*` 的产物目录（否则会命中同目录下的 macOS 产物）；
- 交叉编译用 CMake 内置的 iOS 支持（`CMAKE_SYSTEM_NAME=iOS` + `CMAKE_OSX_SYSROOT/ARCHITECTURES/DEPLOYMENT_TARGET`），
  最低版本默认 15.0（`--ios-target` 可改，与主程序 `IPHONEOS_DEPLOYMENT_TARGET` 一致）；
  Apple 低版本目标的浮点 `std::to_chars` 不可用（iOS 16.3 / macOS 13.3 起才有），插件框架的 JSON 实现里有等价回退
  （见 `src/third_party/cxx_utilxx_base/src/json.cpp` 文件头）；
- **`--run-tests` 在模拟器目标上会真的跑**：模拟器产物是同一 CPU 架构的 Mach-O，构建脚本会自动把
  `DYLD_ROOT_PATH` 指向匹配的模拟器运行时根目录（找不到运行时则跳过并提示
  `xcrun simctl spawn booted <测试可执行文件> <插件目录>`）；设备目标跳过（同 Android）。

### macOS（已接入）

```bash
# 1) 插件框架库由主程序 Runner 的构建阶段保持最新（缺库则构建、C++ 改过则增量重编、已最新则秒退）
#    想先把库单独编好（或排查构建问题）也可以手工跑：macOS 与 Linux 共用 tools/build_native.sh
./tools/build_native.sh        # → .native/output/macos-<架构>-release/bin/libmusicxx_extern_plugin.dylib
# 2) 正常打包：库由主程序的 macos/ 工程复制进应用包
flutter build macos --release  # → musicxx.app/Contents/Frameworks/libmusicxx_extern_plugin.dylib
```

macOS 与其它桌面的差别（改这块之前先看这两处）：

- **复制进应用包的动作由主程序的 Runner 工程做**：`macos/Runner.xcodeproj` 里名为 `musicxx-extern-plugin`
  的构建阶段调用 `macos/tool/copy_extern_plugin_dylib.sh`（与 `musicxx-cli` 阶段同一套路）。macOS 的 Flutter
  插件走 CocoaPods，pod 里的脚本阶段拿不到应用包的输出目录，所以 Windows/Linux 那种"包内 CMakeLists 交给
  `PLUGIN_BUNDLED_LIBRARIES`"在 macOS 上没有对应物；目标位置是 `musicxx.app/Contents/Frameworks/`
  （与预置库 `libmediaxx.dylib` 同一个目录），Dart 侧会去那里找；
- **开关与查找顺序与其它平台一致**：`MUSICXX_EXTERN_PLUGIN_AUTO_BUILD=0` 关掉自动构建、
  `MUSICXX_EXTERN_PLUGIN_CONFIG=Debug` 换配置、`MUSICXX_EXTERN_PLUGIN_LIBRARY=<dylib 路径>` 显式指定；
  复制过去之后，脚本还会用当前签名身份给 dylib 补一次签名（拿不到身份时保持原样，不影响加载）；
- **Hardened Runtime 下必须关掉库签名校验**：主程序是 Hardened Runtime 签名（工程里的
  `ENABLE_HARDENED_RUNTIME = YES`），系统默认只允许加载**同团队签名**的动态库，而插件框架库与插件作者
  自己编的库都不是这个团队 —— 不加 `com.apple.security.cs.disable-library-validation` 时 `dlopen` 会得到
  `code signature ... not valid for use in process: mapping process and mapped file (non-platform) have
  different Team IDs`（用户看到的就是"外部插件框架不可用"）。这一项已经在
  `macos/Runner/DebugProfile.entitlements` 与 `macos/Runner/Release.entitlements` 里，不要再删；
- **库与开发机架构一致**：包内构建脚本按开发机 `uname -m` 产出 `macos-arm64-*` / `macos-x64-*`，脚本按同一规则找库，
  因此不做多架构合并（工程本身也限制只构建当前架构）；要在别的架构上出包，需在该架构机器上构建一次原生库；
- **最低版本跟主程序一致（12.0）**：构建脚本显式把 `CMAKE_OSX_DEPLOYMENT_TARGET` 交给 CMake 与嵌套构建，
  主程序构建阶段会用 `--macos-target` 传自己的 `MACOSX_DEPLOYMENT_TARGET`。不传的话 CMake 用"构建机当前的
  系统版本"，产物会按"只在同样新的系统上运行"编译（旧系统上用到新系统才有的符号时，`dlopen` 报
  `Symbol not found ... (built for macOS X which is newer than running OS)`，表现为外部插件功能不可用）；
- 手工检查（不经过 Xcode）：
  `MUSICXX_EXTERN_PLUGIN_APP_DIR=build/macos/Build/Products/Release/musicxx.app bash macos/tool/copy_extern_plugin_dylib.sh`；
- 用自定义构建流程（不走 Xcode 的 `flutter build macos`）时，也可以用
  `MUSICXX_EXTERN_PLUGIN_LIBRARY=<dylib 绝对路径>` 让 Dart 侧直接加载，不依赖应用包里的那一份。

## 写一个插件

三种形态各有完整指南（本文件只给最小示例与跳转）：

| 形态 | 目录内容 | 指南 |
|---|---|---|
| 动态库插件 | `plugin.yaml` + 库文件 + 可选 `shader/` | [docs/plugin-native-api.md](docs/plugin-native-api.md) |
| 动态库插件（多目标包） | `plugin.yaml` + `lib/<系统>-<架构>/库文件`（一个包支持多个系统/架构） | 同上 §1.3 |
| JS插件 | `plugin.yaml` + `plugin.js` | [docs/plugin-js-api.md](docs/plugin-js-api.md) |
| 着色器（两种形态都能用） | `shader/*.frag` + `bundle.json` + `*.shaderbundle` | [docs/plugin-shader-bundle.md](docs/plugin-shader-bundle.md) |

### 动态库插件（SDK 与构建模板）

插件作者只需要 SDK 头文件 + 一个 CMake 助手，不需要了解插件框架工程结构：

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_plugin LANGUAGES CXX)

# SDK 前缀 = `tools/build_native.ps1` 的安装前缀（含 include/ 与 lib/cmake/）
set(musicxx_extern_plugin_DIR "<安装前缀>/lib/cmake/musicxx_extern_plugin")
find_package(musicxx_extern_plugin CONFIG REQUIRED)

musicxx_plugin_add_target(my_plugin
  SOURCES my_plugin.cpp
  MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
  ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/shader"     # 可选：随插件分发的资源（如 shader bundle）
         "${CMAKE_CURRENT_SOURCE_DIR}/resource"   # 可选：插件自己的图标（声明处写相对插件目录的路径）
)
```

`musicxx_plugin_add_target`（`src/sdk/cmake/musicxx_plugin.cmake`）负责：

- 建 SHARED 库并链接 SDK 与插件框架/工具库（`cxx_pluginxx_static`/`cxx_utilxx_base_static`/`fmt::fmt` 存在就链接，插件作者不用记名字）；
- C++26 + MSVC `/utf-8` + 符号默认隐藏；
- **只导出入口符号**：非 MSVC 平台加 version script（GNU/Clang）或导出符号表（Apple），因此插件不会把插件框架/C++ 运行时符号暴露出去；
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
`pwsh tools/pack_plugin.ps1 -PluginDir <包目录>`（Linux/macOS 用 `./tools/pack_plugin.sh --plugin-dir <包目录>`）
打成安装包（它会打印包内分支与每个分支的库文件，
便于确认齐全）；插件框架运行时按当前系统与 CPU 架构选分支（选择顺序、标签别名与规则见
[docs/plugin-native-api.md](docs/plugin-native-api.md) §1.3，示例插件 `plugins/example_native_multi/`）。
**分支选择规则只有插件框架一处实现**：扫描、加载与主程序安装预检（C ABI `plugin_inspect`）跑的是同一份
判定，Dart 侧只解析结果。不传 `TARGET_TAG` 时行为与以前一致（库与清单同一层，直接作为插件目录）。

### JS插件

```text
my_plugin/
├── plugin.yaml              # name / kind: js / version / api_version / permissions ...
├── plugin.js                # 顶层同步注册钩子与能力
└── resource/svg/example.svg # 可选：插件自己的图标（UI 项写 icon: "resource/svg/example.svg"，
                             #   resource/svg/ 只是推荐位置）
```

不需要 CMake、不需要编译；放进插件目录后「重新扫描」即可。完整 API、硬约束与排障见
[docs/plugin-js-api.md](docs/plugin-js-api.md)；接口本身（动作 / 状态镜像 / 变量 / 事件 / 能力 / 功能点）
见 [docs/plugin-api.md](docs/plugin-api.md)。

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

bundle 里的 `format_version` 必须与目标主程序的 Flutter 版本一致（基线版本 3.47.5 对应 2），
编好后在 UI 项里绑定它。完整字段、uniform 约定与排障见
[docs/plugin-shader-bundle.md](docs/plugin-shader-bundle.md)。

### 装上去、改完刷新（命令行 / MCP）

主程序自带命令行工具 `musicxx-cli`（与主程序同目录，macOS 在 `Musicxx.app/Contents/MacOS/`），
一条命令就能完成"安装 → 启用 → 看日志 → 改完重载 → 卸载"，也能经 `musicxx-cli mcp` 把这些
操作交给 AI 客户端：

```bash
pwsh -NoProfile -File tools/pack_plugin.ps1 -PluginDir D:/work/my_plugin        # 打包（顶层是插件目录内容）
./tools/pack_plugin.sh --plugin-dir /work/my_plugin                             # Linux / macOS 上的同一件事
musicxx-cli plugin install "D:/work/my_plugin-2026_1010_1.zip" --enable        # 安装并启用（包名 = 插件id-清单版本号）
musicxx-cli plugin list                                                         # 每个插件的状态与失败原因
musicxx-cli plugin logs my_plugin --limit 50                                    # 最近日志（新的在前）
musicxx-cli plugin reload my_plugin                                             # 改完代码重新加载
musicxx-cli render select plugin.my_plugin.bg                                   # 让播放页用上插件的背景样式
musicxx-cli plugin uninstall my_plugin                                          # 卸载（--keep-data 保留私有数据）
```

完整命令表、MCP 工具名与参数、退出码约定见
[docs/plugin-agent-cli.md](docs/plugin-agent-cli.md)。

参考实现：`plugins/example_native/`（钩子/能力/动作/事件/UI/存储/日志全演示）、`plugins/example_js/`（等价 JS 版）、
`plugins/example_js_shader/`（只演示播放页背景与动画速率；三种背景样式各一个 bundle：晶格 `bg.frag`、光圈 `ring.frag`、
插件纹理铺底复用 `bg.frag`）、
`plugins/playing_bg_image/`（显示名『示例封面背景』：三种播放页背景模式『模糊』/『沉浸』/『黑胶』+ 接管播放页歌曲图 + 自定义封面取色 + 按封面主色与亮暗主题推播放页前景三色（保留主色色相与彩色程度，按当前模式估出的背景亮度保证对比度）：
背景、前景色、图标槽位与钩子在同一个插件里串起来，覆盖 `cover` 纹理（含圆裁用的 `square`）、同一个槽位注册多项、`playing.icon` 三种形态里的 `none`、
`musicxx.media.palette.provide`、`foregroundColors`（运行期改前景三色）；『黑胶』还演示了线段距离场画的唱臂与按 `speed` 旋转的盘心封面）；
另外 `plugins/example_js_vars/`（变量通道）、
`plugins/example_js_multi_script/`（清单 `scripts` 多脚本），以及下面「对照示例」一组 —— 它们演示插件框架在
插件写坏时的保护行为，写插件前先看一眼可以少踩坑。
钩子总表与派发方式（`sync`/`async`）见生成物 `docs/plugin-hooks.md`；界面字段见 `docs/plugin-ui.md`。

## 原生测试

插件框架按「父目录下的每个子目录 = 一个插件」扫描，因此测试参数要传**插件目录的父目录**：

```powershell
pwsh -NoProfile -File tools/build_examples.ps1 -RunTests    # 自动用 <安装前缀>/plugins 作为插件目录
```

```bash
./tools/build_examples.sh --run-tests                       # Linux / macOS 上的同一件事
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
flutter test                                   # 整套流程验证（框架/JS/配置读写；需要先构建原生库；含版本号校验 version_test.dart）
dart run tools/self_check.dart                 # 纯 Dart 自检（不依赖 Flutter，便于定位 FFI 卡住的位置）
```

Dart 侧用法（详见 `lib/musicxx_extern_plugin.dart` 文件头）：

```dart
// 一次生命周期一个运行时：插件框架需要"停掉再启动"时必须新建实例（dispose 过的实例不能再次 init）
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
runtime.plugins.load('example_native');                      // 加载（同步等待，带超时）
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

// 注册主程序动作实现（插件经 request_action 发起；未注册的动作会立刻收到"未找到"）
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

- 原生库定位顺序：环境变量 `MUSICXX_EXTERN_PLUGIN_LIBRARY` → `.native/output/*/bin/`（按当前平台过滤目录名前缀；
  Release 优先、其次按修改时间）→ `.native/build/*/musicxx-extern-plugin-install/bin/` → 可执行文件旁（含其 `lib/`）
  → macOS 应用包的 `Contents/Frameworks/` 与 iOS 应用包的 `Frameworks/` → 纯库名（系统搜索路径）；
  Android 只按库名加载（库在 APK 的 `lib/<abi>/` 里，绝对路径候选不参与）；
- 找不到库时抛 `MusicxxPluginLibraryException`（含逐个候选与原因），版本不匹配抛 `MusicxxPluginApiVersionException`；
- **版本号**：`MusicxxPluginVersion`（`lib/src/version.dart`，版本 / 提交 / 日期 / 仓库地址）与 C 头宏
  `MUSICXX_EXTERN_PLUGIN_VERSION` 必须是同一个值（`test/version_test.dart` 校验），`runtime.libraryVersion`
  给出加载到的原生库自报的版本 —— 两者不一致说明库不是这次构建的产物；对照表与改版本号的流程见
  `docs/version-history.md`；
- 包内 `test/`、`docs/plugin-hooks.md`、`lib/src/*.g.dart`、`src/sdk/include/musicxx/plugin/api/hook_ids.g.h`
  都是生成物或可重跑的用例，不要手工改生成物。

## 能力现状

插件框架已经实现的能力（主程序接入情况见 musicxx 仓库）：

- **专用接口表**：`musicxx.hooks` / `musicxx.host` / `musicxx.ui` / `musicxx.vars` 四张已实现
  （每张表的版本 / 最低版本 / 基线字节数与可用性判定见 `docs/plugin-native-api.md` §4.1）；
  `musicxx.player` / `library` / `lyrics` / `storage` / `net` / `stats` 的 IID 已冻结、表体未实现
  （查询返回 NULL）→ 这些能力统一走动作名（`requestAction("musicxx.player.play", ...)`），由主程序分派；
- **钩子**：约定 71 个（观察 / 裁决，其中 52 个异步派发）；
  主程序当前**应用派发 13 个**（`docs/plugin-hooks.md` 的「主程序是否已接入」列标「已接入」的共 14 行，
其中 `musicxx.feature.point.call` 是插件自定义功能点的通用通道、由调用方触发），其余注册成功但不会触发；
  两个预留的观察钩子（`musicxx.media.wave.ready` / `musicxx.media.chorus.analysed`）已标废弃（主程序不派发，替代做法见其说明）；
  第 71 条 `musicxx.feature.point.call` 是**插件自定义功能点**的通用通道（不是"主程序会派发的钩子"）：
  插件声明自己的功能点后，用它实现、也用它被调用（见 `docs/plugin-guide.md` 的「功能点」一节）；
- **JS 运行时**：QuickJS 编入插件框架库、共享一条 JS 线程、每个插件独立 `JSRuntime`、
  `js:<pluginId>` 合成实例、顶层注册统一回放、运行期注册由框架线程执行；
  裁决处理器可以返回 Promise（插件框架一直等到它结算，结算后裁决生效）；
- **声明式 UI**：UI 项（主页入口 / 歌曲菜单 / 歌单菜单 / 播放页背景 / 播放页歌曲图）与插件自绘页面
  （`ext://<插件id>/<视图id>`）都已实现；主程序的主页入口、歌曲菜单、歌单菜单与插件页面都已有渲染点
  （字段与渲染表现见 `docs/plugin-ui.md`）；
- **插件渲染**：两个渲染槽位都已接入 —— `player.background`（播放页背景）与 `player.icon`
  （播放页歌曲图，可接管的形态有 `shader` / 界面视图 / 只占位三种）。插件交付 shader bundle（或界面视图），
  主程序运行期加载并渲染；**用户选中才生效**，没被选中时零成本。插件页面里还可以内联 `Shader` 块；
- **配置与网络**：插件配置放在插件目录的 `config.json`（插件自己读写，框架不渲染配置表单）；
  `musicxx.net.fetch`/`download` 经主程序统一网络栈（插件框架不限定域名）；
- **观测**：钩子统计（调用 / 耗时 / 超时 / 失败 / 被暂停的处理器）、插件各阶段耗时、注册项计数、
  JS 实例统计与内存采样、内存环形事件日志；
- **平台打包**：Windows 与 Linux 已接入（插件框架库随主程序分发，见「打包（随主程序分发）」）、
  Android 已接入（NDK 交叉编译 + `jniLibs`，见 `android/README.md`）、
  macOS 已接入（主程序 Runner 的 `musicxx-extern-plugin` 构建阶段复制到 `Contents/Frameworks`，
  另需 `disable-library-validation`，见「打包（随主程序分发）」的 macOS 一节）、
  iOS 已接入（同一个构建阶段由 `ios/tool/embed_extern_plugin_dylib.sh` 复制到 `Runner.app/Frameworks`
  并补签名，见同节的 iOS 一节）；
  OHOS 的平台工程属于后续工作（只跑 JS插件）。

对照示例（故意做出问题行为的示例插件：既服务原生测试，也让插件作者看清「写坏了会怎样」。
它们随 `MUSICXX_EXTERN_PLUGIN_BUILD_TESTS` 一起构建，并安装到 `<安装前缀>/plugins/`）：

| 对照示例 | 演示 / 验证点 |
|---|---|
| `plugins/example_native_fail/` | 钩子处理器总是失败 → 插件框架只记统计（`hook_stats` 里 `failures`，不暂停、不卸载），同插件的其它处理器照常工作 |
| `plugins/example_native_bad_entry/` | 库文件缺 `musicxx_plugin_start`/`stop` → 加载阶段按约定拒绝（明确失败、有可读原因、无注册残留，对应用例 `test_entry_symbols`） |
| `plugins/example_js_async/` | 裁决处理器返回 Promise → 插件框架一直等到结算，裁决生效（`asyncHookSettled`） |
| `plugins/example_js_broken/` | 脚本语法错误 → 加载失败并回滚，插件框架继续可用 |

插件侧可用的能力：

| 能力 | 入口 | 说明 |
|---|---|---|
| 钩子 | `musicxx.hooks.register` / `PluginBase::hook` / `observe` | 观察型与裁决型；裁决处理器可同步返回，也可返回 Promise（JS：插件框架一直等到结算，结算后裁决生效）；`dispatch: async` 的钩子由插件框架异步派发、不占用调用线程。约定与载荷见 `docs/plugin-hooks.md` |
| 动作 | `musicxx.call` / `PluginBase::requestAction` | 播放 / 库 / 歌词 / UI / 存储 / 网络 / 渲染 / 杂项（主程序分派；不做权限校验） |
| 状态镜像 | `musicxx.state.get` / `PluginBase::stateJson` | 只读快照（不含临时直链/token）；键、字段与推送情况见 `docs/plugin-api.md` §2 |
| 配置 | `musicxx.storage.getConfig/setConfig`（命名空间 `config`）/ `PluginBase::configPath()` | 读写插件目录的 `config.json`（默认值由插件自己给，框架不提供配置表单） |
| 网络（可选便利通道） | `musicxx.net.fetch/download` | 经主程序网络栈；插件框架不限定可访问域名 |
| 声明式 UI | `musicxx.ui.registerEntry` / `PluginBase::uiRegister` | 主页入口 / 歌曲菜单 / 歌单菜单（当前版本无渲染点）/ 播放页背景；字段见 `docs/plugin-ui.md` |
| 能力与跨插件调用 | `musicxx.capability.register/call` | JS↔JS 同线程直调；JS→动态库投递框架线程、脚本不阻塞；JS 处理器可以同步返回，也可以返回 Promise（插件框架等到结算）；C++ 处理器同步返回 |
| 插件页面 | `ext://<插件id>/<视图id>` | 主程序调用插件同名能力取视图描述（Text / Divider / Button / Block / 布局块 / Shader 块） |
| 统计自读 | `musicxx.stats.getSelf/reportMemory/reportMetric` | 只观测不限制 |

验证命令：

```powershell
# Windows（构建 + 原生测试；测试结束会打印 checks=... failed=...，失败时退出码非 0）
pwsh -NoProfile -File tools/build_examples.ps1 -RunTests
```

```bash
# Linux / macOS
./tools/build_examples.sh --run-tests         # 构建示例插件 + 打包 + 跑原生测试（测试结束打印 checks=... failed=...）
./tools/build_native.sh --run-tests           # 只构建 + 跑原生测试（不打包）
```

```powershell
dart run tools/gen_contract.dart --check                 # 约定生成物一致（71 个钩子；改动 hooks.def.json 后必须重新生成）
flutter analyze                                          # 期望 0 issue
flutter test                                     # 包内整套流程：框架/JS/配置读写/多 isolate（需先构建原生库，找不到库时跳过而不是失败）
| 钩子总表（id / 模式 / 派发 / 主程序是否已接入 + 已接入钩子的载荷与裁决） | `docs/plugin-hooks.md`（由 `tools/hooks.def.json` 生成） |
