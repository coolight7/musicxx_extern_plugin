# 官方插件目录

本目录放 **musicxx 官方实现与示例插件**（第三方插件不需要放在这里：用户插件走应用内的
「从压缩包安装」或直接放入用户插件目录）。

## 目录约定

```text
plugins/
├── example_native/      示例插件（C++ 动态库）：钩子 / 能力 / 事件 / 状态镜像 / 日志 / UI
│   ├── CMakeLists.txt   参与宿主工程的构建（src/host/CMakeLists.txt 用 add_subdirectory 引入）
│   ├── example_native.cpp
│   ├── shader/          播放页背景样式用的 shader bundle（源码 + 打包脚本 + 编译产物）
│   └── plugin.yaml      清单（插件 id 取 name；库文件名取 entry，按 Linux 写法填）
├── example_js/          示例插件（JS 脚本，零编译）：钩子 / 能力 / 自绘页面 / 网络通道 / 设置页
│   ├── plugin.js
│   ├── pluginxx_ui_kit.js  随插件分发的界面 kit（基础）：由 tools/sync_ui_kit.ps1 复制
│   ├── musicxx_ui_kit.js   随插件分发的界面 kit（musicxx 扩展）：由 tools/gen_ui_kit.ps1 生成后复制
│   └── plugin.yaml      清单里 kind: js，scripts 按顺序列出上面三个脚本
└── example_js_shader/   示例插件（JS 脚本）：播放页背景样式 + 动画速率设置 + 页面内联着色器块
    ├── plugin.js
    ├── pluginxx_ui_kit.js / musicxx_ui_kit.js   界面 kit（同上）
    ├── shader/          shader bundle 的源码 + 打包脚本 + 编译产物（JS 插件的资源直接放在插件目录里）
    └── plugin.yaml
```

- **插件 id** 取清单的 `name`（目录名只作提示）；同名视为同一插件（再次安装即升级覆盖）。
- **每个子目录 = 一个插件**，安装到 `<安装前缀>/plugins/<目录名>/`，该目录可直接作为插件目录使用。
- **native 插件**要在 `src/host/CMakeLists.txt` 里注册构建与安装（`add_subdirectory` + `install`）；
  **js 插件**只要有清单 + 脚本（+ 自己的资源），在同一个文件的 JS 插件列表
  （`foreach (_jsPlugin ...)`）里加一项即可被 `install(DIRECTORY ...)` 复制过去。
- **随插件分发的资源**（shader bundle、图标等）放在插件目录里：native 插件用
  `musicxx_plugin_add_target(... ASSETS <目录>)` 让构建助手复制到产物目录旁，js 插件直接放进去即可
  （照抄 `example_js_shader/shader/`）。
- **界面 kit 要随插件一起分发**（宿主不提供 kit）：JS 插件用
  `pwsh tools/sync_ui_kit.ps1` 把基础 kit（库的 `js/pluginxx_ui_kit.js`）与 musicxx 扩展 kit
  （本包 `js/musicxx_ui_kit.js`，由 `pwsh tools/gen_ui_kit.ps1` 生成）复制进插件目录，
  并在清单里写 `scripts: [pluginxx_ui_kit.js, musicxx_ui_kit.js, plugin.js]` —— 宿主会在同一个
  JS 上下文里按这个顺序执行；C++ 插件不用复制，直接用 SDK 头文件里的 `musicxx::ui::kit`。
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
