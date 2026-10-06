# 插件开发指南（从选形态到发布）

> 这篇是插件的**总入口**：环境准备 → 第一个插件 → 能力地图 → 注意事项 → 调试 → 发布前检查。
> 每节都指向对应的详细文档，遇到具体写法就去看那一篇。

| 你要做的事 | 读这篇 |
|---|---|
| 写 JS 脚本插件（零编译） | [plugin-js-api.md](plugin-js-api.md) |
| 写 C++ 动态库插件（含多目标打包） | [plugin-native-api.md](plugin-native-api.md) |
| 查钩子 id / 模式 / 载荷 / 裁决 | [plugin-hooks.md](plugin-hooks.md)（生成物） |
| 读 / 改 / 订阅变量 | [plugin-vars.md](plugin-vars.md) |
| 加界面（主页入口、歌曲菜单、插件页面） | [plugin-ui.md](plugin-ui.md) |
| 写着色器（播放页背景 / 页面内联块） | [plugin-shader-bundle.md](plugin-shader-bundle.md) |
| **用命令行 / AI agent 装插件、刷新、看日志**（不点管理页） | [plugin-agent-cli.md](plugin-agent-cli.md) |
| 构建宿主库、把宿主库随应用分发 | 本包 `README.md` |

三句话理解这套框架：

1. **插件不写 Flutter 代码**：界面靠声明（UI 项 + 插件页面描述），着色器交给宿主渲染；
2. **两种形态**：JS 脚本（零编译、跨平台、能力处理器要同步返回）与 C++ 动态库
   （同进程、能自己起线程、iOS/OHOS 不可用）；两者共用同一套钩子、变量、动作与界面约定；
3. **宿主是可选功能**：宿主库缺失、插件写坏、用户关掉『拟声++』时，应用照常启动，只是没有外部插件。

---

## 1. 选形态

| 形态 | 目录内容 | 适合 | 代价 |
|---|---|---|---|
| **JS 插件** | `plugin.yaml` + `plugin.js`（+ 可选 kit / 资源） | 绝大多数需求：读状态、裁决、自绘页面、定时任务、走宿主网络 | 不能调系统 API，没有 `require`/`fs`；所有 JS 插件共用一条 JS 线程，回调必须快 |
| **C++ 动态库插件** | `plugin.yaml` + 库文件（+ 可选 `shader/`） | 需要自己起线程、要 CPU 密集计算、要链接自己的库 | 每个系统/架构各构建一次；iOS / OHOS 不能加载未签名动态库 |
| **C++ 多目标包** | `plugin.yaml` + `lib/<系统>-<架构>/库文件` | 一个包同时发布 Windows / Linux / macOS / Android | 打包步骤多一些（分支选择规则见 [plugin-native-api.md](plugin-native-api.md) §1.3） |

建议：**先用 JS 把想法跑通**（改完在管理页「重载」即可，不用编译），确实需要原生能力再换 C++；
两种形态的能力与钩子语义一致，迁移主要是改写法。

---

## 2. 环境准备

**只用 JS 插件**：不需要编译任何东西，也不需要本仓库 —— 装好应用、打开『拟声++』、
在「设置 → 插件 → 外部插件」确认入口可见即可。

**要写/调试动态库插件**（开发者）：

```text
① git submodule update --init --recursive      # 本仓库依赖全是子模块，不用预编译库
② pwsh -NoProfile -File tools/build_native.ps1 # Windows：构建依赖 + 宿主 + 示例插件（Linux/macOS 用 build_native.sh）
③ 产物：<构建目录>/musicxx-extern-plugin-install/
   ├── bin/     宿主库 + 原生测试
   └── plugins/ 示例插件（每个子目录一个插件，可直接拷进应用插件目录试用）
```

- 工具链要求（内核是 C++26）：MSVC ≥ 19.4x（VS 17.14+）、GCC ≥ 14、Clang/NDK ≥ 18；
  Linux 还需要 `pkg-config`；
- 构建脚本会自己准备 Boost 头文件（只用头文件，不链接 Boost 编译库）；
- 详细开关（`-Config Debug`、`-DepsOnly`、`-RunTests`、`-Jobs`、Android 交叉编译）见本包 `README.md`。

---

## 3. 第一个插件（15 分钟）

### 3.1 JS：一个目录两个文件

```text
my_plugin/
├── plugin.yaml    # 清单
└── plugin.js      # 脚本（顶层同步完成注册）
```

```yaml
name: my_plugin
kind: js
version: 1.0.0
api_version: 1
description: "我的第一个插件"
```

```js
// 观察型钩子：切歌时写一条日志
musicxx.hooks.register("musicxx.song.changed", { mode: "observe" }, function (ctx) {
    musicxx.host.log(2, "正在播放: " + ((ctx.song && ctx.song.name) || "未知"));
});

// 主页入口 → 打开插件自己的页面
musicxx.ui.registerEntry({
    name: "card",
    type: "home.entry",
    data: { title: "我的插件", action: { kind: "route", route: "ext://my_plugin/card" } },
});

// 页面内容：宿主调用同名能力取视图描述
musicxx.capability.register("card", function () {
    return { view: { title: "我的插件", blocks: [
        { kind: "Text", text: "你好，插件！" },
        { kind: "Button", title: "播放/暂停", style: "primary",
          action: { kind: "action", name: "musicxx.player.toggle" } },
    ] } };
});
```

### 3.2 C++：SDK + 一个 CMake 助手

```cpp
#include <musicxx/plugin/plugin_kit.h>
```

入口、钩子、能力的完整写法照抄 `plugins/example_native/example_native.cpp`（与 JS 版等价）；
构建只需要 SDK 与 `find_package(musicxx_extern_plugin CONFIG REQUIRED)`，
代码见 [plugin-native-api.md](plugin-native-api.md) §2 与 §9。

### 3.3 装上去

| 方式 | 做法 |
|---|---|
| 直接放目录（开发期最常用） | 把插件目录放进 `<应用支持目录>/musicxx/extern_plugin/plugins/<插件 id>/`，管理页有「打开插件目录」；「重新扫描」后在插件页启用 |
| 压缩包安装 | 打包成 `.zip`（**顶层就是插件目录内容**），管理页「从压缩包安装」 |
| 命令行 / AI agent | `musicxx-cli plugin install "<zip绝对路径>" --enable`（非交互、不弹确认框；之后用 `plugin list` / `plugin logs` / `plugin reload` 控制）。见 [plugin-agent-cli.md](plugin-agent-cli.md) |
| 随包插件 | 放进应用的随包插件目录，宿主启动时与用户目录一起扫描 |

改完代码：JS 插件点「重载」（脚本会重新执行一遍）；动态库插件重新构建后再「重载」。
用命令行时对应 `musicxx-cli plugin reload <插件id>`（插件目录见 `plugin list` 的 `path` 字段）。

---

## 4. 能力地图（能做什么、看哪篇）

| 能力 | 什么时候用 | 入口 | 详细 |
|---|---|---|---|
| **钩子** | 想被应用的事件调用（切歌、播放状态、解析音源、播放出错…），或要拦一下（跳过本曲、换源、停止播放） | `musicxx.hooks.register` / `PluginBase::hook` | [plugin-hooks.md](plugin-hooks.md) |
| **状态镜像** | 想同步读当前状态（播放中？当前歌曲？播放列表？频谱？）——这是最快的读法 | `musicxx.state.get` / `stateJson` | [plugin-js-api.md](plugin-js-api.md) §5 |
| **变量** | 读 / 改应用的设置项（官方键），或把自己的小状态暴露给别的插件与应用（可订阅变化） | `musicxx.vars.*` / `varsGet` / `varsSet` | [plugin-vars.md](plugin-vars.md) |
| **动作** | 要应用做一件事：播放控制、库查询、存储、网络、界面反馈、渲染槽位…（异步，返回结果） | `musicxx.call` / `requestAction` | [plugin-js-api.md](plugin-js-api.md) §7 |
| **事件** | 插件之间 / 插件与宿主之间广播小消息 | `musicxx.events.*` | [plugin-js-api.md](plugin-js-api.md) §8 |
| **能力（跨插件调用）** | 暴露自己的功能给别的插件，或调用别的插件的能力 | `musicxx.capability.*` | [plugin-js-api.md](plugin-js-api.md) §10 |
| **UI 项** | 主页入口、歌曲菜单、歌单菜单、播放页背景 | `musicxx.ui.registerEntry` | [plugin-ui.md](plugin-ui.md) §1 |
| **插件页面** | 插件的功能页 / 设置页（框架不提供设置表单，自己画） | `{ view: … }` + 路由 `ext://<插件id>/<视图id>` | [plugin-ui.md](plugin-ui.md) §2 |
| **着色器 / 渲染槽位** | 播放页背景、页面里画一块自己的着色器；当前歌曲封面（`cover`）与插件自己绑定的图片（`image`）都能当纹理采样 | UI 项 `playing.background` + shader bundle | [plugin-shader-bundle.md](plugin-shader-bundle.md) |
| **封面取色 / 封面变化** | 读 / 写封面取色（`palette` / `setPalette`），用钩子注册取色实现替代内置分析；也可以订阅封面变化（`musicxx.media.cover.changed`，可随时移除） | 动作 + 钩子 `musicxx.media.palette.provide` / `musicxx.media.cover.changed` | [plugin-js-api.md](plugin-js-api.md) §11 |
| **存储** | 插件私有 KV 与用户可见配置（`config.json`） | `musicxx.storage.*` | 本文 §5.6 |
| **网络** | 走应用统一网络栈取数据、下载文件（可作为便利通道） | `musicxx.net.fetch/download` | [plugin-js-api.md](plugin-js-api.md) §7.3 |
| **定时器 / 统计** | 后台任务；读自己的运行统计、自报指标 | `musicxx.timer.*` / `musicxx.stats.*` | [plugin-js-api.md](plugin-js-api.md) §13 |

---

## 5. 注意事项（写代码之前先看一遍）

### 5.1 进程与线程

- **动态库插件与宿主同进程**：没有沙箱，插件崩溃 = 应用崩溃；权限只是声明（安装弹窗与详情页展示），
  不拦截任何调用。
- **不要有可变全局状态**：同一份库文件可能被创建多个实例（不同 id / 参数），全局量会互相干扰。
- **JS 插件共用一条 JS 线程**：多个 JS 插件排队执行，处理器要尽快返回；死循环会一直占住这条线程
  （宿主不打断脚本、也没有中断开关），只能重启应用。
- **原生处理器的调用线程就是宿主线程**：网络、大文件、编解码、`sleep` 都不要直接在处理器里做，
  耗时工作放 `offload` + 回调（见 [plugin-native-api.md](plugin-native-api.md) §3）。

### 5.2 不要"等宿主"（最容易踩的坑）

打开插件页面、点按钮、调用能力，都是**同步进宿主**的（应用线程全程等宿主返回）。所以：

- 能力处理器里 `await` 一个宿主动作，就会与调用方互相等下去：表现为**整个应用卡住几秒**，
  最后以「调用插件能力失败，未在时限内完成」结束；
- 正确做法：当场要的数据同步读状态镜像（`musicxx.state.get`）；动作结果异步读回来，
  记进自己的状态，由页面下一次调用时回读（示例：`plugins/example_js` 的 `crossCall`、
  `plugins/example_js_shader` 的频谱读取）；
- 排查"应用卡住"：看应用日志目录下的 `host_call.log`（进宿主调用的同步轨迹，
  **最后一行就是没有返回的那次调用**）。

### 5.3 钩子

- **只有"已接入"的钩子会触发**：当前版本 9 个（`plugin-hooks.md` 的「应用是否已接入」列），
  其余注册成功但不会触发 —— 别把功能押在未接入的钩子上；
- **同一个钩子只注册一次**：用不同 `ownerTag` 注册同一个钩子，JS 侧只留一个处理器、
  宿主侧却留下多条注册，表现为处理器被重复调用；
- **裁决是"建议"**：调用点会校验裁决的身份字段（如 `sid`），过期的裁决会被丢掉；
  异步派发的钩子尤其要注意（等待期间可能已经切歌 / 切列表）；
- **处理器失败只记统计**：抛异常 / 返回失败不会暂停派发、也不会卸载插件；
- **异步裁决的 Promise 一定要结算**：宿主一直等到它结算（没有超时），不结算就是调用点一直等。

### 5.4 界面

- **页面是"取一次"的快照**：宿主打开页面时调用一次能力拿到视图；改完配置要让能力再返回一次
  `{view: …}`（或重新打开页面），界面才会更新；
- **入口/按钮指向的能力必须真的注册**：只声明入口不实现能力时，打开页面只会提示
  「插件『<插件id>』没有提供『xxx』」；
- **动作写法只有四种**：`route` / `dispatch` / `command` / `none`（`route` 允许跳到任意插件的
  `ext://<插件id>/<视图id>` 页面）；
- **组件要按客户端能力降级**：`host.info().ui` 是客户端如实上报的能力段 —— 它支持哪些块、控件、
  图标都在里面；用 `fallback` 或 kit 的变体处理不支持的情况（不要假设所有客户端都支持 `Image`、
  `Icon` 或 `musicxx.Shader`）；
- **块只画自己的职责**：文本就是一段文本、按钮就是一个按钮，没有隐含边距；要留白就自己写
  `margin` / `padding`（见 [plugin-ui.md](plugin-ui.md) §2.5 与 `docs/musicxx-ui-kit.md`）；
- **字重只来自 `bold: true` 或 `type: "title"`**，正文不要指望客户端的默认字重。

### 5.5 着色器

- **`format_version` 必须与应用使用的 Flutter 版本一致**（当前基线 3.47.5 → 2），否则 bundle 预检不过；
- **只用浮点版的内置函数**：源码先编成 SPIR-V，运行时再翻译一次（GLES 后端 → GLSL ES 1.00），
  那边 `clamp` / `min` / `max` / `abs` 只有浮点重载，对 `int` 用会编不过；
- **不要用运行时下标取 uniform**（GLSL ES 1.00 不允许）：小表用几条比较挑，大表用循环下标；
- **渲染目标是左上角原点的像素坐标**（`gl_FragCoord.y` 向下增大），上下不对称的效果要自己翻 y；
- **本地验证**：`impellerc --runtime-stage-gles --gles-language-version=100 …` 打出来的就是后端要编的那份源码；
- **动画速率由插件自己声明**（`speed`），宿主不做二次缩放；想给用户开关就做成自己设置页里的设置项，
  改完用 `musicxx.ui.updateEntry` 重新登记同一项即可生效；
- **渲染失败会自动回退**：预检不过 / 结构体不符 / 运行期异常 / 插件停用，都会退回内置背景；
  运行期失败最多重试 3 次（每次隔 2 秒），连续失败才停用该样式。

### 5.6 存储与配置

- 配置放插件目录里的 `config.json`，由插件自己读写（框架不渲染配置表单、也不列设置入口）；
- `musicxx.storage.get` / `getConfig` 的**应答就是值本身**（键不存在时是空应答，用你给的第 2 个参数当默认值）；
- 读配置是异步的：**读回来的旧值后到会覆盖用户刚改的值**，自己加"已经改过就不再覆盖"的保护；
- 路径类数据一律用宿主给的插件数据目录，不要写死位置。

### 5.7 网络

- `musicxx.net.fetch` / `download` 走应用统一网络栈（继承应用的超时与代理）；
- **非 2xx 也按请求完成返回**（看 `status` 自己判断）；响应体读到上限会断开并标 `truncated`；
  只有传输失败 / 超时才 `ok:false`；
- 下载只落在插件自己的数据目录里；宿主不限定可访问域名（这条通道是便利，不是限制）。

### 5.8 平台

| 平台 | JS 插件 | 动态库插件 |
|---|---|---|
| Windows / Linux / macOS | 可用 | 可用（随应用分发宿主库） |
| Android | 可用 | **受限**：用户安装的 `.so` 可能装载失败（系统只允许从 APK 的原生库目录加载），宿主按"安全降级"处理：标记不可用 + 给原因 + 不重试 |
| iOS / OHOS | 可用 | 不支持（只跑 JS 插件） |

- 清单 `entry` **按 Linux 写法填 `<名字>.so`**：Windows / macOS 的扩展名由宿主按平台修正，一份清单三平台通用；
- 多目标包的分支目录用 `lib/<系统>-<架构>/`，运行时按「系统+架构 → 只系统 → 只架构 → 通用 → 插件根目录」
  选分支（不认识的分支目录会被忽略）；
- 清单 `platforms` / `arch` 是**整包**的额外限制（不写 = 不限）：取值与分支标签共用同一张别名表
  （`win32` / `osx` / `gnu`、`amd64` / `aarch64` 都认）。它只在"当前运行环境能不能用"这一层比较，
  写错平台的表现是**装不上 / 扫描结果里 `supported=false`**：管理页与安装提示会写明
  「当前平台不在清单声明内 (当前 windows，清单声明: linux)」，照提示改清单即可；
- 每个架构各构建一次，库文件不能互换；
- **32 位 ARM（`armeabi-v7a` / armhf）**：宿主的架构标识报 `armv7`，分支目录按 `lib/android-armeabi-v7a/`
  写即可（`musicxx_plugin_add_target(... TARGET_TAG auto)` 在 Android 上直接用 ABI 名）；清单 `arch` 写
  `armv7` / `arm` / `armeabi-v7a` 都认。

### 5.9 失败与安全模式

- **装载失败会回滚**：脚本顶层报错、缺 `start`/`stop` 入口符号、清单声明的脚本文件缺失，
  都在装载阶段明确失败，不会留下半注册状态；
- **运行期失败不会卸载插件**：处理器/脚本报错只记日志与统计，插件继续工作；
- **安全模式**：应用启动结束前会写一个标记，正常走完才清除；连续两次启动没有走完（插件把应用拖住），
  应用会**自动进入安全模式 —— 本次运行不加载任何外部插件**。用户也可以在管理页手动开启 / 关闭；
- 写插件时把"启动路径上的工作"做轻：启动时不要做重活（大量 IO、同步等待、长时间循环）。

### 5.10 性能

- **高频数据用状态镜像**：镜像按需推送（播放状态按变化推、频谱约 10 Hz），`get` 是同步读，
  比反复发动作便宜得多；
- **声明式界面的动画是局部的**：只有放进 `musicxx.AnimatedBuilder` 的子树会逐帧重建，
  其它部分保持快照；动画等级低时连时间都不推进；
- **变量声明节流**：超过约 5 Hz 的值声明 `throttleMs`，否则宿主会记警告日志；
- **着色器 uniform 成员越多越慢**：实测 16 个频带成员约 5 ms 量级，64 个成员（256 个频点）会到
  90 ms 以上 —— 需要更细的频带就用 `bands64`，不要逐像素在 64 个成员里挑；
- 定时器精度是 ≤ 50 ms 轮询，不要用它做高精度计时；日志不要当数据通道用（高频写日志会拖慢应用）。

---

## 6. 调试

| 手段 | 看什么 |
|---|---|
| 管理页「插件」分页 | 扫描结果、启用 / 禁用 / 重载 / 卸载、插件详情（清单、声明的权限、注册项、日志、统计） |
| 命令行 / MCP | 同一批操作不用点界面：`musicxx-cli plugin list / install / enable / disable / reload / uninstall / restart / logs`，MCP 工具同名对应（见 [plugin-agent-cli.md](plugin-agent-cli.md)） |
| 管理页「调试」分页 | 钩子统计（调用次数 / 平均 / 最大 / 超时 / 失败）、每个插件的阶段耗时与计数、JS 实例统计与内存采样、最近事件、变量快照、界面描述层版本 |
| 管理页「设置」分页 | 框架开关（总开关、动态库 / JS 开关、安全模式、事件日志开关） |
| 插件日志 | `console.*` / `musicxx.host.log(...)` 与宿主日志；设 `MUSICXX_EXTERN_PLUGIN_LOG_STDERR=1` 可让宿主日志打到 stderr |
| `host_call.log` | 应用日志目录下的同步调用轨迹：应用卡住时最后一行就是没返回的那次调用 |
| 原生测试 | `pwsh -NoProfile -File tools/build_native.ps1 -RunTests`（用安装前缀的 `plugins/` 当插件目录） |
| 包内 Dart 测试 | `flutter test`（对真实宿主库做端到端验证；需要先构建原生库） |

**对照示例**（故意写坏的插件，演示宿主怎么保护自己）：`plugins/example_native_fail/`（处理器总是失败）、
`plugins/example_native_bad_entry/`（缺入口符号 → 拒绝装载）、`plugins/example_js_async/`（异步裁决）、
`plugins/example_js_broken/`（脚本语法错误 → 装载失败并回滚）。不要把它们装到日常使用的环境里。

---

## 7. 发布前检查清单

清单与打包：

- [ ] `plugin.yaml` 的 `name` = 插件 id = 目录名；`version` 已递增（同名视为同一插件的升级覆盖）；
- [ ] `api_version` 填当前框架支持的最低版本即可（声明更高不会被拒）；
- [ ] `platforms` / `arch` 只列真的支持的目标；`depends` / `optional_depends` 写对（缺失会拒绝装载 / 只影响顺序）；
- [ ] `permissions` 声明齐全（只做展示：安装弹窗与详情页会列给用户看）；
- [ ] 动态库插件：`entry` 按 Linux 写法填 `<名字>.so`；导出面只有 5 个入口符号
      （`dumpbin /exports` / `nm -D --defined-only` 自查）；
- [ ] 多目标包：每个分支都有库文件，打包时用 `pwsh tools/pack_plugin.ps1 -PluginDir <包目录>` 核对包内分支；
- [ ] JS 插件：`scripts` 列全（用 kit 就必须把 kit 文件写进去），压缩包**顶层就是插件目录内容**。

代码：

- [ ] 顶层同步完成注册（JS 不能有顶层 `await`）；注册写成**幂等**的（每次启用都会重新执行脚本）；
- [ ] 同一个钩子只注册一次；不需要时注销（或随插件停用自动摘除）；
- [ ] 没有长时间阻塞的处理器 / 脚本循环；耗时工作拆成回调、定时器或 `offload`；
- [ ] 没有可变全局状态（原生插件会被创建多个实例）；
- [ ] 能力处理器同步返回或返回一定会结算的 Promise；**没有在里面等宿主动作**；
- [ ] 界面用 `host.info().ui` 的能力段做降级；入口动作指向的能力真的注册了；视图里块类型名拼写正确
      （拼错会被静默忽略）；
- [ ] 设置页自己画在插件页面里，配置读写走 `config.json`（读到旧值不要覆盖用户刚改的值）；
- [ ] 着色器 bundle 用与应用相同的 Flutter 版本编出，`format_version` 对得上；
- [ ] 失败路径都有日志（应用整体卡住时，日志之外还能靠 `host_call.log` 定位）。

---

## 8. 相关文档

- [plugin-js-api.md](plugin-js-api.md) —— JS 插件 API 面、硬约束、常见坑（§15）、v1 边界（§16）
- [plugin-native-api.md](plugin-native-api.md) —— 动态库插件：清单、SDK、线程、构建、部署、排障
- [plugin-hooks.md](plugin-hooks.md) —— 钩子总表与已接入钩子的载荷 / 裁决语义（生成物）
- [plugin-vars.md](plugin-vars.md) —— 官方变量目录与读写规则
- [plugin-ui.md](plugin-ui.md) —— UI 项、插件页面组件、用 kit 装配、页面间跳转
- [plugin-shader-bundle.md](plugin-shader-bundle.md) —— bundle 打包、uniform 约定、参数表达式、排查
- [plugin-agent-cli.md](plugin-agent-cli.md) —— 用命令行 / MCP 装插件、刷新、看日志（给 AI agent 与自动化）
- [musicxx-ui-kit.md](musicxx-ui-kit.md) —— musicxx 扩展 kit 的组件与参数（生成物）
- 本包 `README.md` —— 构建宿主库、随应用打包、包内 Dart 接入、能力现状
- `plugins/` —— 官方示例与对照示例（每个子目录一个插件）
