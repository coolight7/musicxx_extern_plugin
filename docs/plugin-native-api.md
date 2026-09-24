# musicxx 动态库插件（++）作者指南（v1）

> 对应方案：`resource/history/extern-plugin-impl/plan.md` 的 §4.5（生命周期纪律）、§7.1（C++ SDK 与构建模板）、§7.4（权限）。
> 相关文档：钩子总表见 `plugin-hooks.md`（由 `tools/hooks.def.json` 生成）、JS 插件作者指南见 `plugin-js-api.md`。
> 参考实现：`plugins/example_native/`（钩子 / 状态镜像 / 动作 / 事件 / UI / 能力 / 日志全演示）。

## 1. 一个动态库插件长什么样

```text
my_plugin/                     # 一个目录 = 一个插件；目录名只作提示，插件 id 取清单 name
├── plugin.yaml                # 清单（必填）
├── my_plugin.so               # 库文件（清单 entry 指向它；Windows 上是 my_plugin.dll、macOS 上是 my_plugin.dylib）
├── icon.png                   # 可选：管理页图标
└── config.json                # 可选：插件配置（插件自己读写；用户也可以直接编辑，路径见 configPath()）
```

`plugin.yaml`：

```yaml
name: my_plugin                  # 唯一 id（宿主与 Dart 侧都用它；与别的插件重名 = 同一插件的升级覆盖）
entry: my_plugin.so              # 库文件名：**按 Linux 写法填**，扩展名由宿主按平台修正（.dll/.dylib）
kind: native                     # native = 动态库；js = 脚本；builtin = 随宿主编译
version: 1.0.0                   # 版本（升级比较用）
api_version: 1                   # 兼容的插件 API 版本（宿主当前 1）
author: "你的名字"
description: "插件说明"
platforms: [windows, linux, macos]   # 允许的平台；arch: [x64, arm64] 可选
min_app_version: 0.87.0          # 可选：最低应用版本

permissions:                     # 声明式权限：只做展示（安装确认页 / 插件详情），运行时不校验
  - musicxx.player.control
  - musicxx.library.read
  - musicxx.ui
  - musicxx.storage
```

插件的设置页由插件**自己画页面**（§4 的「声明式 UI」里给出写法），框架不再提供配置表单：
配置默认值写在插件里（读不到就用默认值），用户改过的值存在插件目录的 `config.json`。

> **`entry` 的写法（跨平台要点）**：统一按 Linux 写 `<名字>.so`，宿主在 Windows/macOS 上会把扩展名
> 修正为 `.dll`/`.dylib`（内核 `pluginxx::resolvePluginEntryPath`）；因此**同一个插件目录可以三平台通用**，
> 只要库文件基名一致（SDK 的构建助手已把 `PREFIX` 置空，产物就是 `my_plugin.dll`/`my_plugin.so`/`my_plugin.dylib`）。
> 若 `entry` 指向的文件不存在，宿主会按平台默认库名再找一次（Linux `lib<name>.so`、Windows `<name>.dll`）。
> 写 `entry: my_plugin.dll` 会让 Linux/macOS 上装载失败（扫描阶段就会提示"动态库插件库文件缺失"）。

安装方式：管理页「从压缩包安装」（把插件目录内容打包成 `.zip`）或直接把目录放进插件目录后「重新扫描」。
随包分发的插件目录应包含**当前平台**的库文件（例如 Windows 包只放 `.dll`）。

## 2. 最小可用插件

```cpp
#include "musicxx/plugin/api/plugin_kit.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace {

/// 实例上下文：**每个实例一份**，不得放可变全局/静态缓存（多实例铁律）
struct MyCtx : public musicxx::plugin::PluginBase {
    int32_t hits = 0;

    /// start 事务：只做注册，禁止阻塞（网络 / 大文件 / 等待别的一方都不行）
    int32_t onStart() {
        // 裁决型钩子：歌曲名含"广告" → 跳过本曲
        hook(MUSICXX_PLUGIN_HOOK_PLAYER_BEFORE_PLAY_SONG, 0,
             [this](std::string_view input, std::string& out) -> int32_t {
                 if (input.find("\xe5\xb9\xbf\xe5\x91\x8a") != std::string_view::npos) {
                     ++hits;
                     out = R"({"action":"skip","reason":"我的插件: 广告曲目"})";
                 }
                 return 0;   // 0 + 空 out = 不裁决，交给下一个处理器
             });

        // 观察型钩子：切歌时读状态镜像并写日志
        observe(MUSICXX_PLUGIN_HOOK_SONG_CHANGED,
                [](std::string_view, std::string&) -> int32_t {
                    const std::string song = stateJson(MUSICXX_STATE_SONG);
                    log.info("切歌了，状态镜像长度=" + std::to_string(song.size()));
                    return 0;
                });

        // 能力：全名必须是 plugin.<自己的插件 id>.<短名>
        capability(*this, "plugin.my_plugin.probe",
                   [this](std::string_view, std::string_view) -> std::string {
                       return "{\"hits\":" + std::to_string(hits) + "}";
                   });
        return 0;
    }

    /// stop 事务：撤销自管资源（线程/定时器/临时文件）；可重复调用
    int32_t onStop() {
        hits = 0;
        return 0;
    }
};

int32_t myStart(MyCtx& ctx) { return ctx.onStart(); }
int32_t myStop(MyCtx& ctx) { return ctx.onStop(); }

} // namespace

MUSICXX_PLUGIN_EXPORT(
    MyCtx,
    "my_plugin",
    "1.0.0",
    "我的第一个插件",
    myStart,
    myStop
)
```

要点：

- `MUSICXX_PLUGIN_EXPORT` 生成宿主要找的 5 个入口符号 `musicxx_plugin_{get_info,create,start,stop,destroy}`；
  **不要手写入口**（少了 `start`/`stop` 会被宿主按契约拒绝装载）；
- 两个事务函数签名是 `int32_t(Ctx&)`：返回 `0` = 成功，非 0 = 失败并回滚。完成通知由 SDK 负责
  （内核要求"返回前恰好一次 done"，手写容易漏，SDK 已包好）；
- 需要更底层的写法（自己做异步 start）可以传内核原始签名
  `void*(Ctx&, const PluginxxOperatorNotify*, PluginxxString*)`，此时由你自己调用 `notify->done`。

## 3. 生命周期与线程

| 阶段 | 谁调用 | 你要做什么 |
|---|---|---|
| `create` | 宿主线程 | 只构造实例上下文（不要注册、不要 IO） |
| `start` | 宿主线程 | **注册事务**：钩子 / 能力 / UI / 订阅；失败返回非 0（宿主回滚，不留残留） |
| `stop` | 宿主线程 | 撤销自管资源（线程、定时器、临时文件、后台任务）；注册由宿主兜底摘除 |
| `destroy` | 宿主线程 | 只释放本地对象（不要在这里做 IO / 通知） |

线程模型（plan §2.3.1，务必理解）：

- 全部动态库插件的生命周期入口与钩子/能力处理器都跑在**同一条宿主线程**（插件管理器线程），
  因此注册表、统计、状态镜像都不需要锁，处理器之间**稳定按注册顺序执行**；
- **处理器里禁止阻塞**：网络请求、大文件、编解码、`sleep` 都不要直接做 ——
  单个插件阻塞会拖慢同一线程上的其它插件（宿主不会强行终止你，但会记录耗时并提示）；
- 慢操作的两种正确做法：
  1. 用内核调度表 `pluginxx.scheduler`（`iface.scheduler->offload(...)` 把阻塞工作交给宿主的
     工作线程池，完成后回到宿主线程；`post_to_io` 回到宿主线程；`sleep` 是异步等待）；
  2. 把工作拆成"提交 + 回调"两段：`start` 只注册，真正的初始化放在异步任务里，完成后再补注册/写状态。
- 插件自管线程是允许的（由插件负责 stop 时回收），但**调用宿主接口一律经接口表**，不要直接触碰宿主结构。

## 4. 能用的宿主能力

| 能力 | API | 说明 |
|---|---|---|
| 裁决钩子 | `hook(id, priority, fn)` | `int32_t(std::string_view input, std::string& out)`；`out` 为 `{"action":...,"patch":{...}}` 或空 = 不裁决 |
| 观察钩子 | `observe(id, fn)` | 返回值忽略；处理器应尽快返回 |
| 注销钩子 | `unregisterHook(id)` | 不调用也会在实例停用/卸载时被宿主摘除 |
| 状态镜像（只读） | `stateJson(MUSICXX_STATE_SONG / _PLAYER / _PLAYLIST / _LYRIC / _LIBRARY / _ENV / _APP)` | 同步读 JSON 快照；镜像里没有临时直链/token |
| 宿主信息 | `hostInfoJson()` | 应用版本 / 平台 / 语言 / 插件目录 |
| 插件配置 | `configPath()`、`config()`、`argsJson()`、`language()` | `config.json` 在插件目录里；插件自己读写（默认可直接编辑该文件），没有宿主生成的表单 |
| 宿主动作（异步） | `requestAction(action, argsJson, &notify, timeoutMs, &requestId)` | 播放/歌单/歌词/界面等，返回结果经 `notify->done` **恰好一次**回调（SDK 会复制通知，传栈上对象也安全） |
| 事件 | `subscribeTopic(topic, fn)`；发布用 `iface.events->publish` | 主题须 `musicxx.*` 或 `plugin.<自己id>.*`；订阅他人 `plugin.*` 主题允许 |
| 声明式 UI | `uiRegister/uiUpdate/uiUnregister/uiEntries/uiNotify` | 类型取 `MUSICXX_PLUGIN_UI_TYPE_*`；渲染由宿主负责 |
| 能力注册 | `capability(*this, "plugin.<id>.<短名>", fn)` | 处理器须**同步返回**可 JSON 序列化的结果 |
| 日志 | `log.info/warn/error`（内核 Logger） | 进插件日志 → 管理页「日志」；也随 `musicxx.plugin.log` 事件回传 |
| 后台任务/取消 | `iface.scheduler`、`iface.tasks`、`cancelRegistry` | 任务随实例停用自动取消 |

跨边界内存铁律：字符串出参必须用宿主的分配器（`PluginBase::hostStringSet` / `hostStringFree`），
**绝不要用 CRT 的 `malloc/free` 或把插件内部的字符串指针交给宿主长期持有**。

**设置页（页面由插件自绘）**：框架不提供设置控件，插件注册一个 `settings.page` 入口指向自己的页面，
页面内容由同名能力返回（`text` / `divider` / `button` / `list` 块，与 JS 插件一致）：

```cpp
// 入口：应用「设置 → 插件设置」与插件详情页会列出它，点击打开 ext://my_plugin/settings
uiRegister("settings", MUSICXX_PLUGIN_UI_TYPE_SETTINGS_PAGE,
           R"({"title":"我的插件设置","subtitle":"页面由插件绘制",
               "action":{"kind":"route","route":"ext://my_plugin/settings"}})", 120);

// 页面：视图 id 与能力短名同名；配置读写自己做（configPath() 指向 config.json）
capability(*this, "plugin.my_plugin.settings",
           [](std::string_view, std::string_view) -> std::string {
               return R"({"view":{"title":"我的插件设置","blocks":[
                   {"kind":"text","text":"这些值保存在插件目录的 config.json 里。"},
                   {"kind":"button","title":"切换","style":"primary",
                    "action":{"kind":"capability","name":"toggleFeature"}}]}})";
           });
```

插件作者文档（生成物）里有每个钩子的 id、模式、合并策略、派发方式与载荷字段，写代码前先查 `plugin-hooks.md`；
注册未知钩子会被拒绝（返回 `-4`）。

## 5. 构建

插件只需要 SDK 头文件 + 一个 CMake 助手（`musicxx_extern_plugin_sdk` 与 `musicxx_plugin_add_target`）：

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_plugin LANGUAGES CXX)

# SDK 前缀 = 宿主构建产物里的安装前缀（含 include/ 与 lib/cmake/）
#   <musicxx_extern_plugin>/.native/build/<平台>-<配置>/musicxx-extern-plugin-install
set(musicxx_extern_plugin_DIR "<安装前缀>/lib/cmake/musicxx_extern_plugin")
find_package(musicxx_extern_plugin CONFIG REQUIRED)

musicxx_plugin_add_target(my_plugin
  SOURCES my_plugin.cpp
  MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
)
```

```powershell
# Windows
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
# 产物：build/my_plugin.dll + build/plugin.yaml —— 这个目录可以直接当插件目录用
```

```bash
# Linux / macOS（Ninja 或 Unix Makefiles 均可）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
# 产物：build/my_plugin.so（macOS 为 my_plugin.dylib）+ build/plugin.yaml
```

宿主库本身不在 Flutter 构建里编译：Windows 用 `pwsh -NoProfile -File tools/build_native.ps1`、
Linux/macOS 用 `./tools/build_native.sh --run-tests`（见包 `README.md` 的「构建」一节）。

`musicxx_plugin_add_target` 负责：

- 建 SHARED 库并链接 SDK（以及存在就链接的内核/工具库，作者不必记库名）；
- C++26 + MSVC `/utf-8` + 符号默认隐藏，并**只导出 `musicxx_plugin_*` 入口**（GNU/Clang 用 version script，Apple 用导出符号表）；
- 多配置生成器下把库文件与 `plugin.yaml` 放在同一层（该目录即插件目录）。

工具链要求：C++26（MSVC ≥ VS 2022 17.14 / GCC ≥ 14 / Clang 与 NDK ≥ 18）。
插件若自己静态链入第三方库，务必保证符号不外泄（默认隐藏已由助手设置）。
同宿主库一样，插件的导出面也由 version script / 导出符号表收口：`nm -D --defined-only`（Linux）
或 `dumpbin /exports`（Windows）应当只看到 5 个 `musicxx_plugin_*` 入口。

## 6. 部署与排障

- 插件目录：用户插件在 `{应用数据目录}/plugins/<id>/`（管理页可「打开插件目录」）；
  随包插件由安装包/构建脚本放进应用数据目录的 `plugins/`；
- 管理页（设置 → 插件 → 外部插件）：启用/禁用、重载、卸载、清理；
  插件详情页展示清单信息、声明的权限、运行统计与日志，并可打开插件目录；
  插件自己的设置页与功能页由插件绘制（`ext://<插件id>/<视图id>`）；
- 宿主日志默认没有输出目标：排障时设置环境变量 `MUSICXX_EXTERN_PLUGIN_LOG_STDERR=1` 让宿主打印到 stderr；
- 插件自己的日志：`log.info(...)` + `console`（JS）/ 管理页「日志」区块；
- 装载失败：管理页会显示原因（清单非法、缺入口符号、api_version 不匹配、平台/架构不符、脚本错误…），
  **宿主不会自动重试**，可以修好后点「重载」；
- 插件崩溃 = 应用崩溃（同进程）。宿主提供"安全模式"兜底：连续两次启动未完成则本次不加载任何外部插件。
  开发期请优先用 `musicxx_extern_plugin_test` 与示例插件做回归，再放进正式环境。

## 7. 权限与命名空间

| 权限 | 含义 |
|---|---|
| `musicxx.storage` | 插件私有 KV 与数据目录 |
| `musicxx.ui` | 注册 UI 项 / 通知 |
| `musicxx.player.control` | 播放控制动作 |
| `musicxx.library.read` / `musicxx.library.write` | 读 / 写歌单、歌曲、歌词、历史 |
| `musicxx.net` | 允许使用宿主网络代理通道（`musicxx.net.fetch`；宿主不限定域名） |
| `musicxx.fs.read:<路径>` / `musicxx.fs.write:<路径>` | 直接文件访问（路径限定） |
| `musicxx.system` | 打开链接 / 剪贴板 / 托盘等外围能力 |
| `musicxx.agent` | 预留（AI 方案落地后启用） |

- 权限是**声明**：插件清单里写出来，宿主要在安装确认弹窗与插件详情页里展示（让用户知情）；
  **不做运行时校验** —— 动作不会因为没授权而被拒绝，直接在插件目录里放插件也不检查权限；
- 插件升级后新增的权限同样只展示，不会暂停启用；
- 命名空间：官方标识一律 `musicxx.*`（钩子 / 事件主题 / 动作 / UI 类型 / 权限）；
  插件自定义一律 `plugin.<自己的插件 id>.*`（事件主题、能力名、UI 项 id）。冒充他人命名空间会被拒绝。

## 8. v1 已知边界

| 边界 | 说明 |
|---|---|
| 领域接口表 | 已实现 `musicxx.hooks` / `musicxx.host` / `musicxx.ui`；`musicxx.player` / `library` / `lyrics` / `storage` / `net` / `stats` 的 IID 已冻结但**表体未实现**（查询返回 NULL）→ 这些能力统一走 `requestAction("musicxx.player.play", ...)` 等动作名，由 Dart 侧分派（动作不做权限校验） |
| 进程隔离 | 插件与宿主同进程（无沙箱）；权限只是"防误用与用户知情" |
| 平台 | Windows / Linux / macOS / Android 可加载动态库插件（Android 默认启用，加载失败会安全降级并提示）；iOS / OHOS 只能跑 JS 插件 |
| 资源限制 | 宿主不限制插件的内存/耗时/网络，只做自我保护（等待预算 100 ms、动作超时、事件队列上限、连续失败熔断）；统计只观测不限制 |
| 多实例 | 同一份库文件可以被宿主创建多个实例（不同 id/参数），因此**不得有可变全局状态** |

后续计划（不在 v1）：领域表体、插件独立进程、插件市场、Rust/WASM 插件形态 —— 见 plan §15.4。
