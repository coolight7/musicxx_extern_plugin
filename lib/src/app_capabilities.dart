import 'dart:async';
import 'dart:convert';
import 'dart:ffi';

import 'bindings_generated.dart';
import 'events.dart';
import 'native_strings.dart';
import 'runtime.dart';

/// 应用实现的能力的处理器：接收插件参数，返回结果（JSON 可序列化）
typedef MusicxxPluginAppCapabilityHandler =
    Object? Function(MusicxxPluginAppCapabilityInvocation invocation);

/// 一次能力调用（插件 → Dart）
class MusicxxPluginAppCapabilityInvocation {
  const MusicxxPluginAppCapabilityInvocation({
    required this.requestId,
    required this.plugin,
    required this.capability,
    required this.args,
    required this.extendDeadline,
  });

  /// 请求 id（回复时原样带回）
  final int requestId;

  /// 发起插件（可能是 `js:<id>` 的 id 形式）
  final String plugin;

  /// 能力 id（`musicxx.<域>.<名字>`）
  final String capability;

  /// 参数
  final Map<String, Object?> args;

  /// 延长本次请求的超时（长任务用；主程序下限 1 s）—— 需由主程序确认，v1 仅记录
  final void Function(Duration extra) extendDeadline;

  @override
  String toString() =>
      'MusicxxPluginAppCapabilityInvocation($plugin → $capability #$requestId)';
}

/// 应用实现的能力注册表
///
/// 插件通过 `pluginxx.capabilities` 调 `musicxx.*` 的 id，宿主把调用转成一条应用能力请求（
/// 事件 `musicxx.capability.app.request`），Dart 侧在 [register] 注册实现。
/// **未注册的能力会立即收到"未找到"**，避免插件 op 一直挂到超时。
class MusicxxPluginAppCapabilities {
  MusicxxPluginAppCapabilities.internal(this._runtime);

  final MusicxxPluginRuntime _runtime;

  final Map<String, MusicxxPluginAppCapabilityHandler> _handlers =
      <String, MusicxxPluginAppCapabilityHandler>{};

  /// 已注册的能力 id
  Set<String> get registeredCapabilities =>
      Set<String>.unmodifiable(_handlers.keys);

  /// 还没回复的异步能力数量（异步处理器返回 Future 时记一笔）
  ///
  /// 给"兜底轮询要不要跑"用：这些回复还在等结果，事件回传不能漏（见 runtime 的定时器）。
  int get pendingResponses => _pendingResponses;
  int _pendingResponses = 0;

  /// 注册能力实现（同名覆盖）
  void register(String capability, MusicxxPluginAppCapabilityHandler handler) {
    _handlers[capability] = handler;
  }

  /// 注销能力实现
  void unregister(String capability) {
    _handlers.remove(capability);
  }

  /// 主动回复某个请求（一般由内部调用；长任务可先记录 requestId 再异步回复）
  void respond(int requestId, {Object? result, int status = 0, String? error}) {
    if (!_runtime.isRunning) {
      return;
    }
    final String payload = result == null
        ? (error == null ? '' : _errorJson(error))
        : _encode(result);
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = _runtime.bindings
          .musicxx_extern_plugin_capability_app_respond(
            _runtime.host,
            requestId,
            status,
            arena.view(payload),
            log,
          );
      if (rc != 0) {
        _runtime.log(3, 'capability_app_respond(#$requestId) 失败(code=$rc)');
      }
    } finally {
      arena.dispose();
    }
  }

  /// 直接执行一条已注册的应用能力（**主程序 UI 触发**；不经插件 op 通道）
  ///
  /// 用途：插件用声明式 UI 声明 `{"kind":"action","name":"musicxx.player.toggle"}`，
  /// 用户点击后由主程序直接调用该能力的实现（与插件发起调用走同一份实现与权限规则）。
  /// - `pluginId`：调用上下文（权限判定与插件私有存储定位用；可为空）；
  /// - 返回调用结果；能力未注册返回 `null`（调用方可据此提示）。
  Future<Object?> invoke(
    String capability, {
    String pluginId = '',
    Map<String, Object?> args = const <String, Object?>{},
  }) async {
    final MusicxxPluginAppCapabilityHandler? handler = _handlers[capability];
    if (handler == null) {
      return null;
    }
    final MusicxxPluginAppCapabilityInvocation invocation =
        MusicxxPluginAppCapabilityInvocation(
          requestId: 0,
          plugin: pluginId,
          capability: capability,
          args: args,
          extendDeadline: (Duration _) {},
        );
    try {
      final result = handler(invocation);
      if (result is Future) {
        return await result;
      }
      return result;
    } catch (error) {
      return <String, Object?>{'ok': false, 'error': error.toString()};
    }
  }

  // ==================== 内部 ====================

  /// `musicxx.capability.app.request` 事件 → 同步调用 Dart 处理器并回复
  ///
  /// 说明：v1 在事件泵里同步执行处理器（处理器应快速返回；耗时工作请自行异步化并在完成后
  /// 调用 [respond]）。这样插件侧 op 能在第一时间拿到结果，而不是等超时。
  void handleAppCapabilityRequest(MusicxxPluginEvent event) {
    final int requestId = event.intOf('requestId') ?? 0;
    if (requestId == 0) {
      return;
    }
    final String capability = event.stringOf('capability') ?? '';
    final rawArgs = event.payload['args'];
    final Map<String, Object?> args = rawArgs is Map<String, Object?>
        ? rawArgs
        : (rawArgs is Map<Object?, Object?>
              ? rawArgs.cast<String, Object?>()
              : const <String, Object?>{});
    final MusicxxPluginAppCapabilityHandler? handler = _handlers[capability];
    if (handler == null) {
      respond(
        requestId,
        status: -4,
        error: 'app_capability_not_registered: $capability',
      );
      return;
    }
    final MusicxxPluginAppCapabilityInvocation invocation =
        MusicxxPluginAppCapabilityInvocation(
          requestId: requestId,
          plugin: event.plugin,
          capability: capability,
          args: args,
          extendDeadline: (Duration _) {},
        );
    try {
      final result = handler(invocation);
      if (result is Future) {
        // 异步处理器：先说明"已受理"，完成后再由处理器自行 respond（或返回未来值）
        ++_pendingResponses;
        result.then<void>(
          (Object? value) {
            --_pendingResponses;
            respond(requestId, result: value);
          },
          onError: (Object error) {
            --_pendingResponses;
            respond(requestId, status: -99, error: error.toString());
          },
        );
        return;
      }
      respond(requestId, result: result);
    } catch (error) {
      respond(requestId, status: -99, error: error.toString());
    }
  }

  /// `musicxx.capability.app.cancel`（插件取消 / 主程序超时）：通知已注册的处理器
  void handleAppCapabilityCancel(MusicxxPluginEvent event) {
    // v1 只记一条日志；需要中断的处理器的实现可以订阅事件流自行收尾（取消令牌尚未接入）
    _runtime.log(
      3,
      '应用能力请求被取消: #${event.intOf('requestId')} (${event.stringOf('reason') ?? ''})',
    );
  }

  /// 主程序收尾：没回复完的异步调用不再有人接，计数归零
  void handleDisposed() {
    _pendingResponses = 0;
  }

  static String _encode(Object? value) => jsonEncode(value);

  static String _errorJson(String message) =>
      jsonEncode(<String, Object?>{'error': message});
}
