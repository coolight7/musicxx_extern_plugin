/// 插件框架事件模型（原生 → Dart）
///
/// 事件结构：`{"seq":N,"ts":ms,"type":"...","plugin":"...","payload":{...}}`
/// - `seq` 单调递增（可据此发现丢事件）；
/// - 未知 `type` 必须**忽略而不报错**（向前兼容）。
library;

/// 事件类型常量（与原生插件框架 `pushEvent` 的字符串一一对应）
abstract final class MusicxxPluginEventType {
  /// 插件框架启动完成（线程/线程池就绪，插件目录已知）
  static const String hostReady = 'musicxx.host.ready';

  /// 插件框架内部错误/事件队列溢出
  static const String hostError = 'musicxx.host.error';

  /// 扫描到插件
  static const String pluginDiscovered = 'musicxx.plugin.discovered';

  /// 插件加载成功
  static const String pluginLoaded = 'musicxx.plugin.loaded';

  /// 插件卸载完成
  static const String pluginUnloaded = 'musicxx.plugin.unloaded';

  /// 插件启用
  static const String pluginEnabled = 'musicxx.plugin.enabled';

  /// 插件禁用
  static const String pluginDisabled = 'musicxx.plugin.disabled';

  /// 插件加载/运行失败
  static const String pluginError = 'musicxx.plugin.error';

  /// 插件运行告警（例如处理器超出硬性上限，仅记录统计）
  static const String pluginWarn = 'musicxx.plugin.warn';

  /// 插件日志
  static const String pluginLog = 'musicxx.plugin.log';

  /// 能力处理器注册表变化（Dart 侧据此维护"是否有处理器"位图）
  static const String capabilityChanged = 'musicxx.capability.changed';

  /// 广播派发的调试回传（受调试开关控制，默认只在 debug 构建回传）
  static const String capabilityObserve = 'musicxx.capability.observe';

  /// 异步派发完成
  static const String capabilityDecisionResult =
      'musicxx.capability.decision.result';

  /// 插件能力调用的结果（受理是异步的：Dart 侧按 `requestId` 回填等待中的 `Future`）
  ///
  /// 载荷 `{requestId, id, method, ok, result|error, ms}`。
  /// **受理成功 ⇒ 一定有终局**：结果、执行失败、插件卸载（`plugin_unloaded`）、
  /// 插件框架停机（`host_stopped`）四条路都会推一条；受理失败只在返回码上体现，不推事件。
  static const String pluginCallResult = 'musicxx.plugin.call.result';

  /// 插件调用应用实现的能力（Dart 侧执行后必须回执）
  static const String appCapabilityRequest = 'musicxx.capability.app.request';

  /// 应用能力请求被取消 / 超时（Dart 侧应中断在做的工作）
  static const String appCapabilityCancel = 'musicxx.capability.app.cancel';

  /// 插件贡献的 UI 项变化
  static const String uiChanged = 'musicxx.ui.changed';

  /// 广播：订阅数变化
  ///
  /// 载荷 `{id, subscribers}`。主程序据此维护"哪些频道有人听"：`cache: none` 的高频事件
  /// 没人听时就不必构造载荷与发布（应用内部订阅由 Dart 注册表自己服务）。
  static const String broadcastSubscribers = 'musicxx.broadcast.subscribers';

  /// JS 控制台输出（受调试开关控制）
  static const String jsConsole = 'musicxx.js.console';

  /// 周期性统计上报（默认关闭）
  static const String statsReport = 'musicxx.stats.report';

  /// 广播发布的调试回传（受调试开关控制）
  static const String eventPublished = 'musicxx.event.published';
}

/// 一条插件框架事件
class MusicxxPluginEvent {
  const MusicxxPluginEvent({
    required this.seq,
    required this.ts,
    required this.type,
    required this.plugin,
    required this.payload,
  });

  /// 事件序号（单调递增）
  final int seq;

  /// 事件产生时刻（Unix 毫秒）
  final int ts;

  /// 事件类型（见 [MusicxxPluginEventType]）
  final String type;

  /// 相关插件 id（插件框架级事件为空串）
  final String plugin;

  /// 载荷（结构由事件类型决定）
  final Map<String, Object?> payload;

  /// 解析一条事件；缺字段用安全默认值（未知事件类型照常构造）
  factory MusicxxPluginEvent.fromJson(Map<String, Object?> json) {
    final payload = json['payload'];
    return MusicxxPluginEvent(
      seq: (json['seq'] as num?)?.toInt() ?? 0,
      ts: (json['ts'] as num?)?.toInt() ?? 0,
      type: json['type'] as String? ?? '',
      plugin: json['plugin'] as String? ?? '',
      payload: payload is Map<String, Object?>
          ? payload
          : (payload is Map<Object?, Object?>
                ? payload.cast<String, Object?>()
                : const <String, Object?>{}),
    );
  }

  /// 载荷里的字符串字段（缺失返回 `null`）
  String? stringOf(String key) {
    final value = payload[key];
    return value is String ? value : null;
  }

  /// 载荷里的布尔字段（缺失返回 `null`）
  bool? boolOf(String key) {
    final value = payload[key];
    return value is bool ? value : null;
  }

  /// 载荷里的整数字段（缺失返回 `null`）
  int? intOf(String key) {
    final value = payload[key];
    return value is num ? value.toInt() : null;
  }

  @override
  String toString() =>
      'MusicxxPluginEvent(#$seq $type ${plugin.isEmpty ? '-' : plugin})';
}
