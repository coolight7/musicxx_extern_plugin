/// musicxx 外部插件原生插件框架: `musicxx.net` 表 (插件网络能力)
///
/// 表体只有三个入口: 同步 `call` / 异步 `call_async` / `cancel`, 全部用
/// `op + 参数 JSON → 结果 JSON` 的写法 —— 新增操作只加 op 名, 不动 C ABI。
/// 具体实现与线程模型见 host_net.h / host_net.cpp。
///
/// 线程: 表入口可在任意线程调用 (JS 线程、插件框架线程、Dart 线程);
/// 内部投递到网络线程, 需要结果时有界等待。异步完成回调投递回插件框架线程后
/// 才执行 —— 动态库插件代码只在框架线程跑。

#include "host_json.h"
#include "musicxx_host.h"

#include "pluginxx/host/abi_util.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace musicxx {
namespace extern_plugin {

using utilxx_base::Json;

namespace {

std::string viewToStr(const PluginxxStringView *view) {
  if (!view || !view->data) {
    return {};
  }
  return std::string{view->data, static_cast<size_t>(view->size)};
}

/* ==================== 同步操作 ==================== */

int32_t PLUGINXX_CALL xx_net_call(const PluginxxHost *host,
                                  const PluginxxStringView *op,
                                  const PluginxxStringView *argsJson,
                                  PluginxxString *outJson) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!op || !op->data) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto call =
            pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
                host);
        if (!call.ok()) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto mgr = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        const std::string pluginId = MusicxxHostManager::pluginIdOf(inst->name);
        const std::string opText = viewToStr(op);
        const std::string argsText = viewToStr(argsJson);
        std::string result;
        const int32_t rc = mgr->netCall(pluginId, opText, argsText, result);
        if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
          return rc;
        }
        if (outJson) {
          pluginxx::hostMemorySetString(outJson, result);
        }
        return MUSICXX_EXTERN_PLUGIN_OK;
      });
}

/* ==================== 异步操作 ==================== */

int32_t PLUGINXX_CALL xx_net_call_async(
    const PluginxxHost *host, const PluginxxStringView *op,
    const PluginxxStringView *argsJson, const PluginxxOperatorNotify *notify,
    PluginxxString *outJson, int64_t *outRequestId) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!op || !op->data || !notify || !notify->done) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto call =
            pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
                host);
        if (!call.ok()) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto mgr = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        const std::string pluginId = MusicxxHostManager::pluginIdOf(inst->name);
        const std::string opText = viewToStr(op);
        const std::string argsText = viewToStr(argsJson);
        // 通知器由插件 (SDK) 保活到结算: 这里只复制结构体, host_ud 仍指向插件侧
        const PluginxxOperatorNotify notifyCopy = *notify;
        auto done = [notifyCopy](int32_t status, const std::string &payload) {
          if (!notifyCopy.done) {
            return;
          }
          PluginxxStringView view{};
          if (!payload.empty()) {
            view.data = payload.data();
            view.size = payload.size();
          }
          notifyCopy.done(notifyCopy.host_ud, status,
                          payload.empty() ? nullptr : &view);
        };
        std::string result;
        const int32_t rc = mgr->netCallAsync(pluginId, opText, argsText,
                                             std::move(done), result,
                                             outRequestId);
        if (outJson && !result.empty()) {
          pluginxx::hostMemorySetString(outJson, result);
        }
        return rc;
      });
}

/* ==================== 取消 ==================== */

int32_t PLUGINXX_CALL xx_net_cancel(const PluginxxHost *host,
                                    int64_t requestId,
                                    PluginxxString *outJson) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        auto call =
            pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
                host);
        if (!call.ok()) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto mgr = call.manager();
        auto inst = call.instance();
        if (!mgr || !inst) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        const std::string pluginId = MusicxxHostManager::pluginIdOf(inst->name);
        std::string result;
        const int32_t rc = mgr->netCancel(pluginId, requestId, result);
        if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
          return rc;
        }
        if (outJson) {
          pluginxx::hostMemorySetString(outJson, result);
        }
        return MUSICXX_EXTERN_PLUGIN_OK;
      });
}

const MusicxxPluginNetIface g_ifaceNet = {
    /* version */ MUSICXX_PLUGIN_IFACE_NET_VERSION,
    /* struct_size */ sizeof(MusicxxPluginNetIface),
    /* call */ xx_net_call,
    /* call_async */ xx_net_call_async,
    /* cancel */ xx_net_cancel,
};

} // namespace

const void *netIfaceForQuery() { return &g_ifaceNet; }

} // namespace extern_plugin
} // namespace musicxx
