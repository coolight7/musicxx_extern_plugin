/// musicxx 插件 SDK: 总头文件 (插件作者只需包含本头)
///
/// 组成:
/// - 插件框架通用部分: `pluginxx/kit/kit.h` (通用表聚合 / 日志 / 任务 / 协程 / 能力注册) —
///   直接使用插件框架 `Pluginxx*` 名字, **不引入别名层**;
/// - musicxx 专用约定: `musicxx/plugin/api/plugin_api.h`;
/// - musicxx 专用接口聚合 [MusicxxPluginIfaces] 与实例上下文 [PluginBase];
/// - 便捷注册: `PluginBase::hook` / `observe` / `unregisterHook` / `requestAction` /
///   `stateJson` / `hostInfoJson`。
/// - 界面描述层 `pluginxx::ui`（组件模型 / 解析 / 适配 / 文本降级 / 基础 kit）
///   与 musicxx 扩展 kit `musicxx::ui::kit`：插件的页面内容用它们拼装。
///
/// 线程与约定: `create` 只构造上下文; `start` 是注册事务, 在框架线程
/// 执行且**禁止阻塞** (慢操作走 `pluginxx` 的 offload / 后台任务); `stop` 撤销自管资源;
/// `destroy` 只释放本地对象。插件内不得有可变全局状态 (多实例规则)。
#pragma once

#include "musicxx/plugin/api/musicxx_ui_kit.g.h"
#include "musicxx/plugin/api/plugin_api.h"
#include "pluginxx/api/iface_meta.h"
#include "pluginxx/kit/kit.h"
#include "pluginxx/ui.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

/* ==================== musicxx 专用表的元信息 (表判定的依据) ==================== */

/// 每张 musicxx 专用表一项, 取值来自 `plugin_api.h` 里的
/// `MUSICXX_PLUGIN_IFACE_*_MIN_VERSION` / `..._BASELINE_SIZE` (每张表"四项"里的两项)。
///
/// 为什么必须补: [pluginxx::queryInterface] 用 `pluginxx::IfaceMeta<Iface>` 判表可用。
/// 缺了这几项特化时会走 `IfaceMeta` 的默认实现 (最低版本 1、基线 = 整表大小), 于是
/// **以后往表尾追加成员后, 按新布局编译的插件会整张判老主程序给的表不可用** ——
/// 其实基线段成员老主程序都有。补上特化后判据是
/// `version >= MIN_VERSION` 且 `struct_size >= BASELINE_SIZE`,
/// 追加段成员再用 `pluginxx::ifaceHas<&表::成员>(t)` 单独判定。
///
/// 位置要求: 必须在任何 `queryInterface<MusicxxPlugin*Iface>` 调用之前可见, 所以放在本头
/// 最前面 (不能挪到文件末尾)。
namespace pluginxx {

template<>
struct IfaceMeta<MusicxxPluginHooksIface> {
    static constexpr int32_t  min_version   = MUSICXX_PLUGIN_IFACE_HOOKS_MIN_VERSION;
    static constexpr uint32_t baseline_size = MUSICXX_PLUGIN_IFACE_HOOKS_BASELINE_SIZE;
};
static_assert(MUSICXX_PLUGIN_IFACE_HOOKS_BASELINE_SIZE >= 8u);
static_assert(MUSICXX_PLUGIN_IFACE_HOOKS_BASELINE_SIZE <= sizeof(MusicxxPluginHooksIface));

template<>
struct IfaceMeta<MusicxxPluginHostIface> {
    static constexpr int32_t  min_version   = MUSICXX_PLUGIN_IFACE_HOST_MIN_VERSION;
    static constexpr uint32_t baseline_size = MUSICXX_PLUGIN_IFACE_HOST_BASELINE_SIZE;
};
static_assert(MUSICXX_PLUGIN_IFACE_HOST_BASELINE_SIZE >= 8u);
static_assert(MUSICXX_PLUGIN_IFACE_HOST_BASELINE_SIZE <= sizeof(MusicxxPluginHostIface));

template<>
struct IfaceMeta<MusicxxPluginUIIface> {
    static constexpr int32_t  min_version   = MUSICXX_PLUGIN_IFACE_UI_MIN_VERSION;
    static constexpr uint32_t baseline_size = MUSICXX_PLUGIN_IFACE_UI_BASELINE_SIZE;
};
static_assert(MUSICXX_PLUGIN_IFACE_UI_BASELINE_SIZE >= 8u);
static_assert(MUSICXX_PLUGIN_IFACE_UI_BASELINE_SIZE <= sizeof(MusicxxPluginUIIface));

template<>
struct IfaceMeta<MusicxxPluginVarsIface> {
    static constexpr int32_t  min_version   = MUSICXX_PLUGIN_IFACE_VARS_MIN_VERSION;
    static constexpr uint32_t baseline_size = MUSICXX_PLUGIN_IFACE_VARS_BASELINE_SIZE;
};
static_assert(MUSICXX_PLUGIN_IFACE_VARS_BASELINE_SIZE >= 8u);
static_assert(MUSICXX_PLUGIN_IFACE_VARS_BASELINE_SIZE <= sizeof(MusicxxPluginVarsIface));

template<>
struct IfaceMeta<MusicxxPluginNetIface> {
    static constexpr int32_t  min_version   = MUSICXX_PLUGIN_IFACE_NET_MIN_VERSION;
    static constexpr uint32_t baseline_size = MUSICXX_PLUGIN_IFACE_NET_BASELINE_SIZE;
};
static_assert(MUSICXX_PLUGIN_IFACE_NET_BASELINE_SIZE >= 8u);
static_assert(MUSICXX_PLUGIN_IFACE_NET_BASELINE_SIZE <= sizeof(MusicxxPluginNetIface));

} // namespace pluginxx

namespace musicxx {
namespace plugin {

/// 钩子参与方式（与 `MUSICXX_PLUGIN_HOOK_PHASE_*` 一致）
///
/// 钩子 id 表示**一次调用**，参与方式由阶段表达：
/// - `Impl`  ：实现链，给结果 / 裁决（不写阶段就是它）；
/// - `Before`：前链，串行、**正序**，只能改输入或 `stop`；
/// - `After` ：后链，串行、**逆序**，只能改输出或 `drop`。
///
/// 后链逆序是刻意的：外置插件**最后**改输出，应用自己的兜底实现不能盖掉插件的改写
/// （与实现链里"外置优先"同向）。前链 / 后链必须是同步处理器。
enum class HookPhase : int32_t {
    Impl   = MUSICXX_PLUGIN_HOOK_PHASE_IMPL,
    Before = MUSICXX_PLUGIN_HOOK_PHASE_BEFORE,
    After  = MUSICXX_PLUGIN_HOOK_PHASE_AFTER,
};

/// 阶段编号 → 对外字符串（日志用）
inline const char* hookPhaseName(HookPhase phase) {
    switch (phase) {
        case HookPhase::Before:
            return "before";
        case HookPhase::After:
            return "after";
        case HookPhase::Impl:
        default:
            return "impl";
    }
}

/// 网络操作名 (`musicxx.net` 表的 `op` 参数; 完整参数见 `docs/plugin-api.md`)
///
/// 约定: `*_call` 系列都是同步操作 (结果 JSON 里带 `ok`); `http.request` 可以
/// 同步受理 + 听事件, 也可以走 `netCallAsync` 拿完成回调。
/// `http.fetch` / `http.download` 是同一个 HTTP 实现的两个常用形态 (与 JS 的
/// `musicxx.net.fetch` / `musicxx.net.download` 一一对应), 同样走 `netCallAsync`。
namespace net_op {
inline constexpr const char* httpRequest     = "http.request";
inline constexpr const char* httpFetch       = "http.fetch";
inline constexpr const char* httpDownload    = "http.download";
inline constexpr const char* httpCancel      = "http.cancel";
inline constexpr const char* httpServe       = "http.serve";
inline constexpr const char* httpRespond     = "http.respond";
inline constexpr const char* httpStop        = "http.stop";
inline constexpr const char* socketConnect   = "socket.connect";
inline constexpr const char* socketListen    = "socket.listen";
inline constexpr const char* socketSend      = "socket.send";
inline constexpr const char* socketClose     = "socket.close";
inline constexpr const char* socketServerStop = "socket.serverStop";
inline constexpr const char* wsConnect       = "ws.connect";
inline constexpr const char* wsListen        = "ws.listen";
inline constexpr const char* wsSend          = "ws.send";
inline constexpr const char* wsPing          = "ws.ping";
inline constexpr const char* wsClose         = "ws.close";
inline constexpr const char* wsServerStop    = "ws.serverStop";
inline constexpr const char* info            = "net.info";
} // namespace net_op

namespace detail {

/* ==================== 功能点糖的内部实现 (给 PluginBase 用) ==================== */

/// 把处理函数的返回值转成功能点的"回答值"
///
/// 返回 `false` = **这次不回答**（返回了空串），交给同一条通道上的下一个实现。
/// - `utilxx_base::Json` 原样用（`null` 也算回答："算过了，结果是空"）；
/// - 字符串先按 JSON 解析（`{"bpm":128}` → 对象），解析不了就当 JSON 字符串（`hello` → `"hello"`）；
/// - 数字 / 布尔直接当值。
template<typename T>
bool featurePointValueOf(const T& raw, utilxx_base::Json& out) {
    using RawT = std::decay_t<T>;
    if constexpr (std::is_same_v<RawT, utilxx_base::Json>) {
        out = raw;
        return true;
    } else if constexpr (std::is_same_v<RawT, bool> || std::is_arithmetic_v<RawT>) {
        out = raw;
        return true;
    } else if constexpr (std::is_convertible_v<RawT, std::string_view>) {
        const std::string_view text{raw};
        if (text.empty()) {
            return false; ///< 空串 = 这次不回答
        }
        try {
            out = utilxx_base::Json::parse(text);
        } catch (const std::exception&) {
            out = utilxx_base::Json{std::string{text}}; ///< 不是 JSON: 当字符串值
        }
        return true;
    } else {
        static_assert(std::is_convertible_v<RawT, std::string_view> ||
                          std::is_same_v<RawT, utilxx_base::Json>,
                      "featurePointProvide: 处理函数必须返回 Json, 或字符串/数字/布尔");
        return false;
    }
}

/// 功能点处理器：按 `point` 过滤、跳过自己发起的调用、把回答包成 `{"patch": {"value": …}}`
///
/// `fn` 的形参按可调用性选一种：`fn(point, argsJson, caller)` / `fn(point, argsJson)` /
/// `fn(argsJson)`（都是 `std::string_view`）。
///
/// 已经自己写好 `patch` / `action` 的回答（对象里带这两个键）原样通过 —— 手写裁决与糖混用
/// 时不会被再包一层。
template<typename Fn>
int32_t answerFeaturePoint(const std::string& selfId, const std::string& point,
                          Fn& fn, std::string_view input, std::string& out) {
    utilxx_base::Json payload;
    try {
        payload = utilxx_base::Json::parse(input);
    } catch (const std::exception&) {
        return 0; ///< 载荷不是 JSON: 不回答 (交给下一个实现)
    }
    if (!payload.is_object()) {
        return 0;
    }
    if (payload.value<std::string>("point", std::string{}) != point) {
        return 0; ///< 不是自己实现的点
    }
    const std::string caller = payload.value<std::string>("caller", std::string{});
    if (!caller.empty() && caller == selfId) {
        return 0; ///< 自己发起的调用不回答 (免得把自己算出的结果再问回去)
    }
    std::string argsJson{"{}"};
    if (payload.contains("args")) {
        argsJson = payload["args"].dump();
    }

    utilxx_base::Json value;
    bool answered = false;
    if constexpr (std::is_invocable_v<Fn&, std::string_view, std::string_view,
                                      std::string_view>) {
        answered = featurePointValueOf(fn(point, argsJson, caller), value);
    } else if constexpr (std::is_invocable_v<Fn&, std::string_view, std::string_view>) {
        answered = featurePointValueOf(fn(point, argsJson), value);
    } else {
        answered = featurePointValueOf(fn(argsJson), value);
    }
    if (!answered) {
        return 0; ///< 这次不回答
    }
    // 已经写好裁决 (patch / action) 的原样通过, 否则包一层
    if (value.is_object() && (value.contains("patch") || value.contains("action"))) {
        out = value.dump();
        return 0;
    }
    utilxx_base::Json answer = utilxx_base::Json::object();
    answer["patch"]["value"] = value;
    out = answer.dump();
    return 0;
}

} // namespace detail


/// musicxx 专用接口表聚合 = 插件框架通用十表 + musicxx 专用表
///
/// 作为 `pluginxx::PluginBaseT<IfacesT>` 的模板实参使用 (上下文基类会调用 [query]
/// 拉取全部接口表; 框架未实现的表为 nullptr)。
struct MusicxxPluginIfaces : public pluginxx::PluginIfaceCore {
    const MusicxxPluginHooksIface* hooks = nullptr; ///< "musicxx.hooks"
    const MusicxxPluginHostIface*  host  = nullptr; ///< "musicxx.host"
    const MusicxxPluginUIIface*    ui    = nullptr; ///< "musicxx.ui"
    const MusicxxPluginVarsIface*  vars  = nullptr; ///< "musicxx.vars"
    const MusicxxPluginNetIface*   net   = nullptr; ///< "musicxx.net"

    /// 从框架查询全部接口表 (host 为空时返回全 NULL 聚合)
    static MusicxxPluginIfaces query(const PluginxxHost* host) {
        MusicxxPluginIfaces ifaces;
        static_cast<pluginxx::PluginIfaceCore&>(ifaces) = pluginxx::PluginIfaceCore::query(host);
        ifaces.hooks = pluginxx::queryInterface<MusicxxPluginHooksIface>(
            host,
            MUSICXX_PLUGIN_IFACE_HOOKS
        );
        ifaces.host = pluginxx::queryInterface<MusicxxPluginHostIface>(
            host,
            MUSICXX_PLUGIN_IFACE_HOST
        );
        ifaces.ui = pluginxx::queryInterface<MusicxxPluginUIIface>(
            host,
            MUSICXX_PLUGIN_IFACE_UI
        );
        ifaces.vars = pluginxx::queryInterface<MusicxxPluginVarsIface>(
            host,
            MUSICXX_PLUGIN_IFACE_VARS
        );
        ifaces.net = pluginxx::queryInterface<MusicxxPluginNetIface>(
            host,
            MUSICXX_PLUGIN_IFACE_NET
        );
        return ifaces;
    }
};

/// 插件实例上下文基类 (musicxx 专用)
class PluginBase : public pluginxx::PluginBaseT<MusicxxPluginIfaces> {
public:

    /* ---------- 网络能力 (musicxx.net; HTTP / TCP / WebSocket) ---------- */

    /// 网络表是否可用 (老插件框架库没有这张表)
    bool netAvailable() const {
        return iface.net && iface.net->call;
    }

    /// 同步网络操作 (`op` 用 `net_op::*`; 结果写进 `outJson`)
    ///
    /// 返回 0 = 操作执行完毕 (业务结论看结果 JSON 的 `ok`); 表不可用时返回 -1。
    int32_t netCall(const char* op, const char* argsJson, std::string& outJson) {
        if (!netAvailable()) {
            log.warn("musicxx.net 表不可用: 网络能力未生效 (插件框架版本过旧?)");
            return -1;
        }
        PluginxxStringView opView   = pluginxxView(op ? op : "");
        PluginxxStringView argsView = pluginxxView(argsJson ? argsJson : "{}");
        PluginxxString     out{};
        const int32_t      rc = iface.net->call(host, &opView, &argsView, &out);
        if (out.data) {
            outJson.assign(out.data, static_cast<size_t>(out.size));
            hostStringFree(out);
        } else {
            outJson.clear();
        }
        return rc;
    }

    /// 同步网络操作 (参数用 JSON 对象传, 方便拼内容)
    int32_t netCall(const char* op, const utilxx_base::Json& args, std::string& outJson) {
        const std::string text = args.dump();
        return netCall(op, text.c_str(), outJson);
    }

    /// 异步网络操作 (目前 `net_op::httpRequest`): 结果经 `notify->done` 恰好一次 (框架线程)
    ///
    /// `notify` 由 SDK 复制保存, 插件可以传栈上的值。
    int32_t netCallAsync(
        const char*                   op,
        const char*                   argsJson,
        const PluginxxOperatorNotify* notify,
        std::string*                  outJson         = nullptr,
        int64_t*                      outRequestId    = nullptr
    ) {
        if (!netAvailable() || !iface.net->call_async) {
            log.warn("musicxx.net 表不可用: 异步网络能力未生效 (插件框架版本过旧?)");
            return -1;
        }
        if (!notify || !notify->done) {
            return -1;
        }
        auto holder     = std::make_unique<ActionNotifyHolder>();
        holder->user    = *notify;
        holder->forward = PluginxxOperatorNotify{
            &PluginBase::actionNotifyTrampoline,
            holder.get(),
        };
        auto* holderPtr = holder.get();
        actionNotifies_.push_back(std::move(holder));
        pruneActionNotifies();

        PluginxxStringView opView   = pluginxxView(op ? op : "");
        PluginxxStringView argsView = pluginxxView(argsJson ? argsJson : "{}");
        PluginxxString     out{};
        const int32_t      rc = iface.net->call_async(
            host, &opView, &argsView, &holderPtr->forward, &out, outRequestId);
        if (out.data) {
            if (outJson) {
                outJson->assign(out.data, static_cast<size_t>(out.size));
            }
            hostStringFree(out);
        }
        if (rc != 0 && !holderPtr->doneCalled) {
            /// 受理失败: 回收这张通知持有者 (不会有完成回调了)
            holderPtr->doneCalled = true;
        }
        return rc;
    }

    /// 取消一个进行中的异步请求
    int32_t netCancel(int64_t requestId, std::string* outJson = nullptr) {
        if (!netAvailable() || !iface.net->cancel) {
            return -1;
        }
        PluginxxString out{};
        const int32_t  rc = iface.net->cancel(host, requestId, &out);
        if (outJson && out.data) {
            outJson->assign(out.data, static_cast<size_t>(out.size));
        }
        if (out.data) {
            hostStringFree(out);
        }
        return rc;
    }

    /// 订阅本插件的网络事件 (主题 `plugin.<本插件id>.net`)
    ///
    /// 事件载荷带 `kind` (`http.request` / `http.response` / `socket.data` /
    /// `ws.message` / `ws.close` 等), 处理器形如 `void(std::string_view event_json)`。
    template<typename Fn>
    int32_t netSubscribe(Fn&& fn) {
        const std::string topic = "plugin." + pluginId() + ".net";
        return subscribeTopic(topic.c_str(), std::forward<Fn>(fn));
    }

    /// 注册一个裁决型钩子 (同步处理器)
    ///
    /// `fn` 形如 `int32_t(std::string_view input_json, std::string& out_json)`;
    /// 返回 0 且 out_json 非空表示给出裁决, out_json 为空表示不裁决 (交给下一个处理器)。
    template<typename Fn>
    int32_t hook(const char* hookId, int32_t priority, Fn&& fn) {
        return registerSync(
            hookId,
            MUSICXX_PLUGIN_HOOK_MODE_DECISION,
            priority,
            std::forward<Fn>(fn)
        );
    }

    /// 注册某个阶段的钩子处理器（前链 / 实现链 / 后链）
    ///
    /// 阶段必须是该钩子在钩子表里声明过的（`hook_info` 的 `phases`）；没声明就返回 -1
    /// 并记一条可读日志。老插件框架库的 `hook_info` 没有 `phases` 字段时同样拒绝
    /// （不退回"按 impl 注册"，那会让阶段处理器收到错阶段的载荷）。
    template<typename Fn>
    int32_t hook(const char* hookId, HookPhase phase, int32_t priority, Fn&& fn) {
        return registerSyncWithTag(
            hookId,
            "",
            MUSICXX_PLUGIN_HOOK_MODE_DECISION,
            priority,
            static_cast<int32_t>(phase),
            std::forward<Fn>(fn)
        );
    }

    /// 注册前链处理器（改输入 / `stop`），等价于 `hook(id, HookPhase::Before, ...)`
    template<typename Fn>
    int32_t hookBefore(const char* hookId, int32_t priority, Fn&& fn) {
        return hook(hookId, HookPhase::Before, priority, std::forward<Fn>(fn));
    }

    /// 注册后链处理器（改输出 / `drop`），等价于 `hook(id, HookPhase::After, ...)`
    template<typename Fn>
    int32_t hookAfter(const char* hookId, int32_t priority, Fn&& fn) {
        return hook(hookId, HookPhase::After, priority, std::forward<Fn>(fn));
    }

    /// 注册观察型钩子 (返回值忽略; 处理器应尽快返回)
    template<typename Fn>
    int32_t observe(const char* hookId, Fn&& fn) {
        return registerSync(hookId, MUSICXX_PLUGIN_HOOK_MODE_OBSERVE, 0, std::forward<Fn>(fn));
    }

    /// 注销钩子 (owner_tag = "")
    int32_t unregisterHook(const char* hookId) {
        if (!iface.hooks || !iface.hooks->unregister_hook) {
            return -1;
        }
        PluginxxStringView hookView = pluginxxView(hookId);
        PluginxxStringView tagView  = pluginxxView("");
        return iface.hooks->unregister_hook(host, &hookView, &tagView);
    }

    /// 同步读状态镜像 (键不存在/框架未实现返回空串)
    std::string stateJson(const char* key) const {
        if (!iface.host || !iface.host->get_state) {
            return {};
        }
        PluginxxStringView keyView = pluginxxView(key);
        PluginxxString   out{};
        if (iface.host->get_state(host, &keyView, &out) != 0 || !out.data) {
            return {};
        }
        std::string result{out.data, static_cast<size_t>(out.size)};
        hostStringFree(out);
        return result;
    }

    /// 框架信息 JSON (主程序版本/平台/语言/插件目录)
    std::string hostInfoJson() const {
        return readHostString(&MusicxxPluginHostIface::get_info);
    }

    /// 本插件配置路径 (供插件读写自己的配置)
    std::string configPath() const {
        return readHostString(&MusicxxPluginHostIface::get_plugin_config_path);
    }

    /// 调另一个插件的能力 (**协程形态, 推荐**)
    ///
    /// 返回插件框架的 awaiter, 用法:
    /// ```cpp
    /// pluginxx::Task<void> demo() {
    ///     auto result = co_await capabilityCall("plugin.other.probe", "probe", R"({"n":3})");
    ///     // result 按插件框架约定给出结果 / 失败 (见插件框架 kit 的 invoke_cap 说明)
    /// }
    /// ```
    /// - **直连插件框架**: 不经过应用 (Dart), 调用方就是本插件实例 (目标处理器里的 `callerHost`
    ///   是本插件); 期间协程由本实例的驱动桥分步推进, 不会占框架 IO 线程;
    /// - **不要找同步版**: 插件框架的 `invoke_capability_blocking` 在 IO 线程会被直接拒绝
    ///   (`use co_await invoke_cap instead`), 本 SDK 不暴露它;
    /// - 长任务请在协程里 `co_await pluginxx::offload(*this, work)` 把重活交给框架工作线程池。
    auto capabilityCall(
        std::string_view capability,
        std::string_view method,
        std::string_view argsJson = "{}"
    ) {
        return pluginxx::invoke_cap(*this, capability, method, argsJson);
    }

    /// 调另一个插件的能力 (**回调形态**; 给不写协程的场景)
    /// - 结果经 `notify->done` **恰好一次** (框架 IO 线程) 回调; status == 0 时载荷是结果 JSON;
    /// - `notify` 由 SDK 复制保存 (与 [requestAction] 同一做法), 插件可以传栈上的 notify;
    /// - 受理失败 (目标没加载 / 没声明该能力) 时返回负值, 并按 `done(status != 0)` 补一次回调。
    int32_t capabilityCall(
        std::string_view              capability,
        std::string_view              method,
        std::string_view              argsJson,
        const PluginxxOperatorNotify* notify
    ) {
        if (!iface.capabilities || !iface.capabilities->invoke_capability_async) {
            return -1;
        }
        if (!notify || !notify->done) {
            return -1;
        }
        auto holder     = std::make_unique<ActionNotifyHolder>();
        holder->user    = *notify;
        holder->forward = PluginxxOperatorNotify{
            &PluginBase::actionNotifyTrampoline,
            holder.get(),
        };
        auto* holderPtr = holder.get();
        actionNotifies_.push_back(std::move(holder));
        pruneActionNotifies();

        PluginxxStringView capView  = pluginxxView(capability);
        PluginxxStringView methView = pluginxxView(method);
        PluginxxStringView argsView = pluginxxView(argsJson.empty() ? "{}" : argsJson);
        PluginxxString     errorOut{};
        auto* op = iface.capabilities->invoke_capability_async(
            host,
            &capView,
            &methView,
            &argsView,
            &actionNotifyTrampoline,
            holderPtr,
            &errorOut
        );
        if (errorOut.data) {
            hostStringFree(errorOut);
        }
        if (!op) {
            /// 未受理: 完成回调不会被调用, 这里补一次 (与 requestAction 的约定一致)
            if (!holderPtr->doneCalled) {
                holderPtr->doneCalled = true;
                if (holderPtr->user.done) {
                    try {
                        holderPtr->user.done(holderPtr->user.host_ud, -1, nullptr);
                    } catch (...) {
                    }
                }
            }
            return -1;
        }
        return 0;
    }

    /// ---------- 功能点 (插件自己声明 / 实现 / 调用) ----------
    ///
    /// 一个功能点 = 三件事：**声明**（`musicxx.feature.point.define`）、**实现**（注册通用
    /// 通道钩子 `musicxx.feature.point.call` 的处理器）、**调用**（动作
    /// `musicxx.feature.point.call`）。三者都走插件框架，不需要主程序为这个点写任何代码。
    ///
    /// 与能力（capability）的分工见包内 `docs/plugin-guide.md`：
    /// - 能力 = "应用直接调本插件"，参数与结果由插件自己定；
    /// - 功能点 = "这个功能谁都能实现，应用与别的插件按 id 来调"。
    ///
    /// 通道写法（框架约定，糖已经照做）：
    /// 1. 回答**必须包一层 `patch`**（插件框架只在收到 `patch` 时才认为"有人回答了"）；
    /// 2. 处理器要**按 `point` 过滤**（同一条通道上所有实现都收到同一次调用）；
    /// 3. **不回答自己发起的调用**（`caller` 等于自己的插件 id 时返回空）。

    /// 功能点声明信息（对应动作 `musicxx.feature.point.define` 的可读字段）
    struct MusicxxPluginPointSpec {
        const char* title  = ""; ///< 展示名（缺省用 id）
        const char* depict = ""; ///< 一句话说明
        const char* args   = ""; ///< 参数说明（文本：有哪些参数、什么类型）
        const char* result = ""; ///< 结果说明（文本：给出什么形状的结果）
    };

    /// 声明一个**自己的**功能点（`id` 必须形如 `plugin.<自己的插件 id>.<名字>`）
    ///
    /// - 走异步动作（`notify->done` 拿到 `{ok, id}` 或 `{ok:false, error}`）；
    /// - 同一 id 重复声明 = 覆盖；声明跟着插件生命周期走（禁用 / 卸载自动撤销）；
    /// - 声明之后用 [featurePointProvide] 给出实现，别的插件与应用就能调它。
    int32_t featurePointDefine(const char* id, const MusicxxPluginPointSpec& spec,
                               const PluginxxOperatorNotify* notify,
                               uint32_t timeoutMs = 5000) {
        utilxx_base::Json payload = utilxx_base::Json::object();
        payload["id"]     = id ? id : "";
        payload["title"]  = spec.title ? spec.title : "";
        payload["depict"] = spec.depict ? spec.depict : "";
        payload["args"]   = spec.args ? spec.args : "";
        payload["result"] = spec.result ? spec.result : "";
        const std::string text = payload.dump();
        return requestAction("musicxx.feature.point.define", text.c_str(), notify, timeoutMs);
    }

    /// 收回自己声明的功能点（一般不用调：禁用 / 卸载会自动撤销）
    int32_t featurePointUndefine(const char* id, const PluginxxOperatorNotify* notify,
                                 uint32_t timeoutMs = 5000) {
        utilxx_base::Json payload = utilxx_base::Json::object();
        payload["id"] = id ? id : "";
        const std::string text = payload.dump();
        return requestAction("musicxx.feature.point.undefine", text.c_str(), notify, timeoutMs);
    }

    /// 本插件的 id（读一次 `host.info()` 的 `pluginId` 并缓存；拿不到时空串）
    ///
    /// 用途：功能点糖要判断"这次的 `caller` 是不是自己"（自己发起的调用不回答）。
    std::string pluginId() {
        if (!pluginIdLoaded_) {
            pluginIdLoaded_ = true;
            try {
                const utilxx_base::Json info = utilxx_base::Json::parse(hostInfoJson());
                if (info.is_object()) {
                    pluginId_ = info.value<std::string>("pluginId", std::string{});
                }
            } catch (const std::exception&) {
                pluginId_.clear();
            }
        }
        return pluginId_;
    }

    /// 登记一个功能点的实现（**一个点一个处理器**，`owner_tag` = 点 id）
    ///
    /// `fn` 有三种写法（按可调用性自动选一种）：
    /// ```cpp
    /// featurePointProvide("plugin.my_plugin.beat",
    ///     [](std::string_view point, std::string_view argsJson) -> std::string {
    ///         return R"({"bpm":128,"beats":[0,469,938]})";   // 返回的是**值**，糖包 patch
    ///     });
    /// ```
    /// - 形参：`fn(point, argsJson, caller)` / `fn(point, argsJson)` / `fn(argsJson)`；
    /// - 返回：`utilxx_base::Json` / 任何字符串类型 / 数字 / 布尔。字符串会先按 JSON 解析，
    ///   解析不了就当 JSON 字符串（`hello` → `"hello"`）；
    /// - **返回空串 = 这次不回答**（交给同一条通道上的下一个实现）；要回答"结果是空"就返回
    ///   `"null"` 或 `"{}"`；
    /// - 糖自动做三件事：按 `point` 过滤、`caller` 等于自己时返回空、
    ///   把返回值包成 `{"patch":{"value": …}}`；
    /// - **必须同步返回**：裁决型钩子没有异步形态（插件框架按 `hook_sync` 同步调用），长任务要么
    ///   提前算好，要么做成能力（`co_await capabilityCall`）或事件。
    template<typename Fn>
    int32_t featurePointProvide(const char* pointId, Fn&& fn, int32_t priority = 0) {
        const std::string point = pointId ? pointId : "";
        if (point.empty()) {
            log.warn("featurePointProvide: 缺少功能点 id");
            return -1;
        }
        const std::string selfId = pluginId();
        if (selfId.empty()) {
            log.warn("featurePointProvide: 读不到自己的插件 id (host.info 缺 pluginId)");
            return -1;
        }
        return registerSyncWithTag(
            MUSICXX_PLUGIN_HOOK_FEATURE_POINT_CALL,
            point.c_str(),
            MUSICXX_PLUGIN_HOOK_MODE_DECISION,
            priority,
            MUSICXX_PLUGIN_HOOK_PHASE_IMPL,
            [selfId, point, fn = std::forward<Fn>(fn)](std::string_view input,
                                                      std::string& out) mutable -> int32_t {
                return detail::answerFeaturePoint(selfId, point, fn, input, out);
            }
        );
    }

    /// 摘除一个功能点的实现（注销 `owner_tag` = 点 id 的那个处理器）
    int32_t featurePointUnprovide(const char* pointId) {
        if (!iface.hooks || !iface.hooks->unregister_hook) {
            return -1;
        }
        if (!pointId || !*pointId) {
            return -1;
        }
        PluginxxStringView hookView = pluginxxView(MUSICXX_PLUGIN_HOOK_FEATURE_POINT_CALL);
        PluginxxStringView tagView  = pluginxxView(pointId);
        return iface.hooks->unregister_hook(host, &hookView, &tagView);
    }

    /// 调一个功能点（**异步形态**；结果经 `notify->done` 回来）
    ///
    /// - `id` 既可以是应用留出的埋点（`musicxx.media.spectrum.extract` /
    ///   `musicxx.media.chorus.analyse` / `musicxx.media.palette.provide`），
    ///   也可以是插件自己声明的 `plugin.<插件id>.<名字>`；
    /// - `argsJson` 是这个功能点自己的参数（看清单里的 `callDoc`）；
    /// - **默认超时 60 秒**：频谱提取要解码整曲，慢就自己加大；
    /// - **调用零副作用**：不写应用的值仓、不改应用状态；
    /// - **没有同步版**：框架线程上的就地等待等于死锁（处理器可能正等着你返回），
    ///   所以要结果就在 `notify->done` 里处理，或者用协程形态自己封一层。
    int32_t featurePointCall(const char* id, std::string_view argsJson = "{}",
                             uint32_t timeoutMs = 60000,
                             const PluginxxOperatorNotify* notify = nullptr,
                             int64_t* outRequestId = nullptr) {
        if (!notify || !notify->done) {
            log.warn("featurePointCall: 异步形态必须给完成通知 (notify->done)");
            return -1;
        }
        utilxx_base::Json payload = utilxx_base::Json::object();
        payload["id"] = id ? id : "";
        try {
            payload["args"] = argsJson.empty() ? utilxx_base::Json::object()
                                               : utilxx_base::Json::parse(argsJson);
        } catch (const std::exception& e) {
            log.warn(std::string{"featurePointCall: args 不是合法 JSON 对象 ("} + e.what() + ")");
            return -1;
        }
        const std::string text = payload.dump();
        return requestAction("musicxx.feature.point.call", text.c_str(), notify, timeoutMs,
                             outRequestId);
    }

    /// 当前有哪些功能点、谁在实现（行文本；等价动作 `musicxx.feature.point.list`）
    ///
    /// 异步动作：结果经 `notify->done` 回来（`payload` 是清单文本）。
    int32_t featurePointList(const PluginxxOperatorNotify* notify,
                             uint32_t timeoutMs = 5000) {
        if (!notify || !notify->done) {
            return -1;
        }
        return requestAction("musicxx.feature.point.list", "{}", notify, timeoutMs);
    }

    /// 发起主程序动作 (异步; 结果经 `notify->done` 恰好一次回调)
    ///
    /// 主程序/框架会持有完成通知指针直到本次动作终结 (完成 / 超时 / 取消), 因此 SDK 把通知
    /// **复制**到实例内部保存并转接回调 —— 插件可以放心传入栈上的 notify。
    int32_t requestAction(
        const char*                   action,
        const char*                   argsJson,
        const PluginxxOperatorNotify* notify,
        uint32_t                      timeoutMs = 5000,
        int64_t*                      outRequestId = nullptr
    ) {
        if (!iface.host || !iface.host->request_action) {
            return -1;
        }
        if (!notify || !notify->done) {
            return -1;
        }
        auto holder     = std::make_unique<ActionNotifyHolder>();
        holder->user    = *notify;
        holder->forward = PluginxxOperatorNotify{
            &PluginBase::actionNotifyTrampoline,
            holder.get(),
        };
        auto* holderPtr = holder.get();
        actionNotifies_.push_back(std::move(holder));
        pruneActionNotifies();

        PluginxxStringView actionView = pluginxxView(action);
        PluginxxStringView argsView   = pluginxxView(argsJson ? argsJson : "{}");
        return iface.host->request_action(
            host,
            &actionView,
            &argsView,
            timeoutMs,
            &actionNotifies_.back()->forward,
            outRequestId
        );
    }

    /// 订阅事件总线的主题 (**框架线程**执行处理器; 处理器不得阻塞)
    /// - 主题必须符合命名空间规则: 官方 `musicxx.*` 或插件自定义
    ///   `plugin.<本插件id>.*`; 其它前缀会被插件框架拒绝 (返回 -1);
    /// - 处理器形如 `void(std::string_view event_json)`;
    /// - 订阅句柄由插件框架按实例保活 (随 stop/卸载自动撤销), 插件无需保存;
    /// - 处理器持有者与钩子处理器同一存放位置, 生命周期 = 插件实例。
    template<typename Fn>
    int32_t subscribeTopic(const char* topic, Fn&& fn) {
        if (!iface.events || !iface.events->subscribe) {
            log.warn("pluginxx.events 表不可用: 事件订阅未生效 (插件框架版本过旧?)");
            return -1;
        }
        using FnT       = std::decay_t<Fn>;
        auto  holder    = std::make_unique<EventHolder<FnT>>(std::forward<Fn>(fn));
        void* holderPtr = holder.get();
        eventHolders_.push_back(std::move(holder));

        PluginxxStringView topicView = pluginxxView(topic ? topic : "");
        auto*              sub = iface.events->subscribe(host, &topicView, &EventHolder<FnT>::invoke, holderPtr);
        return sub ? 0 : -1;
    }

    /* ---------- 声明式 UI 扩展 (名字用短名, 插件框架自动补前缀) ---------- */

    /// 注册一个 UI 项: `itemName` 用短名 (如 "card"), `type` 取 MUSICXX_PLUGIN_UI_TYPE_*,
    /// `dataJson` 是类型相关的声明式内容 (JSON 对象)
    int32_t uiRegister(const char* itemName, const char* type, const char* dataJson, int32_t order = 0) {
        if (!iface.ui || !iface.ui->register_entry) {
            log.warn("musicxx.ui 表不可用: UI 项未注册 (插件框架版本过旧?)");
            return -1;
        }
        MusicxxPluginUIEntrySpec spec{};
        spec.version     = 1;
        spec.struct_size = sizeof(MusicxxPluginUIEntrySpec);
        spec.item_id     = pluginxxView(itemName ? itemName : "");
        spec.type        = pluginxxView(type ? type : "");
        spec.data_json   = pluginxxView(dataJson ? dataJson : "");
        spec.order       = order;
        spec.flags       = 0;
        return iface.ui->register_entry(host, &spec);
    }

    /// 注册一个 UI 项（`data` 用 Json 对象传；与上面那版等价，便于用 Json 拼内容）
    int32_t uiRegister(const char* itemName, const char* type,
                       const utilxx_base::Json& data, int32_t order = 0) {
        const std::string json = data.dump();
        return uiRegister(itemName, type, json.c_str(), order);
    }

    /// 更新一个 UI 项的声明式内容
    int32_t uiUpdate(const char* itemName, const char* dataJson) {
        if (!iface.ui || !iface.ui->update_entry) {
            return -1;
        }
        PluginxxStringView itemView = pluginxxView(itemName ? itemName : "");
        PluginxxStringView dataView = pluginxxView(dataJson ? dataJson : "");
        return iface.ui->update_entry(host, &itemView, &dataView);
    }

    /// 注销一个 UI 项
    int32_t uiUnregister(const char* itemName) {
        if (!iface.ui || !iface.ui->unregister_entry) {
            return -1;
        }
        PluginxxStringView itemView = pluginxxView(itemName ? itemName : "");
        return iface.ui->unregister_entry(host, &itemView);
    }

    /// 本插件已注册的 UI 项 (JSON 数组; 表不可用时返回空数组)
    std::string uiEntries() const {
        if (!iface.ui || !iface.ui->list_entries) {
            return "[]";
        }
        PluginxxString out{};
        if (iface.ui->list_entries(host, &out) != 0 || !out.data) {
            return "[]";
        }
        std::string result{out.data, static_cast<size_t>(out.size)};
        hostStringFree(out);
        return result;
    }

    /// 通知/提示 (等价动作 `musicxx.ui.notify`; fire-and-forget, 不等待用户界面)
    int32_t uiNotify(const char* messageJson) {
        if (!iface.ui || !iface.ui->notify) {
            return -1;
        }
        PluginxxStringView messageView = pluginxxView(messageJson ? messageJson : "{}");
        return iface.ui->notify(host, &messageView);
    }

    /// ---------- 变量 (musicxx.vars) ----------
    ///
    /// 三条读法按需要选一条：
    /// - `varsGet` 异步、权威（插件框架向属主取一次真实值，结果经 [PluginxxOperatorNotify]
    ///   回来 —— **不要在回调里等结果**，框架线程上的就地等待等于死锁）；
    /// - `varsPeek` 同步读缓存（可能旧，返回里带 revision/ageMs/stale）；
    /// - 订阅变动：调 `varsSubscribe` 登记关心，再订阅事件主题
    ///   `musicxx.var.changed`（载荷里带 key）。
    ///
    /// 写：`varsSet` 的返回值是 JSON —— 写自己的变量（或 `declared` 模式）立即结算，
    /// 交给属主写入时是 `{"pending":true,"requestId":N}`，最终结果经事件
    /// `musicxx.var.writeResult` 送达。
    ///
    /// 注册：`mode = MUSICXX_PLUGIN_VAR_MODE_HANDLER` 时属主自己收
    /// `musicxx.var.read` / `musicxx.var.write` 两个事件，用 `varsRespond` 回答。

    /// 注册/覆盖一个变量（key 可写短名，插件框架自动补 `plugin.<本插件id>.` 前缀）
    int32_t varsRegister(const MusicxxPluginVarSpec& spec) {
        if (!iface.vars || !iface.vars->register_var) {
            return -1;
        }
        return iface.vars->register_var(host, &spec);
    }

    /// 注销自己的变量
    int32_t varsUnregister(const char* key) {
        if (!iface.vars || !iface.vars->unregister_var) {
            return -1;
        }
        PluginxxStringView keyView = pluginxxView(key ? key : "");
        return iface.vars->unregister_var(host, &keyView);
    }

    /// 异步读（结果经 notify 回来；同键进行中请求由插件框架合并）
    int32_t varsGet(const char* key, const PluginxxOperatorNotify* notify,
                    int64_t* outRequestId = nullptr) {
        if (!iface.vars || !iface.vars->get) {
            return -1;
        }
        PluginxxStringView keyView = pluginxxView(key ? key : "");
        return iface.vars->get(host, &keyView, notify, outRequestId);
    }

    /// 同步读缓存（没有缓存过返回 -4；不保证最新）
    std::string varsPeek(const char* key) const {
        return readVarsString(&MusicxxPluginVarsIface::peek, key);
    }

    /// 保活缓存（订阅但不回调；需要 notify 能力位）
    int32_t varsWatch(const char* keysJson) {
        return writeVarsCount(&MusicxxPluginVarsIface::watch, keysJson);
    }
    int32_t varsUnwatch(const char* keysJson) {
        return writeVarsCount(&MusicxxPluginVarsIface::unwatch, keysJson);
    }

    /// 写（自己的变量 = 提交；官方键与 handler 变量交给属主写入）
    std::string varsSet(const char* key, const char* valueJson) {
        if (!iface.vars || !iface.vars->set) {
            return {};
        }
        PluginxxStringView keyView = pluginxxView(key ? key : "");
        PluginxxStringView valueView = pluginxxView(valueJson ? valueJson : "null");
        PluginxxString out{};
        if (iface.vars->set(host, &keyView, &valueView, &out) != 0 || !out.data) {
            return {};
        }
        std::string result{out.data, static_cast<size_t>(out.size)};
        hostStringFree(out);
        return result;
    }

    /// 列出变量（prefix 可空；值字段是缓存值）
    std::string varsList(const char* prefix = nullptr) const {
        return readVarsString(&MusicxxPluginVarsIface::list, prefix);
    }

    /// 声明信息 + 缓存状态
    std::string varsInfo(const char* key) const {
        return readVarsString(&MusicxxPluginVarsIface::info, key);
    }

    /// 订阅/退订变更通知（keys_json 是字符串数组；订阅后事件走 `musicxx.var.changed`）
    int32_t varsSubscribe(const char* keysJson) {
        return writeVarsCount(&MusicxxPluginVarsIface::subscribe, keysJson);
    }
    int32_t varsUnsubscribe(const char* keysJson) {
        return writeVarsCount(&MusicxxPluginVarsIface::unsubscribe, keysJson);
    }

    /// handler 属主回答一次被转过来的请求（ok = 0 表示拒绝，原因写 error）
    std::string varsRespond(int64_t requestId, bool ok, const char* valueJson,
                            const char* error = nullptr) {
        if (!iface.vars || !iface.vars->respond) {
            return {};
        }
        PluginxxStringView valueView = pluginxxView(valueJson ? valueJson : "null");
        PluginxxStringView errorView = pluginxxView(error ? error : "");
        PluginxxString out{};
        if (iface.vars->respond(host, requestId, ok ? 1 : 0, &valueView, &errorView,
                                &out) != 0 || !out.data) {
            return {};
        }
        std::string result{out.data, static_cast<size_t>(out.size)};
        hostStringFree(out);
        return result;
    }

    /// 用框架堆把字符串写进跨边界出参 (插件必须用框架分配器, 不得用 CRT malloc)
    static void hostStringSet(const PluginxxHost* host, PluginxxString* out, std::string_view text) {
        if (!out) {
            return;
        }
        out->data = nullptr;
        out->size = 0;
        if (text.empty() || !host || !host->vtable || !host->vtable->alloc) {
            return;
        }
        auto* p = static_cast<char*>(host->vtable->alloc(text.size() + 1));
        if (!p) {
            return;
        }
        std::memcpy(p, text.data(), text.size());
        p[text.size()] = '\0';
        out->data      = p;
        out->size      = static_cast<uint64_t>(text.size());
    }

    /// 释放框架堆字符串 (经框架 vtable; 插件不得用 CRT free)
    void hostStringFree(PluginxxString& s) const {
        if (s.data && host && host->vtable && host->vtable->free) {
            host->vtable->free(s.data);
        }
        s.data = nullptr;
        s.size = 0;
    }

    /// 变量接口里"输入一个字符串、输出一段 JSON"的那些入口 (peek/info/list)
    using VarsStringFn = int32_t (PLUGINXX_CALL* MusicxxPluginVarsIface::*)(
        const PluginxxHost*, const PluginxxStringView*, PluginxxString*);
    /// 变量接口里"输入一组键、输出条数"的那些入口 (watch/unwatch/subscribe/unsubscribe)
    using VarsCountFn = int32_t (PLUGINXX_CALL* MusicxxPluginVarsIface::*)(
        const PluginxxHost*, const PluginxxStringView*, int32_t*);

    /// 调一个变量接口的 JSON 出参入口 (表不可用或失败返回空串)
    std::string readVarsString(VarsStringFn fn, const char* text) const {
        if (!iface.vars || !fn) {
            return {};
        }
        PluginxxStringView view = pluginxxView(text ? text : "");
        PluginxxString     out{};
        if ((iface.vars->*fn)(host, &view, &out) != 0 || !out.data) {
            return {};
        }
        std::string result{out.data, static_cast<size_t>(out.size)};
        hostStringFree(out);
        return result;
    }

    /// 调一个变量接口的计数入口 (表不可用返回 -1; 失败返回错误码)
    int32_t writeVarsCount(VarsCountFn fn, const char* keysJson) {
        if (!iface.vars || !fn) {
            return -1;
        }
        PluginxxStringView view = pluginxxView(keysJson ? keysJson : "[]");
        int32_t            count = 0;
        const int32_t      rc    = (iface.vars->*fn)(host, &view, &count);
        return rc == 0 ? count : rc;
    }

protected:

    /// 构造 C ABI 字符串视图 (跨边界只读借用)
    static PluginxxStringView pluginxxView(std::string_view text) {
        return PluginxxStringView{text.data(), static_cast<uint64_t>(text.size())};
    }
protected:

    /// 同步处理器持有者基类 (类型擦除: 支持任意 lambda)
    class HookHolderBase {
    public:
        virtual ~HookHolderBase() = default;
    };

    template<typename FnT>
    struct HookHolder : HookHolderBase {
        const PluginxxHost* host = nullptr;
        FnT                 fn;

        HookHolder(const PluginxxHost* h, FnT f) :
            host(h),
            fn(std::move(f)) {}

        /// C ABI 同步处理器 trampoline
        static int32_t PLUGINXX_CALL invoke(
            void*                     user_data,
            const PluginxxStringView* /*hook_id*/,
            const PluginxxStringView* input_json,
            PluginxxString*           out_json,
            PluginxxString*           error_out
        ) {
            auto* self = static_cast<HookHolder*>(user_data);
            if (!self) {
                return -1;
            }
            try {
                std::string_view input{};
                if (input_json && input_json->data) {
                    input = std::string_view{
                        input_json->data,
                        static_cast<size_t>(input_json->size)
                    };
                }
                std::string   out;
                const int32_t rc = self->fn(input, out);
                if (rc != 0) {
                    if (error_out) {
                        hostStringSet(self->host, error_out, "hook handler failed");
                    }
                    return rc;
                }
                if (!out.empty() && out_json) {
                    hostStringSet(self->host, out_json, out);
                }
                return 0;
            } catch (const std::exception& e) {
                if (error_out) {
                    hostStringSet(self->host, error_out, e.what());
                }
                return -1;
            } catch (...) {
                if (error_out) {
                    hostStringSet(self->host, error_out, "unknown exception");
                }
                return -1;
            }
        }
    };

    /// 统一的同步处理器注册路径
    template<typename Fn>
    int32_t registerSync(const char* hookId, int32_t mode, int32_t priority, Fn&& fn) {
        return registerSyncWithTag(hookId, "", mode, priority,
                                   MUSICXX_PLUGIN_HOOK_PHASE_IMPL, std::forward<Fn>(fn));
    }

    /// 统一的同步处理器注册路径（带 `owner_tag`）
    ///
    /// `owner_tag` 是"这个处理器代表着什么"的标签：同一个插件可以在同一个钩子上登记多个
    /// 处理器（靠它区分），自省里也会原样列出；功能点的约定是 `owner_tag = 点 id`
    /// （见 [featurePointProvide]）。
    template<typename Fn>
    int32_t registerSyncWithTag(const char* hookId, const char* ownerTag, int32_t mode,
                                int32_t priority, int32_t phase, Fn&& fn) {
        // 阶段必须是钩子声明过的（老插件框架库没有 phases 字段 → 同样拒绝）
        if (phase != MUSICXX_PLUGIN_HOOK_PHASE_IMPL &&
            !hookDeclaresPhase(hookId, phase)) {
            std::string message = "钩子 `";
            message += hookId ? hookId : "";
            message += "` 没有声明 ";
            message += (phase == MUSICXX_PLUGIN_HOOK_PHASE_BEFORE ? "before" : "after");
            message += " 阶段（或插件框架库太旧）: 注册被拒绝";
            log.warn(message);
            return -1;
        }
        using FnT       = std::decay_t<Fn>;
        auto  holder    = std::make_unique<HookHolder<FnT>>(host, std::forward<Fn>(fn));
        void* holderPtr = holder.get();
        holders_.push_back(std::move(holder));

        MusicxxPluginHookSpec spec{};
        spec.version     = MUSICXX_PLUGIN_HOOK_SPEC_VERSION;
        spec.struct_size = sizeof(MusicxxPluginHookSpec);
        spec.hook_id     = pluginxxView(hookId);
        spec.owner_tag   = pluginxxView(ownerTag ? ownerTag : "");
        spec.mode        = mode;
        spec.priority    = priority;
        spec.flags       = 0;
        spec.hook_sync   = &HookHolder<FnT>::invoke;
        spec.hook_start  = nullptr;
        spec.hook_cancel = nullptr;
        spec.user_data   = holderPtr;
        spec.phase       = phase;

        if (!iface.hooks || !iface.hooks->register_hook) {
            log.warn("musicxx.hooks 表不可用: 钩子未注册 (插件框架版本过旧?)");
            return -1;
        }
        return iface.hooks->register_hook(host, &spec);
    }

private:

    /// 钩子有没有声明某个阶段（读 `hook_info` 的 `phases`）
    ///
    /// 返回 false 有两种情况：这个钩子没声明该阶段，或者插件框架库太旧（`hook_info`
    /// 里没有 `phases` 字段）。两种都按"不能注册"处理 —— 注册了也不会被派发。
    bool hookDeclaresPhase(const char* hookId, int32_t phase) const {
        if (!hookId || !iface.hooks || !iface.hooks->hook_info) {
            return false;
        }
        PluginxxString     out{};
        PluginxxStringView hookView = pluginxxView(hookId);
        if (iface.hooks->hook_info(host, &hookView, &out) != 0 || !out.data) {
            return false;
        }
        std::string text{out.data, static_cast<size_t>(out.size)};
        hostStringFree(out);
        const size_t at = text.find("\"phases\"");
        if (at == std::string::npos) {
            return false;
        }
        const char* name = phase == MUSICXX_PLUGIN_HOOK_PHASE_BEFORE ? "\"before\""
                                                                    : "\"after\"";
        return text.find(name, at) != std::string::npos;
    }

    /// 读取 host 表里的字符串型查询 (统一处理出参所有权)
    template<typename FnT>
    std::string readHostString(FnT method) const {
        if (!iface.host || !(iface.host->*method)) {
            return {};
        }
        PluginxxString out{};
        if ((iface.host->*method)(host, &out) != 0 || !out.data) {
            return {};
        }
        std::string result{out.data, static_cast<size_t>(out.size)};
        hostStringFree(out);
        return result;
    }

    /// 同步处理器持有者 (生命周期与实例相同; start 事务里累积)
    std::vector<std::unique_ptr<HookHolderBase>> holders_;

    /// 动作请求完成通知持有者 (主程序/框架会长期持有该指针; 见 requestAction 说明)
    struct ActionNotifyHolder {
        PluginxxOperatorNotify user{};      ///< 插件传入的完成通知 (原样保留)
        PluginxxOperatorNotify forward{};   ///< 转交给主程序的通知 (地址在堆上, 稳定)
        bool                   doneCalled = false;
    };

    /// 完成通知转接 (框架线程调用): 标记完成并转发给插件自己的回调
    static void PLUGINXX_CALL actionNotifyTrampoline(
        void*                     ud,
        int32_t                   status,
        const PluginxxStringView* payload
    ) noexcept {
        auto* holder = static_cast<ActionNotifyHolder*>(ud);
        if (!holder || holder->doneCalled) {
            return;
        }
        holder->doneCalled = true; ///< 框架保证恰好一次; 这里再兜一层
        if (holder->user.done) {
            try {
                holder->user.done(holder->user.host_ud, status, payload);
            } catch (...) {
                // 完成回调异常不得穿越 C ABI
            }
        }
    }

    /// 回收已完成的动作通知 (保留最近若干张作为墓碑, 防止迟到回调命中已释放内存)
    void pruneActionNotifies() {
        constexpr size_t kFinishedRetention = 8;
        size_t           finished           = 0;
        for (const auto& holder : actionNotifies_) {
            if (holder && holder->doneCalled) {
                ++finished;
            }
        }
        while (finished > kFinishedRetention) {
            const auto it = std::find_if(
                actionNotifies_.begin(),
                actionNotifies_.end(),
                [](const std::unique_ptr<ActionNotifyHolder>& holder) {
                    return holder && holder->doneCalled;
                }
            );
            if (it == actionNotifies_.end()) {
                break;
            }
            actionNotifies_.erase(it);
            --finished;
        }
    }

    std::vector<std::unique_ptr<ActionNotifyHolder>> actionNotifies_;

    /// 事件订阅处理器持有者 (同上)
    template<typename FnT>
    struct EventHolder : HookHolderBase {
        FnT fn;

        explicit EventHolder(FnT f) :
            fn(std::move(f)) {}

        /// C ABI 事件处理器 trampoline (框架线程调用; 异常必须就地吞掉)
        static void PLUGINXX_CALL invoke(const PluginxxStringView* event_json, void* ud) noexcept {
            auto* self = static_cast<EventHolder*>(ud);
            if (!self) {
                return;
            }
            try {
                std::string_view data{};
                if (event_json && event_json->data) {
                    data = std::string_view{event_json->data, static_cast<size_t>(event_json->size)};
                }
                self->fn(data);
            } catch (...) {
                // 事件处理器异常不得穿越 C ABI
            }
        }
    };

    std::vector<std::unique_ptr<HookHolderBase>> eventHolders_;

    /// 本插件 id (从 `host.info().pluginId` 读一次后缓存; 功能点糖用它过滤自己发起的调用)
    std::string pluginId_{};
    bool        pluginIdLoaded_ = false;
};

} // namespace musicxx::plugin
} // namespace musicxx

/* ==================== 生命周期事务适配 ==================== */

namespace musicxx {
namespace plugin {
namespace detail {

/// 生命周期事务适配 (导出宏内部使用): 把插件作者的简单事务函数包装成插件框架入口
///
/// 支持两种写法:
/// - **简单形式** `int32_t(Ctx&)`: 返回 0 = 事务成功, 非 0 = 失败。SDK 按插件框架约定
///   为本次事务调用一次完成通知 (`notify->done`), 插件无需关心通知协议;
/// - **插件框架原始形式** `void*(Ctx&, const PluginxxOperatorNotify*, PluginxxString*)`:
///   由插件自己调用 `notify->done` (进阶写法, 与 cxx_pluginxx 的 kit 一致)。
///
/// 为什么必须有完成通知: 插件框架把"返回 NULL 且从未 done"判为协议违约
/// (`protocol violation: null operation without done/error`), 会拒绝本次事务并回滚加载。
template<typename Ctx, typename Fn>
inline void* runLifecycleEntry(
    Ctx&                          ctx,
    const PluginxxOperatorNotify* notify,
    PluginxxString*               error_out,
    const char*                   label,
    Fn&&                          fn
) noexcept {
    using FnT = std::decay_t<Fn>;
    if constexpr (std::is_invocable_v<FnT&, Ctx&, const PluginxxOperatorNotify*, PluginxxString*>) {
        // 插件框架原始签名: 完成通知由插件自己负责
        return fn(ctx, notify, error_out);
    } else {
        int32_t     rc = -1;
        std::string message;
        try {
            rc = static_cast<int32_t>(fn(ctx));
        } catch (const std::exception& e) {
            message = e.what();
        } catch (...) {
            message = "unknown exception";
        }
        if (notify && notify->done) {
            if (rc == 0) {
                notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
            } else {
                if (message.empty()) {
                    message = std::string{label ? label : "lifecycle"} + ": 事务返回失败";
                }
                PluginxxStringView view{message.data(), static_cast<uint64_t>(message.size())};
                notify->done(notify->host_ud, PLUGINXX_OPERATOR_FAILED, &view);
            }
        }
        return nullptr;
    }
}

} // namespace detail
} // namespace plugin
} // namespace musicxx

/// 插件入口导出宏 (生成 `musicxx_plugin_{get_info,create,start,stop,destroy}`)
///
/// 用法 (插件作者只写两个事务函数, 完成通知由 SDK 处理):
/// ```cpp
/// struct MyCtx : musicxx::plugin::PluginBase {};
/// MUSICXX_PLUGIN_EXPORT(MyCtx, "example_native", "1.0.0", "示例插件",
///                       [](MyCtx& ctx) -> int32_t { ...注册...; return 0; },
///                       [](MyCtx& ctx) -> int32_t { return 0; });
/// ```
/// 运行时框架经 `entrySymbols()` 交出同一批符号名 (见 `pluginxx/api/entry.h`)。
#define MUSICXX_PLUGIN_EXPORT(CtxType, Name, Ver, Desc, StartFn, StopFn)                     \
    PLUGINXX_EXPORT_PLUGIN(                                                                  \
        musicxx_plugin_,                                                                     \
        CtxType,                                                                             \
        Name,                                                                                \
        Ver,                                                                                 \
        Desc,                                                                                \
        +[](CtxType& ctx, const PluginxxOperatorNotify* n, PluginxxString* e) -> void* {      \
            return musicxx::plugin::detail::runLifecycleEntry(ctx, n, e, "plugin start", StartFn); \
        },                                                                                    \
        +[](CtxType& ctx, const PluginxxOperatorNotify* n, PluginxxString* e) -> void* {      \
            return musicxx::plugin::detail::runLifecycleEntry(ctx, n, e, "plugin stop", StopFn);   \
        }                                                                                     \
    )

/// 只导出 start/stop (供手写 create/destroy 的插件使用)
#define MUSICXX_PLUGIN_EXPORT_LIFECYCLE(CtxType, StartFn, StopFn)                            \
    PLUGINXX_EXPORT_PLUGIN_LIFECYCLE(                                                        \
        musicxx_plugin_,                                                                     \
        CtxType,                                                                             \
        +[](CtxType& ctx, const PluginxxOperatorNotify* n, PluginxxString* e) -> void* {      \
            return musicxx::plugin::detail::runLifecycleEntry(ctx, n, e, "plugin start", StartFn); \
        },                                                                                    \
        +[](CtxType& ctx, const PluginxxOperatorNotify* n, PluginxxString* e) -> void* {      \
            return musicxx::plugin::detail::runLifecycleEntry(ctx, n, e, "plugin stop", StopFn);   \
        }                                                                                     \
    )
