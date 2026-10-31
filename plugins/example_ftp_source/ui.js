/*
 * ui.js —— 账号页的视图拼装
 *
 * 插件不画界面：这里只拼"界面描述"（块 + 控件），渲染由主程序完成。
 * 只有一个视图：`accountsView`：账号表单 / 已配置摘要（`ext://example_ftp_source/accounts`）。
 * 挑歌 / 浏览目录的界面由主程序画（`musicxx.source.browse` 声明，见 plugin.js）。
 *
 * 写法上的几条约定（照 docs/plugin-ui.md）：
 * - 用随插件分发的 kit（`pluginxx.ui.kit`）拼常用行，别手拼同样的结构；
 * - 状态不放在界面里：视图由 `plugin.js` 的当前状态生成，控件的动作只报"哪个控件、什么值"。
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
    return blocks;
  }

  /// 账号页（表单或摘要）；`view` 由 plugin.js 组装
  ///
  /// 字段：`{stored, draft, message, messageTone, busy, testing, lastTestAt, lastOk, lastMessage}`
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

})(FtpSource);
