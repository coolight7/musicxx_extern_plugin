/// cxx_pluginxx (插件框架共用库) 原生测试 —— 不依赖 Dart, 也不启动 musicxx 插件框架
///
/// 定位: 共用库被多个主程序使用, 它的执行模型 (能力登记/阶段链/合并、广播缓存与写权限)
/// 换实现时必须有自己的一套测试, 否则只能靠主程序的原生测试间接验证。
///
/// 覆盖:
/// - 接口表元信息 (版本门槛 / 基线字节数 / 追加成员判定 / 语义档位判定);
/// - 作用域引用校验;
/// - 能力登记表 (登记 / 重复 / 归属 / 注销 / 列表);
/// - 事件订阅句柄的幂等撤销入口。
///
/// 运行: musicxx_pluginxx_test (无参数; 由 tools/build_native.ps1 -RunTests 一起跑)

#include "pluginxx/api/abi.h"
#include "pluginxx/api/iface_meta.h"
#include "pluginxx/host/capability_registry.h"
// 事件订阅句柄的撤销入口是内联函数, 实现体在 runtime/instance_base.h
#include "pluginxx/runtime/instance_base.h"

#include <cstddef>
#include <cstdio>
#include <string>
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
  checkIfaceMeta<PluginxxEventsIface>("events", PLUGINXX_IFACE_EVENTS_VERSION,
                                      PLUGINXX_IFACE_EVENTS_MIN_VERSION,
                                      PLUGINXX_IFACE_EVENTS_BASELINE_SIZE);
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

void testCapabilityRegistry() {
  std::printf("\n-- 能力登记表\n");
  pluginxx::CapabilityRegistry reg;

  check(!reg.has("demo.echo"), "未登记的能力 has = false");
  check(reg.get("demo.echo") == nullptr, "未登记的能力 get = nullptr");
  check(reg.providerOf("demo.echo").empty(), "未登记的能力 providerOf 为空串");
  check(reg.names().empty(), "空登记表的 names 为空");

  int ctxA = 1;
  int ctxB = 2;
  check(reg.registerCapability("demo.echo", "plugin_a", &fakeStart, &fakeCancel,
                               &ctxA),
        "登记能力成功");
  check(reg.has("demo.echo"), "登记后 has = true");
  const auto *entry = reg.get("demo.echo");
  check(entry != nullptr && entry->provider == "plugin_a",
        "登记后 provider 是登记方");
  check(entry != nullptr && entry->start == &fakeStart && entry->cancel == &fakeCancel,
        "登记后回调指针原样保存");
  check(entry != nullptr && entry->ctx == &ctxA, "登记后上下文原样保存");
  check(reg.providerOf("demo.echo") == "plugin_a", "providerOf 返回登记方");

  // 同一插件重复登记是幂等的 (插件会被多个作用域各挂载一次), 保留首次的回调与上下文
  check(reg.registerCapability("demo.echo", "plugin_a", nullptr, nullptr, &ctxB),
        "同一登记方重复登记是幂等成功");
  entry = reg.get("demo.echo");
  check(entry != nullptr && entry->ctx == &ctxA,
        "重复登记保留首次的上下文");

  // 别的插件抢同一个名字: 拒绝 (旧语义; 新语义见方案 §4.5 的覆盖)
  check(!reg.registerCapability("demo.echo", "plugin_b", &fakeStart, &fakeCancel,
                                &ctxB),
        "别的插件登记同名能力被拒");
  check(reg.providerOf("demo.echo") == "plugin_a", "被拒后归属不变");

  check(!reg.registerCapability("", "plugin_a"), "空能力名被拒");

  check(reg.registerCapability("demo.two", "plugin_b"), "登记第二条能力");
  {
    const std::vector<std::string> names = reg.names();
    check(names.size() == 2, "names 返回两条");
    check(names.size() == 2 && names[0] == "demo.echo" && names[1] == "demo.two",
          "names 按字典序");
  }

  // 注销只能由登记方发起
  check(!reg.unregisterCapability("demo.echo", "plugin_b"),
        "非登记方注销被拒");
  check(reg.has("demo.echo"), "注销被拒后条目仍在");
  check(reg.unregisterCapability("demo.echo", "plugin_a"), "登记方注销成功");
  check(!reg.has("demo.echo"), "注销后 has = false");
  check(!reg.unregisterCapability("demo.echo", "plugin_a"),
        "重复注销返回 false");
  check(!reg.unregisterCapability("demo.unknown", "plugin_a"),
        "注销不存在的条目返回 false");

  // 事件订阅句柄的撤销入口对空指针是空操作 (插件传过期/空句柄不崩)
  pluginxx::unsubscribePluginSubscription(nullptr);
  check(true, "撤销空的事件订阅句柄不崩");
}

} // namespace

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("cxx_pluginxx native test (api_version=%d, min_api_version=%d)\n",
              PLUGINXX_API_VERSION, PLUGINXX_MIN_API_VERSION);

  testScopeRef();
  testIfaceMeta();
  testCapabilityRegistry();

  std::printf("\nchecks=%d failed=%d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
