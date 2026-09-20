/// musicxx 外部插件原生宿主: 声明式 UI 扩展
///
/// 插件不写 Flutter 代码, 只做**声明**: 注册一个 UI 项 (入口项 / 菜单项 /
/// 设置页 / 附加信息块) 并给出类型相关的 JSON 内容, 由 Dart 侧渲染 (声明式描述
/// + 宿主渲染)。
///
/// 设计要点:
/// - **注册表归宿主线程独占** (无锁不变式): 注册/更新/摘除/快照都在宿主线程;
///   Dart 线程的 `ui_snapshot` 经 C ABI 侧的 `onHostThread` 投递后读取;
/// - **命名空间校验**: 项 id 必须是本插件命名空间 (`plugin.<pluginId>.<名>`),
/// 短名由宿主补前缀;
///   动作里的 `route` 只允许指向本插件页面, `action` 只允许官方 `musicxx.*`
///   动作;
/// - **结构校验在注册时做**: data 必须是 JSON 对象且按类型含必需字段 (title
/// 等),
///   早报错好过让 Dart 侧渲染时才发现;
/// - **生命周期**: 项随插件实例 stop/卸载自动摘除 (宿主在
/// `clearPluginRegistrations` 里兜底),
///   每次变化推送 `musicxx.ui.changed` 事件 (payload 带该插件的全部项, Dart
///   整批替换);
/// - **只存声明**: 宿主不解释 data 的业务含义 (除了动作描述的结构),
/// 渲染细节留给 Dart 侧。
#include "host_json.h"
#include "host_naming.h"
#include "musicxx_host.h"

#include "musicxx/plugin/api/plugin_api.h"
#include "pluginxx/host/abi_util.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <string>
#include <vector>

namespace musicxx {
namespace extern_plugin {

using utilxx_base::Json;

namespace {

/// 单条 UI 项 data 的体积上限 (状态镜像限制同口径)
constexpr uint64_t kMaxUiDataBytes = 64 * 1024;

/// 官方 UI 项类型 (与 plugin_api.h 的 MUSICXX_PLUGIN_UI_TYPE_* 一一对应)
const char *const kKnownUiTypes[] = {
    MUSICXX_PLUGIN_UI_TYPE_HOME_ENTRY,
    MUSICXX_PLUGIN_UI_TYPE_SONG_ACTION,
    MUSICXX_PLUGIN_UI_TYPE_PLAYLIST_ACTION,
    MUSICXX_PLUGIN_UI_TYPE_SETTINGS_PAGE,
    MUSICXX_PLUGIN_UI_TYPE_OVERLAY_WIDGET,
};

bool isKnownUiType(std::string_view type) {
  for (const char *item : kKnownUiTypes) {
    if (type == item) {
      return true;
    }
  }
  return false;
}

/// 需要 `title` 字段的项类型 (入口项与菜单项都要给用户看到标题)
bool requiresTitle(std::string_view type) {
  return type == MUSICXX_PLUGIN_UI_TYPE_HOME_ENTRY ||
         type == MUSICXX_PLUGIN_UI_TYPE_SONG_ACTION ||
         type == MUSICXX_PLUGIN_UI_TYPE_PLAYLIST_ACTION ||
         type == MUSICXX_PLUGIN_UI_TYPE_SETTINGS_PAGE;
}

/// 校验一个动作描述 (data.action)
///
/// 允许的形态:
/// - `{"kind":"route","route":"ext://<本插件id>/<viewId>"}`
/// - `{"kind":"capability","name":"<短名>","args":{...}}`
/// - `{"kind":"action","name":"musicxx.<域>.<动作>","args":{...}}`
/// - `{"kind":"none"}`
bool validateAction(const Json &action, std::string_view pluginId,
                    std::string &err) {
  if (action.is_null()) {
    return true; ///< 没有动作 = 纯展示项
  }
  if (!action.is_object()) {
    err = "action 必须是对象";
    return false;
  }
  const std::string kind =
      (action.contains("kind") && action["kind"].is_string())
          ? action["kind"].get<std::string>()
          : "none";
  if (kind == MUSICXX_PLUGIN_UI_ACTION_NONE) {
    return true;
  }
  if (kind == MUSICXX_PLUGIN_UI_ACTION_ROUTE) {
    if (!action.contains("route") || !action["route"].is_string()) {
      err = "route 动作缺少 route 字段";
      return false;
    }
    const std::string route = action["route"].get<std::string>();
    const std::string prefix = "ext://" + std::string{pluginId} + "/";
    if (route.rfind(prefix, 0) != 0) {
      err = "route 必须指向本插件页面 (" + prefix + "<viewId>)";
      return false;
    }
    return true;
  }
  if (kind == MUSICXX_PLUGIN_UI_ACTION_CAPABILITY) {
    if (!action.contains("name") || !action["name"].is_string() ||
        action["name"].get<std::string>().empty()) {
      err = "capability 动作缺少 name 字段";
      return false;
    }
    return true;
  }
  if (kind == MUSICXX_PLUGIN_UI_ACTION_HOST) {
    if (!action.contains("name") || !action["name"].is_string()) {
      err = "action 动作缺少 name 字段";
      return false;
    }
    // 官方动作只能调 `musicxx.*`; 插件自定义动作 (`plugin.<自己id>.*`) 不作为
    // UI 动作使用
    if (!naming::isOfficial(action["name"].get<std::string>())) {
      err = "action 动作必须使用官方名称 (musicxx.<域>.<动作>)";
      return false;
    }
    return true;
  }
  err = "未知的 action.kind: " + kind;
  return false;
}

/// 校验并规范化一条 UI 项的内容 (注册与更新共用)
bool validateUiData(std::string_view type, const std::string &dataJson,
                    std::string_view pluginId, std::string &err) {
  if (dataJson.size() > kMaxUiDataBytes) {
    err = "data 超过 64 KiB 上限";
    return false;
  }
  bool parseOk = false;
  Json data = parseJsonSafe(dataJson, &parseOk);
  if (!parseOk || !data.is_object()) {
    err = "data 必须是 JSON 对象";
    return false;
  }
  if (requiresTitle(type)) {
    if (!data.contains("title") || !data["title"].is_string() ||
        data["title"].get<std::string>().empty()) {
      err = "缺少 title 字段 (非空字符串)";
      return false;
    }
  }
  if (type == MUSICXX_PLUGIN_UI_TYPE_OVERLAY_WIDGET) {
    if (!data.contains("position") || !data["position"].is_string()) {
      err = "附加信息块缺少 position 字段";
      return false;
    }
    if (!data.contains("content") || !data["content"].is_object()) {
      err = "附加信息块缺少 content 对象";
      return false;
    }
  }
  if (data.contains("action")) {
    return validateAction(data["action"], pluginId, err);
  }
  return true;
}

/// 动作描述里引用的能力名: 允许短名或本插件全名 (宿主不在这里校验能力是否存在,
/// 由 Dart 侧调用时按能力表判定)
std::string normalizeItemId(std::string_view raw, std::string_view pluginId,
                            bool &ok) {
  const std::string text{raw};
  ok = false;
  if (text.empty()) {
    return {};
  }
  if (text.rfind("plugin.", 0) == 0) {
    // 全名: 必须属于本插件
    if (!naming::isOwnPlugin(text, pluginId)) {
      return text;
    }
    ok = true;
    return text;
  }
  if (text.find('.') != std::string::npos) {
    // 带点但不是 `plugin.` 前缀: 当作非法 (避免把 `musicxx.*` 当成自定义项)
    return text;
  }
  ok = true;
  return "plugin." + std::string{pluginId} + "." + text;
}

} // namespace

/* ==================== 注册 / 更新 / 注销 ==================== */

int32_t
MusicxxHostManager::registerUiEntry(MusicxxHostInstance *inst,
                                    const MusicxxPluginUIEntrySpec &spec) {
  if (!inst) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (spec.struct_size != 0 &&
      spec.struct_size < sizeof(MusicxxPluginUIEntrySpec)) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (spec.flags != 0) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册 UI 项: flags 必须为 0", inst->name);
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (inst->unloadRequested || !inst->enabled) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册 UI 项被拒绝: 实例正在关闭或已禁用",
            inst->name);
    return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  }
  if (!spec.item_id.data || spec.item_id.size == 0 || !spec.type.data ||
      spec.type.size == 0) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }

  const std::string pluginId = pluginIdOf(inst->name);
  const std::string type{spec.type.data, static_cast<size_t>(spec.type.size)};
  if (!isKnownUiType(type)) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册未知 UI 项类型 `{}` 被拒绝",
            inst->name, type);
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }

  bool idOk = false;
  const std::string itemId =
      normalizeItemId(std::string_view{spec.item_id.data,
                                       static_cast<size_t>(spec.item_id.size)},
                      pluginId, idOk);
  if (!idOk) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册 UI 项 `{}` 被拒绝: 命名空间非法",
            inst->name, itemId);
    return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
  }

  const std::string dataJson =
      spec.data_json.data
          ? std::string{spec.data_json.data,
                        static_cast<size_t>(spec.data_json.size)}
          : std::string{"{}"};
  std::string err;
  if (!validateUiData(type, dataJson, pluginId, err)) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 注册 UI 项 `{}` 被拒绝: {}", inst->name,
            itemId, err);
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }

  const auto existing = uiEntries_.find(itemId);
  if (existing == uiEntries_.end()) {
    size_t owned = 0;
    for (const auto &[key, entry] : uiEntries_) {
      (void)key;
      if (entry.pluginId == pluginId) {
        ++owned;
      }
    }
    if (owned >= kMaxUiEntriesPerPlugin) {
      XX_LOGW(
          "[musicxx_ext] 插件 `{}` 的 UI 项数量已达上限 ({}), 注册 `{}` 被拒绝",
          inst->name, kMaxUiEntriesPerPlugin, itemId);
      return MUSICXX_EXTERN_PLUGIN_ERR_QUEUE_FULL;
    }
  } else if (existing->second.pluginId != pluginId) {
    // 理论上 normalizeItemId 已经挡掉; 这里再兜一层 (防止 id
    // 规范化逻辑变动引入漏洞)
    XX_LOGW("[musicxx_ext] 插件 `{}` 试图覆盖他人 UI 项 `{}`", inst->name,
            itemId);
    return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
  }

  UiEntry entry;
  entry.plugin = inst->name;
  entry.pluginId = pluginId;
  entry.id = itemId;
  entry.type = type;
  entry.dataJson = dataJson;
  entry.order = spec.order;
  entry.seq = ++uiSeq_;
  if (existing != uiEntries_.end()) {
    entry.seq = existing->second.seq; ///< 覆盖保留原注册顺序
  }
  uiEntries_[itemId] = std::move(entry);

  Json changed;
  changed["action"] = "register";
  changed["id"] = pluginId;
  changed["items"] = parseJsonSafe(uiItemsJson(pluginId));
  pushEvent("musicxx.ui.changed", pluginId, changed.dump());
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::unregisterUiEntry(MusicxxHostInstance *inst,
                                              std::string_view itemId) {
  if (itemId.empty()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  const std::string pluginId = inst ? pluginIdOf(inst->name) : std::string{};
  bool idOk = false;
  // 短名与全名都接受 (脚本/插件作者写短名更省事)
  const std::string fullId = normalizeItemId(itemId, pluginId, idOk);
  if (!idOk) {
    return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
  }
  auto it = uiEntries_.find(fullId);
  if (it == uiEntries_.end()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  if (inst && it->second.pluginId != pluginId) {
    XX_LOGW("[musicxx_ext] 插件 `{}` 试图注销他人 UI 项 `{}`", inst->name,
            itemId);
    return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
  }
  const std::string ownerId = it->second.pluginId;
  uiEntries_.erase(it);

  Json changed;
  changed["action"] = "unregister";
  changed["id"] = ownerId;
  changed["items"] = parseJsonSafe(uiItemsJson(ownerId));
  pushEvent("musicxx.ui.changed", ownerId, changed.dump());
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::updateUiEntry(MusicxxHostInstance *inst,
                                          std::string_view itemId,
                                          std::string_view dataJson) {
  if (itemId.empty()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  const std::string pluginId = inst ? pluginIdOf(inst->name) : std::string{};
  bool idOk = false;
  const std::string fullId = normalizeItemId(itemId, pluginId, idOk);
  if (!idOk) {
    return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
  }
  auto it = uiEntries_.find(fullId);
  if (it == uiEntries_.end()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
  }
  if (inst && it->second.pluginId != pluginId) {
    return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
  }
  const std::string data{dataJson};
  std::string err;
  if (!validateUiData(it->second.type, data, it->second.pluginId, err)) {
    XX_LOGW("[musicxx_ext] 更新 UI 项 `{}` 被拒绝: {}", itemId, err);
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  it->second.dataJson = data;

  Json changed;
  changed["action"] = "update";
  changed["id"] = it->second.pluginId;
  changed["items"] = parseJsonSafe(uiItemsJson(it->second.pluginId));
  pushEvent("musicxx.ui.changed", it->second.pluginId, changed.dump());
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::listUiEntries(MusicxxHostInstance *inst,
                                          std::string &outJson) {
  const std::string pluginId = inst ? pluginIdOf(inst->name) : std::string{};
  outJson = uiItemsJson(pluginId);
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::notifyUi(MusicxxHostInstance *inst,
                                     std::string_view messageJson) {
  if (!inst) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  const std::string message{messageJson};
  bool parseOk = false;
  Json data = parseJsonSafe(message, &parseOk);
  if (!parseOk || !data.is_object()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_JSON;
  }
  // 通知是 fire-and-forget: 走动作请求通道, Dart 侧执行 `musicxx.ui.notify`,
  // 结果不回传 (回复会落在"未登记的在途请求"上, 由宿主按调试日志忽略)
  const std::string pluginId = pluginIdOf(inst->name);
  int64_t effectiveTimeout = 0;
  return pushActionRequestEvent(pluginId, allocateRequestId(),
                                "musicxx.ui.notify", message, 5000,
                                &effectiveTimeout);
}

/* ==================== 序列化与摘除 ==================== */

std::string MusicxxHostManager::uiItemsJson(const std::string &pluginId) const {
  std::vector<const UiEntry *> items;
  items.reserve(uiEntries_.size());
  for (const auto &[key, entry] : uiEntries_) {
    (void)key;
    if (pluginId.empty() || entry.pluginId == pluginId) {
      items.push_back(&entry);
    }
  }
  std::sort(items.begin(), items.end(), [](const UiEntry *a, const UiEntry *b) {
    if (a->type != b->type) {
      return a->type < b->type;
    }
    if (a->order != b->order) {
      return a->order < b->order;
    }
    return a->seq < b->seq;
  });

  Json array = Json::array();
  for (const UiEntry *entry : items) {
    Json item;
    item["id"] = entry->id;
    item["plugin"] = entry->pluginId;
    item["type"] = entry->type;
    item["order"] = entry->order;
    item["data"] = parseJsonSafe(entry->dataJson);
    array.push_back(std::move(item));
  }
  return array.dump();
}

void MusicxxHostManager::detachUiEntries(const std::string &instanceName) {
  const std::string pluginId = pluginIdOf(instanceName);
  bool removed = false;
  for (auto it = uiEntries_.begin(); it != uiEntries_.end();) {
    if (it->second.plugin == instanceName) {
      it = uiEntries_.erase(it);
      removed = true;
    } else {
      ++it;
    }
  }
  if (!removed) {
    return;
  }
  Json changed;
  changed["action"] = "detach";
  changed["id"] = pluginId;
  changed["items"] = parseJsonSafe(uiItemsJson(pluginId));
  pushEvent("musicxx.ui.changed", pluginId, changed.dump());
}

/* ==================== C ABI: musicxx.ui 表 ==================== */

namespace {

int32_t PLUGINXX_CALL xx_ui_register_entry(
    const PluginxxHost *host, const MusicxxPluginUIEntrySpec *spec) {
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
              return mgr->registerUiEntry(inst, specCopy);
            });
      });
}

int32_t PLUGINXX_CALL xx_ui_unregister_entry(const PluginxxHost *host,
                                             const PluginxxStringView *itemId) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        auto call =
            pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
                host);
        if (!call.ok() || !itemId || !itemId->data) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto mgr = call.manager();
        auto inst = call.instance();
        const std::string_view id{itemId->data,
                                  static_cast<size_t>(itemId->size)};
        return pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, inst, id]() -> int32_t {
              return mgr->unregisterUiEntry(inst, id);
            });
      });
}

int32_t PLUGINXX_CALL xx_ui_update_entry(const PluginxxHost *host,
                                         const PluginxxStringView *itemId,
                                         const PluginxxStringView *dataJson) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        auto call =
            pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
                host);
        if (!call.ok() || !itemId || !itemId->data || !dataJson ||
            !dataJson->data) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto mgr = call.manager();
        auto inst = call.instance();
        const std::string_view id{itemId->data,
                                  static_cast<size_t>(itemId->size)};
        const std::string_view data{dataJson->data,
                                    static_cast<size_t>(dataJson->size)};
        return pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, inst, id, data]() -> int32_t {
              return mgr->updateUiEntry(inst, id, data);
            });
      });
}

int32_t PLUGINXX_CALL xx_ui_list_entries(const PluginxxHost *host,
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
        std::string json;
        const int32_t rc = pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, inst, &json]() -> int32_t {
              return mgr->listUiEntries(inst, json);
            });
        if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
          return rc;
        }
        pluginxx::hostMemorySetString(outJson, json);
        return MUSICXX_EXTERN_PLUGIN_OK;
      });
}

int32_t PLUGINXX_CALL xx_ui_notify(const PluginxxHost *host,
                                   const PluginxxStringView *messageJson) {
  return pluginxx::guardVtableCall(
      MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL, [&]() -> int32_t {
        auto call =
            pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
                host);
        if (!call.ok() || !messageJson || !messageJson->data) {
          return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
        }
        auto mgr = call.manager();
        auto inst = call.instance();
        const std::string_view message{messageJson->data,
                                       static_cast<size_t>(messageJson->size)};
        return pluginxx::ioCallSyncKeep<int32_t>(
            call, mgr, [mgr, inst, message]() -> int32_t {
              return mgr->notifyUi(inst, message);
            });
      });
}

const MusicxxPluginUIIface g_ifaceUi = {
    /* version */ MUSICXX_PLUGIN_IFACE_UI_VERSION,
    /* struct_size */ sizeof(MusicxxPluginUIIface),
    /* register_entry */ xx_ui_register_entry,
    /* unregister_entry */ xx_ui_unregister_entry,
    /* update_entry */ xx_ui_update_entry,
    /* list_entries */ xx_ui_list_entries,
    /* notify */ xx_ui_notify,
};

} // namespace

const void *uiIfaceForQuery() { return &g_ifaceUi; }

} // namespace extern_plugin
} // namespace musicxx
