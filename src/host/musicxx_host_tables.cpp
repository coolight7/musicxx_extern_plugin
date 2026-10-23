/// musicxx 外部插件原生插件框架: 专用接口表 (musicxx.hooks / musicxx.host)
/// 与钩子派发
///
/// 线程约定: 表入口在 Dart 线程被调用, 内部投递到框架线程并等待;
/// 已在框架线程时就地执行 (插件框架 `ioCallSyncKeep` 语义)。

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
#include <cstddef>

namespace musicxx {
namespace extern_plugin {

using utilxx_base::Json;

namespace {

/// 已知钩子表 (与钩子总表一致)
///
/// 定义只有一处: `tools/hooks.def.json` → `tools/gen_contract.dart` 生成
/// `musicxx/plugin/api/hook_ids.g.h`（插件框架与插件作者共用同一份）。
/// policy: MUSICXX_PLUGIN_HOOK_POLICY_*；mode: MUSICXX_PLUGIN_HOOK_MODE_*。
using HookMeta = musicxx::plugin::MusicxxPluginHookMeta;

const HookMeta *findHookMeta(std::string_view hookId) {
  for (size_t i = 0; i < musicxx::plugin::musicxxPluginKnownHookCount; ++i) {
    const auto &meta = musicxx::plugin::musicxxPluginKnownHooks[i];
    if (hookId == meta.id) {
      return &meta;
    }
  }
  return nullptr;
}

/// 命名空间规则 单一实现在 host_naming.h
///
/// 说明: 动作名不再做归属校验 (插件可以调任意名字的动作), 这里只保留事件主题的
/// 归属判定 (发布事件仍要求 `musicxx.*` 或自己命名空间)。
using naming::isTopicOwnedBy;

} // namespace

namespace {

/// 阶段编号是否合法 (`impl` / `before` / `after`)
bool isValidPhase(int32_t phase) {
  return phase >= MUSICXX_PLUGIN_HOOK_PHASE_IMPL &&
         phase <= MUSICXX_PLUGIN_HOOK_PHASE_AFTER;
}

/// 阶段编号 → 对外字符串 (`impl` / `before` / `after`; 非法编号给空串)
const char *phaseNameOf(int32_t phase) {
  switch (phase) {
  case MUSICXX_PLUGIN_HOOK_PHASE_IMPL:
    return "impl";
  case MUSICXX_PLUGIN_HOOK_PHASE_BEFORE:
    return "before";
  case MUSICXX_PLUGIN_HOOK_PHASE_AFTER:
    return "after";
  default:
    return "";
  }
}

} // namespace

namespace {

/// 把内部拼好的 JSON 数组文本转成 Json（定义见文件下方）
///
/// 这段文本由 `hookHandlersArrayJson` 自己构造，正常不会解析失败；真失败时退回空数组
/// （本项目的 Json::parse 没有"不抛异常"的重载，所以用 try/catch）。
Json jsonArrayOf(const std::string &text);

} // namespace

/* ==================== 钩子注册 / 注销 ==================== */

int32_t MusicxxHostManager::registerHook(MusicxxHostInstance *inst,
                                         const MusicxxPluginHookSpec &spec) {
  if (!inst) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  /// 规格版本: 接受 1 (老结构, 没有 phase) 与 2 (带 phase)
  if (spec.version != 1 && spec.version != MUSICXX_PLUGIN_HOOK_SPEC_VERSION) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册钩子被拒绝: 规格版本 {} 不认识",
            inst->name, spec.version);
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  /// struct_size: 0 或 >= 老结构大小都接受 (老插件); 大于旧值的部分忽略
  if (spec.struct_size != 0 &&
      spec.struct_size < MUSICXX_PLUGIN_HOOK_SPEC_V1_SIZE) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  /// phase 只在结构体覆盖到它时可信 (老插件没这个成员 → 按 impl)
  const bool hasPhaseField =
      spec.struct_size >=
      (offsetof(MusicxxPluginHookSpec, phase) + sizeof(int32_t));
  if (spec.version >= 2 && !hasPhaseField) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 声明规格版本 2, 但 struct_size 没覆盖 "
            "phase (注册被拒绝)",
            inst->name);
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  const int32_t phase =
      hasPhaseField ? spec.phase : MUSICXX_PLUGIN_HOOK_PHASE_IMPL;
  if (!isValidPhase(phase)) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册钩子被拒绝: 阶段 {} 不合法",
            inst->name, phase);
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (!spec.hook_id.data || spec.hook_id.size == 0) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  const std::string hookId{spec.hook_id.data,
                           static_cast<size_t>(spec.hook_id.size)};
  if (!spec.hook_sync && !spec.hook_start) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  /// 前链 / 后链必须是同步处理器: 要拿到这一步的结果才能跑下一步
  if (phase != MUSICXX_PLUGIN_HOOK_PHASE_IMPL && !spec.hook_sync) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册钩子 `{}` 的 {} 阶段被拒绝: "
            "阶段处理器必须是同步处理器 (hook_start 无法逐步串行)",
            inst->name, hookId, phaseNameOf(phase));
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (inst->unloadRequested || !inst->enabled) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册钩子被拒绝: 实例正在关闭或已禁用",
            inst->name);
    return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  }
  // 未知钩子拒绝注册 (硬规则一): 插件不得制造插件框架不认识的钩子点
  const auto *meta = findHookMeta(hookId);
  if (!meta) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册未知钩子 `{}` 被拒绝", inst->name,
            hookId);
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  // 只接受钩子声明过的阶段: 注册了不派发的阶段等于给插件一个永远不触发的坑
  if ((meta->phases & musicxxPluginHookPhaseMask(phase)) == 0) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册钩子 `{}` 的 {} 阶段被拒绝: "
            "这个钩子没有声明该阶段",
            inst->name, hookId, phaseNameOf(phase));
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (meta->mode == MUSICXX_PLUGIN_HOOK_MODE_DECISION &&
      spec.mode == MUSICXX_PLUGIN_HOOK_MODE_OBSERVE) {
    // 决定型钩子允许以观察型处理器注册: 只观察, 不参与裁决 (handler.spec.mode
    // 决定派发行为)
  }

  const std::string ownerTag =
      spec.owner_tag.data && spec.owner_tag.size > 0
          ? std::string{spec.owner_tag.data,
                        static_cast<size_t>(spec.owner_tag.size)}
          : std::string{};

  auto &handlers = hooks_[hookId];
  // 同一个 (实例, hook_id, owner_tag, 阶段) 覆盖式注册。
  // 同一个 ownerTag 可以分别登记 before / impl / after —— 三者互不覆盖。
  handlers.erase(std::remove_if(handlers.begin(), handlers.end(),
                                [&](const HookHandler &h) {
                                  return h.plugin == inst->name &&
                                         h.ownerTag == ownerTag &&
                                         h.phase == phase;
                                }),
                 handlers.end());

  HookHandler h;
  h.plugin = inst->name;
  h.pluginId = pluginIdOf(inst->name);
  h.ownerTag = ownerTag;
  h.hookId = hookId;
  h.spec = spec;
  h.phase = phase;
  h.seq = ++hookSeq_;
  handlers.push_back(h);

  XX_LOGI("[musicxx_ext] 插件 `{}` 注册钩子 `{}` (phase={}, mode={}, "
          "priority={}, 当前处理器数={})",
          inst->name, hookId, phaseNameOf(phase), spec.mode, spec.priority,
          handlers.size());

  // 通知 Dart 侧刷新"该钩子是否有处理器"的位图与各阶段的处理器列表
  pushEvent("musicxx.hook.changed", h.pluginId,
            hookChangedPayloadJson(hookId, "register", h.pluginId));
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::unregisterHook(MusicxxHostInstance *inst,
                                           std::string_view hookId,
                                           std::string_view ownerTag) {
  if (hookId.empty()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  auto it = hooks_.find(std::string{hookId});
  if (it == hooks_.end()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  const std::string pluginName = inst ? inst->name : std::string{};
  const std::string tag{ownerTag};
  // 一个 (实例, ownerTag) 在所有阶段的登记一起摘掉 (注销函数不区分阶段)
  it->second.erase(std::remove_if(it->second.begin(), it->second.end(),
                                  [&](const HookHandler &h) {
                                    return h.plugin == pluginName &&
                                           h.ownerTag == tag;
                                  }),
                   it->second.end());
  if (it->second.empty()) {
    hooks_.erase(it);
  }
  const std::string hookText{hookId};
  const std::string pluginId = inst ? pluginIdOf(inst->name) : std::string{};
  pushEvent("musicxx.hook.changed", pluginId,
            hookChangedPayloadJson(hookText, "unregister", pluginId));
  return MUSICXX_EXTERN_PLUGIN_OK;
}

std::string MusicxxHostManager::hookChangedPayloadJson(
    const std::string &hookId, const char *action,
    const std::string &pluginId) {
  auto it = hooks_.find(hookId);
  const int32_t total =
      (it == hooks_.end()) ? 0 : static_cast<int32_t>(it->second.size());
  Json counts = Json::object();
  for (int32_t phase = MUSICXX_PLUGIN_HOOK_PHASE_IMPL;
       phase <= MUSICXX_PLUGIN_HOOK_PHASE_AFTER; ++phase) {
    counts[phaseNameOf(phase)] = countOnPhase(hookId, phase);
  }
  Json payload;
  payload["id"] = pluginId;
  payload["hook"] = hookId;
  payload["count"] = total;
  payload["phaseCount"] = counts;
  payload["action"] = action;
  // 一并带上处理器列表 (一个钩子通常 0~3 个, 注册/注销是低频操作):
  // Dart 侧不用再回查插件框架就能知道谁注册了、排第几、在哪个阶段
  payload["handlers"] = jsonArrayOf(hookHandlersArrayJson(hookId));
  return payload.dump();
}

int32_t MusicxxHostManager::countOnPhase(const std::string &hookId,
                                         int32_t phase) const {
  auto it = hooks_.find(hookId);
  if (it == hooks_.end()) {
    return 0;
  }
  int32_t count = 0;
  for (const HookHandler &h : it->second) {
    if (h.phase == phase) {
      ++count;
    }
  }
  return count;
}

int32_t MusicxxHostManager::hookInfoJson(std::string_view hookId,
                                         std::string &outJson) {
  const auto *meta = findHookMeta(hookId);
  if (!meta) {
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  const std::string hookText{hookId};
  Json j;
  j["id"] = hookText;
  j["mode"] = meta->mode;
  j["policy"] = meta->policy;
  // 这个钩子声明了哪几个阶段 (注册时只能选这里有的阶段)
  Json phases = Json::array();
  for (int32_t phase = MUSICXX_PLUGIN_HOOK_PHASE_IMPL;
       phase <= MUSICXX_PLUGIN_HOOK_PHASE_AFTER; ++phase) {
    if ((meta->phases & musicxxPluginHookPhaseMask(phase)) != 0) {
      phases.push_back(phaseNameOf(phase));
    }
  }
  j["phases"] = phases;
  auto it = hooks_.find(hookText);
  j["handlers"] =
      (it == hooks_.end()) ? 0 : static_cast<int32_t>(it->second.size());
  Json counts = Json::object();
  for (int32_t phase = MUSICXX_PLUGIN_HOOK_PHASE_IMPL;
       phase <= MUSICXX_PLUGIN_HOOK_PHASE_AFTER; ++phase) {
    counts[phaseNameOf(phase)] = countOnPhase(hookText, phase);
  }
  j["phaseCount"] = counts;
  outJson = j.dump();
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::hookCount(const std::string &hookId,
                                      int32_t &outCount) {
  auto it = hooks_.find(hookId);
  outCount = (it == hooks_.end()) ? 0 : static_cast<int32_t>(it->second.size());
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::hookCountPhase(const std::string &hookId,
                                           int32_t phase, int32_t &outCount) {
  if (!isValidPhase(phase)) {
    outCount = 0;
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  outCount = countOnPhase(hookId, phase);
  return MUSICXX_EXTERN_PLUGIN_OK;
}

std::vector<std::string>
MusicxxHostManager::hooksOfInstance(const std::string &instanceName) const {
  std::vector<std::string> ids;
  if (instanceName.empty()) {
    return ids;
  }
  std::set<std::string> seen;
  for (const auto &[hookId, handlers] : hooks_) {
    for (const HookHandler &h : handlers) {
      if (h.plugin != instanceName) {
        continue;
      }
      /// 同一个钩子上可能有多个处理器（例如 enabled/disabled 各注册一次）：
      /// 这里的列表是"该实例碰过哪些钩子"，每个 id 只列一次
      if (seen.insert(hookId).second) {
        ids.push_back(hookId);
      }
      break;
    }
  }
  return ids;
}

int32_t MusicxxHostManager::hookStatsJson(std::string &outJson) {
  Json hooks = Json::object();
  for (const auto &[hookId, handlers] : hooks_) {
    Json arr = Json::array();
    for (const auto &h : handlers) {
      arr.push_back({
          {"plugin", h.pluginId},
          {"ownerTag", h.ownerTag},
          {"mode", h.spec.mode},
          {"phase", phaseNameOf(h.phase)},
          {"priority", h.spec.priority},
          {"calls", h.calls},
          {"failures", h.failures},
          {"avgUs", h.calls > 0 ? h.totalUs / h.calls : 0},
          {"maxUs", h.maxUs},
      });
    }
    hooks[hookId] = arr;
  }
  Json j;
  j["hooks"] = hooks;
  outJson = j.dump();
  return MUSICXX_EXTERN_PLUGIN_OK;
}

namespace {

/// 把内部拼好的 JSON 数组文本转成 Json
///
/// 这段文本由 `hookHandlersArrayJson` 自己构造，正常不会解析失败；真失败时退回空数组，
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

std::string MusicxxHostManager::hookHandlersArrayJson(std::string_view hookId,
                                                      int32_t phase) const {
  auto it = hooks_.find(std::string{hookId});
  if (it == hooks_.end() || it->second.empty()) {
    return "[]";
  }
  // 顺序必须与真实调用顺序一致 (前链正序: priority 升序, 同优先级按注册顺序),
  // 否则 Dart 侧自省里看到的"排第几"对不上。
  // `phase < 0` = 全部阶段 (按同一个正序排, 后链的逆序由调用方自己读 phase 判断)。
  std::vector<const HookHandler *> ordered;
  ordered.reserve(it->second.size());
  for (const HookHandler &h : it->second) {
    if (phase < 0 || h.phase == phase) {
      ordered.push_back(&h);
    }
  }
  if (ordered.empty()) {
    return "[]";
  }
  std::stable_sort(ordered.begin(), ordered.end(),
                   [](const HookHandler *a, const HookHandler *b) {
                     if (a->spec.priority != b->spec.priority) {
                       return a->spec.priority < b->spec.priority;
                     }
                     return a->seq < b->seq;
                   });
  Json arr = Json::array();
  for (const HookHandler *h : ordered) {
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

int32_t MusicxxHostManager::hookHandlersJson(std::string_view hookId,
                                             std::string &outJson) {
  return hookHandlersJsonPhase(hookId, -1, outJson);
}

int32_t MusicxxHostManager::hookHandlersJsonPhase(std::string_view hookId,
                                                  int32_t phase,
                                                  std::string &outJson) {
  if (hookId.empty()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (phase < -1 || !isValidPhase(phase == -1 ? 0 : phase)) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (!findHookMeta(hookId)) {
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  const std::string hookText{hookId};
  auto it = hooks_.find(hookText);
  Json j;
  j["hook"] = hookText;
  if (phase < 0) {
    j["count"] = (it == hooks_.end()) ? 0
                                      : static_cast<int32_t>(it->second.size());
  } else {
    j["phase"] = phaseNameOf(phase);
    j["count"] = countOnPhase(hookText, phase);
  }
  j["handlers"] = jsonArrayOf(hookHandlersArrayJson(hookText, phase));
  outJson = j.dump();
  return MUSICXX_EXTERN_PLUGIN_OK;
}

/* ==================== 钩子派发 ==================== */

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

std::string MusicxxHostManager::dispatchHook(const std::string &hookId,
                                             int32_t phase,
                                             const std::string &inputJson,
                                             HookDispatchMode mode) {
  const auto *meta = findHookMeta(hookId);
  if (!meta) {
    return R"({"handled":false,"error":"unknown_hook"})";
  }
  auto it = hooks_.find(hookId);
  if (it == hooks_.end() || it->second.empty()) {
    return R"({"handled":false,"handlers":0})";
  }

  /// 派发前**快照**本阶段的处理器列表 (派发期间注册表变化不影响本轮遍历)
  ///
  /// 插件处理器在框架线程上**就地执行** (`ioCallSyncKeep` 语义), 处理器里调用
  /// `pluginxx.hooks` 的 register / unregister 会改动 hooks_ 里的同一条 vector:
  /// 注册会重新分配, 注销最后一个会让整条 vector 失效 —— 直接遍历活表会出现
  /// 迭代器/引用失效 (漏派发或读到已释放内存)。快照只决定"本轮派发谁被调用",
  /// 注册表的变化从下一轮起生效; 统计写回按 (实例, ownerTag, 阶段) 重新查活表。
  std::vector<HookHandler> handlers;
  handlers.reserve(it->second.size());
  for (const HookHandler &h : it->second) {
    if (h.phase == phase) {
      handlers.push_back(h);
    }
  }
  if (handlers.empty()) {
    return R"({"handled":false,"handlers":0})";
  }
  std::stable_sort(handlers.begin(), handlers.end(),
                   [](const HookHandler &a, const HookHandler &b) {
                     if (a.spec.priority != b.spec.priority) {
                       return a.spec.priority < b.spec.priority;
                     }
                     return a.seq < b.seq;
                   });
  if (phase == MUSICXX_PLUGIN_HOOK_PHASE_AFTER) {
    /// 后链**逆序** (线尾 → 线头): priority 降序, 同优先级注册顺序也反。
    /// 这样外置插件最后改输出, 应用自己的兜底实现不能盖掉插件的改写
    /// (与实现链里"外置优先"同向)。
    std::reverse(handlers.begin(), handlers.end());
  }

  /// 调一个处理器并记账 (统计写回活表; 异常与失败都只记日志, 不影响其它处理器)
  ///
  /// [payloadText] 是这一步处理器看到的载荷, [outText] 是它回答的 JSON 文本
  /// (空 = 没有意见)。异步处理器 (只有实现链允许) 由调用方单独处理。
  auto invokeSync = [&](const HookHandler &h, const std::string &payloadText,
                        std::string &outText) {
    outText.clear();
    PluginxxString out{};
    PluginxxString err{};
    PluginxxStringView hookView{h.hookId.data(), h.hookId.size()};
    PluginxxStringView inputView{payloadText.data(), payloadText.size()};
    const auto t0 = std::chrono::steady_clock::now();
    int32_t rc = -1;
    try {
      rc = h.spec.hook_sync(h.spec.user_data, &hookView, &inputView, &out, &err);
    } catch (const std::exception &e) {
      XX_LOGW("[musicxx_ext] 插件 `{}` 处理器异常 (`{}`, phase={}): {}",
              h.pluginId, hookId, phaseNameOf(h.phase), e.what());
      rc = -1;
    } catch (...) {
      XX_LOGW("[musicxx_ext] 插件 `{}` 处理器未知异常 (`{}`, phase={})",
              h.pluginId, hookId, phaseNameOf(h.phase));
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
    HookHandler *live = nullptr;
    if (auto liveIt = hooks_.find(hookId); liveIt != hooks_.end()) {
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
      instIt->second->hookCalls.fetch_add(1);
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

  /// 观察型钩子的执行镜像 (仅调试开关打开时回传; 默认关闭 → 零开销)
  auto emitObserveMirror = [&](int32_t called) {
    if (mode == HookDispatchMode::Notify &&
        (flags_ & MUSICXX_EXTERN_PLUGIN_FLAG_DEBUG_OBSERVE_EVENTS) != 0) {
      Json mirror;
      mirror["hook"] = hookId;
      mirror["phase"] = phaseNameOf(phase);
      mirror["input"] = parseJsonSafe(inputJson);
      mirror["called"] = called;
      mirror["handlers"] = static_cast<int32_t>(handlers.size());
      pushEvent("musicxx.hook.observe", "", mirror.dump());
    }
  };

  int32_t called = 0;

  /* ---------- 前链: 串行、正序, 载荷逐步累积 ---------- */
  if (phase == MUSICXX_PLUGIN_HOOK_PHASE_BEFORE) {
    Json current = parseJsonSafe(inputJson);
    if (!current.is_object()) {
      current = Json::object();
    }
    Json accumulated = Json::object();
    bool stopped = false;
    for (const HookHandler &h : handlers) {
      if (!h.spec.hook_sync) {
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
      if (action == "stop" || action == MUSICXX_PLUGIN_HOOK_ACTION_CANCEL ||
          action == MUSICXX_PLUGIN_HOOK_ACTION_SKIP) {
        stopped = true;
        break;
      }
    }
    Json result;
    result["handled"] = called > 0 && mode != HookDispatchMode::Notify;
    result["handlers"] = static_cast<int32_t>(handlers.size());
    result["called"] = called;
    if (stopped || !accumulated.empty()) {
      Json res = Json::object();
      if (!accumulated.empty()) {
        res["patch"] = accumulated;
      }
      if (stopped) {
        res["action"] = "stop";
      }
      result["result"] = res;
    }
    emitObserveMirror(called);
    return result.dump();
  }

  /* ---------- 后链: 串行、逆序, 值逐步替换 ---------- */
  if (phase == MUSICXX_PLUGIN_HOOK_PHASE_AFTER) {
    Json payload = parseJsonSafe(inputJson);
    if (!payload.is_object()) {
      payload = Json::object();
    }
    Json value = payload.contains("value") ? payload["value"] : Json(nullptr);
    bool dropped = false;
    for (const HookHandler &h : handlers) {
      if (!h.spec.hook_sync) {
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
      if (actionOf(answer) == "drop") {
        dropped = true;
        break;
      }
    }
    Json result;
    result["handled"] = called > 0 && mode != HookDispatchMode::Notify;
    result["handlers"] = static_cast<int32_t>(handlers.size());
    result["called"] = called;
    Json res = Json::object();
    res["value"] = value;
    if (dropped) {
      res["action"] = "drop";
    }
    result["result"] = res;
    emitObserveMirror(called);
    return result.dump();
  }

  /* ---------- 实现链: 按 policy 合并, firstNonNull / anyCancel 可提前结束 ---------- */
  Json merged = Json::object();
  bool hasPatch = false;
  std::string action;

  for (auto &h : handlers) {
    if (!h.spec.hook_sync) {
      // 异步处理器: 本轮只做通知 (调用 hook_start, 结果不参与同步合并)
      if (h.spec.hook_start) {
        PluginxxString err{};
        PluginxxStringView hookView{h.hookId.data(), h.hookId.size()};
        PluginxxStringView inputView{inputJson.data(), inputJson.size()};
        try {
          h.spec.hook_start(h.spec.user_data, &hookView, &inputView, nullptr,
                            &err);
        } catch (...) {
          XX_LOGW("[musicxx_ext] 插件 `{}` 异步处理器异常 (`{}`)", h.pluginId,
                  hookId);
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
        (action.empty() || action == MUSICXX_PLUGIN_HOOK_ACTION_CONTINUE)) {
      action = a;
    }
    for (auto pit = result.begin(); pit != result.end(); ++pit) {
      if (pit.key() != std::string{"patch"}) {
        merged[pit.key()] = pit.value();
      }
    }

    // 合并策略: firstNonNull / anyCancel 命中即停止询问后续处理器
    if (meta->policy == 0 /*firstNonNull*/ && (hasPatch || !action.empty())) {
      break;
    }
    if (meta->policy == 1 /*anyCancel*/
        && (action == MUSICXX_PLUGIN_HOOK_ACTION_CANCEL ||
            action == MUSICXX_PLUGIN_HOOK_ACTION_SKIP)) {
      break;
    }
  }

  Json result;
  result["handled"] = called > 0 && mode != HookDispatchMode::Notify;
  /// handlers = 本轮快照里的处理器数量 (回调里注册/注销不影响本轮的计数)
  result["handlers"] = static_cast<int32_t>(handlers.size());
  result["called"] = called;
  if (!action.empty()) {
    result["result"] = merged;
    result["result"]["action"] = action;
  } else if (hasPatch) {
    result["result"] = merged;
  }

  /// 观测回传: 观察型钩子的执行镜像 (仅调试开关打开时回传)
  emitObserveMirror(called);
  return result.dump();
}

int32_t MusicxxHostManager::hookEmit(const std::string &hookId,
                                     const std::string &inputJson, bool sync,
                                     uint32_t timeoutMs,
                                     std::string &outJson, std::string &err) {
  return hookEmitPhase(hookId, MUSICXX_PLUGIN_HOOK_PHASE_IMPL, inputJson, sync,
                       timeoutMs, outJson, err);
}

int32_t MusicxxHostManager::hookEmitPhase(const std::string &hookId,
                                          int32_t phase,
                                          const std::string &inputJson,
                                          bool sync, uint32_t /*timeoutMs*/,
                                          std::string &outJson,
                                          std::string &err) {
  if (hookId.empty()) {
    err = "hook_emit: empty hook id";
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (!isValidPhase(phase)) {
    err = "hook_emit: invalid phase";
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (!running_.load(std::memory_order_acquire)) {
    err = "hook_emit: host not started";
    return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  }
  const auto *meta = findHookMeta(hookId);
  if (!meta) {
    outJson = R"({"handled":false,"error":"unknown_hook"})";
    return MUSICXX_EXTERN_PLUGIN_OK;
  }
  if (countOnPhase(hookId, phase) == 0) {
    outJson = R"({"handled":false,"handlers":0})";
    return MUSICXX_EXTERN_PLUGIN_OK;
  }

  /// 裁决型钩子的异步派发：不占用 Dart 线程，完成后经
  /// `musicxx.hook.decision.result` 事件把结果回传（payload 带 callId 供配对）。
  const bool asyncDecide =
      !sync && meta->mode == MUSICXX_PLUGIN_HOOK_MODE_DECISION;
  const HookDispatchMode mode =
      sync ? HookDispatchMode::Sync
           : (asyncDecide ? HookDispatchMode::AsyncDecide
                          : HookDispatchMode::Notify);
  const int64_t callId =
      asyncDecide ? nextHookCallId_.fetch_add(1, std::memory_order_relaxed) : 0;

  auto slot = std::make_shared<WaitSlot<std::string>>();
  auto self = shared_from_this();
  asio::post(ctx_->io, [self, slot, hookId, phase, inputJson, mode, callId]() {
    std::string result = self->dispatchHook(hookId, phase, inputJson, mode);
    if (mode == HookDispatchMode::AsyncDecide) {
      Json payload = parseJsonSafe(result);
      payload["callId"] = callId;
      payload["hook"] = hookId;
      payload["phase"] = phaseNameOf(phase);
      self->pushEvent("musicxx.hook.decision.result", "", payload.dump());
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

/* ==================== 主程序动作请求 (插件 → Dart) ==================== */

int32_t MusicxxHostManager::requestAction(MusicxxHostInstance *inst,
                                          std::string_view action,
                                          std::string_view argsJson,
                                          uint32_t timeoutMs,
                                          const PluginxxOperatorNotify *notify,
                                          int64_t *outRequestId) {
  if (!inst || action.empty()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  const std::string actionName{action};
  const std::string pluginId = pluginIdOf(inst->name);
  /// 动作名不校验、不拒绝: 插件想调什么就调什么, 由 Dart 侧按"注册了没有"决定
  /// 理不理它 (没注册的动作立刻回"未找到")。
  /// 命名建议: 官方动作 `musicxx.<域>.<动作>`; 插件自己的动作用 `plugin.<包名>.*`
  /// 以免与别的插件撞名。

  const int64_t requestId = nextRequestId_++;
  /// 超时不再限制区间: 传 0 表示不设超时 (一直等 Dart 侧回复); 非 0 时按插件给的值
  /// 到点终结该 op 并通知 Dart 收尾。
  const int64_t effectiveTimeoutMs = static_cast<int64_t>(timeoutMs);
  PendingAction pending;
  pending.plugin = inst->name;
  pending.action = actionName;
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
      auto it = pendingActions_.find(requestId);
      if (it == pendingActions_.end()) {
        return;
      }
      const std::string pluginId = pluginIdOf(it->second.plugin);
      if (it->second.notify && it->second.notify->done) {
        it->second.notify->done(it->second.notify->host_ud,
                                PLUGINXX_OPERATOR_CANCELLED, nullptr);
      }
      XX_LOGW("[musicxx_ext] 动作请求超时: plugin={} action={} id={}", pluginId,
              it->second.action, requestId);
      Json payload;
      payload["requestId"] = requestId;
      payload["reason"] = "timeout";
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
  payload["plugin"] = pluginId;
  payload["action"] = actionName;
  payload["timeoutMs"] = effectiveTimeoutMs;
  Json args = parseJsonSafe(argsJson);
  payload["args"] = args;
  pushEvent("musicxx.action.request", pluginId, payload.dump());
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::actionRespond(int64_t requestId, int32_t status,
                                          const std::string &resultJson) {
  /// 先问 JS 引擎的接驳口: 它用自己的表登记未回复的请求, 自带互斥, **任何线程**
  /// 都能就地回答。顺序不能反过来 —— 这条回复往往是由"正在 IO 线程上等 JS"的
  /// 钩子派发 (见 JsEngine::hookSync) 触发的, 先投 IO 线程就会两边互等。
  {
    std::shared_ptr<InternalActionRelay> relay;
    {
      std::lock_guard<std::mutex> lock{registryMutex_};
      relay = internalRelay_;
    }
    if (relay && relay->respondAction(requestId, status, resultJson)) {
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
  }
  /// 动态库插件的请求: 登记表归 IO 线程 (调用方已在 IO 线程时就地执行)
  return pluginxx::ioCallSync<int32_t>(
      this, [this, requestId, status, resultJson]() -> int32_t {
        auto it = pendingActions_.find(requestId);
        if (it == pendingActions_.end()) {
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
        pendingActions_.erase(it);
        return MUSICXX_EXTERN_PLUGIN_OK;
      });
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
    it->second.notify->done(it->second.notify->host_ud,
                            PLUGINXX_OPERATOR_CANCELLED, nullptr);
  }
  Json payload;
  payload["requestId"] = requestId;
  pushEvent("musicxx.action.cancel", pluginIdOf(it->second.plugin),
            payload.dump());
  pendingActions_.erase(it);
}

/* ==================== 状态镜像读取 ==================== */

int32_t MusicxxHostManager::getState(std::string_view key,
                                     std::string &outJson) {
  std::lock_guard<std::mutex> lock{stateMutex_};
  auto it = state_.find(std::string{key});
  if (it == state_.end()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  outJson = it->second;
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::subscribeState(MusicxxHostInstance *inst,
                                           const std::string &keysJson,
                                           int32_t &outCount) {
  if (!inst) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  bool parseOk = false;
  Json keys = parseJsonSafe(keysJson, &parseOk);
  if (!parseOk || !keys.is_array()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_JSON;
  }
  std::vector<std::string> list;
  for (const auto &item : keys) {
    if (item.is_string()) {
      list.push_back(item.get<std::string>());
    }
  }
  stateSubscriptions_[inst->name] = std::move(list);
  outCount = static_cast<int32_t>(stateSubscriptions_[inst->name].size());
  return MUSICXX_EXTERN_PLUGIN_OK;
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
                                         std::string_view instanceName) {
  Json j;
  j["appVersion"] = appVersion_;
  j["platform"] = platform_;
  j["language"] = language_;
  j["dataDir"] = dataDir_;
  j["userPluginDir"] = userPluginDir_;
  j["builtinPluginDir"] = builtinPluginDir_;
  j["apiVersion"] = MUSICXX_PLUGIN_API_VERSION;
  j["hostVersion"] = MUSICXX_EXTERN_PLUGIN_VERSION;
  // 调用方插件自己的 id: 插件要判断"这次调用是不是我自己发起的"就得知道自己的 id
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

int32_t PLUGINXX_CALL xx_register_hook(const PluginxxHost *host,
                                       const MusicxxPluginHookSpec *spec) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        auto call =
            pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
                host);
        if (!call.ok() || !spec) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto mgr = call.manager();
        auto inst = call.instance();
        const auto specCopy = *spec;
        return pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, inst, specCopy]() -> int32_t {
              return mgr->registerHook(inst, specCopy);
            });
      });
}

int32_t PLUGINXX_CALL xx_unregister_hook(const PluginxxHost *host,
                                         const PluginxxStringView *hookId,
                                         const PluginxxStringView *ownerTag) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        auto call =
            pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
                host);
        if (!call.ok() || !hookId || !hookId->data) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto mgr = call.manager();
        auto inst = call.instance();
        const std::string_view id{hookId->data,
                                  static_cast<size_t>(hookId->size)};
        const std::string_view tag =
            (ownerTag && ownerTag->data)
                ? std::string_view{ownerTag->data,
                                   static_cast<size_t>(ownerTag->size)}
                : std::string_view{};
        return pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, inst, id, tag]() -> int32_t {
              return mgr->unregisterHook(inst, id, tag);
            });
      });
}

int32_t PLUGINXX_CALL xx_hook_info(const PluginxxHost *host,
                                   const PluginxxStringView *hookId,
                                   PluginxxString *outJson) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!hookId || !hookId->data || !outJson) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto call =
            pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
                host, true);
        if (!call.ok()) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto mgr = call.manager();
        const std::string_view id{hookId->data,
                                  static_cast<size_t>(hookId->size)};
        std::string json;
        const int32_t rc = pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, id, &json]() -> int32_t {
              return mgr->hookInfoJson(id, json);
            });
        if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
          return rc;
        }
        pluginxx::hostMemorySetString(outJson, json);
        return MUSICXX_EXTERN_PLUGIN_OK;
      });
}

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

int32_t PLUGINXX_CALL xx_host_get_state(const PluginxxHost *host,
                                        const PluginxxStringView *key,
                                        PluginxxString *outJson) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!key || !key->data || !outJson) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto call =
            pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
                host, true);
        if (!call.ok()) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto mgr = call.manager();
        const std::string_view name{key->data, static_cast<size_t>(key->size)};
        std::string json;
        const int32_t rc = pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, name, &json]() -> int32_t {
              return mgr->getState(name, json);
            });
        if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
          return rc;
        }
        pluginxx::hostMemorySetString(outJson, json);
        return MUSICXX_EXTERN_PLUGIN_OK;
      });
}

int32_t PLUGINXX_CALL
xx_host_subscribe_state(const PluginxxHost *host,
                        const PluginxxStringView *keysJson, int32_t *outCount) {
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
        const std::string keys =
            keysJson && keysJson->data
                ? std::string{keysJson->data,
                              static_cast<size_t>(keysJson->size)}
                : std::string{"[]"};
        int32_t count = 0;
        const int32_t rc = pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, inst, keys, &count]() -> int32_t {
              return mgr->subscribeState(inst, keys, count);
            });
        if (outCount) {
          *outCount = count;
        }
        return rc;
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

int32_t PLUGINXX_CALL xx_host_request_action(
    const PluginxxHost *host, const PluginxxStringView *action,
    const PluginxxStringView *argsJson, uint32_t timeoutMs,
    const PluginxxOperatorNotify *notify, int64_t *outRequestId) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        auto call =
            pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
                host);
        if (!call.ok() || !action || !action->data) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto mgr = call.manager();
        auto inst = call.instance();
        const std::string_view actionName{action->data,
                                          static_cast<size_t>(action->size)};
        const std::string args =
            argsJson && argsJson->data
                ? std::string{argsJson->data,
                              static_cast<size_t>(argsJson->size)}
                : std::string{"{}"};
        int64_t requestId = 0;
        const int32_t rc = pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr,
            [mgr, inst, actionName, args, timeoutMs, notify, outRequestId,
             &requestId]() -> int32_t {
              return mgr->requestAction(inst, actionName, args, timeoutMs,
                                        notify,
                                        outRequestId ? &requestId : nullptr);
            });
        if (rc == MUSICXX_EXTERN_PLUGIN_OK && outRequestId) {
          *outRequestId = requestId;
        }
        return rc;
      });
}

void PLUGINXX_CALL xx_host_cancel_action(const PluginxxHost *host,
                                         int64_t requestId) {
  pluginxx::guardVtableCallVoid([&] {
    auto call =
        pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
            host);
    if (!call.ok()) {
      return;
    }
    auto mgr = call.manager();
    pluginxx::ioCallSyncVoidKeep(
        call, mgr, [mgr, requestId]() { mgr->cancelAction(requestId); });
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

void *PLUGINXX_CALL xx_alloc_fn(uint64_t size) {
  return pluginxx::hostMemoryAlloc(size);
}

void PLUGINXX_CALL xx_free_fn(void *ptr) { pluginxx::hostMemoryFree(ptr); }

/* ==================== 事件表 (自定义入口: 增加发布归属校验)
 * ==================== */

/// 订阅事件 (任意线程; 插件框架入口语义 + 主题合法性校验)
PluginxxSubscription *PLUGINXX_CALL xx_subscribe_event(
    const PluginxxHost *host, const PluginxxStringView *topic,
    void(PLUGINXX_CALL *handler)(const PluginxxStringView *event_json,
                                 void *ud),
    void *ud) {
  return pluginxx::guardVtableCall(nullptr, [&]() -> PluginxxSubscription * {
    auto call =
        pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
            host);
    if (!call.ok() || !topic || !topic->data || !handler) {
      return nullptr;
    }
    auto mgr = call.manager();
    auto inst = call.instance();
    const std::string topicText =
        std::string{topic->data, static_cast<size_t>(topic->size)};
    // 订阅只要求主题形状合法 (可以订阅其它插件或官方主题); 发布才做归属校验
    if (mgr->qualifyEventTopic(topicText).empty()) {
      XX_LOGW("[musicxx_ext] 插件 `{}` 订阅非法主题 `{}` 被拒绝", inst->name,
              topicText);
      return nullptr;
    }
    return pluginxx::ioCallSyncKeep<PluginxxSubscription *>(
        call, mgr, [mgr, inst, topicText, handler, ud]() {
          return mgr->subscribe(inst, topicText, handler, ud);
        });
  });
}

/// 撤销订阅 (幂等; 任意线程)
void PLUGINXX_CALL xx_unsubscribe_event(PluginxxSubscription *sub) {
  pluginxx::unsubscribePluginSubscription(sub);
}

/// 发布事件 (任意线程): 主题必须是官方 `musicxx.*` 或本插件自己的
/// `plugin.<id>.*`
int32_t PLUGINXX_CALL xx_publish_event(const PluginxxHost *host,
                                       const PluginxxStringView *topic,
                                       const PluginxxStringView *event_json) {
  return pluginxx::guardVtableCall(-1, [&]() -> int32_t {
    auto call =
        pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
            host);
    if (!call.ok() || !topic || !topic->data || !event_json) {
      return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
    }
    auto mgr = call.manager();
    auto inst = call.instance();
    const std::string topicText =
        std::string{topic->data, static_cast<size_t>(topic->size)};
    const std::string pluginId = MusicxxHostManager::pluginIdOf(inst->name);
    if (!isTopicOwnedBy(topicText, pluginId)) {
      XX_LOGW("[musicxx_ext] 插件 `{}` 发布事件被拒绝: 主题 `{}` "
              "不属于该插件命名空间",
              pluginId, topicText);
      return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
    }
    const std::string payload =
        (event_json->size > 0 && event_json->data)
            ? std::string{event_json->data,
                          static_cast<size_t>(event_json->size)}
            : std::string{"{}"};
    return pluginxx::ioCallSyncKeep<int32_t>(
        call, mgr, [mgr, topicText, payload]() -> int32_t {
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
  // events 表用本框架自己的入口 (需要在发布路径上做命名空间归属校验, 见
  // xx_publish_event)
  if (name == PLUGINXX_IFACE_EVENTS) {
    return &g_ifaceEvents;
  }
  // 其余通用表
  // (log/json/config/plugins/scheduler/coroutine_runtime/tasks/cancel/
  // capabilities) 由插件框架提供; musicxx 专用表在下面分发
  if (const void *generic =
          pluginxx::queryGenericPluginIface<MusicxxHostInstance,
                                            MusicxxHostManager>(name)) {
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
  if (name == MUSICXX_PLUGIN_IFACE_VARS) {
    return varsIfaceForQuery();
  }
  // 预留表 (musicxx.player/library/lyrics/storage/net/stats/agent): v1 返回空,
  // 插件据此判空并降级 (IID 已冻结, 后续实现不再改动约定)
  return nullptr;
}

} // namespace

const PluginxxHostVtable *MusicxxHostManager::hostVtable() {
  return &g_hostVtable;
}

} // namespace extern_plugin
} // namespace musicxx
