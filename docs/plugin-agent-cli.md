# 用命令行 / MCP 控制插件（开发期快捷通道）

> 这篇讲**不用点管理页**怎么把插件装上去、刷新、看日志、卸下来：一条命令（或一次 MCP 工具
> 调用）就是一次操作。命令由正在运行的主程序提供，`musicxx-cli` 只是它的客户端 —— 所以这里
> 做的事和管理页做的是同一批实现、同一份状态（装完在管理页也看得到）。
>
> 第一次写插件先看 [plugin-guide.md](plugin-guide.md)；写法看 [plugin-js-api.md](plugin-js-api.md) /
> [plugin-native-api.md](plugin-native-api.md)；播放页背景看 [plugin-shader-bundle.md](plugin-shader-bundle.md)。

## 1. 什么时候用它

| 场景 | 为什么合适 |
|---|---|
| AI agent 写完一个插件 | 打包 → 安装 → 启用 → 看日志 → 改完重载，全程不用人工点界面 |
| 反复调效果（尤其是插件页面与播放页背景） | 改完文件一条 `plugin reload` 就重新执行脚本；背景还能直接切到插件样式看效果 |
| 排障 | `plugin list` 看框架与每个插件的状态，`plugin logs` 看最近的事件与错误 |
| 收尾清理 | 跑完试验 `plugin uninstall` 卸掉，或 `--keep-data` 只删代码、留下配置数据 |

## 2. 先决条件

| 条件 | 说明 |
|---|---|
| 桌面端 | 命令行入口（`/cli`）只在 Windows / Linux / macOS 开放；Android / iOS 没有，继续用管理页 |
| 有 `musicxx-cli` | 正常构建/安装应用时会与主程序**编译到同一目录**（Windows `musicxx-cli.exe`；Linux / macOS `musicxx-cli`，macOS 在 `Musicxx.app/Contents/MacOS/`）。找不到时看应用里「设置 → 共享与控制 → 命令行接口（CLI）」，那里也写着当前路径 |
| 与主程序同一台机器 | 本机回环直接可用（不需要额外配置）；跨机器使用要开「共享与控制 → 允许远程控制本机」 |
| 主程序在运行 | 没在运行时命令以退出码 2 结束；加 `--start` 可以先拉起再执行，`musicxx-cli start` 拉起并等它就绪 |
| 『拟声++』已开启 | **安装 / 重载 / 重启**要求插件框架正在运行（见 `plugin list` 的 `hostStarted`）；`list` / `enable` / `disable` / `uninstall` / `logs` 没有宿主也能调用 —— 但**未开启『拟声++』时应用不会加载插件记录**，它们看到的是"没有插件" |

自检：`musicxx-cli doctor`（接入点文件、连接结果、命令数、主程序路径），
`musicxx-cli ping`（只看存活与就绪）。

## 3. 30 秒上手

```bash
musicxx-cli plugin list                                      # 装了哪些插件、框架在不在跑
musicxx-cli plugin install "D:/out/my_plugin.zip" --enable    # 安装并启用（路径必须绝对路径）
musicxx-cli plugin logs my_plugin --limit 50                 # 最近的插件日志（新的在前）
musicxx-cli plugin reload my_plugin                          # 改完文件重新加载（JS 脚本会重新执行）
musicxx-cli plugin uninstall my_plugin                       # 卸载（--keep-data 保留插件私有数据）
```

给脚本 / agent 加 `--json`：stdout 只有那份 JSON（`{ok, cmd, data|error}`，缩进多行），文本提示与
进度都在 stderr。单条命令的帮助用 `musicxx-cli help plugin`（任何命令都行）。

## 4. 命令一览

主命令是 `plugin`，子命令如下（`musicxx-cli help plugin` 会打印同一份用法）：

| 命令 | 作用 | 需要框架在跑 |
|---|---|---|
| `plugin list` | 框架状态 + 插件清单（启用/加载、声明的权限、目录、失败原因） | 否 |
| `plugin install <zip路径> [--enable]` | 按压缩包安装（同名 = 覆盖安装，失败回滚） | **是** |
| `plugin enable <插件id>` / `plugin disable <插件id>` | 启用 / 禁用 | 否（不在跑时只记录状态，下次启动生效） |
| `plugin reload [插件id]` | 给 id 只重载这一个；不给 id = 重扫插件目录 + 重载全部启用中的 | **是** |
| `plugin restart` | 重启插件框架（插件文件与安装记录都保留） | **是**（重启后仍未运行会失败，原因在错误里） |
| `plugin uninstall <插件id> [--keep-data]` | 卸载：停实例 → 删插件目录 → 清记录；`--keep-data` 保留插件私有数据 | 否 |
| `plugin logs [插件id] [--limit N]` | 最近日志（新的在前，1~200 条，默认 50） | 否 |
| `plugin clear-logs` | 清空日志缓冲（不影响插件行为） | 否 |

几个别名：`plugin remove` = `uninstall`，`plugin log` = `logs`，`plugin clearlogs` = `clear-logs`。

「需要框架在跑 = 否」的命令在宿主没起来时也能用；但**未开启『拟声++』时应用不会加载插件记录**
（`plugin list` 显示 0 个，按 id 启停/卸载会报「插件未安装」），先开启『拟声++』再操作。

每条子命令背后是一条**服务端命令**，可以用 `musicxx-cli call <命令 id>` 直调（`musicxx-cli list`
能列出全部命令与参数）：

| CLI | 服务端命令 id |
|---|---|
| `plugin list` | `externPlugin.list` |
| `plugin install` | `externPlugin.install` |
| `plugin enable/disable` | `externPlugin.setEnabled` |
| `plugin reload` | `externPlugin.reload` |
| `plugin restart` | `externPlugin.restart` |
| `plugin uninstall` | `externPlugin.uninstall` |
| `plugin logs` | `externPlugin.logs` |
| `plugin clear-logs` | `externPlugin.clearLogs` |

## 5. 常用做法

### 5.1 安装（打包 → 装 → 启用）

```bash
# 1) 打包：压缩包顶层就是插件目录的内容（plugin.yaml 在根）
pwsh -NoProfile -File tools/pack_plugin.ps1 -PluginDir D:/work/my_plugin

# 2) 安装并立刻启用；不给 --enable 时：新插件保持"未启用"，覆盖安装沿用原状态
musicxx-cli plugin install "D:/work/my_plugin.zip" --enable
```

- **路径必须是绝对路径**：agent/命令行的工作目录与应用不同，相对路径会被直接拒绝；
- **非交互**：不弹文件选择器、也不弹确认框。清单声明的权限、选中的平台分支、装到哪个目录
  都在结果里（`--json` 时是 `data` 的字段），调用方自己展示；
- 已存在同名插件时是**覆盖安装**：先备份旧目录，写入失败会回滚；结果里 `isUpgrade: true`；
- 装失败的常见原因（原因文本直接给在输出里）：
  - 「插件框架未启动，无法安装插件」——『拟声++』没开、处于安全模式或宿主库不可用；
  - 「安装包路径必须是绝对路径…」；
  - 压缩包不合格（没有 `plugin.yaml`、条目过多/过大、含非法路径或符号链接）；
  - 清单 `platforms` / `arch` 与当前系统不符（文案形如「当前平台不在清单声明内
    (当前 windows，清单声明: linux)」），或多目标包缺当前平台/架构的分支、库文件缺失。

### 5.2 改完代码怎么刷新

| 改了什么 | 怎么做 |
|---|---|
| JS 脚本 / 页面 / 随插件分发的文件 | 直接改插件目录里的文件，然后 `plugin reload <插件id>`（脚本会重新执行） |
| 动态库插件 | 重新构建 → 把新的库文件（与清单）覆盖进插件目录 → `plugin reload <插件id>` |
| 已打好的 .zip | 再 `plugin install` 一次（覆盖安装），或解开覆盖目录后重载 |
| 只有启动时才读取的框架设置 | `plugin restart`（插件与安装记录都保留） |

- **插件目录在哪**：`plugin list` 里每个插件的 `path` 字段就是它 —— 装完的插件就在这个目录里，
  改文件直接改这里，不用重新打包；
- 重载会**重新执行脚本顶层**，注册要写成幂等的（见 [plugin-js-api.md](plugin-js-api.md) §2）；
- 禁用中的插件不会被重载（`plugin reload <id>` 会明确告诉你"已被禁用，先启用"）；
- 重载只重读代码与清单，**不会动插件的私有数据**（`config.json` 与存储）。

### 5.3 看状态与日志

```bash
musicxx-cli plugin list                # 给人看：框架状态 + 逐条插件（启用/加载、形态、版本、权限、目录，错了带原因）
musicxx-cli plugin list --json         # 给 agent：data 里是完整状态，字段见下
musicxx-cli plugin logs --limit 20     # 全部插件的最近日志
musicxx-cli plugin logs my_plugin      # 只看一个插件
```

`plugin list` 的 `data` 里（重载 / 重启 / 启停 / 卸载的结果里也是同一份字段；安装结果里它在
`data.status` 下）：

| 字段 | 含义 |
|---|---|
| `hostStarted` | 插件框架（宿主）是否在运行 |
| `supportPlus` / `enabled` / `safeMode` / `autoSafeMode` / `lastError` | 『拟声++』开关、框架开关、安全模式、最近一次失败 |
| `pluginDir` | 插件目录（`path` 的父目录） |
| `pluginCount` / `loadedCount` | 已登记插件数 / 已加载数 |
| `plugins[]` | 每个插件：`id`、`version`、`kind`、`enabled`、`loaded`、`supported`、`reason`（不可用原因）、`target`（动态库插件选中的分支）、`path`（插件目录）、`permissions`（声明的权限）、`error`（最近一次失败原因） |

- 日志是**内存里的环形缓冲**（最多 200 条，新的在前，不落盘、重启应用就没了）：宿主事件、
  插件日志（`console.*` / `musicxx.host.log`）与错误都在里面，和管理页「调试」页签看的是同一份；
- **装载失败不会让安装失败**：安装结果只说明"文件装好了"；是否真的跑起来要看该插件的
  `loaded` 与 `error`，再去 `plugin logs <id>` 看明细。

### 5.4 停用与卸载

```bash
musicxx-cli plugin disable my_plugin            # 不加载了，文件与记录都还在
musicxx-cli plugin enable my_plugin             # 再启用（框架在跑时立刻装载）
musicxx-cli plugin uninstall my_plugin          # 删插件目录 + 清记录（私有数据一起删）
musicxx-cli plugin uninstall my_plugin --keep-data   # 保留 config.json 与存储，方便重装后继续用
```

### 5.5 让播放页用上插件背景（render）

插件注册的 `playing.background` 样式**默认不生效**，要显式选中（与设置页「播放页面背景」同一份状态）：

```bash
musicxx-cli render list                          # 谁能画：内置样式 + 各插件样式（含可用性）
musicxx-cli render current                       # 现在是谁在画
musicxx-cli render select plugin.my_plugin.bg    # 切到插件样式
musicxx-cli render select builtin:Auto           # 切回内置样式
```

选中的样式失效时（bundle 预检不过、运行期渲染失败、插件被停用/卸载、关闭『拟声++』）会自动
回退内置样式；细节见 [plugin-shader-bundle.md](plugin-shader-bundle.md) §10。

### 5.6 其它顺手命令

| 命令 | 插件开发时用来 |
|---|---|
| `musicxx-cli status` / `play` / `pause` / `next` | 快速把播放状态摆到你要的位置（触发钩子、看背景效果） |
| `musicxx-cli watch --events song,playerState` | 看应用事件有没有按预期发生（钩子与事件的对应见 [plugin-hooks.md](plugin-hooks.md)） |
| `musicxx-cli toast <文本>` | 在应用界面上弹一条提示（对照插件的 `musicxx.ui.toast`） |
| `musicxx-cli var list` / `var get <键>` | 看官方变量当前值（与插件的变量通道对照） |
| `musicxx-cli feature call GetAudioSpectrum --args '{"path":"D:/a.mp3"}'` | 单独取一份频谱/副歌数据，与插件里的读数对照（[plugin-shader-bundle.md](plugin-shader-bundle.md) §10） |

## 6. 给 agent 与脚本的约定

- **`--json`**：stdout 只放那份 JSON（缩进多行；形状 `{ok, cmd, data}` 或 `{ok:false, error}`），
  人类可读的提示与诊断信息一律走 stderr —— 解析 stdout 不会被污染；
- **退出码**：`0` 成功 / `1` 参数错误（CLI 自己发现的）/ `2` 应用没在运行 / `3` 等响应超时 /
  `4` 命令执行失败（原因在 stderr）/ `5` 协议不匹配（CLI 与主程序版本差太多）；
- 命令名是**自描述**的：`musicxx-cli list --json` 里有全部命令与参数，不要硬编码参数表；
- **这几条命令不在插件的动作表里**：插件里 `musicxx.call("externPlugin.install", …)` 只会得到
  "动作未注册"。装插件 = 执行任意代码，这条通道只留给本机命令行与 MCP（应用侧有意如此，
  不是遗漏）。

## 7. MCP（给只会 stdio 的 AI 客户端）

`musicxx-cli mcp` 把主程序 `/mcp` 的 MCP 工具桥到 **stdio**：客户端不用知道端口、接入点文件或
命令面，配一条命令就能用。

```bash
# Claude Code
claude mcp add musicxx -- "<安装目录>/musicxx-cli" mcp
```

```jsonc
// Claude Desktop / Cursor（Windows 路径里的反斜杠要写两次）
{
  "mcpServers": {
    "musicxx": {
      "command": "D:\\Program Files\\Musicxx\\musicxx-cli.exe",
      "args": ["mcp"]
    }
  }
}
```

插件相关的 8 个工具（名字、参数、说明对外就是契约）：

| MCP 工具 | 参数 | 等价命令 |
|---|---|---|
| `GetExternPlugins` | 无 | `plugin list` |
| `InstallExternPlugin` | `path`（绝对路径）、`enable?` | `plugin install` |
| `SetExternPluginEnabled` | `id`、`enabled` | `plugin enable` / `disable` |
| `ReloadExternPlugins` | `id?` | `plugin reload` |
| `RestartExternPluginHost` | 无 | `plugin restart` |
| `UninstallExternPlugin` | `id`、`keepData?` | `plugin uninstall` |
| `GetExternPluginLogs` | `id?`、`limit?`（1~200，默认 50） | `plugin logs` |
| `ClearExternPluginLogs` | 无 | `plugin clear-logs` |

- 工具结果里都带一份**现在的插件清单**（装了什么、启用/加载状态、失败原因），一次调用就能确认
  结果，不用再问一次；
- 拟声没在运行时桥**不会退出**：请求返回可读的 JSON-RPC 错误（`-32000`），默认会自动拉起主程序
  （`--no-start` 关掉），应用起来后下一次调用自动可用；
- stdout 只放协议消息，日志都在 stderr；排查用 `musicxx-cli mcp --verbose`。

## 8. 一个完整的开发循环

```bash
# 0) 确认连得上（第一次用可以顺手拉起应用）
musicxx-cli doctor

# 1) 打包（JS 与动态库插件同一套；顶层就是插件目录内容）
pwsh -NoProfile -File tools/pack_plugin.ps1 -PluginDir D:/work/my_plugin

# 2) 安装并启用
musicxx-cli plugin install "D:/work/my_plugin.zip" --enable

# 3) 确认真的跑起来了，有问题就看日志
musicxx-cli plugin list
musicxx-cli plugin logs my_plugin --limit 50

# 4) 改完代码刷新（JS：直接改插件目录里的文件；动态库：重新构建后覆盖进插件目录）
musicxx-cli plugin reload my_plugin

# 5) 背景类插件：切上去看效果
musicxx-cli render select plugin.my_plugin.bg

# 6) 收尾
musicxx-cli plugin uninstall my_plugin
```

## 9. 边界与注意

- **装插件等于执行任意代码**：动态库插件与主程序同进程运行；命令行安装不做权限校验、也不弹
  确认框（声明只在结果里列出）。只装可信来源；agent 应当把声明的权限先展示给用户再装。
- **卸载默认不可逆**：`plugin uninstall` 会删掉插件目录，默认连私有数据（`config.json` 与存储）
  一起删；要留就加 `--keep-data`。
- **需要框架在跑的三条**：`install` / `reload` / `restart`；其余命令在宿主没起来时也能用
  （启用/禁用只记录状态，下次启动生效），但未开启『拟声++』时应用不加载插件记录 —— 先开启再操作。
- **命令行是桌面端能力**：Android / iOS 用管理页。Android 上动态库插件可能装载失败的平台限制
  与这里无关，见 [plugin-native-api.md](plugin-native-api.md) §11。
- **日志有上限**：最近 200 条、只在内存里；要长期保留就自己落盘。
- **一次只让一个 agent 装/卸同一个插件**：两台同时在装会出现"文件是这次、记录是那次"的竞争。
- **以运行状态为准**：安装/启用命令成功 ≠ 插件跑起来了，最终看 `plugin list` 的 `loaded` 与
  该插件的 `error`，以及 `plugin logs`。

## 10. 排查

| 现象 | 原因 / 处理 |
|---|---|
| 退出码 2（"拟声未在运行"） | 加 `--start`，或先 `musicxx-cli start`；用 `musicxx-cli doctor` 看接入点文件与连接 |
| 安装失败「插件框架未启动，无法安装插件」 | 未开启『拟声++』、处于安全模式，或宿主库不可用 —— 看 `plugin list` 的 `hostStarted` / `supportPlus` / `safeMode` / `lastError` |
| 安装失败提到"必须绝对路径" | 把路径写成绝对路径（例如 `D:/out/my_plugin.zip`）；相对路径会被直接拒绝 |
| 安装失败提到「当前平台不在清单声明内」/「库文件缺失」 | 清单 `platforms` / `arch` / `entry` 写错，或多目标包少了当前平台/架构的分支（[plugin-native-api.md](plugin-native-api.md) §1.3） |
| 装了但没在跑 | 新插件装完默认不启用：`plugin enable <id>`，或重新安装时加 `--enable` |
| `loaded: false` 且 `error` 有内容 | 装载失败（脚本顶层报错、缺入口符号、依赖缺失…），`plugin logs <id>` 看明细 |
| 改了文件没变化 | 没重载、重载失败，或插件处于禁用；`plugin reload <id>` 后看结果里的 `failed` |
| 应用整体卡住 | 插件占住了宿主线程或 JS 线程：看应用日志目录下 `host_call.log` 的最后一行（[plugin-guide.md](plugin-guide.md) §5.2） |
| 找不到 `musicxx-cli` | 看应用里「设置 → 共享与控制 → 命令行接口（CLI）」，那里写着 CLI 文件路径与可复制的命令 |

---

相关文档：[plugin-guide.md](plugin-guide.md)（总入口）、[plugin-hooks.md](plugin-hooks.md)（钩子总表）、
[plugin-js-api.md](plugin-js-api.md) / [plugin-native-api.md](plugin-native-api.md)（写法）、
[plugin-ui.md](plugin-ui.md)（界面）、[plugin-shader-bundle.md](plugin-shader-bundle.md)（背景渲染）。
