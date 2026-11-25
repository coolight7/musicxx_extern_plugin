/// 能力实现规格: **宿主内部**的类型
///
/// 插件只看 `musicxx.capabilities` / `musicxx.broadcasts`；这个结构只剩宿主自己的
/// 实现表（`MusicxxHostManager::handlers_`）与内置 JS 引擎在进程内使用，不是插件的 ABI。
///
/// 处理器只有一种形态: 受理时给回操作句柄 (可空), 结束时调用一次 `notify->done`。
/// 它在返回之前就调完 (立即完成) 与稍后再调 (等网络 / 等脚本) 是同一件事,
/// 宿主一律按"异步完成"处理。
#pragma once

#include "pluginxx/api/abi.h"

#include <cstdint>

namespace musicxx {
namespace extern_plugin {

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

  /// 处理器 (唯一形态): 结束时间调一次 `notify->done`
  PluginxxCapabilityStartFunction run_start;
  PluginxxOperatorCancelFunction run_cancel; ///< 取消回调 (可空)

  void *user_data;

  /// 参与方式 (`MUSICXX_CAPABILITY_PHASE_*`)
  int32_t phase;
} MusicxxPluginCapabilitySpec;

} // namespace extern_plugin
} // namespace musicxx
