/*
 * ui.js —— 账号页与浏览页的视图拼装
 *
 * 插件不画界面：这里只拼"界面描述"（块 + 控件），渲染由主程序完成。
 * 两个视图：
 * - `accountsView`：账号表单 / 已配置摘要（`ext://example_ftp_source/accounts`，也能嵌在浮层里）；
 * - `browseView`：目录浏览 + 多选 + 加入歌单（`ext://example_ftp_source/browse`，只在浮层里用）。
 *
 * 写法上的几条约定（照 docs/plugin-ui.md）：
 * - 用随插件分发的 kit（`pluginxx.ui.kit`）拼常用行，别手拼同样的结构；
 * - 状态不放在界面里：视图由 `plugin.js` 的当前状态生成，控件的动作只报"哪个控件、什么值"；
 * - 浏览页用目录列表块（`musicxx.DirListView`）：条目、右侧勾选框、顶部路径行都由它画，
 *   当前目录与选中态由插件自己保存（动作里带 `path` / `isDir` / `selected`）；
 * - 外层用 `musicxx.AdaptLayout`：竖屏一欄（列表在上、按钮在下），横屏两欄（右侧放选中与按钮）。
 */

var FtpSource = globalThis.FtpSource || (globalThis.FtpSource = {});

(function (ns) {
  "use strict";

  /// kit（两个 kit 文件在清单 scripts 里先加载）
  function kit() {
    return pluginxx.ui.kit;
  }

  /// 客户端能力段（主程序取视图时会放进参数 `ui`；没有就用中立描述）
  function envOf(args) {
    return (args && typeof args.ui === "object" && args.ui !== null) ? args.ui : null;
  }

  ns.viewEnv = envOf;

  /* ==================== 通用行 ==================== */

  /// 一行"标题 + 说明 + 控件"
  function controlRow(params, env) {
    var control = {
      kind: "Control",
      control: params.control,
      id: params.id,
      value: params.value === undefined ? "" : params.value
    };
    if (params.options) {
      control.options = params.options;
    }
    if (params.integer) {
      control.integer = true;
    }
    if (params.min !== undefined && params.min !== null) {
      control.min = params.min;
    }
    if (params.max !== undefined && params.max !== null) {
      control.max = params.max;
    }
    if (params.action) {
      control.action = params.action;
    }
    var children = [{ kind: "Text", text: params.title }];
    if (params.depict) {
      children.push({ kind: "Text", text: params.depict, type: "caption", tone: "hint" });
    }
    children.push(control);
    return {
      kind: "Block",
      variant: "plain",
      padding: { horizontal: 10, vertical: 6 },
      children: [{ kind: "Column", gap: 6, children: children }]
    };
  }

  /// 一排"几选一"按钮（当前项用 primary 高亮；比 `Control(buttons)` 稳，窄窗口也不会挤）
  function choiceRow(params, env) {
    var buttons = [];
    for (var i = 0; i < params.options.length; i++) {
      var option = params.options[i];
      var selected = option.value === params.value;
      buttons.push({
        kind: "Expanded",
        children: [{
          kind: "Button",
          label: option.label,
          variant: selected ? "primary" : "secondary",
          action: {
            kind: "dispatch",
            name: params.capability,
            args: params.argsOf(option.value)
          }
        }]
      });
    }
    var children = [{ kind: "Text", text: params.title }];
    if (params.depict) {
      children.push({ kind: "Text", text: params.depict, type: "caption", tone: "hint" });
    }
    children.push({ kind: "Row", gap: 12, children: buttons });
    return {
      kind: "Block",
      variant: "plain",
      padding: { horizontal: 10, vertical: 6 },
      children: [{ kind: "Column", gap: 8, children: children }]
    };
  }

  /* ==================== 账号页 ==================== */

  /// 账号表单
  ///
  /// `draft` 是正在编辑的值（在插件内存里，密码不来回传）；`stored` 是已保存的账号
  /// （用它判断"密码留空 = 不修改"）。
  function accountForm(view, env) {
    var k = kit();
    var draft = view.draft;
    var hasSavedPassword = !!(view.stored && view.stored.password);
    var blocks = [
      k.hint({
        text: "账号只保存在本机的插件存储里；歌曲里记录的是「账号指纹 + 路径」，不含密码。"
      }, env),
      k.divider({}, env),
      controlRow({
        id: "host",
        control: "text",
        title: "服务器地址",
        depict: "IP 或域名，例如 192.168.1.20",
        value: draft.host,
        action: { kind: "dispatch", name: "accounts", args: { op: "field" } }
      }, env),
      controlRow({
        id: "port",
        control: "number",
        title: "端口",
        depict: "FTP 默认 21",
        value: draft.port,
        integer: true,
        min: 1,
        max: 65535,
        action: { kind: "dispatch", name: "accounts", args: { op: "field" } }
      }, env),
      controlRow({
        id: "user",
        control: "text",
        title: "用户名",
        depict: "留空表示匿名登录（anonymous）",
        value: draft.user,
        action: { kind: "dispatch", name: "accounts", args: { op: "field" } }
      }, env),
      controlRow({
        id: "password",
        control: "password",
        title: "密码",
        depict: hasSavedPassword ? "留空表示不修改已保存的密码" : "匿名登录时可以留空",
        value: "",
        action: { kind: "dispatch", name: "accounts", args: { op: "field" } }
      }, env),
      controlRow({
        id: "rootPath",
        control: "text",
        title: "根路径",
        depict: "浏览从这里开始，例如 /Music",
        value: draft.rootPath,
        action: { kind: "dispatch", name: "accounts", args: { op: "field" } }
      }, env),
      choiceRow({
        title: "文件名编码",
        depict: "服务器上的中文名乱码时换成 GBK",
        value: draft.encoding,
        capability: "accounts",
        options: [
          { label: "UTF-8", value: "utf-8" },
          { label: "GBK", value: "gbk" }
        ],
        argsOf: function (value) {
          return { op: "field", id: "encoding", value: value };
        }
      }, env),
      controlRow({
        id: "timeoutSec",
        control: "number",
        title: "超时（秒）",
        depict: "连接与命令应答的超时，1 ~ 120",
        value: draft.timeoutSec,
        integer: true,
        min: 1,
        max: 120,
        action: { kind: "dispatch", name: "accounts", args: { op: "field" } }
      }, env),
      k.switchRow({
        id: "audioOnly",
        title: "只看音频文件",
        depict: "浏览时隐藏非音频后缀的文件",
        value: draft.audioOnly,
        action: { kind: "dispatch", name: "accounts", args: { op: "field" } }
      }, env),
      k.divider({}, env),
      k.actionsRow({
        buttons: [
          k.button({
            label: view.testing ? "正在测试…" : "保存并测试",
            variant: "primary",
            disabled: !!view.busy,
            action: { kind: "dispatch", name: "accounts", args: { op: "saveTest" } }
          }, env),
          k.button({
            label: "只保存",
            disabled: !!view.busy,
            action: { kind: "dispatch", name: "accounts", args: { op: "save" } }
          }, env),
          k.button({
            label: "取消",
            variant: "ghost",
            action: { kind: "dispatch", name: "accounts", args: { op: "cancel" } }
          }, env)
        ]
      }, env)
    ];
    if (view.stored) {
      blocks.push(k.button({
        label: "清除已保存的账号",
        variant: "ghost",
        action: { kind: "dispatch", name: "accounts", args: { op: "clear" } }
      }, env));
    }
    return blocks;
  }

  /// 已配置时的摘要
  function accountSummary(view, env) {
    var k = kit();
    var account = view.stored;
    if (!account) {
      // 没配置就没有可列的项（正常不会走到这里：没账号时上面给的是表单）
      return [];
    }
    var tested = "";
    if (view.lastTestAt > 0) {
      var at = new Date(view.lastTestAt);
      tested = (view.lastOk ? "成功 " : "失败 ") +
        (at.getMonth() + 1) + "-" + at.getDate() + " " +
        at.getHours() + ":" + (at.getMinutes() < 10 ? "0" : "") + at.getMinutes();
      if (!view.lastOk && view.lastMessage) {
        tested += "（" + view.lastMessage + "）";
      }
    } else {
      tested = "还没测试过";
    }
    var blocks = [
      k.kv({
        pairs: [
          { k: "服务器", v: "ftp://" + account.host + ":" + account.port },
          { k: "用户名", v: account.user ? account.user : "匿名（anonymous）" },
          { k: "密码", v: account.password ? "已保存（不回显）" : "无" },
          { k: "根路径", v: account.rootPath },
          { k: "文件名编码", v: account.encoding === "gbk" ? "GBK" : "UTF-8" },
          { k: "只看音频", v: account.audioOnly ? "是" : "否" },
          { k: "最近测试", v: tested }
        ]
      }, env),
      k.divider({}, env),
      k.actionsRow({
        buttons: [
          k.button({
            label: "编辑",
            variant: "primary",
            action: { kind: "dispatch", name: "accounts", args: { op: "edit" } }
          }, env),
          k.button({
            label: view.busy ? "正在测试…" : "测试连接",
            disabled: !!view.busy,
            action: { kind: "dispatch", name: "accounts", args: { op: "test" } }
          }, env)
        ]
      }, env)
    ];
    if (view.returnTo === "browse") {
      blocks.push(k.button({
        label: "返回浏览",
        action: { kind: "dispatch", name: "browse", args: { op: "resume" } }
      }, env));
    }
    return blocks;
  }

  /// 账号页（表单或摘要）；`view` 由 plugin.js 组装
  ///
  /// 字段：`{stored, draft, message, messageTone, busy, testing, returnTo, lastTestAt, lastOk, lastMessage}`
  ns.accountsView = function (view) {
    var env = view.env || null;
    var k = kit();
    var blocks = [];
    if (view.message) {
      blocks.push(k.text({
        text: view.message,
        tone: view.messageTone === "error" ? "error" : "accent"
      }, env));
    }
    if (view.draft) {
      blocks = blocks.concat(accountForm(view, env));
    } else {
      blocks = blocks.concat(accountSummary(view, env));
    }
    return {
      title: "FTP 音乐",
      subtitle: "单账号示例：浏览目录、挑选歌曲、播放时转成 http 供播放器读取",
      blocks: blocks
    };
  };

  /* ==================== 浏览页 ==================== */

  /// 浏览页；`view` 由 plugin.js 组装
  ///
  /// 字段：`{configured, account, path, keyword, audioOnly, entries, shown, total, limit,
  ///        selectedCount, pid, playlistName, error, busy}`
  ns.browseView = function (view) {
    var env = view.env || null;
    var k = kit();

    if (!view.configured) {
      return {
        title: "添加 FTP 歌曲",
        blocks: [
          k.hint({ text: "还没有配置 FTP 账号：先填服务器地址、用户名与密码。" }, env),
          k.button({
            label: "去配置账号",
            variant: "primary",
            action: { kind: "dispatch", name: "accounts", args: { op: "edit", from: "browse" } }
          }, env)
        ]
      };
    }

    // 头部（两种布局共用）：账号、操作按钮、筛选
    // 注：头部越长，能用的窗口就越矮；当前目录由列表块自己的路径行显示，这里不重复。
    var head = [
      k.hint({
        text: "ftp://" + view.account.host + ":" + view.account.port +
          (view.account.user ? "（" + view.account.user + "）" : "（匿名）")
      }, env),
      k.actionsRow({
        buttons: [
          k.button({
            label: "上一级",
            disabled: view.path === "/",
            action: { kind: "dispatch", name: "browse", args: { op: "up" } }
          }, env),
          k.button({
            label: "刷新",
            action: { kind: "dispatch", name: "browse", args: { op: "refresh" } }
          }, env),
          k.button({
            label: "账号",
            variant: "ghost",
            action: { kind: "dispatch", name: "accounts", args: { op: "edit", from: "browse" } }
          }, env)
        ]
      }, env),
      controlRow({
        id: "keyword",
        control: "text",
        title: "按名称筛选",
        value: view.keyword,
        action: { kind: "dispatch", name: "browse", args: { op: "keyword" } }
      }, env),
      k.switchRow({
        id: "audioOnly",
        title: "只看音频文件",
        depict: "隐藏 .txt / .jpg 这类文件；文件夹始终显示",
        value: view.audioOnly,
        action: { kind: "dispatch", name: "browse", args: { op: "audioOnly" } }
      }, env),
      k.divider({}, env)
    ];
    if (view.error) {
      head.push(k.text({ text: view.error, tone: "error" }, env));
    }

    // 中间：目录条目交给目录列表块（条目多时只滚这一块；不给 height = 用父给的可用高度）
    // 每一项都带自己的动作：目录是“进入”，文件是“勾上 / 取消”。
    var items = [];
    for (var i = 0; i < view.entries.length; i++) {
      var entry = view.entries[i];
      var item = {
        id: entry.path,
        title: entry.name,
        path: entry.path,
        isDir: !!entry.isDir,
        selected: !!view.selected[entry.path],
        action: {
          kind: "dispatch",
          name: "browse",
          args: { op: entry.isDir ? "enter" : "toggle", path: entry.path }
        }
      };
      var size = ns.formatSize(entry.size);
      if (entry.isDir) {
        item.subtitle = "文件夹";
      } else if (size) {
        item.subtitle = size;
      }
      items.push(item);
    }

    var list = k.dirListView({
      path: view.path,
      items: items,
      // 勾选框（multi 打开才出现）：勾选态就是条目的 selected，变化派发 selectAction
      multi: true,
      pathAction: { kind: "dispatch", name: "browse", args: { op: "up" } },
      selectAction: { kind: "dispatch", name: "browse", args: { op: "select" } },
      emptyText: view.error ? "没有读到目录内容。" : "这个目录里没有符合条件的条目。"
    }, env);

    // 按钮与状态：竖屏放在列表下面，横屏放到右侧那一欄
    var actions = [];
    if (view.total > view.entries.length) {
      actions.push(k.button({
        label: "再显示 " + Math.min(50, view.total - view.entries.length) + " 项（共 " +
          view.total + " 项）",
        variant: "ghost",
        action: { kind: "dispatch", name: "browse", args: { op: "more" } }
      }, env));
    }
    if (view.pid) {
      actions.push(k.button({
        label: "添加 " + view.selectedCount + " 首到『" + (view.playlistName || view.pid) + "』",
        variant: "primary",
        disabled: view.selectedCount <= 0 || !!view.busy,
        action: { kind: "dispatch", name: "browseAdd", args: {} }
      }, env));
    }
    actions.push(k.button({
      label: "播放选中的第一首",
      disabled: view.selectedCount <= 0 || !!view.busy,
      action: { kind: "dispatch", name: "browsePlay", args: {} }
    }, env));
    if (view.busy) {
      actions.push(k.hint({ text: "正在处理，请稍等…" }, env));
    }
    if (view.selectedCount > 0) {
      actions.push(k.hint({ text: "已选 " + view.selectedCount + " 首（同一路径重复添加会被去重）。" }, env));
    }

    // 竖屏：一欄（头部 + 列表 + 按钮）；横屏：两欄（左边头部 + 列表，右边按钮与选中状态）
    var columnBody = head.concat([{ kind: "Expanded", children: [list] }, k.divider({}, env)], actions);
    var rowBody = head.concat([{ kind: "Expanded", children: [list] }]);

    return {
      title: view.pid ? ("添加歌曲到『" + (view.playlistName || view.pid) + "』") : "添加 FTP 歌曲",
      // 顶层直接写 AdaptLayout：主程序会把它放进 Expanded（有界高度），列表才能自己滚
      blocks: [k.adaptLayout({
        columnBody: columnBody,
        rowBody: rowBody,
        rowSecondBody: actions,
        rowBodyRatio: 0.62
      }, env)]
    };
  };
})(FtpSource);
