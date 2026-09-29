# 官方示例插件目录

本目录放 **musicxx 官方实现与示例插件**（第三方插件不需要放在这里：用户插件走应用内的
「从压缩包安装」或直接放入用户插件目录）。

## 目录约定

**每个子目录 = 一个插件**，目录名 = 插件 id = 清单 `name`，安装到 `<安装前缀>/plugins/<目录名>/`；
该目录可以直接作为插件目录使用。因此构建产物里的插件集合与本目录一一对应。

示例分两类：

| 类型 | 插件 | 说明 |
|---|---|---|
| 演示示例 | `example_native` | C++ 动态库：钩子 / 能力 / 事件 / 状态镜像 / 日志 / UI / 播放页背景 |
| 演示示例 | `example_native_multi` | C++ 动态库：**多目标打包**（一个包里放多个系统/架构分支，运行时按当前系统与 CPU 架构选分支） |
| 演示示例 | `example_js` | JS 脚本：与 native 版行为等价（零编译） |
| 演示示例 | `example_js_shader` | JS 脚本：播放页背景样式 + 动画速率设置 + 页面内联着色器块 |
| 演示示例 | `example_js_async` | JS 脚本：裁决型钩子返回 Promise（异步裁决：宿主一直等到结算，结算后裁决生效） |
| 演示示例 | `example_js_vars` | JS 脚本：变量通道（登记插件变量、绑定与读写官方变量） |
| 演示示例 | `example_js_multi_script` | JS 脚本：清单 `scripts` 多脚本按顺序装载（kit 随插件目录分发） |
| 对照示例 | `example_js_broken` | JS 脚本：脚本语法错误 → 演示装载失败与回滚（**永远装载失败**） |
| 对照示例 | `example_native_fail` | C++ 动态库：处理器总是失败 → 演示"失败只记统计"（不暂停、不卸载） |
| 对照示例 | `example_native_bad_entry` | C++ 动态库：缺 `start`/`stop` 入口符号 → 演示装载阶段按契约拒绝（**永远装载失败**） |

> **对照示例是故意做出问题行为的插件**：它们既服务原生测试（`src/tests/test_host.cpp`），
> 也是「写坏了会怎样」的现成例子。不要把它们装到日常使用的环境里 —— 其中两个插件的脚本/库
> 本身就是坏的，装载一定失败。

## 各插件的目录形态

```text
plugins/
├── example_native/             C++ 动态库（构建助手 + 资源目录）
│   ├── CMakeLists.txt          产物目标（用 SDK 构建助手 musicxx_plugin_add_target）
│   ├── example_native.cpp
│   ├── shader/                 播放页背景样式用的 shader bundle（源码 + 打包脚本 + 编译产物）
│   └── plugin.yaml             清单（插件 id 取 name；库文件名取 entry，按 Linux 写法填）
├── example_native_multi/       C++ 动态库（多目标打包：库文件放在 lib/<系统>-<架构>/ 里）
│   ├── CMakeLists.txt          同构建助手，但多传 TARGET_TAG auto（标签与分支目录由助手推导）
│   ├── example_native_multi.cpp
│   └── plugin.yaml             清单只有一份（分支共用）；不用写 platforms/arch
├── example_native_fail/        C++ 动态库（同样形态：CMakeLists.txt + .cpp + plugin.yaml）
├── example_native_bad_entry/   C++ 动态库（不用构建助手，手工建库：导出面必须"有 create、没有 start"）
├── example_js/                  JS 脚本（零编译）
│   ├── plugin.js
│   ├── pluginxx_ui_kit.js      随插件分发的界面 kit（基础）：由 tools/sync_ui_kit.ps1 复制
│   ├── musicxx_ui_kit.js       随插件分发的界面 kit（musicxx 扩展）：由 tools/gen_ui_kit.ps1 生成后复制
│   └── plugin.yaml             清单里 kind: js，scripts 按顺序列出上面三个脚本
├── example_js_shader/           JS 脚本 + shader/ 资源目录（资源直接放在插件目录里）
├── example_js_async/            JS 脚本（只要 plugin.yaml + plugin.js）
├── example_js_vars/             JS 脚本
├── example_js_multi_script/     JS 脚本 + 自己的 kit.js（清单 scripts 里两个脚本）
└── example_js_broken/           JS 脚本（脚本里有语法错误）
```

- **插件 id** 取清单的 `name`（目录名与它保持一致，便于对照构建产物）；
  同名视为同一插件（再次安装即升级覆盖）。
- **动态库插件**：源码放本目录，并在自己的 `CMakeLists.txt` 里建目标（照抄 `example_native/CMakeLists.txt`）；
  然后在 `src/host/CMakeLists.txt` 的插件列表里登记 —— 演示示例加进
  `MUSICXX_EXTERN_PLUGIN_DEMO_NATIVE`、对照示例加进 `MUSICXX_EXTERN_PLUGIN_TEST_NATIVE`，
  该文件的循环会做 `add_subdirectory` 与安装（库文件 + 清单 + 目录里的资源）。
- **JS 插件**：只要有清单 + 脚本（+ 自己的资源），把目录名加进同一个文件的
  `MUSICXX_EXTERN_PLUGIN_DEMO_JS`（或 `MUSICXX_EXTERN_PLUGIN_TEST_JS`）即可被
  `install(DIRECTORY ...)` 复制过去。
- **随插件分发的资源**（shader bundle、图标等）放在插件目录里：动态库插件用
  `musicxx_plugin_add_target(... ASSETS <目录>)` 让构建助手复制到产物目录旁，JS 插件直接放进去即可
  （照抄 `example_js_shader/shader/`）。
- **多目标打包**（`example_native_multi`）：一个包里放多个系统/架构分支时，清单只有一份，
  库文件按 `lib/<系统>-<架构>/` 摆放（仿 APK 的 `lib/<abi>/`）。构建助手在传
  `TARGET_TAG auto` 时按当前构建目标推标签、把库放进 `<包目录>/lib/<标签>/`，
  并把清单与 `ASSETS` 放进 `<包目录>/`；每个平台/架构各构建一次、把 `lib/<标签>/` 合并进同一个
  包目录，就得到一个通用包。宿主扫描与装载时会按当前系统与 CPU 架构选分支，插件详情页
  会显示"已选分支 / 包内分支"。规则与手工打包方式见 `docs/plugin-native-api.md` §1.3。
- **界面 kit 要随插件一起分发**（宿主不提供 kit）：JS 插件用
  `pwsh tools/sync_ui_kit.ps1` 把基础 kit（库的 `js/pluginxx_ui_kit.js`）与 musicxx 扩展 kit
  （本包 `js/musicxx_ui_kit.js`，由 `pwsh tools/gen_ui_kit.ps1` 生成）复制进插件目录，
  并在清单里写 `scripts: [pluginxx_ui_kit.js, musicxx_ui_kit.js, plugin.js]` —— 宿主会在同一个
  JS 上下文里按这个顺序执行；清单 `scripts` 里没写 kit 的插件（例如只演示多脚本装载的
  `example_js_multi_script`）不会被同步，C++ 插件不用复制，直接用 SDK 头文件里的 `musicxx::ui::kit`。
- 新增插件后跑一次 `pwsh tools/build_native.ps1`，产物会复制到
  `.native/output/<平台>-<架构>-<配置>/plugins/`（`-RunTests` 用的就是这个目录）。

## 试着跑一下

```powershell
pwsh -NoProfile -File tools/build_native.ps1              # 构建宿主 + 示例插件
pwsh -NoProfile -File tools/build_native.ps1 -RunTests    # 顺带跑原生测试（父目录 = 插件目录）
```

在应用里试：把 `.native/output/<平台>-<架构>-<配置>/plugins/<插件>/` 整个拷进用户插件目录
（管理页「打开插件目录」），或打包成 `.zip` 用「从压缩包安装」，然后启用。

## 写作参考

| 主题 | 文档 |
|---|---|
| 钩子总表（id / 模式 / 派发 / 预算 / 是否已埋点 + 已埋点钩子的载荷与裁决） | `docs/plugin-hooks.md`（由 `tools/hooks.def.json` 生成） |
| C++ 插件 SDK 与导出宏 | `src/sdk/include/musicxx/plugin/api/plugin_kit.h`（伞头）与 `plugin_api.h`（领域契约） |
| 动态库插件作者指南（清单 / 构建 / 部署 / 排障） | `docs/plugin-native-api.md` |
| JS 插件作者指南（`musicxx` API / 硬约束 / 排障） | `docs/plugin-js-api.md` |
| 界面（UI 项类型与字段 / 插件页面组件 / 用 kit 装配内容 / 设置页写法） | `docs/plugin-ui.md` |
| musicxx 扩展 kit 的组件与参数（生成物） | `docs/musicxx-ui-kit.md` |
| 界面描述层的组件全集、字段与适配规则（库的生成文档） | `src/third_party/cxx_pluginxx_ui/docs/ui-schema.md` |
| 播放页背景（shader bundle 打包与 uniform 契约） | `docs/plugin-shader-bundle.md` |
