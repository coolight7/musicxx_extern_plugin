/// musicxx 外部插件原生插件框架: 能力派发 (宿主自己的实现表) 与专用接口表
///
/// 能力 / 广播的声明只有一份: `tools/capabilities.def.json` → `tools/gen_contract.dart`
/// 生成 `musicxx/plugin/api/capability_ids.g.h`（插件框架与插件作者共用同一份）。
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
#include <chrono>
#include <cstddef>

namespace musicxx {
namespace extern_plugin {

using utilxx_base::Json;

namespace {

/// 命名空间规则 单一实现在 host_naming.h
///
/// 说明: 名字不再做归属校验 (插件可以调任意名字), 这里只保留广播频道的
/// 归属判定 (发布事件仍要求 `musicxx.*` 或自己命名空间)。
using naming::isTopicOwnedBy;

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
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (!spec.capability_id.data || spec.capability_id.size == 0) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  const std::string capabilityId{spec.capability_id.data,
                                 static_cast<size_t>(spec.capability_id.size)};
  if (!spec.run_sync && !spec.run_start) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  const int32_t phase = spec.phase;
  if (!isValidPhase(phase)) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册 `{}` 被拒绝: 阶段 {} 不合法",
            inst->name, capabilityId, phase);
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  /// 前链 / 后链必须是同步处理器: 要拿到这一步的结果才能跑下一步
  if (phase != MUSICXX_CAPABILITY_PHASE_IMPL && !spec.run_sync) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册 `{}` 的 {} 阶段被拒绝: "
            "阶段处理器必须是同步处理器 (run_start 无法逐步串行)",
            inst->name, capabilityId, phaseNameOf(phase));
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (inst->unloadRequested || !inst->enabled) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册 `{}` 被拒绝: 实例正在关闭或已禁用",
            inst->name, capabilityId);
    return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  }
  std::string reason;
  if (!checkRegistrable(capabilityId, phase, &reason)) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册被拒绝: {}", inst->name, reason);
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
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

  XX_LOGI("[musicxx_ext] 插件 `{}` 注册 `{}` (phase={}, priority={}, "
          "当前处理器数={})",
          inst->name, capabilityId, phaseNameOf(phase), spec.priority,
          handlers.size());

  // 通知 Dart 侧刷新"这个能力有没有处理器"的位图与各阶段的处理器列表
  pushEvent("musicxx.capability.changed", h.pluginId,
            capabilityChangedPayloadJson(capabilityId, "register", h.pluginId));
  return MUSICXX_EXTERN_PLUGIN_OK;
}

/* ==================== 能力表 `musicxx.capabilities` 的入口 ==================== */

int32_t MusicxxHostManager::registerCapabilityHandler(
    MusicxxHostInstance *inst, const std::string &capabilityId, int32_t phase,
    int32_t priority, const std::string &ownerTag,
    PluginxxCapabilityStartFunction start,
    PluginxxOperatorCancelFunction cancel, void *ctx, std::string &error) {
  if (!inst || !start) {
    error = "capability: missing instance or handler";
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (!isValidPhase(phase)) {
    error = "capability: illegal phase";
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (inst->unloadRequested || !inst->enabled) {
    error = "capability: instance is closing or disabled";
    return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  }
  if (!checkRegistrable(capabilityId, phase, &error)) {
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
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

  XX_LOGI("[musicxx_ext] 插件 `{}` 通过能力表注册 `{}` (phase={}, "
          "priority={}, 当前处理器数={})",
          inst->name, capabilityId, phaseNameOf(phase), priority, handlers.size());
  pushEvent("musicxx.capability.changed", h.pluginId,
            capabilityChangedPayloadJson(capabilityId, "register", h.pluginId));
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::unregisterCapabilityHandler(
    MusicxxHostInstance *inst, std::string_view capabilityId,
    std::string_view ownerTag) {
  if (capabilityId.empty()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  auto it = handlers_.find(std::string{capabilityId});
  if (it == handlers_.end()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
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
  return MUSICXX_EXTERN_PLUGIN_OK;
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
  // 宿主派发是同步的：异步给结果的处理器这一轮拿不到结果，直接拒绝并说清原因
  if (spec.sync == 0) {
    reason = "capability `" + id +
             "` 的处理器必须同步给结果（登记时 sync = 1）；"
             "异步能力请用普通能力名，不要挂在应用声明的 id 上";
    if (error) {
      *error = reason;
    }
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册实现被拒绝: {}",
            inst ? inst->name : "?", reason);
    return PLUGINXX_RESULT_ERR_ARG;
  }
  if (spec.run_start == nullptr) {
    reason = "capability `" + id + "` 缺少处理器（run_start 为空）";
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
  if (rc != MUSICXX_EXTERN_PLUGIN_OK && error) {
    *error = reason;
  }
  return rc == MUSICXX_EXTERN_PLUGIN_OK ? PLUGINXX_RESULT_OK
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

void MusicxxHostManager::declareAppCapabilities() {
  auto caps = capabilities();
  if (!caps) {
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
      XX_LOGW("[musicxx_ext] 能力 `{}` 声明失败: {}", entry.id, error);
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
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
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
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  auto it = handlers_.find(std::string{capabilityId});
  if (it == handlers_.end()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
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
  return MUSICXX_EXTERN_PLUGIN_OK;
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
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
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
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::handlerCount(const std::string &capabilityId,
                                      int32_t &outCount) {
  auto it = handlers_.find(capabilityId);
  outCount = (it == handlers_.end()) ? 0 : static_cast<int32_t>(it->second.size());
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::handlerCountPhase(const std::string &capabilityId,
                                           int32_t phase, int32_t &outCount) {
  if (!isValidPhase(phase)) {
    outCount = 0;
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  outCount = countOnPhase(capabilityId, phase);
  return MUSICXX_EXTERN_PLUGIN_OK;
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
  return MUSICXX_EXTERN_PLUGIN_OK;
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
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (phase < -1 || !isValidPhase(phase == -1 ? 0 : phase)) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
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
  return MUSICXX_EXTERN_PLUGIN_OK;
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

} // namespace

namespace {

/// 同步调用一个“能力表”形态的处理器时收集它的结果
///
/// `musicxx.capabilities` 的处理器是异步契约（`notify->done` 恰好一次）；宿主派发是同步的，
/// 所以只接受“在 start 返回之前就 done”的处理器，多出来的回调按第一次算。
struct CapabilitySyncReply {
  bool done = false;
  int32_t status = PLUGINXX_OPERATOR_OK;
  std::string payload;
};

void PLUGINXX_CALL capabilitySyncDone(void *ud, int32_t status,
                                      const PluginxxStringView *payload) {
  auto *reply = static_cast<CapabilitySyncReply *>(ud);
  if (!reply || reply->done) {
    return;
  }
  reply->done = true;
  reply->status = status;
  if (payload && payload->data && payload->size > 0) {
    reply->payload.assign(payload->data, static_cast<size_t>(payload->size));
  }
}

} // namespace

std::string MusicxxHostManager::runCapabilityHandlers(
    const std::string &capabilityId, int32_t phase,
    const std::string &inputJson, DispatchMode mode) {
  const CapabilityMeta meta = metaOf(capabilityId);
  auto it = handlers_.find(capabilityId);
  if (it == handlers_.end() || it->second.empty()) {
    return R"({"handled":false,"handlers":0})";
  }

  /// 派发前**快照**本阶段的处理器列表 (派发期间注册表变化不影响本轮遍历)
  ///
  /// 插件处理器在框架线程上**就地执行** (`ioCallSyncKeep` 语义), 处理器里调用
  /// `musicxx.capabilities` 的注册 / 注销会改动 handlers_ 里的同一条 vector:
  /// 注册会重新分配, 注销最后一个会让整条 vector 失效 —— 直接遍历活表会出现
  /// 迭代器/引用失效 (漏派发或读到已释放内存)。快照只决定"本轮派发谁被调用",
  /// 注册表的变化从下一轮起生效; 统计写回按 (实例, ownerTag, 阶段) 重新查活表。
  std::vector<CapabilityHandler> handlers;
  handlers.reserve(it->second.size());
  for (const CapabilityHandler &h : it->second) {
    if (h.phase == phase) {
      handlers.push_back(h);
    }
  }
  if (handlers.empty()) {
    return R"({"handled":false,"handlers":0})";
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

  /// 调一个处理器并记账 (统计写回活表; 异常与失败都只记日志, 不影响其它处理器)
  ///
  /// [payloadText] 是这一步处理器看到的载荷, [outText] 是它回答的 JSON 文本
  /// (空 = 没有意见)。异步处理器 (只有实现链允许) 由调用方单独处理。
  auto invokeSync = [&](const CapabilityHandler &h, const std::string &payloadText,
                        std::string &outText) {
    outText.clear();
    PluginxxString out{};
    PluginxxString err{};
    PluginxxStringView capabilityView{h.capabilityId.data(), h.capabilityId.size()};
    PluginxxStringView inputView{payloadText.data(), payloadText.size()};
    const auto t0 = std::chrono::steady_clock::now();
    int32_t rc = -1;
    try {
      if (h.rowStart) {
        // 走 `musicxx.capabilities` 登记的处理器：同步等它 done
        CapabilitySyncReply reply;
        PluginxxOperatorNotify notify{&capabilitySyncDone, &reply};
        PluginxxString capErr{};
        const PluginxxStringView methodView{h.capabilityId.data(), h.capabilityId.size()};
        const PluginxxStringView argsView{payloadText.data(),
                                          payloadText.size()};
        void *op = nullptr;
        try {
          op = h.rowStart(h.rowCtx, nullptr, &methodView, &argsView, &notify,
                          &capErr);
        } catch (const std::exception &e) {
          XX_LOGW("[musicxx_ext] 插件 `{}` 能力处理器异常 (`{}`, phase={}): {}",
                  h.pluginId, capabilityId, phaseNameOf(h.phase), e.what());
        } catch (...) {
          XX_LOGW("[musicxx_ext] 插件 `{}` 能力处理器未知异常 (`{}`, phase={})",
                  h.pluginId, capabilityId, phaseNameOf(h.phase));
        }
        if (capErr.data) {
          pluginxx::hostMemoryFree(capErr.data);
        }
        if (reply.done) {
          rc = reply.status == PLUGINXX_OPERATOR_OK ? 0 : -1;
          if (0 == rc) {
            outText = reply.payload;
          }
        } else if (op && h.rowCancel) {
          // 异步给结果：注册时已经要求同步，这里只是兜底（撤销这次操作，本轮不给结果）
          h.rowCancel(h.rowCtx, op);
          rc = -1;
        }
      } else {
        rc = h.spec.run_sync(h.spec.user_data, &capabilityView, &inputView, &out,
                              &err);
      }
    } catch (const std::exception &e) {
      XX_LOGW("[musicxx_ext] 插件 `{}` 处理器异常 (`{}`, phase={}): {}",
              h.pluginId, capabilityId, phaseNameOf(h.phase), e.what());
      rc = -1;
    } catch (...) {
      XX_LOGW("[musicxx_ext] 插件 `{}` 处理器未知异常 (`{}`, phase={})",
              h.pluginId, capabilityId, phaseNameOf(h.phase));
      rc = -1;
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    /// 说明: `us` 的类型是 chrono 的 rep (libc++/Android 上是 long long, 而 int64_t 是
    /// long), 直接交给 std::max 会因模板推导出两种类型而编译失败, 统一转成 int64_t 再统计
    const int64_t usValue = static_cast<int64_t>(us);
    /// 统计写回**活表**: 回调里插件可能已经把自己的处理器注销掉 (快照里还有),
    /// 这时丢弃本次统计, 避免统计与注册表不一致
    CapabilityHandler *live = nullptr;
    if (auto liveIt = handlers_.find(capabilityId); liveIt != handlers_.end()) {
      for (auto &candidate : liveIt->second) {
        if (candidate.plugin == h.plugin && candidate.ownerTag == h.ownerTag &&
            candidate.phase == h.phase) {
          live = &candidate;
          break;
        }
      }
    }
    if (auto instIt = loaded_.find(h.plugin);
        instIt != loaded_.end() && instIt->second) {
      instIt->second->capabilityCalls.fetch_add(1);
    }
    if (live) {
      ++live->calls;
      live->totalUs = live->totalUs + usValue;
      live->maxUs = (std::max)(live->maxUs, usValue);
      if (rc != 0) {
        /// 处理器失败只记统计 (不因为失败次数暂停派发): 日志在上面, 这里只累计次数
        ++live->failures;
      }
    }
    if (out.data) {
      const std::string_view text{out.data, static_cast<size_t>(out.size)};
      if (!text.empty() && text != "null") {
        outText.assign(text.data(), text.size());
      }
      pluginxx::hostMemoryFree(out.data);
    }
    if (err.data) {
      pluginxx::hostMemoryFree(err.data);
    }
  };

  /// 通知型能力派发的调试回传 (仅调试开关打开时回传; 默认关闭 → 零开销)
  auto emitNotifyDebugEvent = [&](int32_t called) {
    if (mode == DispatchMode::Notify &&
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
      if (!h.spec.run_sync && !h.rowStart) {
        // 前链 / 后链只接受同步处理器 (注册时已拒绝), 这里只防御
        continue;
      }
      std::string outText;
      invokeSync(h, current.dump(), outText);
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
      const std::string action = actionOf(answer);
      if (action == MUSICXX_PLUGIN_CAPABILITY_ACTION_CANCEL) {
        canceled = true;
        break;
      }
    }
    Json result;
    result["handled"] = called > 0 && mode != DispatchMode::Notify;
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
    return result.dump();
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
      if (!h.spec.run_sync && !h.rowStart) {
        continue;
      }
      Json step = payload;
      step["value"] = value;
      std::string outText;
      invokeSync(h, step.dump(), outText);
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
    result["handled"] = called > 0 && mode != DispatchMode::Notify;
    result["handlers"] = static_cast<int32_t>(handlers.size());
    result["called"] = called;
    Json res = Json::object();
    res["value"] = value;
    if (dropped) {
      res["action"] = MUSICXX_PLUGIN_CAPABILITY_ACTION_DROP;
    }
    result["result"] = res;
    emitNotifyDebugEvent(called);
    return result.dump();
  }

  /* ---------- 实现链: 按声明的合并策略处理 (first / veto 可提前结束) ---------- */
  Json merged = Json::object();
  bool hasPatch = false;
  std::string action;

  for (auto &h : handlers) {
    if (!h.spec.run_sync && !h.rowStart) {
      // 异步处理器: 本轮只做通知 (调用 run_start, 结果不参与同步合并)
      if (h.spec.run_start) {
        PluginxxString err{};
        PluginxxStringView capabilityView{h.capabilityId.data(), h.capabilityId.size()};
        PluginxxStringView inputView{inputJson.data(), inputJson.size()};
        try {
          h.spec.run_start(h.spec.user_data, &capabilityView, &inputView, nullptr,
                            &err);
        } catch (...) {
          XX_LOGW("[musicxx_ext] 插件 `{}` 异步处理器异常 (`{}`)", h.pluginId,
                  capabilityId);
        }
        if (err.data) {
          pluginxx::hostMemoryFree(err.data);
        }
      }
      ++called;
      continue;
    }

    std::string outText;
    invokeSync(h, inputJson, outText);
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
  result["handled"] = called > 0 && mode != DispatchMode::Notify;
  /// handlers = 本轮快照里的处理器数量 (回调里注册/注销不影响本轮的计数)
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
  return result.dump();
}

int32_t MusicxxHostManager::dispatchCapability(const std::string &capabilityId,
                                               const std::string &inputJson,
                                               bool sync, uint32_t timeoutMs,
                                               std::string &outJson,
                                               std::string &err) {
  return dispatchCapabilityPhase(capabilityId, MUSICXX_CAPABILITY_PHASE_IMPL,
                                 inputJson, sync, timeoutMs, outJson, err);
}

int32_t MusicxxHostManager::dispatchCapabilityPhase(
    const std::string &capabilityId, int32_t phase, const std::string &inputJson,
    bool sync, uint32_t /*timeoutMs*/, std::string &outJson, std::string &err) {
  if (capabilityId.empty()) {
    err = "capability dispatch: empty id";
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (!isValidPhase(phase)) {
    err = "capability dispatch: invalid phase";
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (!running_.load(std::memory_order_acquire)) {
    err = "capability dispatch: host not started";
    return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  }
  const CapabilityMeta meta = metaOf(capabilityId);
  if (countOnPhase(capabilityId, phase) == 0) {
    outJson = R"({"handled":false,"handlers":0})";
    return MUSICXX_EXTERN_PLUGIN_OK;
  }

  /// 实现链的异步派发：不占用 Dart 线程，完成后经
  /// `musicxx.capability.decision.result` 事件把结果回传（payload 带 callId 供配对）。
  /// 广播只通知，异步派发不需要结果。
  const bool asyncDecide = !sync && !meta.notifyOnly;
  const DispatchMode mode =
      sync ? DispatchMode::Sync
           : (asyncDecide ? DispatchMode::AsyncDecide
                          : DispatchMode::Notify);
  const int64_t callId =
      asyncDecide ? nextDispatchCallId_.fetch_add(1, std::memory_order_relaxed) : 0;

  auto slot = std::make_shared<WaitSlot<std::string>>();
  auto self = shared_from_this();
  asio::post(ctx_->io, [self, slot, capabilityId, phase, inputJson, mode, callId]() {
    std::string result = self->runCapabilityHandlers(capabilityId, phase, inputJson, mode);
    if (mode == DispatchMode::AsyncDecide) {
      Json payload = parseJsonSafe(result);
      payload["callId"] = callId;
      payload["capability"] = capabilityId;
      payload["phase"] = phaseNameOf(phase);
      self->pushEvent("musicxx.capability.decision.result", "", payload.dump());
      return;
    }
    slot->set(std::move(result));
  });

  if (!sync) {
    Json ack;
    ack["handled"] = true;
    ack["async"] = true;
    if (asyncDecide) {
      ack["callId"] = callId;
    }
    outJson = ack.dump();
    return MUSICXX_EXTERN_PLUGIN_OK;
  }
  /// 同步派发没有等待超时: 一直等到处理器链结束 (处理器链里包含插件自己的代码,
  /// 慢就是调用线程一起等。超时参数保留在 C ABI 里但不再使用)。
  std::string result;
  slot->wait(result);
  outJson = std::move(result);
  return MUSICXX_EXTERN_PLUGIN_OK;
}

/* ==================== 能力调用 (插件 → Dart) ==================== */

int32_t MusicxxHostManager::requestAppCapability(MusicxxHostInstance *inst,
                                          std::string_view action,
                                          std::string_view argsJson,
                                          uint32_t timeoutMs,
                                          const PluginxxOperatorNotify *notify,
                                          int64_t *outRequestId) {
  if (!inst || action.empty()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  const std::string capabilityName{action};
  const std::string pluginId = pluginIdOf(inst->name);
  /// 名字不校验、不拒绝: 插件想调什么就调什么, 由 Dart 侧按"注册了没有"决定
  /// 理不理它 (没注册的名称立刻回"未找到")。
  /// 命名建议: 官方能力 `musicxx.<域>.<名字>`; 插件自己的力量用 `plugin.<包名>.*`
  /// 以免与别的插件撞名。

  const int64_t requestId = nextRequestId_++;
  /// 超时不再限制区间: 传 0 表示不设超时 (一直等 Dart 侧回复); 非 0 时按插件给的值
  /// 到点终结该 op 并通知 Dart 收尾。
  const int64_t effectiveTimeoutMs = static_cast<int64_t>(timeoutMs);
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
      XX_LOGW("[musicxx_ext] 能力调用超时: plugin={} action={} id={}", pluginId,
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
        if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
          utilxx_base::Json error;
          error["ok"] = false;
          error["error"] = "net_call_failed";
          error["message"] =
              out.empty() ? "插件网络能力不可用" : std::move(out);
          result = error.dump();
        }
        /// 传输失败也按"请求完成"回答 (status 0, 结论在结果 JSON 的 ok 里),
        /// 与 `musicxx.net.fetch` 的约定一致。
        self->respondAppCapability(requestId, 0, result);
      });
      return MUSICXX_EXTERN_PLUGIN_OK;
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
  return MUSICXX_EXTERN_PLUGIN_OK;
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
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
  }
  /// 动态库插件的请求: 登记表归 IO 线程 (调用方已在 IO 线程时就地执行)
  return pluginxx::ioCallSync<int32_t>(
      this, [this, requestId, status, resultJson]() -> int32_t {
        auto it = pendingAppCapabilities_.find(requestId);
        if (it == pendingAppCapabilities_.end()) {
          return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
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
        return MUSICXX_EXTERN_PLUGIN_OK;
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
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (!inst->configPath.empty()) {
    out = inst->configPath;
    return MUSICXX_EXTERN_PLUGIN_OK;
  }
  out = deriveConfigPath(pluginIdOf(inst->name), {});
  return out.empty() ? MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND
                     : MUSICXX_EXTERN_PLUGIN_OK;
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
  // (功能点糖用它过滤自己发起的调用)。主程序读同一份信息时不带这个字段。
  if (!instanceName.empty()) {
    j["pluginId"] = pluginIdOf(instanceName);
  }
  // 客户端界面能力段 (由界面描述层库生成, 插件框架原样转发; 没配置时不出现这个字段)
  if (!uiCapabilitiesJson_.empty()) {
    j["ui"] = parseJsonSafe(uiCapabilitiesJson_);
  }
  outJson = j.dump();
  return MUSICXX_EXTERN_PLUGIN_OK;
}

/* ==================== 专用接口表 (C ABI 函数 + vtable) ==================== */

namespace {

int32_t PLUGINXX_CALL xx_host_get_info(const PluginxxHost *host,
                                       PluginxxString *outJson) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!outJson) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto call =
            pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
                host, true);
        if (!call.ok()) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto mgr = call.manager();
        auto inst = call.instance();
        const std::string name = inst ? inst->name : std::string{};
        std::string json;
        const int32_t rc = pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, name, &json]() -> int32_t {
              return mgr->hostInfoJson(json, name);
            });
        if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
          return rc;
        }
        pluginxx::hostMemorySetString(outJson, json);
        return MUSICXX_EXTERN_PLUGIN_OK;
      });
}

int32_t PLUGINXX_CALL xx_host_config_path(const PluginxxHost *host,
                                          PluginxxString *outPath) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!outPath) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto call =
            pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
                host, true);
        if (!call.ok()) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto mgr = call.manager();
        auto inst = call.instance();
        std::string path;
        const int32_t rc = pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, inst, &path]() -> int32_t {
              return mgr->pluginConfigPathFor(inst, path);
            });
        if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
          return rc;
        }
        pluginxx::hostMemorySetString(outPath, path);
        return MUSICXX_EXTERN_PLUGIN_OK;
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

/// 声明频道 (任意线程; 名字必须是官方或本插件命名空间)
int32_t PLUGINXX_CALL xx_declare_broadcast(const PluginxxHost *host,
                                           const PluginxxStringView *declJson,
                                           PluginxxString *errorOut) {
  return pluginxx::guardVtableCall(
      PLUGINXX_RESULT_ERR_ARG, [&]() -> int32_t {
        if (!declJson || !declJson->data) {
          pluginxx::detail::setErrorString(errorOut, "declare_broadcast: missing declaration");
          return PLUGINXX_RESULT_ERR_ARG;
        }
        auto call =
            pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
                host);
        if (!call.ok()) {
          return PLUGINXX_RESULT_ERR_ARG;
        }
        auto mgr = call.manager();
        auto inst = call.instance();
        const std::string text{
            declJson->data, static_cast<size_t>(declJson->size)};
        return pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, inst, text, errorOut]() -> int32_t {
              std::string error;
              const int32_t rc = mgr->declareBroadcast(inst, text, &error);
              if (rc != PLUGINXX_RESULT_OK) {
                pluginxx::detail::setErrorString(errorOut, error);
              }
              return rc;
            });
      });
}

/// 订阅广播 (任意线程; 主题形状与归属规则与旧事件表一致)
///
/// `options_json` 是订阅方自己的通知频率 (`{"intervalMs":200,"mode":"delay"}`; 可空)。
PluginxxSubscription *PLUGINXX_CALL xx_subscribe_broadcast(
    const PluginxxHost *host, const PluginxxScopeRef *scope,
    const PluginxxStringView *id,
    void(PLUGINXX_CALL *handler)(const PluginxxStringView *payload_json,
                                 void *ud),
    void *ud, const PluginxxStringView *optionsJson,
    PluginxxString *errorOut) {
  return pluginxx::guardVtableCall(nullptr, [&]() -> PluginxxSubscription * {
    auto call =
        pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
            host);
    if (!call.ok() || !id || !id->data || !handler) {
      pluginxx::detail::setErrorString(errorOut,
                                      "subscribe_broadcast: missing id or handler");
      return nullptr;
    }
    auto mgr = call.manager();
    auto inst = call.instance();
    const std::string idText =
        std::string{id->data, static_cast<size_t>(id->size)};
    // 订阅只要求 id 形状合法 (可以订阅其它插件或官方频道); 发布才做归属校验
    if (mgr->qualifyEventTopic(idText).empty()) {
      XX_LOGW("[musicxx_ext] 插件 `{}` 订阅非法广播 `{}` 被拒绝", inst->name,
              idText);
      pluginxx::detail::setErrorString(errorOut,
                                      "subscribe_broadcast: illegal id");
      return nullptr;
    }
    // 应用下发声明之前, 宿主按 id 前缀补一份默认声明
    mgr->ensureBroadcastChannel(mgr->qualifyEventTopic(idText));
    // 订阅频率选项是借用视图: 投递前复制成自有的字符串
    const std::string options =
        (optionsJson && optionsJson->data)
            ? std::string{optionsJson->data,
                          static_cast<size_t>(optionsJson->size)}
            : std::string{};
    // 作用域引用里的 id 是借用视图: 投递前复制成自有结构 (与通用表入口同一做法)
    PluginxxScopeRef scopeCopy{};
    const bool hasScope = pluginxx_scope_ref_ok(scope) != 0;
    if (hasScope) {
      scopeCopy = *scope;
    }
    auto *sub = pluginxx::ioCallSyncKeep<PluginxxSubscription *>(
        call, mgr,
        [mgr, inst, hasScope, scopeCopy, idText, handler, ud, options]() {
          return mgr->subscribeBroadcast(inst, hasScope ? &scopeCopy : nullptr,
                                         idText, handler, ud, options);
        });
    if (!sub) {
      pluginxx::detail::setErrorString(errorOut,
                                      "subscribe_broadcast: rejected");
    }
    return sub;
  });
}

/// 撤销订阅 (幂等; 任意线程)
void PLUGINXX_CALL xx_unsubscribe_broadcast(PluginxxSubscription *sub) {
  pluginxx::unsubscribePluginBroadcast(sub);
}

/// 发布广播 (任意线程): id 必须是官方 `musicxx.*` 或本插件自己的
/// `plugin.<id>.*`
int32_t PLUGINXX_CALL xx_publish_broadcast(const PluginxxHost *host,
                                           const PluginxxStringView *id,
                                           const PluginxxStringView *payloadJson,
                                           PluginxxString *errorOut) {
  return pluginxx::guardVtableCall(
      PLUGINXX_RESULT_ERR_ARG, [&]() -> int32_t {
        auto call =
            pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
                host);
        if (!call.ok() || !id || !id->data) {
          pluginxx::detail::setErrorString(errorOut,
                                          "publish_broadcast: missing id");
          return PLUGINXX_RESULT_ERR_ARG;
        }
        auto mgr = call.manager();
        auto inst = call.instance();
        const std::string idText =
            std::string{id->data, static_cast<size_t>(id->size)};
        const std::string pluginId = MusicxxHostManager::pluginIdOf(inst->name);
        if (!isTopicOwnedBy(idText, pluginId)) {
          XX_LOGW("[musicxx_ext] 插件 `{}` 发布广播被拒绝: id `{}` "
                  "不属于该插件命名空间",
                  pluginId, idText);
          pluginxx::detail::setErrorString(
              errorOut, "publish_broadcast: id is not in the plugin namespace");
          return PLUGINXX_RESULT_ERR_PERMISSION;
        }
        const std::string payload =
            (payloadJson && payloadJson->size > 0 && payloadJson->data)
                ? std::string{payloadJson->data,
                              static_cast<size_t>(payloadJson->size)}
                : std::string{"{}"};
        return pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, idText, payload, pluginId, errorOut]() -> int32_t {
              // 事件计数与速率 (只观测不限制): 与 publishOnHostThread 同一份实现
              mgr->accountEventForTopic(idText);
              mgr->ensureBroadcastChannel(idText);
              std::string error;
              const int32_t rc =
                  mgr->publishBroadcast(idText, payload, pluginId, &error);
              if (rc != PLUGINXX_RESULT_OK) {
                pluginxx::detail::setErrorString(errorOut, error);
              }
              return rc;
            });
      });
}

/// 读当前值 (任意线程; 注册表内部加锁)
int32_t PLUGINXX_CALL xx_read_broadcast(const PluginxxHost *host,
                                        const PluginxxStringView *id,
                                        PluginxxString *outJson) {
  return pluginxx::guardVtableCall(
      PLUGINXX_RESULT_ERR_ARG, [&]() -> int32_t {
        if (!id || !id->data || !outJson) {
          return PLUGINXX_RESULT_ERR_ARG;
        }
        auto call =
            pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
                host, true);
        if (!call.ok()) {
          return PLUGINXX_RESULT_ERR_ARG;
        }
        auto mgr = call.manager();
        const std::string idText{
            id->data, static_cast<size_t>(id->size)};
        return mgr->readBroadcast(idText, outJson);
      });
}

/* ==================== 接口表: musicxx.kind (插件形态 / 运行器) ==================== */

/// 登记一种插件形态（本插件成为它的运行器）
int32_t PLUGINXX_CALL xx_kind_register(const PluginxxHost *host,
                                       const PluginxxStringView *spec_json) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!spec_json || !spec_json->data) {
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
        const std::string specText{
            spec_json->data, static_cast<size_t>(spec_json->size)};
        return pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, inst, specText]() -> int32_t {
              std::string err;
              const int32_t rc =
                  mgr->registerPluginKind(inst->name, specText, err);
              if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
                XX_LOGW("[musicxx_ext] 插件 `{}` 登记插件形态失败: {}",
                        inst->name, err);
              }
              return rc;
            });
      });
}

/// 撤销本插件登记的形态
int32_t PLUGINXX_CALL xx_kind_unregister(const PluginxxHost *host,
                                         const PluginxxStringView *kind) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!kind || !kind->data) {
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
        const std::string kindText{kind->data,
                                   static_cast<size_t>(kind->size)};
        return pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, inst, kindText]() -> int32_t {
              std::string err;
              return mgr->unregisterPluginKind(inst->name, kindText, err);
            });
      });
}

/// 列出全部形态（内置 + 插件登记）
int32_t PLUGINXX_CALL xx_kind_list(const PluginxxHost *host,
                                   PluginxxString *out_json) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!out_json) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto call =
            pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
                host, /*allowClosing=*/true);
        if (!call.ok()) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto mgr = call.manager();
        const std::string json = pluginxx::ioCallSyncKeep<std::string>(
            call, mgr, [mgr]() { return mgr->kindsJson(); });
        pluginxx::hostMemorySetString(out_json, json);
        return MUSICXX_EXTERN_PLUGIN_OK;
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
