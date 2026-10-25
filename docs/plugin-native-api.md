# musicxx 动态库插件作者指南（C++ / v1）

本文写给**要写一个动态库插件**的人：目录与清单、代码示例、开发流程、能用的框架与主程序能力、
构建与部署、排障。相关文档：

| 主题 | 文档 |
|---|---|
| **两种形态共用的接口**（状态镜像 / 动作 / 事件 / 变量 / 能力 / 功能点 / 钩子写法 / 约定 / 排障） | [plugin-api.md](plugin-api.md) |
| 总入口：选形态 / 环境准备 / 第一个插件 / 注意事项 / 发布检查 | [plugin-guide.md](plugin-guide.md) |
| 钩子 id / 模式 / 派发，以及已接入钩子的载荷与裁决语义 | [plugin-hooks.md](plugin-hooks.md) |
| 界面（UI 项、插件页面、设置页）的字段与块类型 | [plugin-ui.md](plugin-ui.md) |
| 播放页背景（shader bundle 打包与 uniform 约定） | [plugin-shader-bundle.md](plugin-shader-bundle.md) |
| 装插件、刷新、看日志（命令行 / MCP，给 AI agent 与自动化） | [plugin-agent-cli.md](plugin-agent-cli.md) |
| JS 插件（零编译） | [plugin-js-api.md](plugin-js-api.md) |
| 示例代码 | `plugins/example_native/`（C++）、`plugins/example_js/`（等价 JS） |

> **本文只写 C++ 特有的东西**：清单与多目标打包、SDK、框架线程、接口表判定、构建部署、排障。
> **每个接口"做什么、参数是什么、返回什么、什么时候用、有什么坑"在 [plugin-api.md](plugin-api.md)** ——
> 那份文档 JS 与动态库插件共用（两边的接口是同一批：同一套钩子表、同一份动作注册表、同一份状态镜像）。
> 同一个功能在 C++ 里怎么写，§4.2 有一份对照示例。

> 动态库与主程序**同进程、同地址空间**：插件崩溃 = 主程序崩溃。开发期请先在
> `musicxx_extern_plugin_test` 与示例插件上验证，再放进正式环境；插件框架的安全模式
> （连续两次启动未完成 → 本次不加载任何外部插件）是最后一道保护，不是沙箱。

---

> **术语约定**：本文档中的**主程序**指的就是 **musicxx**（Flutter/Dart 业务层）；外置插件的 C++ 框架层称为**插件框架**；插件目前分为**动态库插件**与**JS插件**。


## 1. 插件长什么样

一个插件 = 一个目录：

```text
my_plugin/                   # 目录名只是提示，插件 id 取清单的 name
├── plugin.yaml              # 清单（必填）
├── my_plugin.so             # 库文件（清单 entry 指向它；Windows 是 .dll、macOS 是 .dylib）
├── shader/                  # 可选：随插件分发的资源（如 shader bundle）
├── resource/svg/logo.svg    # 可选：插件自己的图标（icon 写相对插件目录的路径，见 plugin-ui.md §1.5）
└── config.json              # 可选：插件自己的配置（插件读写，用户也可以直接编辑）
```

一个包也可以同时支持**多个系统 / CPU 架构**（仿 APK 的 `lib/<abi>/`），库文件按分支摆放、
清单只有一份 —— 运行时由插件框架按当前系统与架构选分支，见 §1.3：

```text
my_plugin/
├── plugin.yaml              # 清单只有一份（分支共用；entry 仍然是"分支里的库文件名"）
├── lib/                     # 分支目录（缺省 lib；清单 targets_dir 可改名）
│   ├── windows-x64/my_plugin.dll
│   ├── linux-x64/my_plugin.so
│   ├── linux-arm64/my_plugin.so
│   ├── android-arm64-v8a/my_plugin.so
│   └── universal/my_plugin.so        # 通用分支：任何系统/架构都能用
├── shader/                  # 与分支无关的共享资源（放在包目录里，不在分支目录里）
└── config.json
```

### 1.1 清单字段

```yaml
name: my_plugin                    # 插件 id（唯一；同名视为同一插件的升级覆盖）
entry: my_plugin.so                # 库文件名（按 Linux 写法填，见下方说明）
kind: native                       # native = 动态库；js = 脚本插件；缺省按 entry 推导
version: 2026.1010.1              # 版本号 = `<年>.<月日>.<小版本号>`（例中为 2026-10-10 的第 1 版）；打包脚本用它拼安装包名
api_version: 1                     # 插件 API 版本：不低于插件框架支持的最低版本即可（当前最低 1）；声明更高也不拒绝
author: "你的名字"
description: "插件说明"
platforms: [windows, linux, macos] # 允许加载的平台；不写 = 不限（别名 win32 / osx 也认；多目标包通常不写，见 §1.3）
arch: [x64, arm64]                 # 允许的架构；不写 = 不限（别名 amd64 / aarch64 也认）
targets_dir: lib                   # 多目标包的分支目录名（缺省 lib；空串 = 关闭分支扫描）
depends: [other_plugin]            # 必选依赖（插件框架会先加载它们）
optional_depends: [maybe_plugin]   # 可选依赖（有就排前面，没有也照常加载）

permissions:                       # 声明式权限：只做展示，运行时不校验（见 §8）
  - musicxx.player.control
  - musicxx.ui
  - musicxx.storage
```

`entry` 的写法（跨平台要点）：

- **统一按 Linux 写 `<名字>.so`**：插件框架在 Windows/macOS 上会把扩展名修正为 `.dll`/`.dylib`
  （插件目录因此可以三平台通用，只要库文件基名一致；SDK 的构建助手已把产物前缀置空）；
- `entry` 指向的文件不存在时，插件框架还会按平台默认库名再找一次
  （Linux `lib<name>.so`、Windows `<name>.dll`）；
- 写 `entry: my_plugin.dll` 会让 Linux/macOS 加载失败（扫描阶段就会提示「动态库插件库文件缺失」）。

清单里**没有**运行时行为相关的开关：配置由插件自己读写 `config.json`，插件框架不解析插件配置。

插件框架**不校验** `min_app_version`：Dart 侧清单解析会读这个字段，但当前版本没有用它做拦截，
要表达版本要求请用 `api_version`：只要不低于插件框架支持的最低版本（`MUSICXX_PLUGINXX_MIN_API_VERSION`，当前 1）就照常加载，声明更高也不会被拒。

### 1.2 安装位置

- 用户插件：`<应用支持目录>/musicxx/extern_plugin/plugins/<id>/`（管理页有「打开插件目录」）；
- 随包插件：安装包放进主程序的随包插件目录（插件框架启动时把两个目录都扫描出来）；
- 安装方式：管理页「从压缩包安装」（`.zip`，顶层就是插件目录的内容），或直接把目录放进插件目录后
  「重新扫描」。

### 1.3 多目标打包（一个包放多个系统 / 架构分支）

仿 APK 的原生库目录：**一个包 = 一份清单 + 若干分支目录**，每个分支目录里放一份该目标的库文件；
插件框架在扫描与加载时按当前系统与 CPU 架构选一个分支，管理页的插件信息里会写出"已选分支 / 包内分支"。

```text
my_plugin/
├── plugin.yaml                        # 清单只有一份（entry = 分支里的库文件名）
└── lib/                               # 分支目录（清单 targets_dir 可改名，缺省 lib）
    ├── windows-x64/my_plugin.dll      # 系统 + 架构
    ├── linux-arm64/my_plugin.so
    ├── android-arm64-v8a/my_plugin.so
    ├── windows/my_plugin.dll          # 只限系统（架构不限）
    ├── x64/my_plugin.so               # 只限架构（系统不限）
    └── universal/my_plugin.so         # 通用分支（任何系统 / 架构都能用）
```

**选择顺序**（从具体到通用，先命中先用；每个分支目录名就是一个"标签"）：

1. 系统 + 架构都匹配（`windows-x64`）
2. 只声明系统（`windows`）
3. 只声明架构（`x64`）
4. 通用分支（`universal` / `any` / `noarch` …）
5. 插件根目录里的库文件（旧布局，也是隐式通用分支）

标签的写法很宽松（大小写、`-`/`_`/`.` 分隔符都能认，顺序也可以反过来）：

- 系统名：`windows` / `win` / `win32` · `linux` · `macos` / `mac` / `osx` / `darwin` ·
  `android` · `ios` / `iphoneos` · `ohos` / `harmonyos`；
- 架构名：`x64` / `amd64` / `x86_64` · `x86` / `i386` / `ia32` · `arm64` / `aarch64` /
  `arm64-v8a` · `armv7` / `armeabi-v7a` · `riscv64` · `loongarch64`。

**分支里找库文件的顺序**：清单 `entry`（按平台修正扩展名，Windows `.dll` / macOS `.dylib`）→
平台默认库名（`lib<name>.so` / `<name>.dll` / `lib<name>.dylib`）→ 分支目录里**唯一**的
`*.dll`/`*.so`/`*.dylib`（所以各分支的文件名不必完全一致，但有两个以上就必须靠 `entry` 指明）。

规则要点：

- **插件框架的架构标识**：`x64` / `x86` / `arm64` / `armv7` / `riscv64` / `loongarch64`。
  **32 位 ARM 报 `armv7`**（Android `armeabi-v7a`、Linux armhf），所以 `lib/android-armeabi-v7a/`
  这类分支目录与清单 `arch: [armv7]`（或别名 `arm` / `arm32` / `armeabi-v7a`）能对上；
  插件框架上报给插件的 `host.info().arch` 也是这个名字；
- **不匹配当前系统/架构的分支不会被加载**：扫描结果里 `supported=false`，原因是"包内没有匹配当前
  系统/架构的分支"（附带包内分支清单），主程序安装时也会提前拒绝并说明；
- **判定只有插件框架一处实现**：扫描（`plugin_scan`）、加载与安装预检（C ABI `plugin_inspect`）跑的是
  同一份 `inspectPluginDir`，所以"预检说能不能用、加载就用哪个分支"不会出现分歧；
  Dart 侧只有数据模型与环境取值，不复制别名表与目录遍历；
- 清单 `platforms` / `arch` 仍然有效，含义是"**整包**允许的平台/架构"（例如插件内含脚本或数据资源
  只对某些系统有意义时的额外限制）；多目标包通常不用写它们，支持范围已经由分支表达。
  取值与分支标签**共用同一张别名表**（`win32` / `osx` / `gnu` / `amd64` / `aarch64` 都认），
  比较的是"当前环境与清单声明"，不写 = 不限；
- **不认识的分支目录会被忽略**（例如 `lib/utils/`、`lib/shader/`），可以放心把辅助文件放在同一层；
- `targets_dir` 显式写成空串 = 关闭分支扫描（插件自己管目录布局，插件框架只看清单 `entry`）；
- 分支目录里放的是"目标平台编出来的库"这一事实由你保证：标签与真实库不匹配时，加载阶段
  `dlopen`/`LoadLibrary` 会失败（错误信息里带库路径）。

**构建一个多目标包**：SDK 的构建助手按当前构建目标把库放进分支目录 ——

```cmake
musicxx_plugin_add_target(my_plugin
  SOURCES my_plugin.cpp
  MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
  TARGET_TAG auto            # 生成 lib/<系统>-<架构>/（Android 用 ABI 名：android-arm64-v8a）
  # PACKAGE_DIR <目录>       # 可选：包目录（缺省 <当前构建目录>/package）
)
```

每个平台/架构各构建一次（换机器或在有 NDK 的机器上 `-DANDROID_ABI=...` 交叉编译），
把各次产物里的 `lib/<标签>/` 目录**合并进同一个包目录**，就得到一个通用包；打包用
`pwsh tools/pack_plugin.ps1 -PluginDir <包目录>`（压缩包顶层就是插件目录内容，可直接用
应用里的「从压缩包安装」），它会打印包内的分支清单，便于确认每个分支都有库文件。
助手同时会注入编译宏 `MUSICXX_PLUGIN_BUILD_TAG="<标签>"`，插件可以把它写进日志或自检信息，
便于确认插件框架加载的是哪一份构建（示例见 `plugins/example_native_multi/`）。

不传 `TARGET_TAG` 时行为与以前一致：库文件与清单放在同一层，该目录直接作为插件目录。

---

## 2. 最小可用插件

```cpp
#include "musicxx/plugin/api/plugin_kit.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace {

/// 实例上下文：**每个实例一份**，不要放可变全局/静态状态（同一份库可以被创建多个实例）
struct MyCtx : public musicxx::plugin::PluginBase {
    int32_t hits = 0;

    /// start 事务：只做注册，禁止阻塞（网络 / 大文件 / 等别人都不行）
    int32_t onStart() {
        // 裁决型钩子：载荷里出现「广告」就跳过本曲
        hook(MUSICXX_PLUGIN_HOOK_PLAYER_BEFORE_PLAY_SONG, 0,
             [this](std::string_view input, std::string& out) -> int32_t {
                 if (input.find("\xe5\xb9\xbf\xe5\x91\x8a") != std::string_view::npos) {
                     ++hits;
                     out = R"({"action":"skip","error":"my_plugin: 广告曲目"})";
                 }
                 return 0;   // 返回 0 且 out 为空 = 不裁决，交给下一个处理器
             });

        // 观察型钩子：切歌时同步读状态镜像并写日志
        observe(MUSICXX_PLUGIN_HOOK_SONG_CHANGED,
                [](std::string_view, std::string&) -> int32_t {
                    const std::string song = stateJson(MUSICXX_STATE_SONG);
                    log.info("切歌了，歌曲快照长度=" + std::to_string(song.size()));
                    return 0;
                });

        // 能力：全名必须是 plugin.<自己的插件 id>.<短名>
        capability(*this, "plugin.my_plugin.probe",
                   [this](std::string_view, std::string_view) -> std::string {
                       return "{\"hits\":" + std::to_string(hits) + "}";
                   });
        return 0;
    }

    /// stop 事务：撤销自管资源（线程 / 定时器 / 临时文件）；可能被调用多次
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
    "2026.1010.1",              // 插件版本号：`<年>.<月日>.<小版本号>`（见 §1.1）
    "我的第一个插件",
    myStart,
    myStop
)
```

要点：

- `MUSICXX_PLUGIN_EXPORT` 生成插件框架要找的 5 个入口符号
  （`musicxx_plugin_{get_info,create,start,stop,destroy}`）。**不要手写入口**：少了 `start`/`stop`
  会被插件框架按约定拒绝加载（管理页会给出「缺失/无效入口符号」）；
- 两个事务函数的签名是 `int32_t(Ctx&)`：返回 `0` = 成功，非 0 = 失败并回滚。完成通知由 SDK 负责
  （插件框架要求「返回前恰好一次 done」，手写容易漏）。需要自己做异步 start 时，改用插件框架原始签名
  `void*(Ctx&, const PluginxxOperatorNotify*, PluginxxString*)`，此时由你自己调用 `notify->done`；
- `hook` / `observe` / `capability` / `stateJson` 这些便捷方法都在
  `musicxx::plugin::PluginBase`（头文件 `musicxx/plugin/api/plugin_kit.h`）上；
- 注册失败会返回负错误码（见 §7 错误码），**不要忽略返回值**：未知钩子、命名空间非法、
  数据不合法都会在注册时直接失败。

---

## 3. 生命周期与线程

| 阶段 | 谁调用 | 你要做什么 |
|---|---|---|
| `create` | 框架线程 | 只构造实例上下文（不注册、不做 IO） |
| `start` | 框架线程 | **注册事务**：钩子 / 能力 / UI 项 / 订阅；失败返回非 0（框架回滚，不留残留） |
| 启用中 | 框架线程 | 处理器与能力被调用；可以做动作请求、事件发布、写自己的状态 |
| `stop` | 框架线程 | 撤销自管资源（线程、定时器、后台任务、临时文件）；注册由框架统一摘除 |
| `destroy` | 框架线程 | 只释放本地对象（不要在这里做 IO / 通知） |

- 全部动态库插件代码都在**同一条框架线程**（插件管理器线程）上顺序执行：注册表、统计、状态镜像
  都不需要锁，**处理器之间按 (priority, 注册顺序) 稳定执行**；
- **处理器里禁止阻塞**：网络请求、大文件、编解码、`sleep` 都不要直接做 —— 一个插件卡住会拖慢
  同一线程上的所有插件（插件框架不会强杀你，只会记录耗时并提示）。耗时工作的正确做法：
  1. 用插件框架的调度原语把阻塞工作交给框架的线程池，完成后回到框架线程
     （`pluginxx::offload(ctx, work)` 与协程 `Task<T>`；`pluginxx::sleep(ctx, ms)` 是异步等待，
     不占用线程）；
  2. 把工作拆成「提交 + 回调」两段：`start` 只注册，真正的初始化放进异步任务，完成后再补注册 / 写状态；
- 插件**可以**自己起线程或定时器，但要在 `stop` 里回收，并且**调用框架接口一律经接口表**
  （`iface.*` / `PluginBase` 的便捷方法），不要直接触碰框架内部结构；
- 状态镜像的读取是**同步**的（`stateJson(...)`），动作请求是**异步**的（结果经回调）。

---

## 4. 能用的框架与主程序能力

| 能力 | API | 说明 |
|---|---|---|
| 裁决钩子 | `hook(id, priority, fn)` | `fn = int32_t(std::string_view input, std::string& out)`；`out` 写 `{"action":...,"patch":{...}}`，留空 = 不裁决 |
| 观察钩子 | `observe(id, fn)` | 返回值忽略；处理器要尽快返回 |
| 注销钩子 | `unregisterHook(id)` | 只注销默认 `owner_tag`；不调用也会在停用/卸载时被框架摘除 |
| 状态镜像（同步只读） | `stateJson("musicxx.state.song")` 等 | 见 §5 |
| 框架版本 | `hostInfoJson()` | `{appVersion, platform, language, dataDir, userPluginDir, builtinPluginDir, apiVersion, hostVersion, pluginId, ui?}`：`hostVersion` 是插件框架自己的版本（如 `2026.1011.1`），`pluginId` 是本插件 id（只有插件读时才带），`ui` 是客户端界面能力段（可选）；字段说明见 [plugin-api.md](plugin-api.md) §2.5 |
| 运行环境 | `config()`、`language()`、`argsJson()` | `config()` 是运行环境 JSON（`appVersion/platform/arch/language/dataDir/logDir/pluginDirs/apiVersion`）；`argsJson()` 是加载时框架给的参数（默认 `{}`） |
| 插件配置路径 | `configPath()` | 插件目录下的 `config.json`；插件自己读写（见 [plugin-ui.md](plugin-ui.md) §3） |
| 主程序动作（异步） | `requestAction(action, argsJson, &notify, timeoutMs, &requestId)` | `notify->done` **恰好一次**（SDK 会复制通知，传栈上对象也安全）；动作名与参数见 [plugin-api.md](plugin-api.md) §3（播放控制、歌单、歌词、提示与页面跳转、存储、网络、渲染槽位、封面与频谱、字体、自省…全在那一份）；**缺省 5000 ms，传 0 = 不设超时** |
| 自省（异步） | `requestAction("musicxx.hook.handlers", "{\"hook\":\"<钩子 id>\"}", ...)`、`requestAction("musicxx.feature.point.list", ...)` | 前者答"这个钩子现在有谁在提供、谁先执行"（`handlers`，按调用顺序）；后者答"应用留出了哪些扩展点、现在谁在提供"（功能埋点清单）。要"先看清有没有人接这个钩子再决定要不要登记"就用它们，别猜 |
| 事件 | `subscribeTopic(topic, fn)`；发布用 `iface.events->publish` | 主题须 `musicxx.*` 或 `plugin.<自己 id>.*`；订阅别人的 `plugin.*` 主题是允许的 |
| 声明式 UI | `uiRegister/uiUpdate/uiUnregister/uiEntries/uiNotify` | 类型取 `MUSICXX_PLUGIN_UI_TYPE_*`，渲染由主程序负责（见 [plugin-ui.md](plugin-ui.md)） |
| 播放页背景 | `uiRegister(..., MUSICXX_PLUGIN_UI_TYPE_PLAYING_BACKGROUND, ...)` | 需要打包期编译好的 shader bundle（见 [plugin-shader-bundle.md](plugin-shader-bundle.md)） |
| 能力注册 | `capability(*this, "plugin.<id>.<短名>", fn)` | 处理器可以同步返回，也可以返回 `pluginxx::Task<T>`（异步完成，不占框架线程）；**不要写阻塞处理器**，长任务用 `co_await pluginxx::offload(*this, work)` 或 `pluginxx::sleep` |
| 跨插件调用 | `co_await capabilityCall(能力全名, method, argsJson)` / `capabilityCall(..., &notify)` | 协程形态直连插件框架（不经应用），回调形态走 `invoke_capability_async`；**没有同步版**（插件框架会拒绝在 IO 线程阻塞等待） |
| 功能点（声明 / 实现 / 调用） | `featurePointDefine` / `featurePointProvide` / `featurePointUnprovide` / `featurePointCall` / `featurePointList` | 见 §6.5：声明自己的功能点、给出实现、或调应用留出的功能点（频谱 / 副歌 / 封面取色） |
| 日志 | `log.trace/debug/info/warn/error` | 进插件日志流 → 管理页「日志」；也随 `musicxx.plugin.log` 事件回传 |
| 后台任务 / 取消 | `iface.scheduler`（`offload` / `sleep` / `post_to_io` / `op_cancel`）、`iface.tasks`、`cancelRegistry` | 任务随实例停用自动取消 |

跨边界内存规则：出参字符串必须用插件框架的分配器（`PluginBase::hostStringSet` / `hostStringFree`，
或用插件框架的 `pluginxx::PluginString` RAII）。**绝不要用 CRT 的 `malloc/free` 把指针交给插件框架长期持有。**

### 4.1 接口表怎么判（版本 / 基线 / 追加成员）

`iface.*` 的每一张表都是自述「版本 + 字节长度」的结构体，判定只有两条：
`version >= 该表的最低版本`（`MUSICXX_PLUGIN_IFACE_*_MIN_VERSION`）且
`struct_size >= 该表的基线字节数`（`..._BASELINE_SIZE`）。SDK 的 `pluginxx::queryInterface<表>(host, IID)`
已经按它做完：拿到非空指针 = 表可用，**基线段成员全部可用**；不满足门槛时整张表返回 NULL
（`iface.hooks` / `iface.host` / `iface.ui` / `iface.vars` 为 `nullptr`），按"这个能力没有"降级即可。

- **表能查到 ≠ 表里每个成员都能用**：以后往表尾追加的成员要单独判
  `pluginxx::ifaceHas<&MusicxxPluginUIIface::新成员>(iface.ui) && iface.ui->新成员`。
  只判空指针会读到紧邻的另一块内存（老主程序给的表比插件期望的短）。
- 每张表的 `MUSICXX_PLUGIN_IFACE_*_VERSION` / `..._MIN_VERSION` / `..._BASELINE_SIZE` 与表体一起声明在
  `plugin_api.h`，SDK 在 `plugin_kit.h` 里逐表给出 `pluginxx::IfaceMeta` 特化（上面两条门槛读的就是它）。
  插件不要自己造表、也不要改主程序给的表。
- 现在的 `musicxx.hooks` / `musicxx.host` / `musicxx.ui` / `musicxx.vars` / `musicxx.net` 五张表都是 v1，
  基线 = 整表大小，也就是"表可用即全部成员可用"。

### 4.2 常用接口的 C++ 写法

接口的语义（做什么、参数、返回、坑）在 [plugin-api.md](plugin-api.md)，这里是把它们写出来长什么样。
**异步的东西都是"发起 + 完成通知"两段式**，先准备一个通知：

```cpp
/// 一次异步调用的完成回调（**恰好一次**；框架线程调用，不要在里面做重活）
static void onDone(void* ud, int32_t status, const PluginxxStringView* json) {
    // status == 0：json 是结果 JSON；否则 json（可能为空）里通常有 {"ok":false,"error":"..."}
}
```

**状态镜像 / 框架信息**（同步读，见 [plugin-api.md](plugin-api.md) §2）：

```cpp
const std::string songJson = stateJson(MUSICXX_STATE_SONG);   // 键没推过 = 空串
const std::string envJson  = stateJson(MUSICXX_STATE_ENV);
const std::string infoJson = hostInfoJson();                  // 版本 / 平台 / 目录 / 本插件 id
const std::string cfgPath  = configPath();                    // 本插件 config.json 的路径
```

**动作**（播放 / 歌单 / 提示 / 页面 / 存储 / 网络 / 渲染 / 封面与频谱；清单与参数见
[plugin-api.md](plugin-api.md) §3）：

```cpp
PluginxxOperatorNotify notify{&onDone, this};
requestAction("musicxx.player.toggle", "{}", &notify, 5000);
requestAction("musicxx.player.seek", R"({"positionMs":64000})", &notify, 5000);
requestAction("musicxx.library.querySonglists", "{}", &notify, 5000);
requestAction("musicxx.ui.notify", R"({"text":"已刷新"})", &notify, 5000);
requestAction("musicxx.ui.openRoute", R"({"route":"ext://my_plugin/card"})", &notify, 5000);
// 浮层：等用户操作，超时传 0（不设上限）；结果里 reason 是 confirmed / cancelled / closed
requestAction("musicxx.ui.overlay.show",
              R"({"title":"运行详情","blocks":[],"textConfirm":"知道了"})", &notify, 0);
requestAction("musicxx.ui.overlay.close", R"({})", &notify, 0);
requestAction("musicxx.storage.set", R"({"key":"greeting","value":"早上好"})", &notify, 5000);
requestAction("musicxx.net.fetch", R"({"url":"https://example.com/a.json"})", &notify, 30000);
requestAction("musicxx.render.list", "{}", &notify, 5000);
requestAction("musicxx.media.spectrum", R"({"bandCount":16})", &notify, 5000);
```

- 需要更长时间（等用户点弹窗、解码整曲）就把 `timeoutMs` 给大，或传 `0` = 不设超时；
- 动作**不做权限校验**，名字也不校验：写错名字会在完成通知里拿到 `action_not_registered`。

**事件**（主题与载荷见 [plugin-api.md](plugin-api.md) §4）：

```cpp
subscribeTopic("musicxx.state.changed", [](std::string_view json) {
    // {"key":"musicxx.state.song","value":{...}}
});
subscribeTopic("musicxx.var.changed", [](std::string_view json) { /* ... */ });

// 发布：只能发官方主题或自己命名空间 plugin.<自己 id>.*
PluginxxStringView topic = pluginxxView("plugin.my_plugin.hello");
PluginxxStringView data  = pluginxxView(R"({"at":1})");
iface.events->publish(host, &topic, &data);
```

**变量**（语义与错误码见 [plugin-api.md](plugin-api.md) §5）：

```cpp
std::string cached = varsPeek("musicxx.ui.songIconWave");           // 同步读缓存（可能旧）
PluginxxOperatorNotify n{&onDone, this};
varsGet("musicxx.ui.animatedLevel", &n);                            // 异步权威读
varsSubscribe(R"(["musicxx.ui.animatedLevel"])");                   // 订阅（事件 musicxx.var.changed）
std::string out = varsSet("musicxx.ui.particleAnimate", "false");   // 写：给了结果就结算，否则 {"pending":true,…}
```

**能力 / 功能点 / UI / 日志**：

```cpp
capability(*this, "plugin.my_plugin.probe",
    [](std::string_view method, std::string_view args) -> std::string { return R"({"ok":true})"; });

pluginxx::Task<void> askOther() {                       // 调别的插件：协程形态
    auto r = co_await capabilityCall("plugin.other_plugin.probe", "probe", "{}");
    co_return;
}

uiRegister("card", MUSICXX_PLUGIN_UI_TYPE_HOME_ENTRY, entryJson, 120);
uiNotify(R"({"text":"已刷新"})");

featurePointProvide("plugin.my_plugin.beat", [](std::string_view argsJson) -> std::string {
    return R"({"bpm":128,"beats":[0,469,938]})";
});
PluginxxOperatorNotify fc{&onDone, this};
featurePointCall("musicxx.media.palette.provide", R"({"path":"D:/covers/a.jpg"})", 60000, &fc);

log.info("切歌了: " + songJson);
```

**自省**（"这个钩子现在有谁在提供" / "应用留出了哪些扩展点"）：

```cpp
PluginxxOperatorNotify n2{&onInspectDone, this};
requestAction("musicxx.hook.handlers", R"({"hook":"musicxx.media.spectrum.extract"})", &n2, 5000);
requestAction("musicxx.feature.point.list", "{}", &n2, 5000);
```

- 想"先看清有没有人接这个钩子再决定要不要注册"就用它们，别猜；
- 动态库插件**没有**"读自己的运行统计"的便捷接口（`musicxx.stats` 表未实现，见 §13）：
  看统计用管理页「插件详情 → 运行统计」，上报统计用动作 `musicxx.stats.reportMemory` /
  `reportMetric`。

---

## 5. 状态镜像（插件读主程序状态的最快方式）

插件框架把应用的关键状态做成 JSON 快照，插件**同步**读（不往返、不占线程）：

```cpp
const std::string songJson = stateJson(MUSICXX_STATE_SONG);   // 键没推过 = 空串
```

- **键、每个键的字段、刷新时机与坑**：[plugin-api.md](plugin-api.md) §2（状态镜像那一节）。
  可读的键：`musicxx.state.app` / `player` / `song` / `playlist` / `env` / `renderSlots` / `spectrum`；
  `musicxx.state.lyric` / `musicxx.state.library` 是预留的，当前版本没有推送（返回空串）；
- C++ 常量：`MUSICXX_STATE_APP` / `_PLAYER` / `_SONG` / `_PLAYLIST` / `_LYRIC` / `_LIBRARY` / `_ENV` /
  `_RENDER_SLOTS`；
- 值必须是合法 JSON（大小不限制，早期版本的 512 KiB 上限已移除）；
- 每个键更新都会推 `musicxx.state.changed` 事件（载荷 `{key, value}`），可以订阅它做增量处理：

  ```cpp
  subscribeTopic("musicxx.state.changed", [](std::string_view json) {
      // {"key":"musicxx.state.song","value":{...}}
  });
  ```

- **播放进度要自己拿**：`musicxx.state.player.position` 只在启动、切歌与播放状态变化时刷新
  （**不是实时值**）。要按秒推进度用钩子 `musicxx.player.position`（每秒一条，
  带 `sid` / `positionMs` / `durationMs`），要当场读当前值用变量 `musicxx.player.position`；
- 加载时想立刻确认框架给的环境：`config()`（`appVersion/platform/arch/language/dataDir/logDir/pluginDirs/apiVersion`）
  与 `argsJson()`（默认 `{}`）；
- 接口表里还有一个 `subscribe_state`（声明关心的状态键），当前版本插件侧用事件订阅就够了。

---

## 5.5 变量（有属主、可读、可写、可订阅变动的小值）

> 概念、与镜像/动作/事件的分工、三条读法的取舍、handler 模式、官方变量目录、错误码与超时：
> [plugin-api.md](plugin-api.md) §5 与 [plugin-vars.md](plugin-vars.md)。本节是 C++ 侧的写法。

接口表 `musicxx.vars`（`MusicxxPluginVarsIface`，SDK 里是 `iface.vars`）。
它和状态镜像的区别是：**能双向读写，而且能订阅"值变了"**。
真值在属主那里（主程序的配置，或你自己插件里的状态），插件框架只保留一份服务同步读的缓存。

```cpp
// 声明式（declared）：值由框架代存，set 即提交 —— 零代码
MusicxxPluginVarSpec spec{};
spec.version = 1;
spec.struct_size = sizeof(spec);
spec.key        = pluginxxView("stats.skipCount");          // 短名会自动补前缀
spec.caps       = MUSICXX_PLUGIN_VAR_CAP_GET | MUSICXX_PLUGIN_VAR_CAP_SET | MUSICXX_PLUGIN_VAR_CAP_NOTIFY;
spec.write_scope = MUSICXX_PLUGIN_VAR_WRITE_ANY;             // 或 ..._OWNER
spec.mode        = MUSICXX_PLUGIN_VAR_MODE_DECLARED;
spec.type        = pluginxxView("number");
spec.value_json  = pluginxxView("0");                        // 缓存初值（不是真值来源）
spec.meta_json   = pluginxxView(R"({"title":"跳过次数"})");
iface.vars->register_var(host, &spec);
// SDK 便利包装: varsRegister(spec) / varsUnregister("stats.skipCount")

// 异步读（官方键由应用回答；结果经 notify->done 回来，不要就地等待）
PluginxxOperatorNotify notify{};
notify.done = [](void* ud, int32_t status, const PluginxxStringView* json) {
  // status == 0 时 json 是值
};
int64_t requestId = 0;
iface.vars->get(host, &pluginxxView("musicxx.ui.animatedLevel"), &notify, &requestId);

// 同步读缓存（热路径用；不保证最新，返回里带 revision/ageMs/stale）
PluginxxString cached{};
iface.vars->peek(host, &pluginxxView("musicxx.ui.songIconWave"), &cached);
hostStringFree(cached);   // 插件侧: host->vtable->free

// 写官方键（由应用写入）：立即结算时 out_json 是结果，转给属主时是
// {"pending":true,"requestId":N}，最终结果经事件 `musicxx.var.writeResult` 送达
PluginxxString out{};
iface.vars->set(host, &pluginxxView("musicxx.ui.particleAnimate"), &pluginxxView("false"), &out);

// 订阅变动：先登记关心，再订阅事件主题 `musicxx.var.changed`（载荷里带 key）
int32_t count = 0;
iface.vars->subscribe(host, &pluginxxView(R"(["musicxx.ui.animatedLevel"])"), &count);
// 事件表: events->subscribe(host, &pluginxxView(MUSICXX_PLUGIN_EVENT_VAR_CHANGED), cb, ud)
```

**handler 模式**（值本来就在你手里）：声明 `mode = MUSICXX_PLUGIN_VAR_MODE_HANDLER`，
订阅 `musicxx.var.read` / `musicxx.var.write` 两个事件，处理完用 `respond` 回答
（也可以对自己的键 `set` 一次 = 提交，框架把提交当作回执与回答）：

```cpp
// 收到 musicxx.var.read {requestId, key} 时：
iface.vars->respond(host, requestId, /*ok=*/1, &pluginxxView(当前值), nullptr, &out);
// 收到 musicxx.var.write {requestId, key, value, by} 时：
//   校验/写入后回最终值；拒绝时 ok=0 并把原因写进 error
iface.vars->respond(host, requestId, 1, &pluginxxView(最终值), nullptr, &out);
```

SDK 便利包装：`varsRegister` / `varsUnregister` / `varsGet` / `varsPeek` / `varsWatch` /
`varsUnwatch` / `varsSet` / `varsList` / `varsInfo` / `varsSubscribe` / `varsUnsubscribe` / `varsRespond`
（没有 `own` 与 `subscribers` 的单独方法：用 `varsInfo` 读 `subscribers` 字段）。

容量与超时（变量的数量、键长、值大小、订阅数都不设上限；读默认 2 秒、写 3 秒、应用键 5 秒；
未回执的读/写请求各 ≤ 32 条）见 [plugin-api.md](plugin-api.md) §5.5 与 §11.2。
返回码：`-1` 参数 / `-2` 状态 / `-3` 值不是合法 JSON / `-4` 键不存在 / `-6` 命名空间或写权限 /
`-7` 超限 / `-9` 该变量没有声明对应能力（写只读变量、读只写变量）。

官方变量（`musicxx.*`）的目录见 [plugin-vars.md](plugin-vars.md)：它们由应用声明并提供，
没声明的键读/写一律返回「未找到」。

---

## 5.6 网络（`musicxx.net`：HTTP / TCP / WebSocket）

接口表 `musicxx.net`（`MusicxxPluginNetIface`，SDK 里是 `iface.net`），只有三个入口：
同步 `call`、异步 `call_async`（目前只有 `http.request`）、取消 `cancel`。所有操作都是
`op + 参数 JSON → 结果 JSON`，op 全表、参数、事件字段与上限见
[plugin-api.md](plugin-api.md) §3.2.6「内置网络能力」。

```cpp
// 起一个 HTTP 服务（默认只监听 127.0.0.1；port = 0 由系统分配）
std::string out;
netCall(net_op::httpServe, R"({"id":"api","port":0})", out);
// out = {"ok":true,"id":"api","host":"127.0.0.1","port":52341,"url":"http://127.0.0.1:52341"}

// 静态内容：命中后由框架直接回 200 / 206 / 416，不进 `http.request`
// （也是“把播放器打不开的东西变成 http 地址”的短路径）
netCall(net_op::httpRoute,
        R"({"serverId":"api","file":"D:/music/a.mp3","fileName":"a.mp3"})",
        out);
// out = {"ok":true,"serverId":"api","path":"/f/<token>/a.mp3",
//        "url":"http://127.0.0.1:52341/f/<token>/a.mp3","size":123456,"reused":false,…}
// 起来时一次挂好几条：httpServe 的 "static": [{file|bytes, path?, contentType?, fileName?}]
// 只摘一条用 httpUnroute（{"serverId","path"}）

// 订阅本插件的网络事件（主题 plugin.<插件id>.net）
// 处理器在插件框架线程执行，不能阻塞；netCall 在这里调是安全的。
netSubscribe([](std::string_view eventJson) {
    const utilxx_base::Json event = utilxx_base::Json::parse(eventJson);
    if (event.value("kind", std::string{}) != "http.request") {
        return;
    }
    utilxx_base::Json response = utilxx_base::Json::object();
    response["requestId"] = event.value("requestId", int64_t{0});
    response["status"] = 200;
    response["body"] = "ok";
    std::string ignored;
    netCall(net_op::httpRespond, response, ignored);
});

// 异步 HTTP 客户端（完成通知在插件框架线程恰好一次）
static void PLUGINXX_CALL onHttpDone(void* /*ud*/, int32_t status,
                                     const PluginxxStringView* payload) {
    // status == PLUGINXX_OPERATOR_OK 时 payload 是响应 JSON，否则是错误 JSON
}
PluginxxOperatorNotify notify{&onHttpDone, nullptr};
int64_t requestId = 0;
std::string accepted;
netCallAsync(net_op::httpRequest, R"({"url":"http://127.0.0.1:52341/health"})",
             &notify, &accepted, &requestId);

// 两个常用形态(与 JS 的 musicxx.net.fetch / download 一致):
// httpFetch 拿正文(带 status / headers / body), httpDownload 直接落到
// <data_dir>/<插件id>/data/downloads/ 下并返回 path / bytes
netCallAsync(net_op::httpFetch, R"({"url":"https://example.com/a.json"})",
             &notify, &accepted, &requestId);
netCallAsync(net_op::httpDownload,
             R"({"url":"https://example.com/a.mp3","fileName":"a.mp3"})",
             &notify, &accepted, &requestId);
```

要点：

- **表可能为空**（旧插件框架库）：先判 `netAvailable()`；没有就降级（继续用动作
  `musicxx.net.fetch`，或者直接给播放器本地路径）。
- **服务端默认只监听 `127.0.0.1`**，端口写 `0` 由系统分配；要同网段可访问显式写
  `host: "0.0.0.0"`。
- **客户端支持 `http://` / `https://` / `ws://` / `wss://`**：TLS 由插件框架静态链接的 OpenSSL 完成
  （证书链与主机名校验默认开；参数 `tlsVerify` / `caFile` / `caPem` / `serverName`）。
  **服务端不做 TLS**（`http.serve` / `ws.listen` 只监听明文）。构建里关掉 TLS 时 `https` / `wss`
  得到 `tls_unsupported`；`netCall(net_op::info, ...)` 的 `tls` 字段能提前看出当前构建有没有 TLS。
- **不要在钩子 / 能力处理器里同步等 HTTP 响应**：`netCall` 会等网络线程（有界 5 s），
  响应回来要经过事件回调。客户端请求用 `netCallAsync`（完成通知）或听
  `http.response` 事件。
- **事件处理器在插件框架线程**（与钩子处理器同一条线程），不能阻塞；
  `netCall`（起服务 / 发送 / 关闭）在这里调用不会互锁。
- 服务、连接与未完成的请求都跟着插件实例：停用 / 卸载 / 框架停机自动关（端口释放）。
- 完整写法参考 `plugins/example_js_net`（JS 版，op 与事件与动态库插件完全一致）。

## 6. 已接入裁决钩子的写法

只有**主程序已接入**的钩子会被真正派发（`plugin-hooks.md` 的「主程序是否已接入」列标「已接入」）。
钩子的注册、优先级、裁决外壳、异步裁决、处理器里不该做的事见 [plugin-api.md](plugin-api.md) §8。
当前可用的裁决型钩子有七个（前三个是播放链路，后四个是“改播放值”）：

```cpp
// ① 跳过本曲：只支持 skip（返回其它 action 会被记日志后忽略）
hook(MUSICXX_PLUGIN_HOOK_PLAYER_BEFORE_PLAY_SONG, 0,
     [](std::string_view input, std::string& out) -> int32_t {
         if (input.find("\xe5\xb9\xbf\xe5\x91\x8a") != std::string_view::npos) {
             out = R"({"action":"skip"})";
         }
         return 0;
     });

// ② 不解析某个音源（换下一个）/ 只为本轮换一个地址
hook(MUSICXX_PLUGIN_HOOK_PLAYER_SOURCE_BEFORE_PARSE, 0,
     [](std::string_view input, std::string& out) -> int32_t {
         if (input.find("\"type\":\"Bili\"") != std::string_view::npos) {
             out = R"({"action":"skip"})";   // 本轮不解析该音源
             // 或者只替换本轮使用的音源（不改歌曲实体的音源列表）：
             // out = R"({"action":"replace","patch":{"src":{"type":"UrlLink","src":"https://example.com/a.mp3"}}})";
         }
         return 0;
     });

// ③ 播放错误：停止 / 下一曲 / 不再重试当前源
hook(MUSICXX_PLUGIN_HOOK_PLAYER_ERROR, 0,
     [](std::string_view input, std::string& out) -> int32_t {
         out = R"({"action":"continue","patch":{"tryNextSrc":false}})";
         return 0;
     });

// ④ 改播放值（音量 / 速度 / 音调 / 跳进度）：载荷都有 `from` / `to`
//    `{"patch":{"to":…}}` = 换新值；`{"action":"cancel"}` = 这次不改
//    下面只演示回答形状：真实插件要按自己的规则判断（用 utilxx_base::Json 读 `to`）
hook(MUSICXX_PLUGIN_HOOK_PLAYER_SPEED, 0,
     [](std::string_view input, std::string& out) -> int32_t {
         out = R"({"patch":{"to":2.0}})";   // 例：不管用户改成多少，都压到 2 倍速
         return 0;
     });
```

### 6.1 阶段：前链 / 实现链 / 后链

钩子 id 表示**一次调用**，参与方式由注册时的阶段表达（不再有 `.before` / `.after` 后缀的 id）。
`musicxx.player.seek` / `volume` / `speed` / `pitch` / `error` / `beforePlaySong` /
`source.beforeParse` 声明了 `impl` + `after`，`musicxx.feature.point.call` 声明了三个阶段。

```cpp
// 后链（改最终裁决）：拿到的是实现链合并出来的结果（这一步之前的结果）
hook(MUSICXX_PLUGIN_HOOK_PLAYER_SEEK, musicxx::plugin::HookPhase::After, 0,
     [](std::string_view input, std::string& out) -> int32_t {
         // 载荷里除原有字段外还有 `value` = 当前的合并结果
         out = R"({"value":{"action":"continue","patch":{"to":60000}}})";
         return 0;
     });

// 等价写法
hookAfter(MUSICXX_PLUGIN_HOOK_PLAYER_SEEK, 0, ...);
hookBefore(MUSICXX_PLUGIN_HOOK_PLAYER_SEEK, 0, ...);
```

规则（完整说明见 [plugin-api.md](plugin-api.md) §8.1）：

- 前链（`before`）**正序**、只能改输入（`{"patch": {...}}`）或停下（`{"action":"stop"}`）；
- 后链（`after`）**逆序**、只能改输出（`{"value": ...}`）或丢弃（`{"action":"drop"}`）。
  逆序是刻意的：外置插件最后改输出，应用自己的兜底实现不能盖掉插件的改写；
- 阶段处理器**必须是同步处理器**（`hook_sync`）：要拿到这一步的结果才能跑下一步；
- 只能注册钩子**声明过的**阶段。SDK 在注册前会读 `hook_info` 的 `phases`，没声明就返回 `-1`
  并记一条可读日志（低层直接调 `register_hook` 时插件框架同样拒绝，返回 `-1`）；
- 一个 `ownerTag` 可以分别登记三个阶段（覆盖键含阶段）；`unregisterHook` 会把该 `ownerTag`
  在所有阶段的登记一起摘掉；
- 低层写法：`MusicxxPluginHookSpec` 末尾追加了 `phase` 成员，写它时把 `version` 设成
  `MUSICXX_PLUGIN_HOOK_SPEC_VERSION`（2）并让 `struct_size = sizeof(..)`；
  老写法（`version = 1`、不带 `phase`）照旧可用，等价于 `impl`。

- 载荷字段与裁决语义见 [plugin-hooks.md](plugin-hooks.md) 的「已接入钩子的载荷与裁决」；
- 载荷里没有直链，只有来源类型与稳定 key（`srcKey` 是音源身份的完整 md5）；
- **处理器要快**：异步派发只表示“不占住主程序的调用线程”，插件框架侧仍然是同一个线程
  依次跑处理器。观察型处理器只做通知；裁决型等它结算（`seek` / `volume` / `speed` / `pitch`
  最多等 0.5 s，超时按“没意见”）。
- 想确认自己的处理器有没有被调用：管理页「插件详情 → 统计」，或自己在处理器里 `log.info(...)`。

---

## 6.5 功能点（声明 / 实现 / 调用）

功能点 = "这个功能谁都能实现，应用与别的插件按 id 来调"。与能力的区别：能力是"应用直接调本插件"、
参数与结果由插件自己定；功能点是"应用或第三方按 id 来调，谁实现都行"（可以有多个实现、按优先级问）。
**概念、通道的三条约定（`patch` 包装 / 按 `point` 过滤 / 不回答自己发起的调用）、三个可调的应用功能点、
`busy` 与零副作用规则**见 [plugin-api.md](plugin-api.md) §7；本节是 C++ 的写法。
插件点共用**一条固定钩子** `musicxx.feature.point.call`（插件框架只认固定的钩子表，**不要为每个点造新钩子**）。

```cpp
// 1) 声明自己的功能点（id 必须落在自己的命名空间里；结果经 notify 回来）
PluginxxOperatorNotify defineDone{&myDefineDone, this};
featurePointDefine(
    "plugin.my_plugin.beat",
    MusicxxPluginPointSpec{
        "节拍检测", "按 BPM 与拍号算出若干拍的时刻",
        "bpm（每分钟拍数，缺省 120）、beats（要几拍，缺省 4）",
        "{bpm, beats: [ms, ...], confident}"},
    &defineDone, 5000);

// 2) 给出实现：返回的是**值**（糖负责按 point/caller 过滤，并包成 {"patch":{"value": …}}）
featurePointProvide("plugin.my_plugin.beat",
    [](std::string_view argsJson) -> std::string {
        return R"({"bpm":128,"beats":[0,469,938],"confident":0.9})";
    });
// 也可以拿到点 id 与调用方（形参按可调用性自动选）：
// featurePointProvide("plugin.my_plugin.beat",
//     [](std::string_view point, std::string_view argsJson, std::string_view caller) { … });

// 3) 调一个功能点（应用留出的，或别的插件声明的）—— **只有异步形态**
PluginxxOperatorNotify callDone{&myCallDone, this};
featurePointCall("musicxx.media.palette.provide",
                 R"({"path":"D:/covers/a.jpg"})", 60000, &callDone);

featurePointUnprovide("plugin.my_plugin.beat");   // 摘除实现（一般不用：停用/卸载自动撤）
featurePointList(&someNotify, 5000);              // 有哪些功能点、谁在实现
```

- **处理函数必须同步返回**：功能点走同步裁决通道（插件框架按 `hook_sync` 就地调用），
  返回 `pluginxx::Task<T>` / 等异步结果都拿不到。长任务要么提前算好，要么做成**能力**
  （`co_await capabilityCall`）或事件。
- 返回 `utilxx_base::Json` / 字符串 / 数字 / 布尔；字符串先按 JSON 解析，解析不了就当 JSON 字符串
  （`hello` → `"hello"`）。**返回空串 = 这次不回答**；要回答"结果是空"就返回 `"null"`。
- 糖自动做三件事：按 `point` 过滤、`caller` 等于自己时不回答（免得把自己算出的结果再问回去）、
  把返回值包成 `{"patch":{"value": …}}`。已自带 `patch` / `action` 的返回值原样通过。
- `featurePointCall` 的默认超时 60 秒（频谱要解码整曲）；**没有同步版** ——
  框架线程上就地等待等于死锁（处理器可能正等着你返回），要结果就在 `notify->done` 里处理。
- 现在可调的应用功能点有三个（`featurePointList` 的 `callDoc` 写着参数与结果形状）：整曲频谱
  `musicxx.media.spectrum.extract`、副歌区段 `musicxx.media.chorus.analyse`、封面取色
  `musicxx.media.palette.provide`。**调用零副作用**：不写应用的值仓、不改应用状态。
- 声明跟着插件生命周期走：禁用 / 卸载会撤销全部声明，重新启用时 `start` 事务里再声明一遍。
- 手写处理器（不用糖）时按 [plugin-hooks.md](plugin-hooks.md) 的 `musicxx.feature.point.call`：
  **回答必须用 `patch` 包一层**，否则后面的处理器会覆盖你的结果。

---

## 7. 错误码与命名空间

命名空间规则、容量限制、超时一览这两边一样，见 [plugin-api.md](plugin-api.md) §11；
下面是插件框架返回给**动态库插件**的码：

| 码 | 含义 | 常见原因 |
|---|---|---|
| `0` | 成功 | — |
| `-1` | 参数非法 | `data` 不是 JSON 对象、缺 `title`、结构体大小不符 |
| `-2` | 状态错误 | 实例未启动 / 已禁用 / 正在卸载 |
| `-3` | JSON 非法 | 载荷解析失败 |
| `-4` | 未找到 | **注册未知钩子**、未知 UI 类型、注销不存在的项 |
| `-5` | 超时 | 动作请求 / 能力调用超时 |
| `-6` | 权限拒绝 | 命名空间非法（冒充他人事件主题 / UI 项 id / 变量键；**动作名与钩子 id 不受此限**：动作任意名都受理、钩子必须是约定里已有的） |
| `-7` | 队列满 | 保留的错误码（当前没有容量类限制，不会再产生） |
| `-99` | 内部异常 | 插件框架内部错误（看日志） |

命名空间：官方标识一律 `musicxx.*`（钩子 / 事件主题 / 动作 / UI 类型 / 权限）；
插件自定义一律 `plugin.<自己的插件 id>.*`（事件主题、能力名、UI 项 id）。冒充他人命名空间会被拒绝。

---

## 8. 权限（声明，不拦截）

清单 `permissions` 里写出来，主程序在**安装确认弹窗**与**插件详情页**展示，让用户知情：

| 权限 | 含义 |
|---|---|
| `musicxx.storage` | 插件私有数据目录与 KV |
| `musicxx.ui` | 注册 UI 项 / 页面 / 通知 |
| `musicxx.player.control` | 播放控制动作 |
| `musicxx.library.read` / `musicxx.library.write` | 读 / 写歌单、歌曲、歌词、历史 |
| `musicxx.net` | 使用主程序网络代理通道（`musicxx.net.fetch` / `download`） |
| `musicxx.fs.read:<路径>` / `musicxx.fs.write:<路径>` | 直接文件访问（路径限定） |
| `musicxx.system` | 打开链接 / 剪贴板 / 取目录等外围能力 |
| `musicxx.agent` | 预留 |

**运行时不做权限校验**：动作不会因为没声明而被拒绝，直接放进插件目录的插件同样不检查权限；
权限是「防误用 + 用户知情」，不是沙箱（动态库插件与主程序同进程）。

---

## 9. 构建

插件只需要 SDK 头文件 + 一个 CMake 助手：

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_plugin LANGUAGES CXX)

# SDK 前缀 = 插件框架构建产物的安装前缀（先跑一次 tools/build_native.ps1 / build_native.sh）
set(musicxx_extern_plugin_DIR "<安装前缀>/lib/cmake/musicxx_extern_plugin")
find_package(musicxx_extern_plugin CONFIG REQUIRED)

musicxx_plugin_add_target(my_plugin
  SOURCES my_plugin.cpp
  MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
  ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/shader"      # 可选：随插件分发的资源目录
  # ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/resource"  # 可选：插件自己的图标（resource/svg/*.svg）
)
```

安装前缀在哪：

```text
<musicxx_extern_plugin>/.native/build/<平台>-<配置>/musicxx-extern-plugin-install/
```

```powershell
# Windows（VS 生成器）
cmake -S . -B build -G "Visual Studio 18 2026" -A x64 `
      -Dmusicxx_extern_plugin_DIR="<安装前缀>/lib/cmake/musicxx_extern_plugin"
cmake --build build --config Release
# 产物：build/my_plugin.dll + build/plugin.yaml（这个目录可以直接当插件目录用）
```

```bash
# Linux / macOS（Ninja 或 Unix Makefiles）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -Dmusicxx_extern_plugin_DIR="<安装前缀>/lib/cmake/musicxx_extern_plugin"
cmake --build build
# 产物：build/my_plugin.so（macOS 为 my_plugin.dylib）+ build/plugin.yaml
```

`musicxx_plugin_add_target` 负责（见 `src/sdk/cmake/musicxx_plugin.cmake`）：

| 参数 | 作用 |
|---|---|
| `SOURCES` | 插件源码（至少一个） |
| `MANIFEST` | 清单路径，构建后复制成产物目录旁边的 `plugin.yaml` |
| `ASSETS` | 随插件分发的资源（目录按原名整体复制、文件按原名复制），例如 `shader/`、`resource/` |
| `LIBRARIES` | 额外链接的库（可选） |
| `OUTPUT_DIR` | 产物目录（默认当前构建目录；多配置生成器也不分子目录） |

助手同时会：建 SHARED 库并链接 SDK（以及存在就链接的插件框架/工具库）；设 C++26 与 MSVC `/utf-8`；
符号默认隐藏并**只导出 `musicxx_plugin_*` 入口**（GNU/Clang 用 version script，Apple 用导出符号表）。

工具链要求：C++26（MSVC ≥ VS 2022 17.14、GCC ≥ 14、Clang 与 NDK ≥ 18）。
插件自己静态链入第三方库时，注意不要让符号外泄（默认隐藏已由助手设置）。

插件框架库不在 Flutter 构建里"顺带编译"（依赖链太重），但**主程序构建会自动触发它**：平台工程在配置阶段调用
包内构建脚本的自动模式（已是最新则秒级返回；关掉见 `README.md`「打包（随应用分发）」）。
需要手工构建时：Windows 用 `pwsh -NoProfile -File tools/build_native.ps1`（示例插件用
`tools/build_examples.ps1`），Linux/macOS 用 `./tools/build_native.sh`（`--with-examples` 连示例插件一起构建；
示例插件构建 + 打包成安装包用 `./tools/build_examples.sh`），
iOS（只在 macOS 上）用 `./tools/build_native.sh --ios-simulator` / `--ios`（见 `README.md`「打包（随应用分发）」的 iOS 一节），
Android 见 §11。

---

## 10. 开发流程（从零到跑起来）

```text
① 建目录 my_plugin/：plugin.yaml + my_plugin.cpp
② 构建（§9）→ 得到 build/{my_plugin.dll, plugin.yaml}
③ 让应用看到它：
   - 用户插件目录：把 build/ 整个拷成 <插件目录>/my_plugin/（管理页「打开插件目录」），
     或打包成 zip 用管理页「从压缩包安装」；
   - 随包插件：放进应用的随包插件目录（开发期一般用上面那条）
④ 管理页「外部插件」→「重新扫描」→ 启用（加载即执行 start 事务）
⑤ 触发你挂的钩子 / 打开你的页面，看效果
⑥ 改了代码：重新构建 → 管理页「重载」（或禁用再启用）→ 再试
```

不点管理页也可以（AI agent / 自动化常用）：`musicxx-cli plugin install "<zip绝对路径>" --enable`
→ `plugin list` / `plugin logs` → 重新构建后 `plugin reload <插件id>`。完整用法与 MCP 工具见
[plugin-agent-cli.md](plugin-agent-cli.md)。

开发期建议：

- **先用示例插件做基线**：`plugins/example_native` 演示了钩子、能力、动作、事件、UI、日志，
  行为与 `plugins/example_js` 等价，两边可以对照着读；
- **框架日志默认没有输出目标**：设环境变量 `MUSICXX_EXTERN_PLUGIN_LOG_STDERR=1` 让框架打印到 stderr，
  再启动应用就能看到加载过程与你的 `log.info(...)`；
- 管理页（设置 → 插件 → 外部插件）分「插件 / 设置 / 调试」三个分页：
  插件分页做启用 / 禁用 / 重载 / 卸载 / 详情，设置分页管框架开关与安全模式，
  调试分页给钩子与插件统计、最近事件；
- 调试分页与「插件详情 → 运行统计」能看到：钩子调用次数 / 平均 / 最大 / 超时 / 失败 / 已暂停处理器、
  插件各阶段耗时、钩子数量（JS 插件还有能力 / 订阅 / 定时器数量）、计数器与最近事件；
- 原生侧的回归手段是包内测试（插件框架按「父目录下每个子目录 = 一个插件」扫描）：

  ```powershell
  pwsh -NoProfile -File tools/build_examples.ps1 -RunTests   # 用 <安装前缀>/plugins 当插件目录
  ```

  ```bash
  ./tools/build_examples.sh --run-tests                      # Linux / macOS 上的同一件事
  ```

  也可以把「对照示例」一组插件（`plugins/example_js_broken/`、
  `plugins/example_native_fail/`、`plugins/example_native_bad_entry/`）当成「故意失败」的对照，
  看插件框架怎么保护自己。

---

## 11. 部署与分发

- 用户安装的动态库插件在 **Android** 上可能加载失败：Android 7 起动态链接器只允许应用从 APK 的
  原生库目录加载动态库（应用数据目录里的 `.so` 报
  `is not accessible for the namespace "classloader-namespace"`）。插件框架按「安全降级」处理：
  标记该插件不可用、给出原因、不重试，其它插件照常工作；JS 插件不受影响。管理页会提前说明这条限制；
- 随包分发要放**当前平台**的库文件（Windows 包放 `.dll`，Linux 包放 `.so`，或者按 `platforms` 分目录打包）；
- 每个 ABI / 架构各构建一次（x64 与 arm64 的库不能互换）；
- 插件的导出面应当只有 5 个入口符号，可以自查：

  ```powershell
  dumpbin /exports build\my_plugin.dll     # 只应看到 musicxx_plugin_{get_info,create,start,stop,destroy}
  ```

  ```bash
  nm -D --defined-only build/my_plugin.so  # 同上（`_Z...` 之类的 C++ 符号不该出现）
  ```

---

## 12. 排障

| 现象 | 原因 / 处理 |
|---|---|
| 管理页显示「动态库插件库文件缺失」 | `entry` 名字/位置不对（记住按 Linux 写法填 `<名字>.so`，扩展名插件框架会按平台修正） |
| 「缺失/无效入口符号」 | 用了 `MUSICXX_PLUGIN_EXPORT` 之外的写法，或 `start`/`stop` 没导出；核对 §11 的导出面自查 |
| 「API 版本过低」 | 插件声明的 `api_version` 低于插件框架支持的最低版本（当前 1）；声明更高不会因此失败 |
| 扫描显示「当前平台不支持」 | `platforms` / `arch` 没写当前平台，或插件框架禁用了动态库插件 / 处于安全模式 |
| 注册钩子返回 `-4` | 钩子 id 不在约定表里（拼错或用了未定义的钩子）；用 `hook_ids.g.h` 里的常量，不要手写字符串 |
| 注册 UI 项返回 `-6` | 项名/动作命名空间不属于本插件 |
| 钩子一直不触发 | 该钩子在 `plugin-hooks.md` 里标「未接入」（主程序还没有接）；或插件被禁用 / 处理器没注册成功 |
| 点入口提示「插件『<插件id>』没有提供『xxx』」 | 该动作指向的能力没有注册（页面视图 id 必须与能力短名一致） |
| 页面打开后提示「插件没有提供任何内容块」 | 能力返回的视图没有 `blocks`，或块类型名全部拼错（未识别的块会被忽略） |
| 主程序整体卡住、日志最后一行动不了 | 处理器里做了阻塞操作（网络 / 大文件 / 同步等待）。框架线程被卡住时整个 Dart 线程也会停：把耗时工作改成 `offload` + 回调 |
| 插件加载后主程序启动异常 | 连续两次启动未完成会进入安全模式（本次不加载任何外部插件）；先修好插件再启动 |
| 想知道现在装了什么、某个插件为什么没跑起来 | 命令行 / MCP 一条命令看完：`musicxx-cli plugin list`（每个插件的 `loaded` / `error`）与 `plugin logs <插件id>`，见 [plugin-agent-cli.md](plugin-agent-cli.md) |
| 主程序卡住、不知道卡在哪一次调用 | 看主程序日志目录下的 `host_call.log`：进插件框架调用是同步的，卡住后最后一行就是没返回的那次调用 |
| 设置项改完重启又变回默认 | 把 `musicxx.storage.get` 的应答当成了对象外壳：**应答就是值本身**（见 [plugin-api.md](plugin-api.md) §3.2.5） |
| 切歌 / 换背景样式卡几秒 | 在同步裁决的处理器（取色、频谱提取、错误裁决）里发起了动作并等它：调用线程与主程序线程互等。改成"提前算好 / 异步读回"（[plugin-api.md](plugin-api.md) §8.4） |
| 封面取色 / 频谱"凭空消失" | 有处理器返回了 `{"disable": true}`（显式置空裁决）：用动作 `musicxx.feature.point.list` 看是谁关的（[plugin-api.md](plugin-api.md) §8.5） |
| 不知道某个动作的参数与返回 | 查 [plugin-api.md](plugin-api.md) §3（那一份是两种形态共用的动作清单）；没有列出的名字当前版本没实现 |

---

## 13. v1 边界与后续

| 边界 | 说明 |
|---|---|
| 专用接口表 | 已实现 `musicxx.hooks` / `musicxx.host` / `musicxx.ui` / `musicxx.vars` / `musicxx.net` 五张表（判定与追加规则见 §4.1）；`musicxx.player` / `library` / `lyrics` / `storage` / `stats` 的 IID 已冻结但**表体未实现**（查询返回 NULL）→ 这些能力统一走 `requestAction("musicxx.player.play", ...)` 等动作名，由主程序分派（动作清单见 [plugin-api.md](plugin-api.md) §3） |
| 读自己的统计 | 没有便捷接口（`musicxx.stats` 表未实现）：看统计用管理页「插件详情 → 运行统计」；上报用动作 `musicxx.stats.reportMemory` / `reportMetric` |
| 接口面与 JS 插件的关系 | **同一批接口**：同一套钩子表、同一份动作注册表、同一份状态镜像 / 变量协议 / 功能点协议；差别只在写法（C++ 是"发起 + 完成通知 / 协程"，JS 是 Promise）与语言能力（C++ 能自己起线程、直接用系统 API） |
| 进程隔离 | 插件与主程序同进程（无沙箱）：插件崩溃 = 主程序崩溃，权限只是声明 |
| 平台 | Windows / Linux / macOS / Android 可以加载动态库插件（Android 受限，见 §11）；iOS 上插件框架库随 `.app` 分发即可用，但用户安装的库通常被系统拒绝（只允许加载应用包内、与主程序同一签名身份的库）；OHOS 只允许 JS 插件 |
| 资源限制 | 插件框架不限制插件的内存 / 耗时 / 网络 / 数量，只做自我保护：事件队列上限（溢出丢最旧）与动作超时（插件自己给的时间）；统计只观测不限制 |
| 多实例 | 同一份库文件可以被创建多个实例（不同 id / 参数），因此**不要有可变全局状态** |
| 钩子接入 | 约定里有 66 个钩子，主程序当前**已接入**（`plugin-hooks.md` 里标「已接入」的共 28 条，会真的派发）；其余钩子注册成功但不会触发（它们打算做什么见 [plugin-api.md](plugin-api.md) §8.6）。两个预留观察钩子（`musicxx.media.wave.ready` / `musicxx.media.chorus.analysed`）已废弃 |