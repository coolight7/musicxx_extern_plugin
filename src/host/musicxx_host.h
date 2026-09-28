/// musicxx 外部插件原生宿主 (musicxx_extern_plugin host)
///
/// 结构:
/// - [MusicxxHostInstance]: 继承内核 `pluginxx::PluginInstanceBase`, 提供
/// `musicxx_plugin_destroy`;
/// - [MusicxxHostManager]: 继承内核 `PluginHostLifecycle`
/// (装载/启停/禁用启用/卸载/级联依赖)
///   与 `DomainHooks` (通用表取数), 并实现 musicxx 领域表 (`musicxx.hooks` /
///   `musicxx.host`);
/// - [HostContext]: 宿主线程 (asio::io_context + 1 条线程)
/// 与工作线程池的所有者。
///
/// 线程模型: 全部动态库插件代码在**唯一**宿主线程上顺序执行; Dart
/// 线程只在 同步 FFI 调用期间被借用, 并有等待上界; 插件不得阻塞宿主线程
/// (慢操作走 offload)。
#pragma once

#include "musicxx_extern_plugin_api.h"

#include "musicxx/plugin/api/plugin_api.h"
#include "pluginxx/api/entry.h"
#include "pluginxx/host/lifecycle.h"
#include "pluginxx/host/tables_impl.h"

#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/thread_pool.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace musicxx {
namespace extern_plugin {

/// JS 运行时 (定义在 js/js_engine.h)
class JsEngine;

/// `musicxx.ui` 表的 C ABI 实现 (定义在 musicxx_host_ui.cpp; 供 query_interface
/// 分发)
const void *uiIfaceForQuery();

/// `musicxx.vars` 表的 C ABI 实现 (定义在 musicxx_host_vars.cpp; 供
/// query_interface 分发)
const void *varsIfaceForQuery();

/* ==================== 有界等待槽 ==================== */

/// 宿主线程完成工作后唤醒等待方 (调用方线程等待有上界)
template <typename T> struct WaitSlot {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  T value{};

  void set(T v) {
    {
      std::lock_guard<std::mutex> lock{mutex};
      value = std::move(v);
      done = true;
    }
    cv.notify_all();
  }

  /// 等待完成; 超时返回 false (槽由投递的闭包持有, 超时后仍可安全写入)
  bool wait(uint32_t timeoutMs, T &out) {
    std::unique_lock<std::mutex> lock{mutex};
    if (!cv.wait_for(lock, std::chrono::milliseconds{timeoutMs},
                     [this] { return done; })) {
      return false;
    }
    out = value;
    return true;
  }
};

/* ==================== 宿主内运行时接驳口 ==================== */

/// 宿主内运行时 (JS 引擎) 的"任意线程动作请求"接驳口
///
/// 背景 (这样才不会出现互相等待): JS 代码只在共享 JS 线程上执行,
/// 而宿主 线程派发裁决型钩子时会**等待 JS 处理器**。因此 JS
/// 侧发起的动作请求绝不能"投递到 宿主线程再等待" —— 那会和"宿主线程正等
/// JS"互锁。JS 引擎自己登记还没回复的请求 (任意线程 可读写的表), 只把事件推给 Dart;
/// Dart 的回复经 [MusicxxHostManager::actionRespond] 转交到这里。
class InternalActionRelay {
public:
  virtual ~InternalActionRelay() = default;

  /// 回复一条本接驳口登记的请求 (**任意线程**)
  /// - 返回 false 表示该 requestId 不属于本接驳口 (调用方继续按"未找到"处理)
  virtual bool respondAction(int64_t requestId, int32_t status,
                             const std::string &resultJson) = 0;

  /// 取消某实例的全部未完成请求 (**任意线程**; 实例卸载/禁用/宿主关闭时统一取消)
  virtual void cancelActionsOf(const std::string &instanceName) = 0;
};

/* ==================== 插件实例 ==================== */

/// 宿主侧插件实例 (元信息/生命周期入口由内核框架补齐)
///
/// 前置声明: 实例持有管理器弱引用 (内核 vtable 入口据此解析实例 → 管理器)
class MusicxxHostManager;

class MusicxxHostInstance : public pluginxx::PluginInstanceBase {
public:
  explicit MusicxxHostInstance(std::string instanceName)
      : pluginxx::PluginInstanceBase(std::move(instanceName)) {}

  /// 本宿主插件的 destroy 入口符号名 (与 `MUSICXX_PLUGIN_SYMBOL_DESTROY` 一致)
  const char *pluginDestroySymbol() const noexcept override {
    return MUSICXX_PLUGIN_SYMBOL_DESTROY;
  }

  std::string_view logTag() const noexcept override { return "[musicxx_ext] "; }

  /// 插件形态: native | js | builtin
  std::string kind = "native";

  /// 清单声明的接口段 (内核生命周期框架会写入; 本宿主 v1 不做接口协商)
  pluginxx::PluginManifestInterfaces interfaces;

  /// 管理器弱引用 (内核 vtable 入口据此解析"实例 → 管理器"; 由 createInstance
  /// 设置)
  std::weak_ptr<MusicxxHostManager> manager;

  /// 配置文件路径 (config.json; 宿主推导)
  std::string configPath;

  /// 资源与阶段耗时统计 (只观测不限制)
  struct PhaseMs {
    int64_t load = 0;
    int64_t create = 0;
    int64_t start = 0;
    int64_t stop = 0;
    int64_t destroy = 0;
  } phaseMs;

  std::atomic<int64_t> hookCalls{0};
  std::atomic<int64_t> hookTimeouts{0};
  std::atomic<int64_t> actionRequests{0};
  std::atomic<int64_t> eventsPublished{0};
  std::atomic<int64_t> errors{0};
  std::atomic<int64_t> selfReportedBytes{0};

  /* ---------- 事件发布速率 (宿主线程维护; 统计只观测不限制) ---------- */

  /// 装载完成的 steady 毫秒 (平均速率用)
  int64_t loadedAtMs = 0;
  /// 当前速率窗口起点 (steady 毫秒)
  int64_t eventsWindowStartMs = 0;
  /// 当前窗口内发布的事件数
  int64_t eventsWindowCount = 0;
  /// 最近一个完整窗口 (≥1 秒) 的速率 (空闲时为 0)
  int64_t eventsPerSec = 0;
};

class MusicxxHostManager;

/* ==================== 宿主上下文 (线程与线程池的所有者) ====================
 */

/// 宿主上下文: 保证 io_context 先于管理器存在 (管理器构造函数需要它的 executor)
struct HostContext {
  asio::io_context io;
  asio::executor_work_guard<asio::io_context::executor_type> guard;
  std::unique_ptr<asio::thread_pool> workers;
  std::thread thread;
  std::shared_ptr<MusicxxHostManager> manager;

  HostContext();
  ~HostContext();
};

/* ==================== 宿主管理器 ==================== */

class MusicxxHostManager final
    : public pluginxx::PluginHostLifecycle<MusicxxHostInstance>,
      public pluginxx::DomainHooks,
      public std::enable_shared_from_this<MusicxxHostManager> {
public:
  using Base = pluginxx::PluginHostLifecycle<MusicxxHostInstance>;
  using InstancePtr = std::shared_ptr<MusicxxHostInstance>;

  /// 创建管理器 (不启动线程; HostContext 负责 io_context 生命周期)
  static std::shared_ptr<MusicxxHostManager>
  create(HostContext &ctx, const MusicxxExternPluginHostConfig &cfg,
         std::string &err);

  ~MusicxxHostManager() override;

  /// 启动宿主线程 + 装载插件 (幂等)
  int32_t start(std::string &err);

  /// 同步停止 (幂等): 卸载全部插件 → 停线程池 → 停 io 线程
  int32_t stop(uint32_t timeoutMs, std::string &err);

  bool running() const { return running_.load(std::memory_order_acquire); }

  /* ---------- 事件 (原生 → Dart) ---------- */

  void setWake(MusicxxExternPluginWakeFn fn, void *userData);

  /// 批量取事件 (JSON 数组); 无事件返回 0 且 outJson 为空
  int32_t pollEvents(int32_t maxCount, std::string &outJson);

  /// 入队事件 (任意线程安全; 队列有界, 溢出丢最旧并补发 musicxx.host.error)
  void pushEvent(const std::string &type, const std::string &plugin,
                 const std::string &payloadJson);

  void pushHostError(const std::string &code, const std::string &message);

  /* ---------- 插件管理 ---------- */

  int32_t scanPlugins(std::string &outJson, std::string &err);
  int32_t pluginListJson(std::string &outJson);
  int32_t loadPlugin(const std::string &idOrPath,
                     const std::string &optionsJson, bool sync,
                     uint32_t timeoutMs, std::string &err);
  int32_t enablePlugin(const std::string &id, std::string &err);
  int32_t disablePlugin(const std::string &id, std::string &err);
  int32_t unloadPlugin(const std::string &id, uint32_t timeoutMs,
                       std::string &err);
  int32_t setPluginArgs(const std::string &id, const std::string &argsJson,
                        std::string &err);
  int32_t pluginConfigPath(const std::string &id, std::string &out,
                           std::string &err);

  /// 调用插件能力 (**Dart → 插件**; 调用线程等待, 完成回调在宿主线程)
  ///
  /// - `method` 为能力全名 (`plugin.<pluginId>.<名>`) 或去掉前缀的短名
  /// (宿主自动补齐);
  /// - 能力必须先由目标插件在自己的 `start` 事务里声明 (`pluginxx.capabilities`
  /// 表),
  ///   且归属校验通过 (禁止调用他人命名空间);
  /// - 超时按 ERR_TIMEOUT 返回, 插件已经开始的操作用它自己的节奏跑完 (结果丢弃)。
  int32_t pluginCall(const std::string &id, const std::string &method,
                     const std::string &argsJson, uint32_t timeoutMs,
                     std::string &outJson, std::string &err);

  /// 调用插件能力 (**不等待版本**; 任意线程调用, 完成回调在宿主线程)
  ///
  /// 给"不能让宿主线程停下来等结果"的场景使用 (例如 JS 线程发起的跨插件调用:
  /// 若在那里同步等待宿主线程, 而宿主线程可能正等待 JS 处理器 → 互锁)。
  /// - 语义与 [pluginCall] 一致 (能力名前缀补齐、归属校验、错误码);
  /// - `done(rc, text)` 在**宿主线程**恰好一次; rc == 0 时 text 是结果 JSON,
  /// 否则是错误说明。
  void pluginCallAsync(const std::string &id, const std::string &method,
                       const std::string &argsJson,
                       std::function<void(int32_t, const std::string &)> done);

  /* ---------- 钩子 ---------- */

  int32_t hookEmit(const std::string &hookId, const std::string &inputJson,
                   bool sync, uint32_t timeoutMs, std::string &outJson,
                   std::string &err);
  int32_t hookCount(const std::string &hookId, int32_t &outCount);
  int32_t hookStatsJson(std::string &outJson);

  /* ---------- 动作请求 (插件 → Dart) ---------- */

  int32_t actionRespond(int64_t requestId, int32_t status,
                        const std::string &resultJson);

  /// 取消未完成动作 (插件卸载/宿主关闭时统一取消)
  void cancelAction(int64_t requestId);

  /* ---------- 状态镜像 (Dart → 原生) ---------- */

  int32_t stateUpdate(const std::string &key, const std::string &valueJson,
                      std::string &err);
  int32_t stateUpdateBatch(const std::string &itemsJson, std::string &err);

  /* ---------- 观测与配置 ---------- */

  int32_t debugInfo(std::string &outJson);
  int32_t statsJson(const std::string *scopeJson, std::string &outJson);
  int32_t statsConfig(const std::string &cfgJson, std::string &err);
  int32_t uiSnapshot(std::string &outJson);
  int32_t setConfig(const std::string &cfgJson, std::string &err);
  void applyLanguage(const std::string &lang);
  void logMessage(int32_t level, const std::string &message);

  /// 宿主 → 插件事件总线发布 (主题按命名空间校验; 投递到宿主线程执行)
  ///
  /// Dart 侧 (C ABI) 入口: 宿主自身只能发布官方 `musicxx.*` 主题。
  int32_t publishPluginEvent(const std::string &topic,
                             const std::string &payloadJson);

  /// 在宿主线程上发布事件 (插件 events 表入口调用; 调用方保证已在宿主线程)
  int32_t publishOnHostThread(const std::string &topic,
                              const std::string &payloadJson);

  /// 事件计数与速率 (宿主线程; 只观测不限制): 按主题命名空间归属到插件实例
  void accountEventForTopic(const std::string &topic);

  /* ---------- 领域表实现 (供 tables 调用; 均在宿主线程执行) ---------- */

  int32_t registerHook(MusicxxHostInstance *inst,
                       const MusicxxPluginHookSpec &spec);
  int32_t unregisterHook(MusicxxHostInstance *inst, std::string_view hookId,
                         std::string_view ownerTag);
  int32_t hookInfoJson(std::string_view hookId, std::string &outJson);
  int32_t hostInfoJson(std::string &outJson);
  int32_t getState(std::string_view key, std::string &outJson);
  int32_t subscribeState(MusicxxHostInstance *inst, const std::string &keysJson,
                         int32_t &outCount);
  int32_t pluginConfigPathFor(MusicxxHostInstance *inst, std::string &out);
  int32_t requestAction(MusicxxHostInstance *inst, std::string_view action,
                        std::string_view argsJson, uint32_t timeoutMs,
                        const PluginxxOperatorNotify *notify,
                        int64_t *outRequestId);

  /// 宿主线程执行器 (投递到宿主线程执行用; 动态库插件代码都在这条线程上跑)
  ///
  /// 供宿主内运行时 (JS 引擎) 把"运行期注册/订阅"等操作投递到宿主线程执行 ——
  /// 宿主线程独占注册表, 因此这些操作不能直接在其他线程改状态。
  asio::any_io_executor hostExecutor() const;

  /// 当前插件 id (由实例名推导; JS 实例名为 "js:<id>")
  static std::string pluginIdOf(std::string_view instanceName);

  /* ---------- UI 声明式扩展 (均在宿主线程执行) ---------- */

  /// 注册/覆盖一个 UI 项
  ///
  /// - `spec.item_id` 可以是短名 (宿主补 `plugin.<pluginId>.` 前缀)
  /// 或本插件的全名;
  /// - 类型必须是官方 `MUSICXX_PLUGIN_UI_TYPE_*` 之一, data 结构按类型校验;
  /// - 每个插件最多 [kMaxUiEntriesPerPlugin] 个项, 超出返回 `ERR_QUEUE_FULL`。
  int32_t registerUiEntry(MusicxxHostInstance *inst,
                          const MusicxxPluginUIEntrySpec &spec);

  /// 注销一个 UI 项 (不存在返回 `ERR_NOT_FOUND`)
  int32_t unregisterUiEntry(MusicxxHostInstance *inst, std::string_view itemId);

  /// 更新一个 UI 项的声明式内容
  int32_t updateUiEntry(MusicxxHostInstance *inst, std::string_view itemId,
                        std::string_view dataJson);

  /// 列出某插件已注册的 UI 项 (JSON 数组; `inst` 为空时列出全部)
  int32_t listUiEntries(MusicxxHostInstance *inst, std::string &outJson);

  /// 通知/提示 (fire-and-forget: 走动作 `musicxx.ui.notify`, 结果不回传插件)
  int32_t notifyUi(MusicxxHostInstance *inst, std::string_view messageJson);

  /// UI 项数量上限 (每个插件)
  static constexpr size_t kMaxUiEntriesPerPlugin = 64;

  /* ---------- 变量表 (均在宿主线程执行) ---------- */

  /// 变量表容量与上限 (硬数字; 「超限」一律拒绝并返回明确错误码, 不做截断)
  static constexpr size_t kMaxVarsTotal = 4096;
  static constexpr size_t kMaxVarsPerPlugin = 64;
  static constexpr size_t kMaxVarKeyLength = 200;
  static constexpr size_t kMaxVarValueBytes = 64 * 1024;
  static constexpr size_t kMaxVarSubscriptionsPerPlugin = 256;
  static constexpr int32_t kMaxVarPendingPerPlugin = 32;
  static constexpr int32_t kMaxVarNotifyThrottleMs = 5000;
  static constexpr int32_t kMinVarRefreshAfterMs = 1000;

  /// 注册/覆盖一个变量 (插件注册自己的; 应用声明的官方键走 [declareVars])
  int32_t registerVar(MusicxxHostInstance *inst,
                      const MusicxxPluginVarSpec &spec);

  /// 注销一个变量 (只能注销自己的; 不存在返回 `ERR_NOT_FOUND`)
  int32_t unregisterVar(MusicxxHostInstance *inst, std::string_view key);

  /// 异步读: 宿主向属主取一次真实值, 完成后调用 `notify->done` 恰好一次
  ///
  /// - 官方键由应用回答 (`musicxx.var.read` → `var_read_result`);
  /// - `declared` 模式的插件变量由宿主立即作答 (值就在注册表里);
  /// - 同键在途读请求合并 (新的读只挂到等待者列表上)。
  int32_t getVar(MusicxxHostInstance *inst, std::string_view key,
                 const PluginxxOperatorNotify *notify, int64_t *outRequestId);

  /// 同步读缓存 (给 `peek` 用; 不保证最新)
  int32_t peekVar(std::string_view key, std::string &outJson);

  /// 写: 写自己的变量 (或 declared 模式的变量) = 提交, 立即结算;
  /// 官方键与 handler 模式转给属主落地, 最终结果经 `musicxx.var.writeResult` 送达
  int32_t setVar(MusicxxHostInstance *inst, std::string_view key,
                 std::string_view valueJson, std::string &outJson);

  /// 列出变量 (prefix 可空; 值字段是缓存值)
  int32_t listVars(std::string_view prefix, std::string &outJson);

  /// 订阅/退订/保活某个键的变更 (keys_json = 字符串数组)
  int32_t subscribeVars(MusicxxHostInstance *inst, const std::string &keysJson,
                        int32_t &outCount);
  int32_t unsubscribeVars(MusicxxHostInstance *inst,
                          const std::string &keysJson, int32_t &outCount);
  int32_t watchVars(MusicxxHostInstance *inst, const std::string &keysJson,
                    bool watch, int32_t &outCount);

  /// 单个变量的声明信息 + 缓存状态
  int32_t varInfo(std::string_view key, std::string &outJson);

  /* ---------- 变量表 · 应用侧入口 (C ABI 调用; 均在宿主线程执行) ---------- */

  /// 应用声明一批变量 (items_json = 声明数组; 逐项失败只记日志)
  int32_t declareVars(const std::string &itemsJson, std::string &log);

  /// 应用推值 (官方键的应用侧变动; 只有真变化才通知)
  int32_t updateVar(const std::string &key, const std::string &valueJson,
                    std::string &err);
  int32_t updateVarsBatch(const std::string &itemsJson, std::string &err);

  /// 应用回执 (回执即落地: accepted 且带 value 时落值并按需广播)
  int32_t varWriteResult(int64_t requestId, int32_t accepted,
                         const std::string &valueJson,
                         const std::string &error);

  /// 应用回答读取请求 (官方键被插件 `get` 时用)
  int32_t varReadResult(int64_t requestId, int32_t ok,
                        const std::string &valueJson,
                        const std::string &error);

  /// 应用读插件键 (有界等待; 见 musicxx_extern_plugin_api.cpp)
  int32_t getVarForApp(const std::string &key, std::string &outJson,
                       std::shared_ptr<WaitSlot<std::string>> slot);

  /// 应用写插件键 (有界等待)
  int32_t setVarForApp(const std::string &key, const std::string &valueJson,
                       std::string &outJson,
                       std::shared_ptr<WaitSlot<std::string>> slot);

  /// 应用订阅/退订插件键的变动 (keys_json = 数组或 "plugin." 前缀)
  int32_t subscribeVarsForApp(const std::string &keysJson, bool subscribe,
                              std::string &err);

  /// 变量表快照 (管理页"变量"区块 / 宿主调试信息)
  std::string varsDebugJson() const;

  /// 摘除某实例的全部变量与订阅 (生命周期钩子; 推送 `removed` 通知)
  void detachVars(const std::string &instanceName);

  /// 按**插件 id 或实例名**解析实例 (JS 插件实例名是 `js:<id>`, 与插件 id 不同)
  ///
  /// 插件管理入口 (启停/卸载/能力调用/配置路径) 都接受 Dart 侧传来的插件 id,
  /// 因此统一经这里解析; 动态库插件的实例名恰好等于插件 id。
  std::shared_ptr<MusicxxHostInstance>
  resolveInstance(const std::string &idOrInstance) const;

  /* ---------- 宿主内运行时 (JS 引擎) ---------- */

  /// 注册宿主内运行时的动作请求接驳口 (JS 引擎创建时调用)
  void setInternalActionRelay(std::shared_ptr<InternalActionRelay> relay);

  /// 分配一个请求 id (**任意线程**; 动态库插件与 JS 引擎共用同一计数器,
  /// 避免 id 撞车)
  int64_t allocateRequestId();

  /// 推送"插件 → Dart"的动作请求事件 (**任意线程**; 不做请求登记)
  ///
  /// 供 JS 引擎使用: JS 引擎自己保活 in-flight 状态与超时 (见
  /// [InternalActionRelay]), 宿主这里只做命名空间校验与事件推送。
  /// - 返回 0 = 已推送; -6 = 动作名命名空间非法; -1 = 参数非法
  /// - `outEffectiveTimeoutMs` 输出实际生效的超时 (默认 5s, 限幅 1s~60s)
  int32_t pushActionRequestEvent(const std::string &pluginId, int64_t requestId,
                                 const std::string &action,
                                 const std::string &argsJson,
                                 uint32_t timeoutMs,
                                 int64_t *outEffectiveTimeoutMs);

  /// 插件发布事件 (**任意线程**; 主题归属校验后再投递到宿主线程)
  int32_t publishPluginEventFrom(const std::string &pluginId,
                                 const std::string &topic,
                                 const std::string &payloadJson);

  /// 取消某实例的全部未完成请求 (**任意线程**): 宿主侧登记 + JS 引擎侧登记
  void cancelActionsOfInstance(const std::string &instanceName);

  /// JS 引擎实例 (未启用 JS 运行时或未启动时为空)
  std::shared_ptr<JsEngine> jsEngine() const;

  /// 注册 JS 插件的内置槽位并把插件目录登记到 discovered_ (装载前调用)
  ///
  /// - `pluginDir` 为插件目录; 脚本取清单 `scripts`（按顺序执行），没写时取
  /// `entryHint` (以 .js 结尾时) 或 `plugin.js`;
  /// - 返回 0 成功; -4 找不到脚本文件; -2 状态错误 (JS 运行时不可用/安全模式)。
  int32_t prepareJsPlugin(const std::string &pluginId,
                          const std::string &pluginDir,
                          const std::string &entryHint,
                          const std::vector<std::string> &scriptHints,
                          const std::string &version, std::string &err);

  /* ---------- DomainHooks (事件主题规则对外可见: 插件 events 表入口要用)
   * ---------- */

  std::shared_ptr<pluginxx::EventSource> eventSource() override;
  std::string qualifyEventTopic(std::string_view topic) override;

protected:
  /* ---------- 内核生命周期接缝 ---------- */

  std::shared_ptr<pluginxx::PluginHostLifecycle<MusicxxHostInstance>>
  selfRef() override;
  std::shared_ptr<MusicxxHostInstance>
  createInstance(std::string name) override;
  const PluginxxHostVtable *hostVtable() override;
  pluginxx::PluginEntrySymbols entrySymbols() const override;

  void detachDomainRegistrations(MusicxxHostInstance *inst) override;
  void clearDomainRegistrations(MusicxxHostInstance *inst) override;
  void onInstanceLoaded(MusicxxHostInstance &inst) override;
  void onInstanceUnloaded(MusicxxHostInstance &inst) override;
  void onInstanceEnabledChanged(MusicxxHostInstance &inst,
                                bool enabled) override;

  std::string_view logTag() const noexcept override { return "[musicxx_ext] "; }

  /* ---------- DomainHooks ---------- */

  bool postToWorkerThread(std::function<void()> fn) override;
  std::string configJson() override;
  std::string toolPromptJson(std::string_view toolName) override;
  std::string sessionWorkDir(std::string_view sessionId) override;
  std::string language() override;
  void setLanguage(std::string_view lang) override;
  bool isSessionCancelled(std::string_view sessionId) override;
  std::string pluginsJson() override;
  std::string pluginJson(std::string_view name) override;

private:
  MusicxxHostManager(asio::any_io_executor ex);

  /// 应用宿主配置 (create() 里调用; 解析路径/开关/预算)
  void applyConfig(const MusicxxExternPluginHostConfig &cfg);

  /// 记录客户端界面能力段 (JSON 文本; 只校验是不是 JSON 对象, 不解释内容)
  void setUiCapabilities(const std::string &capabilitiesJson);

  /// 在宿主线程上绑定 IO 线程标识 (start() 里调用; 失败返回 false)
  bool bindIoThread();

  /// 刷新 JS 引擎的宿主信息缓存 (语言/配置变化时; 供 JS 侧同步读)
  void refreshJsHostInfo();

  /// 钩子处理器登记项
  struct HookHandler {
    std::string plugin;   ///< 实例名
    std::string pluginId; ///< 插件 id (去掉 js: 前缀)
    std::string ownerTag;
    std::string hookId;
    MusicxxPluginHookSpec spec{};
    uint64_t seq = 0;

    int64_t calls = 0;
    int64_t failures = 0;
    int64_t timeouts = 0;
    int64_t totalUs = 0;
    int64_t maxUs = 0;
    int32_t consecutiveErrors = 0;
    std::chrono::steady_clock::time_point pausedUntil{};
  };

  /// 未完成动作请求
  struct PendingAction {
    std::string plugin;
    std::string action;
    const PluginxxOperatorNotify *notify = nullptr;
    std::chrono::steady_clock::time_point deadline{};
    /// 超时定时器 (宿主自我保护: Dart 若一直不回复, 到点终结该 op 并通知 Dart
    /// 收尾)
    std::shared_ptr<asio::steady_timer> timer;
  };

  /// 在宿主线程发起能力调用 (**必须已在宿主线程调用**; pluginCall /
  /// pluginCallAsync 共用)
  void invokeCapabilityOnHostThread(
      const std::string &id, const std::string &method,
      const std::string &argsJson,
      std::function<void(int32_t, const std::string &)> done);

  /// 钩子派发模式
  ///
  /// - `Sync`：裁决型同步派发（Dart 线程等待结果，有等待预算）
  /// - `Notify`：观察型派发（入队即返回，不等待、不回报结果）
  /// - `AsyncDecide`：裁决型异步派发（不占用 Dart 线程，完成后推
  ///   `musicxx.hook.decision.result` 事件；等待预算照常生效）
  enum class HookDispatchMode {
    Sync,
    Notify,
    AsyncDecide,
  };

  /// 宿主线程上执行的派发（按 [mode] 决定是否等待结果、是否回报事件）
  std::string dispatchHook(const std::string &hookId,
                           const std::string &inputJson, uint32_t budgetMs,
                           HookDispatchMode mode);
  void clearPluginRegistrations(const std::string &instanceName);
  std::string pluginInstanceJson(const MusicxxHostInstance &inst) const;
  std::string scanDir(const std::string &dir, const std::string &kindHint);
  std::string deriveConfigPath(const std::string &pluginId,
                               const std::string &pluginDir) const;

  HostContext *ctx_ = nullptr;

  std::atomic<bool> running_{false};
  std::atomic<bool> started_{false};

  /// 宿主 IO 线程标识是否已在 start() 里绑定 (见 bindIoThread 的说明)
  std::atomic<bool> ioThreadBound_{false};

  // 事件队列 (多生产者: 宿主线程/工作线程池/Dart 线程)
  mutable std::mutex eventMutex_;
  std::deque<std::string> events_; ///< 已序列化的事件 JSON
  int64_t eventSeq_ = 0;
  int64_t eventDropped_ = 0;
  size_t eventCapacity_ = 16384;
  MusicxxExternPluginWakeFn wakeFn_ = nullptr;
  void *wakeUser_ = nullptr;

  // 状态镜像 (Dart 线程写, 宿主线程读)
  mutable std::mutex stateMutex_;
  std::map<std::string, std::string, std::less<>> state_;

  // 钩子注册表 (仅宿主线程)
  std::map<std::string, std::vector<HookHandler>, std::less<>> hooks_;
  uint64_t hookSeq_ = 0;

  /// 一个已注册的 UI 项 (声明式扩展; 生命周期 = 插件实例)
  struct UiEntry {
    std::string plugin;   ///< 实例名
    std::string pluginId; ///< 插件 id (去掉 js: 前缀)
    std::string id;       ///< 全名 plugin.<pluginId>.<名>
    std::string type;     ///< MUSICXX_PLUGIN_UI_TYPE_*
    std::string dataJson; ///< 声明式内容
    int32_t order = 0;
    uint64_t seq = 0; ///< 注册顺序 (同 order 时稳定排序)
  };

  /// UI 项注册表 (仅宿主线程)
  std::map<std::string, UiEntry, std::less<>> uiEntries_;
  uint64_t uiSeq_ = 0;

  /// 摘除某实例的全部 UI 项 (宿主线程; 有变化时推送 musicxx.ui.changed)
  void detachUiEntries(const std::string &instanceName);

  /// 序列化 UI 项 (pluginId 为空 = 全部; 按 type/order/seq 排序)
  std::string uiItemsJson(const std::string &pluginId) const;

  /* ---------- 变量表 (仅宿主线程) ---------- */

  /// 一个变量 (注册项 + **同步读缓存**)
  ///
  /// 这里**没有真值**: 真值只在属主那里 (应用侧的 Store/配置, 或插件自己的状态)。
  /// `valueJson` 只是"最后一次已知值", 服务 `peek` 这条同步捷径, 来源三处:
  /// 属主推送、每次异步读的结果 (read-through)、注册时的初值。
  struct VarEntry {
    std::string key;
    std::string owner;    ///< 实例名; 空 = 应用 (官方键)
    std::string pluginId; ///< 空 = 应用
    int32_t caps = 0;     ///< MUSICXX_PLUGIN_VAR_CAP_*
    int32_t mode = 0;     ///< MUSICXX_PLUGIN_VAR_MODE_*
    int32_t writeScope = 0;

    std::string type;
    std::string optionsJson; ///< 数组或空
    std::string title;
    std::string depict;
    std::string risk;

    /// 同步读缓存 (见结构体说明)
    std::string valueJson;
    int64_t revision = 0;
    int64_t valueMs = 0; ///< 写入时刻 (steady 毫秒; 算 ageMs)
    /// handler 模式下缓存有没有被属主回答过一次 (没回答过时 peek 返回空)
    bool everRefreshed = false;

    int32_t throttleMs = 0;
    int32_t notifyThrottleMs = 0;
    int32_t refreshAfterMs = 0;

    uint64_t seq = 0; ///< 注册顺序 (list 与调试信息用稳定排序)
    /// 订阅变更的实例 (bind 与 watch 都算"有人关心")
    std::set<std::string> subscribers;
    /// 只用过 watch (不带回调) 的实例: unwatch 不能把 bind 的订阅一起摘掉
    std::set<std::string> watchers;

    /// 通知合并窗口 (notifyThrottleMs > 0 时用; 窗口结束时发一条带 coalesced 的通知)
    std::shared_ptr<asio::steady_timer> notifyTimer;
    int32_t pendingCoalesced = 0;
    std::string pendingPrevJson;

    /// 关心这个键的实例数 (bind 的订阅 + watch 的保活, 去重)
    size_t careCount() const {
      size_t count = subscribers.size();
      for (const std::string &item : watchers) {
        if (subscribers.find(item) == subscribers.end()) {
          ++count;
        }
      }
      return count;
    }
  };

  /// 变量表 (仅宿主线程)
  std::map<std::string, VarEntry, std::less<>> vars_;
  uint64_t varSeq_ = 0;

  /// 未完成的变量请求 (读/写都要等属主回答或落地)
  struct PendingVarRequest {
    std::string key;
    std::string byPluginId; ///< 发起方插件 id (空 = 应用; 写请求里作为 `by`)
    std::string byInstance; ///< 发起方实例名 (写结果的 `musicxx.var.writeResult` 发给它)
    int32_t kind = 0;       ///< VarRequestKind
    std::string valueJson;  ///< 写请求值
    std::string owner;      ///< 属主实例名; 空 = 应用
    int32_t caps = 0;       ///< 申请时的能力位 (结算时回执用)
    /// 插件侧等待者 (同键在途读合并后, 答案扇出给全部等待者)
    std::vector<PluginxxOperatorNotify> pluginWaiters;
    /// 应用侧等待者 (C ABI 的有界等待槽)
    std::vector<std::shared_ptr<WaitSlot<std::string>>> appWaiters;
    std::shared_ptr<asio::steady_timer> timer;
  };

  enum VarRequestKind {
    VarRequestRead = 1,
    VarRequestWrite = 2,
  };

  std::map<int64_t, PendingVarRequest> pendingVarRequests_;
  /// 键 → 在途的"向属主取真实值"请求 id (同键合并)
  std::map<std::string, int64_t, std::less<>> pendingReadByKey_;

  /// 应用订阅的插件键前缀/全名 (var_subscribe; 只用来决定要不要推给应用)
  std::vector<std::string> appVarSubscriptions_;

  /// 实例订阅的键 (bind; 去重的唯一来源)
  std::map<std::string, std::set<std::string>, std::less<>> varSubscriptions_;
  /// 实例只做保活 (watch) 的键: unwatch 不能把 bind 的订阅一起摘掉
  std::map<std::string, std::set<std::string>, std::less<>> varWatchers_;

  /// 全部键的关心者合计 (变化时回传 `musicxx.var.subscriptions`)
  int64_t varCareTotal_ = 0;

  /// 结算一条变量请求 (宿主线程): 读 → 写缓存并扇出答案; 写 → 落值并广播
  void completeVarRequest(int64_t requestId, bool ok, const std::string &valueJson,
                          const std::string &error);
  /// 结算超时的变量请求 (宿主线程): 读按 read_timeout, 写按 not_served / owner_timeout
  void expireVarRequest(int64_t requestId);

  /// 读请求的公共实现 (插件侧与应用侧共用)
  int32_t beginVarRead(const std::string &key, const std::string &byPluginId,
                       const PluginxxOperatorNotify *notify,
                       std::shared_ptr<WaitSlot<std::string>> appSlot,
                       int64_t *outRequestId);

  /// 写请求的公共实现 (立即结算或转给属主落地)
  int32_t beginVarWrite(const std::string &key, const std::string &valueJson,
                        const std::string &byPluginId,
                        const std::string &byInstance,
                        const PluginxxOperatorNotify *notify,
                        std::shared_ptr<WaitSlot<std::string>> appSlot,
                        std::string &outJson);
  /// 请求 id 是否属于变量表 (供 C ABI 的 write_result / read_result 路由)
  bool hasPendingVarRequest(int64_t requestId) const;

  /// 落值 + 广播 (宿主线程; changed 才广播; 返回是否真的变了)
  bool applyVarValue(VarEntry &entry, const std::string &valueJson,
                     const std::string &byPluginId, std::string &prevJson);
  /// 广播一条变量变更 (宿主线程; 按 notifyThrottleMs 合并)
  void pushVarChanged(const std::string &key, const std::string &valueJson,
                      const std::string &prevJson, const std::string &byPluginId,
                      bool removed, int32_t coalesced);
  /// 应用是否订阅了这个键 (决定要不要把插件键的变化推给 Dart)
  bool appCaresAboutVar(const std::string &key) const;
  /// 把一个值写进某个键的缓存并向关心者广播 (官方键的应用侧推值走这条)
  int32_t applyAppVarValue(const std::string &key, const std::string &valueJson,
                           std::string &err);

  /// 序列化一个变量 (list/info/debug 共用; 值字段明确标为缓存值)
  std::string varToJsonText(const VarEntry &entry) const;

  /* ---------- 同步读镜像 (任何线程可读; 给 JS 侧的 peek/info/list 用) ----------
   *
   * 为什么需要: 插件侧绝不能在 JS 线程上等宿主线程 (宿主线程可能正等着 JS
   * 处理器 —— 那是死锁)。因此 `peek`/`info`/`list` 不能投递到宿主线程去查注册表,
   * 而是读这份由宿主线程维护、用互斥量保护的**快照** (与状态镜像同一套做法)。
   * 快照里的值仍然是"缓存值", 不是真值。
   */

  /// 保护 [varMirror_]
  mutable std::mutex varMirrorMutex_;
  /// key → 变量快照 JSON (含 valueMs; ageMs/stale 在读取时算)
  std::map<std::string, std::string, std::less<>> varMirror_;

  /// 把变量的当前快照写进镜像 (宿主线程; 每次注册/落值/订阅变化后调用)
  void publishVarMirror(const VarEntry &entry);
  /// 从镜像里摘掉一个键 (宿主线程)
  void eraseVarMirror(const std::string &key);
  /// 变量快照的 JSON (镜像内容; 不含 ageMs/stale 这两个与时刻有关的字段)
  std::string varMirrorJsonText(const VarEntry &entry) const;

  /// 变量表快照的文本 (调试信息用; 与 [varToJsonText] 同一份口径)
  std::string varsSnapshotText() const;

  /// 取消某实例的变量等待者 (实例停用/卸载时; 宿主线程)
  void cancelVarsOf(const std::string &instanceName);

  /// 记一次订阅/退订 (实例 ↔ 键; 同步维护 VarEntry 上的订阅集合与关心计数)
  ///
  /// - `watch` = true 表示"保活缓存"(不带回调), false 表示订阅 (bind);
  /// - 订阅还不存在的键也会被记下来 (键出现时自动挂上并补一条初始通知);
  /// - `outCount` 累计本次实际生效的条数 (超出容量上限的部分被忽略, 不报错)。
  void markVarSubscriber(const std::string &instanceName, const std::string &key,
                         bool watch, bool add, int32_t &outCount);

  /// 重新计算某键的关心者数量并把数量回传给应用 (`musicxx.var.subscriptions`)
  void refreshVarCare(const VarEntry &entry);
  /// 某插件注册了多少个变量 (容量上限判断用)
  size_t countVarsOfPlugin(const std::string &pluginId) const;

  // 动作请求 (宿主线程)
  std::map<int64_t, PendingAction> pendingActions_;
  /// 请求 id 计数器 (动态库插件与 JS 引擎共用; **任意线程**可用)
  std::atomic<int64_t> nextRequestId_{1};

  /// 异步裁决派发的调用 id 计数器（配 `musicxx.hook.decision.result`
  /// 事件；**任意线程**可用）
  std::atomic<int64_t> nextHookCallId_{1};

  /// 宿主内运行时的请求接驳口 (JS 引擎; 见 InternalActionRelay)
  std::shared_ptr<InternalActionRelay> internalRelay_;

  /// JS 运行时 (启用 JS 时在 start() 里创建, stop() 里释放)
  std::shared_ptr<JsEngine> jsEngine_;

  // 配置
  std::string appVersion_ = "0.0.0";
  std::string platform_ = "unknown";
  std::string language_ = "zh-cn";
  std::string userPluginDir_;
  std::string builtinPluginDir_;
  std::string dataDir_;
  std::string logDir_;
  /// 客户端界面能力段 (JSON 文本; 由 Dart 侧给出, 宿主原样转发到 host.info().ui)
  std::string uiCapabilitiesJson_;
  int32_t logLevel_ = 2;
  int32_t flags_ = 0;
  int32_t hookBudgetMs_ = 30;
  int32_t hookHardMs_ = 100;
  /// 可选 JS 执行上限 (毫秒; 0 = 关闭; 默认关闭)
  int32_t jsExecGuardMs_ = 0;
  bool statsEnabled_ = true;

  // 已知插件 (宿主线程 + Dart 线程都读; 由 registryMutex_ 保护)
  std::map<std::string, InstancePtr, std::less<>> loaded_;

  // 已发现的插件 (id → 目录)
  std::map<std::string, std::string, std::less<>> discovered_;

  /// 保护 loaded_ / discovered_ / stateSubscriptions_ 的跨线程读写
  mutable std::mutex registryMutex_;

  // 已声明的状态订阅 (插件 id → 键列表; 宿主线程)
  std::map<std::string, std::vector<std::string>, std::less<>>
      stateSubscriptions_;

  // 插件事件总线 (进程内主题表; 见 eventSource()/publishPluginEvent)
  std::shared_ptr<pluginxx::EventSource> eventBus_;

  std::chrono::steady_clock::time_point startTime_{};
};

} // namespace extern_plugin
} // namespace musicxx
