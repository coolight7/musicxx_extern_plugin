/// musicxx 外部插件原生宿主: 管理器核心 (配置/启停/事件/领域钩子/生命周期接缝/插件管理)

#include "host_json.h"
#include "host_naming.h"
#include "js/js_engine.h"
#include "musicxx_host.h"

#include "pluginxx/host/abi_util.h"
#include "pluginxx/host/manifest.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include "yaml-cpp/yaml.h"

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/thread_pool.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace musicxx {
namespace extern_plugin {

namespace fs = std::filesystem;

using utilxx_base::Json;

namespace {

int64_t nowWallMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()
    )
        .count();
}

/// steady 时钟毫秒 (速率窗口/耗时统计用; 不受系统时间调整影响)
int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()
    )
        .count();
}

/// 当前线程标识文本 (诊断日志用)
std::string currentThreadIdText() {
    std::ostringstream oss;
    oss << std::this_thread::get_id();
    return oss.str();
}

/* ==================== 开发者日志 sink ==================== */

/// 宿主侧日志输出口 (stderr)
///
/// 背景: 宿主库静态链入 cxx_utilxx_base, 它自带日志分发器**但默认没有任何 sink**,
/// 因此原生侧出问题时"完全没有日志可看"(Dart 未接入时尤其明显)。
/// 这里提供最小可用的诊断出口: 环境变量 `MUSICXX_EXTERN_PLUGIN_LOG_STDERR=1`
/// 时把宿主与插件的日志写到 stderr; 正式落盘/转发由 Dart 侧配置 (plan §9.3)。
class StderrLogSink final : public utilxx_base::ThreadedLogSink {
public:

    /// 必须在最派生类析构里停线程 (基类析构时虚表已切换, 见 ThreadedLogSink 说明)
    ~StderrLogSink() override {
        shutdownThread();
    }

protected:

    void onLog(const utilxx_base::LogEntry& entry) override {
        std::fprintf(
            stderr,
            "[musicxx_ext][%s] %s\n",
            levelName(entry.level),
            entry.message.c_str()
        );
        std::fflush(stderr);
    }

private:

    static const char* levelName(utilxx_base::LogLevel level) {
        switch (level) {
            case utilxx_base::LogLevel::Trace:
                return "trace";
            case utilxx_base::LogLevel::Debug:
                return "debug";
            case utilxx_base::LogLevel::Info:
                return "info";
            case utilxx_base::LogLevel::Warn:
                return "warn";
            case utilxx_base::LogLevel::Error:
                return "error";
            default:
                return "out";
        }
    }
};

/// 安装 stderr 日志 sink (幂等)
///
/// - 注册方必须持有 shared_ptr (分发器只持 weak_ptr), 因此把强引用放在**故意泄漏**的
///   静态持有者里: 避免进程退出时 sink 与分发器的静态析构顺序问题;
/// - 仅当环境变量显式打开时安装, 默认零开销 (不写任何输出)。
void ensureStderrLogSink() {
    static std::shared_ptr<StderrLogSink>* holder = nullptr;
    if (holder) {
        return;
    }
    const char* env = std::getenv("MUSICXX_EXTERN_PLUGIN_LOG_STDERR");
    if (!env || *env == '\0' || std::string_view{env} == "0") {
        holder = new std::shared_ptr<StderrLogSink>{}; ///< 记住"已检查过", 不重复判断
        return;
    }
    holder = new std::shared_ptr<StderrLogSink>{std::make_shared<StderrLogSink>()};
    utilxx_base::LogDispatcher::instance().addSink(*holder);
    std::fprintf(stderr, "[musicxx_ext] stderr 日志已开启 (MUSICXX_EXTERN_PLUGIN_LOG_STDERR)\n");
}

std::string readFileText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

/* ==================== 插件事件总线 (插件 ↔ 宿主) ==================== */

/// 进程内主题事件总线 (pluginxx `events` 表的后端)
///
/// - 与 Dart 事件队列 (pushEvent/pollEvents) 是**两条独立通道**: 本总线服务插件之间
///   与"宿主 → 插件"的事件; Dart 侧事件仍走有界队列;
/// - 订阅/撤销在宿主线程簿记, 回调在**发布者线程**执行 (内核发布入口已投递到宿主线程),
///   因此回调里不得阻塞 (与钩子处理器同一纪律);
/// - 回调在锁外执行: 处理器内部再次订阅/撤销/发布不会自锁。
class TopicEventBus final : public pluginxx::EventSource {
public:

    size_t subscribe(std::string_view topic, std::function<void(std::string_view)> handler)
        override {
        if (topic.empty() || !handler) {
            return 0; ///< 0 = 订阅失败 (内核会记录并拒绝该主题)
        }
        std::lock_guard<std::mutex> lock{mutex_};
        const size_t                id = ++nextId_;
        subscribers_.push_back(Subscriber{std::string{topic}, id, std::move(handler)});
        return id;
    }

    void unsubscribe(std::string_view topic, size_t subscriptionId) override {
        std::lock_guard<std::mutex> lock{mutex_};
        std::erase_if(subscribers_, [&](const Subscriber& sub) {
            return sub.id == subscriptionId && sub.topic == topic;
        });
    }

    int publish(std::string_view topic, std::string_view eventJson) override {
        if (topic.empty()) {
            return -1;
        }
        std::vector<std::function<void(std::string_view)>> targets;
        {
            std::lock_guard<std::mutex> lock{mutex_};
            for (const auto& sub : subscribers_) {
                if (sub.topic == topic) {
                    targets.push_back(sub.handler);
                }
            }
        }
        for (const auto& fn : targets) {
            try {
                fn(eventJson);
            } catch (const std::exception& e) {
                XX_LOGW("[musicxx_ext] 事件处理器异常 (topic={}): {}", topic, e.what());
            } catch (...) {
                XX_LOGW("[musicxx_ext] 事件处理器未知异常 (topic={})", topic);
            }
        }
        return 0;
    }

    /// 某主题当前订阅者数量 (用于"没人订阅就别推"的快速判断)
    int32_t subscriberCount(std::string_view topic) const {
        std::lock_guard<std::mutex> lock{mutex_};
        int32_t                     count = 0;
        for (const auto& sub : subscribers_) {
            if (sub.topic == topic) {
                ++count;
            }
        }
        return count;
    }

private:

    struct Subscriber {
        std::string                           topic;
        size_t                                id = 0;
        std::function<void(std::string_view)> handler;
    };

    mutable std::mutex      mutex_;
    std::vector<Subscriber> subscribers_;
    size_t                  nextId_ = 0;
};

std::string hostArch() {
#if defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
    return "x64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#else
    return "unknown";
#endif
}

std::string viewToString(const MusicxxExternPluginStringView& v) {
    if (!v.data || v.size == 0) {
        return {};
    }
    return std::string{v.data, static_cast<size_t>(v.size)};
}

/* ==================== 插件清单读取 (JS 判定用) ==================== */

/// 读 `plugin.yaml` 的 `kind` 字段 (空 = 未声明, 由调用方按 entry 推导)
std::string readManifestKind(const fs::path& dir) {
    const std::string text = readFileText(dir / "plugin.yaml");
    if (text.empty()) {
        return {};
    }
    try {
        auto node = YAML::Load(text);
        if (node["kind"] && node["kind"].IsScalar()) {
            return node["kind"].as<std::string>();
        }
    } catch (const std::exception& e) {
        XX_LOGW("[musicxx_ext] 读取插件 `{}` 的 kind 失败: {}", dir.string(), e.what());
    }
    return {};
}

/// 读 `plugin.yaml` 的 `entry` 字段 (JS 插件可能写成 plugin.js)
std::string readManifestEntry(const fs::path& dir) {
    const std::string text = readFileText(dir / "plugin.yaml");
    if (text.empty()) {
        return {};
    }
    try {
        auto node = YAML::Load(text);
        if (node["entry"] && node["entry"].IsScalar()) {
            return node["entry"].as<std::string>();
        }
    } catch (const std::exception&) {
        // 解析失败按无 entry 处理 (真正的清单校验在装载路径)
    }
    return {};
}

/// 读 `plugin.yaml` 的 `version` 字段
std::string readManifestVersion(const fs::path& dir) {
    const std::string text = readFileText(dir / "plugin.yaml");
    if (text.empty()) {
        return {};
    }
    try {
        auto node = YAML::Load(text);
        if (node["version"] && node["version"].IsScalar()) {
            return node["version"].as<std::string>();
        }
    } catch (const std::exception&) {
        // 同上
    }
    return {};
}

} // namespace

/* ==================== HostContext ==================== */

HostContext::HostContext() :
    guard(asio::make_work_guard(io)) {}

HostContext::~HostContext() {
    guard.reset();
    io.stop();
    if (thread.joinable()) {
        thread.join();
    }
    if (workers) {
        workers->stop();
        workers->join();
        workers.reset();
    }
}

/* ==================== 创建 / 配置 ==================== */

MusicxxHostManager::MusicxxHostManager(asio::any_io_executor ex) :
    Base(std::move(ex)) {
    // 事件后端在构造期就绪 (DomainHooks 的任何入口都可能被调用)
    eventBus_ = std::make_shared<TopicEventBus>();
}

std::shared_ptr<MusicxxHostManager> MusicxxHostManager::create(
    HostContext&                         ctx,
    const MusicxxExternPluginHostConfig& cfg,
    std::string&                         err
) {
    if (cfg.struct_size != 0 && cfg.struct_size < sizeof(MusicxxExternPluginHostConfig)) {
        err = "host_create: config struct_size 不匹配 (Dart 与原生库版本不一致?)";
        return nullptr;
    }
    ensureStderrLogSink();
    auto mgr   = std::shared_ptr<MusicxxHostManager>(new MusicxxHostManager(ctx.io.get_executor()));
    mgr->ctx_  = &ctx;
    mgr->setDomainHooks(mgr.get());
    mgr->applyConfig(cfg);
    return mgr;
}

void MusicxxHostManager::applyConfig(const MusicxxExternPluginHostConfig& cfg) {
    auto setIfPresent = [](std::string& dst, const MusicxxExternPluginStringView& v) {
        const auto s = viewToString(v);
        if (!s.empty()) {
            dst = s;
        }
    };
    setIfPresent(appVersion_, cfg.app_version);
    setIfPresent(platform_, cfg.platform);
    setIfPresent(language_, cfg.language);
    setIfPresent(userPluginDir_, cfg.user_plugin_dir);
    setIfPresent(builtinPluginDir_, cfg.builtin_plugin_dir);
    setIfPresent(dataDir_, cfg.data_dir);
    setIfPresent(logDir_, cfg.log_dir);

    logLevel_ = cfg.log_level;
    flags_    = cfg.flags;
    if (cfg.event_queue_capacity > 0) {
        eventCapacity_ = cfg.event_queue_capacity;
    }
    XX_LOGI(
        "[musicxx_ext] host config: app={} platform={} arch={} lang={} flags={}",
        appVersion_,
        platform_,
        hostArch(),
        language_,
        flags_
    );
}

/* ==================== 启停 ==================== */

int32_t MusicxxHostManager::start(std::string& err) {
    if (running_.load(std::memory_order_acquire)) {
        return MUSICXX_EXTERN_PLUGIN_OK;
    }
    if (!ctx_) {
        err = "host_start: host context missing";
        return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }

    // 工作线程池 (插件 offload 用) + 宿主线程 (唯一原生插件执行线程)
    if (!ctx_->workers) {
        ctx_->workers = std::make_unique<asio::thread_pool>(2);
    }
    if (!ctx_->thread.joinable()) {
        ctx_->thread = std::thread([this] {
            try {
                ctx_->io.run();
            } catch (const std::exception& e) {
                XX_LOGE("[musicxx_ext] host io thread exited: {}", e.what());
            }
        });
    }
    running_.store(true, std::memory_order_release);
    startTime_ = std::chrono::steady_clock::now();

    // 绑定 IO 线程标识 (关键): 管理器是在**调用方线程**构造的 (host_create), 内核
    // PluginManagerBase 记录的是构造线程; 若不在宿主线程上重绑, 各 vtable 入口会误判
    // "当前不在 IO 线程" → 投递回宿主线程并同步等待, 而宿主线程正卡在该调用栈里
    // (插件 start/create/处理器都在这条线程上跑) → 单线程执行器自锁。
    if (!bindIoThread()) {
        running_.store(false, std::memory_order_release);
        err = "host_start: 宿主 IO 线程未在预算内就绪";
        return MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT;
    }

    // 宿主内运行时: JS 线程 (plan §4.6; 全部 JS 插件实例共享这 1 条线程)
    // 启动失败不影响原生插件: 记事件 + 禁用 JS 能力
    if ((flags_ & MUSICXX_EXTERN_PLUGIN_FLAG_NO_JS) == 0 && JsEngine::available()) {
        jsEngine_ = JsEngine::create(this);
        std::string jsErr;
        if (jsEngine_->start(jsErr) != MUSICXX_EXTERN_PLUGIN_OK) {
            XX_LOGE("[musicxx_ext] JS 运行时未启动: {}", jsErr);
            pushHostError("js_runtime_unavailable", jsErr);
            jsEngine_.reset();
        } else {
            refreshJsHostInfo();
        }
    } else if ((flags_ & MUSICXX_EXTERN_PLUGIN_FLAG_NO_JS) != 0) {
        XX_LOGI("[musicxx_ext] 宿主配置禁用了 JS 插件 (NO_JS)");
    } else {
        XX_LOGW("[musicxx_ext] 本次构建不含 JS 运行时, JS 插件不会加载");
    }

    {
        Json payload;
        payload["apiVersion"]       = MUSICXX_EXTERN_PLUGIN_API_VERSION;
        payload["version"]          = "0.1.0";
        payload["platform"]         = platform_;
        payload["arch"]             = hostArch();
        payload["appVersion"]       = appVersion_;
        payload["language"]         = language_;
        payload["userPluginDir"]    = userPluginDir_;
        payload["builtinPluginDir"] = builtinPluginDir_;
        payload["flags"]            = flags_;
        payload["jsRuntime"]        = jsEngine_ != nullptr;
        pushEvent("musicxx.host.ready", "", payload.dump());
    }
    XX_LOGI(
        "[musicxx_ext] host started (platform={}, safeMode={}, noNative={}, noJs={}, jsRuntime={})",
        platform_,
        (flags_ & MUSICXX_EXTERN_PLUGIN_FLAG_SAFE_MODE) != 0,
        (flags_ & MUSICXX_EXTERN_PLUGIN_FLAG_NO_NATIVE) != 0,
        (flags_ & MUSICXX_EXTERN_PLUGIN_FLAG_NO_JS) != 0,
        jsEngine_ != nullptr
    );
    return MUSICXX_EXTERN_PLUGIN_OK;
}

bool MusicxxHostManager::bindIoThread() {
    if (!ctx_) {
        return false;
    }
    if (ioThreadBound_.load(std::memory_order_acquire)) {
        return true; ///< 本次运行已绑定 (start 幂等)
    }
    // 注意: 这里**不能**用 `isIoThread()` 当快速路径 —— 内核记录的是"调用
    // setIoExecutor 的线程", 而管理器是在宿主线程之外的线程构造的 (host_create),
    // 因此调用线程上 isIoThread() 恰好为 true (误判)。必须无条件投递一次绑定。
    auto                     self = shared_from_this();
    auto                     slot = std::make_shared<WaitSlot<bool>>();
    const asio::any_io_executor ex = ctx_->io.get_executor();
    asio::post(ex, [self, slot, ex] {
        self->setIoExecutor(ex); ///< 记录"当前线程"= 宿主 IO 线程
        XX_LOGI(
            "[musicxx_ext] 宿主 IO 线程已绑定 (thread={}, 原生插件代码只在该线程执行)",
            currentThreadIdText()
        );
        slot->set(true);
    });
    bool ok = false;
    if (!slot->wait(5000, ok) || !ok) {
        return false;
    }
    ioThreadBound_.store(true, std::memory_order_release);
    if (isIoThread()) {
        // 绑定成功后调用线程仍被判为 IO 线程: 说明执行器被绑到了调用线程本身
        // (宿主线程没跑起来 / 记录被覆盖) —— 这时 vtable 入口会在错误的线程就地执行
        XX_LOGW("[musicxx_ext] IO 线程绑定异常: 调用线程被判定为 IO 线程");
    }
    return true;
}

int32_t MusicxxHostManager::stop(uint32_t timeoutMs, std::string& err) {
    (void)err;
    if (!ctx_ || !running_.load(std::memory_order_acquire)) {
        return MUSICXX_EXTERN_PLUGIN_OK;
    }
    auto       slot   = std::make_shared<WaitSlot<bool>>();
    auto       self   = shared_from_this();
    const auto budget = std::chrono::milliseconds{static_cast<int64_t>((std::max)(timeoutMs, 1000u))};
    asio::co_spawn(
        ctx_->io,
        [self, slot, budget]() -> asio::awaitable<void> {
            const bool ok = co_await self->shutdownAsync(budget);
            slot->set(ok);
        },
        asio::detached
    );

    bool ok = false;
    if (!slot->wait(timeoutMs == 0 ? 30000 : timeoutMs + 500, ok) || !ok) {
        XX_LOGW("[musicxx_ext] host stop: 插件卸载未在预算内完成 (可再次调用重试)");
    }

    running_.store(false, std::memory_order_release);
    // JS 运行时: 插件已全部卸载 (上面的 shutdownAsync), 这里释放共享 JS 线程
    if (jsEngine_) {
        jsEngine_->stop();
        jsEngine_.reset();
    }
    if (ctx_->workers) {
        ctx_->workers->stop();
        ctx_->workers->join();
        ctx_->workers.reset();
    }
    ctx_->guard.reset();
    ctx_->io.stop();
    if (ctx_->thread.joinable()) {
        ctx_->thread.join();
    }
    // v1: 宿主为一次性对象 (start 之后 stop 不再重启); 需要重新启动时由 Dart 侧销毁宿主并重建
    XX_LOGI("[musicxx_ext] host stopped");
    return MUSICXX_EXTERN_PLUGIN_OK;
}

MusicxxHostManager::~MusicxxHostManager() {
    XX_LOGI("[musicxx_ext] host manager destroyed");
}

/* ==================== 事件队列 ==================== */

void MusicxxHostManager::setWake(MusicxxExternPluginWakeFn fn, void* userData) {
    std::lock_guard<std::mutex> lock{eventMutex_};
    wakeFn_   = fn;
    wakeUser_ = userData;
}

void MusicxxHostManager::pushEvent(
    const std::string& type,
    const std::string& plugin,
    const std::string& payloadJson
) {
    MusicxxExternPluginWakeFn wake   = nullptr;
    void*                     wakeUd = nullptr;
    {
        std::lock_guard<std::mutex> lock{eventMutex_};
        Json                        ev;
        ev["seq"]     = ++eventSeq_;
        ev["ts"]      = nowWallMs();
        ev["type"]    = type;
        ev["plugin"]  = plugin;
        Json payload  = payloadJson.empty() ? Json::object() : parseJsonSafe(payloadJson);
        ev["payload"] = payload;

        if (events_.size() >= eventCapacity_) {
            // 有界队列: 丢最旧并补发宿主错误 (plan §4.10)
            events_.pop_front();
            ++eventDropped_;
            Json overflow;
            overflow["seq"]     = ++eventSeq_;
            overflow["ts"]      = nowWallMs();
            overflow["type"]    = "musicxx.host.error";
            overflow["plugin"]  = "";
            overflow["payload"] = {
                {"code", "event_queue_overflow"},
                {"message", "事件队列已满, 已丢弃最旧事件"},
                {"dropped", eventDropped_},
            };
            events_.push_back(overflow.dump());
        }
        events_.push_back(ev.dump());
        wake   = wakeFn_;
        wakeUd = wakeUser_;
    }
    if (wake) {
        wake(wakeUd);
    }
}

void MusicxxHostManager::pushHostError(const std::string& code, const std::string& message) {
    Json payload;
    payload["code"]    = code;
    payload["message"] = message;
    pushEvent("musicxx.host.error", "", payload.dump());
}

/* ==================== 宿主内运行时 (JS 引擎) 接驳 ==================== */

void MusicxxHostManager::setInternalActionRelay(std::shared_ptr<InternalActionRelay> relay) {
    std::lock_guard<std::mutex> lock{registryMutex_};
    internalRelay_ = std::move(relay);
}

int64_t MusicxxHostManager::allocateRequestId() {
    return nextRequestId_.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<JsEngine> MusicxxHostManager::jsEngine() const {
    std::lock_guard<std::mutex> lock{registryMutex_};
    return jsEngine_;
}

void MusicxxHostManager::refreshJsHostInfo() {
    auto engine = jsEngine();
    if (!engine) {
        return;
    }
    std::string info;
    hostInfoJson(info);
    engine->setHostInfoCache(info);
}

int32_t MusicxxHostManager::pushActionRequestEvent(
    const std::string& pluginId,
    int64_t            requestId,
    const std::string& action,
    const std::string& argsJson,
    uint32_t           timeoutMs,
    int64_t*           outEffectiveTimeoutMs
) {
    if (action.empty()) {
        return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
    }
    // 命名空间校验 (plan §3.5): 官方 musicxx.* 或本插件自己的 plugin.<id>.*
    if (!naming::isOfficial(action) && !naming::isOwnPlugin(action, pluginId)) {
        XX_LOGW("[musicxx_ext] 插件 `{}` 发起非法动作名 `{}` (命名空间校验失败)", pluginId, action);
        return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
    }
    // 超时预算 (plan §4.8): 默认 5s, 下限 1s (避免 Dart 侧被高频请求淹没), 上限 60s
    const int64_t effectiveTimeoutMs = std::clamp<int64_t>(
        (timeoutMs == 0) ? 5000 : static_cast<int64_t>(timeoutMs),
        1000,
        60000
    );
    Json payload;
    payload["requestId"] = requestId;
    payload["plugin"]    = pluginId;
    payload["action"]    = action;
    payload["timeoutMs"] = effectiveTimeoutMs;
    payload["args"]      = parseJsonSafe(argsJson);
    pushEvent("musicxx.action.request", pluginId, payload.dump());
    if (outEffectiveTimeoutMs) {
        *outEffectiveTimeoutMs = effectiveTimeoutMs;
    }
    return MUSICXX_EXTERN_PLUGIN_OK;
}

void MusicxxHostManager::cancelActionsOfInstance(const std::string& instanceName) {
    std::shared_ptr<InternalActionRelay> relay;
    {
        std::lock_guard<std::mutex> lock{registryMutex_};
        relay = internalRelay_;
    }
    if (relay) {
        relay->cancelActionsOf(instanceName);
    }
}

int32_t MusicxxHostManager::prepareJsPlugin(
    const std::string& pluginId,
    const std::string& pluginDir,
    const std::string& entryHint,
    const std::string& version,
    std::string&       err
) {
    auto engine = jsEngine();
    if (!engine) {
        err = (flags_ & MUSICXX_EXTERN_PLUGIN_FLAG_NO_JS) != 0
                  ? "宿主配置禁用了 JS 插件"
                  : "JS 运行时不可用 (未编译或启动失败)";
        return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
    if ((flags_ & MUSICXX_EXTERN_PLUGIN_FLAG_SAFE_MODE) != 0) {
        err = "安全模式: 本次启动不加载任何外部插件";
        return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
    const fs::path dir{pluginDir};
    fs::path       script;
    if (!entryHint.empty() && entryHint.size() > 3
        && entryHint.compare(entryHint.size() - 3, 3, ".js") == 0) {
        script = dir / entryHint;
    }
    if (script.empty() || !fs::exists(script)) {
        script = dir / "plugin.js";
    }
    std::error_code ec;
    if (!fs::is_regular_file(script, ec)) {
        err = "JS 插件缺少脚本文件: " + script.string();
        return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
    }
    if (!engine->registerBuiltin(pluginId, script.string(), version, err)) {
        return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
    return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::publishPluginEventFrom(
    const std::string& pluginId,
    const std::string& topic,
    const std::string& payloadJson
) {
    if (!running_.load(std::memory_order_acquire)) {
        return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
    // 归属校验 (plan §3.5): 插件只能发布官方主题或自己命名空间下的主题
    if (!naming::isTopicOwnedBy(topic, pluginId)) {
        XX_LOGW(
            "[musicxx_ext] 插件 `{}` 发布事件被拒绝: 主题 `{}` 不属于该插件命名空间",
            pluginId,
            topic
        );
        return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
    }
    const std::string payload = payloadJson.empty() ? std::string{"{}"} : payloadJson;
    auto              self    = shared_from_this();
    // 事件总线的订阅/发布约定在宿主线程: 投递后立即返回 (调用方通常是 JS 线程)
    asio::post(ctx_->io, [self, topic, payload] { self->publishOnHostThread(topic, payload); });
    return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::pollEvents(int32_t maxCount, std::string& outJson) {
    std::lock_guard<std::mutex> lock{eventMutex_};
    if (events_.empty()) {
        outJson.clear();
        return MUSICXX_EXTERN_PLUGIN_OK;
    }
    const size_t take = (maxCount <= 0)
                            ? events_.size()
                            : (std::min)(static_cast<size_t>(maxCount), events_.size());
    std::string joined = "[";
    for (size_t i = 0; i < take; ++i) {
        if (i != 0) {
            joined.push_back(',');
        }
        joined += events_.front();
        events_.pop_front();
    }
    joined.push_back(']');
    outJson = std::move(joined);
    return MUSICXX_EXTERN_PLUGIN_OK;
}

/* ==================== DomainHooks ==================== */

bool MusicxxHostManager::postToWorkerThread(std::function<void()> fn) {
    if (!ctx_ || !ctx_->workers) {
        return false;
    }
    asio::post(*ctx_->workers, std::move(fn));
    return true;
}

std::string MusicxxHostManager::configJson() {
    Json j;
    j["appVersion"] = appVersion_;
    j["platform"]   = platform_;
    j["arch"]       = hostArch();
    j["language"]   = language_;
    j["dataDir"]    = dataDir_;
    j["logDir"]     = logDir_;
    j["pluginDirs"] = Json::array();
    if (!userPluginDir_.empty()) {
        j["pluginDirs"].push_back(userPluginDir_);
    }
    if (!builtinPluginDir_.empty()) {
        j["pluginDirs"].push_back(builtinPluginDir_);
    }
    j["apiVersion"] = MUSICXX_EXTERN_PLUGIN_API_VERSION;
    return j.dump();
}

std::string MusicxxHostManager::toolPromptJson(std::string_view /*toolName*/) {
    return {};
}

std::string MusicxxHostManager::sessionWorkDir(std::string_view /*sessionId*/) {
    return dataDir_;
}

std::string MusicxxHostManager::language() {
    return language_;
}

void MusicxxHostManager::setLanguage(std::string_view lang) {
    language_ = std::string{lang};
}

bool MusicxxHostManager::isSessionCancelled(std::string_view /*sessionId*/) {
    return false;
}

std::string MusicxxHostManager::pluginsJson() {
    std::string json = "[";
    bool        first = true;
    for (const auto& [name, inst] : loaded_) {
        if (!inst) {
            continue;
        }
        if (!first) {
            json.push_back(',');
        }
        first = false;
        json += pluginInstanceJson(*inst);
    }
    json.push_back(']');
    return json;
}

std::string MusicxxHostManager::pluginJson(std::string_view name) {
    for (const auto& [instName, inst] : loaded_) {
        if (instName == name && inst) {
            return pluginInstanceJson(*inst);
        }
    }
    return {};
}

std::string MusicxxHostManager::pluginInstanceJson(const MusicxxHostInstance& inst) const {
    Json j;
    j["id"]          = pluginIdOf(inst.name);
    j["instance"]    = inst.name;
    j["kind"]        = inst.kind;
    j["version"]     = inst.version;
    j["description"] = inst.description;
    j["path"]        = inst.path;
    j["source"]      = "loaded"; ///< 已加载项: 与扫描项的 user/builtin 区分
    j["loaded"]      = true;
    j["valid"]       = true;
    j["supported"]   = true;
    j["enabled"]     = inst.enabled;
    j["configPath"]  = inst.configPath;
    j["counters"]    = {
        {"hookCalls", inst.hookCalls.load()},
        {"hookTimeouts", inst.hookTimeouts.load()},
        {"actionRequests", inst.actionRequests.load()},
        {"events", inst.eventsPublished.load()},
        {"errors", inst.errors.load()},
    };
    return j.dump();
}

/* ==================== 生命周期接缝 ==================== */

std::shared_ptr<pluginxx::PluginHostLifecycle<MusicxxHostInstance>> MusicxxHostManager::selfRef() {
    return shared_from_this();
}

std::shared_ptr<MusicxxHostInstance> MusicxxHostManager::createInstance(std::string name) {
    auto inst    = std::make_shared<MusicxxHostInstance>(std::move(name));
    inst->manager = shared_from_this();
    return inst;
}

pluginxx::PluginEntrySymbols MusicxxHostManager::entrySymbols() const {
    return {
        MUSICXX_PLUGIN_SYMBOL_GET_INFO,
        MUSICXX_PLUGIN_SYMBOL_CREATE,
        MUSICXX_PLUGIN_SYMBOL_START,
        MUSICXX_PLUGIN_SYMBOL_STOP,
    };
}

std::string MusicxxHostManager::pluginIdOf(std::string_view instanceName) {
    // JS 插件实例名形如 "js:<pluginId>" (plan §4.4); 其余即插件 id 本身
    if (instanceName.size() > 3 && instanceName.substr(0, 3) == "js:") {
        return std::string{instanceName.substr(3)};
    }
    return std::string{instanceName};
}

std::shared_ptr<MusicxxHostInstance>
    MusicxxHostManager::resolveInstance(const std::string& idOrInstance) const {
    if (idOrInstance.empty()) {
        return nullptr;
    }
    // 先按实例名找 (原生插件: 实例名 = 插件 id; JS 插件: "js:<id>")
    if (auto inst = find(idOrInstance)) {
        return inst;
    }
    // 再按插件 id 找 JS 合成实例
    if (idOrInstance.rfind("js:", 0) != 0) {
        if (auto inst = find("js:" + idOrInstance)) {
            return inst;
        }
    }
    return nullptr;
}

void MusicxxHostManager::detachDomainRegistrations(MusicxxHostInstance* inst) {
    if (!inst) {
        return;
    }
    clearPluginRegistrations(inst->name);
}

void MusicxxHostManager::clearDomainRegistrations(MusicxxHostInstance* inst) {
    if (!inst) {
        return;
    }
    clearPluginRegistrations(inst->name);
}

void MusicxxHostManager::onInstanceLoaded(MusicxxHostInstance& inst) {
    loaded_[inst.name] = find(inst.name);
    /// 速率窗口与平均速率的时间基准 (统计只观测不限制)
    inst.loadedAtMs          = steadyNowMs();
    inst.eventsWindowStartMs = inst.loadedAtMs;
    inst.eventsWindowCount   = 0;
    inst.eventsPerSec        = 0;
    Json payload;
    payload["id"]         = pluginIdOf(inst.name);
    payload["instance"]   = inst.name;
    payload["kind"]       = inst.kind;
    payload["version"]    = inst.version;
    payload["path"]       = inst.path;
    payload["configPath"] = inst.configPath;
    payload["ok"]         = true;
    pushEvent("musicxx.plugin.loaded", pluginIdOf(inst.name), payload.dump());
}

void MusicxxHostManager::onInstanceUnloaded(MusicxxHostInstance& inst) {
    clearPluginRegistrations(inst.name);
    loaded_.erase(inst.name);
    Json payload;
    payload["id"]       = pluginIdOf(inst.name);
    payload["instance"] = inst.name;
    pushEvent("musicxx.plugin.unloaded", pluginIdOf(inst.name), payload.dump());
}

void MusicxxHostManager::onInstanceEnabledChanged(MusicxxHostInstance& inst, bool enabled) {
    if (!enabled) {
        clearPluginRegistrations(inst.name);
    }
    Json payload;
    payload["id"]       = pluginIdOf(inst.name);
    payload["instance"] = inst.name;
    payload["enabled"]  = enabled;
    pushEvent(
        enabled ? "musicxx.plugin.enabled" : "musicxx.plugin.disabled",
        pluginIdOf(inst.name),
        payload.dump()
    );
}

void MusicxxHostManager::clearPluginRegistrations(const std::string& instanceName) {
    for (auto it = hooks_.begin(); it != hooks_.end();) {
        auto& handlers = it->second;
        handlers.erase(
            std::remove_if(
                handlers.begin(),
                handlers.end(),
                [&](const HookHandler& h) { return h.plugin == instanceName; }
            ),
            handlers.end()
        );
        if (handlers.empty()) {
            it = hooks_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = pendingActions_.begin(); it != pendingActions_.end();) {
        if (it->second.plugin == instanceName) {
            if (it->second.timer) {
                it->second.timer->cancel(); ///< 实例卸载: 撤销超时保护
            }
            if (it->second.notify && it->second.notify->done) {
                it->second.notify->done(
                    it->second.notify->host_ud,
                    PLUGINXX_OPERATOR_CANCELLED,
                    nullptr
                );
            }
            it = pendingActions_.erase(it);
        } else {
            ++it;
        }
    }
    stateSubscriptions_.erase(instanceName);
    // 声明式 UI 项随实例摘除 (plan §5.6: 卸载后不再渲染该插件的入口/菜单)
    detachUiEntries(instanceName);
    // JS 引擎登记的在途动作请求 (JS 侧自己保活) 也要一起取消
    cancelActionsOfInstance(instanceName);
}

/* ==================== 插件管理 ==================== */

int32_t MusicxxHostManager::scanPlugins(std::string& outJson, std::string& err) {    if (!running_.load(std::memory_order_acquire)) {
        err = "plugin_scan: host not started";
        return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
    std::string json  = "[";
    bool        first = true;
    auto        append = [&](const std::string& item) {
        if (item.empty()) {
            return;
        }
        if (!first) {
            json.push_back(',');
        }
        first = false;
        json += item;
    };

    std::vector<std::pair<std::string, std::string>> dirs;
    if (!userPluginDir_.empty()) {
        dirs.emplace_back(userPluginDir_, "user");
    }
    if (!builtinPluginDir_.empty()) {
        dirs.emplace_back(builtinPluginDir_, "builtin");
    }

    discovered_.clear();
    for (const auto& [dir, source] : dirs) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            continue;
        }
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (ec) {
                break;
            }
            if (!entry.is_directory()) {
                continue;
            }
            append(scanDir(entry.path().string(), source));
        }
    }
    json.push_back(']');
    outJson = std::move(json);
    return MUSICXX_EXTERN_PLUGIN_OK;
}

std::string MusicxxHostManager::scanDir(const std::string& dir, const std::string& kindHint) {
    const fs::path dirPath{dir};
    const fs::path manifestPath = dirPath / "plugin.yaml";
    std::error_code ec;
    if (!fs::is_regular_file(manifestPath, ec)) {
        return {};
    }
    const std::string yamlText = readFileText(manifestPath);
    if (yamlText.empty()) {
        Json bad;
        bad["id"]     = dirPath.filename().string();
        bad["path"]   = dir;
        bad["valid"]  = false;
        bad["error"]  = "plugin.yaml 为空";
        bad["source"] = kindHint;
        return bad.dump();
    }

    // 内核解析 name/entry/depends (权威路径); 扩展字段本宿主用 yaml-cpp 读
    std::string              name;
    std::string              entry;
    std::vector<std::string> depends;
    std::vector<std::string> optionalDepends;
    const bool               ok = pluginxx::parsePluginManifest(
        dirPath,
        name,
        entry,
        depends,
        optionalDepends,
        nullptr,
        nullptr
    );
    if (!ok || name.empty()) {
        Json bad;
        bad["id"]     = dirPath.filename().string();
        bad["path"]   = dir;
        bad["valid"]  = false;
        bad["error"]  = "plugin.yaml 解析失败 (缺少 name 或 YAML 非法)";
        bad["source"] = kindHint;
        return bad.dump();
    }

    std::string kind        = entry.empty() ? "js" : "native";
    std::string version;
    std::string description;
    std::string author;
    std::string homepage;
    int32_t     apiVersion   = 1;
    Json        permissions  = Json::array();
    Json        platforms    = Json::array();
    Json        arch         = Json::array();
    try {
        auto node = YAML::Load(yamlText);
        auto readScalar = [&node](const char* key, std::string& dst) {
            if (node[key] && node[key].IsScalar()) {
                dst = node[key].as<std::string>();
            }
        };
        readScalar("kind", kind);
        readScalar("version", version);
        readScalar("description", description);
        readScalar("author", author);
        readScalar("homepage", homepage);
        if (node["api_version"] && node["api_version"].IsScalar()) {
            apiVersion = node["api_version"].as<int32_t>();
        }
        auto readList = [&node](const char* key, Json& dst) {
            if (!node[key]) {
                return;
            }
            if (node[key].IsSequence()) {
                for (const auto& item : node[key]) {
                    dst.push_back(item.as<std::string>());
                }
            } else if (node[key].IsScalar()) {
                dst.push_back(node[key].as<std::string>());
            }
        };
        readList("permissions", permissions);
        readList("platforms", platforms);
        readList("arch", arch);
    } catch (const std::exception& e) {
        XX_LOGW("[musicxx_ext] 插件 `{}` 清单扩展字段解析失败: {}", name, e.what());
    }
    if (kind.empty()) {
        kind = "native";
    }

    // 平台/架构/版本/文件过滤: 不支持的插件照常展示, 但标注 supported=false 与原因
    bool        supported = true;
    std::string reason;
    if (!platforms.empty()) {
        bool match = false;
        for (const auto& p : platforms) {
            if (p.is_string() && p.get<std::string>() == platform_) {
                match = true;
                break;
            }
        }
        if (!match) {
            supported = false;
            reason    = "当前平台不在清单声明内";
        }
    }
    if (supported && !arch.empty()) {
        bool match = false;
        for (const auto& a : arch) {
            if (a.is_string() && a.get<std::string>() == hostArch()) {
                match = true;
                break;
            }
        }
        if (!match) {
            supported = false;
            reason    = "当前架构不在清单声明内";
        }
    }
    if (supported && apiVersion > MUSICXX_PLUGIN_API_VERSION) {
        supported = false;
        reason    = "插件要求的 API 版本高于当前宿主";
    }
    if (supported && kind == "native") {
        if (entry.empty() || !fs::exists(dirPath / entry)) {
            supported = false;
            reason    = "原生插件库文件缺失 (entry=" + entry + ")";
        }
    }
    if (supported && kind == "js") {
        if (!fs::exists(dirPath / "plugin.js")) {
            supported = false;
            reason    = "JS 插件缺少 plugin.js";
        } else if ((flags_ & MUSICXX_EXTERN_PLUGIN_FLAG_NO_JS) != 0) {
            supported = false;
            reason    = "宿主配置禁用了 JS 插件";
        } else if (!jsEngine()) {
            supported = false;
            reason    = "本次运行未启用 JS 运行时 (未编译 QuickJS 或启动失败)";
        }
    }
    if (supported && (flags_ & MUSICXX_EXTERN_PLUGIN_FLAG_NO_NATIVE) != 0 && kind == "native") {
        supported = false;
        reason    = "宿主配置禁用了原生插件";
    }
    if (supported && (flags_ & MUSICXX_EXTERN_PLUGIN_FLAG_NO_JS) != 0 && kind == "js") {
        supported = false;
        reason    = "宿主配置禁用了 JS 插件";
    }
    if ((flags_ & MUSICXX_EXTERN_PLUGIN_FLAG_SAFE_MODE) != 0) {
        supported = false;
        reason    = "安全模式: 本次启动不加载外部插件";
    }

    Json item;
    item["id"]              = name;
    item["path"]            = dir;
    item["dir"]             = dirPath.filename().string();
    item["kind"]            = kind;
    item["version"]         = version;
    item["description"]     = description;
    item["author"]          = author;
    item["homepage"]        = homepage;
    item["entry"]           = entry;
    item["depends"]         = depends;
    item["optionalDepends"] = optionalDepends;
    item["permissions"]     = permissions;
    item["platforms"]       = platforms;
    item["arch"]            = arch;
    item["apiVersion"]      = apiVersion;
    item["valid"]           = true;
    item["supported"]       = supported;
    item["reason"]          = reason;
    item["source"]          = kindHint;
    item["loaded"]          = loaded_.find(name) != loaded_.end();

    discovered_[name] = dir;
    return item.dump();
}

int32_t MusicxxHostManager::pluginListJson(std::string& outJson) {
    outJson = pluginsJson();
    return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::loadPlugin(
    const std::string& idOrPath,
    const std::string& optionsJson,
    bool               sync,
    uint32_t           timeoutMs,
    std::string&       err
) {
    if (!running_.load(std::memory_order_acquire)) {
        err = "plugin_load: host not started";
        return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
    if (idOrPath.empty()) {
        err = "plugin_load: empty id/path";
        return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
    }

    auto options = std::make_shared<pluginxx::PluginLoadOptions>();
    if (!optionsJson.empty()) {
        bool parseOk = true;
        Json parsed  = parseJsonSafe(optionsJson, &parseOk);
        if (!parseOk) {
            err = "plugin_load: options_json 非法";
            return MUSICXX_EXTERN_PLUGIN_ERR_JSON;
        }
        if (parsed.is_object() && parsed.contains("args") && parsed["args"].is_object()) {
            options->args = parsed["args"];
        }
    }

    std::string path = idOrPath;
    {
        auto it = discovered_.find(idOrPath);
        if (it != discovered_.end()) {
            path = it->second;
        } else if (!fs::exists(path) && !userPluginDir_.empty()) {
            const fs::path candidate = fs::path{userPluginDir_} / idOrPath;
            if (fs::exists(candidate)) {
                path = candidate.string();
            }
        }
    }
    std::string pluginId;
    {
        std::error_code ec;
        const fs::path  p{path};
        // 目录形态: 以清单 name 为准 (目录名只作提示, plan §9.3)
        if (fs::is_directory(p, ec)) {
            std::string              manifestName;
            std::string              manifestEntry;
            std::vector<std::string> depends;
            std::vector<std::string> optionalDepends;
            if (pluginxx::parsePluginManifest(
                    p,
                    manifestName,
                    manifestEntry,
                    depends,
                    optionalDepends,
                    nullptr,
                    nullptr
                )
                && !manifestName.empty()) {
                pluginId = manifestName;
            }
        }
        if (pluginId.empty()) {
            // 库文件路径: 去掉扩展名与 Windows/Unix 的 "lib" 前缀
            fs::path leaf = (fs::is_regular_file(p, ec) ? p.stem() : p.filename());
            pluginId      = leaf.string();
            if (pluginId.rfind("lib", 0) == 0 && pluginId.size() > 3) {
                pluginId = pluginId.substr(3);
            }
        }
    }
    if (pluginId.empty()) {
        err = "plugin_load: 无法从路径推导插件 id";
        return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
    }
    options->configPath = deriveConfigPath(pluginId, path);

    auto       self    = shared_from_this();
    auto       slot    = std::make_shared<WaitSlot<int32_t>>();
    auto       failMsg = std::make_shared<std::string>();
    const auto created = std::chrono::steady_clock::now();
    asio::co_spawn(
        ctx_->io,
        [self, path, options, slot, failMsg, pluginId, created]() -> asio::awaitable<void> {
            // JS 插件 (清单 kind: js): 登记内置槽位后按合成实例 js:<id> 装载 (plan §4.4)
            // 脚本与清单都在插件目录里, 插件作者无需编译任何东西。
            const fs::path    pluginPath{path};
            const std::string manifestKind    = readManifestKind(pluginPath);
            const std::string manifestEntry   = readManifestEntry(pluginPath);
            const std::string manifestVersion = readManifestVersion(pluginPath);
            if (manifestKind == "js") {
                std::string   prepareErr;
                const int32_t prepareRc = self->prepareJsPlugin(
                    pluginId,
                    path,
                    manifestEntry,
                    manifestVersion,
                    prepareErr
                );
                if (prepareRc != MUSICXX_EXTERN_PLUGIN_OK) {
                    *failMsg = prepareErr;
                    Json payload;
                    payload["id"]      = pluginId;
                    payload["phase"]   = "load";
                    payload["code"]    = "js_prepare_failed";
                    payload["message"] = prepareErr;
                    self->pushEvent("musicxx.plugin.error", pluginId, payload.dump());
                    slot->set(prepareRc);
                    co_return;
                }
                auto jsInst = co_await self->loadBuiltinAsync(
                    "js:" + pluginId,
                    path,
                    {},
                    {},
                    options.get()
                );
                if (!jsInst) {
                    if (auto engine = self->jsEngine()) {
                        engine->unregisterBuiltin(pluginId);
                    }
                    *failMsg = "JS 插件装载失败 (脚本错误或 start 事务失败; 详见日志与调试信息)";
                    Json payload;
                    payload["id"]      = pluginId;
                    payload["phase"]   = "load";
                    payload["code"]    = "js_load_failed";
                    payload["message"] = *failMsg;
                    self->pushEvent("musicxx.plugin.error", pluginId, payload.dump());
                    slot->set(MUSICXX_EXTERN_PLUGIN_ERR_STATE);
                    co_return;
                }
                jsInst->kind = "js";
                if (!manifestVersion.empty()) {
                    jsInst->version = manifestVersion;
                }
                jsInst->configPath = options->configPath;
                jsInst->phaseMs.load = std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - created
                )
                                           .count();
                slot->set(MUSICXX_EXTERN_PLUGIN_OK);
                co_return;
            }

            auto inst = co_await self->loadPluginAsync(path, options.get(), false);
            if (!inst) {
                *failMsg = "插件装载失败 (缺失/无效入口符号, 必选依赖未加载, 或 start 事务失败; 详见日志)";
                Json payload;
                payload["id"]      = pluginId;
                payload["phase"]   = "load";
                payload["code"]    = "load_failed";
                payload["message"] = *failMsg;
                self->pushEvent("musicxx.plugin.error", pluginId, payload.dump());
                slot->set(MUSICXX_EXTERN_PLUGIN_ERR_STATE);
                co_return;
            }
            if (options->args.contains("kind") && options->args["kind"].is_string()) {
                inst->kind = options->args["kind"].get<std::string>();
            }
            inst->configPath   = options->configPath;
            inst->phaseMs.load = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - created
            )
                                     .count();
            slot->set(MUSICXX_EXTERN_PLUGIN_OK);
        },
        asio::detached
    );

    if (!sync) {
        return MUSICXX_EXTERN_PLUGIN_OK; ///< 结果经事件回报
    }
    int32_t rc = MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT;
    if (!slot->wait(timeoutMs == 0 ? 10000 : timeoutMs, rc)) {
        err = "plugin_load: 装载未在预算内完成 (结果稍后经事件回报)";
        return MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT;
    }
    if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
        err = failMsg->empty() ? "plugin_load failed" : *failMsg;
    }
    return rc;
}

int32_t MusicxxHostManager::enablePlugin(const std::string& id, std::string& err) {
    if (!running_.load(std::memory_order_acquire)) {
        err = "plugin_enable: host not started";
        return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
    auto inst = resolveInstance(id);
    if (!inst) {
        err = "plugin_enable: plugin not loaded: " + id;
        return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
    }
    enable(inst->name);
    return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::disablePlugin(const std::string& id, std::string& err) {
    if (!running_.load(std::memory_order_acquire)) {
        err = "plugin_disable: host not started";
        return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
    auto inst = resolveInstance(id);
    if (!inst) {
        err = "plugin_disable: plugin not loaded: " + id;
        return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
    }
    disable(inst->name);
    return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::unloadPlugin(const std::string& id, uint32_t timeoutMs, std::string& err) {
    if (!running_.load(std::memory_order_acquire)) {
        err = "plugin_unload: host not started";
        return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
    // 幂等: 未加载视为"已经是未加载状态"; 插件 id 与实例名都接受 (JS 插件实例名为 js:<id>)
    auto inst = resolveInstance(id);
    if (!inst) {
        XX_LOGI("[musicxx_ext] plugin_unload: `{}` 未加载, 视为已卸载", id);
        return MUSICXX_EXTERN_PLUGIN_OK;
    }
    const std::string instanceName = inst->name;
    auto              slot         = std::make_shared<WaitSlot<bool>>();
    auto              self         = shared_from_this();
    const auto        budget = std::chrono::milliseconds{static_cast<int64_t>((std::max)(timeoutMs, 1000u))};
    asio::co_spawn(
        ctx_->io,
        [self, slot, instanceName, budget]() -> asio::awaitable<void> {
            slot->set(co_await self->unloadAsync(instanceName, budget));
        },
        asio::detached
    );
    bool ok = false;
    if (!slot->wait(timeoutMs == 0 ? 30000 : timeoutMs + 500, ok) || !ok) {
        err = "plugin_unload: 卸载未完成 (存在长时间运行的任务或 stop 事务未返回)";
        return MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT;
    }
    return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::setPluginArgs(
    const std::string& id,
    const std::string& argsJson,
    std::string&       err
) {
    bool parseOk = false;
    Json args    = parseJsonSafe(argsJson, &parseOk);
    if (!parseOk || !args.is_object()) {
        err = "plugin_set_args: args_json 必须是 JSON 对象";
        return MUSICXX_EXTERN_PLUGIN_ERR_JSON;
    }
    auto        slot = std::make_shared<WaitSlot<bool>>();
    MusicxxHostManager* self = this;
    pluginxx::ioCallSyncVoid(self, [self, id, args, slot]() {
        auto inst = self->resolveInstance(id);
        if (!inst) {
            slot->set(false);
            return;
        }
        inst->args = args;
        slot->set(true);
    });
    bool ok = false;
    if (!slot->wait(3000, ok) || !ok) {
        err = "plugin_set_args: plugin not loaded: " + id;
        return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
    }
    return MUSICXX_EXTERN_PLUGIN_OK;
}

std::string MusicxxHostManager::deriveConfigPath(
    const std::string& pluginId,
    const std::string& pluginDir
) const {
    std::error_code ec;
    if (!pluginDir.empty() && fs::is_directory(pluginDir, ec)) {
        return (fs::path{pluginDir} / "config.json").string();
    }
    if (!userPluginDir_.empty()) {
        return (fs::path{userPluginDir_} / pluginId / "config.json").string();
    }
    return {};
}

int32_t MusicxxHostManager::pluginConfigPath(
    const std::string& id,
    std::string&       out,
    std::string&       err
) {
    auto inst = resolveInstance(id);
    if (inst && !inst->configPath.empty()) {
        out = inst->configPath;
        return MUSICXX_EXTERN_PLUGIN_OK;
    }
    auto dirIt = discovered_.find(id);
    out        = (dirIt != discovered_.end()) ? deriveConfigPath(id, dirIt->second)
                                              : deriveConfigPath(id, {});
    if (out.empty()) {
        err = "plugin_get_config_path: 无法推导插件目录 (用户插件目录未配置)";
        return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
    }
    return MUSICXX_EXTERN_PLUGIN_OK;
}

/* ==================== 插件能力调用 (Dart → 插件) ==================== */

namespace {

/// 能力调用上下文: 宿主线程发布完成, 调用线程等待
struct PluginCallState {
    WaitSlot<int32_t> slot;
    std::string       payload;
    std::string       error;
};

/// 能力完成回调 (宿主线程调用; 负责回收 holder 并唤醒调用线程)
void PLUGINXX_CALL pluginCallDone(
    void*                     ud,
    int32_t                   status,
    const PluginxxStringView* payload
) noexcept {
    std::unique_ptr<std::shared_ptr<PluginCallState>> holder{
        static_cast<std::shared_ptr<PluginCallState>*>(ud)
    };
    if (!holder || !*holder) {
        return;
    }
    auto state = *holder; ///< 复制 shared_ptr: 完成回调期间保活调用状态
    if (payload && payload->data) {
        state->payload.assign(payload->data, static_cast<size_t>(payload->size));
    }
    if (status == PLUGINXX_OPERATOR_OK) {
        state->slot.set(MUSICXX_EXTERN_PLUGIN_OK);
    } else {
        state->error = state->payload.empty() ? "插件能力执行失败" : state->payload;
        state->slot.set(MUSICXX_EXTERN_PLUGIN_ERR_STATE);
    }
}

} // namespace

namespace {

/// 能力调用完成回调的持有者: 把内核回调转发给调用方给的闭包
struct CapabilityCallBridge {
    std::function<void(int32_t, const std::string&)> done;
};

/// 内核能力完成回调 (恒在宿主线程发布)
void PLUGINXX_CALL capabilityCallTrampoline(
    void*                     ud,
    int32_t                   status,
    const PluginxxStringView* payload
) noexcept {
    std::unique_ptr<CapabilityCallBridge> holder{static_cast<CapabilityCallBridge*>(ud)};
    if (!holder) {
        return;
    }
    std::string text;
    if (payload && payload->data) {
        text.assign(payload->data, static_cast<size_t>(payload->size));
    }
    if (holder->done) {
        holder->done(status == PLUGINXX_OPERATOR_OK ? MUSICXX_EXTERN_PLUGIN_OK
                                                    : MUSICXX_EXTERN_PLUGIN_ERR_STATE,
                     text);
    }
}

} // namespace

void MusicxxHostManager::invokeCapabilityOnHostThread(
    const std::string&                               id,
    const std::string&                               method,
    const std::string&                               argsJson,
    std::function<void(int32_t, const std::string&)> done
) {
    auto fail = [&done](int32_t rc, const std::string& message) {
        if (done) {
            done(rc, message);
        }
    };
    if (id.empty() || method.empty()) {
        fail(MUSICXX_EXTERN_PLUGIN_ERR_ARG, "plugin_call: id 与 method 都不能为空");
        return;
    }
    // 能力全名: 官方前缀 (plugin.<pluginId>.) 缺失时由宿主补齐 (plan §3.5)
    const std::string cap = method.rfind("plugin.", 0) == 0 ? method : ("plugin." + id + "." + method);

    auto inst = resolveInstance(id);
    if (!inst) {
        fail(MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND, "插件未加载: " + id);
        return;
    }
    const auto* entry = capabilities()->get(cap);
    if (!entry || !entry->start) {
        fail(
            MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND,
            "插件未声明该能力 (需要在 start 事务里注册): " + cap
        );
        return;
    }
    if (entry->provider != inst->name) {
        fail(
            MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION,
            "能力归属校验失败 (声明的插件: " + entry->provider + ")"
        );
        return;
    }

    auto* bridge   = new CapabilityCallBridge{std::move(done)};
    auto  capView  = std::string_view{cap};
    auto  argsView = std::string_view{argsJson};
    PluginxxString callErr{};
    // caller 传目标实例自身: 内核要求"调用方实例"提供执行 lease (Dart 侧没有
    // 插件实例, 用被调用者自己的 lease 覆盖本次调用, 卸载等待因此覆盖它)
    auto* handle = invokeCapabilityAsync(
        inst.get(),
        capView,
        capView, ///< method: 与能力全名一致 (插件按需再细分)
        argsView,
        &capabilityCallTrampoline,
        bridge,
        &callErr
    );
    if (!handle) {
        // 受理失败: 完成回调不会被调用, 由这里回收 bridge 并终结本次调用
        std::unique_ptr<CapabilityCallBridge> holder{bridge};
        std::string message = callErr.data ? std::string{callErr.data, static_cast<size_t>(callErr.size)}
                                           : std::string{"插件能力调用未被受理"};
        if (callErr.data) {
            pluginxx::hostMemoryFree(callErr.data);
        }
        if (holder->done) {
            holder->done(MUSICXX_EXTERN_PLUGIN_ERR_STATE, message);
        }
    }
}

int32_t MusicxxHostManager::pluginCall(
    const std::string& id,
    const std::string& method,
    const std::string& argsJson,
    uint32_t           timeoutMs,
    std::string&       outJson,
    std::string&       err
) {
    if (!running_.load(std::memory_order_acquire)) {
        err = "plugin_call: host not started";
        return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
    if (id.empty() || method.empty()) {
        err = "plugin_call: id 与 method 都不能为空";
        return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
    }
    const std::string cap = method.rfind("plugin.", 0) == 0 ? method : ("plugin." + id + "." + method);

    auto self  = shared_from_this();
    auto state = std::make_shared<PluginCallState>();

    // 第一步在宿主线程执行: 实例查找、能力归属校验、发起异步调用 (完成回调也在该线程)
    asio::post(ctx_->io, [self, state, id, method, argsJson] {
        self->invokeCapabilityOnHostThread(
            id,
            method,
            argsJson,
            [state](int32_t rc, const std::string& text) {
                if (rc == MUSICXX_EXTERN_PLUGIN_OK) {
                    state->payload = text;
                } else {
                    state->error = text.empty() ? std::string{"plugin_call failed"} : text;
                }
                state->slot.set(rc);
            }
        );
    });

    int32_t rc = MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT;
    if (!state->slot.wait(timeoutMs == 0 ? 5000 : timeoutMs, rc)) {
        err = "plugin_call: 未在预算内完成 (" + cap + ")";
        return MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT;
    }
    if (rc != MUSICXX_EXTERN_PLUGIN_OK) {
        err = state->error.empty() ? "plugin_call failed" : state->error;
        return rc;
    }
    outJson = std::move(state->payload);
    return MUSICXX_EXTERN_PLUGIN_OK;
}

void MusicxxHostManager::pluginCallAsync(
    const std::string&                               id,
    const std::string&                               method,
    const std::string&                               argsJson,
    std::function<void(int32_t, const std::string&)> done
) {
    if (!running_.load(std::memory_order_acquire)) {
        if (done) {
            done(MUSICXX_EXTERN_PLUGIN_ERR_STATE, "plugin_call: host not started");
        }
        return;
    }
    auto self = shared_from_this();
    asio::post(
        ctx_->io,
        [self, id, method, argsJson, done = std::move(done)]() mutable {
            self->invokeCapabilityOnHostThread(id, method, argsJson, std::move(done));
        }
    );
}

std::shared_ptr<pluginxx::EventSource> MusicxxHostManager::eventSource() {
    return eventBus_;
}

std::string MusicxxHostManager::qualifyEventTopic(std::string_view topic) {
    // 命名空间规则 (plan §3.5): 官方 musicxx.* 与插件 plugin.<id>.* 之外一律拒绝
    if (topic.rfind("musicxx.", 0) == 0) {
        return std::string{topic};
    }
    if (topic.rfind("plugin.", 0) == 0) {
        const auto rest = topic.substr(7);
        const auto dot  = rest.find('.');
        if (dot != std::string_view::npos && dot > 0 && dot + 1 < rest.size()) {
            return std::string{topic};
        }
    }
    XX_LOGW("[musicxx_ext] 拒绝非法事件主题 `{}` (必须是 musicxx.* 或 plugin.<pluginId>.*)", topic);
    return {};
}

int32_t MusicxxHostManager::publishPluginEvent(
    const std::string& topic,
    const std::string& payloadJson
) {
    if (!running_.load(std::memory_order_acquire)) {
        return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
    // 宿主自身没有插件命名空间: 只允许发布官方主题 (插件自定义主题由插件自己发布)
    if (topic.rfind("musicxx.", 0) != 0) {
        XX_LOGW("[musicxx_ext] 宿主发布事件被拒绝: 主题 `{}` 必须以 musicxx. 开头", topic);
        return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
    }
    const std::string payload = payloadJson.empty() ? std::string{"{}"} : payloadJson;
    auto              self    = shared_from_this();
    // 事件总线的订阅/发布约定在宿主线程: 投递后立即返回 (调用方通常是 Dart 线程)
    asio::post(ctx_->io, [self, topic, payload] { self->publishOnHostThread(topic, payload); });
    return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::publishOnHostThread(
    const std::string& topic,
    const std::string& payloadJson
) {
    const std::string fullTopic = qualifyEventTopic(topic);
    if (fullTopic.empty()) {
        return MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION;
    }
    /// 事件计数与速率 (plan §4.11; 只观测不限制): 只有插件自定义主题才归属到插件,
    /// 官方 `musicxx.*` 主题 (宿主/Dart 发布) 不计入任何插件的速率
    accountEventForTopic(fullTopic);
    const std::string payload = payloadJson.empty() ? std::string{"{}"} : payloadJson;
    const int32_t     rc      = eventBus_ ? eventBus_->publish(fullTopic, payload) : -1;
    if (rc != 0) {
        return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
    // 观测回传 (plan §4.9): 仅在调试开关打开时把主题镜像给 Dart 侧 (默认关闭零开销)
    if ((flags_ & MUSICXX_EXTERN_PLUGIN_FLAG_DEBUG_OBSERVE_EVENTS) != 0) {
        Json mirror;
        mirror["topic"] = fullTopic;
        mirror["data"]  = parseJsonSafe(payload);
        pushEvent("musicxx.event.published", "", mirror.dump());
    }
    return MUSICXX_EXTERN_PLUGIN_OK;
}

void MusicxxHostManager::accountEventForTopic(const std::string& topic) {
    const std::string ownerId = naming::pluginIdOfTopic(topic);
    if (ownerId.empty()) {
        return; ///< 官方主题: 不计入任何插件
    }
    auto inst = resolveInstance(ownerId);
    if (!inst) {
        return; ///< 插件已卸载 (在途事件); 不统计
    }
    ++inst->eventsPublished;
    const int64_t now = steadyNowMs();
    if (inst->eventsWindowStartMs == 0) {
        inst->eventsWindowStartMs = now;
    }
    ++inst->eventsWindowCount;
    const int64_t elapsed = now - inst->eventsWindowStartMs;
    /// 速率窗口: 1 秒 (不足 1 秒不结算, 避免高频事件把显示刷成瞬时尖峰)
    if (elapsed >= 1000) {
        inst->eventsPerSec        = inst->eventsWindowCount * 1000 / elapsed;
        inst->eventsWindowCount   = 0;
        inst->eventsWindowStartMs = now;
    }
}

/* ==================== 状态镜像 (Dart → 原生) ==================== */
int32_t MusicxxHostManager::stateUpdate(
    const std::string& key,
    const std::string& valueJson,
    std::string&       err
) {
    if (key.empty()) {
        err = "state_update: empty key";
        return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
    }
    bool parseOk = false;
    Json value   = parseJsonSafe(valueJson, &parseOk);
    if (!parseOk) {
        err = "state_update: value 必须是合法 JSON";
        return MUSICXX_EXTERN_PLUGIN_ERR_JSON;
    }
    std::string      text     = value.dump();
    constexpr size_t kMaxSize = 64 * 1024; ///< 单键上限 (plan §4.7)
    bool             truncated = false;
    if (text.size() > kMaxSize) {
        text.resize(kMaxSize);
        truncated = true;
    }
    {
        std::lock_guard<std::mutex> lock{stateMutex_};
        state_[key] = text;
    }
    // 通知"声明关心该键"的插件 (plan §4.2 的 musicxx.host.subscribe_state):
    // 事件处理必须回到宿主线程, 因此这里投递后立即返回 (调用方通常是 Dart 线程)
    {
        auto self = shared_from_this();
        asio::post(ctx_->io, [self, key, text] {
            Json payload;
            payload["key"]   = key;
            payload["value"] = parseJsonSafe(text);
            self->eventBus_->publish("musicxx.state.changed", payload.dump());
        });
    }
    if (truncated) {
        err = "state_update: 值超过 64 KiB, 已截断";
        return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
    }
    return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::stateUpdateBatch(const std::string& itemsJson, std::string& err) {
    bool parseOk = false;
    Json items   = parseJsonSafe(itemsJson, &parseOk);
    if (!parseOk || !items.is_array()) {
        err = "state_update_batch: items 必须是 JSON 数组";
        return MUSICXX_EXTERN_PLUGIN_ERR_JSON;
    }
    int32_t rc = MUSICXX_EXTERN_PLUGIN_OK;
    for (const auto& item : items) {
        if (!item.is_object() || !item.contains("key") || !item.contains("value")) {
            rc = MUSICXX_EXTERN_PLUGIN_ERR_ARG;
            continue;
        }
        std::string subErr;
        const auto  sub = stateUpdate(item["key"].get<std::string>(), item["value"].dump(), subErr);
        if (sub != MUSICXX_EXTERN_PLUGIN_OK && rc == MUSICXX_EXTERN_PLUGIN_OK) {
            rc  = sub;
            err = subErr;
        }
    }
    return rc;
}

/* ==================== 观测 / 统计 ==================== */

int32_t MusicxxHostManager::debugInfo(std::string& outJson) {
    Json j;
    j["apiVersion"]       = MUSICXX_EXTERN_PLUGIN_API_VERSION;
    j["version"]          = "0.1.0";
    j["pluginAbiVersion"] = MUSICXX_PLUGIN_API_VERSION;
    j["platform"]         = platform_;
    j["arch"]             = hostArch();
    j["appVersion"]       = appVersion_;
    j["language"]         = language_;
    j["flags"]            = flags_;
    j["userPluginDir"]    = userPluginDir_;
    j["builtinPluginDir"] = builtinPluginDir_;
    j["dataDir"]          = dataDir_;
    j["logDir"]           = logDir_;
    j["running"]          = running_.load();
    j["loadedCount"]      = static_cast<int32_t>(loaded_.size());
    j["hookPoints"]       = static_cast<int32_t>(hooks_.size());
    {
        std::lock_guard<std::mutex> lock{eventMutex_};
        j["queue"] = {{"size", static_cast<int64_t>(events_.size())}, {"dropped", eventDropped_}};
    }
    // JS 运行时诊断 (是否可用/线程状态/每个 JS 插件的注册与错误计数)
    {
        auto engine = jsEngine();
        j["js"]     = engine ? parseJsonSafe(engine->statsJson()) : parseJsonSafe(R"({"available":false,"running":false})");
    }
    outJson = j.dump();
    return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::statsJson(const std::string* /*scopeJson*/, std::string& outJson) {
    Json j;
    Json host;
    host["uptimeMs"] = running_.load()
                           ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - startTime_
                             )
                                 .count()
                           : 0;
    if (statsEnabled_) {
        {
            std::lock_guard<std::mutex> lock{eventMutex_};
            host["queue"]
                = {{"size", static_cast<int64_t>(events_.size())}, {"dropped", eventDropped_}};
        }
        Json hooks = Json::object();
        for (const auto& [hookId, handlers] : hooks_) {
            int64_t calls    = 0;
            int64_t totalUs  = 0;
            int64_t maxUs    = 0;
            int64_t timeouts = 0;
            for (const auto& h : handlers) {
                calls += h.calls;
                totalUs += h.totalUs;
                maxUs    = (std::max)(maxUs, h.maxUs);
                timeouts += h.timeouts;
            }
            hooks[hookId] = {
                {"calls", calls},
                {"avgUs", calls > 0 ? totalUs / calls : 0},
                {"maxUs", maxUs},
                {"timeouts", timeouts},
            };
        }
        host["hooks"] = hooks;
    }
    /// 共享 JS 线程的排队情况 (plan §4.11: 队列深度/排队等待时长/定时器数;
    /// 宿主不打断脚本, 只用这些数字在管理页提示"某个脚本正在占住 JS 线程")
    if (jsEngine_) {
        host["js"] = parseJsonSafe(jsEngine_->statsJson());
    }
    j["host"] = host;

    Json plugins = Json::array();
    for (const auto& [name, inst] : loaded_) {
        if (!inst) {
            continue;
        }
        Json p;
        p["id"]      = pluginIdOf(inst->name);
        p["kind"]    = inst->kind;
        p["phaseMs"] = {
            {"load", inst->phaseMs.load},
            {"create", inst->phaseMs.create},
            {"start", inst->phaseMs.start},
            {"stop", inst->phaseMs.stop},
            {"destroy", inst->phaseMs.destroy},
        };
        int32_t hookCount = 0;
        for (const auto& [hookId, handlers] : hooks_) {
            for (const auto& h : handlers) {
                if (h.plugin == inst->name) {
                    ++hookCount;
                }
            }
        }
        p["registrations"] = {{"hooks", hookCount}};
        const int64_t uptimeMs = inst->loadedAtMs > 0 ? (steadyNowMs() - inst->loadedAtMs) : 0;
        const int64_t events   = inst->eventsPublished.load();
        p["counters"]      = {
            {"hookCalls", inst->hookCalls.load()},
            {"hookTimeouts", inst->hookTimeouts.load()},
            {"actionRequests", inst->actionRequests.load()},
            {"events", events},
            /// 事件速率 (plan §4.11): eventsPerSec = 最近一个完整窗口; eventsAvgPerSec = 装载以来平均
            {"eventsPerSec", inst->eventsPerSec},
            {"eventsAvgPerSec", uptimeMs > 0 ? events * 1000 / uptimeMs : 0},
            {"errors", inst->errors.load()},
        };
        /// JS 插件: 用 JS 运行时的堆用量采样值 (plan §4.11; native 插件无法精确统计,
        /// 只能由插件经 musicxx.stats 自报, 见 plan 的边界说明)
        int64_t jsHeap = -1;
        if (jsEngine_ && inst->kind == "js") {
            jsHeap = jsEngine_->jsHeapBytesOf(inst->name);
            if (jsHeap < 0) {
                jsHeap = 0;
            }
        }
        p["memory"] = {
            {"selfReportedBytes", inst->selfReportedBytes.load()},
            {"jsHeapBytes", jsHeap < 0 ? 0 : jsHeap},
            {"jsHeapSampled", jsHeap >= 0},
        };
        /// JS 插件附带运行时段的明细 (钩子/能力/订阅/定时器/脚本执行次数/堆用量/
        /// 可选执行上限命中数), 供管理页与排障直接读取
        if (jsEngine_ && inst->kind == "js") {
            p["js"] = parseJsonSafe(jsEngine_->instanceStatsJson(inst->name));
        }
        plugins.push_back(p);
    }
    j["plugins"] = plugins;
    outJson     = j.dump();
    return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::statsConfig(const std::string& cfgJson, std::string& err) {
    bool parseOk = false;
    Json cfg     = parseJsonSafe(cfgJson, &parseOk);
    if (!parseOk || !cfg.is_object()) {
        err = "stats_config: 需要 JSON 对象";
        return MUSICXX_EXTERN_PLUGIN_ERR_JSON;
    }
    if (cfg.contains("enable") && cfg["enable"].is_boolean()) {
        statsEnabled_ = cfg["enable"].get<bool>();
    }
    return MUSICXX_EXTERN_PLUGIN_OK;
}

asio::any_io_executor MusicxxHostManager::hostExecutor() const {
    if (!ctx_) {
        return asio::any_io_executor{};
    }
    return ctx_->io.get_executor();
}

int32_t MusicxxHostManager::uiSnapshot(std::string& outJson) {
    // 声明式 UI 扩展 (plan §5.6): 返回全部插件的 UI 项 (按 type/order/seq 排序),
    // Dart 侧据此渲染主页入口 / 菜单项 / 设置页 / 附加信息块。
    outJson = uiItemsJson(std::string{});
    return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::setConfig(const std::string& cfgJson, std::string& err) {
    bool parseOk = false;
    Json cfg     = parseJsonSafe(cfgJson, &parseOk);
    if (!parseOk || !cfg.is_object()) {
        err = "set_config: 需要 JSON 对象";
        return MUSICXX_EXTERN_PLUGIN_ERR_JSON;
    }
    auto readString = [&cfg](const char* key, std::string& dst) {
        if (cfg.contains(key) && cfg[key].is_string()) {
            dst = cfg[key].get<std::string>();
        }
    };
    readString("language", language_);
    readString("userPluginDir", userPluginDir_);
    readString("builtinPluginDir", builtinPluginDir_);
    if (cfg.contains("flags") && cfg["flags"].is_number_integer()) {
        flags_ = cfg["flags"].get<int32_t>();
    }
    if (cfg.contains("hookBudgetMs") && cfg["hookBudgetMs"].is_number_integer()) {
        hookBudgetMs_ = cfg["hookBudgetMs"].get<int32_t>();
    }
    if (cfg.contains("hookHardBudgetMs") && cfg["hookHardBudgetMs"].is_number_integer()) {
        hookHardMs_ = cfg["hookHardBudgetMs"].get<int32_t>();
    }
    // 可选 JS 执行上限 (0 = 关闭; 默认关闭, 决策 13: 宿主不默认限制插件)
    if (cfg.contains("jsExecGuardMs") && cfg["jsExecGuardMs"].is_number_integer()) {
        const int32_t guardMs = (std::max)(cfg["jsExecGuardMs"].get<int32_t>(), 0);
        if (jsEngine_) {
            jsEngine_->setExecGuardMs(guardMs);
        }
        jsExecGuardMs_ = guardMs;
    }
    // JS 侧同步读宿主信息: 配置变化后刷新缓存 (避免跨线程读管理器字段)
    refreshJsHostInfo();
    return MUSICXX_EXTERN_PLUGIN_OK;
}

void MusicxxHostManager::applyLanguage(const std::string& lang) {
    language_ = lang;
    refreshJsHostInfo();
}

void MusicxxHostManager::logMessage(int32_t level, const std::string& message) {
    switch (level) {
        case 0:
            XX_LOGT("[musicxx_ext][dart] {}", message);
            break;
        case 1:
            XX_LOGD("[musicxx_ext][dart] {}", message);
            break;
        case 2:
            XX_LOGI("[musicxx_ext][dart] {}", message);
            break;
        case 3:
            XX_LOGW("[musicxx_ext][dart] {}", message);
            break;
        default:
            XX_LOGE("[musicxx_ext][dart] {}", message);
            break;
    }
}

} // namespace extern_plugin
} // namespace musicxx
