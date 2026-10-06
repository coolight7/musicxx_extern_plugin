/// musicxx 外部插件原生宿主: JS 插件运行时实现 (QuickJS)
///
/// 见 js_engine.h 的设计说明。本文件包含四部分:
/// 1. 进程级内置槽位表 (`js:<pluginId>` 的合成内置插件描述);
/// 2. 共享 JS 线程与任务/定时器/动作超时循环;
/// 3. `musicxx` 全局对象的 JS 预置代码 (prelude) 与 C 桥接原语;
/// 4. 实例生命周期 (建 runtime → 执行脚本 → 回放注册 → 停止时摘除注册并释放
/// runtime)。
#include "js_engine.h"

#include "host_json.h"
#include "pluginxx/api/tables.h"
#include "pluginxx/host/abi_util.h"
#include "pluginxx/host/manifest.h"
#include "pluginxx/runtime/instance_base.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

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

/// 能力处理器返回 Promise 时的回给宿主的标记 (与 prelude 的
/// `kCapabilityPendingMarker` 一致)
constexpr const char *kCapabilityPendingMarker = "~pending~";

/// 处理器返回 Promise 时, JS 侧回给宿主的标记 (必须与 prelude 的
/// `kHookPendingMarker` 一致)
constexpr const char *kHookPendingMarker = "~pending~";

/// 编译期是否包含 QuickJS
constexpr bool kJsCompiled = MUSICXX_EXTERN_PLUGIN_HAS_JS != 0;

/// 变量接口返回码 → 面向插件作者的原因文本 (JS 侧会把它包进异常)
const char *varErrorText(int32_t rc) {
  switch (rc) {
    case MUSICXX_EXTERN_PLUGIN_ERR_ARG:
      return "参数非法 (键名/值超限)";
    case MUSICXX_EXTERN_PLUGIN_ERR_STATE:
      return "宿主未启动或未回执的请求过多";
    case MUSICXX_EXTERN_PLUGIN_ERR_JSON:
      return "值不是合法 JSON";
    case MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND:
      return "变量不存在 (官方键要先由应用声明)";
    case MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT:
      return "属主没有在超时前回答";
    case MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION:
      return "命名空间不属于本插件, 或没有写权限";
    case MUSICXX_EXTERN_PLUGIN_ERR_QUEUE_FULL:
      return "超出容量上限";
    case MUSICXX_PLUGIN_VAR_ERR_UNSUPPORTED:
      return "该变量没有声明对应能力 (读只写变量 / 写只读变量)";
    default:
      return "变量操作失败";
  }
}

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

/// 一个 JS 插件的内置槽位 (名称/脚本路径/版本)
///
/// `scripts` 按清单 `scripts` 的顺序排列: 同一个 JS 上下文里依次执行 (前面的脚本
/// 先跑完才跑后面的), 装载失败会带上出错脚本的文件名。缺省是 `[plugin.js]`。
struct Slot {
  std::string name; ///< js:<pluginId>
  std::vector<std::string> scripts;
  std::string version;
  bool used = false;
};

/// 槽位表: 槽位数量不设上限 (按需增长)
///
/// 交给内核的两份描述数组 (`infos` / `manifests`) 在每次调用时按当前槽位重建,
/// 内核拿到指针后立即取用 (注册与装载都在宿主线程串行, 不会跨调用持有旧指针)。
struct SlotTable {
  std::vector<Slot> slots; ///< 槽位 (含已注销的, 下标稳定便于复用)
  std::vector<PluginxxBuiltinInfo> infos;
  std::vector<PluginxxBuiltinManifest> manifests;
  std::mutex mutex;
  /// 当前 JS 引擎 (宿主为进程单例; 内置入口是静态函数, 只能经它找到引擎)
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

/// 注入到每个 JS 插件上下文里的 `musicxx` API
///
/// 设计: 原生只暴露少量"桥接原语" (`__ext.*`), 其余 API 面 (Promise
/// 封装、便捷方法、 工具函数) 都用 JS 实现 —— 减少 C 代码量, 也便于后续扩展 API
/// 而不动宿主二进制。
constexpr const char *kPrelude = R"JS(
(function () {
  "use strict";
  var ext = globalThis.__ext;
  if (!ext) { throw new Error("musicxx 运行时桥接缺失"); }

  var hookTable = new Map();
  var subTable = new Map();
  var capTable = new Map();
  var uiTable = new Map();
  var actionWaiters = new Map();
  var capabilityWaiters = new Map();
  var timerTable = new Map();
  /// 裁决型钩子处理器的 Promise 等待登记 (key = 宿主给的等待 id)
  var pendingHookWaits = new Map();
  /// 处理器返回 Promise 时回给宿主的标记 (与 C++ 侧 `kHookPendingMarker` 一致)。
  var kHookPendingMarker = "~pending~";
  /// 能力处理器返回 Promise 时回给宿主的标记 (与 C++ 侧
  /// `kCapabilityPendingMarker` 一致)
  var kCapabilityPendingMarker = "~pending~";
  /// 异步能力处理器还没结算的等待号 (JSON 结果由宿主等待槽接收)
  var pendingCapabilityWaits = new Map();

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
  /// UI 项名称: 允许短名或本插件全名 (宿主按短名登记, 全名去掉前缀)
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
    "playing.background": "musicxx.ui.playing.background"
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

  musicxx.hooks = {
    register: function (id, options, fn) {
      assertName(id, "hooks.register: 钩子 id");
      if (typeof options === "function") { fn = options; options = null; }
      options = options || {};
      if (typeof fn !== "function") { throw new Error("hooks.register: 缺少处理函数"); }
      var entry = {
        mode: options.mode === "observe" ? "observe" : "decision",
        priority: Number.isFinite(options.priority) ? Math.trunc(options.priority) : 0,
        ownerTag: typeof options.ownerTag === "string" ? options.ownerTag : "",
        fn: fn
      };
      hookTable.set(id, entry);
      /// 运行期注册要通知宿主 (顶层登记阶段由引擎统一回放, 这里只是先记下来)
      ext.hookOp("register", String(id), entry.mode, entry.priority, entry.ownerTag);
      return musicxx;
    },
    unregister: function (id) {
      var key = String(id);
      var entry = hookTable.get(key);
      hookTable.delete(key);
      ext.hookOp("unregister", key, "", 0, entry ? entry.ownerTag : "");
      return musicxx;
    },
    has: function (id) { return hookTable.has(String(id)); }
  };

  musicxx.state = {
    get: function (key) {
      assertName(key, "state.get: 键");
      var text = ext.stateGet(key);
      if (!text) { return null; }
      try { return JSON.parse(text); } catch (e) { return null; }
    }
  };

  musicxx.host = {
    info: function () {
      try { return JSON.parse(ext.hostInfo() || "{}"); } catch (e) { return {}; }
    },
    log: function (level, message) {
      ext.log(typeof level === "number" ? level : 2, String(message));
    },
    configPath: function () { return ext.configPath(); }
  };

  /* ---- 变量 (第五条通道: 有属主、可读、可写、可订阅变动的小值) ----
   *
   * 三条读法按需要选一条:
   *   await get(key)  异步、权威 (宿主向属主取一次真实值);
   *   peek(key)       同步、读缓存 (可能旧, 返回里带 revision/ageMs/stale);
   *   bind(key, fn)   订阅变动 (由宿主推送)。
   *
   * 键: 官方 `musicxx.<域>.<名>` 直接写全名; 自己的变量写短名 (如 `tip.start`),
   * 宿主自动补 `plugin.<本插件id>.` 前缀。
   */
  // 变量：注册
  var varsTable = new Map();       // 本插件注册的变量 (短名 → 声明)
  var varBindTable = new Map();    // 键 → [回调]
  var varWatchSet = new Set();     // watch 保活的键
  var varGetWaiters = new Map();   // 请求号 → {resolve, reject, timer}
  var varSetWaiters = new Map();   // 请求号 → {resolve, reject, timer}
  var varHandlers = new Map();     // handler 变量: 全名 → {onRead, onWrite}
  var varTopicsReady = { changed: false, result: false, owner: false };

  function varShortKey(key) {
    var text = String(key);
    var prefix = "plugin." + musicxx.pluginId + ".";
    return text.indexOf(prefix) === 0 ? text.slice(prefix.length) : text;
  }
  function varFullKey(key) {
    assertName(key, "vars: 键");
    var text = String(key);
    if (text.indexOf("musicxx.") === 0 || text.indexOf("plugin.") === 0) { return text; }
    return "plugin." + musicxx.pluginId + "." + text;
  }
  /// 变更通知与写结果都走事件总线: 各订阅一次就够 (宿主按 key 过滤交给 JS 侧)
  ///
  /// 注意: 必须同时记进 `subTable` —— 脚本顶层调用 `ext.subscribeHost` 会被引擎
  /// 推迟到"注册回放"阶段执行, 而回放的内容正是 `collectRegistrations` 从
  /// `subTable` 里取的; 只调原语不记账的话, 顶层绑定会静默地收不到任何事件。
  function ensureVarTopics() {
    if (!varTopicsReady.changed) {
      varTopicsReady.changed = true;
      if (!subTable.has("musicxx.var.changed")) {
        subTable.set("musicxx.var.changed", function () {});
      }
      ext.subscribeHost("musicxx.var.changed");
    }
    if (!varTopicsReady.result) {
      varTopicsReady.result = true;
      if (!subTable.has("musicxx.var.writeResult")) {
        subTable.set("musicxx.var.writeResult", function () {});
      }
      ext.subscribeHost("musicxx.var.writeResult");
    }
  }

  /// handler 属主要收"被转过来的读写请求", 也要让引擎把订阅建起来
  function ensureVarOwnerTopics() {
    if (varTopicsReady.owner) { return; }
    varTopicsReady.owner = true;
    ["musicxx.var.read", "musicxx.var.write"].forEach(function (topic) {
      if (!subTable.has(topic)) { subTable.set(topic, function () {}); }
      ext.subscribeHost(topic);
    });
  }

  /// handler 属主回答一次请求 (投递不等待; 失败只记日志)
  function varRespond(requestId, ok, value, error) {
    try {
      ext.varsRespond(Number(requestId), ok ? 1 : 0,
                      safeJson(value === undefined ? null : value),
                      error ? String(error) : "");
    } catch (e) {
      logError("变量回答失败: " + (e && e.message ? e.message : String(e)));
    }
  }

  /// 把处理器的返回值(可能是 Promise)变成一次回答
  function varAnswerWith(result, requestId) {
    if (result && typeof result.then === "function") {
      Promise.resolve(result).then(function (value) {
        varRespond(requestId, true, value, null);
        return null;
      }, function (e) {
        varRespond(requestId, false, null,
                   (e && e.message) ? e.message : String(e));
        return null;
      });
      return;
    }
    varRespond(requestId, true, result, null);
  }

  /// 属主被取真实值 (handler 模式): 交给 onRead, 返回值就是答案
  function handleVarRead(json) {
    var info = null;
    try { info = JSON.parse(json || "{}"); } catch (e) { info = null; }
    if (!info || typeof info.key !== "string") { return; }
    var entry = varHandlers.get(info.key);
    if (!entry) { return; }   // 不是自己的变量: 忽略 (别的属主会回答)
    var result;
    try {
      result = entry.onRead ? entry.onRead() : null;
    } catch (e) {
      varRespond(info.requestId, false, null,
                 "onRead 异常: " + (e && e.message ? e.message : String(e)));
      return;
    }
    varAnswerWith(result, info.requestId);
  }

  /// 属主收到写请求 (handler 模式): 交给 onWrite, 返回值就是最终值
  function handleVarWrite(json) {
    var info = null;
    try { info = JSON.parse(json || "{}"); } catch (e) { info = null; }
    if (!info || typeof info.key !== "string") { return; }
    var entry = varHandlers.get(info.key);
    if (!entry) { return; }
    if (!entry.onWrite) {
      varRespond(info.requestId, false, null,
                 "该变量是只读的 (没有提供 onWrite)");
      return;
    }
    var result;
    try {
      result = entry.onWrite(info.value);
    } catch (e) {
      varRespond(info.requestId, false, null,
                 "onWrite 异常: " + (e && e.message ? e.message : String(e)));
      return;
    }
    varAnswerWith(result, info.requestId);
  }
  function varNormalizeCaps(value) {
    if (value === undefined || value === null) { return ["get"]; }
    var list = Array.isArray(value) ? value : [value];
    var out = [];
    for (var i = 0; i < list.length; i++) {
      var item = String(list[i]);
      if (item !== "get" && item !== "set" && item !== "notify") {
        throw new Error("vars.register: 未知能力位 " + item);
      }
      if (out.indexOf(item) < 0) { out.push(item); }
    }
    if (out.length === 0) { throw new Error("vars.register: caps 不能为空"); }
    return out;
  }


  musicxx.vars = {
    /// 注册/覆盖一个变量; 返回 true (失败抛错, 原因写在异常里)
    register: function (spec) {
      if (!spec || typeof spec !== "object") {
        throw new Error("vars.register: 需要对象参数");
      }
      var key = varShortKey(spec.key === undefined ? "" : spec.key);
      assertName(key, "vars.register: key");
      var declare = {
        key: key,
        caps: varNormalizeCaps(spec.caps),
        mode: spec.mode === "handler" ? "handler" : "declared",
        write: spec.write === "owner" ? "owner" : "any",
        type: typeof spec.type === "string" ? spec.type : "",
        hasValue: spec.value !== undefined,
        value: spec.value === undefined ? null : spec.value,
        title: typeof spec.title === "string" ? spec.title : "",
        depict: typeof spec.depict === "string" ? spec.depict : "",
        options: Array.isArray(spec.options) ? spec.options : [],
        throttleMs: Number.isFinite(spec.throttleMs) ? Math.trunc(spec.throttleMs) : 0,
        notifyThrottleMs: Number.isFinite(spec.notifyThrottleMs) ? Math.trunc(spec.notifyThrottleMs) : 0,
        refreshAfterMs: Number.isFinite(spec.refreshAfterMs) ? Math.trunc(spec.refreshAfterMs) : 0
      };
      varsTable.set(key, declare);
      if (declare.mode === "handler") {
        // handler 模式: 读写都转回这里 (onRead/onWrite 只留在 JS 侧, 不进声明)
        varHandlers.set("plugin." + musicxx.pluginId + "." + key, {
          onRead: (typeof spec.onRead === "function") ? spec.onRead : null,
          onWrite: (typeof spec.onWrite === "function") ? spec.onWrite : null
        });
        ensureVarOwnerTopics();
      }
      ext.varsOp("register", key, safeJson(declare));
      return true;
    },
    unregister: function (key) {
      var short = varShortKey(key);
      varsTable.delete(short);
      varHandlers.delete(varFullKey(short));
      ext.varsOp("unregister", short, "{}");
      return true;
    },
    /// 变量是否登记过 (读注册表, 不读值)
    has: function (key) {
      if (varsTable.has(varShortKey(key))) { return true; }
      var text = ext.varsRead("info", varFullKey(key));
      return !!text;
    },
    /// 异步、权威读: 宿主向属主取一次真实值 (顺手刷新 peek 缓存)
    get: function (key, options) {
      var full = varFullKey(key);
      var budget = (options && Number.isFinite(options.timeoutMs) && options.timeoutMs > 0)
        ? Math.trunc(options.timeoutMs) : 2000;
      var control = null;
      try { control = JSON.parse(ext.varsGet(full) || "null"); }
      catch (e) { control = null; }
      if (!control || control.status !== "pending") {
        var reason = (control && control.error) ? control.error : "vars_get_failed";
        return Promise.reject(new Error("读取变量失败: " + full + " (" + reason + ")"));
      }
      return new Promise(function (resolve, reject) {
        var id = String(control.id);
        var waiter = { resolve: resolve, reject: reject, timer: 0 };
        // 表用**字符串键**: 宿主回调传进来的 id 是字符串 (数字键会让查找落空)
        waiter.timer = musicxx.timer.setTimeout(function () {
          if (varGetWaiters.delete(id)) {
            reject(new Error("读取超时: " + full));
          }
        }, budget + 500);
        varGetWaiters.set(id, waiter);
      });
    },
    /// 一次读多个键 (同键进行中请求由宿主合并); 单项失败不影响其它项
    getMany: function (keys, options) {
      var list = Array.isArray(keys) ? keys : [keys];
      var result = {};
      var jobs = [];
      for (var i = 0; i < list.length; i++) {
        (function (key) {
          var name = String(key);
          jobs.push(musicxx.vars.get(name, options).then(function (value) {
            result[name] = value;
            return null;
          }, function (e) {
            result[name] = null;
            return null;
          }));
        })(list[i]);
      }
      return Promise.all(jobs).then(function () { return result; });
    },
    /// 同步读缓存 (不保证最新); 没有缓存过返回 null
    peek: function (key) {
      var text = null;
      try { text = ext.varsRead("peek", varFullKey(key)); } catch (e) { text = null; }
      if (!text) { return null; }
      try { return JSON.parse(text); } catch (e) { return null; }
    },
    /// 保活缓存 (等价于"订阅但不回调"; 需要 notify 能力位)
    watch: function (key) {
      var full = varFullKey(key);
      varWatchSet.add(full);
      ensureVarTopics();
      ext.varsOp("watch", full, "{}");
      return true;
    },
    unwatch: function (key) {
      var full = varFullKey(key);
      varWatchSet.delete(full);
      ext.varsOp("unwatch", full, "{}");
      return true;
    },
    /// 异步写; 结果 {key, accepted, value, changed, error?}
    set: function (key, value) {
      var full = varFullKey(key);
      var control = null;
      try { control = JSON.parse(ext.varsSet(full, safeJson(value)) || "null"); }
      catch (e) { control = null; }
      if (!control || control.status !== "pending") {
        var reason = (control && control.error) ? control.error : "vars_set_failed";
        return Promise.reject(new Error("写入变量失败: " + full + " (" + reason + ")"));
      }
      return new Promise(function (resolve, reject) {
        var id = String(control.id);
        var waiter = { resolve: resolve, reject: reject, timer: 0 };
        waiter.timer = musicxx.timer.setTimeout(function () {
          if (varSetWaiters.delete(id)) {
            reject(new Error("写入未回执: " + full));
          }
        }, 6000);
        varSetWaiters.set(id, waiter);
      });
    },
    /// 绑定变动: fn(value, info); 返回解绑函数
    bind: function (key, fn) {
      var full = varFullKey(key);
      if (typeof fn !== "function") { throw new Error("vars.bind: 缺少回调"); }
      var list = varBindTable.get(full);
      var isNew = !list;
      if (!list) { list = []; varBindTable.set(full, list); }
      list.push(fn);
      if (isNew) {
        ensureVarTopics();
        ext.varsOp("subscribe", full, "{}");
      }
      return function () { musicxx.vars.unbind(full, fn); };
    },
    unbind: function (key, fn) {
      var full = varFullKey(key);
      var list = varBindTable.get(full);
      if (!list) { return true; }
      if (typeof fn === "function") {
        var index = list.indexOf(fn);
        if (index >= 0) { list.splice(index, 1); }
      } else {
        list.length = 0;
      }
      if (list.length === 0) {
        varBindTable.delete(full);
        ext.varsOp("unsubscribe", full, "{}");
      }
      return true;
    },
    /// 声明 + 缓存状态 (key/caps/mode/owner/type/title/value/revision/ageMs/stale/subscribers)
    info: function (key) {
      var text = null;
      try { text = ext.varsRead("info", varFullKey(key)); } catch (e) { text = null; }
      if (!text) { return null; }
      try { return JSON.parse(text); } catch (e) { return null; }
    },
    /// 列出变量 (prefix 可空); 值字段是缓存值
    list: function (prefix) {
      var text = null;
      try { text = ext.varsRead("list", prefix === undefined ? "" : String(prefix)); }
      catch (e) { text = null; }
      if (!text) { return []; }
      try { return JSON.parse(text); } catch (e) { return []; }
    },
    /// 本插件注册的变量声明
    own: function () {
      var out = [];
      varsTable.forEach(function (value, key) { out.push({ key: key, declare: value }); });
      return out;
    },
    /// 关心 (订阅或 watch) 该键的数量: 插件自己判断"还要不要继续高频推"的依据
    subscribers: function (key) {
      var info = musicxx.vars.info(key);
      return info && Number.isFinite(info.subscribers) ? info.subscribers : 0;
    }
  };

  /// 异步读的回执 (宿主在 JS 线程上调用)
  ext.onVarGetDone = function (id, json) {
    var key = String(id);
    var waiter = varGetWaiters.get(key);
    if (!waiter) { return; }
    varGetWaiters.delete(key);
    if (waiter.timer) { musicxx.timer.clear(waiter.timer); }
    var info = null;
    try { info = JSON.parse(json || "{}"); } catch (e) { info = null; }
    if (info && info.ok === true) {
      waiter.resolve(info.value === undefined ? null : info.value);
    } else {
      waiter.reject(new Error("读取变量失败: " + ((info && info.error) ? info.error : "read_failed")));
    }
  };

  /// 写请求的即时回执 (宿主在 JS 线程上调用)
  /// - 已经在属主那里结算完: 直接给出结果;
  /// - 交给属主写入: 结果等 `musicxx.var.writeResult` 事件 (键 = requestId)
  ext.onVarSetDone = function (id, json) {
    var key = String(id);
    var waiter = varSetWaiters.get(key);
    if (!waiter) { return; }
    var info = null;
    try { info = JSON.parse(json || "{}"); } catch (e) { info = null; }
    if (!info || info.ok !== true) {
      varSetWaiters.delete(key);
      if (waiter.timer) { musicxx.timer.clear(waiter.timer); }
      waiter.reject(new Error("写入变量失败: " + ((info && info.error) ? info.error : "write_failed")));
      return;
    }
    var result = info.result || {};
    if (result.pending === true) {
      // 等属主写入: 换到 writeResult 的等待表里 (键 = 宿主给的 requestId)
      varSetWaiters.delete(key);
      varSetWaiters.set(String(result.requestId), waiter);
      return;
    }
    varSetWaiters.delete(key);
    if (waiter.timer) { musicxx.timer.clear(waiter.timer); }
    waiter.resolve(result);
  };

  /// 变量变更 (宿主经事件总线送达; 载荷带 key/value/prev/by/revision/ts)
  function handleVarChanged(json) {
    var info = null;
    try { info = JSON.parse(json || "{}"); } catch (e) { info = null; }
    if (!info || typeof info.key !== "string") { return; }
    var list = varBindTable.get(info.key);
    if (!list || list.length === 0) { return; }
    var snapshot = list.slice();
    for (var i = 0; i < snapshot.length; i++) {
      try {
        snapshot[i](info.value, info);
      } catch (e) {
        logError("变量回调异常 (" + info.key + "): " + (e && e.message ? e.message : String(e)));
      }
    }
  }

  /// 写结果 (宿主经事件总线送达; 键 = requestId)
  function handleVarWriteResult(json) {
    var info = null;
    try { info = JSON.parse(json || "{}"); } catch (e) { info = null; }
    if (!info) { return; }
    var id = String(info.requestId);
    var waiter = varSetWaiters.get(id);
    if (!waiter) { return; }
    varSetWaiters.delete(id);
    if (waiter.timer) { musicxx.timer.clear(waiter.timer); }
    if (info.accepted === true) {
      waiter.resolve(info);
    } else {
      waiter.reject(new Error("写入被拒绝: " + (info.error || "rejected")));
    }
  }

  ext.onVarChanged = handleVarChanged;
  ext.onVarWriteResult = handleVarWriteResult;

  musicxx.call = function (name, args, timeoutMs) {
    assertName(name, "call: 动作名");
    return new Promise(function (resolve, reject) {
      var payload = safeJson(args === undefined || args === null ? {} : args);
      var budget = (typeof timeoutMs === "number" && timeoutMs > 0) ? Math.trunc(timeoutMs) : 5000;
      var id = ext.action(name, payload, budget);
      if (typeof id !== "number" || id < 0) {
        reject(new Error("动作请求未被受理: " + name + " (code=" + id + ")"));
        return;
      }
      // 表格用**字符串键**: 宿主回调 `onActionDone` 传进来的 id 是字符串
      // (数字键会让查找落空 → 动作 Promise 永远不结算, 脚本读不到任何动作结果)
      actionWaiters.set(String(id), { resolve: resolve, reject: reject });
    });
  };
  ext.onActionDone = function (id, status, json) {
    // 宿主按字符串传参: 键与状态都要先归一, 不能直接和数字比较
    var key = String(id);
    var waiter = actionWaiters.get(key);
    if (!waiter) { return; }
    actionWaiters.delete(key);
    var value = null;
    if (json) { try { value = JSON.parse(json); } catch (e) { value = json; } }
    if (Number(status) === 0) {
      waiter.resolve(value);
    } else {
      var reason = (value && value.error) ? value.error : ("status=" + status);
      waiter.reject(new Error("动作失败: " + reason));
    }
  };

  musicxx.player = {
    play: function (args) { return musicxx.call("musicxx.player.play", args); },
    pause: function () { return musicxx.call("musicxx.player.pause"); },
    toggle: function () { return musicxx.call("musicxx.player.toggle"); },
    stop: function () { return musicxx.call("musicxx.player.stop"); },
    next: function () { return musicxx.call("musicxx.player.next"); },
    prev: function () { return musicxx.call("musicxx.player.prev"); },
    seek: function (args) {
      return musicxx.call("musicxx.player.seek", typeof args === "number" ? { positionMs: args } : args);
    },
    setVolume: function (value) { return musicxx.call("musicxx.player.setVolume", { volume: value }); },
    setSpeed: function (value) { return musicxx.call("musicxx.player.setSpeed", { speed: value }); },
    setLoopMode: function (mode) { return musicxx.call("musicxx.player.setLoopMode", { mode: mode }); }
  };

  musicxx.library = {
    querySongs: function (args) { return musicxx.call("musicxx.library.querySongs", args); },
    querySonglists: function () { return musicxx.call("musicxx.library.querySonglists"); },
    playSong: function (args) { return musicxx.call("musicxx.library.playSong", args); },
    playSonglist: function (args) { return musicxx.call("musicxx.library.playSonglist", args); }
  };

  /// 应用能力导出（与 MCP 工具同一份实现）：
  /// - list() 拿能力的声明（id/说明/参数）；
  /// - call(id, args) 调一个能力；频谱/副歌这类要解码分析的长任务给更长的超时，
  ///   默认 30 秒（也可以自己传 timeoutMs）。
  musicxx.feature = {
    list: function () { return musicxx.call("musicxx.feature.list"); },
    call: function (id, args, timeoutMs) {
      assertName(id, "feature.call: 能力 id");
      var budget = (typeof timeoutMs === "number" && timeoutMs > 0) ? Math.trunc(timeoutMs) : 30000;
      return musicxx.call("musicxx.feature.call",
                          { id: String(id), args: (args || {}) }, budget);
    }
  };

  musicxx.lyrics = {
    getCurrent: function () { return musicxx.call("musicxx.lyrics.getCurrent"); }
  };

  musicxx.storage = {
    get: function (key, defaultValue) {
      return musicxx.call("musicxx.storage.get", { key: String(key) }).then(function (value) {
        if (value === null || value === undefined) {
          return defaultValue === undefined ? null : defaultValue;
        }
        return value;
      });
    },
    set: function (key, value) {
      return musicxx.call("musicxx.storage.set", { key: String(key), value: value });
    },
    remove: function (key) { return musicxx.call("musicxx.storage.delete", { key: String(key) }); },
    list: function () { return musicxx.call("musicxx.storage.list"); },
    // 插件配置文件 config.json（插件自绘的设置页改的就是这份）
    getConfig: function (key, defaultValue) {
      return musicxx.call("musicxx.storage.get", { key: String(key), namespace: "config" })
        .then(function (value) {
          if (value === null || value === undefined) {
            return defaultValue === undefined ? null : defaultValue;
          }
          return value;
        });
    },
    setConfig: function (key, value) {
      return musicxx.call("musicxx.storage.set",
                          { key: String(key), value: value, namespace: "config" });
    }
  };

  // 网络：宿主代理通道（可选便利能力；需要 musicxx.net 权限）
  // - 非 2xx 也会正常返回（status 交给插件判断）；传输失败/域名未授权时 ok=false
  musicxx.net = {
    fetch: function (options) {
      var req = (typeof options === "string") ? { url: options } : (options || {});
      if (!req.url) { return Promise.reject(new Error("net.fetch: 缺少 url")); }
      // 动作 op 的超时要比 HTTP 超时更长，否则请求还没回来 op 就先超时了
      var budget = (typeof req.timeoutMs === "number" && req.timeoutMs > 0)
        ? Math.trunc(req.timeoutMs) + 5000 : 20000;
      return musicxx.call("musicxx.net.fetch", req, budget);
    },
    download: function (options) {
      var req = (typeof options === "string") ? { url: options } : (options || {});
      if (!req.url) { return Promise.reject(new Error("net.download: 缺少 url")); }
      var budget = (typeof req.timeoutMs === "number" && req.timeoutMs > 0)
        ? Math.trunc(req.timeoutMs) + 5000 : 20000;
      return musicxx.call("musicxx.net.download", req, budget);
    }
  };

  musicxx.ui = {
    notify: function (args) {
      return musicxx.call("musicxx.ui.notify", typeof args === "string" ? { text: args } : args);
    },
    toast: function (args) {
      return musicxx.call("musicxx.ui.toast", typeof args === "string" ? { text: args } : args);
    },
    // 确认弹窗 (敏感操作前先问用户; 返回 { ok, confirmed })
    dialog: function (args) {
      return musicxx.call("musicxx.ui.dialog", typeof args === "string" ? { content: args } : args);
    },
    // 打开页面: 任意插件的声明式页面 (ext://<插件id>/<视图id>, 含本插件), 或官方白名单页面
    // (见作者文档; 跨插件页面由对方插件自己绘制)
    openRoute: function (route, args) {
      return musicxx.call("musicxx.ui.openRoute", { route: String(route), arguments: args });
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

  // 渲染槽位 (插件渲染的背景等): 查询可用样式 / 切换样式 (允许切到宿主内置样式)
  // - 未带 slot 时用宿主默认槽位 (当前是 player.background 播放页背景);
  // - select 只在"该项可用"时成功; 失败返回 { ok:false, error }.
  musicxx.render = {
    list: function (args) {
      return musicxx.call("musicxx.render.list", { slot: (args && args.slot) ? String(args.slot) : undefined });
    },
    current: function (args) {
      return musicxx.call("musicxx.render.current", { slot: (args && args.slot) ? String(args.slot) : undefined });
    },
    select: function (id, args) {
      var req = (args && typeof args === "object") ? args : {};
      req.id = String(id);
      return musicxx.call("musicxx.render.select", req);
    }
  };

  // 封面数据 (只作为颜色/数据来源, 不参与画面绘制; 宿主不推任何直链)
  musicxx.media = {
    // 封面颜色分析结果 + 宿主内置背景实际使用的 4 色
    palette: function (args) {
      var req = (args && typeof args === "object") ? args : {};
      return musicxx.call("musicxx.media.palette", req);
    },
    // 写封面取色 (与内置分析是同一批字段): {main, light, lightMuted, dark, darkMuted}
    setPalette: function (args) {
      var req = (args && typeof args === "object") ? args : {};
      return musicxx.call("musicxx.media.palette.set", req);
    },
    // 封面字节 (jpeg/png/rgba, base64 放在 data 字段; size 16..512);
    // args.bind 非空时同时把这次解码出来的封面绑成纹理 (渲染项 image 引用那个 key)
    cover: function (args) {
      var req = (args && typeof args === "object") ? args : {};
      return musicxx.call("musicxx.media.cover", req);
    },
    // 把插件自己的图片绑成纹理: {key, data(base64), format: png|jpeg|rgba, width?, height?}
    bindImage: function (args) {
      var req = (args && typeof args === "object") ? args : {};
      return musicxx.call("musicxx.media.bindImage", req);
    },
    // 释放之前绑定的图片纹理: {key}
    unbindImage: function (args) {
      var req = (args && typeof args === "object") ? args : {};
      return musicxx.call("musicxx.media.unbindImage", req);
    },
    // 当前音频频谱 (内置『音乐动效』提取的数据):
    //   args.bandCount 1..64 且能整除 256 (缺省 16)
    //   args.unit      normalized(缺省) | db | raw
    //   args.includeBins 是否附带当前帧的 256 个频点
    // 没有数据不是错误: 结果里的 status / available / loading / reason 说明原因
    spectrum: function (args) {
      var req = (args && typeof args === "object") ? args : {};
      return musicxx.call("musicxx.media.spectrum", req);
    }
  };

  musicxx.stats = {
    getSelf: function () {
      try { return JSON.parse(ext.stats() || "{}"); } catch (e) { return {}; }
    },
    reportMemory: function (bytes) {
      return musicxx.call("musicxx.stats.reportMemory", { bytes: Number(bytes) || 0 });
    },
    reportMetric: function (name, value) {
      return musicxx.call("musicxx.stats.reportMetric", { name: String(name), value: value });
    }
  };

  musicxx.events = {
    publish: function (topic, data) {
      assertName(topic, "events.publish: 主题");
      var err = ext.publish(topic, safeJson(data === undefined ? {} : data));
      if (err) { throw new Error(err); }
      return undefined;
    },
    subscribe: function (topic, fn) {
      assertName(topic, "events.subscribe: 主题");
      if (typeof fn !== "function") { throw new Error("events.subscribe: 缺少处理函数"); }
      var isNew = !subTable.has(topic);
      subTable.set(topic, fn);
      if (isNew) { ext.subscribeHost(topic); }
      return undefined;
    },
    unsubscribe: function (topic) {
      var key = String(topic);
      subTable.delete(key);
      return undefined;
    }
  };
  ext.onEvent = function (topic, json) {
    // 变量通道的内部回调: 变更通知与写结果 (与用户订阅同一主题时两边都会收到)
    if (topic === "musicxx.var.changed") { handleVarChanged(json); }
    if (topic === "musicxx.var.writeResult") { handleVarWriteResult(json); }
    // handler 属主: 宿主转过来的读写请求
    if (topic === "musicxx.var.read") { handleVarRead(json); }
    if (topic === "musicxx.var.write") { handleVarWrite(json); }
    var fn = subTable.get(topic);
    if (!fn) { return; }
    var payload = {};
    if (json) { try { payload = JSON.parse(json); } catch (e) { payload = {}; } }
    try {
      fn(payload, topic);
    } catch (e) {
      logError("事件处理异常 (" + topic + "): " + (e && e.message ? e.message : String(e)));
    }
  };

  musicxx.capability = {
    register: function (name, fn) {
      assertName(name, "capability.register: 名称");
      if (typeof fn !== "function") { throw new Error("capability.register: 缺少处理函数"); }
      capTable.set(name, fn);
      return undefined;
    },
    call: function (pluginId, name, args, timeoutMs) {
      assertName(pluginId, "capability.call: 插件 id");
      assertName(name, "capability.call: 能力名");
      // 超时是调用方自己的选择: 不传 = 3 秒; 传 0 = 一直等
      var budget = Number.isFinite(timeoutMs) ? Math.trunc(timeoutMs) : 3000;
      if (budget < 0) { budget = 0; }
      var text = ext.capabilityCall(String(pluginId), String(name),
                                    safeJson(args === undefined ? {} : args), budget);
      var info = null;
      try { info = JSON.parse(text || "{}"); } catch (e) { info = null; }
      if (!info || typeof info.status !== "string") {
        return Promise.reject(new Error("跨插件调用失败: 宿主返回了非法结果"));
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
        // 定时器表用**字符串键**: 宿主回调 `onTimerFire` 传进来的 id 是字符串
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
    now: function () { return Date.now(); }
  };

  /* ---- 宿主回调入口 (native 在 JS 线程上调用) ---- */

  ext.collectRegistrations = function () {
    var hooks = [];
    hookTable.forEach(function (entry, id) {
      hooks.push({ hook: id, mode: entry.mode, priority: entry.priority, ownerTag: entry.ownerTag });
    });
    var capabilities = [];
    capTable.forEach(function (value, name) { capabilities.push(name); });
    var subscriptions = [];
    subTable.forEach(function (value, topic) { subscriptions.push(topic); });
    return JSON.stringify({ hooks: hooks, capabilities: capabilities, subscriptions: subscriptions });
  };

  /// 调用钩子处理器
  ///
  /// - `waitId` 只有裁决型钩子才有 (宿主一直等到结果); 观察型钩子为空字符串/undefined;
  /// - 处理器返回 Promise 时: 有 `waitId` 就登记等待, 结算后经 `ext.hookWaitResolve` 交回结果;
  ///   没有 `waitId` 就让 Promise 正常跑完, 结果丢弃 (观察型本来就不看返回值);
  /// - 宿主不设等待超时: Promise 结算多晚都会被接住。
  ext.invokeHook = function (hookId, inputJson, waitId) {
    var entry = hookTable.get(hookId);
    if (!entry) { return ""; }
    var ctx = {};
    if (inputJson) { try { ctx = JSON.parse(inputJson); } catch (e) { ctx = {}; } }
    var result;
    try { result = entry.fn(ctx); }
    catch (e) {
      logError("钩子 " + hookId + " 异常: " + (e && e.message ? e.message : String(e)));
      return "";
    }
    if (result === null || result === undefined) { return ""; }
    if (typeof result === "object" && typeof result.then === "function") {
      var key = (waitId === undefined || waitId === null || waitId === "") ? "" : String(waitId);
      if (!key) {
        Promise.resolve(result).then(function () { return null; }, function (e) {
          logError("钩子 " + hookId + " 异步执行异常: " + (e && e.message ? e.message : String(e)));
        });
        return "";
      }
      pendingHookWaits.set(key, true);
      Promise.resolve(result).then(function (value) {
        if (!pendingHookWaits.has(key)) { return null; }
        pendingHookWaits.delete(key);
        ext.hookWaitResolve(key, (value === null || value === undefined) ? "" : safeJson(value));
        return null;
      }, function (e) {
        if (!pendingHookWaits.has(key)) { return null; }
        pendingHookWaits.delete(key);
        logError("钩子 " + hookId + " 异步裁决失败: " + (e && e.message ? e.message : String(e)));
        ext.hookWaitResolve(key, "");
        return null;
      });
      return kHookPendingMarker;
    }
    try { return JSON.stringify(result); } catch (e) { return ""; }
  };

  ext.reset = function () {
    hookTable.clear();
    subTable.clear();
    capTable.clear();
    uiTable.clear();
    timerTable.clear();
    pendingHookWaits.clear();
    pendingCapabilityWaits.clear();
    varsTable.clear();
    varBindTable.clear();
    varWatchSet.clear();
    varHandlers.clear();
    varTopicsReady.changed = false;
    varTopicsReady.result = false;
    varTopicsReady.owner = false;
    varGetWaiters.forEach(function (waiter) {
      try { waiter.reject(new Error("插件已停止")); } catch (e) { /* 忽略 */ }
    });
    varGetWaiters.clear();
    varSetWaiters.forEach(function (waiter) {
      try { waiter.reject(new Error("插件已停止")); } catch (e) { /* 忽略 */ }
    });
    varSetWaiters.clear();
    actionWaiters.forEach(function (waiter) {
      try { waiter.reject(new Error("插件已停止")); } catch (e) { /* 忽略 */ }
    });
    actionWaiters.clear();
    capabilityWaiters.forEach(function (waiter) {
      try { waiter.reject(new Error("插件已停止")); } catch (e) { /* 忽略 */ }
    });
    capabilityWaiters.clear();
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
  if (JS_IsObject(exception)) {
    JSValue message = JS_GetPropertyStr(ctx, exception, "message");
    const char *cstr = JS_ToCString(ctx, message);
    if (cstr) {
      text = cstr;
      JS_FreeCString(ctx, cstr);
    }
    JS_FreeValue(ctx, message);
  }
  if (text.empty()) {
    const char *cstr = JS_ToCString(ctx, exception);
    if (cstr) {
      text = cstr;
      JS_FreeCString(ctx, cstr);
    }
  }
  JS_FreeValue(ctx, exception);
  return text;
}

JSValue jsStateGet(JSContext *ctx, JSValueConst /*thisVal*/, int argc,
                   JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine || argc < 1) {
    return JS_NewString(ctx, "");
  }
  const char *key = JS_ToCString(ctx, argv[0]);
  std::string out;
  const bool ok = key && inst->engine->bridgeStateGet(key, out) == 0;
  if (key) {
    JS_FreeCString(ctx, key);
  }
  return JS_NewString(ctx, ok ? out.c_str() : "");
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

JSValue jsAction(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
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
      inst->engine->requestAction(inst->hostInst, name ? name : "",
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

JSValue jsActionCancel(JSContext *ctx, JSValueConst, int /*argc*/,
                       JSValueConst * /*argv*/) {
  auto *inst = contextInstance(ctx);
  if (inst && inst->engine) {
    /// v1: 取消该实例的全部未完成请求 (单条取消留给后续版本)
    inst->engine->cancelActionsOf(inst->name);
  }
  return JS_UNDEFINED;
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
  return JS_NewString(ctx,
                      rc == 0 ? "" : "事件发布被拒绝 (主题命名空间不允许)");
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

JSValue jsEngineVersion(JSContext *ctx, JSValueConst, int /*argc*/,
                        JSValueConst * /*argv*/) {
  return JS_NewString(ctx, "0.1.0");
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

/// 钩子操作 (register | unregister): 运行期注册/注销要落到宿主注册表
///
/// 顶层登记阶段 (liveRegistrations == false) 由 collectRegistrations 统一回放, 这里
/// 不做任何事; 运行期由 C++ 侧投递到宿主线程执行 (投递不等待)。
JSValue jsHookOp(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine || argc < 2) {
    return JS_UNDEFINED;
  }
  const char *op = JS_ToCString(ctx, argv[0]);
  const char *hookId = JS_ToCString(ctx, argv[1]);
  const char *mode = (argc >= 3) ? JS_ToCString(ctx, argv[2]) : nullptr;
  int32_t priority = 0;
  if (argc >= 4) {
    JS_ToInt32(ctx, &priority, argv[3]);
  }
  const char *ownerTag = (argc >= 5) ? JS_ToCString(ctx, argv[4]) : nullptr;
  inst->engine->bridgeHookOp(inst->name, op ? op : "", hookId ? hookId : "",
                             mode ? mode : "decision", priority,
                             ownerTag ? ownerTag : "");
  if (op) {
    JS_FreeCString(ctx, op);
  }
  if (hookId) {
    JS_FreeCString(ctx, hookId);
  }
  if (mode) {
    JS_FreeCString(ctx, mode);
  }
  if (ownerTag) {
    JS_FreeCString(ctx, ownerTag);
  }
  return JS_UNDEFINED;
}

/// 运行期动态订阅 (顶层声明由回放处理; 这里只处理运行期新增)
JSValue jsSubscribeHost(JSContext *ctx, JSValueConst, int argc,
                        JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine || argc < 1) {
    return JS_UNDEFINED;
  }
  const char *topic = JS_ToCString(ctx, argv[0]);
  inst->engine->bridgeSubscribeHost(inst->name, topic ? topic : "");
  if (topic) {
    JS_FreeCString(ctx, topic);
  }
  return JS_UNDEFINED;
}

/// 变量操作 (register | unregister | subscribe | unsubscribe | watch | unwatch)
JSValue jsVarsOp(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine || argc < 2) {
    return JS_UNDEFINED;
  }
  const char *op = JS_ToCString(ctx, argv[0]);
  const char *key = JS_ToCString(ctx, argv[1]);
  const char *json = (argc >= 3) ? JS_ToCString(ctx, argv[2]) : nullptr;
  inst->engine->bridgeVarsOp(inst->name, op ? op : "", key ? key : "",
                             json ? json : "{}");
  if (op) {
    JS_FreeCString(ctx, op);
  }
  if (key) {
    JS_FreeCString(ctx, key);
  }
  if (json) {
    JS_FreeCString(ctx, json);
  }
  return JS_UNDEFINED;
}

/// 异步读的发起 (返回控制对象 JSON; 结果经 ext.onVarGetDone 回到 JS 线程)
JSValue jsVarsGet(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine || argc < 1) {
    return JS_NewString(ctx, R"({"status":"error","error":"参数非法","code":-1})");
  }
  const char *key = JS_ToCString(ctx, argv[0]);
  const std::string text = key ? key : "";
  if (key) {
    JS_FreeCString(ctx, key);
  }
  Json out;
  if (text.empty()) {
    out["status"] = "error";
    out["error"] = "键不能为空";
    out["code"] = -1;
  } else {
    const int64_t id = inst->engine->beginVarGet(inst->name, text);
    if (id < 0) {
      out["status"] = "error";
      out["error"] = "宿主未启动或插件实例已停止";
      out["code"] = -2;
    } else {
      out["status"] = "pending";
      out["id"] = id;
    }
  }
  const std::string json = out.dump();
  return JS_NewStringLen(ctx, json.data(), json.size());
}

/// 写入的发起 (返回控制对象 JSON; 结果经 ext.onVarSetDone 回到 JS 线程)
JSValue jsVarsSet(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine || argc < 2) {
    return JS_NewString(ctx, R"({"status":"error","error":"参数非法","code":-1})");
  }
  const char *key = JS_ToCString(ctx, argv[0]);
  const char *value = JS_ToCString(ctx, argv[1]);
  const std::string keyText = key ? key : "";
  const std::string valueText = value ? value : "null";
  if (key) {
    JS_FreeCString(ctx, key);
  }
  if (value) {
    JS_FreeCString(ctx, value);
  }
  Json out;
  if (keyText.empty()) {
    out["status"] = "error";
    out["error"] = "键不能为空";
    out["code"] = -1;
  } else {
    const int64_t id = inst->engine->beginVarSet(inst->name, keyText, valueText);
    if (id < 0) {
      out["status"] = "error";
      out["error"] = "宿主未启动或插件实例已停止";
      out["code"] = -2;
    } else {
      out["status"] = "pending";
      out["id"] = id;
    }
  }
  const std::string json = out.dump();
  return JS_NewStringLen(ctx, json.data(), json.size());
}

/// handler 属主回答一次被转过来的请求 (投递不等待)
JSValue jsVarsRespond(JSContext *ctx, JSValueConst, int argc,
                      JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine || argc < 3) {
    return JS_UNDEFINED;
  }
  int64_t requestId = 0;
  JS_ToInt64(ctx, &requestId, argv[0]);
  const int32_t ok = JS_ToBool(ctx, argv[1]);
  const char *value = JS_ToCString(ctx, argv[2]);
  const char *error = (argc >= 4) ? JS_ToCString(ctx, argv[3]) : nullptr;
  inst->engine->bridgeVarsRespond(inst->name, requestId, 0 != ok,
                                  value ? value : "null", error ? error : "");
  if (value) {
    JS_FreeCString(ctx, value);
  }
  if (error) {
    JS_FreeCString(ctx, error);
  }
  return JS_UNDEFINED;
}

/// 同步读缓存 (op = peek | info | list)
///
/// 读的是宿主维护的**同步镜像** (互斥量保护), 因此不需要投递到宿主线程 ——
/// JS 线程绝不等宿主线程 (宿主线程可能正等着 JS 处理器)。
JSValue jsVarsRead(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine || argc < 2) {
    return JS_NewString(ctx, "");
  }
  const char *op = JS_ToCString(ctx, argv[0]);
  const char *key = JS_ToCString(ctx, argv[1]);
  const std::string opText = op ? op : "";
  const std::string keyText = key ? key : "";
  if (op) {
    JS_FreeCString(ctx, op);
  }
  if (key) {
    JS_FreeCString(ctx, key);
  }
  const std::string json = inst->engine->bridgeVarsRead(opText, keyText);
  return JS_NewStringLen(ctx, json.data(), json.size());
}

/// 裁决型钩子的 Promise 结算 (JS 线程 → 等待槽; 见
/// JsEngine::bridgeHookWaitResolve)
JSValue jsHookWaitResolve(JSContext *ctx, JSValueConst, int argc,
                          JSValueConst *argv) {
  auto *inst = contextInstance(ctx);
  if (!inst || !inst->engine || argc < 1) {
    return JS_UNDEFINED;
  }
  const char *waitId = JS_ToCString(ctx, argv[0]);
  const char *json = (argc >= 2) ? JS_ToCString(ctx, argv[1]) : nullptr;
  inst->engine->bridgeHookWaitResolve(waitId ? waitId : "", json ? json : "");
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

/// 装好 `__ext` 桥接对象 (返回值的所有权交给调用方 / JS_SetPropertyStr)
JSValue buildBridgeObject(JSContext *ctx) {
  struct Entry {
    const char *name;
    JSCFunction *fn;
    int argc;
  };
  const Entry entries[] = {
      {"stateGet", jsStateGet, 1},
      {"hostInfo", jsHostInfo, 0},
      {"log", jsLog, 2},
      {"configPath", jsConfigPath, 0},
      {"action", jsAction, 3},
      {"actionCancel", jsActionCancel, 0},
      {"publish", jsPublish, 2},
      {"subscribeHost", jsSubscribeHost, 1},
      {"varsOp", jsVarsOp, 3},
      {"varsGet", jsVarsGet, 1},
      {"varsSet", jsVarsSet, 2},
      {"varsRead", jsVarsRead, 2},
      {"varsRespond", jsVarsRespond, 4},
      {"hookOp", jsHookOp, 5},
      {"hookWaitResolve", jsHookWaitResolve, 2},
      {"capabilityWaitResolve", jsCapabilityWaitResolve, 2},
      {"timerSet", jsTimerSet, 2},
      {"timerClear", jsTimerClear, 1},
      {"uiOp", jsUiOp, 5},
      {"stats", jsStats, 0},
      {"capabilityCall", jsCapabilityCall, 4},
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
    mgr_->setInternalActionRelay(nullptr);
  }
}

std::shared_ptr<JsEngine> JsEngine::create(MusicxxHostManager *mgr) {
  auto engine = std::shared_ptr<JsEngine>(new JsEngine());
  engine->mgr_ = mgr;
  slotTable().engine.store(engine.get(), std::memory_order_release);
  installBuiltinProvider();
  if (mgr) {
    /// 动作请求接驳口: JS 侧已发出、还没收到回复的请求由引擎自己登记, Dart 的回复经它转交
    mgr->setInternalActionRelay(engine);
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
  XX_LOGI("[musicxx_ext] JS 插件槽位已登记: {} (脚本 {})", name,
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
      XX_LOGI("[musicxx_ext] JS 插件槽位已注销: {}", name);
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
    return MUSICXX_EXTERN_PLUGIN_OK;
  }
  if (!kJsCompiled) {
    err = "本次构建未包含 JS 运行时 (未编译 QuickJS 子模块)";
    return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  }
  return startThread(err);
}

int32_t JsEngine::startThread(std::string &err) {
  {
    std::lock_guard lock{mutex_};
    tasks_.clear();
    timers_.clear();
    pendingActions_.clear();
    hookWaits_.clear();
    instances_.clear();
    stopping_.store(false, std::memory_order_release);
  }
  running_.store(true, std::memory_order_release);
  try {
    worker_ = std::thread([this] { runLoop(); });
  } catch (const std::exception &e) {
    running_.store(false, std::memory_order_release);
    err = std::string{"JS 线程创建失败: "} + e.what();
    return MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL;
  }
  XX_LOGI("[musicxx_ext] JS 线程已启动 (全部 JS 插件实例共享这 1 条线程)");
  return MUSICXX_EXTERN_PLUGIN_OK;
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
    pendingActions_.clear();
    hookWaits_.clear();
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
  for (const auto &[id, pending] : pendingActions_) {
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
  std::vector<PendingAction> expired;
  {
    std::lock_guard lock{mutex_};
    const auto now = nowSteadyMs();
    for (auto it = pendingActions_.begin(); it != pendingActions_.end();) {
      if (it->second.deadlineMs <= now) {
        expired.push_back(it->second);
        it = pendingActions_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto &item : expired) {
    XX_LOGW("[musicxx_ext] JS 插件 `{}` 的动作请求超时: {} (id={})",
            item.pluginId, item.action, item.requestId);
    const auto inst = lookupInstance(item.instance);
    if (inst) {
      std::string ignored;
      callBridgeString(inst, "onActionDone",
                       {std::to_string(item.requestId), "1",
                        R"({"error":"action_timeout"})"},
                       ignored);
    }
    if (mgr_) {
      Json payload;
      payload["requestId"] = item.requestId;
      payload["reason"] = "timeout";
      mgr_->pushEvent("musicxx.action.cancel", item.pluginId, payload.dump());
    }
  }
}

/* ==================== 桥接 (线程安全) ==================== */

int32_t JsEngine::bridgeStateGet(const std::string &key,
                                 std::string &out) const {
  if (!mgr_) {
    return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  }
  /// 状态镜像由宿主的互斥保护, 任何线程可读 (只读快照)
  return mgr_->getState(key, out);
}

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
    return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  }
  return mgr_->publishPluginEventFrom(pluginId, topic, payload);
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

/* ---- 裁决型钩子的 Promise 等待槽 (异步裁决) ---- */

int64_t
JsEngine::beginHookWait(const std::string &instance,
                        const std::shared_ptr<WaitSlot<std::string>> &slot) {
  const int64_t waitId = nextHookWaitId_.fetch_add(1);
  std::lock_guard lock{mutex_};
  hookWaits_[waitId] = HookWait{instance, slot};
  return waitId;
}

void JsEngine::finishHookWait(int64_t waitId) {
  std::lock_guard lock{mutex_};
  hookWaits_.erase(waitId);
}

void JsEngine::clearHookWaitsOf(const std::string &instance) {
  std::lock_guard lock{mutex_};
  std::erase_if(hookWaits_, [&](const auto &item) {
    return item.second.instance == instance;
  });
}

int64_t JsEngine::beginCapabilityWait(
    const std::string &instance,
    const std::shared_ptr<WaitSlot<std::string>> &slot) {
  const int64_t waitId = nextCapabilityWaitId_.fetch_add(1);
  std::lock_guard lock{mutex_};
  capabilityWaits_[waitId] = HookWait{instance, slot};
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

void JsEngine::bridgeHookWaitResolve(const std::string &waitId,
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
    const auto it = hookWaits_.find(id);
    if (it == hookWaits_.end()) {
      return; ///< 不属于本引擎 / 已经完成
    }
    instance = it->second.instance;
    slot = it->second.slot;
    hookWaits_.erase(it);
  }
  if (!slot) {
    return;
  }
  if (auto inst = lookupInstance(instance)) {
    ++inst->asyncHookSettled;
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
    /// 脚本顶层登记阶段: 先记下来, 由 applyRegistrations 在宿主线程回放
    /// (与钩子/能力/订阅同一套做法, 避免"宿主线程等 JS、JS 等宿主线程")
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
    int32_t rc = MUSICXX_EXTERN_PLUGIN_ERR_STATE;
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
    if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
      Json payload;
      payload["id"] = pluginId;
      payload["phase"] = "ui";
      payload["op"] = op;
      payload["item"] = name;
      payload["code"] = rc;
      payload["message"] = "UI 项操作被拒绝 (检查类型/命名空间/data 结构)";
      mgr_->pushEvent("musicxx.plugin.error", pluginId, payload.dump());
      XX_LOGW("[musicxx_ext] JS 插件 `{}` 运行期 UI 操作 `{}` 失败 (code={})",
              pluginId, op, rc);
    }
  });
}

void JsEngine::bridgeSubscribeHost(const std::string &instanceName,
                                   const std::string &topic) {
  auto inst = lookupInstance(instanceName);
  if (!inst) {
    return;
  }
  if (!inst->liveRegistrations.load(std::memory_order_acquire)) {
    /// 顶层登记阶段的订阅由 applyRegistrations 统一建立
    return;
  }
  asio::post(mgr_->hostExecutor(), [this, instanceName, topic] {
    auto current = lookupInstance(instanceName);
    if (current) {
      ensureSubscriptionOnHost(current, topic);
    }
  });
}

void JsEngine::ensureSubscriptionOnHost(const std::shared_ptr<Instance> &inst,
                                        const std::string &topic) {
  if (!inst || topic.empty() || !inst->hostInst) {
    return;
  }
  for (const auto &existing : inst->subscriptions) {
    if (existing && existing->topic == topic) {
      return;
    }
  }
  const PluginxxHost *host = inst->hostInst->hostView();
  if (!host || !host->vtable || !host->vtable->query_interface) {
    return;
  }
  const PluginxxStringView iid{"pluginxx.events", 15};
  const auto *events = static_cast<const PluginxxEventsIface *>(
      host->vtable->query_interface(host, &iid));
  if (!events || !events->subscribe) {
    return;
  }
  auto holder = std::make_shared<SubscriptionHolder>();
  holder->engine = this;
  holder->instance = inst->name;
  holder->topic = topic;
  const PluginxxStringView topicView{holder->topic.data(),
                                     holder->topic.size()};
  auto *sub = events->subscribe(host, &topicView, &JsEngine::eventTrampoline,
                                holder.get());
  if (!sub) {
    XX_LOGW("[musicxx_ext] JS 插件 `{}` 运行期订阅 `{}` 被拒绝", inst->id,
            topic);
    return;
  }
  inst->subscriptions.push_back(holder);
  XX_LOGI("[musicxx_ext] JS 插件 `{}` 运行期订阅 `{}` 已生效", inst->id, topic);
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

std::vector<JsEngine::Instance::PendingVarOp>
JsEngine::takePendingVarOps(const std::shared_ptr<Instance> &inst) {
  std::vector<Instance::PendingVarOp> ops;
  if (!inst) {
    return ops;
  }
  ops.swap(inst->pendingVarOps);
  return ops;
}

/* ==================== 变量 (musicxx.vars) ==================== */

/// 在宿主线程上执行一次变量声明/订阅操作 (**必须已在宿主线程调用**)
int32_t JsEngine::applyVarOpOnHost(const std::shared_ptr<Instance> &inst,
                                   const std::string &op, const std::string &key,
                                   const std::string &json) {
  if (!inst || !inst->hostInst) {
    return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  }
  MusicxxHostInstance *host = inst->hostInst;
  if (op == "register") {
    bool parseOk = false;
    Json spec = parseJsonSafe(json, &parseOk);
    if (!parseOk || !spec.is_object()) {
      return MUSICXX_EXTERN_PLUGIN_ERR_JSON;
    }
    MusicxxPluginVarSpec native{};
    native.version = 1;
    native.struct_size = sizeof(MusicxxPluginVarSpec);
    native.key = PluginxxStringView{key.data(), key.size()};
    int32_t caps = 0;
    if (spec.contains("caps") && spec["caps"].is_array()) {
      for (const auto &item : spec["caps"]) {
        if (!item.is_string()) {
          continue;
        }
        const std::string name = item.get<std::string>();
        if (name == "get") {
          caps |= MUSICXX_PLUGIN_VAR_CAP_GET;
        } else if (name == "set") {
          caps |= MUSICXX_PLUGIN_VAR_CAP_SET;
        } else if (name == "notify") {
          caps |= MUSICXX_PLUGIN_VAR_CAP_NOTIFY;
        }
      }
    }
    native.caps = caps;
    const std::string mode = (spec.contains("mode") && spec["mode"].is_string())
                                 ? spec["mode"].get<std::string>()
                                 : std::string{"declared"};
    native.mode = (mode == "handler") ? MUSICXX_PLUGIN_VAR_MODE_HANDLER
                                      : MUSICXX_PLUGIN_VAR_MODE_DECLARED;
    const std::string write = (spec.contains("write") && spec["write"].is_string())
                                  ? spec["write"].get<std::string>()
                                  : std::string{"any"};
    native.write_scope = (write == "owner") ? MUSICXX_PLUGIN_VAR_WRITE_OWNER
                                            : MUSICXX_PLUGIN_VAR_WRITE_ANY;
    native.throttle_ms = (spec.contains("throttleMs") && spec["throttleMs"].is_number())
                             ? spec["throttleMs"].get<int32_t>()
                             : 0;
    native.notify_throttle_ms =
        (spec.contains("notifyThrottleMs") && spec["notifyThrottleMs"].is_number())
            ? spec["notifyThrottleMs"].get<int32_t>()
            : 0;
    native.refresh_after_ms =
        (spec.contains("refreshAfterMs") && spec["refreshAfterMs"].is_number())
            ? spec["refreshAfterMs"].get<int32_t>()
            : 0;
    const std::string type = (spec.contains("type") && spec["type"].is_string())
                                 ? spec["type"].get<std::string>()
                                 : std::string{};
    native.type = PluginxxStringView{type.data(), type.size()};
    std::string valueText;
    if (spec.contains("hasValue") && spec["hasValue"].is_boolean() &&
        spec["hasValue"].get<bool>() && spec.contains("value")) {
      valueText = spec["value"].dump();
    }
    native.value_json = PluginxxStringView{valueText.data(), valueText.size()};
    Json meta = Json::object();
    for (const char *field : {"title", "depict"}) {
      if (spec.contains(field) && spec[field].is_string() &&
          !spec[field].get<std::string>().empty()) {
        meta[field] = spec[field].get<std::string>();
      }
    }
    if (spec.contains("options") && spec["options"].is_array() &&
        !spec["options"].empty()) {
      meta["options"] = spec["options"];
    }
    const std::string metaText = meta.empty() ? std::string{} : meta.dump();
    native.meta_json = PluginxxStringView{metaText.data(), metaText.size()};
    const int32_t rc = mgr_->registerVar(host, native);
    return rc;
  }
  if (op == "unregister") {
    return mgr_->unregisterVar(host, key);
  }
  // subscribe / unsubscribe / watch / unwatch: 单键的数组形式
  Json keys = Json::array();
  keys.push_back(key);
  const std::string keysJson = keys.dump();
  int32_t count = 0;
  if (op == "subscribe") {
    return mgr_->subscribeVars(host, keysJson, count);
  }
  if (op == "unsubscribe") {
    return mgr_->unsubscribeVars(host, keysJson, count);
  }
  if (op == "watch") {
    return mgr_->watchVars(host, keysJson, true, count);
  }
  if (op == "unwatch") {
    return mgr_->watchVars(host, keysJson, false, count);
  }
  return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
}

void JsEngine::bridgeVarsOp(const std::string &instanceName,
                            const std::string &op, const std::string &key,
                            const std::string &json) {
  auto inst = lookupInstance(instanceName);
  if (!inst || !mgr_ || !mgr_->running()) {
    return;
  }
  if (!inst->liveRegistrations.load(std::memory_order_acquire)) {
    /// 脚本顶层登记阶段: 先记下来, 由 applyRegistrations 在宿主线程回放
    /// (与钩子/能力/UI 项同一套做法, 避免"宿主线程等 JS、JS 等宿主线程")
    Instance::PendingVarOp entry;
    entry.op = op;
    entry.key = key;
    entry.json = json;
    inst->pendingVarOps.push_back(std::move(entry));
    return;
  }
  const std::string pluginId = inst->id;
  auto self = shared_from_this();
  asio::post(mgr_->hostExecutor(),
             [self, pluginId, instanceName, op, key, json] {
               auto current = self->lookupInstance(instanceName);
               if (!current) {
                 return;
               }
               const int32_t rc =
                   self->applyVarOpOnHost(current, op, key, json);
               if (rc == MUSICXX_EXTERN_PLUGIN_OK) {
                 return;
               }
               Json payload;
               payload["id"] = pluginId;
               payload["phase"] = "vars";
               payload["op"] = op;
               payload["key"] = key;
               payload["code"] = rc;
               payload["message"] =
                   "变量操作被拒绝 (检查 key/caps/命名空间, 或宿主是否支持该模式)";
               self->mgr_->pushEvent("musicxx.plugin.error", pluginId,
                                     payload.dump());
               XX_LOGW(
                   "[musicxx_ext] JS 插件 `{}` 运行期变量操作 `{}` `{}` 失败 "
                   "(code={})",
                   pluginId, op, key, rc);
             });
}

std::string JsEngine::bridgeVarsRead(const std::string &op,
                                     const std::string &key) {
  MusicxxHostManager *mgr = mgr_;
  if (!mgr) {
    return {};
  }
  std::string out;
  int32_t rc = MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  if (op == "peek") {
    rc = mgr->peekVar(key, out);
  } else if (op == "info") {
    rc = mgr->varInfo(key, out);
  } else if (op == "list") {
    rc = mgr->listVars(key, out);
  } else {
    return {};
  }
  return rc == MUSICXX_EXTERN_PLUGIN_OK ? out : std::string{};
}

void JsEngine::bridgeVarsRespond(const std::string &instanceName,
                                 int64_t requestId, bool ok,
                                 const std::string &valueJson,
                                 const std::string &error) {
  MusicxxHostManager *mgr = mgr_;
  if (!mgr || !mgr->running()) {
    return;
  }
  auto self = shared_from_this();
  asio::post(mgr->hostExecutor(), [self, instanceName, requestId, ok,
                                   valueJson, error] {
    auto current = self->lookupInstance(instanceName);
    if (!current || !current->hostInst) {
      return;
    }
    std::string out;
    const int32_t rc = self->mgr_->respondVar(current->hostInst, requestId, ok,
                                              valueJson, error, out);
    if (rc == MUSICXX_EXTERN_PLUGIN_OK) {
      return;
    }
    /// 回答失败只记日志与事件 (脚本侧不等待结果): 常见原因是被转过来的请求
    /// 已经超时结算, 或者回答的不是自己的变量
    Json payload;
    payload["id"] = current->id;
    payload["phase"] = "vars";
    payload["op"] = "respond";
    payload["code"] = rc;
    payload["requestId"] = requestId;
    payload["message"] = "变量回答被拒绝 (请求已结算或不属于本插件)";
    self->mgr_->pushEvent("musicxx.plugin.error", current->id, payload.dump());
    XX_LOGW("[musicxx_ext] JS 插件 `{}` 回答变量请求 {} 失败 (code={})",
            current->id, requestId, rc);
  });
}

int64_t JsEngine::beginVarGet(const std::string &instanceName,
                              const std::string &key) {
  auto inst = lookupInstance(instanceName);
  if (!inst || !inst->hostInst || !mgr_ || !mgr_->running()) {
    return -1;
  }
  auto wait = std::make_shared<VarWait>();
  wait->engine = this;
  wait->instance = instanceName;
  wait->id = nextVarWaitId_.fetch_add(1);
  wait->kind = 1;
  {
    std::lock_guard<std::mutex> lock{varWaitMutex_};
    varWaits_[wait->id] = wait;
  }
  const int64_t waitId = wait->id;
  PluginxxOperatorNotify notify{};
  notify.done = &JsEngine::varWaitDoneTrampoline;
  notify.host_ud = wait.get();
  auto self = shared_from_this();
  asio::post(mgr_->hostExecutor(), [self, instanceName, key, notify, waitId] {
    auto current = self->lookupInstance(instanceName);
    int64_t requestId = 0;
    int32_t rc = MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    if (current && current->hostInst) {
      rc = self->mgr_->getVar(current->hostInst, key, &notify, &requestId);
    }
    if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
      /// 未受理: 属主不会被通知, done 也不会被调用 —— 这里自己回一条失败
      Json out;
      out["ok"] = false;
      out["error"] = varErrorText(rc);
      out["code"] = rc;
      self->postVarWaitResult(instanceName, waitId, "onVarGetDone", out.dump());
    }
  });
  return waitId;
}

int64_t JsEngine::beginVarSet(const std::string &instanceName,
                              const std::string &key,
                              const std::string &valueJson) {
  auto inst = lookupInstance(instanceName);
  if (!inst || !inst->hostInst || !mgr_ || !mgr_->running()) {
    return -1;
  }
  auto wait = std::make_shared<VarWait>();
  wait->engine = this;
  wait->instance = instanceName;
  wait->id = nextVarWaitId_.fetch_add(1);
  wait->kind = 2;
  {
    std::lock_guard<std::mutex> lock{varWaitMutex_};
    varWaits_[wait->id] = wait;
  }
  const int64_t waitId = wait->id;
  auto self = shared_from_this();
  asio::post(mgr_->hostExecutor(), [self, instanceName, key, valueJson, waitId] {
    auto current = self->lookupInstance(instanceName);
    std::string result;
    int32_t rc = MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    if (current && current->hostInst) {
      rc = self->mgr_->setVar(current->hostInst, key, valueJson, result);
    }
    Json out;
    if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
      out["ok"] = false;
      out["error"] = varErrorText(rc);
      out["code"] = rc;
    } else {
      /// 结果可能是"已结算"(accepted) 或"转给属主"(pending + requestId):
      /// 后者由预置层换到 `musicxx.var.writeResult` 的等待表里继续等
      out["ok"] = true;
      out["result"] = parseJsonSafe(result);
    }
    self->postVarWaitResult(instanceName, waitId, "onVarSetDone", out.dump());
  });
  return waitId;
}

void PLUGINXX_CALL
JsEngine::varWaitDoneTrampoline(void *ud, int32_t status,
                                const PluginxxStringView *payload) {
  auto *wait = static_cast<VarWait *>(ud);
  if (!wait || !wait->engine) {
    return;
  }
  /// 先把需要的字段拷出来: 结算之后登记就会被释放 (见 VarWait 的说明)
  JsEngine *engine = wait->engine;
  const int64_t id = wait->id;
  const int kind = wait->kind;
  const std::string instance = wait->instance;
  std::string text;
  if (payload && payload->data) {
    text.assign(payload->data, static_cast<size_t>(payload->size));
  }
  Json out;
  if (status == PLUGINXX_OPERATOR_OK) {
    out["ok"] = true;
    if (kind == 2) {
      out["result"] = parseJsonSafe(text);
    } else {
      out["value"] = parseJsonSafe(text);
    }
  } else {
    out["ok"] = false;
    out["error"] = text.empty() ? "variables_op_failed" : text;
  }
  engine->postVarWaitResult(instance, id,
                            kind == 2 ? "onVarSetDone" : "onVarGetDone",
                            out.dump());
}

void JsEngine::postVarWaitResult(const std::string &instanceName,
                                 int64_t waitId, const std::string &callback,
                                 const std::string &json) {
  auto self = shared_from_this();
  postTask([self, instanceName, waitId, callback, json] {
    auto inst = self->lookupInstance(instanceName);
    if (inst) {
      std::string ignored;
      self->callBridgeString(inst, callback.c_str(),
                             {std::to_string(waitId), json}, ignored);
    }
    self->finishVarWait(waitId);
  });
}

void JsEngine::finishVarWait(int64_t waitId) {
  std::lock_guard<std::mutex> lock{varWaitMutex_};
  varWaits_.erase(waitId);
}

void JsEngine::clearVarWaitsOf(const std::string &instanceName) {
  std::vector<std::shared_ptr<VarWait>> leftovers;
  {
    std::lock_guard<std::mutex> lock{varWaitMutex_};
    for (auto it = varWaits_.begin(); it != varWaits_.end();) {
      if (it->second && it->second->instance == instanceName) {
        leftovers.push_back(it->second);
        it = varWaits_.erase(it);
      } else {
        ++it;
      }
    }
    /// 不能直接释放: 属主的回答可能已经在路上, 回调仍会带着这个指针进来。
    /// 残余登记随引擎一起销毁 (正常路径上属主都已经结算完了)。
    for (auto &item : leftovers) {
      varWaitGraveyard_.push_back(std::move(item));
    }
  }
}

void JsEngine::bridgeHookOp(const std::string &instanceName,
                            const std::string &op, const std::string &hookId,
                            const std::string &mode, int32_t priority,
                            const std::string &ownerTag) {
  auto inst = lookupInstance(instanceName);
  if (!inst || !mgr_ || !mgr_->running()) {
    return;
  }
  if (!inst->liveRegistrations.load(std::memory_order_acquire)) {
    /// 脚本顶层登记阶段: 由 applyRegistrations 统一回放 (与 UI 项/订阅同一套做法,
    /// 避免"宿主线程等 JS、JS 等宿主线程"的互锁)
    return;
  }
  /// 运行期: 投递到宿主线程执行 (**不等待**): 宿主线程可能正等 JS 处理器,
  /// 在这里同步等待就会互锁。失败只记日志与事件, 不回传给脚本。
  auto self = shared_from_this();
  asio::post(mgr_->hostExecutor(),
             [self, instanceName, op, hookId, mode, priority, ownerTag] {
               auto current = self->lookupInstance(instanceName);
               if (current) {
                 self->applyHookOpOnHost(current, op, hookId, mode, priority,
                                         ownerTag);
               }
             });
}

void JsEngine::applyHookOpOnHost(const std::shared_ptr<Instance> &inst,
                                 const std::string &op,
                                 const std::string &hookId,
                                 const std::string &mode, int32_t priority,
                                 const std::string &ownerTag) {
  if (!inst || !inst->hostInst || !mgr_ || hookId.empty()) {
    return;
  }
  const bool unregister = op == "unregister";
  if (!unregister && op != "register") {
    return;
  }
  /// 同一个 (钩子, owner_tag) 只保留一份: 先摘掉宿主注册 (释放注册表对该
  /// `HookHandler` 的引用), 再释放 JS 侧持有的对象, 避免出现"注册表指向已释放对象"
  /// 的窗口; 覆盖式注册随后重新登记
  for (auto it = inst->hooks.begin(); it != inst->hooks.end();) {
    const auto &handler = *it;
    if (handler && handler->hookId == hookId && handler->ownerTag == ownerTag) {
      mgr_->unregisterHook(inst->hostInst, handler->hookId, handler->ownerTag);
      it = inst->hooks.erase(it);
      continue;
    }
    ++it;
  }
  if (unregister) {
    return;
  }

  auto handler = std::make_shared<HookHandler>();
  handler->engine = this;
  handler->instance = inst->name;
  handler->pluginId = inst->id;
  handler->hookId = hookId;
  handler->ownerTag = ownerTag;
  handler->decision = mode != "observe";

  MusicxxPluginHookSpec spec{};
  spec.version = 1;
  spec.struct_size = sizeof(MusicxxPluginHookSpec);
  spec.hook_id =
      PluginxxStringView{handler->hookId.data(), handler->hookId.size()};
  spec.owner_tag = PluginxxStringView{handler->ownerTag.data(),
                                      handler->ownerTag.size()};
  spec.mode = handler->decision ? MUSICXX_PLUGIN_HOOK_MODE_DECISION
                                : MUSICXX_PLUGIN_HOOK_MODE_OBSERVE;
  spec.priority = priority;
  spec.user_data = handler.get();
  if (handler->decision) {
    spec.hook_sync = &JsEngine::hookSync;
  } else {
    spec.hook_start = &JsEngine::hookStart;
  }
  const int32_t rc = mgr_->registerHook(inst->hostInst, spec);
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    Json payload;
    payload["id"] = inst->id;
    payload["phase"] = "hook";
    payload["hook"] = hookId;
    payload["code"] = "hook_register_failed";
    payload["message"] = "运行期钩子注册被拒绝 (未知钩子或命名空间非法)";
    mgr_->pushEvent("musicxx.plugin.error", inst->id, payload.dump());
    XX_LOGW("[musicxx_ext] JS 插件 `{}` 运行期注册钩子 `{}` 被拒绝 (code={})",
            inst->id, hookId, rc);
    return;
  }
  inst->hooks.push_back(handler);
  XX_LOGI("[musicxx_ext] JS 插件 `{}` 运行期注册钩子 `{}` 已生效 (mode={}, "
          "priority={})",
          inst->id, hookId, mode, priority);
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
    for (const auto &[id, item] : pendingActions_) {
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
  item["hooks"] = static_cast<int32_t>(inst->hooks.size());
  item["capabilities"] = static_cast<int32_t>(inst->capabilities.size());
  item["subscriptions"] = static_cast<int32_t>(inst->subscriptions.size());
  item["timers"] = timers;
  item["pendingActions"] = pendingOps;
  item["jsRuns"] = inst->jsRuns.load();
  item["errors"] = inst->errors.load(std::memory_order_relaxed);
  item["jsHeapBytes"] = inst->jsHeapBytes.load(std::memory_order_relaxed);
  /// 异步裁决统计 (裁决处理器返回 Promise 的情况)
  item["asyncHookSettled"] =
      inst->asyncHookSettled.load(std::memory_order_relaxed);
  return item.dump();
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
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  /// JS 目标: 同一 JS 线程上直接调用 (脚本↔脚本零跨线程跳跃)
  auto target = lookupInstance("js:" + targetId);
  if (target) {
    if (!target->scriptLoaded) {
      err = "目标 JS 插件未启用: " + targetId;
      return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
    std::string shortName = name;
    const std::string prefix = "plugin." + targetId + ".";
    if (shortName.rfind("plugin.", 0) == 0) {
      if (shortName.rfind(prefix, 0) != 0) {
        err = "能力命名空间不属于目标插件";
        return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
      }
      shortName = shortName.substr(prefix.size());
    }
    std::string text;
    if (!callBridgeString(target, "invokeCapability", {shortName, argsJson},
                          text)) {
      err = "目标插件未声明该能力 (需要在 start 事务里注册): " + shortName;
      return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
    }
    bool ok = false;
    Json data = parseJsonSafe(text, &ok);
    const bool fine = ok && data.is_object() && data.contains("ok") &&
                      data["ok"].is_boolean() && data["ok"].get<bool>();
    if (!fine) {
      err = (ok && data.contains("error") && data["error"].is_string())
                ? data["error"].get<std::string>()
                : std::string{"capability_call_failed"};
      return MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL;
    }
    outJson =
        data.contains("result") ? data["result"].dump() : std::string{"null"};
    (void)callerInstance;
    return MUSICXX_EXTERN_PLUGIN_OK;
  }
  /// 原生/内置目标: 投递到宿主线程执行, **JS 侧不等待** (宿主线程可能正等 JS
  /// 处理器, 若在这里同步等待就会互锁); 结果经 `ext.onCapabilityResult` 回到 JS
  /// 线程。
  if (!mgr_) {
    err = "宿主不可用";
    return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  }
  const int64_t pendingId = nextCapabilityCallId_.fetch_add(1);
  if (outPendingId) {
    *outPendingId = pendingId;
  }
  auto self = shared_from_this();
  auto finishCall = [self, pendingId, callerInstance](int32_t rc,
                                                      const std::string &text) {
    Json payload;
    payload["ok"] = (rc == MUSICXX_EXTERN_PLUGIN_OK);
    if (rc == MUSICXX_EXTERN_PLUGIN_OK) {
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
  /// 本引擎只在本宿主内使用, 实例必然是 [MusicxxHostInstance]
  auto *hostInst = static_cast<MusicxxHostInstance *>(base.get());
  if (hostInst->name.rfind("js:", 0) != 0) {
    err = "实例名不是 JS 合成实例: " + hostInst->name;
    return nullptr;
  }
  const std::string pluginId = MusicxxHostManager::pluginIdOf(hostInst->name);
  const std::vector<std::string> scripts = slotScripts(hostInst->name);
  if (scripts.empty()) {
    err = "JS 插件槽位不存在 (未登记或已注销): " + hostInst->name;
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
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string info;
  if (mgr_) {
    mgr_->hostInfoJson(info);
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
    return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  }
  applyRegistrations(inst);
  inst->scriptLoaded = true;
  return MUSICXX_EXTERN_PLUGIN_OK;
}

void JsEngine::stopInstance(const std::shared_ptr<Instance> &inst) {
  if (!inst) {
    return;
  }
  detachRegistrations(inst);
  clearHookWaitsOf(inst->name);
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
  // kit 文件）先跑完，插件脚本才跑。任一脚本报错都算装载失败，错误里带上脚本名。
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
    /// 栈深度上限 (宿主自我保护, 避免脚本深递归打爆线程栈;
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

  // ---- 钩子 ----
  if (data.contains("hooks") && data["hooks"].is_array()) {
    for (const auto &item : data["hooks"]) {
      if (!item.is_object() || !item.contains("hook") ||
          !item["hook"].is_string()) {
        continue;
      }
      auto handler = std::make_shared<HookHandler>();
      handler->engine = this;
      handler->instance = inst->name;
      handler->pluginId = inst->id;
      handler->hookId = item["hook"].get<std::string>();
      handler->ownerTag =
          (item.contains("ownerTag") && item["ownerTag"].is_string())
              ? item["ownerTag"].get<std::string>()
              : std::string{};
      const std::string mode =
          (item.contains("mode") && item["mode"].is_string())
              ? item["mode"].get<std::string>()
              : std::string{"decision"};
      handler->decision = mode != "observe";

      MusicxxPluginHookSpec spec{};
      spec.version = 1;
      spec.struct_size = sizeof(MusicxxPluginHookSpec);
      spec.hook_id =
          PluginxxStringView{handler->hookId.data(), handler->hookId.size()};
      spec.owner_tag = PluginxxStringView{handler->ownerTag.data(),
                                          handler->ownerTag.size()};
      spec.mode = handler->decision ? MUSICXX_PLUGIN_HOOK_MODE_DECISION
                                    : MUSICXX_PLUGIN_HOOK_MODE_OBSERVE;
      spec.priority =
          (item.contains("priority") && item["priority"].is_number_integer())
              ? item["priority"].get<int>()
              : 0;
      spec.user_data = handler.get();
      if (handler->decision) {
        spec.hook_sync = &JsEngine::hookSync;
      } else {
        spec.hook_start = &JsEngine::hookStart;
      }
      const int32_t rc = mgr_->registerHook(inst->hostInst, spec);
      if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
        Json payload;
        payload["id"] = inst->id;
        payload["phase"] = "hook";
        payload["hook"] = handler->hookId;
        payload["code"] = "hook_register_failed";
        payload["message"] = "钩子注册被拒绝 (未知钩子或命名空间非法)";
        mgr_->pushEvent("musicxx.plugin.error", inst->id, payload.dump());
        XX_LOGW("[musicxx_ext] JS 插件 `{}` 注册钩子 `{}` 被拒绝 (code={})",
                inst->id, handler->hookId, rc);
        continue;
      }
      inst->hooks.push_back(handler);
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
      const int32_t rc = mgr_->registerCapabilityEx(
          inst->hostInst, handler->fullName, &JsEngine::capabilityStart,
          nullptr, handler.get());
      if (rc != 0) {
        XX_LOGW("[musicxx_ext] JS 插件 `{}` 注册能力 `{}` 失败 (code={})",
                inst->id, handler->fullName, rc);
        continue;
      }
      inst->capabilities.push_back(handler);
    }
  }

  // ---- 事件订阅 ----
  if (data.contains("subscriptions") && data["subscriptions"].is_array()) {
    const PluginxxHost *host = inst->hostInst->hostView();
    for (const auto &item : data["subscriptions"]) {
      if (!item.is_string() || !host) {
        continue;
      }
      auto holder = std::make_shared<SubscriptionHolder>();
      holder->engine = this;
      holder->instance = inst->name;
      holder->topic = item.get<std::string>();
      const PluginxxStringView iid{"pluginxx.events", 15};
      const auto *events = static_cast<const PluginxxEventsIface *>(
          (host->vtable && host->vtable->query_interface)
              ? host->vtable->query_interface(host, &iid)
              : nullptr);
      if (!events || !events->subscribe) {
        XX_LOGW("[musicxx_ext] 事件表不可用: JS 插件 `{}` 的订阅未生效",
                inst->id);
        continue;
      }
      const PluginxxStringView topicView{holder->topic.data(),
                                         holder->topic.size()};
      auto *sub = events->subscribe(host, &topicView,
                                    &JsEngine::eventTrampoline, holder.get());
      if (!sub) {
        XX_LOGW("[musicxx_ext] JS 插件 `{}` 订阅主题 `{}` 被拒绝", inst->id,
                holder->topic);
        continue;
      }
      inst->subscriptions.push_back(holder);
    }
  }

  // ---- UI 声明式扩展项 ----
  int32_t uiApplied = 0;
  for (const auto &op : takePendingUiOps(inst)) {
    int32_t rc = MUSICXX_EXTERN_PLUGIN_ERR_ARG;
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
    if (rc == MUSICXX_EXTERN_PLUGIN_OK) {
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
      XX_LOGW("[musicxx_ext] JS 插件 `{}` 注册 UI 项 `{}` 被拒绝 (code={})",
              inst->id, op.name, rc);
    }
  }

  // ---- 变量登记与订阅 ----
  int32_t varsApplied = 0;
  for (const auto &op : takePendingVarOps(inst)) {
    const int32_t rc = applyVarOpOnHost(inst, op.op, op.key, op.json);
    if (rc == MUSICXX_EXTERN_PLUGIN_OK) {
      ++varsApplied;
      continue;
    }
    Json payload;
    payload["id"] = inst->id;
    payload["phase"] = "vars";
    payload["op"] = op.op;
    payload["key"] = op.key;
    payload["code"] = rc;
    payload["message"] =
        "变量注册被拒绝 (检查 key/caps/命名空间, 或宿主是否支持该模式)";
    mgr_->pushEvent("musicxx.plugin.error", inst->id, payload.dump());
    XX_LOGW("[musicxx_ext] JS 插件 `{}` 注册变量 `{}` 被拒绝 (code={})",
            inst->id, op.key, rc);
  }

  inst->registrationsApplied = true;
  /// 运行期的 UI/订阅/变量操作改为"投递不等待" (顶层登记阶段已结束)
  inst->liveRegistrations.store(true, std::memory_order_release);
  XX_LOGI("[musicxx_ext] JS 插件 `{}` 注册回放完成 (钩子 {} / 能力 {} / 订阅 "
          "{} / UI {} / 变量 {})",
          inst->id, inst->hooks.size(), inst->capabilities.size(),
          inst->subscriptions.size(), uiApplied, varsApplied);
}

void JsEngine::detachRegistrations(const std::shared_ptr<Instance> &inst) {
  if (!inst || !inst->hostInst || !mgr_) {
    return;
  }
  /// 先关掉运行期注册: 摘除过程中脚本若再注册, 只会记在待回放表里
  /// (随实例销毁丢弃)
  inst->liveRegistrations.store(false, std::memory_order_release);
  inst->pendingUiOps.clear();
  inst->pendingVarOps.clear();
  /// 变量登记 (声明/订阅/保活) 与未结算的变量请求都随实例作废
  /// (变量本身由 `clearPluginRegistrations` → `detachVars` 摘除)
  clearVarWaitsOf(inst->name);
  for (const auto &handler : inst->hooks) {
    mgr_->unregisterHook(inst->hostInst, handler->hookId, handler->ownerTag);
  }
  inst->hooks.clear();
  for (const auto &handler : inst->capabilities) {
    mgr_->unregisterCapability(inst->hostInst, handler->fullName);
  }
  inst->capabilities.clear();
  /// 事件订阅由内核随实例 stop/卸载自动撤销 (订阅登记在实例上)
  inst->subscriptions.clear();
  inst->registrationsApplied = false;
}

/* ==================== 钩子处理器 ==================== */

int32_t PLUGINXX_CALL JsEngine::hookSync(void *userData,
                                         const PluginxxStringView * /*hookId*/,
                                         const PluginxxStringView *inputJson,
                                         PluginxxString *outJson,
                                         PluginxxString * /*errorOut*/
) {
  auto *handler = static_cast<HookHandler *>(userData);
  if (!handler || !handler->engine) {
    return -1;
  }
  JsEngine *engine = handler->engine;
  /// 处理器对象的生命周期由实例持有 (卸载/禁用会摘除注册):
  /// 等待期间它可能已经释放, 因此这里先把要用的字段复制出来, 不在等待之后再读
  /// handler。
  const std::string pluginId = handler->pluginId;
  const std::string hookId = handler->hookId;
  const std::string name = handler->instance;
  const std::string input =
      inputJson ? viewToStr(inputJson) : std::string{"{}"};
  auto slot = std::make_shared<WaitSlot<std::string>>();
  /// 等待槽 id: Promise 结算后由 JS 侧经 `ext.hookWaitResolve` 交回结果
  const int64_t waitId = engine->beginHookWait(name, slot);
  engine->postTask([engine, name, hookId, input, slot, waitId] {
    const auto inst = engine->lookupInstance(name);
    std::string result;
    if (!inst || !engine->callBridgeString(
                     inst, "invokeHook",
                     {hookId, input, std::to_string(waitId)}, result)) {
      engine->finishHookWait(waitId);
      slot->set(std::string{});
      return;
    }
    if (result == kHookPendingMarker) {
      /// 处理器返回 Promise: 不结算, 等 [bridgeHookWaitResolve] 把结果交回来
      return;
    }
    engine->finishHookWait(waitId);
    slot->set(std::move(result));
  });

  /// 不设等待上界: 处理器返回 Promise 时一直等到它结算
  /// (宿主不因为处理器慢而按"无裁决"继续)
  std::string result;
  slot->wait(result);
  if (result.empty() || result == "null") {
    return 0;
  }
  pluginxx::hostMemorySetString(outJson, result);
  return 0;
}

void *PLUGINXX_CALL JsEngine::hookStart(void *userData,
                                        const PluginxxStringView * /*hookId*/,
                                        const PluginxxStringView *inputJson,
                                        const PluginxxOperatorNotify *notify,
                                        PluginxxString * /*errorOut*/
) {
  auto *handler = static_cast<HookHandler *>(userData);
  if (!handler || !handler->engine) {
    return nullptr;
  }
  JsEngine *engine = handler->engine;
  const std::string input =
      inputJson ? viewToStr(inputJson) : std::string{"{}"};
  const std::string hookId = handler->hookId;
  const std::string name = handler->instance;
  /// 观察型: 投递到 JS 线程后立即返回, 不等待 (observe 语义)
  engine->postTask([engine, name, hookId, input] {
    const auto inst = engine->lookupInstance(name);
    if (!inst) {
      return;
    }
    std::string ignored;
    engine->callBridgeString(inst, "invokeHook", {hookId, input}, ignored);
  });
  /// 宿主派发观察型处理器时传 nullptr (结果不参与合并); 若将来传了通知器,
  /// 这里按"已同步完成且无载荷"终结它 (观察型处理器不产生裁决)
  if (notify && notify->done) {
    notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
  }
  return nullptr;
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
    /// 同步完成: 内核支持"插件同步 done" (完成通知仍由内核在 IO 线程发布)
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
  const std::string payload =
      eventJson ? viewToStr(eventJson) : std::string{"{}"};
  const std::string topic = holder->topic;
  const std::string name = holder->instance;
  /// 事件回调在宿主线程 (发布者线程) 执行: 只投递, 不等 JS
  holder->engine->postTask([holder, name, topic, payload] {
    JsEngine *engine = holder->engine;
    const auto inst = engine->lookupInstance(name);
    if (!inst) {
      return;
    }
    std::string ignored;
    engine->callBridgeString(inst, "onEvent", {topic, payload}, ignored);
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
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string err;
  auto inst = instanceFromHost(host, err);
  if (!inst) {
    XX_LOGE("[musicxx_ext] JS 插件实例创建失败: {}", err);
    return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  }
  *outCtx = inst.get();
  return MUSICXX_EXTERN_PLUGIN_OK;
}

void *PLUGINXX_CALL JsEngine::builtinStart(void *ctx,
                                           const PluginxxOperatorNotify *notify,
                                           PluginxxString *errorOut) {
  auto *inst = static_cast<Instance *>(ctx);
  int32_t rc = MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  std::string err{"JS 实例上下文无效"};
  if (inst && inst->engine) {
    rc = inst->engine->startInstance(inst->engine->lookupInstance(inst->name),
                                     err);
  }
  if (rc != 0 && errorOut) {
    pluginxx::hostMemorySetString(errorOut, err);
  }
  /// 内核约定: start 事务必须恰好调用一次完成通知 (返回 nullptr = 无异步 op)
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

/* ==================== 动作请求 (JS 侧) ==================== */

int32_t JsEngine::requestAction(MusicxxHostInstance *inst,
                                const std::string &action,
                                const std::string &argsJson, uint32_t timeoutMs,
                                int64_t *outRequestId) {
  if (!inst || action.empty() || !mgr_) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  const std::string pluginId = MusicxxHostManager::pluginIdOf(inst->name);
  const int64_t requestId = mgr_->allocateRequestId();
  int64_t effectiveTimeoutMs = 0;
  const int32_t rc = mgr_->pushActionRequestEvent(
      pluginId, requestId, action, argsJson, timeoutMs, &effectiveTimeoutMs);
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    return rc;
  }
  {
    std::lock_guard lock{mutex_};
    PendingAction pending;
    pending.requestId = requestId;
    pending.instance = inst->name;
    pending.pluginId = pluginId;
    pending.action = action;
    pending.deadlineMs = nowSteadyMs() + effectiveTimeoutMs;
    pendingActions_[requestId] = std::move(pending);
  }
  cv_.notify_all();
  inst->actionRequests.fetch_add(1);
  if (outRequestId) {
    *outRequestId = requestId;
  }
  return MUSICXX_EXTERN_PLUGIN_OK;
}

bool JsEngine::respondAction(int64_t requestId, int32_t status,
                             const std::string &resultJson) {
  PendingAction pending;
  {
    std::lock_guard lock{mutex_};
    auto it = pendingActions_.find(requestId);
    if (it == pendingActions_.end()) {
      return false;
    }
    pending = it->second;
    pendingActions_.erase(it);
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
    callBridgeString(inst, "onActionDone", {idText, statusText, text}, ignored);
  });
  return true;
}

void JsEngine::cancelActionsOf(const std::string &instanceName) {
  std::vector<PendingAction> cancelled;
  {
    std::lock_guard lock{mutex_};
    for (auto it = pendingActions_.begin(); it != pendingActions_.end();) {
      if (it->second.instance == instanceName) {
        cancelled.push_back(it->second);
        it = pendingActions_.erase(it);
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
          inst, "onActionDone",
          {std::to_string(item.requestId), "1", R"({"error":"cancelled"})"},
          ignored);
    }
  });
  if (mgr_) {
    for (const auto &item : cancelled) {
      Json payload;
      payload["requestId"] = item.requestId;
      payload["reason"] = "plugin_unloaded";
      mgr_->pushEvent("musicxx.action.cancel", item.pluginId, payload.dump());
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
      for (const auto &[id, pending] : pendingActions_) {
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
      item["hooks"] = static_cast<int32_t>(inst->hooks.size());
      item["capabilities"] = static_cast<int32_t>(inst->capabilities.size());
      item["subscriptions"] = static_cast<int32_t>(inst->subscriptions.size());
      item["timers"] = timerCount;
      item["pendingActions"] = pendingCount;
      item["jsRuns"] = inst->jsRuns.load();
      item["errors"] = inst->errors.load();
      item["jsHeapBytes"] = inst->jsHeapBytes.load(std::memory_order_relaxed);
      /// 异步裁决统计 (裁决处理器返回 Promise)
      item["asyncHookSettled"] =
          inst->asyncHookSettled.load(std::memory_order_relaxed);
      plugins.push_back(item);
    }
  }
  Json result;
  result["available"] = kJsCompiled;
  result["running"] = running_.load(std::memory_order_acquire);
  result["threads"] = 1;
  /// 共享 JS 线程的排队情况: 队列越深 / 等待越长,
  /// 说明某个脚本正长时间占住线程 (宿主不打断它, 只在管理页展示)
  {
    std::lock_guard lock{mutex_};
    result["queueDepth"] = static_cast<int64_t>(tasks_.size());
    result["timers"] = static_cast<int64_t>(timers_.size());
    result["pendingActions"] = static_cast<int64_t>(pendingActions_.size());
  }
  result["queueWaitLastMs"] = queueWaitLastMs_.load(std::memory_order_relaxed);
  result["queueWaitMaxMs"] = queueWaitMaxMs_.load(std::memory_order_relaxed);
  result["plugins"] = plugins;
  return result.dump();
}

} // namespace extern_plugin
} // namespace musicxx
