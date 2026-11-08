/// 能力实现规格: **宿主内部**的类型
///
/// 插件只看 `musicxx.capabilities` / `musicxx.broadcasts`；这个结构只剩宿主自己的
/// 实现表（`MusicxxHostManager::handlers_`）与内置 JS 引擎在进程内使用，不是插件的 ABI。
///
/// 新写原生插件请用 SDK 的 `implement` / `implementBefore` / `implementAfter`
/// （内部就是往能力表登记）。
#pragma once

#include "pluginxx/api/abi.h"

#include <cstdint>

namespace musicxx {
namespace extern_plugin {

/// 同步处理器: 返回 0 并填 out_json 表示给出裁决; 返回 0 且 out_json 为空 = 不裁决;
/// 非 0 返回值为处理器失败 (计入统计, 不影响其它处理器)
typedef int32_t(PLUGINXX_CALL *MusicxxCapabilityRunSyncFn)(
    void *user_data, const PluginxxStringView *capability_id,
    const PluginxxStringView *input_json, PluginxxString *out_json,
    PluginxxString *error_out);

/// 异步处理器: 返回 op 句柄 (可为 NULL = 已同步完成), 完成时调用 notify->done 恰好一次
typedef void *(PLUGINXX_CALL *MusicxxCapabilityRunStartFn)(
    void *user_data, const PluginxxStringView *capability_id,
    const PluginxxStringView *input_json, const PluginxxOperatorNotify *notify,
    PluginxxString *error_out);

/// 取消异步处理器 (插件框架关闭/卸载时调用)
typedef void(PLUGINXX_CALL *MusicxxCapabilityRunCancelFn)(void *user_data,
                                                         void *op);

/// 一条实现登记的规格 (宿主内部; 入口是 `MusicxxHostManager::registerHandler`)
///
/// 参与方式只有 `phase` 一项: 能登记哪些阶段、一条 id 是能力还是广播、结果怎么合并,
/// 全部由 `tools/capabilities.def.json` 的声明决定 —— 规格里不再带 mode / policy,
/// 插件也不能自选"只观察"。
typedef struct MusicxxPluginCapabilitySpec {
  PluginxxStringView capability_id; ///< 全名, 如 "musicxx.player.beforePlaySong"
  PluginxxStringView owner_tag;     ///< 同一实例内区分多个处理器 (可为空串)

  int32_t priority; ///< 小者先执行; 同优先级按注册顺序
  int32_t flags;    ///< 预留 (必须为 0)

  MusicxxCapabilityRunSyncFn run_sync;     ///< 与 run_start 二者取一
  MusicxxCapabilityRunStartFn run_start;   ///< 异步处理器
  MusicxxCapabilityRunCancelFn run_cancel; ///< 异步处理器的取消回调 (可空)

  void *user_data;

  /// 参与方式 (`MUSICXX_CAPABILITY_PHASE_*`)
  int32_t phase;
} MusicxxPluginCapabilitySpec;

} // namespace extern_plugin
} // namespace musicxx
