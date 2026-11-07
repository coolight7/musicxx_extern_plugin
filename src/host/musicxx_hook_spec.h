/// 钩子注册规格: **宿主内部**的类型
///
/// 阶段 2c 起插件只看 `pluginxx.capabilities` / `pluginxx.broadcasts`，`musicxx.hooks`
/// 表已经删除；这个结构只剩宿主自己的实现表（`MusicxxHostManager::hooks_`）与内置
/// JS 引擎在进程内使用，不再是插件的 ABI。
///
/// 新写原生插件请用 SDK 的 `observe` / `decide`（内部就是往能力表登记）。
#pragma once

#include "pluginxx/api/abi.h"

#include <cstddef>
#include <cstdint>

namespace musicxx {
namespace extern_plugin {

/// 同步处理器: 返回 0 并填 out_json 表示给出裁决; 返回 0 且 out_json 为空 = 不裁决;
/// 非 0 返回值为处理器失败 (计入统计, 不影响其它处理器)
typedef int32_t(PLUGINXX_CALL *MusicxxPluginHookSyncFn)(
    void *user_data, const PluginxxStringView *hook_id,
    const PluginxxStringView *input_json, PluginxxString *out_json,
    PluginxxString *error_out);

/// 异步处理器: 返回 op 句柄 (可为 NULL = 已同步完成), 完成时调用 notify->done 恰好一次
typedef void *(PLUGINXX_CALL *MusicxxPluginHookStartFn)(
    void *user_data, const PluginxxStringView *hook_id,
    const PluginxxStringView *input_json, const PluginxxOperatorNotify *notify,
    PluginxxString *error_out);

/// 取消异步处理器 (插件框架关闭/卸载时调用)
typedef void(PLUGINXX_CALL *MusicxxPluginHookCancelFn)(void *user_data, void *op);

/// 钩子注册规格的版本号（`MusicxxPluginHookSpec.version`）
///
/// - `1`：老结构（没有 `phase` 成员），一律按实现链（`impl`）注册；
/// - `2`：带 `phase`（参与方式，见 `MUSICXX_PLUGIN_HOOK_PHASE_*`）。
#define MUSICXX_PLUGIN_HOOK_SPEC_VERSION 2

/// 老结构（`version = 1`，没有 `phase`）的字节数
#define MUSICXX_PLUGIN_HOOK_SPEC_V1_SIZE (offsetof(MusicxxPluginHookSpec, phase))

typedef struct MusicxxPluginHookSpec {
  int32_t version;      ///< == 1 (老结构) 或 2 (带 phase)
  uint32_t struct_size; ///< == sizeof(MusicxxPluginHookSpec)

  PluginxxStringView hook_id;   ///< 全名, 如 "musicxx.player.beforePlaySong"
  PluginxxStringView owner_tag; ///< 同一实例内区分多个处理器 (可为空串)

  int32_t mode;     ///< MUSICXX_PLUGIN_HOOK_MODE_*
  int32_t priority; ///< 小者先执行; 同优先级按注册顺序
  int32_t flags;    ///< 预留 (必须为 0)

  MusicxxPluginHookSyncFn hook_sync; ///< 与 hook_start 二者取一
  MusicxxPluginHookStartFn hook_start;   ///< 异步处理器
  MusicxxPluginHookCancelFn hook_cancel; ///< 异步处理器的取消回调 (可空)

  void *user_data;

  /// 参与方式 (`MUSICXX_PLUGIN_HOOK_PHASE_*`; `0` = `impl`)
  int32_t phase;
} MusicxxPluginHookSpec;

} // namespace extern_plugin
} // namespace musicxx
