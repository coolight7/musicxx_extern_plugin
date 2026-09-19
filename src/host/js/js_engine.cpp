/// musicxx 外部插件原生宿主: JS 插件运行时实现 (QuickJS)
///
/// 见 js_engine.h 的设计说明。本文件包含四部分:
/// 1. 进程级内置槽位表 (`js:<pluginId>` 的合成内置插件描述);
/// 2. 共享 JS 线程与任务/定时器/动作超时循环;
/// 3. `musicxx` 全局对象的 JS 预置代码 (prelude) 与 C 桥接原语;
/// 4. 实例生命周期 (建 runtime → 执行脚本 → 回放注册 → 停止时摘除注册并释放 runtime)。
#include "js_engine.h"

#include "host_json.h"
#include "pluginxx/api/tables.h"
#include "pluginxx/host/abi_util.h"
#include "pluginxx/host/manifest.h"
#include "pluginxx/runtime/instance_base.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

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

/// 裁决型钩子处理器在宿主线程上的等待上界 (与钩子 meta 的 hardMs 默认值一致):
/// 超时按"无裁决"返回, 不打断脚本 (plan §4.10 的宿主等待预算)
constexpr uint32_t kHookSyncBudgetMs = 100;

/// 建 runtime / 执行脚本的等待预算
constexpr uint32_t kScriptStepBudgetMs = 10000;
/// 其它 JS 交互 (回取注册/释放上下文/能力调用) 的等待预算
constexpr uint32_t kInteractionBudgetMs = 3000;

/// 编译期是否包含 QuickJS
constexpr bool kJsCompiled = MUSICXX_EXTERN_PLUGIN_HAS_JS != 0;

int64_t nowSteadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}

std::string readTextFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

/// 跨边界字符串视图 → C++ 字符串
std::string viewToStr(const PluginxxStringView* view) {
    if (!view || !view->data || view->size == 0) {
        return {};
    }
    return std::string{view->data, static_cast<size_t>(view->size)};
}

/* ==================== 进程级内置槽位表 ==================== */

constexpr size_t kSlotCapacity = 64;

/// 一个 JS 插件的内置槽位 (名称/脚本路径/版本)
struct Slot {
    std::string name; ///< js:<pluginId>
    std::string scriptPath;
    std::string version;
    bool        used = false;
};

/// 槽位表: 内核要求内置插件描述数组在进程生命周期内地址稳定 (plan §4.4 的实现注记),
/// 因此这里用固定容量数组 + 宿主维护的内容 (而不是可增长的 vector)。
struct SlotTable {
    std::array<Slot, kSlotCapacity>                    slots;
    std::array<PluginxxBuiltinInfo, kSlotCapacity>     infos{};
    std::array<PluginxxBuiltinManifest, kSlotCapacity> manifests{};
    std::mutex                                         mutex;
    /// 当前 JS 引擎 (宿主为进程单例; 内置入口是静态函数, 只能经它找到引擎)
    std::atomic<JsEngine*> engine{nullptr};
};

SlotTable& slotTable() {
    static SlotTable table;
    return table;
}

std::string slotScriptPath(std::string_view name) {
    SlotTable&      table = slotTable();
    std::lock_guard lock{table.mutex};
    for (auto& slot : table.slots) {
        if (slot.used && slot.name == name) {
            return slot.scriptPath;
        }
    }
    return {};
}

std::string slotVersion(std::string_view name) {
    SlotTable&      table = slotTable();
    std::lock_guard lock{table.mutex};
    for (auto& slot : table.slots) {
        if (slot.used && slot.name == name) {
            return slot.version;
        }
    }
    return {};
}

/* ==================== JS 预置代码 (prelude) ==================== */

/// 注入到每个 JS 插件上下文里的 `musicxx` API (plan §7.2)
///
/// 设计: 原生只暴露少量"桥接原语" (`__ext.*`), 其余 API 面 (Promise 封装、便捷方法、
/// 工具函数) 都用 JS 实现 —— 减少 C 代码量, 也便于后续扩展 API 而不动宿主二进制。
constexpr const char* kPrelude = R"JS(
(function () {
  "use strict";
  var ext = globalThis.__ext;
  if (!ext) { throw new Error("musicxx 运行时桥接缺失"); }

  var hookTable = new Map();
  var subTable = new Map();
  var capTable = new Map();
  var actionWaiters = new Map();
  var timerTable = new Map();

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

  var musicxx = {};
  musicxx.version = ext.engineVersion();
  musicxx.pluginId = ext.pluginId();

  musicxx.hooks = {
    register: function (id, options, fn) {
      assertName(id, "hooks.register: 钩子 id");
      if (typeof options === "function") { fn = options; options = null; }
      options = options || {};
      if (typeof fn !== "function") { throw new Error("hooks.register: 缺少处理函数"); }
      hookTable.set(id, {
        mode: options.mode === "observe" ? "observe" : "decision",
        priority: Number.isFinite(options.priority) ? Math.trunc(options.priority) : 0,
        ownerTag: typeof options.ownerTag === "string" ? options.ownerTag : "",
        fn: fn
      });
      return musicxx;
    },
    unregister: function (id) { hookTable.delete(String(id)); return musicxx; },
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
      actionWaiters.set(id, { resolve: resolve, reject: reject });
    });
  };
  ext.onActionDone = function (id, status, json) {
    var waiter = actionWaiters.get(id);
    if (!waiter) { return; }
    actionWaiters.delete(id);
    var value = null;
    if (json) { try { value = JSON.parse(json); } catch (e) { value = json; } }
    if (status === 0) {
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
    list: function () { return musicxx.call("musicxx.storage.list"); }
  };

  musicxx.ui = {
    notify: function (args) {
      return musicxx.call("musicxx.ui.notify", typeof args === "string" ? { text: args } : args);
    },
    toast: function (args) {
      return musicxx.call("musicxx.ui.toast", typeof args === "string" ? { text: args } : args);
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
      subTable.set(topic, fn);
      return undefined;
    },
    unsubscribe: function (topic) { subTable.delete(String(topic)); }
  };
  ext.onEvent = function (topic, json) {
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
    call: function () {
      throw new Error("capability.call 暂未支持 (v1 只支持注册)");
    }
  };
  ext.invokeCapability = function (name, argsJson) {
    var fn = capTable.get(name);
    if (!fn) { return JSON.stringify({ ok: false, error: "plugin_capability_not_found: " + name }); }
    var args = {};
    if (argsJson) { try { args = JSON.parse(argsJson); } catch (e) { args = {}; } }
    var result;
    try { result = fn(args); }
    catch (e) { return JSON.stringify({ ok: false, error: (e && e.message ? e.message : String(e)) }); }
    if (result && typeof result.then === "function") {
      return JSON.stringify({ ok: false, error: "capability_async_not_supported" });
    }
    return JSON.stringify({ ok: true, result: result === undefined ? null : result });
  };

  musicxx.timer = {
    _add: function (fn, ms, repeat) {
      if (typeof fn !== "function") { throw new Error("timer: 缺少处理函数"); }
      var id = ext.timerSet(Number.isFinite(ms) ? Math.max(0, Math.trunc(ms)) : 0, repeat);
      if (typeof id === "number" && id >= 0) { timerTable.set(id, { fn: fn, repeat: repeat }); }
      return id;
    },
    setTimeout: function (fn, ms) { return musicxx.timer._add(fn, ms, false); },
    setInterval: function (fn, ms) { return musicxx.timer._add(fn, ms, true); },
    clear: function (id) {
      var key = Number(id);
      ext.timerClear(key);
      timerTable.delete(key);
    },
    clearTimeout: function (id) { musicxx.timer.clear(id); },
    clearInterval: function (id) { musicxx.timer.clear(id); }
  };
  ext.onTimerFire = function (id) {
    var entry = timerTable.get(id);
    if (!entry) { return; }
    if (!entry.repeat) { timerTable.delete(id); }
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

  ext.invokeHook = function (hookId, inputJson) {
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
      logError("钩子 " + hookId + " 返回 Promise (同步钩子不支持异步裁决, 已忽略)");
      return "";
    }
    try { return JSON.stringify(result); } catch (e) { return ""; }
  };

  ext.reset = function () {
    hookTable.clear();
    subTable.clear();
    capTable.clear();
    timerTable.clear();
    actionWaiters.forEach(function (waiter) {
      try { waiter.reject(new Error("插件已停止")); } catch (e) { /* 忽略 */ }
    });
    actionWaiters.clear();
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
JsEngine::Instance* contextInstance(JSContext* ctx) {
    return static_cast<JsEngine::Instance*>(JS_GetContextOpaque(ctx));
}

/// 取回脚本异常文本
std::string takeException(JSContext* ctx) {
    JSValue     exception = JS_GetException(ctx);
    std::string text;
    if (JS_IsObject(exception)) {
        JSValue     message = JS_GetPropertyStr(ctx, exception, "message");
        const char* cstr    = JS_ToCString(ctx, message);
        if (cstr) {
            text = cstr;
            JS_FreeCString(ctx, cstr);
        }
        JS_FreeValue(ctx, message);
    }
    if (text.empty()) {
        const char* cstr = JS_ToCString(ctx, exception);
        if (cstr) {
            text = cstr;
            JS_FreeCString(ctx, cstr);
        }
    }
    JS_FreeValue(ctx, exception);
    return text;
}

JSValue jsStateGet(JSContext* ctx, JSValueConst /*thisVal*/, int argc, JSValueConst* argv) {
    auto* inst = contextInstance(ctx);
    if (!inst || !inst->engine || argc < 1) {
        return JS_NewString(ctx, "");
    }
    const char* key = JS_ToCString(ctx, argv[0]);
    std::string out;
    const bool  ok = key && inst->engine->bridgeStateGet(key, out) == 0;
    if (key) {
        JS_FreeCString(ctx, key);
    }
    return JS_NewString(ctx, ok ? out.c_str() : "");
}

JSValue jsHostInfo(JSContext* ctx, JSValueConst, int /*argc*/, JSValueConst* /*argv*/) {
    auto* inst = contextInstance(ctx);
    if (!inst || !inst->engine) {
        return JS_NewString(ctx, "{}");
    }
    return JS_NewString(ctx, inst->engine->bridgeHostInfo().c_str());
}

JSValue jsLog(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* inst = contextInstance(ctx);
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
        const char* text = JS_ToCString(ctx, argv[1]);
        if (text) {
            message = text;
            JS_FreeCString(ctx, text);
        }
    }
    inst->engine->bridgeLog(inst->id, level, message);
    return JS_UNDEFINED;
}

JSValue jsConfigPath(JSContext* ctx, JSValueConst, int /*argc*/, JSValueConst* /*argv*/) {
    auto* inst = contextInstance(ctx);
    return JS_NewString(ctx, inst ? inst->configPath.c_str() : "");
}

JSValue jsAction(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* inst = contextInstance(ctx);
    if (!inst || !inst->engine || !inst->hostInst || argc < 1) {
        return JS_NewInt32(ctx, -1);
    }
    const char* name      = JS_ToCString(ctx, argv[0]);
    const char* args      = (argc >= 2) ? JS_ToCString(ctx, argv[1]) : nullptr;
    uint32_t    timeoutMs = 5000;
    if (argc >= 3) {
        int32_t parsed = 0;
        if (JS_ToInt32(ctx, &parsed, argv[2]) == 0 && parsed > 0) {
            timeoutMs = static_cast<uint32_t>(parsed);
        }
    }
    int64_t    requestId = -1;
    const auto rc        = inst->engine->requestAction(
        inst->hostInst,
        name ? name : "",
        args ? args : "{}",
        timeoutMs,
        &requestId
    );
    if (name) {
        JS_FreeCString(ctx, name);
    }
    if (args) {
        JS_FreeCString(ctx, args);
    }
    return JS_NewInt32(ctx, rc == 0 ? static_cast<int32_t>(requestId) : static_cast<int32_t>(rc));
}

JSValue jsActionCancel(JSContext* ctx, JSValueConst, int /*argc*/, JSValueConst* /*argv*/) {
    auto* inst = contextInstance(ctx);
    if (inst && inst->engine) {
        /// v1: 取消该实例的全部在途请求 (单条取消留给后续版本)
        inst->engine->cancelActionsOf(inst->name);
    }
    return JS_UNDEFINED;
}

JSValue jsPublish(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* inst = contextInstance(ctx);
    if (!inst || !inst->engine || argc < 1) {
        return JS_NewString(ctx, "参数非法");
    }
    const char* topic = JS_ToCString(ctx, argv[0]);
    const char* data  = (argc >= 2) ? JS_ToCString(ctx, argv[1]) : nullptr;
    const auto  rc    = inst->engine->bridgePublish(inst->id, topic ? topic : "", data ? data : "{}");
    if (topic) {
        JS_FreeCString(ctx, topic);
    }
    if (data) {
        JS_FreeCString(ctx, data);
    }
    return JS_NewString(ctx, rc == 0 ? "" : "事件发布被拒绝 (主题命名空间不允许)");
}

JSValue jsTimerSet(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* inst = contextInstance(ctx);
    if (!inst || !inst->engine) {
        return JS_NewInt32(ctx, -1);
    }
    int32_t delay = 0;
    if (argc >= 1) {
        JS_ToInt32(ctx, &delay, argv[0]);
    }
    const bool repeat = (argc >= 2) && JS_ToBool(ctx, argv[1]) > 0;
    const auto id     = inst->engine->bridgeTimerSet(inst->name, delay, repeat);
    return JS_NewInt32(ctx, static_cast<int32_t>(id));
}

JSValue jsTimerClear(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    auto* inst = contextInstance(ctx);
    if (!inst || !inst->engine || argc < 1) {
        return JS_UNDEFINED;
    }
    int32_t id = 0;
    if (JS_ToInt32(ctx, &id, argv[0]) == 0 && id > 0) {
        inst->engine->bridgeTimerClear(inst->name, id);
    }
    return JS_UNDEFINED;
}

JSValue jsEngineVersion(JSContext* ctx, JSValueConst, int /*argc*/, JSValueConst* /*argv*/) {
    return JS_NewString(ctx, "0.1.0");
}

JSValue jsPluginId(JSContext* ctx, JSValueConst, int /*argc*/, JSValueConst* /*argv*/) {
    auto* inst = contextInstance(ctx);
    return JS_NewString(ctx, inst ? inst->id.c_str() : "");
}

/// 装好 `__ext` 桥接对象 (返回值的所有权交给调用方 / JS_SetPropertyStr)
JSValue buildBridgeObject(JSContext* ctx) {
    struct Entry {
        const char*  name;
        JSCFunction* fn;
        int          argc;
    };
    const Entry entries[] = {
        {"stateGet", jsStateGet, 1},
        {"hostInfo", jsHostInfo, 0},
        {"log", jsLog, 2},
        {"configPath", jsConfigPath, 0},
        {"action", jsAction, 3},
        {"actionCancel", jsActionCancel, 0},
        {"publish", jsPublish, 2},
        {"timerSet", jsTimerSet, 2},
        {"timerClear", jsTimerClear, 1},
        {"engineVersion", jsEngineVersion, 0},
        {"pluginId", jsPluginId, 0},
    };
    JSValue ext = JS_NewObject(ctx);
    for (const auto& entry : entries) {
        JS_SetPropertyStr(ctx, ext, entry.name, JS_NewCFunction(ctx, entry.fn, entry.name, entry.argc));
    }
    return ext;
}

/// 在 JS 线程上释放实例的 runtime (必须在该线程调用)
void freeInstanceContextOnJsThread(JsEngine::Instance& inst, bool resetScriptState) {
    if (!inst.ctx || !inst.rt) {
        inst.ctx = nullptr;
        inst.rt  = nullptr;
        return;
    }
    auto* ctx = static_cast<JSContext*>(inst.ctx);
    auto* rt  = static_cast<JSRuntime*>(inst.rt);
    if (resetScriptState) {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue ext    = JS_GetPropertyStr(ctx, global, "__ext");
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
        JSContext* jobCtx = nullptr;
        while (JS_ExecutePendingJob(rt, &jobCtx) > 0) {
            // 清空挂起的 promise 任务
        }
    }
    JS_FreeContext(ctx);
    JS_FreeRuntime(rt);
    inst.ctx = nullptr;
    inst.rt  = nullptr;
}

#endif // MUSICXX_EXTERN_PLUGIN_HAS_JS

} // namespace

/* ==================== 创建 / 属性 ==================== */

JsEngine::JsEngine() = default;

JsEngine::~JsEngine() {
    stop();
    SlotTable& table    = slotTable();
    auto*      expected = this;
    table.engine.compare_exchange_strong(expected, nullptr);
    if (mgr_) {
        mgr_->setInternalActionRelay(nullptr);
    }
}

std::shared_ptr<JsEngine> JsEngine::create(MusicxxHostManager* mgr) {
    auto engine  = std::shared_ptr<JsEngine>(new JsEngine());
    engine->mgr_ = mgr;
    slotTable().engine.store(engine.get(), std::memory_order_release);
    installBuiltinProvider();
    if (mgr) {
        /// 动作请求接驳口: JS 侧的在途请求由引擎自己登记, Dart 的回复经它转交
        mgr->setInternalActionRelay(engine);
    }
    return engine;
}

bool JsEngine::available() {
    return kJsCompiled;
}

void JsEngine::installBuiltinProvider() {
    static std::once_flag once;
    std::call_once(once, [] {
        pluginxx::BuiltinPluginProvider provider;
        provider.plugins   = &JsEngine::builtinPlugins;
        provider.manifests = &JsEngine::builtinManifests;
        pluginxx::setBuiltinPluginProvider(provider);
    });
}

const PluginxxBuiltinInfo* JsEngine::builtinPlugins(uint64_t* count) {
    SlotTable&      table = slotTable();
    std::lock_guard lock{table.mutex};
    for (size_t i = 0; i < kSlotCapacity; ++i) {
        PluginxxBuiltinInfo& info = table.infos[i];
        if (table.slots[i].used) {
            info.name     = PluginxxStringView{table.slots[i].name.data(), table.slots[i].name.size()};
            info.get_info = &JsEngine::builtinGetInfo;
            info.create   = &JsEngine::builtinCreate;
            info.destroy  = &JsEngine::builtinDestroy;
            info.start    = &JsEngine::builtinStart;
            info.stop     = &JsEngine::builtinStop;
        } else {
            info.name     = PluginxxStringView{nullptr, 0};
            info.get_info = nullptr;
            info.create   = nullptr;
            info.destroy  = nullptr;
            info.start    = nullptr;
            info.stop     = nullptr;
        }
    }
    if (count) {
        *count = kSlotCapacity;
    }
    return table.infos.data();
}

const PluginxxBuiltinManifest* JsEngine::builtinManifests(uint64_t* count) {
    SlotTable&      table = slotTable();
    std::lock_guard lock{table.mutex};
    for (size_t i = 0; i < kSlotCapacity; ++i) {
        table.manifests[i].name = table.slots[i].used
                                      ? PluginxxStringView{
                                            table.slots[i].name.data(),
                                            table.slots[i].name.size()
                                        }
                                      : PluginxxStringView{nullptr, 0};
        table.manifests[i].yaml = PluginxxStringView{nullptr, 0};
    }
    if (count) {
        *count = kSlotCapacity;
    }
    return table.manifests.data();
}

bool JsEngine::registerBuiltin(
    const std::string& pluginId,
    const std::string& scriptPath,
    const std::string& version,
    std::string&       err
) {
    if (pluginId.empty() || scriptPath.empty()) {
        err = "registerBuiltin: 插件 id 与脚本路径都不能为空";
        return false;
    }
    SlotTable&       table = slotTable();
    std::lock_guard  lock{table.mutex};
    const std::string name  = "js:" + pluginId;
    size_t            index = kSlotCapacity;
    for (size_t i = 0; i < kSlotCapacity; ++i) {
        if (table.slots[i].used && table.slots[i].name == name) {
            index = i;
            break;
        }
    }
    if (index == kSlotCapacity) {
        for (size_t i = 0; i < kSlotCapacity; ++i) {
            if (!table.slots[i].used) {
                index = i;
                break;
            }
        }
    }
    if (index == kSlotCapacity) {
        err = "JS 插件槽位已满 (上限 " + std::to_string(kSlotCapacity) + " 个)";
        return false;
    }
    auto& slot      = table.slots[index];
    slot.name       = name;
    slot.scriptPath = scriptPath;
    slot.version    = version.empty() ? std::string{"1.0.0"} : version;
    slot.used       = true;
    XX_LOGI("[musicxx_ext] JS 插件槽位已登记: {} (脚本 {})", name, scriptPath);
    return true;
}

void JsEngine::unregisterBuiltin(const std::string& pluginId) {
    SlotTable&       table = slotTable();
    std::lock_guard  lock{table.mutex};
    const std::string name = "js:" + pluginId;
    for (auto& slot : table.slots) {
        if (slot.used && slot.name == name) {
            slot.used = false;
            slot.name.clear();
            slot.scriptPath.clear();
            slot.version.clear();
            XX_LOGI("[musicxx_ext] JS 插件槽位已注销: {}", name);
            return;
        }
    }
}

bool JsEngine::hasBuiltin(const std::string& pluginId) const {
    return !slotScriptPath("js:" + pluginId).empty();
}

/* ==================== 共享 JS 线程 ==================== */

int32_t JsEngine::start(std::string& err) {
    if (running_.load(std::memory_order_acquire)) {
        return MUSICXX_EXTERN_PLUGIN_OK;
    }
    if (!kJsCompiled) {
        err = "本次构建未包含 JS 运行时 (未编译 QuickJS 子模块)";
        return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
    return startThread(err);
}

int32_t JsEngine::startThread(std::string& err) {
    {
        std::lock_guard lock{mutex_};
        tasks_.clear();
        timers_.clear();
        pendingActions_.clear();
        instances_.clear();
        stopping_.store(false, std::memory_order_release);
    }
    running_.store(true, std::memory_order_release);
    try {
        worker_ = std::thread([this] { runLoop(); });
    } catch (const std::exception& e) {
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
        tasks_.push_back([this] {
#if defined(MUSICXX_EXTERN_PLUGIN_HAS_JS)
            std::lock_guard inner{mutex_};
            for (auto& [name, inst] : instances_) {
                (void)name;
                if (inst->ctx || inst->rt) {
                    freeInstanceContextOnJsThread(*inst, true);
                }
            }
#endif
        });
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
    }
    XX_LOGI("[musicxx_ext] JS 线程已停止");
}

void JsEngine::postTask(std::function<void()> fn) {
    if (!running_.load(std::memory_order_acquire)) {
        return;
    }
    {
        std::lock_guard lock{mutex_};
        tasks_.push_back(std::move(fn));
    }
    cv_.notify_all();
}

int64_t JsEngine::nextWaitMsLocked() const {
    int64_t    wait = 50;
    const auto now  = nowSteadyMs();
    for (const auto& timer : timers_) {
        wait = (std::min)(wait, (std::max)(static_cast<int64_t>(0), timer.dueMs - now));
    }
    for (const auto& [id, pending] : pendingActions_) {
        (void)id;
        wait = (std::min)(wait, (std::max)(static_cast<int64_t>(0), pending.deadlineMs - now));
    }
    return wait;
}

void JsEngine::runLoop() {
    for (;;) {
        std::function<void()> task;
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
        if (task) {
            try {
                task();
            } catch (const std::exception& e) {
                XX_LOGE("[musicxx_ext] JS 任务异常: {}", e.what());
            } catch (...) {
                XX_LOGE("[musicxx_ext] JS 任务未知异常");
            }
        }
        fireDueTimers();
        expireActions();
#if defined(MUSICXX_EXTERN_PLUGIN_HAS_JS)
        // 执行挂起的 promise 任务 (每个实例的 runtime 各自推进)
        std::vector<std::shared_ptr<Instance>> list;
        {
            std::lock_guard lock{mutex_};
            for (auto& [name, inst] : instances_) {
                (void)name;
                list.push_back(inst);
            }
        }
        for (const auto& inst : list) {
            if (!inst->rt) {
                continue;
            }
            auto* rt = static_cast<JSRuntime*>(inst->rt);
            // 注意: JS_ExecutePendingJob 要求非空上下文出参 (它对 pctx 无条件解引用)
            JSContext* jobCtx = nullptr;
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
        int64_t     id = 0;
    };
    std::vector<Fired> fired;
    {
        std::lock_guard lock{mutex_};
        const auto      now = nowSteadyMs();
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
    for (const auto& item : fired) {
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
        const auto      now = nowSteadyMs();
        for (auto it = pendingActions_.begin(); it != pendingActions_.end();) {
            if (it->second.deadlineMs <= now) {
                expired.push_back(it->second);
                it = pendingActions_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& item : expired) {
        XX_LOGW(
            "[musicxx_ext] JS 插件 `{}` 的动作请求超时: {} (id={})",
            item.pluginId,
            item.action,
            item.requestId
        );
        const auto inst = lookupInstance(item.instance);
        if (inst) {
            std::string ignored;
            callBridgeString(
                inst,
                "onActionDone",
                {std::to_string(item.requestId), "1", R"({"error":"action_timeout"})"},
                ignored
            );
        }
        if (mgr_) {
            Json payload;
            payload["requestId"] = item.requestId;
            payload["reason"]    = "timeout";
            mgr_->pushEvent("musicxx.action.cancel", item.pluginId, payload.dump());
        }
    }
}

/* ==================== 桥接 (线程安全) ==================== */

int32_t JsEngine::bridgeStateGet(const std::string& key, std::string& out) const {
    if (!mgr_) {
        return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
    /// 状态镜像由宿主的互斥保护, 任何线程可读 (plan §4.7 的只读快照)
    return mgr_->getState(key, out);
}

std::string JsEngine::bridgeHostInfo() const {
    std::lock_guard lock{hostInfoMutex_};
    return hostInfoJson_.empty() ? std::string{"{}"} : hostInfoJson_;
}

void JsEngine::setHostInfoCache(const std::string& json) {
    std::lock_guard lock{hostInfoMutex_};
    hostInfoJson_ = json;
}

void JsEngine::bridgeLog(const std::string& pluginId, int32_t level, const std::string& message) {
    logPlugin(pluginId, level, message);
}

int32_t JsEngine::bridgePublish(
    const std::string& pluginId,
    const std::string& topic,
    const std::string& payload
) {
    if (!mgr_) {
        return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
    return mgr_->publishPluginEventFrom(pluginId, topic, payload);
}

int64_t JsEngine::bridgeTimerSet(const std::string& instance, int64_t delayMs, bool repeat) {
    std::lock_guard lock{mutex_};
    Timer           timer;
    timer.id         = nextTimerId_++;
    timer.instance   = instance;
    timer.intervalMs = (std::max)(delayMs, static_cast<int64_t>(0));
    timer.dueMs      = nowSteadyMs() + timer.intervalMs;
    timer.repeat     = repeat;
    const int64_t id = timer.id;
    timers_.push_back(std::move(timer));
    cv_.notify_all();
    return id;
}

void JsEngine::bridgeTimerClear(const std::string& instance, int64_t timerId) {
    std::lock_guard lock{mutex_};
    std::erase_if(timers_, [&](const Timer& timer) {
        return timer.id == timerId && timer.instance == instance;
    });
}

void JsEngine::logPlugin(const std::string& pluginId, int32_t level, const std::string& message) {
    if (message.empty()) {
        return;
    }
    Json payload;
    payload["id"]      = pluginId;
    payload["level"]   = level;
    payload["message"] = message;
    if (mgr_) {
        mgr_->pushEvent("musicxx.plugin.log", pluginId, payload.dump());
    }
    XX_LOGW("[musicxx_ext][js:{}] {}", pluginId, message);
}

/* ==================== 实例生命周期 ==================== */

std::shared_ptr<JsEngine::Instance> JsEngine::instanceFromHost(
    const PluginxxHost* host,
    std::string&        err
) {
    SlotTable& table  = slotTable();
    auto       engine = table.engine.load(std::memory_order_acquire);
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
    auto* hostInst = static_cast<MusicxxHostInstance*>(base.get());
    if (hostInst->name.rfind("js:", 0) != 0) {
        err = "实例名不是 JS 合成实例: " + hostInst->name;
        return nullptr;
    }
    const std::string pluginId = MusicxxHostManager::pluginIdOf(hostInst->name);
    const std::string script   = slotScriptPath(hostInst->name);
    if (script.empty()) {
        err = "JS 插件槽位不存在 (未登记或已注销): " + hostInst->name;
        return nullptr;
    }

    auto inst        = std::make_shared<Instance>();
    inst->engine     = engine;
    inst->id         = pluginId;
    inst->name       = hostInst->name;
    inst->scriptPath = script;
    inst->configPath = hostInst->configPath;
    inst->version    = slotVersion(hostInst->name);
    inst->argsJson   = hostInst->args.is_object() ? hostInst->args.dump() : std::string{"{}"};
    inst->hostInst   = hostInst;

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

std::shared_ptr<JsEngine::Instance> JsEngine::lookupInstance(const std::string& name) const {
    std::lock_guard lock{mutex_};
    auto            it = instances_.find(name);
    return it == instances_.end() ? std::shared_ptr<Instance>{} : it->second;
}

int32_t JsEngine::startInstance(const std::shared_ptr<Instance>& inst, std::string& err) {
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

void JsEngine::stopInstance(const std::shared_ptr<Instance>& inst) {
    if (!inst) {
        return;
    }
    detachRegistrations(inst);
    if ((inst->ctx || inst->rt) && running_.load(std::memory_order_acquire)) {
        auto slot = std::make_shared<WaitSlot<bool>>();
        postTask([inst, slot] {
#if defined(MUSICXX_EXTERN_PLUGIN_HAS_JS)
            freeInstanceContextOnJsThread(*inst, true);
#endif
            slot->set(true);
        });
        bool ok = false;
        if (!slot->wait(kInteractionBudgetMs, ok)) {
            XX_LOGW("[musicxx_ext] JS 实例 `{}` 上下文释放在预算内未完成", inst->name);
        }
    }
    inst->scriptLoaded = false;
}

void JsEngine::destroyInstance(const std::shared_ptr<Instance>& inst) {
    if (!inst) {
        return;
    }
    stopInstance(inst);
    {
        std::lock_guard lock{mutex_};
        auto            it = instances_.find(inst->name);
        if (it != instances_.end() && it->second == inst) {
            instances_.erase(it);
        }
        std::erase_if(timers_, [&](const Timer& timer) { return timer.instance == inst->name; });
    }
}

bool JsEngine::runScriptOnJsThread(const std::shared_ptr<Instance>& inst, std::string& err) {
#if !defined(MUSICXX_EXTERN_PLUGIN_HAS_JS)
    err = "本次构建未包含 JS 运行时 (未编译 QuickJS 子模块)";
    return false;
#else
    const std::string code = readTextFile(fs::path{inst->scriptPath});
    if (code.empty()) {
        err = "脚本读取失败或为空: " + inst->scriptPath;
        return false;
    }
    auto              slot       = std::make_shared<WaitSlot<std::string>>(); ///< 空串 = 成功
    const std::string scriptPath = inst->scriptPath;
    const std::string prelude{kPrelude};
    postTask([inst, slot, code, scriptPath, prelude] {
        JSRuntime* rt = JS_NewRuntime();
        if (!rt) {
            slot->set("JSRuntime 创建失败");
            return;
        }
        /// 栈深度上限 (宿主自我保护, 避免脚本深递归打爆线程栈; 不是对插件的资源限制)
        JS_SetMaxStackSize(rt, 1024 * 1024);
        JSContext* ctx = JS_NewContext(rt);
        if (!ctx) {
            JS_FreeRuntime(rt);
            slot->set("JSContext 创建失败");
            return;
        }
        JS_SetContextOpaque(ctx, inst.get());
        inst->rt  = rt;
        inst->ctx = ctx;

        JSValue global = JS_GetGlobalObject(ctx);
        JS_SetPropertyStr(ctx, global, "__ext", buildBridgeObject(ctx));
        JS_FreeValue(ctx, global);

        auto eval = [&](const std::string& source, const char* file) -> std::string {
            JSValue value = JS_Eval(ctx, source.data(), source.size(), file, JS_EVAL_TYPE_GLOBAL);
            if (JS_IsException(value)) {
                std::string text = takeException(ctx);
                JS_FreeValue(ctx, value);
                return text.empty() ? std::string{"脚本执行失败"} : text;
            }
            JS_FreeValue(ctx, value);
            return {};
        };

        std::string error = eval(prelude, "<musicxx-prelude>");
        if (error.empty()) {
            error = eval(code, scriptPath.c_str());
        }
        if (!error.empty()) {
            slot->set("脚本错误: " + error);
            return;
        }
        // 清空顶层微任务 (顶层不允许 await, 一般没有)
        // 注意: JS_ExecutePendingJob 要求非空上下文出参 (它对 pctx 无条件解引用)
        JSContext* jobCtx = nullptr;
        while (JS_ExecutePendingJob(rt, &jobCtx) > 0) {
            // 清空顶层微任务
        }
        slot->set(std::string{});
    });

    std::string result;
    if (!slot->wait(kScriptStepBudgetMs, result)) {
        err = "脚本执行未在预算内完成 (可能是死循环)";
        ++inst->errors;
        return false;
    }
    if (!result.empty()) {
        err = result;
        ++inst->errors;
        return false;
    }
    ++inst->jsRuns;
    return true;
#endif
}

bool JsEngine::callBridgeString(
    const std::shared_ptr<Instance>& inst,
    const char*                      fnName,
    const std::vector<std::string>&  args,
    std::string&                     out
) {
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
    auto*   ctx    = static_cast<JSContext*>(inst->ctx);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ext    = JS_GetPropertyStr(ctx, global, "__ext");
    JSValue fn     = JS_IsObject(ext) ? JS_GetPropertyStr(ctx, ext, fnName) : JS_UNDEFINED;
    bool    ok     = false;
    if (JS_IsFunction(ctx, fn)) {
        std::vector<JSValue> argv;
        argv.reserve(args.size());
        for (const auto& arg : args) {
            argv.push_back(JS_NewStringLen(ctx, arg.data(), arg.size()));
        }
        JSValue value = JS_Call(
            ctx,
            fn,
            JS_UNDEFINED,
            static_cast<int>(argv.size()),
            argv.empty() ? nullptr : argv.data()
        );
        if (JS_IsException(value)) {
            const std::string text = takeException(ctx);
            XX_LOGW("[musicxx_ext] JS 桥接调用 `{}` 异常: {}", fnName, text);
            ++inst->errors;
        } else {
            if (JS_IsString(value)) {
                const char* text = JS_ToCString(ctx, value);
                if (text) {
                    out = text;
                    JS_FreeCString(ctx, text);
                }
            }
            ok = true;
        }
        JS_FreeValue(ctx, value);
        for (auto& item : argv) {
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

void JsEngine::applyRegistrations(const std::shared_ptr<Instance>& inst) {
    if (!inst || !inst->hostInst || !mgr_) {
        return;
    }
    auto collected = std::make_shared<std::string>();
    auto slot      = std::make_shared<WaitSlot<bool>>();
    postTask([this, inst, collected, slot] {
        std::string text;
        const bool  ok = callBridgeString(inst, "collectRegistrations", {}, text);
        if (ok) {
            *collected = std::move(text);
        }
        slot->set(ok);
    });
    bool ok = false;
    if (!slot->wait(kInteractionBudgetMs, ok) || !ok) {
        XX_LOGW("[musicxx_ext] JS 实例 `{}` 注册信息回取失败", inst->name);
        return;
    }

    bool parseOk = false;
    Json data    = parseJsonSafe(*collected, &parseOk);
    if (!parseOk || !data.is_object()) {
        XX_LOGW("[musicxx_ext] JS 实例 `{}` 注册信息不是合法 JSON", inst->name);
        return;
    }

    // ---- 钩子 ----
    if (data.contains("hooks") && data["hooks"].is_array()) {
        for (const auto& item : data["hooks"]) {
            if (!item.is_object() || !item.contains("hook") || !item["hook"].is_string()) {
                continue;
            }
            auto handler      = std::make_shared<HookHandler>();
            handler->engine   = this;
            handler->instance = inst->name;
            handler->pluginId = inst->id;
            handler->hookId   = item["hook"].get<std::string>();
            handler->ownerTag = (item.contains("ownerTag") && item["ownerTag"].is_string())
                                    ? item["ownerTag"].get<std::string>()
                                    : std::string{};
            const std::string mode = (item.contains("mode") && item["mode"].is_string())
                                         ? item["mode"].get<std::string>()
                                         : std::string{"decision"};
            handler->decision = mode != "observe";

            MusicxxPluginHookSpec spec{};
            spec.version     = 1;
            spec.struct_size = sizeof(MusicxxPluginHookSpec);
            spec.hook_id     = PluginxxStringView{handler->hookId.data(), handler->hookId.size()};
            spec.owner_tag   = PluginxxStringView{handler->ownerTag.data(), handler->ownerTag.size()};
            spec.mode        = handler->decision ? MUSICXX_PLUGIN_HOOK_MODE_DECISION
                                                 : MUSICXX_PLUGIN_HOOK_MODE_OBSERVE;
            spec.priority    = (item.contains("priority") && item["priority"].is_number_integer())
                                   ? item["priority"].get<int>()
                                   : 0;
            spec.user_data   = handler.get();
            if (handler->decision) {
                spec.hook_sync = &JsEngine::hookSync;
            } else {
                spec.hook_start = &JsEngine::hookStart;
            }
            const int32_t rc = mgr_->registerHook(inst->hostInst, spec);
            if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
                Json payload;
                payload["id"]      = inst->id;
                payload["phase"]   = "hook";
                payload["hook"]    = handler->hookId;
                payload["code"]    = "hook_register_failed";
                payload["message"] = "钩子注册被拒绝 (未知钩子或命名空间非法)";
                mgr_->pushEvent("musicxx.plugin.error", inst->id, payload.dump());
                XX_LOGW(
                    "[musicxx_ext] JS 插件 `{}` 注册钩子 `{}` 被拒绝 (code={})",
                    inst->id,
                    handler->hookId,
                    rc
                );
                continue;
            }
            inst->hooks.push_back(handler);
        }
    }

    // ---- 能力 ----
    if (data.contains("capabilities") && data["capabilities"].is_array()) {
        for (const auto& item : data["capabilities"]) {
            if (!item.is_string()) {
                continue;
            }
            auto handler       = std::make_shared<CapabilityHandler>();
            handler->engine    = this;
            handler->instance  = inst->name;
            handler->pluginId  = inst->id;
            handler->shortName = item.get<std::string>();
            handler->fullName  = "plugin." + inst->id + "." + handler->shortName;
            const int32_t rc   = mgr_->registerCapabilityEx(
                inst->hostInst,
                handler->fullName,
                &JsEngine::capabilityStart,
                nullptr,
                handler.get()
            );
            if (rc != 0) {
                XX_LOGW(
                    "[musicxx_ext] JS 插件 `{}` 注册能力 `{}` 失败 (code={})",
                    inst->id,
                    handler->fullName,
                    rc
                );
                continue;
            }
            inst->capabilities.push_back(handler);
        }
    }

    // ---- 事件订阅 ----
    if (data.contains("subscriptions") && data["subscriptions"].is_array()) {
        const PluginxxHost* host = inst->hostInst->hostView();
        for (const auto& item : data["subscriptions"]) {
            if (!item.is_string() || !host) {
                continue;
            }
            auto holder      = std::make_shared<SubscriptionHolder>();
            holder->engine   = this;
            holder->instance = inst->name;
            holder->topic    = item.get<std::string>();
            const PluginxxStringView iid{"pluginxx.events", 15};
            const auto*              events = static_cast<const PluginxxEventsIface*>(
                (host->vtable && host->vtable->query_interface)
                    ? host->vtable->query_interface(host, &iid)
                    : nullptr
            );
            if (!events || !events->subscribe) {
                XX_LOGW("[musicxx_ext] 事件表不可用: JS 插件 `{}` 的订阅未生效", inst->id);
                continue;
            }
            const PluginxxStringView topicView{holder->topic.data(), holder->topic.size()};
            auto* sub = events->subscribe(host, &topicView, &JsEngine::eventTrampoline, holder.get());
            if (!sub) {
                XX_LOGW("[musicxx_ext] JS 插件 `{}` 订阅主题 `{}` 被拒绝", inst->id, holder->topic);
                continue;
            }
            inst->subscriptions.push_back(holder);
        }
    }

    inst->registrationsApplied = true;
    XX_LOGI(
        "[musicxx_ext] JS 插件 `{}` 注册回放完成 (钩子 {} / 能力 {} / 订阅 {})",
        inst->id,
        inst->hooks.size(),
        inst->capabilities.size(),
        inst->subscriptions.size()
    );
}

void JsEngine::detachRegistrations(const std::shared_ptr<Instance>& inst) {
    if (!inst || !inst->hostInst || !mgr_) {
        return;
    }
    for (const auto& handler : inst->hooks) {
        mgr_->unregisterHook(inst->hostInst, handler->hookId, handler->ownerTag);
    }
    inst->hooks.clear();
    for (const auto& handler : inst->capabilities) {
        mgr_->unregisterCapability(inst->hostInst, handler->fullName);
    }
    inst->capabilities.clear();
    /// 事件订阅由内核随实例 stop/卸载自动撤销 (订阅登记在实例上)
    inst->subscriptions.clear();
    inst->registrationsApplied = false;
}

/* ==================== 钩子处理器 ==================== */

int32_t PLUGINXX_CALL JsEngine::hookSync(
    void*                     userData,
    const PluginxxStringView* /*hookId*/,
    const PluginxxStringView* inputJson,
    PluginxxString*           outJson,
    PluginxxString*           /*errorOut*/
) {
    auto* handler = static_cast<HookHandler*>(userData);
    if (!handler || !handler->engine) {
        return -1;
    }
    JsEngine*         engine = handler->engine;
    const std::string input  = inputJson ? viewToStr(inputJson) : std::string{"{}"};
    auto              slot   = std::make_shared<WaitSlot<std::string>>();
    const std::string hookId = handler->hookId;
    const std::string name   = handler->instance;
    engine->postTask([engine, name, hookId, input, slot] {
        const auto  inst = engine->lookupInstance(name);
        std::string result;
        if (!inst || !engine->callBridgeString(inst, "invokeHook", {hookId, input}, result)) {
            slot->set(std::string{});
            return;
        }
        slot->set(std::move(result));
    });

    std::string result;
    if (!slot->wait(kHookSyncBudgetMs, result)) {
        XX_LOGW(
            "[musicxx_ext] JS 插件 `{}` 的钩子 `{}` 未在 {} ms 内返回 (按无裁决继续)",
            handler->pluginId,
            handler->hookId,
            kHookSyncBudgetMs
        );
        return 0; ///< 超时按"无裁决" (不打断脚本, 也不计为处理器失败)
    }
    if (result.empty() || result == "null") {
        return 0;
    }
    pluginxx::hostMemorySetString(outJson, result);
    return 0;
}

void* PLUGINXX_CALL JsEngine::hookStart(
    void*                         userData,
    const PluginxxStringView*     /*hookId*/,
    const PluginxxStringView*     inputJson,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               /*errorOut*/
) {
    auto* handler = static_cast<HookHandler*>(userData);
    if (!handler || !handler->engine) {
        return nullptr;
    }
    JsEngine*         engine = handler->engine;
    const std::string input  = inputJson ? viewToStr(inputJson) : std::string{"{}"};
    const std::string hookId = handler->hookId;
    const std::string name   = handler->instance;
    /// 观察型: 投递到 JS 线程后立即返回, 不等待 (plan §2.3 的 observe 语义)
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

void* JsEngine::capabilityStart(
    void*                         ctx,
    const PluginxxHost*           /*callerHost*/,
    const PluginxxStringView*     /*method*/,
    const PluginxxStringView*     argsJson,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               errorOut
) {
    auto* handler = static_cast<CapabilityHandler*>(ctx);
    if (!handler || !handler->engine) {
        return nullptr;
    }
    JsEngine*         engine    = handler->engine;
    const std::string args      = argsJson ? viewToStr(argsJson) : std::string{"{}"};
    const std::string shortName = handler->shortName;
    auto              slot      = std::make_shared<WaitSlot<std::string>>();
    const std::string name      = handler->instance;
    engine->postTask([engine, name, shortName, args, slot] {
        const auto  inst = engine->lookupInstance(name);
        std::string text;
        if (!inst || !engine->callBridgeString(inst, "invokeCapability", {shortName, args}, text)) {
            slot->set(std::string{R"({"ok":false,"error":"capability_invoke_failed"})"});
            return;
        }
        slot->set(std::move(text));
    });

    std::string text;
    if (!slot->wait(kInteractionBudgetMs, text)) {
        if (errorOut) {
            pluginxx::hostMemorySetString(errorOut, "能力调用未在预算内完成");
        }
        XX_LOGW(
            "[musicxx_ext] JS 插件 `{}` 的能力 `{}` 未在预算内完成",
            handler->pluginId,
            handler->fullName
        );
        return nullptr; ///< 未受理 (error_out 已填), 内核据此终结本次调用
    }

    bool        parseOk = false;
    Json        result  = parseJsonSafe(text, &parseOk);
    const bool  ok      = parseOk && result.is_object() && result.contains("ok")
                     && result["ok"].is_boolean() && result["ok"].get<bool>();
    std::string payload;
    if (ok) {
        payload = result.contains("result") ? result["result"].dump() : std::string{"null"};
    } else {
        Json error;
        error["error"] = (parseOk && result.contains("error") && result["error"].is_string())
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
        notify->done(
            notify->host_ud,
            ok ? PLUGINXX_OPERATOR_OK : PLUGINXX_OPERATOR_FAILED,
            ok ? &view : nullptr
        );
        if (out.data) {
            pluginxx::hostMemoryFree(out.data);
        }
    }
    return nullptr;
}

/* ==================== 事件订阅回调 ==================== */

void PLUGINXX_CALL JsEngine::eventTrampoline(
    const PluginxxStringView* eventJson,
    void*                     userData
) {
    auto* holder = static_cast<SubscriptionHolder*>(userData);
    if (!holder || !holder->engine) {
        return;
    }
    const std::string payload = eventJson ? viewToStr(eventJson) : std::string{"{}"};
    const std::string topic   = holder->topic;
    const std::string name    = holder->instance;
    /// 事件回调在宿主线程 (发布者线程) 执行: 只投递, 不等 JS
    holder->engine->postTask([holder, name, topic, payload] {
        JsEngine*  engine = holder->engine;
        const auto inst   = engine->lookupInstance(name);
        if (!inst) {
            return;
        }
        std::string ignored;
        engine->callBridgeString(inst, "onEvent", {topic, payload}, ignored);
    });
}

/* ==================== 内置插件入口 ==================== */

const PluginxxInfo* PLUGINXX_CALL JsEngine::builtinGetInfo() {
    static const PluginxxInfo info = [] {
        PluginxxInfo value{};
        value.api_version = PLUGINXX_API_VERSION;
        value.name        = PluginxxStringView{"js", 2};
        value.version     = PluginxxStringView{"1.0.0", 5};
        value.description = PluginxxStringView{"musicxx JS plugin runtime instance", 35};
        return value;
    }();
    return &info;
}

int32_t PLUGINXX_CALL JsEngine::builtinCreate(const PluginxxHost* host, void** outCtx) {
    if (!outCtx) {
        return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
    }
    std::string err;
    auto        inst = instanceFromHost(host, err);
    if (!inst) {
        XX_LOGE("[musicxx_ext] JS 插件实例创建失败: {}", err);
        return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
    *outCtx = inst.get();
    return MUSICXX_EXTERN_PLUGIN_OK;
}

void* PLUGINXX_CALL JsEngine::builtinStart(
    void*                         ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               errorOut
) {
    auto*       inst = static_cast<Instance*>(ctx);
    int32_t     rc   = MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    std::string err{"JS 实例上下文无效"};
    if (inst && inst->engine) {
        rc = inst->engine->startInstance(inst->engine->lookupInstance(inst->name), err);
    }
    if (rc != 0 && errorOut) {
        pluginxx::hostMemorySetString(errorOut, err);
    }
    /// 内核契约: start 事务必须恰好调用一次完成通知 (返回 nullptr = 无异步 op)
    if (notify && notify->done) {
        notify->done(
            notify->host_ud,
            rc == 0 ? PLUGINXX_OPERATOR_OK : PLUGINXX_OPERATOR_FAILED,
            nullptr
        );
    }
    return nullptr;
}

void* PLUGINXX_CALL JsEngine::builtinStop(
    void*                         ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               /*errorOut*/
) {
    auto* inst = static_cast<Instance*>(ctx);
    if (inst && inst->engine) {
        inst->engine->stopInstance(inst->engine->lookupInstance(inst->name));
    }
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
    return nullptr;
}

void PLUGINXX_CALL JsEngine::builtinDestroy(void* ctx) {
    auto* inst = static_cast<Instance*>(ctx);
    if (inst && inst->engine) {
        inst->engine->destroyInstance(inst->engine->lookupInstance(inst->name));
    }
}

/* ==================== 动作请求 (JS 侧) ==================== */

int32_t JsEngine::requestAction(
    MusicxxHostInstance* inst,
    const std::string&   action,
    const std::string&   argsJson,
    uint32_t             timeoutMs,
    int64_t*             outRequestId
) {
    if (!inst || action.empty() || !mgr_) {
        return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
    }
    const std::string pluginId  = MusicxxHostManager::pluginIdOf(inst->name);
    const int64_t     requestId = mgr_->allocateRequestId();
    int64_t           effectiveTimeoutMs = 0;
    const int32_t     rc                = mgr_->pushActionRequestEvent(
        pluginId,
        requestId,
        action,
        argsJson,
        timeoutMs,
        &effectiveTimeoutMs
    );
    if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
        return rc;
    }
    {
        std::lock_guard lock{mutex_};
        PendingAction   pending;
        pending.requestId          = requestId;
        pending.instance           = inst->name;
        pending.pluginId           = pluginId;
        pending.action             = action;
        pending.deadlineMs         = nowSteadyMs() + effectiveTimeoutMs;
        pendingActions_[requestId] = std::move(pending);
    }
    cv_.notify_all();
    inst->actionRequests.fetch_add(1);
    if (outRequestId) {
        *outRequestId = requestId;
    }
    return MUSICXX_EXTERN_PLUGIN_OK;
}

bool JsEngine::respondAction(int64_t requestId, int32_t status, const std::string& resultJson) {
    PendingAction pending;
    {
        std::lock_guard lock{mutex_};
        auto            it = pendingActions_.find(requestId);
        if (it == pendingActions_.end()) {
            return false;
        }
        pending = it->second;
        pendingActions_.erase(it);
    }
    /// 回复来自 Dart 线程: 只投递到 JS 线程执行 (绝不在调用线程碰 JSContext)
    const std::string text       = resultJson.empty() ? std::string{"null"} : resultJson;
    const std::string idText     = std::to_string(requestId);
    const std::string statusText = std::to_string(status);
    const std::string name       = pending.instance;
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

void JsEngine::cancelActionsOf(const std::string& instanceName) {
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
        for (const auto& item : cancelled) {
            std::string ignored;
            callBridgeString(
                inst,
                "onActionDone",
                {std::to_string(item.requestId), "1", R"({"error":"cancelled"})"},
                ignored
            );
        }
    });
    if (mgr_) {
        for (const auto& item : cancelled) {
            Json payload;
            payload["requestId"] = item.requestId;
            payload["reason"]    = "plugin_unloaded";
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
        for (const auto& [name, inst] : instances_) {
            (void)name;
            int32_t timerCount = 0;
            for (const auto& timer : timers_) {
                if (timer.instance == inst->name) {
                    ++timerCount;
                }
            }
            int32_t pendingCount = 0;
            for (const auto& [id, pending] : pendingActions_) {
                (void)id;
                if (pending.instance == inst->name) {
                    ++pendingCount;
                }
            }
            Json item;
            item["id"]             = inst->id;
            item["instance"]       = inst->name;
            item["kind"]           = "js";
            item["version"]        = inst->version;
            item["script"]         = inst->scriptPath;
            item["scriptLoaded"]   = inst->scriptLoaded;
            item["hooks"]          = static_cast<int32_t>(inst->hooks.size());
            item["capabilities"]   = static_cast<int32_t>(inst->capabilities.size());
            item["subscriptions"]  = static_cast<int32_t>(inst->subscriptions.size());
            item["timers"]         = timerCount;
            item["pendingActions"] = pendingCount;
            item["jsRuns"]         = inst->jsRuns.load();
            item["errors"]         = inst->errors.load();
            plugins.push_back(item);
        }
    }
    Json result;
    result["available"] = kJsCompiled;
    result["running"]   = running_.load(std::memory_order_acquire);
    result["threads"]   = 1;
    result["plugins"]   = plugins;
    return result.dump();
}

} // namespace extern_plugin
} // namespace musicxx
