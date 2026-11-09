/// musicxx 外部插件原生插件框架 (musicxx_extern_plugin host)
///
/// 结构:
/// - [MusicxxHostInstance]: 继承插件框架 `pluginxx::PluginInstanceBase`, 提供
/// `musicxx_plugin_destroy`;
/// - [MusicxxHostManager]: 继承插件框架 `PluginHostLifecycle`
/// (加载/启停/禁用启用/卸载/级联依赖)
///   与 `DomainHooks` (通用表取数), 并实现 musicxx 专用表 (`musicxx.capabilities` /
///   `musicxx.host`);
/// - [HostContext]: 框架线程 (asio::io_context + 1 条线程)
/// 与工作线程池的所有者。
///
/// 线程模型: 全部动态库插件代码在**唯一**框架线程上顺序执行; Dart
/// 线程只在 同步 FFI 调用期间被借用, 并有等待上界; 插件不得阻塞框架线程
/// (慢操作走 offload)。
#pragma once

#include "musicxx_extern_plugin_api.h"

#include "host_kind.h"
#include "host_manifest.h"
#include "musicxx_capability_spec.h"
#include "musicxx/plugin/api/plugin_api.h"
#include "pluginxx/api/entry.h"
#include "pluginxx/api/net.h"
#include "pluginxx/host/lifecycle.h"
#include "pluginxx/host/tables_impl.h"
#include "utilxx/plugin_net_hub.h"

#include <asio/awaitable.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/thread_pool.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace musicxx {
namespace extern_plugin {

/// JS 运行时 (定义在 js/js_engine.h)
class JsEngine;

/// 插件网络能力 (实现在 cxx_utilxx 的 `utilxx/plugin_net_hub.h`)
using PluginNetHub = utilxx::PluginNetHub;

/// `musicxx.ui` 表的 C ABI 实现 (定义在 musicxx_host_ui.cpp; 供 query_interface
/// 分发)
const void *uiIfaceForQuery();

/// `musicxx.net` 表的 C ABI 实现 (定义在 musicxx_host.cpp; 供
/// query_interface 分发)
const void *netIfaceForQuery();

/* ==================== 有界等待槽 ==================== */

/// 框架线程完成工作后唤醒等待方 (调用方线程等待有上界)
template <typename T> struct WaitSlot {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  T value{};

  void set(T v) {
    {
      std::lock_guard<std::mutex> lock{mutex};
      value = std::move(v);
      done = true;
    }
    cv.notify_all();
  }

  /// 等待完成; 超时返回 false (槽由投递的闭包持有, 超时后仍可安全写入)
  bool wait(uint32_t timeoutMs, T &out) {
    std::unique_lock<std::mutex> lock{mutex};
    if (!cv.wait_for(lock, std::chrono::milliseconds{timeoutMs},
                     [this] { return done; })) {
      return false;
    }
    out = value;
    return true;
  }

  /// 无上界等待 (插件处理器/脚本/能力没有等待超时时用)
  ///
  /// 只有"调用方自己决定不做超时"时使用: 被等待的一侧不返回时, 调用线程会一直停留
  /// 在这里 (同步 FFI 场景下就是整个 Dart 线程停下), 因此调用方要清楚这一后果。
  void wait(T &out) {
    std::unique_lock<std::mutex> lock{mutex};
    cv.wait(lock, [this] { return done; });
    out = value;
  }
};

/* ==================== 插件框架内运行时接驳口 ==================== */

/// 插件框架内运行时 (JS 引擎) 的"任意线程能力调用"接驳口
///
/// 背景 (这样才不会出现互相等待): JS 代码只在共享 JS 线程上执行,
/// 而插件框架 线程派发裁决型能力时会**等待 JS 处理器**。因此 JS
/// 侧发起的调用绝不能"投递到 框架线程再等待" —— 那会和"框架线程正等
/// JS"互锁。JS 引擎自己登记还没回复的请求 (任意线程 可读写的表), 只把事件推给 Dart;
/// Dart 的回复经 [MusicxxHostManager::respondAppCapability] 转交到这里。
class InternalAppCapabilityRelay {
public:
  virtual ~InternalAppCapabilityRelay() = default;

  /// 回复一条本接驳口登记的请求 (**任意线程**)
  /// - 返回 false 表示该 requestId 不属于本接驳口 (调用方继续按"未找到"处理)
  virtual bool respondAppCapability(int64_t requestId, int32_t status,
                             const std::string &resultJson) = 0;

  /// 取消某实例的全部未完成请求 (**任意线程**; 实例卸载/禁用/插件框架关闭时统一取消)
  virtual void cancelAppCapabilitiesOf(const std::string &instanceName) = 0;
};

/* ==================== 插件实例 ==================== */

/// 框架侧插件实例 (元信息/生命周期入口由插件框架框架补齐)
///
/// 前置声明: 实例持有管理器弱引用 (插件框架 vtable 入口据此解析实例 → 管理器)
class MusicxxHostManager;

class MusicxxHostInstance : public pluginxx::PluginInstanceBase {
public:
  explicit MusicxxHostInstance(std::string instanceName)
      : pluginxx::PluginInstanceBase(std::move(instanceName)) {}

  /// 本框架插件的 destroy 入口符号名 (与 `MUSICXX_PLUGIN_SYMBOL_DESTROY` 一致)
  const char *pluginDestroySymbol() const noexcept override {
    return MUSICXX_PLUGIN_SYMBOL_DESTROY;
  }

  std::string_view logTag() const noexcept override { return "[musicxx_ext] "; }

  /// 插件形态: native | js | builtin
  std::string kind = "native";

  /// 选中的平台/架构分支标签（空 = 用插件根目录的库文件；见 host_target.h）
  std::string targetTag;

  /// 选中的库文件（相对插件目录的路径；供管理页展示）
  std::string targetEntry;

  /// 清单声明的接口段 (插件框架生命周期框架会写入; 本框架 v1 不做接口协商)
  pluginxx::PluginManifestInterfaces interfaces;

  /// 管理器弱引用 (插件框架 vtable 入口据此解析"实例 → 管理器"; 由 createInstance
  /// 设置)
  std::weak_ptr<MusicxxHostManager> manager;

  /// 配置文件路径 (config.json; 框架推导)
  std::string configPath;

  /// 资源与阶段耗时统计 (只观测不限制)
  struct PhaseMs {
    int64_t load = 0;
    int64_t create = 0;
    int64_t start = 0;
    int64_t stop = 0;
    int64_t destroy = 0;
  } phaseMs;

  std::atomic<int64_t> capabilityCalls{0};
  std::atomic<int64_t> capabilityTimeouts{0};
  std::atomic<int64_t> appCapabilityRequests{0};
  std::atomic<int64_t> eventsPublished{0};
  std::atomic<int64_t> errors{0};
  std::atomic<int64_t> selfReportedBytes{0};

  /* ---------- 事件发布速率 (框架线程维护; 统计只观测不限制) ---------- */

  /// 加载完成的 steady 毫秒 (平均速率用)
  int64_t loadedAtMs = 0;
  /// 当前速率窗口起点 (steady 毫秒)
  int64_t eventsWindowStartMs = 0;
  /// 当前窗口内发布的事件数
  int64_t eventsWindowCount = 0;
  /// 最近一个完整窗口 (≥1 秒) 的速率 (空闲时为 0)
  int64_t eventsPerSec = 0;
};

class MusicxxHostManager;

/* ==================== 插件框架上下文 (线程与线程池的所有者) ====================
 */

/// 插件框架上下文: 保证 io_context 先于管理器存在 (管理器构造函数需要它的 executor)
struct HostContext {
  asio::io_context io;
  asio::executor_work_guard<asio::io_context::executor_type> guard;
  std::unique_ptr<asio::thread_pool> workers;
  std::thread thread;
  std::shared_ptr<MusicxxHostManager> manager;

  HostContext();
  ~HostContext();
};

/* ==================== 插件框架管理器 ==================== */

class MusicxxHostManager final
    : public pluginxx::PluginHostLifecycle<MusicxxHostInstance>,
      public pluginxx::DomainHooks,
      public std::enable_shared_from_this<MusicxxHostManager> {
public:
  using Base = pluginxx::PluginHostLifecycle<MusicxxHostInstance>;
  using InstancePtr = std::shared_ptr<MusicxxHostInstance>;

  /// 创建管理器 (不启动线程; HostContext 负责 io_context 生命周期)
  static std::shared_ptr<MusicxxHostManager>
  create(HostContext &ctx, const MusicxxExternPluginHostConfig &cfg,
         std::string &err);

  ~MusicxxHostManager() override;

  /// 启动框架线程 + 加载插件 (幂等)
  int32_t start(std::string &err);

  /// 同步停止 (幂等): 卸载全部插件 → 停线程池 → 停 io 线程
  int32_t stop(uint32_t timeoutMs, std::string &err);

  bool running() const { return running_.load(std::memory_order_acquire); }

  /* ---------- 事件 (原生 → Dart) ---------- */

  void setWake(MusicxxExternPluginWakeFn fn, void *userData);

  /// 批量取事件 (JSON 数组); 无事件返回 0 且 outJson 为空
  int32_t pollEvents(int32_t maxCount, std::string &outJson);

  /// 入队事件 (任意线程安全; 队列有界, 溢出丢最旧并补发 musicxx.host.error)
  void pushEvent(const std::string &type, const std::string &plugin,
                 const std::string &payloadJson);

  void pushHostError(const std::string &code, const std::string &message);

  /* ---------- 插件管理 ---------- */

  int32_t scanPlugins(std::string &outJson, std::string &err);
  int32_t pluginListJson(std::string &outJson);
  int32_t loadPlugin(const std::string &idOrPath,
                     const std::string &optionsJson, bool sync,
                     uint32_t timeoutMs, std::string &err);
  int32_t enablePlugin(const std::string &id, std::string &err);
  int32_t disablePlugin(const std::string &id, std::string &err);
  int32_t unloadPlugin(const std::string &id, uint32_t timeoutMs,
                       std::string &err);
  int32_t setPluginArgs(const std::string &id, const std::string &argsJson,
                        std::string &err);
  int32_t pluginConfigPath(const std::string &id, std::string &out,
                           std::string &err);

  /* ---------- 插件形态 (kind) ---------- */

  /// 形态注册表（内置 `native` / `js` + 运行器插件登记的形态）
  ///
  /// **只在框架线程用**：注册 / 撤销发生在那里，其它线程要读请取
  /// [kindRegistrySnapshot] 的快照。
  const MusicxxPluginKindRegistry &kindRegistry() const { return kindRegistry_; }

  /// 形态注册表的当前快照（任意线程）
  ///
  /// 安装预检（`plugin_inspect`）在调用线程上跑，而注册 / 撤销在框架线程上发生；
  /// 直接读注册表本体是数据竞争（vector 可能在扩容），所以给它一份拷贝。
  MusicxxPluginKindRegistry kindRegistrySnapshot() const;

  /// 登记一种形态（插件经 `musicxx.kind` 表调用；**框架线程**）
  ///
  /// - 形态名不能覆盖内置形态（`native` / `js`）；同名已由别的插件登记也会被拒；
  /// - 登记的 `loadCapability` / `unloadCapability` / `validateCapability` 必须落在本插件的
  ///   命名空间（`plugin.<插件id>.`）下，否则拒绝；
  /// - 跟着插件生命周期撤销（`clearPluginRegistrations`）。
  int32_t registerPluginKind(const std::string &instanceName,
                             const std::string &specJson, std::string &err);
  int32_t unregisterPluginKind(const std::string &instanceName,
                               const std::string &kind, std::string &err);

  /// 全部形态的 JSON（自省 / 管理页 / 命令行共用）
  std::string kindsJson() const;

  /// 调用插件能力 (**Dart → 插件**; 异步, 不占调用线程)
  ///
  /// - **只受理不等待结果**: 受理成功返回 `MUSICXX_EXTERN_PLUGIN_OK` 并写出
  ///   `outRequestId`; 结果经事件 `musicxx.plugin.call.result` 回给调用方
  ///   (载荷 `{requestId, id, method, ok, result|error, ms}`);
  /// - 受理失败 (插件没加载 / 没声明该能力 / 归属不对 / 插件框架没在跑) 直接返回负错误码,
  ///   **不推结果事件**;
  /// - **没有 timeout 参数**: 超时是调用方的事 (它自己不等了, 结果丢弃);
  /// - **受理成功 ⇒ 一定有终局**: 正常结果、执行失败、插件卸载 (`plugin_unloaded`)、
  ///   框架停机 (`host_stopped`) 四条路都会推一条结果事件, 调用方不会一直悬挂;
  /// - 受理判定要在框架线程上做 (实例表与能力表都归它), 因此这里等一个**很短的受理窗口**:
  ///   框架线程正忙于别的插件代码时不无限期占用调用线程, 超过窗口按"未及时受理"失败。
  int32_t pluginCallAsync(const std::string &id, const std::string &method,
                          const std::string &argsJson, int64_t &outRequestId,
                          std::string &err);

  /// 调用插件能力 (**不等待版本**; 任意线程调用, 完成回调在框架线程)
  ///
  /// 给"不能让框架线程停下来等结果"的场景使用 (例如 JS 线程发起的跨插件调用:
  /// 若在那里同步等待框架线程, 而框架线程可能正等待 JS 处理器 → 互锁)。
  /// - 语义与 [pluginCallAsync] 一致 (能力名前缀补齐、归属校验、错误码);
  /// - `done(rc, text)` 在**框架线程**恰好一次; rc == 0 时 text 是结果 JSON,
  /// 否则是错误说明。
  void pluginCallAsync(const std::string &id, const std::string &method,
                       const std::string &argsJson,
                       std::function<void(int32_t, const std::string &)> done);

  /// 受理窗口 (毫秒): 等框架线程判定"这次能不能受理"的上界
  ///
  /// 只是**受理判定** (查实例表与能力表), 不含插件能力本身执行的时间 —— 后者不等待。
  /// 框架线程正忙于别的插件代码时, 超过这个窗口就按"未及时受理"失败, 不占用调用线程。
  static constexpr uint32_t capabilityAcceptWaitMs = 2000;

  /* ---------- 能力派发 ---------- */

  /// 派发一条能力 (实现链)
  ///
  /// `timeoutMs` 保留在签名里（C ABI 冻结）但**不再使用**: 插件框架不设等待超时,
  /// 同步派发会一直等到处理器链结束。
  int32_t dispatchCapability(const std::string &capabilityId, const std::string &inputJson,
                   bool sync, uint32_t timeoutMs, std::string &outJson,
                   std::string &err);

  /// 按阶段派发（`impl` / `before` / `after`；非法阶段按参数错误拒绝）
  ///
  /// - `impl`：与 [dispatchCapability] 完全一样；
  /// - `before`：串行、**正序**，载荷逐步累积（回执 `result.patch` 是累计结果）；
  /// - `after`：串行、**逆序**，值逐步替换（回执 `result.value` 是最终值）。
  int32_t dispatchCapabilityPhase(const std::string &capabilityId, int32_t phase,
                        const std::string &inputJson, bool sync,
                        uint32_t timeoutMs, std::string &outJson,
                        std::string &err);

  int32_t handlerCount(const std::string &capabilityId, int32_t &outCount);

  /// 某条 id 在某个阶段上的处理器数量（阶段没有处理器时为 0）
  int32_t handlerCountPhase(const std::string &capabilityId, int32_t phase,
                         int32_t &outCount);
  int32_t capabilityStatsJson(std::string &outJson);

  /// 某条 id 当前的处理器列表 (自省用)
  ///
  /// 按调用顺序给出 (priority 升序, 同优先级按注册顺序), 每一项只含
  /// `plugin` / `instance` / `kind` / `phase` / `priority` / `ownerTag` —— 不暴露处理器地址、
  /// 载荷与内部状态。outJson 形如 `{"capability":"...","count":2,"handlers":[...]}`。
  /// `phase < 0` = 全部阶段（与旧行为一致）。
  int32_t capabilityHandlersJson(std::string_view capabilityId, std::string &outJson);
  int32_t capabilityHandlersJsonPhase(std::string_view capabilityId, int32_t phase,
                                std::string &outJson);

  /// 某条 id 当前的处理器列表 (JSON 数组字符串)
  ///
  /// 自省 C ABI 与 `musicxx.capability.changed` 事件共用同一份写法: 两处给出的顺序必须一致,
  /// 否则 Dart 侧看到的"排第几"与真实调用顺序对不上。`phase < 0` = 全部阶段。
  std::string capabilityHandlersArrayJson(std::string_view capabilityId,
                                    int32_t phase = -1) const;

  /// 某实例当前注册的能力 id（去重；生命周期事件带上它，Dart 侧据此对齐
  /// "这条能力有没有处理器"——计数只靠 `musicxx.capability.changed` 维护，丢一条就会停在旧值）
  std::vector<std::string> capabilitiesOfInstance(const std::string &instanceName) const;

  /* ---------- 能力调用 (插件 → Dart) ---------- */

  /// 回答一条插件发起的能力调用 (**任意线程**)
  ///
  /// 先交给 JS 引擎接驳口就地回答 (它自己的表带互斥); 不属于它的才转给 IO 线程
  /// 查动态库插件的登记表。顺序固定: 这条回复可能就是由"IO 线程正在等 JS"的
  /// 能力派发触发的, 先转 IO 线程就会互等。
  int32_t respondAppCapability(int64_t requestId, int32_t status,
                        const std::string &resultJson);

  /// 取消未完成的能力调用 (插件卸载/插件框架关闭时统一取消)
  void cancelAppCapability(int64_t requestId);

  /* ---------- 观测与配置 ---------- */

  int32_t debugInfo(std::string &outJson);
  int32_t statsJson(const std::string *scopeJson, std::string &outJson);
  int32_t statsConfig(const std::string &cfgJson, std::string &err);
  int32_t uiSnapshot(std::string &outJson);
  int32_t setConfig(const std::string &cfgJson, std::string &err);
  void applyLanguage(const std::string &lang);
  void logMessage(int32_t level, const std::string &message);

  /// 插件框架 → 广播发布 (主题按命名空间校验; 投递到框架线程执行)
  ///
  /// Dart 侧 (C ABI) 入口: 插件框架自身只能发布官方 `musicxx.*` 主题。
  int32_t broadcastPublish(const std::string &topic,
                           const std::string &payloadJson);

  /// 在框架线程上发布广播 (插件频道的发布路径调用; 调用方保证已在框架线程)
  int32_t publishOnHostThread(const std::string &topic,
                              const std::string &payloadJson);

  /// 事件计数与速率 (框架线程; 只观测不限制): 按主题命名空间归属到插件实例
  void accountEventForTopic(const std::string &topic);

  /* ---------- 专用表实现 (供 tables 调用; 均在框架线程执行) ---------- */

  int32_t registerHandler(MusicxxHostInstance *inst,
                       const MusicxxPluginCapabilitySpec &spec);
  int32_t unimplement(MusicxxHostInstance *inst, std::string_view capabilityId,
                         std::string_view ownerTag);
  /// 用 `musicxx.capabilities` 的登记形态挂一个能力处理器（新入口）
  ///
  /// 只在"id 是本框架认识的能力"时由 [onCapabilityRowAdded] 调用；其余 id 是普通能力，
  /// 由 pluginxx 自己的派发处理。处理器必须是同步的（`spec.sync = 1`）：宿主派发是同步的，
  /// 异步给结果的处理器在这一轮拿不到结果。
  int32_t registerCapabilityHandler(MusicxxHostInstance *inst,
                                 const std::string &capabilityId, int32_t phase,
                                 int32_t priority, const std::string &ownerTag,
                                 PluginxxCapabilityStartFunction start,
                                 PluginxxOperatorCancelFunction cancel,
                                 void *ctx, std::string &error);
  int32_t unregisterCapabilityHandler(MusicxxHostInstance *inst,
                                   std::string_view capabilityId,
                                   std::string_view ownerTag);
  int32_t capabilityInfoJson(std::string_view capabilityId, std::string &outJson);
  /// 框架信息 JSON (应用版本/平台/语言/插件目录; 供插件 `host.info()` 读)
  ///
  /// - [instanceName] 非空时额外给出 `pluginId`: 插件要判断"这次调用是不是本插件自己发起的"
  ///   就得知道自己的 id（功能点糖用它过滤自己发起的调用）。JS 侧有 `musicxx.pluginId`
  ///   这个独立入口，那份缓存不带 `pluginId`。
  /// - `apiVersion` 报的是**调用方自己形态的接口代次**（动态库插件看 C 接口代次，JS 看 JS 接口
  ///   代次，两套编号独立，见 `host_kind.h`）：[kind] 非空时用它，否则按 [instanceName] 查实例。
  ///   JS 引擎给所有 JS 插件共用一份缓存，所以它显式传 `"js"`。
  int32_t hostInfoJson(
      std::string &outJson,
      std::string_view instanceName = std::string_view{},
      std::string_view kind = std::string_view{});
  /// 声明应用的能力（生成表 `capability_ids.g.h`）
  ///
  /// 在 `start()` 里跑一次（已绑定 IO 线程）：声明之后插件的 `invoke_capability`
  /// 才认识官方 id；应用实现的能力由 [onAppCapabilityStart] 转给 Dart。
  void declareAppCapabilities();
  int32_t pluginConfigPathFor(MusicxxHostInstance *inst, std::string &out);
  int32_t requestAppCapability(MusicxxHostInstance *inst, std::string_view action,
                        std::string_view argsJson, uint32_t timeoutMs,
                        const PluginxxOperatorNotify *notify,
                        int64_t *outRequestId);

  /// 框架线程执行器 (投递到框架线程执行用; 动态库插件代码都在这条线程上跑)
  ///
  /// 供插件框架内运行时 (JS 引擎) 把"运行期注册/订阅"等操作投递到框架线程执行 ——
  /// 框架线程独占注册表, 因此这些操作不能直接在其他线程改状态。
  asio::any_io_executor hostExecutor() const;

  /// 当前插件 id (由实例名推导; JS 实例名为 "js:<id>")
  static std::string pluginIdOf(std::string_view instanceName);

  /* ---------- UI 声明式扩展 (均在框架线程执行) ---------- */

  /// 注册/覆盖一个 UI 项
  ///
  /// - `spec.item_id` 可以是短名 (插件框架补 `plugin.<pluginId>.` 前缀)
  /// 或本插件的全名;
  /// - 类型必须是官方 `MUSICXX_PLUGIN_UI_TYPE_*` 之一, data 结构按类型校验;
  /// - 项数不设上限。
  int32_t registerUiEntry(MusicxxHostInstance *inst,
                          const MusicxxPluginUIEntrySpec &spec);

  /// 注销一个 UI 项 (不存在返回 `ERR_NOT_FOUND`)
  int32_t unregisterUiEntry(MusicxxHostInstance *inst, std::string_view itemId);

  /// 更新一个 UI 项的声明式内容
  int32_t updateUiEntry(MusicxxHostInstance *inst, std::string_view itemId,
                        std::string_view dataJson);

  /// 列出某插件已注册的 UI 项 (JSON 数组; `inst` 为空时列出全部)
  int32_t listUiEntries(MusicxxHostInstance *inst, std::string &outJson);

  /// 提示 (fire-and-forget: 调能力 `musicxx.ui.toast`, 结果不回传插件)
  int32_t notifyUi(MusicxxHostInstance *inst, std::string_view messageJson);

  /// UI 项数量不设上限 (早期版本每个插件最多 64 项, 已移除)

  /// 插件网络能力 (`musicxx.net` 表; 插件框架启动时创建)
  std::shared_ptr<PluginNetHub> netHub() const { return netHub_; }

  /// `musicxx.net` 表入口: 同步操作
  int32_t netCall(const std::string &pluginId, const std::string &op,
                  const std::string &argsJson, std::string &outJson);

  /// `musicxx.net` 表入口: 异步操作 (`http.request` / `http.fetch` / `http.download`)
  int32_t netCallAsync(const std::string &pluginId, const std::string &op,
                       const std::string &argsJson,
                       std::function<void(utilxx::NetDoneStatus,
                                          const std::string &)> done,
                       std::string &outJson, int64_t *outRequestId);

  /// `musicxx.net` 表入口: 取消进行中的异步请求
  int32_t netCancel(const std::string &pluginId, int64_t requestId,
                    std::string &outJson);

  /// 按**插件 id 或实例名**解析实例 (JS插件实例名是 `js:<id>`, 与插件 id 不同)
  ///
  /// 插件管理入口 (启停/卸载/能力调用/配置路径) 都接受 Dart 侧传来的插件 id,
  /// 因此统一经这里解析; 动态库插件的实例名恰好等于插件 id。
  std::shared_ptr<MusicxxHostInstance>
  resolveInstance(const std::string &idOrInstance) const;

  /* ---------- 插件框架内运行时 (JS 引擎) ---------- */

  /// 注册插件框架内运行时的能力调用接驳口 (JS 引擎创建时调用)
  void setInternalAppCapabilityRelay(std::shared_ptr<InternalAppCapabilityRelay> relay);

  /// 分配一个请求 id (**任意线程**; 动态库插件与 JS 引擎共用同一计数器,
  /// 避免 id 撞车)
  int64_t allocateRequestId();

  /// 推送"插件 → Dart"的能力调用事件 (**任意线程**; 不做请求登记)
  ///
  /// 供 JS 引擎使用: JS 引擎自己保活 in-flight 状态 (见 [InternalAppCapabilityRelay]),
  /// 插件框架这里只做事件推送 (名字不做校验, 由 Dart 侧按注册表决定理不理它)。
  /// - 返回 0 = 已推送; -1 = 参数非法
  /// - `outEffectiveTimeoutMs` 输出实际生效的超时 (0 = 不设超时, 由调用方/插件自担)
  int32_t pushAppCapabilityRequestEvent(const std::string &pluginId, int64_t requestId,
                                 const std::string &action,
                                 const std::string &argsJson,
                                 uint32_t timeoutMs,
                                 int64_t *outEffectiveTimeoutMs);

  /// 插件发布广播 (**任意线程**; 主题归属校验后再投递到框架线程)
  int32_t broadcastPublishFrom(const std::string &pluginId,
                               const std::string &topic,
                               const std::string &payloadJson);

  /// 取消某实例的全部未完成请求 (**任意线程**): 框架侧登记 + JS 引擎侧登记
  void cancelAppCapabilitiesOfInstance(const std::string &instanceName);

  /// JS 引擎实例 (未启用 JS 运行时或未启动时为空)
  std::shared_ptr<JsEngine> jsEngine() const;

  /// 注册 JS插件的内置槽位并把插件目录登记到 discovered_ (加载前调用)
  ///
  /// - `pluginDir` 为插件目录; 脚本取清单 `scripts`（按顺序执行），没写时取
  /// `entryHint` (以 .js 结尾时) 或 `plugin.js`;
  /// - 返回 0 成功; -4 找不到脚本文件; -2 状态错误 (JS 运行时不可用/安全模式)。
  int32_t prepareJsPlugin(const std::string &pluginId,
                          const std::string &pluginDir,
                          const std::string &entryHint,
                          const std::vector<std::string> &scriptHints,
                          const std::string &version, std::string &err);

  /* ---------- DomainHooks (主题规则对外可见: 插件 broadcasts 表入口要用)
   * ---------- */

  /// 广播 id 的命名空间规则: 只接受官方 `musicxx.*` 与插件 `plugin.<id>.*`
  std::string qualifyEventTopic(std::string_view topic) override;

  /// 实例名 → 插件 id (`js:<id>` → `<id>`; 其余原样), 能力 / 广播的归属都用它
  std::string pluginId(std::string_view instanceName) override;

  /// 按生成表 `capability_ids.g.h` 补一份频道声明 (应用还没下发时的默认做法)
  ///
  /// - 表里的 id: 用声明里的 `cache` / `publish` / `pushOnSubscribe` / `coalesce` / `scope`;
  /// - 表外的 id: 默认属性 (只有事件、属主能发布、允许合并、作用域 `app`);
  /// - `plugin.<id>.*` 的属主是那个插件 (只有它能发布)。
  void ensureBroadcastChannel(const std::string &id);

protected:
  /* ---------- 插件框架生命周期接口实现 ---------- */

  std::shared_ptr<pluginxx::PluginHostLifecycle<MusicxxHostInstance>>
  selfRef() override;
  std::shared_ptr<MusicxxHostInstance>
  createInstance(std::string name) override;
  const PluginxxHostVtable *hostVtable() override;
  pluginxx::PluginEntrySymbols entrySymbols() const override;

  void detachDomainRegistrations(MusicxxHostInstance *inst) override;
  void clearDomainRegistrations(MusicxxHostInstance *inst) override;
  void onInstanceLoaded(MusicxxHostInstance &inst) override;
  void onInstanceUnloaded(MusicxxHostInstance &inst) override;
  void onInstanceEnabledChanged(MusicxxHostInstance &inst,
                                bool enabled) override;

  std::string_view logTag() const noexcept override { return "[musicxx_ext] "; }

  /* ---------- 能力注册路径的挂接（`musicxx.capabilities` 登记落到能力引擎） ---------- */

  /// 一条能力实现行登记好了：是能力 id 就挂进 `handlers_`，其余 id 原样交给 pluginxx 派发
  int onCapabilityRowAdded(MusicxxHostInstance *inst, const std::string &id,
                           const PluginxxCapabilitySpec &spec,
                           std::string *error) override;
  void onCapabilityRowRemoved(MusicxxHostInstance *inst, const std::string &id,
                              std::string_view ownerTag) override;

  /// 官方 id 的声明来源：生成物 `capability_ids.g.h` 就是应用声明的权威副本
  ///
  /// 主程序还没有把声明下发给宿主的通道（阶段 2b 只做到命令面），所以这里按生成表回答，
  /// 让插件可以先用 `musicxx.capabilities` 登记能力；等真的接上下发通道，这个回退就只
  /// 用来兜底。
  bool appCapabilityDecl(std::string_view id,
                         pluginxx::CapabilityDecl *out) override;

  /// 应用实现的能力被调用：转给应用（Dart），结果原样回答
  ///
  /// 应用实现的能力用能力名暴露（`capability_ids.g.h` 里 `runner = app` 那些），它们没有
  /// 插件实现行；`invoke_capability` 因此把调用先交给这里。转发走同一套
  /// `requestAppCapability`（事件 `musicxx.capability.app.request` +
  /// `musicxx_extern_plugin_capability_app_respond`），取消走
  /// `cancelAppCapability`（事件 `musicxx.capability.app.cancel`）。
  void *onAppCapabilityStart(MusicxxHostInstance *inst,
                             const std::string &capability,
                             std::string_view argsJson, int32_t timeoutMs,
                             const PluginxxOperatorNotify *notify,
                             PluginxxString *errorOut) override;
  void onAppCapabilityCancel(MusicxxHostInstance *inst,
                             const std::string &capability,
                             void *handle) override;

  /* ---------- DomainHooks ---------- */

  bool postToWorkerThread(std::function<void()> fn) override;
  std::string configJson() override;
  std::string language() override;
  void setLanguage(std::string_view lang) override;
  std::string pluginsJson() override;
  std::string pluginJson(std::string_view name) override;

private:
  MusicxxHostManager(asio::any_io_executor ex);

  /// 应用框架配置 (create() 里调用; 解析路径/开关)
  void applyConfig(const MusicxxExternPluginHostConfig &cfg);

  /// 记录客户端界面能力段 (JSON 文本; 只校验是不是 JSON 对象, 不解释内容)
  void setUiCapabilities(const std::string &capabilitiesJson);

  /// 在框架线程上绑定 IO 线程标识 (start() 里调用; 失败返回 false)
  bool bindIoThread();

  /// 刷新 JS 引擎的框架信息缓存 (语言/配置变化时; 供 JS 侧同步读)
  void refreshJsHostInfo();

  /// 能力处理器登记项
  struct CapabilityHandler {
    std::string plugin;   ///< 实例名
    std::string pluginId; ///< 插件 id (去掉 js: 前缀)
    std::string ownerTag;
    std::string capabilityId;
    MusicxxPluginCapabilitySpec spec{};
    uint64_t seq = 0;

    /// 参与方式 (`MUSICXX_CAPABILITY_PHASE_*`; 与 `spec.phase` 同值, 方便查询与自省)
    int32_t phase = 0;

    /// 走 `musicxx.capabilities` 表登记的处理器 (插件 SDK 的常规入口)
    ///
    /// 非空时派发直接调它，`spec.run_sync` / `spec.run_start` 都不参与 ——
    /// 两条入口的区别只有处理器签名与"同步还是异步"，派发与合并规则共用同一份。
    PluginxxCapabilityStartFunction rowStart = nullptr;
    PluginxxOperatorCancelFunction rowCancel = nullptr;
    void *rowCtx = nullptr;

    /// 只观测不限制的统计 (耗时/失败次数都只写进管理页, 不参与任何判定)
    int64_t calls = 0;
    int64_t failures = 0;
    int64_t totalUs = 0;
    int64_t maxUs = 0;
  };

  /// 同一个 (实例, ownerTag, 阶段) 覆盖式登记: 先摘掉旧的那一条
  ///
  /// 同一个 ownerTag 可以分别登记 before / impl / after —— 三者互不覆盖。
  static void dropHandlerSlot(std::vector<CapabilityHandler> &handlers,
                              const std::string &plugin,
                              const std::string &ownerTag, int32_t phase);

  /// 未完成的能力调用
  struct PendingAppCapability {
    std::string plugin;
    std::string capability;
    const PluginxxOperatorNotify *notify = nullptr;
    /// 完成通知的**持有者**: 有的调用方 (插件框架的 Operation) 给的通知是临时的,
    /// 只在调用当场有效; 这里存一份, 随这次请求一起活到终结
    std::shared_ptr<PluginxxOperatorNotify> notifyOwner;
    std::chrono::steady_clock::time_point deadline{};
    /// 超时定时器 (只在调用方给了超时时创建: Dart 若一直不回复, 到点终结该 op
    /// 并通知 Dart 收尾)
    std::shared_ptr<asio::steady_timer> timer;
  };

  /// 在框架线程发起能力调用 (**必须已在框架线程调用**; 两个 pluginCallAsync 共用)
  void invokeCapabilityOnHostThread(
      const std::string &id, const std::string &method,
      const std::string &argsJson,
      std::function<void(int32_t, const std::string &)> done);

  /// 受理一次能力调用 (**必须已在框架线程调用**)
  ///
  /// 做三件事: 能力全名补齐 (`method` 缺 `plugin.` 前缀时补 `plugin.<id>.`)、
  /// 目标实例查找、能力声明与归属校验。
  /// 受理失败返回负错误码并给出可读原因 (不发起能力、不推结果事件)。
  int32_t acceptCapabilityCall(const std::string &id,
                               const std::string &method, std::string &cap,
                               InstancePtr &outInst, std::string &problem);

  /// 发起一次已受理的能力调用 (**必须已在框架线程调用**)
  void startCapabilityCall(MusicxxHostInstance *inst, const std::string &cap,
                           const std::string &argsJson,
                           std::function<void(int32_t, const std::string &)>
                               done);

  /// 由运行器插件托管一个插件：调它声明的加载 / 卸载能力并等结果（协程）
  ///
  /// - 返回 `{ok, message}`：`ok` = 运行器回答 `{"ok":true}`（或能力返回 0）；
  /// - `timeoutMs` 到点按失败返回（运行器没回答不会把加载卡死）；
  /// - 只做"转发 + 记账"，**不创建插件框架实例**：托管插件的能力 / UI 都归运行器插件。
  asio::awaitable<std::pair<bool, std::string>> invokeKindProviderAsync(
      const std::string &provider, const std::string &capability,
      const std::string &argsJson, uint32_t timeoutMs);

  /// 由运行器插件托管的插件（形态不是内置形态：运行器负责跑，插件框架只记录与转发）
  struct HostedKindPlugin {
    std::string id;
    std::string kind;
    std::string provider; ///< 运行器插件的实例名（= 插件 id）
    std::string path;     ///< 插件目录（重新加载用）
    std::string version;
    bool enabled = true;
  };

  /// 交给运行器加载一个非内置形态的插件（协程；**框架线程**）
  ///
  /// 先调它声明的校验能力（`validateCapability`，没声明就跳过），通过后再交给加载能力；
  /// 成功时在 `hostedKindPlugins_` 里留一条记录，并推 `musicxx.plugin.loaded`。
  asio::awaitable<bool> loadHostedKindAsync(
      const MusicxxPluginKindSpec &spec, const std::string &pluginId,
      const std::string &path, const MusicxxManifestFields &fields,
      const pluginxx::PluginLoadOptions &options, std::string &failReason);

  /// 让运行器启动 / 停止一个已托管的插件（协程；**框架线程**）
  asio::awaitable<bool> startHostedKindAsync(const HostedKindPlugin &record,
                                             std::string &failReason);
  asio::awaitable<bool> stopHostedKindAsync(const HostedKindPlugin &record);

  /// 让运行器卸载一个托管插件，并删掉记录（**框架线程**；协程）
  asio::awaitable<void> unloadHostedKindAsync(const std::string &pluginId);

  /// 一次 Dart → 插件能力调用的记账 (受理成功到终局之间)
  ///
  /// 作用只有一个: **保证每一次受理都有终局** —— 插件被卸载、框架停机时,
  /// 还没回来的调用要按 `plugin_unloaded` / `host_stopped` 收尾, 调用方不能一直等。
  struct PendingCapabilityCall {
    std::string id;      ///< 目标插件 id (卸载时按它匹配)
    std::string method;
    std::chrono::steady_clock::time_point started{};
  };

  /// 受理成功后记账 (**框架线程**)
  void rememberPendingCall(int64_t requestId, const std::string &id,
                           const std::string &method,
                           std::chrono::steady_clock::time_point started);

  /// 取出并删掉一条记账; 返回 false = 这条已经有终局了 (卸载/停机已经收尾过)
  bool takePendingCall(int64_t requestId);

  /// 推一条能力调用结果事件 `musicxx.plugin.call.result`
  ///
  /// [reason] 非空时表示"不是插件给的结果" (插件卸载 / 框架停机), `error` 用它的说法。
  void pushCapabilityCallResult(int64_t requestId, const std::string &id,
                                const std::string &method, int32_t rc,
                                const std::string &text,
                                std::chrono::steady_clock::time_point started,
                                const std::string &reason = std::string{});

  /// 某个插件身上还没回来的调用按 `plugin_unloaded` 收尾 (**框架线程**)
  void finishPendingCallsOf(const std::string &pluginId);

  /// 全部还没回来的调用按 `host_stopped` 收尾 (**框架线程**)
  void finishAllPendingCalls();

  /// 未完成的 Dart → 插件能力调用 (requestId → 记账); 只在框架线程上改
  std::map<int64_t, PendingCapabilityCall> pendingCalls_;

  /// 能力调用 requestId 分配器 (受理阶段在任意线程取号, 所以要原子)
  std::atomic<int64_t> nextCallId_{1};

  /// 能力派发模式
  ///
  /// - `Sync`：实现链同步派发（Dart 线程等待结果；插件框架不设等待超时）
  /// - `Notify`：广播派发（入队即返回，不等待、不回报结果）
  /// - `AsyncDecide`：实现链异步派发（不占用 Dart 线程，完成后推
  ///   `musicxx.capability.decision.result` 事件）
  enum class DispatchMode {
    Sync,
    Notify,
    AsyncDecide,
  };

  /// 框架线程上执行的派发（按 [mode] 决定是否等待结果、是否回报事件）
  ///
  /// [phase] 决定派发哪一组处理器：`impl` = 实现链（按声明的 merge 合并、可提前结束）、
  /// `before` = 前链（正序、载荷累积）、`after` = 后链（逆序、值替换）。
  std::string runCapabilityHandlers(const std::string &capabilityId, int32_t phase,
                           const std::string &inputJson,
                           DispatchMode mode);

  /// `musicxx.capability.changed` 的载荷（注册/注销共用一份写法；返回 JSON 文本）
  ///
  /// 含 `count`（全部阶段总数）与 `phaseCount`（逐阶段数量）、`handlers`（全阶段列表）。
  std::string capabilityChangedPayloadJson(const std::string &capabilityId,
                                     const char *action,
                                     const std::string &pluginId);

  /// 某条 id 在某个阶段上的处理器数量（只在框架线程读写注册表）
  int32_t countOnPhase(const std::string &capabilityId, int32_t phase) const;
  void clearPluginRegistrations(const std::string &instanceName);
  std::string pluginInstanceJson(const MusicxxHostInstance &inst) const;
  std::string scanDir(const std::string &dir, const std::string &kindHint);
  std::string deriveConfigPath(const std::string &pluginId,
                               const std::string &pluginDir) const;

  /// 加载动态库插件目录：先按多目标布局选出当前系统/架构的分支库文件，再交给插件框架
  /// `loadNativeAsync`（清单读 name/depends/资源声明，依赖检查与目录加载一致）。
  ///
  /// - `failReason` 在失败时给出用户可读原因（分支不匹配 / 库文件缺失 / 依赖未加载）；
  /// - 目录形态之外的路径（直接给库文件）不经过这里，由插件框架按文件加载。
  asio::awaitable<InstancePtr>
  loadNativeDirAsync(const std::string &dirPath,
                     const pluginxx::PluginLoadOptions *options,
                     std::string &failReason);

  HostContext *ctx_ = nullptr;

  /// 插件网络能力 (HTTP / TCP / WebSocket; 自己的网络线程)
  std::shared_ptr<PluginNetHub> netHub_;

  std::atomic<bool> running_{false};
  std::atomic<bool> started_{false};

  /// 插件框架 IO 线程标识是否已在 start() 里绑定 (见 bindIoThread 的说明)
  std::atomic<bool> ioThreadBound_{false};

  // 事件队列 (多生产者: 框架线程/工作线程池/Dart 线程)
  mutable std::mutex eventMutex_;
  std::deque<std::string> events_; ///< 已序列化的事件 JSON
  int64_t eventSeq_ = 0;
  int64_t eventDropped_ = 0;
  size_t eventCapacity_ = 16384;
  MusicxxExternPluginWakeFn wakeFn_ = nullptr;
  void *wakeUser_ = nullptr;

  // 状态广播 (Dart 线程写, 框架线程读)
  /// 频道声明簿记 (避免每个事件都去查一遍注册表; 只增不减)
  std::mutex declaredBroadcastsMutex_;
  std::set<std::string, std::less<>> declaredBroadcasts_;

  // 能力处理器注册表 (仅框架线程)
  std::map<std::string, std::vector<CapabilityHandler>, std::less<>> handlers_;
  uint64_t handlerSeq_ = 0;

  /// 一个已注册的 UI 项 (声明式扩展; 生命周期 = 插件实例)
  struct UiEntry {
    std::string plugin;   ///< 实例名
    std::string pluginId; ///< 插件 id (去掉 js: 前缀)
    std::string id;       ///< 全名 plugin.<pluginId>.<名>
    std::string type;     ///< MUSICXX_PLUGIN_UI_TYPE_*
    std::string dataJson; ///< 声明式内容
    int32_t order = 0;
    uint64_t seq = 0; ///< 注册顺序 (同 order 时稳定排序)
  };

  /// UI 项注册表 (仅框架线程)
  std::map<std::string, UiEntry, std::less<>> uiEntries_;
  uint64_t uiSeq_ = 0;

  /// 摘除某实例的全部 UI 项 (框架线程; 有变化时推送 musicxx.ui.changed)
  void detachUiEntries(const std::string &instanceName);

  /// 序列化 UI 项 (pluginId 为空 = 全部; 按 type/order/seq 排序)
  std::string uiItemsJson(const std::string &pluginId) const;

  // 能力调用 (框架线程)
  std::map<int64_t, PendingAppCapability> pendingAppCapabilities_;
  /// 请求 id 计数器 (动态库插件与 JS 引擎共用; **任意线程**可用)
  std::atomic<int64_t> nextRequestId_{1};

  /// 异步派发的调用 id 计数器（配 `musicxx.capability.decision.result`
  /// 事件；**任意线程**可用）
  std::atomic<int64_t> nextDispatchCallId_{1};

  /// 插件框架内运行时的请求接驳口 (JS 引擎; 见 InternalAppCapabilityRelay)
  std::shared_ptr<InternalAppCapabilityRelay> internalCapabilityRelay_;

  /// JS 运行时 (启用 JS 时在 start() 里创建, stop() 里释放)
  std::shared_ptr<JsEngine> jsEngine_;

  /// 插件形态注册表（内置 native / js；运行器插件可登记别的形态）
  MusicxxPluginKindRegistry kindRegistry_;

  // 配置
  std::string appVersion_ = "0.0.0";
  std::string platform_ = "unknown";
  std::string language_ = "zh-cn";
  std::string userPluginDir_;
  std::string builtinPluginDir_;
  std::string dataDir_;
  std::string logDir_;
  /// 客户端界面能力段 (JSON 文本; 由 Dart 侧给出, 插件框架原样转发到 host.info().ui)
  std::string uiCapabilitiesJson_;
  int32_t logLevel_ = 2;
  int32_t flags_ = 0;
  bool statsEnabled_ = true;

  // 已知插件 (框架线程 + Dart 线程都读; 由 registryMutex_ 保护)
  std::map<std::string, InstancePtr, std::less<>> loaded_;

  // 已发现的插件 (id → 目录)
  std::map<std::string, std::string, std::less<>> discovered_;

  /// 由运行器插件托管的插件 (id → 记录; 由 registryMutex_ 保护)
  std::map<std::string, HostedKindPlugin, std::less<>> hostedKindPlugins_;

  /// 保护 loaded_ / discovered_ 的跨线程读写
  mutable std::mutex registryMutex_;

  // 插件广播注册表由插件框架持有 (PluginHostCore::broadcasts())

  std::chrono::steady_clock::time_point startTime_{};
};

} // namespace extern_plugin
} // namespace musicxx
