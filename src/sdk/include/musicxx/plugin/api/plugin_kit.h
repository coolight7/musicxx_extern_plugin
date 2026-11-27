/// musicxx 插件 SDK: 总头文件 (插件开发者只需包含本头)
///
/// 组成:
/// - 插件框架通用部分: `pluginxx/kit/kit.h` (通用表聚合 / 日志 / 任务 / 协程 / 能力注册) —
///   直接使用插件框架 `Pluginxx*` 名字, **不引入别名层**;
/// - musicxx 专用约定: `musicxx/plugin/api/plugin_api.h`;
/// - musicxx 专用接口聚合 [MusicxxPluginIfaces] 与实例上下文 [PluginBase];
/// - 便捷注册: `PluginBase::implement` / `observe` / `unimplement` / `capabilityCall` /
///   `readBroadcast` / `hostInfoJson`。
/// - 界面描述层 `pluginxx::ui`（组件模型 / 解析 / 适配 / 文本降级 / 基础 kit）
///   与 musicxx 扩展 kit `musicxx::ui::kit`：插件的页面内容用它们拼装。
///
/// 线程与约定: `create` 只构造上下文; `start` 是注册事务, 在框架线程
/// 执行且**禁止阻塞** (慢操作走 `pluginxx` 的 offload / 后台任务); `stop` 撤销自管资源;
/// `destroy` 只释放本地对象。插件内不得有可变全局状态 (多实例规则)。
#pragma once

#include "musicxx/plugin/api/capability_ids.g.h"
#include "musicxx/plugin/api/musicxx_ui_kit.g.h"
#include "musicxx/plugin/api/plugin_api.h"
#include "pluginxx/api/iface_meta.h"
#include "pluginxx/kit/kit.h"
#include "pluginxx/net/ops.h"
#include "pluginxx/ui.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
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

// 网络表是插件框架的通用表 (`<应用名>.net`), 元信息定义在插件框架库里, 这里直接用。
static_assert(PLUGINXX_IFACE_NET_BASELINE_SIZE >= 8u);
static_assert(PLUGINXX_IFACE_NET_BASELINE_SIZE <= sizeof(PluginxxNetIface));

template<>
struct IfaceMeta<MusicxxPluginKindIface> {
    static constexpr int32_t  min_version   = MUSICXX_PLUGIN_IFACE_KIND_MIN_VERSION;
    static constexpr uint32_t baseline_size = MUSICXX_PLUGIN_IFACE_KIND_BASELINE_SIZE;
};
static_assert(MUSICXX_PLUGIN_IFACE_KIND_BASELINE_SIZE >= 8u);
static_assert(MUSICXX_PLUGIN_IFACE_KIND_BASELINE_SIZE <= sizeof(MusicxxPluginKindIface));

} // namespace pluginxx

namespace musicxx {
namespace plugin {

/// 能力参与方式（与 `MUSICXX_CAPABILITY_PHASE_*` 一致）
///
/// 能力 id 表示**一次调用**，参与方式由阶段表达：
/// - `Impl`  ：实现链，给结果 / 裁决（不写阶段就是它）；
/// - `Before`：前链，串行、**正序**，只能改输入或 `cancel`；
/// - `After` ：后链，串行、**逆序**，只能改输出或 `drop`。
///
/// 后链逆序是刻意的：外置插件**最后**改输出，应用自己的兜底实现不能盖掉插件的改写
/// （与实现链里"外置优先"同向）。前链 / 后链必须是同步处理器。
/// 阶段编号 (对外字符串用)
///
/// 取值跟 `MUSICXX_CAPABILITY_PHASE_*` (`capability_ids.g.h`, 0/1/2) 一致;
/// **交给插件框架的接口表时要用 ABI 自己的编号** (1/2/4), 两套不同,
/// 转换见 [pluginxxPhaseOf]。
enum class CapabilityPhase : int32_t {
    Impl   = MUSICXX_CAPABILITY_PHASE_IMPL,
    Before = MUSICXX_CAPABILITY_PHASE_BEFORE,
    After  = MUSICXX_CAPABILITY_PHASE_AFTER,
};

/// 阶段编号 → 对外字符串（日志用）
inline const char* capabilityPhaseName(CapabilityPhase phase) {
    switch (phase) {
        case CapabilityPhase::Before:
            return "before";
        case CapabilityPhase::After:
            return "after";
        case CapabilityPhase::Impl:
        default:
            return "impl";
    }
}

/// 网络操作名 (`<应用名>.net` 表的 `op` 参数; 常量定义在 `pluginxx/net/ops.h`)
///
/// 约定: 结果 JSON 里带 `ok`; `http.request` 可以“提交请求 + 听事件”, 也可以
/// `co_await netCall(...)` 等它完成。`http.fetch` / `http.download` 是同一个 HTTP
/// 实现的另外两个形态。
namespace net_op = pluginxx::net_op;

namespace detail {

/// musicxx 的阶段编号 (0/1/2) → 接口表的阶段常量 (1/2/4)
///
/// 两套编号不同是历史原因: `capability_ids.g.h` 与 Dart 侧一致用 0/1/2,
/// 插件框架的 `PluginxxCapabilitySpec::phase` 用位值 1/2/4。写错会让前链 / 后链的
/// 行被当成 impl 行登记 (数量对不上、派发也不触发)。
constexpr int32_t pluginxxPhaseOf(int32_t phase) {
    switch (phase) {
        case MUSICXX_CAPABILITY_PHASE_BEFORE:
            return PLUGINXX_CAPABILITY_PHASE_BEFORE;
        case MUSICXX_CAPABILITY_PHASE_AFTER:
            return PLUGINXX_CAPABILITY_PHASE_AFTER;
        case MUSICXX_CAPABILITY_PHASE_IMPL:
        default:
            return PLUGINXX_CAPABILITY_PHASE_IMPL;
    }
}

/// 处理器是不是协程写法
///
/// 现在两种写法都当"异步完成"处理（同步写法的结果在 `start` 里当场交给
/// `notify.done`），这个判断只为日志与排障保留。
template<typename FnT>
constexpr bool handlerReturnsAwaitable() {
    if constexpr (std::is_invocable_v<FnT&, std::string_view, std::string_view>) {
        return pluginxx::detail::IsPluginAwaitable<
            std::invoke_result_t<FnT&, std::string_view, std::string_view>>::value;
    } else if constexpr (std::is_invocable_v<FnT&, std::string_view>) {
        return pluginxx::detail::IsPluginAwaitable<
            std::invoke_result_t<FnT&, std::string_view>>::value;
    } else {
        return false;
    }
}

/* ==================== 功能埋点糖的内部实现 (给 PluginBase 用) ==================== */

/// 把处理函数的返回值转成功能埋点的"回答值"
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
                      "featurePointProvide: the handler must return Json, or a string/number/boolean");
        return false;
    }
}

/// 功能埋点处理器：按 `point` 过滤、跳过自己发起的调用、把回答包成 `{"patch": {"value": …}}`
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
    const MusicxxPluginHostIface*  host  = nullptr; ///< "musicxx.host"
    const MusicxxPluginUIIface*    ui    = nullptr; ///< "musicxx.ui"
    const PluginxxNetIface*        net   = nullptr; ///< "<应用名>.net"
    const MusicxxPluginKindIface*  kind  = nullptr; ///< "musicxx.kind"（插件形态 / 运行器）

    /// 从框架查询全部接口表 (host 为空时返回全 NULL 聚合)
    static MusicxxPluginIfaces query(const PluginxxHost* host) {
        MusicxxPluginIfaces ifaces;
        static_cast<pluginxx::PluginIfaceCore&>(ifaces) = pluginxx::PluginIfaceCore::query(host);
        ifaces.host = pluginxx::queryInterface<MusicxxPluginHostIface>(
            host,
            MUSICXX_PLUGIN_IFACE_HOST
        );
        ifaces.ui = pluginxx::queryInterface<MusicxxPluginUIIface>(
            host,
            MUSICXX_PLUGIN_IFACE_UI
        );
        ifaces.net = pluginxx::queryInterface<PluginxxNetIface>(
            host,
            PLUGINXX_IFACE_NET
        );
        ifaces.kind = pluginxx::queryInterface<MusicxxPluginKindIface>(
            host,
            MUSICXX_PLUGIN_IFACE_KIND
        );
        return ifaces;
    }
};

/// 插件实例上下文基类 (musicxx 专用)
class PluginBase : public pluginxx::PluginBaseT<MusicxxPluginIfaces> {
public:

    /* ==================== 表调用的公共写法 (协程) ==================== */
    //
    // 接口表成员一律是"受理 + 完成回调恰好一次": 这里把它包成可以 `co_await` 的协程,
    // 调用方写顺序逻辑 (读配置 → 注册 → 订阅) 就是一行一行的 `co_await`。
    // 协程被取消 (实例关闭、业务主动取消) 时, 正在等的那次调用会在主程序侧 `op_cancel` 掉。

    /// 提交一次表调用并等结果
    template<typename StartFn>
    asio::awaitable<pluginxx::TableOpResult> tableCall(StartFn start) {
        auto ex = co_await asio::this_coro::executor;
        co_return co_await pluginxx::asyncTableOp(*this, ex, std::move(start));
    }

    /// 同上, 成功时给结果正文; 失败抛 `std::runtime_error` (取消抛 `CanceledException`)
    ///
    /// [label] 只用于错误文本 (`<label>: <原因>`), 排查时能看出是哪一步失败。
    template<typename StartFn>
    asio::awaitable<std::string> tableText(StartFn start,
                                           const char* label = nullptr) {
        auto r = co_await tableCall(std::move(start));
        const auto withLabel = [label](std::string text) {
            if (!label || !*label) {
                return text;
            }
            return std::string{label} + ": " + text;
        };
        if (!r.accepted()) {
            throw std::runtime_error(withLabel(r.startError));
        }
        if (!r.ok()) {
            throw std::runtime_error(withLabel(
                r.text().empty() ? std::string{"table call failed"}
                                 : std::string{r.text()}));
        }
        co_return std::string{r.text()};
    }

    /// 同上, 只关心成败
    template<typename StartFn>
    asio::awaitable<void> tableVoid(StartFn start, const char* label = nullptr) {
        (void)co_await tableText(std::move(start), label);
        co_return;
    }

    /// 字符串视图 → C ABI 借用视图 (指向入参内存, 只在本次调用期间有效)
    static PluginxxStringView pluginxxView(std::string_view text) {
        return PluginxxStringView{text.data(), static_cast<uint64_t>(text.size())};
    }

    /// C ABI 借用视图 → string_view (空视图给空)
    static std::string_view stringViewOf(const PluginxxStringView* view) {
        if (!view || !view->data) {
            return {};
        }
        return std::string_view{view->data, static_cast<size_t>(view->size)};
    }

    /* ==================== 网络能力 (musicxx.net) ==================== */

    bool netAvailable() const { return iface.net && iface.net->call; }

    /// 一次网络操作 (`op` 用 `pluginxx::net_op::*`), 结果是 JSON 文本
    ///
    /// 结果 JSON 里带 `ok`; 传输失败也只是一次正常完成。不想等就把它放进
    /// `spawn(...)` 起的一条任务里, 不要丢掉返回值当异步用。
    asio::awaitable<std::string> netCall(
        std::string_view op, std::string_view argsJson = "{}") {
        if (!netAvailable()) {
            throw std::runtime_error("musicxx.net table unavailable: network capability is not enabled");
        }
        std::string opText{op};
        std::string argsText{argsJson.empty() ? "{}" : argsJson};
        co_return co_await tableText(
            [this, opText, argsText](PluginxxOperatorCallback cb, void* ud,
                                     PluginxxString* err) {
                const PluginxxStringView opView = pluginxxView(opText);
                const PluginxxStringView argsView = pluginxxView(argsText);
                return iface.net->call(host, &opView, &argsView, nullptr, cb, ud, err);
            }
        );
    }

    /// 取消一个进行中的网络请求
    int32_t netCancel(int64_t requestId, std::string* outJson = nullptr) {
        if (!netAvailable() || !iface.net->cancel) {
            return -1;
        }
        PluginxxString out{};
        const int32_t rc = iface.net->cancel(host, requestId, &out);
        if (outJson && out.data) {
            outJson->assign(out.data, static_cast<size_t>(out.size));
        }
        if (out.data) {
            hostStringFree(out);
        }
        return rc;
    }

    /// 订阅本插件的网络事件 (主题 `plugin.<本插件id>.net`)
    template<typename Fn>
    asio::awaitable<PluginxxSubscription*> netSubscribe(Fn&& fn) {
        const std::string topic = "plugin." + (co_await pluginId()) + ".net";
        co_return co_await subscribeBroadcast(topic, std::forward<Fn>(fn));
    }

    /* ==================== 能力登记 (处理器可以写成协程) ==================== */

    /// 实现一条能力 (默认 `impl` 阶段)
    ///
    /// 处理器写法: `std::string(std::string_view args)` 或
    /// `asio::awaitable<std::string>(std::string_view args)`。
    /// 返回空串 = 没意见 (交给下一个实现); 协程处理器在等表调用 / 计时器时不会占住框架线程。
    template<typename Fn>
    asio::awaitable<void> implement(const char* capabilityId,
                                              int32_t priority, Fn&& fn) {
        co_await implementOn(capabilityId, "", priority,
                             MUSICXX_CAPABILITY_PHASE_IMPL,
                             std::forward<Fn>(fn));
    }

    /// 实现某条能力的某个阶段 (前链 / 实现链 / 后链)
    template<typename Fn>
    asio::awaitable<void> implement(const char* capabilityId,
                                              CapabilityPhase phase,
                                              int32_t priority, Fn&& fn) {
        co_await implementOn(capabilityId, "", priority,
                             static_cast<int32_t>(phase), std::forward<Fn>(fn));
    }

    template<typename Fn>
    asio::awaitable<void> implementBefore(const char* capabilityId,
                                                    int32_t priority, Fn&& fn) {
        co_await implement(capabilityId, CapabilityPhase::Before, priority,
                           std::forward<Fn>(fn));
    }

    template<typename Fn>
    asio::awaitable<void> implementAfter(const char* capabilityId,
                                                   int32_t priority, Fn&& fn) {
        co_await implement(capabilityId, CapabilityPhase::After, priority,
                           std::forward<Fn>(fn));
    }

    /// 撤掉本插件在这条能力上的实现 (owner_tag = "")
    asio::awaitable<void> unimplement(const char* capabilityId) {
        co_await unimplementWithTag(capabilityId, "");
    }

    /// 在一条共享能力上登记一条带 `ownerTag` 的实现 (同一个 id 上可以留多行)
    ///
    /// 典型用法: 功能埋点通道 `musicxx.feature.point.call` (ownerTag = 点 id)。
    template<typename Fn>
    asio::awaitable<void> implementWithTag(const char* capabilityId,
                                           const char* ownerTag,
                                           int32_t priority, Fn&& fn) {
        co_await implementOn(capabilityId, ownerTag ? ownerTag : "", priority,
                             MUSICXX_CAPABILITY_PHASE_IMPL, std::forward<Fn>(fn));
    }
    /// 撤掉本插件在某条能力上带 `ownerTag` 的那个实现
    asio::awaitable<void> unimplementWithTag(const char* capabilityId,
                                                       const char* ownerTag) {
        std::string id = capabilityId ? capabilityId : "";
        std::string tag = ownerTag ? ownerTag : "";
        if (!iface.capabilities || !iface.capabilities->unregister_capability) {
            throw std::runtime_error("musicxx.capabilities table unavailable");
        }
        co_await tableVoid([this, id, tag](PluginxxOperatorCallback cb, void* ud,
                                           PluginxxString* err) {
            const PluginxxStringView idView = pluginxxView(id);
            const PluginxxStringView tagView = pluginxxView(tag);
            return iface.capabilities->unregister_capability(host, &idView,
                                                             &tagView, cb, ud, err);
        }, "unregister_capability");
    }

    /* ==================== 广播 ==================== */

    /// 读一条频道的当前值 (没有当前值时抛异常, 原因 `no_value` / `not_declared`)
    asio::awaitable<std::string> readBroadcast(std::string_view key) {
        if (key.empty()) {
            throw std::runtime_error("readBroadcast: empty key");
        }
        std::string id{key};
        co_return co_await tableText(
            [this, id](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                const PluginxxStringView idView = pluginxxView(id);
                return iface.broadcasts->read_broadcast(host, &idView, cb, ud, err);
            },
            "read_broadcast"
        );
    }

    /// 发布一条广播 (只能发布本插件命名空间下的频道)
    asio::awaitable<void> publishBroadcast(
        std::string_view channel, std::string_view payloadJson = "{}") {
        std::string id{channel};
        std::string payload{payloadJson.empty() ? "{}" : payloadJson};
        co_await tableVoid(
            [this, id, payload](PluginxxOperatorCallback cb, void* ud,
                                PluginxxString* err) {
                const PluginxxStringView idView = pluginxxView(id);
                const PluginxxStringView payloadView = pluginxxView(payload);
                return iface.broadcasts->publish_broadcast(host, &idView,
                                                           &payloadView, cb, ud, err);
            }, "publish_broadcast");
    }

    /// 订阅一条频道 (处理器形如 `void(std::string_view payload_json)`)
    ///
    /// 订阅是异步的: 完成时主程序交回订阅句柄 (十进制文本), 这里解析成指针返回。
    /// 句柄由实例保活 (卸载时一并回收), 插件不需要自己记。
    template<typename Fn>
    asio::awaitable<PluginxxSubscription*> subscribeBroadcast(
        std::string_view topic, Fn&& fn, std::string_view optionsJson = {}) {
        co_return co_await subscribeBroadcastImpl(topic, std::forward<Fn>(fn),
                                                  nullptr, optionsJson);
    }

    /// 订阅并接收"流结束"通知 (处理器形如 `void(std::string_view reason_json)`)
    template<typename Fn, typename EndFn>
    asio::awaitable<PluginxxSubscription*> subscribeBroadcastWithEnd(
        std::string_view topic, Fn&& fn, EndFn&& onEnd,
        std::string_view optionsJson = {}) {
        co_return co_await subscribeBroadcastImpl(topic, std::forward<Fn>(fn),
                                                  std::forward<EndFn>(onEnd),
                                                  optionsJson);
    }

    /// 结束自己的一条流 (只有频道所有者能结束)
    asio::awaitable<void> endBroadcast(
        std::string_view topic, std::string_view reasonJson = "{}") {
        std::string id{topic};
        std::string reason{reasonJson.empty() ? "{}" : reasonJson};
        co_await tableVoid(
            [this, id, reason](PluginxxOperatorCallback cb, void* ud,
                               PluginxxString* err) {
                const PluginxxStringView idView = pluginxxView(id);
                const PluginxxStringView reasonView = pluginxxView(reason);
                return iface.broadcasts->end_broadcast(host, &idView,
                                                       &reasonView, cb, ud, err);
            }, "end_broadcast");
    }

    /* ==================== 宿主信息 ==================== */

    /// 框架信息 JSON (主程序版本 / 平台 / 语言 / 插件目录 / 本插件 id)
    asio::awaitable<std::string> hostInfoJson() {
        co_return co_await tableText(
            [this](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return iface.host->get_info(host, cb, ud, err);
            }, "get_info");
    }

    /// 本插件配置文件路径
    asio::awaitable<std::string> configPath() {
        co_return co_await tableText(
            [this](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return iface.host->get_plugin_config_path(host, cb, ud, err);
            }, "get_plugin_config_path");
    }

    /// 本插件的 id (读一次 `host.info()` 里的 `pluginId` 后缓存)
    asio::awaitable<std::string> pluginId() {
        if (!pluginIdLoaded_) {
            pluginIdLoaded_ = true;
            try {
                const utilxx_base::Json info =
                    utilxx_base::Json::parse(co_await hostInfoJson());
                if (info.is_object()) {
                    pluginId_ = info.value<std::string>("pluginId", std::string{});
                }
            } catch (const std::exception&) {
                pluginId_.clear();
            }
        }
        co_return pluginId_;
    }

    /* ==================== 调别的插件 / 应用的能力 ==================== */

    /// 调一条能力 (协程): 结果 JSON 文本; 业务失败抛异常 (正文是原因)
    asio::awaitable<std::string> capabilityCall(
        std::string_view capability, std::string_view argsJson = "{}") {
        co_return co_await capabilityCallWithTimeout(capability, argsJson, 0);
    }

    /// 调一条能力 (协程), 并给**这一次调用**一个超时 (毫秒)
    ///
    /// - `timeoutMs > 0`: 到点按取消终结, 本协程抛出异常;
    /// - `timeoutMs <= 0`: 只按声明里的超时 (一直等)。
    asio::awaitable<std::string> capabilityCallWithTimeout(
        std::string_view capability, std::string_view argsJson,
        int32_t timeoutMs) {
        if (!iface.capabilities || !iface.capabilities->invoke_capability) {
            throw std::runtime_error("musicxx.capabilities table unavailable");
        }
        std::string cap{capability};
        std::string args{argsJson.empty() ? "{}" : argsJson};
        co_return co_await tableText(
            [this, cap, args, timeoutMs](PluginxxOperatorCallback cb, void* ud,
                                         PluginxxString* err) {
                const PluginxxStringView capView = pluginxxView(cap);
                const PluginxxStringView argsView = pluginxxView(args);
                return iface.capabilities->invoke_capability(
                    host, nullptr, &capView, &argsView, 0, timeoutMs, cb, ud, err);
            });
    }

    /* ==================== 功能埋点 ==================== */

    /// 功能埋点声明信息 (可读字段)
    struct MusicxxPluginPointSpec {
        const char* title  = ""; ///< 展示名 (缺省用 id)
        const char* depict = ""; ///< 一句话说明
        const char* args   = ""; ///< 参数说明
        const char* result = ""; ///< 结果说明
    };

    /// 声明一个自己的功能埋点 (`id` 必须形如 `plugin.<自己的插件 id>.<名字>`)
    asio::awaitable<void> featurePointDefine(
        const char* id, const MusicxxPluginPointSpec& spec) {
        utilxx_base::Json payload = utilxx_base::Json::object();
        payload["id"] = id ? id : "";
        payload["title"] = spec.title ? spec.title : "";
        payload["depict"] = spec.depict ? spec.depict : "";
        payload["args"] = spec.args ? spec.args : "";
        payload["result"] = spec.result ? spec.result : "";
        (void)co_await capabilityCall("musicxx.feature.point.define", payload.dump());
    }

    /// 收回自己声明的功能埋点
    asio::awaitable<void> featurePointUndefine(const char* id) {
        utilxx_base::Json payload = utilxx_base::Json::object();
        payload["id"] = id ? id : "";
        (void)co_await capabilityCall("musicxx.feature.point.undefine", payload.dump());
    }

    /// 登记一个功能埋点的实现
    ///
    /// 功能埋点共用**一条固定通道** `musicxx.feature.point.call` (点是运行期数据,
    /// 不可能逐条登记成能力): 处理器登记在这条通道上, `owner_tag` = 点 id。
    ///
    /// 处理器三种写法 (按可调用性自动选一种):
    /// `fn(point, argsJson, caller)` / `fn(point, argsJson)` / `fn(argsJson)`;
    /// 返回 `utilxx_base::Json` / 字符串 / 数字 / 布尔, 返回空串 = 这次不回答。
    template<typename Fn>
    asio::awaitable<void> featurePointProvide(const char* pointId,
                                                        Fn&& fn,
                                                        int32_t priority = 0) {
        using FnT = std::decay_t<Fn>;
        const std::string selfId = co_await pluginId();
        auto handler = [fn = FnT(std::forward<Fn>(fn)),
                        point = std::string{pointId ? pointId : ""},
                        selfId](std::string_view argsJson) mutable -> std::string {
            std::string out;
            detail::answerFeaturePoint(selfId, point, fn, argsJson, out);
            return out;
        };
        co_await implementWithTag(MUSICXX_CAPABILITY_FEATURE_POINT_CALL, pointId,
                                  priority, std::move(handler));
    }

    /// 撤回某个功能埋点的实现
    asio::awaitable<void> featurePointUnprovide(const char* pointId) {
        co_await unimplementWithTag(MUSICXX_CAPABILITY_FEATURE_POINT_CALL,
                                    pointId ? pointId : "");
    }

    /// 调一个功能埋点: 结果是被回答的值 (没人回答给 `null`)
    asio::awaitable<utilxx_base::Json> featurePointCall(
        const char* id, std::string_view argsJson = "{}") {
        utilxx_base::Json payload = utilxx_base::Json::object();
        payload["point"] = id ? id : "";
        payload["caller"] = co_await pluginId();
        payload["args"] = utilxx_base::Json::parse(
            argsJson.empty() ? "{}" : std::string{argsJson});
        const std::string answer =
            co_await capabilityCall("musicxx.feature.point.call", payload.dump());
        utilxx_base::Json parsed = utilxx_base::Json::parse(answer.empty() ? "null" : answer);
        if (parsed.is_object() && parsed.contains("patch") &&
            parsed["patch"].is_object() && parsed["patch"].contains("value")) {
            co_return parsed["patch"]["value"];
        }
        co_return utilxx_base::Json(nullptr);
    }

    /// 列出现存的全部功能埋点 (JSON 文本)
    asio::awaitable<std::string> featurePointList() {
        co_return co_await capabilityCall("musicxx.feature.point.list", "{}");
    }

    /* ==================== 插件形态 (kind) ==================== */

    asio::awaitable<void> kindRegister(std::string_view specJson) {
        std::string spec{specJson};
        co_await tableVoid(
            [this, spec](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                const PluginxxStringView view = pluginxxView(spec);
                return iface.kind->register_kind(host, &view, cb, ud, err);
            });
    }

    asio::awaitable<void> kindUnregister(std::string_view kind) {
        std::string name{kind};
        co_await tableVoid(
            [this, name](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                const PluginxxStringView view = pluginxxView(name);
                return iface.kind->unregister_kind(host, &view, cb, ud, err);
            });
    }

    asio::awaitable<std::string> kindList() {
        co_return co_await tableText(
            [this](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return iface.kind->list_kinds(host, cb, ud, err);
            });
    }

    /* ==================== 界面 (声明项 / 内容 / 槽位) ==================== */

    asio::awaitable<void> uiItemRegister(
        const char* itemName, const char* type, const char* dataJson,
        int32_t order = 0) {
        std::string name = itemName ? itemName : "";
        std::string typeText = type ? type : "";
        std::string data = (dataJson && *dataJson) ? dataJson : "{}";
        co_await tableVoid(
            [this, name, typeText, data, order](PluginxxOperatorCallback cb,
                                                void* ud, PluginxxString* err) {
                MusicxxPluginUIEntrySpec spec{};
                spec.version = 1;
                spec.struct_size = sizeof(MusicxxPluginUIEntrySpec);
                spec.item_id = pluginxxView(name);
                spec.type = pluginxxView(typeText);
                spec.data_json = pluginxxView(data);
                spec.order = order;
                return iface.ui->register_entry(host, &spec, cb, ud, err);
            });
    }

    asio::awaitable<void> uiItemUnregister(const char* itemName) {
        std::string name = itemName ? itemName : "";
        co_await tableVoid(
            [this, name](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                const PluginxxStringView view = pluginxxView(name);
                return iface.ui->unregister_entry(host, &view, cb, ud, err);
            });
    }

    asio::awaitable<std::string> uiItemEntries() {
        co_return co_await tableText(
            [this](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return iface.ui->list_entries(host, cb, ud, err);
            });
    }

    /// 注册/覆盖一份内容
    ///
    /// `generate` 是 C 函数指针 (签名见 `plugin_api.h`): 拿到 context 后经 `notify`
    /// 恰好一次交回 `{"json":…,"data":…}`。它可以同步完成, 也可以先返回、稍后再补
    /// (例如协程里 `co_await` 一次网络请求) —— 主程序不会为了等它停下帧。
    template<typename GenerateFn>
    asio::awaitable<void> uiContentRegister(
        const char* contentId, const char* slot, GenerateFn generate,
        std::string_view title = {}, std::string_view depict = {},
        std::string_view routesJson = {}, std::string_view depsJson = "[]",
        int32_t order = 0, int32_t minIntervalMs = 0) {
        std::string id = contentId ? contentId : "";
        std::string slotName = slot ? slot : "";
        std::string titleText{title};
        std::string depictText{depict};
        // 空串不能当“没给”: 主程序按 `data != nullptr` 判定有没有给值, 空串会被
        // 当成 "" 去解析 JSON 而失败。routes / deps 缺省都是空数组。
        std::string routes = routesJson.empty() ? std::string{"[]"}
                                                : std::string{routesJson};
        std::string deps = depsJson.empty() ? std::string{"[]"}
                                            : std::string{depsJson};
        auto generatePtr = +generate;
        co_await tableVoid(
            [this, id, slotName, titleText, depictText, routes, deps, order,
             minIntervalMs, generatePtr](PluginxxOperatorCallback cb, void* ud,
                                          PluginxxString* err) {
                MusicxxPluginUIContentSpec spec{};
                spec.version = 1;
                spec.struct_size = sizeof(MusicxxPluginUIContentSpec);
                spec.id = pluginxxView(id);
                spec.slot = pluginxxView(slotName);
                spec.title = pluginxxView(titleText);
                spec.depict = pluginxxView(depictText);
                spec.routes_json = pluginxxView(routes);
                spec.deps_json = pluginxxView(deps);
                spec.order = order;
                spec.min_interval_ms = minIntervalMs;
                spec.generate = generatePtr;
                spec.user_data = nullptr;
                return iface.ui->register_content(host, &spec, cb, ud, err);
            });
    }

    asio::awaitable<void> uiContentUnregister(const char* contentId) {
        std::string id = contentId ? contentId : "";
        co_await tableVoid(
            [this, id](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                const PluginxxStringView view = pluginxxView(id);
                return iface.ui->unregister_content(host, &view, cb, ud, err);
            });
    }

    asio::awaitable<void> uiContentPush(const char* contentId,
                                                 std::string_view contentJson) {
        std::string id = contentId ? contentId : "";
        std::string json{contentJson};
        co_await tableVoid(
            [this, id, json](PluginxxOperatorCallback cb, void* ud,
                             PluginxxString* err) {
                const PluginxxStringView idView = pluginxxView(id);
                const PluginxxStringView jsonView = pluginxxView(json);
                return iface.ui->push_content(host, &idView, &jsonView, cb, ud, err);
            });
    }

    asio::awaitable<std::string> uiContentEntries() {
        co_return co_await tableText(
            [this](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return iface.ui->list_contents(host, cb, ud, err);
            });
    }

    asio::awaitable<void> uiSlotUpdate(const char* slot,
                                                std::string_view idsJson = {}) {
        std::string slotName = slot ? slot : "";
        std::string ids{idsJson};
        co_await tableVoid(
            [this, slotName, ids](PluginxxOperatorCallback cb, void* ud,
                                  PluginxxString* err) {
                const PluginxxStringView slotView = pluginxxView(slotName);
                const PluginxxStringView idsView = pluginxxView(ids);
                return iface.ui->update_slot(host, &slotView,
                                             ids.empty() ? nullptr : &idsView,
                                             cb, ud, err);
            });
    }

    asio::awaitable<void> uiSlotDefine(const char* slot,
                                                const char* kind) {
        std::string slotName = slot ? slot : "";
        std::string kindText = kind ? kind : "";
        co_await tableVoid(
            [this, slotName, kindText](PluginxxOperatorCallback cb, void* ud,
                                       PluginxxString* err) {
                MusicxxPluginUISlotSpec spec{};
                spec.version = 1;
                spec.struct_size = sizeof(MusicxxPluginUISlotSpec);
                spec.id = pluginxxView(slotName);
                spec.kind = pluginxxView(kindText);
                return iface.ui->define_slot(host, &spec, cb, ud, err);
            });
    }

    asio::awaitable<void> uiSlotUndefine(const char* slot) {
        std::string slotName = slot ? slot : "";
        co_await tableVoid(
            [this, slotName](PluginxxOperatorCallback cb, void* ud,
                             PluginxxString* err) {
                const PluginxxStringView view = pluginxxView(slotName);
                return iface.ui->undefine_slot(host, &view, cb, ud, err);
            });
    }

    asio::awaitable<std::string> uiSlotList() {
        co_return co_await tableText(
            [this](PluginxxOperatorCallback cb, void* ud, PluginxxString* err) {
                return iface.ui->list_slots(host, cb, ud, err);
            });
    }

    /// 提示 (等价于能力 `musicxx.ui.toast`)
    asio::awaitable<void> uiNotify(std::string_view messageJson) {
        std::string message{messageJson};
        co_await tableVoid(
            [this, message](PluginxxOperatorCallback cb, void* ud,
                            PluginxxString* err) {
                const PluginxxStringView view = pluginxxView(message);
                return iface.ui->notify(host, &view, cb, ud, err);
            });
    }

protected:

    /* ==================== 处理器与订阅的持有者 ==================== */

    /// 协程处理器的“已受理”句柄 (交回主程序的那个 `void*`)
    ///
    /// 协议要求: 入口返回非空句柄 = 已受理, 完成经 `notify.done` 交回; 返回空且还
    /// 未通知会被判成协议违约。句柄留在实例里, 主程序迟到的取消只会置一个标记,
    /// 不会碰到已释放内存。
    struct AsyncJobHandle final : pluginxx::detail::CancelableProvider {
        std::shared_ptr<std::atomic<bool>> cancelRequested =
            std::make_shared<std::atomic<bool>>(false);
        std::atomic<bool> finished{false};

        void requestCancel() noexcept override {
            cancelRequested->store(true, std::memory_order_release);
        }
    };

    /// 保留一个协程处理器的句柄 (完成之后的句柄下次登记时清掉)
    void keepAsyncJob(const std::shared_ptr<AsyncJobHandle>& job) {
        if (!job) {
            return;
        }
        for (auto it = asyncJobs_.begin(); it != asyncJobs_.end();) {
            const bool finished =
                *it && (*it)->finished.load(std::memory_order_acquire);
            it = finished ? asyncJobs_.erase(it) : std::next(it);
        }
        asyncJobs_.push_back(job);
    }

    /// 能力处理器基类 (生命周期 = 实例; 注销时只摘登记, 对象留到实例销毁)
    struct HandlerBase {
        virtual ~HandlerBase() = default;
    };

    template<typename FnT>
    struct HandlerHolder final : HandlerBase {
        PluginBase* base = nullptr;
        FnT         fn;

        HandlerHolder(PluginBase* owner, FnT f) :
            base(owner),
            fn(std::move(f)) {}

        /// 上报同步处理器的结果 (空串 = 这次没意见, 交给下一个实现)
        static void report(const PluginxxOperatorNotify* notify,
                           std::string_view body) {
            if (!notify || !notify->done) {
                return;
            }
            if (body.empty()) {
                notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
                return;
            }
            const PluginxxStringView view{body.data(), body.size()};
            notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &view);
        }

        /// 上报失败 (正文写人看得懂的原因)
        static void reportFailure(const PluginxxOperatorNotify* notify,
                                  std::string_view reason) {
            if (!notify || !notify->done) {
                return;
            }
            const PluginxxStringView view{reason.data(), reason.size()};
            notify->done(notify->host_ud, PLUGINXX_OPERATOR_FAILED, &view);
        }

        /// 协程处理器: 交给一条后台任务跑, 算完经 notify 交回
        ///
        /// 不走 provider 根: 能力行只需要"算出一个值交回主程序", 而后台任务的生命
        /// 周期由实例统一管理 (停用 / 卸载 / 实例析构时一起收束)。交回的句柄只是
        /// "已受理"的凭据 (主程序用它做取消), 协程完成后置位却在实例里留着。
        template<typename MakeAw>
        static void* startJob(PluginBase* owner,
                              const PluginxxOperatorNotify* notify,
                              MakeAw makeAw) {
            auto job = std::make_shared<AsyncJobHandle>();
            owner->keepAsyncJob(job);
            const PluginxxOperatorNotify notifyCopy = *notify;
            owner->spawn([job, notifyCopy, makeAw = std::move(makeAw)](
                             PluginBase&, pluginxx::OpCtl&) mutable
                             -> asio::awaitable<void> {
                try {
                    std::string out = co_await makeAw();
                    report(&notifyCopy, out);
                } catch (const pluginxx::CanceledException&) {
                    if (notifyCopy.done) {
                        notifyCopy.done(notifyCopy.host_ud,
                                        PLUGINXX_OPERATOR_CANCELED, nullptr);
                    }
                } catch (const std::exception& e) {
                    reportFailure(&notifyCopy, e.what());
                } catch (...) {
                    reportFailure(&notifyCopy, "unknown exception");
                }
                job->finished.store(true, std::memory_order_release);
                co_return;
            });
            return job.get();
        }

        /// 能力处理器入口 (插件框架在插件执行线程调用)
        ///
        /// 处理器四种写法, 按可调用性选一种:
        /// - `awaitable<string>(method, args)` / `awaitable<string>(args)`: 协程处理器,
        ///   等待表调用 / 计时器时让出框架线程;
        /// - `string(method, args)` / `string(args)`: 同步处理器;
        /// - 老写法 `int32_t(args, out&)`: 0 = 成功, `out` 空 = 这次没意见。
        static void* PLUGINXX_CALL start(
            void*                         userData,
            const PluginxxHost*           /*callerHost*/,
            const PluginxxStringView*     methodView,
            const PluginxxStringView*     argsJson,
            const PluginxxOperatorNotify* notify,
            PluginxxString*               /*errorOut*/
        ) {
            auto* self = static_cast<HandlerHolder*>(userData);
            if (!self || !notify || !notify->done) {
                return nullptr;
            }
            const std::string method{stringViewOf(methodView)};
            const std::string input{stringViewOf(argsJson)};
            try {
                if constexpr (std::is_invocable_v<FnT&, std::string_view,
                                                  std::string_view>) {
                    using Ret = std::invoke_result_t<FnT&, std::string_view,
                                                     std::string_view>;
                    if constexpr (pluginxx::detail::IsPluginAwaitable<Ret>::value) {
                        auto fnCopy = self->fn;
                        const auto methodPtr = std::make_shared<std::string>(method);
                        const auto inputPtr = std::make_shared<std::string>(input);
                        return startJob(
                            self->base, notify,
                            [fnCopy, methodPtr, inputPtr]() mutable {
                                return fnCopy(std::string_view{*methodPtr},
                                              std::string_view{*inputPtr});
                            });
                    } else {
                        report(notify, self->fn(std::string_view{method},
                                               std::string_view{input}));
                    }
                } else if constexpr (std::is_invocable_v<FnT&, std::string_view>) {
                    using Ret = std::invoke_result_t<FnT&, std::string_view>;
                    if constexpr (pluginxx::detail::IsPluginAwaitable<Ret>::value) {
                        auto fnCopy = self->fn;
                        const auto inputPtr = std::make_shared<std::string>(input);
                        return startJob(self->base, notify,
                                        [fnCopy, inputPtr]() mutable {
                                            return fnCopy(std::string_view{*inputPtr});
                                        });
                    } else {
                        report(notify, self->fn(std::string_view{input}));
                    }
                } else if constexpr (std::is_invocable_v<FnT&, std::string_view,
                                                         std::string&>) {
                    // 老写法: int32_t(input, out&) —— 0 = 成功 (out 空 = 没意见)
                    std::string out;
                    const int32_t rc = self->fn(std::string_view{input}, out);
                    if (rc != 0) {
                        reportFailure(notify, "handler failed");
                    } else {
                        report(notify, out);
                    }
                } else {
                    static_assert(
                        std::is_invocable_v<FnT&, std::string_view>,
                        "capability handler must be callable as (method, args) / (args) / (args, out)");
                }
                return nullptr;
            } catch (const std::exception& e) {
                reportFailure(notify, e.what());
            } catch (...) {
                reportFailure(notify, "unknown exception");
            }
            return nullptr;
        }

        /// 取消一个还在跑的协程处理器 (框架关闭 / 调用方撤销)
        static void PLUGINXX_CALL cancel(void* /*userData*/, void* op) {
            if (!op) {
                return;
            }
            // 协程处理器不交回可取消句柄 (由后台任务自己的取消机制收尾)
            static_cast<pluginxx::detail::CancelableProvider*>(op)->requestCancel();
        }
    };

    /// 事件订阅的持有者 (生命周期 = 实例)
    struct EventHolder {
        std::function<void(std::string_view)> onEvent;
        std::function<void(std::string_view)> onEnd;
    };

    static void PLUGINXX_CALL eventTrampoline(
        const PluginxxStringView* payloadJson, void* ud) {
        auto* holder = static_cast<EventHolder*>(ud);
        if (!holder || !holder->onEvent) {
            return;
        }
        try {
            holder->onEvent(stringViewOf(payloadJson));
        } catch (...) {
            // 处理器异常不得穿越 C ABI
        }
    }

    static void PLUGINXX_CALL endTrampoline(
        const PluginxxStringView* reasonJson, void* ud) {
        auto* holder = static_cast<EventHolder*>(ud);
        if (!holder || !holder->onEnd) {
            return;
        }
        try {
            holder->onEnd(stringViewOf(reasonJson));
        } catch (...) {
        }
    }

    /// 订阅的公共实现 (带 / 不带流结束回调)
    template<typename Fn, typename EndFn>
    asio::awaitable<PluginxxSubscription*> subscribeBroadcastImpl(
        std::string_view topic, Fn&& fn, EndFn&& onEnd,
        std::string_view optionsJson) {
        if (!iface.broadcasts) {
            throw std::runtime_error("musicxx.broadcasts table unavailable");
        }
        auto holder = std::make_shared<EventHolder>();
        holder->onEvent = [fn = std::decay_t<Fn>(std::forward<Fn>(fn))](
                              std::string_view payload) mutable { fn(payload); };
        if constexpr (!std::is_same_v<std::decay_t<EndFn>, std::nullptr_t>) {
            holder->onEnd = [end = std::decay_t<EndFn>(std::forward<EndFn>(onEnd))](
                                std::string_view reason) mutable { end(reason); };
        }
        events_.push_back(holder);
        std::string topicText{topic};
        std::string options{optionsJson};
        const std::string handle = co_await tableText(
            [this, topicText, options, holder](PluginxxOperatorCallback cb,
                                               void* ud, PluginxxString* err) {
                const PluginxxStringView topicView = pluginxxView(topicText);
                const PluginxxStringView optionsView = pluginxxView(options);
                const PluginxxStringView* optionsPtr =
                    options.empty() ? nullptr : &optionsView;
                if (iface.broadcasts->subscribe_broadcast_ex &&
                    holder->onEnd) {
                    return iface.broadcasts->subscribe_broadcast_ex(
                        host, nullptr, &topicView, &eventTrampoline,
                        &endTrampoline, holder.get(), optionsPtr, cb, ud, err);
                }
                return iface.broadcasts->subscribe_broadcast(
                    host, nullptr, &topicView, &eventTrampoline, holder.get(),
                    optionsPtr, cb, ud, err);
            }, "subscribe_broadcast");
        uint64_t value = 0;
        for (const char c : handle) {
            if (c < '0' || c > '9') {
                value = 0;
                break;
            }
            value = value * 10 + static_cast<uint64_t>(c - '0');
        }
        co_return reinterpret_cast<PluginxxSubscription*>(
            static_cast<uintptr_t>(value));
    }

    /// 统一的处理器登记路径 (带 owner_tag / 阶段 / 优先级)
    template<typename Fn>
    asio::awaitable<void> implementOn(const char* capabilityId,
                                                const char* ownerTag,
                                                int32_t priority, int32_t phase,
                                                Fn&& fn) {
        if (!iface.capabilities || !iface.capabilities->register_capability) {
            throw std::runtime_error("musicxx.capabilities table unavailable");
        }
        using FnT = std::decay_t<Fn>;
        auto holder = std::make_shared<HandlerHolder<FnT>>(
            this, FnT(std::forward<Fn>(fn)));
        handlers_.push_back(holder);
        HandlerHolder<FnT>* raw = holder.get();
        std::string id = capabilityId ? capabilityId : "";
        std::string tag = ownerTag ? ownerTag : "";
        co_await tableVoid(
            [this, id, tag, priority, phase, raw](PluginxxOperatorCallback cb,
                                                  void* ud, PluginxxString* err) {
                PluginxxCapabilitySpec spec{};
                spec.version = PLUGINXX_API_VERSION;
                spec.struct_size = sizeof(PluginxxCapabilitySpec);
                spec.id = pluginxxView(id);
                spec.owner_tag = pluginxxView(tag);
                // 自己声明的 id 只保留一条实现 (再注册即覆盖): 合并策略只在
                // "多个实现共存"时才有意义, 这里不开放 (主程序会拒绝 many + none)
                spec.impls = PLUGINXX_CAPABILITY_IMPLS_ONE;
                spec.merge = PLUGINXX_CAPABILITY_MERGE_NONE;
                spec.phases = PLUGINXX_CAPABILITY_PHASE_MASK_IMPL |
                              PLUGINXX_CAPABILITY_PHASE_MASK_BEFORE |
                              PLUGINXX_CAPABILITY_PHASE_MASK_AFTER;
                spec.phase = detail::pluginxxPhaseOf(phase);
                spec.priority = priority;
                // 处理器只有一种形态: 受理时给回句柄 (可空), 结束时调一次 `notify.done`
                // (同步写法的结果由 `start` 当场交回, 不再有 `sync` 标记)
                // 两形态共用同一个取消入口: 同步处理器不交回句柄 (已经通知过),
                // 协程处理器交回 [HandlerHolder::AsyncJobHandle]
                spec.cancel = &HandlerHolder<FnT>::cancel;
                spec.user_data = raw;
                spec.run_start = &HandlerHolder<FnT>::start;
                return iface.capabilities->register_capability(host, nullptr,
                                                               &spec, cb, ud, err);
            }, id.c_str());
    }

private:

    /// 主程序堆字符串 → std::string (并把内存还回去)
    void hostStringFree(PluginxxString& s) const {
        if (s.data && host && host->vtable && host->vtable->free) {
            host->vtable->free(s.data);
        }
        s.data = nullptr;
        s.size = 0;
    }

    std::vector<std::shared_ptr<HandlerBase>>    handlers_;
    std::vector<std::shared_ptr<EventHolder>>    events_;
    std::vector<std::shared_ptr<AsyncJobHandle>> asyncJobs_;
    std::string pluginId_{};
    bool        pluginIdLoaded_ = false;
};

} // namespace plugin
} // namespace musicxx


/* ==================== 生命周期事务适配 ==================== */

namespace musicxx {
namespace plugin {
namespace detail {

/// 生命周期事务适配 (导出宏内部使用): 把插件开发者的事务函数包装成插件框架入口
///
/// 支持三种写法:
/// - **同步** `int32_t(Ctx&)` / `void(Ctx&)`: 跑完就通知 (0 = 成功);
/// - **协程** `asio::awaitable<void>(Ctx&)`: 在实例的驱动桥上跑, 事务里可以 `co_await`
///   注册 / 订阅 / 读配置, 完成或抛异常时通知恰好一次;
/// - **插件框架原始形参** `(Ctx&, const PluginxxOperatorNotify*, PluginxxString*)`:
///   插件自己负责完成通知。
template<typename Ctx, typename Fn>
inline void runLifecycleEntry(Ctx&                          ctx,
                              const PluginxxOperatorNotify* notify,
                              PluginxxString*               error_out,
                              const char*                   label,
                              Fn&&                          fn) noexcept {
    using FnT = std::decay_t<Fn>;
    auto finish = [notify](int32_t status, const std::string& message) {
        if (!notify || !notify->done) {
            return;
        }
        if (status == PLUGINXX_OPERATOR_OK) {
            notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
            return;
        }
        const PluginxxStringView view{message.data(), message.size()};
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_FAILED, &view);
    };

    if constexpr (std::is_invocable_v<FnT&, Ctx&, const PluginxxOperatorNotify*,
                                      PluginxxString*>) {
        fn(ctx, notify, error_out);
        return;
    } else {
        try {
            using Ret = std::invoke_result_t<FnT&, Ctx&>;
            if constexpr (pluginxx::detail::IsPluginAwaitable<Ret>::value) {
                const PluginxxOperatorNotify notifyCopy =
                    notify ? *notify : PluginxxOperatorNotify{nullptr, nullptr};
                ctx.spawn(
                    [fn = FnT(std::forward<Fn>(fn)), notifyCopy, label](
                        Ctx& owner, pluginxx::OpCtl&) mutable
                        -> asio::awaitable<void> {
                              (void)owner;
                              int32_t     status = PLUGINXX_OPERATOR_OK;
                              std::string message;
                              try {
                                  co_await fn(owner);
                              } catch (const std::exception& e) {
                                  status  = PLUGINXX_OPERATOR_FAILED;
                                  message = e.what();
                              } catch (...) {
                                  status  = PLUGINXX_OPERATOR_FAILED;
                                  message = std::string{label ? label : "lifecycle"} +
                                            ": unknown exception";
                              }
                              if (!notifyCopy.done) {
                                  co_return;
                              }
                              if (status == PLUGINXX_OPERATOR_OK) {
                                  notifyCopy.done(notifyCopy.host_ud,
                                                  PLUGINXX_OPERATOR_OK, nullptr);
                              } else {
                                  const PluginxxStringView view{message.data(),
                                                                message.size()};
                                  notifyCopy.done(notifyCopy.host_ud,
                                                  PLUGINXX_OPERATOR_FAILED, &view);
                              }
                          });
                return;
            } else if constexpr (std::is_void_v<Ret>) {
                fn(ctx);
                finish(PLUGINXX_OPERATOR_OK, {});
            } else {
                const int32_t rc = static_cast<int32_t>(fn(ctx));
                finish(rc == 0 ? PLUGINXX_OPERATOR_OK : PLUGINXX_OPERATOR_FAILED,
                       rc == 0 ? std::string{}
                               : std::string{label ? label : "lifecycle"} +
                                     ": transaction failed");
            }
        } catch (const std::exception& e) {
            finish(PLUGINXX_OPERATOR_FAILED, e.what());
        } catch (...) {
            finish(PLUGINXX_OPERATOR_FAILED, "unknown exception");
        }
    }
}

} // namespace detail
} // namespace plugin
} // namespace musicxx

/// 插件入口导出宏 (生成 `musicxx_plugin_{get_info,create,stop,destroy}`)
///
/// 用法 (插件开发者只写两个事务函数, 完成通知由 SDK 处理):
/// ```cpp
/// struct MyCtx : musicxx::plugin::PluginBase {};
/// MUSICXX_PLUGIN_EXPORT(MyCtx, "example_native", "1.0.0", "示例插件",
///                       [](MyCtx& ctx) -> asio::awaitable<void> {
///                           co_await ctx.uiItemRegister("card", "home.entry", R"({"title":"卡片"})");
///                       },
///                       [](MyCtx& ctx) -> asio::awaitable<void> {
///                           co_await ctx.uiItemUnregister("card");
///                       });
/// ```
/// `create` 就是"构造 + 注册事务": 插件框架先建实例, 再调这里的第一个事务;
/// 禁用后再启用时不会再建实例, 只重跑注册事务。
#define MUSICXX_PLUGIN_EXPORT(CtxType, Name, Ver, Desc, CreateFn, StopFn)                    \
    PLUGINXX_EXPORT_PLUGIN(                                                                  \
        musicxx_plugin_,                                                                     \
        CtxType,                                                                             \
        Name,                                                                                \
        Ver,                                                                                 \
        Desc,                                                                                \
        +[](CtxType& ctx, const PluginxxOperatorNotify* n, PluginxxString* e) -> void {       \
            musicxx::plugin::detail::runLifecycleEntry(ctx, n, e, "plugin create", CreateFn); \
        },                                                                                    \
        +[](CtxType& ctx, const PluginxxOperatorNotify* n, PluginxxString* e) -> void {       \
            musicxx::plugin::detail::runLifecycleEntry(ctx, n, e, "plugin stop", StopFn);     \
        }                                                                                     \
    )

/// 只导出 stop (供手写 create/destroy 的插件使用)
#define MUSICXX_PLUGIN_EXPORT_LIFECYCLE(CtxType, StopFn)                                     \
    PLUGINXX_EXPORT_PLUGIN_LIFECYCLE(                                                        \
        musicxx_plugin_,                                                                     \
        CtxType,                                                                             \
        +[](CtxType& ctx, const PluginxxOperatorNotify* n, PluginxxString* e) -> void {       \
            musicxx::plugin::detail::runLifecycleEntry(ctx, n, e, "plugin stop", StopFn);     \
        }                                                                                     \
    )
