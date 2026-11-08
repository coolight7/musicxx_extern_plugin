/// 对照示例: "处理器总是失败"的动态库插件, 演示插件框架对失败处理器的统计行为
///
/// 行为:
/// - 前链注册 `musicxx.player.volume`: 每次返回非 0 (处理器失败) —— 插件框架只累计 failures,
///   不暂停派发、不卸载插件;
/// - 前链注册 `musicxx.player.speed`: 始终成功 —— 用于证明"另一个处理器照常被调用";
/// - **走 `pluginxx.capabilities` 注册 `musicxx.player.pitch` 的前链**（能力表入口）：
///   处理器是能力契约（同步 `notify->done`），派发与合并和 `pluginxx.capabilities` 那套完全一样；
/// - 能力 `plugin.example_native_fail.probe`: 回报两个处理器的调用次数, 供原生测试断言。
///
/// 三个能力都是"改播放值"的固定实现点（应用自己改值）: 插件只能注册前链 / 后链,
/// 注册 `impl` 会被拒绝（返回 `MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND`）。
///
/// 与示例插件同一套约定: `start` 事务里只做注册, 不阻塞、不做 IO。

#include "musicxx/plugin/api/plugin_kit.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace {

/// 插件实例上下文 (每个实例一份; 不得使用全局可变状态 —— 多实例规则)
struct FailCtx : public musicxx::plugin::PluginBase {
  int32_t failingCalls = 0; ///< 失败处理器的调用次数 (插件框架暂停后不再增长)
  int32_t goodCalls = 0; ///< 正常处理器的调用次数 (不受另一个处理器被暂停的影响)
  int32_t pitchCalls = 0; ///< 能力表注册的处理器被调用的次数
  int32_t pitchRegisterRc = -1; ///< 能力表注册的返回码 (0 = 成功)

  /// 能力表注册的处理器: 同步给结果 (`sync = 1` 才允许挂到能力上)
  static void *PLUGINXX_CALL pitchStart(
      void *ctx, const PluginxxHost *, const PluginxxStringView *,
      const PluginxxStringView *, const PluginxxOperatorNotify *notify,
      PluginxxString *) {
    auto *self = static_cast<FailCtx *>(ctx);
    if (self) {
      ++self->pitchCalls;
    }
    if (notify && notify->done) {
      const std::string answer = R"({"patch":{"pitch":"1.25"}})";
      const PluginxxStringView view{answer.data(), answer.size()};
      notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, &view);
    }
    return nullptr;
  }

  /// 用 `pluginxx.capabilities` 直接注册一个前链处理器（手写能力规格的写法）
  void registerPitchImplementation() {
    if (!iface.capabilities || !iface.capabilities->register_capability) {
      return;
    }
    PluginxxCapabilitySpec spec{};
    spec.version = PLUGINXX_API_VERSION;
    spec.struct_size = sizeof(PluginxxCapabilitySpec);
    spec.id = pluginxxView(MUSICXX_CAPABILITY_PLAYER_PITCH);
    spec.owner_tag = pluginxxView("");
    spec.impls = PLUGINXX_CAPABILITY_IMPLS_ONE;
    spec.merge = PLUGINXX_CAPABILITY_MERGE_NONE;
    spec.phases = PLUGINXX_CAPABILITY_PHASE_MASK_BEFORE;
    spec.phase = PLUGINXX_CAPABILITY_PHASE_BEFORE;
    spec.sync = 1; ///< 前链必须是同步处理器: 要拿到这一步的结果才能跑下一步
    spec.run_start = &FailCtx::pitchStart;
    spec.user_data = this;
    PluginxxString error{};
    pitchRegisterRc =
        iface.capabilities->register_capability(host, nullptr, &spec, &error);
    if (error.data) {
      hostStringFree(error);
    }
  }

  int32_t onStart() {
    // 1) 总是失败的前链处理器: 返回非 0 = 处理器失败
    implementBefore(MUSICXX_CAPABILITY_PLAYER_VOLUME, 0,
         [this](std::string_view, std::string &) -> int32_t {
           ++failingCalls;
           return -1; ///< 非 0: 插件框架计入 failures, 每次都照常派发
         });

    // 2) 始终成功的前链处理器: 用于验证失败统计只算到"单个处理器"
    implementBefore(MUSICXX_CAPABILITY_PLAYER_SPEED, 0,
         [this](std::string_view, std::string &) -> int32_t {
           ++goodCalls;
           return 0;
         });

    // 3) 走能力表注册的能力（阶段 2c 的新入口）
    registerPitchImplementation();

    // 4) 探针能力: 回报调用次数 (插件框架/测试据此判断处理器是否被执行)
    capability(*this, "plugin.example_native_fail.probe",
               [this](std::string_view, std::string_view) -> std::string {
                 return std::string{"{\"failingCalls\":"} +
                        std::to_string(failingCalls) +
                        ",\"goodCalls\":" + std::to_string(goodCalls) +
                        ",\"pitchCalls\":" + std::to_string(pitchCalls) +
                        ",\"pitchRegisterRc\":" +
                        std::to_string(pitchRegisterRc) + "}";
               });
    return 0;
  }

  int32_t onStop() { return 0; }
};

int32_t failStart(FailCtx &ctx) { return ctx.onStart(); }

int32_t failStop(FailCtx &ctx) { return ctx.onStop(); }

} // namespace

MUSICXX_PLUGIN_EXPORT(
    FailCtx, "example_native_fail", "1.0.0",
    "对照示例: 总是失败的能力处理器 (演示暂停派发); 只用于演示与原生测试",
    failStart, failStop)
