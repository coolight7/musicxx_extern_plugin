# 版本对照：musicxx ↔ musicxx_extern_plugin

主程序的每个版本对应本包的一份代码状态。这张表记录「某个 musicxx 版本用的是哪个插件框架版本、
哪个提交、哪一天」，用来回答：

- 用户装的 musicxx 是 0.87.3，插件框架有哪些能力？（拿版本号去 [CHANGELOG.md](../CHANGELOG.md) 对）
- 这个插件错误发生在哪个插件框架版本上？（同上，再按提交看改动）
- 主程序管理页「外部插件框架」区块显示的就是这里的版本号、提交与日期，按钮直接打开对应提交。

## 版本号规则

格式 `<年>.<月日>.<小版本号>`，例 `2026.1011.1` = 2026 年 10 月 11 日的第 1 版：

| 段 | 位数 | 含义 |
|---|---|---|
| 年 | 4 | 发版那天的年份 |
| 月日 | 4 | 发版那天的月份与日期（`MMDD`，1—9 月补 0；例 1 月 5 日写作 `0105`） |
| 小版本号 | 不限 | 同一天里的第几次版本变更，从 1 开始 |

- **一处声明**：Dart 侧是 `lib/src/version.dart` 的 `MusicxxPluginVersion`，
  C 侧是 `src/include/musicxx_extern_plugin_api.h` 的 `MUSICXX_EXTERN_PLUGIN_VERSION`，
  `pubspec.yaml` 的 `version` 也跟着走；三处必须一致（`flutter test test/version_test.dart` 校验）。
- **插件清单用同一套格式**：`plugin.yaml` 的 `version` 例 `2026.1010.1`，打包脚本把它带进安装包名
  （见 `plugins/README.md`）。
- **与版本号无关的三个版本**：
  - 插件清单的 `api_version` 与插件 SDK 的 `MUSICXX_EXTERN_PLUGIN_MIN_API_VERSION` 是**插件 ABI 代次**
    （当前 7），只有破坏性改动时才 +1；低于它的插件在加载阶段被拒；
    宿主构建时用 `MUSICXX_EXTERN_PLUGIN_MIN_API_VERSION` 覆盖 cxx_pluginxx 的 `PLUGINXX_API_VERSION`
    与 `PLUGINXX_MIN_API_VERSION`（两个值相同）；
  - C ABI 的 `MUSICXX_EXTERN_PLUGIN_API_VERSION`（当前 5）是主程序 ↔ 插件框架库那层 `musicxx_extern_plugin_*`
    函数与结构体的版本，同样只在破坏性改动时 +1；
  - 每个形态另有自己的**接口代次对**（清单 `api_version` 按所属形态的下限判）：内置 `native` 跟随插件 ABI
    （当前 7/7），内置 `js` 当前 3/3（详见 `docs/plugin-native-api.md` §4.3）。
- 插件读到的版本号：JS 是 `musicxx.version`，动态库插件是 `host.info().hostVersion`
  （与 `musicxx_extern_plugin_version()` 同一个值）。

## 对照表

「日期」是该插件框架**提交**的日期。提交列给的是这一版对应代码状态的提交：

- **0.87.4 起（有版本号声明）**：该**版本号首次出现的提交** —— 应用里 `MusicxxPluginVersion.commit`
  显示的就是它，管理页的「查看该版本的 GitHub 提交」按钮跳的也是它。补记提交号的那一次提交属于
  同一个版本号（两次提交的差别只有 `commit` 字段）。
- **更早的版本**：那时插件框架还没有自己的版本号，给的是 musicxx 仓库里**子模块指针**记下的提交
  （即那一版发出去时代码状态指向的地方），版本号按同一规则用提交日期回填。

GitHub 上打开：`https://github.com/coolight7/musicxx_extern_plugin/commit/<提交号>`。

| musicxx 版本 | 插件框架版本 | 插件框架提交 | 日期 | 说明 |
|---|---|---|---|---|
| 未发布 | 2026.1108.1 | 5280bf4 | 2026-11-08 | 能力与广播只剩一份声明：删掉 `tools/hooks.def.json` 与它的两份生成物，派发层直接读 `capabilities.def.json`；插件 ABI 4 → 5、C ABI 2 → 3；C 接口 `musicxx_extern_plugin_hook_*` → `_capability_*`、`MusicxxPluginHookSpec` → `MusicxxPluginCapabilitySpec`（加 `phase`，删 v1 字段）；SDK `hook*` → `implement*`；JS / Dart 同步改名（`runtime.capabilities`、`MusicxxCapabilityCatalogue`）；预设表只做常量 / 文档 / SDK 快捷函数（删掉 `dynamicBroadcasts`，任何广播 id 都能用） |
| 2026.1106.1 | 2026.1106.1 | （待补记） | 2026-11-06 | 新增同步歌单音源的动作 `musicxx.playlist.syncSongs.sync`（传 `pid` 就把歌单绑定音源里新增的歌加进歌单）；JS 糖 `musicxx.playlist.syncSongs`，本机 CLI 糖命令 `sync-list`，MCP 工具 `SyncPlaylistSources`；外部触发的同步在应用信息栏里显示提示（成功 30 秒后自动移除、失败保留） |
| 未发布 | 2026.1105.1 | （待补记） | 2026-11-05 | 歌词动作迁到单数域 `musicxx.lyric.*`：`current.get` / `current.update` / `current.updateContent` / `getBySrc` / `search`；旧 `musicxx.lyrics.*` 与从未实现的 `sync` 删除（不保留别名）；行格式折叠译文、`timeMs` 改为毫秒；C 侧预留 IID 改成 `MUSICXX_PLUGIN_IFACE_LYRIC` |
| 未发布 | 2026.1104.2 | （待补记） | 2026-11-04 | 歌单与播放列表的写入动作：`musicxx.playlist.create` / `removeSong` / `updateSong`、`musicxx.songlist.addSong` / `removeSong` / `updateSong`；`querySongs` 新增 `detail`；权限名拆成 `musicxx.playlist.*` / `musicxx.songlist.*`（旧 `musicxx.library.*` 仍认得）；C 侧接口表 IID 与状态键同步改名 |
| 未发布 | 2026.1104.1 | （待补记） | 2026-11-04 | 动作分类拆分：`musicxx.library.*` 里的“读歌单 / 读歌曲 / 播歌”拆成 `musicxx.playlist.*`（歌单）与 `musicxx.songlist.*`（播放列表）；旧名字保留并标为过时（仍可使用），从未实现的那几条也换了名字（仍未实现）；JS 新增 `musicxx.playlist` / `musicxx.songlist`，`musicxx.library` 保留 |
| 未发布 | 2026.1031.1 | 8314706 | 2026-10-31 | 一个音源类型可以有多个浏览分类：`musicxx.source.browse` 可重复调用（`id` 相同、`view` 不同），每个分类独立的标题 / 图标 / 顺序 / 根目录 / 页大小 / 排序方式 / 索引栅 / 读写能力；`drive.*` 入参恒带 `type` 与 `view`；`source.undefine` 多一个 `view`；`pickSongs` / `pickDir` / `browseDrive` 多一个 `view` 并在返回里回报最终分类；`source.list` 的 `browse` 改为按类型分组（带 `entries`）；分类不进音源身份 |
| 未发布 | 2026.1026.1 | （待补记） | 2026-10-26 | 依赖库 `cxx_*` 升级到「作用域引用 + 业务语义不进通用表」的一代：通用表 `pluginxx.events` / `capabilities` / `config` / `cancel` 升到 v3（订阅与调能力要带一个可为空的作用域引用，`config` 去掉工具提示词/会话工作目录，`cancel` 只剩 `op_cancel`），插件 ABI 契约代次 1 → 3，旧插件要按新头文件重编；`PLUGINXX_API_VERSION` 由构建侧覆盖（`src/CMakeLists.txt` 默认 3），插件构建由 SDK 自动带上；插件形态（kind）改为注册表（新表 `musicxx.kind`：插件可登记自己的运行器，并可选声明 `validateCapability` 由它判定“这个包能不能装 / 能不能加载”），接口代次按形态分开（`host_kind.h`）：内置 `native` 3/3（C 接口代次）、内置 `js` 2/1（JS 接口只增不改，下限仍 1），插件读到的 `host.info().apiVersion` / `config().apiVersion` 按自己的形态报 |
| 未发布 | 2026.1025.1 | （待补记） | 2026-10-25 | 静态内容并到 HTTP 服务：`http.serve` 的 `static` / `staticOnly` 与新增 op `http.route` / `http.unroute`（命中后由框架直接回 200/206/416，不进 `http.request`）；删掉单独的 JS 本地文件服务（`musicxx.net.serveFile/serveBytes/serverInfo/stopServer`，从未发布） |
| 未发布 | 2026.1024.3 | （待补记） | 2026-10-24 | 插件网络能力支持客户端 TLS（静态链接 OpenSSL：`https` / `wss`，含证书链与主机名校验、`tlsVerify`/`caFile`/`caPem`/`serverName`；服务端仍只有明文）；新增 op `http.fetch` / `http.download`，JS 的 `musicxx.net.fetch` / `download` 改用它实现 |
| 未发布 | 2026.1024.2 | （待补记） | 2026-10-24 | 新增插件网络能力 `musicxx.net`（HTTP / TCP / WebSocket 的客户端与服务端，JS 与动态库插件共用）；新增示例 `example_js_net` |
| 未发布 | 2026.1024.1 | （待补记） | 2026-10-24 | 新增歌词加载钩子 `musicxx.lyric.load` 与五个细分阶段（`musicxx_server` / `cache` / `builtin` / `builtout` / `match`），主程序把原来的歌词加载改成功能埋点；钩子总数 66 |
| 未发布 | 2026.1023.1 | （待补记） | 2026-10-23 | 钩子阶段机制：一个调用 = 前链 + 实现链 + 后链（后链逆序）；删除 8 条从未派发的 id、合并 4 对成一条钩子的两个阶段；钩子总数 60 |
| 未发布 | 2026.1019.1 | 36e4b85 | 2026-10-19 | 裁决型钩子里发起的动作请求不再等到超时（回答不再绕框架线程）；新增 HTTP 服务的静态内容路由（当时是单独的 JS 本地文件服务，一个版本后并到 `http.serve`）；新增示例插件 `example_override_media_source`（Bili 播放地址覆盖 / 插件音源类型 / 本地文件 HTTP 代理） |
| 0.87.4 | 2026.1011.1 | f0a4a51 | 2026-10-11 | 开始声明版本号；主程序管理页显示版本 / 提交 / 日期并可跳 GitHub；示例插件清单改用同一套格式；接口表判定改为「最低版本 + 基线字节数」（依赖库 `cxx_*` 更新，见 CHANGELOG） |
| 0.87.3 | 2026.1007.1 | ac4cb52 | 2026-10-07 | 版本号按提交日期回填（当时未声明）：示例封面背景、播放页前景色、注册频谱/副歌实现 |
| 0.87.2 | 2026.1004.1 | 6077ffd | 2026-10-04 | 版本号按提交日期回填：界面描述层、插件页面动画与值表达式、musicxx-cli 说明 |
| 0.87.1 | 2026.0930.1 | 540ef23 | 2026-09-30 | 版本号按提交日期回填：多目标包安装、变量系统、能力导出 |
| 0.87.0 | 2026.0926.1 | 3b61b6a | 2026-09-26 | 版本号按提交日期回填：插件框架首次随主程序发布（Dart 接入层、JS 运行时、管理页、安全模式） |

musicxx 的一个版本期间可能更新过好几次插件框架，表里只记最后一个（0.87.4 起为版本号声明提交 `f0a4a51`，紧接的补记提交 `35577a5` 属于同一个版本号），中间的提交按日期查本包 `git log`。

## 维护这张表

### 1. 发布 musicxx 新版本时加一行

先在主程序仓库（mymusic）里看这个 musicxx 版本最后一次更新插件框架指针的提交：

```bash
git log --oneline -1 <该 musicxx 版本的提交> -- package_extend/musicxx_extern_plugin
```

**0.87.4 起（插件框架有版本号声明）**：插件框架版本填该提交里的 `MusicxxPluginVersion.version`，
提交填**声明这个版本号的那次提交**（见上一节的规则），日期填它自己的日期：

```bash
git -C package_extend/musicxx_extern_plugin show -s --date=short --pretty='%h %ad %s' <插件框架提交>
```

**更早的版本（当初回填的规则）**：提交取子模块指针记下的那个提交：

```bash
git rev-parse <那个提交>:package_extend/musicxx_extern_plugin
```

把「插件框架版本 / 提交 / 日期」填进表里，`说明` 写这次插件框架改了什么（一两句即可，
细节在 `CHANGELOG.md`）。

### 2. 改插件框架版本号（两个提交）

提交号写不进它自己所在的提交，所以是两步：

1. **改版本号并提交**：`lib/src/version.dart` 的 `version` 与 `date`、
   `src/include/musicxx_extern_plugin_api.h` 的 `MUSICXX_EXTERN_PLUGIN_VERSION`、
   `pubspec.yaml` 的 `version`，`CHANGELOG.md` 记这次改动，本表加一行（提交列先留空），
   `MusicxxPluginVersion.commit` 先留空；提交这一次改动。
2. **补记提交号并提交**：把第 1 步的提交号（短号即可，7 位以上）填进
   `MusicxxPluginVersion.commit` 与本表的提交列，再提交一次。

第 1 步的提交就是"这个版本号的提交"：表里和应用里显示的提交号都指向它（它里面 `commit` 字段
还是空的，这是正常的 —— 见上面两句）。
