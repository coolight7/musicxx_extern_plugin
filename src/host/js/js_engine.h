/// musicxx 外部插件原生宿主: JS 插件运行时 (QuickJS)
///
/// 设计要点:
/// - **零编译**: 一个 JS 插件 = 一个目录 (`plugin.yaml` + `plugin.js`),
/// 宿主把它注册成
///   内置插件 (合并编译) 的一个**合成实例** (`name = "js:<pluginId>"`),
///   生命周期 (create/start/stop/destroy) 全部由本引擎实现,
///   因此仍然走内核的完整事务语义;
/// - **共享 1 条 JS 线程**: 全部 JS 插件实例的脚本都在该线程顺序执行;
///   每个实例持有独立 `JSRuntime`/`JSContext` (全局隔离), 只是时间片共享;
/// - **不阻塞宿主线程**: 脚本顶层与处理器里调用宿主能力时, 只做"投递不等待"
///   (动作请求走 [InternalActionRelay], 状态读取直接读互斥保护的镜像);
/// - **顶层注册 + 回放**: 脚本顶层调用 `musicxx.hooks.register`
/// 等注册接口时先在 JS 侧
///   先记下来; 脚本执行结束、引擎回到宿主线程后再回放注册 —— 避免"宿主线程等 JS、JS
///   等宿主线程" 的互锁 (无死锁不变式);
/// - **规则与动态库插件一致**:
/// 命名空间校验、未知钩子拒绝、禁用/卸载摘除注册都沿用同一套约定
///   (动作只做命名空间校验: 权限声明由 Dart 侧展示, 不做运行时校验)。
///
/// 线程约定:
/// - `JSRuntime`/`JSContext`/`JSValue` 只在共享 JS 线程上访问;
/// - 内核生命周期入口 (builtin*) 与钩子处理器由宿主线程调用;
/// - `requestAction` / `respondAction` / `cancelActionsOf` 允许任意线程。
#pragma once

#include "musicxx_host.h"

#include "musicxx/plugin/api/plugin_api.h"
#include "pluginxx/api/abi.h"

#include <array>
#include <atomic>
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

/// JS 插件运行时 (共享 1 条 JS 线程; 进程内单例, 与宿主同生命周期)
class JsEngine final : public InternalActionRelay,
                       public std::enable_shared_from_this<JsEngine> {
public:
  ~JsEngine() override;

  /// 创建引擎 (不启动线程)
  static std::shared_ptr<JsEngine> create(MusicxxHostManager *mgr);

  /// 本次构建是否包含 JS 运行时 (缺 QuickJS 子模块时为 false)
  static bool available();

  /// 启动共享 JS 线程 (幂等)
  int32_t start(std::string &err);

  /// 停止共享 JS 线程 (幂等; 会先释放全部实例上下文)
  void stop();

  bool running() const { return running_.load(std::memory_order_acquire); }

  /* ---------- 内置插件槽位 (js:<pluginId>) ---------- */

  /// 登记/更新一个 JS 插件的内置槽位 (装载前调用; 宿主线程)
  bool registerBuiltin(const std::string &pluginId,
                       const std::string &scriptPath,
                       const std::string &version, std::string &err);

  /// 注销槽位 (卸载完成/装载失败回滚时调用; 宿主线程)
  void unregisterBuiltin(const std::string &pluginId);

  bool hasBuiltin(const std::string &pluginId) const;

  /// 把内置插件提供者交给内核 (进程级; 幂等)
  static void installBuiltinProvider();

  /* ---------- JS 桥接 (primitive 用; 线程安全) ---------- */

  int32_t bridgeStateGet(const std::string &key, std::string &out) const;
  std::string bridgeHostInfo() const;
  void bridgeLog(const std::string &pluginId, int32_t level,
                 const std::string &message);
  int32_t bridgePublish(const std::string &pluginId, const std::string &topic,
                        const std::string &payload);
  int64_t bridgeTimerSet(const std::string &instance, int64_t delayMs,
                         bool repeat);
  void bridgeTimerClear(const std::string &instance, int64_t timerId);

  /// UI 项操作 (op = register | update | unregister; 只在 JS 线程调用)
  ///
  /// 脚本顶层登记阶段 (`liveRegistrations == false`) 只把操作记在实例上, 等
  /// [applyRegistrations] 在宿主线程回放 (这样注册错误能即刻上报, 也不会出现
  /// "宿主线程等 JS、JS 等宿主线程" 的互锁);
  /// 运行期则**投递不等待**地交给宿主线程。
  void bridgeUiOp(const std::string &instanceName, const std::string &op,
                  const std::string &name, const std::string &type,
                  const std::string &dataJson, int32_t order);

  /// 钩子操作 (op = register | unregister; 只在 JS 线程调用)
  ///
  /// 与 [bridgeUiOp] 同一套规则: 顶层登记阶段只记下来 (由
  /// [applyRegistrations] 回放), 运行期投递到宿主线程执行 —— 否则
  /// `musicxx.hooks.register/unregister` 只改了 JS 侧的表, 宿主注册没变
  /// (表现为"注销了但每次派发仍然跨线程调用", 或"新注册的钩子永远不触发")。
  void bridgeHookOp(const std::string &instanceName, const std::string &op,
                    const std::string &hookId, const std::string &mode,
                    int32_t priority, const std::string &ownerTag);

  /// 运行期动态订阅 (投递到宿主线程建立订阅; 顶层声明由回放处理)
  void bridgeSubscribeHost(const std::string &instanceName,
                           const std::string &topic);

  /// 裁决型钩子的异步结算 (只在 JS 线程调用)
  ///
  /// 裁决处理器返回 Promise 时, 宿主线程仍在 [hookSync] 里按等待预算等待;
  /// Promise 结算后 JS
  /// 侧调用本接口把结果交给对应的等待槽。**已经超时的调用不再接收结果** (只计入
  /// "迟到丢弃"统计并打一条日志), 因此不会出现"用了过期裁决"的情况。
  void bridgeHookWaitResolve(const std::string &waitId,
                             const std::string &json);

  /// 本实例统计 JSON (任意线程; 只读原子字段)
  std::string instanceStatsJson(const std::string &instanceName) const;

  /// 某实例的 JS 堆用量 (任意线程; 负数 = 未采样)
  int64_t jsHeapBytesOf(const std::string &instanceName) const;

  /// 跨插件能力调用 (只在 JS 线程调用)
  ///
  /// - 目标是 JS 插件: **同一 JS 线程上直接调用**, 结果同步返回 (返回
  /// [kCallCapabilityDone]);
  /// - 目标是原生/内置插件: 投递到宿主线程执行 (原生处理器只能在宿主线程跑),
  /// 结果稍后经
  ///   `ext.onCapabilityResult(id, json)` 回到 JS 线程 —— JS
  ///   侧**不等待宿主线程** (无锁/无死锁不变式: 宿主线程可能正等待 JS 处理器)。
  ///
  /// 返回: `kCallCapabilityDone` = outJson 有结果; `kCallCapabilityPending` =
  /// 已受理, *outPendingId 是回执 id; 负数 = 立即失败 (err 说明原因)。
  static constexpr int32_t kCallCapabilityPending = 1;
  static constexpr int32_t kCallCapabilityDone = 0;

  int32_t callCapability(const std::string &callerInstance,
                         const std::string &targetId, const std::string &name,
                         const std::string &argsJson, uint32_t timeoutMs,
                         int64_t *outPendingId, std::string &outJson,
                         std::string &err);

  /// 可选执行上限 (毫秒; 0 = 关闭, 默认关闭): 开启后单次进入脚本超时会被
  /// QuickJS 中断并计为失败 ("用户自选保护", 不是宿主默认限制)
  void setExecGuardMs(int32_t ms);
  int32_t execGuardMs() const;

  /// 刷新宿主信息缓存 (宿主线程: 语言/配置变化时调用)
  void setHostInfoCache(const std::string &json);

  /* ---------- 内部数据结构 (宿主内部使用, 不是对外契约) ---------- */

  /// 一个已注册的钩子处理器 (生命周期 = 实例)
  struct HookHandler {
    JsEngine *engine = nullptr;
    std::string instance; ///< js:<id>
    std::string pluginId;
    std::string hookId;
    std::string ownerTag;
    bool decision =
        true; ///< true = 裁决型 (hook_sync), false = 观察型 (hook_start)
  };

  /// 一个已声明的能力 (生命周期 = 实例)
  struct CapabilityHandler {
    JsEngine *engine = nullptr;
    std::string instance;
    std::string pluginId;
    std::string shortName; ///< 脚本里注册的短名
    std::string fullName; ///< `plugin.<pluginId>.<短名>` (注册与撤销用)
  };

  /// 一个事件订阅的持有者 (回调 ud 指向它; 生命周期 = 实例)
  struct SubscriptionHolder {
    JsEngine *engine = nullptr;
    std::string instance;
    std::string topic;
  };

  /// 脚本定时器 (`musicxx.timer.*`)
  struct Timer {
    int64_t id = 0;
    std::string instance;
    int64_t dueMs = 0;
    int64_t intervalMs = 0;
    bool repeat = false;
  };

  /// 未完成动作请求 (JS 侧登记; 任意线程读写, 由 mutex_ 保护)
  struct PendingAction {
    int64_t requestId = 0;
    std::string instance;
    std::string pluginId;
    std::string action;
    int64_t deadlineMs = 0;
  };

  /// 一个 JS 插件实例 (JSRuntime/JSContext 只在 JS 线程访问)
  struct Instance {
    JsEngine *engine = nullptr;
    std::string id;
    std::string name; ///< js:<id>
    std::string scriptPath;
    std::string configPath;
    std::string version;
    std::string argsJson;
    void *rt = nullptr;  ///< JSRuntime*
    void *ctx = nullptr; ///< JSContext*

    /// 宿主侧实例指针 (create 时从 host 视图解析; 与实例同生命周期)
    MusicxxHostInstance *hostInst = nullptr;

    std::vector<std::shared_ptr<HookHandler>> hooks;
    std::vector<std::shared_ptr<CapabilityHandler>> capabilities;
    std::vector<std::shared_ptr<SubscriptionHolder>> subscriptions;
    bool registrationsApplied = false;
    bool scriptLoaded = false;

    /// 运行期注册是否已生效 (脚本顶层登记阶段为 false; JS 线程读, 宿主线程写)
    std::atomic<bool> liveRegistrations{false};

    /// 脚本顶层登记的 UI 项操作 (JS 线程写, 宿主线程在回放时取走)
    struct PendingUiOp {
      std::string op; ///< register | update | unregister
      std::string name;
      std::string type;
      std::string dataJson;
      int32_t order = 0;
    };
    std::vector<PendingUiOp> pendingUiOps;

    std::atomic<int64_t> jsRuns{0};
    std::atomic<int64_t> errors{0};
    /// JS 堆用量 (字节; 负数 = 还没采样过; 只观测不限制)
    std::atomic<int64_t> jsHeapBytes{-1};
    /// 被可选执行上限中断的次数
    std::atomic<int64_t> execGuardHits{0};

    /// 裁决型钩子的 Promise 在等待预算内结算的次数 (异步裁决)
    std::atomic<int64_t> asyncHookSettled{0};
    /// Promise 未在等待预算内结算的次数 (按无裁决继续, 不计处理器失败)
    std::atomic<int64_t> asyncHookTimeouts{0};
    /// 超时之后才结算、被丢弃的次数 (只用于排障, 不改变裁决结果)
    std::atomic<int64_t> asyncHookLateDrops{0};
  };

  /* ---------- 内核内置插件入口 (C ABI 形态; 内核在宿主线程调用) ---------- */

  static const PluginxxInfo *PLUGINXX_CALL builtinGetInfo();
  static int32_t PLUGINXX_CALL builtinCreate(const PluginxxHost *host,
                                             void **outCtx);
  static void *PLUGINXX_CALL builtinStart(void *ctx,
                                          const PluginxxOperatorNotify *notify,
                                          PluginxxString *errorOut);
  static void *PLUGINXX_CALL builtinStop(void *ctx,
                                         const PluginxxOperatorNotify *notify,
                                         PluginxxString *errorOut);
  static void PLUGINXX_CALL builtinDestroy(void *ctx);

  /// 内置插件描述数组 (内核要求地址稳定; 固定容量槽位表, 见 js_engine.cpp)
  static const PluginxxBuiltinInfo *builtinPlugins(uint64_t *count);
  static const PluginxxBuiltinManifest *builtinManifests(uint64_t *count);

  /* ---------- 钩子处理器 (宿主线程调用; 见 HookHandler) ---------- */

  static int32_t PLUGINXX_CALL hookSync(void *userData,
                                        const PluginxxStringView *hookId,
                                        const PluginxxStringView *inputJson,
                                        PluginxxString *outJson,
                                        PluginxxString *errorOut);

  static void *PLUGINXX_CALL hookStart(void *userData,
                                       const PluginxxStringView *hookId,
                                       const PluginxxStringView *inputJson,
                                       const PluginxxOperatorNotify *notify,
                                       PluginxxString *errorOut);

  /// 能力处理器 (内核在宿主线程调用; 内部投递到 JS 线程并等待结果)
  static void *PLUGINXX_CALL capabilityStart(
      void *ctx, const PluginxxHost *callerHost,
      const PluginxxStringView *method, const PluginxxStringView *argsJson,
      const PluginxxOperatorNotify *notify, PluginxxString *errorOut);

  /// 事件订阅回调 (宿主线程调用; 只投递到 JS 线程, 不等待)
  static void PLUGINXX_CALL eventTrampoline(const PluginxxStringView *eventJson,
                                            void *userData);

  /* ---------- InternalActionRelay (任意线程) ---------- */

  bool respondAction(int64_t requestId, int32_t status,
                     const std::string &resultJson) override;
  void cancelActionsOf(const std::string &instanceName) override;

  /// JS 侧发起动作请求 (任意线程; 不阻塞): 校验命名空间 → 登记 → 推事件给 Dart
  int32_t requestAction(MusicxxHostInstance *inst, const std::string &action,
                        const std::string &argsJson, uint32_t timeoutMs,
                        int64_t *outRequestId);

  /// 运行时统计 (管理页/调试页)
  std::string statsJson() const;

  /// 实例数 (测试/统计用)
  int32_t instanceCount() const;

private:
  JsEngine();

  /* ---------- JS 线程 ---------- */

  /// 本次进入脚本的执行上界 (0 = 不限制; 见 setExecGuardMs)
  void armExecGuard();
  void disarmExecGuard();

  /// 采样 JS 堆用量 (JS 线程调用; 结果写入实例字段供统计读取)
  void sampleHeap(const std::shared_ptr<Instance> &inst);

  /// 在宿主线程上建立一个事件订阅 (幂等; 同一 (实例, 主题) 只建一次)
  void ensureSubscriptionOnHost(const std::shared_ptr<Instance> &inst,
                                const std::string &topic);

  /// 在宿主线程上执行一次运行期钩子操作 (register / unregister)
  void applyHookOpOnHost(const std::shared_ptr<Instance> &inst,
                         const std::string &op, const std::string &hookId,
                         const std::string &mode, int32_t priority,
                         const std::string &ownerTag);

  /// 取回脚本登记的 UI 项操作 (宿主线程; 回放后清空实例上的缓存)
  std::vector<Instance::PendingUiOp>
  takePendingUiOps(const std::shared_ptr<Instance> &inst);

  int32_t startThread(std::string &err);
  void runLoop();
  void postTask(std::function<void()> fn);
  int64_t nextWaitMsLocked() const;
  void fireDueTimers();
  void expireActions();

  /* ---------- 实例生命周期 (宿主线程调用; 内部经 JS 线程执行脚本) ----------
   */

  static std::shared_ptr<Instance> instanceFromHost(const PluginxxHost *host,
                                                    std::string &err);
  /// 按实例名取实例 (任意线程; 保活返回)
  std::shared_ptr<Instance> lookupInstance(const std::string &name) const;
  int32_t startInstance(const std::shared_ptr<Instance> &inst,
                        std::string &err);
  void stopInstance(const std::shared_ptr<Instance> &inst);
  void destroyInstance(const std::shared_ptr<Instance> &inst);

  /// 在 JS 线程上建 runtime/context, 装好 `musicxx` 全局并执行脚本顶层
  bool runScriptOnJsThread(const std::shared_ptr<Instance> &inst,
                           std::string &err);

  /// 回放脚本顶层登记的注册 (宿主线程; 钩子/能力/事件订阅)
  void applyRegistrations(const std::shared_ptr<Instance> &inst);
  /// 摘除本实例的全部注册 (宿主线程)
  void detachRegistrations(const std::shared_ptr<Instance> &inst);

  /* ---------- JS 调用桥 (只在 JS 线程调用) ---------- */

  /// 调用实例里的全局函数并取字符串结果 (`__ext.<fnName>`); 失败返回 false
  bool callBridgeString(const std::shared_ptr<Instance> &inst,
                        const char *fnName,
                        const std::vector<std::string> &args, std::string &out);

  /// 记录统计 + 日志 (任意线程)
  void logPlugin(const std::string &pluginId, int32_t level,
                 const std::string &message);

  MusicxxHostManager *mgr_ = nullptr;

  /// 宿主信息缓存 (start/语言变化时刷新; 供 JS 侧同步读,
  /// 避免跨线程读管理器字段)
  mutable std::mutex hostInfoMutex_;
  std::string hostInfoJson_;

  std::atomic<bool> running_{false};
  std::atomic<bool> stopping_{false};

  /// 可选执行上限 (毫秒; 0 = 关闭; 默认关闭, 见 setExecGuardMs)
  std::atomic<int32_t> execGuardMs_{0};

  /// 上次 JS 堆采样的时刻 (JS 线程使用)
  int64_t heapSampledAtMs_ = 0;

  /// 跨插件能力调用的回执 id 计数器 (任意线程)
  std::atomic<int64_t> nextCapabilityCallId_{1};

  mutable std::mutex mutex_;
  std::condition_variable cv_;

  /// JS 线程任务 (带入队时刻, 用于"排队等待时长"统计)
  struct Task {
    int64_t enqueuedMs = 0;
    std::function<void()> fn;
  };

  std::deque<Task> tasks_;
  std::vector<Timer> timers_;
  int64_t nextTimerId_ = 1;

  /// 任务排队等待时长 (最近一次 / 历史最大; 只观测不限制)
  std::atomic<int64_t> queueWaitLastMs_{0};
  std::atomic<int64_t> queueWaitMaxMs_{0};

  /// 已装载的实例 (key = js:<id>; 任意线程读写)
  std::map<std::string, std::shared_ptr<Instance>, std::less<>> instances_;

  /// 未完成动作请求 (任意线程读写)
  std::map<int64_t, PendingAction> pendingActions_;
  std::atomic<int64_t> nextRequestId_{1};

  /// 裁决型钩子的等待槽 (key = 等待 id)
  ///
  /// 宿主线程在 [hookSync] 里登记, JS 线程在 Promise
  /// 结算时取走。超时的条目保留为 "墓碑" (`slot == nullptr`),
  /// 只为了让迟到的结算能计入统计并打一条日志 —— 结果一律丢弃。
  struct HookWait {
    std::string instance;                        ///< js:<id> (统计归属)
    std::shared_ptr<WaitSlot<std::string>> slot; ///< nullptr = 已超时 (墓碑)
  };

  std::map<int64_t, HookWait> hookWaits_;
  std::atomic<int64_t> nextHookWaitId_{1};

  /// 登记等待槽 (宿主线程); 返回等待 id
  int64_t beginHookWait(const std::string &instance,
                        const std::shared_ptr<WaitSlot<std::string>> &slot);
  /// 正常完成: 移除等待槽 (JS 侧若再结算, 找不到条目即丢弃)
  void finishHookWait(int64_t waitId);
  /// 超时: 留墓碑, 用于统计迟到的结算 (宿主线程)
  void expireHookWait(int64_t waitId);
  /// 实例停止/卸载时清理它的等待条目
  void clearHookWaitsOf(const std::string &instance);

  std::thread worker_;

  // 说明: 内置槽位表 (`js:<pluginId>` 的合成描述)
  // 与"当前引擎"指针是**进程级静态** (宿主为进程单例), 定义在 js_engine.cpp
  // 的匿名命名空间里。
};

} // namespace extern_plugin
} // namespace musicxx
