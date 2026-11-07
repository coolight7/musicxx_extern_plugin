/// cxx_pluginxx (插件框架共用库) 原生测试 —— 不依赖 Dart, 也不启动 musicxx 插件框架
///
/// 定位: 共用库被多个主程序使用, 它的执行模型 (能力登记/阶段链/合并、广播缓存与写权限)
/// 换实现时必须有自己的一套测试, 否则只能靠主程序的原生测试间接验证。
///
/// 覆盖:
/// - 接口表元信息 (版本门槛 / 基线字节数 / 追加成员判定 / 语义档位判定);
/// - 作用域引用校验;
/// - 能力登记表 (声明 / 多实现 / 阶段链 / 覆盖 / 注销 / 自省);
/// - 广播登记表 (缓存 / 写权限 / 订阅即推 / 退订 / 自省);
/// - 广播订阅句柄的幂等撤销入口。
///
/// 运行: musicxx_pluginxx_test (无参数; 由 tools/build_native.ps1 -RunTests 一起跑)

#include "pluginxx/api/abi.h"
#include "pluginxx/api/iface_meta.h"
#include "pluginxx/host/broadcast_registry.h"
#include "pluginxx/host/capability_chain.h"
#include "pluginxx/host/capability_registry.h"
// 事件订阅句柄的撤销入口是内联函数, 实现体在 runtime/instance_base.h
#include "pluginxx/runtime/instance_base.h"
// 生成物：能力 / 广播的 id 常量与已知声明表（定义源 tools/capabilities.def.json）
#include "musicxx/plugin/api/capability_ids.g.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failed = 0;
int g_checks = 0;

void check(bool ok, const char *what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("  [FAIL] %s\n", what);
  } else {
    std::printf("  [ ok ] %s\n", what);
  }
}

/// 能力启动回调 (只用于登记表测试, 不会被调用)
void *PLUGINXX_CALL fakeStart(void *, const PluginxxHost *, const PluginxxStringView *,
                              const PluginxxStringView *,
                              const PluginxxOperatorNotify *, PluginxxString *) {
  return nullptr;
}

/// 能力取消回调 (只用于登记表测试, 不会被调用)
void PLUGINXX_CALL fakeCancel(void *, void *) {}

/// 一个只带表头的空表 (表长不足以覆盖任何成员)
template<typename Iface>
Iface headerOnly() {
  Iface t{};
  t.version = 1;
  t.struct_size = 8;  // 只有 version + struct_size
  return t;
}

/* ==================== 作用域引用 ==================== */

void testScopeRef() {
  std::printf("\n-- 作用域引用\n");
  check(pluginxx_scope_ref_ok(nullptr) == 0, "空指针判为不可读");

  PluginxxScopeRef ref{};
  ref.struct_size = 0;
  check(pluginxx_scope_ref_ok(&ref) == 1, "struct_size = 0 视为旧的完整引用");

  ref.struct_size = 8;
  check(pluginxx_scope_ref_ok(&ref) == 0, "struct_size 不足基线判为不可读");

  ref.struct_size = static_cast<uint32_t>(sizeof(PluginxxScopeRef));
  check(pluginxx_scope_ref_ok(&ref) == 1, "完整 struct_size 判为可读");
}

/* ==================== 接口表元信息 ==================== */

template<typename Iface>
void checkIfaceMeta(const char *name, int32_t version, int32_t minVersion,
                    uint32_t baseline) {
  char what[160];
  std::snprintf(what, sizeof(what), "%s: 最低版本不高于当前版本 (%d <= %d)",
                name, minVersion, version);
  check(minVersion <= version, what);

  std::snprintf(what, sizeof(what), "%s: 基线不少于表头 (%u >= 8)",
                name, baseline);
  check(baseline >= 8u, what);

  // IfaceMeta 的特化必须与表头声明的常量一致, 否则判定门槛会和头文件说的不是一件事
  std::snprintf(what, sizeof(what), "%s: IfaceMeta 的基线等于表头常量", name);
  check(pluginxx::IfaceMeta<Iface>::baseline_size == baseline, what);

  std::snprintf(what, sizeof(what), "%s: IfaceMeta 的最低版本等于表头常量", name);
  check(pluginxx::IfaceMeta<Iface>::min_version == minVersion, what);

  // 追加成员是往表尾加的: 基线必须能落在表内
  std::snprintf(what, sizeof(what), "%s: 基线不超过完整表长 (%u <= %zu)",
                name, baseline, sizeof(Iface));
  check(baseline <= sizeof(Iface), what);
}

void testIfaceMeta() {
  std::printf("\n-- 接口表元信息\n");
  checkIfaceMeta<PluginxxBroadcastsIface>("broadcasts",
                                          PLUGINXX_IFACE_BROADCASTS_VERSION,
                                          PLUGINXX_IFACE_BROADCASTS_MIN_VERSION,
                                          PLUGINXX_IFACE_BROADCASTS_BASELINE_SIZE);
  checkIfaceMeta<PluginxxCapabilitiesIface>(
      "capabilities", PLUGINXX_IFACE_CAPABILITIES_VERSION,
      PLUGINXX_IFACE_CAPABILITIES_MIN_VERSION,
      PLUGINXX_IFACE_CAPABILITIES_BASELINE_SIZE);
  checkIfaceMeta<PluginxxSchedulerIface>("scheduler",
                                         PLUGINXX_IFACE_SCHEDULER_VERSION,
                                         PLUGINXX_IFACE_SCHEDULER_MIN_VERSION,
                                         PLUGINXX_IFACE_SCHEDULER_BASELINE_SIZE);
  checkIfaceMeta<PluginxxCoroutineRuntimeIface>(
      "coroutine_runtime", PLUGINXX_IFACE_COROUTINE_RUNTIME_VERSION,
      PLUGINXX_IFACE_COROUTINE_RUNTIME_MIN_VERSION,
      PLUGINXX_IFACE_COROUTINE_RUNTIME_BASELINE_SIZE);
  checkIfaceMeta<PluginxxPluginsIface>("plugins", PLUGINXX_IFACE_PLUGINS_VERSION,
                                       PLUGINXX_IFACE_PLUGINS_MIN_VERSION,
                                       PLUGINXX_IFACE_PLUGINS_BASELINE_SIZE);
  checkIfaceMeta<PluginxxConfigIface>("config", PLUGINXX_IFACE_CONFIG_VERSION,
                                      PLUGINXX_IFACE_CONFIG_MIN_VERSION,
                                      PLUGINXX_IFACE_CONFIG_BASELINE_SIZE);
  checkIfaceMeta<PluginxxCancelIface>("cancel", PLUGINXX_IFACE_CANCEL_VERSION,
                                      PLUGINXX_IFACE_CANCEL_MIN_VERSION,
                                      PLUGINXX_IFACE_CANCEL_BASELINE_SIZE);
  checkIfaceMeta<PluginxxJsonIface>("json", PLUGINXX_IFACE_JSON_VERSION,
                                    PLUGINXX_IFACE_JSON_MIN_VERSION,
                                    PLUGINXX_IFACE_JSON_BASELINE_SIZE);
  checkIfaceMeta<PluginxxLogIface>("log", PLUGINXX_IFACE_LOG_VERSION,
                                   PLUGINXX_IFACE_LOG_MIN_VERSION,
                                   PLUGINXX_IFACE_LOG_BASELINE_SIZE);
  checkIfaceMeta<PluginxxTasksIface>("tasks", PLUGINXX_IFACE_TASKS_VERSION,
                                     PLUGINXX_IFACE_TASKS_MIN_VERSION,
                                     PLUGINXX_IFACE_TASKS_BASELINE_SIZE);

  // 成员结束偏移: 偏移 + 成员大小
  const uint32_t logEnd =
      pluginxx::memberEndOffset<&PluginxxLogIface::log, PluginxxLogIface>();
  check(logEnd == offsetof(PluginxxLogIface, log) + sizeof(void *),
        "memberEndOffset = 成员偏移 + 成员大小");
  check(logEnd == sizeof(PluginxxLogIface), "log 表只有表头与这一个成员");

  // 追加成员判定: 表长不够时不能读那个成员
  check(!pluginxx::ifaceHas<&PluginxxLogIface::log, PluginxxLogIface>(nullptr),
        "ifaceHas: 空表判为没有该成员");
  PluginxxLogIface shortTable = headerOnly<PluginxxLogIface>();
  check(!pluginxx::ifaceHas<&PluginxxLogIface::log, PluginxxLogIface>(&shortTable),
        "ifaceHas: 表长不足判为没有该成员");
  PluginxxLogIface fullTable{};
  fullTable.version = PLUGINXX_IFACE_LOG_VERSION;
  fullTable.struct_size = sizeof(PluginxxLogIface);
  check(pluginxx::ifaceHas<&PluginxxLogIface::log, PluginxxLogIface>(&fullTable),
        "ifaceHas: 完整表判为有该成员");

  // 语义档位判定
  check(!pluginxx::ifaceHasVersion<PluginxxLogIface>(nullptr, 1),
        "ifaceHasVersion: 空表判为不满足");
  check(pluginxx::ifaceHasVersion<PluginxxLogIface>(&fullTable, 1),
        "ifaceHasVersion: 达到档位");
  check(!pluginxx::ifaceHasVersion<PluginxxLogIface>(&fullTable, 99),
        "ifaceHasVersion: 未达到档位");
}

/* ==================== 能力登记表 ==================== */

/// 声明一条应用能力（`impls: one` + `impl`）
pluginxx::CapabilityDecl appDeclOne(const std::string &id) {
  pluginxx::CapabilityDecl decl;
  decl.id = id;
  decl.impls = pluginxx::CapabilityImpls_e::One;
  decl.merge = pluginxx::CapabilityMerge_e::First;
  decl.phases = pluginxx::kCapabilityPhaseMaskImpl;
  return decl;
}

/// 一条实现行
pluginxx::CapabilityProvider row(const std::string &provider,
                                 pluginxx::CapabilityPhase_e phase,
                                 int32_t priority, int32_t tag) {
  pluginxx::CapabilityProvider r;
  r.provider = provider;
  r.ownerTag = "tag" + std::to_string(tag);
  r.phase = phase;
  r.priority = priority;
  r.start = &fakeStart;
  r.cancel = &fakeCancel;
  r.ctx = reinterpret_cast<void *>(static_cast<intptr_t>(tag));
  return r;
}

void testCapabilityRegistry() {
  std::printf("\n-- 能力登记表（多实现 + 阶段 + 覆盖）\n");
  pluginxx::CapabilityRegistry reg;

  check(!reg.has("demo.echo"), "未登记的能力 has = false");
  check(!reg.declares("demo.echo"), "未声明的能力 declares = false");
  check(reg.get("demo.echo") == nullptr, "未登记的能力 get = nullptr");
  check(reg.providerOf("demo.echo").empty(), "未登记的能力 providerOf 为空串");
  check(reg.names().empty(), "空登记表的 names 为空");

  int ctxA = 1;
  int ctxB = 2;
  check(reg.registerCapability("demo.echo", "plugin_a", &fakeStart, &fakeCancel,
                               &ctxA),
        "登记能力成功（未声明时自动声明单实现）");
  check(reg.has("demo.echo"), "登记后 has = true");
  const auto *entry = reg.get("demo.echo");
  check(entry != nullptr && entry->provider == "plugin_a",
        "登记后 provider 是登记方");
  check(entry != nullptr && entry->start == &fakeStart && entry->cancel == &fakeCancel,
        "登记后回调指针原样保存");
  check(entry != nullptr && entry->ctx == &ctxA, "登记后上下文原样保存");
  check(reg.providerOf("demo.echo") == "plugin_a", "providerOf 返回登记方");

  // 同一插件再次登记 = 同一行的替换（插件会被多个作用域各挂载一次，重复登记不能长出新行）
  check(reg.registerCapability("demo.echo", "plugin_a", &fakeStart, &fakeCancel,
                               &ctxB),
        "同一登记方重复登记成功");
  entry = reg.get("demo.echo");
  check(entry != nullptr && entry->ctx == &ctxB, "重复登记替换成新的上下文");
  check(reg.names().size() == 1, "重复登记不长出新条目");

  // 新语义：impls: one 的 impl 行后登记（含跨插件）整体覆盖
  check(reg.registerCapability("demo.echo", "plugin_b", &fakeStart, &fakeCancel,
                               &ctxB),
        "别的插件登记同名能力成功（覆盖是设计语义）");
  check(reg.providerOf("demo.echo") == "plugin_b", "覆盖后归属跟着新登记方");

  check(!reg.registerCapability("", "plugin_a"), "空能力名被拒");

  // before / after 是干预链：多个插件可以各登记一份
  pluginxx::CapabilityDecl chainDecl;
  chainDecl.id = "demo.chain";
  chainDecl.impls = pluginxx::CapabilityImpls_e::One;
  chainDecl.merge = pluginxx::CapabilityMerge_e::First;
  chainDecl.phases = pluginxx::kCapabilityPhaseMaskImpl |
                     pluginxx::kCapabilityPhaseMaskBefore |
                     pluginxx::kCapabilityPhaseMaskAfter;
  check(reg.declare(chainDecl), "声明带阶段链的能力");
  check(reg.addProvider("demo.chain", row("plugin_a", pluginxx::CapabilityPhase_e::Impl, 0, 1)),
        "登记 impl 行");
  check(reg.addProvider("demo.chain", row("plugin_a", pluginxx::CapabilityPhase_e::Before, 10, 2)),
        "plugin_a 登记 before 行");
  check(reg.addProvider("demo.chain", row("plugin_b", pluginxx::CapabilityPhase_e::Before, 5, 3)),
        "plugin_b 登记 before 行（不覆盖别人）");
  check(reg.addProvider("demo.chain", row("plugin_a", pluginxx::CapabilityPhase_e::After, 0, 4)),
        "plugin_a 登记 after 行");
  {
    const auto beforeRows =
        reg.rowsOf("demo.chain", pluginxx::CapabilityPhase_e::Before);
    check(beforeRows.size() == 2, "before 链共存两行");
    check(beforeRows.size() == 2 && beforeRows[0]->provider == "plugin_b" &&
              beforeRows[1]->provider == "plugin_a",
          "before 链按 priority 升序");
    check(reg.rowsOf("demo.chain", pluginxx::CapabilityPhase_e::Impl).size() == 1,
          "impl 只有一个槽位");
    check(reg.rowsOf("demo.chain", pluginxx::CapabilityPhase_e::After).size() == 1,
          "after 链一行");
  }
  // 同插件同阶段再登记 = 替换那一行（ownerTag 一并参与判定）
  check(reg.addProvider("demo.chain",
                        row("plugin_a", pluginxx::CapabilityPhase_e::Before, 10, 2)),
        "同插件同阶段重复登记成功");
  check(reg.rowsOf("demo.chain", pluginxx::CapabilityPhase_e::Before).size() == 2,
        "同插件同阶段重复登记不长出新行");

  // 阶段未开放：拒绝并写清原因
  {
    pluginxx::CapabilityDecl implOnly = appDeclOne("demo.implOnly");
    check(reg.declare(implOnly), "声明只开放 impl");
    std::string error;
    check(!reg.addProvider(
              "demo.implOnly",
              row("plugin_a", pluginxx::CapabilityPhase_e::After, 0, 9), &error),
          "未开放的阶段被拒");
    check(error.find("after") != std::string::npos, "拒绝原因写明阶段");
  }

  // 未声明就登记：拒绝
  {
    std::string error;
    check(!reg.addProvider("demo.undeclared",
                           row("plugin_a", pluginxx::CapabilityPhase_e::Impl, 0, 1),
                           &error),
          "未声明就登记实现被拒");
    check(error.find("not declared") != std::string::npos, "拒绝原因写明未声明");
  }

  // 声明校验
  {
    pluginxx::CapabilityRegistry r;
    pluginxx::CapabilityDecl empty;
    std::string error;
    check(!r.declare(empty, &error), "空 id 的声明被拒");

    pluginxx::CapabilityDecl noPhases;
    noPhases.id = "demo.none";
    noPhases.phases = 0;
    check(!r.declare(noPhases), "phases 为空的声明被拒");

    pluginxx::CapabilityDecl noImplMany;
    noImplMany.id = "demo.noImplMany";
    noImplMany.impls = pluginxx::CapabilityImpls_e::Many;
    noImplMany.phases = pluginxx::kCapabilityPhaseMaskBefore;
    check(!r.declare(noImplMany), "不开放 impl 却声明多实现被拒");

    pluginxx::CapabilityDecl oneVeto;
    oneVeto.id = "demo.oneVeto";
    oneVeto.impls = pluginxx::CapabilityImpls_e::One;
    oneVeto.merge = pluginxx::CapabilityMerge_e::Veto;
    check(!r.declare(oneVeto), "单实现配 veto 被拒");

    pluginxx::CapabilityDecl negTimeout;
    negTimeout.id = "demo.neg";
    negTimeout.timeoutMs = -1;
    check(!r.declare(negTimeout), "负超时被拒");
  }

  // 应用声明是权威来源：插件不能改
  {
    pluginxx::CapabilityRegistry r;
    pluginxx::CapabilityDecl appDecl = appDeclOne("demo.authority");
    appDecl.phases = pluginxx::kCapabilityPhaseMaskImpl |
                     pluginxx::kCapabilityPhaseMaskBefore;
    check(r.declare(appDecl), "应用声明成功");
    pluginxx::CapabilityDecl pluginDecl = appDeclOne("demo.authority");
    pluginDecl.declaredBy = "plugin_a";
    std::string error;
    check(!r.declare(pluginDecl, &error), "插件覆盖应用声明被拒");
    check(error.find("application") != std::string::npos, "拒绝原因写明应用声明");
  }

  // 插件自声明：卸载时连同声明一起消失；应用声明的保留空槽位
  {
    pluginxx::CapabilityRegistry r;
    pluginxx::CapabilityDecl own;
    own.id = "plugin_a.drive.list";
    own.impls = pluginxx::CapabilityImpls_e::Many;
    own.merge = pluginxx::CapabilityMerge_e::First;
    own.phases = pluginxx::kCapabilityPhaseMaskImpl;
    own.declaredBy = "plugin_a";
    check(r.declare(own), "插件自声明成功");
    check(r.addProvider("plugin_a.drive.list",
                        row("plugin_a", pluginxx::CapabilityPhase_e::Impl, 0, 1)),
          "插件登记自己的实现");
    check(r.removeProvider("plugin_a") == 1, "removeProvider 移除实现行");
    check(!r.declares("plugin_a.drive.list"),
          "插件声明的槽位在最后一行移除后删除");

    pluginxx::CapabilityDecl appDecl = appDeclOne("musicxx.app.only");
    check(r.declare(appDecl), "应用声明成功");
    check(r.removeProvider("plugin_a") == 0, "没有行的插件移除数量为 0");
    check(r.declares("musicxx.app.only"), "应用声明的槽位保留");
  }

  // 精确注销一行 / 旧入口注销整个插件
  {
    pluginxx::CapabilityRegistry r;
    pluginxx::CapabilityDecl decl = appDeclOne("demo.unreg");
    decl.phases = pluginxx::kCapabilityPhaseMaskImpl |
                  pluginxx::kCapabilityPhaseMaskBefore;
    check(r.declare(decl), "声明注销用例");
    check(r.addProvider("demo.unreg", row("plugin_a", pluginxx::CapabilityPhase_e::Impl, 0, 1)),
          "登记 impl");
    check(r.addProvider("demo.unreg", row("plugin_a", pluginxx::CapabilityPhase_e::Before, 0, 1)),
          "登记 before");
    check(r.unregisterProvider("demo.unreg", "plugin_a", "tag1",
                               pluginxx::CapabilityPhase_e::Before),
          "按 ownerTag + 阶段注销一行");
    check(!r.unregisterProvider("demo.unreg", "plugin_a", "tag1",
                                pluginxx::CapabilityPhase_e::Before),
          "重复注销同一行返回 false");
    check(r.has("demo.unreg"), "还有 impl 行时 has = true");
    check(r.unregisterCapability("demo.unreg", "plugin_a"), "旧入口注销整个插件");
    check(!r.has("demo.unreg"), "注销后 has = false");
    check(r.declares("demo.unreg"), "应用声明保留");
    check(!r.unregisterCapability("demo.unreg", "plugin_a"), "重复注销返回 false");
    check(!r.unregisterCapability("demo.unknown", "plugin_a"),
          "注销不存在的条目返回 false");
  }

  // 自省
  {
    pluginxx::CapabilityRegistry r;
    pluginxx::CapabilityDecl decl = appDeclOne("demo.info");
    decl.phases = pluginxx::kCapabilityPhaseMaskImpl |
                  pluginxx::kCapabilityPhaseMaskAfter;
    decl.timeoutMs = 500;
    check(r.declare(decl), "声明自省用例");
    check(r.addProvider("demo.info", row("plugin_a", pluginxx::CapabilityPhase_e::Impl, 0, 1)),
          "登记自省实现");
    const std::string info = r.infoJson("demo.info");
    check(info.find("\"impls\":\"one\"") != std::string::npos, "info 含 impls");
    check(info.find("\"phases\":[\"impl\",\"after\"]") != std::string::npos,
          "info 含 phases");
    check(info.find("\"timeoutMs\":500") != std::string::npos, "info 含 timeoutMs");
    check(info.find("\"provider\":\"plugin_a\"") != std::string::npos,
          "info 含实现行");
    const std::string list = r.listJson();
    check(list.find("demo.info") != std::string::npos, "list 含能力 id");
    check(r.infoJson("demo.unknown").empty(), "未声明能力的 info 为空串");
  }

  // 事件订阅句柄的撤销入口对空指针是空操作 (插件传过期/空句柄不崩)
  pluginxx::unsubscribePluginBroadcast(nullptr);
  check(true, "撤销空的广播订阅句柄不崩");
}

/* ==================== 能力链工具 (补丁合并 / 阶段顺序) ==================== */

void testCapabilityChainHelpers() {
  std::printf("\n-- 能力链工具\n");
  using pluginxx::detail::mergeJsonPatch;
  using pluginxx::detail::parseJson;

  auto base  = parseJson(R"({"a":1,"b":{"c":2,"d":3},"e":[1,2]})");
  auto patch = parseJson(R"({"b":{"c":20},"e":[9],"f":true})");
  check(base.ok && patch.ok, "解析对象与补丁");
  auto target = base.value;
  mergeJsonPatch(target, patch.value);
  check(target["a"].is_number() && target["a"].get<int>() == 1,
        "补丁不动没提到的键");
  check(target["b"]["c"].get<int>() == 20, "嵌套对象逐键合并");
  check(target["b"]["d"].get<int>() == 3, "嵌套对象保留没提到的子键");
  check(target["e"].is_array() && target["e"].size() == 1, "数组整体替换");
  check(target["f"].is_bool(), "新键加入");

  auto scalar = parseJson("42");
  auto object = parseJson(R"({"a":1})");
  mergeJsonPatch(object.value, scalar.value);
  check(object.value.is_number(), "非对象补丁直接替换目标");

  check(!parseJson("").ok, "空串解析失败");
  check(!parseJson("{bad").ok, "坏 JSON 解析失败");

  pluginxx::CapabilityRegistry reg;
  pluginxx::CapabilityDecl   decl;
  decl.id     = "demo.chain";
  decl.impls  = pluginxx::CapabilityImpls_e::Many;
  decl.merge  = pluginxx::CapabilityMerge_e::First;
  decl.phases = pluginxx::kCapabilityPhaseMaskBefore | pluginxx::kCapabilityPhaseMaskImpl |
                pluginxx::kCapabilityPhaseMaskAfter;
  check(reg.declare(decl), "声明三阶段能力");
  check(reg.addProvider("demo.chain",
                        row("plugin_a", pluginxx::CapabilityPhase_e::Before, 10, 1)),
        "登记 before (priority 10)");
  check(reg.addProvider("demo.chain",
                        row("plugin_b", pluginxx::CapabilityPhase_e::Before, 5, 2)),
        "登记 before (priority 5)");
  check(reg.addProvider("demo.chain",
                        row("plugin_a", pluginxx::CapabilityPhase_e::Impl, 0, 3)),
        "登记 impl");
  check(reg.addProvider("demo.chain",
                        row("plugin_a", pluginxx::CapabilityPhase_e::After, 10, 4)),
        "登记 after (priority 10)");
  check(reg.addProvider("demo.chain",
                        row("plugin_b", pluginxx::CapabilityPhase_e::After, 5, 5)),
        "登记 after (priority 5)");

  std::vector<pluginxx::CapabilityProvider> out;
  pluginxx::detail::appendStage(out, reg, "demo.chain",
                                pluginxx::CapabilityPhase_e::Before, false);
  check(out.size() == 2 && out[0].priority == 5 && out[1].priority == 10,
        "before 阶段按 priority 升序");
  out.clear();
  pluginxx::detail::appendStage(out, reg, "demo.chain",
                                pluginxx::CapabilityPhase_e::After, true);
  check(out.size() == 2 && out[0].priority == 10 && out[1].priority == 5,
        "after 阶段倒序");
  out.clear();
  pluginxx::detail::appendStage(out, reg, "demo.chain",
                                pluginxx::CapabilityPhase_e::Impl, false);
  check(out.size() == 1, "impl 阶段只有一行");
}

/* ==================== 广播注册表 ==================== */

void testBroadcastRegistry() {
  std::printf("\n-- 广播注册表（缓存 / 写权限 / 订阅即推）\n");
  pluginxx::BroadcastRegistry reg;

  check(!reg.declares("musicxx.state.player"), "未声明的频道 declares = false");
  check(reg.ids().empty(), "空登记表没有频道");
  {
    std::string error;
    check(!reg.publish("musicxx.state.player", "{}", "",
                       &error),
          "未声明的频道发布被拒");
    check(error.find("not declared") != std::string::npos, "拒绝原因写明未声明");
    std::string value;
    check(!reg.read("musicxx.state.player", &value), "未声明的频道读不到值");
    check(reg.subscribe("musicxx.state.player", "plugin_a",
                        [](std::string_view) {}, &error) == 0,
          "未声明的频道订阅失败");
  }

  pluginxx::BroadcastDecl state;
  state.id = "musicxx.state.player";
  state.cache = pluginxx::BroadcastCache_e::Latest;
  state.writable = pluginxx::BroadcastWritable_e::Owner;
  state.pushOnSubscribe = true;
  state.coalesce = true;
  check(reg.declare(state), "声明状态镜像频道（应用属主）");

  std::vector<std::string> seen;
  const uint64_t token = reg.subscribe("musicxx.state.player", "plugin_a",
                                      [&seen](std::string_view v) {
                                        seen.emplace_back(v);
                                      });
  check(token != 0, "订阅成功返回令牌");
  check(seen.empty(), "还没有值时订阅即推不回调");

  check(reg.publish("musicxx.state.player", R"({"playing":true})", ""),
        "应用写状态镜像成功");
  check(seen.size() == 1 && seen[0] == R"({"playing":true})",
        "订阅方收到发布值");
  check(reg.publish("musicxx.state.player", R"({"playing":false})", ""),
        "应用再次写状态镜像成功");
  {
    std::string value;
    check(reg.read("musicxx.state.player", &value) &&
              value == R"({"playing":false})",
          "读当前值只有最后一份");
    check(seen.size() == 2 && seen.back() == R"({"playing":false})",
          "订阅方收到最后一次");
  }
  {
    std::string error;
    check(!reg.publish("musicxx.state.player", "{}", "plugin_a", &error),
          "插件写属主频道被拒");
    check(error.find("owned by") != std::string::npos, "拒绝原因写明属主");
  }
  // 订阅即推：新订阅立刻拿到当前值
  {
    std::vector<std::string> pushed;
    const uint64_t t2 = reg.subscribe("musicxx.state.player", "plugin_b",
                                     [&pushed](std::string_view v) {
                                       pushed.emplace_back(v);
                                     });
    check(t2 != 0 && pushed.size() == 1 &&
              pushed[0] == R"({"playing":false})",
          "订阅即推拿到当前值");
    check(reg.unsubscribe("musicxx.state.player", t2), "订阅即推后退订成功");
  }

  // 变量频道：任何人都能写、订阅即推、不合并
  pluginxx::BroadcastDecl var;
  var.id = "musicxx.var.player.volume";
  var.cache = pluginxx::BroadcastCache_e::Latest;
  var.writable = pluginxx::BroadcastWritable_e::Anyone;
  var.pushOnSubscribe = true;
  var.coalesce = false;
  check(reg.declare(var), "声明变量频道（任何人可写）");
  {
    std::vector<std::string> values;
    const uint64_t t = reg.subscribe("musicxx.var.player.volume", "plugin_a",
                                    [&values](std::string_view v) {
                                      values.emplace_back(v);
                                    });
    check(t != 0, "变量频道订阅成功");
    check(reg.publish("musicxx.var.player.volume", "10", "plugin_a"),
          "插件写变量成功");
    check(reg.publish("musicxx.var.player.volume", "20", "plugin_b"),
          "另一个插件写同一个变量成功");
    check(reg.publish("musicxx.var.player.volume", "30", ""),
          "应用写变量成功");
    check(values.size() == 3 && values.back() == "30",
          "coalesce: false 时每次发布都投递");
    std::string value;
    check(reg.read("musicxx.var.player.volume", &value) && value == "30",
          "变量读当前值");
  }

  // 事件频道：没有当前值，订阅收到每一条
  pluginxx::BroadcastDecl event;
  event.id = "plugin_a.net";
  event.cache = pluginxx::BroadcastCache_e::None;
  event.writable = pluginxx::BroadcastWritable_e::Anyone;
  event.owner = "plugin_a";
  check(reg.declare(event), "声明事件频道（插件属主）");
  {
    std::vector<std::string> events;
    const uint64_t t = reg.subscribe("plugin_a.net", "plugin_b",
                                     [&events](std::string_view v) {
                                       events.emplace_back(v);
                                     });
    check(t != 0, "事件频道订阅成功");
    check(reg.publish("plugin_a.net", R"({"n":1})", "plugin_a"), "发布事件 1");
    check(reg.publish("plugin_a.net", R"({"n":2})", "plugin_a"), "发布事件 2");
    check(events.size() == 2, "cache: none 时两条都收到");
    std::string value;
    check(!reg.read("plugin_a.net", &value), "cache: none 读不到当前值");
    // 订阅的回调捕获了块内变量, 离开块之前必须退订 (真实调用方由实例卸载路径撤销)
    check(reg.unsubscribe("plugin_a.net", t), "事件频道退订成功");
  }

  // 系统频道：插件不能写
  pluginxx::BroadcastDecl system;
  system.id = "musicxx.system.debug";
  system.scope = pluginxx::BroadcastScope_e::System;
  system.cache = pluginxx::BroadcastCache_e::Latest;
  check(reg.declare(system), "声明系统频道");
  {
    std::string error;
    check(!reg.publish("musicxx.system.debug", "{}", "plugin_a", &error),
          "插件写系统频道被拒");
    check(error.find("system") != std::string::npos, "拒绝原因写明系统频道");
    check(reg.publish("musicxx.system.debug", "{}", ""), "应用写系统频道成功");
  }
  {
    pluginxx::BroadcastDecl bad = system;
    bad.id = "musicxx.system.bad";
    bad.writable = pluginxx::BroadcastWritable_e::Anyone;
    check(!reg.declare(bad), "系统频道声明 anyone 可写被拒");
    pluginxx::BroadcastDecl badPush = event;
    badPush.id = "plugin_a.badPush";
    badPush.pushOnSubscribe = true;
    check(!reg.declare(badPush), "cache: none 配订阅即推被拒");
    pluginxx::BroadcastDecl empty;
    check(!reg.declare(empty), "空 id 的广播声明被拒");
  }

  // 应用声明不能被插件覆盖
  {
    pluginxx::BroadcastDecl pluginDecl = state;
    pluginDecl.owner = "plugin_a";
    std::string error;
    check(!reg.declare(pluginDecl, &error), "插件覆盖应用声明被拒");
    check(error.find("application") != std::string::npos, "拒绝原因写明应用声明");
  }

  // 订阅回调抛异常不影响其它订阅者与发布结果
  {
    pluginxx::BroadcastDecl throwing = event;
    throwing.id = "plugin_a.throwing";
    check(reg.declare(throwing), "声明异常回调用例频道");
    int goodCalls = 0;
    check(reg.subscribe("plugin_a.throwing", "plugin_b",
                        [](std::string_view) {
                          throw std::runtime_error("boom");
                        }) != 0,
          "登记抛异常的订阅");
    check(reg.subscribe("plugin_a.throwing", "plugin_c",
                        [&goodCalls](std::string_view) { ++goodCalls; }) != 0,
          "登记正常订阅");
    check(reg.publish("plugin_a.throwing", "{}", "plugin_a"),
          "有订阅抛异常时发布仍成功");
    check(goodCalls == 1, "抛异常的订阅不影响后面的订阅");
  }

  // 退订
  {
    int calls = 0;
    const uint64_t t = reg.subscribe("plugin_a.net", "plugin_b",
                                    [&calls](std::string_view) { ++calls; });
    check(t != 0, "登记待退订的订阅");
    check(reg.unsubscribe("plugin_a.net", t), "退订成功");
    check(!reg.unsubscribe("plugin_a.net", t), "重复退订返回 false");
    check(reg.publish("plugin_a.net", "{}", "plugin_a"), "退订后发布成功");
    check(calls == 0, "退订后不再收到");

    check(reg.subscribe("musicxx.var.player.volume", "plugin_d",
                        [](std::string_view) {}) != 0,
          "登记按属主退订的订阅");
    check(reg.removeSubscriber("plugin_d") >= 1, "按属主批量退订");
    check(reg.removeSubscriber("plugin_d") == 0, "重复按属主退订数量为 0");
    check(reg.removeChannelsOwnedBy("plugin_a") == 2,
          "按属主删除插件频道（net / throwing）");
    check(!reg.declares("plugin_a.net"), "插件频道删除后查不到");
    check(reg.declares("musicxx.state.player"), "应用频道不受影响");
  }

  // 自省
  {
    const std::string info = reg.infoJson("musicxx.state.player");
    check(info.find("\"cache\":\"latest\"") != std::string::npos, "info 含 cache");
    check(info.find("\"writable\":\"owner\"") != std::string::npos,
          "info 含 writable");
    check(info.find("\"pushOnSubscribe\":true") != std::string::npos,
          "info 含 pushOnSubscribe");
    check(info.find("\"hasValue\":true") != std::string::npos, "info 含 hasValue");
    const std::string list = reg.listJson();
    check(list.find("musicxx.state.player") != std::string::npos,
          "list 含频道 id");
    check(reg.infoJson("musicxx.unknown.channel").empty(),
          "未声明频道的 info 为空串");
  }
}

} // namespace

/* ==================== 能力与广播声明表（生成物） ==================== */

namespace {

/// 声明的 id 必须落在两个命名空间之一
bool cppIdNamespaced(const char *id) {
  if (!id) {
    return false;
  }
  return std::strncmp(id, "musicxx.", 8) == 0 ||
         std::strncmp(id, "plugin.", 7) == 0;
}

/// 一条能力声明的编码自洽（与 tools/capabilities.def.json 的校验规则一致）
bool cppCapabilityConsistent(const MusicxxCapabilityDecl &decl) {
  const bool openForImpl =
      (decl.phase_mask & MUSICXX_CAPABILITY_PHASE_MASK_IMPL) != 0;
  const uint32_t usages = decl.usage_mask & (MUSICXX_CAPABILITY_USAGE_POINT |
                                             MUSICXX_CAPABILITY_USAGE_COMMAND |
                                             MUSICXX_CAPABILITY_USAGE_SERVICE);
  if (usages == 0) {
    return false;
  }
  // 不开放实现时只有一个槽位、没有合并策略
  if (!openForImpl &&
      (decl.impls != MUSICXX_CAPABILITY_IMPLS_ONE || decl.merge != 0)) {
    return false;
  }
  // 多实现共存时必须写合并策略
  if (openForImpl && decl.impls == MUSICXX_CAPABILITY_IMPLS_MANY &&
      decl.merge != MUSICXX_CAPABILITY_MERGE_FIRST &&
      decl.merge != MUSICXX_CAPABILITY_MERGE_VETO) {
    return false;
  }
  return true;
}

void testCapabilityDeclarations() {
  std::printf("\n-- 能力与广播声明表\n");
  check(musicxxCapabilityDeclCount > 0, "能力声明表非空");
  check(musicxxBroadcastDeclCount > 0, "广播声明表非空");

  bool idsOk = true;
  bool idsUnique = true;
  bool consistent = true;
  for (size_t i = 0; i < musicxxCapabilityDeclCount; ++i) {
    const MusicxxCapabilityDecl &decl = musicxxCapabilityDecls[i];
    if (!cppIdNamespaced(decl.id)) {
      idsOk = false;
    }
    if (!cppCapabilityConsistent(decl)) {
      consistent = false;
    }
    for (size_t j = i + 1; j < musicxxCapabilityDeclCount; ++j) {
      if (std::strcmp(decl.id, musicxxCapabilityDecls[j].id) == 0) {
        idsUnique = false;
      }
    }
  }
  check(idsOk, "每条能力 id 都在 musicxx.* / plugin.* 命名空间里");
  check(idsUnique, "能力 id 不重复");
  check(consistent, "每条能力声明的 impls / merge / phases / usage 自洽");

  bool broadcastIdsOk = true;
  bool broadcastIdsUnique = true;
  bool broadcastConsistent = true;
  for (size_t i = 0; i < musicxxBroadcastDeclCount; ++i) {
    const MusicxxBroadcastDecl &decl = musicxxBroadcastDecls[i];
    if (!cppIdNamespaced(decl.id)) {
      broadcastIdsOk = false;
    }
    // 没有当前值就不能"订阅即推值"
    if (decl.push_on_subscribe != 0 &&
        decl.cache != MUSICXX_BROADCAST_CACHE_LATEST) {
      broadcastConsistent = false;
    }
    if (decl.cache != MUSICXX_BROADCAST_CACHE_NONE &&
        decl.cache != MUSICXX_BROADCAST_CACHE_LATEST) {
      broadcastConsistent = false;
    }
    for (size_t j = i + 1; j < musicxxBroadcastDeclCount; ++j) {
      if (std::strcmp(decl.id, musicxxBroadcastDecls[j].id) == 0) {
        broadcastIdsUnique = false;
      }
    }
  }
  check(broadcastIdsOk, "每条广播 id 都在 musicxx.* / plugin.* 命名空间里");
  check(broadcastIdsUnique, "广播 id 不重复");
  check(broadcastConsistent, "每条广播声明的 cache / pushOnSubscribe 自洽");

  // 具体声明：换表（阶段 P1）之后这些字段是插件框架判阶段的依据，先钉住几个代表
  const MusicxxCapabilityDecl *seek =
      musicxxCapabilityDeclOf("musicxx.player.seek");
  check(seek != nullptr, "查得到 musicxx.player.seek");
  check(seek && seek->impls == MUSICXX_CAPABILITY_IMPLS_MANY &&
            seek->merge == MUSICXX_CAPABILITY_MERGE_VETO &&
            seek->timeout_ms == 500,
        "seek：多实现 + veto + 500 ms");
  check(seek && (seek->phase_mask & MUSICXX_CAPABILITY_PHASE_MASK_IMPL) != 0 &&
            (seek->phase_mask & MUSICXX_CAPABILITY_PHASE_MASK_AFTER) != 0,
        "seek：开放 impl 与 after");
  check(seek && (seek->usage_mask & MUSICXX_CAPABILITY_USAGE_COMMAND) != 0,
        "seek：也能当命令调");

  const MusicxxCapabilityDecl *lyricCache =
      musicxxCapabilityDeclOf("musicxx.lyric.load.cache");
  check(lyricCache && lyricCache->timeout_ms == 8000 &&
            (lyricCache->phase_mask & MUSICXX_CAPABILITY_PHASE_MASK_BEFORE) != 0,
        "歌词缓存阶段：8 s、开放 before");

  const MusicxxCapabilityDecl *featureCall =
      musicxxCapabilityDeclOf("musicxx.feature.point.call");
  check(featureCall != nullptr &&
            (featureCall->expose_mask & MUSICXX_CAPABILITY_EXPOSE_CLI) != 0,
        "通用功能点通道：对外暴露命令行");

  check(musicxxCapabilityDeclOf("musicxx.unknown.point") == nullptr,
        "未知能力返回 NULL");
  check(musicxxCapabilityDeclOf(nullptr) == nullptr, "空 id 返回 NULL");

  const MusicxxBroadcastDecl *statePlayer =
      musicxxBroadcastDeclOf("musicxx.state.player");
  check(statePlayer && statePlayer->cache == MUSICXX_BROADCAST_CACHE_LATEST &&
            statePlayer->push_on_subscribe == 1,
        "状态镜像：快照 + 订阅即推");
  const MusicxxBroadcastDecl *songChanged =
      musicxxBroadcastDeclOf("musicxx.song.changed");
  check(songChanged && songChanged->cache == MUSICXX_BROADCAST_CACHE_NONE &&
            songChanged->push_on_subscribe == 0,
        "业务事件：没有当前值");
  check(musicxxBroadcastDeclOf("musicxx.unknown.channel") == nullptr,
        "未知广播返回 NULL");

  // 同一个 id 可以既是广播（事件）又是能力（命令）：两张表各自寻址
  check(musicxxBroadcastDeclOf("musicxx.app.foreground") != nullptr &&
            musicxxCapabilityDeclOf("musicxx.app.foreground") != nullptr,
        "app.foreground：广播与命令同 id");
}

} // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("cxx_pluginxx native test (api_version=%d, min_api_version=%d)\n",
              PLUGINXX_API_VERSION, PLUGINXX_MIN_API_VERSION);

  testScopeRef();
  testIfaceMeta();
  testCapabilityRegistry();
  testCapabilityChainHelpers();
  testBroadcastRegistry();
  testCapabilityDeclarations();

  std::printf("\nchecks=%d failed=%d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
