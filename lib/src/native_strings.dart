import 'dart:convert';
import 'dart:ffi';

import 'package:ffi/ffi.dart';

import 'bindings_generated.dart';

/// FFI 字符串与内存工具
///
/// 契约：
/// - 入参用**只读借用视图** [MusicxxExternPluginStringView]，指向调用方内存，
///   调用返回前不得释放；
/// - 出参用**宿主堆字符串** [MusicxxExternPluginString]，必须调用
///   `musicxx_extern_plugin_string_free` 释放（本文件负责这件事）。

/// 一次 FFI 调用用到的原生内存（调用结束后一次性释放）
///
/// 用法：
/// ```dart
/// final arena = MusicxxPluginArena();
/// try {
///   bindings.musicxx_extern_plugin_state_update(host, arena.view(key), arena.view(json), log);
/// } finally {
///   arena.dispose();
/// }
/// ```
class MusicxxPluginArena {
  MusicxxPluginArena();

  final List<Pointer<Uint8>> _blocks = <Pointer<Uint8>>[];
  bool _disposed = false;

  /// 构造只读字符串视图（`null` 或空串 → data=NULL, size=0）
  Pointer<MusicxxExternPluginStringView> view(String? text) {
    final Pointer<MusicxxExternPluginStringView> out =
        malloc<MusicxxExternPluginStringView>();
    _blocks.add(out.cast<Uint8>());
    if (text == null || text.isEmpty) {
      out.ref
        ..data = nullptr
        ..size = 0;
      return out;
    }
    final List<int> bytes = utf8.encode(text);
    final Pointer<Uint8> buf = malloc<Uint8>(bytes.length + 1);
    _blocks.add(buf);
    buf.asTypedList(bytes.length + 1).setRange(0, bytes.length, bytes);
    buf[bytes.length] = 0;
    out.ref
      ..data = buf.cast<Char>()
      ..size = bytes.length;
    return out;
  }

  /// 分配一个宿主堆字符串出参槽（值由调用方/宿主填充，用 [takeOutString] 读取并释放）
  Pointer<MusicxxExternPluginString> outString() {
    final Pointer<MusicxxExternPluginString> out =
        malloc<MusicxxExternPluginString>();
    _blocks.add(out.cast<Uint8>());
    out.ref
      ..data = nullptr
      ..size = 0;
    return out;
  }

  /// 分配一个 int32 出参槽（用于 `hook_count` 等计数型出参）
  Pointer<Int32> mallocInt() {
    final Pointer<Int32> out = malloc<Int32>();
    _blocks.add(out.cast<Uint8>());
    out.value = 0;
    return out;
  }

  void dispose() {
    if (_disposed) {
      return;
    }
    _disposed = true;
    for (final Pointer<Uint8> block in _blocks) {
      malloc.free(block);
    }
    _blocks.clear();
  }
}

/// 读取并释放宿主堆字符串（幂等：释放后槽位清零）
String takeOutString(
  Pointer<MusicxxExternPluginString> out,
  MusicxxExternPluginBindings b,
) {
  final MusicxxExternPluginString value = out.ref;
  final String text = (value.data == nullptr || value.size == 0)
      ? ''
      : value.data.cast<Utf8>().toDartString(length: value.size);
  if (value.data != nullptr) {
    b.musicxx_extern_plugin_string_free(out);
  }
  return text;
}

/// 只读借用视图 → Dart 字符串（不释放内存）
String viewToDartString(Pointer<MusicxxExternPluginStringView> view) {
  final MusicxxExternPluginStringView value = view.ref;
  if (value.data == nullptr || value.size == 0) {
    return '';
  }
  return value.data.cast<Utf8>().toDartString(length: value.size);
}

/// 把 JSON 文本解析成对象（解析失败返回 `null`，绝不抛异常）
///
/// 原生侧出参一律是 JSON 文本；某些失败路径会返回空串，调用方按"无数据"处理。
Object? tryDecodeJson(String text) {
  if (text.isEmpty) {
    return null;
  }
  try {
    return jsonDecode(text);
  } on FormatException {
    return null;
  }
}

/// 把 JSON 文本解析成字符串键映射（解析失败/类型不符返回空映射）
Map<String, Object?> decodeJsonObject(String text) {
  final Object? decoded = tryDecodeJson(text);
  return decoded is Map<String, Object?> ? decoded : <String, Object?>{};
}

/// 把 JSON 文本解析成对象数组（解析失败/类型不符返回空列表）
List<Map<String, Object?>> decodeJsonArray(String text) {
  final Object? decoded = tryDecodeJson(text);
  if (decoded is! List) {
    return <Map<String, Object?>>[];
  }
  return decoded
      .whereType<Map<Object?, Object?>>()
      .map((Map<Object?, Object?> item) => item.cast<String, Object?>())
      .toList(growable: false);
}
