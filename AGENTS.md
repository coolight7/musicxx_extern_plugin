# musicxx 外部插件框架 —— Agent 指南

写给 AI Agent 与自动化脚本：**这是什么 / 插件怎么写 / 改这个包要注意什么**。
更细的说明在 `README.md` 与 `docs/`；本文件是浓缩与索引，冲突时以 `docs/` 和源码为准。

## 1. musicxx 是什么（宿主）

| 项 | 说明 |
|---|---|
| 产品 | **musicxx**（中文名"拟声"）：多端音乐 / 媒体播放器，主界面 Flutter，播放内核 libmpv |
| 能播放 | 本地文件、Bilibili、阿里云盘、115、百度云盘、WebDAV、Subsonic、Jellyfin、Emby、自建云；音频 / 视频 / 歌词 / 逐字歌词 / 频谱 / 主题 / 悬浮歌词 |
| 还能做 | 局域网控制与"一起听"、投屏、本机 HTTP / WebSocket 服务、MCP（给 AI 客户端）、命令行 `musicxx-cli` |
| 平台 | Windows / Linux / macOS / Android / iOS / OHOS |
| 开源情况 | **应用本体不开源**。插件作者与 AI 能看到的只有本包 —— 所以插件会遇到的行为、限制、排障都必须写进本包 `docs/` |

宿主怎么用插件：

```text
用户安装插件（管理页装 .zip / 直接把目录放进插件目录 / musicxx-cli plugin install）
  → 应用启动时加载宿主库 → 扫描插件目录 → 装载"用户已启用"的插件
  → 运行期：派发钩子、执行插件的动作请求、同步读状态镜像、渲染插件声明的界面与着色器
```

- **宿主是可选能力**：宿主库缺失或初始化失败时应用照常启动，只是没有外部插件；用户没开『拟声++』或处于安全模式时同样不加载插件。
- **动态库插件与宿主同进程**（能崩掉整个应用）；权限只是**声明与展示**，不是沙箱。
- 应用侧边界：插件崩溃 / 连续两次启动没走完 → 应用自动进入**安全模式**（本次不加载任何外部插件），用户可在管理页手动恢复。

## 2. 先选插件形态

| | JS 脚本插件（推荐先试） | C++ 动态库插件 |
|---|---|---|
| 编译 | 不需要（一个目录两个文件） | 需要工具链（MSVC ≥ 19.4x / GCC ≥ 14 / NDK ≥ 18，C++26） |
| 平台 | 全部平台（含 iOS / OHOS） | Windows / Linux / macOS / Android，且 iOS / OHOS 不可用 |
| 执行线程 | 宿主的一条共享 JS 线程（多插件共用一个线程） | 宿主线程（自己能起线程） |
| 处理器返回 | 可同步返回，也可返回 Promise（宿主等到结算） | 同步返回 |
| 适合 | 几乎所有功能：钩子、动作、界面、背景、变量 | 需要自己起线程、用第三方 C++ 库、重计算、直接调本地库 |

两者**共用同一套约定**：钩子、动作、状态镜像、UI 项、插件页面、渲染槽位。学会一套即可迁移。

## 3. 插件长什么样

### 3.1 JS 插件（零编译）

```text
my_plugin/
├── plugin.yaml                 # 清单（必填）
├── plugin.js                   # 脚本（缺省入口）
├── pluginxx_ui_kit.js          # 可选：随插件分发的界面 kit（用了就必须写进 scripts）
├── shader/                     # 可选：随插件分发的资源（如 shader bundle）
└── config.json                 # 可选：插件自己的配置（运行时读写）
```

```yaml
name: my_plugin                  # 插件 id（唯一；同名 = 升级覆盖）
kind: js
entry: plugin.js                 # 可省略（清单只要求 name）
scripts: [pluginxx_ui_kit.js, plugin.js]   # 可选：同一 JS 上下文里按顺序执行
version: 1.0.0
api_version: 1                   # 不低于宿主支持的最低版本（当前 1）即可；声明更高也不拒绝
platforms: [windows, linux, macos, android, ios]
permissions: [musicxx.ui, musicxx.storage]   # 声明式：只做展示，运行时不校验
```

```js
musicxx.hooks.register("musicxx.song.changed", { mode: "observe" }, (ctx) => {
    musicxx.host.log(2, "正在播放: " + ctx.song.name);
});

musicxx.capability.register("card", (args) => ({
    view: { blocks: [{ kind: "Text", text: "你好" }] },
}));
```

### 3.2 动态库插件

```text
my_plugin/
├── plugin.yaml
├── my_plugin.so               # Windows 是 .dll、macOS 是 .dylib
├── shader/                    # 可选资源（与分支无关，放包目录里）
└── config.json
```

- 清单里 `entry` **统一按 Linux 写 `<名>.so`**：宿主在 Windows / macOS 上会修正扩展名（一份目录三平台通用）；
  文件不存在时宿主还会按平台默认库名再找一次（`lib<name>.so` / `<name>.dll`）。
- 入口符号固定：`musicxx_plugin_start` / `musicxx_plugin_stop`（缺任一个装载阶段就拒绝，可读原因）。
- 最小骨架与完整 API 照抄 `plugins/example_native/`（含 SDK 用法、钩子、能力、事件订阅、声明式 UI）。

### 3.3 多目标包（一份包放多个系统 / 架构，仅动态库插件）

```text
my_plugin/
├── plugin.yaml                # 清单只有一份，entry 仍是"分支里的库文件名"
└── lib/                       # 分支目录（缺省 lib；清单 targets_dir 可改名，空串 = 关闭分支扫描）
    ├── windows-x64/my_plugin.dll
    ├── linux-x64/my_plugin.so
    ├── android-arm64-v8a/my_plugin.so
    └── universal/my_plugin.so        # 通用分支
```

标签别名很宽（`x86_64`/`amd64`、`aarch64`/`arm64-v8a`、`armeabi-v7a`、`win32`、`osx`/`darwin` 都认），
不认识的目录（如 `lib/utils/`）会被忽略；选哪个分支完全由宿主决定，插件不用自己判断。
**JS 插件不需要这套**：脚本本身跨平台，`targets_dir` 对它没有作用。

### 3.4 安装与更新

- 压缩包安装：`.zip` 的**顶层就是插件目录内容**（`plugin.yaml` 在根）；同名 = 覆盖升级，失败会回滚。
- 也可以把目录直接放进 `<应用支持目录>/musicxx/extern_plugin/plugins/<id>/`，再在管理页「重新扫描」（默认不启用）。
- 插件 id = 清单 `name` = 目录名；插件私有数据（`config.json` 与存储）默认随卸载一起删，命令行可用 `--keep-data` 保留。

## 4. 能做什么（能力地图）

| 能力 | 入口 | 要点 |
|---|---|---|
| 钩子 | `musicxx.hooks.register/unregister` | 观察型 / 裁决型；**只有"已接入"的钩子会触发**（见 §5.5）；`musicxx.hooks.handlers(id)` 读"这个钩子现在有谁在提供、排第几" |
| 动作（调应用） | `musicxx.call(...)` + 分类封装 `musicxx.player/library/lyrics/storage/net/render/media/ui` | 播放、界面提示、跳页面、渲染切换、封面取色……失败会返回原因 |
| 状态镜像 | JS `musicxx.state.get(key)` / C++ `stateJson` | 同步只读快照（播放状态、当前歌曲、播放列表、频谱、渲染槽位……），最快的读法 |
| 变量 | `musicxx.vars.get/set/bind/watch/peek` | 有属主、可读可写可订阅的小值；官方键 `musicxx.*`，插件键 `plugin.<id>.*` |
| 事件 | `musicxx.events.subscribe/publish/unsubscribe` | 插件之间、插件与页面之间的小消息（官方主题如 `musicxx.state.changed`） |
| 能力 | `musicxx.capability.register/call` | 自己的功能也能被别人调用；跨插件调用 JS→JS 直调、JS→原生投递（默认 3 s 超时） |
| UI 项 | `musicxx.ui.registerEntry/updateEntry` | 主页入口 / 歌曲菜单 / 歌单菜单 / 播放页背景 / 播放页歌曲图 |
| 插件页面 | `ext://<插件id>/<视图id>` | 插件自己画的功能页与设置页（能力返回 `{view: {...}}`） |
| 渲染槽位 | UI 项 `playing.background` / `playing.icon` | 交 shader bundle（或界面视图）接管播放页背景 / 歌曲图；背景项可以用 `foregroundColors` 一起指定播放页前景三色（普通色 / 减淡色 / block 背景色，见 `docs/plugin-shader-bundle.md` §8.1） |
| 封面与图片 | 渲染项 `cover` / `image` + 动作 `musicxx.media.cover/bindImage` | 封面可当纹理采样或只取 4 个颜色；插件自己的图可绑成纹理 |
| 音频频谱与副歌 | 读：动作 `musicxx.media.spectrum` / 镜像 `musicxx.state.spectrum` / shader 来源 `musicxx.spectrum.*`；**提供实现**：钩子 `musicxx.media.spectrum.extract` / `musicxx.media.chorus.analyse` | 每帧 256 频点 + 响度（10 帧/秒），自动做帧间过渡；注册提取/分析实现可以替代内置实现（装了『音乐动效』时是它），两边都没有则这个功能整体不可用。**外置实现永远排在内置与应用自身之前**；返回 `{"disable": true}` 表示"这次不要用别的实现"（应用自身的兜底也不再被问，处理器摘除后自动还原） |
| 功能埋点自省 | 动作 `musicxx.feature.point.list`、`musicxx.hook.handlers` | 功能埋点 = 应用留出的扩展点（频谱 / 副歌 / 封面取色 / 播放链路裁决 / 应用事件）；清单给出每个埋点的各层实现（外置 / 内置 / 应用自身）、优先级、谁生效、被谁置空、**能不能被主动调用与怎么调**（`callable` / `callDoc`）。命令行 `featurePoint.list`、MCP `GetFeatureProviders` 读同一份数据 |
| 功能点（声明 / 实现 / 调用） | `musicxx.feature.define/provide/callPoint/unprovide`（JS）、`featurePointDefine/Provide/Unprovide/Call`（native） | **主动调应用留出的功能点**（整曲频谱 `musicxx.media.spectrum.extract`、副歌 `musicxx.media.chorus.analyse`、封面取色 `musicxx.media.palette.provide`），拿到原始数据；也可以**声明自己的功能点**并给出实现，让应用与别的插件按 id 来调。调用零副作用（不写应用的值仓、不改应用状态）。实现走一条固定通道 `musicxx.feature.point.call`（不要为每个点造新钩子） |
| 存储与配置 | `musicxx.storage.get/set`（`getConfig`/`setConfig` 针对插件目录的 `config.json`） | **默认值由插件自己给**；`config.json` 用户也能直接编辑 |
| 网络 | `musicxx.net.fetch/download` | 走应用网络栈（继承代理/超时）；响应体流式读取，上限 16 MiB（默认 2 MiB）；**非 2xx 也算请求完成** |
| 定时器与工具 | `musicxx.timer.*`、`musicxx.util.*` | 定时任务、字符串/编码等小工具 |
| 界面提示 | `musicxx.ui.notify/toast/dialog/openRoute`、`musicxx.ui.setFontFamily` | 通知、弹窗、跳官方页面或插件页面、换全局字体 |
| 统计 | `musicxx.stats.getSelf/reportMemory/reportMetric` | 只观测，不影响调度 |

## 5. 硬约束（动手前读一遍，坑都在这里）

1. **处理器里绝不等宿主动作。** 打开插件页面、点按钮、部分钩子（同步派发）都是"应用线程同步进宿主"调用你的代码；
   你在处理器里 `await musicxx.call(...)` 会与调用方互相等，表现是**整个应用卡住几秒或一直卡**（内核侧连超时都没有）。
   正确做法：要"现在这一帧"的数据读状态镜像；要动作结果就**异步读**、把结果记在自己的状态里，由能力（页面）下一次调用时回读
   （样板：`plugins/example_js` 的 `crossCall`、`example_js_shader` 的 `spectrumProbe`、`example_js_font` 的按钮）。
2. **JS 顶层必须同步完成注册**，位置放 `await` 会让装载失败并回滚。
3. **回调要尽快返回**：所有 JS 插件共用一条 JS 线程；长时间同步计算或死循环会卡住其它插件，
   宿主**不会打断脚本**，只能重启应用。启动阶段的钩子（`app.start`）不要做重活。
4. **裁决型钩子返回裁决对象**；需要 `await` 才能决定时，先同步判断"这次要不要裁决"，只在需要的那次返回 Promise
   （宿主一直等到它结算）。返回 `null` / `undefined` = 不裁决。裁决只是建议，调用点会校验身份字段（如 `sid`），过期裁决会被丢弃。
5. **钩子还没接入就不会触发**：约定 71 个钩子，应用侧当前真正调用的只有 13 个
   （`docs/plugin-hooks.md` 的「应用是否已接入」列）；别把功能押在未接入的钩子上。
   两个预留的观察钩子（`musicxx.media.wave.ready` / `musicxx.media.chorus.analysed`）已标**废弃**：
   注册了也不会收到事件，替代做法写在它们的说明里。
6. **界面规则**：块只画自己（没有隐含边距，留白靠 `padding`/`margin`/kit）；
   加粗要显式写 `bold: true` 或 `type: "title"`；行内的开关 / 勾选框放在**定宽 `SizedBox`** 里，"几选一"用一排 `Button`；
   行尾的长文本要自己约束宽度（否则窄窗口会撑破整行）；按钮文案要短（字不换行）；
   **图标只能是插件自己带的 svg**（相对插件目录的路径），写宿主图标名等于没有图标。
7. **着色器**：只用浮点版内置函数（整型 `clamp/min/max/abs` 在 GLES 后端编不过）；
   不要用运行时下标取 uniform（小表用几条比较挑，大表用循环下标整组读，逐像素挑 64 个成员实测多花 90 ms 以上）；
   渲染目标是左上角原点（`gl_FragCoord.y` 向下增大），写反了画面上下颠倒；
   改完用 `shader/build_bundle.ps1` 重新编译（bundle 带 `format_version`，一份 bundle 全平台通用）。
8. **渲染槽位默认不生效**：用户要在设置里选中才渲染，没被选中时框架零成本；运行期出错会回退内置样式（连续失败会停用候选）。
   声明了 `cover` 才采样对应 `sampler2D`（名字两边要一致）；没有封面时宿主绑 1×1 透明纹理，用 `uCoverInfo.w` 判断。
9. **存储**：默认值写在插件里（`getConfig(key, 默认值)`）；读到旧值不要覆盖用户刚改的；
   改了影响界面/渲染的字段要用 `musicxx.ui.updateEntry` 重新声明（整体替换，不是合并）；
   影响帧调度或分辨率的字段（`maxFps` / `resolutionScale` / `animate`）要用户重新选中该项才完全生效。
10. **动作和跨插件调用都可能失败**：检查返回值 / 捕获 Promise 拒绝，把原因写进日志；
    所有插件命名空间（UI 项名、变量名、能力名）都归 `plugin.<插件id>.*`，不要冒充别人。
11. **失败是可见的**：脚本顶层报错 = 装载失败并回滚；处理器里抛异常只记统计（不暂停、不卸载）；
    不要在启动阶段调用未接入的能力或做重活。
12. **权限只是声明**：写进 `permissions` 只影响安装确认弹窗与详情页展示，框架不做运行时校验（放目录里的插件同样不检查）。
13. **验证用真机/真宿主**：宿主不限制内存 / 网络 / 数量，只做自我保护（事件队列上限、动作超时）；有问题先看日志。

## 6. 开发循环与命令

### 6.1 准备：构建宿主库与 SDK

```powershell
git submodule update --init --recursive       # 首次
pwsh -NoProfile -File tools/setup_native_env.ps1            # 配置开发编译环境（子模块/Boost/NDK → 缓存，可先跳过）
pwsh -NoProfile -File tools/build_native.ps1                # 插件框架：Release 宿主库 + 插件 SDK
pwsh -NoProfile -File tools/build_examples.ps1              # 示例插件（构建 + 打包成 .zip）
pwsh -NoProfile -File tools/build_native.ps1 -Config Debug  # 独立构建目录
pwsh -NoProfile -File tools/build_native.ps1 -DepsOnly      # 只构建依赖库（调试依赖链用，不产出 SDK 安装前缀）
pwsh -NoProfile -File tools/build_native.ps1 -RunTests      # 构建后跑原生测试（隐含示例与测试）
pwsh -NoProfile -File tools/build_examples.ps1 -RunTests    # 同上，从示例插件那条线走
```

- **应用构建会自动触发**：`flutter build windows|linux|apk` 会在配置阶段调用 `build_native.ps1` 的自动模式
  （已是最新则秒级返回；关掉用环境变量 `MUSICXX_EXTERN_PLUGIN_AUTO_BUILD=0`），因此改完宿主 C++ 直接
  `flutter run` 就会重新构建，不需要先手工跑脚本。
- **分三条线**：`setup_native_env.ps1` = 配环境（写 `.native/env/environment.json` 缓存，重复执行只读缓存）；
  `build_native.ps1` = 插件框架（宿主库 + SDK，示例插件默认不建）；`build_examples.ps1` = 示例插件（构建 + 打包）。

```bash
./tools/build_native.sh --run-tests                # Linux / macOS（同一套流程）
./tools/build_native.sh --android --abi arm64-v8a  # Android 交叉编译（-Android -Abi 同义）
```

- 安装前缀 = `<构建目录>/musicxx-extern-plugin-install`（SDK 的 `include/`、`lib/cmake/`、`bin/` 都在这里），
  另有一份稳定复制在 `.native/output/<平台>-<架构>-<配置>/`。
- 插件工程用 `find_package(musicxx_extern_plugin CONFIG REQUIRED)` + `musicxx_plugin_add_target(...)`，
  照抄 `plugins/example_native/CMakeLists.txt` 即可。

### 6.2 改完代码怎么看到效果

| 改了什么 | 怎么做 |
|---|---|
| JS 脚本 / 页面 / 随插件分发的文件 | 直接改插件目录里的文件，然后 `musicxx-cli plugin reload <插件id>`（脚本会重新执行） |
| 动态库插件 | 重新构建 → 新库文件覆盖进插件目录 → `musicxx-cli plugin reload <插件id>` |
| 已打包的 `.zip` | 再装一次（覆盖安装），或解开覆盖目录后重载 |
| 只有启动时才读的框架设置 | `musicxx-cli plugin restart` |

### 6.3 命令行 / MCP（开发期最省事的通道）

```bash
musicxx-cli plugin list                       # 框架状态 + 插件清单（含失败原因）
musicxx-cli plugin install "D:/out/my_plugin.zip" --enable   # 路径必须绝对路径
musicxx-cli plugin logs my_plugin --limit 50  # 最近的插件日志（新的在前）
musicxx-cli plugin reload my_plugin           # 只重载这一个（不给 id = 重扫 + 重载全部）
musicxx-cli plugin enable my_plugin             # disable / uninstall / restart / clear-logs 同理
musicxx-cli doctor                            # 接入点 / 连接 / 版本 / 命令数自检
```

给脚本和 agent 加 `--json`：stdout 只有 JSON、提示都走 stderr；`musicxx-cli help plugin` 看用法。
MCP 客户端用 `musicxx-cli mcp`（stdio ↔ 本机 `/mcp` 的桥），工具名与命令一一对应。
完整命令表与排查见 `docs/plugin-agent-cli.md`。

### 6.4 测试与检查（在包内跑）

```powershell
flutter pub get
pwsh -NoProfile -File tools/build_examples.ps1   # 先构建示例插件与原生测试（测试需要它们；已是最新则只重新打包）
dart run tools/gen_contract.dart --check   # 钩子约定生成物一致（改动后必须重新生成）
dart run ffigen --config ffigen.yaml       # 改过 C ABI 后重新生成 Dart 绑定
flutter analyze                            # 期望 0 issue
flutter test                               # 真实原生库上的整套流程（找不到库/示例插件时跳过）
dart run tools/self_check.dart             # 纯 Dart 自检（定位 FFI 卡住的位置）
```

### 6.5 打包一个插件

```powershell
pwsh -NoProfile -File tools/pack_plugin.ps1 -PluginDir D:/work/my_plugin   # 产物 .zip（顶层就是插件目录内容）
pwsh -NoProfile -File tools/build_examples.ps1 -PackOut D:/out              # 构建示例插件并把它们全部打包
```

## 7. 改这个包时（框架 / SDK 自身）

- **约定只在一处**：钩子 id / 模式 / 合并策略 / 派发方式定义在 `tools/hooks.def.json`，
  由 `dart run tools/gen_contract.dart` 生成 Dart 枚举、C++ 头与作者文档 `docs/plugin-hooks.md`；
  改完必须重新生成并提交生成物（`--check` 可校验）。
- **生成物不要手改**：`lib/src/bindings_generated.dart`、`lib/src/hook_ids.g.dart`、
  `src/sdk/include/musicxx/plugin/api/hook_ids.g.h`、`docs/plugin-hooks.md`、`docs/musicxx-ui-kit.md`、
  `js/musicxx_ui_kit.js`，以及界面描述层子模块生成的头文件 / Dart / JS。它们都能由工具重跑。
- **分支选择只在一处**：多目标包选哪个分支由宿主实现（`src/host/host_inspect.cpp` + `host_target.*` + `host_manifest.*`），
  Dart 侧只保留模型与环境取值（`lib/src/plugin_target.dart` / `plugin_inspect.dart`），安装预检也走宿主的 `plugin_inspect`；
  **不要在 Dart 侧复制这套判定**。
- **界面组件只在一处**：组件 / 字段 / 上限定义在子模块 `src/third_party/cxx_pluginxx_ui/schema/ui.def.json`，
  在该子模块里跑 `dart run tools/gen_ui.dart` 生成 C++ / Dart / JS 绑定、文档与样例；两端手写模型与解析器要同步改。
  扩展 kit 定义在 `schema/musicxx-ui-kit.def.json`（`tools/gen_ui_kit.ps1` 生成、`tools/sync_ui_kit.ps1` 复制进插件目录）。
- **宿主库导出面**只允许 `musicxx_extern_plugin_*`（MSVC 导出宏 / GNU version script / Apple 隐藏可见性）；
  改导出、C ABI 或线程收尾后重新构建并跑原生测试。**改 C 头文件后要重新生成 Dart 绑定**：
  `fvm dart run ffigen --config ffigen.yaml`（Windows 上 LLVM 路径在配置里）。
  新增查询型 ABI 时，Dart 侧要按"**探测式查找**"接：旧宿主库里没有这个符号，直接查会抛，
  应在 `try/catch` 里探测一次并缓存结果，缺失时降级（`musicxx_extern_plugin_hook_handlers`
  就是这么接的：旧库只报处理器数量）。
- **Android**：宿主库必须随 APK 分发（`jniLibs`，系统只允许从 APK 原生库目录加载，放数据目录会报
  `is not accessible for the namespace "classloader-namespace"`）；交叉编译固定 Ninja + `c++_static`；
  改宿主生命周期 / 析构 / 加解锁后要在**设备上**跑一次原生测试（bionic 的 FORTIFY 会把"加锁已销毁的互斥量"变成 SIGABRT）。
- **示例插件**：`plugins/` 下每个子目录一个插件，**目录名 = 插件 id = 清单 `name`**；
  演示示例一律 `example_` 开头；故意做坏的对照示例（`*_fail` / `*_bad_entry` / `*_broken`）用来演示宿主的保护机制，
  随 `MUSICXX_EXTERN_PLUGIN_BUILD_TESTS` 构建；新增动态库插件要在 `src/host/CMakeLists.txt` 的插件列表登记。
  构建与打包走 `tools/build_examples.ps1`（插件框架本身默认不构建它们，见下一条）。
- **文档纪律**：任何"插件作者会遇到的行为、限制、排查方法"都必须写进本包（应用不开源，写在应用侧等于没写）；
  改 API / UI 项 / 钩子 / 构建方式时，同步更新 `docs/`、`README.md` 的索引表与本文件。
- **构建结构**：`src/CMakeLists.txt` 是 superbuild（`XX_IS_*` 宏 + `ExternalProject_Add` 构建 `src/third_party` 依赖），
  `src/host/CMakeLists.txt` 是嵌套宿主工程（`find_package` 取依赖）；依赖全部从子模块源码构建（不用预编译库），
  升级子模块后核对选项名与目标名。
- **三个构建脚本的分工**（Windows 侧）：`tools/setup_native_env.ps1` 配环境并写缓存
  `.native/env/environment.json`；`tools/build_native.ps1` 构建宿主库 + SDK（`-Auto` 供应用构建自动调用、
  已是最新则秒退）；`tools/build_examples.ps1` 构建示例插件并可打包。公共实现在 `tools/native_common.ps1`。
  平台工程（`windows/CMakeLists.txt`、`linux/CMakeLists.txt`、`android/build.gradle`）通过
  `tools/cmake/host_autobuild.cmake` 在应用构建时触发 `build_native` 的自动模式；改这套触发方式要同时看这三处。

## 8. 排障速查

| 现象 | 先看 / 怎么做 |
|---|---|
| 装不上：「插件框架未启动，无法安装插件」 | 用户没开『拟声++』、处于安全模式，或宿主库不可用（`plugin list` 会显示框架状态） |
| 装载失败：「API 版本过低」 | 清单 `api_version` 低于宿主支持的最低版本（当前 1；声明更高不会被拒） |
| 「动态库插件库文件缺失」/「JS 插件缺少脚本文件」 | `entry` 写错（动态库统一写 `<名>.so`）、分支目录缺当前平台，或脚本文件没跟着分发 |
| 点入口提示「插件『x』没有提供『y』」 | 动作指向的能力没注册，或页面视图 id 与能力短名不一致 |
| 「插件没有提供任何内容块」 | 能力返回的视图没有 `blocks`，或块类型名全拼错（未识别的块会被忽略） |
| 应用卡住几秒 / 一直卡 | 处理器里在等宿主动作或做了长时间同步计算；应用日志目录下 `host_call.log` 的**最后一行**就是没返回的那次调用 |
| 改完没生效 | JS 需要 `plugin reload`；宿主库被正在运行的应用占用时复制的是旧文件；影响帧调度的字段要重新选中该项 |
| 改了宿主 C++ 但应用里行为没变 | 应用构建会自动重建宿主库（`flutter build` 日志里有"宿主库就绪"一行）；没生效时先看它是不是跳过了（"已是最新"判定看的是 `src/`、`plugins/` 的改动时间与构建标记），必要时 `tools/build_native.ps1 -Force` |
| 选中了插件样式但画面没变 / 黑屏 | 用户要先在设置里选中；检查日志里的渲染失败；没有封面时 `uCoverInfo.w` 是 0 |
| 着色器报渲染失败 | 检查整型内置函数、运行时下标取 uniform、采样步长（`res / (texSize * 缩放)`）与 sampler 名字是否和渲染项一致 |
| 应用进了安全模式 | 说明上次启动没走完（插件崩溃 / 死循环）；在管理页恢复，并修掉触发问题 |
| 跨插件调用失败 | `plugin_capability_not_found` / `capability_call_failed` / 超时（JS→原生默认 3 s，可传超时或 0 = 一直等） |

## 9. 文档索引

| 要看什么 | 文件 |
|---|---|
| 第一次写插件（选形态 / 环境 / 第一个插件 / 注意事项 / 调试 / 发布检查） | `docs/plugin-guide.md` |
| JS 插件作者指南（`musicxx` API、生命周期、硬约束、排障） | `docs/plugin-js-api.md` |
| 动态库插件作者指南（清单 / SDK / 线程 / 构建 / 部署） | `docs/plugin-native-api.md` |
| 钩子总表（id / 模式 / 派发 / 是否已接入 + 载荷与裁决） | `docs/plugin-hooks.md`（生成物） |
| 官方变量目录（键 / 能力位 / 取值 / 风险） | `docs/plugin-vars.md` |
| 界面（UI 项 / 插件页面 / 自己的设置页 / 布局约定） | `docs/plugin-ui.md` |
| shader bundle（打包 / uniform / 参数表达式 / 渲染槽位） | `docs/plugin-shader-bundle.md` |
| 命令行与 MCP（装 / 刷新 / 看日志） | `docs/plugin-agent-cli.md` |
| 界面 kit 的组件与参数 | `docs/musicxx-ui-kit.md`（生成物） |
| 本包怎么构建、宿主库怎么随应用分发、Dart 侧怎么接 | `README.md` |
| Android 打包与平台限制 | `android/README.md` |
| 示例与对照示例清单 | `plugins/README.md` |
