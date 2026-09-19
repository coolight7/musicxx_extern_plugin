/// 宿主内部 JSON 小工具 (utilxx_base::Json 的安全解析包装)
///
/// 为什么需要: `utilxx_base::Json::parse` 对非法输入抛异常 (没有 nlohmann 的
/// `parse(..., allow_exceptions=false)` 重载); C ABI 边界不希望异常穿越, 因此统一
/// 走这里的包装, 由调用方按 `ok` 判断输入是否合法。
#pragma once

#include "utilxx_base/json.h"

#include <string>
#include <string_view>

namespace musicxx {
namespace extern_plugin {

using utilxx_base::Json;

/// 安全解析 JSON 文本
/// - `ok` 非空时写出"输入是否为合法 JSON"; 失败时返回空对象
/// - 空输入按合法空对象处理 (调用方语义: 缺省参数)
inline Json parseJsonSafe(std::string_view text, bool* ok = nullptr) {
    if (ok) {
        *ok = false;
    }
    if (text.empty()) {
        if (ok) {
            *ok = true;
        }
        return Json::object();
    }
    try {
        auto parsed = utilxx_base::Json::parse(text);
        if (ok) {
            *ok = true;
        }
        return parsed;
    } catch (...) {
        return Json::object();
    }
}

} // namespace extern_plugin
} // namespace musicxx
