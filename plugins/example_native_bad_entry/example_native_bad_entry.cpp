/// 对照示例: 缺少 `destroy` 入口符号的动态库 (对应用例 test_entry_symbols)
///
/// 只导出插件框架会查找的部分入口: `get_info` / `create` / `stop`;
/// **故意不导出** `musicxx_plugin_destroy`。
///
/// 约定要求 `create` / `stop` / `destroy` 三个入口都在 (create 就是"构造 + 注册事务"),
/// 因此插件框架应在"查找入口符号"阶段直接拒绝加载并给出可读原因 —— 不是超时、
/// 不会调用 create, 也不会留下任何注册残留。本插件故意加载失败。
///
/// 这里不用 SDK 的导出宏: 宏会一并生成 destroy, 而本示例的目的正是"缺这个符号"。

#include "musicxx/plugin/api/plugin_api.h"

#include "pluginxx/api/abi.h"

namespace {

/// 把 C 字符串字面量变成跨边界只读视图 (C++ 下 PluginxxStringView 是聚合类型)
constexpr PluginxxStringView viewOf(const char* text, uint64_t size) {
    return PluginxxStringView{text, size};
}

#define BAD_ENTRY_VIEW(text) viewOf(text, sizeof(text) - 1)

/// 元信息: 框架在调用 `get_info` 前会先填好 `api_version` / `struct_size`,
/// 插件只写其余字段 (这里全部写一遍, 方便直接看布局)
const PluginxxInfo kBadEntryInfo{
    PLUGINXX_API_VERSION,                          ///< api_version (框架覆盖)
    0,                                             ///< min_api_version
    static_cast<uint32_t>(sizeof(PluginxxInfo)),   ///< struct_size (框架覆盖)
    0,                                             ///< flags
    BAD_ENTRY_VIEW("example_native_bad_entry"),
    BAD_ENTRY_VIEW("2026.1010.1"),
    BAD_ENTRY_VIEW("对照示例: 缺少 destroy 入口符号; 只用于演示与原生测试"),
};

#undef BAD_ENTRY_VIEW

} // namespace

extern "C" PLUGINXX_EXPORT int32_t PLUGINXX_CALL musicxx_plugin_get_info(PluginxxInfo* out) {
    if (!out) {
        return -1;
    }
    *out = kBadEntryInfo;
    return 0;
}

/// create 入口存在 (否则失败原因是"缺少 create", 而不是本夹具要验证的 destroy)
extern "C" PLUGINXX_EXPORT void PLUGINXX_CALL musicxx_plugin_create(
    const PluginxxHost* /*host*/, void** plugin_ctx,
    const PluginxxOperatorNotify* notify, PluginxxString* /*error_out*/) {
    if (plugin_ctx) {
        *plugin_ctx = nullptr;
    }
    // 插件框架在找到 destroy 之前就应当拒绝加载, 因此正常情况下这里不会被调用
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
}

extern "C" PLUGINXX_EXPORT void PLUGINXX_CALL musicxx_plugin_stop(
    void* /*plugin_ctx*/, const PluginxxOperatorNotify* notify,
    PluginxxString* /*error_out*/) {
    if (notify && notify->done) {
        notify->done(notify->host_ud, PLUGINXX_OPERATOR_OK, nullptr);
    }
}

/* 故意缺失:
 * musicxx_plugin_destroy
 */
