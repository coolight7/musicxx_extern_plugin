// 广播示例（`musicxx.broadcast`）
//
// 覆盖四件事：
// - 顶层声明自己的频道（`cache: latest` = 有当前值，订阅方先收到一次）；
// - 发布自己的频道（属主才能发，订阅方当场收到通知）；
// - 声明一个 `publish: anyone` 的共享频道，谁都能往它发布；
// - 订阅官方频道（官方频道只读：要改应用的状态调能力）。
//
// 广播没有"写入别人的频道"，也没有回执：要改应用或别的插件的状态调能力。
// 能力处理器不能返回 Promise（跨边界不支持异步返回值），所以把订阅结果记在变量里，
// 由 probe 能力回报 —— 原生测试与包内流程测试都读这几个字段。

var changes = []; // 订阅到的通知（最近一条在最后）

// 自己的频道：有当前值，订阅方订阅时先收到一次，之后每次 publish 都推
musicxx.broadcast.declare({
  id: "tip.start",
  cache: "latest",
  title: "开场提示",
  depict: "为真时播放页显示一句开场提示",
});
musicxx.broadcast.publish("tip.start", false);

// 自己的计数器频道：同样是 cache: latest，订阅方同步读得到当前值
musicxx.broadcast.declare({
  id: "stats.reads",
  cache: "latest",
  title: "读次数",
});
musicxx.broadcast.publish("stats.reads", 0);

// 共享频道：publish: anyone 表示别的插件也能往这里发
musicxx.broadcast.declare({
  id: "shared.ping",
  cache: "latest",
  publish: "anyone",
  title: "共享心跳",
});

// 官方频道：只订阅。官方频道由应用发布，插件发布会被拒
musicxx.broadcast.subscribe("musicxx.ui.animatedLevel", function (value) {
  changes.push({ topic: "musicxx.ui.animatedLevel", value: value });
});

// 高频频道不必自己降频：声明自己的通知频率即可（这里每 500 ms 最多一条，只留最新）
var positionNotices = 0;
var lastPosition = null;
musicxx.broadcast.subscribe(
  "musicxx.player.position",
  function (value) {
    ++positionNotices;
    lastPosition = value;
  },
  { intervalMs: 500, mode: "delay" },
);

musicxx.capability.register("probe", function () {
  return {
    // declared() 返回短名（不带 plugin.<id>. 前缀）
    own: musicxx.broadcast.declared().map(function (item) {
      return item.id;
    }),
    listCount: musicxx.broadcast.list("plugin." + musicxx.pluginId).length,
    tipPeek: musicxx.broadcast.read("tip.start"),
    tipInfo: musicxx.broadcast.info("tip.start"),
    sharedSubscribers: musicxx.broadcast.subscribers("shared.ping"),
    changes: changes.length,
    lastChange: changes.length ? changes[changes.length - 1] : null,
    positionNotices: positionNotices,
    lastPosition: lastPosition,
    // 插件框架按本插件的形态报接口代次：JS 插件看到的是 JS 接口代次（当前 2）
    apiVersion: musicxx.host.info().apiVersion,
  };
});

// 属主改自己的值：publish 一次，订阅方才看得到
musicxx.capability.register("publishTip", function (args) {
  var next = args ? args.value === true : false;
  musicxx.broadcast.publish("tip.start", next);
  return { tipStart: next };
});

// 往共享频道发一条（`publish: anyone`，别的插件也能发）
musicxx.capability.register("publishShared", function (args) {
  var value = args && args.value !== undefined ? args.value : null;
  musicxx.broadcast.publish("shared.ping", value);
  return { published: true };
});
