/// musicxx 插件 SDK: 伞头 (插件作者只需包含本头)
///
/// 组成:
/// - 内核通用部分: `pluginxx/kit/kit.h` (通用表聚合 / 日志 / 任务 / 协程 / 能力注册) —
///   直接使用内核 `Pluginxx*` 名字, **不引入别名层** (决策 11);
/// - musicxx 领域契约: `musicxx/plugin/api/plugin_api.h`;
/// - musicxx 领域聚合 [MusicxxPluginIfaces] 与实例上下文 [PluginBase];
/// - 便捷注册: `PluginBase::hook` / `observe` / `unregisterHook` / `requestAction` /
///   `stateJson` / `hostInfoJson`。
///
/// 线程与纪律 (见 plan §4.5): `create` 只构造上下文; `start` 是注册事务, 在宿主线程
/// 执行且**禁止阻塞** (慢操作走 `pluginxx` 的 offload / 后台任务); `stop` 撤销自管资源;
/// `destroy` 只释放本地对象。插件内不得有可变全局状态 (多实例铁律)。
#pragma once

#include "musicxx/plugin/api/plugin_api.h"
#include "pluginxx/kit/kit.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace musicxx {
namespace plugin {


/// musicxx 领域接口表聚合 = 内核通用十表 + musicxx 领域表
///
/// 作为 `pluginxx::PluginBaseT<IfacesT>` 的模板实参使用 (上下文基类会调用 [query]
/// 拉取全部接口表; 宿主未实现的表为 nullptr)。
struct MusicxxPluginIfaces : public pluginxx::PluginIfaceCore {
    const MusicxxPluginHooksIface* hooks = nullptr; ///< "musicxx.hooks"
    const MusicxxPluginHostIface*  host  = nullptr; ///< "musicxx.host"
    const MusicxxPluginUIIface*    ui    = nullptr; ///< "musicxx.ui"

    /// 从宿主查询全部接口表 (host 为空时返回全 NULL 聚合)
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
        return ifaces;
    }
};

/// 插件实例上下文基类 (musicxx 领域)
class PluginBase : public pluginxx::PluginBaseT<MusicxxPluginIfaces> {
public:

    /// 注册裁决型钩子 (同步处理器)
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

    /// 同步读状态镜像 (键不存在/宿主未实现返回空串)
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

    /// 宿主信息 JSON (应用版本/平台/语言/插件目录)
    std::string hostInfoJson() const {
        return readHostString(&MusicxxPluginHostIface::get_info);
    }

    /// 本插件配置路径 (供插件读写自己的配置)
    std::string configPath() const {
        return readHostString(&MusicxxPluginHostIface::get_plugin_config_path);
    }

    /// 发起宿主动作 (异步; 结果经 `notify->done` 恰好一次回调)
    ///
    /// 宿主会持有完成通知指针直到本次动作终结 (完成 / 超时 / 取消), 因此 SDK 把通知
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

    /// 订阅宿主事件总线的主题 (**宿主线程**执行处理器; 处理器不得阻塞)
    /// - 主题必须符合命名空间规则 (plan §3.5): 官方 `musicxx.*` 或插件自定义
    ///   `plugin.<本插件id>.*`; 其它前缀会被宿主拒绝 (返回 -1);
    /// - 处理器形如 `void(std::string_view event_json)`;
    /// - 订阅句柄由宿主按实例保活 (随 stop/卸载自动撤销), 插件无需保存;
    /// - 处理器持有者与钩子处理器同一存放位置, 生命周期 = 插件实例。
    template<typename Fn>
    int32_t subscribeTopic(const char* topic, Fn&& fn) {
        if (!iface.events || !iface.events->subscribe) {
            log.warn("pluginxx.events 表不可用: 事件订阅未生效 (宿主版本过旧?)");
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

    /* ---------- 声明式 UI 扩展 (plan §5.6; 名字用短名, 宿主自动补前缀) ---------- */

    /// 注册一个 UI 项: `itemName` 用短名 (如 "card"), `type` 取 MUSICXX_PLUGIN_UI_TYPE_*,
    /// `dataJson` 是类型相关的声明式内容 (JSON 对象)
    int32_t uiRegister(const char* itemName, const char* type, const char* dataJson, int32_t order = 0) {
        if (!iface.ui || !iface.ui->register_entry) {
            log.warn("musicxx.ui 表不可用: UI 项未注册 (宿主版本过旧?)");
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

    /// 用宿主堆把字符串写进跨边界出参 (插件必须用宿主的分配器, 不得用 CRT malloc)
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

    /// 释放宿主堆字符串 (经宿主 vtable; 插件不得用 CRT free)
    void hostStringFree(PluginxxString& s) const {
        if (s.data && host && host->vtable && host->vtable->free) {
            host->vtable->free(s.data);
        }
        s.data = nullptr;
        s.size = 0;
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
        using FnT       = std::decay_t<Fn>;
        auto  holder    = std::make_unique<HookHolder<FnT>>(host, std::forward<Fn>(fn));
        void* holderPtr = holder.get();
        holders_.push_back(std::move(holder));

        MusicxxPluginHookSpec spec{};
        spec.version     = 1;
        spec.struct_size = sizeof(MusicxxPluginHookSpec);
        spec.hook_id     = pluginxxView(hookId);
        spec.owner_tag   = pluginxxView("");
        spec.mode        = mode;
        spec.priority    = priority;
        spec.flags       = 0;
        spec.hook_sync   = &HookHolder<FnT>::invoke;
        spec.hook_start  = nullptr;
        spec.hook_cancel = nullptr;
        spec.user_data   = holderPtr;

        if (!iface.hooks || !iface.hooks->register_hook) {
            log.warn("musicxx.hooks 表不可用: 钩子未注册 (宿主版本过旧?)");
            return -1;
        }
        return iface.hooks->register_hook(host, &spec);
    }

private:

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

    /// 动作请求完成通知持有者 (宿主会长期持有该指针; 见 requestAction 说明)
    struct ActionNotifyHolder {
        PluginxxOperatorNotify user{};      ///< 插件传入的完成通知 (原样保留)
        PluginxxOperatorNotify forward{};   ///< 转交给宿主的通知 (地址在堆上, 稳定)
        bool                   doneCalled = false;
    };

    /// 完成通知转接 (宿主线程调用): 标记完成并转发给插件自己的回调
    static void PLUGINXX_CALL actionNotifyTrampoline(
        void*                     ud,
        int32_t                   status,
        const PluginxxStringView* payload
    ) noexcept {
        auto* holder = static_cast<ActionNotifyHolder*>(ud);
        if (!holder || holder->doneCalled) {
            return;
        }
        holder->doneCalled = true; ///< 宿主保证恰好一次; 这里再兜一层
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

        /// C ABI 事件处理器 trampoline (宿主线程调用; 异常必须就地吞掉)
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
};

} // namespace musicxx::plugin
} // namespace musicxx

/* ==================== 生命周期事务适配 ==================== */

namespace musicxx {
namespace plugin {
namespace detail {

/// 生命周期事务适配 (导出宏内部使用): 把插件作者的简单事务函数包装成内核入口
///
/// 支持两种写法:
/// - **简单形式** `int32_t(Ctx&)`: 返回 0 = 事务成功, 非 0 = 失败。SDK 按内核契约
///   为本次事务调用一次完成通知 (`notify->done`), 插件无需关心通知协议;
/// - **内核原始形式** `void*(Ctx&, const PluginxxOperatorNotify*, PluginxxString*)`:
///   由插件自己调用 `notify->done` (进阶写法, 与 cxx_pluginxx 的 kit 一致)。
///
/// 为什么必须有完成通知: 内核把"返回 NULL 且从未 done"判为协议违约
/// (`protocol violation: null operation without done/error`), 会拒绝本次事务并回滚装载。
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
        // 内核原始签名: 完成通知由插件自己负责
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
/// 运行时宿主经 `entrySymbols()` 交出同一批符号名 (见 `pluginxx/api/entry.h`)。
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
