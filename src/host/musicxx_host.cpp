/// musicxx 外部插件原生宿主: 管理器核心 (配置/启停/事件/领域钩子/生命周期接缝/插件管理)

#include "host_json.h"
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

std::string readFileText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    std::ostringstream oss;
    oss << in.rdbuf();
    return oss.str();
}

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
    Base(std::move(ex)) {}

std::shared_ptr<MusicxxHostManager> MusicxxHostManager::create(
    HostContext&                         ctx,
    const MusicxxExternPluginHostConfig& cfg,
    std::string&                         err
) {
    if (cfg.struct_size != 0 && cfg.struct_size < sizeof(MusicxxExternPluginHostConfig)) {
        err = "host_create: config struct_size 不匹配 (Dart 与原生库版本不一致?)";
        return nullptr;
    }
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
        pushEvent("musicxx.host.ready", "", payload.dump());
    }
    XX_LOGI(
        "[musicxx_ext] host started (platform={}, safeMode={}, noNative={}, noJs={})",
        platform_,
        (flags_ & MUSICXX_EXTERN_PLUGIN_FLAG_SAFE_MODE) != 0,
        (flags_ & MUSICXX_EXTERN_PLUGIN_FLAG_NO_NATIVE) != 0,
        (flags_ & MUSICXX_EXTERN_PLUGIN_FLAG_NO_JS) != 0
    );
    return MUSICXX_EXTERN_PLUGIN_OK;
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
}

/* ==================== 插件管理 ==================== */

int32_t MusicxxHostManager::scanPlugins(std::string& outJson, std::string& err) {
    if (!running_.load(std::memory_order_acquire)) {
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
            reason    = "JS 插件缺少 plugin.js (JS 运行时按 M3 落地)";
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
    const std::string pluginId = fs::path{path}.filename().string();
    if (pluginId.empty()) {
        err = "plugin_load: 无法从路径推导插件 id";
        return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
    }
    options->configPath = deriveConfigPath(pluginId, path);

    auto       self    = shared_from_this();
    auto       slot    = std::make_shared<WaitSlot<int32_t>>();
    const auto created = std::chrono::steady_clock::now();
    asio::co_spawn(
        ctx_->io,
        [self, path, options, slot, pluginId, created]() -> asio::awaitable<void> {
            auto inst = co_await self->loadPluginAsync(path, options.get(), false);
            if (!inst) {
                Json payload;
                payload["id"]      = pluginId;
                payload["phase"]   = "load";
                payload["code"]    = "load_failed";
                payload["message"] = "插件装载失败 (详见日志: 缺入口符号/依赖缺失/平台不匹配)";
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
        err = "plugin_load failed";
    }
    return rc;
}

int32_t MusicxxHostManager::enablePlugin(const std::string& id, std::string& err) {
    if (!running_.load(std::memory_order_acquire)) {
        err = "plugin_enable: host not started";
        return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
    if (loaded_.find(id) == loaded_.end()) {
        err = "plugin_enable: plugin not loaded: " + id;
        return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
    }
    enable(id);
    return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::disablePlugin(const std::string& id, std::string& err) {
    if (!running_.load(std::memory_order_acquire)) {
        err = "plugin_disable: host not started";
        return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
    if (loaded_.find(id) == loaded_.end()) {
        err = "plugin_disable: plugin not loaded: " + id;
        return MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND;
    }
    disable(id);
    return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t MusicxxHostManager::unloadPlugin(const std::string& id, uint32_t timeoutMs, std::string& err) {
    if (!running_.load(std::memory_order_acquire)) {
        err = "plugin_unload: host not started";
        return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
    }
    auto       slot   = std::make_shared<WaitSlot<bool>>();
    auto       self   = shared_from_this();
    const auto budget = std::chrono::milliseconds{static_cast<int64_t>((std::max)(timeoutMs, 1000u))};
    asio::co_spawn(
        ctx_->io,
        [self, slot, id, budget]() -> asio::awaitable<void> {
            slot->set(co_await self->unloadAsync(id, budget));
        },
        asio::detached
    );
    bool ok = false;
    if (!slot->wait(timeoutMs == 0 ? 30000 : timeoutMs, ok) || !ok) {
        err = "plugin_unload: 卸载未完成 (存在长时间运行的任务?)";
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
        auto it = self->loaded_.find(id);
        if (it == self->loaded_.end() || !it->second) {
            slot->set(false);
            return;
        }
        it->second->args = args;
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
    auto it = loaded_.find(id);
    if (it != loaded_.end() && it->second && !it->second->configPath.empty()) {
        out = it->second->configPath;
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
        state_[key] = std::move(text);
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
        p["counters"]      = {
            {"hookCalls", inst->hookCalls.load()},
            {"hookTimeouts", inst->hookTimeouts.load()},
            {"actionRequests", inst->actionRequests.load()},
            {"events", inst->eventsPublished.load()},
            {"errors", inst->errors.load()},
        };
        p["memory"] = {{"selfReportedBytes", inst->selfReportedBytes.load()}, {"jsHeapBytes", 0}};
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

int32_t MusicxxHostManager::uiSnapshot(std::string& outJson) {
    // UI 声明式扩展按 M5 落地; v1 返回空数组 (Dart 侧渲染空列表)
    outJson = "[]";
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
    return MUSICXX_EXTERN_PLUGIN_OK;
}

void MusicxxHostManager::applyLanguage(const std::string& lang) {
    language_ = lang;
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
