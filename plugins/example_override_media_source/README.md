# example_override_media_source（插件音源解析示例）

演示**插件参与音源解析**：主程序要把一首歌的音源换成"播放器内核能打开的地址 + 请求头"时，
会先问钩子 `musicxx.player.source.resolve`（外置插件 → 内置插件 → 应用自身，第一个给出合法结果的生效）。
这个插件在该钩子上注册一个处理器，按载荷里的来源类型分流。

当前实现的功能：

| # | 来源 | 音源字符串写什么 | 插件返回什么 |
|---|---|---|---|
| 1 | 内置 Bili（`type = "bili"`） | 歌曲实体里是 BV 号（钩子载荷的 `crudeSrc`） | B 站音频直链 + `Referer/Origin/User-Agent` |
| 2 | `plugin.<插件 id>.plugin_bili`（`type = "link"`） | BV 号，例 `...plugin_bili://BV1GJ411x7h7` | 同一份 B 站直链 + 请求头 |

后续提交会加：

| # | 来源 | 音源字符串写什么 | 插件返回什么 |
|---|---|---|---|
| 3 | `plugin.<插件 id>.plugin_proxy_local` | 本地文件路径 | 插件框架本地 HTTP 文件服务的地址（流式读文件） |

## 文件与开关

```text
example_override_media_source/
├── plugin.yaml          # 清单（scripts 里按顺序加载两个界面 kit 与本脚本）
├── plugin.js            # 全部逻辑
├── pluginxx_ui_kit.js   # 随插件分发的界面 kit（tools/sync_ui_kit.ps1 复制）
├── musicxx_ui_kit.js    # musicxx 扩展 kit（switchRow 在它里面）
├── resource/svg/        # 主页入口图标
└── config.json          # 用户改过的设置（插件自己写；没有就是默认值）
```

设置项（`config.json`，设置页里是开关）：

| 键 | 默认 | 关掉之后 |
|---|---|---|
| `overrideBili` | `true` | 不接管内置 Bili 解析，主程序原有逻辑照旧 |
| `pluginBiliEnabled` | `true` | 自定义音源 `plugin_bili` 不接管（用这个来源的歌曲会解析失败） |

设置页：主页入口「音源解析示例」→ `ext://example_override_media_source/settings`。
页面上还能看到被问到的次数、成功次数与最近一次错误。

## 功能 1：覆盖内置 Bili 解析

主程序在问这条钩子之前，已经把 Bili 歌曲的 BV 号换成了临时直链（`MediaPlayer_c.parseMediaSrc`），
所以钩子载荷里：

- `type = "bili"`；
- `src` = 已经换好的 `https://...m4s?...` 临时直链；
- `crudeSrc` = 歌曲里保存的 **BV 号**；
- `quality` = 用户音质偏好（默认 `upper`）。

插件做的事（`plugin.js` 的 `resolveBili`）：

1. 从 `crudeSrc` 取 BV 号（取不到再看 `src`）；
2. `GET https://api.bilibili.com/x/web-interface/view?bvid=...` 拿 `cid` 与 `pages`；
3. `GET https://api.bilibili.com/x/player/playurl?...&fnval=16&platform=pc` 拿 `dash.audio`；
4. 按用户音质偏好选一路（30216 / 30232 / 30280 / 30250 / 30251），回答
   `{patch: {src: {path: 直链, headers: {Referer, Origin, User-Agent}, kind: "url", ...}}}`。

### 两个必须知道的限制

- **多 P 视频不接管**：钩子载荷里没有分 P 序号（`biliPIndex` 存在歌曲实体的 `appInfo1` 里，
  载荷只给只读视图，不带它）。插件看到 `pages.length > 1` 时直接返回 null，让主程序已经算好的
  那一份继续生效 —— 否则第 2 P 会被播成第 1 P。
- **匿名解析**：`musicxx.net.fetch` 不带主程序的 B 站登录 Cookie，所以只能拿到匿名档位
  （一般最高 192k）。要大会员档位就得自己带 Cookie（例如让用户填在设置里），这超出示例范围。

另外：

- B 站音频 CDN 要求 `Referer`，且对 `HEAD` 请求返回 404 —— 播放器与主程序缓存流发的都是
  `GET + Range`，所以正常；
- 同一个 BV 的 preload / 播放 / 封面 / 媒体信息会连着问好几次，插件按 `BV + 音质` 缓存 10 分钟
  （直链自身还有 `deadline`，快过期时也会重新解析）。

### 手动验证

```bash
# 用统一入口播放一首 B 站歌曲（BV 号就是歌曲的音源）
musicxx-cli call musicxx.player.playSrc --args '{"src":"BV1GJ411x7h7","type":"bili"}'
```

播放时看插件日志（管理页 → 插件 → 日志，或 `musicxx-cli plugin logs example_override_media_source`）；
关掉开关后再播同一首歌，插件处理器返回 null，主程序内置解析照旧。

## 功能 2：自定义音源类型 `plugin_bili`

插件可以在**不改主程序来源枚举**的前提下声明自己的音源类型：地址写成
`plugin.<插件 id>.<类型名>://<插件自己的标识>`，按普通链接（`XXSrcType_e.UrlLink`）保存。
播放到这种地址时主程序不会自己猜（核心层拒绝解析 `plugin.` 开头的地址），而是问同一条钩子：

- `type = "link"`；
- `src` = 完整地址 `plugin.example_override_media_source.plugin_bili://BV1GJ411x7h7`；
- `pluginType` = 类型 id `plugin.example_override_media_source.plugin_bili`；
- `crudeSrc` = 歌曲里保存的原地址（这里与 `src` 相同）。

`<插件自己的标识>` 怎么解释完全由插件决定，本示例把它当 BV 号（`opaqueOf()` 取 `://` 之后的部分）。

声明（`declareSourceTypes()`，脚本加载时跑一次）：

```js
musicxx.call("musicxx.source.define", {
    id: "plugin.example_override_media_source.plugin_bili",
    title: "B 站（插件解析）",
    depict: "地址里写 BV 号，插件自己查 B 站接口换播放地址与请求头",
    srcDoc: "BV1GJ411x7h7",     // 给用户与自省看的地址写法示例
});
```

- 声明的用处是**自省与更清楚的报错**：能不能播看的是有没有注册 `musicxx.player.source.resolve`
  的处理器，不是有没有声明；
- 声明跟着插件生命周期走（停用 / 卸载 / 关『拟声++』自动撤销），不写磁盘，每次启用都会重新声明；
- 查清单：`musicxx-cli call musicxx.source.list`、MCP `ListMediaSources`、插件动作 `musicxx.source.list`；
- 只写标识（`BV1GJ411x7h7`）也存得下（主程序会补前缀），但自己拼全更清楚。

手动验证：

```bash
musicxx-cli call musicxx.player.playSrc --args '{"src":"plugin.example_override_media_source.plugin_bili://BV1GJ411x7h7","type":"link"}'
```

## 开发与调试

```bash
# 同步界面 kit（改了 kit 或第一次建目录后）
pwsh -NoProfile -File tools/sync_ui_kit.ps1 -Plugin example_override_media_source
# 构建示例插件（框架有改动时会一起重建）
pwsh -NoProfile -File tools/build_examples.ps1
# 装进正在运行的应用并重载
musicxx-cli plugin install "<zip 绝对路径>" --enable
musicxx-cli plugin reload example_override_media_source
musicxx-cli plugin logs example_override_media_source --limit 50
```

## 写作参考

| 主题 | 文档 |
|---|---|
| 插件总入口（选形态 / 第一个插件 / 注意事项） | 包内 `docs/plugin-guide.md` |
| 钩子总表与 `musicxx.player.source.resolve` 的载荷 / 裁决 | 包内 `docs/plugin-hooks.md` |
| 音源解析链、插件音源地址写法、自省命令 | 包内 `docs/plugin-api.md` §3.2.6 / §8.6 |
| JS 插件写法（顶层注册、处理器不能等动作） | 包内 `docs/plugin-js-api.md` |
