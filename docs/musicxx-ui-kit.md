# 扩展 kit（musicxx）（v1）

> 本文件由 `tools/gen_ui.dart` 生成。定义来源：`schema/ui.def.json` / `schema/kit.def.json`；扩展 kit 定义：`schema/musicxx-ui-kit.def.json`。

纪律：① 只写数值单位 u（8 / 12 / 20 这类）；② 不引用客户端专属块；③ 不含逻辑（只装配）。
需要项目特有的间距口径时，由扩展 kit 覆盖同名组件实现。

| 组件 | 参数 | 说明 |
|---|---|---|
| `title` | `text`* | 标题行 |
| `hint` | `text`* | 次要说明行 |
| `text` | `text`* `tone`=normal `mono`=false | 正文行 |
| `badge` | `text`* `tone`=accent | 状态小标签 |
| `icon` | `name` `glyph` `size` `tone`=normal | 图标（目标不支持 Icon 时退化成 glyph 文本） |
| `gap` | `size`=gap | 竖直留白（缺省用客户端默认行距） |
| `divider` | — | 分隔线 |
| `button` | `label`* `variant`=secondary `icon` `disabled`=false `action` | 按钮 |
| `actionsRow` | `buttons`* | 一排等宽按钮（按钮列表里的每一项占一等份） |
| `card` | `title` `variant`=card `padding` `margin` `children` | 内容块（卡片） |
| `listRow` | `title`* `subtitle` `trailing` `action` | 卡片里的一行（覆盖基础 kit：musicxx 的卡片行留白更紧，副标题用说明样式） |
| `section` | `title`* `rows`* | 小节标题 + 若干行 |
| `kv` | `pairs`* `sep` | 键值块（键列按最长键自适应） |
| `table` | `columns`* `rows` `header`=true | 表格（未指定的列宽由客户端自动分配） |
| `tree` | `nodes`* `connector`=true | 层级列表 |
| `sparkline` | `data`* `height`=1 `glyphStyle`=block `showLast`=true `tone`=accent | 迷你趋势图 |
| `progressRow` | `label` `value`* `total`=100 `unit`=% | 一行进度（覆盖基础 kit：musicxx 默认按百分比显示数值） |
| `settingRow` | `title`* `depict` `value` `action` | 设置行：标题 + 说明 + 右侧当前值（整行可点，点开会话由插件给的动作处理） |
| `switchRow` | `id`* `title`* `depict` `value` `action` | 开关行：标题 + 说明 + 右侧开关（值变化即派发动作，参数里带 id 与当前值） |
| `inputRow` | `id`* `title`* `depict` `value` `multiline`=false `action` | 输入行：标题 + 说明 + 右侧单行/多行输入框（输入完成即派发动作） |
| `shaderBlock` | `bundle`* `args` `speed`=1 `maxFps` `animate`=true `resolutionScale`=1 | 插件着色器块（musicxx 专属组件；终端之类不认识它的客户端会跳过） |
| `coverRow` | `cover`* `title`* `subtitle` `trailing` `size`=40 `action` | 带封面的行：封面 + 标题 + 副标题 + 可选右侧文字（取不到图片时显示标题） |

按格换算的辅助函数（用客户端 `cell`，缺省取 `defaults.cell`）：

