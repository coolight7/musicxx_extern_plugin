# musicxx_extern_plugin

musicxx 的**外部插件框架**（原生宿主库 + 插件 SDK + 示例插件 + 原生测试）。
设计文档：`<musicxx 仓库>/resource/history/extern-plugin-impl/plan.md`（§3 仓库与包结构、§4 原生宿主、§6 C ABI、§11 构建打包）。

## 目录

```
src/include/     Dart ⇄ 原生 C ABI v1（ffigen 入口，唯一导出契约）
src/host/        宿主实现：宿主线程/事件队列/状态镜像/动作路由/领域表(musicxx.hooks、musicxx.host)/统计
src/sdk/include/ 插件作者 SDK（musicxx/plugin/api/*；插件只依赖头文件）
src/tests/       原生测试（不依赖 Dart）
src/third_party/ 依赖子模块（cxx_pluginxx / cxx_utilxx_base / fmt / yaml-cpp / simdjson / uchardet / quickjs）
example/example_native/ 示例插件（C++，演示钩子/状态镜像/日志）
tools/           build_native.ps1（依赖与宿主构建）、check_submodules.ps1（依赖版本校验）
.native/         本地构建产物与依赖安装前缀（**不入版本库**）
```

## 依赖管理

依赖一律通过 **git submodule** 引入，版本锁定记录在 `third_party_versions.json`（URL + commit）：

```powershell
git submodule update --init --recursive      # 首次拉取
pwsh -NoProfile -File tools/check_submodules.ps1   # 校验 commit 与记录是否一致
```

- 上游推送完成前，`src/third_party/cxx_pluginxx` 的 URL 指向本地路径（通用化改造提交尚未推送）。
  切换方式：`git submodule set-url src/third_party/cxx_pluginxx https://github.com/coolight7/cxx_pluginxx` + `git submodule sync`。
- **Boost** 目前不是 submodule（本机 Boost 不是 git 仓库）：构建脚本支持 `-BoostDir` / `-BoostInclude`
  或环境变量 `BOOST_DIR` / `BOOST_INCLUDE`，未指定时回退到同机既有 Boost 构建（临时措施，见 `third_party_versions.json` 的 `external` 段）。

## 构建

```powershell
# 依赖（fmt → yaml-cpp → cxx_utilxx_base → cxx_pluginxx）装到 .native/<platform>-<arch>，随后构建宿主/示例/测试
pwsh -NoProfile -File tools/build_native.ps1

# 只构建依赖 / 只构建宿主
pwsh -NoProfile -File tools/build_native.ps1 -DepsOnly
pwsh -NoProfile -File tools/build_native.ps1 -HostOnly
```

产物（`.native/build/host/Release/`）：`musicxx_extern_plugin.dll`、`example_native.dll`、`musicxx_extern_plugin_test.exe`。

> MSVC 必须带 `/utf-8`（源码头文件含中文注释，否则按代码页 936 解析会破坏换行）；CMake 已在各目标上设置。

## 原生测试

宿主按「父目录下的每个子目录 = 一个插件」扫描，因此测试参数要传**插件目录的父目录**：

```powershell
# 准备运行时插件目录
mkdir .native/build/host/test-plugins/example_native
copy .native/build/host/example_native/Release/example_native.dll .native/build/host/test-plugins/example_native/
copy example/example_native/plugin.yaml .native/build/host/test-plugins/example_native/

.native/build/host/Release/musicxx_extern_plugin_test.exe .native/build/host/test-plugins
```

## 当前状态

见 musicxx 仓库 `resource/history/extern-plugin-impl/work.md`（阶段 S2：三件产物可构建，原生测试部分用例仍失败，装载链路与 `host_stop` 收尾待修）。
