/// 测试夹具: "处理器总是失败"的原生插件, 用于验证宿主的熔断与派发暂停 (plan §4.10)
///
/// 行为:
/// - 注册 `musicxx.song.changed` (观察型): 每次返回非 0 (处理器失败) —— 宿主连续计数,
///   达到 3 次即临时暂停该处理器的派发 (只暂停, 不卸载插件);
/// - 注册 `musicxx.player.completed` (观察型): 始终成功 —— 用于验证"熔断只影响出问题的
///   处理器, 同一插件的其它处理器照常被调用";
/// - 能力 `plugin.fail_native.probe`: 回报两个处理器的调用次数, 供原生测试断言
///   "暂停后处理器真的没有被调用"。
///
/// 纪律与示例插件一致: `start` 事务里只做注册, 不阻塞、不做 IO。

#include "musicxx/plugin/api/plugin_kit.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace {

/// 插件实例上下文 (每个实例一份; 不得使用可变全局状态 —— 多实例铁律)
struct FailCtx : public musicxx::plugin::PluginBase {
    int32_t failingCalls = 0; ///< 失败处理器的调用次数 (宿主暂停后不再增长)
    int32_t goodCalls    = 0; ///< 正常处理器的调用次数 (不受另一个处理器熔断影响)

    int32_t onStart() {
        // 1) 总是失败的处理器: 返回非 0 = 处理器失败
        observe(MUSICXX_PLUGIN_HOOK_SONG_CHANGED, [this](std::string_view, std::string&) -> int32_t {
            ++failingCalls;
            return -1; ///< 非 0: 宿主计入 failures; 连续 3 次后暂停派发 60 秒
        });

        // 2) 始终成功的处理器: 用于验证熔断的粒度只到"单个处理器"
        observe(MUSICXX_PLUGIN_HOOK_PLAYER_COMPLETED, [this](std::string_view, std::string&) -> int32_t {
            ++goodCalls;
            return 0;
        });

        // 3) 探针能力: 回报调用次数 (宿主/测试据此判断处理器是否被执行)
        capability(
            *this,
            "plugin.fail_native.probe",
            [this](std::string_view, std::string_view) -> std::string {
                return std::string{"{\"failingCalls\":"} + std::to_string(failingCalls)
                       + ",\"goodCalls\":" + std::to_string(goodCalls) + "}";
            }
        );
        return 0;
    }

    int32_t onStop() {
        return 0;
    }
};

int32_t failStart(FailCtx& ctx) {
    return ctx.onStart();
}

int32_t failStop(FailCtx& ctx) {
    return ctx.onStop();
}

} // namespace

MUSICXX_PLUGIN_EXPORT(
    FailCtx,
    "fail_native",
    "1.0.0",
    "测试夹具: 总是失败的钩子处理器 (验证熔断); 不应出现在正式发布里",
    failStart,
    failStop
)
