/// musicxx 外部插件宿主 C ABI 实现 (Dart ⇄ 原生, v1)
///
/// 约定 (见 src/include/musicxx_extern_plugin_api.h):
/// - 所有导出函数用 `pluginxx::guardCall` 兜住异常, 绝不把异常带过 C ABI;
/// - 出参字符串一律经宿主堆分配 (musicxx_extern_plugin_string_free 释放);
/// - 涉及宿主线程状态的操作投递到宿主线程执行 (ioCallSync);
///   装载/卸载是异步事务, 在调用线程做有界等待 (不占宿主线程)。

#include "musicxx_extern_plugin_api.h"
#include "host_json.h"
#include "musicxx_host.h"

#include "pluginxx/host/abi_util.h"
#include "pluginxx/kit/guard.h"
#include "utilxx_base/json.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <string>

using musicxx::extern_plugin::HostContext;
using musicxx::extern_plugin::MusicxxHostManager;

namespace {

constexpr const char *kHostVersion = "0.1.0";

/// 宿主句柄 (C ABI 层持有上下文与管理器; 由 host_create/host_destroy
/// 管理生命周期)
struct HostHandle {
  HostContext ctx;
  std::shared_ptr<MusicxxHostManager> manager;
};

HostHandle *asHandle(MusicxxExternPluginHost *h) {
  return reinterpret_cast<HostHandle *>(h);
}

std::string viewToStd(const MusicxxExternPluginStringView *v) {
  if (!v || !v->data) {
    return {};
  }
  return std::string{v->data, static_cast<size_t>(v->size)};
}

} // namespace

namespace {

/// 出参写入: 用宿主分配器 (malloc) 分配, 由调用方 string_free 释放
void setOut(MusicxxExternPluginString *out, std::string_view text) {
  if (!out) {
    return;
  }
  out->data = nullptr;
  out->size = 0;
  if (text.empty()) {
    return;
  }
  auto *p = static_cast<char *>(pluginxx::hostMemoryAlloc(text.size() + 1));
  if (!p) {
    return;
  }
  std::memcpy(p, text.data(), text.size());
  p[text.size()] = '\0';
  out->data = p;
  out->size = static_cast<uint64_t>(text.size());
}

void setErr(MusicxxExternPluginString *log, std::string_view text) {
  if (!log || text.empty()) {
    return;
  }
  setOut(log, text);
}

/// 在宿主线程执行并同步取回结果 (调用方非宿主线程时投递等待)
template <typename Fn>
auto onHostThread(MusicxxHostManager *mgr, Fn &&fn) -> decltype(fn()) {
  return pluginxx::ioCallSync<decltype(fn())>(
      mgr, std::function<decltype(fn())()>(std::forward<Fn>(fn)));
}

} // namespace

extern "C" {

/* ==================== 内存 / 版本 ==================== */

MUSICXX_EXTERN_PLUGIN_EXPORT void *MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_malloc(uint64_t size) {
  return pluginxx::hostMemoryAlloc(size);
}

MUSICXX_EXTERN_PLUGIN_EXPORT void MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_free(const void *ptr) {
  pluginxx::hostMemoryFree(const_cast<void *>(ptr));
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_strdup_n(const MusicxxExternPluginStringView *src,
                               MusicxxExternPluginString *out) {
  if (!src || !out) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  setOut(out, std::string_view{src->data ? src->data : "",
                               static_cast<size_t>(src->size)});
  return MUSICXX_EXTERN_PLUGIN_OK;
}

MUSICXX_EXTERN_PLUGIN_EXPORT void MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_string_free(MusicxxExternPluginString *s) {
  if (!s) {
    return;
  }
  if (s->data) {
    pluginxx::hostMemoryFree(s->data);
  }
  s->data = nullptr;
  s->size = 0;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_api_version(void) {
  return MUSICXX_EXTERN_PLUGIN_API_VERSION;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_version(MusicxxExternPluginString *out) {
  if (!out) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  setOut(out, kHostVersion);
  return MUSICXX_EXTERN_PLUGIN_OK;
}

/* ==================== 宿主生命周期 ==================== */

/// 进程级"宿主已存在"标记 (宿主是进程单例)
///
/// 已存在的宿主意味着**一条宿主线程 + 一套插件实例 + 一条事件队列**;
/// 若允许再次创建, 同一进程会出现两套插件加载 (重复生命周期,
/// 插件目录/配置文件并发写), 因此第二次创建 直接拒绝并给出可读原因 (Dart
/// 侧表现为 init 抛出带诊断的异常)。
///
/// 复位时机: 宿主被销毁 (`host_destroy`)
/// 或创建失败时。因此"先销毁再创建"是正常路径 (测试里的 init → dispose → init
/// 不受影响), 只有"同时存在两个宿主"才会被拒绝。
std::atomic<bool> &hostAliveFlag() {
  static std::atomic<bool> flag{false};
  return flag;
}

MUSICXX_EXTERN_PLUGIN_EXPORT MusicxxExternPluginHost *MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_host_create(const MusicxxExternPluginHostConfig *cfg,
                                  MusicxxExternPluginString *log) {
  if (!cfg) {
    setErr(log, "host_create: cfg is null");
    return nullptr;
  }
  return pluginxx::guardCall(
      [log](std::string_view msg) { setErr(log, msg); }, nullptr,
      [&]() -> MusicxxExternPluginHost * {
        bool expected = false;
        if (!hostAliveFlag().compare_exchange_strong(expected, true)) {
          setErr(log, "host_create: 本进程已有宿主实例 (宿主是进程单例) —— "
                      "其它 isolate / 线程请复用它的句柄");
          return nullptr;
        }
        auto handle = std::make_unique<HostHandle>();
        std::string err;
        handle->manager = MusicxxHostManager::create(handle->ctx, *cfg, err);
        if (!handle->manager) {
          hostAliveFlag().store(false, std::memory_order_release);
          setErr(log, err.empty() ? "host_create failed" : err);
          return nullptr;
        }
        return reinterpret_cast<MusicxxExternPluginHost *>(handle.release());
      });
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_host_start(MusicxxExternPluginHost *h,
                                 MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager) {
    setErr(log, "host_start: invalid handle");
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  return pluginxx::guardCall([log](std::string_view msg) { setErr(log, msg); },
                             MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL,
                             [&]() -> int32_t {
                               std::string err;
                               const auto rc = handle->manager->start(err);
                               if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
                                 setErr(log, err);
                               }
                               return rc;
                             });
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_host_stop(MusicxxExternPluginHost *h, uint32_t timeout_ms,
                                MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  return pluginxx::guardCall([log](std::string_view msg) { setErr(log, msg); },
                             MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL,
                             [&]() -> int32_t {
                               std::string err;
                               const auto rc =
                                   handle->manager->stop(timeout_ms, err);
                               if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
                                 setErr(log, err);
                               }
                               return rc;
                             });
}

MUSICXX_EXTERN_PLUGIN_EXPORT void MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_host_destroy(MusicxxExternPluginHost *h) {
  auto *handle = asHandle(h);
  if (!handle) {
    return;
  }
  pluginxx::guardCallVoid([](std::string_view) {},
                          [&] {
                            if (handle->manager) {
                              std::string err;
                              handle->manager->stop(5000, err);
                              handle->manager.reset();
                            }
                            delete handle; ///< HostContext 析构会停线程与线程池
                          });
  /// 宿主已销毁: 允许再次创建 (进程单例标记复位)
  hostAliveFlag().store(false, std::memory_order_release);
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_set_language(MusicxxExternPluginHost *h,
                                   const MusicxxExternPluginStringView *lang,
                                   MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager) {
    setErr(log, "set_language: invalid handle");
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  const std::string langText = viewToStd(lang);
  return onHostThread(handle->manager.get(), [&]() -> int32_t {
    handle->manager->applyLanguage(langText);
    return MUSICXX_EXTERN_PLUGIN_OK;
  });
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_set_config(MusicxxExternPluginHost *h,
                                 const MusicxxExternPluginStringView *json,
                                 MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager) {
    setErr(log, "set_config: invalid handle");
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string err;
  const std::string text = viewToStd(json);
  const auto rc = onHostThread(handle->manager.get(), [&]() -> int32_t {
    return handle->manager->setConfig(text.empty() ? "{}" : text, err);
  });
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    setErr(log, err);
  }
  return rc;
}

/* ==================== 事件 ==================== */

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_set_wake_callback(MusicxxExternPluginHost *h,
                                        MusicxxExternPluginWakeFn fn,
                                        void *user_data) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  handle->manager->setWake(fn, user_data);
  return MUSICXX_EXTERN_PLUGIN_OK;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_poll_events(MusicxxExternPluginHost *h, int32_t max_count,
                                  MusicxxExternPluginString *out_json,
                                  MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_json) {
    setErr(log, "poll_events: invalid argument");
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string json;
  const int32_t rc = handle->manager->pollEvents(max_count, json);
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    return rc;
  }
  if (json.empty()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT; ///< 无事件
  }
  setOut(out_json, json);
  return MUSICXX_EXTERN_PLUGIN_OK;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_event_publish(
    MusicxxExternPluginHost *h, const MusicxxExternPluginStringView *topic,
    const MusicxxExternPluginStringView *payload_json,
    MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  const std::string topicText = viewToStd(topic);
  const std::string payload = viewToStd(payload_json);
  // 发布到**插件事件总线** (插件订阅者); 主题命名空间校验在宿主内完成。
  // 注意与 musicxx.host.* / musicxx.plugin.* 事件区分: 那些是宿主 → Dart
  // 的事件队列。
  const auto rc = handle->manager->publishPluginEvent(
      topicText, payload.empty() ? "{}" : payload);
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    setErr(log, "event_publish: 主题必须是 musicxx.* 或 plugin.<pluginId>.*");
  }
  return rc;
}

/* ==================== 插件管理 ==================== */

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_plugin_scan(MusicxxExternPluginHost *h,
                                  MusicxxExternPluginString *out_json,
                                  MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_json) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  return pluginxx::guardCall(
      [log](std::string_view msg) { setErr(log, msg); },
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL,
      [&]() -> int32_t {
        std::string json;
        std::string err;
        const auto rc = onHostThread(handle->manager.get(), [&]() -> int32_t {
          return handle->manager->scanPlugins(json, err);
        });
        if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
          setErr(log, err);
          return rc;
        }
        setOut(out_json, json);
        return MUSICXX_EXTERN_PLUGIN_OK;
      });
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_plugin_list(MusicxxExternPluginHost *h,
                                  MusicxxExternPluginString *out_json,
                                  MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_json) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string json;
  const auto rc = onHostThread(handle->manager.get(), [&]() -> int32_t {
    return handle->manager->pluginListJson(json);
  });
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    return rc;
  }
  setOut(out_json, json);
  (void)log;
  return MUSICXX_EXTERN_PLUGIN_OK;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_plugin_load(
    MusicxxExternPluginHost *h, const MusicxxExternPluginStringView *id_or_path,
    const MusicxxExternPluginStringView *options_json,
    MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string err;
  const auto rc = handle->manager->loadPlugin(viewToStd(id_or_path),
                                              viewToStd(options_json),
                                              /*sync=*/false, 0, err);
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    setErr(log, err);
  }
  return rc;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_plugin_load_sync(
    MusicxxExternPluginHost *h, const MusicxxExternPluginStringView *id_or_path,
    const MusicxxExternPluginStringView *options_json, uint32_t timeout_ms,
    MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string err;
  const auto rc = handle->manager->loadPlugin(viewToStd(id_or_path),
                                              viewToStd(options_json),
                                              /*sync=*/true, timeout_ms, err);
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    setErr(log, err);
  }
  return rc;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_plugin_enable(MusicxxExternPluginHost *h,
                                    const MusicxxExternPluginStringView *id,
                                    MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string err;
  const auto rc = onHostThread(handle->manager.get(), [&]() -> int32_t {
    return handle->manager->enablePlugin(viewToStd(id), err);
  });
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    setErr(log, err);
  }
  return rc;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_plugin_disable(MusicxxExternPluginHost *h,
                                     const MusicxxExternPluginStringView *id,
                                     MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string err;
  const auto rc = onHostThread(handle->manager.get(), [&]() -> int32_t {
    return handle->manager->disablePlugin(viewToStd(id), err);
  });
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    setErr(log, err);
  }
  return rc;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_plugin_unload(MusicxxExternPluginHost *h,
                                    const MusicxxExternPluginStringView *id,
                                    MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string err;
  const auto rc = handle->manager->unloadPlugin(viewToStd(id), 5000, err);
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    setErr(log, err);
  }
  return rc;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_plugin_reload(MusicxxExternPluginHost *h,
                                    const MusicxxExternPluginStringView *id,
                                    MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  const std::string pluginId = viewToStd(id);
  /// 插件参数保存在实例上 (`plugin_set_args`), 卸载后实例就没了: 先把它取出来,
  /// 重新装载时作为 options 传下去 (否则"设置参数 + 重载"会丢参数)。
  /// 读实例表必须在宿主线程上做 (内核的实例表只在宿主线程改动) —— 用一次
  /// onHostThread 投递取值, 不做同步等待以外的操作。
  std::string optionsJson{"{}"};
  onHostThread(handle->manager.get(), [&]() -> int32_t {
    auto inst = handle->manager->resolveInstance(pluginId);
    if (inst && inst->args.is_object() && false == inst->args.empty()) {
      utilxx_base::Json options;
      options["args"] = inst->args;
      optionsJson = options.dump();
    }
    return MUSICXX_EXTERN_PLUGIN_OK;
  });
  std::string err;
  if (handle->manager->unloadPlugin(pluginId, 5000, err) !=
      MUSICXX_EXTERN_PLUGIN_OK) {
    setErr(log, err);
    return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  }
  return handle->manager->loadPlugin(pluginId, optionsJson, true, 10000,
                                     err) == MUSICXX_EXTERN_PLUGIN_OK
             ? MUSICXX_EXTERN_PLUGIN_OK
             : (setErr(log, err), MUSICXX_EXTERN_PLUGIN_ERR_STATE);
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_plugin_set_args(
    MusicxxExternPluginHost *h, const MusicxxExternPluginStringView *id,
    const MusicxxExternPluginStringView *args_json,
    MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string err;
  const auto rc = onHostThread(handle->manager.get(), [&]() -> int32_t {
    return handle->manager->setPluginArgs(viewToStd(id), viewToStd(args_json),
                                          err);
  });
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    setErr(log, err);
  }
  return rc;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_plugin_get_config_path(
    MusicxxExternPluginHost *h, const MusicxxExternPluginStringView *id,
    MusicxxExternPluginString *out, MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string path;
  std::string err;
  const auto rc = onHostThread(handle->manager.get(), [&]() -> int32_t {
    return handle->manager->pluginConfigPath(viewToStd(id), path, err);
  });
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    setErr(log, err);
    return rc;
  }
  setOut(out, path);
  return MUSICXX_EXTERN_PLUGIN_OK;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_plugin_call(
    MusicxxExternPluginHost *h, const MusicxxExternPluginStringView *id,
    const MusicxxExternPluginStringView *method,
    const MusicxxExternPluginStringView *args_json, uint32_t timeout_ms,
    MusicxxExternPluginString *out_json, MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_json) {
    setErr(log, "plugin_call: invalid argument");
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  return pluginxx::guardCall([log](std::string_view msg) { setErr(log, msg); },
                             MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL,
                             [&]() -> int32_t {
                               std::string out;
                               std::string err;
                               const auto rc = handle->manager->pluginCall(
                                   viewToStd(id), viewToStd(method),
                                   viewToStd(args_json), timeout_ms, out, err);
                               if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
                                 setErr(log, err);
                                 return rc;
                               }
                               setOut(out_json, out);
                               return MUSICXX_EXTERN_PLUGIN_OK;
                             });
}

/* ==================== 钩子 ==================== */

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_hook_emit(MusicxxExternPluginHost *h,
                                const MusicxxExternPluginStringView *hook_id,
                                const MusicxxExternPluginStringView *input_json,
                                int32_t flags, uint32_t timeout_ms,
                                MusicxxExternPluginString *out_json,
                                MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !hook_id || !out_json) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  const bool sync = (flags & MUSICXX_EXTERN_PLUGIN_HOOK_SYNC) != 0;
  std::string result;
  std::string err;
  const auto rc = handle->manager->hookEmit(
      viewToStd(hook_id), viewToStd(input_json), sync, timeout_ms, result, err);
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    setErr(log, err);
    return rc;
  }
  setOut(out_json, result);
  return MUSICXX_EXTERN_PLUGIN_OK;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_hook_count(MusicxxExternPluginHost *h,
                                 const MusicxxExternPluginStringView *hook_id,
                                 int32_t *out_count,
                                 MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !hook_id || !out_count) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  int32_t count = 0;
  const auto rc = onHostThread(handle->manager.get(), [&]() -> int32_t {
    return handle->manager->hookCount(viewToStd(hook_id), count);
  });
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    setErr(log, "hook_count failed");
    return rc;
  }
  *out_count = count;
  return MUSICXX_EXTERN_PLUGIN_OK;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_hook_stats(MusicxxExternPluginHost *h,
                                 MusicxxExternPluginString *out_json,
                                 MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_json) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string json;
  const auto rc = onHostThread(handle->manager.get(), [&]() -> int32_t {
    return handle->manager->hookStatsJson(json);
  });
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    setErr(log, "hook_stats failed");
    return rc;
  }
  setOut(out_json, json);
  return MUSICXX_EXTERN_PLUGIN_OK;
}

/* ==================== 动作回应 ==================== */

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_action_respond(
    MusicxxExternPluginHost *h, int64_t request_id, int32_t status,
    const MusicxxExternPluginStringView *result_json,
    MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  const std::string result = viewToStd(result_json);
  const auto rc = onHostThread(handle->manager.get(), [&]() -> int32_t {
    return handle->manager->actionRespond(request_id, status, result);
  });
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    setErr(log, "action_respond: 未知 requestId");
  }
  return rc;
}

/* ==================== 状态镜像 ==================== */

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_state_update(
    MusicxxExternPluginHost *h, const MusicxxExternPluginStringView *key,
    const MusicxxExternPluginStringView *value_json,
    MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string err;
  const auto rc =
      handle->manager->stateUpdate(viewToStd(key), viewToStd(value_json), err);
  if (!err.empty()) {
    setErr(log, err);
  }
  return rc;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_state_update_batch(
    MusicxxExternPluginHost *h, const MusicxxExternPluginStringView *items_json,
    MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string err;
  const auto rc = handle->manager->stateUpdateBatch(viewToStd(items_json), err);
  if (!err.empty()) {
    setErr(log, err);
  }
  return rc;
}

/* ==================== UI / 日志 / 调试 / 统计 ==================== */

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_ui_snapshot(MusicxxExternPluginHost *h,
                                  MusicxxExternPluginString *out_json,
                                  MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_json) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string json;
  const auto rc = onHostThread(handle->manager.get(), [&]() -> int32_t {
    return handle->manager->uiSnapshot(json);
  });
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    setErr(log, "ui_snapshot failed");
    return rc;
  }
  setOut(out_json, json);
  return MUSICXX_EXTERN_PLUGIN_OK;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_log(MusicxxExternPluginHost *h, int32_t level,
                          const MusicxxExternPluginStringView *message,
                          MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  handle->manager->logMessage(level, viewToStd(message));
  (void)log;
  return MUSICXX_EXTERN_PLUGIN_OK;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_debug_info(MusicxxExternPluginHost *h,
                                 MusicxxExternPluginString *out_json,
                                 MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_json) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string json;
  const auto rc = onHostThread(handle->manager.get(), [&]() -> int32_t {
    return handle->manager->debugInfo(json);
  });
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    setErr(log, "debug_info failed");
    return rc;
  }
  setOut(out_json, json);
  return MUSICXX_EXTERN_PLUGIN_OK;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_stats(MusicxxExternPluginHost *h,
                            const MusicxxExternPluginStringView *scope_json,
                            MusicxxExternPluginString *out_json,
                            MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_json) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string scope = viewToStd(scope_json);
  const std::string json;
  std::string out;
  const auto rc = onHostThread(handle->manager.get(), [&]() -> int32_t {
    return handle->manager->statsJson(scope.empty() ? nullptr : &scope, out);
  });
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    setErr(log, "stats failed");
    return rc;
  }
  setOut(out_json, out);
  return MUSICXX_EXTERN_PLUGIN_OK;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_stats_config(
    MusicxxExternPluginHost *h, const MusicxxExternPluginStringView *cfg_json,
    MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string err;
  const auto rc = onHostThread(handle->manager.get(), [&]() -> int32_t {
    return handle->manager->statsConfig(viewToStd(cfg_json), err);
  });
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    setErr(log, err);
  }
  return rc;
}

} // extern "C"
