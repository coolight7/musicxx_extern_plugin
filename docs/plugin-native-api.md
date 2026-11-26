# musicxx 动态库插件开发指南（C++）

本文写给**要写一个动态库插件**的人：目录与清单、代码示例、开发流程、能用的框架与主程序能力、
构建与部署、排障。相关文档：

| 主题 | 文档 |
|---|---|
| **两种形态共用的接口**（能力与广播 / 约定 / 排障） | [plugin-api.md](plugin-api.md) |
| 总入口：选形态 / 环境准备 / 第一个插件 / 注意事项 / 发布检查 | [plugin-guide.md](plugin-guide.md) |
| 能力 id / 能不能参与 / 载荷与回答 | [plugin-capabilities.md](plugin-capabilities.md) |
| 广播频道 id / 有没有当前值 / 谁能写 / 载荷 | [plugin-broadcasts.md](plugin-broadcasts.md) |
| 界面（UI 项、插件页面、设置页）的字段与块类型 | [plugin-ui.md](plugin-ui.md) |
| 播放页背景（shader bundle 打包与 uniform 约定） | [plugin-shader-bundle.md](plugin-shader-bundle.md) |
| 装插件、刷新、看日志（命令行 / MCP，给 AI agent 与自动化） | [plugin-agent-cli.md](plugin-agent-cli.md) |
| JS 插件（零编译） | [plugin-js-api.md](plugin-js-api.md) |
| 示例代码 | `plugins/example_native/`（C++）、`plugins/example_js/`（等价 JS） |

> **本文只写 C++ 特有的东西**：清单与多目标打包、SDK、插件执行线程、接口表判定、构建部署、排障。
> **每个接口"做什么、参数是什么、返回什么、什么时候用、有什么坑"在 [plugin-api.md](plugin-api.md)** ——
> 那份文档 JS 与动态库插件共用（两边的接口是同一批：同一套能力声明、同一份广播频道）。
> 同一个功能在 C++ 里怎么写，§4.2 有一份对照示例。

> 动态库与主程序**同进程、同地址空间**：插件崩溃 = 主程序崩溃。开发期请先在
> `musicxx_extern_plugin_test` 与示例插件上验证，再放进正式环境；插件框架的安全模式
> （连续两次启动未完成 → 不加载任何外部插件，要用户在管理页关掉才会恢复）是最后一道保护，不是沙箱。

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
api_version: 9                     # 插件所属形态的接口代次：动态库插件写 C 接口代次（当前 10）；JS 插件写 3（JS 接口当前 3、兼容下限 3，见 §4.3）
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
要表达版本要求请用 `api_version`：它表示"插件按哪一代接口写的"，判定是 `api_version >= 插件所属形态的
兼容下限`（动态库插件 10、JS 插件 3，见 §4.3）。**每代接口只增不改**时下限不动，只有破坏性变更才抬
下限；清单写更高不会被拒（插件想表达自己用了新接口时可以写高）。

### 1.2 安装位置

- 用户插件：`<应用支持目录>/musicxx/extern_plugin/plugins/<id>/`；
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

#include <asio/awaitable.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace {

/// 实例上下文：**每个实例一份**，不要放可变全局/静态状态（同一份库可以被创建多个实例）
struct MyCtx : public musicxx::plugin::PluginBase {
    int32_t hits = 0;

    /// create 事务：注册写在协程里 —— 接口表是异步操作契约，注册要 `co_await`
    asio::awaitable<void> onCreate() {
        // 决策型能力：载荷里出现「广告」就跳过本曲（普通函数处理器，当场给出结论）
        co_await implement(
            MUSICXX_CAPABILITY_PLAYER_BEFORE_PLAY_SONG, 0,
            [this](std::string_view input, std::string &out) -> int32_t {
                if (input.find("\xe5\xb9\xbf\xe5\x91\x8a") != std::string_view::npos) {
                    ++hits;
                    out = R"({"action":"cancel","error":"my_plugin: 广告曲目"})";
                }
                return 0; // 0 且 out 为空 = 不裁决，交给下一个处理器
            });

        // 切歌是广播频道（`musicxx.song.changed`）：订阅后收推送
        co_await subscribeBroadcast(MUSICXX_BROADCAST_SONG_CHANGED,
                                    [this](std::string_view) {
                                        ++hits;
                                        log.info("切歌了");
                                    });

        // 自己的能力：全名必须是 plugin.<自己的插件 id>.<短名>
        // 处理器也可以写成协程：等待期间不占插件执行线程
        co_await implement(
            "plugin.my_plugin.probe", 0,
            [this](std::string_view) -> asio::awaitable<std::string> {
                const std::string song =
                    co_await readBroadcast(MUSICXX_BROADCAST_STATE_SONG);
                co_return "{\"hits\":" + std::to_string(hits) +
                          ",\"song\":" + song + "}";
            });
    }

    /// stop 事务：撤销自管资源（线程 / 定时器 / 临时文件）；可能被调用多次
    asio::awaitable<void> onStop() {
        hits = 0;
        co_return;
    }
};

} // namespace

MUSICXX_PLUGIN_EXPORT(
    MyCtx,
    "my_plugin",
    "2026.1125.1",              // 插件版本号：`<年>.<月日>.<小版本号>`（见 §1.1）
    "我的第一个插件",
    [](MyCtx &ctx) -> asio::awaitable<void> { co_await ctx.onCreate(); },
    [](MyCtx &ctx) -> asio::awaitable<void> { co_await ctx.onStop(); })
```

要点：

- `MUSICXX_PLUGIN_EXPORT` 生成插件框架要找的 **4 个入口符号**
  （`musicxx_plugin_{get_info,create,stop,destroy}`，没有 `start`）：`create` 就是"构造 + 注册事务"，
  禁用后再启用只重跑它。**不要手写入口**：少一个会被插件框架拒绝加载（管理页会给出「缺失/无效入口符号」）；
- 两个事务可以写成**协程**（`asio::awaitable<void>(Ctx&)`，注册类调用只能这么写）、**同步**
  （`int32_t(Ctx&)` / `void(Ctx&)`：返回 0 = 成功，非 0 = 失败并回滚）或插件框架原始形参
  `void*(Ctx&, const PluginxxOperatorNotify*, PluginxxString*)`（此时你自己 `notify->done`）。
  完成通知由 SDK 负责，不用自己写 done；
- **接口表是异步操作契约**：SDK 每个业务方法都返回 `asio::awaitable`，等结果写 `co_await`；
  同步函数里调不了它们（没有阻塞版），所以注册一律写在事务里；
- 失败不是错误码：抛 `std::runtime_error`（取消是 `pluginxx::CanceledException`），`what()` 以原因开头
  （`arg` / `state` / `namespace` / `not_found` / `not_declared` / `no_value` / `ended` …，
  后面跟一句人话解释），详见 §7；
- 处理器两种写法：**普通函数**（`std::string(std::string_view)`，当场返回结论）与**协程**
  （`asio::awaitable<std::string>`，可以 `co_await` 表调用与计时器）。两种都在插件执行线程上跑，
  也都由框架按“受理 → 完成”把结果交回主程序，主程序不区分写法；
- `implement` / `subscribeBroadcast` / `readBroadcast` 这些便捷方法都在
  `musicxx::plugin::PluginBase`（头文件 `musicxx/plugin/api/plugin_kit.h`）上；
- 自己造的能力用 `plugin.<你的插件 id>.*`，不需要事先声明；官方 id 没被应用声明、命名空间非法、
  数据不合法都会在登记时直接失败（异常里的原因说明为什么）。

---

## 3. 生命周期与线程

入口符号是插件框架定死的四个：`musicxx_plugin_get_info`、`musicxx_plugin_create`、
`musicxx_plugin_stop`、`musicxx_plugin_destroy`。用 `MUSICXX_PLUGIN_EXPORT(Ctx, id, 版本, 描述, 建, 停)`
时你只写「建」与「停」两个事务，`get_info` / `destroy` 由宏生成。

| 阶段 | 谁调用 | 你要做什么 |
|---|---|---|
| `create` | 插件执行线程 | **构造实例 + 注册事务**（两件事在一个入口里）：能力 / UI 项 / 订阅 / 读配置都写在这里；抛异常 = 这一次启用失败，框架回滚已登记的项 |
| 启用中 | 插件执行线程 | 处理器与能力被调用；可以做能力调用、发布广播、写自己的状态 |
| `stop` | 插件执行线程 | 撤销自管资源（线程、定时器、后台任务、临时文件）；注册由框架统一摘除 |
| `destroy` | 插件执行线程 | 只释放本地对象（不要在这里做 IO / 通知） |

- **没有单独的 `start`**：登记就是 `create` 的一部分，所以 `create` 里可以直接
  `co_await` 注册与订阅。禁用后再启用**不会重新构造实例**，只重跑注册事务；
  上一次 `stop` 之后留下的成员变量仍然在，别假设它被重置；
- 事务三种写法（`MUSICXX_PLUGIN_EXPORT` 都接受）：同步 `int32_t(Ctx&)`（0 = 成功）、
  同步 `void(Ctx&)`、协程 `asio::awaitable<void>(Ctx&)`。协程写法里完成通知由 SDK 在协程结束时
  补上一次（抛异常 = 失败，`what()` 作为原因回给框架）；
- 全部动态库插件代码都在**同一条插件执行线程**上顺序执行：注册表、统计、广播频道都不需要锁，
  **处理器之间按 (priority, 注册顺序) 稳定执行**。它**不是**框架的 IO 线程：插件代码再慢也不会
  拖住框架收发消息、也不会卡住主程序的界面；
- **处理器两种写法**：普通函数 `std::string(std::string_view)`（当场返回结论）与协程
  `asio::awaitable<std::string>(std::string_view)`（`co_await` 期间不占插件执行线程，
  别的插件照常跑）。三个阶段都收两种写法，登记时不区分；
- 处理器里**不要做阻塞工作**：普通函数处理器是就地调用的，卡多久就把这条链上的其它处理器
  推迟多久；网络请求、大文件、编解码交给
  `co_await pluginxx::asyncOffload(*this, ex, work)`（或直接调调度表的 `offload`），
  要等一会儿用 `co_await pluginxx::sleepAsync(*this, ms)`（挂起协程，不占线程）；
- 插件**可以**自己起线程或定时器，但要在 `stop` 里回收，并且**调用框架接口一律经接口表**
  （`iface.*` / `PluginBase` 的便捷方法），不要直接触碰框架内部结构；
- 广播的当前值是**协程**读的（`co_await readBroadcast(...)`），能力调用也是协程的
  （`co_await capabilityCall(action, args)`，失败抛异常）。**没有同步版**，同步上下文里调不了它们。

### 3.1 超时与取消（与 agentxx 共用的口径）

- **库侧默认不设超时**：插件框架不会因为你调用一个能力而替你计时。一次受理的请求可以一直等到终局；
  "等多久算失败"由调用方决定（`capabilityCallWithTimeout(..., timeoutMs)` 只是加上限，
  `timeoutMs <= 0` 就是不限）。Dart 侧的 `timeout:` 是"这次不再等"，本地 Future 结束，晚到的结果丢弃。
- **每个受理的请求恰好一条结果**，终局只有六种：成功 / 失败 / `canceled` / `plugin_unloaded` /
  `host_stopped` / 超时（由调用方自己的计时器产生，不是库产生的）。受理失败（返回空、参数错）不推结果。
- **插件卸载与框架停机必须给终局**：卸载时该插件名下未完成的调用按 `plugin_unloaded` 结算，
  停机时全部未完成请求按 `host_stopped` 结算，然后才发停止事件 —— 宿主不会一直挂着。
- **取消**：C ABI 侧由 `musicxx_extern_plugin_request_cancel` 完成，立刻以 `canceled` 结算；
  晚到的应答丢弃；请求不存在返回 OK（幂等）。
- 这些规则在 `cxx_pluginxx/host/ffi_bridge.h` 有一份共用实现（事件队列 + 请求记账 + 跨堆字符串），
  库内不放定时器：`nextDeadlineMs()` / `popExpired()` 由宿主用自己的事件循环驱动。

---

## 4. 能用的框架与应用能力

| 能力 | API | 说明 |
|---|---|---|
| 裁决型能力 | `co_await implement(能力 id, priority, fn)`（`implementBefore` / `implementAfter` 管前链 / 后链） | `fn = int32_t(std::string_view input, std::string& out)` 或 `std::string(std::string_view input)`；`out` 写 `{"action":...,"patch":{...}}`，留空 = 不裁决。返回类型决定写法：普通函数当场给结论，`asio::awaitable<std::string>` 可以等 IO；前链 / 后链同样收两种 |
| 收通知（切歌 / 播放状态 / 进度…） | `co_await subscribeBroadcast(频道, fn, optionsJson)` | 就是**广播频道**（`MUSICXX_BROADCAST_*`）；订阅是异步的，`co_await` 完成后拿到订阅句柄（由实例保活，不用自己记）；回调是普通函数，在插件执行线程上跑，要尽快返回 |
| 注销能力 | `co_await unimplement(能力 id)` | 只注销默认 `owner_tag`；不调用也会在停用/卸载时被框架摘除 |
| 带当前值的广播 | `co_await readBroadcast("musicxx.state.song")` 等 | 没有当前值时抛异常（原因 `no_value` / `not_declared`）；见 §5 |
| 框架版本 | `co_await hostInfoJson()` | `{appVersion, platform, language, dataDir, userPluginDir, builtinPluginDir, apiVersion, hostVersion, pluginId, ui?}`：`hostVersion` 是插件框架自己的版本（如 `2026.1011.1`），`pluginId` 是本插件 id（只有插件读时才带），`ui` 是客户端界面能力段（可选）；字段说明见 [plugin-api.md](plugin-api.md) §2.5 |
| 运行环境 | `co_await configAsync()`、`languageAsync()`、`argsJsonAsync()` | 来自插件框架基类。`configAsync()` 是运行环境 JSON（`appVersion/platform/arch/language/dataDir/logDir/pluginDirs/apiVersion`）；`argsJsonAsync()` 是加载时框架给的参数（默认 `{}`）。每次都是往框架要一次，高频值（语言、目录）自己缓存在实例上 |
| 插件配置路径 | `co_await configPath()` | 插件目录下的 `config.json`；插件自己读写（见 [plugin-ui.md](plugin-ui.md) §3） |
| 应用实现的能力（协程） | `co_await capabilityCall(action, argsJson)`；要单独限时用 `capabilityCallWithTimeout(action, argsJson, timeoutMs)` | 拿到的是结果 JSON 文本，业务失败抛异常（`what()` 是原因）。能力名与参数见 [plugin-api.md](plugin-api.md) §3（播放控制、歌单、歌词、提示与页面跳转、存储、网络、渲染槽位、封面与频谱、字体、自省…全在那一份）；内部按声明表（`tools/capabilities.def.json` 生成的 `capability_ids.g.h`）判，没声明过的名字直接拒绝；默认只按声明里的超时，`timeoutMs <= 0` 不额外加时限 |
| 自省（协程） | `co_await capabilityCall("musicxx.capability.subscribers", R"({"id":"<能力 id>"})")`、`capabilityCall("musicxx.feature.point.list")` | 前者答"这条能力现在有谁在提供、谁先执行"（`handlers`，按调用顺序）；后者答"应用留出了哪些扩展点、现在谁在提供"（功能埋点清单）。要"先看清有没有人接这条能力再决定要不要登记"就用它们，别猜 |
| 事件 | `co_await subscribeBroadcast(topic, fn)`；发布用 `co_await publishBroadcast(topic, payload)` | 频道 id 须 `musicxx.*` 或 `plugin.<自己 id>.*`；订阅别人的 `plugin.*` 是允许的 |
| 界面声明项 | `co_await uiItemRegister/uiItemUnregister/uiItemEntries/uiNotify` | 只有数据、没有界面树；**只对“用户列表里的条目”有效**（主页入口、歌曲/歌单菜单、「添加歌曲」来源项、音效）。类型取 `MUSICXX_PLUGIN_UI_SLOT_*`，字段见 [plugin-ui.md](plugin-ui.md) §1.3 |
| 界面内容 | `co_await uiContentRegister/uiContentUnregister/uiContentPush/uiSlotUpdate` | 挂在**槽位**上的界面 JSON，由生成函数按需给出：`uiContentRegister(短名, 槽位, 生成函数, title, depict, routesJson, depsJson, order, minIntervalMs)`（`depsJson` / `order` / `minIntervalMs` 可省）；生成函数可以同步完成，也可以先返回、稍后经 `notify` 补（主程序不会为等它停帧）；`uiSlotUpdate(槽位, idsJson)` = 让主程序按当前变量再问一次（不要求槽位已显示）；槽位清单见包内生成的 [ui-slots.md](ui-slots.md) |
| 播放页背景 | `uiContentRegister(..., MUSICXX_PLUGIN_UI_SLOT_PLAYING_BACKGROUND, ...)` | 生成函数给一棵块树（着色器是里面的 `musicxx.Shader`）；`title` / `depict` 在注册时给（用户选中之前就要列在设置里）；前景（三色）用能力 `musicxx.ui.playing.foreground.set`，见 [plugin-shader-bundle.md](plugin-shader-bundle.md) §5.1 / §8.1 |
| 普通页面背景 | `uiContentRegister(..., MUSICXX_PLUGIN_UI_SLOT_PAGE_BACKGROUND, 生成函数, title, depict, routesJson)` | 给歌单页 / 设置页 / 插件页面这类普通页面铺背景：`routesJson` 是适用路由数组（不写 = 所有普通页面），不需要用户选择，同一页面里最新注册的生效，弹窗与播放页不包含（见 [plugin-shader-bundle.md](plugin-shader-bundle.md) §5.5） |
| 播放页控制按钮栏 | `uiContentRegister(..., MUSICXX_PLUGIN_UI_SLOT_CONTROL_BAR, ...)` 或 `..._CONTROL_BAR_MENU` / `_PREVIOUS` / `_PLAY_OR_PAUSE` / `_NEXT` / `_FAVORITE` | 整栏或其中某个按钮换成插件画的（**替换**关系，取不到内容回退内置界面）；生成函数拿到的 ctx 里带依赖变量，写法见 [plugin-ui.md](plugin-ui.md) §1.8 |
| 插件页面 | `uiContentRegister(短名, MUSICXX_PLUGIN_UI_SLOT_PAGE, ...)` | 页面地址 `ext://<插件id>/<内容短名>`：主程序按内容 id 来要一份块树（插件页面不再走同名能力） |
| 音效 | `uiItemRegister(..., MUSICXX_PLUGIN_UI_SLOT_AUDIO_EFFECT, ...)` | 一段音频滤镜（`filter` / `pitch` / `cpuCost`），追加在『音效』插件的音效页面里与内置预设同一个列表；字段与规则见 [plugin-ui.md](plugin-ui.md) §1.9 |
| 自己的能力注册 | `co_await implement("plugin.<id>.<短名>", priority, fn)` | 处理器可以同步（`std::string(std::string_view)`），也可以协程（`asio::awaitable<std::string>(std::string_view)`，等待期间不占执行线程）；返回空串 = 没意见。**不要写阻塞处理器**，长任务走 `co_await pluginxx::asyncOffload(*this, ex, work)` |
| 一条 id 留多行 | `co_await implementWithTag(能力 id, ownerTag, priority, fn)` / `unimplementWithTag` | 主要用于功能埋点通道 `musicxx.feature.point.call`（`ownerTag` = 点 id）；同一个 `ownerTag` 再登记即覆盖 |
| 跨插件调用 | `co_await capabilityCall("plugin.<别的插件 id>.<短名>", argsJson)` | 同一个方法，只是 id 指到别人；直连插件框架、不经应用；**没有同步版**（插件框架会拒绝在 IO 线程阻塞等待） |
| 功能埋点（声明 / 实现 / 调用） | `co_await featurePointDefine` / `featurePointProvide` / `featurePointUnprovide` / `featurePointCall` / `featurePointList` | 见 §6.5：声明自己的功能埋点、给出实现、或调应用留出的功能埋点（频谱 / 副歌 / 封面取色）。`featurePointCall` 直接给被回答的值（没人回答给 `null`） |
| 日志 | `log.trace/debug/info/warn/error` | 同步的（写日志不进接口表），进插件日志流 → 管理页「日志」；也随 `musicxx.plugin.log` 事件回传 |
| 后台任务 / 取消 | 协程用 `spawn(...)`（随实例停用自动取消）；调度表 `iface.scheduler`（`sleep` / `offload`）；取消走 `cancel.op_cancel` | 计时一律 `co_await pluginxx::sleepAsync(*this, ms)`：插件自己没有会自走的 reactor，自建 `steady_timer` 不会自然到期 |

跨边界内存规则：`PluginBase` 的业务方法已经把字符串拷回 `std::string`，不用你管。只有自己**直接写接口表**时才需要：出参 `PluginxxString` 用 `pluginxx::PluginString`（RAII）接管，入参视图只在本次调用期间有效（要留就拷走），**绝不要用 CRT 的 `malloc/free` 把指针交给插件框架长期持有**。

### 4.1 接口表怎么判（版本 / 基线 / 追加成员）

`iface.*` 的每一张表都是自述「版本 + 字节长度」的结构体，判定只有两条：
`version >= 该表的最低版本`（`MUSICXX_PLUGIN_IFACE_*_MIN_VERSION`）且
`struct_size >= 该表的基线字节数`（`..._BASELINE_SIZE`）。SDK 的 `pluginxx::queryInterface<表>(host, IID)`
已经按它做完：拿到非空指针 = 表可用，**基线段成员全部可用**；不满足门槛时整张表返回 NULL
（`iface.host` / `iface.ui` / `iface.net` 为 `nullptr`），按"这个能力没有"降级即可。

- **表能查到 ≠ 表里每个成员都能用**：以后往表尾追加的成员要单独判
  `pluginxx::ifaceHas<&MusicxxPluginUIIface::新成员>(iface.ui) && iface.ui->新成员`。
  只判空指针会读到紧邻的另一块内存（老主程序给的表比插件期望的短）。
- 专用表（`musicxx.host` / `musicxx.ui` / `musicxx.kind`）的 `MUSICXX_PLUGIN_IFACE_*_VERSION` /
  `..._MIN_VERSION` / `..._BASELINE_SIZE` 与表体一起声明在 `plugin_api.h`；通用表（能力、广播、
  网络）的在插件框架库的 `pluginxx/api/*.h` 里。SDK 在 `plugin_kit.h` 里逐表给出 `pluginxx::IfaceMeta`
  特化（上面两条门槛读的就是它）。插件不要自己造表、也不要改主程序给的表。
- 现在的 `musicxx.host` / `musicxx.ui` / `musicxx.kind` 三张专用表都是 v1，
  基线 = 整表大小，也就是"表可用即全部成员可用"；
- **只有能力与广播两套表**：能力处理器往能力表登记（SDK 的 `implement` / `implementBefore` /
  `implementAfter` / `unimplement` 就是这么做的），通知走广播表的频道；应用的状态只能读与订阅，
  要改就调能力。`musicxx.host` 表里也没有"读状态 / 订阅状态 / 请求动作"成员（表版本 2）——
  当前值读广播频道，要应用做事就调能力（走能力表）。
- **插件形态（kind）不是写死的**：插件框架自带 `native`（动态库，按 C ABI 编译）与 `js`
  （内置 JS 运行器，不按 C ABI 编译），其它形态（以后的 Python 等）由插件用
  `musicxx.kind` 登记，见 §4.3。

### 4.2 常用接口的 C++ 写法

接口的语义（做什么、参数、返回、坑）在 [plugin-api.md](plugin-api.md)，这里是把它们写出来长什么样。
**业务方法都是协程**：`co_await` 之后直接拿结果，失败抛异常（`what()` 开头是原因，见 §7）。
既没有“发起 + 完成通知”的写法，也没有同步版 —— 这些活都在协程里做：`create` / `stop` 事务、
协程处理器，或 `spawn(...)` 起的一条任务。

**状态广播 / 框架信息**（见 [plugin-api.md](plugin-api.md) §2）：

```cpp
const std::string songJson = co_await readBroadcast(MUSICXX_BROADCAST_STATE_SONG);
const std::string envJson  = co_await readBroadcast(MUSICXX_BROADCAST_STATE_ENV);
const std::string infoJson = co_await hostInfoJson();      // 版本 / 平台 / 目录 / 本插件 id
const std::string cfgPath  = co_await configPath();        // 本插件 config.json 的路径
const std::string lang     = co_await languageAsync();     // "zh_Hans" 这类；高频值自己缓存
```

- 频道没有当前值时 `readBroadcast` 抛异常（原因 `no_value`），所以“没推过”要自己 `try` 接住，
  不要当空串处理；

**能力**（播放 / 歌单 / 提示 / 页面 / 存储 / 网络 / 渲染 / 封面与频谱；清单与参数见
[plugin-api.md](plugin-api.md) §3）：

```cpp
co_await capabilityCall("musicxx.player.toggle");
co_await capabilityCall("musicxx.player.seek", R"({"positionMs":64000})");
co_await capabilityCall("musicxx.ui.toast", R"({"text":"已刷新"})");
co_await capabilityCall("musicxx.ui.openRoute", R"({"route":"ext://my_plugin/card"})");
co_await capabilityCall("musicxx.storage.set", R"({"key":"greeting","value":"早上好"})");
co_await capabilityCall("musicxx.net.fetch", R"({"url":"https://example.com/a.json"})");

// 这一次要更长的上限（等用户操作、解码整曲）：timeoutMs <= 0 = 只按声明里的超时
const std::string shown = co_await capabilityCallWithTimeout(
    "musicxx.ui.overlay.show",
    R"({"title":"运行详情","blocks":[],"textConfirm":"知道了"})", 1200000);
co_await capabilityCall("musicxx.ui.overlay.close");
```

- 失败抛异常，`what()` 形如 `not_found: capability is not declared: ...` / `timeout: ...` /
  `invalid_argument: ...`；要区分“用户关掉了浮层”与“调用失败”就按原因文本判；
- 能力调用**按声明里的通道块校验**：写错名字 / 不在声明表里拿到 `not_found`，声明里没有 `plugin` 块的拿到
  `not_allowed`，声明里有但当前没人实现的拿到 `no_impl`。

**事件**（主题与载荷见 [plugin-api.md](plugin-api.md) §4）：

```cpp
auto sub = co_await subscribeBroadcast(MUSICXX_BROADCAST_STATE_SONG,
                                       [](std::string_view json) {
                                           // {"sid":..., "name":...} 当前歌曲的视图
                                       });

// 发布：只能发自己命名空间 plugin.<自己 id>.*（要带标题 / 允许合并这类声明时，
// 用广播表的 declare_broadcast 先声明）
co_await publishBroadcast("plugin.my_plugin.hello", R"({"at":1})");
```

- 订阅是异步的，`co_await` 完成才有订阅；回调是普通函数（不是协程），在插件执行线程上跑，
  要尽快返回。句柄由实例保活，`stop` 时自动收回，不用自己记；
- 限频写在第三个参数（JSON 文本）：`{"intervalMs": 200, "mode": "delay|drop"}`，0 / 不写 = 不限频。

**官方广播频道**（语义与原因文本见 [plugin-api.md](plugin-api.md) §5；原生侧只有广播表的读与订阅）：

```cpp
// 读当前值（协程，走插件框架里的缓存）
const std::string level = co_await readBroadcast(MUSICXX_BROADCAST_UI_ANIMATED_LEVEL);
// 订变化：带 pushOnSubscribe 的频道先补一次当前值，之后按自己声明的频率
co_await subscribeBroadcast(MUSICXX_BROADCAST_PLAYER_POSITION,
                            [](std::string_view json) { /* {sid, positionMs, durationMs} */ },
                            R"({"intervalMs":500,"mode":"delay"})");
// 要改它们：调能力（§6）；广播是只读的
```

**结束自己的一条流**（广播表 v3；语义与原因文本见 [plugin-api.md](plugin-api.md) §4.1）：

```cpp
// 订阅时给结束回调：属主结束时回调一次
co_await subscribeBroadcastWithEnd("plugin.my_plugin.task",
                                   [](std::string_view progress) { /* 值 */ },
                                   [](std::string_view reason)   { /* reason JSON */ });

// 只有属主能结束自己的频道；结束后再发布抛异常（原因 `ended`），
// 再 declareBroadcast 同一个 id = 开一条新流
co_await endBroadcast("plugin.my_plugin.task", R"({"reason":"done"})");
```

**能力登记 / 功能埋点 / UI / 日志**：

```cpp
// 注册成协程处理器：等待期间不占插件执行线程
co_await implement("plugin.my_plugin.probe", 0,
                   [](std::string_view args) -> asio::awaitable<std::string> {
                       co_return R"({"ok":true})";
                   });

// 调别的插件：同一个 capabilityCall，id 写到别人
const std::string other = co_await capabilityCall("plugin.other_plugin.probe", R"({"probe":1})");

co_await uiItemRegister("card", MUSICXX_PLUGIN_UI_SLOT_HOME_ENTRY, entryJson, 120);
co_await uiContentRegister("card", MUSICXX_PLUGIN_UI_SLOT_PAGE, &onGenerateCard,
                           "我的插件", "说明页");
co_await uiSlotUpdate(MUSICXX_PLUGIN_UI_SLOT_PAGE);   // 改完状态：让主程序再要一份
co_await uiNotify(R"({"text":"已刷新"})");

co_await featurePointProvide("plugin.my_plugin.beat",
                             [](std::string_view argsJson) -> std::string {
                                 return R"({"bpm":128,"beats":[0,469,938]})";
                             });
const auto palette = co_await featurePointCall("musicxx.media.palette.analyse",
                                                R"({"path":"D:/covers/a.jpg"})");

log.info("切歌了: " + songJson);
```

**自省**（“这条能力现在有谁在提供” / “应用留出了哪些扩展点”）：

```cpp
const std::string subs = co_await capabilityCall(
    "musicxx.capability.subscribers", R"({"id":"musicxx.media.spectrum.extract"})");
const std::string points = co_await capabilityCall("musicxx.feature.point.list");
```

- 想"先看清有没有人接这条能力再决定要不要注册"就用它们，别猜；
- 动态库插件**没有**"读自己的运行统计"的便捷接口（`musicxx.stats` 表未实现，见 §13）：
  看统计用管理页「插件详情 → 运行统计」，上报统计用能力 `musicxx.stats.reportMemory` /
  `reportMetric`。

### 4.3 插件形态（运行器）：让插件支持一种新的插件

插件框架本身只实现**动态库插件**（`kind: native`），内置的 JS 运行器是第二种形态（`kind: js`）。
这两种都不是写死的：扫描、判定与加载都先按 `kind` 查一张注册表，所以"把 JS 运行器搬到插件里"、
"以后接 Python"都不需要改插件框架 —— 由一个插件登记"本插件提供 kind=X"，交出加载 / 卸载能力即可。

```cpp
co_await kindRegister(R"({
  "kind": "python",
  "requiresPluginAbi": false,
  "interfaceVersion": 3,
  "minInterfaceVersion": 1,
  "loadCapability": "plugin.my_runtime.kind_load",
  "unloadCapability": "plugin.my_runtime.kind_unload",
  "validateCapability": "plugin.my_runtime.kind_validate",
  "describe": "Python 运行器"
})");
// 配套：co_await kindUnregister("python") / co_await kindList()
```

**预置形态的归属**（与 agentxx 共用同一张表，实现在 `cxx_pluginxx/host/kind_registry.h`）：

| 形态 | 谁实现 | 能否被接管 | 接口代次对 |
|---|---|---|---|
| `native` | 插件框架 | **不能**（换不了加载 / 卸载） | 跟随插件 ABI |
| `js` | 框架内置运行器 | **能**（登记同名形态 + 覆盖预置标记） | 由注册方给，撤销后回到内置那一份 |
| 其它 | 插件 | — | 由注册方给 |

接管只用于**解释执行**的形态；`native` 的加载与卸载不允许换出去。撤销接管后回到预置实现，
而不是变成"未登记形态"。框架对外的支持集就是当前登记过的形态 + 各自的代次对；
`plugin_inspect` 对未登记形态给 `kindKnown = false`，不会猜。

登记之后：

- **判定**：`kind: python` 的插件目录在扫描与安装预检里不再是"认不出的包"；提供者没加载时，
  原因写"由插件 X 提供，但它没有加载"。
- **"这个包能不能跑"由运行器回答**（`validateCapability`，可选）：加载前先调它一次，载荷
  `{"action":"validate","kind","id","path","version"}`；回答 `{"ok":true}` = 可以跑，
  `{"ok":false,"error":"..."}` = 拒绝（那句 `error` 会原样给用户）。运行器比插件框架更清楚
  这种插件长什么样（要哪些文件、清单哪些字段必填、依赖装没装），所以这类检查不该写在
  插件框架里。没写 `validateCapability` = 不做这一步。
- **安装预检也是问运行器**：`plugin_inspect` 是同步 C ABI，回答不了"要问插件"的问题，所以它
  只把形态事实报出来，由主程序在解包之后的预检里拿着去问：

  ```json
  {"kind":"python","kindKnown":true,"kindRequiresPluginAbi":false,
   "kindInterfaceVersion":3,"kindMinInterfaceVersion":1,
   "kindProvider":"py_runtime","kindProviderId":"py_runtime",
   "kindValidateCapability":"plugin.py_runtime.kind_validate"}
  ```

  主程序据此展示"运行环境接口版本"，再把上面那句 `validate` 请求发出去，被拒就不装。
  **`api_version` 的判定不在主程序做**（结论在 `supported` / `reason` 里）；宿主也可以直接读
  `co_await kindList()`（`status_info` 里的 `host.kinds`）拿到同一份信息。
- **每个形态有自己的接口代次对**（`interfaceVersion` / `minInterfaceVersion`）：清单里的
  `api_version` 表示"插件按哪一代接口写的"，判定就是 `api_version >= 该形态的 minInterfaceVersion`；
  插件读到的 `host.info().apiVersion` / `config().apiVersion` 是该形态的**当前代次**。

  | 形态 | 当前代次 | 兼容下限 | 含义 |
  |---|---|---|---|
  | `native`（内置） | 9 | 9 | C 接口代次（跟 `PLUGINXX_API_VERSION` / `MIN_API_VERSION` 一起抬）；通用表变了就是破坏性变更，下限跟着抬 |
  | `js`（内置） | **3** | **3** | JS 接口代次；2026-11 广播只剩读与订阅（旧的取值 / 写入 / 监听三个方法删掉）属破坏性变更，下限一起抬到 3 |
  | 运行器登记的形态 | 自己写 | 自己写 | `requiresPluginAbi: true` 的用框架的 C 接口那一对；解释执行的自报，不写 = 不判 |

  - **只增不改 → 只抬当前代次**：以后 JS 接口只做新增时，只改 `jsInterfaceVersion`，老插件照常加载，
    插件可以读 `config().apiVersion` 判断能不能用新接口；
  - **破坏性变更才抬下限**：已经发生过一次（2026-11 广播只剩读与订阅），把 `jsMinInterfaceVersion` 抬到了当时的当前代次——老插件（`api_version` 写 1 / 2）
    会被判“跑不了”，理由里带着它能用的最低代次（与 C 接口的 `PLUGINXX_MIN_API_VERSION` 同一套规则）；
  - **两套编号独立**：JS 的 3 与 C 接口的 10 不能互相比较（插件只看得到自己那一份）。
- **加载 / 卸载**：插件框架把插件目录交给登记的能力（请求
  `{"action":"load","kind","id","path","instance","version"}`），回答 `{"ok":true}` 表示接手；
  卸载同一条能力（`"action":"unload"`），也可以另给 `unloadCapability`。字段与超时见
  `plugin_api.h` 的 `MUSICXX_PLUGIN_IFACE_KIND`。
- **归属**：托管插件的能力 / UI 都是**运行器插件自己的注册**（插件框架不为它们创建实例）；
  停用 / 卸载托管插件只是转告运行器。管理页与 `plugin_list` 会把它们列成独立插件
  （带 `hosted: true` 与 `provider`）。
- 登记的形态**跟着插件生命周期撤销**（停用 / 卸载 / 关闭『拟声++』），不写磁盘；内置的
  `native` / `js` 不能被普通登记覆盖；要把**内置 JS 运行器搬进插件**，就在登记的 spec 里写
  `"overrideBuiltin": true`：此时 `js` 的加载 / 卸载都交给这个插件，撤销登记后自动回到内置
  JS 运行器（`native` 不允许接管 —— 动态库加载与生命周期是插件框架自己的职责）。

---

## 5. 状态广播（`cache: latest`）：读应用状态最快的方式

插件框架把应用的关键状态缓存成 JSON，插件用 `co_await readBroadcast(...)` 取（不往返、不占线程）：

```cpp
const std::string songJson = co_await readBroadcast(MUSICXX_BROADCAST_STATE_SONG);
```

- **键、每个键的字段、刷新时机与坑**：[plugin-api.md](plugin-api.md) §2（状态广播那一节）。
  可读的键：`musicxx.state.app` / `player` / `song` / `playlist` / `env` / `uiSlots` / `spectrum`；
  `musicxx.state.lyric` / `musicxx.state.songlists` 是预留的，当前版本没有推送（读取会抛 `no_value`）；
- C++ 常量：生成的 `MUSICXX_BROADCAST_STATE_APP` / `_PLAYER` / `_SONG` / `_PLAYLIST` / `_LYRIC` /
  `_PLAYLISTS` / `_ENV` / `_RENDER_SLOTS` / `_SPECTRUM`（`capability_ids.g.h`，不要手写键名）；
- 值必须是合法 JSON（大小不限制，早期版本的 512 KiB 上限已移除）；
- 每条状态广播带当前值：订阅哪个就收哪个（订阅时先推一次当前值）

  ```cpp
  co_await subscribeBroadcast(MUSICXX_BROADCAST_STATE_SONG, [](std::string_view json) {
      // {"sid":..., "name":...} 当前歌曲的视图
  });
  ```

- **播放进度要自己拿**：`musicxx.state.player.position` 只在启动、切歌与播放状态变化时刷新
  （**不是实时值**）。要按秒推进度就订阅广播频道 `musicxx.player.position`（不限频，
  带 `sid` / `positionMs` / `durationMs`，订阅时给 `{"intervalMs": …, "mode": "delay|drop"}`），
  要当场读当前值用 `co_await readBroadcast`；
- 加载时想立刻确认框架给的环境：`co_await configAsync()`（`appVersion/platform/arch/language/dataDir/logDir/pluginDirs/apiVersion`）
  与 `co_await argsJsonAsync()`（默认 `{}`）；
- 接口表里**已经没有** `subscribe_state`（框架 API 2 删掉）：状态键用广播频道的订阅与 `cache` 拿，当场读值用 `readBroadcast`。

---

## 5.5 官方广播频道：原生插件只能读与订阅

> **广播是只读的**：原生插件没有写应用状态的入口。读当前值用 `co_await readBroadcast(<频道宏>)`、
> 订变化用 `co_await subscribeBroadcast(<频道宏>, fn, optionsJson)`；改应用的状态调**能力**（§6），
> 发布自己命名空间下的频道用 `co_await publishBroadcast`（要先声明频道时用广播表的 `declare_broadcast`）。
>
> JS 插件侧是同一套（`musicxx.broadcast.read` / `subscribe` / `declare` / `publish`），
> 写法见 [plugin-api.md](plugin-api.md) §5，频道与取值域见 [plugin-broadcasts.md](plugin-broadcasts.md)。

**订阅一条 mpv 属性就是观察它**：频道 id 是 `musicxx.player.mpv.` + mpv 属性名原样
（`musicxx.player.mpv.audio-delay`、`musicxx.player.mpv.track-list/0/codec`）。有人订阅才绑
`mpv_observe_property`，退订 / 插件停用就解绑，**没人订阅时应用完全不避 mpv**（所以不需要“先登记再订阅”）：

```cpp
co_await subscribeBroadcast("musicxx.player.mpv.volume",
    [](std::string_view json) { /* 载荷 {name, value} */ },
    R"({"intervalMs":500,"mode":"delay"})");
```

绑不上（节点型属性、只能按二进制读的 `audio-fft`）会收到一条 `{name, value: null, error}`。
要**写**属性 / 执行命令 / 加滤镜用能力 `musicxx.player.mpv.set` / `.command` / `.filter`（高危，
见 [musicxx-interfaces.md](musicxx-interfaces.md) §4.8）。

---

## 5.6 网络（`<应用名>.net`：HTTP / TCP / WebSocket / UDP）

接口表 `<应用名>.net`（本项目是 `musicxx.net`）是插件框架的**通用表**，声明在
`pluginxx/api/net.h`：IID `PLUGINXX_IFACE_NET`（= `PLUGINXX_APP_ID("net")`）、表体类型
`PluginxxNetIface`（SDK 里是 `iface.net`）。实现不在本包：它是 `cxx_utilxx` 的
`utilxx::PluginNetHub`（自有网络线程）；本包只做“宿主 + 实例 → 插件 id”的解析。

只有两个入口：`call`（异步操作契约：受理就返回，结果经完成回调交回）与 `cancel`。所有操作都是 `op + 参数 JSON → 结果 JSON`，op 全表、参数、
事件字段与上限见 [plugin-api.md](plugin-api.md) §3.2.6「内置网络能力」；op 常量在
`pluginxx/net/ops.h`（SDK 把它以 `net_op` 别名引入，写法不变）。

```cpp
// 起一个 HTTP 服务（默认只监听 127.0.0.1；port = 0 由系统分配）
const std::string served = co_await netCall(net_op::httpServe, R"({"id":"api","port":0})");
// served = {"ok":true,"id":"api","host":"127.0.0.1","port":52341,"url":"http://127.0.0.1:52341"}

// 静态内容：命中后由框架直接回 200 / 206 / 416，不进 `http.request`
// （也是“把播放器打不开的东西变成 http 地址”的短路径）
const std::string routed = co_await netCall(
    net_op::httpRoute,
    R"({"serverId":"api","file":"D:/music/a.mp3","fileName":"a.mp3"})");
// routed = {"ok":true,"serverId":"api","path":"/f/<token>/a.mp3",
//           "url":"http://127.0.0.1:52341/f/<token>/a.mp3","size":123456,"reused":false,…}
// 起来时一次挂好几条：httpServe 的 "static": [{file|bytes, path?, contentType?, fileName?}]
// 只摘一条用 httpUnroute（{"serverId","path"}）

// 订阅本插件的网络事件（主题 plugin.<插件id>.net）
// 回调是普通函数，不能 co_await：要调表就把它放进一条任务里
co_await netSubscribe([this](std::string_view eventJson) {
    const std::string payload{eventJson};
    spawn([payload](MyCtx&, pluginxx::OpCtl&) -> asio::awaitable<void> {
        const utilxx_base::Json event = utilxx_base::Json::parse(payload);
        if (event.value("kind", std::string{}) != "http.request") {
            co_return;
        }
        utilxx_base::Json response = utilxx_base::Json::object();
        response["requestId"] = event.value("requestId", int64_t{0});
        response["status"] = 200;
        response["body"] = "ok";
        co_await netCall(net_op::httpRespond, response.dump());
    });
});

// HTTP 客户端：一样是 co_await，结果里带 status / headers / body
const std::string fetched = co_await netCall(
    net_op::httpFetch, R"({"url":"http://127.0.0.1:52341/health"})");
// 下载直接落到 <data_dir>/<插件id>/data/downloads/ 下，结果里给 path / bytes
const std::string downloaded = co_await netCall(
    net_op::httpDownload, R"({"url":"https://example.com/a.mp3","fileName":"a.mp3"})");
```

要点：

- **表可能为空**（旧插件框架库）：先判 `netAvailable()`；没有就降级（继续用能力调用
  `musicxx.net.fetch`，或者直接给播放器本地路径）。
- **服务端默认只监听 `127.0.0.1`**，端口写 `0` 由系统分配；要同网段可访问显式写
  `host: "0.0.0.0"`。
- **转发大流量要做背压**：`socket.send` 结果里的 `queuedBytes` 是发送队列里没写出去的字节数
  （上限 4 MiB，超了回 `queue_full`）；队列长了就 `socket.pause`（`{"id","paused":true}`）
  暂停上游读（不再产生 `data` 事件），缓过来再 `paused:false`。
- **客户端支持 `http://` / `https://` / `ws://` / `wss://`**：TLS 由插件框架静态链接的 OpenSSL 完成
  （证书链与主机名校验默认开；参数 `tlsVerify` / `caFile` / `caPem` / `serverName`）。
  **服务端不做 TLS**（`http.serve` / `ws.listen` 只监听明文）。所有平台的插件框架都带 TLS：
  `netCall(net_op::info, ...)` 的 `tls` 恒为 `true`，不会出现没有 `https` / `wss` 的构建。
- **不要在普通函数处理器 / 事件回调里直接联网**：`netCall` 是协程，没有阻塞版；
  在这类地方要么把活交给 `spawn(...)` 起的任务，要么写成协程处理器；
- **事件回调在插件执行线程**（与能力处理器同一条线程），不能阻塞；
  要在这时调表（如 `httpRespond`）就 `ctx.spawn(...)` 起一条任务。
- 服务、连接与未完成的请求都跟着插件实例：停用 / 卸载 / 框架停机自动关（端口释放）。
- 完整写法参考 `plugins/example_js_net`（JS 版，op 与事件与动态库插件完全一致）。

## 6. 已接入裁决型能力的写法

只有**主程序已接入**的能力会被真正派发（`plugin-capabilities.md` 的「状态」列标「已接入」）。
能力的注册、优先级、裁决外壳、异步裁决、处理器里不该做的事见 [plugin-api.md](plugin-api.md) §8。
当前可用的决策型能力有七个（前三个是播放链路，后四个是“改播放值”）：

```cpp
// ① 跳过本曲：只支持 skip（返回其它 action 会被记日志后忽略）
co_await implement(MUSICXX_CAPABILITY_PLAYER_BEFORE_PLAY_SONG, 0,
     [](std::string_view input, std::string& out) -> int32_t {
         if (input.find("\xe5\xb9\xbf\xe5\x91\x8a") != std::string_view::npos) {
             out = R"({"action":"skip"})";
         }
         return 0;
     });

// ② 不解析某个音源（换下一个）/ 只为本轮换一个地址
co_await implement(MUSICXX_CAPABILITY_PLAYER_SOURCE_BEFORE_PARSE, 0,
     [](std::string_view input, std::string& out) -> int32_t {
         if (input.find("\"type\":\"Bili\"") != std::string_view::npos) {
             out = R"({"action":"skip"})";   // 本轮不解析该音源
             // 或者只替换本轮使用的音源（不改歌曲实体的音源列表）：
             // out = R"({"patch":{"src":{"type":"UrlLink","src":"https://example.com/a.mp3"}}})";
         }
         return 0;
     });

// ③ 播放错误：停止 / 下一曲 / 不再重试当前源
co_await implement(MUSICXX_CAPABILITY_PLAYER_ERROR, 0,
     [](std::string_view input, std::string& out) -> int32_t {
         out = R"({"action":"continue","patch":{"tryNextSrc":false}})";
         return 0;
     });

// ④ 改播放值（音量 / 速度 / 音调 / 跳进度）：载荷都有 `from` / `to`
//    这四个能力的“改值”由应用自己执行（实现固定）：只能注册前链 / 后链，`impl` 会被拒
//    前链 `{"patch":{"to":…}}` = 改这次的目标值；`{"action":"cancel"}` = 这次不改
//    后链改最终值 / `{"action":"drop"}` 作废（后链拦不下已经改过的值，应用仍按前链后的值执行）
//    下面只演示回答形状：真实插件要按自己的规则判断（用 utilxx_base::Json 读 `to`）
co_await implementBefore(MUSICXX_CAPABILITY_PLAYER_SPEED, 0,
     [](std::string_view input, std::string& out) -> int32_t {
         out = R"({"patch":{"to":2.0}})";   // 例：不管用户改成多少，都压到 2 倍速
         return 0;
     });
```

### 6.1 阶段：前链 / 实现链 / 后链

能力 id 表示**一次调用**，参与方式由注册时的阶段表达（不再有 `.before` / `.after` 后缀的 id）。
`musicxx.player.seek` / `volume` / `speed` / `pitch` 的实现在应用自己身上（只开放
`before` + `after`），`musicxx.player.error` / `beforePlaySong` / `source.beforeParse`
声明了 `impl` + `after`，`musicxx.feature.point.call` 声明了三个阶段。

```cpp
// 后链（改最终值）：拿到的是实现链合出来的结果（这一步之前的结果）
co_await implementAfter(MUSICXX_CAPABILITY_PLAYER_SEEK, 0,
     [](std::string_view input, std::string& out) -> int32_t {
         // 载荷里除原有字段外还有 `value` = 当前的结果
         out = R"({"value":{"patch":{"to":60000}}})";
         return 0;
     });

// 等价写法
co_await implement(MUSICXX_CAPABILITY_PLAYER_SEEK, CapabilityPhase::After, 0, ...);
co_await implementBefore(MUSICXX_CAPABILITY_PLAYER_SEEK, 0, ...);
```

规则（完整说明见 [plugin-api.md](plugin-api.md) §8.1）：

- 前链（`before`）**正序**、只能改输入（`{"patch": {...}}`）或拦下（`{"action":"cancel"}`）；
- 后链（`after`）**逆序**、只能改输出（`{"value": ...}`）或丢弃（`{"action":"drop"}`）。
  逆序是刻意的：外置插件最后改输出，应用自己的兜底实现不能盖掉插件的改写；
- 三个阶段的处理器写法一样（普通函数或协程）：框架按 (priority, 注册顺序) 依次跑，
  上一步的结果就是下一步的输入；
- 只能注册能力**声明过的**阶段：应用没声明的官方 id 会被拒（`-2`），声明里没有该阶段的也拒；
  拒绝原因会原样回来，SDK 同时记一条可读日志；
- 一个 `ownerTag` 可以分别登记三个阶段（覆盖键含阶段）；`unimplement` 会把该 `ownerTag`
  在所有阶段的登记一起摘掉；
- 低层写法：往能力表登记一条能力 —— `id` = 能力 id、`phase`、`priority`，处理器只有
  `run_start` 一种形态（`musicxx.capability.implement` 的 C++ 侧就是这个调用）。

- 载荷字段与裁决语义见 [plugin-capabilities.md](plugin-capabilities.md) 的「载荷与回答（已接入的能力）」；
- 载荷里没有直链，只有来源类型与稳定 key（`srcKey` 是音源身份的完整 md5）；
- **处理器要快**：异步派发只表示“不占住主程序的调用线程”，插件框架侧仍然是同一个线程
  依次跑处理器。通知型（广播）只发通知；裁决型等它结算（`seek` / `volume` / `speed` / `pitch`
  最多等 0.5 s，超时按“没意见”）。
- 想确认自己的处理器有没有被调用：管理页「插件详情 → 统计」，或自己在处理器里 `log.info(...)`。

---

## 6.5 功能埋点（声明 / 实现 / 调用）

功能埋点 = "这个功能谁都能实现，应用与别的插件按 id 来调"。与能力的区别：能力是"应用直接调本插件"、
参数与结果由插件自己定；功能埋点是"应用或第三方按 id 来调，谁实现都行"（可以有多个实现、按优先级问）。
**概念、通道的三条约定（`patch` 包装 / 按 `point` 过滤 / 不回答自己发起的调用）、三个可调的应用功能埋点、
`busy` 与零副作用规则**见 [plugin-api.md](plugin-api.md) §7；本节是 C++ 的写法。
插件点共用**一条固定能力** `musicxx.feature.point.call`（插件框架只认固定的能力表，**不要为每个点造新能力**）。

```cpp
// 1) 声明自己的功能埋点（id 必须落在自己的命名空间里）
co_await featurePointDefine(
    "plugin.my_plugin.beat",
    MusicxxPluginPointSpec{
        "节拍检测", "按 BPM 与拍号算出若干拍的时刻",
        "bpm（每分钟拍数，缺省 120）、beats（要几拍，缺省 4）",
        "{bpm, beats: [ms, ...], confident}"});

// 2) 给出实现：返回的是**值**（糖负责按 point/caller 过滤，并包成 {"patch":{"value": …}}）
co_await featurePointProvide("plugin.my_plugin.beat",
    [](std::string_view argsJson) -> std::string {
        return R"({"bpm":128,"beats":[0,469,938],"confident":0.9})";
    });
// 也可以拿到点 id 与调用方（形参按可调用性自动选）：
// featurePointProvide("plugin.my_plugin.beat",
//     [](std::string_view point, std::string_view argsJson, std::string_view caller) { … });

// 3) 调一个功能埋点（应用留出的，或别的插件声明的）：直接拿被回答的值，没人回答是 null
const utilxx_base::Json palette = co_await featurePointCall(
    "musicxx.media.palette.analyse", R"({"path":"D:/covers/a.jpg"})");

co_await featurePointUnprovide("plugin.my_plugin.beat"); // 摘除实现（一般不用：停用/卸载自动撤）
const std::string points = co_await featurePointList();  // 有哪些功能埋点、谁在实现
```

- **处理函数要当场给回答**：糖把处理函数包成普通函数处理器（`std::string(std::string_view)`），
  这条通道是裁决型、回答随这次派发交回；协程处理器在这里编译不过。长任务要么提前算好，要么做成**能力**
  （`co_await capabilityCall`）或事件。
- 返回 `utilxx_base::Json` / 字符串 / 数字 / 布尔；字符串先按 JSON 解析，解析不了就当 JSON 字符串
  （`hello` → `"hello"`）。**返回空串 = 这次不回答**；要回答"结果是空"就返回 `"null"`。
- 糖自动做三件事：按 `point` 过滤、`caller` 等于自己时不回答（免得把自己算出的结果再问回去）、
  把返回值包成 `{"patch":{"value": …}}`。已自带 `patch` / `action` 的返回值原样通过。
- `featurePointCall` 不设超时（主程序最多等到调用方自己的动作超时），返回的是被回答的值：
  没人回答给 `null`。它是协程，返回后直接往下写就行，不用回调。
- 现在可调的应用功能埋点有三个（`featurePointList` 的 `callDoc` 写着参数与结果形状）：整曲频谱
  `musicxx.media.spectrum.extract`、副歌区段 `musicxx.media.chorus.analyse`、封面取色
  `musicxx.media.palette.analyse`。**调用零副作用**：不写应用的值仓、不改应用状态。
- 声明跟着插件生命周期走：禁用 / 卸载会撤销全部声明，重新启用时 `create` 事务里再声明一遍。
- 手写处理器（不用糖）时按 [plugin-capabilities.md](plugin-capabilities.md) 的 `musicxx.feature.point.call`：
  **回答必须用 `patch` 包一层**，否则后面的处理器会覆盖你的结果。

---

## 7. 失败原因与命名空间

命名空间规则、容量限制、超时一览两边一样，见 [plugin-api.md](plugin-api.md) §11；
这一节只说“失败了怎么告诉你”。

**失败不是错误码**：`PluginBase` 的业务方法失败时抛 `std::runtime_error`（被取消抛
`pluginxx::CanceledException`），`what()` 形如 `<原因>: <一句人话>`。原因名就是
`pluginxx::result::nameOf()` 给的词，插件按前缀判断就行：

| 原因 | 含义 | 常见场景 |
|---|---|---|
| `invalid_argument` | 参数非法 | `data` 不是 JSON 对象、缺 `title`、结构体大小不符 |
| `invalid_state` | 状态不对 | 实例未启动 / 已禁用 / 正在卸载；官方 id 没有被应用声明 |
| `invalid_json` | JSON 非法 | 载荷解析失败 |
| `not_found` | 没找到 | 未知 UI 类型、注销不存在的项、能力名不在声明表里 |
| `timeout` | 超时 | 能力调用超时 |
| `permission_denied` | 权限拒绝 | 命名空间非法（冒充他人事件主题 / UI 项 id / 广播频道 id；**调用的名字与能力 id 不受此限**：任意名字都受理；自定义能力用 `plugin.<自己>.` 即可） |
| `busy` / `out_of_memory` | 忙 / 内存不够 | 保留：当前没有容量类限制，不会产生 |
| `not_declared` | 没声明 | 调了没在声明表里的能力 / 广播频道 |
| `no_value` | 没当前值 | `readBroadcast` 的频道无值（应用自己也没读过一次） |
| `ended` | 流已结束 | 结束了的频道再发布 |
| `not_owner` | 不是属主 | 去结束 / 声明别人的频道 |
| `not_ready` | 还没准备好 | 框架未就绪 |
| `internal` | 内部错 | 插件框架内部问题（看日志） |

- 能力面另有两条应用自定义的原因：`not_allowed`（能力声明里没有 `plugin` 块，不允许插件用）与
  `no_impl`（声明里有、但当前没人实现）。它们也在 `what()` 前缀里，同样按文本判；
- **数字码只属于 C ABI**：`musicxx_extern_plugin_*` 的返回码与 `pluginxx/api/result.h`
  的 `PLUGINXX_RESULT_*`（`-1` 参数 / `-2` 状态 / `-3` JSON / `-4` 未找到 / `-5` 超时 /
  `-6` 权限 / `-7` 队列满 / `-500` 未就绪 / `-999` 内部）。需要把码给人看的时用
  `pluginxx::result::nameOf(code)`，不要自己再写一张表；插件自己的错误码用 `-10000 ~ -19999` 段。

命名空间：官方标识一律 `musicxx.*`（能力 / 广播频道 / UI 类型 / 权限）；
插件自定义一律 `plugin.<自己的插件 id>.*`（事件主题、能力名、UI 项 id）。冒充他人命名空间会被拒绝。

---

## 8. 权限（声明，不拦截）

清单 `permissions` 里写出来，主程序在**安装确认弹窗**与**插件详情页**展示，让用户知情：

| 权限 | 含义 |
|---|---|
| `musicxx.storage` | 插件私有数据目录与 KV |
| `musicxx.ui` | 注册 UI 项 / 页面 / 通知 |
| `musicxx.player.control` | 播放控制能力 |
| `musicxx.songlist.read` / `musicxx.songlist.write` | 读 / 写歌单（保存下来的列表：新建 / 删除歌单，增删改里面的歌） |
| `musicxx.playlist.read` / `musicxx.playlist.write` | 读 / 写当前播放列表（正在播的那一串歌） |
| `musicxx.net` | 使用插件框架的网络能力（`musicxx.net.*`：HTTP 客户端 / WebSocket / TCP / UDP；插件也可以自行联网） |
| `musicxx.fs.read:<路径>` / `musicxx.fs.write:<路径>` | 直接文件访问（路径限定） |
| `musicxx.system` | 打开链接 / 剪贴板 / 取目录等外围能力 |
| `musicxx.variables` / `musicxx.agent` | 预留（本版本不做限制） |

**运行时不做权限校验**：能力调用不会因为没声明而被拒绝，直接放进插件目录的插件同样不检查权限；
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

**插件 API 契约版本（`PLUGINXX_API_VERSION`）必须与插件框架库一致**：它决定接口表按哪套写法调
（例如能力怎么登记、广播怎么订阅），两侧不一致时是 ABI 不兼容。当前值是 **10**，由 SDK 的
INTERFACE 目标自动带上（`musicxx_plugin_add_target` 也用它），清单 `api_version` 写同一个数。
自己手写 CMake 时要补：

```cmake
target_compile_definitions(my_plugin PRIVATE PLUGINXX_API_VERSION=9 PLUGINXX_MIN_API_VERSION=9)
```

**按旧头文件编译过的插件必须重新编译**：这一代把能力表升到 v8、广播表升到 v4（两者所有入口
改成异步操作契约：受理即返回、结果经完成回调交回），旧二进制在加载阶段会被拒（「API 版本过低」）。

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
④ 管理页「外部插件」→「重新扫描」→ 启用（加载即执行 create 事务）
⑤ 触发你实现的能力 / 打开你的页面，看效果
⑥ 改了代码：重新构建 → 管理页「重载」（或禁用再启用）→ 再试
```

不点管理页也可以（AI agent / 自动化常用）：`musicxx-cli plugin install "<zip绝对路径>"`
（新插件装完默认启用；插件包在公网上时用 `plugin install --url <链接>`）
→ `plugin list` / `plugin logs` → 重新构建后 `plugin reload <插件id>`。完整用法与 MCP 工具见
[plugin-agent-cli.md](plugin-agent-cli.md)。

开发期建议：

- **先用示例插件做基线**：`plugins/example_native` 演示了能力、广播、UI、日志，
  行为与 `plugins/example_js` 等价，两边可以对照着读；
- **框架日志默认没有输出目标**：设环境变量 `MUSICXX_EXTERN_PLUGIN_LOG_STDERR=1` 让框架打印到 stderr，
  再启动应用就能看到加载过程与你的 `log.info(...)`；
- 管理页（设置 → 插件 → 外部插件）分「插件 / 设置 / 调试」三个分页：
  插件分页做启用 / 禁用 / 重载 / 卸载 / 详情，设置分页管框架开关与安全模式，
  调试分页给能力与插件统计、最近事件；
- 调试分页与「插件详情 → 运行统计」能看到：能力调用次数 / 平均 / 最大 / 超时 / 失败 / 已暂停处理器、
  插件各阶段耗时、处理器数量（JS 插件还有自己登记的能力 / 订阅 / 定时器数量）、计数器与最近事件；
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
- 插件的导出面应当只有 4 个入口符号，可以自查：

  ```powershell
  dumpbin /exports build\my_plugin.dll     # 只应看到 musicxx_plugin_{get_info,create,stop,destroy}
  ```

  ```bash
  nm -D --defined-only build/my_plugin.so  # 同上（`_Z...` 之类的 C++ 符号不该出现）
  ```

---

## 12. 排障

| 现象 | 原因 / 处理 |
|---|---|
| 管理页显示「动态库插件库文件缺失」 | `entry` 名字/位置不对（记住按 Linux 写法填 `<名字>.so`，扩展名插件框架会按平台修正） |
| 「缺失/无效入口符号」 | 用了 `MUSICXX_PLUGIN_EXPORT` 之外的写法，或 `create`/`stop` 没导出；核对 §11 的导出面自查 |
| 「API 版本过低」 | 清单 `api_version` 低于插件所属形态的兼容下限（动态库插件 10、JS 插件 3）；插件框架上报的 `apiVersion` 是该形态的**当前代次**（10 / 3），声明更高不会被拒 |
| 扫描显示「当前平台不支持」 | `platforms` / `arch` 没写当前平台，或插件框架禁用了动态库插件 / 处于安全模式 |
| 注册能力失败，原因 `not_found` | 能力 id 不在约定表里（拼错或用了没声明的能力）；用 `capability_ids.g.h` 里的常量，不要手写字符串 |
| 注册 UI 项失败，原因 `permission_denied` | 项名 / 界面动作的命名空间不属于本插件 |
| 处理器一直不触发 | 这条能力在 `plugin-capabilities.md` 里标「未接入」（主程序还没有接）；或插件被禁用 / 处理器没注册成功 |
| 点入口提示「插件『<插件id>』没有提供『xxx』」 | 入口的 `action` 写错了能力名（页面应该是 `route` + 已注册的内容） |
| 页面打开后提示「插件没有提供任何内容块」 | 内容没有 `blocks`，或块类型名全部拼错（未识别的块会被忽略） |
| 主程序界面卡住、日志最后一行动不了 | 处理器里做了阻塞操作（网络 / 大文件 / 同步等待）。插件代码虽然在独立线程上，但会把这条链与这个插件的其它调用一起卡住：把耗时工作改成协程处理器或交给 `asyncOffload` |
| 插件加载后主程序启动异常 | 连续两次启动未完成会进入安全模式（不加载任何外部插件），这个状态跨启动保留，要在管理页关掉安全模式才会重新加载；先修好插件再关 |
| 想知道现在装了什么、某个插件为什么没跑起来 | 命令行 / MCP 一条命令看完：`musicxx-cli plugin list`（每个插件的 `loaded` / `error`）与 `plugin logs <插件id>`，见 [plugin-agent-cli.md](plugin-agent-cli.md) |
| 主程序卡住、不知道卡在哪一次调用 | 看主程序日志目录下的 `host_call.log`：它用同步写记录“进入插件框架 / 返回”，卡住后最后一行就是没返回的那次调用（例如「能力派发开始: <id>」） |
| 设置项改完重启又变回默认 | 把 `musicxx.storage.get` 的应答当成了对象外壳：**应答就是值本身**（见 [plugin-api.md](plugin-api.md) §3.2.5） |
| 切歌 / 换背景样式卡几秒 | 处理器里做了阻塞等待（自建线程 `join`、同步文件 / 网络 IO）。普通函数处理器是就地调用的，卡多久就把这次派发推迟多久：把等待改成协程（协程处理器 + `co_await`） |
| 封面取色 / 频谱"凭空消失" | 有处理器返回了 `{"action":"cancel"}`（拦下这次调用）：用能力 `musicxx.feature.point.list` 看是谁拦的（[plugin-api.md](plugin-api.md) §8.5） |
| 不知道某条能力的参数与返回 | 查 [plugin-api.md](plugin-api.md) §3（那一份是两种形态共用的能力清单）；没有列出的名字当前版本没实现 |

---

## 13. v1 边界与后续

| 边界 | 说明 |
|---|---|
| 专用接口表 | 已实现 `musicxx.host` / `musicxx.ui` / `musicxx.kind` 三张专用表（判定与追加规则见 §4.1；形态注册见 §4.3）；网络是插件框架的通用表 `<应用名>.net`（§5.6）；`musicxx.player` / `playlist` / `songlist` / `lyric` / `storage` / `stats` 的 IID 已冻结但**表体未实现**（查询返回 NULL）→ 这些能力统一走 `capabilityCall("musicxx.player.play", ...)` 等能力名，由应用分派（能力清单见 [plugin-api.md](plugin-api.md) §3） |
| 读自己的统计 | 没有便捷接口（`musicxx.stats` 表未实现）：看统计用管理页「插件详情 → 运行统计」；上报用能力 `musicxx.stats.reportMemory` / `reportMetric` |
| 接口面与 JS 插件的关系 | **同一批接口**：同一套能力声明、同一份广播频道、同一份功能埋点协议；差别只在写法（C++ 是 asio 协程，JS 是 Promise）与语言能力（C++ 能自己起线程、直接用系统 API） |
| 进程隔离 | 插件与主程序同进程（无沙箱）：插件崩溃 = 主程序崩溃，权限只是声明 |
| 平台 | Windows / Linux / macOS / Android 可以加载动态库插件（Android 受限，见 §11）；iOS 上插件框架库随 `.app` 分发即可用，但用户安装的库通常被系统拒绝（只允许加载应用包内、与主程序同一签名身份的库）；OHOS 只允许 JS 插件 |
| 资源限制 | 插件框架不限制插件的内存 / 耗时 / 网络 / 数量，只做自我保护：事件队列上限（溢出丢最旧）与能力调用超时（插件自己给的时间）；统计只观测不限制 |
| 多实例 | 同一份库文件可以被创建多个实例（不同 id / 参数），因此**不要有可变全局状态** |
| 能力接入 | 能力表与「状态」列只看 [plugin-capabilities.md](plugin-capabilities.md)（由 `tools/capabilities.def.json` 的 `implemented` 字段生成）：标「已接入」的会真的派发，其余注册成功但不会触发（它们打算做什么见 [plugin-api.md](plugin-api.md) §8.7）。曾经预留过的频道 `musicxx.media.wave.ready` / `musicxx.media.chorus.analysed` 已从声明表删除（订阅收不到任何东西），对应能力改用 `musicxx.media.spectrum.extract` / `musicxx.media.chorus.analyse` |
