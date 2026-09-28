// 变量通道测试夹具（只用于包内端到端测试，不是可发布的插件）
//
// 覆盖：
// - 顶层登记自己的变量（declared 模式：值由宿主代存，set 即提交）；
// - 绑定官方变量（订阅还不存在的键也是允许的，键出现时会补一条初始通知）；
// - 异步读官方变量（await get，由宿主向应用取一次真实值）；
// - 异步写官方变量（set 由应用落地，结果等 writeResult 事件）。
//
// 能力处理器**不能返回 Promise**（跨边界不支持异步返回值），因此把异步结果记在
// 变量里，由 probe 能力回报。

var changes = [];        // 绑定回调收到的变更（最近一条在最后）
var readValue = null;    // 最近一次异步读到的值
var readError = "";      // 最近一次异步读的失败原因
var writeResult = null;  // 最近一次异步写的结果
var writeError = "";     // 最近一次异步写的失败原因
var pendingReads = 0;    // 还在等结果的读次数

musicxx.vars.register({
  key: "tip.start",
  caps: ["get", "set", "notify"],
  type: "bool",
  value: false,
  title: "开场提示",
  depict: "为真时播放页显示一句开场提示"
});

musicxx.vars.register({
  key: "stats.reads",
  caps: ["get", "set"],
  type: "number",
  value: 0
});

// 绑定官方变量：记录每次变化（这是"订阅 + 回调"）
musicxx.vars.bind("musicxx.test.bound", function (value, info) {
  changes.push({
    value: value,
    by: info.by,
    revision: info.revision,
    removed: info.removed === true
  });
  // 收到变化时顺手保活缓存（watch 就是"订阅但不要回调"）
  musicxx.vars.watch("musicxx.test.bound");
});

musicxx.capability.register("probe", function () {
  return {
    own: musicxx.vars.own().map(function (item) { return item.key; }),
    listCount: musicxx.vars.list("plugin.vars_js").length,
    tipPeek: musicxx.vars.peek("tip.start"),
    boundInfo: musicxx.vars.info("musicxx.test.bound"),
    changes: changes.length,
    lastChange: changes.length ? changes[changes.length - 1] : null,
    readValue: readValue,
    readError: readError,
    writeResult: writeResult,
    writeError: writeError,
    pendingReads: pendingReads
  };
});

musicxx.capability.register("triggerRead", function (args) {
  var key = (args && args.key) ? String(args.key) : "musicxx.test.bound";
  readError = "";
  ++pendingReads;
  // 异步、权威读：宿主向属主取一次真实值，结果回到这里
  musicxx.vars.get(key).then(function (value) {
    readValue = value;
    --pendingReads;
  }, function (e) {
    readError = (e && e.message) ? e.message : String(e);
    --pendingReads;
  });
  return { started: true };
});

musicxx.capability.register("triggerWrite", function (args) {
  var key = (args && args.key) ? String(args.key) : "musicxx.test.bound";
  var value = args ? args.value : null;
  writeResult = null;
  writeError = "";
  musicxx.vars.set(key, value).then(function (result) {
    writeResult = result;
  }, function (e) {
    writeError = (e && e.message) ? e.message : String(e);
  });
  return { started: true };
});
