/// musicxx 插件 SDK: 领域 C 契约 (与宿主领域相关, 与内核通用部分分离)
///
/// 命名: 官方标识统一 `musicxx.*` 前缀; 插件自定义统一 `plugin.<pluginId>.*`。
/// 本头只声明**跨边界稳定契约** (类型/宏/IID), 纯 C 可包含 (便于 C 插件与外部工具)。
///
/// 目录:
/// - 入口符号名: `musicxx_plugin_*` (由宿主在 entrySymbols() 中交出同一批名字)
/// - 领域接口表 IID: `musicxx.hooks` / `musicxx.host` / `musicxx.ui` / `musicxx.vars` /
///   `musicxx.player` / `musicxx.library` / `musicxx.lyrics` / `musicxx.storage` /
///   `musicxx.net` / `musicxx.stats`
///   (v1 已实现 hooks / host / ui / vars 四张; 其余 IID 先冻结, 表版本 1 留待后续实现)
/// - 钩子模式 / 标志 / 裁决动作常量
#ifndef MUSICXX_PLUGIN_API_H
#define MUSICXX_PLUGIN_API_H

#include <stddef.h>
#include <stdint.h>

#include "musicxx/plugin/api/hook_ids.g.h"
#include "pluginxx/api/abi.h"
#include "pluginxx/api/tables.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 入口符号名 (宿主自定义) ==================== */

#define MUSICXX_PLUGIN_SYMBOL_GET_INFO "musicxx_plugin_get_info"
#define MUSICXX_PLUGIN_SYMBOL_CREATE   "musicxx_plugin_create"
#define MUSICXX_PLUGIN_SYMBOL_START    "musicxx_plugin_start"
#define MUSICXX_PLUGIN_SYMBOL_STOP     "musicxx_plugin_stop"
#define MUSICXX_PLUGIN_SYMBOL_DESTROY  "musicxx_plugin_destroy"

/// 插件 ABI 版本 (清单 api_version 必须 >= 本值)
#define MUSICXX_PLUGIN_API_VERSION 1

#pragma pack(push, 8)

/* ==================== 钩子: 模式 / 标志 / 裁决动作 ==================== */

/// 观察型 (通知, 无裁决): 处理器返回值忽略
#define MUSICXX_PLUGIN_HOOK_MODE_OBSERVE  0
/// 裁决型: 处理器可返回裁决对象 (JSON), 宿主按钩子的合并策略处理
#define MUSICXX_PLUGIN_HOOK_MODE_DECISION 1

/// 只允许同步处理器 (异步处理器注册会被拒绝)
#define MUSICXX_PLUGIN_HOOK_FLAG_SYNC_ONLY 0x0001

/// 裁决动作 (与 Dart 侧 `MusicxxPluginHookAction` 一一对应)
#define MUSICXX_PLUGIN_HOOK_ACTION_CONTINUE "continue"
#define MUSICXX_PLUGIN_HOOK_ACTION_SKIP     "skip"
#define MUSICXX_PLUGIN_HOOK_ACTION_CANCEL   "cancel"
#define MUSICXX_PLUGIN_HOOK_ACTION_REPLACE  "replace"

/* ==================== 钩子注册规格 ==================== */

/// 同步处理器: 返回 0 并填 out_json 表示给出裁决; 返回 0 且 out_json 为空 = 不裁决;
/// 非 0 返回值为处理器失败 (计入统计, 不影响其它处理器)
typedef int32_t(PLUGINXX_CALL* MusicxxPluginHookSyncFn)(
    void*                     user_data,
    const PluginxxStringView* hook_id,
    const PluginxxStringView* input_json,
    PluginxxString*           out_json,
    PluginxxString*           error_out
);

/// 异步处理器: 返回 op 句柄 (可为 NULL = 已同步完成), 完成时调用 notify->done 恰好一次
typedef void*(PLUGINXX_CALL* MusicxxPluginHookStartFn)(
    void*                             user_data,
    const PluginxxStringView*         hook_id,
    const PluginxxStringView*         input_json,
    const PluginxxOperatorNotify*     notify,
    PluginxxString*                   error_out
);

/// 取消异步处理器 (宿主关闭/卸载时调用)
typedef void(PLUGINXX_CALL* MusicxxPluginHookCancelFn)(void* user_data, void* op);

typedef struct MusicxxPluginHookSpec {
    int32_t          version;     ///< == 1
    uint32_t         struct_size; ///< == sizeof(MusicxxPluginHookSpec)

    PluginxxStringView hook_id;   ///< 全名, 如 "musicxx.player.beforePlaySong"
    /// 同一实例内区分多个处理器 (可为空串)
    PluginxxStringView owner_tag;

    int32_t mode;     ///< MUSICXX_PLUGIN_HOOK_MODE_*
    int32_t priority; ///< 小者先执行; 同优先级按注册顺序
    int32_t flags;    ///< MUSICXX_PLUGIN_HOOK_FLAG_*

    MusicxxPluginHookSyncFn  hook_sync;   ///< 可与 hook_start 二者取一 (推荐决定型用同步)
    MusicxxPluginHookStartFn hook_start;  ///< 异步处理器
    MusicxxPluginHookCancelFn hook_cancel;///< 异步处理器的取消回调 (可空)

    void* user_data;
} MusicxxPluginHookSpec;

/* ==================== 接口表: musicxx.hooks ==================== */

#define MUSICXX_PLUGIN_IFACE_HOOKS         "musicxx.hooks"
#define MUSICXX_PLUGIN_IFACE_HOOKS_VERSION 1

typedef struct MusicxxPluginHooksIface {
    int32_t  version;     ///< == MUSICXX_PLUGIN_IFACE_HOOKS_VERSION
    uint32_t struct_size;

    /// 注册处理器 (同一个 (实例, hook_id, owner_tag) 覆盖式注册)
    /// - 返回 0 成功; -1 参数非法; -2 状态错误 (未启动/已关闭/已禁用);
    ///   -4 未知钩子 (宿主未知的 hook_id 一律拒绝)
    int32_t(PLUGINXX_CALL* register_hook)(
        const PluginxxHost*               host,
        const MusicxxPluginHookSpec*      spec
    );

    /// 注销处理器 (hook_id + owner_tag 定位; owner_tag 为空串表示该 hook 的默认处理器)
    int32_t(PLUGINXX_CALL* unregister_hook)(
        const PluginxxHost*               host,
        const PluginxxStringView*         hook_id,
        const PluginxxStringView*         owner_tag
    );

    /// 钩子信息 (mode/policy/budget 等; JSON 对象); 未知钩子返回 -4
    int32_t(PLUGINXX_CALL* hook_info)(
        const PluginxxHost*               host,
        const PluginxxStringView*         hook_id,
        PluginxxString*                   out_json
    );
} MusicxxPluginHooksIface;

/* ==================== 接口表: musicxx.host ==================== */

#define MUSICXX_PLUGIN_IFACE_HOST         "musicxx.host"
#define MUSICXX_PLUGIN_IFACE_HOST_VERSION 1

typedef struct MusicxxPluginHostIface {
    int32_t  version;     ///< == MUSICXX_PLUGIN_IFACE_HOST_VERSION
    uint32_t struct_size;

    /// 宿主信息 (应用版本/平台/语言/插件目录; JSON 对象)
    int32_t(PLUGINXX_CALL* get_info)(const PluginxxHost* host, PluginxxString* out_json);

    /// 同步读状态镜像 (键不存在返回 -4; 值经 out_json 输出)
    int32_t(PLUGINXX_CALL* get_state)(
        const PluginxxHost*               host,
        const PluginxxStringView*         key,
        PluginxxString*                   out_json
    );

    /// 声明关心的状态键 (宿主可在这些键变化时向插件推送事件)
    int32_t(PLUGINXX_CALL* subscribe_state)(
        const PluginxxHost*               host,
        const PluginxxStringView*         keys_json,
        int32_t*                          out_count
    );

    /// 本插件配置路径 (宿主按插件 id 推导; 供插件读写自己的配置)
    int32_t(PLUGINXX_CALL* get_plugin_config_path)(
        const PluginxxHost*               host,
        PluginxxString*                   out_path
    );

    /// 发起宿主动作 (插件 → Dart, 异步 request/response)
    /// - action 全名: 官方 `musicxx.<域>.<动作>` / 插件自定义 `plugin.<pluginId>.<动作>`
    /// - 完成时 notify->done(状态码, 结果 JSON) 恰好一次
    /// - 受理成功返回 0 并写出 request_id (可用于 cancel_action); 未受理返回负错误码
    int32_t(PLUGINXX_CALL* request_action)(
        const PluginxxHost*               host,
        const PluginxxStringView*         action,
        const PluginxxStringView*         args_json,
        uint32_t                          timeout_ms,
        const PluginxxOperatorNotify*     notify,
        int64_t*                          out_request_id
    );

    /// 取消未完成动作请求
    void(PLUGINXX_CALL* cancel_action)(const PluginxxHost* host, int64_t request_id);
} MusicxxPluginHostIface;

/* ==================== 接口表: musicxx.ui (声明式 UI 扩展) ==================== */

#define MUSICXX_PLUGIN_IFACE_UI         "musicxx.ui"
#define MUSICXX_PLUGIN_IFACE_UI_VERSION 1

/* UI 项类型 (官方; 插件注册的项必须取其中之一, 否则注册被拒绝) */
/// 功能主页入口项: data = {title, subtitle?, icon?, action?}
#define MUSICXX_PLUGIN_UI_TYPE_HOME_ENTRY      "musicxx.ui.home.entry"
/// 歌曲菜单项: data = {title, icon?, action?}
#define MUSICXX_PLUGIN_UI_TYPE_SONG_ACTION     "musicxx.ui.song.action"
/// 歌单菜单项: data = {title, icon?, action?}
#define MUSICXX_PLUGIN_UI_TYPE_PLAYLIST_ACTION "musicxx.ui.playlist.action"
/// 播放页背景样式 (渲染槽位 `player.background`), 由用户在设置里选择后生效:
/// data = {title, depict?, enabled?, shader:{bundle, vertex?, fragment?},
///         args:[{name, source?, convert?, value?, valueNight?}],
///         speed?, resolutionScale?, maxFps?, animate?, scrim?, foregroundStyle?}
/// 说明: 着色器参数只有 `args` 一种写法 (旧的 `colors` 字段已移除);
/// 不声明 `args` 时宿主默认给内置背景的 4 个绘制色 (`icon.themeMapping.0..3`)。
#define MUSICXX_PLUGIN_UI_TYPE_PLAYING_BACKGROUND "musicxx.ui.playing.background"

/// 说明: 框架**没有**插件设置页类型 —— 插件要渲染设置界面, 就把设置画在自己注册的
/// 页面里 (`ext://<插件id>/<视图id>`, 可从主页入口等入口打开), 框架不管理设置入口。

/* UI 项动作类型 (data.action; 与界面描述层的动作写法一致) */
/// 打开声明式插件页面: {"kind":"route","route":"ext://<插件id>/<视图id>"}
/// (允许指向任意插件: 页面由被跳转插件自己绘制)
#define MUSICXX_PLUGIN_UI_ACTION_ROUTE      "route"
/// 调用本插件自己的能力: {"kind":"dispatch","name":"<短名>","args":{...}}
/// (字符串短写 "动作名" 等价于这一种)
#define MUSICXX_PLUGIN_UI_ACTION_DISPATCH   "dispatch"
/// 调用宿主官方动作: {"kind":"command","name":"musicxx.<域>.<动作>","args":{...}}
#define MUSICXX_PLUGIN_UI_ACTION_COMMAND    "command"
/// 无动作 (纯展示项)
#define MUSICXX_PLUGIN_UI_ACTION_NONE       "none"

/// UI 项注册规格
///
/// `item_id` 可以是本插件的**短名**（宿主自动补 `plugin.<pluginId>.` 前缀），
/// 也可以是完整全名 `plugin.<pluginId>.<名>`（必须属于本插件命名空间，否则拒绝注册）。
/// `data_json` 是类型相关的声明式内容（JSON 对象，单条上限 64 KiB）。
typedef struct MusicxxPluginUIEntrySpec {
    int32_t  version;      ///< == 1
    uint32_t struct_size;  ///< == sizeof(MusicxxPluginUIEntrySpec)

    PluginxxStringView item_id;   ///< 短名 "card" 或全名 "plugin.ad_skipper.card"
    PluginxxStringView type;      ///< MUSICXX_PLUGIN_UI_TYPE_*
    PluginxxStringView data_json; ///< 声明式内容 (JSON 对象)

    int32_t order;  ///< 排序权重 (小者靠前; 同权重按注册顺序)
    int32_t flags;  ///< 预留 (必须为 0)
} MusicxxPluginUIEntrySpec;

typedef struct MusicxxPluginUIIface {
    int32_t  version;     ///< == MUSICXX_PLUGIN_IFACE_UI_VERSION
    uint32_t struct_size;

    /// 注册/覆盖一个 UI 项
    /// - 返回 0 成功; -1 参数非法 (含 data 结构不合法);
    ///   -2 状态错误 (未启动/已禁用); -4 未知类型; -6 命名空间不属于本插件; -7 项数超限
    int32_t(PLUGINXX_CALL* register_entry)(
        const PluginxxHost*                host,
        const MusicxxPluginUIEntrySpec*    spec
    );

    /// 注销一个 UI 项 (item_id 必须是本插件的项; 不存在返回 -4)
    int32_t(PLUGINXX_CALL* unregister_entry)(
        const PluginxxHost*               host,
        const PluginxxStringView*         item_id
    );

    /// 更新一个 UI 项的声明式内容 (data 校验规则与注册一致)
    int32_t(PLUGINXX_CALL* update_entry)(
        const PluginxxHost*               host,
        const PluginxxStringView*         item_id,
        const PluginxxStringView*         data_json
    );

    /// 列出本插件已注册的 UI 项 (JSON 数组)
    int32_t(PLUGINXX_CALL* list_entries)(
        const PluginxxHost*               host,
        PluginxxString*                   out_json
    );

    /// 通知/提示 (等价于动作 `musicxx.ui.notify`; fire-and-forget, 不等待用户界面)
    int32_t(PLUGINXX_CALL* notify)(
        const PluginxxHost*               host,
        const PluginxxStringView*         message_json
    );
} MusicxxPluginUIIface;

/* ==================== 接口表: musicxx.vars (变量) ==================== */

/// 变量是第五条通道, 与既有四条分工不同:
/// - 钩子 = 拦截/改写流程; 状态镜像 = 应用推、插件同步只读; 事件 = 只通知、没有"当前值";
///   动作 = 插件向应用发命令;
/// - **变量 = 有属主、可读、可写、可订阅变动的小值** (配置/小状态)。
///
/// 三条读法 (按需要选一条, 不是三种都能替代):
/// - `get`  异步、权威: 宿主向属主取一次真实值, 结果回给调用方; 顺手刷新 `peek` 缓存;
/// - `peek` 同步、读缓存: 不保证最新 (返回里带 revision/ageMs/stale), 热路径/钩子处理器里用;
/// - 绑定变动: 订阅 (subscribe) 后由宿主推送 `musicxx.var.changed` 事件。
///
/// 命名空间:
/// - 官方 `musicxx.<域>.<名>` —— 由**应用**声明并提供, 插件只能读/写已声明的;
/// - 插件 `plugin.<插件id>.<分组>...` —— 分组可以多级 (`plugin.hello.tip.start`),
///   注册时可以写短名 (`tip.start`), 宿主自动补 `plugin.<本插件id>.` 前缀。
///
/// 值: 一律 JSON (bool/number/string/array/object), 单个值上限 64 KiB, 超限拒绝写入。
/// 权限: 与钩子/动作一样只做声明与展示, 不做运行时校验 (见清单 permissions)。
#define MUSICXX_PLUGIN_IFACE_VARS         "musicxx.vars"
#define MUSICXX_PLUGIN_IFACE_VARS_VERSION 1

/* 能力位 (声明里三者可只实现一部分; 能力位不含某项时对应操作返回 -9) */
#define MUSICXX_PLUGIN_VAR_CAP_GET    0x1 ///< 可读
#define MUSICXX_PLUGIN_VAR_CAP_SET    0x2 ///< 可写
#define MUSICXX_PLUGIN_VAR_CAP_NOTIFY 0x4 ///< 值变化可订阅 (watch 也要求这一位)

/* 写权限 (只对插件注册的变量有意义) */
#define MUSICXX_PLUGIN_VAR_WRITE_ANY   0 ///< 任何插件与应用都可写 (默认)
#define MUSICXX_PLUGIN_VAR_WRITE_OWNER 1 ///< 只有属主自己可写

/* 变量模式 */
/// 值由宿主代存 (默认): 插件 `set` 自己的变量即提交, 不需要维护副本
#define MUSICXX_PLUGIN_VAR_MODE_DECLARED 0
/// 读写都转给属主 (宿主只留同步读缓存): 属主处理 `musicxx.var.read` /
/// `musicxx.var.write` 两个事件, 用 `set` 自己完成回答/提交
#define MUSICXX_PLUGIN_VAR_MODE_HANDLER  1

/* 本接口表返回码 (含义与 C ABI 的错误码一致) */
#define MUSICXX_PLUGIN_VAR_ERR_ARG         -1 ///< 参数非法 (键名/值超限/caps 为空等)
#define MUSICXX_PLUGIN_VAR_ERR_STATE       -2 ///< 状态错误 (宿主未启动/未回执请求过多)
#define MUSICXX_PLUGIN_VAR_ERR_JSON        -3 ///< 值不是合法 JSON
#define MUSICXX_PLUGIN_VAR_ERR_NOT_FOUND   -4 ///< 键不存在 (含官方键还没被应用声明)
#define MUSICXX_PLUGIN_VAR_ERR_TIMEOUT     -5 ///< 属主落地/回答超时
#define MUSICXX_PLUGIN_VAR_ERR_PERMISSION  -6 ///< 命名空间不属于本插件 / 写权限不足
/// 超限 (变量数/订阅数/变量总数等容量类限制)
#define MUSICXX_PLUGIN_VAR_ERR_LIMIT       -7
/// 该变量没有声明对应能力 (读只写变量、写只读变量)
/// 说明: C ABI 的 -8 已用于"内存不足", 因此这里取 -9 避免同一数值两种含义
#define MUSICXX_PLUGIN_VAR_ERR_UNSUPPORTED -9

/* 变量变更事件 (宿主 → 插件; 订阅后经事件总线送达) */
/// 载荷: {key, value, prev, by, revision, ts, removed?, coalesced?}
/// - `by` = 写入方插件 id (空串 = 应用); 变量随插件停用/卸载消失时带 `removed:true`;
/// - 被宿主按 `notifyThrottleMs` 合并过的通知带 `coalesced` (合并掉几次)
#define MUSICXX_PLUGIN_EVENT_VAR_CHANGED "musicxx.var.changed"
/// 写请求的最终结果 (发给发起 `set` 的插件)
/// 载荷: {requestId, key, accepted, value?, changed?, error?}
#define MUSICXX_PLUGIN_EVENT_VAR_WRITE_RESULT "musicxx.var.writeResult"
/// 属主被取真实值 (handler 模式专用; 属主收到后用 set 自己回答)
/// 载荷: {requestId, key}
#define MUSICXX_PLUGIN_EVENT_VAR_READ "musicxx.var.read"
/// 属主收到写请求 (handler 模式专用; 属主落地后用 set 自己提交最终值)
/// 载荷: {requestId, key, value, by}
#define MUSICXX_PLUGIN_EVENT_VAR_WRITE "musicxx.var.write"

/// 变量注册规格
typedef struct MusicxxPluginVarSpec {
    int32_t  version;      ///< == 1
    uint32_t struct_size;  ///< == sizeof(MusicxxPluginVarSpec)

    PluginxxStringView key; ///< 短名 "tip.start" 或本插件全名 "plugin.<id>.tip.start"
    int32_t caps;           ///< MUSICXX_PLUGIN_VAR_CAP_* (非 0)
    int32_t write_scope;    ///< MUSICXX_PLUGIN_VAR_WRITE_*
    int32_t mode;           ///< MUSICXX_PLUGIN_VAR_MODE_*

    /// 属主侧的推送节流口径 (毫秒; 0 = 不节流)
    ///
    /// 只是声明 (宿主不强制度量): 属主在推送前按同一窗口合并变化。
    /// 高频值 (超过约 5 Hz) 必须声明, 否则宿主会记一条警告日志。
    int32_t throttle_ms;
    /// 宿主侧通知合并窗口 (毫秒; 0 = 不合并; 上限 5000)
    int32_t notify_throttle_ms;
    /// handler 模式: 有人 watch 之后, 宿主每隔多久来取一次真实值
    /// (0 = 只在 watch 时取一次; 非 0 时下限 1000)
    int32_t refresh_after_ms;

    PluginxxStringView type;       ///< 可空: bool | number | string | json (只做展示与粗校验)
    PluginxxStringView value_json; ///< 可空: 缓存初值 (不是真值来源; handler 模式可留空)
    /// 可空: {"title":"..","depict":"..","options":[".."],"risk":"low"}
    PluginxxStringView meta_json;
} MusicxxPluginVarSpec;

typedef struct MusicxxPluginVarsIface {
    int32_t  version;     ///< == MUSICXX_PLUGIN_IFACE_VARS_VERSION
    uint32_t struct_size;

    /// 注册/覆盖一个变量 (同一个 key 覆盖式注册)
    /// - 返回 0 成功; -1 参数非法 (键/值超限、caps 为空、窗口超限);
    ///   -2 状态错误; -3 meta_json 不是对象; -4 未知模式;
    ///   -6 命名空间不属于本插件; -7 变量数超限
    int32_t(PLUGINXX_CALL* register_var)(
        const PluginxxHost*         host,
        const MusicxxPluginVarSpec* spec
    );

    /// 注销一个变量 (只能注销自己的; 不存在返回 -4)
    int32_t(PLUGINXX_CALL* unregister_var)(
        const PluginxxHost*       host,
        const PluginxxStringView* key
    );

    /// 读 (异步、权威): 宿主向属主取一次真实值, 完成后调用 notify->done 恰好一次
    /// - 结果同时写入 `peek` 缓存 (read-through);
    /// - 同键在途读请求由宿主合并 (等待者不限);
    /// - 返回 0 并写出 request_id; -4 键不存在; -9 无 get 能力; -2 未回执读请求过多
    int32_t(PLUGINXX_CALL* get)(
        const PluginxxHost*           host,
        const PluginxxStringView*     key,
        const PluginxxOperatorNotify* notify,
        int64_t*                      out_request_id
    );

    /// 同步读缓存 (不保证最新)
    /// - 返回 0 并写出 {"value":..,"revision":..,"ageMs":..,"stale":..};
    /// - 没有缓存过 (没人推、也没人读过) 返回 -4; 无 get 能力返回 -9
    int32_t(PLUGINXX_CALL* peek)(
        const PluginxxHost*       host,
        const PluginxxStringView* key,
        PluginxxString*           out_json
    );

    /// 保活缓存 (等价于"订阅但不回调"; 需要 notify 能力位)
    /// - keys_json 是字符串数组; out_count 输出实际生效的条数
    int32_t(PLUGINXX_CALL* watch)(
        const PluginxxHost*       host,
        const PluginxxStringView* keys_json,
        int32_t*                  out_count
    );
    int32_t(PLUGINXX_CALL* unwatch)(
        const PluginxxHost*       host,
        const PluginxxStringView* keys_json,
        int32_t*                  out_count
    );

    /// 写
    /// - 写自己的变量 (或 declared 模式的变量) = 提交, 立即结算:
    ///   out_json = {"key":..,"accepted":true,"value":..,"changed":..,"revision":..};
    /// - 转给属主落地时 out_json = {"pending":true,"requestId":N}, 最终结果经事件
    ///   `musicxx.var.writeResult` 送达;
    /// - 返回 0 (含被拒绝的情况, 原因在 out_json 的 error 字段);
    ///   -4 键不存在; -9 无 set 能力; -6 写权限不足; -1 参数非法;
    ///   -2 未回执写请求过多
    int32_t(PLUGINXX_CALL* set)(
        const PluginxxHost*       host,
        const PluginxxStringView* key,
        const PluginxxStringView* value_json,
        PluginxxString*           out_json
    );

    /// 列出变量 (prefix 可空; 值字段是缓存值)
    int32_t(PLUGINXX_CALL* list)(
        const PluginxxHost*       host,
        const PluginxxStringView* prefix,
        PluginxxString*           out_json
    );

    /// 订阅/退订变更通知 (keys_json = 字符串数组; 订阅还不存在的键是允许的)
    /// - 订阅后请自行订阅事件主题 `musicxx.var.changed` (见下面的常量), 变更经它送达;
    /// - out_count 输出实际生效条数 (超出容量上限的部分被忽略)
    int32_t(PLUGINXX_CALL* subscribe)(
        const PluginxxHost*       host,
        const PluginxxStringView* keys_json,
        int32_t*                  out_count
    );
    int32_t(PLUGINXX_CALL* unsubscribe)(
        const PluginxxHost*       host,
        const PluginxxStringView* keys_json,
        int32_t*                  out_count
    );

    /// 单个变量的声明信息 + 缓存状态
    /// (key/caps/mode/write/owner/type/options/title/depict/value/revision/ageMs/
    ///  stale/subscribers)
    int32_t(PLUGINXX_CALL* info)(
        const PluginxxHost*       host,
        const PluginxxStringView* key,
        PluginxxString*           out_json
    );
} MusicxxPluginVarsIface;

/* ==================== 预留接口表 (v1 只冻结 IID 与版本; 表体后续实现) ==================== */

#define MUSICXX_PLUGIN_IFACE_PLAYER  "musicxx.player"
#define MUSICXX_PLUGIN_IFACE_LIBRARY "musicxx.library"
#define MUSICXX_PLUGIN_IFACE_LYRICS  "musicxx.lyrics"
#define MUSICXX_PLUGIN_IFACE_STORAGE "musicxx.storage"
#define MUSICXX_PLUGIN_IFACE_NET     "musicxx.net"
#define MUSICXX_PLUGIN_IFACE_STATS   "musicxx.stats"
#define MUSICXX_PLUGIN_IFACE_AGENT   "musicxx.agent" ///< 预留: 实现 AI 相关能力时使用

/* ==================== 状态镜像键 (官方) ==================== */

#define MUSICXX_STATE_APP      "musicxx.state.app"
#define MUSICXX_STATE_PLAYER   "musicxx.state.player"
#define MUSICXX_STATE_SONG     "musicxx.state.song"
#define MUSICXX_STATE_PLAYLIST "musicxx.state.playlist"
#define MUSICXX_STATE_LYRIC    "musicxx.state.lyric"
#define MUSICXX_STATE_LIBRARY  "musicxx.state.library"
#define MUSICXX_STATE_ENV      "musicxx.state.env"

/// 渲染槽位的运行状态 (谁在画 / 是否可见 / 尺寸 / 昼夜)
/// 不随启动全量推送: 生效、可见性、昼夜或尺寸变化时按需推送
#define MUSICXX_STATE_RENDER_SLOTS "musicxx.state.renderSlots"

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* MUSICXX_PLUGIN_API_H */
