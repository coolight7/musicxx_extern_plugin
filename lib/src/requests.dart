import 'dart:async';
import 'dart:ffi';

import 'package:ffi/ffi.dart';

import 'bindings_generated.dart';
import 'events.dart';
import 'native_strings.dart';
import 'runtime.dart';

/// 一次提交的结果
///
/// - [ok] 为真时 [data] 是原生给出的数据（JSON 解码后的值；没有数据的操作是 `null`）；
/// - [ok] 为假时 [error] 带原因，**不抛异常**（要不要抛由包层的类型化 API 决定）。
class MusicxxPluginRequestResult {
  const MusicxxPluginRequestResult({
    required this.requestId,
    required this.op,
    required this.ok,
    this.data,
    this.error,
  });

  /// 请求号（原生受理时给出；未受理时为 0）
  final int requestId;

  /// 操作名（`plugin.load` / `broadcast.publish` …，见 C 头 `musicxx.request.result`）
  final String op;

  /// 结果是否成功
  final bool ok;

  /// 结果数据（JSON 解码后；没有数据的操作是 `null`）
  final Object? data;

  /// 失败原因（[ok] 为真时为 `null`）
  final MusicxxPluginRequestError? error;

  /// 把数据当对象读（不是对象时给空映射）
  Map<String, Object?> get dataObject {
    final Object? value = data;
    if (value is Map<String, Object?>) {
      return value;
    }
    if (value is Map<Object?, Object?>) {
      return value.cast<String, Object?>();
    }
    return const <String, Object?>{};
  }

  /// 把数据当数组读（不是数组时给空列表）
  List<Map<String, Object?>> get dataList {
    final Object? value = data;
    if (value is! List) {
      return const <Map<String, Object?>>[];
    }
    return value
        .whereType<Map<Object?, Object?>>()
        .map((Map<Object?, Object?> item) => item.cast<String, Object?>())
        .toList(growable: false);
  }

  @override
  String toString() =>
      'musicxx.request.result#$requestId $op ${ok ? "ok" : "error"}'
      '${ok ? "" : " (${error?.reason}: ${error?.message})"}';
}

/// 一次请求失败的原因
class MusicxxPluginRequestError {
  const MusicxxPluginRequestError({
    required this.code,
    required this.reason,
    required this.message,
  });

  /// 结果码（见 C 头 `pluginxx/api/result.h`）
  final int code;

  /// 结果码名称（`not_found` / `host_stopped` …）
  final String reason;

  /// 人可读的原因
  final String message;

  @override
  String toString() => '$reason (code=$code): $message';
}

/// 请求总线：每次提交换一个 `requestId`，结果由事件 `musicxx.request.result` 交回
///
/// 约定（与 C 头一致）：
/// - 提交成功 = 已受理，之后**恰好一条**结果事件；提交失败 = 未受理，不会有结果；
/// - 受理成功 ⇒ 一定有终局：插件卸载、框架停机、请求被取消都按失败回一条，
///   调用方不会悬挂；
/// - 失败**不抛异常**，放在 [MusicxxPluginRequestResult.error] 里 —— 不关心结果的调用
///   直接 `.ignore()` 也不会产生未处理的异步异常；要异常语气的包层 API（`load` /
///   `scan` / …）自己按 `ok` 抛 [MusicxxPluginApiException]。
class MusicxxPluginRequests {
  MusicxxPluginRequests.internal(this._runtime);

  final MusicxxPluginRuntime _runtime;

  /// 等待结果的事件（`requestId` → 完成器）
  final Map<int, Completer<MusicxxPluginRequestResult>> _pending =
      <int, Completer<MusicxxPluginRequestResult>>{};

  /// 已经发出、还没等到结果的请求数
  int get pendingCount => _pending.length;

  /// 提交一次请求
  ///
  /// [call] 只负责"受理"：返回 0 并写 `outId`。它会在 [MusicxxPluginArena] 上构造入参，
  /// 调用返回后入参内存立即释放。
  ///
  /// [timeout] 只是"调用方不再等了"：到点给一个超时的失败结果，晚到的结果丢弃。
  /// `null` = 一直等到宿主的终局（受理成功⇒一定有终局）。
  Future<MusicxxPluginRequestResult> submit(
    String op,
    int Function(
      MusicxxPluginArena arena,
      Pointer<Int64> outId,
      Pointer<MusicxxExternPluginString> log,
    )
    call, {
    Duration? timeout,
  }) {
    if (!_runtime.hasHost) {
      return Future<MusicxxPluginRequestResult>.value(
        MusicxxPluginRequestResult(
          requestId: 0,
          op: op,
          ok: false,
          error: const MusicxxPluginRequestError(
            code: -2,
            reason: 'invalid_state',
            message: '插件框架未启动',
          ),
        ),
      );
    }
    final MusicxxPluginArena arena = MusicxxPluginArena();
    try {
      final Pointer<Int64> outId = arena.mallocInt64();
      final Pointer<MusicxxExternPluginString> log = arena.outString();
      final int rc = call(arena, outId, log);
      if (rc != 0) {
        return Future<MusicxxPluginRequestResult>.value(
          MusicxxPluginRequestResult(
            requestId: 0,
            op: op,
            ok: false,
            error: MusicxxPluginRequestError(
              code: rc,
              reason: _reasonOf(rc),
              message: _messageOf(log),
            ),
          ),
        );
      }
      final int requestId = outId.value;
      if (requestId <= 0) {
        return Future<MusicxxPluginRequestResult>.value(
          MusicxxPluginRequestResult(
            requestId: 0,
            op: op,
            ok: false,
            error: const MusicxxPluginRequestError(
              code: -2,
              reason: 'invalid_state',
              message: '插件框架没有给出 requestId（无法配对结果）',
            ),
          ),
        );
      }
      final Completer<MusicxxPluginRequestResult> completer =
          Completer<MusicxxPluginRequestResult>();
      _pending[requestId] = completer;
      if (null != timeout) {
        Timer(timeout, () {
          final Completer<MusicxxPluginRequestResult>? pending = _pending
              .remove(requestId);
          if (null == pending || pending.isCompleted) {
            return;
          }
          pending.complete(
            MusicxxPluginRequestResult(
              requestId: requestId,
              op: op,
              ok: false,
              error: const MusicxxPluginRequestError(
                code: -5,
                reason: 'timeout',
                message: '等待插件框架的结果超时（结果晚到丢弃）',
              ),
            ),
          );
        });
      }
      return completer.future;
    } finally {
      arena.dispose();
    }
  }

  /// `musicxx.request.result`：把结果交给等待中的调用点
  void handleResultEvent(MusicxxPluginEvent event) {
    final int? requestId = event.intOf('requestId');
    if (null == requestId) {
      return;
    }
    final Completer<MusicxxPluginRequestResult>? completer = _pending.remove(
      requestId,
    );
    if (null == completer || completer.isCompleted) {
      // 已经收尾过的请求（框架重启后 requestId 空间从头开始）晚到的结果丢弃
      return;
    }
    final bool ok = event.boolOf('ok') ?? false;
    if (ok) {
      completer.complete(
        MusicxxPluginRequestResult(
          requestId: requestId,
          op: event.stringOf('op') ?? '',
          ok: true,
          data: event.payload['data'],
        ),
      );
      return;
    }
    final Object? raw = event.payload['error'];
    final Map<String, Object?> error = raw is Map<String, Object?>
        ? raw
        : (raw is Map<Object?, Object?>
              ? raw.cast<String, Object?>()
              : const <String, Object?>{});
    completer.complete(
      MusicxxPluginRequestResult(
        requestId: requestId,
        op: event.stringOf('op') ?? '',
        ok: false,
        error: MusicxxPluginRequestError(
          code: error['code'] is num ? (error['code']! as num).toInt() : -2,
          reason: error['reason'] is String ? error['reason']! as String : '',
          message: error['message'] is String
              ? error['message']! as String
              : '插件框架没有给出原因',
        ),
      ),
    );
  }

  /// 框架停止：还没等到结果的请求一律按 `host_stopped` 收尾
  void handleDisposed() {
    final List<Completer<MusicxxPluginRequestResult>> pending =
        List<Completer<MusicxxPluginRequestResult>>.of(_pending.values);
    _pending.clear();
    for (final Completer<MusicxxPluginRequestResult> completer in pending) {
      if (completer.isCompleted) {
        continue;
      }
      completer.complete(
        const MusicxxPluginRequestResult(
          requestId: 0,
          op: '',
          ok: false,
          error: MusicxxPluginRequestError(
            code: -2,
            reason: 'host_stopped',
            message: '插件框架已停止，请求未完成',
          ),
        ),
      );
    }
  }

  String _messageOf(Pointer<MusicxxExternPluginString> log) {
    final String message = takeOutString(log, _runtime.bindings);
    return message.isEmpty ? '插件框架未给出原因' : message;
  }

  /// 结果码名称：查原生导出的码表（不在这里另抄一份）
  String _reasonOf(int code) {
    try {
      final Pointer<Char> name = _runtime.bindings
          .musicxx_extern_plugin_result_name(code);
      if (name == nullptr) {
        return '';
      }
      return name.cast<Utf8>().toDartString();
    } catch (_) {
      return '';
    }
  }
}
