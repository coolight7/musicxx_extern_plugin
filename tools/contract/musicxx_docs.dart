/// musicxx 的两份插件开发文档生成器（**项目描述层**）
///
/// 结构字段来自 `contract_core.dart`，这里只写"怎么讲给人听"：通道与阶段的中文说法、
/// 表格措辞、示例与注意事项。换项目时整份替换本文件，生成核心不用动。
library;

import 'contract_core.dart';

/// 通道的中文说法（多个用顿号连起来）
String _channelTextOf(CapabilityDeclaration decl) {
  final List<String> names = <String>[
    if (decl.cli) '命令',
    if (decl.mcp) 'MCP 工具',
    if (decl.plugin) '插件',
  ];
  return names.isEmpty ? '-' : names.join('、');
}

/// 阶段的中文说法（没有 impl 时写清"只能 before / after"）
String _phaseTextOf(CapabilityDeclaration decl) {
  if (!decl.openForImpl) {
    return '只能 ${decl.phases.join(" / ")}';
  }
  return decl.phases.join(' / ');
}
/* ==================== 插件开发文档 ==================== */

String generateCapabilityDoc(CapabilityContract contract) {
  final StringBuffer sb = StringBuffer()
    ..writeln('# 能力总表（插件开发文档）')
    ..writeln()
    ..writeln(
      '> 本文件由 `tools/gen_contract.dart` 从 `tools/capabilities.def.json` 生成，请勿手改。',
    )
    ..writeln()
    ..writeln(
      '> 两张表：**能力**（前链改输入 / 实现链给结果 / 后链改输出）就是本文里的这些，'
      '**广播**（只通知、不回答）在 [plugin-broadcasts.md](plugin-broadcasts.md)。'
      '写法（注册、阶段顺序、裁决对象）见 [plugin-api.md](plugin-api.md) 第 8 节。',
    )
    ..writeln()
    ..writeln('**能力** = 调用方发一次请求、等一个结果。同一个 id 只有一份声明，声明里写清：')
    ..writeln()
    ..writeln('| 字段 | 含义 |')
    ..writeln('|---|---|')
    ..writeln(
      '| `impls` | `one` = 一个槽位（后登记的覆盖先登记的，跨插件也覆盖）；'
      '`many` = 多实现共存，按 (层, priority, 登记顺序) 组成链 |',
    )
    ..writeln(
      '| `merge` | 多实现的结果怎么合并：`first` = 第一个合法结果生效；'
      '`veto` = 任一实现能否决 |',
    )
    ..writeln(
      '| `phases` | 能参与哪些阶段：`before`（正序改输入）/ `impl`（给结果）/ '
      '`after`（逆序改输出）。**不含 `impl` = 实现固定由应用提供，插件只能注册 `before` / `after`** |',
    )
    ..writeln(
      '| `dispatch` | 派发方式。**现在全是 `async`**：请求提交后由结果事件回话，'
      '处理器在插件自己的线程上跑，不占用调用线程 |',
    )
    ..writeln('| `timeoutMs` | 等实现的上限，超时按"没给"处理；`0` = 不设上限 |')
    ..writeln('| `cache` | `byIdentity` = 同一个请求身份不重复算；`none` = 每次都问实现 |')
    ..writeln(
      '| `cli` | 命令面能调（命令行 / `/cli` / 链接）。块内可写 `title`（命令标题）、'
      '`consent`（从链接调时先问用户的原因） |',
    )
    ..writeln(
      '| `mcp` | 进 MCP 工具表。块内可写 `name`（工具名，PascalCase）、`format`（`text` / `json`） |',
    )
    ..writeln('| `plugin` | 插件可以在 `musicxx.capability.call(id, args)` 里调 |')
    ..writeln()
    ..writeln('## 怎么读这张表')
    ..writeln()
    ..writeln(
      '- **通道**列就是"谁能调它"：`命令` = 命令行 / `/cli` / 链接；`MCP 工具` = 给模型的工具；'
      '`插件` = 插件能调；`-` = 只能应用内部调（纯实现 / 干预类，插件只能注册前后链）；',
    )
    ..writeln(
      '- **可注册阶段**列写清插件能参与哪一步；只写 `before / after` 的能力**不能**注册实现'
      '（注册会被拒并说明原因）；',
    )
    ..writeln('- **MCP 工具**列就是模型看到的工具名（`mcp.name`，不写时按 id 生成）；不做工具的能力写 `-`；')
    ..writeln(
      '- 注册时写全名：官方 `musicxx.*`；实现 / 声明自己的能力时写名字'
      '（宿主补成 `plugin.<本插件 id>.<名字>`，名字可以含点，例如 `drive.list`）。',
    )
    ..writeln()
    ..writeln('## 能力清单')
    ..writeln()
    ..writeln('| 能力 id | 通道 | 可注册阶段 | 合并 | 派发 | 超时 | 缓存 | MCP 工具 |')
    ..writeln('|---|---|---|---|---|---|---|---|');

  for (final CapabilityDeclaration decl in contract.capabilities) {
    final String timeout = decl.timeoutMs == 0 ? '-' : '${decl.timeoutMs} ms';
    final String cache = decl.cache == 'byIdentity' ? '按身份' : '-';
    final String tool = decl.mcp ? '`${decl.effectiveMcpName}`' : '-';
    sb.writeln(
      '| `${decl.id}` | ${_channelTextOf(decl)} | ${_phaseTextOf(decl)} | '
      '${decl.merge.isEmpty ? "-" : decl.merge} | ${decl.dispatch} | $timeout | '
      '$cache | $tool |',
    );
  }

  sb
    ..writeln()
    ..writeln('## 载荷与回答');

  for (final CapabilityDeclaration decl in contract.capabilities) {
    if (decl.doc.trim().isEmpty) {
      continue;
    }
    final String heading = decl.title.isEmpty ? '' : ' ${decl.title}';
    sb
      ..writeln()
      ..writeln('### `${decl.id}`$heading')
      ..writeln()
      ..writeln(decl.doc.trim());
  }
  return sb.toString();
}

String generateBroadcastDoc(CapabilityContract contract) {
  final StringBuffer sb = StringBuffer()
    ..writeln('# 广播总表（插件开发文档）')
    ..writeln()
    ..writeln(
      '> 本文件由 `tools/gen_contract.dart` 从 `tools/capabilities.def.json` 生成，请勿手改。',
    )
    ..writeln()
    ..writeln(
      '> 两张表：**广播频道**（只通知、不回答、不参与合并）就是本文里的这些，'
      '**能力**（能改输入、给结果、改输出）在 [plugin-capabilities.md](plugin-capabilities.md)。'
      '订阅 / 发布的写法见 [plugin-api.md](plugin-api.md) 第 8 节。',
    )
    ..writeln()
    ..writeln('**广播** = 所有者改了东西、通知所有关心的人。它没有返回值、不等待、允许丢中间值；')
    ..writeln('要不要"当前值"由声明决定：`cache: latest` 的频道订阅时就能拿到当前值，也能同步读；')
    ..writeln('`cache: none` 没有当前值，只能等下一次通知。')
    ..writeln()
    ..writeln('广播只做这几件事：**声明自己的频道 / 发布 / 读当前值 / 订阅 / 结束自己的流**。')
    ..writeln('它**不提供写入**（没有"给别人的频道写值"这个操作，也没有回执）：')
    ..writeln(
      '要改应用状态一律调能力（见 [plugin-capabilities.md](plugin-capabilities.md)），',
    )
    ..writeln('结果的校验与拒绝原因由能力告诉你。')
    ..writeln()
    ..writeln('| 字段 | 含义 |')
    ..writeln('|---|---|')
    ..writeln('| `cache` | `latest` = 有当前值（只保留最后一份）；`none` = 没有当前值，只能等下一次通知 |')
    ..writeln('| `publish` | `owner` = 只有所有者能发布；`anyone` = 任何人都能发布（共享频道） |')
    ..writeln('| `pushOnSubscribe` | 订阅时立刻收到当前缓存值（仅 `cache: latest`） |')
    ..writeln('| `coalesce` | 允许合并（允许丢中间值）；带当前值的频道一般写 `false`，每次发布都通知 |')
    ..writeln('| `scope` | `app` = 业务频道；`system` = 调试与自省频道（插件不能发布） |')
    ..writeln(
      '| `type` | 值的类型提示（`bool` / `number` / `string` / `json`，可选；只用于文档与自省） |',
    )
    ..writeln('| `options` | 枚举取值域（稳定 id，不用本地化文案；可选，只用于文档与自省） |')
    ..writeln()
    ..writeln('## 频道清单')
    ..writeln()
    ..writeln('| 广播 id | 缓存 | 谁能发布 | 订阅即推 | 合并 | 作用域 |')
    ..writeln('|---|---|---|---|---|---|');

  for (final BroadcastDeclaration decl in contract.broadcasts) {
    sb.writeln(
      '| `${decl.id}` | ${decl.cache} | ${decl.publish} | '
      '${decl.pushOnSubscribe ? "是" : "否"} | '
      '${decl.coalesce ? "允许" : "不合并"} | '
      '${decl.scope} |',
    );
  }

  sb
    ..writeln()
    ..writeln('## 官方频道当前值')
    ..writeln()
    ..writeln('下面这些频道带当前值（`cache: latest` + 订阅即推），`type` / `options` 说明值长什么样：')
    ..writeln()
    ..writeln('| 广播 id | 类型 | 取值 | 要改它请调能力 |')
    ..writeln('|---|---|---|---|');

  /// 官方频道对应的写入能力（广播只读，写入一律走能力）
  const Map<String, String> writerOf = <String, String>{
    'musicxx.app.lifecycle': '',
    'musicxx.player.position': '`musicxx.player.seek`',
    'musicxx.ui.animatedLevel': '`musicxx.ui.setAnimatedLevel`',
    'musicxx.ui.particleAnimate': '`musicxx.ui.setParticleAnimate`',
    'musicxx.ui.songIconRotate': '`musicxx.ui.setSongIconRotate`',
    'musicxx.ui.songIconWave': '`musicxx.ui.setSongIconWave`',
    'musicxx.ui.fontFamily': '`musicxx.ui.setFontFamily`',
    'musicxx.theme.available': '',
    'musicxx.theme.current':
        '`musicxx.theme.setCurrent` / `musicxx.theme.setTempMode`',
    'musicxx.theme.mode': '`musicxx.theme.setMode`',
  };

  for (final BroadcastDeclaration decl in contract.broadcasts) {
    if (!writerOf.containsKey(decl.id)) {
      continue;
    }
    final String options = decl.options.isEmpty
        ? '-'
        : decl.options.join(' / ');
    sb.writeln(
      '| `${decl.id}` | ${decl.type.isEmpty ? "-" : decl.type} | $options | '
      '${writerOf[decl.id]!.isEmpty ? "（只读，由应用自己决定）" : writerOf[decl.id]} |',
    );
  }

  sb
    ..writeln()
    ..writeln('## 订阅频率限制')
    ..writeln()
    ..writeln('高频频道（典型是 `musicxx.player.position`）不必让应用降频：**每个订阅方自己声明通知频率**，')
    ..writeln('互不影响。订阅时多传一个 `options`：')
    ..writeln()
    ..writeln('```json')
    ..writeln('{ "intervalMs": 200, "mode": "delay" }')
    ..writeln('```')
    ..writeln()
    ..writeln('| 字段 | 含义 |')
    ..writeln('|---|---|')
    ..writeln('| `intervalMs` | 通知间隔（毫秒）；`0` / 缺省 = 不限频，每次发布都通知 |')
    ..writeln(
      '| `mode` | `delay`（默认）= 间隔内只保留**最新一条**，间隔到点补发一次；'
      '`drop` = 间隔内的通知直接丢掉，只有距上次通知已满 `intervalMs` 的才通知 |',
    )
    ..writeln()
    ..writeln('几条约定：')
    ..writeln()
    ..writeln('- **订阅后的第一条通知立即到达**；`pushOnSubscribe` 补的当前值算第一条，也是限频窗口的起点；')
    ..writeln('- 计时用单调时钟，改系统时间不影响；`intervalMs` 传负数按 0 处理，`mode` 不认识按 `delay`；')
    ..writeln('- 退订 / 插件停用：等着的补发一并丢弃，不会再回调；')
    ..writeln('- 例：`intervalMs = 200`，广播时刻 `0 / 50 / 100 / 250 / 300` 时，')
    ..writeln(
      '  不限频收到 `0 50 100 250 300`，`delay` 收到 `0`（值=0）、`200`（值=100）、`400`（值=300），',
    )
    ..writeln('  `drop` 收到 `0`、`250`。')
    ..writeln()
    ..writeln('只看候选值范围、临时把 `intervalMs` 调大都不需要重新订阅；换频率就退订后重新订阅。');

  sb
    ..writeln()
    ..writeln('## 结束一条流')
    ..writeln()
    ..writeln(
      '频道可以表示一段有限的东西（一次任务的结果流、一次会话的通知流）。所有者可以把它**结束**：'
      '结束之后发布返回 `-5`（`PLUGINXX_RESULT_ERR_ENDED`）、读不到当前值、全部订阅先用 reason JSON '
      '回调一次再摘掉；再次声明同一个 id = 开一条新流（清掉结束标记与缓存）。',
    )
    ..writeln()
    ..writeln('- 只有所有者能结束；结束别人的频道（含应用频道）会被拒绝；')
    ..writeln('- `reason` 是给订阅方看的 JSON（可空，默认 `{}`），原样交给结束回调；')
    ..writeln('- 重复结束是幂等成功；频道不存在 / 不归自己时返回非 0（原因在日志里）；')
    ..writeln(
      '- 订阅：动态库插件用 `subscribeBroadcastWithEnd(id, fn, onEnd, options)`，'
      'JS 用 `musicxx.broadcast.subscribe(id, fn, options, onEnd)`；',
    )
    ..writeln(
      '- 结束：动态库插件用 `endBroadcast(id, reason)`，JS 用 `musicxx.broadcast.end(id, reason)`；',
    )
    ..writeln('- `-5` 是广播表的结果码，与插件框架自己的错误码（那里的 `-5` 是超时）不是一套，不要混用。');

  sb
    ..writeln()
    ..writeln('## 表外的频道（id 自己造）')
    ..writeln()
    ..writeln(
      '这张表只是**预设**：应用与插件都可以用表里没有的 id 发布 / 订阅，不需要事先登记。'
      '表外 id 的默认属性是「没有当前值（`cache: none`）、只有所有者能发布、允许合并、作用域 `app`」；'
      '所有者按 id 前缀判：`plugin.<插件 id>.*` 归那个插件，其它归应用。',
    )
    ..writeln()
    ..writeln(
      '- 插件自定义频道：`plugin.<你的插件 id>.<名字>`（网络请求的进度 / 完成建议叫 `.net`），'
      '只有你能发，别的插件能订阅；',
    )
    ..writeln(
      '- 应用自己的运行期频道（例如埋点广播 `musicxx.point.<埋点 id>.<种类>`）同理，'
      '不需要在定义文件里占一行。',
    )
    ..writeln()
    ..writeln('要当前值（订阅时就能拿到）/ 允许别人发布这类属性时，才需要在定义文件的 `broadcasts` 里占一行。');

  final List<BroadcastDeclaration> documented = <BroadcastDeclaration>[
    for (final BroadcastDeclaration decl in contract.broadcasts)
      if (decl.doc.trim().isNotEmpty) decl,
  ];
  if (documented.isNotEmpty) {
    sb
      ..writeln()
      ..writeln('## 载荷');
    for (final BroadcastDeclaration decl in documented) {
      final String heading = decl.title.isEmpty ? '' : ' ${decl.title}';
      sb
        ..writeln()
        ..writeln('### `${decl.id}`$heading')
        ..writeln()
        ..writeln(decl.doc.trim());
    }
  }
  return sb.toString();
}
