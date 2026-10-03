/// musicxx 外部插件原生宿主: 变量表 (第五条通道)
///
/// 变量 = **有属主、可读、可写、可订阅变动**的小值 (配置/小状态)。它与既有四条通道
/// 分工不同: 钩子做拦截、状态镜像是"应用推 + 插件同步只读"、事件只通知没有当前值、
/// 动作是命令; 变量则把"谁提供 / 能不能写 / 值是什么"合在一个明确的模型里。
///
/// 设计要点:
/// - **宿主里没有真值**: 真值只在属主那里 (应用侧的 Store/配置, 或插件自己的状态)。
///   这里的 `VarEntry::valueJson` 只是服务 `peek` 这条同步捷径的**缓存**, 来源有三处 ——
///   属主推送、每次异步读的结果 (read-through)、注册时的初值;
/// - **读默认异步**(`get`): 宿主把请求转给属主, 属主回答后回给调用方; 宿主的线程全程
///   不等 (等的是发起方)。同一个键的进行中读请求合并成一次属主往返, 答案分发给全部等待者;
/// - **写一律交给属主写入**: 官方键交给应用 (走设置同一条路径), 插件 `declared` 模式由
///   宿主代存 (= 属主已同意), `handler` 模式转给属主插件 (§M3 实现);
/// - **注册表归宿主线程独占** (无锁不变式): 注册/注销/读写/通知/摘除都在宿主线程;
///   Dart 线程的入口经 C ABI 侧投递后执行;
/// - **零成本**: 宿主没启动时不声明、不订阅; 应用侧值变化只在"该键有人关心 (订阅或
///   watch) 且宿主在跑"时才推 (关心数由这里回传给 Dart);
/// - **高频值建议节流**: 属主按 `throttleMs` 合并推送, 宿主按 `notifyThrottleMs` 合并
///   通知 (载荷带 `coalesced`), 且没人关心就不推; 这些窗口只是声明, 宿主不强制度量。
///
/// 容量与大小都不再限制 (变量数 / 变量总数 / 键长 / 值大小 / 订阅数); 唯一的容量类
/// 限制是"每个插件未回执的读写请求数" (避免慢属主把宿主事件队列灌满)。
#include "host_json.h"
#include "host_naming.h"
#include "musicxx_host.h"

#include "musicxx/plugin/api/plugin_api.h"
#include "pluginxx/host/abi_util.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace musicxx {
namespace extern_plugin {

using utilxx_base::Json;

namespace {

/// 等属主回答读取、写入值的超时
constexpr int64_t kVarReadTimeoutMs = 2000;
constexpr int64_t kVarWriteOwnerTimeoutMs = 3000;
constexpr int64_t kVarWriteAppTimeoutMs = 5000;

/// steady 毫秒 (算 `ageMs` 用; 不受系统时间调整影响)
int64_t steadyNowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

/// 键的字符集与形状校验
///
/// - 非空;
/// - 只允许 `A-Za-z0-9_.-`;
/// - 不含连续点、不以点开头或结尾。
bool isValidVarKeyText(std::string_view key) {
  if (key.empty()) {
    return false;
  }
  if (key.front() == '.' || key.back() == '.') {
    return false;
  }
  bool lastDot = false;
  for (const char raw : key) {
    const auto c = static_cast<unsigned char>(raw);
    const bool isAlpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool isDigit = (c >= '0' && c <= '9');
    if (isAlpha || isDigit || c == '_' || c == '-') {
      lastDot = false;
      continue;
    }
    if (c == '.') {
      if (lastDot) {
        return false;
      }
      lastDot = true;
      continue;
    }
    return false;
  }
  return true;
}

/// 短名补全: 插件写 `tip.start` → `plugin.<本插件id>.tip.start`; 全名必须属于本插件
///
/// - 返回 false 表示命名空间非法 (注册官方键、注册别人的键、键名不合法);
/// - 官方键 (`musicxx.`) 只能由应用声明, 插件注册一律拒绝。
bool normalizeVarKey(std::string_view raw, std::string_view pluginId,
                     std::string &outKey, std::string &err) {
  const std::string text{raw};
  if (text.empty()) {
    err = "键不能为空";
    return false;
  }
  if (naming::isOfficial(text)) {
    err = "官方键 (musicxx.*) 由应用声明, 插件不能注册: " + text;
    return false;
  }
  if (text.rfind("plugin.", 0) == 0) {
    if (pluginId.empty() || !naming::isOwnPlugin(text, pluginId)) {
      err = "键不属于本插件的命名空间: " + text;
      return false;
    }
    // 命名空间本身 (只有两段) 不能当变量
    if (text.find('.', 7) == std::string::npos) {
      err = "键至少要有三段 (plugin.<插件id>.<分组>...): " + text;
      return false;
    }
    if (!isValidVarKeyText(text)) {
      err = "键名不合法 (只允许 A-Za-z0-9_.-, 不能有连续点): " + text;
      return false;
    }
    outKey = text;
    return true;
  }
  // 短名 (可以多级分组, 如 tip.start)
  if (text.find('.') == std::string::npos) {
    if (!isValidVarKeyText(text)) {
      err = "键名不合法 (只允许 A-Za-z0-9_.-): " + text;
      return false;
    }
  } else if (!isValidVarKeyText(text)) {
    err = "键名不合法 (只允许 A-Za-z0-9_.-, 不能有连续点): " + text;
    return false;
  }
  if (pluginId.empty()) {
    err = "插件命名空间为空, 无法补全键: " + text;
    return false;
  }
  outKey = "plugin." + std::string{pluginId} + "." + text;
  if (!isValidVarKeyText(outKey)) {
    err = "补全后的键名不合法: " + outKey;
    return false;
  }
  return true;
}

/// 能力位文本 → 位图 (`["get","set","notify"]`)
int32_t capsFromTextArray(const Json &array, bool &ok) {
  ok = true;
  int32_t caps = 0;
  if (!array.is_array()) {
    ok = false;
    return 0;
  }
  for (const auto &item : array) {
    if (!item.is_string()) {
      ok = false;
      return 0;
    }
    const std::string name = item.get<std::string>();
    if (name == "get") {
      caps |= MUSICXX_PLUGIN_VAR_CAP_GET;
    } else if (name == "set") {
      caps |= MUSICXX_PLUGIN_VAR_CAP_SET;
    } else if (name == "notify") {
      caps |= MUSICXX_PLUGIN_VAR_CAP_NOTIFY;
    } else {
      ok = false;
      return 0;
    }
  }
  return caps;
}

Json capsToJson(int32_t caps) {
  Json array = Json::array();
  if ((caps & MUSICXX_PLUGIN_VAR_CAP_GET) != 0) {
    array.push_back("get");
  }
  if ((caps & MUSICXX_PLUGIN_VAR_CAP_SET) != 0) {
    array.push_back("set");
  }
  if ((caps & MUSICXX_PLUGIN_VAR_CAP_NOTIFY) != 0) {
    array.push_back("notify");
  }
  return array;
}

const char *varModeName(int32_t mode) {
  return mode == MUSICXX_PLUGIN_VAR_MODE_HANDLER ? "handler" : "declared";
}

const char *varWriteScopeName(int32_t scope) {
  return scope == MUSICXX_PLUGIN_VAR_WRITE_OWNER ? "owner" : "any";
}

/// 值文本 → 规范化 JSON 文本 (非法返回 false; 大小不限制)
///
/// 规范化很重要: "值有没有变" 用文本比较, 同一个值的不同写法 (空白/键顺序) 必须归一,
/// 否则会出现"没变也通知"。
bool normalizeValueText(std::string_view raw, std::string &out, std::string &err) {
  bool parseOk = false;
  Json value = parseJsonSafe(raw, &parseOk);
  if (!parseOk) {
    err = "值不是合法 JSON";
    return false;
  }
  out = value.dump();
  return true;
}

/// 键是否匹配某个订阅项 (全名或前缀)
bool varKeyMatchesSubscription(const std::string &key,
                               const std::string &subscription) {
  if (subscription.empty()) {
    return false;
  }
  if (key == subscription) {
    return true;
  }
  // 前缀写法: "plugin." (全部插件键) / "plugin.<id>." / "plugin.<id>.<域>"
  if (subscription.back() == '.') {
    return key.size() > subscription.size() &&
           key.compare(0, subscription.size(), subscription) == 0;
  }
  return false;
}

} // namespace

/* ==================== 注册 / 注销 ==================== */

int32_t MusicxxHostManager::registerVar(MusicxxHostInstance *inst,
                                        const MusicxxPluginVarSpec &spec) {
  if (!inst) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (spec.struct_size != 0 && spec.struct_size < sizeof(MusicxxPluginVarSpec)) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (inst->unloadRequested || !inst->enabled) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册变量被拒绝: 实例正在关闭或已禁用",
            inst->name);
    return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  }
  if (!spec.key.data || spec.key.size == 0) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (spec.caps == 0) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册变量 `{}` 被拒绝: caps 不能为空",
            inst->name, std::string{spec.key.data, spec.key.size});
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if ((spec.caps & ~(MUSICXX_PLUGIN_VAR_CAP_GET | MUSICXX_PLUGIN_VAR_CAP_SET |
                     MUSICXX_PLUGIN_VAR_CAP_NOTIFY)) != 0) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (spec.write_scope != MUSICXX_PLUGIN_VAR_WRITE_ANY &&
      spec.write_scope != MUSICXX_PLUGIN_VAR_WRITE_OWNER) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (spec.mode != MUSICXX_PLUGIN_VAR_MODE_DECLARED &&
      spec.mode != MUSICXX_PLUGIN_VAR_MODE_HANDLER) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册变量: 未知模式 {} (取值 0/1)",
            inst->name, spec.mode);
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  /// 节流/保活窗口只是"属主侧的规则声明", 不再做取值范围校验 (负值按 0 处理):
  /// 它不参与宿主的行为判定, 拒绝注册只会给作者添麻烦。

  const std::string pluginId = pluginIdOf(inst->name);
  std::string key;
  std::string err;
  if (!normalizeVarKey(
          std::string_view{spec.key.data, static_cast<size_t>(spec.key.size)},
          pluginId, key, err)) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册变量 `{}` 被拒绝: {}", inst->name,
            std::string{spec.key.data, spec.key.size}, err);
    return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
  }

  // 类型与展示字段 (type 只是给界面看的提示: 不认识的值不拒绝, 按无类型处理)
  std::string type;
  if (spec.type.data && spec.type.size > 0) {
    type = std::string{spec.type.data, static_cast<size_t>(spec.type.size)};
    if (type != "bool" && type != "number" && type != "string" &&
        type != "json") {
      XX_LOGW("[musicxx_ext] 插件 `{}` 注册变量 `{}`: 未知 type `{}` (按无类型处理)",
              inst->name, key, type);
      type.clear();
    }
  }

  Json meta = Json::object();
  if (spec.meta_json.data && spec.meta_json.size > 0) {
    bool metaOk = false;
    meta = parseJsonSafe(
        std::string_view{spec.meta_json.data,
                         static_cast<size_t>(spec.meta_json.size)},
        &metaOk);
    if (!metaOk || !meta.is_object()) {
      XX_LOGW("[musicxx_ext] 插件 `{}` 注册变量 `{}`: meta_json 必须是对象",
              inst->name, key);
      return MUSICXX_EXTERN_PLUGIN_ERR_JSON;
    }
    if (meta.contains("options") && !meta["options"].is_array()) {
      return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
    }
  }

  std::string valueText = "null";
  if (spec.value_json.data && spec.value_json.size > 0) {
    if (!normalizeValueText(
            std::string_view{spec.value_json.data,
                             static_cast<size_t>(spec.value_json.size)},
            valueText, err)) {
      XX_LOGW("[musicxx_ext] 插件 `{}` 注册变量 `{}` 被拒绝: {}", inst->name,
              key, err);
      return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
    }
  }

  auto existing = vars_.find(key);
  if (existing != vars_.end() && existing->second.owner != inst->name) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 试图覆盖他人变量 `{}`", inst->name, key);
    return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
  }
  /// 变量数不设上限 (早期版本的"每插件 64 个 / 总数 4096"已移除)。

  VarEntry entry;
  entry.key = key;
  entry.owner = inst->name;
  entry.pluginId = pluginId;
  entry.caps = spec.caps;
  entry.mode = spec.mode;
  entry.writeScope = spec.write_scope;
  entry.type = type;
  entry.optionsJson =
      (meta.contains("options") && meta["options"].is_array())
          ? meta["options"].dump()
          : std::string{};
  if (meta.contains("title") && meta["title"].is_string()) {
    entry.title = meta["title"].get<std::string>();
  }
  if (meta.contains("depict") && meta["depict"].is_string()) {
    entry.depict = meta["depict"].get<std::string>();
  }
  if (meta.contains("risk") && meta["risk"].is_string()) {
    entry.risk = meta["risk"].get<std::string>();
  }
  entry.throttleMs = spec.throttle_ms;
  entry.notifyThrottleMs = spec.notify_throttle_ms;
  entry.refreshAfterMs = spec.refresh_after_ms;
  entry.valueJson = valueText;
  entry.everRefreshed = true;
  entry.valueMs = steadyNowMs();
  entry.seq = (existing != vars_.end()) ? existing->second.seq : ++varSeq_;
  const bool isNew = (existing == vars_.end());
  if (!isNew) {
    // 覆盖式注册: 订阅关系与修订号沿用 (作者改的是声明, 不是值)
    entry.subscribers = existing->second.subscribers;
    entry.watchers = existing->second.watchers;
    entry.revision = existing->second.revision;
  }
  vars_[key] = std::move(entry);

  if (isNew) {
    // 先订阅、后声明: 键出现时把已有订阅挂上, 并补一条初始通知
    // (值 = 刚声明的初值), 这样插件可以先绑定、后读到值
    for (const auto &[instanceName, keys] : varSubscriptions_) {
      if (keys.count(key) != 0) {
        vars_[key].subscribers.insert(instanceName);
      }
    }
    for (const auto &[instanceName, keys] : varWatchers_) {
      if (keys.count(key) != 0) {
        vars_[key].watchers.insert(instanceName);
      }
    }
  }
  if (vars_[key].careCount() > 0) {
    pushVarChanged(key, valueText, std::string{"null"}, pluginId, false, 0);
  }
  refreshVarCare(vars_[key]);
  XX_LOGI("[musicxx_ext] 插件 `{}` 注册变量 `{}` (caps={} mode={} write={})",
          pluginId, key, capsToJson(spec.caps).dump(), varModeName(spec.mode),
          varWriteScopeName(spec.write_scope));
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::unregisterVar(MusicxxHostInstance *inst,
                                          std::string_view key) {
  if (key.empty()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  const std::string pluginId = inst ? pluginIdOf(inst->name) : std::string{};
  std::string fullKey;
  std::string err;
  if (!normalizeVarKey(key, pluginId, fullKey, err)) {
    return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
  }
  auto it = vars_.find(fullKey);
  if (it == vars_.end()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  if (inst && it->second.owner != inst->name) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 试图注销他人变量 `{}`", inst->name,
            fullKey);
    return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
  }
  const std::string ownerId = it->second.pluginId;
  const std::string valueText = it->second.valueJson;
  const bool cared = it->second.careCount() > 0;
  // 订阅关系留在实例侧 (键再次出现时自动挂回), 这里只发一条"变量没了"的通知
  if (cared) {
    pushVarChanged(fullKey, valueText, valueText, ownerId, true, 0);
  }
  if (it->second.notifyTimer) {
    it->second.notifyTimer->cancel();
  }
  vars_.erase(it);
  eraseVarMirror(fullKey);
  return MUSICXX_EXTERN_PLUGIN_OK;
}

size_t MusicxxHostManager::countVarsOfPlugin(const std::string &pluginId) const {
  size_t count = 0;
  for (const auto &[key, entry] : vars_) {
    (void)key;
    if (entry.pluginId == pluginId && !entry.owner.empty()) {
      ++count;
    }
  }
  return count;
}

/* ==================== 读 ==================== */

int32_t MusicxxHostManager::beginVarRead(
    const std::string &key, const std::string &byPluginId,
    const PluginxxOperatorNotify *notify,
    std::shared_ptr<WaitSlot<std::string>> appSlot, int64_t *outRequestId) {
  auto it = vars_.find(key);
  if (it == vars_.end()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  VarEntry &entry = it->second;
  if ((entry.caps & MUSICXX_PLUGIN_VAR_CAP_GET) == 0) {
    return MUSICXX_PLUGIN_VAR_ERR_UNSUPPORTED;
  }
  if (outRequestId) {
    *outRequestId = 0;
  }

  // 属主是应用 (官方键): 需要一次 Dart 往返; 插件 handler 属主同理 (§M3)
  const bool needOwner = entry.owner.empty() ||
                         entry.mode == MUSICXX_PLUGIN_VAR_MODE_HANDLER;
  if (!needOwner) {
    // `declared` 模式的插件变量: 值就在注册表里, 宿主即属主 → 立即回答
    // (用 post 延后一次, 让发起方先把等待者登记好)
    const std::string value = entry.valueJson;
    if (notify && notify->done) {
      const PluginxxOperatorNotify notifyCopy = *notify;
      auto self = shared_from_this();
      asio::post(ctx_->io, [notifyCopy, value] {
        PluginxxString payload =
            pluginxx::hostMemoryCreateString(std::string_view{value});
        const PluginxxStringView view{payload.data, payload.size};
        notifyCopy.done(notifyCopy.host_ud, PLUGINXX_OPERATOR_OK, &view);
        if (payload.data) {
          pluginxx::hostMemoryFree(payload.data);
        }
      });
    }
    if (appSlot) {
      Json result;
      result["ok"] = true;
      result["value"] = parseJsonSafe(value);
      appSlot->set(result.dump());
    }
    return MUSICXX_EXTERN_PLUGIN_OK;
  }

  // 同键进行中读合并: 只向属主发一次请求, 答案分发给全部等待者
  auto pendingIt = pendingReadByKey_.find(key);
  if (pendingIt != pendingReadByKey_.end()) {
    auto reqIt = pendingVarRequests_.find(pendingIt->second);
    if (reqIt != pendingVarRequests_.end()) {
      if (notify && notify->done) {
        reqIt->second.pluginWaiters.push_back(*notify);
      }
      if (appSlot) {
        reqIt->second.appWaiters.push_back(std::move(appSlot));
      }
      if (outRequestId) {
        *outRequestId = reqIt->first;
      }
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    pendingReadByKey_.erase(pendingIt);
  }

  // 未回执读请求的数量上限 (避免慢属主把宿主事件队列灌满)
  if (!byPluginId.empty()) {
    int32_t pending = 0;
    for (const auto &[id, req] : pendingVarRequests_) {
      (void)id;
      if (req.kind == VarRequestRead && req.byPluginId == byPluginId) {
        ++pending;
      }
    }
    if (pending >= kMaxVarPendingPerPlugin) {
      return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
  }

  const int64_t requestId = nextRequestId_++;
  PendingVarRequest request;
  request.key = key;
  request.byPluginId = byPluginId;
  request.kind = VarRequestRead;
  request.owner = entry.owner;
  request.caps = entry.caps;
  if (notify && notify->done) {
    request.pluginWaiters.push_back(*notify);
  }
  if (appSlot) {
    request.appWaiters.push_back(std::move(appSlot));
  }
  if (ctx_) {
    auto timer = std::make_shared<asio::steady_timer>(ctx_->io);
    timer->expires_after(std::chrono::milliseconds{kVarReadTimeoutMs});
    auto self = shared_from_this();
    timer->async_wait([self, requestId](const utilxx_base::AsioErrorCode &ec) {
      if (ec) {
        return; ///< 已结算: 定时器被主动取消
      }
      self->expireVarRequest(requestId);
    });
    request.timer = std::move(timer);
  }
  pendingVarRequests_[requestId] = std::move(request);
  pendingReadByKey_[key] = requestId;
  if (outRequestId) {
    *outRequestId = requestId;
  }

  if (entry.owner.empty()) {
    // 官方键: 转给应用 (应用侧直接问实现, 本地权威)
    Json payload;
    payload["requestId"] = requestId;
    payload["key"] = key;
    pushEvent("musicxx.var.read", "", payload.dump());
  } else {
    // handler 属主: 用事件总线把请求转给属主实例 (属主订阅 `musicxx.var.read`)
    Json payload;
    payload["requestId"] = requestId;
    payload["key"] = key;
    eventBus_->publish(MUSICXX_PLUGIN_EVENT_VAR_READ, payload.dump());
  }
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::getVar(MusicxxHostInstance *inst,
                                   std::string_view key,
                                   const PluginxxOperatorNotify *notify,
                                   int64_t *outRequestId) {
  if (!inst || key.empty()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string fullKey;
  std::string err;
  // 读的时候键必须是全名或短名; 官方键直接用, 插件键补全
  const std::string text{key};
  if (naming::isOfficial(text)) {
    fullKey = text;
  } else if (!normalizeVarKey(key, pluginIdOf(inst->name), fullKey, err)) {
    return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
  }
  return beginVarRead(fullKey, pluginIdOf(inst->name), notify, nullptr,
                      outRequestId);
}

int32_t MusicxxHostManager::getVarForApp(
    const std::string &key, std::string &outJson,
    std::shared_ptr<WaitSlot<std::string>> slot) {
  if (key.empty() || !slot) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  const int32_t rc = beginVarRead(key, "", nullptr, slot, nullptr);
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    outJson.clear();
  }
  return rc;
}

int32_t MusicxxHostManager::peekVar(std::string_view key, std::string &outJson) {
  // 只读镜像 (任何线程可用): JS 线程绝不等宿主线程, 见 varMirror_ 的说明
  std::string snapshot;
  {
    std::lock_guard<std::mutex> lock{varMirrorMutex_};
    auto it = varMirror_.find(std::string{key});
    if (it == varMirror_.end()) {
      return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
    }
    snapshot = it->second;
  }
  bool ok = false;
  Json item = parseJsonSafe(snapshot, &ok);
  if (!ok || !item.is_object()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  // 能力位没有 get 时不给 peek (与 get 的规则一致)
  if (item.contains("caps") && item["caps"].is_array()) {
    bool hasGet = false;
    for (const auto &cap : item["caps"]) {
      if (cap.is_string() && cap.get<std::string>() == "get") {
        hasGet = true;
        break;
      }
    }
    if (!hasGet) {
      return MUSICXX_PLUGIN_VAR_ERR_UNSUPPORTED;
    }
  }
  const bool hasValue =
      item.contains("hasValue") && item["hasValue"].is_boolean() &&
      item["hasValue"].get<bool>();
  if (!hasValue) {
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND; ///< 没有缓存过 (没人推也没人读过)
  }
  const int64_t valueMs = item.contains("valueMs") && item["valueMs"].is_number()
                              ? item["valueMs"].get<int64_t>()
                              : 0;
  const int64_t ageMs = valueMs > 0 ? steadyNowMs() - valueMs : 0;
  const bool cared =
      item.contains("subscribers") && item["subscribers"].is_number() &&
      item["subscribers"].get<int64_t>() > 0;
  Json out;
  out["value"] = item.contains("value") ? item["value"] : Json(nullptr);
  out["revision"] = item.contains("revision") ? item["revision"] : Json(0);
  out["ageMs"] = ageMs;
  /// stale = 缓存可能已经不新鲜 (没有订阅保活时如实报告, 不是错误)
  out["stale"] = !cared || ageMs > kVarReadTimeoutMs;
  outJson = out.dump();
  return MUSICXX_EXTERN_PLUGIN_OK;
}

/* ==================== 写 ==================== */

int32_t MusicxxHostManager::beginVarWrite(
    const std::string &key, const std::string &valueJson,
    const std::string &byPluginId, const std::string &byInstance,
    const PluginxxOperatorNotify *notify,
    std::shared_ptr<WaitSlot<std::string>> appSlot, std::string &outJson) {
  auto it = vars_.find(key);
  if (it == vars_.end()) {
    outJson.clear();
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  VarEntry &entry = it->second;
  if ((entry.caps & MUSICXX_PLUGIN_VAR_CAP_SET) == 0) {
    outJson.clear();
    return MUSICXX_PLUGIN_VAR_ERR_UNSUPPORTED;
  }
  // 写权限: `write: owner` 只允许属主自己写 (应用写官方键不走这条路径)
  if (entry.writeScope == MUSICXX_PLUGIN_VAR_WRITE_OWNER &&
      byInstance != entry.owner) {
    outJson.clear();
    return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
  }
  std::string valueText;
  std::string err;
  if (!normalizeValueText(valueJson, valueText, err)) {
    outJson.clear();
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }

  // 属主是应用 (官方键) 或插件 handler 模式: 一律交给属主写入, 属主点头才算数
  const std::string ownerInstance = entry.owner;
  const bool forward = ownerInstance.empty() ||
                       entry.mode == MUSICXX_PLUGIN_VAR_MODE_HANDLER;
  const bool isOwnerWrite = (byInstance == ownerInstance);

  if (!forward || isOwnerWrite) {
    // 属主自己写自己的变量 = 提交 (declared 模式的宿主就是写入者);
    // 这样也不会出现"属主写自己的变量还要转给自己"的死循环
    std::string prev;
    const bool changed = applyVarValue(entry, valueText, byPluginId, prev);
    // 提交同时结算该键上未完成的请求: 写请求 = 写入回执, 读请求 = 这次读的答案
    // (handler 属主用 `set` 回答与提交, 见设计文档 §3.5)
    settleVarRequestsOnCommit(key, valueText);
    const int64_t revision = entry.revision;
    Json result;
    result["key"] = key;
    result["accepted"] = true;
    result["value"] = parseJsonSafe(valueText);
    result["changed"] = changed;
    result["revision"] = revision;
    outJson = result.dump();
    if (notify && notify->done) {
      const PluginxxOperatorNotify notifyCopy = *notify;
      const std::string payload = outJson;
      asio::post(ctx_->io, [notifyCopy, payload] {
        PluginxxString out = pluginxx::hostMemoryCreateString(
            std::string_view{payload});
        const PluginxxStringView view{out.data, out.size};
        notifyCopy.done(notifyCopy.host_ud, PLUGINXX_OPERATOR_OK, &view);
        if (out.data) {
          pluginxx::hostMemoryFree(out.data);
        }
      });
    }
    if (appSlot) {
      appSlot->set(outJson);
    }
    return MUSICXX_EXTERN_PLUGIN_OK;
  }

  // 未回执写请求的数量上限 (避免慢属主把宿主事件队列灌满)
  if (!byPluginId.empty()) {
    int32_t pending = 0;
    for (const auto &[id, req] : pendingVarRequests_) {
      (void)id;
      if (req.kind == VarRequestWrite && req.byPluginId == byPluginId) {
        ++pending;
      }
    }
    if (pending >= kMaxVarPendingPerPlugin) {
      outJson.clear();
      return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
  }

  const int64_t requestId = nextRequestId_++;
  PendingVarRequest request;
  request.key = key;
  request.byPluginId = byPluginId;
  request.byInstance = byInstance;
  request.kind = VarRequestWrite;
  request.valueJson = valueText;
  request.owner = ownerInstance;
  request.caps = entry.caps;
  if (notify && notify->done) {
    request.pluginWaiters.push_back(*notify);
  }
  if (appSlot) {
    request.appWaiters.push_back(std::move(appSlot));
  }
  if (ctx_) {
    const int64_t budget =
        ownerInstance.empty() ? kVarWriteAppTimeoutMs : kVarWriteOwnerTimeoutMs;
    auto timer = std::make_shared<asio::steady_timer>(ctx_->io);
    timer->expires_after(std::chrono::milliseconds{budget});
    auto self = shared_from_this();
    timer->async_wait([self, requestId](const utilxx_base::AsioErrorCode &ec) {
      if (ec) {
        return; ///< 已结算: 定时器被主动取消
      }
      self->expireVarRequest(requestId);
    });
    request.timer = std::move(timer);
  }
  pendingVarRequests_[requestId] = std::move(request);

  Json payload;
  payload["requestId"] = requestId;
  payload["key"] = key;
  payload["value"] = parseJsonSafe(valueText);
  payload["by"] = byPluginId;
  if (ownerInstance.empty()) {
    // 官方键: 请求应用按"用户在设置里改"的同一条路径写入
    pushEvent("musicxx.var.write", "", payload.dump());
  } else {
    // handler 属主: 用事件总线把写请求转给属主实例 (属主订阅 `musicxx.var.write`)
    eventBus_->publish(MUSICXX_PLUGIN_EVENT_VAR_WRITE, payload.dump());
  }

  Json pending;
  pending["pending"] = true;
  pending["requestId"] = requestId;
  pending["key"] = key;
  outJson = pending.dump();
  if (notify && notify->done) {
    const PluginxxOperatorNotify notifyCopy = *notify;
    const std::string text = outJson;
    auto self = shared_from_this();
    asio::post(ctx_->io, [notifyCopy, text] {
      PluginxxString out =
          pluginxx::hostMemoryCreateString(std::string_view{text});
      const PluginxxStringView view{out.data, out.size};
      notifyCopy.done(notifyCopy.host_ud, PLUGINXX_OPERATOR_OK, &view);
      if (out.data) {
        pluginxx::hostMemoryFree(out.data);
      }
    });
  }
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::setVar(MusicxxHostInstance *inst,
                                   std::string_view key,
                                   std::string_view valueJson,
                                   std::string &outJson) {
  if (!inst || key.empty() || valueJson.data() == nullptr) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string fullKey;
  std::string err;
  const std::string text{key};
  if (naming::isOfficial(text)) {
    fullKey = text;
  } else if (!normalizeVarKey(key, pluginIdOf(inst->name), fullKey, err)) {
    return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
  }
  return beginVarWrite(fullKey, std::string{valueJson},
                       pluginIdOf(inst->name), inst->name, nullptr, nullptr,
                       outJson);
}

int32_t MusicxxHostManager::setVarForApp(
    const std::string &key, const std::string &valueJson, std::string &outJson,
    std::shared_ptr<WaitSlot<std::string>> slot) {
  if (key.empty() || !slot) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  // `beginVarWrite` 在"交给属主写入"时会写一个 `{pending:true,...}` 信封 —— 那对
  // 应用侧没有用 (应用要的是最终结果), 因此这里只把**已结算**的结果交出去:
  // 结果是"待写入"时清空 outJson, 让调用方去等等待槽 (C ABI 就是这么区分的)。
  std::string local;
  const int32_t rc =
      beginVarWrite(key, valueJson, "", "", nullptr, slot, local);
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    outJson = local;
    return rc;
  }
  outJson = (local.find("\"accepted\"") != std::string::npos) ? local
                                                             : std::string{};
  return MUSICXX_EXTERN_PLUGIN_OK;
}

/* ==================== 结算 ==================== */

bool MusicxxHostManager::hasPendingVarRequest(int64_t requestId) const {
  return pendingVarRequests_.find(requestId) != pendingVarRequests_.end();
}

void MusicxxHostManager::completeVarRequest(int64_t requestId, bool ok,
                                            const std::string &valueJson,
                                            const std::string &error) {
  auto it = pendingVarRequests_.find(requestId);
  if (it == pendingVarRequests_.end()) {
    return;
  }
  if (it->second.timer) {
    it->second.timer->cancel();
  }
  PendingVarRequest request = std::move(it->second);
  pendingVarRequests_.erase(it);
  if (request.kind == VarRequestRead) {
    pendingReadByKey_.erase(request.key);
  }

  std::string valueText;
  if (ok) {
    std::string err;
    if (!normalizeValueText(valueJson, valueText, err)) {
      ok = false;
    }
  }

  if (request.kind == VarRequestWrite) {
    bool accepted = ok;
    bool changed = false;
    int64_t revision = 0;
    std::string finalText = valueText;
    if (accepted) {
      auto entryIt = vars_.find(request.key);
      if (entryIt == vars_.end()) {
        accepted = false;
      } else {
        std::string prev;
        changed = applyVarValue(entryIt->second, valueText, request.byPluginId,
                                prev);
        revision = entryIt->second.revision;
        finalText = entryIt->second.valueJson;
      }
    }
    Json result;
    result["requestId"] = requestId;
    result["key"] = request.key;
    result["accepted"] = accepted;
    if (accepted) {
      result["value"] = parseJsonSafe(finalText);
      result["changed"] = changed;
      result["revision"] = revision;
    } else {
      result["error"] = error.empty() ? "rejected" : error;
      result["value"] = parseJsonSafe(finalText);
    }
    const std::string payload = result.dump();
    // 写方自己收结果: 插件的即时回执走 `musicxx.var.writeResult` 事件
    // (与通知分开: 通知只表达"值变了", 回执表达"这次写的结果")
    eventBus_->publish(MUSICXX_PLUGIN_EVENT_VAR_WRITE_RESULT, payload);
    for (const PluginxxOperatorNotify &waiter : request.pluginWaiters) {
      if (waiter.done) {
        PluginxxString out = pluginxx::hostMemoryCreateString(payload);
        const PluginxxStringView view{out.data, out.size};
        waiter.done(waiter.host_ud, accepted ? PLUGINXX_OPERATOR_OK
                                             : PLUGINXX_OPERATOR_FAILED,
                    &view);
        if (out.data) {
          pluginxx::hostMemoryFree(out.data);
        }
      }
    }
    for (const auto &waiter : request.appWaiters) {
      if (waiter) {
        waiter->set(payload);
      }
    }
    return;
  }

  // 读: 顺手写进缓存 (read-through), 再把答案分发给全部等待者
  std::string answer;
  if (ok) {
    auto entryIt = vars_.find(request.key);
    if (entryIt == vars_.end()) {
      ok = false;
    } else {
      VarEntry &entry = entryIt->second;
      if (entry.valueJson != valueText) {
        // 学到了新值: 更新缓存; handler 属主不主动推时这条就是"通知的来源",
        // 因此只有 handler 模式才广播 (declared 的值变化本来就由属主 `set` 推)
        const std::string prev = entry.valueJson;
        entry.valueJson = valueText;
        entry.valueMs = steadyNowMs();
        entry.everRefreshed = true;
        publishVarMirror(entry);
        if (entry.mode == MUSICXX_PLUGIN_VAR_MODE_HANDLER) {
          pushVarChanged(entry.key, valueText, prev, entry.pluginId, false, 0);
        }
      } else {
        entry.valueMs = steadyNowMs();
        entry.everRefreshed = true;
        publishVarMirror(entry);
      }
      answer = valueText;
    }
  }
  if (ok) {
    for (const PluginxxOperatorNotify &waiter : request.pluginWaiters) {
      if (waiter.done) {
        PluginxxString out = pluginxx::hostMemoryCreateString(answer);
        const PluginxxStringView view{out.data, out.size};
        waiter.done(waiter.host_ud, PLUGINXX_OPERATOR_OK, &view);
        if (out.data) {
          pluginxx::hostMemoryFree(out.data);
        }
      }
    }
    for (const auto &waiter : request.appWaiters) {
      if (waiter) {
        Json result;
        result["ok"] = true;
        result["value"] = parseJsonSafe(answer);
        waiter->set(result.dump());
      }
    }
    return;
  }

  const std::string reason = error.empty() ? "read_failed" : error;
  for (const PluginxxOperatorNotify &waiter : request.pluginWaiters) {
    if (waiter.done) {
      PluginxxString out = pluginxx::hostMemoryCreateString(reason);
      const PluginxxStringView view{out.data, out.size};
      waiter.done(waiter.host_ud, PLUGINXX_OPERATOR_FAILED, &view);
      if (out.data) {
        pluginxx::hostMemoryFree(out.data);
      }
    }
  }
  for (const auto &waiter : request.appWaiters) {
    if (waiter) {
      Json result;
      result["ok"] = false;
      result["error"] = reason;
      waiter->set(result.dump());
    }
  }
}

void MusicxxHostManager::expireVarRequest(int64_t requestId) {
  auto it = pendingVarRequests_.find(requestId);
  if (it == pendingVarRequests_.end()) {
    return;
  }
  const bool isWrite = it->second.kind == VarRequestWrite;
  const std::string key = it->second.key;
  const std::string reason =
      isWrite ? (it->second.owner.empty() ? "not_served" : "owner_timeout")
              : "read_timeout";
  XX_LOGW("[musicxx_ext] 变量请求超时: kind={} key={} id={}", 
          isWrite ? "write" : "read", key, requestId);
  // 超时**不动缓存**: `peek` 继续返回旧值并如实报告 stale
  completeVarRequest(requestId, false, std::string{}, reason);
}

/* ==================== 应用侧入口 ==================== */

int32_t MusicxxHostManager::declareVars(const std::string &itemsJson,
                                        std::string &log) {
  bool parseOk = false;
  Json items = parseJsonSafe(itemsJson, &parseOk);
  if (!parseOk || !items.is_array()) {
    log = "var_declare: items 必须是 JSON 数组";
    return MUSICXX_EXTERN_PLUGIN_ERR_JSON;
  }
  int32_t applied = 0;
  std::string failures;
  for (const auto &item : items) {
    if (!item.is_object() || !item.contains("key") ||
        !item["key"].is_string()) {
      failures += " 一项缺少 key;";
      continue;
    }
    const std::string key = item["key"].get<std::string>();
    if (!naming::isOfficial(key) || !isValidVarKeyText(key)) {
      failures += " " + key + " 不是合法官方键;";
      continue;
    }
    int32_t caps = 0;
    if (item.contains("caps")) {
      if (item["caps"].is_array()) {
        bool capsOk = false;
        caps = capsFromTextArray(item["caps"], capsOk);
        if (!capsOk) {
          failures += " " + key + " 的 caps 含未知项;";
          continue;
        }
      } else if (item["caps"].is_number_integer()) {
        caps = item["caps"].get<int32_t>();
      }
    }
    if (caps == 0) {
      failures += " " + key + " 的 caps 为空;";
      continue;
    }
    std::string type;
    if (item.contains("type") && item["type"].is_string()) {
      type = item["type"].get<std::string>();
    }
    std::string optionsJson;
    if (item.contains("options") && item["options"].is_array()) {
      optionsJson = item["options"].dump();
    }
    std::string valueText = "null";
    bool hasValue = false;
    if (item.contains("value")) {
      valueText = item["value"].dump();
      hasValue = true;
    }
    const int32_t notifyThrottle =
        item.contains("notifyThrottleMs") && item["notifyThrottleMs"].is_number()
            ? item["notifyThrottleMs"].get<int32_t>()
            : 0;
    const int32_t throttle =
        item.contains("throttleMs") && item["throttleMs"].is_number()
            ? item["throttleMs"].get<int32_t>()
            : 0;

    auto existing = vars_.find(key);
    if (existing != vars_.end() && !existing->second.owner.empty()) {
      failures += " " + key + " 与插件变量重名;";
      continue;
    }

    VarEntry entry;
    entry.key = key;
    entry.caps = caps;
    entry.mode = MUSICXX_PLUGIN_VAR_MODE_DECLARED; ///< 官方键的写一律转给应用
    entry.writeScope = MUSICXX_PLUGIN_VAR_WRITE_ANY;
    entry.type = type;
    entry.optionsJson = optionsJson;
    entry.throttleMs = throttle < 0 ? 0 : throttle;
    entry.notifyThrottleMs = notifyThrottle;
    entry.valueJson = valueText;
    entry.everRefreshed = hasValue;
    entry.valueMs = hasValue ? steadyNowMs() : 0;
    for (const char *field : {"title", "depict", "risk"}) {
      if (item.contains(field) && item[field].is_string()) {
        const std::string text = item[field].get<std::string>();
        if (std::string{field} == "title") {
          entry.title = text;
        } else if (std::string{field} == "depict") {
          entry.depict = text;
        } else {
          entry.risk = text;
        }
      }
    }
    const bool isNew = (existing == vars_.end());
    entry.seq = isNew ? ++varSeq_ : existing->second.seq;
    if (!isNew) {
      entry.revision = existing->second.revision;
      entry.subscribers = existing->second.subscribers;
      entry.watchers = existing->second.watchers;
    }
    vars_[key] = std::move(entry);

    if (isNew) {
      for (const auto &[instanceName, keys] : varSubscriptions_) {
        if (keys.count(key) != 0) {
          vars_[key].subscribers.insert(instanceName);
        }
      }
      for (const auto &[instanceName, keys] : varWatchers_) {
        if (keys.count(key) != 0) {
          vars_[key].watchers.insert(instanceName);
        }
      }
      // 声明也是通知时机: 已经订阅了该键的实例补一条初始通知
      if (vars_[key].careCount() > 0) {
        pushVarChanged(key, valueText, std::string{"null"}, std::string{}, false,
                       0);
      }
    }
    refreshVarCare(vars_[key]);
    ++applied;
  }
  if (!failures.empty()) {
    XX_LOGW("[musicxx_ext] 变量声明有被拒绝的项:{}", failures);
    log = "部分声明被拒绝:" + failures;
  }
  XX_LOGI("[musicxx_ext] 应用声明变量 {} 条", applied);
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::applyAppVarValue(const std::string &key,
                                             const std::string &valueJson,
                                             std::string &err) {
  auto it = vars_.find(key);
  if (it == vars_.end()) {
    err = "变量不存在 (应用还没声明): " + key;
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  if (!it->second.owner.empty()) {
    err = "这是插件变量, 应用侧推值请用 var_set: " + key;
    return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
  }
  std::string valueText;
  if (!normalizeValueText(valueJson, valueText, err)) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string prev;
  const bool changed = applyVarValue(it->second, valueText, std::string{}, prev);
  if (!changed) {
    return MUSICXX_EXTERN_PLUGIN_OK;
  }
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::updateVar(const std::string &key,
                                      const std::string &valueJson,
                                      std::string &err) {
  return applyAppVarValue(key, valueJson, err);
}

int32_t MusicxxHostManager::updateVarsBatch(const std::string &itemsJson,
                                            std::string &err) {
  bool parseOk = false;
  Json items = parseJsonSafe(itemsJson, &parseOk);
  if (!parseOk || !items.is_array()) {
    err = "var_update_batch: items 必须是 JSON 数组";
    return MUSICXX_EXTERN_PLUGIN_ERR_JSON;
  }
  int32_t applied = 0;
  for (const auto &item : items) {
    if (!item.is_object() || !item.contains("key") ||
        !item["key"].is_string() || !item.contains("value")) {
      continue;
    }
    std::string reason;
    const std::string key = item["key"].get<std::string>();
    if (applyAppVarValue(key, item["value"].dump(), reason) ==
        MUSICXX_EXTERN_PLUGIN_OK) {
      ++applied;
    } else {
      XX_LOGW("[musicxx_ext] 变量批量推值被拒绝: {} ({})", key, reason);
    }
  }
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::varWriteResult(int64_t requestId, int32_t accepted,
                                           const std::string &valueJson,
                                           const std::string &error) {
  auto it = pendingVarRequests_.find(requestId);
  if (it == pendingVarRequests_.end()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  if (it->second.kind != VarRequestWrite) {
    return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  }
  completeVarRequest(requestId, accepted != 0, valueJson,
                     error.empty() ? std::string{"rejected"} : error);
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::varReadResult(int64_t requestId, int32_t ok,
                                          const std::string &valueJson,
                                          const std::string &error) {
  auto it = pendingVarRequests_.find(requestId);
  if (it == pendingVarRequests_.end()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  if (it->second.kind != VarRequestRead) {
    return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  }
  completeVarRequest(requestId, ok != 0, valueJson,
                     error.empty() ? std::string{"read_failed"} : error);
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::subscribeVarsForApp(const std::string &keysJson,
                                                bool subscribe,
                                                std::string &err) {
  bool parseOk = false;
  Json keys = parseJsonSafe(keysJson, &parseOk);
  std::vector<std::string> list;
  if (parseOk && keys.is_array()) {
    for (const auto &item : keys) {
      if (item.is_string()) {
        list.push_back(item.get<std::string>());
      }
    }
  } else if (parseOk && keys.is_string()) {
    list.push_back(keys.get<std::string>());
  } else {
    err = "var_subscribe: keys 必须是数组或字符串";
    return MUSICXX_EXTERN_PLUGIN_ERR_JSON;
  }
  for (const std::string &key : list) {
    auto it = std::find(appVarSubscriptions_.begin(),
                        appVarSubscriptions_.end(), key);
    if (subscribe && it == appVarSubscriptions_.end()) {
      appVarSubscriptions_.push_back(key);
    } else if (!subscribe && it != appVarSubscriptions_.end()) {
      appVarSubscriptions_.erase(it);
    }
  }
  return MUSICXX_EXTERN_PLUGIN_OK;
}

bool MusicxxHostManager::appCaresAboutVar(const std::string &key) const {
  for (const std::string &item : appVarSubscriptions_) {
    if (varKeyMatchesSubscription(key, item)) {
      return true;
    }
  }
  return false;
}

/* ==================== 订阅 / 保活 ==================== */

void MusicxxHostManager::markVarSubscriber(const std::string &instanceName,
                                           const std::string &key, bool watch,
                                           bool add, int32_t &outCount) {
  if (instanceName.empty() || key.empty()) {
    return;
  }
  auto &table = watch ? varWatchers_ : varSubscriptions_;
  auto &other = watch ? varSubscriptions_ : varWatchers_;
  (void)other;
  auto instIt = table.find(instanceName);
  if (add) {
    /// 订阅 + 保活不设数量上限 (早期版本的"每插件 256 条"已移除)
    table[instanceName].insert(key);
    ++outCount;
  } else {
    if (instIt == table.end()) {
      return;
    }
    if (instIt->second.erase(key) == 0) {
      return;
    }
    if (instIt->second.empty()) {
      table.erase(instIt);
    }
    ++outCount;
  }

  auto entryIt = vars_.find(key);
  if (entryIt == vars_.end()) {
    return; ///< 键还不存在: 订阅先记在实例上, 键出现时自动挂
  }
  VarEntry &entry = entryIt->second;
  if (add) {
    if (watch) {
      entry.watchers.insert(instanceName);
    } else {
      entry.subscribers.insert(instanceName);
    }
  } else {
    if (watch) {
      entry.watchers.erase(instanceName);
    } else {
      entry.subscribers.erase(instanceName);
    }
  }
  refreshVarCare(entry);
}

int32_t MusicxxHostManager::subscribeVars(MusicxxHostInstance *inst,
                                          const std::string &keysJson,
                                          int32_t &outCount) {
  return watchVars(inst, keysJson, false, outCount);
}

int32_t MusicxxHostManager::unsubscribeVars(MusicxxHostInstance *inst,
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
  outCount = 0;
  for (const auto &item : keys) {
    if (!item.is_string()) {
      continue;
    }
    const std::string text = item.get<std::string>();
    const std::string fullKey =
        naming::isOfficial(text)
            ? text
            : "plugin." + pluginIdOf(inst->name) + "." + text;
    markVarSubscriber(inst->name, fullKey, false, false, outCount);
  }
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::watchVars(MusicxxHostInstance *inst,
                                      const std::string &keysJson, bool watch,
                                      int32_t &outCount) {
  if (!inst) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  bool parseOk = false;
  Json keys = parseJsonSafe(keysJson, &parseOk);
  if (!parseOk || !keys.is_array()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_JSON;
  }
  outCount = 0;
  for (const auto &item : keys) {
    if (!item.is_string()) {
      continue;
    }
    const std::string text = item.get<std::string>();
    const std::string fullKey =
        naming::isOfficial(text)
            ? text
            : "plugin." + pluginIdOf(inst->name) + "." + text;
    // watch 需要 notify 能力位 (它就是"不带回调的订阅")
    auto entryIt = vars_.find(fullKey);
    if (entryIt != vars_.end() &&
        (entryIt->second.caps & MUSICXX_PLUGIN_VAR_CAP_NOTIFY) == 0) {
      XX_LOGW("[musicxx_ext] 插件 `{}` {} `{}` 被拒绝: 该变量没有声明 notify",
              pluginIdOf(inst->name), watch ? "watch" : "subscribe", fullKey);
      continue;
    }
    markVarSubscriber(inst->name, fullKey, watch, true, outCount);
  }
  return MUSICXX_EXTERN_PLUGIN_OK;
}

void MusicxxHostManager::refreshVarCare(const VarEntry &entry) {
  const size_t subscribers = entry.careCount();
  int64_t total = 0;
  for (const auto &[key, item] : vars_) {
    (void)key;
    total += static_cast<int64_t>(item.careCount());
  }
  varCareTotal_ = total;
  Json payload;
  payload["key"] = entry.key;
  payload["subscribers"] = static_cast<int64_t>(subscribers);
  payload["totalSubscribers"] = total;
  payload["ts"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  // 应用侧据此维护"哪些键有人看": 没人看的键值变化只更新本地缓存, 不推宿主
  pushEvent("musicxx.var.subscriptions", "", payload.dump());
  // 关心数也进同步读镜像 (JS 侧 `subscribers()` 要读)
  publishVarMirror(entry);
  // handler 属主不主动推时: 有人关心就按 refreshAfterMs 定期去取一次真实值
  ensureVarRefresh(entry.key);
}

void MusicxxHostManager::cancelVarsOf(const std::string &instanceName) {
  std::vector<std::string> keys;
  auto collect = [&](std::map<std::string, std::set<std::string>, std::less<>>
                         &table) {
    auto it = table.find(instanceName);
    if (it == table.end()) {
      return;
    }
    for (const std::string &key : it->second) {
      keys.push_back(key);
    }
    table.erase(it);
  };
  collect(varSubscriptions_);
  collect(varWatchers_);
  for (const std::string &key : keys) {
    auto entryIt = vars_.find(key);
    if (entryIt == vars_.end()) {
      continue;
    }
    entryIt->second.subscribers.erase(instanceName);
    entryIt->second.watchers.erase(instanceName);
    refreshVarCare(entryIt->second);
  }
  // 该实例发起的未回执请求也一起结算 (实例没了, 结果没人要)
  std::vector<int64_t> pending;
  for (const auto &[id, request] : pendingVarRequests_) {
    if (request.byInstance == instanceName) {
      pending.push_back(id);
    }
  }
  for (const int64_t id : pending) {
    completeVarRequest(id, false, std::string{}, "plugin_stopped");
  }
}

void MusicxxHostManager::detachVars(const std::string &instanceName) {
  const std::string pluginId = pluginIdOf(instanceName);
  // 1) 该实例注册的变量随之消失, 通知订阅者 (removed)
  std::vector<std::string> owned;
  for (const auto &[key, entry] : vars_) {
    if (entry.owner == instanceName) {
      owned.push_back(key);
    }
  }
  for (const std::string &key : owned) {
    auto it = vars_.find(key);
    if (it == vars_.end()) {
      continue;
    }
    const bool cared = it->second.careCount() > 0;
    const std::string valueText = it->second.valueJson;
    const std::string ownerId = it->second.pluginId;
    if (it->second.notifyTimer) {
      it->second.notifyTimer->cancel();
    }
    if (cared) {
      pushVarChanged(key, valueText, valueText, ownerId, true, 0);
    }
    vars_.erase(it);
    eraseVarMirror(key);
  }
  // 2) 该实例的订阅与保活一起摘掉
  cancelVarsOf(instanceName);
  if (!owned.empty()) {
    XX_LOGI("[musicxx_ext] 插件 `{}` 的 {} 个变量已随实例摘除", pluginId,
            owned.size());
  }
}

/* ==================== 值与通知 ==================== */

bool MusicxxHostManager::applyVarValue(VarEntry &entry,
                                       const std::string &valueJson,
                                       const std::string &byPluginId,
                                       std::string &prevJson) {
  if (entry.valueJson == valueJson) {
    entry.valueMs = steadyNowMs();
    prevJson = entry.valueJson;
    publishVarMirror(entry);
    return false; ///< 值没变: 不广播通知 (但算一次"刚确认过")
  }
  prevJson = entry.valueJson;
  entry.valueJson = valueJson;
  entry.valueMs = steadyNowMs();
  entry.everRefreshed = true;
  ++entry.revision;
  publishVarMirror(entry);
  pushVarChanged(entry.key, valueJson, prevJson, byPluginId, false, 0);
  return true;
}

void MusicxxHostManager::pushVarChanged(const std::string &key,
                                        const std::string &valueJson,
                                        const std::string &prevJson,
                                        const std::string &byPluginId,
                                        bool removed, int32_t coalesced) {
  auto it = vars_.find(key);
  const int64_t revision = (it != vars_.end()) ? it->second.revision : 0;
  const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count();
  Json payload;
  payload["key"] = key;
  payload["value"] = parseJsonSafe(valueJson);
  payload["prev"] = parseJsonSafe(prevJson);
  payload["by"] = byPluginId;
  payload["revision"] = revision;
  payload["ts"] = now;
  if (removed) {
    payload["removed"] = true;
  }
  if (coalesced > 0) {
    payload["coalesced"] = coalesced;
  }
  const std::string text = payload.dump();

  // 1) 发给插件: 事件总线 (订阅了 `musicxx.var.changed` 的实例收到)
  if (it != vars_.end() && it->second.careCount() > 0) {
    eventBus_->publish(MUSICXX_PLUGIN_EVENT_VAR_CHANGED, text);
  }
  // 2) 发给应用 (应用订阅了该键时才推; 没人看就不产生事件)
  if (appCaresAboutVar(key)) {
    pushEvent("musicxx.var.changed", (it != vars_.end()) ? it->second.pluginId
                                                         : std::string{},
              text);
  }
}

/* ==================== handler 属主的保活与结算 ==================== */

void MusicxxHostManager::ensureVarRefresh(const std::string &key) {
  auto it = vars_.find(key);
  if (it == vars_.end()) {
    return;
  }
  VarEntry &entry = it->second;
  const bool need = entry.mode == MUSICXX_PLUGIN_VAR_MODE_HANDLER &&
                    !entry.owner.empty() && entry.refreshAfterMs > 0 &&
                    entry.careCount() > 0;
  if (!need) {
    if (entry.refreshTimer) {
      entry.refreshTimer->cancel();
      entry.refreshTimer.reset();
    }
    return;
  }
  if (entry.refreshTimer || !ctx_) {
    return; ///< 已经在跑 (或没有 io 上下文)
  }
  auto timer = std::make_shared<asio::steady_timer>(ctx_->io);
  timer->expires_after(std::chrono::milliseconds{entry.refreshAfterMs});
  entry.refreshTimer = timer;
  auto self = shared_from_this();
  timer->async_wait([self, key](const utilxx_base::AsioErrorCode &ec) {
    if (ec) {
      return; ///< 已取消 (没人关心了/变量摘除了)
    }
    self->onVarRefreshTick(key);
  });
}

void MusicxxHostManager::onVarRefreshTick(const std::string &key) {
  auto it = vars_.find(key);
  if (it == vars_.end()) {
    return;
  }
  it->second.refreshTimer.reset(); ///< 本次用完了, 下面重新排
  if (it->second.refreshAfterMs <= 0 || it->second.careCount() == 0) {
    return;
  }
  // 去属主那里取一次真实值: 没有等待者, 只为把同步读缓存刷新
  beginVarRead(key, std::string{}, nullptr, nullptr, nullptr);
  ensureVarRefresh(key);
}

void MusicxxHostManager::settleVarRequestsOnCommit(
    const std::string &key, const std::string &valueJson) {
  std::vector<int64_t> ids;
  for (const auto &[id, request] : pendingVarRequests_) {
    if (request.key == key) {
      ids.push_back(id);
    }
  }
  for (const int64_t id : ids) {
    auto it = pendingVarRequests_.find(id);
    if (it == pendingVarRequests_.end()) {
      continue;
    }
    // 读: 属主提交的值就是答案; 写: 提交即视为写完
    completeVarRequest(id, true, valueJson, std::string{});
  }
}

int32_t MusicxxHostManager::respondVar(MusicxxHostInstance *inst,
                                       int64_t requestId, bool ok,
                                       const std::string &valueJson,
                                       const std::string &error,
                                       std::string &outJson) {
  if (!inst || requestId <= 0) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  auto it = pendingVarRequests_.find(requestId);
  if (it == pendingVarRequests_.end()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  if (it->second.owner != inst->name) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 试图回答不属于自己的变量请求 {}", inst->name,
            requestId);
    return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
  }
  const bool isWrite = it->second.kind == VarRequestWrite;
  completeVarRequest(requestId, ok, valueJson, error);
  Json result;
  result["requestId"] = requestId;
  result[isWrite ? "accepted" : "ok"] = ok;
  if (ok) {
    result["value"] = parseJsonSafe(valueJson);
  } else {
    result["error"] = error.empty() ? "rejected" : error;
  }
  outJson = result.dump();
  return MUSICXX_EXTERN_PLUGIN_OK;
}

/* ==================== 同步读镜像 / 自省 ==================== */

std::string MusicxxHostManager::varMirrorJsonText(const VarEntry &entry) const {
  Json item;
  item["key"] = entry.key;
  item["owner"] = entry.pluginId;
  item["caps"] = capsToJson(entry.caps);
  item["mode"] = varModeName(entry.mode);
  item["write"] = varWriteScopeName(entry.writeScope);
  item["type"] = entry.type;
  if (!entry.optionsJson.empty()) {
    item["options"] = parseJsonSafe(entry.optionsJson);
  }
  item["title"] = entry.title;
  item["depict"] = entry.depict;
  item["risk"] = entry.risk;
  item["throttleMs"] = entry.throttleMs;
  item["notifyThrottleMs"] = entry.notifyThrottleMs;
  item["refreshAfterMs"] = entry.refreshAfterMs;
  /// 值字段明确是**缓存值** (真值在属主那里); `hasValue=false` 表示还没缓存过
  item["value"] = parseJsonSafe(entry.valueJson);
  item["hasValue"] = entry.everRefreshed;
  item["revision"] = entry.revision;
  item["valueMs"] = entry.valueMs;
  item["subscribers"] = static_cast<int64_t>(entry.careCount());
  return item.dump();
}

void MusicxxHostManager::publishVarMirror(const VarEntry &entry) {
  const std::string text = varMirrorJsonText(entry);
  std::lock_guard<std::mutex> lock{varMirrorMutex_};
  varMirror_[entry.key] = text;
}

void MusicxxHostManager::eraseVarMirror(const std::string &key) {
  std::lock_guard<std::mutex> lock{varMirrorMutex_};
  varMirror_.erase(key);
}

std::string MusicxxHostManager::varToJsonText(const VarEntry &entry) const {
  const bool fresh = entry.everRefreshed && entry.valueMs > 0;
  const int64_t ageMs = fresh ? steadyNowMs() - entry.valueMs : 0;
  Json item = parseJsonSafe(varMirrorJsonText(entry));
  item["ageMs"] = ageMs;
  item["stale"] =
      !fresh || (entry.subscribers.empty() && entry.watchers.empty()) ||
      ageMs > kVarReadTimeoutMs;
  return item.dump();
}

/// 给一个镜像快照补上"与时刻有关"的字段 (ageMs / stale)
std::string withFreshness(const std::string &snapshot, bool cared) {
  bool ok = false;
  Json item = parseJsonSafe(snapshot, &ok);
  if (!ok || !item.is_object()) {
    return snapshot;
  }
  const bool hasValue = item.contains("hasValue") && item["hasValue"].is_boolean() &&
                        item["hasValue"].get<bool>();
  const int64_t valueMs = item.contains("valueMs") && item["valueMs"].is_number()
                              ? item["valueMs"].get<int64_t>()
                              : 0;
  const int64_t ageMs = (hasValue && valueMs > 0) ? steadyNowMs() - valueMs : 0;
  item["ageMs"] = ageMs;
  item["stale"] = !hasValue || ageMs > kVarReadTimeoutMs || !cared;
  return item.dump();
}

std::string MusicxxHostManager::varsSnapshotText() const {
  std::vector<const VarEntry *> items;
  items.reserve(vars_.size());
  for (const auto &[key, entry] : vars_) {
    (void)key;
    items.push_back(&entry);
  }
  std::sort(items.begin(), items.end(), [](const VarEntry *a, const VarEntry *b) {
    return a->seq < b->seq;
  });
  Json array = Json::array();
  for (const VarEntry *entry : items) {
    array.push_back(parseJsonSafe(varToJsonText(*entry)));
  }
  return array.dump();
}

std::string MusicxxHostManager::varsDebugJson() const {
  int32_t getCaps = 0;
  int32_t setCaps = 0;
  int32_t notifyCaps = 0;
  int64_t subscribers = 0;
  int32_t appVars = 0;
  for (const auto &[key, entry] : vars_) {
    (void)key;
    if ((entry.caps & MUSICXX_PLUGIN_VAR_CAP_GET) != 0) {
      ++getCaps;
    }
    if ((entry.caps & MUSICXX_PLUGIN_VAR_CAP_SET) != 0) {
      ++setCaps;
    }
    if ((entry.caps & MUSICXX_PLUGIN_VAR_CAP_NOTIFY) != 0) {
      ++notifyCaps;
    }
    if (entry.owner.empty()) {
      ++appVars;
    }
    subscribers += static_cast<int64_t>(entry.careCount());
  }
  int32_t pendingRead = 0;
  int32_t pendingWrite = 0;
  for (const auto &[id, request] : pendingVarRequests_) {
    (void)id;
    if (request.kind == VarRequestRead) {
      ++pendingRead;
    } else {
      ++pendingWrite;
    }
  }
  Json info;
  info["count"] = static_cast<int64_t>(vars_.size());
  info["appVars"] = appVars;
  info["pluginVars"] = static_cast<int64_t>(vars_.size()) - appVars;
  info["caps"] = {{"get", getCaps}, {"set", setCaps}, {"notify", notifyCaps}};
  info["subscribers"] = subscribers;
  info["pendingRead"] = pendingRead;
  info["pendingWrite"] = pendingWrite;
  info["appSubscriptions"] =
      static_cast<int64_t>(appVarSubscriptions_.size());
  return info.dump();
}

int32_t MusicxxHostManager::listVars(std::string_view prefix,
                                     std::string &outJson) {
  const std::string head{prefix};
  // 只读镜像 (任何线程可用; 顺序按 key, 便于比对)
  std::vector<std::pair<std::string, std::string>> items;
  {
    std::lock_guard<std::mutex> lock{varMirrorMutex_};
    items.reserve(varMirror_.size());
    for (const auto &[key, snapshot] : varMirror_) {
      if (!head.empty() && key.compare(0, head.size(), head) != 0) {
        continue;
      }
      items.emplace_back(key, snapshot);
    }
  }
  std::sort(items.begin(), items.end(),
            [](const auto &a, const auto &b) { return a.first < b.first; });
  Json array = Json::array();
  for (const auto &[key, snapshot] : items) {
    (void)key;
    Json item = parseJsonSafe(snapshot);
    const bool cared = item.contains("subscribers") && item["subscribers"].is_number() &&
                       item["subscribers"].get<int64_t>() > 0;
    array.push_back(parseJsonSafe(withFreshness(snapshot, cared)));
  }
  outJson = array.dump();
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::varInfo(std::string_view key,
                                    std::string &outJson) {
  std::string snapshot;
  {
    std::lock_guard<std::mutex> lock{varMirrorMutex_};
    auto it = varMirror_.find(std::string{key});
    if (it == varMirror_.end()) {
      return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
    }
    snapshot = it->second;
  }
  Json item = parseJsonSafe(snapshot);
  const bool cared = item.contains("subscribers") && item["subscribers"].is_number() &&
                     item["subscribers"].get<int64_t>() > 0;
  outJson = withFreshness(snapshot, cared);
  return MUSICXX_EXTERN_PLUGIN_OK;
}

/* ==================== C ABI: musicxx.vars 表 ==================== */

namespace {

/// 入参视图 → std::string (空视图安全)
std::string viewText(const PluginxxStringView *view) {
  if (!view || !view->data) {
    return {};
  }
  return std::string{view->data, static_cast<size_t>(view->size)};
}

int32_t PLUGINXX_CALL xx_vars_register(const PluginxxHost *host,
                                       const MusicxxPluginVarSpec *spec) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!spec) {
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
        const MusicxxPluginVarSpec specCopy = *spec;
        return pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, inst, specCopy]() -> int32_t {
              return mgr->registerVar(inst, specCopy);
            });
      });
}

int32_t PLUGINXX_CALL xx_vars_unregister(const PluginxxHost *host,
                                         const PluginxxStringView *key) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!key || !key->data) {
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
        const std::string text = viewText(key);
        return pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, inst, text]() -> int32_t {
              return mgr->unregisterVar(inst, text);
            });
      });
}

int32_t PLUGINXX_CALL xx_vars_get(const PluginxxHost *host,
                                  const PluginxxStringView *key,
                                  const PluginxxOperatorNotify *notify,
                                  int64_t *outRequestId) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!key || !key->data) {
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
        const std::string text = viewText(key);
        // `notify` 由调用方 (插件/JS 桥) 构造: 这里按值复制它的 "done + host_ud"
        // (宿主线程不等待, 结果稍后回调)
        PluginxxOperatorNotify notifyCopy{};
        const bool hasNotify = (notify != nullptr && notify->done != nullptr);
        if (hasNotify) {
          notifyCopy = *notify;
        }
        int64_t requestId = 0;
        const int32_t rc = pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, inst, text, notifyCopy, hasNotify,
                        &requestId]() -> int32_t {
              return mgr->getVar(inst, text, hasNotify ? &notifyCopy : nullptr,
                                 &requestId);
            });
        if (rc == MUSICXX_EXTERN_PLUGIN_OK && outRequestId) {
          *outRequestId = requestId;
        }
        return rc;
      });
}

int32_t PLUGINXX_CALL xx_vars_peek(const PluginxxHost *host,
                                   const PluginxxStringView *key,
                                   PluginxxString *outJson) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!key || !key->data || !outJson) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto call =
            pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
                host, /*allowClosing=*/true);
        if (!call.ok()) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        // 只读同步镜像: 可以直接在调用线程回答 (JS 线程绝不等宿主线程)
        std::string json;
        const int32_t rc =
            call.manager()->peekVar(viewText(key), json);
        if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
          return rc;
        }
        pluginxx::hostMemorySetString(outJson, json);
        return MUSICXX_EXTERN_PLUGIN_OK;
      });
}

int32_t PLUGINXX_CALL xx_vars_set(const PluginxxHost *host,
                                  const PluginxxStringView *key,
                                  const PluginxxStringView *valueJson,
                                  PluginxxString *outJson) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!key || !key->data || !outJson) {
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
        const std::string text = viewText(key);
        const std::string value = viewText(valueJson);
        std::string result;
        const int32_t rc = pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, inst, text, value, &result]() -> int32_t {
              return mgr->setVar(inst, text, value, result);
            });
        if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
          return rc;
        }
        pluginxx::hostMemorySetString(outJson, result);
        return MUSICXX_EXTERN_PLUGIN_OK;
      });
}

int32_t PLUGINXX_CALL xx_vars_list(const PluginxxHost *host,
                                   const PluginxxStringView *prefix,
                                   PluginxxString *outJson) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!outJson) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto call =
            pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
                host, /*allowClosing=*/true);
        if (!call.ok()) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        // 与 peek 同一套: 读同步镜像, 不投递到宿主线程
        std::string json;
        const int32_t rc = call.manager()->listVars(viewText(prefix), json);
        if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
          return rc;
        }
        pluginxx::hostMemorySetString(outJson, json);
        return MUSICXX_EXTERN_PLUGIN_OK;
      });
}

int32_t PLUGINXX_CALL xx_vars_subscribe(const PluginxxHost *host,
                                        const PluginxxStringView *keysJson,
                                        int32_t *outCount) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!keysJson || !keysJson->data) {
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
        const std::string keys = viewText(keysJson);
        int32_t count = 0;
        const int32_t rc = pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, inst, keys, &count]() -> int32_t {
              return mgr->subscribeVars(inst, keys, count);
            });
        if (rc == MUSICXX_EXTERN_PLUGIN_OK && outCount) {
          *outCount = count;
        }
        return rc;
      });
}

int32_t PLUGINXX_CALL xx_vars_unsubscribe(const PluginxxHost *host,
                                          const PluginxxStringView *keysJson,
                                          int32_t *outCount) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!keysJson || !keysJson->data) {
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
        const std::string keys = viewText(keysJson);
        int32_t count = 0;
        const int32_t rc = pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, inst, keys, &count]() -> int32_t {
              return mgr->unsubscribeVars(inst, keys, count);
            });
        if (rc == MUSICXX_EXTERN_PLUGIN_OK && outCount) {
          *outCount = count;
        }
        return rc;
      });
}

int32_t PLUGINXX_CALL xx_vars_watch(const PluginxxHost *host,
                                    const PluginxxStringView *keysJson,
                                    int32_t *outCount) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!keysJson || !keysJson->data) {
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
        const std::string keys = viewText(keysJson);
        int32_t count = 0;
        const int32_t rc = pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, inst, keys, &count]() -> int32_t {
              return mgr->watchVars(inst, keys, true, count);
            });
        if (rc == MUSICXX_EXTERN_PLUGIN_OK && outCount) {
          *outCount = count;
        }
        return rc;
      });
}

int32_t PLUGINXX_CALL xx_vars_unwatch(const PluginxxHost *host,
                                      const PluginxxStringView *keysJson,
                                      int32_t *outCount) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!keysJson || !keysJson->data) {
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
        const std::string keys = viewText(keysJson);
        int32_t count = 0;
        const int32_t rc = pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, inst, keys, &count]() -> int32_t {
              return mgr->watchVars(inst, keys, false, count);
            });
        if (rc == MUSICXX_EXTERN_PLUGIN_OK && outCount) {
          *outCount = count;
        }
        return rc;
      });
}

int32_t PLUGINXX_CALL xx_vars_info(const PluginxxHost *host,
                                   const PluginxxStringView *key,
                                   PluginxxString *outJson) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!key || !key->data || !outJson) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto call =
            pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
                host, /*allowClosing=*/true);
        if (!call.ok()) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        // 与 peek 同一套: 读同步镜像, 不投递到宿主线程
        std::string json;
        const int32_t rc = call.manager()->varInfo(viewText(key), json);
        if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
          return rc;
        }
        pluginxx::hostMemorySetString(outJson, json);
        return MUSICXX_EXTERN_PLUGIN_OK;
      });
}

int32_t PLUGINXX_CALL xx_vars_respond(const PluginxxHost *host,
                                      int64_t requestId, int32_t ok,
                                      const PluginxxStringView *valueJson,
                                      const PluginxxStringView *error,
                                      PluginxxString *outJson) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        if (!outJson) {
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
        const std::string value = viewText(valueJson);
        const std::string reason = viewText(error);
        std::string result;
        const int32_t rc = pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, inst, requestId, ok, value, reason,
                        &result]() -> int32_t {
              return mgr->respondVar(inst, requestId, ok != 0, value, reason,
                                     result);
            });
        if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
          return rc;
        }
        pluginxx::hostMemorySetString(outJson, result);
        return MUSICXX_EXTERN_PLUGIN_OK;
      });
}

const MusicxxPluginVarsIface g_ifaceVars = {
    /* version */ MUSICXX_PLUGIN_IFACE_VARS_VERSION,
    /* struct_size */ sizeof(MusicxxPluginVarsIface),
    /* register_var */ xx_vars_register,
    /* unregister_var */ xx_vars_unregister,
    /* get */ xx_vars_get,
    /* peek */ xx_vars_peek,
    /* watch */ xx_vars_watch,
    /* unwatch */ xx_vars_unwatch,
    /* set */ xx_vars_set,
    /* list */ xx_vars_list,
    /* subscribe */ xx_vars_subscribe,
    /* unsubscribe */ xx_vars_unsubscribe,
    /* info */ xx_vars_info,
    /* respond */ xx_vars_respond,
};

} // namespace

const void *varsIfaceForQuery() { return &g_ifaceVars; }

} // namespace extern_plugin
} // namespace musicxx
