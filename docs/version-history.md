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
- **与版本号无关的两个版本**：插件清单的 `api_version` 与 C ABI 的
  `MUSICXX_EXTERN_PLUGIN_API_VERSION`（当前 1）只在破坏性改动时才 +1。
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
| 未发布 | 2026.1023.1 | （待补记） | 2026-10-23 | 钩子阶段机制：一个调用 = 前链 + 实现链 + 后链（后链逆序）；删除 8 条从未派发的 id、合并 4 对成一条钩子的两个阶段；钩子总数 60 |
| 未发布 | 2026.1019.1 | 36e4b85 | 2026-10-19 | 裁决型钩子里发起的动作请求不再等到超时（回答不再绕框架线程）；新增本地 HTTP 文件服务（仅 JS）；新增示例插件 `example_override_media_source`（Bili 播放地址覆盖 / 插件音源类型 / 本地文件 HTTP 代理） |
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
