/// musicxx 外部插件原生插件框架: 变量表 (第五条通道)
///
/// 变量 = **有属主、可读、可写、可订阅变动**的小值 (配置/小状态)。它与既有四条通道
/// 分工不同: 能力做拦截、快照频道是"应用推 + 插件同步只读"、事件只通知没有当前值、
/// 动作是命令; 变量则把"谁提供 / 能不能写 / 值是什么"合在一个明确的模型里。
///
/// 设计要点:
/// - **插件框架里没有真值**: 真值只在属主那里 (主程序的 Store/配置, 或插件自己的状态)。
///   这里的 `ChannelEntry::valueJson` 只是服务 `peek` 这条同步捷径的**缓存**, 来源有三处 ——
///   属主推送、每次异步读的结果 (read-through)、注册时的初值;
/// - **读默认异步**(`get`): 插件框架把请求转给属主, 属主回答后回给调用方; 插件框架的线程全程
///   不等 (等的是发起方)。同一个键的进行中读请求合并成一次属主往返, 答案分发给全部等待者;
/// - **写一律交给属主写入**: 官方键交给应用 (走设置同一条路径), 插件 `declared` 模式由
///   框架代存 (= 属主已同意), `handler` 模式转给属主插件 (§M3 实现);
/// - **注册表归框架线程独占** (无锁不变式): 注册/注销/读写/通知/摘除都在框架线程;
///   Dart 线程的入口经 C ABI 侧投递后执行;
/// - **零成本**: 插件框架没启动时不声明、不订阅; 主程序值变化只在"该键有人关心 (订阅或
///   watch) 且插件框架在跑"时才推 (关心数由这里回传给 Dart);
/// - **高频值建议节流**: 属主按 `throttleMs` 合并推送, 插件框架按 `notifyThrottleMs` 合并
///   通知 (载荷带 `coalesced`), 且没人关心就不推; 这些窗口只是声明, 插件框架不强制度量。
///
/// 容量与大小都不再限制 (变量数 / 变量总数 / 键长 / 值大小 / 订阅数); 唯一的容量类
/// 限制是"每个插件未回执的读写请求数" (避免慢属主把框架事件队列灌满)。
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

/// 变量快照的广播频道 id: `musicxx.broadcast.<key>`
///
/// 用这个前缀而不是直接用变量键: 频道 id 要进一张全局的广播表, 带命名空间才不会与
/// 各键的快照频道 / 业务事件 / 插件频道混在一起 (宿主也按这个前缀给默认声明)。
std::string channelSnapshotId(std::string_view key) {
  std::string out{"musicxx.broadcast."};
  out.append(key);
  return out;
}

} // namespace

namespace {

/// 等属主回答读取、写入值的超时
constexpr int64_t kChannelReadTimeoutMs = 2000;
constexpr int64_t kChannelWriteOwnerTimeoutMs = 3000;
constexpr int64_t kChannelWriteAppTimeoutMs = 5000;

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
bool isValidChannelKeyText(std::string_view key) {
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
bool normalizeChannelKey(std::string_view raw, std::string_view pluginId,
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
    if (!isValidChannelKeyText(text)) {
      err = "键名不合法 (只允许 A-Za-z0-9_.-, 不能有连续点): " + text;
      return false;
    }
    outKey = text;
    return true;
  }
  // 短名 (可以多级分组, 如 tip.start)
  if (text.find('.') == std::string::npos) {
    if (!isValidChannelKeyText(text)) {
      err = "键名不合法 (只允许 A-Za-z0-9_.-): " + text;
      return false;
    }
  } else if (!isValidChannelKeyText(text)) {
    err = "键名不合法 (只允许 A-Za-z0-9_.-, 不能有连续点): " + text;
    return false;
  }
  if (pluginId.empty()) {
    err = "插件命名空间为空, 无法补全键: " + text;
    return false;
  }
  outKey = "plugin." + std::string{pluginId} + "." + text;
  if (!isValidChannelKeyText(outKey)) {
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
      caps |= MUSICXX_PLUGIN_CHANNEL_CAP_GET;
    } else if (name == "set") {
      caps |= MUSICXX_PLUGIN_CHANNEL_CAP_SET;
    } else if (name == "notify") {
      caps |= MUSICXX_PLUGIN_CHANNEL_CAP_NOTIFY;
    } else {
      ok = false;
      return 0;
    }
  }
  return caps;
}

Json capsToJson(int32_t caps) {
  Json array = Json::array();
  if ((caps & MUSICXX_PLUGIN_CHANNEL_CAP_GET) != 0) {
    array.push_back("get");
  }
  if ((caps & MUSICXX_PLUGIN_CHANNEL_CAP_SET) != 0) {
    array.push_back("set");
  }
  if ((caps & MUSICXX_PLUGIN_CHANNEL_CAP_NOTIFY) != 0) {
    array.push_back("notify");
  }
  return array;
}

const char *channelModeName(int32_t mode) {
  return mode == MUSICXX_PLUGIN_CHANNEL_MODE_HANDLER ? "handler" : "declared";
}

const char *channelWriteScopeName(int32_t scope) {
  return scope == MUSICXX_PLUGIN_CHANNEL_WRITE_OWNER ? "owner" : "any";
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
bool channelKeyMatchesSubscription(const std::string &key,
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

int32_t MusicxxHostManager::registerChannel(MusicxxHostInstance *inst,
                                        const MusicxxPluginChannelSpec &spec) {
  if (!inst) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (spec.struct_size != 0 && spec.struct_size < sizeof(MusicxxPluginChannelSpec)) {
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
  /// 声明里的键原文：C ABI 的 size 是 uint64_t，32 位目标上要显式收窄给 std::string
  const std::string keyText{spec.key.data, static_cast<size_t>(spec.key.size)};
  if (spec.caps == 0) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册变量 `{}` 被拒绝: caps 不能为空",
            inst->name, keyText);
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if ((spec.caps & ~(MUSICXX_PLUGIN_CHANNEL_CAP_GET | MUSICXX_PLUGIN_CHANNEL_CAP_SET |
                     MUSICXX_PLUGIN_CHANNEL_CAP_NOTIFY)) != 0) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (spec.write_scope != MUSICXX_PLUGIN_CHANNEL_WRITE_ANY &&
      spec.write_scope != MUSICXX_PLUGIN_CHANNEL_WRITE_OWNER) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (spec.mode != MUSICXX_PLUGIN_CHANNEL_MODE_DECLARED &&
      spec.mode != MUSICXX_PLUGIN_CHANNEL_MODE_HANDLER) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册变量: 未知模式 {} (取值 0/1)",
            inst->name, spec.mode);
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  /// 节流/保活窗口只是"属主侧的规则声明", 不再做取值范围校验 (负值按 0 处理):
  /// 它不参与插件框架的行为判定, 拒绝注册只会给作者添麻烦。

  const std::string pluginId = pluginIdOf(inst->name);
  std::string key;
  std::string err;
  if (!normalizeChannelKey(keyText, pluginId, key, err)) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册变量 `{}` 被拒绝: {}", inst->name,
            keyText, err);
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

  auto existing = channels_.find(key);
  if (existing != channels_.end() && existing->second.owner != inst->name) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 试图覆盖他人变量 `{}`", inst->name, key);
    return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
  }
  /// 变量数不设上限 (早期版本的"每插件 64 个 / 总数 4096"已移除)。

  ChannelEntry entry;
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
  entry.seq = (existing != channels_.end()) ? existing->second.seq : ++channelSeq_;
  const bool isNew = (existing == channels_.end());
  if (!isNew) {
    // 覆盖式注册: 订阅关系与修订号沿用 (作者改的是声明, 不是值)
    entry.subscribers = existing->second.subscribers;
    entry.watchers = existing->second.watchers;
    entry.revision = existing->second.revision;
  }
  channels_[key] = std::move(entry);

  if (isNew) {
    // 先订阅、后声明: 键出现时把已有订阅挂上, 并补一条初始通知
    // (值 = 刚声明的初值), 这样插件可以先绑定、后读到值
    for (const auto &[instanceName, keys] : channelSubscriptions_) {
      if (keys.count(key) != 0) {
        channels_[key].subscribers.insert(instanceName);
      }
    }
    for (const auto &[instanceName, keys] : channelWatchers_) {
      if (keys.count(key) != 0) {
        channels_[key].watchers.insert(instanceName);
      }
    }
  }
  if (channels_[key].careCount() > 0) {
    pushChannelChanged(key, valueText, std::string{"null"}, pluginId, false, 0);
  }
  refreshChannelCare(channels_[key]);
  XX_LOGI("[musicxx_ext] 插件 `{}` 注册变量 `{}` (caps={} mode={} write={})",
          pluginId, key, capsToJson(spec.caps).dump(), channelModeName(spec.mode),
          channelWriteScopeName(spec.write_scope));
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::unregisterChannel(MusicxxHostInstance *inst,
                                          std::string_view key) {
  if (key.empty()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  const std::string pluginId = inst ? pluginIdOf(inst->name) : std::string{};
  std::string fullKey;
  std::string err;
  if (!normalizeChannelKey(key, pluginId, fullKey, err)) {
    return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
  }
  auto it = channels_.find(fullKey);
  if (it == channels_.end()) {
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
    pushChannelChanged(fullKey, valueText, valueText, ownerId, true, 0);
  }
  if (it->second.notifyTimer) {
    it->second.notifyTimer->cancel();
  }
  channels_.erase(it);
  eraseChannelSnapshot(fullKey);
  return MUSICXX_EXTERN_PLUGIN_OK;
}

size_t MusicxxHostManager::countChannelsOfPlugin(const std::string &pluginId) const {
  size_t count = 0;
  for (const auto &[key, entry] : channels_) {
    (void)key;
    if (entry.pluginId == pluginId && !entry.owner.empty()) {
      ++count;
    }
  }
  return count;
}

/* ==================== 读 ==================== */

int32_t MusicxxHostManager::beginChannelRead(
    const std::string &key, const std::string &byPluginId,
    const PluginxxOperatorNotify *notify,
    std::shared_ptr<WaitSlot<std::string>> appSlot, int64_t *outRequestId) {
  auto it = channels_.find(key);
  if (it == channels_.end()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  ChannelEntry &entry = it->second;
  if ((entry.caps & MUSICXX_PLUGIN_CHANNEL_CAP_GET) == 0) {
    return MUSICXX_PLUGIN_CHANNEL_ERR_UNSUPPORTED;
  }
  if (outRequestId) {
    *outRequestId = 0;
  }

  // 属主是应用 (官方键): 需要一次 Dart 往返; 插件 handler 属主同理 (§M3)
  const bool needOwner = entry.owner.empty() ||
                         entry.mode == MUSICXX_PLUGIN_CHANNEL_MODE_HANDLER;
  if (!needOwner) {
    // `declared` 模式的插件变量: 值就在注册表里, 插件框架即属主 → 立即回答
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
    auto reqIt = pendingChannelRequests_.find(pendingIt->second);
    if (reqIt != pendingChannelRequests_.end()) {
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

  // 未回执读请求的数量上限 (避免慢属主把框架事件队列灌满)
  if (!byPluginId.empty()) {
    int32_t pending = 0;
    for (const auto &[id, req] : pendingChannelRequests_) {
      (void)id;
      if (req.kind == ChannelRequestRead && req.byPluginId == byPluginId) {
        ++pending;
      }
    }
    if (pending >= kMaxChannelPendingPerPlugin) {
      return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
  }

  const int64_t requestId = nextRequestId_++;
  PendingChannelRequest request;
  request.key = key;
  request.byPluginId = byPluginId;
  request.kind = ChannelRequestRead;
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
    timer->expires_after(std::chrono::milliseconds{kChannelReadTimeoutMs});
    auto self = shared_from_this();
    timer->async_wait([self, requestId](const utilxx_base::AsioErrorCode &ec) {
      if (ec) {
        return; ///< 已结算: 定时器被主动取消
      }
      self->expireChannelRequest(requestId);
    });
    request.timer = std::move(timer);
  }
  pendingChannelRequests_[requestId] = std::move(request);
  pendingReadByKey_[key] = requestId;
  if (outRequestId) {
    *outRequestId = requestId;
  }

  if (entry.owner.empty()) {
    // 官方键: 转给应用 (主程序直接问实现, 本地权威)
    Json payload;
    payload["requestId"] = requestId;
    payload["key"] = key;
    pushEvent("musicxx.broadcast.read", "", payload.dump());
  } else {
    // handler 属主: 用事件总线把请求转给属主实例 (属主订阅 `musicxx.broadcast.read`)
    Json payload;
    payload["requestId"] = requestId;
    payload["key"] = key;
    publishOnHostThread(MUSICXX_PLUGIN_EVENT_BROADCAST_READ, payload.dump());
  }
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::getChannel(MusicxxHostInstance *inst,
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
  } else if (!normalizeChannelKey(key, pluginIdOf(inst->name), fullKey, err)) {
    return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
  }
  return beginChannelRead(fullKey, pluginIdOf(inst->name), notify, nullptr,
                      outRequestId);
}

int32_t MusicxxHostManager::getChannelForApp(
    const std::string &key, std::string &outJson,
    std::shared_ptr<WaitSlot<std::string>> slot) {
  if (key.empty() || !slot) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  const int32_t rc = beginChannelRead(key, "", nullptr, slot, nullptr);
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    outJson.clear();
  }
  return rc;
}

int32_t MusicxxHostManager::peekChannel(std::string_view key, std::string &outJson) {
  // 只读镜像 (任何线程可用): JS 线程绝不等框架线程, 快照在广播注册表里
  std::string snapshot;
  if (!broadcasts()->read(channelSnapshotId(key), &snapshot)) {
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
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
      return MUSICXX_PLUGIN_CHANNEL_ERR_UNSUPPORTED;
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
  out["stale"] = !cared || ageMs > kChannelReadTimeoutMs;
  outJson = out.dump();
  return MUSICXX_EXTERN_PLUGIN_OK;
}

/* ==================== 写 ==================== */

int32_t MusicxxHostManager::beginChannelWrite(
    const std::string &key, const std::string &valueJson,
    const std::string &byPluginId, const std::string &byInstance,
    const PluginxxOperatorNotify *notify,
    std::shared_ptr<WaitSlot<std::string>> appSlot, std::string &outJson) {
  auto it = channels_.find(key);
  if (it == channels_.end()) {
    outJson.clear();
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  ChannelEntry &entry = it->second;
  if ((entry.caps & MUSICXX_PLUGIN_CHANNEL_CAP_SET) == 0) {
    outJson.clear();
    return MUSICXX_PLUGIN_CHANNEL_ERR_UNSUPPORTED;
  }
  // 写权限: `write: owner` 只允许属主自己写 (应用写官方键不走这条路径)
  if (entry.writeScope == MUSICXX_PLUGIN_CHANNEL_WRITE_OWNER &&
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
                       entry.mode == MUSICXX_PLUGIN_CHANNEL_MODE_HANDLER;
  const bool isOwnerWrite = (byInstance == ownerInstance);

  if (!forward || isOwnerWrite) {
    // 属主自己写自己的变量 = 提交 (declared 模式的插件框架就是写入者);
    // 这样也不会出现"属主写自己的变量还要转给自己"的死循环
    std::string prev;
    const bool changed = applyChannelValue(entry, valueText, byPluginId, prev);
    // 提交同时结算该键上未完成的请求: 写请求 = 写入回执, 读请求 = 这次读的答案
    // (handler 属主用 `set` 回答与提交, 见设计文档 §3.5)
    settleChannelRequestsOnCommit(key, valueText);
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

  // 未回执写请求的数量上限 (避免慢属主把框架事件队列灌满)
  if (!byPluginId.empty()) {
    int32_t pending = 0;
    for (const auto &[id, req] : pendingChannelRequests_) {
      (void)id;
      if (req.kind == ChannelRequestWrite && req.byPluginId == byPluginId) {
        ++pending;
      }
    }
    if (pending >= kMaxChannelPendingPerPlugin) {
      outJson.clear();
      return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
  }

  const int64_t requestId = nextRequestId_++;
  PendingChannelRequest request;
  request.key = key;
  request.byPluginId = byPluginId;
  request.byInstance = byInstance;
  request.kind = ChannelRequestWrite;
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
        ownerInstance.empty() ? kChannelWriteAppTimeoutMs : kChannelWriteOwnerTimeoutMs;
    auto timer = std::make_shared<asio::steady_timer>(ctx_->io);
    timer->expires_after(std::chrono::milliseconds{budget});
    auto self = shared_from_this();
    timer->async_wait([self, requestId](const utilxx_base::AsioErrorCode &ec) {
      if (ec) {
        return; ///< 已结算: 定时器被主动取消
      }
      self->expireChannelRequest(requestId);
    });
    request.timer = std::move(timer);
  }
  pendingChannelRequests_[requestId] = std::move(request);

  Json payload;
  payload["requestId"] = requestId;
  payload["key"] = key;
  payload["value"] = parseJsonSafe(valueText);
  payload["by"] = byPluginId;
  if (ownerInstance.empty()) {
    // 官方键: 请求应用按"用户在设置里改"的同一条路径写入
    pushEvent("musicxx.broadcast.write", "", payload.dump());
  } else {
    // handler 属主: 用事件总线把写请求转给属主实例 (属主订阅 `musicxx.broadcast.write`)
    publishOnHostThread(MUSICXX_PLUGIN_EVENT_BROADCAST_WRITE, payload.dump());
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

int32_t MusicxxHostManager::setChannel(MusicxxHostInstance *inst,
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
  } else if (!normalizeChannelKey(key, pluginIdOf(inst->name), fullKey, err)) {
    return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
  }
  return beginChannelWrite(fullKey, std::string{valueJson},
                       pluginIdOf(inst->name), inst->name, nullptr, nullptr,
                       outJson);
}

int32_t MusicxxHostManager::setChannelForApp(
    const std::string &key, const std::string &valueJson, std::string &outJson,
    std::shared_ptr<WaitSlot<std::string>> slot) {
  if (key.empty() || !slot) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  // `beginChannelWrite` 在"交给属主写入"时会写一个 `{pending:true,...}` 信封 —— 那对
  // 主程序没有用 (应用要的是最终结果), 因此这里只把**已结算**的结果交出去:
  // 结果是"待写入"时清空 outJson, 让调用方去等等待槽 (C ABI 就是这么区分的)。
  std::string local;
  const int32_t rc =
      beginChannelWrite(key, valueJson, "", "", nullptr, slot, local);
  if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
    outJson = local;
    return rc;
  }
  outJson = (local.find("\"accepted\"") != std::string::npos) ? local
                                                             : std::string{};
  return MUSICXX_EXTERN_PLUGIN_OK;
}

/* ==================== 结算 ==================== */

bool MusicxxHostManager::hasPendingChannelRequest(int64_t requestId) const {
  return pendingChannelRequests_.find(requestId) != pendingChannelRequests_.end();
}

void MusicxxHostManager::completeChannelRequest(int64_t requestId, bool ok,
                                            const std::string &valueJson,
                                            const std::string &error) {
  auto it = pendingChannelRequests_.find(requestId);
  if (it == pendingChannelRequests_.end()) {
    return;
  }
  if (it->second.timer) {
    it->second.timer->cancel();
  }
  PendingChannelRequest request = std::move(it->second);
  pendingChannelRequests_.erase(it);
  if (request.kind == ChannelRequestRead) {
    pendingReadByKey_.erase(request.key);
  }

  std::string valueText;
  if (ok) {
    std::string err;
    if (!normalizeValueText(valueJson, valueText, err)) {
      ok = false;
    }
  }

  if (request.kind == ChannelRequestWrite) {
    bool accepted = ok;
    bool changed = false;
    int64_t revision = 0;
    std::string finalText = valueText;
    if (accepted) {
      auto entryIt = channels_.find(request.key);
      if (entryIt == channels_.end()) {
        accepted = false;
      } else {
        std::string prev;
        changed = applyChannelValue(entryIt->second, valueText, request.byPluginId,
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
    // 写方自己收结果: 插件的即时回执走 `musicxx.broadcast.writeResult` 事件
    // (与通知分开: 通知只表达"值变了", 回执表达"这次写的结果")
    publishOnHostThread(MUSICXX_PLUGIN_EVENT_BROADCAST_WRITE_RESULT, payload);
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
    auto entryIt = channels_.find(request.key);
    if (entryIt == channels_.end()) {
      ok = false;
    } else {
      ChannelEntry &entry = entryIt->second;
      if (entry.valueJson != valueText) {
        // 学到了新值: 更新缓存; handler 属主不主动推时这条就是"通知的来源",
        // 因此只有 handler 模式才广播 (declared 的值变化本来就由属主 `set` 推)
        const std::string prev = entry.valueJson;
        entry.valueJson = valueText;
        entry.valueMs = steadyNowMs();
        entry.everRefreshed = true;
        publishChannelSnapshot(entry);
        if (entry.mode == MUSICXX_PLUGIN_CHANNEL_MODE_HANDLER) {
          pushChannelChanged(entry.key, valueText, prev, entry.pluginId, false, 0);
        }
      } else {
        entry.valueMs = steadyNowMs();
        entry.everRefreshed = true;
        publishChannelSnapshot(entry);
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

void MusicxxHostManager::expireChannelRequest(int64_t requestId) {
  auto it = pendingChannelRequests_.find(requestId);
  if (it == pendingChannelRequests_.end()) {
    return;
  }
  const bool isWrite = it->second.kind == ChannelRequestWrite;
  const std::string key = it->second.key;
  const std::string reason =
      isWrite ? (it->second.owner.empty() ? "not_served" : "owner_timeout")
              : "read_timeout";
  XX_LOGW("[musicxx_ext] 变量请求超时: kind={} key={} id={}", 
          isWrite ? "write" : "read", key, requestId);
  // 超时**不动缓存**: `peek` 继续返回旧值并如实报告 stale
  completeChannelRequest(requestId, false, std::string{}, reason);
}

/* ==================== 主程序入口 ==================== */

int32_t MusicxxHostManager::declareChannels(const std::string &itemsJson,
                                        std::string &log) {
  bool parseOk = false;
  Json items = parseJsonSafe(itemsJson, &parseOk);
  if (!parseOk || !items.is_array()) {
    log = "broadcast_declare: items 必须是 JSON 数组";
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
    if (!naming::isOfficial(key) || !isValidChannelKeyText(key)) {
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

    auto existing = channels_.find(key);
    if (existing != channels_.end() && !existing->second.owner.empty()) {
      failures += " " + key + " 与插件变量重名;";
      continue;
    }

    ChannelEntry entry;
    entry.key = key;
    entry.caps = caps;
    entry.mode = MUSICXX_PLUGIN_CHANNEL_MODE_DECLARED; ///< 官方键的写一律转给应用
    entry.writeScope = MUSICXX_PLUGIN_CHANNEL_WRITE_ANY;
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
    const bool isNew = (existing == channels_.end());
    entry.seq = isNew ? ++channelSeq_ : existing->second.seq;
    if (!isNew) {
      entry.revision = existing->second.revision;
      entry.subscribers = existing->second.subscribers;
      entry.watchers = existing->second.watchers;
    }
    channels_[key] = std::move(entry);

    if (isNew) {
      for (const auto &[instanceName, keys] : channelSubscriptions_) {
        if (keys.count(key) != 0) {
          channels_[key].subscribers.insert(instanceName);
        }
      }
      for (const auto &[instanceName, keys] : channelWatchers_) {
        if (keys.count(key) != 0) {
          channels_[key].watchers.insert(instanceName);
        }
      }
      // 声明也是通知时机: 已经订阅了该键的实例补一条初始通知
      if (channels_[key].careCount() > 0) {
        pushChannelChanged(key, valueText, std::string{"null"}, std::string{}, false,
                       0);
      }
    }
    refreshChannelCare(channels_[key]);
    ++applied;
  }
  if (!failures.empty()) {
    XX_LOGW("[musicxx_ext] 变量声明有被拒绝的项:{}", failures);
    log = "部分声明被拒绝:" + failures;
  }
  XX_LOGI("[musicxx_ext] 应用声明变量 {} 条", applied);
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::applyAppChannelValue(const std::string &key,
                                             const std::string &valueJson,
                                             std::string &err) {
  auto it = channels_.find(key);
  if (it == channels_.end()) {
    err = "变量不存在 (应用还没声明): " + key;
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  if (!it->second.owner.empty()) {
    err = "这是插件变量, 主程序推值请用 broadcast_write: " + key;
    return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
  }
  std::string valueText;
  if (!normalizeValueText(valueJson, valueText, err)) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  std::string prev;
  const bool changed = applyChannelValue(it->second, valueText, std::string{}, prev);
  if (!changed) {
    return MUSICXX_EXTERN_PLUGIN_OK;
  }
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::updateChannel(const std::string &key,
                                      const std::string &valueJson,
                                      std::string &err) {
  return applyAppChannelValue(key, valueJson, err);
}

int32_t MusicxxHostManager::publishChannels(const std::string &itemsJson,
                                            std::string &err) {
  bool parseOk = false;
  Json items = parseJsonSafe(itemsJson, &parseOk);
  if (!parseOk || !items.is_array()) {
    err = "broadcast_update_batch: items 必须是 JSON 数组";
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
    if (applyAppChannelValue(key, item["value"].dump(), reason) ==
        MUSICXX_EXTERN_PLUGIN_OK) {
      ++applied;
    } else {
      XX_LOGW("[musicxx_ext] 变量批量推值被拒绝: {} ({})", key, reason);
    }
  }
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::channelWriteResult(int64_t requestId, int32_t accepted,
                                           const std::string &valueJson,
                                           const std::string &error) {
  auto it = pendingChannelRequests_.find(requestId);
  if (it == pendingChannelRequests_.end()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  if (it->second.kind != ChannelRequestWrite) {
    return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  }
  completeChannelRequest(requestId, accepted != 0, valueJson,
                     error.empty() ? std::string{"rejected"} : error);
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::channelReadResult(int64_t requestId, int32_t ok,
                                          const std::string &valueJson,
                                          const std::string &error) {
  auto it = pendingChannelRequests_.find(requestId);
  if (it == pendingChannelRequests_.end()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  if (it->second.kind != ChannelRequestRead) {
    return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  }
  completeChannelRequest(requestId, ok != 0, valueJson,
                     error.empty() ? std::string{"read_failed"} : error);
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::subscribeChannelsForApp(const std::string &keysJson,
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
    err = "broadcast_subscribe: keys 必须是数组或字符串";
    return MUSICXX_EXTERN_PLUGIN_ERR_JSON;
  }
  for (const std::string &key : list) {
    auto it = std::find(appChannelSubscriptions_.begin(),
                        appChannelSubscriptions_.end(), key);
    if (subscribe && it == appChannelSubscriptions_.end()) {
      appChannelSubscriptions_.push_back(key);
    } else if (!subscribe && it != appChannelSubscriptions_.end()) {
      appChannelSubscriptions_.erase(it);
    }
  }
  return MUSICXX_EXTERN_PLUGIN_OK;
}

bool MusicxxHostManager::appCaresAboutChannel(const std::string &key) const {
  for (const std::string &item : appChannelSubscriptions_) {
    if (channelKeyMatchesSubscription(key, item)) {
      return true;
    }
  }
  return false;
}

/* ==================== 订阅 / 保活 ==================== */

void MusicxxHostManager::markChannelSubscriber(const std::string &instanceName,
                                           const std::string &key, bool watch,
                                           bool add, int32_t &outCount) {
  if (instanceName.empty() || key.empty()) {
    return;
  }
  auto &table = watch ? channelWatchers_ : channelSubscriptions_;
  auto &other = watch ? channelSubscriptions_ : channelWatchers_;
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

  auto entryIt = channels_.find(key);
  if (entryIt == channels_.end()) {
    return; ///< 键还不存在: 订阅先记在实例上, 键出现时自动挂
  }
  ChannelEntry &entry = entryIt->second;
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
  refreshChannelCare(entry);
}

int32_t MusicxxHostManager::subscribeChannels(MusicxxHostInstance *inst,
                                          const std::string &keysJson,
                                          int32_t &outCount) {
  return watchChannels(inst, keysJson, false, outCount);
}

int32_t MusicxxHostManager::unsubscribeChannels(MusicxxHostInstance *inst,
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
    markChannelSubscriber(inst->name, fullKey, false, false, outCount);
  }
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::watchChannels(MusicxxHostInstance *inst,
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
    auto entryIt = channels_.find(fullKey);
    if (entryIt != channels_.end() &&
        (entryIt->second.caps & MUSICXX_PLUGIN_CHANNEL_CAP_NOTIFY) == 0) {
      XX_LOGW("[musicxx_ext] 插件 `{}` {} `{}` 被拒绝: 该变量没有声明 notify",
              pluginIdOf(inst->name), watch ? "watch" : "subscribe", fullKey);
      continue;
    }
    markChannelSubscriber(inst->name, fullKey, watch, true, outCount);
  }
  return MUSICXX_EXTERN_PLUGIN_OK;
}

void MusicxxHostManager::refreshChannelCare(const ChannelEntry &entry) {
  const size_t subscribers = entry.careCount();
  int64_t total = 0;
  for (const auto &[key, item] : channels_) {
    (void)key;
    total += static_cast<int64_t>(item.careCount());
  }
  channelCareTotal_ = total;
  Json payload;
  payload["key"] = entry.key;
  payload["subscribers"] = static_cast<int64_t>(subscribers);
  payload["totalSubscribers"] = total;
  payload["ts"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  // 主程序据此维护"哪些键有人看": 没人看的键值变化只更新本地缓存, 不推插件框架
  pushEvent("musicxx.broadcast.subscriptions", "", payload.dump());
  // 关心数也进同步读镜像 (JS 侧 `subscribers()` 要读)
  publishChannelSnapshot(entry);
  // handler 属主不主动推时: 有人关心就按 refreshAfterMs 定期去取一次真实值
  ensureChannelRefresh(entry.key);
}

void MusicxxHostManager::cancelChannelsOf(const std::string &instanceName) {
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
  collect(channelSubscriptions_);
  collect(channelWatchers_);
  for (const std::string &key : keys) {
    auto entryIt = channels_.find(key);
    if (entryIt == channels_.end()) {
      continue;
    }
    entryIt->second.subscribers.erase(instanceName);
    entryIt->second.watchers.erase(instanceName);
    refreshChannelCare(entryIt->second);
  }
  // 该实例发起的未回执请求也一起结算 (实例没了, 结果没人要)
  std::vector<int64_t> pending;
  for (const auto &[id, request] : pendingChannelRequests_) {
    if (request.byInstance == instanceName) {
      pending.push_back(id);
    }
  }
  for (const int64_t id : pending) {
    completeChannelRequest(id, false, std::string{}, "plugin_stopped");
  }
}

void MusicxxHostManager::detachChannels(const std::string &instanceName) {
  const std::string pluginId = pluginIdOf(instanceName);
  // 1) 该实例注册的变量随之消失, 通知订阅者 (removed)
  std::vector<std::string> owned;
  for (const auto &[key, entry] : channels_) {
    if (entry.owner == instanceName) {
      owned.push_back(key);
    }
  }
  for (const std::string &key : owned) {
    auto it = channels_.find(key);
    if (it == channels_.end()) {
      continue;
    }
    const bool cared = it->second.careCount() > 0;
    const std::string valueText = it->second.valueJson;
    const std::string ownerId = it->second.pluginId;
    if (it->second.notifyTimer) {
      it->second.notifyTimer->cancel();
    }
    if (cared) {
      pushChannelChanged(key, valueText, valueText, ownerId, true, 0);
    }
    channels_.erase(it);
    eraseChannelSnapshot(key);
  }
  // 2) 该实例的订阅与保活一起摘掉
  cancelChannelsOf(instanceName);
  if (!owned.empty()) {
    XX_LOGI("[musicxx_ext] 插件 `{}` 的 {} 个变量已随实例摘除", pluginId,
            owned.size());
  }
}

/* ==================== 值与通知 ==================== */

bool MusicxxHostManager::applyChannelValue(ChannelEntry &entry,
                                       const std::string &valueJson,
                                       const std::string &byPluginId,
                                       std::string &prevJson) {
  if (entry.valueJson == valueJson) {
    entry.valueMs = steadyNowMs();
    prevJson = entry.valueJson;
    publishChannelSnapshot(entry);
    return false; ///< 值没变: 不广播通知 (但算一次"刚确认过")
  }
  prevJson = entry.valueJson;
  entry.valueJson = valueJson;
  entry.valueMs = steadyNowMs();
  entry.everRefreshed = true;
  ++entry.revision;
  publishChannelSnapshot(entry);
  pushChannelChanged(entry.key, valueJson, prevJson, byPluginId, false, 0);
  return true;
}

void MusicxxHostManager::pushChannelChanged(const std::string &key,
                                        const std::string &valueJson,
                                        const std::string &prevJson,
                                        const std::string &byPluginId,
                                        bool removed, int32_t coalesced) {
  auto it = channels_.find(key);
  const int64_t revision = (it != channels_.end()) ? it->second.revision : 0;
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

  // 1) 发给插件: 事件总线 (订阅了 `musicxx.broadcast.changed` 的实例收到)
  if (it != channels_.end() && it->second.careCount() > 0) {
    publishOnHostThread(MUSICXX_PLUGIN_EVENT_BROADCAST_CHANGED, text);
  }
  // 2) 发给应用 (应用订阅了该键时才推; 没人看就不产生事件)
  if (appCaresAboutChannel(key)) {
    pushEvent("musicxx.broadcast.changed", (it != channels_.end()) ? it->second.pluginId
                                                         : std::string{},
              text);
  }
}

/* ==================== handler 属主的保活与结算 ==================== */

void MusicxxHostManager::ensureChannelRefresh(const std::string &key) {
  auto it = channels_.find(key);
  if (it == channels_.end()) {
    return;
  }
  ChannelEntry &entry = it->second;
  const bool need = entry.mode == MUSICXX_PLUGIN_CHANNEL_MODE_HANDLER &&
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
    self->onChannelRefreshTick(key);
  });
}

void MusicxxHostManager::onChannelRefreshTick(const std::string &key) {
  auto it = channels_.find(key);
  if (it == channels_.end()) {
    return;
  }
  it->second.refreshTimer.reset(); ///< 本次用完了, 下面重新排
  if (it->second.refreshAfterMs <= 0 || it->second.careCount() == 0) {
    return;
  }
  // 去属主那里取一次真实值: 没有等待者, 只为把同步读缓存刷新
  beginChannelRead(key, std::string{}, nullptr, nullptr, nullptr);
  ensureChannelRefresh(key);
}

void MusicxxHostManager::settleChannelRequestsOnCommit(
    const std::string &key, const std::string &valueJson) {
  std::vector<int64_t> ids;
  for (const auto &[id, request] : pendingChannelRequests_) {
    if (request.key == key) {
      ids.push_back(id);
    }
  }
  for (const int64_t id : ids) {
    auto it = pendingChannelRequests_.find(id);
    if (it == pendingChannelRequests_.end()) {
      continue;
    }
    // 读: 属主提交的值就是答案; 写: 提交即视为写完
    completeChannelRequest(id, true, valueJson, std::string{});
  }
}

int32_t MusicxxHostManager::respondChannel(MusicxxHostInstance *inst,
                                       int64_t requestId, bool ok,
                                       const std::string &valueJson,
                                       const std::string &error,
                                       std::string &outJson) {
  if (!inst || requestId <= 0) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  auto it = pendingChannelRequests_.find(requestId);
  if (it == pendingChannelRequests_.end()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  if (it->second.owner != inst->name) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 试图回答不属于自己的变量请求 {}", inst->name,
            requestId);
    return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
  }
  const bool isWrite = it->second.kind == ChannelRequestWrite;
  completeChannelRequest(requestId, ok, valueJson, error);
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

std::string MusicxxHostManager::channelSnapshotJsonText(const ChannelEntry &entry) const {
  Json item;
  item["key"] = entry.key;
  item["owner"] = entry.pluginId;
  item["caps"] = capsToJson(entry.caps);
  item["mode"] = channelModeName(entry.mode);
  item["write"] = channelWriteScopeName(entry.writeScope);
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

void MusicxxHostManager::publishChannelSnapshot(const ChannelEntry &entry) {
  const std::string channel = channelSnapshotId(entry.key);
  ensureBroadcastChannel(channel);
  std::string error;
  if (publishBroadcast(channel, channelSnapshotJsonText(entry), std::string{}, &error) !=
      PLUGINXX_RESULT_OK) {
    XX_LOGW("[musicxx_ext] 变量镜像 `{}` 写入广播失败: {}", entry.key, error);
  }
}

void MusicxxHostManager::eraseChannelSnapshot(const std::string &key) {
  broadcasts()->removeChannel(channelSnapshotId(key));
}

std::string MusicxxHostManager::channelToJsonText(const ChannelEntry &entry) const {
  const bool fresh = entry.everRefreshed && entry.valueMs > 0;
  const int64_t ageMs = fresh ? steadyNowMs() - entry.valueMs : 0;
  Json item = parseJsonSafe(channelSnapshotJsonText(entry));
  item["ageMs"] = ageMs;
  item["stale"] =
      !fresh || (entry.subscribers.empty() && entry.watchers.empty()) ||
      ageMs > kChannelReadTimeoutMs;
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
  item["stale"] = !hasValue || ageMs > kChannelReadTimeoutMs || !cared;
  return item.dump();
}

std::string MusicxxHostManager::channelsSnapshotText() const {
  std::vector<const ChannelEntry *> items;
  items.reserve(channels_.size());
  for (const auto &[key, entry] : channels_) {
    (void)key;
    items.push_back(&entry);
  }
  std::sort(items.begin(), items.end(), [](const ChannelEntry *a, const ChannelEntry *b) {
    return a->seq < b->seq;
  });
  Json array = Json::array();
  for (const ChannelEntry *entry : items) {
    array.push_back(parseJsonSafe(channelToJsonText(*entry)));
  }
  return array.dump();
}

std::string MusicxxHostManager::channelsDebugJson() const {
  int32_t getCaps = 0;
  int32_t setCaps = 0;
  int32_t notifyCaps = 0;
  int64_t subscribers = 0;
  int32_t appChannels = 0;
  for (const auto &[key, entry] : channels_) {
    (void)key;
    if ((entry.caps & MUSICXX_PLUGIN_CHANNEL_CAP_GET) != 0) {
      ++getCaps;
    }
    if ((entry.caps & MUSICXX_PLUGIN_CHANNEL_CAP_SET) != 0) {
      ++setCaps;
    }
    if ((entry.caps & MUSICXX_PLUGIN_CHANNEL_CAP_NOTIFY) != 0) {
      ++notifyCaps;
    }
    if (entry.owner.empty()) {
      ++appChannels;
    }
    subscribers += static_cast<int64_t>(entry.careCount());
  }
  int32_t pendingRead = 0;
  int32_t pendingWrite = 0;
  for (const auto &[id, request] : pendingChannelRequests_) {
    (void)id;
    if (request.kind == ChannelRequestRead) {
      ++pendingRead;
    } else {
      ++pendingWrite;
    }
  }
  Json info;
  info["count"] = static_cast<int64_t>(channels_.size());
  info["appChannels"] = appChannels;
  info["pluginChannels"] = static_cast<int64_t>(channels_.size()) - appChannels;
  info["caps"] = {{"get", getCaps}, {"set", setCaps}, {"notify", notifyCaps}};
  info["subscribers"] = subscribers;
  info["pendingRead"] = pendingRead;
  info["pendingWrite"] = pendingWrite;
  info["appSubscriptions"] =
      static_cast<int64_t>(appChannelSubscriptions_.size());
  return info.dump();
}

int32_t MusicxxHostManager::listChannels(std::string_view prefix,
                                     std::string &outJson) {
  const std::string head{prefix};
  static constexpr std::string_view kChannelChannelPrefix{"musicxx.broadcast."};
  // 只读镜像 (任何线程可用; 顺序按 key, 便于比对)
  std::vector<std::pair<std::string, std::string>> items;
  for (const auto &id : broadcasts()->ids()) {
    if (id.rfind(kChannelChannelPrefix, 0) != 0) {
      continue;
    }
    std::string key = id.substr(kChannelChannelPrefix.size());
    if (!head.empty() && key.compare(0, head.size(), head) != 0) {
      continue;
    }
    std::string snapshot;
    if (broadcasts()->read(id, &snapshot)) {
      items.emplace_back(std::move(key), std::move(snapshot));
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

int32_t MusicxxHostManager::channelInfo(std::string_view key,
                                    std::string &outJson) {
  std::string snapshot;
  if (!broadcasts()->read(channelSnapshotId(key), &snapshot)) {
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  Json item = parseJsonSafe(snapshot);
  const bool cared = item.contains("subscribers") && item["subscribers"].is_number() &&
                     item["subscribers"].get<int64_t>() > 0;
  outJson = withFreshness(snapshot, cared);
  return MUSICXX_EXTERN_PLUGIN_OK;
}

} // namespace extern_plugin
} // namespace musicxx
