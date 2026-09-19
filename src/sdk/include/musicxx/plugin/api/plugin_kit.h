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

#include <cstring>
#include <memory>
#include <string>
#include <string_view>
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
        PluginxxStringView actionView = pluginxxView(action);
        PluginxxStringView argsView   = pluginxxView(argsJson ? argsJson : "{}");
        return iface.host->request_action(host, &actionView, &argsView, timeoutMs, notify, outRequestId);
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
};

} // namespace musicxx::plugin
} // namespace musicxx

/// 插件入口导出宏 (生成 `musicxx_plugin_{get_info,create,start,stop,destroy}`)
///
/// 用法 (与 agentxx 插件一致的心智模型):
/// ```cpp
/// struct MyCtx : musicxx::plugin::PluginBase {};
/// MUSICXX_PLUGIN_EXPORT(MyCtx, "example_native", "1.0.0", "示例插件",
///                       [](MyCtx& ctx) -> int32_t { ...注册...; return 0; },
///                       [](MyCtx& ctx) -> int32_t { return 0; });
/// ```
/// 运行时宿主经 `entrySymbols()` 交出同一批符号名 (见 `pluginxx/api/entry.h`)。
#define MUSICXX_PLUGIN_EXPORT(CtxType, Name, Ver, Desc, StartFn, StopFn) \
    PLUGINXX_EXPORT_PLUGIN(musicxx_plugin_, CtxType, Name, Ver, Desc, StartFn, StopFn)

/// 只导出 start/stop (供手写 create/destroy 的插件使用)
#define MUSICXX_PLUGIN_EXPORT_LIFECYCLE(CtxType, StartFn, StopFn) \
    PLUGINXX_EXPORT_PLUGIN_LIFECYCLE(musicxx_plugin_, CtxType, StartFn, StopFn)
