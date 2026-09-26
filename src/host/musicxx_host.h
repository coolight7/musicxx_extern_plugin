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
  /// - `pluginDir` 为插件目录; 脚本取 `entryHint` (以 .js 结尾时) 或
  /// `plugin.js`;
  /// - 返回 0 成功; -4 找不到脚本文件; -2 状态错误 (JS 运行时不可用/安全模式)。
  int32_t prepareJsPlugin(const std::string &pluginId,
                          const std::string &pluginDir,
                          const std::string &entryHint,
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
