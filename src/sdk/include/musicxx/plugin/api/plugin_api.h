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
#include "pluginxx/api/net.h"
#include "pluginxx/api/tables.h"

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

/// 界面分两半:
/// - **声明项**(entry): 入口 / 菜单项 / 音效 / 可选样式的元信息, 只含数据, 不含界面树;
/// - **内容**(content): 挂在某个槽位上的界面 JSON, 由生成函数在需要时生成, 结果进主程序的缓存。
///
/// 主程序的渲染路径**只读缓存**, 不会在渲染时同步调用插件: 没有内容时插入位置不渲染、
/// 替换位置画内置界面、`UISlot` 画 `fallback`。
///
/// 槽位 id 由生成头 `ui_slots.g.h` 给出(`MUSICXX_PLUGIN_UI_SLOT_*`, 声明源
/// `tools/ui_slots.def.json`), 写简称(去掉 `musicxx.ui.` 前缀)或全名都可以。
/// 插件也可以声明自己的槽位(`plugin.<插件id>.ui.<名字>`), 由别的插件往里注册内容。
#define MUSICXX_PLUGIN_IFACE_UI             "musicxx.ui"
#ifdef __cplusplus
static_assert(std::string_view(PLUGINXX_APP_ID("ui")) == MUSICXX_PLUGIN_IFACE_UI);
#endif
/// v3: 内容 + 槽位 + 声明项分开 (删掉 `update_entry`: 声明没变时要重画就重新生成内容)
/// v4: 成员与生成函数都改成**异步操作契约**（结果经完成回调 / 通知恰好一次交回）
#define MUSICXX_PLUGIN_IFACE_UI_VERSION     4
#define MUSICXX_PLUGIN_IFACE_UI_MIN_VERSION 4
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为第一个追加成员的偏移) */
#define MUSICXX_PLUGIN_IFACE_UI_BASELINE_SIZE sizeof(MusicxxPluginUIIface)

/* UI 项动作类型 (data.action; 与界面描述层的写法一致) */
/// 打开声明式插件页面: {"kind":"route","route":"ext://<插件id>/<内容 id>"}
/// (允许指向任意插件: 页面由被跳转插件自己绘制)
#define MUSICXX_PLUGIN_UI_ACTION_ROUTE      "route"
/// 调用一个能力: {"kind":"call","name":"<名字>","args":{...},"timeoutMs":?}
/// 名字按前缀决定交给谁: 短名回自己(`plugin.<自己>.<名字>`)、`plugin.<插件id>.<名字>` 找那个插件、
/// `musicxx.*` 调应用能力。字符串短写 `"名字"` 等价于这一种。
#define MUSICXX_PLUGIN_UI_ACTION_CALL       "call"
/// 无动作 (纯展示项)
#define MUSICXX_PLUGIN_UI_ACTION_NONE       "none"

/// 声明项注册规格 (入口 / 菜单 / 音效 / 可选样式的元信息)
///
/// `item_id` 可以是本插件的**短名**(插件框架自动补 `plugin.<pluginId>.` 前缀),
/// 也可以是完整全名(必须属于本插件命名空间, 否则拒绝注册)。
/// `data_json` 是类型相关的声明式数据(JSON 对象); 界面内容不写在这里 ——
/// 要画界面就注册内容(`register_content`)。
typedef struct MusicxxPluginUIEntrySpec {
    int32_t  version;      ///< == 1
    uint32_t struct_size;  ///< == sizeof(MusicxxPluginUIEntrySpec)

    PluginxxStringView item_id;   ///< 短名 "card" 或全名 "plugin.ad_skipper.card"
    PluginxxStringView type;      ///< MUSICXX_PLUGIN_UI_SLOT_*
    PluginxxStringView data_json; ///< 声明式数据 (JSON 对象)

    int32_t order;  ///< 排序权重 (小者靠前; 同权重按注册顺序)
    int32_t flags;  ///< 预留 (必须为 0)
} MusicxxPluginUIEntrySpec;

/// 一份内容的注册规格
///
/// 生成函数把"声明里的变量"变成界面 JSON: 入参是 `context`(JSON 对象, 键是 `deps` 里声明的变量),
/// 返回 `{"json": <界面 JSON>, "data": <任意 JSON, 第一版只保存>}`。
/// **必须快**(几毫秒内): 要等网络 / 文件就先返回一个"加载中"的结构, 拿到结果后再 `push_content`。
///
/// `title` / `depict` 是给人看的: 可选样式槽位(`musicxx.ui.playing.background` 这类)的候选列表
/// 要在用户选中之前就列出来, 而那时还没有内容, 所以名字只能在注册时给。
typedef struct MusicxxPluginUIContentSpec {
    int32_t  version;      ///< == 1
    uint32_t struct_size;  ///< == sizeof(MusicxxPluginUIContentSpec)

    PluginxxStringView id;        ///< 短 id (同一插件同一槽位内唯一)
    PluginxxStringView slot;      ///< 槽位全名或简称; 也可以是 plugin.<自己>.ui.<名字>
    /// 展示名 (可选样式槽位的候选列表用; 空 = 用 id)
    ///
    /// 必须在声明里就给: 用户要在**选中之前**就看到名字, 不能等生成。
    PluginxxStringView title;
    /// 一句话说明 (可选样式槽位的副标题; 可为空)
    PluginxxStringView depict;
    /// 适用的页面路由 (JSON 数组; 只对 `pageBackground` 槽位有意义)
    ///
    /// 要在注册时给: 客户端得先按路由挑出"这一页用哪一份", 而那时内容还没生成。
    /// 不写 / 空数组 = 所有普通页面。
    PluginxxStringView routes_json;
    PluginxxStringView deps_json; ///< 变量 id 的 JSON 数组 (只订阅这些, 也只把这些放进 context)
    int32_t            order;     ///< 排序权重 (小者靠前)
    int32_t min_interval_ms;      ///< 0 = 不按时间节流 (默认); >0 = 两次生成之间的最短间隔
    int32_t            flags;     ///< 预留 (必须为 0)

    /// 生成函数: ctx_json 是 context (JSON 对象)，完成后经 `notify` 恰好一次交回结果
    ///
    /// - 成功: `notify->done(host_ud, PLUGINXX_OPERATOR_OK, {"json":…,"data":…})`；
    /// - 可以同步完成，也可以先返回、拿到数据后再完成（协程 / 异步请求都行）；
    /// - 失败: `notify->done(host_ud, PLUGINXX_OPERATOR_FAILED, "原因")`
    ///   （主程序保留上一次有效内容并记一条日志）；
    /// - 生成**必须快**：要等网络 / 文件就先给一个"加载中"的结构，拿到结果后用
    ///   `push_content` 再发一份。主程序不会为了等它而停下帧。
    ///
    /// 完成协议沿用了插件框架的能力处理器：参数顺序 `(host, ctx, user_data, notify, error_out)`。
    void(PLUGINXX_CALL *generate)(
        const PluginxxHost*           host,
        const PluginxxStringView*     ctx_json,
        void*                         user_data,
        const PluginxxOperatorNotify* notify,
        PluginxxString*               error_out
    );
    void* user_data; ///< 生成函数需要的上下文 (可为空)
} MusicxxPluginUIContentSpec;

/// 插件自己声明一个槽位 (只允许 `insert` / `replace`)
typedef struct MusicxxPluginUISlotSpec {
    int32_t  version;      ///< == 1
    uint32_t struct_size;  ///< == sizeof(MusicxxPluginUISlotSpec)

    PluginxxStringView id;   ///< plugin.<自己>.ui.<名字>
    PluginxxStringView kind; ///< "insert" / "replace"
    int32_t            flags; ///< 预留 (必须为 0)
} MusicxxPluginUISlotSpec;

typedef struct MusicxxPluginUIIface {
    int32_t  version;     ///< 主程序填 MUSICXX_PLUGIN_IFACE_UI_VERSION
    uint32_t struct_size;

    /* ---- 声明项 (入口 / 菜单 / 音效 / 可选样式的元信息) ---- */

    /// 注册/覆盖一个声明项
    ///
    /// 完成时: 成功 `status = OK` (无结果正文); 失败正文是原因 (`arg` / `state` /
    /// `unknown_type` / `namespace`)。
    PluginxxOperatorHandle*(PLUGINXX_CALL* register_entry)(
        const PluginxxHost*                host,
        const MusicxxPluginUIEntrySpec*    spec,
        PluginxxOperatorCallback           cb,
        void*                              cb_ud,
        PluginxxString*                    error_out
    );

    /// 注销一个声明项 (item_id 必须是本插件的项; 不存在按 `not_found` 失败)
    PluginxxOperatorHandle*(PLUGINXX_CALL* unregister_entry)(
        const PluginxxHost*               host,
        const PluginxxStringView*         item_id,
        PluginxxOperatorCallback          cb,
        void*                             cb_ud,
        PluginxxString*                   error_out
    );

    /// 列出本插件已注册的声明项 (结果 JSON 数组)
    PluginxxOperatorHandle*(PLUGINXX_CALL* list_entries)(
        const PluginxxHost*               host,
        PluginxxOperatorCallback          cb,
        void*                             cb_ud,
        PluginxxString*                   error_out
    );

    /* ---- 内容 (挂在槽位上的界面) ---- */

    /// 注册/覆盖一份内容
    /// - 同一个 (槽位, 短 id) 再注册 = 覆盖这一条 (deps / order / 生成函数一起换);
    /// - 完成时: 成功无结果正文; 失败正文是原因 (`arg` / `state` / `unknown_slot` / `namespace`)。
    PluginxxOperatorHandle*(PLUGINXX_CALL* register_content)(
        const PluginxxHost*                  host,
        const MusicxxPluginUIContentSpec*    spec,
        PluginxxOperatorCallback             cb,
        void*                                cb_ud,
        PluginxxString*                      error_out
    );

    /// 注销一份内容 (id 可以是短 id 或全名; 不存在按 `not_found` 失败)
    PluginxxOperatorHandle*(PLUGINXX_CALL* unregister_content)(
        const PluginxxHost*               host,
        const PluginxxStringView*         content_id,
        PluginxxOperatorCallback          cb,
        void*                             cb_ud,
        PluginxxString*                   error_out
    );

    /// 直接给一份内容 (不等主程序来问): content_json = {"json":…,"data":…}
    ///
    /// 框架只入队, 不回调插件、不等结果 (不重入)。版本号与请求产物同一条递增。
    PluginxxOperatorHandle*(PLUGINXX_CALL* push_content)(
        const PluginxxHost*               host,
        const PluginxxStringView*         content_id,
        const PluginxxStringView*         content_json,
        PluginxxOperatorCallback          cb,
        void*                             cb_ud,
        PluginxxString*                   error_out
    );

    /// 列出本插件已注册的内容 (结果 JSON 数组: slot / id / deps / order)
    PluginxxOperatorHandle*(PLUGINXX_CALL* list_contents)(
        const PluginxxHost*               host,
        PluginxxOperatorCallback          cb,
        void*                             cb_ud,
        PluginxxString*                   error_out
    );

    /// 要求主程序用当前变量再生成一次 (不受"槽位没被挂载"限制)
    ///
    /// - `slot`: 槽位 id (简称或全名);
    /// - `ids_json`: 自己的内容短 id 数组; 空串 / `[]` = 这个槽位上本插件的全部内容。
    /// 结果经 `musicxx.ui.content` 事件交给主程序。
    PluginxxOperatorHandle*(PLUGINXX_CALL* update_slot)(
        const PluginxxHost*               host,
        const PluginxxStringView*         slot,
        const PluginxxStringView*         ids_json,
        PluginxxOperatorCallback          cb,
        void*                             cb_ud,
        PluginxxString*                   error_out
    );

    /* ---- 插件声明的槽位 ---- */

    /// 声明一个槽位 (id 必须是 plugin.<自己>.ui.<名字>; kind 只允许 insert / replace)
    PluginxxOperatorHandle*(PLUGINXX_CALL* define_slot)(
        const PluginxxHost*               host,
        const MusicxxPluginUISlotSpec*    spec,
        PluginxxOperatorCallback          cb,
        void*                             cb_ud,
        PluginxxString*                   error_out
    );

    /// 撤销自己声明的槽位 (挂在上面的内容一起失效)
    PluginxxOperatorHandle*(PLUGINXX_CALL* undefine_slot)(
        const PluginxxHost*               host,
        const PluginxxStringView*         slot,
        PluginxxOperatorCallback          cb,
        void*                             cb_ud,
        PluginxxString*                   error_out
    );

    /// 列出槽位表: 应用预留的 + 插件声明的 (结果 JSON 数组)
    PluginxxOperatorHandle*(PLUGINXX_CALL* list_slots)(
        const PluginxxHost*               host,
        PluginxxOperatorCallback          cb,
        void*                             cb_ud,
        PluginxxString*                   error_out
    );

    /* ---- 提示 ---- */

    /// 提示 (等价于能力 `musicxx.ui.toast`; fire-and-forget, 不等待用户界面)
    PluginxxOperatorHandle*(PLUGINXX_CALL* notify)(
        const PluginxxHost*               host,
        const PluginxxStringView*         message_json,
        PluginxxOperatorCallback          cb,
        void*                             cb_ud,
        PluginxxString*                   error_out
    );
} MusicxxPluginUIIface;

/* ==================== 接口表: musicxx.kind (插件形态 / 运行器) ==================== */

/// 插件框架自己只实现**动态库插件**与内置 JS 运行器；其它形态（以后的 Python 等）由
/// 插件登记：声明"本插件提供 kind=X"，并交出一对能力（load / unload）。插件框架在扫描、
/// 判定与加载 `kind: X` 的插件时都查这张表：
///
/// - **判定**：没有登记的 `kind` 判为"当前跑不了"（用户会看到"先装运行器插件"）；
/// - **每个形态有自己的接口代次对**（`interfaceVersion` / `minInterfaceVersion`）：清单的
///   `api_version` 按它判（`>= 下限`）；内置 `native` 是 C 接口那一对（7/7），内置 `js` 是
///   JS 接口那一对（3/3：删接口后下限一起抬）；
/// - **校验**：可选的 `validateCapability` —— 加载前（以及主程序安装预检时）先问它
///   "这个目录能不能作为这个形态跑"，它比框架更清楚这种插件长什么样；
/// - **加载 / 卸载**：插件框架调登记时给的能力，`{"ok":true}` 表示接手成功。
///
/// 登记的形态跟着插件生命周期撤销（停用 / 卸载 / 关闭『拟声++』），不写磁盘。内置形态
/// `native` / `js` 不能被覆盖。
#define MUSICXX_PLUGIN_IFACE_KIND             "musicxx.kind"
#ifdef __cplusplus
static_assert(std::string_view(PLUGINXX_APP_ID("kind")) == MUSICXX_PLUGIN_IFACE_KIND);
#endif
#define MUSICXX_PLUGIN_IFACE_KIND_VERSION     2
#define MUSICXX_PLUGIN_IFACE_KIND_MIN_VERSION 2
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为第一个追加成员的偏移) */
#define MUSICXX_PLUGIN_IFACE_KIND_BASELINE_SIZE sizeof(MusicxxPluginKindIface)

typedef struct MusicxxPluginKindIface {
    int32_t  version;     ///< 主程序填 MUSICXX_PLUGIN_IFACE_KIND_VERSION
    uint32_t struct_size;

    /// 登记一种插件形态（本插件成为它的运行器）
    ///
    /// `spec_json`：
    /// ```json
    /// {"kind":"python","requiresPluginAbi":false,
    ///  "interfaceVersion":3,"minInterfaceVersion":1,
    ///  "loadCapability":"plugin.my_runtime.kind_load",
    ///  "unloadCapability":"plugin.my_runtime.kind_unload",
    ///  "validateCapability":"plugin.my_runtime.kind_validate",
    ///  "describe":"Python 运行器"}
    /// ```
    /// - `kind`：清单里写的形态名（小写字母、数字、`_` `-` `.`，1~32 字）；
    /// - `requiresPluginAbi`：该形态的插件是否按 C ABI 编译（缺省 false）。写 true 表示
    ///   它们会调通用接口表，接口代次对固定用框架的 C 接口那一对（写了自己也不采信）；
    /// - `interfaceVersion` / `minInterfaceVersion`：解释执行的形态自己的**接口代次对**
    ///   （当前代次 / 兼容下限）。清单 `api_version` 的判定是 `>= minInterfaceVersion`；
    ///   插件读 `config().apiVersion` 拿到的是 `interfaceVersion`。只增不改时只抬当前代次，
    ///   破坏性变更才抬下限；都不写 = 不判（内置 `js` 是 4 / 4）；
    /// - `loadCapability` / `unloadCapability`：**必须写在本插件的命名空间**
    ///   （`plugin.<插件id>.`）下；缺 `loadCapability` 会被拒绝；
    /// - `validateCapability`（可选，同样要在本插件命名空间下）：加载一个该形态的插件之前
    ///   先问它一次，载荷 `{"action":"validate","kind","id","path","version"}`，回答
    ///   `{"ok":true}` = 可以跑，`{"ok":false,"error":"..."}` = 拒绝（理由直接给用户）；
    /// - `overrideBuiltin`：写 true 表示**接管框架自带的形态**（目前只允许 `js`）。
    ///
    /// 完成时: 成功无结果正文; 失败正文是原因（`arg` / `state` / `taken` / `namespace`）。
    PluginxxOperatorHandle*(PLUGINXX_CALL* register_kind)(
        const PluginxxHost*       host,
        const PluginxxStringView* spec_json,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    );

    /// 撤销本插件登记的形态（不是自己登记的按 `not_owner` 失败）
    PluginxxOperatorHandle*(PLUGINXX_CALL* unregister_kind)(
        const PluginxxHost*       host,
        const PluginxxStringView* kind,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    );

    /// 列出全部形态（内置 + 插件登记），结果 JSON 数组：
    /// `[{"kind","builtin","requiresPluginAbi","interfaceVersion","minInterfaceVersion",
    ///    "provider","providerId","loadCapability","unloadCapability","validateCapability",
    ///    "describe","overridden","available"}]`
    PluginxxOperatorHandle*(PLUGINXX_CALL* list_kinds)(
        const PluginxxHost*       host,
        PluginxxOperatorCallback  cb,
        void*                     cb_ud,
        PluginxxString*           error_out
    );
} MusicxxPluginKindIface;

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
