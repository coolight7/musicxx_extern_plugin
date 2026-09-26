# musicxx 动态库插件作者指南（C++ / v1）

本文写给**要写一个 C++ 动态库插件**的人：目录与清单、代码示例、开发流程、能用的宿主能力、
构建与部署、排障。相关文档：

| 主题 | 文档 |
|---|---|
| 钩子 id / 模式 / 派发 / 预算，以及已埋点钩子的载荷与裁决语义 | [plugin-hooks.md](plugin-hooks.md) |
| 界面（UI 项、插件页面、设置页）的字段与块类型 | [plugin-ui.md](plugin-ui.md) |
| 播放页背景（shader bundle 打包与 uniform 契约） | [plugin-shader-bundle.md](plugin-shader-bundle.md) |
| JS 插件（零编译） | [plugin-js-api.md](plugin-js-api.md) |
| 示例代码 | `plugins/example_native/`（C++）、`plugins/example_js/`（等价 JS） |

> 动态库与宿主**同进程、同地址空间**：插件崩溃 = 应用崩溃。开发期请先在
> `musicxx_extern_plugin_test` 与示例插件上验证，再放进正式环境；宿主的安全模式
> （连续两次启动未完成 → 本次不加载任何外部插件）是最后一道保护，不是沙箱。

---

## 1. 插件长什么样

一个插件 = 一个目录：

```text
my_plugin/                   # 目录名只是提示，插件 id 取清单的 name
├── plugin.yaml              # 清单（必填）
├── my_plugin.so             # 库文件（清单 entry 指向它；Windows 是 .dll、macOS 是 .dylib）
├── shader/                  # 可选：随插件分发的资源（如 shader bundle）
└── config.json              # 可选：插件自己的配置（插件读写，用户也可以直接编辑）
```

### 1.1 清单字段

```yaml
name: my_plugin                    # 插件 id（唯一；同名视为同一插件的升级覆盖）
entry: my_plugin.so                # 库文件名（按 Linux 写法填，见下方说明）
kind: native                       # native = 动态库；js = 脚本插件；缺省按 entry 推导
version: 1.0.0                     # 版本（升级比较用）
api_version: 1                     # 兼容的插件 API 版本（宿主当前是 1）
author: "你的名字"
description: "插件说明"
platforms: [windows, linux, macos] # 允许加载的平台；不写 = 不限
arch: [x64, arm64]                 # 允许的架构；不写 = 不限
depends: [other_plugin]            # 必选依赖（宿主会先加载它们）
optional_depends: [maybe_plugin]   # 可选依赖（有就排前面，没有也照常加载）

permissions:                       # 声明式权限：只做展示，运行时不校验（见 §8）
  - musicxx.player.control
  - musicxx.ui
  - musicxx.storage
```

`entry` 的写法（跨平台要点）：

- **统一按 Linux 写 `<名字>.so`**：宿主在 Windows/macOS 上会把扩展名修正为 `.dll`/`.dylib`
  （插件目录因此可以三平台通用，只要库文件基名一致；SDK 的构建助手已把产物前缀置空）；
- `entry` 指向的文件不存在时，宿主还会按平台默认库名再找一次
  （Linux `lib<name>.so`、Windows `<name>.dll`）；
- 写 `entry: my_plugin.dll` 会让 Linux/macOS 装载失败（扫描阶段就会提示「动态库插件库文件缺失」）。

清单里**没有**运行时行为相关的开关：配置由插件自己读写 `config.json`，宿主不解析插件配置。

宿主**不校验** `min_app_version`：Dart 侧清单解析会读这个字段，但当前版本没有用它做拦截，
要表达版本要求请用 `api_version`（宿主会拒绝 `api_version > 1` 的插件）。

### 1.2 安装位置

- 用户插件：`<应用支持目录>/musicxx/extern_plugin/plugins/<id>/`（管理页有「打开插件目录」）；
- 随包插件：安装包放进应用的随包插件目录（宿主启动时把两个目录都扫描出来）；
- 安装方式：管理页「从压缩包安装」（`.zip`，顶层就是插件目录的内容），或直接把目录放进插件目录后
  「重新扫描」。

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
    "1.0.0",
    "我的第一个插件",
    myStart,
    myStop
)
```

要点：

- `MUSICXX_PLUGIN_EXPORT` 生成宿主要找的 5 个入口符号
  （`musicxx_plugin_{get_info,create,start,stop,destroy}`）。**不要手写入口**：少了 `start`/`stop`
  会被宿主按契约拒绝装载（管理页会给出「缺失/无效入口符号」）；
- 两个事务函数的签名是 `int32_t(Ctx&)`：返回 `0` = 成功，非 0 = 失败并回滚。完成通知由 SDK 负责
  （内核要求「返回前恰好一次 done」，手写容易漏）。需要自己做异步 start 时，改用内核原始签名
  `void*(Ctx&, const PluginxxOperatorNotify*, PluginxxString*)`，此时由你自己调用 `notify->done`；
- `hook` / `observe` / `capability` / `stateJson` 这些便捷方法都在
  `musicxx::plugin::PluginBase`（头文件 `musicxx/plugin/api/plugin_kit.h`）上；
- 注册失败会返回负错误码（见 §7 错误码），**不要忽略返回值**：未知钩子、命名空间非法、
  数据不合法都会在注册时直接失败。

---

## 3. 生命周期与线程

| 阶段 | 谁调用 | 你要做什么 |
|---|---|---|
| `create` | 宿主线程 | 只构造实例上下文（不注册、不做 IO） |
| `start` | 宿主线程 | **注册事务**：钩子 / 能力 / UI 项 / 订阅；失败返回非 0（宿主回滚，不留残留） |
| 启用中 | 宿主线程 | 处理器与能力被调用；可以做动作请求、事件发布、写自己的状态 |
| `stop` | 宿主线程 | 撤销自管资源（线程、定时器、后台任务、临时文件）；注册由宿主统一摘除 |
| `destroy` | 宿主线程 | 只释放本地对象（不要在这里做 IO / 通知） |

- 全部动态库插件代码都在**同一条宿主线程**（插件管理器线程）上顺序执行：注册表、统计、状态镜像
  都不需要锁，**处理器之间按 (priority, 注册顺序) 稳定执行**；
- **处理器里禁止阻塞**：网络请求、大文件、编解码、`sleep` 都不要直接做 —— 一个插件卡住会拖慢
  同一线程上的所有插件（宿主不会强杀你，只会记录耗时并提示）。耗时工作的正确做法：
  1. 用内核的调度原语把阻塞工作交给宿主的线程池，完成后回到宿主线程
     （`pluginxx::offload(ctx, work)` 与协程 `Task<T>`；`pluginxx::sleep(ctx, ms)` 是异步等待，
     不占用线程）；
  2. 把工作拆成「提交 + 回调」两段：`start` 只注册，真正的初始化放进异步任务，完成后再补注册 / 写状态；
- 插件**可以**自己起线程或定时器，但要在 `stop` 里回收，并且**调用宿主接口一律经接口表**
  （`iface.*` / `PluginBase` 的便捷方法），不要直接触碰宿主内部结构；
- 状态镜像的读取是**同步**的（`stateJson(...)`），动作请求是**异步**的（结果经回调）。

---

## 4. 能用的宿主能力

| 能力 | API | 说明 |
|---|---|---|
| 裁决钩子 | `hook(id, priority, fn)` | `fn = int32_t(std::string_view input, std::string& out)`；`out` 写 `{"action":...,"patch":{...}}`，留空 = 不裁决 |
| 观察钩子 | `observe(id, fn)` | 返回值忽略；处理器要尽快返回 |
| 注销钩子 | `unregisterHook(id)` | 只注销默认 `owner_tag`；不调用也会在停用/卸载时被宿主摘除 |
| 状态镜像（同步只读） | `stateJson("musicxx.state.song")` 等 | 见 §5 |
| 宿主信息 | `hostInfoJson()` | `{appVersion, platform, language, dataDir, userPluginDir, builtinPluginDir, apiVersion, hostVersion}` |
| 宿主环境 | `config()`、`language()`、`argsJson()` | `config()` 是宿主环境 JSON（`appVersion/platform/arch/language/dataDir/logDir/pluginDirs/apiVersion`）；`argsJson()` 是装载时宿主给的参数（默认 `{}`） |
| 插件配置路径 | `configPath()` | 插件目录下的 `config.json`；插件自己读写（见 [plugin-ui.md](plugin-ui.md) §3） |
| 宿主动作（异步） | `requestAction(action, argsJson, &notify, timeoutMs, &requestId)` | `notify->done` **恰好一次**（SDK 会复制通知，传栈上对象也安全）；默认超时 5 s，实际生效范围 1 s ~ 60 s |
| 事件 | `subscribeTopic(topic, fn)`；发布用 `iface.events->publish` | 主题须 `musicxx.*` 或 `plugin.<自己 id>.*`；订阅别人的 `plugin.*` 主题是允许的 |
| 声明式 UI | `uiRegister/uiUpdate/uiUnregister/uiEntries/uiNotify` | 类型取 `MUSICXX_PLUGIN_UI_TYPE_*`，渲染由宿主负责（见 [plugin-ui.md](plugin-ui.md)） |
| 播放页背景 | `uiRegister(..., MUSICXX_PLUGIN_UI_TYPE_PLAYING_BACKGROUND, ...)` | 需要打包期编译好的 shader bundle（见 [plugin-shader-bundle.md](plugin-shader-bundle.md)） |
| 能力注册 | `capability(*this, "plugin.<id>.<短名>", fn)` | 处理器必须**同步返回**可 JSON 序列化的结果 |
| 日志 | `log.trace/debug/info/warn/error` | 进插件日志流 → 管理页「日志」；也随 `musicxx.plugin.log` 事件回传 |
| 后台任务 / 取消 | `iface.scheduler`（`offload` / `sleep` / `post_to_io` / `op_cancel`）、`iface.tasks`、`cancelRegistry` | 任务随实例停用自动取消 |

跨边界内存规则：出参字符串必须用宿主的分配器（`PluginBase::hostStringSet` / `hostStringFree`，
或用内核的 `pluginxx::PluginString` RAII）。**绝不要用 CRT 的 `malloc/free` 把指针交给宿主长期持有。**

---

## 5. 状态镜像（插件读宿主状态的最快方式）

宿主把常用状态做成 JSON 快照，插件**同步**读（不需要动作往返，也不占用宿主线程）：

| 键 | 内容 | 当前版本 |
|---|---|---|
| `musicxx.state.app` | `{version, versionStr, platform, lang, branch, installId, isNight, firstRun}` | 已推送 |
| `musicxx.state.player` | `{state, position, duration, volume, speed, quality, srcKey, mediaType, cacheLength}` | 已推送（启动 / 切歌 / 播放状态变化时刷新） |
| `musicxx.state.song` | 当前歌曲的只读视图（与 `song.changed` 的 `song` 同结构） | 已推送 |
| `musicxx.state.playlist` | `{pid, name, count, songNum, type, index, loopMode, autoPlayMode}` | 已推送 |
| `musicxx.state.env` | `{isPlaying, page, lanServerOn, userLogged}` | 已推送 |
| `musicxx.state.lyric` / `musicxx.state.library` | 预留 | **当前版本没有推送**（`stateJson` 返回空串） |
| `musicxx.state.renderSlots` | 渲染槽位的运行状态（谁在画、是否可见、尺寸、昼夜） | 按需推送，见 [plugin-shader-bundle.md](plugin-shader-bundle.md) §10 |

- 单键上限 **512 KiB**：超限时宿主**直接拒绝写入**并记日志（不会截断成半个 JSON）；
- 每个键更新都会推 `musicxx.state.changed` 事件（载荷 `{key, value}`），插件可以订阅它做增量处理；
- **播放进度当前版本没有推送**：`musicxx.state.player.position` 只在启动、切歌与播放状态变化时刷新，
  `musicxx.player.position` 钩子也尚未埋点。不要把它当每秒更新的进度用；
- 镜像里**不放临时直链与 token**：需要地址请走 `musicxx.net.*` 动作或自己请求。

C++ 常量：`MUSICXX_STATE_APP` / `_PLAYER` / `_SONG` / `_PLAYLIST` / `_LYRIC` / `_LIBRARY` / `_ENV` /
`_RENDER_SLOTS`。

---

## 6. 三个已埋点裁决钩子的写法

只有**应用侧已埋点**的钩子会被真正派发（`plugin-hooks.md` 的「是否已埋点」列标「已埋点」）。
下面三个裁决型钩子是当前版本可用的：

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
```

- 载荷字段与裁决语义见 [plugin-hooks.md](plugin-hooks.md) 的「已埋点钩子的载荷与裁决」；
- 载荷里没有直链，只有来源类型与稳定 key（`srcKey` 是音源身份的完整 md5）；
- **处理器要快**：裁决链有等待预算（同步钩子会被调用线程等待；`player.error` 只等 120 ms），
  超时按「不裁决」继续，迟到的结果被丢弃；
- 想确认自己的处理器有没有被调用：管理页「插件详情 → 统计」，或自己在处理器里 `log.info(...)`。

---

## 7. 错误码与命名空间

| 码 | 含义 | 常见原因 |
|---|---|---|
| `0` | 成功 | — |
| `-1` | 参数非法 | `data` 不是 JSON 对象、缺 `title`、结构体大小不符 |
| `-2` | 状态错误 | 实例未启动 / 已禁用 / 正在卸载 |
| `-3` | JSON 非法 | 载荷解析失败 |
| `-4` | 未找到 | **注册未知钩子**、未知 UI 类型、注销不存在的项 |
| `-5` | 超时 | 动作请求 / 能力调用超时 |
| `-6` | 权限拒绝 | 命名空间非法（冒充他人插件 / 非官方动作名） |
| `-7` | 队列满 | UI 项超过 64 个 |
| `-99` | 内部异常 | 宿主内部错误（看日志） |

命名空间：官方标识一律 `musicxx.*`（钩子 / 事件主题 / 动作 / UI 类型 / 权限）；
插件自定义一律 `plugin.<自己的插件 id>.*`（事件主题、能力名、UI 项 id）。冒充他人命名空间会被拒绝。

---

## 8. 权限（声明，不拦截）

清单 `permissions` 里写出来，宿主在**安装确认弹窗**与**插件详情页**展示，让用户知情：

| 权限 | 含义 |
|---|---|
| `musicxx.storage` | 插件私有数据目录与 KV |
| `musicxx.ui` | 注册 UI 项 / 页面 / 通知 |
| `musicxx.player.control` | 播放控制动作 |
| `musicxx.library.read` / `musicxx.library.write` | 读 / 写歌单、歌曲、歌词、历史 |
| `musicxx.net` | 使用宿主网络代理通道（`musicxx.net.fetch` / `download`） |
| `musicxx.fs.read:<路径>` / `musicxx.fs.write:<路径>` | 直接文件访问（路径限定） |
| `musicxx.system` | 打开链接 / 剪贴板 / 取目录等外围能力 |
| `musicxx.agent` | 预留 |

**运行时不做权限校验**：动作不会因为没声明而被拒绝，直接放进插件目录的插件同样不检查权限；
权限是「防误用 + 用户知情」，不是沙箱（动态库插件与宿主同进程）。

---

## 9. 构建

插件只需要 SDK 头文件 + 一个 CMake 助手：

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_plugin LANGUAGES CXX)

# SDK 前缀 = 宿主构建产物的安装前缀（先跑一次 tools/build_native.ps1 / build_native.sh）
set(musicxx_extern_plugin_DIR "<安装前缀>/lib/cmake/musicxx_extern_plugin")
find_package(musicxx_extern_plugin CONFIG REQUIRED)

musicxx_plugin_add_target(my_plugin
  SOURCES my_plugin.cpp
  MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
  ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/shader"      # 可选：随插件分发的资源目录
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
| `ASSETS` | 随插件分发的资源（目录按原名整体复制、文件按原名复制），例如 `shader/` |
| `LIBRARIES` | 额外链接的库（可选） |
| `OUTPUT_DIR` | 产物目录（默认当前构建目录；多配置生成器也不分子目录） |

助手同时会：建 SHARED 库并链接 SDK（以及存在就链接的内核/工具库）；设 C++26 与 MSVC `/utf-8`；
符号默认隐藏并**只导出 `musicxx_plugin_*` 入口**（GNU/Clang 用 version script，Apple 用导出符号表）。

工具链要求：C++26（MSVC ≥ VS 2022 17.14、GCC ≥ 14、Clang 与 NDK ≥ 18）。
插件自己静态链入第三方库时，注意不要让符号外泄（默认隐藏已由助手设置）。

宿主库本身不在 Flutter 构建里编译：Windows 用 `pwsh -NoProfile -File tools/build_native.ps1`，
Linux/macOS 用 `./tools/build_native.sh --run-tests`，Android 见 §11。

---

## 10. 开发流程（从零到跑起来）

```text
① 建目录 my_plugin/：plugin.yaml + my_plugin.cpp
② 构建（§9）→ 得到 build/{my_plugin.dll, plugin.yaml}
③ 让应用看到它：
   - 用户插件目录：把 build/ 整个拷成 <插件目录>/my_plugin/（管理页「打开插件目录」），
     或打包成 zip 用管理页「从压缩包安装」；
   - 随包插件：放进应用的随包插件目录（开发期一般用上面那条）
④ 管理页「外部插件」→「重新扫描」→ 启用（装载即执行 start 事务）
⑤ 触发你挂的钩子 / 打开你的页面，看效果
⑥ 改了代码：重新构建 → 管理页「重载」（或禁用再启用）→ 再试
```

开发期建议：

- **先用示例插件做基线**：`plugins/example_native` 演示了钩子、能力、动作、事件、UI、日志，
  行为与 `plugins/example_js` 等价，两边可以对照着读；
- **宿主日志默认没有输出目标**：设环境变量 `MUSICXX_EXTERN_PLUGIN_LOG_STDERR=1` 让宿主打印到 stderr，
  再启动应用就能看到装载过程与你的 `log.info(...)`；
- 管理页（设置 → 插件 → 外部插件）分「插件 / 设置 / 调试」三个分页：
  插件分页做启用 / 禁用 / 重载 / 卸载 / 详情，设置分页管框架开关与安全模式，
  调试分页给钩子与插件统计、最近事件；
- 调试分页与「插件详情 → 运行统计」能看到：钩子调用次数 / 平均 / 最大 / 超时 / 失败 / 已暂停处理器、
  插件各阶段耗时、钩子数量（JS 插件还有能力 / 订阅 / 定时器数量）、计数器与最近事件；
- 原生侧的回归手段是包内测试（宿主按「父目录下每个子目录 = 一个插件」扫描）：

  ```powershell
  pwsh -NoProfile -File tools/build_native.ps1 -RunTests   # 用 <安装前缀>/plugins 当插件目录
  ```

  也可以把 fixture 插件（`src/tests/fixtures/`）当成「故意失败」的对照来看宿主怎么保护自己。

---

## 11. 部署与分发

- 用户安装的动态库插件在 **Android** 上可能装载失败：Android 7 起动态链接器只允许应用从 APK 的
  原生库目录加载动态库（应用数据目录里的 `.so` 报
  `is not accessible for the namespace "classloader-namespace"`）。宿主按「安全降级」处理：
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
| 管理页显示「动态库插件库文件缺失」 | `entry` 名字/位置不对（记住按 Linux 写法填 `<名字>.so`，扩展名宿主会按平台修正） |
| 「缺失/无效入口符号」 | 用了 `MUSICXX_PLUGIN_EXPORT` 之外的写法，或 `start`/`stop` 没导出；核对 §11 的导出面自查 |
| 「api_version 不匹配」 | 插件声明的 `api_version` 高于宿主（当前 1） |
| 扫描显示「当前平台不支持」 | `platforms` / `arch` 没写当前平台，或宿主禁用了动态库插件 / 处于安全模式 |
| 注册钩子返回 `-4` | 钩子 id 不在契约表里（拼错或用了未定义的钩子）；用 `hook_ids.g.h` 里的常量，不要手写字符串 |
| 注册 UI 项返回 `-6` | 项名/动作命名空间不属于本插件 |
| 钩子一直不触发 | 该钩子在 `plugin-hooks.md` 里标「未埋点」（应用侧还没有接）；或插件被禁用 / 处理器已被暂停派发（管理页统计里 `paused`） |
| 点入口提示「插件『<插件id>』没有提供『xxx』」 | 该动作指向的能力没有注册（页面视图 id 必须与能力短名一致） |
| 页面打开后提示「插件没有提供任何内容块」 | 能力返回的视图没有 `blocks`，或块类型名全部拼错（未识别的块会被忽略） |
| 应用整体卡住、日志最后一行动不了 | 处理器里做了阻塞操作（网络 / 大文件 / 同步等待）。宿主线程被卡住时整个 Dart 线程也会停：把耗时工作改成 `offload` + 回调 |
| 插件装载后应用启动异常 | 宿主连续两次启动未完成会进入安全模式（本次不加载任何外部插件）；先修好插件再启动 |

---

## 13. v1 边界与后续

| 边界 | 说明 |
|---|---|
| 领域接口表 | 已实现 `musicxx.hooks` / `musicxx.host` / `musicxx.ui` 三张表；`musicxx.player` / `library` / `lyrics` / `storage` / `net` / `stats` 的 IID 已冻结但**表体未实现**（查询返回 NULL）→ 这些能力统一走 `requestAction("musicxx.player.play", ...)` 等动作名，由应用侧分派 |
| 进程隔离 | 插件与宿主同进程（无沙箱）：插件崩溃 = 应用崩溃，权限只是声明 |
| 平台 | Windows / Linux / macOS / Android 可以加载动态库插件（Android 受限，见 §11）；iOS / OHOS 只允许 JS 插件 |
| 资源限制 | 宿主不限制插件的内存 / 耗时 / 网络，只做自我保护：等待预算、动作超时、事件队列上限、连续失败后暂停该处理器；统计只观测不限制 |
| 多实例 | 同一份库文件可以被创建多个实例（不同 id / 参数），因此**不要有可变全局状态** |
| 钩子埋点 | 契约里有 66 个钩子，应用侧当前已经埋点的是 `plugin-hooks.md` 里标「已埋点」的那 9 个；其余钩子注册成功但不会触发 |
