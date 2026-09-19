/// 示例原生插件: 演示钩子注册、状态镜像读取、宿主动作请求与日志
///
/// 行为:
/// 1. 注册 `musicxx.player.beforePlaySong` (decision): 歌曲名含"广告" → 返回 skip 裁决;
/// 2. 注册 `musicxx.song.changed` (observe): 读状态镜像里的歌曲名并写日志;
/// 3. 注册 `musicxx.player.error` (decision): 连续错误 2 次后建议换源。
///
/// 纪律 (plan §4.5): start 事务里只做注册, 不阻塞、不发起网络/大文件操作。

#include "musicxx/plugin/api/plugin_kit.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace {

/// 插件实例上下文 (每个实例一份; 不得使用可变全局状态 —— 多实例铁律)
struct ExampleCtx : public musicxx::plugin::PluginBase {
    /// 连续播放错误计数 (仅示例)
    int32_t consecutiveErrors = 0;

    int32_t onStart() {
        // 1) 跳过"广告"曲目 (决定型钩子: 返回 {"action":"skip"})
        hook(
            "musicxx.player.beforePlaySong",
            0,
            [this](std::string_view input, std::string& out) -> int32_t {
                // 载荷: {"sid":"...","song":{"name":"...","artist":"..."},...}
                if (input.find("\xe5\xb9\xbf\xe5\x91\x8a") != std::string_view::npos) {
                    out = R"({"action":"skip","reason":"example_native: 广告曲目"})";
                    log.info("example_native: 命中广告曲目, 请求跳过");
                }
                return 0;
            }
        );

        // 2) 观察切歌 (只读状态镜像 + 日志)
        observe("musicxx.song.changed", [this](std::string_view input, std::string& out) -> int32_t {
            (void)out;
            (void)input;
            const std::string song = stateJson("musicxx.state.song");
            log.info("example_native: 歌曲变化事件; 状态镜像长度=" + std::to_string(song.size()));
            return 0;
        });

        // 3) 播放错误: 连续 2 次错误后建议换源 (不改写宿主既有错误策略)
        hook("musicxx.player.error", 0, [this](std::string_view input, std::string& out) -> int32_t {
            (void)input;
            ++consecutiveErrors;
            if (consecutiveErrors >= 2) {
                out = R"({"action":"continue","patch":{"tryNextSrc":true}})";
            }
            return 0;
        });

        return 0;
    }

    int32_t onStop() {
        // 撤销自管资源 (示例没有线程/定时器; 钩子注册由宿主在 detach 时兜底摘除)
        consecutiveErrors = 0;
        return 0;
    }
};

/// start 事务: 只做注册 (禁止阻塞)
///
/// 入口约定 (内核 kit): 返回 NULL 表示事务成功 (同步完成); 失败时写 error_out 并返回非空。
void* exampleStart(ExampleCtx& ctx, const PluginxxOperatorNotify*, PluginxxString*) {
    return ctx.onStart() == 0 ? nullptr : reinterpret_cast<void*>(1);
}

/// stop 事务: 撤销自管资源
void* exampleStop(ExampleCtx& ctx, const PluginxxOperatorNotify*, PluginxxString*) {
    return ctx.onStop() == 0 ? nullptr : reinterpret_cast<void*>(1);
}

} // namespace

MUSICXX_PLUGIN_EXPORT(
    ExampleCtx,
    "example_native",
    "1.0.0",
    "musicxx 外部插件示例 (钩子/状态镜像/日志)",
    exampleStart,
    exampleStop
)
