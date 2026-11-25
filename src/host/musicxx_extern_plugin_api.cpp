/// musicxx 外部插件框架 C ABI 实现 (Dart ⇄ 原生, v7)
///
/// 约定 (见 src/include/musicxx_extern_plugin_api.h):
/// - 所有导出函数用 `pluginxx::guardCall` 捕获异常, 绝不把异常带过 C ABI;
/// - 出参字符串一律经框架堆分配 (musicxx_extern_plugin_string_free 释放);
/// - **一次操作只有一种形状**: 受理返回 `requestId`, 结果经事件
///   `musicxx.request.result` 交回; 受理之后一定有一条结果 (插件卸载 / 框架停机 /
///   队列满时按失败回), 调用方不会悬挂;
/// - 受理阶段只做参数检查与记账: 工作体投到框架 IO 线程 (注册表 / 事件) 或插件执行
///   线程 (插件代码), **调用线程不会被占用**。

#include "musicxx_extern_plugin_api.h"
#include "host_inspect.h"
#include "host_json.h"
#include "musicxx_host.h"

#include "pluginxx/kit/guard.h"
#include "utilxx/net_error.h"
#include "utilxx_base/json.h"

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

using musicxx::extern_plugin::HostContext;
using musicxx::extern_plugin::MusicxxHostManager;
using musicxx::extern_plugin::RequestOutcome;

namespace {

/// 插件框架库自报的版本 (与 C 头里的 MUSICXX_EXTERN_PLUGIN_VERSION、Dart 侧
/// MusicxxPluginVersion.version 同一个值)
constexpr const char *kHostVersion = MUSICXX_EXTERN_PLUGIN_VERSION;

/// 框架句柄 (C ABI 层持有上下文与管理器; 由 host_create/host_destroy
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

/// 出参写入: 用框架分配器 (malloc) 分配, 由调用方 string_free 释放
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

/// 纯文本结果按 JSON 字符串交回 (例如配置路径)
std::string asJsonString(const std::string &text) {
  utilxx_base::Json value = text;
  return value.dump();
}

/// 受理失败时写 log 并返回 (事件队列满 / 参数错 / 未启动)
int32_t rejectWith(int32_t rc, MusicxxExternPluginString *log,
                   std::string_view what) {
  setErr(log, what);
  return rc;
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
    return PLUGINXX_RESULT_ERR_ARG;
  }
  setOut(out, std::string_view{src->data ? src->data : "",
                               static_cast<size_t>(src->size)});
  return PLUGINXX_RESULT_OK;
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

MUSICXX_EXTERN_PLUGIN_EXPORT const char* MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_result_name(int32_t code) {
  // 顺序无关：两段不重叠（通用 / 广播 / 生命周期在内，网络段 -100 ~ -199 在外）
  const char* name = pluginxx::result::nameOf(code);
  if (name) {
    return name;
  }
  return utilxx::net_error::nameOf(code);
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_version(MusicxxExternPluginString *out) {
  if (!out) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  setOut(out, kHostVersion);
  return PLUGINXX_RESULT_OK;
}

/* ==================== 框架生命周期 ==================== */

/// 进程级"框架实例已存在"标记 (框架运行时是进程单例)
///
/// 已存在的插件框架意味着**一条框架线程 + 一套插件实例 + 一条事件队列**;
/// 若允许再次创建, 同一进程会出现两套插件加载 (重复生命周期,
/// 插件目录/配置文件并发写), 因此第二次创建 直接拒绝并给出可读原因 (Dart
/// 侧表现为 init 抛出带诊断的异常)。
///
/// 复位时机: 框架实例被销毁 (`host_destroy`)
/// 或创建失败时。因此"先销毁再创建"是正常路径 (测试里的 init → dispose → init
/// 不受影响), 只有"同时存在两个插件框架"才会被拒绝。
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
          setErr(log, "host_create: 本进程已有插件框架实例 (框架运行时是进程单例) —— "
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
                                 int64_t *out_request_id,
                                 MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    setErr(log, "host_start: invalid handle");
    return PLUGINXX_RESULT_ERR_ARG;
  }
  return pluginxx::guardCall([log](std::string_view msg) { setErr(log, msg); },
                             PLUGINXX_RESULT_ERR_INTERNAL,
                             [&]() -> int32_t {
                               int64_t requestId = 0;
                               const int32_t acceptRc =
                                   handle->manager->acceptRequest("host.start",
                                                                 &requestId);
                               if (acceptRc != PLUGINXX_RESULT_OK) {
                                 return rejectWith(
                                     acceptRc, log,
                                     "host_start: 事件队列已满, 拒绝受理");
                               }
                               /// 起线程必须就地做: 框架线程此时还没有跑, 投递过去没人执行。
                               /// 这一步只是建线程与登记内置提供者, 不加载插件。
                               std::string err;
                               const int32_t rc = handle->manager->start(err);
                               if (rc != PLUGINXX_RESULT_OK) {
                                 handle->manager->completeRequest(
                                     requestId,
                                     RequestOutcome::fail(rc, err.empty()
                                                                 ? "host_start failed"
                                                                 : err));
                                 setErr(log, err);
                               } else {
                                 handle->manager->completeRequest(
                                     requestId, RequestOutcome::ok(R"({"ok":true})"));
                               }
                               *out_request_id = requestId;
                               return PLUGINXX_RESULT_OK;
                             });
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_host_shutdown(MusicxxExternPluginHost *h,
                                    int64_t *out_request_id,
                                    MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    setErr(log, "host_shutdown: invalid handle");
    return PLUGINXX_RESULT_ERR_ARG;
  }
  return pluginxx::guardCall([log](std::string_view msg) { setErr(log, msg); },
                             PLUGINXX_RESULT_ERR_INTERNAL,
                             [&]() -> int32_t {
                               int64_t requestId = 0;
                               const int32_t acceptRc = handle->manager->acceptRequest(
                                   "host.shutdown", &requestId);
                               if (acceptRc != PLUGINXX_RESULT_OK) {
                                 return rejectWith(
                                     acceptRc, log,
                                     "host_shutdown: 事件队列已满, 拒绝受理");
                               }
                               /// 关线程必须就地做 (关的就是框架线程, 投递过去会等自己)。
                               /// 结果在两条线程都停好之后才回。
                               std::string err;
                               const int32_t rc = handle->manager->stop(5000, err);
                               handle->manager->completeRequest(
                                   requestId,
                                   rc == PLUGINXX_RESULT_OK
                                       ? RequestOutcome::ok(R"({"ok":true})")
                                       : RequestOutcome::fail(rc, err));
                               if (rc != PLUGINXX_RESULT_OK) {
                                 setErr(log, err);
                               }
                               *out_request_id = requestId;
                               return PLUGINXX_RESULT_OK;
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
  /// 框架实例已销毁: 允许再次创建 (进程单例标记复位)
  hostAliveFlag().store(false, std::memory_order_release);
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_set_language(MusicxxExternPluginHost *h,
                                   const MusicxxExternPluginStringView *lang,
                                   int64_t *out_request_id,
                                   MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    setErr(log, "set_language: invalid handle");
    return PLUGINXX_RESULT_ERR_ARG;
  }
  const std::string langText = viewToStd(lang);
  auto mgr = handle->manager;
  return mgr->submitOnHostThread(
      "host.language",
      [mgr, langText]() -> RequestOutcome {
        mgr->applyLanguage(langText);
        return RequestOutcome::ok();
      },
      out_request_id);
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_set_config(MusicxxExternPluginHost *h,
                                 const MusicxxExternPluginStringView *json,
                                 int64_t *out_request_id,
                                 MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    setErr(log, "set_config: invalid handle");
    return PLUGINXX_RESULT_ERR_ARG;
  }
  auto mgr = handle->manager;
  std::string text = viewToStd(json);
  if (text.empty()) {
    text = "{}";
  }
  return mgr->submitOnHostThread(
      "host.config",
      [mgr, text]() -> RequestOutcome {
        std::string err;
        const int32_t rc = mgr->setConfig(text, err);
        if (rc != PLUGINXX_RESULT_OK) {
          return RequestOutcome::fail(rc, err);
        }
        return RequestOutcome::ok();
      },
      out_request_id);
}

/* ==================== 事件 ==================== */

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_set_wake_callback(MusicxxExternPluginHost *h,
                                        MusicxxExternPluginWakeFn fn,
                                        void *user_data) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  handle->manager->setWake(fn, user_data);
  return PLUGINXX_RESULT_OK;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_poll_events(MusicxxExternPluginHost *h, int32_t max_count,
                                  MusicxxExternPluginString *out_json,
                                  MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_json) {
    setErr(log, "poll_events: invalid argument");
    return PLUGINXX_RESULT_ERR_ARG;
  }
  std::string json;
  const int32_t rc = handle->manager->pollEvents(max_count, json);
  if (rc != PLUGINXX_RESULT_OK) {
    return rc;
  }
  if (json.empty()) {
    return PLUGINXX_RESULT_ERR_TIMEOUT; ///< 无事件
  }
  setOut(out_json, json);
  return PLUGINXX_RESULT_OK;
}

/* ==================== 插件管理 ==================== */

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_plugin_scan(MusicxxExternPluginHost *h,
                                  int64_t *out_request_id,
                                  MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  auto mgr = handle->manager;
  return mgr->submitOnHostThread(
      "plugin.scan",
      [mgr]() -> RequestOutcome {
        std::string json;
        std::string err;
        const int32_t rc = mgr->scanPlugins(json, err);
        if (rc != PLUGINXX_RESULT_OK) {
          return RequestOutcome::fail(rc, err);
        }
        return RequestOutcome::ok(json);
      },
      out_request_id);
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_plugin_inspect(MusicxxExternPluginHost *h,
                                     const MusicxxExternPluginStringView *dir,
                                     const MusicxxExternPluginStringView *os,
                                     const MusicxxExternPluginStringView *arch,
                                     int64_t *out_request_id,
                                     MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !dir || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  const std::string dirText = viewToStd(dir);
  if (dirText.empty()) {
    setErr(log, "plugin_inspect: dir 不能为空");
    return PLUGINXX_RESULT_ERR_ARG;
  }
  const std::string osText = viewToStd(os);
  const std::string archText = viewToStd(arch);
  auto mgr = handle->manager;
  /// 只读的目录判定: 占的是框架线程, 但对手是文件系统 —— 不占调用线程就达到目的了
  return mgr->submitOnHostThread(
      "plugin.inspect",
      [mgr, dirText, osText, archText]() -> RequestOutcome {
        // 形态注册表取一份快照: 注册 / 撤销在框架线程, 这里也读同一线程的本体
        const musicxx::extern_plugin::MusicxxPluginKindRegistry kinds =
            mgr->kindRegistrySnapshot();
        const musicxx::extern_plugin::MusicxxPluginInspect info =
            musicxx::extern_plugin::inspectPluginDir(
                std::filesystem::path{dirText}, osText, archText, &kinds);
        return RequestOutcome::ok(
            musicxx::extern_plugin::inspectToJson(info));
      },
      out_request_id);
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_plugin_list(MusicxxExternPluginHost *h,
                                  int64_t *out_request_id,
                                  MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  (void)log;
  auto mgr = handle->manager;
  return mgr->submitOnHostThread(
      "plugin.list",
      [mgr]() -> RequestOutcome {
        std::string json;
        const int32_t rc = mgr->pluginListJson(json);
        if (rc != PLUGINXX_RESULT_OK) {
          return RequestOutcome::fail(rc, "plugin_list failed");
        }
        return RequestOutcome::ok(json);
      },
      out_request_id);
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_plugin_load(
    MusicxxExternPluginHost *h, const MusicxxExternPluginStringView *id_or_path,
    const MusicxxExternPluginStringView *options_json, int64_t *out_request_id,
    MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  std::string err;
  const int32_t rc = handle->manager->loadPlugin(
      viewToStd(id_or_path), viewToStd(options_json), out_request_id, err);
  if (rc != PLUGINXX_RESULT_OK) {
    setErr(log, err);
  }
  return rc;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_plugin_enable(MusicxxExternPluginHost *h,
                                    const MusicxxExternPluginStringView *id,
                                    int64_t *out_request_id,
                                    MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  std::string err;
  const int32_t rc =
      handle->manager->enablePlugin(viewToStd(id), out_request_id, err);
  if (rc != PLUGINXX_RESULT_OK) {
    setErr(log, err);
  }
  return rc;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_plugin_disable(MusicxxExternPluginHost *h,
                                     const MusicxxExternPluginStringView *id,
                                     int64_t *out_request_id,
                                     MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  std::string err;
  const int32_t rc =
      handle->manager->disablePlugin(viewToStd(id), out_request_id, err);
  if (rc != PLUGINXX_RESULT_OK) {
    setErr(log, err);
  }
  return rc;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_plugin_unload(MusicxxExternPluginHost *h,
                                    const MusicxxExternPluginStringView *id,
                                    int64_t *out_request_id,
                                    MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  std::string err;
  const int32_t rc =
      handle->manager->unloadPlugin(viewToStd(id), out_request_id, err);
  if (rc != PLUGINXX_RESULT_OK) {
    setErr(log, err);
  }
  return rc;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_plugin_reload(MusicxxExternPluginHost *h,
                                    const MusicxxExternPluginStringView *id,
                                    int64_t *out_request_id,
                                    MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  std::string err;
  const int32_t rc =
      handle->manager->reloadPlugin(viewToStd(id), out_request_id, err);
  if (rc != PLUGINXX_RESULT_OK) {
    setErr(log, err);
  }
  return rc;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_plugin_set_args(
    MusicxxExternPluginHost *h, const MusicxxExternPluginStringView *id,
    const MusicxxExternPluginStringView *args_json, int64_t *out_request_id,
    MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  auto mgr = handle->manager;
  const std::string idText = viewToStd(id);
  const std::string argsText = viewToStd(args_json);
  return mgr->submitOnHostThread(
      "plugin.setArgs",
      [mgr, idText, argsText]() -> RequestOutcome {
        std::string err;
        const int32_t rc = mgr->setPluginArgs(idText, argsText, err);
        if (rc != PLUGINXX_RESULT_OK) {
          return RequestOutcome::fail(rc, err);
        }
        return RequestOutcome::ok();
      },
      out_request_id);
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_plugin_get_config_path(
    MusicxxExternPluginHost *h, const MusicxxExternPluginStringView *id,
    int64_t *out_request_id, MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  (void)log;
  auto mgr = handle->manager;
  const std::string idText = viewToStd(id);
  return mgr->submitOnHostThread(
      "plugin.configPath",
      [mgr, idText]() -> RequestOutcome {
        std::string path;
        std::string err;
        const int32_t rc = mgr->pluginConfigPath(idText, path, err);
        if (rc != PLUGINXX_RESULT_OK) {
          return RequestOutcome::fail(rc, err);
        }
        return RequestOutcome::ok(asJsonString(path));
      },
      out_request_id);
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_plugin_call(
    MusicxxExternPluginHost *h, const MusicxxExternPluginStringView *id,
    const MusicxxExternPluginStringView *method,
    const MusicxxExternPluginStringView *args_json, int64_t *out_request_id,
    MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    setErr(log, "plugin_call: invalid argument");
    return PLUGINXX_RESULT_ERR_ARG;
  }
  std::string err;
  const int32_t rc = handle->manager->pluginCall(
      viewToStd(id), viewToStd(method), viewToStd(args_json), out_request_id,
      err);
  if (rc != PLUGINXX_RESULT_OK) {
    setErr(log, err);
  }
  return rc;
}

/* ==================== 能力派发 ==================== */

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_capability_dispatch(
    MusicxxExternPluginHost *h,
    const MusicxxExternPluginStringView *capability_id,
    const MusicxxExternPluginStringView *input_json, int64_t *out_request_id,
    MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !capability_id || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  std::string err;
  const int32_t rc = handle->manager->dispatchCapability(
      viewToStd(capability_id), viewToStd(input_json), out_request_id, err);
  if (rc != PLUGINXX_RESULT_OK) {
    setErr(log, err);
  }
  return rc;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_capability_stats(MusicxxExternPluginHost *h,
                                       int64_t *out_request_id,
                                       MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  (void)log;
  auto mgr = handle->manager;
  return mgr->submitOnHostThread(
      "capability.stats",
      [mgr]() -> RequestOutcome {
        std::string json;
        const int32_t rc = mgr->capabilityStatsJson(json);
        if (rc != PLUGINXX_RESULT_OK) {
          return RequestOutcome::fail(rc, "capability stats failed");
        }
        return RequestOutcome::ok(json);
      },
      out_request_id);
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_capability_handlers(
    MusicxxExternPluginHost *h, const MusicxxExternPluginStringView *capability_id,
    int64_t *out_request_id, MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !capability_id || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  (void)log;
  auto mgr = handle->manager;
  const std::string idText = viewToStd(capability_id);
  return mgr->submitOnHostThread(
      "capability.handlers",
      [mgr, idText]() -> RequestOutcome {
        std::string json;
        const int32_t rc = mgr->capabilityHandlersJson(idText, json);
        if (rc != PLUGINXX_RESULT_OK) {
          return RequestOutcome::fail(rc, "capability handlers failed");
        }
        return RequestOutcome::ok(json);
      },
      out_request_id);
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_capability_dispatch_phase(
    MusicxxExternPluginHost *h, const MusicxxExternPluginStringView *capability_id,
    int32_t phase, const MusicxxExternPluginStringView *input_json,
    int64_t *out_request_id, MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !capability_id || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  if (phase < MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_IMPL ||
      phase > MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_AFTER) {
    setErr(log, "capability dispatch: invalid phase");
    return PLUGINXX_RESULT_ERR_ARG;
  }
  std::string err;
  const int32_t rc = handle->manager->dispatchCapabilityPhase(
      viewToStd(capability_id), phase, viewToStd(input_json), out_request_id,
      err);
  if (rc != PLUGINXX_RESULT_OK) {
    setErr(log, err);
  }
  return rc;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_capability_handlers_phase(
    MusicxxExternPluginHost *h, const MusicxxExternPluginStringView *capability_id,
    int32_t phase, int64_t *out_request_id, MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !capability_id || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  (void)log;
  auto mgr = handle->manager;
  const std::string idText = viewToStd(capability_id);
  return mgr->submitOnHostThread(
      "capability.handlers",
      [mgr, idText, phase]() -> RequestOutcome {
        std::string json;
        const int32_t rc =
            mgr->capabilityHandlersJsonPhase(idText, phase, json);
        if (rc != PLUGINXX_RESULT_OK) {
          return RequestOutcome::fail(rc, "capability handlers (phase) failed");
        }
        return RequestOutcome::ok(json);
      },
      out_request_id);
}

/* ==================== 能力调用的回应 ==================== */
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_capability_app_respond(
    MusicxxExternPluginHost *h, int64_t request_id, int32_t status,
    const MusicxxExternPluginStringView *result_json, int64_t *out_request_id,
    MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  (void)log;
  auto mgr = handle->manager;
  const std::string result = viewToStd(result_json);
  /// 处理器链已经搬离框架 IO 线程, 这条回复可以放心排队 (以前不能转投是因为
  /// 框架线程常常正等 JS 的处理器返回)
  return mgr->submitOnHostThread(
      "capability.respond",
      [mgr, request_id, status, result]() -> RequestOutcome {
        const int32_t rc =
            mgr->respondAppCapability(request_id, status, result);
        if (rc != PLUGINXX_RESULT_OK) {
          return RequestOutcome::fail(rc, "capability_app_respond: 未知 requestId",
                                      "not_found");
        }
        return RequestOutcome::ok();
      },
      out_request_id);
}

/* ==================== 广播 (只发布 / 读 / 列表) ==================== */

int32_t MUSICXX_EXTERN_PLUGIN_CALL musicxx_extern_plugin_feature_bits(void) {
  return MUSICXX_EXTERN_PLUGIN_FEATURE_CAPABILITIES |
         MUSICXX_EXTERN_PLUGIN_FEATURE_BROADCASTS |
         MUSICXX_EXTERN_PLUGIN_FEATURE_UI |
         MUSICXX_EXTERN_PLUGIN_FEATURE_CAPABILITY_PHASES |
         MUSICXX_EXTERN_PLUGIN_FEATURE_NET |
         MUSICXX_EXTERN_PLUGIN_FEATURE_REQUESTS;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_broadcast_publish(
    MusicxxExternPluginHost *h, const MusicxxExternPluginStringView *topic,
    const MusicxxExternPluginStringView *payload_json, int64_t *out_request_id,
    MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !topic || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  (void)log;
  auto mgr = handle->manager;
  std::string topicText = viewToStd(topic);
  std::string payload = viewToStd(payload_json);
  if (payload.empty()) {
    payload = "{}";
  }
  /// 发布到**广播注册表** (插件订阅者); 主题命名空间校验在框架内完成.
  /// 注意与 `musicxx.host.*` / `musicxx.plugin.*` 事件区分: 那些是插件框架 → Dart
  /// 的事件队列.
  return mgr->submitOnHostThread(
      "broadcast.publish",
      [mgr, topicText = std::move(topicText),
       payload = std::move(payload)]() -> RequestOutcome {
        const int32_t rc = mgr->broadcastPublish(topicText, payload);
        if (rc != PLUGINXX_RESULT_OK) {
          return RequestOutcome::fail(
              rc, "broadcast_publish: 主题必须是 musicxx.* 或 plugin.<pluginId>.*");
        }
        return RequestOutcome::ok();
      },
      out_request_id);
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_broadcast_read(MusicxxExternPluginHost *h,
                                     const MusicxxExternPluginStringView *id,
                                     int64_t *out_request_id,
                                     MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !id || !id->data || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  (void)log;
  auto mgr = handle->manager;
  const std::string idText = viewToStd(id);
  return mgr->submitOnHostThread(
      "broadcast.read",
      [mgr, idText]() -> RequestOutcome {
        // 读当前值只查广播注册表 (内部加锁), 不需要投递到框架线程
        std::string value;
        if (mgr->broadcasts()->read(idText, &value)) {
          return RequestOutcome::ok(value);
        }
        if (!mgr->broadcasts()->declares(idText)) {
          return RequestOutcome::fail(PLUGINXX_RESULT_ERR_NOT_FOUND,
                                      "频道未声明 (" + idText + ")",
                                      "not_declared");
        }
        return RequestOutcome::fail(PLUGINXX_RESULT_ERR_ARG,
                                    "频道没有当前值 (" + idText + ")",
                                    "no_value");
      },
      out_request_id);
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_broadcast_list(MusicxxExternPluginHost *h,
                                     const MusicxxExternPluginStringView *prefix,
                                     int64_t *out_request_id,
                                     MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  (void)log;
  auto mgr = handle->manager;
  const std::string head = prefix ? viewToStd(prefix) : std::string{};
  return mgr->submitOnHostThread(
      "broadcast.list",
      [mgr, head]() -> RequestOutcome {
        return RequestOutcome::ok(mgr->broadcasts()->listJson(head));
      },
      out_request_id);
}

/* ==================== UI / 日志 / 调试 / 统计 ==================== */

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_ui_snapshot(MusicxxExternPluginHost *h,
                                  int64_t *out_request_id,
                                  MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  (void)log;
  auto mgr = handle->manager;
  return mgr->submitOnHostThread(
      "ui.snapshot",
      [mgr]() -> RequestOutcome {
        std::string json;
        const int32_t rc = mgr->uiSnapshot(json);
        if (rc != PLUGINXX_RESULT_OK) {
          return RequestOutcome::fail(rc, "ui_snapshot failed");
        }
        return RequestOutcome::ok(json);
      },
      out_request_id);
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_ui_contents(MusicxxExternPluginHost *h,
                                  int64_t *out_request_id,
                                  MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  (void)log;
  auto mgr = handle->manager;
  return mgr->submitOnHostThread(
      "ui.contents",
      [mgr]() -> RequestOutcome {
        std::string json;
        const int32_t rc = mgr->listUiContents(nullptr, json);
        if (rc != PLUGINXX_RESULT_OK) {
          return RequestOutcome::fail(rc, "ui_contents failed");
        }
        return RequestOutcome::ok(json);
      },
      out_request_id);
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_ui_slots(MusicxxExternPluginHost *h,
                               int64_t *out_request_id,
                               MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  (void)log;
  auto mgr = handle->manager;
  return mgr->submitOnHostThread(
      "ui.slots",
      [mgr]() -> RequestOutcome {
        std::string json;
        const int32_t rc = mgr->listUiSlots(nullptr, json);
        if (rc != PLUGINXX_RESULT_OK) {
          return RequestOutcome::fail(rc, "ui_slots failed");
        }
        return RequestOutcome::ok(json);
      },
      out_request_id);
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_ui_request(MusicxxExternPluginHost *h,
                                 const MusicxxExternPluginStringView *id,
                                 const MusicxxExternPluginStringView *context_json,
                                 int64_t *out_request_id,
                                 MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !id || !id->data || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  const std::string contentId = viewToStd(id);
  const std::string context = viewToStd(context_json);
  const int32_t rc = handle->manager->requestUiContent(contentId, context,
                                                       out_request_id);
  if (rc == PLUGINXX_RESULT_ERR_NOT_FOUND) {
    setErr(log, "ui_request: 没有这份内容 (" + contentId + ")");
    return rc;
  }
  if (rc != PLUGINXX_RESULT_OK) {
    setErr(log, "ui_request: 插件生成失败 (" + contentId + ")");
    return rc;
  }
  return PLUGINXX_RESULT_OK;
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_broadcast_watch(MusicxxExternPluginHost *h,
                                      const MusicxxExternPluginStringView *id,
                                      int32_t watch, int64_t *out_request_id,
                                      MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !id || !id->data || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  (void)log;
  auto mgr = handle->manager;
  const std::string topic = viewToStd(id);
  return mgr->submitOnHostThread(
      "broadcast.watch",
      [mgr, topic, watch]() -> RequestOutcome {
        const int32_t rc = mgr->watchBroadcast(topic, watch != 0);
        if (rc != PLUGINXX_RESULT_OK) {
          return RequestOutcome::fail(
              rc, "广播频道 id 形状不合法 (" + topic + ")",
              "invalid_argument");
        }
        return RequestOutcome::ok();
      },
      out_request_id);
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_log(MusicxxExternPluginHost *h, int32_t level,
                          const MusicxxExternPluginStringView *message,
                          int64_t *out_request_id,
                          MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  (void)log;
  auto mgr = handle->manager;
  const std::string text = viewToStd(message);
  return mgr->submitOnHostThread(
      "log",
      [mgr, level, text]() -> RequestOutcome {
        mgr->logMessage(level, text);
        return RequestOutcome::ok();
      },
      out_request_id);
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_debug_info(MusicxxExternPluginHost *h,
                                 int64_t *out_request_id,
                                 MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  (void)log;
  auto mgr = handle->manager;
  return mgr->submitOnHostThread(
      "debug.info",
      [mgr]() -> RequestOutcome {
        std::string json;
        const int32_t rc = mgr->debugInfo(json);
        if (rc != PLUGINXX_RESULT_OK) {
          return RequestOutcome::fail(rc, "debug_info failed");
        }
        return RequestOutcome::ok(json);
      },
      out_request_id);
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_stats(MusicxxExternPluginHost *h,
                            const MusicxxExternPluginStringView *scope_json,
                            int64_t *out_request_id,
                            MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  (void)log;
  auto mgr = handle->manager;
  const std::string scope = viewToStd(scope_json);
  return mgr->submitOnHostThread(
      "stats",
      [mgr, scope]() -> RequestOutcome {
        std::string out;
        const int32_t rc =
            mgr->statsJson(scope.empty() ? nullptr : &scope, out);
        if (rc != PLUGINXX_RESULT_OK) {
          return RequestOutcome::fail(rc, "stats failed");
        }
        return RequestOutcome::ok(out);
      },
      out_request_id);
}

MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
musicxx_extern_plugin_stats_config(MusicxxExternPluginHost *h,
                                   const MusicxxExternPluginStringView *cfg_json,
                                   int64_t *out_request_id,
                                   MusicxxExternPluginString *log) {
  auto *handle = asHandle(h);
  if (!handle || !handle->manager || !out_request_id) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  (void)log;
  auto mgr = handle->manager;
  const std::string cfg = viewToStd(cfg_json);
  return mgr->submitOnHostThread(
      "stats.config",
      [mgr, cfg]() -> RequestOutcome {
        std::string err;
        const int32_t rc = mgr->statsConfig(cfg, err);
        if (rc != PLUGINXX_RESULT_OK) {
          return RequestOutcome::fail(rc, err);
        }
        return RequestOutcome::ok();
      },
      out_request_id);
}

} // extern "C"
