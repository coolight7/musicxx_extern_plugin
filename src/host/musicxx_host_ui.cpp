/// musicxx 外部插件原生插件框架: 声明式 UI 扩展
///
/// 插件不写 Flutter 代码, 只做**声明**:
/// - **声明项**(entry): 入口 / 菜单项 / 音效 / 可选样式的元信息 (只有数据, 没有界面树);
/// - **内容**(content): 挂在某个槽位上的界面 JSON, 由生成函数在需要时生成;
/// - **槽位**(slot): 插件也可以声明自己的槽位 (`plugin.<自己>.ui.<名字>`), 让别人往里填内容。
///
/// 设计要点:
/// - **注册表归框架线程独占**: 注册/摘除/快照都在框架线程; Dart 线程的读取经 C ABI 投递后执行;
/// - **命名空间校验**: 项 / 内容 id 必须是本插件命名空间, 短名由框架补前缀;
/// - **生成函数同步执行且必须快**: 主程序只在"槽位被激活 / 要求刷新 / 依赖变化"时调用;
/// - **生命周期**: 项 / 内容 / 槽位随插件实例 stop/卸载自动摘除; 变化推事件
///   (`musicxx.ui.changed` / `musicxx.ui.contents` / `musicxx.ui.content` / `musicxx.ui.slots`)。
#include "host_json.h"
#include "host_naming.h"
#include "js/js_engine.h"
#include "musicxx_host.h"

#include "musicxx/plugin/api/plugin_api.h"
#include "musicxx/plugin/api/ui_variables.g.h"
#include "pluginxx/host/abi_util.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace musicxx {
namespace extern_plugin {

using utilxx_base::Json;

/// 校验一个动作描述 (声明项的 data.action; 与界面描述层的写法一致)
bool validateAction(const Json &action, std::string_view pluginId,
                    std::string &err) {
  if (action.is_null()) {
    return true; ///< 没有动作 = 纯展示项
  }
  if (action.is_string()) {
    if (action.get<std::string>().empty()) {
      err = "the short form must give a non-empty capability name";
      return false;
    }
    return true;
  }
  if (!action.is_object()) {
    err = "action must be an object or a capability short name";
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
      err = "the route action has no route field";
      return false;
    }
    const std::string route = action["route"].get<std::string>();
    constexpr const char *kPrefix = "ext://";
    constexpr size_t kPrefixLen = 6; ///< sizeof("ext://") - 1
    const std::string hint =
        "ext://<pluginId>/<contentId> (any plugin; for this plugin ext://" +
        std::string{pluginId} + "/<contentId>)";
    if (route.rfind(kPrefix, 0) != 0) {
      err = "route must start with ext:// (" + hint + ")";
      return false;
    }
    const std::string rest = route.substr(kPrefixLen);
    const size_t slash = rest.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= rest.size()) {
      err = "route address is incomplete (" + hint + ")";
      return false;
    }
    return true;
  }
  if (kind == MUSICXX_PLUGIN_UI_ACTION_CALL) {
    if (!action.contains("name") || !action["name"].is_string() ||
        action["name"].get<std::string>().empty()) {
      err = "the call action has no name field";
      return false;
    }
    const std::string name = action["name"].get<std::string>();
    if (name.find('.') != std::string::npos &&
        name.rfind("plugin.", 0) != 0 && !naming::isOfficial(name)) {
      err = "a call name is either a short name, plugin.<pluginId>.<name> or musicxx.<domain>.<name>";
      return false;
    }
    return true;
  }
  err = "unknown action.kind: " + kind + " (must be call / route / none)";
  return false;
}

namespace {

/// 槽位与变量目录：由生成表建一次（声明源 tools/ui_slots.def.json）
///
/// 共享目录（`pluginxx/host/ui_slots.h`）只认"前缀 + 全名 + kind 文本"，不认识 musicxx
/// 的槽位；表本身由这里注入，别的项目换成自己的表即可。
const pluginxx::host::UiSlotCatalog &catalogOf() {
  static const pluginxx::host::UiSlotCatalog catalog = [] {
    std::vector<pluginxx::host::UiSlotDecl> slots;
    const size_t slotCount = sizeof(kKnownUiSlots) / sizeof(kKnownUiSlots[0]);
    slots.reserve(slotCount);
    for (size_t i = 0; i < slotCount; ++i) {
      slots.push_back(pluginxx::host::UiSlotDecl{kKnownUiSlots[i].id,
                                                 kKnownUiSlots[i].kind});
    }
    std::vector<std::string> variables;
    const size_t variableCount =
        sizeof(kKnownUiVariables) / sizeof(kKnownUiVariables[0]);
    variables.reserve(variableCount);
    for (size_t i = 0; i < variableCount; ++i) {
      variables.emplace_back(kKnownUiVariables[i].id);
    }
    return pluginxx::host::UiSlotCatalog{"musicxx.ui.", std::move(slots),
                                         std::move(variables)};
  }();
  return catalog;
}

/// 已知槽位 / 声明项（目录里的一项）
const pluginxx::host::UiSlotDecl *knownSlotById(std::string_view id) {
  return catalogOf().find(id);
}

/// 简称 → 全名（`musicxx.ui.` 前缀）；表里没有的 id 原样返回
std::string slotFullName(std::string_view raw) {
  return catalogOf().fullName(raw);
}

bool isKnownVariable(std::string_view id) {
  return catalogOf().isKnownVariable(id);
}

/// 变量 id 形状校验: 生成表里的 / `plugin.<插件id>.<名字>`
bool validDependency(std::string_view id, std::string &err) {
  if (isKnownVariable(id)) {
    return true;
  }
  if (id.rfind("plugin.", 0) == 0) {
    const size_t first = id.find('.', 7);
    if (first != std::string_view::npos && first + 1 < id.size()) {
      return true;
    }
    err = "a plugin.* variable must be written plugin.<pluginId>.<name>";
    return false;
  }
  err = "unknown variable `" + std::string{id} + "` (not in the generated table; plugin.* is accepted too)";
  return false;
}

/// 是否要求 `title` (要出现在用户能看到的列表里)
bool requiresTitle(std::string_view kind) {
  return kind == "item" || kind == "selectable" || kind == "pageBackground";
}

/// 校验并规范化一条声明项的内容
///
/// - 入口/菜单/样式候选要有 `title`;
/// - 音效的 `pitch` / `cpuCost` 越界时**夹到边界** (存进去的就是夹过的值);
/// - 页面背景的 `routes` 必须是非空字符串数组。
bool validateEntryData(std::string_view kind, std::string &dataJson,
                       std::string_view pluginId, std::string &err) {
  bool parseOk = false;
  Json data = parseJsonSafe(dataJson, &parseOk);
  if (!parseOk || !data.is_object()) {
    err = "data must be a JSON object";
    return false;
  }
  if (requiresTitle(kind)) {
    if (!data.contains("title") || !data["title"].is_string() ||
        data["title"].get<std::string>().empty()) {
      err = "the title field is missing (non-empty string)";
      return false;
    }
  }
  if (kind == "pageBackground" && data.contains("routes")) {
    const Json &routes = data["routes"];
    if (!routes.is_array()) {
      err = "page background routes must be an array of page route ids (missing = every ordinary page)";
      return false;
    }
    for (const Json &item : routes) {
      if (!item.is_string() || item.get<std::string>().empty()) {
        err = "page background routes must be non-empty page route ids";
        return false;
      }
    }
  }
  if (data.contains("filter") && !data["filter"].is_string()) {
    err = "an audio effect filter must be a string (a complete af fragment)";
    return false;
  }
  bool changed = false;
  if (data.contains("pitch") && data["pitch"].is_number()) {
    const double pitch = data["pitch"].get<double>();
    const double clamped = std::clamp(pitch, 0.25, 2.0);
    if (clamped != pitch) {
      data["pitch"] = clamped;
      changed = true;
    }
  }
  if (data.contains("cpuCost") && data["cpuCost"].is_number()) {
    const double raw = data["cpuCost"].get<double>();
    const double clamped = std::clamp(std::round(raw), 1.0, 5.0);
    if (clamped != raw) {
      data["cpuCost"] = clamped;
      changed = true;
    }
  }
  if (changed) {
    dataJson = data.dump();
  }
  if (data.contains("action")) {
    return validateAction(data["action"], pluginId, err);
  }
  return true;
}

std::string normalizeItemId(std::string_view raw, std::string_view pluginId,
                            bool &ok) {
  const std::string text{raw};
  ok = false;
  if (text.empty()) {
    return {};
  }
  if (text.rfind("plugin.", 0) == 0) {
    if (!naming::isOwnPlugin(text, pluginId)) {
      return text;
    }
    ok = true;
    return text;
  }
  if (text.find('.') != std::string::npos) {
    return text;
  }
  ok = true;
  return "plugin." + std::string{pluginId} + "." + text;
}

} // namespace

/* ==================== 声明项 ==================== */

int32_t MusicxxHostManager::registerUiEntry(MusicxxHostInstance *inst,
                                            const MusicxxPluginUIEntrySpec &spec) {
  if (!inst) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  if (spec.struct_size != 0 &&
      spec.struct_size < sizeof(MusicxxPluginUIEntrySpec)) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  if (spec.flags != 0) {
    XX_LOGW("[musicxx_ext] entry registration by plugin `{}`: flags must be 0", inst->name);
    return PLUGINXX_RESULT_ERR_ARG;
  }
  if (inst->unloadRequested || !inst->enabled) {
    return PLUGINXX_RESULT_ERR_STATE;
  }
  if (!spec.item_id.data || spec.item_id.size == 0 || !spec.type.data ||
      spec.type.size == 0) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  const std::string pluginId = pluginIdOf(inst->name);
  const std::string type{spec.type.data, static_cast<size_t>(spec.type.size)};
  const std::string typeId = slotFullName(type);
  const pluginxx::host::UiSlotDecl *known = knownSlotById(typeId);
  if (nullptr == known) {
    XX_LOGW("[musicxx_ext] entry registration by plugin `{}` rejected: unknown slot/entry `{}`", inst->name,
            type);
    return PLUGINXX_RESULT_ERR_NOT_FOUND;
  }
  const std::string_view kindOf{known->kind ? known->kind : ""};
  if (kindOf != "item" && kindOf != "selectable" && kindOf != "pageBackground") {
    XX_LOGW("[musicxx_ext] plugin `{}` registered slot `{}` through register_entry"
            " (that slot is for content)", inst->name, typeId);
    return PLUGINXX_RESULT_ERR_ARG;
  }

  bool idOk = false;
  const std::string itemId = normalizeItemId(
      std::string_view{spec.item_id.data, static_cast<size_t>(spec.item_id.size)},
      pluginId, idOk);
  if (!idOk) {
    XX_LOGW("[musicxx_ext] entry registration by plugin `{}` of `{}` rejected: invalid namespace",
            inst->name, itemId);
    return PLUGINXX_RESULT_ERR_PERMISSION;
  }

  std::string dataJson =
      spec.data_json.data
          ? std::string{spec.data_json.data,
                        static_cast<size_t>(spec.data_json.size)}
          : std::string{"{}"};
  std::string err;
  if (!validateEntryData(kindOf, dataJson, pluginId, err)) {
    XX_LOGW("[musicxx_ext] entry registration by plugin `{}` of `{}` rejected: {}", inst->name,
            itemId, err);
    return PLUGINXX_RESULT_ERR_ARG;
  }

  const auto existing = uiRegistry_.entries().find(itemId);
  if (existing != uiRegistry_.entries().end() && existing->second.pluginId != pluginId) {
    return PLUGINXX_RESULT_ERR_PERMISSION;
  }

  UiEntry entry;
  entry.plugin = inst->name;
  entry.pluginId = pluginId;
  entry.id = itemId;
  entry.type = typeId;
  entry.dataJson = dataJson;
  entry.order = spec.order;
  entry.seq = uiRegistry_.nextEntrySeq();
  if (existing != uiRegistry_.entries().end()) {
    entry.seq = existing->second.seq;
    entry.rev = existing->second.rev;
  }
  uiRegistry_.entries()[itemId] = std::move(entry);

  Json changed;
  changed["action"] = "register";
  changed["id"] = pluginId;
  changed["items"] = parseJsonSafe(uiItemsJson(pluginId));
  pushEvent("musicxx.ui.changed", pluginId, changed.dump());
  return PLUGINXX_RESULT_OK;
}

int32_t MusicxxHostManager::unregisterUiEntry(MusicxxHostInstance *inst,
                                              std::string_view itemId) {
  if (itemId.empty()) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  const std::string pluginId = inst ? pluginIdOf(inst->name) : std::string{};
  bool idOk = false;
  const std::string fullId = normalizeItemId(itemId, pluginId, idOk);
  if (!idOk) {
    return PLUGINXX_RESULT_ERR_PERMISSION;
  }
  auto it = uiRegistry_.entries().find(fullId);
  if (it == uiRegistry_.entries().end()) {
    return PLUGINXX_RESULT_ERR_NOT_FOUND;
  }
  if (inst && it->second.pluginId != pluginId) {
    return PLUGINXX_RESULT_ERR_PERMISSION;
  }
  const std::string ownerId = it->second.pluginId;
  uiRegistry_.entries().erase(it);

  Json changed;
  changed["action"] = "unregister";
  changed["id"] = ownerId;
  changed["items"] = parseJsonSafe(uiItemsJson(ownerId));
  pushEvent("musicxx.ui.changed", ownerId, changed.dump());
  return PLUGINXX_RESULT_OK;
}

int32_t MusicxxHostManager::listUiEntries(MusicxxHostInstance *inst,
                                          std::string &outJson) {
  const std::string pluginId = inst ? pluginIdOf(inst->name) : std::string{};
  outJson = uiItemsJson(pluginId);
  return PLUGINXX_RESULT_OK;
}

int32_t MusicxxHostManager::notifyUi(MusicxxHostInstance *inst,
                                     std::string_view messageJson) {
  if (!inst) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  const std::string message{messageJson};
  bool parseOk = false;
  Json data = parseJsonSafe(message, &parseOk);
  if (!parseOk || !data.is_object()) {
    return PLUGINXX_RESULT_ERR_JSON;
  }
  const std::string pluginId = pluginIdOf(inst->name);
  int64_t effectiveTimeout = 0;
  return pushAppCapabilityRequestEvent(pluginId, allocateRequestId(),
                                       "musicxx.ui.toast", message, 0,
                                       &effectiveTimeout);
}

/* ==================== 内容 ==================== */

int32_t MusicxxHostManager::registerUiContent(
    MusicxxHostInstance *inst, const MusicxxPluginUIContentSpec &spec,
    bool fromJs) {
  if (!inst) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  if (spec.struct_size != 0 &&
      spec.struct_size < sizeof(MusicxxPluginUIContentSpec)) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  if (spec.flags != 0 || spec.min_interval_ms < 0 || !spec.generate) {
    XX_LOGW("[musicxx_ext] content registration by plugin `{}`: flags / min_interval_ms / generate "
            "is invalid", inst->name);
    return PLUGINXX_RESULT_ERR_ARG;
  }
  if (inst->unloadRequested || !inst->enabled) {
    return PLUGINXX_RESULT_ERR_STATE;
  }
  if (!spec.id.data || spec.id.size == 0 || !spec.slot.data ||
      spec.slot.size == 0) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  const std::string pluginId = pluginIdOf(inst->name);
  bool idOk = false;
  const std::string fullId = normalizeItemId(
      std::string_view{spec.id.data, static_cast<size_t>(spec.id.size)}, pluginId,
      idOk);
  if (!idOk) {
    XX_LOGW("[musicxx_ext] content registration by plugin `{}` of `{}` rejected: invalid namespace",
            inst->name, fullId);
    return PLUGINXX_RESULT_ERR_PERMISSION;
  }
  const std::string ownPrefix = "plugin." + pluginId + ".";
  const std::string shortId = fullId.rfind(ownPrefix, 0) == 0
                                  ? fullId.substr(ownPrefix.size())
                                  : fullId;

  const std::string slot = slotFullName(
      std::string_view{spec.slot.data, static_cast<size_t>(spec.slot.size)});
  if (slot.empty()) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  const pluginxx::host::UiSlotDecl *known = knownSlotById(slot);
  const auto pluginSlot = uiRegistry_.pluginSlots().find(slot);
  const bool declared = pluginSlot != uiRegistry_.pluginSlots().end();
  if (nullptr == known && !declared) {
    XX_LOGW("[musicxx_ext] plugin `{}` registered content on an unknown slot `{}`", inst->name, slot);
    return PLUGINXX_RESULT_ERR_NOT_FOUND;
  }
  if (nullptr != known && std::string_view{known->kind ? known->kind : ""} == "item") {
    XX_LOGW("[musicxx_ext] slot `{}` is an entry slot, content cannot be attached", slot);
    return PLUGINXX_RESULT_ERR_ARG;
  }

  const std::string depsJson =
      spec.deps_json.data
          ? std::string{spec.deps_json.data,
                        static_cast<size_t>(spec.deps_json.size)}
          : std::string{"[]"};
  std::vector<std::string> deps;
  {
    bool ok = false;
    Json parsed = parseJsonSafe(depsJson, &ok);
    if (!ok || (!parsed.is_null() && !parsed.is_array())) {
      XX_LOGW("[musicxx_ext] deps of content `{}` from plugin `{}` must be a string array",
              fullId, inst->name);
      return PLUGINXX_RESULT_ERR_ARG;
    }
    if (parsed.is_array()) {
      for (const Json &item : parsed) {
        if (!item.is_string()) {
          return PLUGINXX_RESULT_ERR_ARG;
        }
        std::string err;
        const std::string dep = item.get<std::string>();
        if (!validDependency(dep, err)) {
          XX_LOGW("[musicxx_ext] content `{}` from plugin `{}` has an invalid dependency `{}`: {}",
                  fullId, inst->name, dep, err);
          return PLUGINXX_RESULT_ERR_ARG;
        }
        deps.push_back(dep);
      }
    }
  }

  UiContent content;
  content.plugin = inst->name;
  content.pluginId = pluginId;
  content.id = shortId;
  content.fullId = fullId;
  content.slot = slot;
  content.title = spec.title.data
                      ? std::string{spec.title.data,
                                    static_cast<size_t>(spec.title.size)}
                      : std::string{};
  content.depict = spec.depict.data
                       ? std::string{spec.depict.data,
                                     static_cast<size_t>(spec.depict.size)}
                       : std::string{};
  content.routesJson =
      spec.routes_json.data
          ? std::string{spec.routes_json.data,
                        static_cast<size_t>(spec.routes_json.size)}
          : std::string{"[]"};
  {
    // 路由只在注册时给: 客户端要先按路由挑"这一页用哪一份"
    bool ok = false;
    Json parsed = parseJsonSafe(content.routesJson, &ok);
    if (!ok || (!parsed.is_null() && !parsed.is_array())) {
      XX_LOGW("[musicxx_ext] routes of content `{}` from plugin `{}` must be a string array",
              fullId, inst->name);
      return PLUGINXX_RESULT_ERR_ARG;
    }
    if (parsed.is_array()) {
      for (const Json &item : parsed) {
        if (!item.is_string() || item.get<std::string>().empty()) {
          return PLUGINXX_RESULT_ERR_ARG;
        }
      }
    }
  }
  content.depsJson = depsJson;
  content.deps = std::move(deps);
  content.order = spec.order;
  content.minIntervalMs = spec.min_interval_ms;
  content.fromJs = fromJs;
  content.spec = spec;
  const auto existing = uiRegistry_.contents().find(fullId);
  if (existing != uiRegistry_.contents().end()) {
    if (existing->second.pluginId != pluginId) {
      return PLUGINXX_RESULT_ERR_PERMISSION;
    }
    content.seq = existing->second.seq;
    content.rev = existing->second.rev;
  } else {
    content.seq = uiRegistry_.nextContentSeq();
  }
  uiRegistry_.contents()[fullId] = std::move(content);

  Json changed;
  changed["action"] = "register";
  changed["id"] = pluginId;
  changed["contents"] = parseJsonSafe(uiContentsJson(pluginId));
  pushEvent("musicxx.ui.contents", pluginId, changed.dump());
  return PLUGINXX_RESULT_OK;
}

int32_t MusicxxHostManager::unregisterUiContent(MusicxxHostInstance *inst,
                                                std::string_view contentId) {
  if (contentId.empty()) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  const std::string pluginId = inst ? pluginIdOf(inst->name) : std::string{};
  bool idOk = false;
  const std::string fullId = normalizeItemId(contentId, pluginId, idOk);
  if (!idOk) {
    return PLUGINXX_RESULT_ERR_PERMISSION;
  }
  auto it = uiRegistry_.contents().find(fullId);
  if (it == uiRegistry_.contents().end()) {
    return PLUGINXX_RESULT_ERR_NOT_FOUND;
  }
  if (inst && it->second.pluginId != pluginId) {
    return PLUGINXX_RESULT_ERR_PERMISSION;
  }
  const std::string owner = it->second.pluginId;
  uiRegistry_.contents().erase(it);

  Json changed;
  changed["action"] = "unregister";
  changed["id"] = owner;
  changed["contents"] = parseJsonSafe(uiContentsJson(owner));
  pushEvent("musicxx.ui.contents", owner, changed.dump());
  return PLUGINXX_RESULT_OK;
}

int32_t MusicxxHostManager::pushUiContent(MusicxxHostInstance *inst,
                                          std::string_view contentId,
                                          std::string_view contentJson) {
  if (contentId.empty()) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  const std::string pluginId = inst ? pluginIdOf(inst->name) : std::string{};
  bool idOk = false;
  const std::string fullId = normalizeItemId(contentId, pluginId, idOk);
  if (!idOk) {
    return PLUGINXX_RESULT_ERR_PERMISSION;
  }
  UiContent *content = findUiContent(fullId);
  if (nullptr == content) {
    return PLUGINXX_RESULT_ERR_NOT_FOUND;
  }
  bool parseOk = false;
  const std::string text{contentJson};
  Json json = parseJsonSafe(text, &parseOk);
  if (!parseOk || !json.is_object()) {
    return PLUGINXX_RESULT_ERR_JSON;
  }
  // 只入队: 不回调插件, 不等结果
  pushUiContentResult(*content, text, std::string{});
  return PLUGINXX_RESULT_OK;
}

int32_t MusicxxHostManager::listUiContents(MusicxxHostInstance *inst,
                                           std::string &outJson) {
  outJson = uiContentsJson(inst ? pluginIdOf(inst->name) : std::string{});
  return PLUGINXX_RESULT_OK;
}

int32_t MusicxxHostManager::updateUiSlot(MusicxxHostInstance *inst,
                                         std::string_view slot,
                                         std::string_view idsJson) {
  if (!inst) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  const std::string pluginId = pluginIdOf(inst->name);
  const std::string slotId = slotFullName(slot);
  if (slotId.empty()) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  std::vector<std::string> wanted;
  {
    const std::string text{idsJson};
    if (!text.empty()) {
      bool ok = false;
      Json parsed = parseJsonSafe(text, &ok);
      if (!ok || !parsed.is_array()) {
        return PLUGINXX_RESULT_ERR_ARG;
      }
      for (const Json &item : parsed) {
        if (!item.is_string()) {
          return PLUGINXX_RESULT_ERR_ARG;
        }
        wanted.push_back(item.get<std::string>());
      }
    }
  }
  int32_t count = 0;
  Json emptyContext = Json::object();
  for (auto &[fullId, content] : uiRegistry_.contents()) {
    (void)fullId;
    if (content.pluginId != pluginId || content.slot != slotId) {
      continue;
    }
    if (!wanted.empty() &&
        std::find(wanted.begin(), wanted.end(), content.id) == wanted.end()) {
      continue;
    }
    // 空 context: 主程序会用自己的变量再问一次 (生成函数只看给它的键)
    runUiContent(content, emptyContext.dump());
    ++count;
  }
  return count > 0 ? PLUGINXX_RESULT_OK : PLUGINXX_RESULT_ERR_NOT_FOUND;
}

/* ==================== 插件声明的槽位 ==================== */

int32_t MusicxxHostManager::defineUiSlot(MusicxxHostInstance *inst,
                                         const MusicxxPluginUISlotSpec &spec) {
  if (!inst) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  if (spec.struct_size != 0 &&
      spec.struct_size < sizeof(MusicxxPluginUISlotSpec)) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  if (spec.flags != 0 || !spec.id.data || spec.id.size == 0 || !spec.kind.data ||
      spec.kind.size == 0) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  const std::string pluginId = pluginIdOf(inst->name);
  const std::string id{spec.id.data, static_cast<size_t>(spec.id.size)};
  const std::string kind{spec.kind.data, static_cast<size_t>(spec.kind.size)};
  if (kind != "insert" && kind != "replace") {
    XX_LOGW("[musicxx_ext] slot `{}` declared by plugin `{}` must be insert / replace",
            id, inst->name);
    return PLUGINXX_RESULT_ERR_ARG;
  }
  const std::string prefix = "plugin." + pluginId + ".ui.";
  if (id.rfind(prefix, 0) != 0 || id.size() == prefix.size()) {
    XX_LOGW("[musicxx_ext] slot `{}` declared by plugin `{}` must be written plugin.{}.ui.<name>",
            id, inst->name, pluginId);
    return PLUGINXX_RESULT_ERR_PERMISSION;
  }
  if (nullptr != knownSlotById(id)) {
    return PLUGINXX_RESULT_ERR_PERMISSION;
  }
  const auto existing = uiRegistry_.pluginSlots().find(id);
  if (existing != uiRegistry_.pluginSlots().end() && existing->second.pluginId != pluginId) {
    return PLUGINXX_RESULT_ERR_PERMISSION;
  }
  UiPluginSlot slot;
  slot.plugin = inst->name;
  slot.pluginId = pluginId;
  slot.id = id;
  slot.kind = kind;
  slot.seq = existing != uiRegistry_.pluginSlots().end() ? existing->second.seq
                                              : uiRegistry_.nextSlotSeq();
  uiRegistry_.pluginSlots()[id] = std::move(slot);

  Json changed;
  changed["action"] = "define";
  changed["id"] = pluginId;
  changed["slots"] = parseJsonSafe(uiSlotsJson(std::string{}));
  pushEvent("musicxx.ui.slots", pluginId, changed.dump());
  return PLUGINXX_RESULT_OK;
}

int32_t MusicxxHostManager::undefineUiSlot(MusicxxHostInstance *inst,
                                           std::string_view slot) {
  if (!inst || slot.empty()) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  const std::string pluginId = pluginIdOf(inst->name);
  auto it = uiRegistry_.pluginSlots().find(std::string{slot});
  if (it == uiRegistry_.pluginSlots().end()) {
    return PLUGINXX_RESULT_ERR_NOT_FOUND;
  }
  if (it->second.pluginId != pluginId) {
    return PLUGINXX_RESULT_ERR_PERMISSION;
  }
  const std::string id = it->second.id;
  uiRegistry_.pluginSlots().erase(it);
  // 挂在这个槽位上的内容一起失效
  for (auto contentIt = uiRegistry_.contents().begin(); contentIt != uiRegistry_.contents().end();) {
    if (contentIt->second.slot == id) {
      contentIt = uiRegistry_.contents().erase(contentIt);
    } else {
      ++contentIt;
    }
  }

  Json changed;
  changed["action"] = "undefine";
  changed["id"] = pluginId;
  changed["slot"] = id;
  changed["slots"] = parseJsonSafe(uiSlotsJson(std::string{}));
  pushEvent("musicxx.ui.slots", pluginId, changed.dump());
  return PLUGINXX_RESULT_OK;
}

int32_t MusicxxHostManager::listUiSlots(MusicxxHostInstance *inst,
                                        std::string &outJson) {
  (void)inst;
  outJson = uiSlotsJson(std::string{});
  return PLUGINXX_RESULT_OK;
}

/* ==================== 生成与推送 ==================== */

MusicxxHostManager::UiContent *MusicxxHostManager::findUiContent(
    std::string_view fullId) {
  return uiRegistry_.findContent(fullId);
}

int32_t MusicxxHostManager::requestUiContent(std::string_view contentId,
                                             std::string_view contextJson,
                                             int64_t *outRequestId) {
  const std::string fullId{contentId};
  if (fullId.empty()) {
    return PLUGINXX_RESULT_ERR_ARG;
  }
  int64_t requestId = 0;
  const int32_t acceptRc = acceptRequest("ui.request", &requestId);
  if (acceptRc != PLUGINXX_RESULT_OK) {
    return acceptRc;
  }
  const std::string ctx{contextJson.empty() ? "{}" : contextJson};
  auto self = shared_from_this();
  /// 注册表归框架线程; 生成函数投到插件执行线程 (原生) / JS 线程 (JS)
  asio::post(ctx_->io, [self, requestId, fullId, ctx]() {
    auto *content = self->findUiContent(fullId);
    if (nullptr == content) {
      self->completeRequest(
          requestId,
          RequestOutcome::fail(PLUGINXX_RESULT_ERR_NOT_FOUND,
                               "no such content (" + fullId + ")", "not_found"));
      return;
    }
    self->runUiContent(*content, ctx, requestId);
  });
  if (outRequestId) {
    *outRequestId = requestId;
  }
  return PLUGINXX_RESULT_OK;
}


/* ==================== 原生内容生成的完成等待者 ==================== */

namespace {

/// 一次原生内容生成的完成等待者 (插件调用 notify 时释放)
struct NativeGenerateWaiter {
  std::atomic<bool> done{false};
  std::function<void(int32_t, std::string_view)> finish;
};

void PLUGINXX_CALL nativeGenerateDoneTrampoline(
    void *ud, int32_t status, const PluginxxStringView *payload) {
  auto *waiter = static_cast<NativeGenerateWaiter *>(ud);
  if (!waiter) {
    return;
  }
  if (waiter->done.exchange(true)) {
    return;
  }
  const std::string_view text =
      (payload && payload->data && payload->size > 0)
          ? std::string_view{payload->data, static_cast<size_t>(payload->size)}
          : std::string_view{};
  waiter->finish(status, text);
  delete waiter;
}

} // namespace

int32_t MusicxxHostManager::runUiContent(UiContent &content,
                                         std::string_view contextJson,
                                         int64_t requestId) {
  const std::string ctx{contextJson.empty() ? "{}" : contextJson};
  /// JS 插件的生成函数在 JS 线程执行: 框架线程只投递、不等结果
  ///
  /// 为什么不能同步等: JS 线程此刻可能在等一个应用能力的结果 (框架线程又在等它
  /// 就是互锁), 而生成函数是插件作者写的普通函数, 不能假设它随时可重入。
  if (content.fromJs) {
    auto engine = jsEngine();
    if (!engine) {
      finishUiGeneration(requestId, content.fullId, std::string{},
                         "the JS runtime is unavailable");
      return PLUGINXX_RESULT_ERR_STATE;
    }
    const std::string fullId = content.fullId;
    const std::string instanceName = content.plugin;
    auto self = shared_from_this();
    engine->requestUiContent(
        instanceName, fullId, ctx,
        [self, fullId, requestId](std::string json, std::string error) {
          /// 回到框架线程再写注册表 (推送与 rev 都只在那条线程)
          asio::post(self->ctx_->io,
                     [self, fullId, requestId, json = std::move(json),
                      error = std::move(error)]() {
                       self->finishUiGeneration(requestId, fullId, json, error);
                     });
        });
    return PLUGINXX_RESULT_OK;
  }
  /// 原生插件的生成函数也是异步操作契约: 参数在这里复制, 插件可以同步完成,
  /// 也可以先返回、稍后再调 notify (协程里 await 别的东西)。完成一律回到框架线程,
  /// 只写注册表, 不碰插件状态。
  const std::string fullId = content.fullId;
  const PluginxxHost *host = nullptr;
  if (auto instance = resolveInstance(content.plugin)) {
    host = instance->hostView();
  }
  auto self = shared_from_this();
  auto context = std::make_shared<std::string>(ctx);
  auto *waiter = new NativeGenerateWaiter();
  waiter->finish = [self, fullId,
                    requestId](int32_t status, std::string_view payload) {
    std::string text{payload};
    std::string error;
    if (status != PLUGINXX_OPERATOR_OK) {
      error = text.empty() ? std::string{"the generate function reported a failure"} : text;
      text.clear();
    } else {
      bool parseOk = false;
      Json parsed = parseJsonSafe(text, &parseOk);
      if (!parseOk || !parsed.is_object()) {
        error = "the generate function did not return a JSON object";
        text.clear();
      }
    }
    asio::post(
        self->ctx_->io,
        [self, fullId, requestId, text = std::move(text),
         error = std::move(error)]() {
          self->finishUiGeneration(requestId, fullId, text, error);
        });
  };
  PluginxxOperatorNotify notify{&nativeGenerateDoneTrampoline, waiter};
  auto generate = content.spec.generate;
  void *userData = content.spec.user_data;
  /// 生成函数是插件代码: 只在**插件执行线程**上调用, 框架线程投递完就回去处理
  /// 注册表与事件 (慢的生成函数不会挡住别的请求)
  asio::post(pluginxx::pluginExecutionExecutor(runtime()),
             [context, host, generate, userData, notify]() {
               const PluginxxStringView ctxView{context->data(),
                                                context->size()};
               PluginxxString errorOut{};
               try {
                 generate(host, &ctxView, userData, &notify, &errorOut);
               } catch (const std::exception &e) {
                 if (errorOut.data) {
                   if (host && host->vtable && host->vtable->free) {
                     host->vtable->free(errorOut.data);
                   }
                   errorOut = PluginxxString{};
                 }
                 const std::string reason{e.what()};
                 const PluginxxStringView reasonView{reason.data(),
                                                     reason.size()};
                 nativeGenerateDoneTrampoline(notify.host_ud,
                                              PLUGINXX_OPERATOR_FAILED,
                                              &reasonView);
                 return;
               } catch (...) {
                 if (errorOut.data) {
                   if (host && host->vtable && host->vtable->free) {
                     host->vtable->free(errorOut.data);
                   }
                   errorOut = PluginxxString{};
                 }
                 /// 未知异常也必须收尾: 不收就可能没人再调 notify, 请求一直不完成
                 const std::string reason{"the generate function threw an unknown exception"};
                 const PluginxxStringView reasonView{reason.data(),
                                                     reason.size()};
                 nativeGenerateDoneTrampoline(notify.host_ud,
                                              PLUGINXX_OPERATOR_FAILED,
                                              &reasonView);
                 return;
               }
               if (errorOut.data) {
                 std::string reason{errorOut.data,
                                    static_cast<size_t>(errorOut.size)};
                 if (host && host->vtable && host->vtable->free) {
                   host->vtable->free(errorOut.data);
                 }
                 // 未受理: 插件不会再调 notify, 这里按失败收尾 (waiter 由 trampoline 释放)
                 const PluginxxStringView reasonView{reason.data(), reason.size()};
                 nativeGenerateDoneTrampoline(notify.host_ud,
                                              PLUGINXX_OPERATOR_FAILED,
                                              &reasonView);
               }
             });
  return PLUGINXX_RESULT_OK;
}

std::string MusicxxHostManager::pushUiContentResult(
    const UiContent &content, const std::string &jsonText,
    const std::string &error) {
  UiContent *mutable_content = findUiContent(content.fullId);
  if (nullptr == mutable_content) {
    return std::string{};
  }
  const std::string text =
      uiRegistry_.buildContentPayload(mutable_content->fullId, jsonText, error);
  if (text.empty()) {
    return std::string{};
  }
  pushEvent("musicxx.ui.content", mutable_content->pluginId, text);
  return text;
}

void MusicxxHostManager::finishUiGeneration(int64_t requestId,
                                           const std::string &fullId,
                                           std::string jsonText,
                                           std::string error) {
  /// 框架线程: 写注册表 (rev) + 推 `musicxx.ui.content`, 再把同一条载乀
  /// 当作请求结果交回 (要等生成结果的调用点读它)
  std::string payloadText;
  if (auto *item = findUiContent(fullId)) {
    payloadText = pushUiContentResult(*item, jsonText, error);
  }
  if (requestId <= 0) {
    return;
  }
  if (!error.empty()) {
    completeRequest(requestId, RequestOutcome::fail(
                                   PLUGINXX_RESULT_ERR_STATE, error, "ui_failed"));
    return;
  }
  if (payloadText.empty()) {
    completeRequest(
        requestId,
        RequestOutcome::fail(PLUGINXX_RESULT_ERR_NOT_FOUND,
                             "the plugin removed this content (" + fullId + ")", "not_found"));
    return;
  }
  completeRequest(requestId, RequestOutcome::ok(payloadText));
}

/* ==================== 序列化与摘除 ==================== */

std::string MusicxxHostManager::uiItemsJson(const std::string &pluginId) const {
  return uiRegistry_.entriesJson(pluginId);
}

std::string MusicxxHostManager::uiContentsJson(
    const std::string &pluginId) const {
  return uiRegistry_.contentsJson(pluginId);
}

std::string MusicxxHostManager::uiSlotsJson(const std::string &pluginId) const {
  return uiRegistry_.slotsJson(catalogOf(), pluginId);
}

const pluginxx::host::UiSlotCatalog &MusicxxHostManager::uiSlotCatalog() const {
  return catalogOf();
}

void MusicxxHostManager::detachUiEntries(const std::string &instanceName) {
  const std::string pluginId = pluginIdOf(instanceName);
  const pluginxx::host::UiDetachResult removed =
      uiRegistry_.detachOwner(instanceName);
  if (removed.entries) {
    Json changed;
    changed["action"] = "detach";
    changed["id"] = pluginId;
    changed["items"] = parseJsonSafe(uiItemsJson(pluginId));
    pushEvent("musicxx.ui.changed", pluginId, changed.dump());
  }
  if (removed.contents) {
    Json changed;
    changed["action"] = "detach";
    changed["id"] = pluginId;
    changed["contents"] = parseJsonSafe(uiContentsJson(pluginId));
    pushEvent("musicxx.ui.contents", pluginId, changed.dump());
  }
  if (removed.slots) {
    Json changed;
    changed["action"] = "detach";
    changed["id"] = pluginId;
    changed["slots"] = parseJsonSafe(uiSlotsJson(std::string{}));
    pushEvent("musicxx.ui.slots", pluginId, changed.dump());
  }
}

/* ==================== C ABI: musicxx.ui 表 ==================== */

/* ==================== `musicxx.ui` 表的 C ABI 入口 ==================== */
//
// 每个成员都是异步操作契约: 参数在这一层复制成自有字符串 (借用视图在排队执行时可能
// 已经失效), 工作体在框架 IO 线程上跑, 结果经完成回调恰好一次交回。

namespace {

std::string uiViewToString(const PluginxxStringView &view) {
  if (!view.data || view.size == 0) {
    return {};
  }
  return std::string{view.data, static_cast<size_t>(view.size)};
}

PluginxxStringView uiViewOf(const std::string &text) {
  return PluginxxStringView{text.data(), static_cast<uint64_t>(text.size())};
}

/// 表成员的业务失败原因 (kit 与脚本看到的就是这段文本)
const char *uiReasonOf(int32_t rc) {
  switch (rc) {
  case PLUGINXX_RESULT_ERR_ARG:
    return "arg";
  case PLUGINXX_RESULT_ERR_PERMISSION:
    return "namespace";
  case PLUGINXX_RESULT_ERR_NOT_DECLARED:
  case PLUGINXX_RESULT_ERR_NOT_FOUND:
    return "not_found";
  case PLUGINXX_RESULT_ERR_NO_VALUE:
    return "no_value";
  case PLUGINXX_RESULT_ERR_ENDED:
    return "ended";
  default:
    return "state";
  }
}

/// 业务失败统一抛 [pluginxx::TableOpError]: 由 startTableOp 转成 FAILED + 原因
[[noreturn]] void throwUiFailure(const char *what, int32_t rc) {
  throw pluginxx::TableOpError(std::string{what} + ": " + uiReasonOf(rc));
}

PluginxxOperatorHandle *PLUGINXX_CALL xx_ui_register_entry(
    const PluginxxHost *host, const MusicxxPluginUIEntrySpec *spec,
    PluginxxOperatorCallback cb, void *cbUd, PluginxxString *errorOut) {
  if (!spec) {
    pluginxx::detail::setErrorString(errorOut, "register_entry: missing spec");
    return nullptr;
  }
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(errorOut,
                                     "register_entry: host is unavailable");
    return nullptr;
  }
  const std::string itemId = uiViewToString(spec->item_id);
  const std::string type = uiViewToString(spec->type);
  const std::string data = uiViewToString(spec->data_json);
  const int32_t order = spec->order;
  const int32_t flags = spec->flags;
  return pluginxx::startTableOp(
      call.manager(), cb, cbUd, errorOut, "musicxx.ui.register_entry",
      [call, itemId, type, data, order, flags]() -> std::string {
        MusicxxPluginUIEntrySpec owned{};
        owned.version = 1;
        owned.struct_size = sizeof(MusicxxPluginUIEntrySpec);
        owned.item_id = uiViewOf(itemId);
        owned.type = uiViewOf(type);
        owned.data_json = uiViewOf(data);
        owned.order = order;
        owned.flags = flags;
        const int32_t rc =
            call.manager()->registerUiEntry(call.instance(), owned);
        if (rc != PLUGINXX_RESULT_OK) {
          throwUiFailure("register_entry", rc);
        }
        return std::string{};
      });
}

PluginxxOperatorHandle *PLUGINXX_CALL xx_ui_unregister_entry(
    const PluginxxHost *host, const PluginxxStringView *itemId,
    PluginxxOperatorCallback cb, void *cbUd, PluginxxString *errorOut) {
  if (!itemId || !itemId->data) {
    pluginxx::detail::setErrorString(errorOut,
                                     "unregister_entry: missing item id");
    return nullptr;
  }
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(errorOut,
                                     "unregister_entry: host is unavailable");
    return nullptr;
  }
  const std::string id = uiViewToString(*itemId);
  return pluginxx::startTableOp(
      call.manager(), cb, cbUd, errorOut, "musicxx.ui.unregister_entry",
      [call, id]() -> std::string {
        const int32_t rc =
            call.manager()->unregisterUiEntry(call.instance(), id);
        if (rc != PLUGINXX_RESULT_OK) {
          throwUiFailure("unregister_entry", rc);
        }
        return std::string{};
      });
}

PluginxxOperatorHandle *PLUGINXX_CALL xx_ui_list_entries(
    const PluginxxHost *host, PluginxxOperatorCallback cb, void *cbUd,
    PluginxxString *errorOut) {
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
          host, /*allowClosing=*/true);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(errorOut,
                                     "list_entries: host is unavailable");
    return nullptr;
  }
  return pluginxx::startTableOp(
      call.manager(), cb, cbUd, errorOut, "musicxx.ui.list_entries",
      [call]() -> std::string {
        std::string json;
        const int32_t rc =
            call.manager()->listUiEntries(call.instance(), json);
        if (rc != PLUGINXX_RESULT_OK) {
          throwUiFailure("list_entries", rc);
        }
        return json;
      });
}

PluginxxOperatorHandle *PLUGINXX_CALL xx_ui_register_content(
    const PluginxxHost *host, const MusicxxPluginUIContentSpec *spec,
    PluginxxOperatorCallback cb, void *cbUd, PluginxxString *errorOut) {
  if (!spec) {
    pluginxx::detail::setErrorString(errorOut,
                                     "register_content: missing spec");
    return nullptr;
  }
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(errorOut,
                                     "register_content: host is unavailable");
    return nullptr;
  }
  const std::string id = uiViewToString(spec->id);
  const std::string slot = uiViewToString(spec->slot);
  const std::string title = uiViewToString(spec->title);
  const std::string depict = uiViewToString(spec->depict);
  const std::string routes = uiViewToString(spec->routes_json);
  const std::string deps = uiViewToString(spec->deps_json);
  const int32_t order = spec->order;
  const int32_t minIntervalMs = spec->min_interval_ms;
  const int32_t flags = spec->flags;
  auto generate = spec->generate;
  void *userData = spec->user_data;
  return pluginxx::startTableOp(
      call.manager(), cb, cbUd, errorOut, "musicxx.ui.register_content",
      [call, id, slot, title, depict, routes, deps, order, minIntervalMs, flags,
       generate, userData]() -> std::string {
        MusicxxPluginUIContentSpec owned{};
        owned.version = 1;
        owned.struct_size = sizeof(MusicxxPluginUIContentSpec);
        owned.id = uiViewOf(id);
        owned.slot = uiViewOf(slot);
        owned.title = uiViewOf(title);
        owned.depict = uiViewOf(depict);
        owned.routes_json = uiViewOf(routes);
        owned.deps_json = uiViewOf(deps);
        owned.order = order;
        owned.min_interval_ms = minIntervalMs;
        owned.flags = flags;
        owned.generate = generate;
        owned.user_data = userData;
        const int32_t rc =
            call.manager()->registerUiContent(call.instance(), owned);
        if (rc != PLUGINXX_RESULT_OK) {
          throwUiFailure("register_content", rc);
        }
        return std::string{};
      });
}

PluginxxOperatorHandle *PLUGINXX_CALL xx_ui_unregister_content(
    const PluginxxHost *host, const PluginxxStringView *contentId,
    PluginxxOperatorCallback cb, void *cbUd, PluginxxString *errorOut) {
  if (!contentId || !contentId->data) {
    pluginxx::detail::setErrorString(errorOut,
                                     "unregister_content: missing content id");
    return nullptr;
  }
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(
        errorOut, "unregister_content: host is unavailable");
    return nullptr;
  }
  const std::string id = uiViewToString(*contentId);
  return pluginxx::startTableOp(
      call.manager(), cb, cbUd, errorOut, "musicxx.ui.unregister_content",
      [call, id]() -> std::string {
        const int32_t rc =
            call.manager()->unregisterUiContent(call.instance(), id);
        if (rc != PLUGINXX_RESULT_OK) {
          throwUiFailure("unregister_content", rc);
        }
        return std::string{};
      });
}

PluginxxOperatorHandle *PLUGINXX_CALL xx_ui_push_content(
    const PluginxxHost *host, const PluginxxStringView *contentId,
    const PluginxxStringView *contentJson, PluginxxOperatorCallback cb,
    void *cbUd, PluginxxString *errorOut) {
  if (!contentId || !contentId->data) {
    pluginxx::detail::setErrorString(errorOut,
                                     "push_content: missing content id");
    return nullptr;
  }
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(errorOut,
                                     "push_content: host is unavailable");
    return nullptr;
  }
  const std::string id = uiViewToString(*contentId);
  const std::string json =
      contentJson ? uiViewToString(*contentJson) : std::string{};
  return pluginxx::startTableOp(
      call.manager(), cb, cbUd, errorOut, "musicxx.ui.push_content",
      [call, id, json]() -> std::string {
        const int32_t rc =
            call.manager()->pushUiContent(call.instance(), id, json);
        if (rc != PLUGINXX_RESULT_OK) {
          throwUiFailure("push_content", rc);
        }
        return std::string{};
      });
}

PluginxxOperatorHandle *PLUGINXX_CALL xx_ui_list_contents(
    const PluginxxHost *host, PluginxxOperatorCallback cb, void *cbUd,
    PluginxxString *errorOut) {
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
          host, /*allowClosing=*/true);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(errorOut,
                                     "list_contents: host is unavailable");
    return nullptr;
  }
  return pluginxx::startTableOp(
      call.manager(), cb, cbUd, errorOut, "musicxx.ui.list_contents",
      [call]() -> std::string {
        std::string json;
        const int32_t rc =
            call.manager()->listUiContents(call.instance(), json);
        if (rc != PLUGINXX_RESULT_OK) {
          throwUiFailure("list_contents", rc);
        }
        return json;
      });
}

PluginxxOperatorHandle *PLUGINXX_CALL xx_ui_update_slot(
    const PluginxxHost *host, const PluginxxStringView *slot,
    const PluginxxStringView *idsJson, PluginxxOperatorCallback cb, void *cbUd,
    PluginxxString *errorOut) {
  if (!slot || !slot->data) {
    pluginxx::detail::setErrorString(errorOut, "update_slot: missing slot");
    return nullptr;
  }
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(errorOut,
                                     "update_slot: host is unavailable");
    return nullptr;
  }
  const std::string slotId = uiViewToString(*slot);
  const std::string ids = idsJson ? uiViewToString(*idsJson) : std::string{};
  return pluginxx::startTableOp(
      call.manager(), cb, cbUd, errorOut, "musicxx.ui.update_slot",
      [call, slotId, ids]() -> std::string {
        const int32_t rc =
            call.manager()->updateUiSlot(call.instance(), slotId, ids);
        if (rc != PLUGINXX_RESULT_OK) {
          throwUiFailure("update_slot", rc);
        }
        return std::string{};
      });
}

PluginxxOperatorHandle *PLUGINXX_CALL xx_ui_define_slot(
    const PluginxxHost *host, const MusicxxPluginUISlotSpec *spec,
    PluginxxOperatorCallback cb, void *cbUd, PluginxxString *errorOut) {
  if (!spec) {
    pluginxx::detail::setErrorString(errorOut, "define_slot: missing spec");
    return nullptr;
  }
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(errorOut,
                                     "define_slot: host is unavailable");
    return nullptr;
  }
  const std::string id = uiViewToString(spec->id);
  const std::string kind = uiViewToString(spec->kind);
  const int32_t flags = spec->flags;
  return pluginxx::startTableOp(
      call.manager(), cb, cbUd, errorOut, "musicxx.ui.define_slot",
      [call, id, kind, flags]() -> std::string {
        MusicxxPluginUISlotSpec owned{};
        owned.version = 1;
        owned.struct_size = sizeof(MusicxxPluginUISlotSpec);
        owned.id = uiViewOf(id);
        owned.kind = uiViewOf(kind);
        owned.flags = flags;
        const int32_t rc =
            call.manager()->defineUiSlot(call.instance(), owned);
        if (rc != PLUGINXX_RESULT_OK) {
          throwUiFailure("define_slot", rc);
        }
        return std::string{};
      });
}

PluginxxOperatorHandle *PLUGINXX_CALL xx_ui_undefine_slot(
    const PluginxxHost *host, const PluginxxStringView *slot,
    PluginxxOperatorCallback cb, void *cbUd, PluginxxString *errorOut) {
  if (!slot || !slot->data) {
    pluginxx::detail::setErrorString(errorOut, "undefine_slot: missing slot");
    return nullptr;
  }
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(errorOut,
                                     "undefine_slot: host is unavailable");
    return nullptr;
  }
  const std::string id = uiViewToString(*slot);
  return pluginxx::startTableOp(
      call.manager(), cb, cbUd, errorOut, "musicxx.ui.undefine_slot",
      [call, id]() -> std::string {
        const int32_t rc =
            call.manager()->undefineUiSlot(call.instance(), id);
        if (rc != PLUGINXX_RESULT_OK) {
          throwUiFailure("undefine_slot", rc);
        }
        return std::string{};
      });
}

PluginxxOperatorHandle *PLUGINXX_CALL xx_ui_list_slots(
    const PluginxxHost *host, PluginxxOperatorCallback cb, void *cbUd,
    PluginxxString *errorOut) {
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(
          host, /*allowClosing=*/true);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(errorOut,
                                     "list_slots: host is unavailable");
    return nullptr;
  }
  return pluginxx::startTableOp(
      call.manager(), cb, cbUd, errorOut, "musicxx.ui.list_slots",
      [call]() -> std::string {
        std::string json;
        const int32_t rc = call.manager()->listUiSlots(call.instance(), json);
        if (rc != PLUGINXX_RESULT_OK) {
          throwUiFailure("list_slots", rc);
        }
        return json;
      });
}

PluginxxOperatorHandle *PLUGINXX_CALL xx_ui_notify(
    const PluginxxHost *host, const PluginxxStringView *messageJson,
    PluginxxOperatorCallback cb, void *cbUd, PluginxxString *errorOut) {
  if (!messageJson || !messageJson->data) {
    pluginxx::detail::setErrorString(errorOut, "notify: missing message");
    return nullptr;
  }
  auto call =
      pluginxx::enterPluginHost<MusicxxHostInstance, MusicxxHostManager>(host);
  if (!call.ok()) {
    pluginxx::detail::setErrorString(errorOut,
                                     "notify: host is unavailable");
    return nullptr;
  }
  const std::string message = uiViewToString(*messageJson);
  return pluginxx::startTableOp(
      call.manager(), cb, cbUd, errorOut, "musicxx.ui.notify",
      [call, message]() -> std::string {
        const int32_t rc = call.manager()->notifyUi(call.instance(), message);
        if (rc != PLUGINXX_RESULT_OK) {
          throwUiFailure("notify", rc);
        }
        return std::string{};
      });
}

const MusicxxPluginUIIface g_ifaceUi = {
    /* version */ MUSICXX_PLUGIN_IFACE_UI_VERSION,
    /* struct_size */ sizeof(MusicxxPluginUIIface),
    /* register_entry */ xx_ui_register_entry,
    /* unregister_entry */ xx_ui_unregister_entry,
    /* list_entries */ xx_ui_list_entries,
    /* register_content */ xx_ui_register_content,
    /* unregister_content */ xx_ui_unregister_content,
    /* push_content */ xx_ui_push_content,
    /* list_contents */ xx_ui_list_contents,
    /* update_slot */ xx_ui_update_slot,
    /* define_slot */ xx_ui_define_slot,
    /* undefine_slot */ xx_ui_undefine_slot,
    /* list_slots */ xx_ui_list_slots,
    /* notify */ xx_ui_notify,
};

} // namespace

const void *uiIfaceForQuery() { return &g_ifaceUi; }

} // namespace extern_plugin
} // namespace musicxx
