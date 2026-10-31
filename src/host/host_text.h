/// musicxx 外部插件原生插件框架: 文本编码小工具
///
/// 为什么需要: 系统与系统库给出的错误文本是**本机 ANSI 编码**（中文 Windows 上是 GBK，
/// 来自 `FormatMessageA`），而插件框架里所有字符串都按 UTF-8 处理（JSON、QuickJS 字符串、
/// 主程序的 Dart 侧）。把 ANSI 字节直接当 UTF-8 塞进事件、日志或插件结果，中文就是乱码。
///
/// 只在需要把"操作系统产出的文本"交给插件 / 主程序时用它；框架自己写的文案本来就是 UTF-8。
#pragma once

#include <string>

namespace musicxx {
namespace extern_plugin {

/// 把一段可能来自操作系统的文本转成 UTF-8
///
/// - 已经是合法 UTF-8（包括纯 ASCII）的文本原样返回，所以对框架自己写的文案调用它也没有副作用；
/// - Windows: 不是合法 UTF-8 时按当前 ANSI 代码页（`CP_ACP`）解码后转成 UTF-8；
/// - Linux / macOS / Android / iOS: 系统错误文本本来就是 UTF-8，原样返回。
std::string osTextToUtf8(const std::string &text);

} // namespace extern_plugin
} // namespace musicxx
