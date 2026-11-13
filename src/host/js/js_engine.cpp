/// musicxx 外部插件原生插件框架: JS插件运行时实现 (QuickJS)
///
/// 见 js_engine.h 的设计说明。本文件包含四部分:
/// 1. 进程级内置槽位表 (`js:<pluginId>` 的合成内置插件描述);
/// 2. 共享 JS 线程与任务/定时器/能力调用超时循环;
/// 3. `musicxx` 全局对象的 JS 预置代码 (prelude) 与 C 桥接原语;
/// 4. 实例生命周期 (建 runtime → 执行脚本 → 回放注册 → 停止时摘除注册并释放
/// runtime)。
#include "js_engine.h"

#include "host_json.h"
#include "pluginxx/api/iface_meta.h"
#include "pluginxx/api/tables.h"
#include "pluginxx/host/abi_util.h"
#include "pluginxx/host/manifest.h"
#include "pluginxx/runtime/instance_base.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"

#include <asio/post.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(MUSICXX_EXTERN_PLUGIN_HAS_JS)
#include "quickjs.h"
#endif

#ifndef MUSICXX_EXTERN_PLUGIN_HAS_JS
#define MUSICXX_EXTERN_PLUGIN_HAS_JS 0
#endif

namespace musicxx {
namespace extern_plugin {

namespace fs = std::filesystem;
using utilxx_base::Json;

namespace {

/// 能力处理器返回 Promise 时的回给插件框架的标记 (与 prelude 的
/// `kCapabilityPendingMarker` 一致)
constexpr const char *kCapabilityPendingMarker = "~pending~";

/// 处理器返回 Promise 时, JS 侧回给插件框架的标记 (必须与 prelude 的
/// `kPendingMarker` 一致)
constexpr const char *kPendingMarker = "~pending~";

/// 编译期是否包含 QuickJS
constexpr bool kJsCompiled = MUSICXX_EXTERN_PLUGIN_HAS_JS != 0;

int64_t nowSteadyMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

std::string readTextFile(const fs::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  std::ostringstream oss;
  oss << in.rdbuf();
  return oss.str();
}

/// 跨边界字符串视图 → C++ 字符串
std::string viewToStr(const PluginxxStringView *view) {
  if (!view || !view->data || view->size == 0) {
    return {};
  }
  return std::string{view->data, static_cast<size_t>(view->size)};
}

/* ==================== 进程级内置槽位表 ==================== */

/// 一个 JS插件的内置槽位 (名称/脚本路径/版本)
///
/// `scripts` 按清单 `scripts` 的顺序排列: 同一个 JS 上下文里依次执行 (前面的脚本
/// 先跑完才跑后面的), 加载失败会带上出错脚本的文件名。缺省是 `[plugin.js]`。
struct Slot {
  std::string name; ///< js:<pluginId>
  std::vector<std::string> scripts;
  std::string version;
  bool used = false;
};

/// 槽位表: 槽位数量不设上限 (按需增长)
///
/// 交给插件框架的两份描述数组 (`infos` / `manifests`) 在每次调用时按当前槽位重建,
/// 插件框架拿到指针后立即取用 (注册与加载都在框架线程串行, 不会跨调用持有旧指针)。
struct SlotTable {
  std::vector<Slot> slots; ///< 槽位 (含已注销的, 下标稳定便于复用)
  std::vector<PluginxxBuiltinInfo> infos;
  std::vector<PluginxxBuiltinManifest> manifests;
  std::mutex mutex;
  /// 当前 JS 引擎 (插件框架为进程单例; 内置入口是静态函数, 只能经它找到引擎)
  std::atomic<JsEngine *> engine{nullptr};
};

SlotTable &slotTable() {
  static SlotTable table;
  return table;
}

std::vector<std::string> slotScripts(std::string_view name) {
  SlotTable &table = slotTable();
  std::lock_guard lock{table.mutex};
  for (auto &slot : table.slots) {
    if (slot.used && slot.name == name) {
      return slot.scripts;
    }
  }
  return {};
}

/// 脚本路径拼成一行（日志/调试信息用）
std::string joinPaths(const std::vector<std::string> &paths) {
  std::string out;
  for (const std::string &path : paths) {
    if (!out.empty()) {
      out += ", ";
    }
    out += path;
  }
  return out;
}

/// 在调试信息里发布脚本名（只给文件名，路径可能含用户名等本机信息）
utilxx_base::Json scriptNamesJson(const std::vector<std::string> &paths) {
  utilxx_base::Json array = utilxx_base::Json::array();
  for (const std::string &path : paths) {
    const size_t slash = path.find_last_of("/\\");
    array.push_back(slash == std::string::npos ? path : path.substr(slash + 1));
  }
  return array;
}

std::string slotVersion(std::string_view name) {
  SlotTable &table = slotTable();
  std::lock_guard lock{table.mutex};
  for (auto &slot : table.slots) {
    if (slot.used && slot.name == name) {
      return slot.version;
    }
  }
  return {};
}

/* ==================== JS 预置代码 (prelude) ==================== */

/// 注入到每个 JS插件上下文里的 `musicxx` API
///
/// 设计: 原生只暴露少量"桥接原语" (`__ext.*`), 其余 API 面 (Promise
/// 封装、便捷方法、 工具函数) 都用 JS 实现 —— 减少 C 代码量, 也便于后续扩展 API
/// 而不动插件框架二进制。
constexpr const char *kPrelude = R"JS(
(function () {
  "use strict";
  var ext = globalThis.__ext;
  if (!ext) { throw new Error("musicxx 运行时桥接缺失"); }

  var implementationTable = new Map();
  var capTable = new Map();
  var uiTable = new Map();
  var appCapabilityWaiters = new Map();
  var capabilityWaiters = new Map();
  var timerTable = new Map();
  /// 裁决型能力处理器的 Promise 等待登记 (key = 插件框架给的等待 id)
  var pendingImplementationWaits = new Map();
  /// 处理器返回 Promise 时回给插件框架的标记 (与 C++ 侧 `kPendingMarker` 一致)。
  var kPendingMarker = "~pending~";
  /// 能力处理器返回 Promise 时回给插件框架的标记 (与 C++ 侧
  /// `kCapabilityPendingMarker` 一致)
  var kCapabilityPendingMarker = "~pending~";
  /// 异步能力处理器还没结算的等待号 (JSON 结果由插件框架等待槽接收)
  var pendingCapabilityWaits = new Map();
/// 插件自己声明的功能埋点的实现表 (点 id → 处理函数)
///
/// 功能埋点走**一条固定的通用通道能力** (`musicxx.feature.point.call`):
/// 同一条通道上所有实现的处理器都会收到同一次调用, 所以这里用一个分发器按 `point` 过滤
/// (而不是每个点各注册一个处理器 —— JS插件在同一能力上只能注册一个处理器)。
var featurePointImpls = new Map();

  function logError(message) {
    try { ext.log(4, String(message)); } catch (e) { /* 日志失败不影响脚本 */ }
  }
  function assertName(value, what) {
    if (typeof value !== "string" || value.length === 0) {
      throw new Error(what + " 必须是非空字符串");
    }
  }
  function safeJson(value) {
    try { return JSON.stringify(value); } catch (e) { return "{}"; }
  }
  /// UI 项名称: 允许短名或本插件全名 (插件框架按短名登记, 全名去掉前缀)
  function shortName(value) {
    var text = String(value);
    var prefix = "plugin." + musicxx.pluginId + ".";
    return text.indexOf(prefix) === 0 ? text.slice(prefix.length) : text;
  }
  /// UI 项类型: 允许写官方全名 (`musicxx.ui.home.entry`) 或简称 (`home.entry`)
  var UI_TYPES = {
    "home.entry": "musicxx.ui.home.entry",
    "song.action": "musicxx.ui.song.action",
    "playlist.action": "musicxx.ui.playlist.action",
    "addsong.entry": "musicxx.ui.addsong.entry",
    "playing.background": "musicxx.ui.playing.background",
    "playing.icon": "musicxx.ui.playing.icon",
    "playing.visualizer": "musicxx.ui.playing.visualizer",
    "playing.controlBar": "musicxx.ui.playing.controlBar",
    "playing.controlBar.menu": "musicxx.ui.playing.controlBar.menu",
    "playing.controlBar.previous": "musicxx.ui.playing.controlBar.previous",
    "playing.controlBar.playOrPause": "musicxx.ui.playing.controlBar.playOrPause",
    "playing.controlBar.next": "musicxx.ui.playing.controlBar.next",
    "playing.controlBar.favorite": "musicxx.ui.playing.controlBar.favorite",
    "playing.lyric.top": "musicxx.ui.playing.lyric.top",
    "playing.lyric.bottom": "musicxx.ui.playing.lyric.bottom",
    "playing.lyric.header": "musicxx.ui.playing.lyric.header",
    "playing.lyric.footer": "musicxx.ui.playing.lyric.footer",
    "playing.moreInfo.header": "musicxx.ui.playing.moreInfo.header",
    "playing.moreInfo.footer": "musicxx.ui.playing.moreInfo.footer",
    "playing.songlist.top": "musicxx.ui.playing.songlist.top",
    "playing.songlist.bottom": "musicxx.ui.playing.songlist.bottom",
    "playing.songlist.header": "musicxx.ui.playing.songlist.header",
    "playing.songlist.footer": "musicxx.ui.playing.songlist.footer"
  };
  function normalizeType(value) {
    assertName(value, "ui.registerEntry: type");
    var text = String(value);
    return UI_TYPES[text] || text;
  }
  function normalizeEntry(spec) {
    if (typeof spec === "string") { spec = { name: spec }; }
    if (!spec || typeof spec !== "object") { throw new Error("ui.registerEntry: 需要对象参数"); }
    var name = (typeof spec.name === "string" && spec.name)
                 ? spec.name
                 : (typeof spec.id === "string" ? spec.id : "");
    name = shortName(name);
    assertName(name, "ui.registerEntry: name");
    spec.type = normalizeType(spec.type);
    var data = spec.data;
    if (data === undefined || data === null) {
      data = {};
      Object.keys(spec).forEach(function (key) {
        if (key === "name" || key === "id" || key === "type" || key === "order" || key === "data") { return; }
        data[key] = spec[key];
      });
    }
    if (typeof data !== "object" || data === null) { throw new Error("ui.registerEntry: data 必须是对象"); }
    return {
      name: name,
      type: String(spec.type),
      order: Number.isFinite(spec.order) ? Math.trunc(spec.order) : 0,
      data: data
    };
  }

  var musicxx = {};
  musicxx.version = ext.engineVersion();
  musicxx.pluginId = ext.pluginId();

  /// 能力阶段 (参与方式): 能力 id 表示**一次调用**, 由阶段表达怎么参与
  ///
  /// - `before`: 前链, 串行、正序, 只能改输入 (`{patch: {...}}`) 或拦下 (`{action: "cancel"}`);
  /// - `impl`  : 实现链, 给结果 / 裁决 (不写阶段就是它);
  /// - `after` : 后链, 串行、**逆序**, 只能改输出 (`{value: ...}`) 或丢弃 (`{action: "drop"}`)。
  var PHASES = { impl: "impl", before: "before", after: "after" };
  function normalizePhase(value) {
    if (value === undefined || value === null || value === "") { return "impl"; }
    var text = String(value);
    if (!PHASES[text]) {
      throw new Error("implementations: 未知阶段 " + text + " (只能是 before / impl / after)");
    }
    return text;
  }
  function implementationKey(id, phase) { return String(id) + "|" + phase; }


  /// 宿主自己的信息与日志 (不是频道, 所以留在 host 上)
  musicxx.host = {
    info: function () {
      try { return JSON.parse(ext.hostInfo() || "{}"); } catch (e) { return {}; }
    },
    log: function (level, message) {
      ext.log(typeof level === "number" ? level : 2, String(message));
    },
    configPath: function () { return ext.configPath(); }
  };

  /* ---- 广播 (属主发布, 任何人读 / 订阅) ----
   *
   * 广播只做四件事: **声明自己的频道 / 发布 / 读当前值 / 订阅**。
   * 它**没有写入**: 没有"给别人的频道写值"这个操作, 也没有回执;
   * 要改应用或别的插件的状态, 调能力 (`musicxx.capability.call(id, args)`)。
   *
   * 频道 id: 官方 `musicxx.<域>.<名>` 直接写全名; 自己的频道写短名 (如 `score.total`),
   * 插件框架自动补 `plugin.<本插件id>.` 前缀。
   *
   * 回调只有一种形状: `fn(payload, info)`, `info.id` 是频道全名。
   * 高频频道不必自己降频: 订阅时给 `{intervalMs, mode}` 声明你的通知频率。
   */
  var declaredBroadcasts = new Map();  // 短名 → 声明 (declare 记下的; 供 declared() 与回放)
  var broadcastSubs = new Map();       // 全名 → [{fn, options}]
  var broadcastHostSubs = new Set();   // 已向插件框架订过的 "全名\u0000options"

  function broadcastShortKey(key) {
    var text = String(key);
    var prefix = "plugin." + musicxx.pluginId + ".";
    return text.indexOf(prefix) === 0 ? text.slice(prefix.length) : text;
  }
  function broadcastFullKey(key) {
    assertName(key, "broadcast: 频道");
    var text = String(key);
    if (text.indexOf("musicxx.") === 0 || text.indexOf("plugin.") === 0) { return text; }
    return "plugin." + musicxx.pluginId + "." + text;
  }
  /// 订阅频率选项 → 交给插件框架的 JSON (空串 = 不限频)
  function broadcastOptionsJson(options) {
    if (!options || typeof options !== "object") { return ""; }
    var intervalMs = Number.isFinite(options.intervalMs) ? Math.trunc(options.intervalMs) : 0;
    if (intervalMs < 0) { intervalMs = 0; }
    var mode = options.mode === "drop" ? "drop" : "delay";
    if (intervalMs === 0 && mode === "delay") { return ""; }
    return JSON.stringify({ intervalMs: intervalMs, mode: mode });
  }
  /// 向插件框架订一次 (同一个频道 + 同一套频率只订一次; 回放阶段由引擎统一建立)
  ///
  /// 注意: 必须同时记进 `broadcastSubs` —— 脚本顶层调用 `ext.subscribeHost` 会被引擎
  /// 推迟到"注册回放"阶段执行, 而回放的内容正是 `collectRegistrations` 从
  /// `broadcastSubs` 里取的; 只调原语不记账的话, 顶层绑定会静默地收不到任何事件。
  function ensureBroadcastHostSubscription(topic, optionsJson) {
    var key = topic + "\u0000" + optionsJson;
    if (broadcastHostSubs.has(key)) { return; }
    broadcastHostSubs.add(key);
    ext.subscribeHost(topic, optionsJson);
  }

  musicxx.broadcast = {
    /// 声明 / 覆盖自己的一个频道; 返回 true (失败抛错, 原因写在异常里)
    ///
    /// `declare({id, cache, publish, pushOnSubscribe, coalesce, title, depict})`
    /// - `cache`: `none`(默认, 只有事件)/ `latest`(有当前值, 订阅即推);
    /// - `publish`: `owner`(默认, 只有自己能发)/ `anyone`(共享频道, 谁都能发);
    /// - 运行期重复声明同一个 id 会覆盖属性, 当前值仍然保留 (要刷新就用 `publish`)。
    declare: function (spec) {
      if (!spec || typeof spec !== "object") {
        throw new Error("broadcast.declare: 需要对象参数");
      }
      var full = broadcastFullKey(spec.id === undefined ? "" : spec.id);
      var cache = spec.cache === "latest" ? "latest" : "none";
      var decl = {
        id: full,
        cache: cache,
        publish: spec.publish === "anyone" ? "anyone" : "owner",
        pushOnSubscribe: cache === "latest" && spec.pushOnSubscribe !== false,
        coalesce: spec.coalesce === true,
        scope: spec.scope === "system" ? "system" : "app",
        title: typeof spec.title === "string" ? spec.title : "",
        depict: typeof spec.depict === "string" ? spec.depict : ""
      };
      declaredBroadcasts.set(broadcastShortKey(full), decl);
      ext.declareBroadcast(safeJson(decl));
      return true;
    },
    /// 发布一条广播 (自己的频道, 或 `publish: anyone` 的共享频道)
    publish: function (id, payload) {
      var full = broadcastFullKey(id);
      var err = ext.publish(full, safeJson(payload === undefined ? null : payload));
      if (err) { throw new Error(err); }
      return undefined;
    },
    /// 同步读当前值 (没有当前值返回 null; `cache: none` 的频道永远没有)
    read: function (id) {
      var text = null;
      try { text = ext.broadcastRead(broadcastFullKey(id)); } catch (e) { text = null; }
      if (!text) { return null; }
      try { return JSON.parse(text); } catch (e) { return text; }
    },
    /// 订阅一个频道: `fn(payload, info)`; `options` = `{intervalMs, mode}`
    ///
    /// - `intervalMs` = 通知间隔 (毫秒, 0 / 缺省 = 不限频);
    /// - `mode` = `delay`(默认, 间隔内只留最新一条、到点补发)/ `drop`(间隔内直接丢掉);
    /// - 第一条通知立即到达 (`cache: latest` 的频道会先补一次当前值);
    /// - `onEnd`（可选）: 频道被属主结束时回调一次 `onEnd(reason, info)`，这个订阅随即失效；
    /// - 返回解绑函数。同一个频道可以用不同频率订阅多次。
    subscribe: function (id, fn, options, onEnd) {
      if (typeof fn !== "function") { throw new Error("broadcast.subscribe: 缺少回调"); }
      var full = broadcastFullKey(id);
      var optionsJson = broadcastOptionsJson(options);
      var list = broadcastSubs.get(full);
      if (!list) { list = []; broadcastSubs.set(full, list); }
      list.push({ fn: fn,
                  onEnd: typeof onEnd === "function" ? onEnd : null,
                  options: optionsJson });
      ensureBroadcastHostSubscription(full, optionsJson);
      return function () { musicxx.broadcast.unsubscribe(full, fn); };
    },
    /// 退订: 传 id 摘掉这个频道上的全部回调; 再传 fn 只摘那一个
    unsubscribe: function (id, fn) {
      var full = broadcastFullKey(id);
      var list = broadcastSubs.get(full);
      if (!list) { return false; }
      if (typeof fn === "function") {
        for (var i = 0; i < list.length; i += 1) {
          if (list[i].fn === fn) { list.splice(i, 1); break; }
        }
      } else {
        list.length = 0;
      }
      if (list.length === 0) { broadcastSubs.delete(full); }
      return true;
    },
    /// 结束自己的一条流（只有频道属主能结束）
    ///
    /// - `reason` 原样交给每个订阅方的结束回调（可空，默认 `{}`）；
    /// - 结束后发布被拒 (抛错)、`read` 读不到值，订阅方的 `onEnd` 各回调一次；
    /// - 再次 `declare` 同一个 id = 开一条新流。
    end: function (id, reason) {
      var full = broadcastFullKey(id);
      var err = ext.endBroadcast(full, reason === undefined ? "" : safeJson(reason));
      if (err) { throw new Error(err); }
      return undefined;
    },
    /// 频道声明过没有 (读注册表, 不读值)
    has: function (id) {
      var text = null;
      try { text = ext.broadcastInfo(broadcastFullKey(id)); } catch (e) { text = null; }
      return !!text;
    },
    /// 声明 + 当前值状态 (id/cache/publish/scope/owner/subscribers/hasValue/...)
    info: function (id) {
      var text = null;
      try { text = ext.broadcastInfo(broadcastFullKey(id)); } catch (e) { text = null; }
      if (!text) { return null; }
      try { return JSON.parse(text); } catch (e) { return null; }
    },
    /// 列出频道 (prefix 可空); 每项带声明与当前值
    list: function (prefix) {
      var text = null;
      try {
        text = ext.broadcastList(prefix === undefined ? "" : String(prefix));
      } catch (e) { text = null; }
      if (!text) { return []; }
      try { return JSON.parse(text); } catch (e) { return []; }
    },
    /// 有几个插件在听这个频道 (自己判断"还要不要继续高频发"的依据)
    subscribers: function (id) {
      var info = musicxx.broadcast.info(id);
      return info && Number.isFinite(info.subscribers) ? info.subscribers : 0;
    },
    /// 本插件声明过的频道
    declared: function () {
      var out = [];
      declaredBroadcasts.forEach(function (value, key) {
        out.push({ id: key, declare: value });
      });
      return out;
    }
  };

  /// 调一次**应用实现的能力**（`musicxx.*`），返回 Promise
  ///
  /// 内部入口，插件用 `musicxx.capability.call(id, args, timeoutMs?)`。
  /// 请求当场从 JS 线程推给应用（不投递到框架线程：start 事务期间会互等），
  /// 结果经 `ext.onCapabilityResult` 回来，与本文件里跨插件调用共用一张等待表。
  function appCall(id, args, budget) {
    assertName(id, "capability.call: 能力 id");
    return new Promise(function (resolve, reject) {
      var payload = safeJson(args === undefined || args === null ? {} : args);
      var timeout = (typeof budget === "number" && budget > 0) ? Math.trunc(budget) : 5000;
      var requestId = ext.appCapabilityCall(id, payload, timeout);
      if (typeof requestId !== "number" || requestId < 0) {
        reject(new Error("应用能力调用未被受理: " + id + " (code=" + requestId + ")"));
        return;
      }
      // 键用字符串: 插件框架回调传进来的 id 是字符串
      appCapabilityWaiters.set(String(requestId), { resolve: resolve, reject: reject });
    });
  }
  ext.onAppCapabilityDone = function (id, status, json) {
    var key = String(id);
    var waiter = appCapabilityWaiters.get(key);
    if (!waiter) { return; }
    appCapabilityWaiters.delete(key);
    var value = null;
    if (json) { try { value = JSON.parse(json); } catch (e) { value = json; } }
    if (Number(status) === 0) {
      waiter.resolve(value);
    } else {
      var reason = (value && value.error) ? value.error : ("status=" + status);
      waiter.reject(new Error("应用能力调用失败: " + reason));
    }
  };

  musicxx.player = {
    play: function (args) { return appCall("musicxx.player.play", args); },
    pause: function () { return appCall("musicxx.player.pause"); },
    toggle: function () { return appCall("musicxx.player.toggle"); },
    stop: function () { return appCall("musicxx.player.stop"); },
    next: function () { return appCall("musicxx.player.next"); },
    prev: function () { return appCall("musicxx.player.prev"); },
    seek: function (args) {
      return appCall("musicxx.player.seek", typeof args === "number" ? { positionMs: args } : args);
    },
    setVolume: function (value) { return appCall("musicxx.player.setVolume", { volume: value }); },
    setSpeed: function (value) { return appCall("musicxx.player.setSpeed", { speed: value }); },
    setLoopMode: function (mode) { return appCall("musicxx.player.setLoopMode", { mode: mode }); }
  };

  // 播放列表（playlist = 当前正在播的那一串歌）
  musicxx.playlist = {
    listSongs: function (args) { return appCall("musicxx.playlist.listSongs", args); },
    playSong: function (args) { return appCall("musicxx.playlist.playSong", args); },
    addSong: function (args) { return appCall("musicxx.playlist.addSong", args); },
    removeSong: function (args) { return appCall("musicxx.playlist.removeSong", args); },
    updateSong: function (args) { return appCall("musicxx.playlist.updateSong", args); }
  };

  // 歌单（songlist = 保存下来的列表，读歌要带 pid）
  musicxx.songlist = {
    list: function () { return appCall("musicxx.songlist.list"); },
    listSongs: function (args) { return appCall("musicxx.songlist.listSongs", args); },
    play: function (args) { return appCall("musicxx.songlist.play", args); },
    addSong: function (args) { return appCall("musicxx.songlist.addSong", args); },
    removeSong: function (args) { return appCall("musicxx.songlist.removeSong", args); },
    create: function (args) { return appCall("musicxx.songlist.create", args); },
    updateSong: function (args) { return appCall("musicxx.songlist.updateSong", args); },
    sync: function (args) { return appCall("musicxx.songlist.sync", args); }
  };

  /// 通用通道能力 `musicxx.feature.point.call` 的分发器
  ///
  /// 约定 (见包内 `docs/plugin-capabilities.md`):
  /// - 按 `point` 过滤: 不是自己实现的点直接返回空 (空 = 不回答, 链条继续问别人);
  /// - 不回答自己发起的调用 (`caller` 等于自己的插件 id);
  /// - 回答必须包一层 `patch` (插件框架的 firstNonNull 只在收到 `patch` / `action` 时停止遍历);
  /// - `value` 键就是"实现方给出了结果"的判据 (值可以是 null);
  /// - 处理器抛异常时给 `{error: ...}` (等于"不提供", 不打断链条)。
  function featurePointDispatch(input) {
    // 能力处理器拿到的是**已解析的载荷对象**（`ext.invokeImplementation` 做的解析）；
    // 这里同时接受 JSON 文本（直接调这个分发器时方便）。
    if (typeof input === "string") {
      try { input = JSON.parse(input || "{}"); } catch (e) { input = null; }
    }
    if (!input || typeof input !== "object" || typeof input.point !== "string") {
      return "";
    }
    var fn = featurePointImpls.get(input.point);
    if (!fn) { return ""; }
    if (input.caller && input.caller === musicxx.pluginId) { return ""; }
    var args = (input.args && typeof input.args === "object") ? input.args : {};
    var value;
    try {
      // 第二个参数给上下文（点 id 与调用方）: 与能力处理器拿到 ctx 的写法一致
      value = fn(args, { point: input.point, caller: input.caller || "" });
    } catch (e) {
      var message = (e && e.message) ? e.message : String(e);
      musicxx.host.log(3, "feature.provide: 处理器抛异常 (" + input.point + "): " + message);
      return { error: message };
    }
    if (value && typeof value.then === "function") {
      // 功能埋点是同步裁决通道: 异步处理器拿不到结果, 直接按"不回答"处理并说明原因
      musicxx.host.log(3, "feature.provide: 处理器返回了 Promise (" + input.point +
                       "); 功能埋点走同步裁决通道, 请提前算好或改成能力调用 (musicxx.capability.call)");
      return "";
    }
    // 返回**对象**: 能力处理器返回非 null 值会被 `JSON.stringify` 成裁决 JSON,
    // 只有对象才带得上 `patch`（插件框架的 firstNonNull 靠 patch 判"这一轮有人回答"）
    return { patch: { value: value === undefined ? null : value } };
  }

  /// 功能埋点的分发器与实现表：声明与调用的糖在 `musicxx.capability` 上
  /// （`points` / `define` / `undefine` / `provide` / `unprovide`），这里只有内部状态。
  ///
  /// 歌词：当前歌曲的歌词信息与内容、选拟声云歌词、改这首歌的偏移、
  /// 临时设置歌词内容、按歌词源读取、搜索云歌词。
  /// 名字统一在 `musicxx.lyric.*`（旧的 `musicxx.lyrics.*` 已删除，不保留别名）。
  musicxx.lyric = {
    current: {
      /// 当前歌曲的歌词信息和内容（`{limit?, word?}`；`index` 是 `lines` 里的下标）
      get: function (args) { return appCall("musicxx.lyric.current.get", args || {}); },
      /// 改当前歌曲的拟声云歌词（`lrcid`）与这首歌的歌词偏移（`offsetMs`，正 = 提前）
      update: function (args) { return appCall("musicxx.lyric.current.update", args || {}); },
      /// 用 LRC 文本设置当前歌词：只影响显示，不写缓存、不上传、不改歌曲绑定
      updateContent: function (lrcOrArgs) {
        var args = (typeof lrcOrArgs === "string") ? { lrc: lrcOrArgs } : (lrcOrArgs || {});
        return appCall("musicxx.lyric.current.updateContent", args);
      }
    },
    /// 按歌词源读内容：`{lrcid}` 或 `{type, src}`（`myid` / `local` / `link`）
    getBySrc: function (args) { return appCall("musicxx.lyric.getBySrc", args || {}); },
    /// 搜索拟声云歌词：`search("歌名 - 歌手")` 或 `search({name, artist})`
    search: function (keywordOrArgs, artist) {
      var args;
      if (typeof keywordOrArgs === "string") {
        args = { keyword: keywordOrArgs };
        if (artist !== undefined) { args.artist = String(artist); }
      } else {
        args = keywordOrArgs || {};
      }
      return appCall("musicxx.lyric.search", args);
    }
  };

  musicxx.storage = {
    get: function (key, defaultValue) {
      return appCall("musicxx.storage.get", { key: String(key) }).then(function (value) {
        if (value === null || value === undefined) {
          return defaultValue === undefined ? null : defaultValue;
        }
        return value;
      });
    },
    set: function (key, value) {
      return appCall("musicxx.storage.set", { key: String(key), value: value });
    },
    remove: function (key) { return appCall("musicxx.storage.delete", { key: String(key) }); },
    list: function () { return appCall("musicxx.storage.list"); },
    // 插件配置文件 config.json（插件自绘的设置页改的就是这份）
    getConfig: function (key, defaultValue) {
      return appCall("musicxx.storage.get", { key: String(key), namespace: "config" })
        .then(function (value) {
          if (value === null || value === undefined) {
            return defaultValue === undefined ? null : defaultValue;
          }
          return value;
        });
    },
    setConfig: function (key, value) {
      return appCall("musicxx.storage.set",
                          { key: String(key), value: value, namespace: "config" });
    }
  };

  /* ---- 网络能力 (插件框架内置: HTTP / TCP / WebSocket) ----
   *
   * 所有联网能力都由**插件框架自己**提供, 不再经过应用实现的能力:
   * - `musicxx.net.fetch` / `download` 与 `http.request` 同一套实现 (支持 https);
   * - http / socket / ws 是插件框架自己的 socket 实现 (http / https / ws / wss),
   *   好处是能与本地服务、TCP、WebSocket 一起用;
   * - 服务端 (http.serve / ws.listen) 只监听 http / ws —— 插件自己拷服务时不做 TLS;
   *   默认只监听 127.0.0.1, 要监听局域网得显式给 `host: "0.0.0.0"`。
   *
   * 事件统一发到主题 `plugin.<本插件id>.net`, 这里按 kind 再分给订阅者。
   * 事件先到 `musicxx.net.on(...)`, 再按前缀分给 `http.on` / `socket.on` / `ws.on`。
   */
  var netSubs = [];                // {prefix, fn}
  var netHttpWaiters = new Map();  // String(requestId) → {resolve, reject}

  function netError(event) {
    var err = new Error((event && (event.message || event.error)) || "网络操作失败");
    if (event && event.error) { err.code = String(event.error); }
    return err;
  }

  /// 同步网络操作 (op + 参数对象 → 结果对象); 失败也给对象 (`ok:false`)
  function netCall(op, args) {
    if (typeof ext.netCall !== "function") {
      return { ok: false, error: "runtime_unsupported",
               message: "这个插件框架版本没有网络能力" };
    }
    try {
      return JSON.parse(ext.netCall(String(op), safeJson(args || {})));
    } catch (e) {
      return { ok: false, error: "bad_result", message: String(e) };
    }
  }

  function netSubscribe(prefix, fn) {
    if (typeof fn !== "function") { throw new Error("net: 缺少处理函数"); }
    netSubs.push({ prefix: String(prefix || ""), fn: fn });
    return musicxx.net;
  }

  /// 发起一个异步网络操作: 先同步受理, 结果等 `http.response` 事件
  function netRequestAsync(op, options) {
    var req = (typeof options === "string") ? { url: options } : (options || {});
    return new Promise(function (resolve, reject) {
      var accepted = netCall(op, req);
      if (!accepted || !accepted.ok) { reject(netError(accepted)); return; }
      netHttpWaiters.set(String(accepted.requestId),
                         { resolve: resolve, reject: reject });
    });
  }

  /// `musicxx.net.fetch` / `http.fetch` 的结果整理
  ///
  /// 形状与以前主程序实现的 fetch 一致:
  /// `{ok, status, url, headers, contentType, bodyBytes, receivedBytes, contentLength,
  ///   truncated, body}`; `responseType: "bytes"` 时给 `bodyBase64`。
  /// 非 2xx 也算请求完成 (状态码交给插件判断); 传输/TLS 失败才 reject。
  function netFetchResult(event, responseType) {
    var out = {
      ok: true,
      status: event.status || 0,
      url: event.url || "",
      headers: event.headers || {},
      contentType: event.contentType || "",
      bodyBytes: event.bodyBytes || 0,
      receivedBytes: event.receivedBytes || 0,
      contentLength: (typeof event.contentLength === "number") ? event.contentLength : -1,
      truncated: !!event.truncated
    };
    if (responseType === "bytes") { out.bodyBase64 = event.bodyBase64 || ""; }
    else { out.body = (typeof event.body === "string") ? event.body : ""; }
    return out;
  }

  function netFetchAsync(options) {
    var req = (typeof options === "string") ? { url: options } : (options || {});
    if (!req.url) { return Promise.reject(new Error("net.fetch: 缺少 url")); }
    var responseType = (req.responseType === "bytes") ? "bytes" : "text";
    return netRequestAsync("http.fetch", req).then(function (event) {
      return netFetchResult(event, responseType);
    });
  }

  function netDownloadAsync(options) {
    var req = (typeof options === "string") ? { url: options } : (options || {});
    if (!req.url) { return Promise.reject(new Error("net.download: 缺少 url")); }
    return netRequestAsync("http.download", req).then(function (event) {
      return {
        ok: true,
        status: event.status || 0,
        path: event.path || "",
        bytes: event.bytes || 0
      };
    });
  }

  /// 事件名 → 完整 kind (`socket.on("data", fn)` → `socket.data`)
  function netKind(group, event) {
    var suffix = String(event);
    return suffix.indexOf(".") >= 0 ? suffix : (group + "." + suffix);
  }

  function handleNetEvent(jsonText) {
    var event;
    try { event = JSON.parse(jsonText); } catch (e) { return; }
    if (!event || !event.kind) { return; }
    // HTTP 客户端的 Promise (框架一定会结算: 超时/取消也会来一条)
    if (event.kind === "http.response" && event.requestId !== undefined) {
      var key = String(event.requestId);
      var waiter = netHttpWaiters.get(key);
      if (waiter) {
        netHttpWaiters.delete(key);
        if (event.ok) { waiter.resolve(event); } else { waiter.reject(netError(event)); }
      }
    }
    for (var i = 0; i < netSubs.length; i++) {
      var sub = netSubs[i];
      if (sub.prefix && event.kind.indexOf(sub.prefix) !== 0) { continue; }
      try { sub.fn(event); }
      catch (e) {
        logError("网络事件处理异常 (" + event.kind + "): " +
                 (e && e.message ? e.message : String(e)));
      }
    }
  }

  /// 插件框架把网络事件直接投递到 JS 线程 (不经事件总线, 所以不会与
  /// "返回 Promise 的能力/能力"互相等)
  ext.onNetEvent = function (json) { handleNetEvent(json); };

  // 网络：插件框架内置实现 (需 musicxx.net 权限)
  // - 支持 http / https (以及 socket / ws / wss); 服务端仍然只有 http / ws
  // - 非 2xx 也会正常返回（status 交给插件判断）；传输/TLS 失败时 reject
  musicxx.net = {
    fetch: function (options) { return netFetchAsync(options); },
    download: function (options) { return netDownloadAsync(options); },

    /* ---- 通用网络能力 (插件框架内置; 事件在 `plugin.<插件id>.net` 上) ---- */

    /// 订阅全部网络事件: fn({kind, ...})
    on: function (fn) { return netSubscribe("", fn); },
    /// 只收 HTTP 请求事件 (等价 `http.onRequest`)
    onHttpRequest: function (fn) { return netSubscribe("http.request", fn); },
    onSocketEvent: function (fn) { return netSubscribe("socket.", fn); },
    onWsEvent: function (fn) { return netSubscribe("ws.", fn); },
    /// 只收 UDP 事件 (`udp.message` / `udp.open` / `udp.closed`)
    onUdpEvent: function (fn) { return netSubscribe("udp.", fn); },
    /// 服务 / 连接 / 上限快照
    info: function () { return netCall("net.info", {}); },

    http: {
      /// HTTP 客户端请求 (Promise): {url, method?, headers?, body?, bodyBase64?,
      /// json?, timeoutMs?, maxBytes?, followRedirects?, maxRedirects?, tlsVerify?, caFile?, caPem?, serverName?}
      /// 支持 http:// 与 https:// (https 走插件框架静态链接的 OpenSSL)
      request: function (options) {
        return netRequestAsync("http.request", options);
      },
      /// 简化取正文 (Promise): 结果形状同 musicxx.net.fetch
      fetch: function (options) {
        return netFetchAsync(options);
      },
      /// 下载到插件自己的数据目录 (Promise): 结果形状同 musicxx.net.download
      download: function (options) {
        return netDownloadAsync(options);
      },
      cancel: function (requestId) {
        return netCall("http.cancel", { requestId: requestId });
      },
      /// 起一个 HTTP 服务: {id?, host?, port?, path?, staticOnly?, requestTimeoutMs?,
      /// maxBodyBytes?, maxConnections?, static?} → {ok, id, host, port, url, routes?}
      /// `static` 是静态内容清单 (命中后由框架直接回 200/206/416, 不发 request 事件):
      /// [{file, path?, contentType?, fileName?} | {bytes, bytesBase64?, ...}]
      /// `staticOnly: true` = 没命中静态路由的请求直接 404, 不发给插件
      serve: function (options) { return netCall("http.serve", options || {}); },
      /// 给已存在的服务加一条静态路由 (参数同上一条 static) → {ok, path, url, size, reused}
      route: function (options) { return netCall("http.route", options || {}); },
      /// 摘掉一条静态路由: {serverId, path} → {ok, removed}
      unroute: function (options) { return netCall("http.unroute", options || {}); },
      /// 回答一条 http.request 事件: respond(requestId, {status, headers, body})
      respond: function (requestId, response) {
        var res = (response && typeof response === "object") ? response : {};
        res.requestId = requestId;
        return netCall("http.respond", res);
      },
      stop: function (id) { return netCall("http.stop", { id: String(id || "") }); },
      info: function () { return netCall("http.info", {}); },
      /// on("request"|"listening"|"serverClosed"|"response", fn)
      on: function (event, fn) { return netSubscribe(netKind("http", event), fn); },
      onRequest: function (fn) { return netSubscribe("http.request", fn); }
    },

    socket: {
      /// TCP 客户端: {id?, host, port, timeoutMs?} → {ok, id}
      connect: function (options) {
        var req = (typeof options === "string") ? { host: options } : (options || {});
        return netCall("socket.connect", req);
      },
      /// TCP 服务: {id?, host?, port?, maxConnections?} → {ok, id, host, port}
      listen: function (options) { return netCall("socket.listen", options || {}); },
      /// send(id, "文本") 或 send({id, dataBase64})
      /// `send({id, dataBase64})` / `send({id, data})` / `send(id, "文本")` / `send(id, {dataBase64})`
      /// 注: `{id, dataBase64}` 这种"一个对象"的写法很常用（二进制转发全靠它），
      /// 解析时先认"第一个参数就是参数对象"，再认 (id, data) 两个参数的形式。
      send: function (id, data, options) {
        var req;
        if (id && typeof id === "object") {
          req = {};
          for (var key in id) { req[key] = id[key]; }
          req.id = String(id.id === undefined ? "" : id.id);
        } else {
          req = (options && typeof options === "object") ? options : {};
          req.id = String(id === undefined ? "" : id);
          if (data && typeof data === "object") {
            for (var field in data) { req[field] = data[field]; }
          } else {
            req.data = String(data === undefined ? "" : data);
          }
        }
        return netCall("socket.send", req);
      },
      close: function (id) { return netCall("socket.close", { id: String(id || "") }); },
      /// 暂停 / 恢复读: {id, paused} → {ok, id, paused}
      /// `paused: true` 之后这条连接不再给 data 事件（用于背压: 下游写不动时先暂停上游）
      pause: function (id, paused) {
        return netCall("socket.pause", {
          id: String(id || ""),
          paused: !(paused === false || paused === 0)
        });
      },
      stop: function (id) { return netCall("socket.serverStop", { id: String(id || "") }); },
      info: function () { return netCall("socket.info", {}); },
      /// on("connected"|"data"|"closed"|"accepted"|"listening"|"serverClosed", fn)
      on: function (event, fn) { return netSubscribe(netKind("socket", event), fn); },
      onEvent: function (fn) { return netSubscribe("socket.", fn); }
    },

    ws: {
      /// WebSocket 客户端: ws.connect("ws://host:port/path", {id?, headers?})
      /// 或 ws.connect({url}) / ws.connect({host, port, path})
      connect: function (url, options) {
        var req = (url && typeof url === "object") ? url : (options || {});
        if (!(url && typeof url === "object")) { req.url = String(url || ""); }
        return netCall("ws.connect", req);
      },
      /// WebSocket 服务: {id?, host?, port?, path?, maxConnections?}
      listen: function (options) { return netCall("ws.listen", options || {}); },
      /// `send(id, "文本")` / `send({id, text})` / `send({id, dataBase64})`
      send: function (id, data, options) {
        var req;
        if (id && typeof id === "object") {
          req = {};
          for (var key in id) { req[key] = id[key]; }
          req.id = String(id.id === undefined ? "" : id.id);
        } else {
          req = (options && typeof options === "object") ? options : {};
          req.id = String(id === undefined ? "" : id);
          if (data && typeof data === "object") {
            for (var field in data) { req[field] = data[field]; }
          } else {
            req.text = String(data === undefined ? "" : data);
          }
        }
        return netCall("ws.send", req);
      },
      ping: function (id, text) {
        return netCall("ws.ping", { id: String(id || ""), text: String(text || "") });
      },
      close: function (id, code, reason) {
        var req = { id: String(id || "") };
        if (typeof code === "number") { req.code = code; }
        if (typeof reason === "string") { req.reason = reason; }
        return netCall("ws.close", req);
      },
      stop: function (id) { return netCall("ws.serverStop", { id: String(id || "") }); },
      info: function () { return netCall("ws.info", {}); },
      /// on("open"|"message"|"close"|"pong"|"listening"|"serverClosed", fn)
      on: function (event, fn) { return netSubscribe(netKind("ws", event), fn); },
      onEvent: function (fn) { return netSubscribe("ws.", fn); }
    },

    /// UDP 通道：绑定端口收发数据报（广播 / 组播 / 单播都走它）
    /// - 收到包 → 事件 `udp.message`：{kind, id, host, port, bytes, dataBase64, text?, data?}
    /// - 通道关闭 → `udp.closed`：{kind, id, reason}
    udp: {
      /// 建通道: {id?, host?, port?, multicast?: "239.1.2.3" | ["239.1.2.3"], reuseAddress?}
      /// host 缺省 `0.0.0.0`（收广播 / 组播就用它），port 缺省 0 = 系统分配
      /// → {ok, id, host, port, multicast}
      open: function (options) { return netCall("udp.open", options || {}); },
      /// 发一个包: {id, host|"address"|"to", port?, data?|text?|dataBase64?}
      /// port 缺省 = 通道自己的端口；广播写 host: "255.255.255.255"
      /// 也支持 `send(id, "文本")` / `send({id, dataBase64})`
      send: function (id, data, options) {
        var req;
        if (id && typeof id === "object") {
          req = {};
          for (var key in id) { req[key] = id[key]; }
          req.id = String(id.id === undefined ? "" : id.id);
        } else {
          req = (options && typeof options === "object") ? options : {};
          req.id = String(id === undefined ? "" : id);
          if (data && typeof data === "object") {
            for (var field in data) { req[field] = data[field]; }
          } else {
            req.data = String(data === undefined ? "" : data);
          }
        }
        return netCall("udp.send", req);
      },
      /// 加入 / 退出组播组: {id, group} → {ok, id, group, multicast}
      join: function (id, group) {
        return netCall("udp.join", { id: String(id || ""), group: String(group || "") });
      },
      leave: function (id, group) {
        return netCall("udp.leave", { id: String(id || ""), group: String(group || "") });
      },
      close: function (id) { return netCall("udp.close", { id: String(id || "") }); },
      /// 当前通道清单（收发包数 / 字节数）
      info: function () { return netCall("udp.info", {}); },
      /// on("message"|"open"|"closed", fn)
      on: function (event, fn) { return netSubscribe(netKind("udp", event), fn); },
      onMessage: function (fn) { return netSubscribe("udp.message", fn); }
    }
  };

  // 等用户操作的调用用的预算（毫秒）：缺省 10 分钟，调用方可以自己给
  function waitUserBudget(timeoutMs) {
    return (typeof timeoutMs === "number" && timeoutMs > 0) ? Math.trunc(timeoutMs) : 600000;
  }

  musicxx.ui = {
    toast: function (args) {
      return appCall("musicxx.ui.toast", typeof args === "string" ? { text: args } : args);
    },
    // 确认弹窗 (敏感操作前先问用户; 返回 { ok, confirmed })
    // * 要等用户点，所以缺省预算给 10 分钟（musicxx.call 的缺省 5 秒不够）
    dialog: function (args, timeoutMs) {
      return appCall("musicxx.ui.dialog", typeof args === "string" ? { content: args } : args,
                          waitUserBudget(timeoutMs));
    },
    // 浮层 (代码打开一层 sheet, 等用户关掉它; 返回 { ok, reason, value? })
    // - args: { preset, id, title, depict, blocks, options, value, textConfirm, textCancel, size, dismissible }
    // - 参数全部同能力 musicxx.ui.overlay.show，页面里的块也可以用 command 动作指向它
    overlay: function (args, timeoutMs) {
      return appCall("musicxx.ui.overlay.show", args || {}, waitUserBudget(timeoutMs));
    },
    // 选择浮层 (overlay 的 preset=picker: 标题 + 多行描述; 返回 { ok, selected } / { ok, cancelled })
    pick: function (args, timeoutMs) {
      var req = {};
      var src = args || {};
      for (var key in src) { req[key] = src[key]; }
      req.preset = "picker";
      return appCall("musicxx.ui.overlay.show", req, waitUserBudget(timeoutMs));
    },
    // 主动收起这个插件当前那一层浮层 (返回 { ok, closed })
    overlayClose: function (args) {
      var id = (typeof args === "string") ? args : ((args && args.id) ? String(args.id) : "");
      return appCall("musicxx.ui.overlay.close", { id: id });
    },
    // 打开页面: 任意插件的声明式页面 (ext://<插件id>/<视图id>, 含本插件), 或官方白名单页面
    // (见作者文档; 跨插件页面由对方插件自己绘制)
    openRoute: function (route, args) {
      return appCall("musicxx.ui.openRoute", { route: String(route), arguments: args });
    },
    // 更换全局字体 (支持设置系统字体名, 或传入 'default'/null/'' 恢复默认)
    setFontFamily: function (args) {
      var req = (typeof args === "string") ? { fontFamily: args } : (args || {});
      return appCall("musicxx.ui.setFontFamily", req);
    },
    // 获取当前生效的全局字体与默认字体信息
    getFontFamily: function () {
      return appCall("musicxx.ui.getFontFamily", {});
    },
    // 动态加载并注册字体文件 (支持插件目录相对路径 path 或 base64 data, 默认立即应用)
    loadFont: function (options) {
      return appCall("musicxx.ui.loadFont", options || {});
    },
    registerEntry: function (spec) {
      var entry = normalizeEntry(spec);
      uiTable.set(entry.name, { type: entry.type, order: entry.order, data: entry.data });
      ext.uiOp("register", entry.name, entry.type, safeJson(entry.data), entry.order);
      return musicxx;
    },
    updateEntry: function (name, data) {
      assertName(name, "ui.updateEntry: 名称");
      var key = shortName(name);
      var previous = uiTable.get(key);
      var type = previous ? previous.type : "";
      if (previous) { previous.data = data; }
      ext.uiOp("update", key, type, safeJson(data), 0);
      return musicxx;
    },
    unregisterEntry: function (name) {
      assertName(name, "ui.unregisterEntry: 名称");
      var key = shortName(name);
      uiTable.delete(key);
      ext.uiOp("unregister", key, "", "{}", 0);
      return musicxx;
    },
    entries: function () {
      var list = [];
      uiTable.forEach(function (value, name) {
        list.push({ id: "plugin." + musicxx.pluginId + "." + name, name: name,
                    type: value.type, order: value.order, data: value.data });
      });
      return list;
    }
  };

  // 渲染槽位 (插件渲染的背景等): 查询可用样式 / 切换样式 (允许切到主程序内置样式)
  // - 未带 slot 时用插件框架默认槽位 (当前是 player.background 播放页背景);
  // - select 只在"该项可用"时成功; 失败返回 { ok:false, error }.
  musicxx.render = {
    list: function (args) {
      return appCall("musicxx.render.list", { slot: (args && args.slot) ? String(args.slot) : undefined });
    },
    current: function (args) {
      return appCall("musicxx.render.current", { slot: (args && args.slot) ? String(args.slot) : undefined });
    },
    select: function (id, args) {
      var req = (args && typeof args === "object") ? args : {};
      req.id = String(id);
      return appCall("musicxx.render.select", req);
    }
  };

  // 封面数据 (只作为颜色/数据来源, 不参与画面绘制; 插件框架不推任何直链)
  musicxx.media = {
    // 封面颜色分析结果 + 主程序内置背景实际使用的 4 色
    palette: function (args) {
      var req = (args && typeof args === "object") ? args : {};
      return appCall("musicxx.media.palette", req);
    },
    // 写封面取色 (与内置分析是同一批字段): {main, light, lightMuted, dark, darkMuted}
    setPalette: function (args) {
      var req = (args && typeof args === "object") ? args : {};
      return appCall("musicxx.media.palette.set", req);
    },
    // 封面字节 (jpeg/png/rgba, base64 放在 data 字段; size 16..512);
    // args.bind 非空时同时把这次解码出来的封面绑成纹理 (渲染项 image 引用那个 key)
    cover: function (args) {
      var req = (args && typeof args === "object") ? args : {};
      return appCall("musicxx.media.cover", req);
    },
    // 把插件自己的图片绑成纹理: {key, data(base64), format: png|jpeg|rgba, width?, height?}
    bindImage: function (args) {
      var req = (args && typeof args === "object") ? args : {};
      return appCall("musicxx.media.bindImage", req);
    },
    // 释放之前绑定的图片纹理: {key}
    unbindImage: function (args) {
      var req = (args && typeof args === "object") ? args : {};
      return appCall("musicxx.media.unbindImage", req);
    },
    // 当前音频频谱 (内置『音乐动效』提取的数据):
    //   args.bandCount 1..64 且能整除 256 (缺省 16)
    //   args.unit      normalized(缺省) | db | raw
    //   args.includeBins 是否附带当前帧的 256 个频点
    // 没有数据不是错误: 结果里的 status / available / loading / reason 说明原因
    spectrum: function (args) {
      var req = (args && typeof args === "object") ? args : {};
      return appCall("musicxx.media.spectrum", req);
    },
    // 播放内核信息 (mpv / ffmpeg 版本、能写进 af/vf 的过滤器、解码器、认得的音视频后缀):
    //   args.part     all(缺省) | versions | filters | decoders | formats (只影响 text)
    //   args.refresh  重新采集而不是用缓存
    // 返回 {ok, kernel, text}; 判断某个过滤器名能不能写进 af 用 kernel.audioFilters
    kernel: function (args) {
      var req = (args && typeof args === "object") ? args : {};
      return appCall("musicxx.media.kernel", req);
    }
  };

  musicxx.stats = {
    getSelf: function () {
      try { return JSON.parse(ext.stats() || "{}"); } catch (e) { return {}; }
    },
    reportMemory: function (bytes) {
      return appCall("musicxx.stats.reportMemory", { bytes: Number(bytes) || 0 });
    },
    reportMetric: function (name, value) {
      return appCall("musicxx.stats.reportMetric", { name: String(name), value: value });
    }
  };

  ext.onEvent = function (topic, json) {
    var list = broadcastSubs.get(topic);
    if (!list || list.length === 0) { return; }
    var payload = {};
    if (json) { try { payload = JSON.parse(json); } catch (e) { payload = {}; } }
    var info = { id: topic };
    var snapshot = list.slice();
    for (var i = 0; i < snapshot.length; i++) {
      try {
        snapshot[i].fn(payload, info);
      } catch (e) {
        logError("广播回调异常 (" + topic + "): " + (e && e.message ? e.message : String(e)));
      }
    }
  };

  /// 流结束: 这个频道上的订阅全部失效（宿主已经摘掉订阅），逐个回调 onEnd
  ext.onEventEnd = function (topic, json) {
    var list = broadcastSubs.get(topic);
    broadcastSubs.delete(topic);
    // 宿主侧订阅已随结束被摘掉: 清掉记账，之后重新 declare + subscribe 才能再建
    var stale = [];
    broadcastHostSubs.forEach(function (value, key) {
      if (key.indexOf(topic + "\u0000") === 0) { stale.push(key); }
    });
    for (var s = 0; s < stale.length; s += 1) { broadcastHostSubs.delete(stale[s]); }
    if (!list || list.length === 0) { return; }
    var reason = {};
    if (json) { try { reason = JSON.parse(json); } catch (e) { reason = {}; } }
    var info = { id: topic };
    var snapshot = list.slice();
    for (var i = 0; i < snapshot.length; i += 1) {
      if (typeof snapshot[i].onEnd !== "function") { continue; }
      try {
        snapshot[i].onEnd(reason, info);
      } catch (e) {
        logError("广播结束回调异常 (" + topic + "): " + (e && e.message ? e.message : String(e)));
      }
    }
  };

  musicxx.capability = {
    /// 给应用声明的能力（`musicxx.*`，含能力）提供实现
    ///
    /// `implement(id, {phase, priority, ownerTag}, fn)`
    /// - 同一条能力上可以分别实现三个阶段（同一个 ownerTag 互不覆盖）；
    /// - 观察（`mode: "observe"`）在框架 API 2 之后改用广播订阅：用 `musicxx.broadcast.subscribe`，
    ///   这里再写 `mode: "observe"` 会被拒绝。
    implement: function (id, options, fn) {
      assertName(id, "capability.implement: 能力 id");
      if (typeof options === "function") { fn = options; options = null; }
      options = options || {};
      if (options.mode === "observe") {
        throw new Error("capability.implement: 观察已改成广播订阅，请用 musicxx.broadcast.subscribe");
      }
      if (typeof fn !== "function") { throw new Error("capability.implement: 缺少处理函数"); }
      var entry = {
        id: String(id),
        phase: normalizePhase(options.phase),
        priority: Number.isFinite(options.priority) ? Math.trunc(options.priority) : 0,
        ownerTag: typeof options.ownerTag === "string" ? options.ownerTag : "",
        fn: fn
      };
      implementationTable.set(implementationKey(entry.id, entry.phase), entry);
      /// 运行期登记要通知插件框架 (顶层登记由引擎统一回放, 这里只是先记下来)
      ext.implementationOp("register", entry.id, entry.priority, entry.ownerTag, entry.phase);
      return musicxx;
    },
    /// 注销实现：`unregister(id)` 摘掉这条能力上自己的全部阶段；
    /// `unregister(id, phase)` 只摘那个阶段（插件框架按 (能力, ownerTag) 摘）。
    unregister: function (id, phase) {
      var key = String(id);
      var wantPhase = (phase === undefined || phase === null) ? "" : normalizePhase(phase);
      var keys = [];
      implementationTable.forEach(function (entry, k) {
        if (entry.id !== key) { return; }
        if (wantPhase && entry.phase !== wantPhase) { return; }
        keys.push(k);
      });
      var tags = [];
      keys.forEach(function (k) {
        var entry = implementationTable.get(k);
        implementationTable.delete(k);
        if (entry && tags.indexOf(entry.ownerTag) < 0) { tags.push(entry.ownerTag); }
      });
      tags.forEach(function (tag) {
        ext.implementationOp("unregister", key, 0, tag, wantPhase);
      });
      return musicxx;
    },
    /// 自己有没有在这条能力上登记 (phase 缺省 = impl)
    has: function (id, phase) {
      return implementationTable.has(implementationKey(String(id), normalizePhase(phase)));
    },
    /// 读某条能力当前的实现行 (自省: 谁提供、在哪个阶段、排第几)。
    /// 返回 Promise: {capability, count, handlers:[{plugin, instance, kind, phase, priority, ownerTag}]}
    handlers: function (id, phase) {
      assertName(id, "capability.handlers: 能力 id");
      var args = { id: String(id) };
      if (phase !== undefined && phase !== null) { args.phase = normalizePhase(phase); }
      return appCall("musicxx.capability.subscribers", args);
    },
    register: function (name, fn) {
      assertName(name, "capability.register: 名称");
      if (typeof fn !== "function") { throw new Error("capability.register: 缺少处理函数"); }
      capTable.set(name, fn);
      return undefined;
    },
    call: function (pluginId, name, args, timeoutMs) {
      assertName(pluginId, "capability.call");
      /// 应用侧的能力：第一个参数直接写能力 id（`musicxx.<域>.<名字>`, 或插件声明、
      /// 应用侧回答的 `plugin.<插件id>.<名字>`），后面是 `(id, args?, timeoutMs?)`。
      /// 应用能力与功能埋点都是能力，调用方不需要区分——应用侧按 id 自己找。
      /// 不以这两类前缀开头的才是跨插件调用（第一节参数是插件 id）。
      if (pluginId.indexOf("musicxx.") === 0 || pluginId.indexOf("plugin.") === 0) {
        var appArgs = (name === undefined || name === null) ? {} : name;
        var appBudget = (typeof args === "number") ? args
                       : ((typeof timeoutMs === "number") ? timeoutMs : undefined);
        return appCall(pluginId, appArgs, appBudget);
      }
      assertName(name, "capability.call: 能力名");
      // 超时是调用方自己的选择: 不传 = 3 秒; 传 0 = 一直等
      var budget = Number.isFinite(timeoutMs) ? Math.trunc(timeoutMs) : 3000;
      if (budget < 0) { budget = 0; }
      var text = ext.capabilityCall(String(pluginId), String(name),
                                    safeJson(args === undefined ? {} : args), budget);
      var info = null;
      try { info = JSON.parse(text || "{}"); } catch (e) { info = null; }
      if (!info || typeof info.status !== "string") {
        return Promise.reject(new Error("跨插件调用失败: 插件框架返回了非法结果"));
      }
      if (info.status === "ok") {
        return Promise.resolve(info.result === undefined ? null : info.result);
      }
      if (info.status === "pending") {
        // 原生/内置目标: 结果稍后经 ext.onCapabilityResult 回到 JS 线程
        return new Promise(function (resolve, reject) {
          var waiter = { resolve: resolve, reject: reject, timer: 0 };
          // 不传超时 / 传 0 时不设本地备用定时器 (一直等结果)
          if (budget > 0) {
            waiter.timer = musicxx.timer.setTimeout(function () {
              if (capabilityWaiters.delete(info.id)) {
                reject(new Error("跨插件调用超时: " + pluginId + "." + name));
              }
            }, budget + 1000);
          }
          capabilityWaiters.set(info.id, waiter);
        });
      }
      return Promise.reject(new Error("跨插件调用失败: " + (info.error || "capability_call_failed")));
    },
    /// 能力清单：应用留出了哪些能力、现在谁在实现它（谁生效、谁备选、谁把它关掉了）。
    ///
    /// `points()` 看全部；`points({id: "..."})` 看一个；`points({provided: true})` 只看有实现的。
    /// 每条的 `callDoc` 写着“参数与结果形状”（调它之前先看这一行）。
    points: function (options) {
      options = options || {};
      var args = {};
      if (options.id) { args.id = String(options.id); }
      if (options.provided !== undefined) { args.provided = !!options.provided; }
      return appCall("musicxx.feature.point.list", args);
    },
    /// 声明一个**自己的**能力（`id` 必须形如 `plugin.<自己的插件 id>.<名字>`）
    ///
    /// - `define({id, title, depict, args, result})` 或 `define(id, {title, ...})`；
    /// - 同一 id 重复声明 = 覆盖（最后一次生效）；
    /// - 声明跟着插件生命周期走：禁用 / 卸载会自动撤销，重新启用时脚本会再声明一遍；
    /// - 声明之后用 `provide(id, fn)` 给出实现，别的插件与应用就能用
    ///   `musicxx.capability.call(id, args)` 调它。
    define: function (specOrId, spec) {
      var full;
      if (specOrId && typeof specOrId === "object") {
        full = Object.assign({}, specOrId);
      } else {
        full = Object.assign({}, spec || {});
        full.id = specOrId;
      }
      if (!full.id) { throw new Error("capability.define: 缺少能力 id"); }
      assertName(full.id, "capability.define: 能力 id");
      return appCall("musicxx.feature.point.define", {
        id: String(full.id),
        title: full.title === undefined ? "" : String(full.title),
        depict: full.depict === undefined ? "" : String(full.depict),
        args: full.args === undefined ? "" : String(full.args),
        result: full.result === undefined ? "" : String(full.result)
      });
    },
    /// 收回自己声明的能力（一般不用调：停用 / 卸载会自动撤销）
    undefine: function (id) {
      assertName(id, "capability.undefine: 能力 id");
      return appCall("musicxx.feature.point.undefine", { id: String(id) });
    },
    /// 登记自己声明的能力的实现：`provide(id, fn)` / `provide([id1, id2], fn)`
    ///
    /// - `fn(args)` **同步返回**结果（对象原样给，其它类型包成 `{value: …}`）；
    /// - 不是自己的点、或调用方就是自己时，糖不会回答（分发器按 `point` 与 `caller` 过滤）；
    /// - 这条通道是同步裁决型能力：`fn` 返回 Promise 拿不到结果（会记一条日志），
    ///   长任务请提前算好，或者改成普通能力实现（`musicxx.capability.implement`）。
    provide: function (idOrIds, fn, options) {
      if (typeof fn !== "function") {
        throw new Error("capability.provide: 缺少处理函数");
      }
      options = options || {};
      var ids = Array.isArray(idOrIds) ? idOrIds : [idOrIds];
      if (ids.length === 0) { throw new Error("capability.provide: 缺少能力 id"); }
      ids.forEach(function (id) {
        assertName(id, "capability.provide: 能力 id");
        featurePointImpls.set(String(id), fn);
      });
      if (!implementationTable.has(implementationKey("musicxx.feature.point.call", "impl"))) {
        musicxx.capability.implement("musicxx.feature.point.call", {
          priority: Number.isFinite(options.priority) ? Math.trunc(options.priority) : 0,
          ownerTag: ids.map(String).join(",")
        }, function (ctx) { return featurePointDispatch(ctx); });
      }
      return musicxx;
    },
    /// 摘除实现（给了 `fn` 时只有“就是它”才摘）
    unprovide: function (id, fn) {
      var key = String(id);
      if (arguments.length > 1 && featurePointImpls.get(key) !== fn) {
        return musicxx;
      }
      featurePointImpls.delete(key);
      if (featurePointImpls.size === 0 && implementationTable.has(implementationKey("musicxx.feature.point.call", "impl"))) {
        musicxx.capability.unregister("musicxx.feature.point.call");
      }
      return musicxx;
    }
  };
  ext.onCapabilityResult = function (id, json) {
    var key = Number(id);
    var waiter = capabilityWaiters.get(key);
    if (!waiter) { return; }
    capabilityWaiters.delete(key);
    if (waiter.timer) { musicxx.timer.clear(waiter.timer); }
    var info = null;
    try { info = JSON.parse(json || "{}"); } catch (e) { info = null; }
    if (info && info.ok === true) {
      waiter.resolve(info.result === undefined ? null : info.result);
    } else {
      var reason = (info && info.error) ? info.error : "capability_call_failed";
      waiter.reject(new Error("跨插件调用失败: " + reason));
    }
  };
  ext.invokeCapability = function (name, argsJson, waitId) {
    var fn = capTable.get(name);
    if (!fn) { return JSON.stringify({ ok: false, error: "plugin_capability_not_found: " + name }); }
    var args = {};
    if (argsJson) { try { args = JSON.parse(argsJson); } catch (e) { args = {}; } }
    var result;
    try { result = fn(args); }
    catch (e) { return JSON.stringify({ ok: false, error: (e && e.message ? e.message : String(e)) }); }
    if (result && typeof result.then === "function") {
      // 异步能力处理器: 记下等待号, Promise 结算后经 capabilityWaitResolve 回结果
      var key = (waitId === undefined || waitId === null || waitId === "") ? "" : String(waitId);
      if (!key) { return JSON.stringify({ ok: false, error: "capability_async_without_wait" }); }
      pendingCapabilityWaits.set(key, true);
      Promise.resolve(result).then(function (value) {
        if (!pendingCapabilityWaits.has(key)) { return null; }
        pendingCapabilityWaits.delete(key);
        ext.capabilityWaitResolve(key, JSON.stringify({ ok: true, result: value === undefined ? null : value }));
        return null;
      }, function (e) {
        if (!pendingCapabilityWaits.has(key)) { return null; }
        pendingCapabilityWaits.delete(key);
        ext.capabilityWaitResolve(key, JSON.stringify({ ok: false, error: (e && e.message ? e.message : String(e)) }));
        return null;
      });
      return kCapabilityPendingMarker;
    }
    return JSON.stringify({ ok: true, result: result === undefined ? null : result });
  };

  musicxx.timer = {
    _add: function (fn, ms, repeat) {
      if (typeof fn !== "function") { throw new Error("timer: 缺少处理函数"); }
      var id = ext.timerSet(Number.isFinite(ms) ? Math.max(0, Math.trunc(ms)) : 0, repeat);
      if (typeof id === "number" && id >= 0) {
        // 定时器表用**字符串键**: 插件框架回调 `onTimerFire` 传进来的 id 是字符串
        // (数字键会让查找落空 → 回调永远不执行)
        timerTable.set(String(id), { fn: fn, repeat: repeat });
      }
      return id;
    },
    setTimeout: function (fn, ms) { return musicxx.timer._add(fn, ms, false); },
    setInterval: function (fn, ms) { return musicxx.timer._add(fn, ms, true); },
    clear: function (id) {
      ext.timerClear(Number(id));
      timerTable.delete(String(id));
    },
    clearTimeout: function (id) { musicxx.timer.clear(id); },
    clearInterval: function (id) { musicxx.timer.clear(id); }
  };
  ext.onTimerFire = function (id) {
    var entry = timerTable.get(String(id));
    if (!entry) { return; }
    if (!entry.repeat) { timerTable.delete(String(id)); }
    try { entry.fn(); }
    catch (e) { logError("定时器异常: " + (e && e.message ? e.message : String(e))); }
  };

  function pad2(value) { return value < 10 ? "0" + value : String(value); }

  /// 编码转换与 base64（原生层: utilxx_base 的 iconv + base64）
  ///
  /// FTP / 局域网共享这类协议的名字常常不是 UTF-8，QuickJS 没有 TextDecoder，
  /// 所以转换放到原生层做。都是**同步**返回（目录列表要当场解出文件名）。
  function textCodecResult(text) {
    var res = null;
    try { res = JSON.parse(text || "{}"); } catch (e) { res = null; }
    if (!res || typeof res !== "object") {
      return { ok: false, error: "bad_result" };
    }
    return res;
  }

  musicxx.util = {
    json: JSON,
    urlEncode: function (text) { return encodeURIComponent(String(text)); },
    urlDecode: function (text) { return decodeURIComponent(String(text)); },
    formatTime: function (ms) {
      var total = Math.max(0, Math.floor(Number(ms) / 1000));
      var hours = Math.floor(total / 3600);
      var minutes = Math.floor((total % 3600) / 60);
      var seconds = total % 60;
      return hours > 0 ? (hours + ":" + pad2(minutes) + ":" + pad2(seconds))
                       : (minutes + ":" + pad2(seconds));
    },
    now: function () { return Date.now(); },

    /// 文本 → 指定编码的 base64: {text, encoding?} → {ok, base64, bytes}
    /// encoding 缺省 utf-8；`gbk` / `gb18030` / `big5` … 交给原生 iconv
    textEncode: function (options) {
      var o = (options && typeof options === "object") ? options : {};
      var text = (o.text === undefined || o.text === null) ? "" : String(o.text);
      var encoding = (typeof o.encoding === "string" && o.encoding) ? o.encoding : "utf-8";
      return textCodecResult(ext.textEncode(text, encoding));
    },

    /// base64 字节（可以是字符串或字符串数组）→ UTF-8 文本: {base64, encoding?} → {ok, text}
    /// 分片给数组时由原生层先拼字节再转编码（直接拼 base64 文本会因 padding 出错）
    textDecode: function (options) {
      var o = (options && typeof options === "object") ? options : {};
      var chunks = o.base64;
      if (!Array.isArray(chunks)) {
        chunks = [(chunks === undefined || chunks === null) ? "" : String(chunks)];
      }
      var encoding = (typeof o.encoding === "string" && o.encoding) ? o.encoding : "utf-8";
      return textCodecResult(ext.textDecode(JSON.stringify(chunks), encoding));
    },

    /// UTF-8 文本 → base64: {text} → {ok, base64}
    base64Encode: function (options) {
      var o = (options && typeof options === "object") ? options : {};
      var text = (o.text === undefined || o.text === null) ? "" : String(o.text);
      return textCodecResult(ext.textEncode(text, "utf-8"));
    },

    /// base64 → UTF-8 文本: {base64} → {ok, text}
    base64Decode: function (options) {
      var o = (options && typeof options === "object") ? options : {};
      var chunks = o.base64;
      if (!Array.isArray(chunks)) {
        chunks = [(chunks === undefined || chunks === null) ? "" : String(chunks)];
      }
      return textCodecResult(ext.textDecode(JSON.stringify(chunks), "utf-8"));
    }
  };

  /* ---- 插件框架回调入口 (native 在 JS 线程上调用) ---- */

  ext.collectRegistrations = function () {
    var implementations = [];
    implementationTable.forEach(function (entry) {
      implementations.push({
        capability: entry.id,
        phase: entry.phase,
        mode: entry.mode,
        priority: entry.priority,
        ownerTag: entry.ownerTag
      });
    });
    var capabilities = [];
    capTable.forEach(function (value, name) { capabilities.push(name); });
    var subscriptions = [];
    broadcastSubs.forEach(function (list, topic) {
      for (var i = 0; i < list.length; i += 1) {
        subscriptions.push({ topic: topic, options: list[i].options });
      }
    });
    var broadcasts = [];
    declaredBroadcasts.forEach(function (decl) { broadcasts.push(decl); });
    return JSON.stringify({
      implementations: implementations,
      capabilities: capabilities,
      subscriptions: subscriptions,
      broadcasts: broadcasts
    });
  };

  /// 调用能力处理器
  ///
  /// - `waitId` 只有裁决型能力才有 (插件框架一直等到结果); 广播频道为空字符串/undefined;
  /// - 处理器返回 Promise 时: 有 `waitId` 就登记等待, 结算后经 `ext.implementationWaitResolve` 交回结果;
  ///   没有 `waitId` 就让 Promise 正常跑完, 结果丢弃 (通知型本来就不看返回值);
  /// - 插件框架不设等待超时: Promise 结算多晚都会被接住。
  ext.invokeImplementation = function (capabilityId, phase, inputJson, waitId) {
    var entry = implementationTable.get(implementationKey(capabilityId, phase || "impl"));
    if (!entry) { return ""; }
    var ctx = {};
    if (inputJson) { try { ctx = JSON.parse(inputJson); } catch (e) { ctx = {}; } }
    var result;
    try { result = entry.fn(ctx); }
    catch (e) {
      logError("能力 " + capabilityId + " 异常: " + (e && e.message ? e.message : String(e)));
      return "";
    }
    if (result === null || result === undefined) { return ""; }
    if (typeof result === "object" && typeof result.then === "function") {
      var key = (waitId === undefined || waitId === null || waitId === "") ? "" : String(waitId);
      if (!key) {
        Promise.resolve(result).then(function () { return null; }, function (e) {
          logError("能力 " + capabilityId + " 异步执行异常: " + (e && e.message ? e.message : String(e)));
        });
        return "";
      }
      pendingImplementationWaits.set(key, true);
      Promise.resolve(result).then(function (value) {
        if (!pendingImplementationWaits.has(key)) { return null; }
        pendingImplementationWaits.delete(key);
        ext.implementationWaitResolve(key, (value === null || value === undefined) ? "" : safeJson(value));
        return null;
      }, function (e) {
        if (!pendingImplementationWaits.has(key)) { return null; }
        pendingImplementationWaits.delete(key);
        logError("能力 " + capabilityId + " 异步裁决失败: " + (e && e.message ? e.message : String(e)));
        ext.implementationWaitResolve(key, "");
        return null;
      });
      return kPendingMarker;
    }
    try { return JSON.stringify(result); } catch (e) { return ""; }
  };

  ext.reset = function () {
    implementationTable.clear();
    capTable.clear();
    uiTable.clear();
    timerTable.clear();
    pendingImplementationWaits.clear();
    pendingCapabilityWaits.clear();
    declaredBroadcasts.clear();
    broadcastSubs.clear();
    broadcastHostSubs.clear();
    appCapabilityWaiters.forEach(function (waiter) {
      try { waiter.reject(new Error("插件已停止")); } catch (e) { /* 忽略 */ }
    });
    appCapabilityWaiters.clear();
    capabilityWaiters.forEach(function (waiter) {
      try { waiter.reject(new Error("插件已停止")); } catch (e) { /* 忽略 */ }
    });
    capabilityWaiters.clear();
    netSubs.length = 0;
    netHttpWaiters.forEach(function (waiter) {
      try { waiter.reject(new Error("插件已停止")); } catch (e) { /* 忽略 */ }
    });
    netHttpWaiters.clear();
  };

  /* ---- console ---- */
  var consoleShim = {};
  ["log", "info", "warn", "error"].forEach(function (level, index) {
    consoleShim[level] = function () {
      var parts = [];
      for (var i = 0; i < arguments.length; i++) {
        var item = arguments[i];
        if (typeof item === "string") { parts.push(item); continue; }
        try { parts.push(JSON.stringify(item)); } catch (e) { parts.push(String(item)); }
      }
      ext.log(index === 3 ? 4 : (index === 2 ? 3 : 2), parts.join(" "));
    };
  });
  globalThis.console = consoleShim;

  /* ---- 便捷全局 (与 musicxx.timer 同一实现) ---- */
  globalThis.setTimeout = musicxx.timer.setTimeout;
  globalThis.setInterval = musicxx.timer.setInterval;
  globalThis.clearTimeout = musicxx.timer.clearTimeout;
  globalThis.clearInterval = musicxx.timer.clearInterval;

  Object.defineProperty(globalThis, "__ext", {
    value: ext, writable: false, enumerable: false, configurable: false
  });
  globalThis.musicxx = musicxx;
})();
)JS";

#if defined(MUSICXX_EXTERN_PLUGIN_HAS_JS)

/* ==================== C 桥接原语 ==================== */

/// 取当前上下文的实例 (上下文创建时写入 opaque)
JsEngine::Instance *contextInstance(JSContext *ctx) {
  return static_cast<JsEngine::Instance *>(JS_GetContextOpaque(ctx));
}

/// 取回脚本异常文本
std::string takeException(JSContext *ctx) {
  JSValue exception = JS_GetException(ctx);
  std::string text;
  std::string stack;
  if (JS_IsObject(exception)) {
    JSValue message = JS_GetPropertyStr(ctx, exception, "message");
    const char *cstr = JS_ToCString(ctx, message);
    if (cstr) {
      text = cstr;
      JS_FreeCString(ctx, cstr);
    }
    JS_FreeValue(ctx, message);
    // 栈的第一行就是出错位置 (`at <文件>:<行>`): 带上它, 作者不用猜是哪一行
    JSValue trace = JS_GetPropertyStr(ctx, exception, "stack");
    if (JS_IsString(trace)) {
      const char *traceText = JS_ToCString(ctx, trace);
      if (traceText) {
        stack = traceText;
        JS_FreeCString(ctx, traceText);
      }
    }
    JS_FreeValue(ctx, trace);
  }
  if (text.empty()) {
    const char *cstr = JS_ToCString(ctx, exception);
    if (cstr) {
      text = cstr;
      JS_FreeCString(ctx, cstr);
    }
  }
  JS_FreeValue(ctx, exception);
  if (!stack.empty() && stack.find(text) == std::string::npos) {
    const auto lineEnd = stack.find('\n');
    text += " (" + stack.substr(0, lineEnd) + ")";
  }
  return text;
}

JSValue jsHostInfo(JSContext *ctx, JSValueConst, int /*argc*/,
                   JSValueConst * /*argv*/) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine) {
    return JS_NewString(ctx, "{}");
  }
  return JS_NewString(ctx, inst->engine->bridgeHostInfo().c_str());
}

JSValue jsLog(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine) {
    return JS_UNDEFINED;
  }
  int32_t level = 2;
  if (argc >= 1) {
    int32_t parsed = 2;
    if (JS_ToInt32(ctx, &parsed, argv[0]) == 0) {
      level = parsed;
    }
  }
  std::string message;
  if (argc >= 2) {
    const char *text = JS_ToCString(ctx, argv[1]);
    if (text) {
      message = text;
      JS_FreeCString(ctx, text);
    }
  }
  inst->engine->bridgeLog(inst->id, level, message);
  return JS_UNDEFINED;
}

JSValue jsConfigPath(JSContext *ctx, JSValueConst, int /*argc*/,
                     JSValueConst * /*argv*/) {
  auto *inst = contextInstance(ctx);
  return JS_NewString(ctx, inst ? inst->configPath.c_str() : "");
}

/// 网络能力: 同步操作 (op + 参数 JSON → 结果 JSON)
JSValue jsNetCall(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine) {
    return JS_NewString(
        ctx,
        R"({"ok":false,"error":"no_instance","message":"插件实例已停止"})");
  }
  const char *op = (argc >= 1) ? JS_ToCString(ctx, argv[0]) : nullptr;
  const char *args = (argc >= 2) ? JS_ToCString(ctx, argv[1]) : nullptr;
  const std::string json = inst->engine->bridgeNetCall(
      inst->name, op ? op : "", args ? args : "{}");
  if (op) {
    JS_FreeCString(ctx, op);
  }
  if (args) {
    JS_FreeCString(ctx, args);
  }
  return JS_NewStringLen(ctx, json.data(), json.size());
}

/// 网络能力: 取消进行中的 HTTP 请求
JSValue jsNetCancel(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine) {
    return JS_NewString(
        ctx,
        R"({"ok":false,"error":"no_instance","message":"插件实例已停止"})");
  }
  int64_t requestId = 0;
  if (argc >= 1) {
    JS_ToInt64(ctx, &requestId, argv[0]);
  }
  const std::string json = inst->engine->bridgeNetCancel(inst->name, requestId);
  return JS_NewStringLen(ctx, json.data(), json.size());
}

/// 受理一次应用能力调用（`ext.appCapabilityCall`）
///
/// 参数: `(能力 id, 参数 JSON, 超时毫秒)`；返回请求号（`< 0` = 未受理）。
/// 结果经 `ext.onAppCapabilityDone` 回到 JS 线程。
JSValue jsAppCapabilityCall(JSContext *ctx, JSValueConst, int argc,
                            JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine || !inst->hostInst || argc < 1) {
    return JS_NewInt32(ctx, -1);
  }
  const char *name = JS_ToCString(ctx, argv[0]);
  const char *args = (argc >= 2) ? JS_ToCString(ctx, argv[1]) : nullptr;
  uint32_t timeoutMs = 5000;
  if (argc >= 3) {
    int32_t parsed = 0;
    if (JS_ToInt32(ctx, &parsed, argv[2]) == 0 && parsed > 0) {
      timeoutMs = static_cast<uint32_t>(parsed);
    }
  }
  int64_t requestId = -1;
  const auto rc =
      inst->engine->requestAppCapability(inst->hostInst, name ? name : "",
                                  args ? args : "{}", timeoutMs, &requestId);
  if (name) {
    JS_FreeCString(ctx, name);
  }
  if (args) {
    JS_FreeCString(ctx, args);
  }
  return JS_NewInt32(ctx, rc == 0 ? static_cast<int32_t>(requestId)
                                  : static_cast<int32_t>(rc));
}

JSValue jsPublish(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine || argc < 1) {
    return JS_NewString(ctx, "参数非法");
  }
  const char *topic = JS_ToCString(ctx, argv[0]);
  const char *data = (argc >= 2) ? JS_ToCString(ctx, argv[1]) : nullptr;
  const auto rc = inst->engine->bridgePublish(inst->id, topic ? topic : "",
                                              data ? data : "{}");
  if (topic) {
    JS_FreeCString(ctx, topic);
  }
  if (data) {
    JS_FreeCString(ctx, data);
  }
  if (rc == 0) {
    return JS_NewString(ctx, "");
  }
  if (rc == PLUGINXX_RESULT_ERR_PERMISSION) {
    return JS_NewString(ctx, "事件发布被拒绝 (只有属主能发布这条频道)");
  }
  // 广播表把"流已结束"与"频道不存在"归到同一个码上 (见 broadcastPublishFrom):
  // 已结束的频道终归发不出去, 文案把两种可能都说清楚
  return JS_NewString(ctx, "事件发布被拒绝 (频道不存在, 或这条流已经被属主结束)");
}

/// 结束本插件的一条流: 返回空串 = 已结束, 否则是给脚本的错误文本
JSValue jsEndBroadcast(JSContext *ctx, JSValueConst, int argc,
                       JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine || argc < 1) {
    return JS_NewString(ctx, "参数非法");
  }
  const char *topic = JS_ToCString(ctx, argv[0]);
  const char *reason = (argc >= 2) ? JS_ToCString(ctx, argv[1]) : nullptr;
  const int32_t rc = inst->engine->bridgeEndBroadcast(
      inst->id, topic ? topic : "", reason ? reason : "");
  if (topic) {
    JS_FreeCString(ctx, topic);
  }
  if (reason) {
    JS_FreeCString(ctx, reason);
  }
  if (rc == 0) {
    return JS_NewString(ctx, "");
  }
  if (rc == PLUGINXX_RESULT_ERR_STATE) {
    return JS_NewString(ctx, "结束流被拒绝 (插件登记还没完成)");
  }
  return JS_NewString(
      ctx, rc == PLUGINXX_RESULT_ERR_PERMISSION
               ? "结束流被拒绝 (不是自己命名空间下的频道)"
               : "结束流被拒绝 (频道不存在, 或不是它的属主)");
}

JSValue jsTimerSet(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine) {
    return JS_NewInt32(ctx, -1);
  }
  int32_t delay = 0;
  if (argc >= 1) {
    JS_ToInt32(ctx, &delay, argv[0]);
  }
  const bool repeat = (argc >= 2) && JS_ToBool(ctx, argv[1]) > 0;
  const auto id = inst->engine->bridgeTimerSet(inst->name, delay, repeat);
  return JS_NewInt32(ctx, static_cast<int32_t>(id));
}

JSValue jsTimerClear(JSContext *ctx, JSValueConst, int argc,
                     JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine || argc < 1) {
    return JS_UNDEFINED;
  }
  int32_t id = 0;
  if (JS_ToInt32(ctx, &id, argv[0]) == 0 && id > 0) {
    inst->engine->bridgeTimerClear(inst->name, id);
  }
  return JS_UNDEFINED;
}

/// `musicxx.version`：插件框架版本（与 C 头里的 MUSICXX_EXTERN_PLUGIN_VERSION、
/// `host.info().hostVersion` 同一个值）
JSValue jsEngineVersion(JSContext *ctx, JSValueConst, int /*argc*/,
                        JSValueConst * /*argv*/) {
  return JS_NewString(ctx, MUSICXX_EXTERN_PLUGIN_VERSION);
}

/// UI 项操作 (register | update | unregister)
JSValue jsUiOp(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine || argc < 4) {
    return JS_UNDEFINED;
  }
  const char *op = JS_ToCString(ctx, argv[0]);
  const char *name = JS_ToCString(ctx, argv[1]);
  const char *type = JS_ToCString(ctx, argv[2]);
  const char *data = JS_ToCString(ctx, argv[3]);
  int32_t order = 0;
  if (argc >= 5) {
    JS_ToInt32(ctx, &order, argv[4]);
  }
  inst->engine->bridgeUiOp(inst->name, op ? op : "", name ? name : "",
                           type ? type : "", data ? data : "{}", order);
  if (op) {
    JS_FreeCString(ctx, op);
  }
  if (name) {
    JS_FreeCString(ctx, name);
  }
  if (type) {
    JS_FreeCString(ctx, type);
  }
  if (data) {
    JS_FreeCString(ctx, data);
  }
  return JS_UNDEFINED;
}

namespace {

/// 阶段名 → 阶段编号 (`before` / `after`; 其它一律按实现链)
int32_t capabilityPhaseOfName(const std::string &name) {
  if (name == "before") {
    return MUSICXX_CAPABILITY_PHASE_BEFORE;
  }
  if (name == "after") {
    return MUSICXX_CAPABILITY_PHASE_AFTER;
  }
  return MUSICXX_CAPABILITY_PHASE_IMPL;
}

/// 阶段编号 → JS 侧使用的阶段名
const char *capabilityPhaseNameOf(int32_t phase) {
  if (phase == MUSICXX_CAPABILITY_PHASE_BEFORE) {
    return "before";
  }
  if (phase == MUSICXX_CAPABILITY_PHASE_AFTER) {
    return "after";
  }
  return "impl";
}

} // namespace

/// 能力操作 (register | unregister): 运行期注册/注销要落到插件框架注册表
///
/// 顶层登记阶段 (liveRegistrations == false) 由 collectRegistrations 统一回放, 这里
/// 不做任何事; 运行期由 C++ 侧投递到框架线程执行 (投递不等待)。
JSValue jsImplementationOp(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine || argc < 2) {
    return JS_UNDEFINED;
  }
  const char *op = JS_ToCString(ctx, argv[0]);
  const char *capabilityId = JS_ToCString(ctx, argv[1]);
  int32_t priority = 0;
  if (argc >= 3) {
    JS_ToInt32(ctx, &priority, argv[2]);
  }
  const char *ownerTag = (argc >= 4) ? JS_ToCString(ctx, argv[3]) : nullptr;
  const char *phase = (argc >= 5) ? JS_ToCString(ctx, argv[4]) : nullptr;
  inst->engine->bridgeImplementationOp(inst->name, op ? op : "",
                                       capabilityId ? capabilityId : "", priority,
                                       ownerTag ? ownerTag : "",
                                       phase ? phase : "impl");
  if (op) {
    JS_FreeCString(ctx, op);
  }
  if (capabilityId) {
    JS_FreeCString(ctx, capabilityId);
  }
  if (ownerTag) {
    JS_FreeCString(ctx, ownerTag);
  }
  if (phase) {
    JS_FreeCString(ctx, phase);
  }
  return JS_UNDEFINED;
}

/// 动态订阅一个广播频道
///
/// 顶层声明由注册回放处理；`optionsJson` 空串 = 不限频。
JSValue jsSubscribeHostOpts(JSContext *ctx, JSValueConst, int argc,
                            JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine || argc < 1) {
    return JS_UNDEFINED;
  }
  const char *topic = JS_ToCString(ctx, argv[0]);
  const char *options = (argc >= 2) ? JS_ToCString(ctx, argv[1]) : nullptr;
  inst->engine->bridgeSubscribeHost(inst->name, topic ? topic : "",
                                    options ? options : "");
  if (topic) {
    JS_FreeCString(ctx, topic);
  }
  if (options) {
    JS_FreeCString(ctx, options);
  }
  return JS_UNDEFINED;
}

/// 运行期声明自己的广播频道（顶层声明由注册回放处理）
JSValue jsDeclareBroadcast(JSContext *ctx, JSValueConst, int argc,
                           JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine || argc < 1) {
    return JS_UNDEFINED;
  }
  const char *decl = JS_ToCString(ctx, argv[0]);
  inst->engine->bridgeDeclareBroadcast(inst->name, decl ? decl : "{}");
  if (decl) {
    JS_FreeCString(ctx, decl);
  }
  return JS_UNDEFINED;
}

/// 同步读一个频道的当前值（没有当前值返回空串）
///
/// 读的是广播注册表里的当前值（内部加锁），不需要投递到框架线程 ——
/// JS 线程绝不等框架线程（框架线程可能正等着 JS 处理器）。
JSValue jsBroadcastRead(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine || argc < 1) {
    return JS_NewString(ctx, "");
  }
  const char *key = JS_ToCString(ctx, argv[0]);
  const std::string keyText = key ? key : "";
  if (key) {
    JS_FreeCString(ctx, key);
  }
  const std::string json = inst->engine->bridgeBroadcastRead(keyText);
  return JS_NewStringLen(ctx, json.data(), json.size());
}

/// 同步读一个频道的声明与状态（未声明返回空串）
JSValue jsBroadcastInfo(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine || argc < 1) {
    return JS_NewString(ctx, "");
  }
  const char *key = JS_ToCString(ctx, argv[0]);
  const std::string keyText = key ? key : "";
  if (key) {
    JS_FreeCString(ctx, key);
  }
  const std::string json = inst->engine->bridgeBroadcastInfo(keyText);
  return JS_NewStringLen(ctx, json.data(), json.size());
}

/// 同步列频道（`prefix` 可空; 返回 JSON 数组）
JSValue jsBroadcastList(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine) {
    return JS_NewString(ctx, "[]");
  }
  const char *prefix = (argc >= 1) ? JS_ToCString(ctx, argv[0]) : nullptr;
  const std::string head = prefix ? prefix : "";
  if (prefix) {
    JS_FreeCString(ctx, prefix);
  }
  const std::string json = inst->engine->bridgeBroadcastList(head);
  return JS_NewStringLen(ctx, json.data(), json.size());
}

/// 裁决型能力的 Promise 结算 (JS 线程 → 等待槽; 见
/// JsEngine::bridgeImplementationWaitResolve)
JSValue jsImplementationWaitResolve(JSContext *ctx, JSValueConst, int argc,
                          JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine || argc < 1) {
    return JS_UNDEFINED;
  }
  const char *waitId = JS_ToCString(ctx, argv[0]);
  const char *json = (argc >= 2) ? JS_ToCString(ctx, argv[1]) : nullptr;
  inst->engine->bridgeImplementationWaitResolve(waitId ? waitId : "", json ? json : "");
  if (waitId) {
    JS_FreeCString(ctx, waitId);
  }
  if (json) {
    JS_FreeCString(ctx, json);
  }
  return JS_UNDEFINED;
}

/// 能力处理器的 Promise 结算 (JS 线程 → 等待槽; 见
/// JsEngine::bridgeCapabilityWaitResolve)
JSValue jsCapabilityWaitResolve(JSContext *ctx, JSValueConst, int argc,
                                JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine || argc < 1) {
    return JS_UNDEFINED;
  }
  const char *waitId = JS_ToCString(ctx, argv[0]);
  const char *json = (argc >= 2) ? JS_ToCString(ctx, argv[1]) : nullptr;
  inst->engine->bridgeCapabilityWaitResolve(waitId ? waitId : "",
                                          json ? json : "");
  if (waitId) {
    JS_FreeCString(ctx, waitId);
  }
  if (json) {
    JS_FreeCString(ctx, json);
  }
  return JS_UNDEFINED;
}

/// 本实例统计 (JSON 文本)
JSValue jsStats(JSContext *ctx, JSValueConst, int /*argc*/,
                JSValueConst * /*argv*/) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine) {
    return JS_NewString(ctx, "{}");
  }
  std::string json = inst->engine->instanceStatsJson(inst->name);
  return JS_NewStringLen(ctx, json.data(), json.size());
}

/// 跨插件能力调用 (返回控制对象 JSON, 由 prelude 决定同步解析还是等回执)
JSValue jsCapabilityCall(JSContext *ctx, JSValueConst, int argc,
                         JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine || argc < 3) {
    return JS_NewString(ctx, R"({"status":"error","error":"参数非法"})");
  }
  const char *pluginId = JS_ToCString(ctx, argv[0]);
  const char *name = JS_ToCString(ctx, argv[1]);
  const char *args = JS_ToCString(ctx, argv[2]);
  int32_t timeout = 3000;
  if (argc >= 4) {
    JS_ToInt32(ctx, &timeout, argv[3]);
  }
  std::string out;
  std::string err;
  int64_t pendingId = 0;
  const int32_t rc = inst->engine->callCapability(
      inst->name, pluginId ? pluginId : "", name ? name : "",
      args ? args : "{}", timeout > 0 ? static_cast<uint32_t>(timeout) : 3000u,
      &pendingId, out, err);
  if (pluginId) {
    JS_FreeCString(ctx, pluginId);
  }
  if (name) {
    JS_FreeCString(ctx, name);
  }
  if (args) {
    JS_FreeCString(ctx, args);
  }

  Json payload;
  if (rc == JsEngine::kCallCapabilityDone) {
    payload["status"] = "ok";
    payload["result"] = out.empty() ? Json(nullptr) : parseJsonSafe(out);
  } else if (rc == JsEngine::kCallCapabilityPending) {
    payload["status"] = "pending";
    payload["id"] = pendingId;
  } else {
    payload["status"] = "error";
    payload["error"] = err.empty() ? "capability_call_failed" : err;
  }
  const std::string text = payload.dump();
  return JS_NewStringLen(ctx, text.data(), text.size());
}

JSValue jsPluginId(JSContext *ctx, JSValueConst, int /*argc*/,
                   JSValueConst * /*argv*/) {
  auto *inst = contextInstance(ctx);
  return JS_NewString(ctx, inst ? inst->id.c_str() : "");
}

/// 文本 → 指定编码的字节（base64）
///
/// 参数: (text, encoding) → `{"ok":true,"base64":"…"}` / `{"ok":false,"error":…}`。
/// 编码名交给 `utilxx_base::convertCharset`（`utf-8` / `gbk` / `gb18030` …）。
JSValue jsTextEncode(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  const char *text = (argc >= 1) ? JS_ToCString(ctx, argv[0]) : nullptr;
  const char *encoding = (argc >= 2) ? JS_ToCString(ctx, argv[1]) : nullptr;
  const std::string source = text ? text : "";
  const std::string target = (encoding && *encoding) ? encoding : "utf-8";
  if (text) {
    JS_FreeCString(ctx, text);
  }
  if (encoding) {
    JS_FreeCString(ctx, encoding);
  }

  Json out = Json::object();
  std::string bytes = source;
  bool ok = true;
  if (!utilxx_base::isIgnoreCaseEqual(target, "utf-8")) {
    const auto converted =
        utilxx_base::convertCharset(source, "utf-8", target);
    ok = std::get<0>(converted);
    if (ok && std::get<1>(converted).has_value()) {
      bytes = *std::get<1>(converted);
    } else if (!ok) {
      ok = false;
    }
  }
  if (!ok) {
    out["ok"] = false;
    out["error"] = "bad_encoding";
  } else {
    out["ok"] = true;
    out["base64"] = utilxx_base::base64Encode(bytes);
    out["bytes"] = static_cast<int64_t>(bytes.size());
  }
  const std::string json = out.dump();
  return JS_NewStringLen(ctx, json.data(), json.size());
}

/// 指定编码的字节（base64 分片数组）→ UTF-8 文本
///
/// 参数: (chunksJson, encoding) → `{"ok":true,"text":"…"}`。
/// `chunksJson` 是 base64 字符串的 JSON 数组：分片各自解码后拼字节再转编码
/// （直接把 base64 文本拼起来会因为 padding 出错，所以由原生层拼字节）。
JSValue jsTextDecode(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  const char *chunks = (argc >= 1) ? JS_ToCString(ctx, argv[0]) : nullptr;
  const char *encoding = (argc >= 2) ? JS_ToCString(ctx, argv[1]) : nullptr;
  const std::string chunksJson = chunks ? chunks : "[]";
  const std::string source = (encoding && *encoding) ? encoding : "utf-8";
  if (chunks) {
    JS_FreeCString(ctx, chunks);
  }
  if (encoding) {
    JS_FreeCString(ctx, encoding);
  }

  Json out = Json::object();
  const Json parsed = parseJsonSafe(chunksJson);
  std::string bytes;
  bool badBase64 = false;
  if (parsed.is_array()) {
    for (const auto &item : parsed) {
      if (!item.is_string()) {
        continue;
      }
      const auto decoded = utilxx_base::base64Decode(item.get<std::string>());
      if (!decoded.has_value()) {
        badBase64 = true;
        break;
      }
      bytes.append(*decoded);
    }
  } else if (parsed.is_string()) {
    const auto decoded = utilxx_base::base64Decode(parsed.get<std::string>());
    if (decoded.has_value()) {
      bytes = *decoded;
    } else {
      badBase64 = true;
    }
  }
  if (badBase64) {
    out["ok"] = false;
    out["error"] = "bad_base64";
  } else if (bytes.empty()) {
    out["ok"] = true;
    out["text"] = "";
    out["bytes"] = int64_t{0};
  } else if (utilxx_base::isIgnoreCaseEqual(source, "utf-8")) {
    out["ok"] = true;
    out["text"] = bytes;
    out["bytes"] = static_cast<int64_t>(bytes.size());
  } else {
    const auto converted = utilxx_base::convertCharset(bytes, source, "utf-8");
    if (!std::get<0>(converted)) {
      out["ok"] = false;
      out["error"] = "bad_encoding";
    } else {
      out["ok"] = true;
      out["text"] = std::get<1>(converted).has_value() ? *std::get<1>(converted)
                                                       : bytes;
      out["bytes"] = static_cast<int64_t>(bytes.size());
    }
  }
  const std::string json = out.dump();
  return JS_NewStringLen(ctx, json.data(), json.size());
}

/// 装好 `__ext` 桥接对象 (返回值的所有权交给调用方 / JS_SetPropertyStr)
JSValue buildBridgeObject(JSContext *ctx) {
  struct Entry {
    const char *name;
    JSCFunction *fn;
    int argc;
  };
  const Entry entries[] = {
      {"hostInfo", jsHostInfo, 0},
      {"log", jsLog, 2},
      {"configPath", jsConfigPath, 0},
      {"appCapabilityCall", jsAppCapabilityCall, 3},
      {"publish", jsPublish, 2},
      {"endBroadcast", jsEndBroadcast, 2},
      {"subscribeHost", jsSubscribeHostOpts, 2},
      {"declareBroadcast", jsDeclareBroadcast, 1},
      {"broadcastRead", jsBroadcastRead, 1},
      {"broadcastInfo", jsBroadcastInfo, 1},
      {"broadcastList", jsBroadcastList, 1},
      {"implementationOp", jsImplementationOp, 5},
      {"implementationWaitResolve", jsImplementationWaitResolve, 2},
      {"capabilityWaitResolve", jsCapabilityWaitResolve, 2},
      {"timerSet", jsTimerSet, 2},
      {"timerClear", jsTimerClear, 1},
      {"uiOp", jsUiOp, 5},
      {"stats", jsStats, 0},
      {"netCall", jsNetCall, 2},
      {"netCancel", jsNetCancel, 1},
      {"capabilityCall", jsCapabilityCall, 4},
      {"textEncode", jsTextEncode, 2},
      {"textDecode", jsTextDecode, 2},
      {"engineVersion", jsEngineVersion, 0},
      {"pluginId", jsPluginId, 0},
  };
  JSValue ext = JS_NewObject(ctx);
  for (const auto &entry : entries) {
    JS_SetPropertyStr(ctx, ext, entry.name,
                      JS_NewCFunction(ctx, entry.fn, entry.name, entry.argc));
  }
  return ext;
}

/// 在 JS 线程上释放实例的 runtime (必须在该线程调用)
void freeInstanceContextOnJsThread(JsEngine::Instance &inst,
                                   bool resetScriptState) {
  if (!inst.ctx || !inst.rt) {
    inst.ctx = nullptr;
    inst.rt = nullptr;
    return;
  }
  auto *ctx = static_cast<JSContext *>(inst.ctx);
  auto *rt = static_cast<JSRuntime *>(inst.rt);
  if (resetScriptState) {
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ext = JS_GetPropertyStr(ctx, global, "__ext");
    if (JS_IsObject(ext)) {
      JSValue reset = JS_GetPropertyStr(ctx, ext, "reset");
      if (JS_IsFunction(ctx, reset)) {
        JSValue ret = JS_Call(ctx, reset, JS_UNDEFINED, 0, nullptr);
        JS_FreeValue(ctx, ret);
      }
      JS_FreeValue(ctx, reset);
    }
    JS_FreeValue(ctx, ext);
    JS_FreeValue(ctx, global);
    // 注意: JS_ExecutePendingJob 要求非空上下文出参 (它对 pctx 无条件解引用)
    JSContext *jobCtx = nullptr;
    while (JS_ExecutePendingJob(rt, &jobCtx) > 0) {
      // 清空挂起的 promise 任务
    }
  }
  JS_FreeContext(ctx);
  JS_FreeRuntime(rt);
  inst.ctx = nullptr;
  inst.rt = nullptr;
}

#endif // MUSICXX_EXTERN_PLUGIN_HAS_JS

} // namespace

/* ==================== 创建 / 属性 ==================== */

JsEngine::JsEngine() = default;

JsEngine::~JsEngine() {
  stop();
  SlotTable &table = slotTable();
  auto *expected = this;
  table.engine.compare_exchange_strong(expected, nullptr);
  if (mgr_) {
    mgr_->setInternalAppCapabilityRelay(nullptr);
  }
}

std::shared_ptr<JsEngine> JsEngine::create(MusicxxHostManager *mgr) {
  auto engine = std::shared_ptr<JsEngine>(new JsEngine());
  engine->mgr_ = mgr;
  slotTable().engine.store(engine.get(), std::memory_order_release);
  installBuiltinProvider();
  if (mgr) {
    /// 能力调用接驳口: JS 侧已发出、还没收到回复的请求由引擎自己登记, Dart 的回复经它转交
    mgr->setInternalAppCapabilityRelay(engine);
  }
  return engine;
}

bool JsEngine::available() { return kJsCompiled; }

void JsEngine::installBuiltinProvider() {
  static std::once_flag once;
  std::call_once(once, [] {
    pluginxx::BuiltinPluginProvider provider;
    provider.plugins = &JsEngine::builtinPlugins;
    provider.manifests = &JsEngine::builtinManifests;
    pluginxx::setBuiltinPluginProvider(provider);
  });
}

const PluginxxBuiltinInfo *JsEngine::builtinPlugins(uint64_t *count) {
  SlotTable &table = slotTable();
  std::lock_guard lock{table.mutex};
  table.infos.clear();
  table.infos.reserve(table.slots.size());
  for (Slot &slot : table.slots) {
    if (!slot.used) {
      continue;
    }
    PluginxxBuiltinInfo info{};
    info.name = PluginxxStringView{slot.name.data(), slot.name.size()};
    info.get_info = &JsEngine::builtinGetInfo;
    info.create = &JsEngine::builtinCreate;
    info.destroy = &JsEngine::builtinDestroy;
    info.start = &JsEngine::builtinStart;
    info.stop = &JsEngine::builtinStop;
    table.infos.push_back(info);
  }
  if (count) {
    *count = static_cast<uint64_t>(table.infos.size());
  }
  return table.infos.empty() ? nullptr : table.infos.data();
}

const PluginxxBuiltinManifest *JsEngine::builtinManifests(uint64_t *count) {
  SlotTable &table = slotTable();
  std::lock_guard lock{table.mutex};
  table.manifests.clear();
  table.manifests.reserve(table.slots.size());
  for (Slot &slot : table.slots) {
    if (!slot.used) {
      continue;
    }
    PluginxxBuiltinManifest manifest{};
    manifest.name = PluginxxStringView{slot.name.data(), slot.name.size()};
    manifest.yaml = PluginxxStringView{nullptr, 0};
    table.manifests.push_back(manifest);
  }
  if (count) {
    *count = static_cast<uint64_t>(table.manifests.size());
  }
  return table.manifests.empty() ? nullptr : table.manifests.data();
}

bool JsEngine::registerBuiltin(const std::string &pluginId,
                               const std::vector<std::string> &scriptPaths,
                               const std::string &version, std::string &err) {
  if (pluginId.empty() || scriptPaths.empty()) {
    err = "registerBuiltin: 插件 id 与脚本路径都不能为空";
    return false;
  }
  for (const std::string &script : scriptPaths) {
    if (script.empty()) {
      err = "registerBuiltin: 脚本路径不能为空";
      return false;
    }
  }
  SlotTable &table = slotTable();
  std::lock_guard lock{table.mutex};
  const std::string name = "js:" + pluginId;
  /// 槽位数量不设上限: 先复用同名槽位, 再找空槽, 都没有就追加一个
  Slot *slot = nullptr;
  for (Slot &candidate : table.slots) {
    if (candidate.used && candidate.name == name) {
      slot = &candidate;
      break;
    }
  }
  if (!slot) {
    for (Slot &candidate : table.slots) {
      if (!candidate.used) {
        slot = &candidate;
        break;
      }
    }
  }
  if (!slot) {
    table.slots.emplace_back();
    slot = &table.slots.back();
  }
  slot->name = name;
  slot->scripts = scriptPaths;
  slot->version = version.empty() ? std::string{"1.0.0"} : version;
  slot->used = true;
  XX_LOGI("[musicxx_ext] JS插件槽位已登记: {} (脚本 {})", name,
          joinPaths(scriptPaths));
  return true;
}

void JsEngine::unregisterBuiltin(const std::string &pluginId) {
  SlotTable &table = slotTable();
  std::lock_guard lock{table.mutex};
  const std::string name = "js:" + pluginId;
  for (auto &slot : table.slots) {
    if (slot.used && slot.name == name) {
      slot.used = false;
      slot.name.clear();
      slot.scripts.clear();
      slot.version.clear();
      XX_LOGI("[musicxx_ext] JS插件槽位已注销: {}", name);
      return;
    }
  }
}

bool JsEngine::hasBuiltin(const std::string &pluginId) const {
  return !slotScripts("js:" + pluginId).empty();
}

/* ==================== 共享 JS 线程 ==================== */

int32_t JsEngine::start(std::string &err) {
  if (running_.load(std::memory_order_acquire)) {
    return PLUGINXX_RESULT_OK;
  }
  if (!kJsCompiled) {
    err = "本次构建未包含 JS 运行时 (未编译 QuickJS 子模块)";
    return PLUGINXX_RESULT_ERR_STATE;
  }
  return startThread(err);
}

int32_t JsEngine::startThread(std::string &err) {
  {
    std::lock_guard lock{mutex_};
    tasks_.clear();
    timers_.clear();
    pendingAppCapabilities_.clear();
    dispatchWaits_.clear();
    instances_.clear();
    stopping_.store(false, std::memory_order_release);
  }
  running_.store(true, std::memory_order_release);
  try {
    worker_ = std::thread([this] { runLoop(); });
  } catch (const std::exception &e) {
    running_.store(false, std::memory_order_release);
    err = std::string{"JS 线程创建失败: "} + e.what();
    return PLUGINXX_RESULT_ERR_INTERNAL;
  }
  XX_LOGI("[musicxx_ext] JS 线程已启动 (全部 JS插件实例共享这 1 条线程)");
  return PLUGINXX_RESULT_OK;
}

void JsEngine::stop() {
  if (!running_.load(std::memory_order_acquire)) {
    if (worker_.joinable()) {
      worker_.join();
    }
    return;
  }
  /// 退出循环前先在 JS 线程上释放全部实例上下文
  /// (JS_FreeContext/JS_FreeRuntime 必须在拥有它们的线程上调用)
  {
    std::lock_guard lock{mutex_};
    tasks_.push_back(Task{nowSteadyMs(), [this] {
#if defined(MUSICXX_EXTERN_PLUGIN_HAS_JS)
                            std::lock_guard inner{mutex_};
                            for (auto &[name, inst] : instances_) {
                              (void)name;
                              if (inst->ctx || inst->rt) {
                                freeInstanceContextOnJsThread(*inst, true);
                              }
                            }
#endif
                          }});
    stopping_.store(true, std::memory_order_release);
  }
  cv_.notify_all();
  if (worker_.joinable()) {
    worker_.join();
  }
  running_.store(false, std::memory_order_release);
  {
    std::lock_guard lock{mutex_};
    tasks_.clear();
    timers_.clear();
    instances_.clear();
    pendingAppCapabilities_.clear();
    dispatchWaits_.clear();
  }
  XX_LOGI("[musicxx_ext] JS 线程已停止");
}

void JsEngine::postTask(std::function<void()> fn) {
  if (!running_.load(std::memory_order_acquire)) {
    return;
  }
  {
    std::lock_guard lock{mutex_};
    tasks_.push_back(Task{nowSteadyMs(), std::move(fn)});
  }
  cv_.notify_all();
}

int64_t JsEngine::nextWaitMsLocked() const {
  int64_t wait = 50;
  const auto now = nowSteadyMs();
  for (const auto &timer : timers_) {
    wait = (std::min)(wait,
                      (std::max)(static_cast<int64_t>(0), timer.dueMs - now));
  }
  for (const auto &[id, pending] : pendingAppCapabilities_) {
    (void)id;
    wait = (std::min)(
        wait, (std::max)(static_cast<int64_t>(0), pending.deadlineMs - now));
  }
  return wait;
}

void JsEngine::runLoop() {
  for (;;) {
    Task task;
    {
      std::unique_lock lock{mutex_};
      if (tasks_.empty() && !stopping_.load(std::memory_order_acquire)) {
        const auto waitMs = nextWaitMsLocked();
        cv_.wait_for(lock, std::chrono::milliseconds{waitMs > 0 ? waitMs : 1});
      }
      if (tasks_.empty() && stopping_.load(std::memory_order_acquire)) {
        break;
      }
      if (!tasks_.empty()) {
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
    }
    if (task.fn) {
      /// 排队等待时长 (从入队到开始执行): 观察共享 JS 线程是否被单个插件占满
      const int64_t waited = nowSteadyMs() - task.enqueuedMs;
      queueWaitLastMs_.store(waited, std::memory_order_relaxed);
      if (waited > queueWaitMaxMs_.load(std::memory_order_relaxed)) {
        queueWaitMaxMs_.store(waited, std::memory_order_relaxed);
      }
      try {
        task.fn();
      } catch (const std::exception &e) {
        XX_LOGE("[musicxx_ext] JS 任务异常: {}", e.what());
      } catch (...) {
        XX_LOGE("[musicxx_ext] JS 任务未知异常");
      }
    }
    fireDueTimers();
    expireActions();
#if defined(MUSICXX_EXTERN_PLUGIN_HAS_JS)
    {
      /// JS 堆用量采样 (只观测不限制; 默认 10 秒一次)
      constexpr int64_t kHeapSampleIntervalMs = 10000;
      const auto now = nowSteadyMs();
      if (now - heapSampledAtMs_ >= kHeapSampleIntervalMs) {
        heapSampledAtMs_ = now;
        std::vector<std::shared_ptr<Instance>> sample;
        {
          std::lock_guard lock{mutex_};
          for (auto &[name, inst] : instances_) {
            (void)name;
            sample.push_back(inst);
          }
        }
        for (const auto &inst : sample) {
          sampleHeap(inst);
        }
      }
    }
#endif
#if defined(MUSICXX_EXTERN_PLUGIN_HAS_JS)
    // 执行挂起的 promise 任务 (每个实例的 runtime 各自推进)
    std::vector<std::shared_ptr<Instance>> list;
    {
      std::lock_guard lock{mutex_};
      for (auto &[name, inst] : instances_) {
        (void)name;
        list.push_back(inst);
      }
    }
    for (const auto &inst : list) {
      if (!inst->rt) {
        continue;
      }
      auto *rt = static_cast<JSRuntime *>(inst->rt);
      // 注意: JS_ExecutePendingJob 要求非空上下文出参 (它对 pctx 无条件解引用)
      JSContext *jobCtx = nullptr;
      while (JS_ExecutePendingJob(rt, &jobCtx) > 0) {
        // 清空本轮微任务
      }
    }
#endif
  }
  XX_LOGI("[musicxx_ext] JS 线程循环退出");
}

void JsEngine::fireDueTimers() {
  struct Fired {
    std::string instance;
    int64_t id = 0;
  };
  std::vector<Fired> fired;
  {
    std::lock_guard lock{mutex_};
    const auto now = nowSteadyMs();
    for (auto it = timers_.begin(); it != timers_.end();) {
      if (it->dueMs <= now) {
        fired.push_back(Fired{it->instance, it->id});
        if (it->repeat) {
          it->dueMs = now + (std::max)(it->intervalMs, static_cast<int64_t>(1));
          ++it;
        } else {
          it = timers_.erase(it);
        }
      } else {
        ++it;
      }
    }
  }
  for (const auto &item : fired) {
    const auto inst = lookupInstance(item.instance);
    if (!inst) {
      continue;
    }
    std::string ignored;
    callBridgeString(inst, "onTimerFire", {std::to_string(item.id)}, ignored);
  }
}

void JsEngine::expireActions() {
  std::vector<PendingAppCapability> expired;
  {
    std::lock_guard lock{mutex_};
    const auto now = nowSteadyMs();
    for (auto it = pendingAppCapabilities_.begin(); it != pendingAppCapabilities_.end();) {
      if (it->second.deadlineMs <= now) {
        expired.push_back(it->second);
        it = pendingAppCapabilities_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto &item : expired) {
    XX_LOGW("[musicxx_ext] JS插件 `{}` 的能力调用超时: {} (id={})",
            item.pluginId, item.capability, item.requestId);
    const auto inst = lookupInstance(item.instance);
    if (inst) {
      std::string ignored;
      callBridgeString(inst, "onAppCapabilityDone",
                       {std::to_string(item.requestId), "1",
                        R"({"error":"action_timeout"})"},
                       ignored);
    }
    if (mgr_) {
      Json payload;
      payload["requestId"] = item.requestId;
      payload["reason"] = "timeout";
      mgr_->pushEvent("musicxx.capability.app.cancel", item.pluginId, payload.dump());
    }
  }
}

/* ==================== 桥接 (线程安全) ==================== */

std::string JsEngine::bridgeHostInfo() const {
  std::lock_guard lock{hostInfoMutex_};
  return hostInfoJson_.empty() ? std::string{"{}"} : hostInfoJson_;
}

void JsEngine::setHostInfoCache(const std::string &json) {
  std::lock_guard lock{hostInfoMutex_};
  hostInfoJson_ = json;
}

void JsEngine::bridgeLog(const std::string &pluginId, int32_t level,
                         const std::string &message) {
  logPlugin(pluginId, level, message);
}

int32_t JsEngine::bridgePublish(const std::string &pluginId,
                                const std::string &topic,
                                const std::string &payload) {
  if (!mgr_) {
    return PLUGINXX_RESULT_ERR_STATE;
  }
  // 脚本顶层还没有把声明交给框架: 先存着, 注册回放完再按顺序发 (见 pendingPublishes)
  // 否则 `declare` + `publish` 初始值的写法会被判成"频道未声明"。
  if (const auto inst = lookupInstance("js:" + pluginId)) {
    if (!inst->registrationsApplied) {
      std::lock_guard lock{mutex_};
      inst->pendingPublishes.emplace_back(topic, payload);
      return PLUGINXX_RESULT_OK;
    }
  }
  return mgr_->broadcastPublishFrom(pluginId, topic, payload);
}

int64_t JsEngine::bridgeTimerSet(const std::string &instance, int64_t delayMs,
                                 bool repeat) {
  std::lock_guard lock{mutex_};
  Timer timer;
  timer.id = nextTimerId_++;
  timer.instance = instance;
  timer.intervalMs = (std::max)(delayMs, static_cast<int64_t>(0));
  timer.dueMs = nowSteadyMs() + timer.intervalMs;
  timer.repeat = repeat;
  const int64_t id = timer.id;
  timers_.push_back(std::move(timer));
  cv_.notify_all();
  return id;
}

void JsEngine::bridgeTimerClear(const std::string &instance, int64_t timerId) {
  std::lock_guard lock{mutex_};
  std::erase_if(timers_, [&](const Timer &timer) {
    return timer.id == timerId && timer.instance == instance;
  });
}

/* ==================== 桥接: UI / 订阅 / 统计 / 能力调用 ====================
 */

/* ---- 裁决型能力的 Promise 等待槽 (异步裁决) ---- */

int64_t
JsEngine::beginDispatchWait(const std::string &instance,
                        const std::shared_ptr<WaitSlot<std::string>> &slot) {
  const int64_t waitId = nextDispatchWaitId_.fetch_add(1);
  std::lock_guard lock{mutex_};
  dispatchWaits_[waitId] = DispatchWait{instance, slot};
  return waitId;
}

void JsEngine::finishDispatchWait(int64_t waitId) {
  std::lock_guard lock{mutex_};
  dispatchWaits_.erase(waitId);
}

void JsEngine::clearDispatchWaitsOf(const std::string &instance) {
  std::lock_guard lock{mutex_};
  std::erase_if(dispatchWaits_, [&](const auto &item) {
    return item.second.instance == instance;
  });
}

int64_t JsEngine::beginCapabilityWait(
    const std::string &instance,
    const std::shared_ptr<WaitSlot<std::string>> &slot) {
  const int64_t waitId = nextCapabilityWaitId_.fetch_add(1);
  std::lock_guard lock{mutex_};
  capabilityWaits_[waitId] = DispatchWait{instance, slot};
  return waitId;
}

void JsEngine::finishCapabilityWait(int64_t waitId) {
  std::lock_guard lock{mutex_};
  capabilityWaits_.erase(waitId);
}

void JsEngine::clearCapabilityWaitsOf(const std::string &instance) {
  std::lock_guard lock{mutex_};
  std::erase_if(capabilityWaits_, [&](const auto &item) {
    return item.second.instance == instance;
  });
  /// "JS → JS"的异步调用: 调用方或目标任一方停了，这条等待都没意义了
  /// （调用方的 Promise 由 JS 侧 `ext.reset` 收尾）
  std::erase_if(jsCallWaits_, [&](const auto &item) {
    return item.second.first == instance || item.second.second == instance;
  });
}

void JsEngine::bridgeCapabilityWaitResolve(const std::string &waitId,
                                           const std::string &json) {
  int64_t id = 0;
  try {
    size_t used = 0;
    id = std::stoll(waitId, &used);
  } catch (const std::exception &) {
    return;
  }
  /// ① "JS插件 → JS插件"的异步能力: 按调用方实例把结果回执过去
  ///    （本函数就在 JS 线程上, 可以直接回调 JS）
  {
    std::string caller;
    bool found = false;
    {
      std::lock_guard lock{mutex_};
      const auto it = jsCallWaits_.find(id);
      if (it != jsCallWaits_.end()) {
        caller = it->second.first;
        jsCallWaits_.erase(it);
        found = true;
      }
    }
    if (found) {
      deliverCapabilityResult(caller, id, json);
      return;
    }
  }
  /// ② 被调用的 JS 能力处理器 (等待槽形态): 交给等着的框架线程
  std::shared_ptr<WaitSlot<std::string>> slot;
  {
    std::lock_guard lock{mutex_};
    const auto it = capabilityWaits_.find(id);
    if (it == capabilityWaits_.end()) {
      return; ///< 不属于本引擎 / 已经完成
    }
    slot = it->second.slot;
    capabilityWaits_.erase(it);
  }
  if (!slot) {
    return;
  }
  slot->set(json);
}

void JsEngine::deliverCapabilityResult(const std::string &callerInstance,
                                       int64_t pendingId,
                                       const std::string &json) {
  auto inst = lookupInstance(callerInstance);
  if (!inst || !inst->ctx) {
    return; ///< 调用方已经不在了 (停用 / 卸载): 结果丢弃
  }
  std::string ignored;
  callBridgeString(inst, "onCapabilityResult",
                   {std::to_string(pendingId), json}, ignored);
}

void JsEngine::bridgeImplementationWaitResolve(const std::string &waitId,
                                     const std::string &json) {
  int64_t id = 0;
  try {
    size_t used = 0;
    id = std::stoll(waitId, &used);
  } catch (const std::exception &) {
    return;
  }
  std::string instance;
  std::shared_ptr<WaitSlot<std::string>> slot;
  {
    std::lock_guard lock{mutex_};
    const auto it = dispatchWaits_.find(id);
    if (it == dispatchWaits_.end()) {
      return; ///< 不属于本引擎 / 已经完成
    }
    instance = it->second.instance;
    slot = it->second.slot;
    dispatchWaits_.erase(it);
  }
  if (!slot) {
    return;
  }
  if (auto inst = lookupInstance(instance)) {
    ++inst->asyncDispatchSettled;
  }
  slot->set(json);
}

void JsEngine::bridgeUiOp(const std::string &instanceName,
                          const std::string &op, const std::string &name,
                          const std::string &type, const std::string &dataJson,
                          int32_t order) {
  auto inst = lookupInstance(instanceName);
  if (!inst || !mgr_ || !mgr_->running()) {
    return;
  }
  if (!inst->liveRegistrations.load(std::memory_order_acquire)) {
    /// 脚本顶层登记阶段: 先记下来, 由 applyRegistrations 在框架线程回放
    /// (与能力/能力/订阅同一套做法, 避免"框架线程等 JS、JS 等框架线程")
    Instance::PendingUiOp entry;
    entry.op = op;
    entry.name = name;
    entry.type = type;
    entry.dataJson = dataJson;
    entry.order = order;
    inst->pendingUiOps.push_back(std::move(entry));
    return;
  }
  /// 运行期: 投递不等待; 失败只记日志与事件, 不回传给脚本
  const std::string pluginId = inst->id;
  asio::post(mgr_->hostExecutor(), [this, pluginId, instanceName, op, name,
                                    type, dataJson, order] {
    auto current = mgr_->resolveInstance(instanceName);
    if (!current) {
      return;
    }
    int32_t rc = PLUGINXX_RESULT_ERR_STATE;
    if (op == "register") {
      MusicxxPluginUIEntrySpec spec{};
      spec.version = 1;
      spec.struct_size = sizeof(MusicxxPluginUIEntrySpec);
      spec.item_id = PluginxxStringView{name.data(), name.size()};
      spec.type = PluginxxStringView{type.data(), type.size()};
      spec.data_json = PluginxxStringView{dataJson.data(), dataJson.size()};
      spec.order = order;
      spec.flags = 0;
      rc = mgr_->registerUiEntry(current.get(), spec);
    } else if (op == "update") {
      rc = mgr_->updateUiEntry(current.get(), name, dataJson);
    } else if (op == "unregister") {
      rc = mgr_->unregisterUiEntry(current.get(), name);
    }
    if (rc != PLUGINXX_RESULT_OK) {
      Json payload;
      payload["id"] = pluginId;
      payload["phase"] = "ui";
      payload["op"] = op;
      payload["item"] = name;
      payload["code"] = rc;
      payload["message"] = "UI 项操作被拒绝 (检查类型/命名空间/data 结构)";
      mgr_->pushEvent("musicxx.plugin.error", pluginId, payload.dump());
      XX_LOGW("[musicxx_ext] JS插件 `{}` 运行期 UI 操作 `{}` 失败 (code={})",
              pluginId, op, rc);
    }
  });
}

void JsEngine::bridgeSubscribeHost(const std::string &instanceName,
                                   const std::string &topic,
                                   const std::string &optionsJson) {
  auto inst = lookupInstance(instanceName);
  if (!inst) {
    return;
  }
  if (!inst->liveRegistrations.load(std::memory_order_acquire)) {
    /// 顶层登记阶段的订阅由 applyRegistrations 统一建立
    return;
  }
  asio::post(mgr_->hostExecutor(), [this, instanceName, topic, optionsJson] {
    auto current = lookupInstance(instanceName);
    if (current) {
      ensureSubscriptionOnHost(current, topic, optionsJson);
    }
  });
}

void JsEngine::ensureSubscriptionOnHost(const std::shared_ptr<Instance> &inst,
                                        const std::string &topic,
                                        const std::string &optionsJson) {
  if (!inst || topic.empty() || !inst->hostInst) {
    return;
  }
  for (size_t i = 0; i < inst->subscriptions.size(); ++i) {
    const auto &existing = inst->subscriptions[i];
    if (!existing || existing->topic != topic
        || existing->options != optionsJson) {
      continue;
    }
    if (!existing->ended.load(std::memory_order_acquire)) {
      return;
    }
    /// 上一次"结束流"之后宿主已经摘掉了那条订阅: 去掉旧记账, 重新订一次
    inst->subscriptions.erase(inst->subscriptions.begin() +
                              static_cast<std::ptrdiff_t>(i));
    break;
  }
  const PluginxxHost *host = inst->hostInst->hostView();
  if (!host || !host->vtable || !host->vtable->query_interface) {
    return;
  }
  // 广播表的 IID 由应用名拼出 (`<应用名>.broadcasts`), 不写字面量
  const PluginxxStringView iid{PLUGINXX_IFACE_BROADCASTS, sizeof(PLUGINXX_IFACE_BROADCASTS) - 1};
  const auto *broadcasts = static_cast<const PluginxxBroadcastsIface *>(
      host->vtable->query_interface(host, &iid));
  if (!broadcasts || !broadcasts->subscribe_broadcast) {
    return;
  }
  auto holder = std::make_shared<SubscriptionHolder>();
  holder->engine = this;
  holder->instance = inst->name;
  holder->topic = topic;
  holder->options = optionsJson;
  holder->weakSelf = holder;
  const PluginxxStringView topicView{holder->topic.data(),
                                     holder->topic.size()};
  const PluginxxStringView optionsView{holder->options.data(),
                                       holder->options.size()};
  const PluginxxStringView *optionsPtr =
      holder->options.empty() ? nullptr : &optionsView;
  // 表 v3 才能带流结束回调: 频道被属主结束时回调一次 (与值回调同一个 ud)
  const bool hasEnd = pluginxx::ifaceHas<
      &PluginxxBroadcastsIface::subscribe_broadcast_ex>(broadcasts)
      && broadcasts->subscribe_broadcast_ex != nullptr;
  auto *sub = hasEnd
                  ? broadcasts->subscribe_broadcast_ex(
                        host, nullptr, &topicView, &JsEngine::eventTrampoline,
                        &JsEngine::endTrampoline, holder.get(), optionsPtr,
                        nullptr)
                  : broadcasts->subscribe_broadcast(
                        host, nullptr, &topicView, &JsEngine::eventTrampoline,
                        holder.get(), optionsPtr, nullptr);
  if (!sub) {
    XX_LOGW("[musicxx_ext] JS插件 `{}` 运行期订阅 `{}` 被拒绝", inst->id,
            topic);
    return;
  }
  inst->subscriptions.push_back(holder);
  XX_LOGI("[musicxx_ext] JS插件 `{}` 运行期订阅 `{}` 已生效 (intervalMs={})",
          inst->id, topic, holder->options);
}

std::vector<JsEngine::Instance::PendingUiOp>
JsEngine::takePendingUiOps(const std::shared_ptr<Instance> &inst) {
  std::vector<Instance::PendingUiOp> ops;
  if (!inst) {
    return ops;
  }
  ops.swap(inst->pendingUiOps);
  return ops;
}

/* ==================== 广播 (只读与订阅) ==================== */

/// 运行期声明自己的一个广播频道 (**投递到框架线程; 不等待**)
///
/// 顶层登记阶段的声明由 `applyRegistrations` 从 `collectRegistrations` 的
/// `broadcasts` 段统一回放, 这里什么都不做 (避免"框架线程等 JS、JS 等框架线程")。
void JsEngine::bridgeDeclareBroadcast(const std::string &instanceName,
                                      const std::string &declJson) {
  auto inst = lookupInstance(instanceName);
  if (!inst || !mgr_ || !mgr_->running()) {
    return;
  }
  if (!inst->liveRegistrations.load(std::memory_order_acquire)) {
    return;
  }
  auto self = shared_from_this();
  asio::post(mgr_->hostExecutor(), [self, instanceName, declJson] {
    auto current = self->lookupInstance(instanceName);
    if (current) {
      self->applyBroadcastDeclOnHost(current, declJson);
    }
  });
}

/// 在框架线程上声明一个广播频道 (**必须已在框架线程调用**)
int32_t JsEngine::applyBroadcastDeclOnHost(const std::shared_ptr<Instance> &inst,
                                           const std::string &declJson) {
  if (!inst || !inst->hostInst || !mgr_) {
    return PLUGINXX_RESULT_ERR_STATE;
  }
  std::string error;
  const int32_t rc = mgr_->declareBroadcast(inst->hostInst, declJson, &error);
  if (rc != PLUGINXX_RESULT_OK) {
    Json payload;
    payload["id"] = inst->id;
    payload["phase"] = "broadcast";
    payload["op"] = "declare";
    payload["code"] = rc;
    payload["message"] = error.empty() ? "广播声明被拒绝" : error;
    mgr_->pushEvent("musicxx.plugin.error", inst->id, payload.dump());
    XX_LOGW("[musicxx_ext] JS插件 `{}` 声明广播 `{}` 被拒绝 (code={}): {}",
            inst->id, declJson, rc, error);
  }
  return rc;
}

/// 同步读一个频道的当前值 (`cache: latest` 才可能有值; 内部加锁)
std::string JsEngine::bridgeBroadcastRead(const std::string &key) {
  MusicxxHostManager *mgr = mgr_;
  if (!mgr || key.empty()) {
    return {};
  }
  std::string out;
  if (mgr->broadcasts()->read(key, &out)) {
    return out;
  }
  return {};
}

/// 同步读一个频道的声明与状态 (未声明返回空串)
std::string JsEngine::bridgeBroadcastInfo(const std::string &key) {
  MusicxxHostManager *mgr = mgr_;
  if (!mgr || key.empty()) {
    return {};
  }
  return mgr->broadcasts()->infoJson(key);
}

/// 同步列频道 (`prefix` 可空; 返回声明数组, 每项带当前值)
std::string JsEngine::bridgeBroadcastList(const std::string &prefix) {
  MusicxxHostManager *mgr = mgr_;
  if (!mgr) {
    return "[]";
  }
  bool parseOk = false;
  Json list = parseJsonSafe(mgr->broadcasts()->listJson(prefix), &parseOk);
  if (!parseOk || !list.is_object() || !list.contains("broadcasts")) {
    return "[]";
  }
  return list["broadcasts"].dump();
}

int32_t JsEngine::bridgeEndBroadcast(const std::string &pluginId,
                                    const std::string &topic,
                                    const std::string &reasonJson) {
  if (!mgr_) {
    return PLUGINXX_RESULT_ERR_STATE;
  }
  // 脚本顶层还没有把声明交给框架: 现在结束只会得到"频道不存在"
  if (const auto inst = lookupInstance("js:" + pluginId)) {
    if (!inst->registrationsApplied) {
      return PLUGINXX_RESULT_ERR_STATE;
    }
  }
  // 与发布同一套做法: 在调用方线程直接结束 (见 bridgePublish)。
  // 投递到框架线程再等会和"框架线程等 JS 处理器返回"互锁。
  return mgr_->broadcastEndFrom(pluginId, topic, reasonJson);
}

void JsEngine::bridgeImplementationOp(const std::string &instanceName,
                                      const std::string &op,
                                      const std::string &capabilityId,
                                      int32_t priority,
                                      const std::string &ownerTag,
                                      const std::string &phase) {
  auto inst = lookupInstance(instanceName);
  if (!inst || !mgr_ || !mgr_->running()) {
    return;
  }
  if (!inst->liveRegistrations.load(std::memory_order_acquire)) {
    /// 脚本顶层登记阶段: 由 applyRegistrations 统一回放 (与 UI 项/订阅同一套做法,
    /// 避免"框架线程等 JS、JS 等框架线程"的互锁)
    return;
  }
  /// 运行期: 投递到框架线程执行 (**不等待**): 框架线程可能正等 JS 处理器,
  /// 在这里同步等待就会互锁。失败只记日志与事件, 不回传给脚本。
  auto self = shared_from_this();
  asio::post(mgr_->hostExecutor(),
             [self, instanceName, op, capabilityId, priority, ownerTag, phase] {
               auto current = self->lookupInstance(instanceName);
               if (current) {
                 self->applyImplementationOpOnHost(current, op, capabilityId, priority,
                                                   ownerTag, phase);
               }
             });
}

void JsEngine::applyImplementationOpOnHost(
    const std::shared_ptr<Instance> &inst, const std::string &op,
    const std::string &capabilityId, int32_t priority,
    const std::string &ownerTag, const std::string &phase) {
  if (!inst || !inst->hostInst || !mgr_ || capabilityId.empty()) {
    return;
  }
  const bool unregister = op == "unregister";
  if (!unregister && op != "register") {
    return;
  }
  const int32_t phaseCode = capabilityPhaseOfName(phase);
  /// 同一个 (能力, owner_tag) 只保留一份: 先摘掉插件框架注册 (释放注册表对该
  /// `Implementation` 的引用), 再释放 JS 侧持有的对象, 避免出现"注册表指向已释放对象"
  /// 的窗口; 覆盖式注册随后重新登记
  for (auto it = inst->implementations.begin(); it != inst->implementations.end();) {
    const auto &handler = *it;
    if (handler && handler->capabilityId == capabilityId && handler->ownerTag == ownerTag &&
        handler->phase == phaseCode) {
      mgr_->unimplement(inst->hostInst, handler->capabilityId, handler->ownerTag);
      it = inst->implementations.erase(it);
      continue;
    }
    ++it;
  }
  if (unregister) {
    return;
  }

  auto handler = std::make_shared<Implementation>();
  handler->engine = this;
  handler->instance = inst->name;
  handler->pluginId = inst->id;
  handler->capabilityId = capabilityId;
  handler->ownerTag = ownerTag;
  handler->phase = phaseCode;

  MusicxxPluginCapabilitySpec spec{};
  spec.phase = phaseCode;
  spec.capability_id =
      PluginxxStringView{handler->capabilityId.data(), handler->capabilityId.size()};
  spec.owner_tag = PluginxxStringView{handler->ownerTag.data(),
                                      handler->ownerTag.size()};
  spec.priority = priority;
  spec.user_data = handler.get();
  spec.run_sync = &JsEngine::implementationRunSync;
  const int32_t rc = mgr_->registerHandler(inst->hostInst, spec);
  if (rc != PLUGINXX_RESULT_OK) {
    Json payload;
    payload["id"] = inst->id;
    payload["phase"] = "capability";
    payload["capability"] = capabilityId;
    payload["code"] = "capability_register_failed";
    payload["message"] =
        "运行期能力注册被拒绝 (这条 id 没开这个阶段 / 阶段必须用同步处理器)";
    mgr_->pushEvent("musicxx.plugin.error", inst->id, payload.dump());
    XX_LOGW("[musicxx_ext] JS插件 `{}` 运行期注册能力 `{}` 被拒绝 (code={})",
            inst->id, capabilityId, rc);
    return;
  }
  inst->implementations.push_back(handler);
  XX_LOGI("[musicxx_ext] JS插件 `{}` 运行期注册能力 `{}` 已生效 (phase={}, "
          "priority={})",
          inst->id, capabilityId, phase, priority);
}

std::string JsEngine::instanceStatsJson(const std::string &instanceName) const {
  auto inst = lookupInstance(instanceName);
  if (!inst) {
    return "{}";
  }
  int32_t timers = 0;
  int32_t pendingOps = 0;
  {
    std::lock_guard lock{mutex_};
    for (const auto &timer : timers_) {
      if (timer.instance == inst->name) {
        ++timers;
      }
    }
    for (const auto &[id, item] : pendingAppCapabilities_) {
      (void)id;
      if (item.instance == inst->name) {
        ++pendingOps;
      }
    }
  }
  Json item;
  item["id"] = inst->id;
  item["kind"] = "js";
  item["version"] = inst->version;
  item["scripts"] = scriptNamesJson(inst->scriptPaths);
  item["scriptLoaded"] = inst->scriptLoaded;
  item["implementations"] = static_cast<int32_t>(inst->implementations.size());
  item["capabilities"] = static_cast<int32_t>(inst->capabilities.size());
  item["subscriptions"] = static_cast<int32_t>(inst->subscriptions.size());
  item["timers"] = timers;
  item["pendingAppCapabilities"] = pendingOps;
  item["jsRuns"] = inst->jsRuns.load();
  item["errors"] = inst->errors.load(std::memory_order_relaxed);
  item["jsHeapBytes"] = inst->jsHeapBytes.load(std::memory_order_relaxed);
  /// 异步裁决统计 (裁决处理器返回 Promise 的情况)
  item["asyncDispatchSettled"] =
      inst->asyncDispatchSettled.load(std::memory_order_relaxed);
  return item.dump();
}

std::string JsEngine::bridgeNetCall(const std::string &instanceName,
                                    const std::string &op,
                                    const std::string &argsJson) {
  auto inst = lookupInstance(instanceName);
  if (!inst || !mgr_) {
    return R"({"ok":false,"error":"no_instance","message":"插件实例已停止"})";
  }
  std::string out;
  const int32_t rc = mgr_->netCall(inst->id, op, argsJson, out);
  if (rc != PLUGINXX_RESULT_OK) {
    Json err = Json::object();
    err["ok"] = false;
    err["error"] = "call_failed";
    err["code"] = rc;
    err["message"] = "插件框架没有执行这个网络操作";
    return err.dump();
  }
  return out.empty() ? std::string{"{\"ok\":true}"} : out;
}

std::string JsEngine::bridgeNetCancel(const std::string &instanceName,
                                      int64_t requestId) {
  auto inst = lookupInstance(instanceName);
  if (!inst || !mgr_) {
    return R"({"ok":false,"error":"no_instance","message":"插件实例已停止"})";
  }
  std::string out;
  const int32_t rc = mgr_->netCancel(inst->id, requestId, out);
  if (rc != PLUGINXX_RESULT_OK) {
    Json err = Json::object();
    err["ok"] = false;
    err["error"] = "cancel_failed";
    err["code"] = rc;
    return err.dump();
  }
  return out.empty() ? std::string{"{\"ok\":true}"} : out;
}

void JsEngine::deliverNetEvent(const std::string &pluginId,
                               const std::string &payloadJson) {
  auto inst = lookupInstance("js:" + pluginId);
  if (!inst || !inst->scriptLoaded) {
    return;
  }
  const std::string name = inst->name;
  postTask([this, name, payloadJson] {
    const auto target = lookupInstance(name);
    if (!target) {
      return;
    }
    std::string ignored;
    callBridgeString(target, "onNetEvent", {payloadJson}, ignored);
  });
}

void JsEngine::sampleHeap(const std::shared_ptr<Instance> &inst) {
#if defined(MUSICXX_EXTERN_PLUGIN_HAS_JS)
  if (!inst || !inst->rt) {
    return;
  }
  auto *rt = static_cast<JSRuntime *>(inst->rt);
  JSMemoryUsage usage{};
  JS_ComputeMemoryUsage(rt, &usage);
  /// 只取本实例 runtime 的 JS 堆占用 (malloc/字符串/对象/函数等本体大小)
  const int64_t bytes = usage.malloc_size + usage.str_size + usage.obj_size +
                        usage.prop_size + usage.shape_size +
                        usage.js_func_size + usage.js_func_pc2line_size +
                        usage.c_func_count * 0 + usage.atom_size;
  inst->jsHeapBytes.store(bytes, std::memory_order_relaxed);
#else
  (void)inst;
#endif
}

int64_t JsEngine::jsHeapBytesOf(const std::string &instanceName) const {
  auto inst = lookupInstance(instanceName);
  return inst ? inst->jsHeapBytes.load(std::memory_order_relaxed) : -1;
}

int32_t JsEngine::callCapability(const std::string &callerInstance,
                                 const std::string &targetId,
                                 const std::string &name,
                                 const std::string &argsJson,
                                 uint32_t timeoutMs, int64_t *outPendingId,
                                 std::string &outJson, std::string &err) {
  if (outPendingId) {
    *outPendingId = 0;
  }
  if (targetId.empty() || name.empty()) {
    err = "插件 id 与能力名都不能为空";
    return PLUGINXX_RESULT_ERR_ARG;
  }
  /// JS 目标: 同一 JS 线程上直接调用 (脚本↔脚本零跨线程跳跃)
  auto target = lookupInstance("js:" + targetId);
  if (target) {
    if (!target->scriptLoaded) {
      err = "目标 JS插件未启用: " + targetId;
      return PLUGINXX_RESULT_ERR_STATE;
    }
    std::string shortName = name;
    const std::string prefix = "plugin." + targetId + ".";
    if (shortName.rfind("plugin.", 0) == 0) {
      if (shortName.rfind(prefix, 0) != 0) {
        err = "能力命名空间不属于目标插件";
        return PLUGINXX_RESULT_ERR_PERMISSION;
      }
      shortName = shortName.substr(prefix.size());
    }
    /// 等待号: 处理器的 **同步返回** 当场就有结果; 返回 Promise 时它按这个号
    /// 在结算后经 `ext.capabilityWaitResolve` 交回来, 这条路以前会直接失败
    /// (`capability_async_without_wait`), 现在与"JS → 原生"一样能拿到结果。
    const int64_t pendingId = nextJsCallWaitId_.fetch_add(1);
    std::string text;
    if (!callBridgeString(target, "invokeCapability",
                          {shortName, argsJson, std::to_string(pendingId)},
                          text)) {
      err = "目标插件未声明该能力 (需要在 start 事务里注册): " + shortName;
      return PLUGINXX_RESULT_ERR_NOT_FOUND;
    }
    if (text == kCapabilityPendingMarker) {
      {
        std::lock_guard lock{mutex_};
        jsCallWaits_[pendingId] = std::make_pair(callerInstance, target->name);
      }
      if (outPendingId) {
        *outPendingId = pendingId;
      }
      return kCallCapabilityPending;
    }
    bool ok = false;
    Json data = parseJsonSafe(text, &ok);
    const bool fine = ok && data.is_object() && data.contains("ok") &&
                      data["ok"].is_boolean() && data["ok"].get<bool>();
    if (!fine) {
      err = (ok && data.contains("error") && data["error"].is_string())
                ? data["error"].get<std::string>()
                : std::string{"capability_call_failed"};
      return PLUGINXX_RESULT_ERR_INTERNAL;
    }
    outJson =
        data.contains("result") ? data["result"].dump() : std::string{"null"};
    return PLUGINXX_RESULT_OK;
  }
  /// 原生/内置目标: 投递到框架线程执行, **JS 侧不等待** (框架线程可能正等 JS
  /// 处理器, 若在这里同步等待就会互锁); 结果经 `ext.onCapabilityResult` 回到 JS
  /// 线程。
  if (!mgr_) {
    err = "插件框架不可用";
    return PLUGINXX_RESULT_ERR_STATE;
  }
  const int64_t pendingId = nextCapabilityCallId_.fetch_add(1);
  if (outPendingId) {
    *outPendingId = pendingId;
  }
  auto self = shared_from_this();
  auto finishCall = [self, pendingId, callerInstance](int32_t rc,
                                                      const std::string &text) {
    Json payload;
    payload["ok"] = (rc == PLUGINXX_RESULT_OK);
    if (rc == PLUGINXX_RESULT_OK) {
      payload["result"] = text.empty() ? Json(nullptr) : parseJsonSafe(text);
    } else {
      payload["error"] =
          text.empty() ? std::string{"capability_call_failed"} : text;
    }
    const std::string json = payload.dump();
    /// 回执必须回到 JS 线程解析 (promise 只能在 JS 线程结算)
    self->postTask([self, pendingId, callerInstance, json] {
      auto inst = self->lookupInstance(callerInstance);
      if (!inst || !inst->ctx) {
        return;
      }
      std::string ignored;
      self->callBridgeString(inst, "onCapabilityResult",
                             {std::to_string(pendingId), json}, ignored);
    });
  };
  mgr_->pluginCallAsync(targetId, name, argsJson, std::move(finishCall));
  return kCallCapabilityPending;
}
void JsEngine::logPlugin(const std::string &pluginId, int32_t level,
                         const std::string &message) {
  if (message.empty()) {
    return;
  }
  Json payload;
  payload["id"] = pluginId;
  payload["level"] = level;
  payload["message"] = message;
  if (mgr_) {
    mgr_->pushEvent("musicxx.plugin.log", pluginId, payload.dump());
  }
  XX_LOGW("[musicxx_ext][js:{}] {}", pluginId, message);
}

/* ==================== 实例生命周期 ==================== */

std::shared_ptr<JsEngine::Instance>
JsEngine::instanceFromHost(const PluginxxHost *host, std::string &err) {
  SlotTable &table = slotTable();
  auto engine = table.engine.load(std::memory_order_acquire);
  if (!engine) {
    err = "JS 运行时未创建";
    return nullptr;
  }
  auto control = pluginxx::resolvePluginHostControl(host);
  if (!control) {
    err = "host 视图无效";
    return nullptr;
  }
  auto base = control->instance();
  if (!base) {
    err = "插件实例已关闭";
    return nullptr;
  }
  /// 本引擎只在本框架内使用, 实例必然是 [MusicxxHostInstance]
  auto *hostInst = static_cast<MusicxxHostInstance *>(base.get());
  if (hostInst->name.rfind("js:", 0) != 0) {
    err = "实例名不是 JS 合成实例: " + hostInst->name;
    return nullptr;
  }
  const std::string pluginId = MusicxxHostManager::pluginIdOf(hostInst->name);
  const std::vector<std::string> scripts = slotScripts(hostInst->name);
  if (scripts.empty()) {
    err = "JS插件槽位不存在 (未登记或已注销): " + hostInst->name;
    return nullptr;
  }

  auto inst = std::make_shared<Instance>();
  inst->engine = engine;
  inst->id = pluginId;
  inst->name = hostInst->name;
  inst->scriptPaths = scripts;
  inst->configPath = hostInst->configPath;
  inst->version = slotVersion(hostInst->name);
  inst->argsJson =
      hostInst->args.is_object() ? hostInst->args.dump() : std::string{"{}"};
  inst->hostInst = hostInst;

  {
    std::lock_guard lock{engine->mutex_};
    if (engine->instances_.count(inst->name) > 0) {
      err = "JS 实例已存在: " + inst->name;
      return nullptr;
    }
    engine->instances_[inst->name] = inst;
  }
  return inst;
}

std::shared_ptr<JsEngine::Instance>
JsEngine::lookupInstance(const std::string &name) const {
  std::lock_guard lock{mutex_};
  auto it = instances_.find(name);
  return it == instances_.end() ? std::shared_ptr<Instance>{} : it->second;
}

int32_t JsEngine::startInstance(const std::shared_ptr<Instance> &inst,
                                std::string &err) {
  if (!inst) {
    err = "实例为空";
    return PLUGINXX_RESULT_ERR_ARG;
  }
  std::string info;
  if (mgr_) {
    // JS 插件共用这份缓存：接口代次按 JS 形态报（不是动态库插件用的 C 接口代次）
    mgr_->hostInfoJson(info, std::string_view{}, "js");
  }
  setHostInfoCache(info.empty() ? std::string{"{}"} : info);

  if (!runScriptOnJsThread(inst, err)) {
    /// 脚本执行失败: 释放上下文 (注册回放还没开始, 无需摘注册)
    if (inst->ctx || inst->rt) {
      postTask([inst] {
#if defined(MUSICXX_EXTERN_PLUGIN_HAS_JS)
        freeInstanceContextOnJsThread(*inst, false);
#endif
      });
    }
    return PLUGINXX_RESULT_ERR_STATE;
  }
  applyRegistrations(inst);
  inst->scriptLoaded = true;
  return PLUGINXX_RESULT_OK;
}

void JsEngine::stopInstance(const std::shared_ptr<Instance> &inst) {
  if (!inst) {
    return;
  }
  detachRegistrations(inst);
  clearDispatchWaitsOf(inst->name);
  clearCapabilityWaitsOf(inst->name);
  if ((inst->ctx || inst->rt) && running_.load(std::memory_order_acquire)) {
    auto slot = std::make_shared<WaitSlot<bool>>();
    postTask([inst, slot] {
#if defined(MUSICXX_EXTERN_PLUGIN_HAS_JS)
      freeInstanceContextOnJsThread(*inst, true);
#endif
      slot->set(true);
    });
    bool ok = false;
    /// 不设等待上界: 上下文释放要么完成, 要么进程已经退出
    slot->wait(ok);
    (void)ok;
  }
  inst->scriptLoaded = false;
}

void JsEngine::destroyInstance(const std::shared_ptr<Instance> &inst) {
  if (!inst) {
    return;
  }
  stopInstance(inst);
  {
    std::lock_guard lock{mutex_};
    auto it = instances_.find(inst->name);
    if (it != instances_.end() && it->second == inst) {
      instances_.erase(it);
    }
    std::erase_if(timers_, [&](const Timer &timer) {
      return timer.instance == inst->name;
    });
  }
}

bool JsEngine::runScriptOnJsThread(const std::shared_ptr<Instance> &inst,
                                   std::string &err) {
#if !defined(MUSICXX_EXTERN_PLUGIN_HAS_JS)
  err = "本次构建未包含 JS 运行时 (未编译 QuickJS 子模块)";
  return false;
#else
  // 清单里的多个脚本在**同一个 JS 上下文**里按顺序执行：前面（通常是随插件分发的
  // kit 文件）先跑完，插件脚本才跑。任一脚本报错都算加载失败，错误里带上脚本名。
  std::vector<std::pair<std::string, std::string>> scripts;
  scripts.reserve(inst->scriptPaths.size());
  for (const std::string &path : inst->scriptPaths) {
    std::string code = readTextFile(fs::path{path});
    if (code.empty()) {
      err = "脚本读取失败或为空: " + path;
      return false;
    }
    scripts.emplace_back(path, std::move(code));
  }
  auto slot = std::make_shared<WaitSlot<std::string>>(); ///< 空串 = 成功
  const std::string prelude{kPrelude};
  postTask([inst, slot, scripts, prelude] {
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) {
      slot->set("JSRuntime 创建失败");
      return;
    }
    /// 栈深度上限 (插件框架自我保护, 避免脚本深递归打爆线程栈;
    /// 不是对插件的资源限制)
    JS_SetMaxStackSize(rt, 1024 * 1024);
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) {
      JS_FreeRuntime(rt);
      slot->set("JSContext 创建失败");
      return;
    }
    JS_SetContextOpaque(ctx, inst.get());
    inst->rt = rt;
    inst->ctx = ctx;

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__ext", buildBridgeObject(ctx));
    JS_FreeValue(ctx, global);

    auto eval = [&](const std::string &source,
                    const char *file) -> std::string {
      JSValue value =
          JS_Eval(ctx, source.data(), source.size(), file, JS_EVAL_TYPE_GLOBAL);
      if (JS_IsException(value)) {
        std::string text = takeException(ctx);
        JS_FreeValue(ctx, value);
        return text.empty() ? std::string{"脚本执行失败"} : text;
      }
      JS_FreeValue(ctx, value);
      return {};
    };

    std::string error = eval(prelude, "<musicxx-prelude>");
    for (const auto &[scriptPath, code] : scripts) {
      if (!error.empty()) {
        break;
      }
      error = eval(code, scriptPath.c_str());
      if (!error.empty()) {
        // 带脚本名：多个脚本时能直接看出是哪一个出错
        error = "脚本 " + scriptPath + ": " + error;
        break;
      }
    }
    if (!error.empty()) {
      slot->set("脚本错误: " + error);
      return;
    }
    // 清空顶层微任务 (顶层不允许 await, 一般没有)
    // 注意: JS_ExecutePendingJob 要求非空上下文出参 (它对 pctx 无条件解引用)
    JSContext *jobCtx = nullptr;
    while (JS_ExecutePendingJob(rt, &jobCtx) > 0) {
      // 清空顶层微任务
    }
    slot->set(std::string{});
  });

  std::string result;
  /// 不设等待上界: 脚本跑多久就等多久 (死循环会占住共享 JS 线程, 由作者自己避免)
  slot->wait(result);
  if (!result.empty()) {
    err = result;
    ++inst->errors;
    return false;
  }
  ++inst->jsRuns;
  return true;
#endif
}

bool JsEngine::callBridgeString(const std::shared_ptr<Instance> &inst,
                                const char *fnName,
                                const std::vector<std::string> &args,
                                std::string &out) {
  if (!inst) {
    return false;
  }
#if !defined(MUSICXX_EXTERN_PLUGIN_HAS_JS)
  (void)fnName;
  (void)args;
  (void)out;
  return false;
#else
  if (!inst->ctx) {
    return false;
  }
  auto *ctx = static_cast<JSContext *>(inst->ctx);
  JSValue global = JS_GetGlobalObject(ctx);
  JSValue ext = JS_GetPropertyStr(ctx, global, "__ext");
  JSValue fn =
      JS_IsObject(ext) ? JS_GetPropertyStr(ctx, ext, fnName) : JS_UNDEFINED;
  bool ok = false;
  if (JS_IsFunction(ctx, fn)) {
    std::vector<JSValue> argv;
    argv.reserve(args.size());
    for (const auto &arg : args) {
      argv.push_back(JS_NewStringLen(ctx, arg.data(), arg.size()));
    }
    JSValue value =
        JS_Call(ctx, fn, JS_UNDEFINED, static_cast<int>(argv.size()),
                argv.empty() ? nullptr : argv.data());
    if (JS_IsException(value)) {
      const std::string text = takeException(ctx);
      XX_LOGW("[musicxx_ext] JS 桥接调用 `{}` 异常: {}", fnName, text);
      ++inst->errors;
    } else {
      if (JS_IsString(value)) {
        const char *text = JS_ToCString(ctx, value);
        if (text) {
          out = text;
          JS_FreeCString(ctx, text);
        }
      }
      ok = true;
    }
    JS_FreeValue(ctx, value);
    for (auto &item : argv) {
      JS_FreeValue(ctx, item);
    }
  }
  JS_FreeValue(ctx, fn);
  JS_FreeValue(ctx, ext);
  JS_FreeValue(ctx, global);
  return ok;
#endif
}

/* ==================== 注册回放 / 摘除 ==================== */

void JsEngine::applyRegistrations(const std::shared_ptr<Instance> &inst) {
  if (!inst || !inst->hostInst || !mgr_) {
    return;
  }
  auto collected = std::make_shared<std::string>();
  auto slot = std::make_shared<WaitSlot<bool>>();
  postTask([this, inst, collected, slot] {
    std::string text;
    const bool ok = callBridgeString(inst, "collectRegistrations", {}, text);
    if (ok) {
      *collected = std::move(text);
    }
    slot->set(ok);
  });
  bool ok = false;
  slot->wait(ok);
  if (!ok) {
    XX_LOGW("[musicxx_ext] JS 实例 `{}` 注册信息回取失败", inst->name);
    return;
  }

  bool parseOk = false;
  Json data = parseJsonSafe(*collected, &parseOk);
  if (!parseOk || !data.is_object()) {
    XX_LOGW("[musicxx_ext] JS 实例 `{}` 注册信息不是合法 JSON", inst->name);
    return;
  }

  // ---- 能力 ----
  if (data.contains("implementations") && data["implementations"].is_array()) {
    for (const auto &item : data["implementations"]) {
      if (!item.is_object() || !item.contains("capability") ||
          !item["capability"].is_string()) {
        continue;
      }
      auto handler = std::make_shared<Implementation>();
      handler->engine = this;
      handler->instance = inst->name;
      handler->pluginId = inst->id;
      handler->capabilityId = item["capability"].get<std::string>();
      handler->ownerTag =
          (item.contains("ownerTag") && item["ownerTag"].is_string())
              ? item["ownerTag"].get<std::string>()
              : std::string{};
      /// 阶段: 能力 id 表示一次调用, 参与方式由阶段表达 (老脚本不写 = impl)
      handler->phase = capabilityPhaseOfName(
          (item.contains("phase") && item["phase"].is_string())
              ? item["phase"].get<std::string>()
              : std::string{"impl"});

      MusicxxPluginCapabilitySpec spec{};
      spec.phase = handler->phase;
      spec.capability_id =
          PluginxxStringView{handler->capabilityId.data(), handler->capabilityId.size()};
      spec.owner_tag = PluginxxStringView{handler->ownerTag.data(),
                                          handler->ownerTag.size()};
      spec.priority =
          (item.contains("priority") && item["priority"].is_number_integer())
              ? item["priority"].get<int>()
              : 0;
      spec.user_data = handler.get();
      spec.run_sync = &JsEngine::implementationRunSync;
      const int32_t rc = mgr_->registerHandler(inst->hostInst, spec);
      if (rc != PLUGINXX_RESULT_OK) {
        Json payload;
        payload["id"] = inst->id;
        payload["phase"] = "capability";
        payload["capability"] = handler->capabilityId;
        payload["code"] = "capability_register_failed";
        payload["message"] = "能力注册被拒绝 (这条 id 没开这个阶段 / 阶段必须用同步处理器)";
        mgr_->pushEvent("musicxx.plugin.error", inst->id, payload.dump());
        XX_LOGW("[musicxx_ext] JS插件 `{}` 注册能力 `{}` 被拒绝 (code={})",
                inst->id, handler->capabilityId, rc);
        continue;
      }
      inst->implementations.push_back(handler);
    }
  }

  // ---- 能力 ----
  if (data.contains("capabilities") && data["capabilities"].is_array()) {
    for (const auto &item : data["capabilities"]) {
      if (!item.is_string()) {
        continue;
      }
      auto handler = std::make_shared<CapabilityHandler>();
      handler->engine = this;
      handler->instance = inst->name;
      handler->pluginId = inst->id;
      handler->shortName = item.get<std::string>();
      handler->fullName = "plugin." + inst->id + "." + handler->shortName;
      PluginxxCapabilitySpec spec{};
      spec.version = PLUGINXX_API_VERSION;
      spec.struct_size = sizeof(PluginxxCapabilitySpec);
      const PluginxxStringView nameView{handler->fullName.data(),
                                        handler->fullName.size()};
      spec.id = nameView;
      spec.impls = PLUGINXX_CAPABILITY_IMPLS_ONE;
      spec.merge = PLUGINXX_CAPABILITY_MERGE_NONE;
      spec.phases = PLUGINXX_CAPABILITY_PHASE_MASK_IMPL;
      spec.phase = PLUGINXX_CAPABILITY_PHASE_IMPL;
      spec.run_start = &JsEngine::capabilityStart;
      spec.user_data = handler.get();
      std::string registerError;
      const int32_t rc = mgr_->registerCapability(
          inst->hostInst, nullptr, spec, &registerError);
      if (rc != 0) {
        XX_LOGW("[musicxx_ext] JS插件 `{}` 注册能力 `{}` 失败 (code={})",
                inst->id, handler->fullName, rc);
        continue;
      }
      inst->capabilities.push_back(handler);
    }
  }

  // ---- 广播声明 ----
  int32_t broadcastsApplied = 0;
  if (data.contains("broadcasts") && data["broadcasts"].is_array()) {
    for (const auto &item : data["broadcasts"]) {
      if (!item.is_object()) {
        continue;
      }
      if (applyBroadcastDeclOnHost(inst, item.dump()) == PLUGINXX_RESULT_OK) {
        ++broadcastsApplied;
      }
    }
  }

  // ---- 广播订阅 (带各自的频率选项) ----
  if (data.contains("subscriptions") && data["subscriptions"].is_array()) {
    const PluginxxHost *host = inst->hostInst->hostView();
    for (const auto &item : data["subscriptions"]) {
      if (!item.is_object() || !host) {
        continue;
      }
      if (!item.contains("topic") || !item["topic"].is_string()) {
        continue;
      }
      auto holder = std::make_shared<SubscriptionHolder>();
      holder->engine = this;
      holder->instance = inst->name;
      holder->topic = item["topic"].get<std::string>();
      holder->options =
          (item.contains("options") && item["options"].is_string())
              ? item["options"].get<std::string>()
              : std::string{};
      holder->weakSelf = holder;
      // 广播表的 IID 由应用名拼出 (`<应用名>.broadcasts`), 不写字面量
      const PluginxxStringView iid{
          PLUGINXX_IFACE_BROADCASTS, sizeof(PLUGINXX_IFACE_BROADCASTS) - 1};
      const auto *broadcasts = static_cast<const PluginxxBroadcastsIface *>(
          (host->vtable && host->vtable->query_interface)
              ? host->vtable->query_interface(host, &iid)
              : nullptr);
      if (!broadcasts || !broadcasts->subscribe_broadcast) {
        XX_LOGW("[musicxx_ext] 广播表不可用: JS插件 `{}` 的订阅未生效",
                inst->id);
        continue;
      }
      const PluginxxStringView topicView{holder->topic.data(),
                                         holder->topic.size()};
      const PluginxxStringView optionsView{holder->options.data(),
                                           holder->options.size()};
      const PluginxxStringView *optionsPtr =
          holder->options.empty() ? nullptr : &optionsView;
      const bool hasEnd = pluginxx::ifaceHas<
          &PluginxxBroadcastsIface::subscribe_broadcast_ex>(broadcasts)
          && broadcasts->subscribe_broadcast_ex != nullptr;
      auto *sub = hasEnd
                      ? broadcasts->subscribe_broadcast_ex(
                            host, nullptr, &topicView,
                            &JsEngine::eventTrampoline,
                            &JsEngine::endTrampoline, holder.get(), optionsPtr,
                            nullptr)
                      : broadcasts->subscribe_broadcast(
                            host, nullptr, &topicView,
                            &JsEngine::eventTrampoline, holder.get(), optionsPtr,
                            nullptr);
      if (!sub) {
        XX_LOGW("[musicxx_ext] JS插件 `{}` 订阅主题 `{}` 被拒绝", inst->id,
                holder->topic);
        continue;
      }
      inst->subscriptions.push_back(holder);
    }
  }

  // ---- UI 声明式扩展项 ----
  int32_t uiApplied = 0;
  for (const auto &op : takePendingUiOps(inst)) {
    int32_t rc = PLUGINXX_RESULT_ERR_ARG;
    if (op.op == "register") {
      MusicxxPluginUIEntrySpec spec{};
      spec.version = 1;
      spec.struct_size = sizeof(MusicxxPluginUIEntrySpec);
      spec.item_id = PluginxxStringView{op.name.data(), op.name.size()};
      spec.type = PluginxxStringView{op.type.data(), op.type.size()};
      spec.data_json =
          PluginxxStringView{op.dataJson.data(), op.dataJson.size()};
      spec.order = op.order;
      spec.flags = 0;
      rc = mgr_->registerUiEntry(inst->hostInst, spec);
    } else if (op.op == "update") {
      rc = mgr_->updateUiEntry(inst->hostInst, op.name, op.dataJson);
    } else if (op.op == "unregister") {
      rc = mgr_->unregisterUiEntry(inst->hostInst, op.name);
    }
    if (rc == PLUGINXX_RESULT_OK) {
      ++uiApplied;
    } else {
      Json payload;
      payload["id"] = inst->id;
      payload["phase"] = "ui";
      payload["op"] = op.op;
      payload["item"] = op.name;
      payload["code"] = rc;
      payload["message"] = "UI 项注册被拒绝 (检查类型/命名空间/data 结构)";
      mgr_->pushEvent("musicxx.plugin.error", inst->id, payload.dump());
      XX_LOGW("[musicxx_ext] JS插件 `{}` 注册 UI 项 `{}` 被拒绝 (code={})",
              inst->id, op.name, rc);
    }
  }

  /// 广播只有声明(上面)、发布、读、订阅: 回放完把脚本顶层存下的发布按顺序发出去
  /// (顶层的 declare + publish 初始值不能早于声明落到框架上)
  std::vector<std::pair<std::string, std::string>> queued;
  {
    std::lock_guard lock{mutex_};
    queued.swap(inst->pendingPublishes);
  }
  for (const auto &[topic, payload] : queued) {
    const int32_t rc = mgr_->broadcastPublishFrom(inst->id, topic, payload);
    if (rc != PLUGINXX_RESULT_OK) {
      XX_LOGW("[musicxx_ext] JS插件 `{}` 顶层发布 `{}` 被拒绝 (code={})",
              inst->id, topic, rc);
    }
  }
  inst->registrationsApplied = true;
  /// 运行期的 UI/订阅/变量操作改为"投递不等待" (顶层登记阶段已结束)
  inst->liveRegistrations.store(true, std::memory_order_release);
  XX_LOGI("[musicxx_ext] JS插件 `{}` 注册回放完成 (能力 {} / 能力 {} / 订阅 "
          "{} / UI {} / 广播 {})",
          inst->id, inst->implementations.size(), inst->capabilities.size(),
          inst->subscriptions.size(), uiApplied, broadcastsApplied);
}

void JsEngine::detachRegistrations(const std::shared_ptr<Instance> &inst) {
  if (!inst) {
    return;
  }
  /// 插件自己的 HTTP / TCP / WS 服务与连接由 `PluginNetHub` 按插件 id 一起收掉
  /// (`detachPlugin`; 静态路由也挂在那份服务上, 不关就会留下还在给已卸载插件打工的监听)
  if (!inst->hostInst || !mgr_) {
    return;
  }
  /// 先关掉运行期注册: 摘除过程中脚本若再注册, 只会记在待回放表里
  /// (随实例销毁丢弃)
  inst->liveRegistrations.store(false, std::memory_order_release);
  inst->pendingUiOps.clear();
  {
    std::lock_guard lock{mutex_};
    inst->pendingPublishes.clear();
  }
  /// 广播订阅由插件框架随实例 stop/卸载自动撤销; 声明出来的频道由
  /// `removeChannelsOwnedBy` 随实例一起摘除
  for (const auto &handler : inst->implementations) {
    mgr_->unimplement(inst->hostInst, handler->capabilityId, handler->ownerTag);
  }
  inst->implementations.clear();
  for (const auto &handler : inst->capabilities) {
    mgr_->unregisterCapability(inst->hostInst, handler->fullName,
                               std::string_view{});
  }
  inst->capabilities.clear();
  /// 事件订阅由插件框架随实例 stop/卸载自动撤销 (订阅登记在实例上)
  inst->subscriptions.clear();
  inst->registrationsApplied = false;
}

/* ==================== 能力处理器 ==================== */

int32_t PLUGINXX_CALL JsEngine::implementationRunSync(void *userData,
                                         const PluginxxStringView * /*capabilityId*/,
                                         const PluginxxStringView *inputJson,
                                         PluginxxString *outJson,
                                         PluginxxString * /*errorOut*/
) {
  auto *handler = static_cast<Implementation *>(userData);
  if (!handler || !handler->engine) {
    return -1;
  }
  JsEngine *engine = handler->engine;
  /// 处理器对象的生命周期由实例持有 (卸载/禁用会摘除注册):
  /// 等待期间它可能已经释放, 因此这里先把要用的字段复制出来, 不在等待之后再读
  /// handler。
  const std::string pluginId = handler->pluginId;
  const std::string capabilityId = handler->capabilityId;
  const std::string name = handler->instance;
  const std::string phaseName = capabilityPhaseNameOf(handler->phase);
  const std::string input =
      inputJson ? viewToStr(inputJson) : std::string{"{}"};
  auto slot = std::make_shared<WaitSlot<std::string>>();
  /// 等待槽 id: Promise 结算后由 JS 侧经 `ext.implementationWaitResolve` 交回结果
  const int64_t waitId = engine->beginDispatchWait(name, slot);
  engine->postTask([engine, name, capabilityId, phaseName, input, slot, waitId] {
    const auto inst = engine->lookupInstance(name);
    std::string result;
    if (!inst || !engine->callBridgeString(
                     inst, "invokeImplementation",
                     {capabilityId, phaseName, input, std::to_string(waitId)},
                     result)) {
      engine->finishDispatchWait(waitId);
      slot->set(std::string{});
      return;
    }
    if (result == kPendingMarker) {
      /// 处理器返回 Promise: 不结算, 等 [bridgeImplementationWaitResolve] 把结果交回来
      return;
    }
    engine->finishDispatchWait(waitId);
    slot->set(std::move(result));
  });

  /// 不设等待上界: 处理器返回 Promise 时一直等到它结算
  /// (插件框架不因为处理器慢而按"无裁决"继续)
  std::string result;
  slot->wait(result);
  if (result.empty() || result == "null") {
    return 0;
  }
  pluginxx::hostMemorySetString(outJson, result);
  return 0;
}

/* ==================== 能力处理器 ==================== */

void *JsEngine::capabilityStart(void *ctx, const PluginxxHost * /*callerHost*/,
                                const PluginxxStringView * /*method*/,
                                const PluginxxStringView *argsJson,
                                const PluginxxOperatorNotify *notify,
                                PluginxxString *errorOut) {
  auto *handler = static_cast<CapabilityHandler *>(ctx);
  if (!handler || !handler->engine) {
    return nullptr;
  }
  JsEngine *engine = handler->engine;
  const std::string args = argsJson ? viewToStr(argsJson) : std::string{"{}"};
  const std::string shortName = handler->shortName;
  auto slot = std::make_shared<WaitSlot<std::string>>();
  const std::string name = handler->instance;
  /// 等待槽 id: 处理器返回 Promise 时, JS 侧在结算后经
  /// `ext.capabilityWaitResolve` 把结果交回来
  const int64_t waitId = engine->beginCapabilityWait(name, slot);
  engine->postTask([engine, name, shortName, args, slot, waitId] {
    const auto inst = engine->lookupInstance(name);
    std::string text;
    if (!inst ||
        !engine->callBridgeString(inst, "invokeCapability",
                                  {shortName, args, std::to_string(waitId)},
                                  text)) {
      engine->finishCapabilityWait(waitId);
      slot->set(
          std::string{R"({"ok":false,"error":"capability_invoke_failed"})"});
      return;
    }
    if (text == kCapabilityPendingMarker) {
      /// 处理器返回 Promise: 不结算, 等 [bridgeCapabilityWaitResolve]
      return;
    }
    engine->finishCapabilityWait(waitId);
    slot->set(std::move(text));
  });

  /// 不设等待上界: 能力处理器可以是异步的 (返回 Promise 时会一直等到它结算)
  std::string text;
  slot->wait(text);

  bool parseOk = false;
  Json result = parseJsonSafe(text, &parseOk);
  const bool ok = parseOk && result.is_object() && result.contains("ok") &&
                  result["ok"].is_boolean() && result["ok"].get<bool>();
  std::string payload;
  if (ok) {
    payload = result.contains("result") ? result["result"].dump()
                                        : std::string{"null"};
  } else {
    Json error;
    error["error"] =
        (parseOk && result.contains("error") && result["error"].is_string())
            ? result["error"].get<std::string>()
            : std::string{"capability_failed"};
    payload = error.dump();
  }
  if (notify && notify->done) {
    PluginxxString out{};
    if (ok) {
      out = pluginxx::hostMemoryCreateString(std::string_view{payload});
    }
    const PluginxxStringView view{out.data, out.size};
    /// 同步完成: 插件框架支持"插件同步 done" (完成通知仍由插件框架在 IO 线程发布)
    notify->done(notify->host_ud,
                 ok ? PLUGINXX_OPERATOR_OK : PLUGINXX_OPERATOR_FAILED,
                 ok ? &view : nullptr);
    if (out.data) {
      pluginxx::hostMemoryFree(out.data);
    }
  }
  return nullptr;
}

/* ==================== 事件订阅回调 ==================== */

void PLUGINXX_CALL
JsEngine::eventTrampoline(const PluginxxStringView *eventJson, void *userData) {
  auto *holder = static_cast<SubscriptionHolder *>(userData);
  if (!holder || !holder->engine) {
    return;
  }
  /// 先拿住一份引用: 投递之后记账可能被摘掉 (流结束 / 重新订阅), 任务靠它活到跑完
  const auto keep = holder->weakSelf.lock();
  if (!keep) {
    return;
  }
  const std::string payload =
      eventJson ? viewToStr(eventJson) : std::string{"{}"};
  const std::string topic = holder->topic;
  const std::string name = holder->instance;
  /// 事件回调在框架线程 (发布者线程) 执行: 只投递, 不等 JS
  holder->engine->postTask([keep, name, topic, payload] {
    JsEngine *engine = keep->engine;
    const auto inst = engine->lookupInstance(name);
    if (!inst) {
      return;
    }
    std::string ignored;
    engine->callBridgeString(inst, "onEvent", {topic, payload}, ignored);
  });
}

void PLUGINXX_CALL
JsEngine::endTrampoline(const PluginxxStringView *reasonJson, void *userData) {
  auto *holder = static_cast<SubscriptionHolder *>(userData);
  if (!holder || !holder->engine) {
    return;
  }
  /// 宿主已经随结束摘掉了这条订阅: 记账留着也不当真 (重新订阅时按它清掉)
  holder->ended.store(true, std::memory_order_release);
  const auto keep = holder->weakSelf.lock();
  if (!keep) {
    return;
  }
  const std::string reason =
      reasonJson ? viewToStr(reasonJson) : std::string{"{}"};
  const std::string topic = holder->topic;
  const std::string name = holder->instance;
  /// 与值回调同一套投递方式: 不在框架线程上跑脚本
  holder->engine->postTask([keep, name, topic, reason] {
    JsEngine *engine = keep->engine;
    const auto inst = engine->lookupInstance(name);
    if (!inst) {
      return;
    }
    std::string ignored;
    engine->callBridgeString(inst, "onEventEnd", {topic, reason}, ignored);
  });
}

/* ==================== 内置插件入口 ==================== */

const PluginxxInfo *PLUGINXX_CALL JsEngine::builtinGetInfo() {
  static const PluginxxInfo info = [] {
    PluginxxInfo value{};
    value.api_version = PLUGINXX_API_VERSION;
    value.name = PluginxxStringView{"js", 2};
    value.version = PluginxxStringView{"1.0.0", 5};
    value.description =
        PluginxxStringView{"musicxx JS plugin runtime instance", 35};
    return value;
  }();
  return &info;
}

int32_t PLUGINXX_CALL JsEngine::builtinCreate(const PluginxxHost *host,
                                              void **outCtx) {
  if (!outCtx) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  std::string err;
  auto inst = instanceFromHost(host, err);
  if (!inst) {
    XX_LOGE("[musicxx_ext] JS插件实例创建失败: {}", err);
    return PLUGINXX_RESULT_ERR_STATE;
  }
  *outCtx = inst.get();
  return PLUGINXX_RESULT_OK;
}

void *PLUGINXX_CALL JsEngine::builtinStart(void *ctx,
                                           const PluginxxOperatorNotify *notify,
                                           PluginxxString *errorOut) {
  auto *inst = static_cast<Instance *>(ctx);
  int32_t rc = PLUGINXX_RESULT_ERR_STATE;
  std::string err{"JS 实例上下文无效"};
  if (inst && inst->engine) {
    rc = inst->engine->startInstance(inst->engine->lookupInstance(inst->name),
                                     err);
  }
  if (rc != 0 && errorOut) {
    pluginxx::hostMemorySetString(errorOut, err);
  }
  /// 插件框架约定: start 事务必须恰好调用一次完成通知 (返回 nullptr = 无异步 op)
  if (notify && notify->done) {
    notify->done(notify->host_ud,
                 rc == 0 ? PLUGINXX_OPERATOR_OK : PLUGINXX_OPERATOR_FAILED,
                 nullptr);
  }
  return nullptr;
}

void *PLUGINXX_CALL JsEngine::builtinStop(void *ctx,
                                          const PluginxxOperatorNotify *notify,
                                          PluginxxString * /*errorOut*/
) {
  auto *inst = static_cast<Instance *>(ctx);
  if (inst && inst->engine) {
    inst->engine->stopInstance(inst->engine->lookupInstance(inst->name));
  }
  if (notify && notify->done) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
  }
  return nullptr;
}

void PLUGINXX_CALL JsEngine::builtinDestroy(void *ctx) {
  auto *inst = static_cast<Instance *>(ctx);
  if (inst && inst->engine) {
    inst->engine->destroyInstance(inst->engine->lookupInstance(inst->name));
  }
}

/* ==================== 能力调用 (JS 侧) ==================== */

int32_t JsEngine::requestAppCapability(MusicxxHostInstance *inst,
                                const std::string &action,
                                const std::string &argsJson, uint32_t timeoutMs,
                                int64_t *outRequestId) {
  if (!inst || action.empty() || !mgr_) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  const std::string pluginId = MusicxxHostManager::pluginIdOf(inst->name);
  const int64_t requestId = mgr_->allocateRequestId();
  int64_t effectiveTimeoutMs = 0;
  const int32_t rc = mgr_->pushAppCapabilityRequestEvent(
      pluginId, requestId, action, argsJson, timeoutMs, &effectiveTimeoutMs);
  if (rc != PLUGINXX_RESULT_OK) {
    return rc;
  }
  {
    std::lock_guard lock{mutex_};
    PendingAppCapability pending;
    pending.requestId = requestId;
    pending.instance = inst->name;
    pending.pluginId = pluginId;
    pending.capability = action;
    pending.deadlineMs = nowSteadyMs() + effectiveTimeoutMs;
    pendingAppCapabilities_[requestId] = std::move(pending);
  }
  cv_.notify_all();
  inst->appCapabilityRequests.fetch_add(1);
  if (outRequestId) {
    *outRequestId = requestId;
  }
  return PLUGINXX_RESULT_OK;
}

bool JsEngine::respondAppCapability(int64_t requestId, int32_t status,
                             const std::string &resultJson) {
  PendingAppCapability pending;
  {
    std::lock_guard lock{mutex_};
    auto it = pendingAppCapabilities_.find(requestId);
    if (it == pendingAppCapabilities_.end()) {
      return false;
    }
    pending = it->second;
    pendingAppCapabilities_.erase(it);
  }
  /// 回复来自 Dart 线程: 只投递到 JS 线程执行 (绝不在调用线程碰 JSContext)
  const std::string text =
      resultJson.empty() ? std::string{"null"} : resultJson;
  const std::string idText = std::to_string(requestId);
  const std::string statusText = std::to_string(status);
  const std::string name = pending.instance;
  postTask([this, name, idText, statusText, text] {
    const auto inst = lookupInstance(name);
    if (!inst) {
      return;
    }
    std::string ignored;
    callBridgeString(inst, "onAppCapabilityDone", {idText, statusText, text}, ignored);
  });
  return true;
}

void JsEngine::cancelAppCapabilitiesOf(const std::string &instanceName) {
  std::vector<PendingAppCapability> cancelled;
  {
    std::lock_guard lock{mutex_};
    for (auto it = pendingAppCapabilities_.begin(); it != pendingAppCapabilities_.end();) {
      if (it->second.instance == instanceName) {
        cancelled.push_back(it->second);
        it = pendingAppCapabilities_.erase(it);
      } else {
        ++it;
      }
    }
  }
  if (cancelled.empty()) {
    return;
  }
  const std::string name = instanceName;
  postTask([this, name, cancelled] {
    const auto inst = lookupInstance(name);
    if (!inst) {
      return;
    }
    for (const auto &item : cancelled) {
      std::string ignored;
      callBridgeString(
          inst, "onAppCapabilityDone",
          {std::to_string(item.requestId), "1", R"({"error":"cancelled"})"},
          ignored);
    }
  });
  if (mgr_) {
    for (const auto &item : cancelled) {
      Json payload;
      payload["requestId"] = item.requestId;
      payload["reason"] = "plugin_unloaded";
      mgr_->pushEvent("musicxx.capability.app.cancel", item.pluginId, payload.dump());
    }
  }
}

/* ==================== 统计 ==================== */

int32_t JsEngine::instanceCount() const {
  std::lock_guard lock{mutex_};
  return static_cast<int32_t>(instances_.size());
}

std::string JsEngine::statsJson() const {
  Json plugins = Json::array();
  {
    std::lock_guard lock{mutex_};
    for (const auto &[name, inst] : instances_) {
      (void)name;
      int32_t timerCount = 0;
      for (const auto &timer : timers_) {
        if (timer.instance == inst->name) {
          ++timerCount;
        }
      }
      int32_t pendingCount = 0;
      for (const auto &[id, pending] : pendingAppCapabilities_) {
        (void)id;
        if (pending.instance == inst->name) {
          ++pendingCount;
        }
      }
      Json item;
      item["id"] = inst->id;
      item["instance"] = inst->name;
      item["kind"] = "js";
      item["version"] = inst->version;
      item["scripts"] = scriptNamesJson(inst->scriptPaths);
      item["scriptLoaded"] = inst->scriptLoaded;
      item["implementations"] = static_cast<int32_t>(inst->implementations.size());
      item["capabilities"] = static_cast<int32_t>(inst->capabilities.size());
      item["subscriptions"] = static_cast<int32_t>(inst->subscriptions.size());
      item["timers"] = timerCount;
      item["pendingAppCapabilities"] = pendingCount;
      item["jsRuns"] = inst->jsRuns.load();
      item["errors"] = inst->errors.load();
      item["jsHeapBytes"] = inst->jsHeapBytes.load(std::memory_order_relaxed);
      /// 异步裁决统计 (裁决处理器返回 Promise)
      item["asyncDispatchSettled"] =
          inst->asyncDispatchSettled.load(std::memory_order_relaxed);
      plugins.push_back(item);
    }
  }
  Json result;
  result["available"] = kJsCompiled;
  result["running"] = running_.load(std::memory_order_acquire);
  result["threads"] = 1;
  /// 共享 JS 线程的排队情况: 队列越深 / 等待越长,
  /// 说明某个脚本正长时间占住线程 (插件框架不打断它, 只在管理页展示)
  {
    std::lock_guard lock{mutex_};
    result["queueDepth"] = static_cast<int64_t>(tasks_.size());
    result["timers"] = static_cast<int64_t>(timers_.size());
    result["pendingAppCapabilities"] = static_cast<int64_t>(pendingAppCapabilities_.size());
  }
  result["queueWaitLastMs"] = queueWaitLastMs_.load(std::memory_order_relaxed);
  result["queueWaitMaxMs"] = queueWaitMaxMs_.load(std::memory_order_relaxed);
  result["plugins"] = plugins;
  return result.dump();
}

} // namespace extern_plugin
} // namespace musicxx
