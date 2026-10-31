/// musicxx 外部插件原生插件框架: 文本编码小工具（实现）
///
/// 只在这里 include windows.h: 它会把 min / max 定义成宏，放进公共头会破坏各处的
/// std::min / std::max。需要转换的调用方只拿 host_text.h 的声明。
#include "host_text.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <string_view>

namespace musicxx {
namespace extern_plugin {

namespace {

/// 是否是合法 UTF-8（只看结构，不判断码位是否有意义）
bool isValidUtf8Text(std::string_view text) {
  size_t i = 0;
  const size_t n = text.size();
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      ++i;
      continue;
    }
    size_t extra = 0;
    unsigned int code = 0;
    if ((c & 0xE0) == 0xC0) {
      extra = 1;
      code = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      extra = 2;
      code = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      extra = 3;
      code = c & 0x07;
    } else {
      return false;
    }
    if (i + extra >= n) {
      return false;
    }
    for (size_t k = 1; k <= extra; ++k) {
      const unsigned char next = static_cast<unsigned char>(text[i + k]);
      if ((next & 0xC0) != 0x80) {
        return false;
      }
      code = (code << 6) | (next & 0x3F);
    }
    // 过长编码、代理区、超出 Unicode 范围都不是合法 UTF-8
    if ((1 == extra && code < 0x80) || (2 == extra && code < 0x800) ||
        (3 == extra && code < 0x10000) || code > 0x10FFFF ||
        (code >= 0xD800 && code <= 0xDFFF)) {
      return false;
    }
    i += extra + 1;
  }
  return true;
}

} // namespace

std::string osTextToUtf8(const std::string &text) {
  if (text.empty() || isValidUtf8Text(text)) {
    return text;
  }
#if defined(_WIN32)
  const int length = static_cast<int>(text.size());
  const int wideLength =
      ::MultiByteToWideChar(CP_ACP, 0, text.data(), length, nullptr, 0);
  if (wideLength <= 0) {
    return text;
  }
  std::wstring wide(static_cast<size_t>(wideLength), L'\0');
  ::MultiByteToWideChar(CP_ACP, 0, text.data(), length, wide.data(), wideLength);
  const int utf8Length = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                               wideLength, nullptr, 0, nullptr,
                                               nullptr);
  if (utf8Length <= 0) {
    return text;
  }
  std::string out(static_cast<size_t>(utf8Length), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, out.data(),
                        utf8Length, nullptr, nullptr);
  return out;
#else
  // Linux / macOS / Android / iOS 的系统错误文本已经是 UTF-8: 转不了也不该乱改
  return text;
#endif
}

} // namespace extern_plugin
} // namespace musicxx
