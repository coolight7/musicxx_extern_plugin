/// musicxx 插件 SDK: musicxx 专用约定 (与主程序业务相关, 与插件框架通用部分分开)
///
/// 命名: 官方标识统一 `musicxx.*` 前缀; 插件自定义统一 `plugin.<pluginId>.*`。
/// 本头只声明**跨边界稳定约定** (类型/宏/IID), 纯 C 可包含 (便于 C 插件与外部工具)。
///
/// 目录:
/// - 入口符号名: `musicxx_plugin_*` (由框架在 entrySymbols() 中交出同一批名字)
/// - 专用接口表 IID: 通用表（能力与广播等）的 IID 由应用名拼出，形如 `musicxx.capabilities` /
///   `musicxx.broadcasts`（见 `pluginxx/api/app.h`）与
///   `musicxx.host` / `musicxx.ui` / `musicxx.kind` / `musicxx.player` /
///   `musicxx.playlist` / `musicxx.songlist` / `musicxx.lyric` / `musicxx.storage` / `musicxx.stats`
///   (已实现 host / ui / kind 三张专用表; 网络表 `<应用名>.net` 只依赖"op + JSON"的通用写法,
///   已移到插件框架的通用表 `pluginxx/api/net.h`, 本头照样 include 它;
///   其余 IID 先冻结, 表版本 1 留待后续实现;
///   每张表都要带齐"四件" —— IID + `X_VERSION` + `X_MIN_VERSION` + `X_BASELINE_SIZE`,
///   规则见下面"专用接口表: 每张表四项 + 判定规则")
/// - 能力的裁决动作常量
#ifndef MUSICXX_PLUGIN_API_H
#define MUSICXX_PLUGIN_API_H

#include <stddef.h>
#include <stdint.h>

#include "musicxx/plugin/api/capability_ids.g.h"
#include "musicxx/plugin/api/ui_slots.g.h"
#include "pluginxx/api/abi.h"
#include "pluginxx/api/kinds.h"
#include "pluginxx/api/net.h"
#include "pluginxx/api/tables.h"
#include "pluginxx/api/ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 入口符号名 (框架自定义) ==================== */

/// 入口符号名: 与插件框架的 `PluginEntrySymbols` 一致 —— 只有四个
/// (`get_info` 可空, 其余三个必需), "开始播放 / 开始工作" 不再单独立一个符号:
/// `create` 就是"构造 + 注册事务", `stop` 撤销注册, `destroy` 释放对象。
#define MUSICXX_PLUGIN_SYMBOL_GET_INFO "musicxx_plugin_get_info"
#define MUSICXX_PLUGIN_SYMBOL_CREATE   "musicxx_plugin_create"
#define MUSICXX_PLUGIN_SYMBOL_STOP     "musicxx_plugin_stop"
#define MUSICXX_PLUGIN_SYMBOL_DESTROY  "musicxx_plugin_destroy"

/// 插件 ABI 代次 (= cxx_pluginxx 的 `PLUGINXX_MIN_API_VERSION`, 也是宿主编译时下发给
/// `PLUGINXX_API_VERSION` 的同一个值: 目前“当前代次”与“最低接受代次”是同一个数)
///
/// 供插件读 `get_info` 展示与能力判定, 清单 `api_version` 也写这个值。
/// 值由构建侧给出 (本包 `src/CMakeLists.txt` 用 `MUSICXX_EXTERN_PLUGIN_MIN_API_VERSION`
/// 覆盖 cxx_pluginxx 的默认值): 插件包含本头即拿到与插件框架库一致的值, 不要手写数字。
#define MUSICXX_EXTERN_PLUGIN_MIN_API_VERSION PLUGINXX_MIN_API_VERSION

/// 框架支持的最低插件 API 版本 (清单 `api_version` 与 `PluginxxInfo.api_version`
/// 都按它检查; 就是上面的 `MUSICXX_EXTERN_PLUGIN_MIN_API_VERSION`)
///
/// 规则只有一条: **插件版本必须 >= 本值**。声明比当前框架更高的版本不再被拒绝
/// (插件自己按 `host.info()` 里的 `apiVersion` 决定降级), 声明更低版本只要不低于
/// 本值也照常加载。

#pragma pack(push, 8)

/* ==================== 能力: 裁决结果里的 action ==================== */

/// 裁决结果里的 `action` 取值 (与 Dart 侧 `MusicxxPluginCapabilityAction` 一一对应)
///
/// 一个 id 是能力还是广播由 `tools/capabilities.def.json` 的声明决定 (插件不能自选):
/// 广播只通知, 处理器给什么都不看; 能力按声明里的 `merge` 合并结果。
#define MUSICXX_PLUGIN_CAPABILITY_ACTION_CONTINUE "continue"
#define MUSICXX_PLUGIN_CAPABILITY_ACTION_SKIP     "skip"
#define MUSICXX_PLUGIN_CAPABILITY_ACTION_CANCEL   "cancel"
#define MUSICXX_PLUGIN_CAPABILITY_ACTION_DROP     "drop"

/* ==================== 专用接口表: 每张表四项 + 判定规则 ==================== */

/// 下面每张表都带四项, 与 pluginxx 通用表同一套规矩 (判定实现见 `pluginxx/api/iface_meta.h`):
/// - **IID** (`MUSICXX_PLUGIN_IFACE_<名>`): 查询用的键; 换约定 = 换 IID;
/// - **`version` 字段** (`X_VERSION`): 本头描述的完整布局与含义级别; 追加成员或含义变化时 +1;
/// - **`X_MIN_VERSION`**: 本头能与之协同工作的最低表版本 (不再兼容旧布局/旧含义时才 +1);
/// - **`X_BASELINE_SIZE`**: 表头之后所有主程序都应具备的字节数; 重置时 = 整表大小,
///   以后追加成员时保持不变 (改成第一个追加成员的偏移)。
///
/// 判定: 表可用 = IID 命中 且 `version >= X_MIN_VERSION` 且 `struct_size >= X_BASELINE_SIZE`
/// (SDK 的 `pluginxx::queryInterface` 按它做, 插件不用自己判)。
/// **表可用不等于全部成员可用**: 基线以内的成员由上面两条门槛保证, 追加在表尾的成员
/// 必须再判 `pluginxx::ifaceHas<&表::成员>(t)` (只判空指针会读到紧邻的另一块内存)。
///
/// 改动规则: 成员**只能追加在表尾**; 追加时 `X_VERSION` +1、`X_BASELINE_SIZE` 不变;
/// **禁止**重排成员、改成员签名、删成员 (结构体大小不变, 门槛挡不住, 会静默调到隔壁函数)。

/* ==================== 接口表: musicxx.host ==================== */

#define MUSICXX_PLUGIN_IFACE_HOST             "musicxx.host"
/// 专用表的 IID 是**已发布的冻结值**, 继续写字面量; 它与应用名拼出来的一致
/// (`PLUGINXX_APP_ID("host")` → `"musicxx.host"`), 改了应用名就对不上。
#ifdef __cplusplus
static_assert(std::string_view(PLUGINXX_APP_ID("host")) == MUSICXX_PLUGIN_IFACE_HOST);
#endif
/// v2: 删掉 `get_state` / `subscribe_state`（读当前值走广播频道）与
/// `request_action` / `cancel_action`（要应用做事就调能力，走 `musicxx.capabilities`）
/// v3: 成员改成**异步操作契约**（与插件框架的通用表同一套：返回句柄 = 已受理，
///     结果经完成回调恰好一次交回，返回 `NULL` = 未受理并写 `error_out`）
#define MUSICXX_PLUGIN_IFACE_HOST_VERSION     3
#define MUSICXX_PLUGIN_IFACE_HOST_MIN_VERSION 3
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为第一个追加成员的偏移) */
#define MUSICXX_PLUGIN_IFACE_HOST_BASELINE_SIZE sizeof(MusicxxPluginHostIface)

typedef struct MusicxxPluginHostIface {
    int32_t  version;     ///< 主程序填 MUSICXX_PLUGIN_IFACE_HOST_VERSION
    uint32_t struct_size;

    /// 框架信息 (主程序版本/平台/语言/插件目录; 结果 JSON 对象)
    PluginxxOperatorHandle*(PLUGINXX_CALL* get_info)(
        const PluginxxHost*      host,
        PluginxxOperatorCallback cb,
        void*                    cb_ud,
        PluginxxString*          error_out
    );

    /// 本插件配置路径 (插件框架按插件 id 推导; 供插件读写自己的配置)
    PluginxxOperatorHandle*(PLUGINXX_CALL* get_plugin_config_path)(
        const PluginxxHost*      host,
        PluginxxOperatorCallback cb,
        void*                    cb_ud,
        PluginxxString*          error_out
    );
} MusicxxPluginHostIface;

/* ==================== 接口表: musicxx.ui (声明式 UI 扩展) ==================== */

/// 表本体在通用侧（子模块 `pluginxx/api/ui.h`）：成员、版本、基线与 IID 拼法
/// （`PLUGINXX_APP_ID("ui")` → `musicxx.ui`）只有一份。本头只保留 musicxx 侧的宏名与
/// typedef，插件代码继续用 `MUSICXX_PLUGIN_IFACE_UI*` / `MusicxxPluginUIIface`。
///
/// musicxx 自己的约定（写在通用头里的是与项目无关的部分）：
/// - 槽位 id 由生成头 `ui_slots.g.h` 给出（`MUSICXX_PLUGIN_UI_SLOT_*`，声明源
///   `tools/ui_slots.def.json`），写简称（去掉 `musicxx.ui.` 前缀）或全名都可以；
/// - 声明项的动作里，`call` 的名字可以是短名（回自己）、`plugin.<插件id>.<名字>`（找那个插件）
///   或 `musicxx.*`（调应用能力）；`route` 的目标是 `ext://<插件id>/<内容 id>`，
///   允许指向任意插件（页面由被跳转的插件自己绘制）。

#define MUSICXX_PLUGIN_IFACE_UI                 "musicxx.ui"
#ifdef __cplusplus
static_assert(std::string_view(PLUGINXX_APP_ID("ui")) == MUSICXX_PLUGIN_IFACE_UI);
#endif
#define MUSICXX_PLUGIN_IFACE_UI_VERSION         PLUGINXX_IFACE_UI_VERSION
#define MUSICXX_PLUGIN_IFACE_UI_MIN_VERSION     PLUGINXX_IFACE_UI_MIN_VERSION
#define MUSICXX_PLUGIN_IFACE_UI_BASELINE_SIZE   PLUGINXX_IFACE_UI_BASELINE_SIZE

/* UI 项动作类型 (data.action; 与界面描述层的写法一致) */
/// 打开声明式插件页面: {"kind":"route","route":"ext://<插件id>/<内容 id>"}
#define MUSICXX_PLUGIN_UI_ACTION_ROUTE           PLUGINXX_UI_ACTION_ROUTE
/// 调用一个能力: {"kind":"call","name":"<名字>","args":{...},"timeoutMs":?}
#define MUSICXX_PLUGIN_UI_ACTION_CALL            PLUGINXX_UI_ACTION_CALL
/// 无动作 (纯展示项)
#define MUSICXX_PLUGIN_UI_ACTION_NONE            PLUGINXX_UI_ACTION_NONE

typedef PluginxxUiEntrySpec   MusicxxPluginUIEntrySpec;
typedef PluginxxUiContentSpec MusicxxPluginUIContentSpec;
typedef PluginxxUiSlotSpec    MusicxxPluginUISlotSpec;
typedef PluginxxUiIface       MusicxxPluginUIIface;


/* ==================== 接口表: <应用名>.kind (插件形态 / 运行器) ==================== */

/// 表本体在通用侧（子模块 `pluginxx/api/kinds.h`）：IID / 版本 / 基线与成员都只有一份，
/// 注册表实现在 `pluginxx/host/kind_registry.h`。本头只保留 musicxx 侧的旧宏名与 typedef，
/// 插件代码继续用 `MUSICXX_PLUGIN_IFACE_KIND*` / `MusicxxPluginKindIface`。
///
/// 语义（登记 / 撤销 / 列表、每个形态自己的接口代次对、可选的 `validateCapability`、
/// 只能接管解释执行的自带形态）见通用头与 `docs/plugin-native-api.md` §4.3。

#define MUSICXX_PLUGIN_IFACE_KIND               PLUGINXX_IFACE_KIND
#ifdef __cplusplus
static_assert(std::string_view(PLUGINXX_APP_ID("kind")) == MUSICXX_PLUGIN_IFACE_KIND);
#endif
#define MUSICXX_PLUGIN_IFACE_KIND_VERSION       PLUGINXX_IFACE_KIND_VERSION
#define MUSICXX_PLUGIN_IFACE_KIND_MIN_VERSION   PLUGINXX_IFACE_KIND_MIN_VERSION
#define MUSICXX_PLUGIN_IFACE_KIND_BASELINE_SIZE PLUGINXX_IFACE_KIND_BASELINE_SIZE

typedef PluginxxKindIface MusicxxPluginKindIface;

/// 运行器能力的**载荷与回答**（`loadCapability` / `unloadCapability` 收发的 JSON）
///
/// 请求（插件框架 → 运行器）：
/// ```json
/// {"action":"load","kind":"python","id":"demo","path":"<插件目录>",
///  "instance":"python:demo","version":"1.0.0"}
/// ```
/// `action` 取 `load` / `unload`；`instance` 是建议的实例名（`<kind>:<插件id>`），运行器
/// 可以只用它做日志与命名空间。回答 `{"ok":true}` 表示接手成功；`{"ok":false,
/// "error":"..."}` 或能力失败表示这次加载 / 卸载失败。运行器**不要在能力里阻塞**：
/// 插件框架会等这次回答（加载 30 s、卸载 15 s），长任务请自己排队。

/* ==================== 预留接口表 (v1 只冻结 IID 与版本; 表体后续实现) ==================== */

#define MUSICXX_PLUGIN_IFACE_PLAYER    "musicxx.player"
#define MUSICXX_PLUGIN_IFACE_PLAYLIST  "musicxx.playlist" ///< 歌单（保存下来的列表）
#define MUSICXX_PLUGIN_IFACE_SONGLIST  "musicxx.songlist" ///< 播放列表（当前正在播的那一串歌）
#define MUSICXX_PLUGIN_IFACE_LYRIC     "musicxx.lyric" ///< 歌词（单数域，与能力 / 广播频道一致）
#define MUSICXX_PLUGIN_IFACE_STORAGE   "musicxx.storage"
#define MUSICXX_PLUGIN_IFACE_STATS     "musicxx.stats"
#define MUSICXX_PLUGIN_IFACE_AGENT     "musicxx.agent" ///< 预留: 实现 AI 相关能力时使用

/* ==================== 状态广播的键 ==================== */

/// 官方的 `musicxx.state.*` 广播频道键由生成头给出:
/// `MUSICXX_BROADCAST_STATE_*`（`capability_ids.g.h`，声明源是 `tools/capabilities.def.json`）。
/// 读当前值用广播频道（`musicxx.broadcast.read` / SDK 的 `readBroadcast`）。

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* MUSICXX_PLUGIN_API_H */
