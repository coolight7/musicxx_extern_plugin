/// 值频道注册规格: **宿主内部**的类型
///
/// 阶段 2c 起插件只看 `musicxx.capabilities` / `musicxx.broadcasts`，`musicxx.broadcasts`
/// 表已经删除；这里的常量与结构只剩宿主自己的值频道表
/// (`MusicxxHostManager::channels_` / `musicxx_host_channels.cpp`) 与内置 JS 引擎在进程内使用，
/// 不再是插件的 ABI。
///
/// 新写插件请用广播频道读当前值、用能力读写或命令应用。
#pragma once

#include "pluginxx/api/abi.h"

#include <cstdint>

/* 能力位 (声明里三者可只实现一部分; 能力位不含某项时对应操作返回 -9) */
#define MUSICXX_PLUGIN_CHANNEL_CAP_GET    0x1 ///< 可读
#define MUSICXX_PLUGIN_CHANNEL_CAP_SET    0x2 ///< 可写
#define MUSICXX_PLUGIN_CHANNEL_CAP_NOTIFY 0x4 ///< 值变化可订阅 (watch 也要求这一位)

/* 写权限 (只对插件注册的值频道有意义) */
#define MUSICXX_PLUGIN_CHANNEL_WRITE_ANY   0 ///< 任何插件与应用都可写 (默认)
#define MUSICXX_PLUGIN_CHANNEL_WRITE_OWNER 1 ///< 只有属主自己可写

/* 值频道模式 */
/// 值由框架代存 (默认): 插件 `set` 自己的值频道即提交, 不需要维护副本
#define MUSICXX_PLUGIN_CHANNEL_MODE_DECLARED 0
/// 读写都转给属主 (框架只留同步读缓存): 属主处理 `musicxx.broadcast.read` /
/// `musicxx.broadcast.write` 两个事件, 用 `set` 自己完成回答/提交
#define MUSICXX_PLUGIN_CHANNEL_MODE_HANDLER 1

/* 值频道表返回码 (含义与 C ABI 的错误码一致) */
#define MUSICXX_PLUGIN_CHANNEL_ERR_ARG        -1 ///< 参数非法 (键名不合法 / caps 为空等)
#define MUSICXX_PLUGIN_CHANNEL_ERR_STATE      -2 ///< 状态错误 (插件框架未启动)
#define MUSICXX_PLUGIN_CHANNEL_ERR_JSON       -3 ///< 值不是合法 JSON
#define MUSICXX_PLUGIN_CHANNEL_ERR_NOT_FOUND  -4 ///< 键不存在 (含官方键还没被应用声明)
#define MUSICXX_PLUGIN_CHANNEL_ERR_TIMEOUT    -5 ///< 属主没在超时前回答 (读写请求)
#define MUSICXX_PLUGIN_CHANNEL_ERR_PERMISSION -6 ///< 命名空间不属于本插件 / 写权限不足
/// 超限: 当前框架已无容量类限制 (值频道数 / 值频道总数 / 订阅数 / 值大小都不再限制),
/// 该错误码保留供老插件兼容, 不会再由此类检查产生
#define MUSICXX_PLUGIN_CHANNEL_ERR_LIMIT -7
/// 该值频道没有声明对应能力 (读只写值频道、写只读值频道)
/// 说明: C ABI 的 -8 已用于"内存不足", 因此这里取 -9 避免同一数值两种含义
#define MUSICXX_PLUGIN_CHANNEL_ERR_UNSUPPORTED -9

/* 值频道变更事件 (插件框架 → 插件; 订阅后经事件总线送达) */
/// 载荷: {key, value, prev, by, revision, ts, removed?, coalesced?}
/// - `by` = 写入方插件 id (空串 = 应用); 值频道随插件停用/卸载消失时带 `removed:true`;
/// - 被插件框架按 `notifyThrottleMs` 合并过的通知带 `coalesced` (合并掉几次)
#define MUSICXX_PLUGIN_EVENT_BROADCAST_CHANGED "musicxx.broadcast.changed"
/// 写请求的最终结果 (发给发起 `set` 的插件)
/// 载荷: {requestId, key, accepted, value?, changed?, error?}
#define MUSICXX_PLUGIN_EVENT_BROADCAST_WRITE_RESULT "musicxx.broadcast.writeResult"
/// 属主被取真实值 (handler 模式专用; 属主收到后用 set 自己回答)
/// 载荷: {requestId, key}
#define MUSICXX_PLUGIN_EVENT_BROADCAST_READ "musicxx.broadcast.read"
/// 属主收到写请求 (handler 模式专用; 属主写入后用 set 自己提交最终值)
/// 载荷: {requestId, key, value, by}
#define MUSICXX_PLUGIN_EVENT_BROADCAST_WRITE "musicxx.broadcast.write"

/// 值频道注册规格
typedef struct MusicxxPluginChannelSpec {
  int32_t version;      ///< == 1
  uint32_t struct_size; ///< == sizeof(MusicxxPluginChannelSpec)

  PluginxxStringView key; ///< 短名 "tip.start" 或本插件全名 "plugin.<id>.tip.start"
  int32_t caps;           ///< MUSICXX_PLUGIN_CHANNEL_CAP_* (非 0)
  int32_t write_scope;    ///< MUSICXX_PLUGIN_CHANNEL_WRITE_*
  int32_t mode;           ///< MUSICXX_PLUGIN_CHANNEL_MODE_*

  /// 属主侧的推送节流设置 (毫秒; 0 = 不节流)
  ///
  /// 只是声明 (插件框架不强制度量): 属主在推送前按同一窗口合并变化。
  /// 高频值 (超过约 5 Hz) 建议声明, 否则插件框架会记一条警告日志。
  int32_t throttle_ms;
  /// 框架侧通知合并窗口 (毫秒; 0 = 不合并)
  int32_t notify_throttle_ms;
  /// handler 模式: 有人 watch 之后, 插件框架每隔多久来取一次真实值
  /// (0 = 只在 watch 时取一次)
  int32_t refresh_after_ms;

  PluginxxStringView type;       ///< 可空: bool | number | string | json (只做展示与粗校验)
  PluginxxStringView value_json; ///< 可空: 缓存初值 (不是真值来源; handler 模式可留空)
  /// 可空: {"title":"..","depict":"..","options":[".."],"risk":"low"}
  PluginxxStringView meta_json;
} MusicxxPluginChannelSpec;
