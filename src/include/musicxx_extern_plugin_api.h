/// musicxx 外部插件框架 C ABI (Dart ⇄ 原生, v7)
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
/// **一次业务操作只有一种格式: 提交 + 结果事件** (v7 起)
///
/// ```c
/// int32_t musicxx_extern_plugin_<操作>(h, /* 入参 */ ..., int64_t* out_request_id,
///                                      MusicxxExternPluginString* log);
/// ```
///
/// - 返回 `0` = **已受理**, `out_request_id` 有效, 之后**恰好一条**结果事件
///   `musicxx.request.result` 交回结果;
/// - 返回负数 = **未受理** (参数错 / 状态错 / 事件队列满), 不会推结果事件, 原因写在 `log`;
/// - 结果事件 `{"seq":..,"type":"musicxx.request.result","requestId":42,"op":"plugin.load",
///   "ok":true,"data":{...}}`, 失败时是 `"error":{"code":-4,"reason":"not_found",
///   "message":"..."}`;
/// - **没有结果的操作 (发布 / 订阅 / 日志 / 回复 / 关机) 同样回一条确认**, `data` 为
///   `null`: 要保证顺序 (先发布再读、先订阅再取值) 时直接 `await` 就行;
/// - 不需要结果的调用方可以不管它, 但**能等**是默认能力。
///
/// 允许保留的同步函数只有两类 (不涉及插件代码、也没有可等待的工作): 句柄
/// ([musicxx_extern_plugin_host_create] / [musicxx_extern_plugin_host_destroy]),
/// 以及事件通道与纯函数 ([musicxx_extern_plugin_poll_events] /
/// [musicxx_extern_plugin_set_wake_callback] / 版本号 / 返回码名称 / 内存与字符串函数)。
///
/// 线程: 除特别注明外任意线程可调用。提交在调用线程完成 (只记账), 工作体在框架线程
/// (注册表 / 事件) 或插件执行线程 (插件代码) 上跑, 调用线程**不会被占用**。
/// 唤醒回调必须快速返回。
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

#define MUSICXX_EXTERN_PLUGIN_API_VERSION 7

/// 插件框架版本号：`<年>.<月日>.<小版本号>`（例 `2026.1011.1` = 2026 年 10 月 11 日的第 1 版）
///
/// 这是**插件框架自己的版本**（发布节奏用），与 [MUSICXX_EXTERN_PLUGIN_API_VERSION]
/// （跳边界 C ABI 版本，只在破坏性改动时才 +1）是两件事。
/// Dart 侧同名声明是 `MusicxxPluginVersion.version`（`lib/src/version.dart`），
/// 两处必须一致（包内 `test/version_test.dart` 校验）；插件清单的 `version` 用同一套格式。
/// 插件在 `host.info().hostVersion`（JS 是 `musicxx.version`）读到它。
#define MUSICXX_EXTERN_PLUGIN_VERSION "2026.1125.1"

/* ==================== 返回码 ==================== */
//
// 码表只有一份，在 `pluginxx/api/result.h`：通用段（-1 ~ -99）、广播段（-200 ~ -299）、
// 生命周期与内部段（-500 / -999），以及自定义区间（应用 -5000 ~ -9999、插件 -10000 ~ -19999）。
// 网络段（-100 ~ -199）属于实现网络的那一层（`utilxx/net_error.h`），主程序与插件包都有它。
//
// 数字只到 C / FFI 边界：JSON 与上层接口里用名称，查名称用导出的
// [musicxx_extern_plugin_result_name]（Dart 侧也是这样查，不另抄一份表）。

#include "pluginxx/api/result.h"

/* ==================== 插件框架配置标志 ==================== */

#define MUSICXX_EXTERN_PLUGIN_FLAG_SAFE_MODE            0x0001 ///< 不加载任何外部插件
#define MUSICXX_EXTERN_PLUGIN_FLAG_NO_NATIVE            0x0002 ///< 跳过原生(DSO)插件
#define MUSICXX_EXTERN_PLUGIN_FLAG_NO_JS                0x0004 ///< 跳过 JS 插件
#define MUSICXX_EXTERN_PLUGIN_FLAG_DEBUG_OBSERVE_EVENTS 0x0008 ///< 回传 observe/console 观测事件

/// 结果事件的类型名 (唯一完成通道)
#define MUSICXX_EXTERN_PLUGIN_REQUEST_RESULT_EVENT "musicxx.request.result"

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

/// 返回码的名称（`invalid_argument` / `not_found` / `no_value` / …）
///
/// - 覆盖通用段、广播段、生命周期 / 内部段与网络段；
/// - 不认识（包括未定义区间与应用 / 插件自定义区间）返回 NULL，由所有者自己解释；
/// - 返回的字符串是静态字面量，不需要释放。
MUSICXX_EXTERN_PLUGIN_EXPORT const char* MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_result_name(int32_t code);

/// 插件框架库版本字符串 (形如 "2026.1011.1"; 与 MUSICXX_EXTERN_PLUGIN_VERSION 一致);
/// 写入 out (框架堆, 需 string_free)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_version(MusicxxExternPluginString* out);

/* ==================== 插件框架生命周期 ==================== */

/// 创建插件框架实例 (纯构造, 不起线程/不加载插件); 失败返回 NULL 并写 log
///
/// **进程单例**: 同一进程只允许存在一个插件框架实例 (它持有框架线程 / 插件实例 / 事件队列)。
/// 已存在时返回 NULL 并写明确原因 —— 其它 isolate / 线程请复用已有句柄调用管理类 API。
/// `host_destroy` 之后可以再次创建 (init → dispose → init 是正常路径)。
///
/// 同步返回的理由: 句柄无法经事件交回。它只做构造, 不做任何可等待的工作。
MUSICXX_EXTERN_PLUGIN_EXPORT MusicxxExternPluginHost* MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_host_create(
        const MusicxxExternPluginHostConfig* cfg,
        MusicxxExternPluginString*           log
    );

/// 启动插件框架 (`op = "host.start"`)
/// - 起框架线程 / 工作线程池 / 插件执行线程、注册内置提供者;
/// - 受理成功 ⇒ 结果事件在**真的起来之后**才回 (`data = {"ok":true}`),
///   同时照发领域通知 `musicxx.host.ready`;
/// - 加载插件不在这一步: 起好之后由调用方提交 `plugin.scan` / `plugin.load`。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL musicxx_extern_plugin_host_start(
    MusicxxExternPluginHost*    h,
    int64_t*                    out_request_id,
    MusicxxExternPluginString* log
);

/// 关闭插件框架 (`op = "host.shutdown"`; 幂等)
/// - 结果在框架线程与插件执行线程都停好之后才回; 同时照发 `musicxx.host.stopped`;
/// - 停机时表里还没回来的请求一律按 `host_stopped` 回一条 `ok = false`
///   (调用方不会悬挂)。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL musicxx_extern_plugin_host_shutdown(
    MusicxxExternPluginHost*    h,
    int64_t*                    out_request_id,
    MusicxxExternPluginString* log
);

/// 销毁插件框架实例 (未关闭会自动关闭; NULL 安全)
///
/// 同步的原因与 `host_create` 相同: 它释放句柄。
MUSICXX_EXTERN_PLUGIN_EXPORT void MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_host_destroy(MusicxxExternPluginHost* h);

/// 取消一条已受理的请求（**幂等**；不推 `musicxx.request.result` 之外的东西）
///
/// - 立刻以 `canceled`（`PLUGINXX_RESULT_ERR_CANCELED`，`reason = "canceled"`）收尾，
///   晚到的真实结果被丢弃（扣不住，因为请求已经结算过）；
/// - 请求不存在 / 已经收尾过：返回 `0`（OK）且不补第二条结果 —— 调用方可以无脑重试；
/// - 同步调用（只改请求表）；不涉及插件代码，不会阻塞。
///
/// 与“库侧不默认超时”一致：何时放弃等待由调用方决定，超时后调它把这条请求收尾，
/// 否则插件卡住时宿主的 Future 会一直挂着（只有卸载 / 停机才能结束）。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_request_cancel(
        MusicxxExternPluginHost* h,
        int64_t                  request_id,
        MusicxxExternPluginString* log
    );

/// 热更新语言 (`op = "host.language"`; 插件可读; 插件侧 `musicxx.config` 表 get_language)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_set_language(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* lang,
        int64_t*                             out_request_id,
        MusicxxExternPluginString*           log
    );

/// 热更新插件框架配置 (`op = "host.config"`; 插件目录/开关/统计采集等)
/// - 支持字段: pluginDirs[], enableNative, enableJs, safeMode, observeEvents,
///   statsEnabled, statsPeriodMs
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_set_config(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* json,
        int64_t*                             out_request_id,
        MusicxxExternPluginString*           log
    );

/// 下发应用自己的能力声明 (`op = "host.capabilities"`; 整份替换)
///
/// 应用是声明表的权威来源: 主程序启动时把自己那一份推过来, 之后插件想给
/// `<应用名>.*` 的能力提供实现、或问“这条能力开放哪些阶段”, 回答都以推来的为准。
/// 没有推过时插件框架按自带的那份 (`capability_ids.g.h`) 回答。
///
/// - `json`: 数组, 每项 `{id, impls, merge?, phases, timeoutMs}`: `impls` 取
///   `"one"` / `"many"`, `merge` 取 `"none"` / `"first"` / `"veto"` (可省),
///   `phases` 是 `["before","impl","after"]` 的子集 (缺省 `impl`), `timeoutMs` 是
///   声明级超时 (0 = 不上限);
/// - 只认这五个字段, 其余（标题 / 命令行 / MCP 一类的界面声明）不跨边界;
/// - 结果在声明表换好之后才回; 推两次以最后一次为准, 已声明的 id 按新内容覆盖
///   （不再出现在新表里的 id 不会被撤销声明, 插件仍能继续用）。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_set_app_capabilities(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* json,
        int64_t*                             out_request_id,
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

/// 批量取事件 (JSON 数组字符串); 无事件返回 PLUGINXX_RESULT_ERR_TIMEOUT
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_poll_events(
        MusicxxExternPluginHost*    h,
        int32_t                     max_count,
        MusicxxExternPluginString* out_json,
        MusicxxExternPluginString* log
    );

/* ==================== 插件管理 ==================== */

/// 扫描插件目录 (`op = "plugin.scan"`) + 内置清单 → 结果 `data` 是 JSON 数组
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_plugin_scan(
        MusicxxExternPluginHost*    h,
        int64_t*                    out_request_id,
        MusicxxExternPluginString* log
    );

/// 检查一个插件目录 (`op = "plugin.inspect"`; **只读**, 不加载、不改动任何状态)
///
/// 用途: 主程序「从压缩包安装」的预检 —— 把解包出来的临时目录交给插件框架判定
/// (清单可解析 / 当前系统与架构该用哪个分支 / 库文件或脚本在不在), 装之前就有结论。
/// 判定与扫描、加载**完全同源** (同一份 `inspectPluginDir`), 不会出现
/// "预检通过、加载却用另一个分支"的分歧。
///
/// - `dir`: 插件目录 (绝对路径; 压缩包解包后的临时目录同样可以);
/// - `os` / `arch`: 要判定的目标环境 (规范化名字, 别名都认; 传空 = 用插件框架自己的);
/// - 结果 `data`: `{valid, error, path, dirName, id, kind, version, description, author,
///   homepage, entry, apiVersion, depends, optionalDepends, permissions, platforms,
///   arch, scripts?, targetsDir?, target?, targetEntry?, targets[]?, supported, reason,
///   kindKnown, kindRequiresPluginAbi, kindProvider, kindProviderId, kindValidateCapability}`
///   (动态库插件才有 target/targetEntry/targetsDir/targets; JS 插件才有 scripts);
/// - `kind*` 几项是**形态事实**（由形态注册表回答）：未登记的形态 `kindKnown=false`；
///   `kindRequiresPluginAbi` 决定要不要看 `api_version`（解释执行的形态不看）；
///   `kindValidateCapability` 非空时，调用方应在安装前再调一次该能力请运行器判定
///   （参约定见 `docs/plugin-native-api.md` §4.3）；
/// - 目录不存在/清单非法时 `valid=false` + `error` (仍是一次成功的结果);
/// - 清单 `api_version` 低于 `MUSICXX_EXTERN_PLUGIN_MIN_API_VERSION` 时 `supported=false`
///   (+ reason); 声明更高版本不算不支持;
/// - 不看运行期开关 (JS 运行时是否可用、安全模式、禁用动态库), 那些由 `plugin_scan` 叠加。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_plugin_inspect(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* dir,
        const MusicxxExternPluginStringView* os,
        const MusicxxExternPluginStringView* arch,
        int64_t*                             out_request_id,
        MusicxxExternPluginString*           log
    );

/// 当前已加载插件状态快照 (`op = "plugin.list"`; 结果 `data` 是 JSON 数组)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_plugin_list(
        MusicxxExternPluginHost*    h,
        int64_t*                    out_request_id,
        MusicxxExternPluginString* log
    );

/// 加载插件 (`op = "plugin.load"`)
/// - `id_or_path` 可为插件 id 或插件目录/库文件路径;
/// - `options_json`: {"args":{...},"enabled":true,"tempDir":"...","kind":"native|js|builtin"};
/// - 结果在加载事务结束时回; 领域通知 `musicxx.plugin.loaded` / `musicxx.plugin.error`
///   仍照发, **结果事件是权威完成信号**;
/// - 旧符号 `plugin_load_sync` 已删除: 要等加载完成就 `await` 这一次提交。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL musicxx_extern_plugin_plugin_load(
    MusicxxExternPluginHost*              h,
    const MusicxxExternPluginStringView* id_or_path,
    const MusicxxExternPluginStringView* options_json,
    int64_t*                             out_request_id,
    MusicxxExternPluginString*           log
);

/// 启用 / 禁用 / 卸载 / 重载 (`op = "plugin.enable|disable|unload|reload"`)
///
/// - 结果在事务结束时回; 领域事件 (`musicxx.plugin.enabled` 等) 仍照发;
/// - `plugin.reload` = 按当前参数重载: 卸载完成后再加载, 结果在重新加载结束时回;
/// - 卸载时该插件身上还没回来的请求按 `plugin_unloaded` 回 `ok = false`。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL musicxx_extern_plugin_plugin_enable(
    MusicxxExternPluginHost*              h,
    const MusicxxExternPluginStringView* id,
    int64_t*                             out_request_id,
    MusicxxExternPluginString*           log
);
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL musicxx_extern_plugin_plugin_disable(
    MusicxxExternPluginHost*              h,
    const MusicxxExternPluginStringView* id,
    int64_t*                             out_request_id,
    MusicxxExternPluginString*           log
);
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL musicxx_extern_plugin_plugin_unload(
    MusicxxExternPluginHost*              h,
    const MusicxxExternPluginStringView* id,
    int64_t*                             out_request_id,
    MusicxxExternPluginString*           log
);
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL musicxx_extern_plugin_plugin_reload(
    MusicxxExternPluginHost*              h,
    const MusicxxExternPluginStringView* id,
    int64_t*                             out_request_id,
    MusicxxExternPluginString*           log
);

/// 更新插件参数 (`op = "plugin.setArgs"`; 下次 `reload` 生效, 由 Dart 决定何时重载)
/// - `reload` 会把当前参数带过去, 因此"设置参数 → 重载"不会丢参数
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_plugin_set_args(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* id,
        const MusicxxExternPluginStringView* args_json,
        int64_t*                             out_request_id,
        MusicxxExternPluginString*           log
    );

/// 插件配置路径 (`op = "plugin.configPath"`; 未加载的插件按目录推导)
/// 结果 `data` 是路径字符串 (JSON 字符串)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_plugin_get_config_path(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* id,
        int64_t*                             out_request_id,
        MusicxxExternPluginString*           log
    );

/// 调用插件能力 (`op = "plugin.call"`; Dart → 插件)
///
/// - 只受理不等待结果: 成功返回 `PLUGINXX_RESULT_OK` 并写出 `out_request_id`;
///   结果经 `musicxx.request.result` 交回, `data` 是插件给的 JSON;
/// - 受理失败 (参数错 / 队列满 / 插件框架没在跑) 直接返回负错误码, **不推结果事件**;
///   插件没加载 / 没声明该能力 / 归属不对属于**业务失败**, 接一条 `ok = false` 的结果;
/// - **没有 timeout 参数**: 超时是调用方的事 (它自己不等了, 结果丢弃);
/// - **受理成功 ⇒ 一定有终局**: 正常结果、执行失败、插件卸载 (`plugin_unloaded`)、
///   框架停机 (`host_stopped`) 四条路都会推一条结果, 调用方不会一直悬挂;
/// - 旧符号 `plugin_call_async` 已删除 (代价是"库太旧"要靠 C ABI 版本号探测)。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_plugin_call(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* id,
        const MusicxxExternPluginStringView* method,
        const MusicxxExternPluginStringView* args_json,
        int64_t*                             out_request_id,
        MusicxxExternPluginString*           log
    );

/* ==================== 能力派发 ==================== */

/// 派发一条能力 (`op = "capability.dispatch"`; 实现链)
///
/// - 受理只记账与参数检查; 处理器链在插件执行线程上跑 (逐个等处理器完成),
///   **不占调用线程**;
/// - 结果 `data` 形如 `{"handled":true,"result":{...},"called":1,"handlers":2}`;
/// - 广播型 id: 链跑完只回 `{"handled":false,"called":N,"handlers":M}`, 不做合并;
/// - 旧的 `flags` (`DISPATCH_SYNC` / `DISPATCH_ASYNC`) 与 `timeout_ms` 已删除:
///   要不要等结果由调用方决定 (都一样, 结果经事件回), 插件框架不设等待超时。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_capability_dispatch(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* capability_id,
        const MusicxxExternPluginStringView* input_json,
        int64_t*                             out_request_id,
        MusicxxExternPluginString*           log
    );

/* ---------- 分阶段派发 / 查询（不分阶段的符号 = impl 阶段 / 全部阶段） ---------- */

/// 阶段编号（与生成头 `capability_ids.g.h` 的 `MUSICXX_CAPABILITY_PHASE_*` 一致）
#define MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_IMPL   0
#define MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_BEFORE 1
#define MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_AFTER  2

/// 按阶段派发能力 (`op = "capability.dispatch"`)
///
/// - `phase` = `impl`：与 `musicxx_extern_plugin_capability_dispatch` 完全一样；
/// - `phase` = `before`：串行、**正序**（priority 升序、同优先级按注册顺序），载荷逐步累积；
///   `result.patch` 是累计后的 patch，`result.action` 为 `stop` 时表示整条链被停下；
/// - `phase` = `after`：串行、**逆序**（priority 降序、同优先级注册顺序也反），值逐步替换；
///   `result.value` 是最终值，`result.action` 为 `drop` 时表示结果被丢弃；
/// - 该阶段没有处理器时结果 `data` 是 `{"handled":false,"handlers":0}`。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_capability_dispatch_phase(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* capability_id,
        int32_t                               phase,
        const MusicxxExternPluginStringView* input_json,
        int64_t*                             out_request_id,
        MusicxxExternPluginString*           log
    );

/// 该条 id 当前的处理器列表 (`op = "capability.handlers"`; 自省用)
///
/// 按**调用顺序**给出: priority 升序, 同优先级按注册顺序。结果 `data` 形如:
/// {"capability":"musicxx.media.spectrum.extract","count":2,"handlers":[
///   {"plugin":"example_js","instance":"js:example_js","kind":"js","phase":"impl",
///    "priority":0,"ownerTag":""},
///   {"plugin":"example_native","instance":"example_native","kind":"native",
///    "phase":"after","priority":10,"ownerTag":""}]}
///
/// 只给出 plugin / instance / kind / phase / priority / ownerTag: 不暴露处理器地址、载荷与内部状态。
/// `instance` 与插件框架里的实例名一致 (JS 是 `js:<id>`)。
/// 这是**全部阶段**的列表；只看一个阶段用 `_capability_handlers_phase`。
///
/// 计数由 Dart 侧按 `musicxx.capability.changed` 事件与 `plugin.load` 结果里的清单维护,
/// 需要精确回查时用这一个入口 (旧的 `capability_handler_count(_phase)` 已删除)。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_capability_handlers(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* capability_id,
        int64_t*                             out_request_id,
        MusicxxExternPluginString*           log
    );

/// 某条 id 在某个阶段上的处理器列表（形状与 `capability_handlers` 相同，只有该阶段）
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_capability_handlers_phase(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* capability_id,
        int32_t                               phase,
        int64_t*                             out_request_id,
        MusicxxExternPluginString*           log
    );

/// 能力处理器耗时/失败统计 (`op = "capability.stats"`; 调试页; 只观测不限制)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_capability_stats(
        MusicxxExternPluginHost*    h,
        int64_t*                    out_request_id,
        MusicxxExternPluginString* log
    );

/* ==================== 能力请求 (插件 → Dart) ==================== */
/// 回复插件发起的能力请求 (`op = "capability.respond"`; status 见错误码; result_json 可为空)
///
/// 处理器链已经搬离框架 IO 线程, 这条回复可以放心排队。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_capability_app_respond(
        MusicxxExternPluginHost*              h,
        int64_t                               request_id,
        int32_t                               status,
        const MusicxxExternPluginStringView* result_json,
        int64_t*                             out_request_id,
        MusicxxExternPluginString*           log
    );

/* ==================== 广播 (Dart ⇄ 原生; 只发布 / 读 / 列表) ==================== */

/// 广播 = 所有者发一次, 关心的人读当前值或订阅。
///
/// **只有发布、读当前值、列频道三个入口**:
/// - 能改应用状态的通道是**能力** (改设置、切主题、跳转进度…), 广播侧没有写入;
/// - 声明由宿主按生成表 (`capability_ids.h`) 补; 插件声明自己的频道走插件接口表;
/// - 订阅在插件侧 (`musicxx.broadcasts` 表, 带自己的 `intervalMs` / `mode`);
///   Dart 侧订阅走 `broadcast_watch`, 读当前值走本地缓存。

/// 插件框架能力位 (Dart 侧据此判断加载到的插件框架库是不是这次构建的产物)
///
/// 缺位时**不要**继续调用对应的一批 API: 老库上没有这些符号, 报错会是一句难懂的
/// `undefined symbol`。包层在 init 时读一次并给出"请重新构建插件框架库"的说明。
#define MUSICXX_EXTERN_PLUGIN_FEATURE_CAPABILITIES  0x1
/// 广播 (发布 / 读当前值 / 列频道); 声明与订阅走插件接口表
#define MUSICXX_EXTERN_PLUGIN_FEATURE_BROADCASTS 0x16
#define MUSICXX_EXTERN_PLUGIN_FEATURE_UI     0x8
/// 分阶段派发（`_capability_dispatch_phase` / `_capability_handlers_phase`）
#define MUSICXX_EXTERN_PLUGIN_FEATURE_CAPABILITY_PHASES 0x20
/// 插件网络能力 (`musicxx.net` 表: HTTP / TCP / WebSocket)
#define MUSICXX_EXTERN_PLUGIN_FEATURE_NET 0x40
/// 提交 + 结果事件 (`musicxx.request.result`) 与同步入口的删除
#define MUSICXX_EXTERN_PLUGIN_FEATURE_REQUESTS 0x80

/// 插件框架能力位图 (无需框架实例; 任意线程可调用)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_feature_bits(void);

/// 发布一条广播 (`op = "broadcast.publish"`; 任何频道都用它)
///
/// - `topic` 必须是 `musicxx.*` 或 `plugin.<id>.*`;
/// - 频道未声明时按生成表补一份声明, 表外 id 用默认属性
///   (`cache: none` / `publish: owner` / `coalesce: true` / `scope: app`);
/// - 所有者才能发布: 官方频道只有应用能发, 插件频道只有所有者能发 (发布失败是 `ok = false`);
/// - 同值重复发布默认仍会通知 (去重在 Dart 侧封装里做);
/// - 结果 `data` 是 `null`: 要"发布完再看"就 `await` 这一条。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_broadcast_publish(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* topic,
        const MusicxxExternPluginStringView* payload_json,
        int64_t*                             out_request_id,
        MusicxxExternPluginString*           log
    );

/// 读一条频道的当前值 (`op = "broadcast.read"`; 冷读; 频繁执行的路径读 Dart 侧缓存)
///
/// - 成功: `data` 是当前值 JSON;
/// - 频道没声明: `ok = false` + `reason = not_declared`;
/// - `cache: none` / 还没发过: `ok = false` + `reason = no_value`。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_broadcast_read(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* id,
        int64_t*                             out_request_id,
        MusicxxExternPluginString*           log
    );

/// 列出全部频道 (`op = "broadcast.list"`; 声明 + 当前值 + 订阅数; `prefix` 可空)
///
/// 结果 `data` 形如 `{"broadcasts":[{"id","cache","publish","scope","subscribers","hasValue","value"}]}`。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_broadcast_list(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* prefix,
        int64_t*                             out_request_id,
        MusicxxExternPluginString*           log
    );

/* ==================== UI / 日志 / 调试 / 统计 ==================== */

/// 插件贡献的 UI 项快照 (`op = "ui.snapshot"`; 结果 `data` 是 JSON 数组)
/// 变更另有 `musicxx.ui.changed` 事件
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_ui_snapshot(
        MusicxxExternPluginHost*    h,
        int64_t*                    out_request_id,
        MusicxxExternPluginString* log
    );

/// 内容注册表快照 (`op = "ui.contents"`; 结果 `data` 是 JSON 数组:
/// id / shortId / plugin / slot / title / depict / order / deps / minIntervalMs)
///
/// 变更另有 `musicxx.ui.contents` 事件 (载荷带该插件的全部内容)。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_ui_contents(
        MusicxxExternPluginHost*    h,
        int64_t*                    out_request_id,
        MusicxxExternPluginString* log
    );

/// 槽位表快照 (`op = "ui.slots"`; 结果 `data` 是 JSON 数组: id / kind / owner)
///
/// 变更另有 `musicxx.ui.slots` 事件。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_ui_slots(
        MusicxxExternPluginHost*    h,
        int64_t*                    out_request_id,
        MusicxxExternPluginString* log
    );

/// 让插件为一份内容生成一次界面 (`op = "ui.request"`; Dart → 原生)
///
/// - `id`: 内容全名 `plugin.<插件id>.<内容 id>`;
///   `context_json`: 声明过的依赖变量 → 当前值 (JSON 对象);
/// - 生成在**插件执行线程**上跑 (原生插件) / JS 线程 (JS 插件), 不占框架线程;
/// - 结果 `data` 是 `{slot, id, shortId, plugin, rev, json, data}` 或带 `error`;
///   同一份内容照旧推 `musicxx.ui.content` 事件 (界面挂载点读那份);
/// - 没有这份内容: `ok = false` + `reason = not_found`。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_ui_request(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* id,
        const MusicxxExternPluginStringView* context_json,
        int64_t*                             out_request_id,
        MusicxxExternPluginString*           log
    );

/// 订阅 / 退订一个广播频道的值变化 (`op = "broadcast.watch"`)
///
/// - 订阅成功后宿主**先补发当前值** (`musicxx.broadcast.changed`, 没有值就不发),
///   再回这一条确认 —— 所以 `await watch` 之后本地缓存里就有当前值了;
/// - 有人订阅时频道值变化会推 `musicxx.broadcast.changed {id, value}` 事件;
/// - 订阅计数归零就停 (插件停用 / 卸载时框架自己清掉)。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_broadcast_watch(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* id,
        int32_t                              watch,
        int64_t*                             out_request_id,
        MusicxxExternPluginString*           log
    );

/// Dart → 原生日志 (`op = "log"`; 统一写入磁盘/转发; level: 0 trace .. 4 error)
///
/// 结果 `data` 是 `null`; 不关心就 `.ignore()`。
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL musicxx_extern_plugin_log(
    MusicxxExternPluginHost*              h,
    int32_t                               level,
    const MusicxxExternPluginStringView* message,
    int64_t*                             out_request_id,
    MusicxxExternPluginString*           log
);

/// 调试信息 (`op = "debug.info"`; 版本/依赖/平台/插件统计; 支持页复制)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_debug_info(
        MusicxxExternPluginHost*    h,
        int64_t*                    out_request_id,
        MusicxxExternPluginString* log
    );

/// 资源与耗时统计快照 (`op = "stats"`; scope_json = NULL 取全部)
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL musicxx_extern_plugin_stats(
    MusicxxExternPluginHost*              h,
    const MusicxxExternPluginStringView* scope_json,
    int64_t*                             out_request_id,
    MusicxxExternPluginString*           log
);

/// 统计采集配置 (`op = "stats.config"`; {"enable":bool,"jsHeapSampleMs":int,"observeEvents":bool})
MUSICXX_EXTERN_PLUGIN_EXPORT int32_t MUSICXX_EXTERN_PLUGIN_CALL
    musicxx_extern_plugin_stats_config(
        MusicxxExternPluginHost*              h,
        const MusicxxExternPluginStringView* cfg_json,
        int64_t*                             out_request_id,
        MusicxxExternPluginString*           log
    );

#pragma pack(pop)

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MUSICXX_EXTERN_PLUGIN_API_H */
