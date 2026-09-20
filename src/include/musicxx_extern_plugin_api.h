/// musicxx 外部插件宿主 C ABI (Dart ⇄ 原生, v1)
///
/// 定位: 本头是 **Dart 侧唯一入口**(ffigen 由此生成绑定), 也是原生宿主的导出契约。
/// 规范详见 `resource/history/extern-plugin-impl/plan.md` §6:
/// - 对齐/类型: 8 字节对齐, 只用定长类型, 禁止裸 int/long/size_t;
/// - 调用约定: 全部导出函数与回调带 `MUSICXX_EXTERN_PLUGIN_CALL`;
/// - 结构体: 入参 `const Struct*`, 返回值一律经 `out` 参数 + `int32_t` 状态码;
/// - 字符串: 只读借用用 [MusicxxExternPluginStringView], 跨堆分配用
///   [MusicxxExternPluginString] (用后必须 musicxx_extern_plugin_string_free);
/// - 内存: 跨边界只用 musicxx_extern_plugin_malloc/free/strdup_n, 绝不跨 CRT free。
///
/// 错误码: 0 成功 / -1 参数非法 / -2 状态错误 / -3 JSON 非法 / -4 未找到 /
///         -5 超时 / -6 权限拒绝 / -7 队列满 / -8 内存不足 / -99 内部异常。
///
/// 线程: 除特别注明外任意线程可调用; 标注 `[IO]` 的内部投递到宿主线程并等待
/// (有等待上界); 唤醒回调必须快速返回。
#ifndef MUSICXX_EXTERN_PLUGIN_API_H
#define MUSICXX_EXTERN_PLUGIN_API_H

#include <stddef.h>
#include <stdint.h>

/* ==================== 导出符号与调用约定 ==================== */

#if defined(MUSICXX_EXTERN_PLUGIN_BUILTIN)
#define MUSICXX_EXTERN_PLUGIN_EXPORT
#elif defined(_WIN32)
#define MUSICXX_EXTERN_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define MUSICXX_EXTERN_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define MUSICXX_EXTERN_PLUGIN_EXPORT
#endif

#if defined(_WIN32)
#define MUSICXX_EXTERN_PLUGIN_CALL __stdcall
#elif defined(__GNUC__) || defined(__clang__)
#if defined(__i386__)
#define MUSICXX_EXTERN_PLUGIN_CALL __attribute__((stdcall))
#else
#define MUSICXX_EXTERN_PLUGIN_CALL
#endif
#else
#define MUSICXX_EXTERN_PLUGIN_CALL
#endif

#define MUSICXX_EXTERN_PLUGIN_API_VERSION 1

/* ==================== 错误码 ==================== */

#define MUSICXX_EXTERN_PLUGIN_OK                 0
#define MUSICXX_EXTERN_PLUGIN_ERR_ARG           -1
#define MUSICXX_EXTERN_PLUGIN_ERR_STATE         -2
#define MUSICXX_EXTERN_PLUGIN_ERR_JSON          -3
#define MUSICXX_EXTERN_PLUGIN_ERR_NOT_FOUND     -4
#define MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT       -5
#define MUSICXX_EXTERN_PLUGIN_ERR_PERMISSION    -6
#define MUSICXX_EXTERN_PLUGIN_ERR_QUEUE_FULL    -7
#define MUSICXX_EXTERN_PLUGIN_ERR_NO_MEMORY     -8
#define MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL      -99

/* ==================== 宿主配置标志 ==================== */

#define MUSICXX_EXTERN_PLUGIN_FLAG_SAFE_MODE            0x0001 ///< 不加载任何外部插件
#define MUSICXX_EXTERN_PLUGIN_FLAG_NO_NATIVE            0x0002 ///< 跳过原生(DSO)插件
#define MUSICXX_EXTERN_PLUGIN_FLAG_NO_JS                0x0004 ///< 跳过 JS 插件
#define MUSICXX_EXTERN_PLUGIN_FLAG_DEBUG_OBSERVE_EVENTS 0x0008 ///< 回传 observe/console 观测事件

/* ==================== 钩子派发标志 ==================== */

#define MUSICXX_EXTERN_PLUGIN_HOOK_SYNC  1 ///< 等待处理器链结果 (有等待预算)
#define MUSICXX_EXTERN_PLUGIN_HOOK_ASYNC 0 ///< 入队即返回

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 8)

/* ==================== 跨边界字符串 ==================== */

/// 只读借用字符串视图 (指向调用方内存, UTF-8, 不要求 NUL 结尾)
typedef struct MusicxxExternPluginStringView {
    const char* data;
    uint64_t    size;
} MusicxxExternPluginStringView;

/// 宿主堆分配字符串 (显式所有权: 由宿主分配, 调用方用 string_free 释放)
typedef struct MusicxxExternPluginString {
    char*    data; ///< 以 \0 结尾; 空串或未设置时为 NULL
    uint64_t size; ///< 字节数 (不含结尾 \0)
} MusicxxExternPluginString;

/// 宿主句柄 (不透明; 进程内单例)
typedef struct MusicxxExternPluginHost MusicxxExternPluginHost;

/// 事件唤醒回调 (原生事件入队时调用; 必须快速返回)
typedef int32_t(MUSICXX_EXTERN_PLUGIN_CALL* MusicxxExternPluginWakeFn)(void* user_data);

/* ==================== 宿主配置 ==================== */

typedef struct MusicxxExternPluginHostConfig {
    uint32_t struct_size; ///< == sizeof(MusicxxExternPluginHostConfig)

    MusicxxExternPluginStringView app_version;       ///< 应用版本 (如 "0.87.0")
    MusicxxExternPluginStringView platform;          ///< 平台标识 (windows/linux/macos/android/ios/ohos)
    MusicxxExternPluginStringView language;          ///< 语言代码 (如 "zh-cn")
    MusicxxExternPluginStringView user_plugin_dir;   ///< 用户插件目录 (绝对路径; 可为空)
    MusicxxExternPluginStringView builtin_plugin_dir;///< 随包插件目录 (绝对路径; 可为空)
    MusicxxExternPluginStringView data_dir;          ///< 插件私有数据根目录 (绝对路径; 可为空)
    MusicxxExternPluginStringView log_dir;           ///< 日志目录 (绝对路径; 可为空)

    int32_t  log_level;          ///< 0 trace .. 4 error
    int32_t  flags;              ///< MUSICXX_EXTERN_PLUGIN_FLAG_*
    uint32_t event_queue_capacity; ///< 0 = 默认 16384
} MusicxxExternPluginHostConfig;

/* ==================== 内存 / 版本 ==================== */

/// 跨边界堆分配 (与 string_free 同一分配器)
MUSICXX_EXTERN_PLUGIN_EXPORT void* MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_malloc(uint64_t size);

/// 释放 musicxx_extern_plugin_malloc 分配的指针 (NULL 安全)
MUSICXX_EXTERN_PLUGIN_EXPORT void MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_free(const void* ptr);

/// 复制为跨边界堆字符串 (size 为字节数, 不要求 NUL 结尾)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL musicxx_extern_plugin_strdup_n(
    const MusicxxExternPluginStringView* src,
    MusicxxExternPluginString*           out
);

/// 释放宿主堆字符串 (幂等; NULL 安全)
MUSICXX_EXTERN_PLUGIN_EXPORT void MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_string_free(MusicxxExternPluginString* s);

/// C ABI 版本 (Dart 侧校验: 必须 == MUSICXX_EXTERN_PLUGIN_API_VERSION)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_api_version(void);

/// 宿主库版本字符串 (形如 "1.0.0"); 写入 out (宿主堆, 需 string_free)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_version(MusicxxExternPluginString* out);

/* ==================== 宿主生命周期 ==================== */

/// 创建宿主 (纯构造, 不起线程/不加载插件); 失败返回 NULL 并写 log
///
/// **进程单例**: 同一进程只允许存在一个宿主 (它持有宿主线程 / 插件实例 / 事件队列)。
/// 已存在时返回 NULL 并写明确原因 —— 其它 isolate / 线程请复用已有句柄调用管理类 API。
/// `host_destroy` 之后可以再次创建 (init → dispose → init 是正常路径)。
MUSICXX_EXTERN_PLUGIN_EXPORT MusicxxExternPluginHost* MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_host_create(
        const MusicxxExternPluginHostConfig* cfg,
        MusicxxExternPluginString*           log
    );

/// 启动宿主: 起宿主线程/工作线程池、注册内置提供者、按配置装载插件
/// - 就绪后入队 `musicxx.host.ready` 事件
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL musicxx_extern_plugin_host_start(
    MusicxxExternPluginHost*    h,
    MusicxxExternPluginString* log
);

/// 同步停止 (幂等); 超时返回 MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL musicxx_extern_plugin_host_stop(
    MusicxxExternPluginHost*    h,
    uint32_t                    timeout_ms,
    MusicxxExternPluginString* log
);

/// 销毁宿主 (未停止会自动停止; NULL 安全)
MUSICXX_EXTERN_PLUGIN_EXPORT void MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_host_destroy(MusicxxExternPluginHost* h);

/// 热更新语言 (插件可读; 插件侧 `pluginxx.config` 表 get_language)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_set_language(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* lang,
        MusicxxExternPluginString*           log
    );

/// 热更新宿主配置 JSON (插件目录/开关/超时阈值/统计采集等)
/// - 支持字段: pluginDirs[], enableNative, enableJs, safeMode, hookBudgetMs,
///   hookHardBudgetMs, observeEvents, statsEnabled, statsPeriodMs
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_set_config(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* json,
        MusicxxExternPluginString*           log
    );

/* ==================== 事件 (原生 → Dart) ==================== */

/// 注册唤醒回调 (原生事件入队时调用; 未注册时 Dart 侧退回定时轮询)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_set_wake_callback(
        MusicxxExternPluginHost*        h,
        MusicxxExternPluginWakeFn       fn,
        void*                           user_data
    );

/// 批量取事件 (JSON 数组字符串); 无事件返回 MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_poll_events(
        MusicxxExternPluginHost*    h,
        int32_t                     max_count,
        MusicxxExternPluginString* out_json,
        MusicxxExternPluginString* log
    );

/// 宿主 → 插件事件总线发布 (topic 必须 musicxx.* 或 plugin.<id>.*)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_event_publish(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* topic,
        const MusicxxExternPluginStringView* payload_json,
        MusicxxExternPluginString*           log
    );

/* ==================== 插件管理 ==================== */

/// 扫描插件目录 (用户目录 + 随包目录) + 内置清单 → JSON 数组
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_plugin_scan(
        MusicxxExternPluginHost*    h,
        MusicxxExternPluginString* out_json,
        MusicxxExternPluginString* log
    );

/// 当前已加载插件状态快照 (JSON 数组)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_plugin_list(
        MusicxxExternPluginHost*    h,
        MusicxxExternPluginString* out_json,
        MusicxxExternPluginString* log
    );

/// 异步装载 (结果经 `musicxx.plugin.loaded` / `musicxx.plugin.error` 事件回报)
/// - `id_or_path` 可为插件 id 或插件目录/库文件路径
/// - `options_json`: {"args":{...},"enabled":true,"tempDir":"...","kind":"native|js|builtin"}
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL musicxx_extern_plugin_plugin_load(
    MusicxxExternPluginHost*              h,
    const MusicxxExternPluginStringView* id_or_path,
    const MusicxxExternPluginStringView* options_json,
    MusicxxExternPluginString*           log
);

/// 同步装载 (仅管理页/测试的隔离 isolate 使用; 带超时)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_plugin_load_sync(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* id_or_path,
        const MusicxxExternPluginStringView* options_json,
        uint32_t                              timeout_ms,
        MusicxxExternPluginString*           log
    );

/// 启用 / 禁用 / 卸载 / 重载 (异步, 事件回报)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL musicxx_extern_plugin_plugin_enable(
    MusicxxExternPluginHost*              h,
    const MusicxxExternPluginStringView* id,
    MusicxxExternPluginString*           log
);
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL musicxx_extern_plugin_plugin_disable(
    MusicxxExternPluginHost*              h,
    const MusicxxExternPluginStringView* id,
    MusicxxExternPluginString*           log
);
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL musicxx_extern_plugin_plugin_unload(
    MusicxxExternPluginHost*              h,
    const MusicxxExternPluginStringView* id,
    MusicxxExternPluginString*           log
);
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL musicxx_extern_plugin_plugin_reload(
    MusicxxExternPluginHost*              h,
    const MusicxxExternPluginStringView* id,
    MusicxxExternPluginString*           log
);

/// 更新插件参数 (需 reload 生效, 由 Dart 决定何时重载)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_plugin_set_args(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* id,
        const MusicxxExternPluginStringView* args_json,
        MusicxxExternPluginString*           log
    );

/// 插件配置路径 (供管理页/插件读取); 未加载的插件按目录推导
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_plugin_get_config_path(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* id,
        MusicxxExternPluginString*           out,
        MusicxxExternPluginString*           log
    );

/// 调用插件能力 (Dart → 插件; 同步带超时)
/// - native/JS 插件统一用 `plugin.<pluginId>.<能力名>`
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL musicxx_extern_plugin_plugin_call(
    MusicxxExternPluginHost*              h,
    const MusicxxExternPluginStringView* id,
    const MusicxxExternPluginStringView* method,
    const MusicxxExternPluginStringView* args_json,
    uint32_t                              timeout_ms,
    MusicxxExternPluginString*           out_json,
    MusicxxExternPluginString*           log
);

/* ==================== 钩子 ==================== */

/// 派发钩子
/// - `flags` = MUSICXX_EXTERN_PLUGIN_HOOK_SYNC 时等待处理器链 (预算内) 并输出合并结果;
///   MUSICXX_EXTERN_PLUGIN_HOOK_ASYNC 时入队即返回
/// - 裁决型钩子在 ASYNC 模式下为"异步裁决": 立即返回 `{"handled":true,"async":true,"callId":N}`,
///   处理器链跑完后由事件 `musicxx.hook.decision.result` 回传结果 (payload 含同一个 callId,
///   以及 result/timedOut/called/handlers 字段); 观察型钩子的 ASYNC 派发不回报结果
/// - out_json 形如 {"handled":true,"result":{...},"timedOut":false,"handlers":2}
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_hook_emit(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* hook_id,
        const MusicxxExternPluginStringView* input_json,
        int32_t                               flags,
        uint32_t                              timeout_ms,
        MusicxxExternPluginString*           out_json,
        MusicxxExternPluginString*           log
    );

/// 该钩子当前的原生/JS 处理器数量 (Dart 快速路径判定; 事件增量更新)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_hook_count(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* hook_id,
        int32_t*                              out_count,
        MusicxxExternPluginString*           log
    );

/// 钩子耗时/超时/失败统计 (调试页)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_hook_stats(
        MusicxxExternPluginHost*    h,
        MusicxxExternPluginString* out_json,
        MusicxxExternPluginString* log
    );

/* ==================== 动作请求 (插件 → Dart) ==================== */

/// 回复插件发起的动作请求 (status 见错误码; result_json 可为空)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_action_respond(
        MusicxxExternPluginHost*              h,
        int64_t                               request_id,
        int32_t                               status,
        const MusicxxExternPluginStringView* result_json,
        MusicxxExternPluginString*           log
    );

/* ==================== 状态镜像 (Dart → 原生) ==================== */

/// 推送单个状态键 (值必须为合法 JSON; 单键上限 64 KiB, 超出截断并告警)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_state_update(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* key,
        const MusicxxExternPluginStringView* value_json,
        MusicxxExternPluginString*           log
    );

/// 批量推送状态键: [{"key":"...","value":{...}}, ...]
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_state_update_batch(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* items_json,
        MusicxxExternPluginString*           log
    );

/* ==================== UI / 日志 / 调试 / 统计 ==================== */

/// 插件贡献的 UI 项快照 (JSON 数组; 变更另有 musicxx.ui.changed 事件)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_ui_snapshot(
        MusicxxExternPluginHost*    h,
        MusicxxExternPluginString* out_json,
        MusicxxExternPluginString* log
    );

/// Dart → 原生日志 (统一落盘/转发; level: 0 trace .. 4 error)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL musicxx_extern_plugin_log(
    MusicxxExternPluginHost*              h,
    int32_t                               level,
    const MusicxxExternPluginStringView* message,
    MusicxxExternPluginString*           log
);

/// 调试信息 (版本/依赖/平台/插件统计; 支持页复制)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_debug_info(
        MusicxxExternPluginHost*    h,
        MusicxxExternPluginString* out_json,
        MusicxxExternPluginString* log
    );

/// 资源与耗时统计快照 (scope_json = NULL 取全部)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL musicxx_extern_plugin_stats(
    MusicxxExternPluginHost*              h,
    const MusicxxExternPluginStringView* scope_json,
    MusicxxExternPluginString*           out_json,
    MusicxxExternPluginString*           log
);

/// 统计采集配置 ({"enable":bool,"jsHeapSampleMs":int,"observeEvents":bool})
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_stats_config(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* cfg_json,
        MusicxxExternPluginString*           log
    );

#pragma pack(pop)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MUSICXX_EXTERN_PLUGIN_API_H */
