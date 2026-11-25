import 'dart:async';
import 'dart:convert';
import 'dart:ffi';

import 'bindings_generated.dart';
import 'events.dart';
import 'native_strings.dart';
import 'plugin_info.dart';
import 'plugin_inspect.dart';
import 'requests.dart';
import 'runtime.dart';
import 'ui.dart';

/// 插件管理（Dart → 原生插件框架的插件生命周期与查询）
///
/// 全部方法都是**提交式**的：受理时只交换 `requestId`，结果经事件
/// `musicxx.request.result` 回到这里。调用方拿到的是 `Future`，要不要等由调用点决定
/// （命令行与自省里大量"发出去就行"的操作可以 `.ignore()`）。
///
/// 失败一律抛 [MusicxxPluginApiException]（`ok == false` 时），方便业务 `try/catch`。
class MusicxxPluginManager {
  MusicxxPluginManager.internal(this._runtime);

  final MusicxxPluginRuntime _runtime;

  /// 已加载插件缓存（事件到达时重新查询；以插件框架快照为准）
  List<MusicxxPluginInfo> _cache = const <MusicxxPluginInfo>[];
  final Map<String, MusicxxPluginInfo> _byId = <String, MusicxxPluginInfo>{};

  /// 最近一次扫描结果
  List<MusicxxPluginInfo> _scanned = const <MusicxxPluginInfo>[];

  /// 生命周期事件触发的刷新：一次在途 + 结束后补跑一次（事件风暴时不排长队）
  bool _refreshRunning = false;
  bool _refreshQueued = false;

  /// 是否处于安全模式（本次运行不加载外部插件）
  bool get safeMode => _runtime.config?.safeMode ?? false;

  /// 已加载插件（缓存快照；权威结果请 `await` [list]）
  List<MusicxxPluginInfo> get loaded =>
      List<MusicxxPluginInfo>.unmodifiable(_cache);

  /// 最近一次扫描结果
  List<MusicxxPluginInfo> get discovered =>
      List<MusicxxPluginInfo>.unmodifiable(_scanned);

  /// 扫描插件目录（用户目录 + 随包目录）
  ///
  /// 返回全部发现的清单（含 `valid/supported/reason`，管理页据此标注"为什么不能用"）。
  Future<List<MusicxxPluginInfo>> scan() async {
    if (!_runtime.isRunning) {
      return const <MusicxxPluginInfo>[];
    }
    final MusicxxPluginRequestResult result = await _runtime.requests.submit(
      'plugin.scan',
      (
        MusicxxPluginArena arena,
        Pointer<Int64> outId,
        Pointer<MusicxxExternPluginString> log,
      ) => _runtime.bindings.musicxx_extern_plugin_plugin_scan(
        _runtime.host,
        outId,
        log,
      ),
    );
    _throwIfFailed(result, 'plugin_scan');
    final List<MusicxxPluginInfo> list = _parsePluginList(result.data);
    _scanned = list;
    return list;
  }

  /// 检查一个插件目录（**只读**，不加载、不改动任何状态）
  ///
  /// 与 [scan] 同源：原生侧是同一份 `inspectPluginDir`，因此"预检通过、加载却用另一个
  /// 分支"这类分歧不会出现。用途是主程序「从压缩包安装」的预检：把解包出来的临时目录
  /// 交给插件框架判定，装之前就知道"当前系统/架构能不能用、会用哪个分支、库文件在不在"。
  ///
  /// - [dir] 插件目录（绝对路径；压缩包解包后的临时目录同样可以）；
  /// - [os] / [arch] 要判定的目标环境（留空 = 插件框架自己的平台与架构）；
  /// - 目录不存在 / 清单非法时返回的对象里 `valid == false`（不抛异常，看 [error]）；
  /// - 不看运行期开关（JS 运行时是否可用、安全模式、禁用动态库）——那些由 [scan] 叠加；
  /// - 形态（`kind`）的事实也在这里：`kindKnown` / `kindRequiresPluginAbi` /
  ///   `kindProviderId` / `kindValidateCapability`。装之前要不要拦 `api_version`、
  ///   要不要再请运行器校验一遍，都按这几项来判断（不要在调用方写形态名分支）。
  Future<MusicxxPluginInspect_c> inspect(
    String dir, {
    String os = '',
    String arch = '',
  }) async {
    _requireRunning('plugin_inspect');
    final MusicxxPluginRequestResult result = await _runtime.requests.submit(
      'plugin.inspect',
      (
        MusicxxPluginArena arena,
        Pointer<Int64> outId,
        Pointer<MusicxxExternPluginString> log,
      ) => _runtime.bindings.musicxx_extern_plugin_plugin_inspect(
        _runtime.host,
        arena.view(dir),
        arena.view(os),
        arena.view(arch),
        outId,
        log,
      ),
    );
    _throwIfFailed(result, 'plugin_inspect');
    return MusicxxPluginInspect_c.fromJson(result.dataObject);
  }

  /// 当前已加载插件状态快照
  Future<List<MusicxxPluginInfo>> list() async {
    if (!_runtime.isRunning) {
      return const <MusicxxPluginInfo>[];
    }
    final MusicxxPluginRequestResult result = await _runtime.requests.submit(
      'plugin.list',
      (
        MusicxxPluginArena arena,
        Pointer<Int64> outId,
        Pointer<MusicxxExternPluginString> log,
      ) => _runtime.bindings.musicxx_extern_plugin_plugin_list(
        _runtime.host,
        outId,
        log,
      ),
    );
    _throwIfFailed(result, 'plugin_list');
    final List<MusicxxPluginInfo> list = _parsePluginList(result.data);
    _cache = list;
    _byId
      ..clear()
      ..addEntries(
        list.map(
          (MusicxxPluginInfo info) =>
              MapEntry<String, MusicxxPluginInfo>(info.id, info),
        ),
      );
    return list;
  }

  /// 按 id 查已加载插件（缓存）
  MusicxxPluginInfo? findLoaded(String id) => _byId[id];

  /// 加载插件（等这次加载事务真的走完）
  ///
  /// - [idOrPath] 传插件 id（扫描结果的 `id`）或插件目录/库文件路径；
  /// - [args] 作为插件参数下发（插件经 `<应用名>.config` 表的 `get_plugin_args` 读取）；
  /// - 领域事件 `musicxx.plugin.loaded` / `musicxx.plugin.error` 照发；这里的结果是
  ///   **权威完成信号**（事务失败会抛异常）。
  Future<void> load(String idOrPath, {Map<String, Object?>? args}) async {
    _requireRunning('plugin_load');
    final String options = _optionsJson(args);
    final MusicxxPluginRequestResult result = await _runtime.requests.submit(
      'plugin.load',
      (
        MusicxxPluginArena arena,
        Pointer<Int64> outId,
        Pointer<MusicxxExternPluginString> log,
      ) => _runtime.bindings.musicxx_extern_plugin_plugin_load(
        _runtime.host,
        arena.view(idOrPath),
        arena.view(options),
        outId,
        log,
      ),
    );
    _throwIfFailed(result, 'plugin_load');
    await list();
  }

  /// 启用插件（重新执行插件 `start` 事务，插件借此重新注册能力 / 订阅）
  Future<void> enable(String id) async {
    _requireRunning('plugin_enable');
    final MusicxxPluginRequestResult result = await _runtime.requests.submit(
      'plugin.enable',
      (
        MusicxxPluginArena arena,
        Pointer<Int64> outId,
        Pointer<MusicxxExternPluginString> log,
      ) => _runtime.bindings.musicxx_extern_plugin_plugin_enable(
        _runtime.host,
        arena.view(id),
        outId,
        log,
      ),
    );
    _throwIfFailed(result, 'plugin_enable');
    await list();
  }

  /// 禁用插件（摘除注册并调用插件 `stop`；实例保留在内存，可再 [enable]）
  Future<void> disable(String id) async {
    _requireRunning('plugin_disable');
    final MusicxxPluginRequestResult result = await _runtime.requests.submit(
      'plugin.disable',
      (
        MusicxxPluginArena arena,
        Pointer<Int64> outId,
        Pointer<MusicxxExternPluginString> log,
      ) => _runtime.bindings.musicxx_extern_plugin_plugin_disable(
        _runtime.host,
        arena.view(id),
        outId,
        log,
      ),
    );
    _throwIfFailed(result, 'plugin_disable');
    await list();
  }

  /// 卸载插件（释放实例与动态库；插件框架侧卸载是幂等的）
  Future<void> unload(String id) async {
    _requireRunning('plugin_unload');
    final MusicxxPluginRequestResult result = await _runtime.requests.submit(
      'plugin.unload',
      (
        MusicxxPluginArena arena,
        Pointer<Int64> outId,
        Pointer<MusicxxExternPluginString> log,
      ) => _runtime.bindings.musicxx_extern_plugin_plugin_unload(
        _runtime.host,
        arena.view(id),
        outId,
        log,
      ),
    );
    _throwIfFailed(result, 'plugin_unload');
    await list();
  }

  /// 卸载后按当前配置重新加载
  Future<void> reload(String id) async {
    _requireRunning('plugin_reload');
    final MusicxxPluginRequestResult result = await _runtime.requests.submit(
      'plugin.reload',
      (
        MusicxxPluginArena arena,
        Pointer<Int64> outId,
        Pointer<MusicxxExternPluginString> log,
      ) => _runtime.bindings.musicxx_extern_plugin_plugin_reload(
        _runtime.host,
        arena.view(id),
        outId,
        log,
      ),
    );
    _throwIfFailed(result, 'plugin_reload');
    await list();
  }

  /// 更新插件参数（下次 [reload] 生效；由调用方决定何时重载）
  Future<void> setArgs(String id, Map<String, Object?> args) async {
    _requireRunning('plugin_set_args');
    final String json = _optionsJson(args);
    final MusicxxPluginRequestResult result = await _runtime.requests.submit(
      'plugin.setArgs',
      (
        MusicxxPluginArena arena,
        Pointer<Int64> outId,
        Pointer<MusicxxExternPluginString> log,
      ) => _runtime.bindings.musicxx_extern_plugin_plugin_set_args(
        _runtime.host,
        arena.view(id),
        arena.view(json),
        outId,
        log,
      ),
    );
    _throwIfFailed(result, 'plugin_set_args');
  }

  /// 插件配置文件路径（管理页/插件自读配置用）
  Future<String> configPath(String id) async {
    _requireRunning('plugin_get_config_path');
    final MusicxxPluginRequestResult result = await _runtime.requests.submit(
      'plugin.configPath',
      (
        MusicxxPluginArena arena,
        Pointer<Int64> outId,
        Pointer<MusicxxExternPluginString> log,
      ) => _runtime.bindings.musicxx_extern_plugin_plugin_get_config_path(
        _runtime.host,
        arena.view(id),
        outId,
        log,
      ),
    );
    _throwIfFailed(result, 'plugin_get_config_path');
    final Object? data = result.data;
    return data is String ? data : '';
  }

  /// 调用插件能力（Dart → 插件）
  ///
  /// - native/JS 插件统一用 `plugin.<插件id>.<能力名>`（短名由插件框架补齐；
  ///   能力必须在插件 `start` 事务里声明，归属校验由插件框架完成）；
  /// - 受理成功 ⇒ 一定有终局：结果、执行失败、插件卸载（`plugin_unloaded`）、
  ///   插件框架停机（`host_stopped`）都会给这个 `Future` 一个交代，不会悬挂；
  /// - [timeout] 只是"调用方不再等了"：到点抛出超时错误，晚到的结果**丢弃**并记一条日志
  ///   （不是取消，目标插件照它自己的节奏跑完）；`null` = 一直等到插件框架给出终局；
  /// - 结果形态与插件给的 JSON 一致（对象按 Map 解码，其余按值）。
  Future<Object?> call(
    String id,
    String method, [
    Map<String, Object?> args = const <String, Object?>{},
    Duration? timeout = const Duration(seconds: 30),
  ]) async {
    _requireRunning('plugin_call');
    final String body = _optionsJson(args);
    final MusicxxPluginRequestResult result = await _runtime.requests.submit(
      'plugin.call',
      (
        MusicxxPluginArena arena,
        Pointer<Int64> outId,
        Pointer<MusicxxExternPluginString> log,
      ) => _runtime.bindings.musicxx_extern_plugin_plugin_call(
        _runtime.host,
        arena.view(id),
        arena.view(method),
        arena.view(body),
        outId,
        log,
      ),
      timeout: timeout,
    );
    _throwIfFailed(result, 'plugin_call');
    return result.data;
  }

  /// 资源与耗时统计快照（各阶段耗时 / 能力耗时 / 注册项 / 计数器；只统计不限制）
  Future<Map<String, Object?>> stats({Map<String, Object?>? scope}) async {
    _requireRunning('stats');
    final String body = null == scope || scope.isEmpty ? '' : jsonEncode(scope);
    final MusicxxPluginRequestResult result = await _runtime.requests.submit(
      'stats',
      (
        MusicxxPluginArena arena,
        Pointer<Int64> outId,
        Pointer<MusicxxExternPluginString> log,
      ) => _runtime.bindings.musicxx_extern_plugin_stats(
        _runtime.host,
        body.isEmpty ? nullptr : arena.view(body),
        outId,
        log,
      ),
    );
    _throwIfFailed(result, 'stats');
    return result.dataObject;
  }

  /// 插件贡献的 UI 项全量快照（声明式 UI 扩展）
  ///
  /// 变更会额外推送 `musicxx.ui.changed` 事件（载荷带该插件的全部项），
  /// 主程序按插件整批替换即可（见 `MusicxxPluginUIItems.replacePlugin`）。
  Future<List<MusicxxPluginUIItem>> uiSnapshot() async {
    _requireRunning('ui_snapshot');
    final MusicxxPluginRequestResult result = await _runtime.requests.submit(
      'ui.snapshot',
      (
        MusicxxPluginArena arena,
        Pointer<Int64> outId,
        Pointer<MusicxxExternPluginString> log,
      ) => _runtime.bindings.musicxx_extern_plugin_ui_snapshot(
        _runtime.host,
        outId,
        log,
      ),
    );
    _throwIfFailed(result, 'ui_snapshot');
    return MusicxxPluginUIItem.parseList(result.data);
  }

  /// 内容注册表快照（插件挂在槽位上的界面 JSON 的生成器）
  ///
  /// 变更会额外推送 `musicxx.ui.contents` 事件（载荷带该插件的全部内容）。
  Future<List<MusicxxPluginUIContent>> uiContents() async {
    _requireRunning('ui_contents');
    final MusicxxPluginRequestResult result = await _runtime.requests.submit(
      'ui.contents',
      (
        MusicxxPluginArena arena,
        Pointer<Int64> outId,
        Pointer<MusicxxExternPluginString> log,
      ) => _runtime.bindings.musicxx_extern_plugin_ui_contents(
        _runtime.host,
        outId,
        log,
      ),
    );
    _throwIfFailed(result, 'ui_contents');
    return MusicxxPluginUIContent.parseList(result.data);
  }

  /// 槽位表快照（应用预留的 + 插件声明的）
  ///
  /// 插件声明 / 撤销槽位时推 `musicxx.ui.slots` 事件。
  Future<List<MusicxxPluginUISlot>> uiSlots() async {
    _requireRunning('ui_slots');
    final MusicxxPluginRequestResult result = await _runtime.requests.submit(
      'ui.slots',
      (
        MusicxxPluginArena arena,
        Pointer<Int64> outId,
        Pointer<MusicxxExternPluginString> log,
      ) => _runtime.bindings.musicxx_extern_plugin_ui_slots(
        _runtime.host,
        outId,
        log,
      ),
    );
    _throwIfFailed(result, 'ui_slots');
    return MusicxxPluginUISlot.parseList(result.data);
  }

  /// 让插件为一份内容生成一次界面（等这次生成的结果）
  ///
  /// - `context` 是声明过的依赖变量 → 当前值（JSON 对象）；
  /// - 结果是那份界面的载荷（`{slot, id, shortId, plugin, rev, json?, data?, error?}`），
  ///   与 `musicxx.ui.content` 事件同形；
  /// - 没有这份内容 / 生成函数报错时抛异常。
  Future<Map<String, Object?>> requestUiContent(
    String contentId,
    Map<String, Object?> context,
  ) async {
    _requireRunning('ui_request');
    final String ctxJson = jsonEncode(context);
    final MusicxxPluginRequestResult result = await _runtime.requests.submit(
      'ui.request',
      (
        MusicxxPluginArena arena,
        Pointer<Int64> outId,
        Pointer<MusicxxExternPluginString> log,
      ) => _runtime.bindings.musicxx_extern_plugin_ui_request(
        _runtime.host,
        arena.view(contentId),
        arena.view(ctxJson),
        outId,
        log,
      ),
    );
    _throwIfFailed(result, 'ui_request');
    return result.dataObject;
  }

  /// 订阅 / 退订一个广播频道的值变化
  ///
  /// 有人订阅时，频道每次发布都会推一条 `musicxx.broadcast.changed`（`{id, value}`）；
  /// 计数归零就停。**订阅成功时插件框架先补发一条当前值**，所以 Dart 侧缓存（
  /// `broadcasts.valueOf`）不需要单独冷读。
  Future<void> watchBroadcast(String id, {bool watch = true}) async {
    _requireRunning('broadcast_watch');
    final MusicxxPluginRequestResult result = await _runtime.requests.submit(
      'broadcast.watch',
      (
        MusicxxPluginArena arena,
        Pointer<Int64> outId,
        Pointer<MusicxxExternPluginString> log,
      ) => _runtime.bindings.musicxx_extern_plugin_broadcast_watch(
        _runtime.host,
        arena.view(id),
        watch ? 1 : 0,
        outId,
        log,
      ),
    );
    _throwIfFailed(result, 'broadcast_watch');
  }

  /// 统计采集配置（`{"enable":false}` 可整体关闭采集）
  Future<void> statsConfig(Map<String, Object?> cfg) async {
    _requireRunning('stats_config');
    final String json = _optionsJson(cfg);
    final MusicxxPluginRequestResult result = await _runtime.requests.submit(
      'stats.config',
      (
        MusicxxPluginArena arena,
        Pointer<Int64> outId,
        Pointer<MusicxxExternPluginString> log,
      ) => _runtime.bindings.musicxx_extern_plugin_stats_config(
        _runtime.host,
        arena.view(json),
        outId,
        log,
      ),
    );
    _throwIfFailed(result, 'stats_config');
  }

  /// 某插件的日志流（来自 `musicxx.plugin.log` 事件）
  Stream<MusicxxPluginLog> logs(String id) => _runtime.events
      .where(
        (MusicxxPluginEvent event) =>
            event.type == MusicxxPluginEventType.pluginLog &&
            event.plugin == id,
      )
      .map(MusicxxPluginLog.fromEvent);

  // ==================== 内部 ====================

  void handleDisposed() {
    _cache = const <MusicxxPluginInfo>[];
    _byId.clear();
    _scanned = const <MusicxxPluginInfo>[];
    _refreshRunning = false;
    _refreshQueued = false;
  }

  /// 插件卸载 / 加载失败：立刻从缓存里摘掉（不等下一次 `plugin.list`）
  void handleLifecycleEvent(MusicxxPluginEvent event) {
    if (!_runtime.isRunning) {
      return;
    }
    final String id = event.plugin;
    if (id.isNotEmpty &&
        (event.type == MusicxxPluginEventType.pluginUnloaded ||
            event.type == MusicxxPluginEventType.pluginError) &&
        _byId.containsKey(id)) {
      _byId.remove(id);
      _cache = _cache
          .where((MusicxxPluginInfo info) => info.id != id)
          .toList(growable: false);
    }
    scheduleRefresh();
  }

  /// 排一次"重新查询已加载插件"
  ///
  /// 生命周期事件会连着来（加载时 discovered → loaded → enabled），一次在途 + 结束后
  /// 补跑一次就够，不必为每条事件排一次提交。
  void scheduleRefresh() {
    if (!_runtime.isRunning) {
      return;
    }
    if (_refreshRunning) {
      _refreshQueued = true;
      return;
    }
    _refreshRunning = true;
    unawaited(
      list()
          .catchError((Object _) => const <MusicxxPluginInfo>[])
          .whenComplete(() {
            _refreshRunning = false;
            if (_refreshQueued) {
              _refreshQueued = false;
              scheduleRefresh();
            }
          }),
    );
  }

  void _requireRunning(String operation) {
    if (!_runtime.isRunning) {
      throw MusicxxPluginApiException(-2, '插件框架未启动', operation: operation);
    }
  }

  void _throwIfFailed(MusicxxPluginRequestResult result, String operation) {
    if (result.ok) {
      return;
    }
    final MusicxxPluginRequestError? error = result.error;
    throw MusicxxPluginApiException(
      error?.code ?? MusicxxPluginResultCode_c.internal,
      (error?.message.isNotEmpty ?? false) ? error!.message : '插件框架未给出原因',
      operation: operation,
    );
  }

  static List<MusicxxPluginInfo> _parsePluginList(Object? data) {
    if (data is! List) {
      return const <MusicxxPluginInfo>[];
    }
    return data
        .whereType<Map<Object?, Object?>>()
        .map(
          (Map<Object?, Object?> item) =>
              MusicxxPluginInfo.fromJson(item.cast<String, Object?>()),
        )
        .toList(growable: false);
  }

  static String _optionsJson(Map<String, Object?>? value) {
    if (value == null || value.isEmpty) {
      return '{}';
    }
    return jsonEncode(value);
  }
}
