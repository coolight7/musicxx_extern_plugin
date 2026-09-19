/// musicxx 外部插件原生宿主: JS 插件运行时 (QuickJS, plan §4.4 / §4.6)
///
/// 设计要点:
/// - **零编译**: 一个 JS 插件 = 一个目录 (`plugin.yaml` + `plugin.js`), 宿主把它注册成
///   内置插件 (合并编译) 的一个**合成实例** (`name = "js:<pluginId>"`), 生命周期
///   (create/start/stop/destroy) 全部由本引擎实现, 因此仍然走内核的完整事务语义;
/// - **共享 1 条 JS 线程** (plan §2.3.1): 全部 JS 插件实例的脚本都在该线程顺序执行;
///   每个实例持有独立 `JSRuntime`/`JSContext` (全局隔离), 只是时间片共享;
/// - **不阻塞宿主线程**: 脚本顶层与处理器里调用宿主能力时, 只做"投递不等待"
///   (动作请求走 [InternalActionRelay], 状态读取直接读互斥保护的镜像);
/// - **顶层注册 + 回放**: 脚本顶层调用 `musicxx.hooks.register` 等注册接口时先在 JS 侧
///   记账; 脚本执行结束、引擎回到宿主线程后再回放注册 —— 避免"宿主线程等 JS、JS 等宿主线程"
///   的互锁 (plan §2.3.2 的无死锁不变式);
/// - **规则与原生插件一致**: 命名空间校验、未知钩子拒绝、禁用/卸载摘除注册、权限(由 Dart
///   侧动作分发时校验) 都沿用同一套约定。
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
class JsEngine final : public InternalActionRelay, public std::enable_shared_from_this<JsEngine> {
public:

    ~JsEngine() override;

    /// 创建引擎 (不启动线程)
    static std::shared_ptr<JsEngine> create(MusicxxHostManager* mgr);

    /// 本次构建是否包含 JS 运行时 (缺 QuickJS 子模块时为 false)
    static bool available();

    /// 启动共享 JS 线程 (幂等)
    int32_t start(std::string& err);

    /// 停止共享 JS 线程 (幂等; 会先释放全部实例上下文)
    void stop();

    bool running() const {
        return running_.load(std::memory_order_acquire);
    }

    /* ---------- 内置插件槽位 (js:<pluginId>) ---------- */

    /// 登记/更新一个 JS 插件的内置槽位 (装载前调用; 宿主线程)
    bool registerBuiltin(
        const std::string& pluginId,
        const std::string& scriptPath,
        const std::string& version,
        std::string&       err
    );

    /// 注销槽位 (卸载完成/装载失败回滚时调用; 宿主线程)
    void unregisterBuiltin(const std::string& pluginId);

    bool hasBuiltin(const std::string& pluginId) const;

    /// 把内置插件提供者交给内核 (进程级; 幂等)
    static void installBuiltinProvider();

    /* ---------- JS 桥接 (primitive 用; 线程安全) ---------- */

    int32_t bridgeStateGet(const std::string& key, std::string& out) const;
    std::string bridgeHostInfo() const;
    void bridgeLog(const std::string& pluginId, int32_t level, const std::string& message);
    int32_t bridgePublish(const std::string& pluginId, const std::string& topic, const std::string& payload);
    int64_t bridgeTimerSet(const std::string& instance, int64_t delayMs, bool repeat);
    void    bridgeTimerClear(const std::string& instance, int64_t timerId);

    /// 刷新宿主信息缓存 (宿主线程: 语言/配置变化时调用)
    void setHostInfoCache(const std::string& json);

    /* ---------- 内部数据结构 (宿主内部使用, 不是对外契约) ---------- */

    /// 一个已注册的钩子处理器 (生命周期 = 实例)
    struct HookHandler {
        JsEngine*   engine   = nullptr;
        std::string instance; ///< js:<id>
        std::string pluginId;
        std::string hookId;
        std::string ownerTag;
        bool        decision = true; ///< true = 裁决型 (hook_sync), false = 观察型 (hook_start)
    };

    /// 一个已声明的能力 (生命周期 = 实例)
    struct CapabilityHandler {
        JsEngine*   engine = nullptr;
        std::string instance;
        std::string pluginId;
        std::string shortName; ///< 脚本里注册的短名
        std::string fullName;  ///< `plugin.<pluginId>.<短名>` (注册与撤销用)
    };

    /// 一个事件订阅的持有者 (回调 ud 指向它; 生命周期 = 实例)
    struct SubscriptionHolder {
        JsEngine*   engine = nullptr;
        std::string instance;
        std::string topic;
    };

    /// 脚本定时器 (plan §7.2 `musicxx.timer.*`)
    struct Timer {
        int64_t     id         = 0;
        std::string instance;
        int64_t     dueMs      = 0;
        int64_t     intervalMs = 0;
        bool        repeat     = false;
    };

    /// 在途动作请求 (JS 侧登记; 任意线程读写, 由 mutex_ 保护)
    struct PendingAction {
        int64_t     requestId = 0;
        std::string instance;
        std::string pluginId;
        std::string action;
        int64_t     deadlineMs = 0;
    };

    /// 一个 JS 插件实例 (JSRuntime/JSContext 只在 JS 线程访问)
    struct Instance {
        JsEngine*   engine = nullptr;
        std::string id;
        std::string name; ///< js:<id>
        std::string scriptPath;
        std::string configPath;
        std::string version;
        std::string argsJson;
        void*       rt  = nullptr; ///< JSRuntime*
        void*       ctx = nullptr; ///< JSContext*

        /// 宿主侧实例指针 (create 时从 host 视图解析; 与实例同生命周期)
        MusicxxHostInstance* hostInst = nullptr;

        std::vector<std::shared_ptr<HookHandler>>         hooks;
        std::vector<std::shared_ptr<CapabilityHandler>>   capabilities;
        std::vector<std::shared_ptr<SubscriptionHolder>>  subscriptions;
        bool registrationsApplied = false;
        bool scriptLoaded         = false;

        std::atomic<int64_t> jsRuns{0};
        std::atomic<int64_t> errors{0};
    };

    /* ---------- 内核内置插件入口 (C ABI 形态; 内核在宿主线程调用) ---------- */

    static const PluginxxInfo* PLUGINXX_CALL builtinGetInfo();
    static int32_t PLUGINXX_CALL                          builtinCreate(
        const PluginxxHost* host,
        void**              outCtx
    );
    static void* PLUGINXX_CALL builtinStart(
        void*                         ctx,
        const PluginxxOperatorNotify* notify,
        PluginxxString*               errorOut
    );
    static void* PLUGINXX_CALL builtinStop(
        void*                         ctx,
        const PluginxxOperatorNotify* notify,
        PluginxxString*               errorOut
    );
    static void PLUGINXX_CALL builtinDestroy(void* ctx);

    /// 内置插件描述数组 (内核要求地址稳定; 固定容量槽位表, 见 js_engine.cpp)
    static const PluginxxBuiltinInfo*     builtinPlugins(uint64_t* count);
    static const PluginxxBuiltinManifest* builtinManifests(uint64_t* count);

    /* ---------- 钩子处理器 (宿主线程调用; 见 HookHandler) ---------- */

    static int32_t PLUGINXX_CALL hookSync(
        void*                     userData,
        const PluginxxStringView* hookId,
        const PluginxxStringView* inputJson,
        PluginxxString*           outJson,
        PluginxxString*           errorOut
    );

    static void* PLUGINXX_CALL hookStart(
        void*                         userData,
        const PluginxxStringView*     hookId,
        const PluginxxStringView*     inputJson,
        const PluginxxOperatorNotify* notify,
        PluginxxString*               errorOut
    );

    /// 能力处理器 (内核在宿主线程调用; 内部投递到 JS 线程并等待结果)
    static void* PLUGINXX_CALL capabilityStart(
        void*                         ctx,
        const PluginxxHost*           callerHost,
        const PluginxxStringView*     method,
        const PluginxxStringView*     argsJson,
        const PluginxxOperatorNotify* notify,
        PluginxxString*               errorOut
    );

    /// 事件订阅回调 (宿主线程调用; 只投递到 JS 线程, 不等待)
    static void PLUGINXX_CALL eventTrampoline(
        const PluginxxStringView* eventJson,
        void*                     userData
    );

    /* ---------- InternalActionRelay (任意线程) ---------- */

    bool respondAction(int64_t requestId, int32_t status, const std::string& resultJson) override;
    void cancelActionsOf(const std::string& instanceName) override;

    /// JS 侧发起动作请求 (任意线程; 不阻塞): 校验命名空间 → 登记 → 推事件给 Dart
    int32_t requestAction(
        MusicxxHostInstance* inst,
        const std::string&   action,
        const std::string&   argsJson,
        uint32_t             timeoutMs,
        int64_t*             outRequestId
    );

    /// 运行时统计 (管理页/调试页)
    std::string statsJson() const;

    /// 实例数 (测试/统计用)
    int32_t instanceCount() const;

private:

    JsEngine();

    /* ---------- JS 线程 ---------- */

    int32_t  startThread(std::string& err);
    void     runLoop();
    void     postTask(std::function<void()> fn);
    int64_t  nextWaitMsLocked() const;
    void     fireDueTimers();
    void     expireActions();

    /* ---------- 实例生命周期 (宿主线程调用; 内部经 JS 线程执行脚本) ---------- */

    static std::shared_ptr<Instance> instanceFromHost(const PluginxxHost* host, std::string& err);
    /// 按实例名取实例 (任意线程; 保活返回)
    std::shared_ptr<Instance> lookupInstance(const std::string& name) const;
    int32_t startInstance(const std::shared_ptr<Instance>& inst, std::string& err);
    void    stopInstance(const std::shared_ptr<Instance>& inst);
    void    destroyInstance(const std::shared_ptr<Instance>& inst);

    /// 在 JS 线程上建 runtime/context, 装好 `musicxx` 全局并执行脚本顶层
    bool runScriptOnJsThread(const std::shared_ptr<Instance>& inst, std::string& err);

    /// 回放脚本顶层登记的注册 (宿主线程; 钩子/能力/事件订阅)
    void applyRegistrations(const std::shared_ptr<Instance>& inst);
    /// 摘除本实例的全部注册 (宿主线程)
    void detachRegistrations(const std::shared_ptr<Instance>& inst);

    /* ---------- JS 调用桥 (只在 JS 线程调用) ---------- */

    /// 调用实例里的全局函数并取字符串结果 (`__ext.<fnName>`); 失败返回 false
    bool callBridgeString(const std::shared_ptr<Instance>& inst, const char* fnName,
                          const std::vector<std::string>& args, std::string& out);

    /// 记录统计 + 日志 (任意线程)
    void logPlugin(const std::string& pluginId, int32_t level, const std::string& message);

    MusicxxHostManager* mgr_ = nullptr;

    /// 宿主信息缓存 (start/语言变化时刷新; 供 JS 侧同步读, 避免跨线程读管理器字段)
    mutable std::mutex hostInfoMutex_;
    std::string        hostInfoJson_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};

    mutable std::mutex      mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> tasks_;
    std::vector<Timer>      timers_;
    int64_t                 nextTimerId_ = 1;

    /// 已装载的实例 (key = js:<id>; 任意线程读写)
    std::map<std::string, std::shared_ptr<Instance>, std::less<>> instances_;

    /// 在途动作请求 (任意线程读写)
    std::map<int64_t, PendingAction> pendingActions_;
    std::atomic<int64_t>             nextRequestId_{1};

    std::thread worker_;

    // 说明: 内置槽位表 (`js:<pluginId>` 的合成描述) 与"当前引擎"指针是**进程级静态**
    // (宿主为进程单例, plan §4.4), 定义在 js_engine.cpp 的匿名命名空间里。
};

} // namespace extern_plugin
} // namespace musicxx
