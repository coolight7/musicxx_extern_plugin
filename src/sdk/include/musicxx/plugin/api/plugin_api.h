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
#include "pluginxx/api/abi.h"
#include "pluginxx/api/net.h"
#include "pluginxx/api/tables.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ==================== 入口符号名 (框架自定义) ==================== */

#define MUSICXX_PLUGIN_SYMBOL_GET_INFO "musicxx_plugin_get_info"
#define MUSICXX_PLUGIN_SYMBOL_CREATE   "musicxx_plugin_create"
#define MUSICXX_PLUGIN_SYMBOL_START    "musicxx_plugin_start"
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

/* ==================== 能力: 裁决动作 ==================== */

/// 裁决动作 (与 Dart 侧 `MusicxxPluginCapabilityAction` 一一对应)
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
/// `request_action` / `cancel_action`（动作就是能力，走 `musicxx.capabilities`）
#define MUSICXX_PLUGIN_IFACE_HOST_VERSION     2
#define MUSICXX_PLUGIN_IFACE_HOST_MIN_VERSION 2
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为第一个追加成员的偏移) */
#define MUSICXX_PLUGIN_IFACE_HOST_BASELINE_SIZE sizeof(MusicxxPluginHostIface)

typedef struct MusicxxPluginHostIface {
    int32_t  version;     ///< 主程序填 MUSICXX_PLUGIN_IFACE_HOST_VERSION
    uint32_t struct_size;

    /// 框架信息 (主程序版本/平台/语言/插件目录; JSON 对象)
    int32_t(PLUGINXX_CALL* get_info)(const PluginxxHost* host, PluginxxString* out_json);

    /// 本插件配置路径 (插件框架按插件 id 推导; 供插件读写自己的配置)
    int32_t(PLUGINXX_CALL* get_plugin_config_path)(
        const PluginxxHost*               host,
        PluginxxString*                   out_path
    );
} MusicxxPluginHostIface;

/* ==================== 接口表: musicxx.ui (声明式 UI 扩展) ==================== */

#define MUSICXX_PLUGIN_IFACE_UI             "musicxx.ui"
#ifdef __cplusplus
static_assert(std::string_view(PLUGINXX_APP_ID("ui")) == MUSICXX_PLUGIN_IFACE_UI);
#endif
#define MUSICXX_PLUGIN_IFACE_UI_VERSION     1
#define MUSICXX_PLUGIN_IFACE_UI_MIN_VERSION 1
/* 基线: 重置时 = 完整布局; 以后追加成员保持不变 (改为第一个追加成员的偏移) */
#define MUSICXX_PLUGIN_IFACE_UI_BASELINE_SIZE sizeof(MusicxxPluginUIIface)

/* UI 项类型 (官方; 插件注册的项必须取其中之一, 否则注册被拒绝) */
/// 功能主页入口项: data = {title, subtitle?, icon?, action?}
#define MUSICXX_PLUGIN_UI_TYPE_HOME_ENTRY      "musicxx.ui.home.entry"
/// 歌曲菜单项: data = {title, icon?, action?}
#define MUSICXX_PLUGIN_UI_TYPE_SONG_ACTION     "musicxx.ui.song.action"
/// 歌单菜单项: data = {title, icon?, action?}
#define MUSICXX_PLUGIN_UI_TYPE_PLAYLIST_ACTION "musicxx.ui.playlist.action"
/// 「添加歌曲」弹窗里的来源项 (歌曲列表页 [+] → 添加歌曲): data = {title, subtitle?, icon?, action?}
///
/// 点它时主程序会把当前歌单的上下文 (pid / name / songNum) 合进动作参数, 与歌单菜单项一致。
/// 常见写法是把动作指向 `musicxx.ui.openOverlaySheet`, 由主程序打开一层浮层并等用户关掉
/// (浮层内容是插件视图, 参数见 docs/plugin-api.md §3.2.4)。
#define MUSICXX_PLUGIN_UI_TYPE_ADDSONG_ENTRY "musicxx.ui.addsong.entry"
/// 播放页背景样式 (渲染槽位 `player.background`), 由用户在设置里选择后生效:
/// data = {title, depict?, enabled?, shader:{bundle, vertex?, fragment?},
///         args? (成员名 → 值表达式), cover? (封面纹理), image? (插件绑定的图片),
///         speed?, resolutionScale?, maxFps?, animate?, scrim?, foregroundStyle?}
/// 说明: 着色器参数只有 `args` 一种写法 (旧的 `colors` 与扁平写法都已移除);
/// 不声明 `args` 时主程序默认给内置背景的 4 个绘制色 (`icon.themeMapping.0..3`);
/// 声明 `cover` / `image` 时主程序准备纹理并每帧绑定 (见 docs/plugin-shader-bundle.md §5)。
#define MUSICXX_PLUGIN_UI_TYPE_PLAYING_BACKGROUND "musicxx.ui.playing.background"
/// 播放页歌曲图 (渲染槽位 `player.icon`): 接管播放页中间那张歌曲图 / 视频的显示
/// data = {title, depict?, enabled?, mode?: "none"|"shader"|"view", keepSpace?,
///         fill?, shader?:{bundle, vertex?, fragment?}, view?: "<视图id>"|{视图对象},
///         cover?, image?, args?, speed?, resolutionScale?, maxFps?, animate?}
/// 说明: 三种接管方式 —— none = 不显示内容 (keepSpace 决定是否保留占位),
/// shader = 插件着色器绘制 (渲染输入与播放页背景完全一致),
/// view = 插件声明的界面组合; 不写 `mode` 时按字段推断。
#define MUSICXX_PLUGIN_UI_TYPE_PLAYING_ICON "musicxx.ui.playing.icon"
/// 播放页音频可视化样式 (渲染槽位 `player.visualizer`): 与内置『音乐动效』的四种样式
/// 在同一个列表里选, 没安装内置插件时插件样式照样可用
/// data = {title, depict?, enabled?, mode?: "none"|"shader"|"view",
///         shader?:{bundle, vertex?, fragment?}, view?: "<视图id>"|{视图对象},
///         cover?, image?, args?, speed?, resolutionScale?, maxFps?, animate?}
/// 说明: 字段与播放页歌曲图 (`MUSICXX_PLUGIN_UI_TYPE_PLAYING_ICON`) 完全一致:
/// shader = 插件着色器绘制 (渲染输入与背景一致, `musicxx.spectrum.*` 来源现读频谱),
/// view = 插件声明的界面组合, none = 不显示. 渲染区域是播放页的动效区,
/// 没有点击交互 (原来的封面点击 / 拖动切歌 / 迷你歌词要继续可用).
#define MUSICXX_PLUGIN_UI_TYPE_PLAYING_VISUALIZER "musicxx.ui.playing.visualizer"
/// 播放页子页面插入块：十个位置各有一个类型（类型本身就是位置，不再需要 `slot`）
/// data = {view: "<视图id>"|{视图对象}|[块数组], enabled?}
/// 说明: 同一位置可以注册多项, 按 UI 项的 `order` 与注册顺序从上到下排列;
/// 不需要 `title`. 下面十个宏与 docs/plugin-ui.md §1.6 的位置表一一对应.
/// `top` / `bottom` 是页面固定区域（不随列表滚动）；`header` / `footer` 在列表的滚动内容里。

/// 歌词页顶部（列表之上，不随内容滚动）
#define MUSICXX_PLUGIN_UI_TYPE_PLAYING_LYRIC_TOP "musicxx.ui.playing.lyric.top"
/// 歌词页底部（列表之下，不随内容滚动）
#define MUSICXX_PLUGIN_UI_TYPE_PLAYING_LYRIC_BOTTOM "musicxx.ui.playing.lyric.bottom"
/// 歌词列表内顶部（列表的 header，跟随内容滚动）
#define MUSICXX_PLUGIN_UI_TYPE_PLAYING_LYRIC_HEADER "musicxx.ui.playing.lyric.header"
/// 歌词列表内底部（列表的 footer，跟随内容滚动）
#define MUSICXX_PLUGIN_UI_TYPE_PLAYING_LYRIC_FOOTER "musicxx.ui.playing.lyric.footer"
/// 更多信息页内容开头（整页滚动，这一块在内容的最前面）
#define MUSICXX_PLUGIN_UI_TYPE_PLAYING_MOREINFO_HEADER "musicxx.ui.playing.moreinfo.header"
/// 更多信息页内容末尾（整页滚动，这一块在内容的最后面）
#define MUSICXX_PLUGIN_UI_TYPE_PLAYING_MOREINFO_FOOTER "musicxx.ui.playing.moreinfo.footer"
/// 播放列表页顶部（列表之上，不随内容滚动）
#define MUSICXX_PLUGIN_UI_TYPE_PLAYING_SONGLIST_TOP "musicxx.ui.playing.songlist.top"
/// 播放列表页底部（列表之下，不随内容滚动）
#define MUSICXX_PLUGIN_UI_TYPE_PLAYING_SONGLIST_BOTTOM "musicxx.ui.playing.songlist.bottom"
/// 播放列表内顶部（列表的 header，跟随内容滚动）
#define MUSICXX_PLUGIN_UI_TYPE_PLAYING_SONGLIST_HEADER "musicxx.ui.playing.songlist.header"
/// 播放列表内底部（列表的 footer，跟随内容滚动）
#define MUSICXX_PLUGIN_UI_TYPE_PLAYING_SONGLIST_FOOTER "musicxx.ui.playing.songlist.footer"

/// 说明: 入口项与菜单项的 `icon` 写**插件自己带的 svg 路径** (一条相对插件目录的路径,
/// 可省 `.svg` 后缀, 用 `/` 分隔; 例如 `resource/svg/logo.svg`) —— `resource/svg/` 只是
/// 推荐位置, 插件可以按自己的目录结构放。主程序不接受自己的 svg 名字: 路径不合法或找不到文件
/// 就按"没有图标"处理 (入口/菜单退回默认图标, 页面里的 `Icon` 块退回 `glyph`)。
/// 详见 docs/plugin-ui.md §1.5。
/// 说明: 框架**没有**插件设置页类型 —— 插件要渲染设置界面, 就把设置画在自己注册的
/// 页面里 (`ext://<插件id>/<视图id>`, 可从主页入口等入口打开), 框架不管理设置入口。

/* UI 项动作类型 (data.action; 与界面描述层的动作写法一致) */
/// 打开声明式插件页面: {"kind":"route","route":"ext://<插件id>/<视图id>"}
/// (允许指向任意插件: 页面由被跳转插件自己绘制)
#define MUSICXX_PLUGIN_UI_ACTION_ROUTE      "route"
/// 调用本插件自己的能力: {"kind":"dispatch","name":"<短名>","args":{...}}
/// (字符串短写 "动作名" 等价于这一种)
#define MUSICXX_PLUGIN_UI_ACTION_DISPATCH   "dispatch"
/// 调用主程序官方动作: {"kind":"command","name":"musicxx.<域>.<动作>","args":{...}}
#define MUSICXX_PLUGIN_UI_ACTION_COMMAND    "command"
/// 无动作 (纯展示项)
#define MUSICXX_PLUGIN_UI_ACTION_NONE       "none"

/// UI 项注册规格
///
/// `item_id` 可以是本插件的**短名**（插件框架自动补 `plugin.<pluginId>.` 前缀），
/// 也可以是完整全名 `plugin.<pluginId>.<名>`（必须属于本插件命名空间，否则拒绝注册）。
/// `data_json` 是类型相关的声明式内容（JSON 对象）。
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
    int32_t  version;     ///< 主程序填 MUSICXX_PLUGIN_IFACE_UI_VERSION
    uint32_t struct_size;

    /// 注册/覆盖一个 UI 项
    /// - 返回 0 成功; -1 参数非法 (含 data 结构不合法);
    ///   -2 状态错误 (未启动/已禁用); -4 未知类型; -6 命名空间不属于本插件
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

    /// 提示 (等价于动作 `musicxx.ui.toast`; fire-and-forget, 不等待用户界面)
    int32_t(PLUGINXX_CALL* notify)(
        const PluginxxHost*               host,
        const PluginxxStringView*         message_json
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
#define MUSICXX_PLUGIN_IFACE_KIND_VERSION     1
#define MUSICXX_PLUGIN_IFACE_KIND_MIN_VERSION 1
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
    ///   破坏性变更才抬下限；都不写 = 不判（内置 `js` 是 3 / 3）；
    /// - `loadCapability` / `unloadCapability`：**必须写在本插件的命名空间**
    ///   （`plugin.<插件id>.`）下；缺 `loadCapability` 会被拒绝；
    /// - `validateCapability`（可选，同样要在本插件命名空间下）：加载一个该形态的插件之前
    ///   先问它一次，载荷 `{"action":"validate","kind","id","path","version"}`，回答
    ///   `{"ok":true}` = 可以跑，`{"ok":false,"error":"..."}` = 拒绝（理由直接给用户）。
    ///   主程序安装预检也会调它（`plugin_inspect` 的出参里有能力名，见
    ///   `docs/plugin-native-api.md` §4.3）；
    /// - `overrideBuiltin`：写 true 表示**接管框架自带的形态**（目前只允许 `js`：把 JS 运行器
    ///   搬到插件里）。`native` 不能接管（动态库加载与生命周期是插件框架自己的职责）；
    ///   撤销登记后自动回到内置实现。
    ///
    /// 返回 0 成功；-1 参数非法；-2 状态错误；-4 形态已由别的插件提供；-6 命名空间不属于本插件。
    int32_t(PLUGINXX_CALL* register_kind)(
        const PluginxxHost*       host,
        const PluginxxStringView* spec_json
    );

    /// 撤销本插件登记的形态（不是自己登记的返回 -6）
    int32_t(PLUGINXX_CALL* unregister_kind)(
        const PluginxxHost*       host,
        const PluginxxStringView* kind
    );

    /// 列出全部形态（内置 + 插件登记），JSON 数组：
    /// `[{"kind","builtin","requiresPluginAbi","interfaceVersion","minInterfaceVersion",
    ///    "provider","providerId","loadCapability","unloadCapability","validateCapability",
    ///    "describe","overridden","available"}]`
    int32_t(PLUGINXX_CALL* list_kinds)(
        const PluginxxHost* host,
        PluginxxString*     out_json
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
#define MUSICXX_PLUGIN_IFACE_LYRIC     "musicxx.lyric" ///< 歌词（单数域，与动作 / 能力 / 变量一致）
#define MUSICXX_PLUGIN_IFACE_STORAGE   "musicxx.storage"
#define MUSICXX_PLUGIN_IFACE_STATS     "musicxx.stats"
#define MUSICXX_PLUGIN_IFACE_AGENT     "musicxx.agent" ///< 预留: 实现 AI 相关能力时使用

/* ==================== 快照频道的键 ==================== */

/// 官方的 `musicxx.state.*` 快照频道键由生成头给出:
/// `MUSICXX_BROADCAST_STATE_*`（`capability_ids.g.h`，声明源是 `tools/capabilities.def.json`）。
/// 读当前值用广播频道（`musicxx.broadcast.read` / SDK 的 `broadcastJson`）。

#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif /* MUSICXX_PLUGIN_API_H */
