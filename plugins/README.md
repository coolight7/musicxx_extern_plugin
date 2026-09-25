# 官方插件目录

本目录放 **musicxx 官方实现与示例插件**（第三方插件不需要放在这里，用户插件走应用内的
「从压缩包安装」或直接放入用户插件目录）。

## 目录约定

```text
plugins/
├── example_native/      示例插件（C++ 动态库）：演示钩子/能力/事件/状态镜像/日志
│   ├── CMakeLists.txt   参与宿主工程的构建（src/host/CMakeLists.txt 用 add_subdirectory 引入）
│   ├── example_native.cpp
│   ├── shader/          播放页背景样式用的 shader bundle（源码 + 打包脚本 + 编译产物）
│   └── plugin.yaml      清单（插件 id 取 name；原生库文件名取 entry）
├── example_js/          示例插件（JS 脚本，零编译，无需 CMake）：钩子/能力/自绘页面/网络通道/配置
│   ├── plugin.js
│   └── plugin.yaml      清单里 kind: js
└── example_js_shader/   示例插件（JS 脚本）：自定义播放页面背景 + 动画速率设置
    ├── plugin.js
    ├── shader/          shader bundle 的源码 + 打包脚本 + 编译产物（JS 插件的资源直接放在插件目录里）
    └── plugin.yaml
```

- **插件 id** 取清单的 `name`（目录名只作提示）；同名视为同一插件。
- **每个子目录 = 一个插件**，安装到 `<安装前缀>/plugins/<目录名>/`，该目录可直接作为插件目录使用。
- **native 插件**需要在 `src/host/CMakeLists.txt` 里注册构建与安装（`add_subdirectory` + `install`）；
  **js 插件**只需要清单 + 脚本（+ 自己的资源），在 `src/host/CMakeLists.txt` 的 JS 插件列表
  （`foreach (_jsPlugin ...)`）里加一项即可被 `install(DIRECTORY ...)` 复制过去。
- **随插件分发的资源**（shader bundle、图标等）放在插件目录里：native 插件用
  `musicxx_plugin_add_target(... ASSETS <目录>)` 让构建助手复制到产物目录旁，js 插件直接放进去即可
  （照抄 `example_js_shader/shader/`）。
- 新增插件后跑一次 `pwsh tools/build_native.ps1`，产物会复制到 `.native/output/<平台>-<架构>-<配置>/plugins/`。
- 播放页背景（`musicxx.ui.playing.background`）的完整示例：`example_js_shader`（JS，含速率设置页）
  与 `example_native`（C++）；着色器写法与 uniform 契约见 `docs/plugin-shader-bundle.md`。

## 写作参考

| 主题 | 文档 |
|---|---|
| 钩子总表（id / 模式 / 合并策略 / 载荷） | `docs/plugin-hooks.md`（由 `tools/hooks.def.json` 生成） |
| C++ 插件 SDK 与导出宏 | `src/sdk/include/musicxx/plugin/api/plugin_kit.h` |
| JS 插件作者指南 | `docs/plugin-js-api.md` |
| 插件渲染背景（shader bundle 打包与 uniform 契约） | `docs/plugin-shader-bundle.md` |
| 框架设计（宿主/线程模型/权限/统计） | `<musicxx 仓库>/resource/history/extern-plugin-impl/plan.md` |
