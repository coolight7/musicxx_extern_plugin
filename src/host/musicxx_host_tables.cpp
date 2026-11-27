/// musicxx 外部插件原生插件框架: 能力派发 (宿主自己的实现表) 与专用接口表
///
/// 能力 / 广播的声明只有一份: `tools/capabilities.def.json` → `tools/gen_contract.dart`
/// 生成 `musicxx/plugin/api/capability_ids.g.h`（插件框架与插件开发者共用同一份）。
/// 宿主在这里按 id 查声明，决定一条 id 能不能派发、开哪几个阶段、异步要不要回传结果。
///
/// 线程约定: 表入口在 Dart 线程被调用, 内部投递到框架线程并等待;
/// 已在框架线程时就地执行 (插件框架 `ioCallSyncKeep` 语义)。

#include "host_json.h"
#include "host_naming.h"
#include "musicxx_host.h"

#include "musicxx/plugin/api/capability_ids.g.h"
#include "pluginxx/host/abi_util.h"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <asio/steady_timer.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace musicxx {
namespace extern_plugin {

using utilxx_base::Json;

namespace {

/// 命名空间规则 单一实现在 host_naming.h
///
/// 说明: 名字不再做归属校验 (插件可以调任意名字), 这里只保留广播频道的
/// 归属判定 (发布事件仍要求 `musicxx.*` 或自己命名空间)。
using naming::isTopicOwnedBy;

/// 应用给一条能力声明的超时上限 (`tools/capabilities.def.json` 的 `timeout_ms`)
///
/// 0 = 不限制; 未声明的 id 也返回 0。
int32_t declaredTimeoutOf(const std::string &capabilityId) {
  for (size_t i = 0; i < musicxxCapabilityDeclCount; ++i) {
    const MusicxxCapabilityDecl &entry = musicxxCapabilityDecls[i];
    if (entry.id && capabilityId == entry.id) {
      return entry.timeout_ms;
    }
  }
  return 0;
}

/// 三处超时取"最小的正数": 0 / 负数都当"这一处不限制"
int64_t minPositiveTimeout(int64_t a, int64_t b) {
  if (a <= 0) {
    return b > 0 ? b : 0;
  }
  if (b <= 0) {
    return a;
  }
  return (std::min)(a, b);
}

/// 阶段编号是否合法 (`impl` / `before` / `after`)
bool isValidPhase(int32_t phase) {
  return phase >= MUSICXX_CAPABILITY_PHASE_IMPL &&
         phase <= MUSICXX_CAPABILITY_PHASE_AFTER;
}

/// 阶段编号 → 位掩码 (`1 << 阶段编号`; 非法编号给 0)
uint32_t phaseMaskOf(int32_t phase) {
  return isValidPhase(phase) ? (1u << static_cast<uint32_t>(phase)) : 0u;
}

/// 阶段编号 → 对外字符串 (`impl` / `before` / `after`; 非法编号给空串)
const char *phaseNameOf(int32_t phase) {
  switch (phase) {
  case MUSICXX_CAPABILITY_PHASE_IMPL:
    return "impl";
  case MUSICXX_CAPABILITY_PHASE_BEFORE:
    return "before";
  case MUSICXX_CAPABILITY_PHASE_AFTER:
    return "after";
  default:
    return "";
  }
}

/// 一条 id 的派发属性
///
/// 能力与广播各有一份声明, 同一个 id 也可能两样都写 (`musicxx.app.foreground` 这类
/// 既是能力又是广播): 派发一律按广播处理 —— 广播只通知, 不回报结果。
struct CapabilityMeta {
  bool notifyOnly = false; ///< 广播: 只通知, 异步派发不回传结果
  uint32_t phases = 0;     ///< 开放的阶段位掩码
  int32_t merge = 0;       ///< 结果合并策略 (MUSICXX_CAPABILITY_MERGE_*; 0 = 不适用)
};

/// 查一条 id 的**预设**派发属性 (预设表里没有它返回 false)
bool presetMetaOf(std::string_view id, CapabilityMeta *out) {
  if (!out || id.empty()) {
    return false;
  }
  const std::string text{id};
  const MusicxxBroadcastDecl *broadcast = musicxxBroadcastDeclOf(text.c_str());
  if (broadcast != nullptr) {
    out->notifyOnly = true;
    out->phases = phaseMaskOf(MUSICXX_CAPABILITY_PHASE_IMPL);
    return true;
  }
  const MusicxxCapabilityDecl *decl = musicxxCapabilityDeclOf(text.c_str());
  if (decl == nullptr) {
    return false;
  }
  out->notifyOnly = false;
  out->merge = decl->merge;
  out->phases = decl->phase_mask == 0 ? phaseMaskOf(MUSICXX_CAPABILITY_PHASE_IMPL)
                                      : decl->phase_mask;
  return true;
}

/// 派发属性：预设表里没有的 id 用默认值
///
/// 预设表只是“常用能力的属性表”，不是白名单：自定义 id 照样能注册与派发，
/// 只是没有预设的合并策略与阶段限制（三个阶段都能注册、不提前终止）。
CapabilityMeta metaOf(std::string_view id) {
  CapabilityMeta meta;
  if (presetMetaOf(id, &meta)) {
    return meta;
  }
  meta.notifyOnly = false;
  meta.merge = 0;
  meta.phases = phaseMaskOf(MUSICXX_CAPABILITY_PHASE_IMPL) |
                phaseMaskOf(MUSICXX_CAPABILITY_PHASE_BEFORE) |
                phaseMaskOf(MUSICXX_CAPABILITY_PHASE_AFTER);
  return meta;
}

/// 把内部拼好的 JSON 数组文本转成 Json（定义见文件下方）
///
/// 这段文本由 `capabilityHandlersArrayJson` 自己构造，正常不会解析失败；真失败时退回空数组
/// （本项目的 Json::parse 没有"不抛异常"的重载，所以用 try/catch）。
Json jsonArrayOf(const std::string &text);

} // namespace

/* ==================== 实现注册 / 注销 ==================== */

namespace {

/// 一条 id 能不能登记这个阶段
///
/// 预设表里没有的 id 也能登记（三个阶段都开放）；预设过的 id 只能登记它开了的阶段 ——
/// 登记了不派发的阶段等于给插件一个永远不触发的坑。
bool checkRegistrable(std::string_view capabilityId, int32_t phase,
                      std::string *reason) {
  const CapabilityMeta meta = metaOf(capabilityId);
  if ((meta.phases & phaseMaskOf(phase)) == 0) {
    if (reason) {
      *reason = "capability `" + std::string{capabilityId} +
                "` does not open phase " + phaseNameOf(phase);
    }
    return false;
  }
  return true;
}

} // namespace

void MusicxxHostManager::dropHandlerSlot(
    std::vector<CapabilityHandler> &handlers, const std::string &plugin,
    const std::string &ownerTag, int32_t phase) {
  handlers.erase(std::remove_if(handlers.begin(), handlers.end(),
                                [&](const CapabilityHandler &h) {
                                  return h.plugin == plugin &&
                                         h.ownerTag == ownerTag &&
                                         h.phase == phase;
                                }),
                 handlers.end());
}

int32_t MusicxxHostManager::registerHandler(
    MusicxxHostInstance *inst, const MusicxxPluginCapabilitySpec &spec) {
  if (!inst) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  if (!spec.capability_id.data || spec.capability_id.size == 0) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  const std::string capabilityId{spec.capability_id.data,
                                 static_cast<size_t>(spec.capability_id.size)};
  if (!spec.run_start) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  const int32_t phase = spec.phase;
  if (!isValidPhase(phase)) {
    XX_LOGW("[musicxx_ext] registration by plugin `{}` of `{}` rejected: invalid phase {}",
            inst->name, capabilityId, phase);
    return PLUGINXX_RESULT_ERR_ARG;
  }
  if (inst->unloadRequested || !inst->enabled) {
    XX_LOGW("[musicxx_ext] registration by plugin `{}` of `{}` rejected: instance is unloading or disabled",
            inst->name, capabilityId);
    return PLUGINXX_RESULT_ERR_STATE;
  }
  std::string reason;
  if (!checkRegistrable(capabilityId, phase, &reason)) {
    XX_LOGW("[musicxx_ext] registration by plugin `{}` rejected: {}", inst->name, reason);
    return PLUGINXX_RESULT_ERR_NOT_FOUND;
  }

  const std::string ownerTag =
      spec.owner_tag.data && spec.owner_tag.size > 0
          ? std::string{spec.owner_tag.data,
                        static_cast<size_t>(spec.owner_tag.size)}
          : std::string{};

  auto &handlers = handlers_[capabilityId];
  dropHandlerSlot(handlers, inst->name, ownerTag, phase);

  CapabilityHandler h;
  h.plugin = inst->name;
  h.pluginId = pluginIdOf(inst->name);
  h.ownerTag = ownerTag;
  h.capabilityId = capabilityId;
  h.spec = spec;
  h.phase = phase;
  h.seq = ++handlerSeq_;
  handlers.push_back(h);

  XX_LOGI("[musicxx_ext] plugin `{}` registered `{}` (phase={}, priority={}, "
          "handlers={})",
          inst->name, capabilityId, phaseNameOf(phase), spec.priority,
          handlers.size());

  // 通知 Dart 侧刷新"这个能力有没有处理器"的位图与各阶段的处理器列表
  pushEvent("musicxx.capability.changed", h.pluginId,
            capabilityChangedPayloadJson(capabilityId, "register", h.pluginId));
  return PLUGINXX_RESULT_OK;
}

/* ==================== 能力表 `musicxx.capabilities` 的入口 ==================== */

int32_t MusicxxHostManager::registerCapabilityHandler(
    MusicxxHostInstance *inst, const std::string &capabilityId, int32_t phase,
    int32_t priority, const std::string &ownerTag,
    PluginxxCapabilityStartFunction start,
    PluginxxOperatorCancelFunction cancel, void *ctx, std::string &error) {
  if (!inst || !start) {
    error = "capability: missing instance or handler";
    return PLUGINXX_RESULT_ERR_ARG;
  }
  if (!isValidPhase(phase)) {
    error = "capability: illegal phase";
    return PLUGINXX_RESULT_ERR_ARG;
  }
  if (inst->unloadRequested || !inst->enabled) {
    error = "capability: instance is closing or disabled";
    return PLUGINXX_RESULT_ERR_STATE;
  }
  if (!checkRegistrable(capabilityId, phase, &error)) {
    return PLUGINXX_RESULT_ERR_NOT_FOUND;
  }

  auto &handlers = handlers_[capabilityId];
  dropHandlerSlot(handlers, inst->name, ownerTag, phase);

  CapabilityHandler h;
  h.plugin = inst->name;
  h.pluginId = pluginIdOf(inst->name);
  h.ownerTag = ownerTag;
  h.capabilityId = capabilityId;
  h.phase = phase;
  h.seq = ++handlerSeq_;
  h.rowStart = start;
  h.rowCancel = cancel;
  h.rowCtx = ctx;
  h.spec.priority = priority;
  handlers.push_back(h);

  XX_LOGI("[musicxx_ext] plugin `{}` registered `{}` via the capability table "
          "(phase={}, priority={}, handlers={})",
          inst->name, capabilityId, phaseNameOf(phase), priority, handlers.size());
  pushEvent("musicxx.capability.changed", h.pluginId,
            capabilityChangedPayloadJson(capabilityId, "register", h.pluginId));
  return PLUGINXX_RESULT_OK;
}

int32_t MusicxxHostManager::unregisterCapabilityHandler(
    MusicxxHostInstance *inst, std::string_view capabilityId,
    std::string_view ownerTag) {
  if (capabilityId.empty()) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  auto it = handlers_.find(std::string{capabilityId});
  if (it == handlers_.end()) {
    return PLUGINXX_RESULT_ERR_NOT_FOUND;
  }
  const std::string pluginName = inst ? inst->name : std::string{};
  const std::string tag{ownerTag};
  it->second.erase(std::remove_if(it->second.begin(), it->second.end(),
                                  [&](const CapabilityHandler &h) {
                                    return h.plugin == pluginName &&
                                           h.ownerTag == tag;
                                  }),
                   it->second.end());
  if (it->second.empty()) {
    handlers_.erase(it);
  }
  const std::string capabilityText{capabilityId};
  const std::string pluginId = inst ? pluginIdOf(inst->name) : std::string{};
  pushEvent("musicxx.capability.changed", pluginId,
            capabilityChangedPayloadJson(capabilityText, "unregister", pluginId));
  return PLUGINXX_RESULT_OK;
}

int MusicxxHostManager::onCapabilityRowAdded(
    MusicxxHostInstance *inst, const std::string &id,
    const PluginxxCapabilitySpec &spec, std::string *error) {
  // 只接预设表里有的 id；其余 id 是插件自己的普通能力，交给 pluginxx 派发
  CapabilityMeta meta;
  if (!presetMetaOf(id, &meta)) {
    return PLUGINXX_RESULT_OK;
  }
  std::string reason;
  // 处理器只有一种形态 (结束时调一次 notify->done): 不再区分同步 / 异步
  if (spec.run_start == nullptr) {
    reason = "capability `" + id + "` has no handler (run_start is null)";
    if (error) {
      *error = reason;
    }
    return PLUGINXX_RESULT_ERR_ARG;
  }
  int32_t phase = MUSICXX_CAPABILITY_PHASE_IMPL;
  switch (spec.phase) {
  case PLUGINXX_CAPABILITY_PHASE_BEFORE:
    phase = MUSICXX_CAPABILITY_PHASE_BEFORE;
    break;
  case PLUGINXX_CAPABILITY_PHASE_AFTER:
    phase = MUSICXX_CAPABILITY_PHASE_AFTER;
    break;
  default:
    phase = MUSICXX_CAPABILITY_PHASE_IMPL;
    break;
  }
  const std::string ownerTag = spec.owner_tag.data
                                   ? std::string{spec.owner_tag.data,
                                                 static_cast<size_t>(
                                                     spec.owner_tag.size)}
                                   : std::string{};
  const int32_t rc =
      registerCapabilityHandler(inst, id, phase, spec.priority, ownerTag,
                             spec.run_start, spec.cancel, spec.user_data,
                             reason);
  if (rc != PLUGINXX_RESULT_OK && error) {
    *error = reason;
  }
  return rc == PLUGINXX_RESULT_OK ? PLUGINXX_RESULT_OK
                                       : PLUGINXX_RESULT_ERR_ARG;
}

void MusicxxHostManager::onCapabilityRowRemoved(MusicxxHostInstance *inst,
                                               const std::string &id,
                                               std::string_view ownerTag) {
  CapabilityMeta meta;
  if (!presetMetaOf(id, &meta)) {
    return;
  }
  const std::string tag{ownerTag};
  unregisterCapabilityHandler(inst, id, tag);
}

bool MusicxxHostManager::appCapabilityDecl(std::string_view id,
                                           pluginxx::CapabilityDecl *out) {
  if (!out || id.empty()) {
    return false;
  }
  // 应用推过声明就以它为准 (推过来就是全部): 表里没有这个 id = 没声明。
  if (!appDecls_.empty()) {
    auto it = appDecls_.find(std::string{id});
    if (it == appDecls_.end()) {
      return false;
    }
    *out = it->second;
    return true;
  }
  const std::string text{id};
  const MusicxxCapabilityDecl *decl = musicxxCapabilityDeclOf(text.c_str());
  if (!decl) {
    return false;
  }
  out->id = text;
  out->impls = decl->impls == MUSICXX_CAPABILITY_IMPLS_MANY
                   ? pluginxx::CapabilityImpls_e::Many
                   : pluginxx::CapabilityImpls_e::One;
  if (decl->merge == MUSICXX_CAPABILITY_MERGE_FIRST) {
    out->merge = pluginxx::CapabilityMerge_e::First;
  } else if (decl->merge == MUSICXX_CAPABILITY_MERGE_VETO) {
    out->merge = pluginxx::CapabilityMerge_e::Veto;
  } else {
    out->merge = pluginxx::CapabilityMerge_e::None;
  }
  uint32_t phases = 0;
  if ((decl->phase_mask & MUSICXX_CAPABILITY_PHASE_MASK_IMPL) != 0) {
    phases |= pluginxx::kCapabilityPhaseMaskImpl;
  }
  if ((decl->phase_mask & MUSICXX_CAPABILITY_PHASE_MASK_BEFORE) != 0) {
    phases |= pluginxx::kCapabilityPhaseMaskBefore;
  }
  if ((decl->phase_mask & MUSICXX_CAPABILITY_PHASE_MASK_AFTER) != 0) {
    phases |= pluginxx::kCapabilityPhaseMaskAfter;
  }
  out->phases = phases == 0 ? pluginxx::kCapabilityPhaseMaskImpl : phases;
  out->timeoutMs = decl->timeout_ms;
  out->declaredBy = "";
  return true;
}

int32_t MusicxxHostManager::setAppCapabilityDecls(const std::string &json,
                                                    std::string &error) {
  Json doc;
  try {
    doc = Json::parse(json.empty() ? std::string{"[]"} : json);
  } catch (const std::exception &e) {
    error = std::string{"app capabilities: invalid json: "} + e.what();
    return PLUGINXX_RESULT_ERR_ARG;
  }
  if (!doc.is_array()) {
    error = "app capabilities: expected a JSON array";
    return PLUGINXX_RESULT_ERR_ARG;
  }
  std::map<std::string, pluginxx::CapabilityDecl, std::less<>> decls;
  for (const auto &item : doc) {
    if (!item.is_object()) {
      continue;
    }
    const std::string id = item.value("id", std::string{});
    if (id.empty()) {
      continue;
    }
    pluginxx::CapabilityDecl decl;
    decl.id    = id;
    decl.impls = item.value("impls", std::string{"one"}) == "many"
                     ? pluginxx::CapabilityImpls_e::Many
                     : pluginxx::CapabilityImpls_e::One;
    const std::string merge = item.value("merge", std::string{});
    if (merge == "first") {
      decl.merge = pluginxx::CapabilityMerge_e::First;
    } else if (merge == "veto") {
      decl.merge = pluginxx::CapabilityMerge_e::Veto;
    } else {
      decl.merge = pluginxx::CapabilityMerge_e::None;
    }
    uint32_t phases = 0;
    if (item.contains("phases") && item["phases"].is_array()) {
      for (const auto &phase : item["phases"]) {
        if (!phase.is_string()) {
          continue;
        }
        const std::string name = phase.get<std::string>();
        if (name == "before") {
          phases |= pluginxx::kCapabilityPhaseMaskBefore;
        } else if (name == "impl") {
          phases |= pluginxx::kCapabilityPhaseMaskImpl;
        } else if (name == "after") {
          phases |= pluginxx::kCapabilityPhaseMaskAfter;
        }
      }
    }
    decl.phases    = phases == 0 ? pluginxx::kCapabilityPhaseMaskImpl : phases;
    decl.timeoutMs = item.value("timeoutMs", int32_t{0});
    decls.emplace(id, std::move(decl));
  }
  appDecls_ = std::move(decls);
  XX_LOGI("[musicxx_ext] app capability declarations pushed: {}", appDecls_.size());
  // 已启动时立刻生效; start() 之前推的由 start 里那次声明带上
  declareAppCapabilities();
  return PLUGINXX_RESULT_OK;
}

void MusicxxHostManager::declareAppCapabilities() {
  auto caps = capabilities();
  if (!caps) {
    return;
  }
  // 应用推过就声明推来的那整份 (推的动作也会调到这里, 所以重推即覆盖);
  // 没推过时用自带表 (它是应用声明的内置副本)。
  if (!appDecls_.empty()) {
    for (const auto &[id, decl] : appDecls_) {
      std::string error;
      if (!caps->declare(decl, &error)) {
        XX_LOGW("[musicxx_ext] failed to declare capability `{}`: {}", id, error);
      }
    }
    return;
  }
  for (size_t i = 0; i < musicxxCapabilityDeclCount; ++i) {
    const MusicxxCapabilityDecl &entry = musicxxCapabilityDecls[i];
    if (!entry.id) {
      continue;
    }
    pluginxx::CapabilityDecl decl;
    decl.id = entry.id;
    decl.impls = entry.impls == MUSICXX_CAPABILITY_IMPLS_MANY
                     ? pluginxx::CapabilityImpls_e::Many
                     : pluginxx::CapabilityImpls_e::One;
    if (entry.merge == MUSICXX_CAPABILITY_MERGE_FIRST) {
      decl.merge = pluginxx::CapabilityMerge_e::First;
    } else if (entry.merge == MUSICXX_CAPABILITY_MERGE_VETO) {
      decl.merge = pluginxx::CapabilityMerge_e::Veto;
    } else {
      decl.merge = pluginxx::CapabilityMerge_e::None;
    }
    uint32_t phases = 0;
    if ((entry.phase_mask & MUSICXX_CAPABILITY_PHASE_MASK_IMPL) != 0) {
      phases |= pluginxx::kCapabilityPhaseMaskImpl;
    }
    if ((entry.phase_mask & MUSICXX_CAPABILITY_PHASE_MASK_BEFORE) != 0) {
      phases |= pluginxx::kCapabilityPhaseMaskBefore;
    }
    if ((entry.phase_mask & MUSICXX_CAPABILITY_PHASE_MASK_AFTER) != 0) {
      phases |= pluginxx::kCapabilityPhaseMaskAfter;
    }
    decl.phases = phases == 0 ? pluginxx::kCapabilityPhaseMaskImpl : phases;
    decl.timeoutMs = entry.timeout_ms;
    decl.declaredBy.clear(); ///< 应用声明
    std::string error;
    if (!caps->declare(decl, &error)) {
      XX_LOGW("[musicxx_ext] failed to declare capability `{}`: {}", entry.id, error);
    }
  }
}

void *MusicxxHostManager::onAppCapabilityStart(
    MusicxxHostInstance *inst, const std::string &capability,
    std::string_view argsJson, int32_t timeoutMs,
    const PluginxxOperatorNotify *notify, PluginxxString *errorOut) {
  if (!inst || !notify || !notify->done) {
    pluginxx::detail::setErrorString(
        errorOut, "app capability: missing instance or completion notify");
    return nullptr;
  }
  int64_t       requestId = 0;
  const uint32_t timeout =
      timeoutMs > 0 ? static_cast<uint32_t>(timeoutMs) : uint32_t{0};
  /// 插件框架给的完成通知是临时对象 (只在 start 调用期间有效): 存一份,
  /// 跟这次请求一起活到终结
  auto holder = std::make_shared<PluginxxOperatorNotify>(*notify);
  const int32_t rc = requestAppCapability(inst, capability, argsJson, timeout,
                                   holder.get(), &requestId);
  if (rc != PLUGINXX_RESULT_OK) {
    pluginxx::detail::setErrorString(
        errorOut, "app capability: request could not be queued");
    return nullptr;
  }
  auto it = pendingAppCapabilities_.find(requestId);
  if (it == pendingAppCapabilities_.end()) {
    pluginxx::detail::setErrorString(errorOut,
                                     "app capability: request is gone");
    return nullptr;
  }
  it->second.notifyOwner = std::move(holder);
  it->second.notify = it->second.notifyOwner.get();
  /// 句柄只用来取消: 能力调用号是宿主自增的 int64, 放得进指针
  return reinterpret_cast<void *>(static_cast<intptr_t>(requestId));
}

void MusicxxHostManager::onAppCapabilityCancel(MusicxxHostInstance * /*inst*/,
                                               const std::string & /*capability*/,
                                               void *handle) {
  if (!handle) {
    return;
  }
  cancelAppCapability(static_cast<int64_t>(reinterpret_cast<intptr_t>(handle)));
}

int32_t MusicxxHostManager::unimplement(MusicxxHostInstance *inst,
                                           std::string_view capabilityId,
                                           std::string_view ownerTag) {
  if (capabilityId.empty()) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  auto it = handlers_.find(std::string{capabilityId});
  if (it == handlers_.end()) {
    return PLUGINXX_RESULT_ERR_NOT_FOUND;
  }
  const std::string pluginName = inst ? inst->name : std::string{};
  const std::string tag{ownerTag};
  // 一个 (实例, ownerTag) 在所有阶段的登记一起摘掉 (注销函数不区分阶段)
  it->second.erase(std::remove_if(it->second.begin(), it->second.end(),
                                  [&](const CapabilityHandler &h) {
                                    return h.plugin == pluginName &&
                                           h.ownerTag == tag;
                                  }),
                   it->second.end());
  if (it->second.empty()) {
    handlers_.erase(it);
  }
  const std::string capabilityText{capabilityId};
  const std::string pluginId = inst ? pluginIdOf(inst->name) : std::string{};
  pushEvent("musicxx.capability.changed", pluginId,
            capabilityChangedPayloadJson(capabilityText, "unregister", pluginId));
  return PLUGINXX_RESULT_OK;
}

std::string MusicxxHostManager::capabilityChangedPayloadJson(
    const std::string &capabilityId, const char *action,
    const std::string &pluginId) {
  auto it = handlers_.find(capabilityId);
  const int32_t total =
      (it == handlers_.end()) ? 0 : static_cast<int32_t>(it->second.size());
  Json counts = Json::object();
  for (int32_t phase = MUSICXX_CAPABILITY_PHASE_IMPL;
       phase <= MUSICXX_CAPABILITY_PHASE_AFTER; ++phase) {
    counts[phaseNameOf(phase)] = countOnPhase(capabilityId, phase);
  }
  Json payload;
  payload["id"] = pluginId;
  payload["capability"] = capabilityId;
  payload["count"] = total;
  payload["phaseCount"] = counts;
  payload["action"] = action;
  // 一并带上处理器列表 (一条能力通常 0~3 个, 注册/注销是低频操作):
  // Dart 侧不用再回查插件框架就能知道谁注册了、排第几、在哪个阶段
  payload["handlers"] = jsonArrayOf(capabilityHandlersArrayJson(capabilityId));
  return payload.dump();
}

int32_t MusicxxHostManager::countOnPhase(const std::string &capabilityId,
                                         int32_t phase) const {
  auto it = handlers_.find(capabilityId);
  if (it == handlers_.end()) {
    return 0;
  }
  int32_t count = 0;
  for (const CapabilityHandler &h : it->second) {
    if (h.phase == phase) {
      ++count;
    }
  }
  return count;
}

int32_t MusicxxHostManager::capabilityInfoJson(std::string_view capabilityId,
                                               std::string &outJson) {
  if (capabilityId.empty()) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  const CapabilityMeta meta = metaOf(capabilityId);
  const std::string capabilityText{capabilityId};
  Json j;
  j["id"] = capabilityText;
  // 预设表里有没有这一条（没有也能用，只是属性是默认值）
  CapabilityMeta preset;
  j["preset"] = presetMetaOf(capabilityId, &preset) ? true : false;
  // 一个 id 要么是广播 (只通知), 要么是实现链 (按声明里的 merge 合并结果)
  j["broadcast"] = meta.notifyOnly;
  // 这条 id 声明了哪几个阶段 (注册时只能选这里有的阶段)
  Json phases = Json::array();
  for (int32_t phase = MUSICXX_CAPABILITY_PHASE_IMPL;
       phase <= MUSICXX_CAPABILITY_PHASE_AFTER; ++phase) {
    if ((meta.phases & phaseMaskOf(phase)) != 0) {
      phases.push_back(phaseNameOf(phase));
    }
  }
  j["phases"] = phases;
  auto it = handlers_.find(capabilityText);
  j["handlers"] =
      (it == handlers_.end()) ? 0 : static_cast<int32_t>(it->second.size());
  Json counts = Json::object();
  for (int32_t phase = MUSICXX_CAPABILITY_PHASE_IMPL;
       phase <= MUSICXX_CAPABILITY_PHASE_AFTER; ++phase) {
    counts[phaseNameOf(phase)] = countOnPhase(capabilityText, phase);
  }
  j["phaseCount"] = counts;
  outJson = j.dump();
  return PLUGINXX_RESULT_OK;
}

std::vector<std::string>
MusicxxHostManager::capabilitiesOfInstance(const std::string &instanceName) const {
  std::vector<std::string> ids;
  if (instanceName.empty()) {
    return ids;
  }
  std::set<std::string> seen;
  for (const auto &[capabilityId, handlers] : handlers_) {
    for (const CapabilityHandler &h : handlers) {
      if (h.plugin != instanceName) {
        continue;
      }
      /// 同一个能力上可能有多个处理器（例如 enabled/disabled 各注册一次）：
      /// 这里的列表是"该实例碰过哪些能力"，每个 id 只列一次
      if (seen.insert(capabilityId).second) {
        ids.push_back(capabilityId);
      }
      break;
    }
  }
  return ids;
}

int32_t MusicxxHostManager::capabilityStatsJson(std::string &outJson) {
  Json capabilities = Json::object();
  for (const auto &[capabilityId, handlers] : handlers_) {
    Json arr = Json::array();
    for (const auto &h : handlers) {
      arr.push_back({
          {"plugin", h.pluginId},
          {"ownerTag", h.ownerTag},
          {"phase", phaseNameOf(h.phase)},
          {"priority", h.spec.priority},
          {"calls", h.calls},
          {"failures", h.failures},
          {"avgUs", h.calls > 0 ? h.totalUs / h.calls : 0},
          {"maxUs", h.maxUs},
      });
    }
    capabilities[capabilityId] = arr;
  }
  Json j;
  j["capabilities"] = capabilities;
  outJson = j.dump();
  return PLUGINXX_RESULT_OK;
}

namespace {

/// 把内部拼好的 JSON 数组文本转成 Json
///
/// 这段文本由 `capabilityHandlersArrayJson` 自己构造，正常不会解析失败；真失败时退回空数组，
/// 不影响事件与自省接口的其它字段（这里用 try/catch 是因为本项目的 Json::parse 没有
/// "不抛异常" 的重载）。
Json jsonArrayOf(const std::string &text) {
  try {
    return Json::parse(text);
  } catch (...) {
    return Json::array();
  }
}

} // namespace

std::string MusicxxHostManager::capabilityHandlersArrayJson(std::string_view capabilityId,
                                                      int32_t phase) const {
  auto it = handlers_.find(std::string{capabilityId});
  if (it == handlers_.end() || it->second.empty()) {
    return "[]";
  }
  // 顺序必须与真实调用顺序一致 (前链正序: priority 升序, 同优先级按注册顺序),
  // 否则 Dart 侧自省里看到的"排第几"对不上。
  // `phase < 0` = 全部阶段 (按同一个正序排, 后链的逆序由调用方自己读 phase 判断)。
  std::vector<const CapabilityHandler *> ordered;
  ordered.reserve(it->second.size());
  for (const CapabilityHandler &h : it->second) {
    if (phase < 0 || h.phase == phase) {
      ordered.push_back(&h);
    }
  }
  if (ordered.empty()) {
    return "[]";
  }
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const CapabilityHandler *a, const CapabilityHandler *b) {
                     if (a->spec.priority != b->spec.priority) {
                       return a->spec.priority < b->spec.priority;
                     }
                     return a->seq < b->seq;
                   });
  Json arr = Json::array();
  for (const CapabilityHandler *h : ordered) {
    // JS 实例名是 "js:<id>", 其余按动态库插件 (与插件框架里的实例名一致)
    const bool js = h->plugin.rfind("js:", 0) == 0;
    arr.push_back({
        {"plugin", h->pluginId},
        {"instance", h->plugin},
        {"kind", js ? "js" : "native"},
        {"phase", phaseNameOf(h->phase)},
        {"priority", h->spec.priority},
        {"ownerTag", h->ownerTag},
    });
  }
  return arr.dump();
}

int32_t MusicxxHostManager::capabilityHandlersJson(std::string_view capabilityId,
                                             std::string &outJson) {
  return capabilityHandlersJsonPhase(capabilityId, -1, outJson);
}

int32_t MusicxxHostManager::capabilityHandlersJsonPhase(std::string_view capabilityId,
                                                  int32_t phase,
                                                  std::string &outJson) {
  if (capabilityId.empty()) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  if (phase < -1 || !isValidPhase(phase == -1 ? 0 : phase)) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  const std::string capabilityText{capabilityId};
  auto it = handlers_.find(capabilityText);
  Json j;
  j["capability"] = capabilityText;
  if (phase < 0) {
    j["count"] = (it == handlers_.end()) ? 0
                                      : static_cast<int32_t>(it->second.size());
  } else {
    j["phase"] = phaseNameOf(phase);
    j["count"] = countOnPhase(capabilityText, phase);
  }
  j["handlers"] = jsonArrayOf(capabilityHandlersArrayJson(capabilityText, phase));
  outJson = j.dump();
  return PLUGINXX_RESULT_OK;
}

/* ==================== 能力派发 ==================== */

namespace {

/// 浅合并 `patch` 子对象 (allMerge / anyCancel 的合并规则; 后面的键覆盖先前的键)
void mergePatch(Json &target, const Json &source) {
  if (!source.is_object()) {
    return;
  }
  const Json &patch = source.contains("patch") ? source["patch"] : source;
  if (!patch.is_object()) {
    return;
  }
  if (!target.is_object()) {
    target = Json::object();
  }
  for (auto it = patch.begin(); it != patch.end(); ++it) {
    target[it.key()] = it.value();
  }
}

/// 深合并 `patch` 子对象（前链：载荷逐步累积，嵌套对象递归合并）
void deepMergeInto(Json &target, const Json &source) {
  if (!source.is_object()) {
    return;
  }
  if (!target.is_object()) {
    target = Json::object();
  }
  for (auto it = source.begin(); it != source.end(); ++it) {
    if (it.value().is_object() && target.contains(it.key()) &&
        target[it.key()].is_object()) {
      deepMergeInto(target[it.key()], it.value());
      continue;
    }
    target[it.key()] = it.value();
  }
}

std::string actionOf(const Json &j) {
  if (j.is_object() && j.contains("action") && j["action"].is_string()) {
    return j["action"].get<std::string>();
  }
  return {};
}

/// 处理器完成通知的转发载体: 把 ABI 的 `notify->done` 变成协程侧的 [AsyncReply]
///
/// 处理器可能在任何线程完成 (网络回调 / JS 线程 / 定时器), 所以这里只把结果交给
/// `AsyncReply`, 唤醒由它投递回插件执行线程。
///
/// 生命周期: 两侧各记一个"我走了", 谁最后走谁释放。等待侧可能先走 (宿主收尾 /
/// 插件卸载把这条派发取消掉), 之后处理器才结算 —— 那时 `notify` 里的 `host_ud`
/// 仍然是这里, 所以不能随等待侧一起释放。
struct HandlerReplyBridge {
  using Reply = pluginxx::AsyncReply<std::pair<int32_t, std::string>>;

  std::mutex mutex_;
  std::shared_ptr<Reply> reply;
  std::atomic<bool> completed{false}; ///< 完成通知只认第一次
  bool waiterGone = false;
  bool completerGone = false;

  /// 完成侧取结果槽 (等待侧已经走了给空)
  std::shared_ptr<Reply> take() {
    std::lock_guard<std::mutex> lock{mutex_};
    return reply;
  }

  /// 等待侧离开: 放掉结果槽, 处理器之后调 `notify->done` 就是空操作
  void waiterLeave() {
    bool freeNow = false;
    {
      std::lock_guard<std::mutex> lock{mutex_};
      reply.reset();
      waiterGone = true;
      freeNow = completerGone;
    }
    if (freeNow) {
      delete this;
    }
  }

  /// 完成侧离开: 等待侧已经走了就立即释放
  void completerLeave() {
    bool freeNow = false;
    {
      std::lock_guard<std::mutex> lock{mutex_};
      completerGone = true;
      freeNow = waiterGone;
    }
    if (freeNow) {
      delete this;
    }
  }
};

/// 处理器的 `notify->done` (任意线程; 只认第一次)
void PLUGINXX_CALL handlerReplyTrampoline(
    void *ud, int32_t status, const PluginxxStringView *payload) noexcept {
  auto *bridge = static_cast<HandlerReplyBridge *>(ud);
  if (!bridge || bridge->completed.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  std::pair<int32_t, std::string> answer{status, std::string{}};
  if (payload && payload->data && payload->size > 0) {
    answer.second.assign(payload->data, static_cast<size_t>(payload->size));
  }
  if (auto reply = bridge->take()) {
    reply->set(std::move(answer));
  }
  bridge->completerLeave();
}

} // namespace

std::vector<MusicxxHostManager::CapabilityHandler>
MusicxxHostManager::handlerSnapshot(const std::string &capabilityId,
                                    int32_t phase) {
  std::vector<CapabilityHandler> handlers;
  auto it = handlers_.find(capabilityId);
  if (it == handlers_.end()) {
    return handlers;
  }
  handlers.reserve(it->second.size());
  for (const CapabilityHandler &h : it->second) {
    if (h.phase == phase) {
      handlers.push_back(h);
    }
  }
  if (handlers.empty()) {
    return handlers;
  }
  std::stable_sort(handlers.begin(), handlers.end(),
                   [](const CapabilityHandler &a, const CapabilityHandler &b) {
                     if (a.spec.priority != b.spec.priority) {
                       return a.spec.priority < b.spec.priority;
                     }
                     return a.seq < b.seq;
                   });
  if (phase == MUSICXX_CAPABILITY_PHASE_AFTER) {
    /// 后链**逆序** (线尾 → 线头): priority 降序, 同优先级注册顺序也反。
    /// 这样外置插件最后改输出, 应用自己的兜底实现不能盖掉插件的改写
    /// (与实现链里"外置优先"同向)。
    std::reverse(handlers.begin(), handlers.end());
  }
  return handlers;
}

void MusicxxHostManager::applyHandlerStats(const std::string &capabilityId,
                                           const std::vector<HandlerStat> &stats) {
  for (const HandlerStat &stat : stats) {
    if (auto instIt = loaded_.find(stat.plugin); instIt != loaded_.end() &&
                                                  instIt->second) {
      instIt->second->capabilityCalls.fetch_add(1);
    }
    /// 统计写回**活表**: 处理器跑的时候插件可能已经把自己的处理器注销掉 (快照里还有),
    /// 这时丢弃本次统计, 避免统计与注册表不一致
    auto it = handlers_.find(capabilityId);
    if (it == handlers_.end()) {
      continue;
    }
    for (auto &candidate : it->second) {
      if (candidate.plugin != stat.plugin ||
          candidate.ownerTag != stat.ownerTag ||
          candidate.phase != stat.phase) {
        continue;
      }
      ++candidate.calls;
      candidate.totalUs += stat.us;
      candidate.maxUs = (std::max)(candidate.maxUs, stat.us);
      if (stat.failed) {
        ++candidate.failures;
      }
      break;
    }
  }
}

asio::awaitable<std::string>
MusicxxHostManager::invokeHandler(const CapabilityHandler &handler,
                                  const std::string &payloadText,
                                  HandlerStat &stat) {
  stat.plugin = handler.plugin;
  stat.ownerTag = handler.ownerTag;
  stat.phase = handler.phase;
  stat.us = 0;
  stat.failed = false;

  /// 处理器来源两处: `musicxx.capabilities` 表登记的 (rowStart) 与宿主内部登记的
  /// (JS 引擎, 走 `spec.run_start`) —— 两者签名相同, 派发只有一条路径
  PluginxxCapabilityStartFunction start =
      handler.rowStart ? handler.rowStart : handler.spec.run_start;
  PluginxxOperatorCancelFunction cancel =
      handler.rowStart ? handler.rowCancel : handler.spec.run_cancel;
  void *ctx = handler.rowStart ? handler.rowCtx : handler.spec.user_data;
  if (!start) {
    co_return std::string{};
  }

  const auto executor = co_await asio::this_coro::executor;
  auto reply =
      pluginxx::AsyncReply<std::pair<int32_t, std::string>>::create(executor);
  /// 载体自己管生命周期 (见 [HandlerReplyBridge]): 等待侧离开时由下面的守卫通知它
  auto *bridge = new HandlerReplyBridge{};
  bridge->reply = reply;
  struct BridgeGuard {
    HandlerReplyBridge *bridge;
    ~BridgeGuard() {
      if (bridge) {
        bridge->waiterLeave();
      }
    }
  } bridgeGuard{bridge};

  const PluginxxStringView methodView{handler.capabilityId.data(),
                                      handler.capabilityId.size()};
  const PluginxxStringView argsView{payloadText.data(), payloadText.size()};
  const PluginxxOperatorNotify notify{&handlerReplyTrampoline, bridge};
  PluginxxString err{};
  void *op = nullptr;
  std::string failure;
  const auto t0 = std::chrono::steady_clock::now();
  try {
    op = start(ctx, nullptr, &methodView, &argsView, &notify, &err);
  } catch (const std::exception &e) {
    failure = e.what();
  } catch (...) {
    failure = "unknown exception";
  }
  if (err.data) {
    if (failure.empty()) {
      failure.assign(err.data, static_cast<size_t>(err.size));
    }
    pluginxx::hostMemoryFree(err.data);
  }
  if (!failure.empty()) {
    /// 处理器起不来: 撤掉它可能留下的句柄, 按"这一步失败"收尾 (不影响其它处理器)
    if (op && cancel) {
      cancel(ctx, op);
    }
    reply->cancel();
  }

  const auto answer = co_await reply->wait();
  std::string outText;
  if (!failure.empty()) {
    stat.failed = true;
  } else if (answer.first == PLUGINXX_OPERATOR_OK) {
    outText = answer.second;
  } else {
    /// 失败与取消都只记统计 (不因为失败次数暂停派发): 这一轮按"没有意见"处理
    stat.failed = true;
    if (answer.first == PLUGINXX_OPERATOR_FAILED && !answer.second.empty()) {
      XX_LOGW("[musicxx_ext] handler `{}` for `{}` failed: {}", handler.pluginId,
              handler.capabilityId, answer.second);
    }
  }

  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
  /// 说明: `us` 的类型是 chrono 的 rep (libc++/Android 上是 long long, 而 int64_t 是
  /// long), 直接交给 std::max 会因模板推导出两种类型而编译失败, 统一转成 int64_t
  stat.us = static_cast<int64_t>(us);
  co_return outText;
}

asio::awaitable<std::string> MusicxxHostManager::runCapabilityHandlers(
    std::vector<CapabilityHandler> handlers, const std::string &capabilityId,
    int32_t phase, std::string inputJson, bool notifyOnly,
    std::vector<HandlerStat> &stats) {
  stats.clear();
  stats.reserve(handlers.size());
  if (handlers.empty()) {
    co_return R"({"handled":false,"handlers":0})";
  }
  const CapabilityMeta meta = metaOf(capabilityId);

  /// 调一个处理器并把统计收进本轮结果
  ///
  /// 返回它的回答文本 (空 = 没有意见)。处理器按操作契约完成: 可能在返回前调完
  /// `notify->done`, 也可能稍后在别的线程调 —— 都由 [AsyncReply] 接住。
  auto step = [&](const CapabilityHandler &h, const std::string &payload,
                  std::string &outText) -> asio::awaitable<void> {
    HandlerStat stat;
    outText = co_await invokeHandler(h, payload, stat);
    stats.push_back(std::move(stat));
  };

  /// 通知型能力派发的调试回传 (仅调试开关打开时回传; 默认关闭 → 零开销)
  auto emitNotifyDebugEvent = [&](int32_t called) {
    if (notifyOnly &&
        (flags_ & MUSICXX_EXTERN_PLUGIN_FLAG_DEBUG_OBSERVE_EVENTS) != 0) {
      Json debugEvent;
      debugEvent["capability"] = capabilityId;
      debugEvent["phase"] = phaseNameOf(phase);
      debugEvent["input"] = parseJsonSafe(inputJson);
      debugEvent["called"] = called;
      debugEvent["handlers"] = static_cast<int32_t>(handlers.size());
      pushEvent("musicxx.capability.observe", "", debugEvent.dump());
    }
  };

  int32_t called = 0;

  /* ---------- 前链: 串行、正序, 载荷逐步累积 ---------- */
  if (phase == MUSICXX_CAPABILITY_PHASE_BEFORE) {
    Json current = parseJsonSafe(inputJson);
    if (!current.is_object()) {
      current = Json::object();
    }
    Json accumulated = Json::object();
    bool canceled = false;
    for (const CapabilityHandler &h : handlers) {
      std::string outText;
      co_await step(h, current.dump(), outText);
      ++called;
      if (outText.empty()) {
        continue;
      }
      Json answer = parseJsonSafe(outText);
      if (!answer.is_object()) {
        continue;
      }
      if (answer.contains("patch")) {
        // 只认 patch 里的键: 逐步合并进本轮载荷, 同时记进累计 patch 交回调用方
        deepMergeInto(current, answer["patch"]);
        deepMergeInto(accumulated, answer["patch"]);
      }
      if (actionOf(answer) == MUSICXX_PLUGIN_CAPABILITY_ACTION_CANCEL) {
        canceled = true;
        break;
      }
    }
    Json result;
    result["handled"] = called > 0 && !notifyOnly;
    result["handlers"] = static_cast<int32_t>(handlers.size());
    result["called"] = called;
    if (canceled || !accumulated.empty()) {
      Json res = Json::object();
      if (!accumulated.empty()) {
        res["patch"] = accumulated;
      }
      if (canceled) {
        res["action"] = MUSICXX_PLUGIN_CAPABILITY_ACTION_CANCEL;
      }
      result["result"] = res;
    }
    emitNotifyDebugEvent(called);
    co_return result.dump();
  }

  /* ---------- 后链: 串行、逆序, 值逐步替换 ---------- */
  if (phase == MUSICXX_CAPABILITY_PHASE_AFTER) {
    Json payload = parseJsonSafe(inputJson);
    if (!payload.is_object()) {
      payload = Json::object();
    }
    Json value = payload.contains("value") ? payload["value"] : Json(nullptr);
    bool dropped = false;
    for (const CapabilityHandler &h : handlers) {
      Json step_payload = payload;
      step_payload["value"] = value;
      std::string outText;
      co_await step(h, step_payload.dump(), outText);
      ++called;
      if (outText.empty()) {
        continue;
      }
      Json answer = parseJsonSafe(outText);
      if (!answer.is_object()) {
        continue;
      }
      // `value` 键存在就算答了 (值可以是 null: "算过了、结果是空")
      if (answer.contains("value")) {
        value = answer["value"];
      }
      if (actionOf(answer) == MUSICXX_PLUGIN_CAPABILITY_ACTION_DROP) {
        dropped = true;
        break;
      }
    }
    Json result;
    result["handled"] = called > 0 && !notifyOnly;
    result["handlers"] = static_cast<int32_t>(handlers.size());
    result["called"] = called;
    Json res = Json::object();
    res["value"] = value;
    if (dropped) {
      res["action"] = MUSICXX_PLUGIN_CAPABILITY_ACTION_DROP;
    }
    result["result"] = res;
    emitNotifyDebugEvent(called);
    co_return result.dump();
  }

  /* ---------- 实现链: 按声明的合并策略处理 (first / veto 可提前结束) ---------- */
  Json merged = Json::object();
  bool hasPatch = false;
  std::string action;

  for (const CapabilityHandler &h : handlers) {
    std::string outText;
    co_await step(h, inputJson, outText);
    ++called;
    if (outText.empty()) {
      continue;
    }
    Json result = parseJsonSafe(outText);
    if (!result.is_object()) {
      continue;
    }
    if (result.contains("patch")) {
      mergePatch(merged, result);
      hasPatch = true;
    }
    const std::string a = actionOf(result);
    if (!a.empty() &&
        (action.empty() || action == MUSICXX_PLUGIN_CAPABILITY_ACTION_CONTINUE)) {
      action = a;
    }
    for (auto pit = result.begin(); pit != result.end(); ++pit) {
      if (pit.key() != std::string{"patch"}) {
        merged[pit.key()] = pit.value();
      }
    }

    // `cancel` = 拦下这次调用: 后面谁都不再问 (与合并策略无关)
    if (a == MUSICXX_PLUGIN_CAPABILITY_ACTION_CANCEL) {
      break;
    }
    // 合并策略来自声明: first = 第一个合法结果生效; veto = 任一否决即停
    if (meta.merge == MUSICXX_CAPABILITY_MERGE_FIRST &&
        (hasPatch || !action.empty())) {
      break;
    }
    if (meta.merge == MUSICXX_CAPABILITY_MERGE_VETO &&
        action == MUSICXX_PLUGIN_CAPABILITY_ACTION_SKIP) {
      break;
    }
  }

  Json result;
  result["handled"] = called > 0 && !notifyOnly;
  /// handlers = 本轮快照里的处理器数量 (处理器里注册/注销不影响本轮的计数)
  result["handlers"] = static_cast<int32_t>(handlers.size());
  result["called"] = called;
  if (!action.empty()) {
    result["result"] = merged;
    result["result"]["action"] = action;
  } else if (hasPatch) {
    result["result"] = merged;
  }

  /// 调试回传: 通知型能力的派发过程 (仅调试开关打开时回传)
  emitNotifyDebugEvent(called);
  co_return result.dump();
}

int32_t MusicxxHostManager::dispatchCapability(const std::string &capabilityId,
                                               const std::string &inputJson,
                                               int64_t *outRequestId,
                                               std::string &err) {
  return dispatchCapabilityPhase(capabilityId, MUSICXX_CAPABILITY_PHASE_IMPL,
                                 inputJson, outRequestId, err);
}

int32_t MusicxxHostManager::dispatchCapabilityPhase(
    const std::string &capabilityId, int32_t phase, const std::string &inputJson,
    int64_t *outRequestId, std::string &err) {
  if (capabilityId.empty()) {
    err = "capability dispatch: empty id";
    return PLUGINXX_RESULT_ERR_ARG;
  }
  if (!isValidPhase(phase)) {
    err = "capability dispatch: invalid phase";
    return PLUGINXX_RESULT_ERR_ARG;
  }
  if (!running_.load(std::memory_order_acquire)) {
    err = "capability dispatch: host not started";
    return PLUGINXX_RESULT_ERR_STATE;
  }

  /// 受理只记账与取声明 (**不碰处理器表** —— 它归框架 IO 线程)
  int64_t requestId = 0;
  const int32_t acceptRc = acceptRequest("capability.dispatch", &requestId);
  if (acceptRc != PLUGINXX_RESULT_OK) {
    err = "capability dispatch: event queue is full, request rejected";
    return acceptRc;
  }

  const bool notifyOnly = metaOf(capabilityId).notifyOnly;
  auto self = shared_from_this();
  asio::post(ctx_->io, [self, requestId, capabilityId, phase, inputJson,
                        notifyOnly]() {
    /// 快照在框架线程上取: 派发期间插件注册/注销不影响本轮遍历
    auto snapshot = self->handlerSnapshot(capabilityId, phase);
    if (snapshot.empty()) {
      self->completeRequest(
          requestId, RequestOutcome::ok(R"({"handled":false,"handlers":0})"));
      return;
    }
    /// 处理器链在**插件执行线程**上跑: 慢处理器只占住插件线程, 框架 IO 线程照常
    /// 处理注册表 / 事件 / 其它请求
    auto stats = std::make_shared<std::vector<HandlerStat>>();
    asio::co_spawn(
        pluginxx::pluginExecutionExecutor(self->runtime()),
        [self, requestId, capabilityId, phase, inputJson, notifyOnly, snapshot,
         stats]() -> asio::awaitable<void> {
          std::string result;
          try {
            result = co_await self->runCapabilityHandlers(
                snapshot, capabilityId, phase, inputJson, notifyOnly, *stats);
          } catch (const std::exception &e) {
            const std::string message = e.what();
            asio::post(self->hostExecutor(), [self, requestId, message] {
              self->completeRequest(
                  requestId, RequestOutcome::fail(PLUGINXX_RESULT_ERR_INTERNAL,
                                                  message));
            });
            co_return;
          } catch (...) {
            asio::post(self->hostExecutor(), [self, requestId] {
              self->completeRequest(requestId,
                                    RequestOutcome::fail(
                                        PLUGINXX_RESULT_ERR_INTERNAL,
                                        "handler chain ended with an exception"));
            });
            co_return;
          }
          asio::post(self->hostExecutor(),
                     [self, requestId, capabilityId, result, stats]() {
                       self->applyHandlerStats(capabilityId, *stats);
                       self->completeRequest(requestId,
                                             RequestOutcome::ok(result));
                     });
        },
        asio::detached);
  });

  if (outRequestId) {
    *outRequestId = requestId;
  }
  return PLUGINXX_RESULT_OK;
}
/* ==================== 能力调用 (插件 → Dart) ==================== */

int32_t MusicxxHostManager::requestAppCapability(MusicxxHostInstance *inst,
                                          std::string_view action,
                                          std::string_view argsJson,
                                          uint32_t timeoutMs,
                                          const PluginxxOperatorNotify *notify,
                                          int64_t *outRequestId) {
  if (!inst || action.empty()) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  const std::string capabilityName{action};
  const std::string pluginId = pluginIdOf(inst->name);
  /// 名字不校验、不拒绝: 插件想调什么就调什么, 由 Dart 侧按"注册了没有"决定
  /// 理不理它 (没注册的名称立刻回"未找到")。
  /// 命名建议: 官方能力 `musicxx.<域>.<名字>`; 插件自己的力量用 `plugin.<包名>.*`
  /// 以免与别的插件撞名。

  const int64_t requestId = nextRequestId_++;
  /// 超时: 调用方与**应用声明**取最小的正数 (0 = 这一处不限制; 都缺省就一直等)。
  /// 应用内部的调用点不再写死默认值, 确实需要上限的由调用方自己传。
  const int64_t effectiveTimeoutMs = minPositiveTimeout(
      static_cast<int64_t>(timeoutMs), declaredTimeoutOf(capabilityName));
  PendingAppCapability pending;
  pending.plugin = inst->name;
  pending.capability = capabilityName;
  pending.notify = notify;
  pending.deadline = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds{effectiveTimeoutMs};
  // 超时保护: 到点仍未收到 Dart 回复就终结 op, 并推事件让 Dart 中断在做的活
  // (只有调用方给了超时才建定时器)
  if (ctx_ && effectiveTimeoutMs > 0) {
    auto timer = std::make_shared<asio::steady_timer>(ctx_->io);
    timer->expires_after(std::chrono::milliseconds{effectiveTimeoutMs});
    timer->async_wait([this, requestId](const utilxx_base::AsioErrorCode &ec) {
      if (ec) {
        return; ///< 已回复/已取消: 定时器被主动取消
      }
      auto it = pendingAppCapabilities_.find(requestId);
      if (it == pendingAppCapabilities_.end()) {
        return;
      }
      const std::string pluginId = pluginIdOf(it->second.plugin);
      if (it->second.notify && it->second.notify->done) {
        it->second.notify->done(it->second.notify->host_ud,
                                PLUGINXX_OPERATOR_CANCELED, nullptr);
      }
      XX_LOGW("[musicxx_ext] capability call timed out: plugin={} action={} id={}", pluginId,
              it->second.capability, requestId);
      Json payload;
      payload["requestId"] = requestId;
      payload["reason"] = "timeout";
      pushEvent("musicxx.capability.app.cancel", pluginId, payload.dump());
      pendingAppCapabilities_.erase(it);
    });
    pending.timer = std::move(timer);
  }
  pendingAppCapabilities_[requestId] = std::move(pending);
  if (outRequestId) {
    *outRequestId = requestId;
  }
  inst->appCapabilityRequests.fetch_add(1);

  /// 插件框架自己的网络能力与两条能力同名。这两条按**框架原生实现**回答
  /// (与 JS `musicxx.net.fetch` / 动态库插件的 `iface.net` 同一份实现), 不再转给
  /// Dart —— 否则同名接口会有两套行为 (代理设置、TLS 选项、下载落盘位置都不一样)。
  /// 跑在框架的工作线程上, 不占插件与主程序的线程。
  if (capabilityName == "musicxx.net.fetch" ||
      capabilityName == "musicxx.net.download") {
    if (ctx_ && ctx_->workers) {
      const std::string op =
          capabilityName == "musicxx.net.fetch" ? "http.fetch"
                                                                : "http.download";
      /// 与 JS 封装一致: `body` 给对象 / 数组时按 JSON 发送 (框架的 `http.fetch`
      /// 只认字符串 `body` 或 `json` 字段)。
      utilxx_base::Json netArgs = parseJsonSafe(argsJson);
      if (netArgs.is_object() && netArgs.contains("body") &&
          !netArgs.contains("json")) {
        const auto body = netArgs.find("body");
        if (body != netArgs.end() && !body->is_string()) {
          netArgs["json"] = *body;
          netArgs.erase("body");
        }
      }
      const std::string args = netArgs.dump();
      auto self = shared_from_this();
      asio::post(*ctx_->workers, [self, requestId, pluginId, op, args]() {
        std::string out;
        const int32_t rc = self->netCall(pluginId, op, args, out);
        std::string result = out;
        if (rc != PLUGINXX_RESULT_OK) {
          utilxx_base::Json error;
          error["ok"] = false;
          error["error"] = "net_call_failed";
          error["message"] =
              out.empty() ? "plugin network capability is unavailable" : std::move(out);
          result = error.dump();
        }
        /// 传输失败也按"请求完成"回答 (status 0, 结论在结果 JSON 的 ok 里),
        /// 与 `musicxx.net.fetch` 的约定一致。
        self->respondAppCapability(requestId, 0, result);
      });
      return PLUGINXX_RESULT_OK;
    }
    /// 没有工作线程 (未启动) 时继续走 Dart 侧, 由它回一个错误
  }

  Json payload;
  payload["requestId"] = requestId;
  payload["plugin"] = pluginId;
  payload["capability"] = capabilityName;
  payload["timeoutMs"] = effectiveTimeoutMs;
  Json args = parseJsonSafe(argsJson);
  payload["args"] = args;
  pushEvent("musicxx.capability.app.request", pluginId, payload.dump());
  return PLUGINXX_RESULT_OK;
}

int32_t MusicxxHostManager::respondAppCapability(int64_t requestId, int32_t status,
                                          const std::string &resultJson) {
  /// 先问 JS 引擎的接驳口: 它用自己的表登记未回复的请求, 自带互斥, **任何线程**
  /// 都能就地回答。顺序不能反过来 —— 这条回复往往是由"正在 IO 线程上等 JS"的
  /// 能力派发 (见 JsEngine::implementationRunSync) 触发的, 先投 IO 线程就会两边互等。
  {
    std::shared_ptr<InternalAppCapabilityRelay> relay;
    {
      std::lock_guard<std::mutex> lock{registryMutex_};
      relay = internalCapabilityRelay_;
    }
    if (relay && relay->respondAppCapability(requestId, status, resultJson)) {
      return PLUGINXX_RESULT_OK;
    }
  }
  /// 动态库插件的请求: 登记表归 IO 线程 (调用方已在 IO 线程时就地执行)
  return pluginxx::ioCallSync<int32_t>(
      this, [this, requestId, status, resultJson]() -> int32_t {
        auto it = pendingAppCapabilities_.find(requestId);
        if (it == pendingAppCapabilities_.end()) {
          return PLUGINXX_RESULT_ERR_NOT_FOUND;
        }
        if (it->second.timer) {
          it->second.timer->cancel(); ///< 已回复: 撤销超时保护
        }
        if (it->second.notify && it->second.notify->done) {
          PluginxxString payload{};
          if (status == 0 && !resultJson.empty()) {
            payload = pluginxx::hostMemoryCreateString(
                std::string_view{resultJson});
          }
          const PluginxxStringView view{payload.data, payload.size};
          it->second.notify->done(it->second.notify->host_ud, status, &view);
          if (payload.data) {
            pluginxx::hostMemoryFree(payload.data);
          }
        }
        pendingAppCapabilities_.erase(it);
        return PLUGINXX_RESULT_OK;
      });
}

void MusicxxHostManager::cancelAppCapability(int64_t requestId) {
  auto it = pendingAppCapabilities_.find(requestId);
  if (it == pendingAppCapabilities_.end()) {
    return;
  }
  if (it->second.timer) {
    it->second.timer->cancel();
  }
  if (it->second.notify && it->second.notify->done) {
    it->second.notify->done(it->second.notify->host_ud,
                            PLUGINXX_OPERATOR_CANCELED, nullptr);
  }
  Json payload;
  payload["requestId"] = requestId;
  pushEvent("musicxx.capability.app.cancel", pluginIdOf(it->second.plugin),
            payload.dump());
  pendingAppCapabilities_.erase(it);
}

int32_t MusicxxHostManager::pluginConfigPathFor(MusicxxHostInstance *inst,
                                                std::string &out) {
  if (!inst) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  if (!inst->configPath.empty()) {
    out = inst->configPath;
    return PLUGINXX_RESULT_OK;
  }
  out = deriveConfigPath(pluginIdOf(inst->name), {});
  return out.empty() ? PLUGINXX_RESULT_ERR_NOT_FOUND
                     : PLUGINXX_RESULT_OK;
}

int32_t MusicxxHostManager::hostInfoJson(std::string &outJson,
                                         std::string_view instanceName,
                                         std::string_view kind) {
  Json j;
  j["appVersion"] = appVersion_;
  j["platform"] = platform_;
  j["language"] = language_;
  j["dataDir"] = dataDir_;
  j["userPluginDir"] = userPluginDir_;
  j["builtinPluginDir"] = builtinPluginDir_;
  // 接口代次按**调用方自己的形态**报：动态库插件看到 C 接口代次（3），JS 插件看到 JS 接口代次
  // （2）—— 两套编号独立，插件据此判断自己能用哪些接口（见 host_kind.h）。
  // JS 引擎给所有 JS 插件共用一份缓存，它显式传 kind="js"。
  std::string_view callerKind = kind;
  if (callerKind.empty() && !instanceName.empty()) {
    if (auto inst = find(instanceName)) {
      callerKind = inst->kind;
    }
  }
  j["apiVersion"] = callerKind.empty()
                         ? MUSICXX_EXTERN_PLUGIN_MIN_API_VERSION
                         : kindRegistry_.interfaceVersion(callerKind);
  j["hostVersion"] = MUSICXX_EXTERN_PLUGIN_VERSION;
  // 调用方插件自己的 id: 插件要判断"这次调用是不是本插件自己发起的"就得知道自己的 id
  // (功能埋点糖用它过滤自己发起的调用)。主程序读同一份信息时不带这个字段。
  if (!instanceName.empty()) {
    j["pluginId"] = pluginIdOf(instanceName);
  }
  // 客户端界面能力段 (由界面描述层库生成, 插件框架原样转发; 没配置时不出现这个字段)
  if (!uiCapabilitiesJson_.empty()) {
    j["ui"] = parseJsonSafe(uiCapabilitiesJson_);
  }
  outJson = j.dump();
  return PLUGINXX_RESULT_OK;
}

/* ==================== 专用接口表 (C ABI 函数 + vtable) ==================== */

namespace {

PluginxxOperatorHandle *PLUGINXX_CALL xx_host_get_info(
    const PluginxxHost *host, PluginxxOperatorCallback cb, void *cbUd,
    PluginxxString *errorOut) {
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host,
                                                                        true);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(errorOut, "get_info: host is unavailable");
    return nullptr;
  }
  auto mgr = call.manager();
  auto inst = call.instance();
  const std::string name = inst ? inst->name : std::string{};
  return pluginxx::startTableOp(
      mgr, cb, cbUd, errorOut, "musicxx.host.get_info",
      [call, name]() -> std::string {
        std::string json;
        const int32_t rc = call.manager()->hostInfoJson(json, name);
        if (rc != PLUGINXX_RESULT_OK) {
          throw pluginxx::TableOpError("get_info: host info is unavailable");
        }
        return json;
      });
}

PluginxxOperatorHandle *PLUGINXX_CALL xx_host_config_path(
    const PluginxxHost *host, PluginxxOperatorCallback cb, void *cbUd,
    PluginxxString *errorOut) {
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host,
                                                                        true);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(errorOut,
                                     "get_plugin_config_path: host is unavailable");
    return nullptr;
  }
  return pluginxx::startTableOp(
      call.manager(), cb, cbUd, errorOut, "musicxx.host.get_plugin_config_path",
      [call]() -> std::string {
        std::string path;
        const int32_t rc =
            call.manager()->pluginConfigPathFor(call.instance(), path);
        if (rc != PLUGINXX_RESULT_OK) {
          throw pluginxx::TableOpError(
              "get_plugin_config_path: path is unavailable");
        }
        return path;
      });
}

const MusicxxPluginHostIface g_ifaceHost = {
    /* version */ MUSICXX_PLUGIN_IFACE_HOST_VERSION,
    /* struct_size */ sizeof(MusicxxPluginHostIface),
    /* get_info */ xx_host_get_info,
    /* get_plugin_config_path */ xx_host_config_path,
};

void *PLUGINXX_CALL xx_alloc_fn(uint64_t size) {
  return pluginxx::hostMemoryAlloc(size);
}

void PLUGINXX_CALL xx_free_fn(void *ptr) { pluginxx::hostMemoryFree(ptr); }

/* ==================== 广播表 (自定义入口: 增加 id 归属校验与频道补齐) ==================== */

/// 订阅句柄的完成载荷形态: 十进制地址文本 (插件原样交回 unsubscribe_broadcast)
std::string subscriptionPayloadOf(PluginxxSubscription *sub) {
  return std::to_string(static_cast<uint64_t>(
      reinterpret_cast<uintptr_t>(sub)));
}

/// 声明频道 (任意线程; 名字必须是官方或本插件命名空间)
PluginxxOperatorHandle *PLUGINXX_CALL xx_declare_broadcast(
    const PluginxxHost *host, const PluginxxStringView *declJson,
    PluginxxOperatorCallback cb, void *cbUd, PluginxxString *errorOut) {
  if (!declJson || !declJson->data) {
    pluginxx::detail::setErrorString(errorOut,
                                     "declare_broadcast: missing declaration");
    return nullptr;
  }
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(errorOut,
                                     "declare_broadcast: host is unavailable");
    return nullptr;
  }
  const std::string text{declJson->data, static_cast<size_t>(declJson->size)};
  return pluginxx::startTableOp(
      call.manager(), cb, cbUd, errorOut, "musicxx.broadcasts.declare",
      [call, text]() -> std::string {
        std::string error;
        const int32_t rc =
            call.manager()->declareBroadcast(call.instance(), text, &error);
        if (rc != PLUGINXX_RESULT_OK) {
          throw pluginxx::TableOpError(
              error.empty() ? "declare_broadcast: rejected" : error);
        }
        return std::string{};
      });
}

/// 订阅广播的共同实现 (任意线程; 主题形状与归属规则与旧事件表一致)
///
/// `options_json` 是订阅方自己的通知频率 (`{"intervalMs":200,"mode":"delay"}`; 可空)。
/// `subscribe_broadcast` 与 `subscribe_broadcast_ex` 只差 `onEnd`:
/// 频道被属主结束时用 reason JSON 回调一次, 随后这个订阅被摘掉。
PluginxxOperatorHandle *subscribeBroadcastFromTable(
    const PluginxxHost *host, const PluginxxScopeRef *scope,
    const PluginxxStringView *id,
    void(PLUGINXX_CALL *handler)(const PluginxxStringView *payloadJson,
                                 void *ud),
    void(PLUGINXX_CALL *onEnd)(const PluginxxStringView *reasonJson, void *ud),
    void *ud, const PluginxxStringView *optionsJson, PluginxxOperatorCallback cb,
    void *cbUd, PluginxxString *errorOut) {
  if (!id || !id->data || !handler) {
    pluginxx::detail::setErrorString(
        errorOut, "subscribe_broadcast: missing id or handler");
    return nullptr;
  }
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(errorOut,
                                     "subscribe_broadcast: host is unavailable");
    return nullptr;
  }
  auto mgr = call.manager();
  const std::string idText{id->data, static_cast<size_t>(id->size)};
  // 订阅只要求 id 形状合法 (可以订阅其它插件或官方频道); 发布才做归属校验
  if (mgr->qualifyEventTopic(idText).empty()) {
    XX_LOGW("[musicxx_ext] subscribe to an illegal broadcast by plugin `{}` rejected: `{}`",
            call.instance()->name, idText);
    pluginxx::detail::setErrorString(errorOut,
                                     "subscribe_broadcast: illegal id");
    return nullptr;
  }
  const std::string options =
      (optionsJson && optionsJson->data)
          ? std::string{optionsJson->data, static_cast<size_t>(optionsJson->size)}
          : std::string{};
  // 作用域引用里的 id 是借用视图: 投递前复制成自有结构
  const bool hasScope = pluginxx_scope_ref_ok(scope) != 0;
  const PluginxxScopeRef scopeCopy = hasScope ? *scope : PluginxxScopeRef{};
  return pluginxx::startTableOp(
      mgr, cb, cbUd, errorOut, "musicxx.broadcasts.subscribe",
      [call, hasScope, scopeCopy, idText, handler, onEnd, ud, options]()
          -> std::string {
        auto *mgr = call.manager();
        // 应用下发声明之前, 宿主按 id 前缀补一份默认声明
        mgr->ensureBroadcastChannel(mgr->qualifyEventTopic(idText));
        auto *sub = mgr->subscribeBroadcast(call.instance(),
                                            hasScope ? &scopeCopy : nullptr,
                                            idText, handler, ud, options, onEnd);
        if (!sub) {
          throw pluginxx::TableOpError("subscribe_broadcast: rejected");
        }
        return subscriptionPayloadOf(sub);
      });
}

/// 订阅广播 (任意线程)
PluginxxOperatorHandle *PLUGINXX_CALL xx_subscribe_broadcast(
    const PluginxxHost *host, const PluginxxScopeRef *scope,
    const PluginxxStringView *id,
    void(PLUGINXX_CALL *handler)(const PluginxxStringView *payloadJson,
                                 void *ud),
    void *ud, const PluginxxStringView *optionsJson, PluginxxOperatorCallback cb,
    void *cbUd, PluginxxString *errorOut) {
  return subscribeBroadcastFromTable(host, scope, id, handler, nullptr, ud,
                                     optionsJson, cb, cbUd, errorOut);
}

/// 订阅广播, 并带流结束回调 (追加成员; 任意线程)
PluginxxOperatorHandle *PLUGINXX_CALL xx_subscribe_broadcast_ex(
    const PluginxxHost *host, const PluginxxScopeRef *scope,
    const PluginxxStringView *id,
    void(PLUGINXX_CALL *handler)(const PluginxxStringView *payloadJson,
                                 void *ud),
    void(PLUGINXX_CALL *onEnd)(const PluginxxStringView *reasonJson, void *ud),
    void *ud, const PluginxxStringView *optionsJson, PluginxxOperatorCallback cb,
    void *cbUd, PluginxxString *errorOut) {
  return subscribeBroadcastFromTable(host, scope, id, handler, onEnd, ud,
                                     optionsJson, cb, cbUd, errorOut);
}

/// 撤销订阅 (幂等; 任意线程)
PluginxxOperatorHandle *PLUGINXX_CALL xx_unsubscribe_broadcast(
    const PluginxxHost *host, PluginxxSubscription *sub,
    PluginxxOperatorCallback cb, void *cbUd, PluginxxString *errorOut) {
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
          host, /*allowClosing=*/true);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(
        errorOut, "unsubscribe_broadcast: host is unavailable");
    return nullptr;
  }
  return pluginxx::startTableOp(
      call.manager(), cb, cbUd, errorOut, "musicxx.broadcasts.unsubscribe",
      [sub]() -> std::string {
        pluginxx::unsubscribePluginBroadcast(sub);
        return std::string{};
      });
}

/// 结束一条流 (任意线程): 只有频道属主能结束自己的频道
///
/// 结束后: 发布按 `ended` 失败, 读不到当前值, 全部订阅先用 reason JSON 回调一次
/// 再摘掉; 再次声明同一个 id = 开一条新流。
PluginxxOperatorHandle *PLUGINXX_CALL xx_end_broadcast(
    const PluginxxHost *host, const PluginxxStringView *id,
    const PluginxxStringView *reasonJson, PluginxxOperatorCallback cb,
    void *cbUd, PluginxxString *errorOut) {
  if (!id || !id->data || id->size == 0) {
    pluginxx::detail::setErrorString(errorOut, "end_broadcast: missing id");
    return nullptr;
  }
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(errorOut,
                                     "end_broadcast: host is unavailable");
    return nullptr;
  }
  const std::string idText{id->data, static_cast<size_t>(id->size)};
  // 与发布同一条归属规则: 只能结束自己命名空间下的频道
  const std::string pluginId =
      MusicxxHostManager::pluginIdOf(call.instance()->name);
  if (!isTopicOwnedBy(idText, pluginId)) {
    XX_LOGW("[musicxx_ext] end-broadcast by plugin `{}` rejected: id `{}` "
            "is not in the plugin namespace",
            pluginId, idText);
    pluginxx::detail::setErrorString(
        errorOut, "end_broadcast: id is not in the plugin namespace");
    return nullptr;
  }
  const std::string reason =
      (reasonJson && reasonJson->data && reasonJson->size > 0)
          ? std::string{reasonJson->data, static_cast<size_t>(reasonJson->size)}
          : std::string{"{}"};
  return pluginxx::startTableOp(
      call.manager(), cb, cbUd, errorOut, "musicxx.broadcasts.end",
      [call, idText, reason]() -> std::string {
        std::string error;
        const int32_t rc = call.manager()->endBroadcastFrom(
            call.instance(), idText, reason, &error);
        if (rc != PLUGINXX_RESULT_OK) {
          throw pluginxx::TableOpError(
              error.empty() ? "end_broadcast: rejected" : error);
        }
        return std::string{};
      });
}

/// 发布广播 (任意线程): id 必须是官方 `musicxx.*` 或本插件自己的
/// `plugin.<id>.*`
PluginxxOperatorHandle *PLUGINXX_CALL xx_publish_broadcast(
    const PluginxxHost *host, const PluginxxStringView *id,
    const PluginxxStringView *payloadJson, PluginxxOperatorCallback cb,
    void *cbUd, PluginxxString *errorOut) {
  if (!id || !id->data) {
    pluginxx::detail::setErrorString(errorOut,
                                     "publish_broadcast: missing id");
    return nullptr;
  }
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(errorOut,
                                     "publish_broadcast: host is unavailable");
    return nullptr;
  }
  const std::string idText{id->data, static_cast<size_t>(id->size)};
  const std::string pluginId =
      MusicxxHostManager::pluginIdOf(call.instance()->name);
  if (!isTopicOwnedBy(idText, pluginId)) {
    XX_LOGW("[musicxx_ext] publish-broadcast by plugin `{}` rejected: id `{}` "
            "is not in the plugin namespace",
            pluginId, idText);
    pluginxx::detail::setErrorString(
        errorOut, "publish_broadcast: id is not in the plugin namespace");
    return nullptr;
  }
  const std::string payload =
      (payloadJson && payloadJson->size > 0 && payloadJson->data)
          ? std::string{payloadJson->data,
                        static_cast<size_t>(payloadJson->size)}
          : std::string{"{}"};
  return pluginxx::startTableOp(
      call.manager(), cb, cbUd, errorOut, "musicxx.broadcasts.publish",
      [call, idText, payload, pluginId]() -> std::string {
        auto *mgr = call.manager();
        // 事件计数与速率 (只观测不限制): 与 publishOnHostThread 同一份实现
        mgr->accountEventForTopic(idText);
        mgr->ensureBroadcastChannel(idText);
        std::string error;
        const int32_t rc =
            mgr->publishBroadcast(idText, payload, pluginId, &error);
        if (rc != PLUGINXX_RESULT_OK) {
          throw pluginxx::TableOpError(
              error.empty() ? "publish_broadcast: rejected" : error);
        }
        return std::string{};
      });
}

/// 读当前值 (任意线程; 注册表内部加锁)
PluginxxOperatorHandle *PLUGINXX_CALL xx_read_broadcast(
    const PluginxxHost *host, const PluginxxStringView *id,
    PluginxxOperatorCallback cb, void *cbUd, PluginxxString *errorOut) {
  if (!id || !id->data) {
    pluginxx::detail::setErrorString(errorOut, "read_broadcast: missing id");
    return nullptr;
  }
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
          host, /*allowClosing=*/true);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(errorOut,
                                     "read_broadcast: host is unavailable");
    return nullptr;
  }
  const std::string idText{id->data, static_cast<size_t>(id->size)};
  return pluginxx::startTableOp(
      call.manager(), cb, cbUd, errorOut, "musicxx.broadcasts.read",
      [call, idText]() -> std::string {
        std::string value;
        const int32_t rc = call.manager()->readBroadcastText(idText, &value);
        if (rc == PLUGINXX_RESULT_OK) {
          return value;
        }
        if (rc == PLUGINXX_RESULT_ERR_ENDED) {
          throw pluginxx::TableOpError("ended");
        }
        if (rc == PLUGINXX_RESULT_ERR_NO_VALUE) {
          throw pluginxx::TableOpError("no_value");
        }
        if (rc == PLUGINXX_RESULT_ERR_NOT_DECLARED) {
          throw pluginxx::TableOpError("not_declared");
        }
        throw pluginxx::TableOpError("read_broadcast: failed");
      });
}

/* ==================== 接口表: musicxx.kind (插件形态 / 运行器) ==================== */

/// 登记一种插件形态（本插件成为它的运行器）
PluginxxOperatorHandle *PLUGINXX_CALL xx_kind_register(
    const PluginxxHost *host, const PluginxxStringView *specJson,
    PluginxxOperatorCallback cb, void *cbUd, PluginxxString *errorOut) {
  if (!specJson || !specJson->data) {
    pluginxx::detail::setErrorString(errorOut,
                                     "register_kind: missing spec");
    return nullptr;
  }
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(errorOut,
                                     "register_kind: host is unavailable");
    return nullptr;
  }
  const std::string specText{specJson->data,
                             static_cast<size_t>(specJson->size)};
  return pluginxx::startTableOp(
      call.manager(), cb, cbUd, errorOut, "musicxx.kind.register",
      [call, specText]() -> std::string {
        std::string error;
        const int32_t rc = call.manager()->registerPluginKind(
            call.instance()->name, specText, error);
        if (rc != PLUGINXX_RESULT_OK) {
          XX_LOGW("[musicxx_ext] failed to register plugin kind for plugin `{}`: {}",
                  call.instance()->name, error);
          throw pluginxx::TableOpError(
              error.empty() ? "register_kind: rejected" : error);
        }
        return std::string{};
      });
}

/// 撤销本插件登记的形态
PluginxxOperatorHandle *PLUGINXX_CALL xx_kind_unregister(
    const PluginxxHost *host, const PluginxxStringView *kind,
    PluginxxOperatorCallback cb, void *cbUd, PluginxxString *errorOut) {
  if (!kind || !kind->data) {
    pluginxx::detail::setErrorString(errorOut,
                                     "unregister_kind: missing kind");
    return nullptr;
  }
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(errorOut,
                                     "unregister_kind: host is unavailable");
    return nullptr;
  }
  const std::string kindText{kind->data, static_cast<size_t>(kind->size)};
  return pluginxx::startTableOp(
      call.manager(), cb, cbUd, errorOut, "musicxx.kind.unregister",
      [call, kindText]() -> std::string {
        std::string error;
        const int32_t rc = call.manager()->unregisterPluginKind(
            call.instance()->name, kindText, error);
        if (rc != PLUGINXX_RESULT_OK) {
          throw pluginxx::TableOpError(
              error.empty() ? "unregister_kind: rejected" : error);
        }
        return std::string{};
      });
}

/// 列出全部形态（内置 + 插件登记）
PluginxxOperatorHandle *PLUGINXX_CALL xx_kind_list(
    const PluginxxHost *host, PluginxxOperatorCallback cb, void *cbUd,
    PluginxxString *errorOut) {
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
          host, /*allowClosing=*/true);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(errorOut,
                                     "list_kinds: host is unavailable");
    return nullptr;
  }
  return pluginxx::startTableOp(
      call.manager(), cb, cbUd, errorOut, "musicxx.kind.list",
      [call]() -> std::string {
        return call.manager()->kindsJson();
      });
}

const MusicxxPluginKindIface g_ifaceKind = {
    /* version */ MUSICXX_PLUGIN_IFACE_KIND_VERSION,
    /* struct_size */ sizeof(MusicxxPluginKindIface),
    /* register_kind */ xx_kind_register,
    /* unregister_kind */ xx_kind_unregister,
    /* list_kinds */ xx_kind_list,
};

const PluginxxBroadcastsIface g_ifaceBroadcasts = {
    /* version */ PLUGINXX_IFACE_BROADCASTS_VERSION,
    /* struct_size */ sizeof(PluginxxBroadcastsIface),
    /* declare_broadcast */ xx_declare_broadcast,
    /* publish_broadcast */ xx_publish_broadcast,
    /* subscribe_broadcast */ xx_subscribe_broadcast,
    /* read_broadcast */ xx_read_broadcast,
    /* unsubscribe_broadcast */ xx_unsubscribe_broadcast,
    /* end_broadcast */ xx_end_broadcast,
    /* subscribe_broadcast_ex */ xx_subscribe_broadcast_ex,
};

const void *PLUGINXX_CALL xx_query_interface(const PluginxxHost *,
                                             const PluginxxStringView *iid);

const PluginxxHostVtable g_hostVtable = {
    /* alloc */ xx_alloc_fn,
    /* free */ xx_free_fn,
    /* query_interface */ xx_query_interface,
};

const void *PLUGINXX_CALL xx_query_interface(const PluginxxHost *,
                                             const PluginxxStringView *iid) {
  if (!iid || !iid->data) {
    return nullptr;
  }
  const std::string_view name{iid->data, static_cast<size_t>(iid->size)};
  if (name == "__vtable") {
    return &g_hostVtable;
  }
  // 广播表用本框架自己的入口 (需要在发布路径上做命名空间归属校验, 见
  // xx_publish_broadcast)
  if (name == PLUGINXX_IFACE_BROADCASTS) {
    return &g_ifaceBroadcasts;
  }
  // 其余通用表
  // (log/json/config/plugins/scheduler/coroutine_runtime/tasks/cancel/
  // capabilities) 由插件框架提供; musicxx 专用表在下面分发
  if (const void *generic =
          pluginxx::queryGenericPluginIface<MusicxxHostInstance,
                                            MusicxxHostManager>(name)) {
    return generic;
  }
  if (name == MUSICXX_PLUGIN_IFACE_HOST) {
    return &g_ifaceHost;
  }
  if (name == MUSICXX_PLUGIN_IFACE_UI) {
    return uiIfaceForQuery();
  }
  if (name == MUSICXX_PLUGIN_IFACE_KIND) {
    return &g_ifaceKind;
  }
  if (name == PLUGINXX_IFACE_NET) {
    return netIfaceForQuery();
  }
  // 预留表 (musicxx.player/playlist/songlist/lyric/storage/stats/agent): v1
  // 返回空, 插件据此判空并降级 (IID 已冻结, 后续实现不再改动约定)
  return nullptr;
}

} // namespace

const PluginxxHostVtable *MusicxxHostManager::hostVtable() {
  return &g_hostVtable;
}

} // namespace extern_plugin
} // namespace musicxx
