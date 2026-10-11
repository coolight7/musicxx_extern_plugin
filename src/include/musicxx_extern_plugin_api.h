/// musicxx 外部插件框架 C ABI (Dart ⇄ 原生, v1)
///
/// 定位: 本头是 **Dart 侧唯一入口**(ffigen 由此生成绑定), 也是插件框架原生库的导出约定。
/// 跨边界约定:
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
/// 线程: 除特别注明外任意线程可调用; 标注 `[IO]` 的内部投递到框架线程并等待
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

/* ==================== 插件框架配置标志 ==================== */

#define MUSICXX_EXTERN_PLUGIN_FLAG_SAFE_MODE            0x0001 ///< 不加载任何外部插件
#define MUSICXX_EXTERN_PLUGIN_FLAG_NO_NATIVE            0x0002 ///< 跳过原生(DSO)插件
#define MUSICXX_EXTERN_PLUGIN_FLAG_NO_JS                0x0004 ///< 跳过 JS 插件
#define MUSICXX_EXTERN_PLUGIN_FLAG_DEBUG_OBSERVE_EVENTS 0x0008 ///< 回传 observe/console 观测事件

/* ==================== 钩子派发标志 ==================== */

#define MUSICXX_EXTERN_PLUGIN_HOOK_SYNC  1 ///< 等待处理器链结果 (等待期间不设超时)
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

/// 插件框架堆分配字符串 (显式所有权: 由插件框架分配, 调用方用 string_free 释放)
typedef struct MusicxxExternPluginString {
    char*    data; ///< 以 \0 结尾; 空串或未设置时为 NULL
    uint64_t size; ///< 字节数 (不含结尾 \0)
} MusicxxExternPluginString;

/// 插件框架运行时句柄 (不透明; 进程内单例)
typedef struct MusicxxExternPluginHost MusicxxExternPluginHost;

/// 事件唤醒回调 (原生事件入队时调用; 必须快速返回)
typedef int32_t(MUSICXX_EXTERN_PLUGIN_CALL* MusicxxExternPluginWakeFn)(void* user_data);

/* ==================== 插件框架配置 ==================== */

typedef struct MusicxxExternPluginHostConfig {
    uint32_t struct_size; ///< == sizeof(MusicxxExternPluginHostConfig)

    MusicxxExternPluginStringView app_version;       ///< 应用版本 (如 "0.87.0")
    MusicxxExternPluginStringView platform;          ///< 平台标识 (windows/linux/macos/android/ios/ohos)
    MusicxxExternPluginStringView language;          ///< 语言代码 (如 "zh-cn")
    MusicxxExternPluginStringView user_plugin_dir;   ///< 用户插件目录 (绝对路径; 可为空)
    MusicxxExternPluginStringView builtin_plugin_dir;///< 随包插件目录 (绝对路径; 可为空)
    MusicxxExternPluginStringView data_dir;          ///< 插件私有数据根目录 (绝对路径; 可为空)
    MusicxxExternPluginStringView log_dir;           ///< 日志目录 (绝对路径; 可为空)

    /// 客户端界面能力段 (JSON 文本; 可为空)
    ///
    /// 内容由界面描述层库生成 (支持的组件 / 控件 / 图标 / 上限等), 插件框架只做转发:
    /// `host.info().ui` 与 JS 侧 `musicxx.host.info().ui` 返回同一份,
    /// 插件据此决定该给什么界面内容 (取页面时也会带一份, 见各客户端的取数参数)。
    MusicxxExternPluginStringView ui_capabilities;

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

/// 释放框架堆字符串 (幂等; NULL 安全)
MUSICXX_EXTERN_PLUGIN_EXPORT void MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_string_free(MusicxxExternPluginString* s);

/// C ABI 版本 (Dart 侧校验: 必须 == MUSICXX_EXTERN_PLUGIN_API_VERSION)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_api_version(void);

/// 插件框架库版本字符串 (形如 "1.0.0"); 写入 out (框架堆, 需 string_free)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_version(MusicxxExternPluginString* out);

/* ==================== 插件框架生命周期 ==================== */

/// 创建插件框架实例 (纯构造, 不起线程/不加载插件); 失败返回 NULL 并写 log
///
/// **进程单例**: 同一进程只允许存在一个插件框架实例 (它持有框架线程 / 插件实例 / 事件队列)。
/// 已存在时返回 NULL 并写明确原因 —— 其它 isolate / 线程请复用已有句柄调用管理类 API。
/// `host_destroy` 之后可以再次创建 (init → dispose → init 是正常路径)。
MUSICXX_EXTERN_PLUGIN_EXPORT MusicxxExternPluginHost* MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_host_create(
        const MusicxxExternPluginHostConfig* cfg,
        MusicxxExternPluginString*           log
    );

/// 启动插件框架: 起框架线程/工作线程池、注册内置提供者、按配置加载插件
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

/// 销毁插件框架实例 (未停止会自动停止; NULL 安全)
MUSICXX_EXTERN_PLUGIN_EXPORT void MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_host_destroy(MusicxxExternPluginHost* h);

/// 热更新语言 (插件可读; 插件侧 `pluginxx.config` 表 get_language)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_set_language(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* lang,
        MusicxxExternPluginString*           log
    );

/// 热更新插件框架配置 JSON (插件目录/开关/统计采集等)
/// - 支持字段: pluginDirs[], enableNative, enableJs, safeMode, observeEvents,
///   statsEnabled, statsPeriodMs
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

/// 插件框架 → 插件事件总线发布 (topic 必须 musicxx.* 或 plugin.<id>.*)
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

/// 检查一个插件目录 (**只读**, 不加载、不改动任何状态) → JSON 对象
///
/// 用途: 主程序「从压缩包安装」的预检 —— 把解包出来的临时目录交给插件框架判定
/// (清单可解析 / 当前系统与架构该用哪个分支 / 库文件或脚本在不在), 装之前就有结论。
/// 判定与扫描、加载**完全同源** (同一份 `inspectPluginDir`), 不会出现
/// "预检通过、加载却用另一个分支"的分歧。
///
/// - `dir`: 插件目录 (绝对路径; 压缩包解包后的临时目录同样可以);
/// - `os` / `arch`: 要判定的目标环境 (规范化名字, 别名都认; 传空 = 用插件框架自己的);
/// - 出参 JSON: `{valid, error, path, dirName, id, kind, version, description, author,
///   homepage, entry, apiVersion, depends, optionalDepends, permissions, platforms,
///   arch, scripts?, targetsDir?, target?, targetEntry?, targets[]?, supported, reason}`
///   (动态库插件才有 target/targetEntry/targetsDir/targets; JS 插件才有 scripts);
/// - 返回 OK 表示"判定完成", 目录不存在/清单非法时 `valid=false` + `error`
///   (同样返回 OK —— 调用方看 `valid`);
/// - 清单 `api_version` 低于 `MUSICXX_PLUGINXX_MIN_API_VERSION` 时 `supported=false`
///   (+ reason); 声明更高版本不算不支持;
/// - 不看运行期开关 (JS 运行时是否可用、安全模式、禁用动态库), 那些由 `plugin_scan` 叠加。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_plugin_inspect(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* dir,
        const MusicxxExternPluginStringView* os,
        const MusicxxExternPluginStringView* arch,
        MusicxxExternPluginString*           out_json,
        MusicxxExternPluginString*           log
    );

/// 当前已加载插件状态快照 (JSON 数组)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_plugin_list(
        MusicxxExternPluginHost*    h,
        MusicxxExternPluginString* out_json,
        MusicxxExternPluginString* log
    );

/// 异步加载 (结果经 `musicxx.plugin.loaded` / `musicxx.plugin.error` 事件回报)
/// - `id_or_path` 可为插件 id 或插件目录/库文件路径
/// - `options_json`: {"args":{...},"enabled":true,"tempDir":"...","kind":"native|js|builtin"}
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL musicxx_extern_plugin_plugin_load(
    MusicxxExternPluginHost*              h,
    const MusicxxExternPluginStringView* id_or_path,
    const MusicxxExternPluginStringView* options_json,
    MusicxxExternPluginString*           log
);

/// 同步加载 (仅管理页/测试的隔离 isolate 使用; 带超时)
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

/// 更新插件参数 (下次 `reload` 生效, 由 Dart 决定何时重载)
/// - `reload` 会把当前参数带过去, 因此"设置参数 → 重载"不会丢参数
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

/// 调用插件能力 (Dart → 插件; **异步, 不占调用线程**)
///
/// - 只受理不等待结果: 成功返回 `MUSICXX_EXTERN_PLUGIN_OK` 并写出 `out_request_id`;
///   结果经事件 `musicxx.plugin.call.result` 回给 Dart
///   (载荷 `{requestId, id, method, ok, result|error, ms}`);
/// - 受理失败 (插件没加载 / 没声明该能力 / 归属不对 / 插件框架没在跑) 直接返回负错误码,
///   **不推结果事件**;
/// - 语义与旧同步版一致: 能力名补 `plugin.<id>.` 前缀、归属校验、同样的错误码;
/// - **没有 timeout 参数**: 超时是调用方的事 (它自己不等了, 结果丢弃);
/// - **受理成功 ⇒ 一定有终局**: 正常结果、执行失败、插件卸载 (`plugin_unloaded`)、
///   框架停机 (`host_stopped`) 四条路都会推一条结果事件, 调用方不会一直悬挂;
/// - **不提供同步版**: 旧符号 `musicxx_extern_plugin_plugin_call` 已删除。
///   换符号名是为了让"插件框架库太旧"能被探测到 —— 同名改签名时混用新旧库会用新参数
///   去打旧函数 (内存越界), 换名 + 探测式查找能把这件事变成一条可读错误。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_plugin_call_async(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* id,
        const MusicxxExternPluginStringView* method,
        const MusicxxExternPluginStringView* args_json,
        int64_t*                             out_request_id,
        MusicxxExternPluginString*           log
    );

/* ==================== 钩子 ==================== */

/// 派发钩子
/// - `flags` = MUSICXX_EXTERN_PLUGIN_HOOK_SYNC 时等待处理器链结束 (等待期间不设超时) 并输出合并结果;
///   MUSICXX_EXTERN_PLUGIN_HOOK_ASYNC 时入队即返回
/// - 裁决型钩子在 ASYNC 模式下为"异步裁决": 立即返回 `{"handled":true,"async":true,"callId":N}`,
///   处理器链跑完后由事件 `musicxx.hook.decision.result` 回传结果 (payload 含同一个 callId,
///   以及 result/called/handlers 字段); 观察型钩子的 ASYNC 派发不回报结果
/// - out_json 形如 {"handled":true,"result":{...},"called":1,"handlers":2}
/// - `timeout_ms` 保留在签名里 (约定固定) 但不再使用: 插件框架不设等待超时, 处理器链慢就是
///   调用线程一起等
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

/// 该钩子当前的处理器列表 (自省用; 按**调用顺序**给出: priority 升序, 同优先级按注册顺序)
///
/// out_json 形如:
/// {"hook":"musicxx.media.spectrum.extract","count":2,"handlers":[
///   {"plugin":"example_js","instance":"js:example_js","kind":"js","priority":0,"ownerTag":""},
///   {"plugin":"example_native","instance":"example_native","kind":"native","priority":10,"ownerTag":""}]}
///
/// 只给出 plugin / instance / kind / priority / ownerTag: 不暴露处理器地址、载荷与内部状态。
/// `instance` 与内核里的实例名一致 (JS 是 `js:<id>`); 钩子 id 不认识时返回 ERR_NOT_FOUND。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_hook_handlers(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* hook_id,
        MusicxxExternPluginString*           out_json,
        MusicxxExternPluginString*           log
    );

/// 钩子耗时/失败统计 (调试页; 只观测不限制)
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

/// 推送单个状态键 (值必须为合法 JSON; 大小不限制)
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

/* ==================== 变量表 (Dart ⇄ 原生; 第五条通道) ==================== */

/// 变量 = 有属主、可读、可写、可订阅变动的小值 (配置/小状态)。
///
/// 与状态镜像的区别: 状态镜像只能应用推、插件同步只读; 变量可以双向读写, 且能订阅
/// "值变了"。**插件框架里没有真值**: 真值只在属主那里 (主程序 Store/配置, 或插件自己的
/// 状态), 插件框架只保留一份服务 `peek` 同步读的缓存。
///
/// 命名空间: 官方 `musicxx.<域>.<名>` (由应用声明), 插件 `plugin.<插件id>.<分组>...`。
///
/// Dart 侧要处理两个事件 (插件发起的读写):
/// - `musicxx.var.read`  {requestId, key}         → 用 var_read_result 回答
/// - `musicxx.var.write` {requestId, key, value, by} → 用 var_write_result 回执

/// 插件框架能力位 (Dart 侧据此判断加载到的插件框架库是不是这次构建的产物)
///
/// 缺位时**不要**继续调用对应的一批 API: 老库上没有这些符号, 报错会是一句难懂的
/// `undefined symbol`。包层在 init 时读一次并给出"请重新构建插件框架库"的说明。
#define MUSICXX_EXTERN_PLUGIN_FEATURE_HOOKS  0x1
#define MUSICXX_EXTERN_PLUGIN_FEATURE_STATE  0x2
#define MUSICXX_EXTERN_PLUGIN_FEATURE_EVENTS 0x4
#define MUSICXX_EXTERN_PLUGIN_FEATURE_UI     0x8
#define MUSICXX_EXTERN_PLUGIN_FEATURE_VARS   0x10

/// 插件框架能力位图 (无需框架实例; 任意线程可调用)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_feature_bits(void);

/// 变量表 · 应用声明: items_json = [{"key":"musicxx.ui.animatedLevel",
///   "caps":["get","set","notify"],"type":"string","options":["a","b"],
///   "value":true,"title":"..","depict":"..","risk":"low",
///   "throttleMs":0,"notifyThrottleMs":0}, ...]
///
/// 逐项失败只写进 `log` (不影响其它项; 也不影响启动 —— 与"启动绝不失败"一致)。
/// 主程序需要在插件框架启动后、加载插件之前声明一次, 插件才能读到这些键。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_var_declare(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* items_json,
        MusicxxExternPluginString*           log
    );

/// 变量表 · 主程序推值 (官方键的主程序变动; 值没变不会通知)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_var_update(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* key,
        const MusicxxExternPluginStringView* value_json,
        MusicxxExternPluginString*           log
    );

/// 变量表 · 应用批量推值: [{"key":"...","value":...}, ...]
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_var_update_batch(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* items_json,
        MusicxxExternPluginString*           log
    );

/// 变量表 · 应用回执写请求 (**回执即写入**): accepted 且带 value 时写入值并按需广播
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_var_write_result(
        MusicxxExternPluginHost*              h,
        int64_t                               request_id,
        int32_t                               accepted,
        const MusicxxExternPluginStringView* value_json,
        const MusicxxExternPluginStringView* error,
        MusicxxExternPluginString*           log
    );

/// 变量表 · 主程序回答读取请求 (插件 `get` 官方键时插件框架转过来的那条)
/// - 一条读请求只回答一次; 不答则调用方按 `read_timeout` 结算
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_var_read_result(
        MusicxxExternPluginHost*              h,
        int64_t                               request_id,
        int32_t                               ok,
        const MusicxxExternPluginStringView* value_json,
        const MusicxxExternPluginStringView* error,
        MusicxxExternPluginString*           log
    );

/// 变量表 · 主程序读插件键 (有界等待插件框架与属主; 结果经 out_json 给出)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_var_get(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* key,
        MusicxxExternPluginString*           out_json,
        MusicxxExternPluginString*           log
    );

/// 变量表 · 应用写插件键 (declared 立即结算; handler 等属主, 有界等待)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_var_set(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* key,
        const MusicxxExternPluginStringView* value_json,
        MusicxxExternPluginString*           out_json,
        MusicxxExternPluginString*           log
    );

/// 变量表 · 应用列变量 (prefix 可空; 值字段是缓存值, 带 revision/ageMs/stale)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_var_list(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* prefix,
        MusicxxExternPluginString*           out_json,
        MusicxxExternPluginString*           log
    );

/// 变量表 · 应用订阅/退订插件键的变动
/// - keys_json = 字符串数组, 支持前缀写法 ("plugin." / "plugin.<id>.")
/// - 应用订阅了某个键, 该键变化时才会推 `musicxx.var.changed` 事件
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_var_subscribe(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* keys_json,
        MusicxxExternPluginString*           log
    );
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_var_unsubscribe(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* keys_json,
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

/// Dart → 原生日志 (统一写入磁盘/转发; level: 0 trace .. 4 error)
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
