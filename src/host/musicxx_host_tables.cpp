/// musicxx 外部插件原生宿主: 领域接口表 (musicxx.hooks / musicxx.host) 与钩子派发
///
/// 线程约定 (plan §2.3): 表入口在 Dart 线程被调用, 内部投递到宿主线程并等待;
/// 已在宿主线程时就地执行 (内核 `ioCallSyncKeep` 语义)。

#include "host_json.h"
#include "host_naming.h"
#include "musicxx_host.h"

#include "musicxx/plugin/api/hook_ids.g.h"
#include "pluginxx/host/abi_util.h"
#include "utilxx_base/asio_error.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <asio/steady_timer.hpp>

#include <algorithm>
#include <chrono>

namespace musicxx {
namespace extern_plugin {

using utilxx_base::Json;

namespace {

/// 已知钩子表 (与 plan §8.2 的钩子总表一致)
///
/// 定义只有一处: `tools/hooks.def.json` → `tools/gen_contract.dart` 生成
/// `musicxx/plugin/api/hook_ids.g.h`（宿主与插件作者共用同一份）。
/// policy: MUSICXX_PLUGIN_HOOK_POLICY_*；mode: MUSICXX_PLUGIN_HOOK_MODE_*。
using HookMeta = musicxx::plugin::MusicxxPluginHookMeta;

const HookMeta* findHookMeta(std::string_view hookId) {
    for (size_t i = 0; i < musicxx::plugin::musicxxPluginKnownHookCount; ++i) {
        const auto& meta = musicxx::plugin::musicxxPluginKnownHooks[i];
        if (hookId == meta.id) {
            return &meta;
        }
    }
    return nullptr;
}

/// 命名空间规则 (plan §3.5) 的单一实现在 host_naming.h
using naming::isOfficial;
using naming::isOwnPlugin;
using naming::isTopicOwnedBy;

} // namespace

/* ==================== 钩子注册 / 注销 ==================== */

int32_t MusicxxHostManager::registerHook(
    MusicxxHostInstance*         inst,
    const MusicxxPluginHookSpec& spec
) {
    if (!inst) {
        return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
    }
    if (spec.struct_size != 0 && spec.struct_size < sizeof(MusicxxPluginHookSpec)) {
        return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
    }
    if (!spec.hook_id.data || spec.hook_id.size == 0) {
        return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
    }
    const std::string hookId{spec.hook_id.data, static_cast<size_t>(spec.hook_id.size)};
    if (!spec.hook_sync && !spec.hook_start) {
        return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
    }
    if (inst->unloadRequested || !inst->enabled) {
        XX_LOGW("[musicxx_ext] 插件 `{}` 注册钩子被拒绝: 实例正在关闭或已禁用", inst->name);
        return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
    // 未知钩子拒绝注册 (plan §8.3.1 硬规则一): 插件不得制造宿主不认识的钩子点
    const auto* meta = findHookMeta(hookId);
    if (!meta) {
        XX_LOGW("[musicxx_ext] 插件 `{}` 注册未知钩子 `{}` 被拒绝", inst->name, hookId);
        return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
    }
    if (meta->mode == MUSICXX_PLUGIN_HOOK_MODE_DECISION && spec.mode == MUSICXX_PLUGIN_HOOK_MODE_OBSERVE) {
        // 决定型钩子允许以观察型处理器注册: 只观察, 不参与裁决 (handler.spec.mode 决定派发行为)
    }

    const std::string ownerTag = spec.owner_tag.data && spec.owner_tag.size > 0
                                     ? std::string{spec.owner_tag.data, static_cast<size_t>(spec.owner_tag.size)}
                                     : std::string{};

    auto& handlers = hooks_[hookId];
    // 同一个 (实例, hook_id, owner_tag) 覆盖式注册 (plan §4.3 语义要点 1)
    handlers.erase(
        std::remove_if(
            handlers.begin(),
            handlers.end(),
            [&](const HookHandler& h) { return h.plugin == inst->name && h.ownerTag == ownerTag; }
        ),
        handlers.end()
    );

    HookHandler h;
    h.plugin   = inst->name;
    h.pluginId = pluginIdOf(inst->name);
    h.ownerTag = ownerTag;
    h.hookId   = hookId;
    h.spec     = spec;
    h.seq      = ++hookSeq_;
    handlers.push_back(h);

    XX_LOGI(
        "[musicxx_ext] 插件 `{}` 注册钩子 `{}` (mode={}, priority={}, 当前处理器数={})",
        inst->name,
        hookId,
        spec.mode,
        spec.priority,
        handlers.size()
    );

    // 通知 Dart 侧刷新"该钩子是否有处理器"的位图
    Json payload;
    payload["id"]     = h.pluginId;
    payload["hook"]   = hookId;
    payload["mode"]   = spec.mode;
    payload["count"]  = static_cast<int32_t>(handlers.size());
    payload["action"] = "register";
    pushEvent("musicxx.hook.changed", h.pluginId, payload.dump());
    return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::unregisterHook(
    MusicxxHostInstance* inst,
    std::string_view     hookId,
    std::string_view     ownerTag
) {
    if (hookId.empty()) {
        return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
    }
    auto it = hooks_.find(std::string{hookId});
    if (it == hooks_.end()) {
        return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
    }
    const std::string pluginName = inst ? inst->name : std::string{};
    const std::string tag{ownerTag};
    it->second.erase(
        std::remove_if(
            it->second.begin(),
            it->second.end(),
            [&](const HookHandler& h) { return h.plugin == pluginName && h.ownerTag == tag; }
        ),
        it->second.end()
    );
    const int32_t remaining = static_cast<int32_t>(it->second.size());
    if (it->second.empty()) {
        hooks_.erase(it);
    }
    Json payload;
    payload["id"]     = inst ? pluginIdOf(inst->name) : std::string{};
    payload["hook"]   = std::string{hookId};
    payload["count"]  = remaining;
    payload["action"] = "unregister";
    pushEvent("musicxx.hook.changed", payload["id"].get<std::string>(), payload.dump());
    return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::hookInfoJson(std::string_view hookId, std::string& outJson) {
    const auto* meta = findHookMeta(hookId);
    if (!meta) {
        return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
    }
    Json j;
    j["id"]       = std::string{hookId};
    j["mode"]     = meta->mode;
    j["policy"]   = meta->policy;
    j["budgetMs"] = meta->budgetMs > 0 ? meta->budgetMs : hookBudgetMs_;
    j["hardMs"]   = meta->hardMs > 0 ? meta->hardMs : hookHardMs_;
    auto it       = hooks_.find(std::string{hookId});
    j["handlers"] = (it == hooks_.end()) ? 0 : static_cast<int32_t>(it->second.size());
    outJson       = j.dump();
    return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::hookCount(const std::string& hookId, int32_t& outCount) {
    auto it = hooks_.find(hookId);
    outCount = (it == hooks_.end()) ? 0 : static_cast<int32_t>(it->second.size());
    return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::hookStatsJson(std::string& outJson) {
    Json hooks = Json::object();
    for (const auto& [hookId, handlers] : hooks_) {
        Json arr = Json::array();
        for (const auto& h : handlers) {
            arr.push_back({
                {"plugin", h.pluginId},
                {"ownerTag", h.ownerTag},
                {"mode", h.spec.mode},
                {"priority", h.spec.priority},
                {"calls", h.calls},
                {"failures", h.failures},
                {"timeouts", h.timeouts},
                {"avgUs", h.calls > 0 ? h.totalUs / h.calls : 0},
                {"maxUs", h.maxUs},
                {"paused", h.pausedUntil > std::chrono::steady_clock::now()},
            });
        }
        hooks[hookId] = arr;
    }
    Json j;
    j["hooks"] = hooks;
    outJson    = j.dump();
    return MUSICXX_EXTERN_PLUGIN_OK;
}

/* ==================== 钩子派发 ==================== */

namespace {

/// 浅合并 `patch` 子对象 (allMerge / anyCancel 的合并口径; 后续键覆盖先前键)
void mergePatch(Json& target, const Json& source) {
    if (!source.is_object()) {
        return;
    }
    const Json& patch = source.contains("patch") ? source["patch"] : source;
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

std::string actionOf(const Json& j) {
    if (j.is_object() && j.contains("action") && j["action"].is_string()) {
        return j["action"].get<std::string>();
    }
    return {};
}

} // namespace

std::string MusicxxHostManager::dispatchHook(
    const std::string& hookId,
    const std::string& inputJson,
    uint32_t           budgetMs,
    HookDispatchMode   mode
) {
    const auto* meta = findHookMeta(hookId);
    if (!meta) {
        return R"({"handled":false,"error":"unknown_hook"})";
    }
    /// 观察型派发不等待、不施加等待预算（入队即返回的语义）；两种裁决派发都要收敛时间
    const bool waitBudget = mode != HookDispatchMode::Notify;
    auto it = hooks_.find(hookId);
    if (it == hooks_.end() || it->second.empty()) {
        return R"({"handled":false,"handlers":0})";
    }

    auto& handlers = it->second;
    std::stable_sort(handlers.begin(), handlers.end(), [](const HookHandler& a, const HookHandler& b) {
        if (a.spec.priority != b.spec.priority) {
            return a.spec.priority < b.spec.priority;
        }
        return a.seq < b.seq;
    });

    const auto budget  = budgetMs > 0 ? budgetMs : static_cast<uint32_t>((std::max)(meta->budgetMs, 1));
    const auto hardMs  = static_cast<int64_t>((std::max)(meta->hardMs, meta->budgetMs));
    const auto started = std::chrono::steady_clock::now();

    int32_t     called   = 0;
    bool        timedOut = false;
    Json        merged   = Json::object();
    bool        hasPatch = false;
    std::string action;

    for (auto& h : handlers) {
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - started
        )
                                   .count();
        if (waitBudget && elapsedMs >= static_cast<int64_t>(budget)) {
            timedOut = true;
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        if (h.pausedUntil > now) {
            continue; ///< 熔断中: 仅暂停派发, 不卸载插件 (plan §4.10)
        }
        if (!h.spec.hook_sync) {
            // 异步处理器: 本轮只做通知 (调用 hook_start, 结果不参与同步合并)
            if (h.spec.hook_start) {
                PluginxxString err{};
                PluginxxStringView hookView{h.hookId.data(), h.hookId.size()};
                PluginxxStringView inputView{inputJson.data(), inputJson.size()};
                try {
                    h.spec.hook_start(h.spec.user_data, &hookView, &inputView, nullptr, &err);
                } catch (...) {
                    XX_LOGW("[musicxx_ext] 插件 `{}` 异步处理器异常 (`{}`)", h.pluginId, hookId);
                }
                if (err.data) {
                    pluginxx::hostMemoryFree(err.data);
                }
            }
            ++called;
            continue;
        }

        const std::string&      inText = inputJson;
        PluginxxString          out{};
        PluginxxString          err{};
        PluginxxStringView      hookView{h.hookId.data(), h.hookId.size()};
        PluginxxStringView      inputView{inText.data(), inText.size()};
        const auto              t0 = std::chrono::steady_clock::now();
        int32_t                 rc = -1;
        try {
            rc = h.spec.hook_sync(h.spec.user_data, &hookView, &inputView, &out, &err);
        } catch (const std::exception& e) {
            XX_LOGW("[musicxx_ext] 插件 `{}` 处理器异常 (`{}`): {}", h.pluginId, hookId, e.what());
            rc = -1;
        } catch (...) {
            XX_LOGW("[musicxx_ext] 插件 `{}` 处理器未知异常 (`{}`)", h.pluginId, hookId);
            rc = -1;
        }
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0
        )
                            .count();
        ++h.calls;
        ++called;
        h.totalUs = h.totalUs + us;
        h.maxUs   = (std::max)(h.maxUs, us);
        if (auto instIt = loaded_.find(h.plugin); instIt != loaded_.end() && instIt->second) {
            instIt->second->hookCalls.fetch_add(1);
        }

        if (rc != 0) {
            ++h.failures;
            if (++h.consecutiveErrors >= 3) {
                h.pausedUntil       = std::chrono::steady_clock::now() + std::chrono::seconds{60};
                h.consecutiveErrors = 0;
                Json payload;
                payload["id"]      = h.pluginId;
                payload["phase"]   = "hook";
                payload["hook"]    = hookId;
                payload["code"]    = "handler_failed";
                payload["message"] = "处理器连续失败, 已临时暂停派发 60 秒";
                pushEvent("musicxx.plugin.error", h.pluginId, payload.dump());
            }
        } else {
            h.consecutiveErrors = 0;
            if (us > hardMs * 1000) {
                ++h.timeouts;
                Json payload;
                payload["id"]      = h.pluginId;
                payload["phase"]   = "hook";
                payload["hook"]    = hookId;
                payload["code"]    = "handler_slow";
                payload["message"] = "处理器耗时超过硬预算 (未打断插件, 仅记录统计)";
                pushEvent("musicxx.plugin.warn", h.pluginId, payload.dump());
            }
        }

        if (out.data) {
            const std::string_view text{out.data, static_cast<size_t>(out.size)};
            if (!text.empty() && text != "null") {
                Json result = parseJsonSafe(text);
                if (result.is_object()) {
                    if (result.contains("patch")) {
                        mergePatch(merged, result);
                        hasPatch = true;
                    }
                    const std::string a = actionOf(result);
                    if (!a.empty() && (action.empty() || action == MUSICXX_PLUGIN_HOOK_ACTION_CONTINUE)) {
                        action = a;
                    }
                    for (auto pit = result.begin(); pit != result.end(); ++pit) {
                        if (pit.key() != std::string{"patch"}) {
                            merged[pit.key()] = pit.value();
                        }
                    }
                }
            }
            pluginxx::hostMemoryFree(out.data);
        }
        if (err.data) {
            pluginxx::hostMemoryFree(err.data);
        }

        // 合并策略: firstNonNull / anyCancel 命中即停止询问后续处理器
        if (meta->policy == 0 /*firstNonNull*/ && (hasPatch || !action.empty())) {
            break;
        }
        if (meta->policy == 1 /*anyCancel*/
            && (action == MUSICXX_PLUGIN_HOOK_ACTION_CANCEL || action == MUSICXX_PLUGIN_HOOK_ACTION_SKIP)) {
            break;
        }
    }

    Json result;
    result["handled"]  = called > 0 && mode != HookDispatchMode::Notify;
    result["handlers"] = static_cast<int32_t>(handlers.size());
    result["called"]   = called;
    result["timedOut"] = timedOut;
    if (!action.empty()) {
        result["result"] = merged.is_object() ? merged : merged;
        result["result"]["action"] = action;
    } else if (hasPatch) {
        result["result"] = merged;
    }

    /// 观测回传 (plan §4.9 / 决策 10): 观察型钩子的执行镜像, 仅在调试开关打开时回传
    /// (默认关闭 → 零开销、零事件量)。它只用于调试/统计, 调用点与插件都不得依赖它。
    if (mode == HookDispatchMode::Notify
        && (flags_ & MUSICXX_EXTERN_PLUGIN_FLAG_DEBUG_OBSERVE_EVENTS) != 0) {
        Json mirror;
        mirror["hook"]     = hookId;
        mirror["input"]    = parseJsonSafe(inputJson);
        mirror["called"]   = called;
        mirror["handlers"] = static_cast<int32_t>(handlers.size());
        pushEvent("musicxx.hook.observe", "", mirror.dump());
    }
    return result.dump();
}

int32_t MusicxxHostManager::hookEmit(
    const std::string& hookId,
    const std::string& inputJson,
    bool               sync,
    uint32_t           timeoutMs,
    std::string&       outJson,
    std::string&       err
) {
    if (hookId.empty()) {
        err = "hook_emit: empty hook id";
        return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
    }
    if (!running_.load(std::memory_order_acquire)) {
        err = "hook_emit: host not started";
        return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
    const auto* meta = findHookMeta(hookId);
    if (!meta) {
        outJson = R"({"handled":false,"error":"unknown_hook"})";
        return MUSICXX_EXTERN_PLUGIN_OK;
    }
    {
        auto it = hooks_.find(hookId);
        if (it == hooks_.end() || it->second.empty()) {
            outJson = R"({"handled":false,"handlers":0})";
            return MUSICXX_EXTERN_PLUGIN_OK;
        }
    }

    /// 裁决型钩子的异步派发（plan §5.3）：不占用 Dart 线程，完成后经
    /// `musicxx.hook.decision.result` 事件把结果回传（payload 带 callId 供配对）。
    const bool asyncDecide = !sync && meta->mode == MUSICXX_PLUGIN_HOOK_MODE_DECISION;
    const HookDispatchMode mode = sync ? HookDispatchMode::Sync
                                       : (asyncDecide ? HookDispatchMode::AsyncDecide
                                                      : HookDispatchMode::Notify);
    const int64_t callId = asyncDecide ? nextHookCallId_.fetch_add(1, std::memory_order_relaxed) : 0;

    const uint32_t budget = timeoutMs > 0 ? timeoutMs : static_cast<uint32_t>(hookBudgetMs_);
    auto           slot   = std::make_shared<WaitSlot<std::string>>();
    auto           self   = shared_from_this();
    asio::post(ctx_->io, [self, slot, hookId, inputJson, budget, mode, callId]() {
        std::string result = self->dispatchHook(hookId, inputJson, budget, mode);
        if (mode == HookDispatchMode::AsyncDecide) {
            Json payload      = parseJsonSafe(result);
            payload["callId"] = callId;
            payload["hook"]   = hookId;
            self->pushEvent("musicxx.hook.decision.result", "", payload.dump());
            return;
        }
        slot->set(std::move(result));
    });

    if (!sync) {
        Json ack;
        ack["handled"] = true;
        ack["async"]   = true;
        if (asyncDecide) {
            ack["callId"] = callId;
        }
        outJson = ack.dump();
        return MUSICXX_EXTERN_PLUGIN_OK;
    }
    std::string result;
    if (!slot->wait(budget + static_cast<uint32_t>(hookHardMs_) + 20, result)) {
        outJson = R"({"handled":false,"timedOut":true})";
        err     = "hook_emit: 派发未在预算内完成";
        XX_LOGW("[musicxx_ext] 钩子 `{}` 派发超时 (按无裁决继续)", hookId);
        return MUSICXX_EXTERN_PLUGIN_OK; ///< 超时按"无裁决"处理, 不是错误
    }
    outJson = std::move(result);
    return MUSICXX_EXTERN_PLUGIN_OK;
}

/* ==================== 宿主动作请求 (插件 → Dart) ==================== */

int32_t MusicxxHostManager::requestAction(
    MusicxxHostInstance*                    inst,
    std::string_view                        action,
    std::string_view                        argsJson,
    uint32_t                                timeoutMs,
    const PluginxxOperatorNotify* notify,
    int64_t*                                outRequestId
) {
    if (!inst || action.empty()) {
        return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
    }
    const std::string actionName{action};
    const std::string pluginId = pluginIdOf(inst->name);
    // 命名空间校验 (plan §3.5): 官方 musicxx.* 或本插件自己的 plugin.<id>.*
    if (!isOfficial(actionName) && !isOwnPlugin(actionName, pluginId)) {
        XX_LOGW("[musicxx_ext] 插件 `{}` 发起非法动作名 `{}` (命名空间校验失败)", pluginId, actionName);
        return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
    }

    const int64_t requestId = nextRequestId_++;
    // 超时预算 (plan §4.8): 默认 5s, 下限 1s (避免 Dart 侧被高频请求淹没), 上限 60s
    const int64_t effectiveTimeoutMs = std::clamp<int64_t>(
        (timeoutMs == 0) ? 5000 : static_cast<int64_t>(timeoutMs),
        1000,
        60000
    );
    PendingAction pending;
    pending.plugin   = inst->name;
    pending.action   = actionName;
    pending.notify   = notify;
    pending.deadline = std::chrono::steady_clock::now()
                       + std::chrono::milliseconds{effectiveTimeoutMs};
    // 超时保护 (plan §4.8): 到点仍未收到 Dart 回复就终结 op, 并推事件让 Dart 中断在做的活
    if (ctx_) {
        auto timer = std::make_shared<asio::steady_timer>(ctx_->io);
        timer->expires_after(std::chrono::milliseconds{effectiveTimeoutMs});
        timer->async_wait([this, requestId](const utilxx_base::AsioErrorCode& ec) {
            if (ec) {
                return; ///< 已回复/已取消: 定时器被主动取消
            }
            auto it = pendingActions_.find(requestId);
            if (it == pendingActions_.end()) {
                return;
            }
            const std::string pluginId = pluginIdOf(it->second.plugin);
            if (it->second.notify && it->second.notify->done) {
                it->second.notify->done(
                    it->second.notify->host_ud,
                    PLUGINXX_OPERATOR_CANCELLED,
                    nullptr
                );
            }
            XX_LOGW(
                "[musicxx_ext] 动作请求超时: plugin={} action={} id={}",
                pluginId,
                it->second.action,
                requestId
            );
            Json payload;
            payload["requestId"] = requestId;
            payload["reason"]    = "timeout";
            pushEvent("musicxx.action.cancel", pluginId, payload.dump());
            pendingActions_.erase(it);
        });
        pending.timer = std::move(timer);
    }
    pendingActions_[requestId] = std::move(pending);
    if (outRequestId) {
        *outRequestId = requestId;
    }
    inst->actionRequests.fetch_add(1);

    Json payload;
    payload["requestId"] = requestId;
    payload["plugin"]    = pluginId;
    payload["action"]    = actionName;
    payload["timeoutMs"] = effectiveTimeoutMs;
    Json args            = parseJsonSafe(argsJson);
    payload["args"]      = args;
    pushEvent("musicxx.action.request", pluginId, payload.dump());
    return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::actionRespond(
    int64_t            requestId,
    int32_t            status,
    const std::string& resultJson
) {
    auto it = pendingActions_.find(requestId);
    if (it == pendingActions_.end()) {
        // 不属于原生插件登记表: 可能是 JS 引擎登记的在途请求 (见 InternalActionRelay)
        std::shared_ptr<InternalActionRelay> relay;
        {
            std::lock_guard<std::mutex> lock{registryMutex_};
            relay = internalRelay_;
        }
        if (relay && relay->respondAction(requestId, status, resultJson)) {
            return MUSICXX_EXTERN_PLUGIN_OK;
        }
        return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
    }
    if (it->second.timer) {
        it->second.timer->cancel(); ///< 已回复: 撤销超时保护
    }
    if (it->second.notify && it->second.notify->done) {
        PluginxxString payload{};
        if (status == 0 && !resultJson.empty()) {
            payload = pluginxx::hostMemoryCreateString(std::string_view{resultJson});
        }
        const PluginxxStringView view{payload.data, payload.size};
        it->second.notify->done(it->second.notify->host_ud, status, &view);
        if (payload.data) {
            pluginxx::hostMemoryFree(payload.data);
        }
    }
    pendingActions_.erase(it);
    return MUSICXX_EXTERN_PLUGIN_OK;
}

void MusicxxHostManager::cancelAction(int64_t requestId) {
    auto it = pendingActions_.find(requestId);
    if (it == pendingActions_.end()) {
        return;
    }
    if (it->second.timer) {
        it->second.timer->cancel();
    }
    if (it->second.notify && it->second.notify->done) {
        it->second.notify->done(it->second.notify->host_ud, PLUGINXX_OPERATOR_CANCELLED, nullptr);
    }
    Json payload;
    payload["requestId"] = requestId;
    pushEvent("musicxx.action.cancel", pluginIdOf(it->second.plugin), payload.dump());
    pendingActions_.erase(it);
}

/* ==================== 状态镜像读取 ==================== */

int32_t MusicxxHostManager::getState(std::string_view key, std::string& outJson) {
    std::lock_guard<std::mutex> lock{stateMutex_};
    auto                        it = state_.find(std::string{key});
    if (it == state_.end()) {
        return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
    }
    outJson = it->second;
    return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::subscribeState(
    MusicxxHostInstance* inst,
    const std::string&   keysJson,
    int32_t&             outCount
) {
    if (!inst) {
        return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
    }
    bool parseOk = false;
    Json keys    = parseJsonSafe(keysJson, &parseOk);
    if (!parseOk || !keys.is_array()) {
        return MUSICXX_EXTERN_PLUGIN_ERR_JSON;
    }
    std::vector<std::string> list;
    for (const auto& item : keys) {
        if (item.is_string()) {
            list.push_back(item.get<std::string>());
        }
    }
    stateSubscriptions_[inst->name] = std::move(list);
    outCount = static_cast<int32_t>(stateSubscriptions_[inst->name].size());
    return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::pluginConfigPathFor(MusicxxHostInstance* inst, std::string& out) {
    if (!inst) {
        return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
    }
    if (!inst->configPath.empty()) {
        out = inst->configPath;
        return MUSICXX_EXTERN_PLUGIN_OK;
    }
    out = deriveConfigPath(pluginIdOf(inst->name), {});
    return out.empty() ? MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND : MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::hostInfoJson(std::string& outJson) {
    Json j;
    j["appVersion"]       = appVersion_;
    j["platform"]         = platform_;
    j["language"]         = language_;
    j["dataDir"]          = dataDir_;
    j["userPluginDir"]    = userPluginDir_;
    j["builtinPluginDir"] = builtinPluginDir_;
    j["apiVersion"]       = MUSICXX_PLUGIN_API_VERSION;
    j["hostVersion"]      = "0.1.0";
    outJson               = j.dump();
    return MUSICXX_EXTERN_PLUGIN_OK;
}

/* ==================== 领域接口表 (C ABI 函数 + vtable) ==================== */

namespace {


int32_t PLUGINXX_CALL xx_register_hook(
    const PluginxxHost*          host,
    const MusicxxPluginHookSpec* spec
) {
    return pluginxx::guardVtableCall(MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        auto call = pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host);
        if (!call.ok() || !spec) {
            return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto        mgr     = call.manager();
        auto        inst    = call.instance();
        const auto  specCopy = *spec;
        return pluginxx::ioCallSyncKeep<int32_t>(call, mgr, [mgr, inst, specCopy]() -> int32_t {
            return mgr->registerHook(inst, specCopy);
        });
    });
}

int32_t PLUGINXX_CALL xx_unregister_hook(
    const PluginxxHost*       host,
    const PluginxxStringView* hookId,
    const PluginxxStringView* ownerTag
) {
    return pluginxx::guardVtableCall(MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        auto call = pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host);
        if (!call.ok() || !hookId || !hookId->data) {
            return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto             mgr = call.manager();
        auto             inst = call.instance();
        const std::string_view id{hookId->data, static_cast<size_t>(hookId->size)};
        const std::string_view tag = (ownerTag && ownerTag->data)
                                         ? std::string_view{ownerTag->data, static_cast<size_t>(ownerTag->size)}
                                         : std::string_view{};
        return pluginxx::ioCallSyncKeep<int32_t>(call, mgr, [mgr, inst, id, tag]() -> int32_t {
            return mgr->unregisterHook(inst, id, tag);
        });
    });
}

int32_t PLUGINXX_CALL xx_hook_info(
    const PluginxxHost*       host,
    const PluginxxStringView* hookId,
    PluginxxString*           outJson
) {
    return pluginxx::guardVtableCall(MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!hookId || !hookId->data || !outJson) {
            return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto call = pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host, true);
        if (!call.ok()) {
            return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto                    mgr = call.manager();
        const std::string_view  id{hookId->data, static_cast<size_t>(hookId->size)};
        std::string             json;
        const int32_t           rc = pluginxx::ioCallSyncKeep<int32_t>(call, mgr, [mgr, id, &json]() -> int32_t {
            return mgr->hookInfoJson(id, json);
        });
        if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
            return rc;
        }
        pluginxx::hostMemorySetString(outJson, json);
        return MUSICXX_EXTERN_PLUGIN_OK;
    });
}

int32_t PLUGINXX_CALL xx_host_get_info(const PluginxxHost* host, PluginxxString* outJson) {
    return pluginxx::guardVtableCall(MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!outJson) {
            return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto call = pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host, true);
        if (!call.ok()) {
            return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto          mgr = call.manager();
        std::string   json;
        const int32_t rc = pluginxx::ioCallSyncKeep<int32_t>(call, mgr, [mgr, &json]() -> int32_t {
            return mgr->hostInfoJson(json);
        });
        if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
            return rc;
        }
        pluginxx::hostMemorySetString(outJson, json);
        return MUSICXX_EXTERN_PLUGIN_OK;
    });
}

int32_t PLUGINXX_CALL xx_host_get_state(
    const PluginxxHost*       host,
    const PluginxxStringView* key,
    PluginxxString*           outJson
) {
    return pluginxx::guardVtableCall(MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!key || !key->data || !outJson) {
            return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto call = pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host, true);
        if (!call.ok()) {
            return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto                   mgr = call.manager();
        const std::string_view name{key->data, static_cast<size_t>(key->size)};
        std::string            json;
        const int32_t          rc = pluginxx::ioCallSyncKeep<int32_t>(call, mgr, [mgr, name, &json]() -> int32_t {
            return mgr->getState(name, json);
        });
        if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
            return rc;
        }
        pluginxx::hostMemorySetString(outJson, json);
        return MUSICXX_EXTERN_PLUGIN_OK;
    });
}

int32_t PLUGINXX_CALL xx_host_subscribe_state(
    const PluginxxHost*       host,
    const PluginxxStringView* keysJson,
    int32_t*                  outCount
) {
    return pluginxx::guardVtableCall(MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        auto call = pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host);
        if (!call.ok()) {
            return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto                  mgr = call.manager();
        auto                  inst = call.instance();
        const std::string     keys = keysJson && keysJson->data
                                         ? std::string{keysJson->data, static_cast<size_t>(keysJson->size)}
                                         : std::string{"[]"};
        int32_t               count = 0;
        const int32_t         rc = pluginxx::ioCallSyncKeep<int32_t>(call, mgr, [mgr, inst, keys, &count]() -> int32_t {
            return mgr->subscribeState(inst, keys, count);
        });
        if (outCount) {
            *outCount = count;
        }
        return rc;
    });
}

int32_t PLUGINXX_CALL xx_host_config_path(const PluginxxHost* host, PluginxxString* outPath) {
    return pluginxx::guardVtableCall(MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!outPath) {
            return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto call = pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host, true);
        if (!call.ok()) {
            return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto          mgr = call.manager();
        auto          inst = call.instance();
        std::string   path;
        const int32_t rc = pluginxx::ioCallSyncKeep<int32_t>(call, mgr, [mgr, inst, &path]() -> int32_t {
            return mgr->pluginConfigPathFor(inst, path);
        });
        if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
            return rc;
        }
        pluginxx::hostMemorySetString(outPath, path);
        return MUSICXX_EXTERN_PLUGIN_OK;
    });
}

int32_t PLUGINXX_CALL xx_host_request_action(
    const PluginxxHost*               host,
    const PluginxxStringView*         action,
    const PluginxxStringView*         argsJson,
    uint32_t                          timeoutMs,
    const PluginxxOperatorNotify*     notify,
    int64_t*                          outRequestId
) {
    return pluginxx::guardVtableCall(MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        auto call = pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host);
        if (!call.ok() || !action || !action->data) {
            return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto                  mgr  = call.manager();
        auto                  inst = call.instance();
        const std::string_view actionName{action->data, static_cast<size_t>(action->size)};
        const std::string      args = argsJson && argsJson->data
                                        ? std::string{argsJson->data, static_cast<size_t>(argsJson->size)}
                                        : std::string{"{}"};
        int64_t                requestId = 0;
        const int32_t          rc = pluginxx::ioCallSyncKeep<int32_t>(
            call,
            mgr,
            [mgr, inst, actionName, args, timeoutMs, notify, outRequestId, &requestId]() -> int32_t {
                return mgr->requestAction(
                    inst,
                    actionName,
                    args,
                    timeoutMs,
                    notify,
                    outRequestId ? &requestId : nullptr
                );
            }
        );
        if (rc == MUSICXX_EXTERN_PLUGIN_OK && outRequestId) {
            *outRequestId = requestId;
        }
        return rc;
    });
}

void PLUGINXX_CALL xx_host_cancel_action(const PluginxxHost* host, int64_t requestId) {
    pluginxx::guardVtableCallVoid([&] {
        auto call = pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host);
        if (!call.ok()) {
            return;
        }
        auto mgr = call.manager();
        pluginxx::ioCallSyncVoidKeep(call, mgr, [mgr, requestId]() { mgr->cancelAction(requestId); });
    });
}

const MusicxxPluginHooksIface g_ifaceHooks = {
    /* version */ MUSICXX_PLUGIN_IFACE_HOOKS_VERSION,
    /* struct_size */ sizeof(MusicxxPluginHooksIface),
    /* register_hook */ xx_register_hook,
    /* unregister_hook */ xx_unregister_hook,
    /* hook_info */ xx_hook_info,
};

const MusicxxPluginHostIface g_ifaceHost = {
    /* version */ MUSICXX_PLUGIN_IFACE_HOST_VERSION,
    /* struct_size */ sizeof(MusicxxPluginHostIface),
    /* get_info */ xx_host_get_info,
    /* get_state */ xx_host_get_state,
    /* subscribe_state */ xx_host_subscribe_state,
    /* get_plugin_config_path */ xx_host_config_path,
    /* request_action */ xx_host_request_action,
    /* cancel_action */ xx_host_cancel_action,
};

void* PLUGINXX_CALL xx_alloc_fn(uint64_t size) {
    return pluginxx::hostMemoryAlloc(size);
}

void PLUGINXX_CALL xx_free_fn(void* ptr) {
    pluginxx::hostMemoryFree(ptr);
}

/* ==================== 事件表 (自定义入口: 增加发布归属校验) ==================== */

/// 订阅事件 (任意线程; 内核入口语义 + 主题合法性校验)
PluginxxSubscription* PLUGINXX_CALL xx_subscribe_event(
    const PluginxxHost*       host,
    const PluginxxStringView* topic,
    void(PLUGINXX_CALL* handler)(const PluginxxStringView* event_json, void* ud),
    void* ud
) {
    return pluginxx::guardVtableCall(nullptr, [&]() -> PluginxxSubscription* {
        auto call = pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host);
        if (!call.ok() || !topic || !topic->data || !handler) {
            return nullptr;
        }
        auto              mgr       = call.manager();
        auto              inst      = call.instance();
        const std::string topicText = std::string{topic->data, static_cast<size_t>(topic->size)};
        // 订阅只要求主题形状合法 (可以订阅其它插件或官方主题); 发布才做归属校验
        if (mgr->qualifyEventTopic(topicText).empty()) {
            XX_LOGW("[musicxx_ext] 插件 `{}` 订阅非法主题 `{}` 被拒绝", inst->name, topicText);
            return nullptr;
        }
        return pluginxx::ioCallSyncKeep<PluginxxSubscription*>(
            call,
            mgr,
            [mgr, inst, topicText, handler, ud]() {
                return mgr->subscribe(inst, topicText, handler, ud);
            }
        );
    });
}

/// 撤销订阅 (幂等; 任意线程)
void PLUGINXX_CALL xx_unsubscribe_event(PluginxxSubscription* sub) {
    pluginxx::unsubscribePluginSubscription(sub);
}

/// 发布事件 (任意线程): 主题必须是官方 `musicxx.*` 或本插件自己的 `plugin.<id>.*`
int32_t PLUGINXX_CALL xx_publish_event(
    const PluginxxHost*       host,
    const PluginxxStringView* topic,
    const PluginxxStringView* event_json
) {
    return pluginxx::guardVtableCall(-1, [&]() -> int32_t {
        auto call = pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host);
        if (!call.ok() || !topic || !topic->data || !event_json) {
            return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto              mgr       = call.manager();
        auto              inst      = call.instance();
        const std::string topicText = std::string{topic->data, static_cast<size_t>(topic->size)};
        const std::string pluginId  = MusicxxHostManager::pluginIdOf(inst->name);
        if (!isTopicOwnedBy(topicText, pluginId)) {
            XX_LOGW(
                "[musicxx_ext] 插件 `{}` 发布事件被拒绝: 主题 `{}` 不属于该插件命名空间",
                pluginId,
                topicText
            );
            return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
        }
        const std::string payload = (event_json->size > 0 && event_json->data)
                                        ? std::string{event_json->data, static_cast<size_t>(event_json->size)}
                                        : std::string{"{}"};
        return pluginxx::ioCallSyncKeep<int32_t>(call, mgr, [mgr, topicText, payload]() -> int32_t {
            return mgr->publishOnHostThread(topicText, payload);
        });
    });
}

const PluginxxEventsIface g_ifaceEvents = {
    /* version */ PLUGINXX_IFACE_EVENTS_VERSION,
    /* struct_size */ sizeof(PluginxxEventsIface),
    /* subscribe */ xx_subscribe_event,
    /* unsubscribe */ xx_unsubscribe_event,
    /* publish */ xx_publish_event,
};

const void* PLUGINXX_CALL xx_query_interface(const PluginxxHost*, const PluginxxStringView* iid);

const PluginxxHostVtable g_hostVtable = {
    /* alloc */ xx_alloc_fn,
    /* free */ xx_free_fn,
    /* query_interface */ xx_query_interface,
};

const void* PLUGINXX_CALL xx_query_interface(const PluginxxHost*, const PluginxxStringView* iid) {
    if (!iid || !iid->data) {
        return nullptr;
    }
    const std::string_view name{iid->data, static_cast<size_t>(iid->size)};
    if (name == "__vtable") {
        return &g_hostVtable;
    }
    // events 表用本宿主自己的入口 (需要在发布路径上做命名空间归属校验, 见 xx_publish_event)
    if (name == PLUGINXX_IFACE_EVENTS) {
        return &g_ifaceEvents;
    }
    // 其余通用表 (log/json/config/plugins/scheduler/coroutine_runtime/tasks/cancel/
    // capabilities) 由内核提供; 领域表在下面分发
    if (const void* generic
        = pluginxx::queryGenericPluginIface<MusicxxHostInstance, MusicxxHostManager>(name)) {
        return generic;
    }
    if (name == MUSICXX_PLUGIN_IFACE_HOOKS) {
        return &g_ifaceHooks;
    }
    if (name == MUSICXX_PLUGIN_IFACE_HOST) {
        return &g_ifaceHost;
    }
    if (name == MUSICXX_PLUGIN_IFACE_UI) {
        return uiIfaceForQuery();
    }
    // 预留表 (musicxx.player/library/lyrics/storage/net/stats/agent): v1 返回空,
    // 插件据此判空并降级 (IID 已冻结, 后续实现不再改动契约)
    return nullptr;
}

} // namespace

const PluginxxHostVtable* MusicxxHostManager::hostVtable() {
    return &g_hostVtable;
}

} // namespace extern_plugin
} // namespace musicxx
