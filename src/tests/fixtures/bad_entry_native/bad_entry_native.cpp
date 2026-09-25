/// 测试夹具: 缺少 start/stop 入口符号的动态库 (对应用例 test_entry_symbols)
///
/// 只导出宿主会查找的部分入口: `get_info` / `create` / `destroy`;
/// **故意不导出** `musicxx_plugin_start` 与 `musicxx_plugin_stop`。
///
/// 契约要求 start/stop 成对存在 (create 只构造, start 才是注册事务), 因此宿主内核应在
/// "查找入口符号"阶段直接拒绝装载并给出可读原因 —— 不是超时、不会调用 create,
/// 也不会留下任何注册残留。
///
/// 这里不用 SDK 的导出宏: 宏会一并生成 start/stop, 而本夹具的目的正是"缺这两个符号"。

#include "musicxx/plugin/api/plugin_api.h"

#include "pluginxx/api/abi.h"

namespace {

/// 把 C 字符串字面量变成跨边界只读视图 (C++ 下 PluginxxStringView 是聚合类型)
constexpr PluginxxStringView viewOf(const char* text, uint64_t size) {
    return PluginxxStringView{text, size};
}

#define BAD_ENTRY_VIEW(text) viewOf(text, sizeof(text) - 1)

const PluginxxInfo kBadEntryInfo{
    PLUGINXX_API_VERSION,
    0,
    BAD_ENTRY_VIEW("bad_entry_native"),
    BAD_ENTRY_VIEW("1.0.0"),
    BAD_ENTRY_VIEW("测试夹具: 缺少 start/stop 入口符号; 不应出现在正式发布里"),
};

#undef BAD_ENTRY_VIEW

} // namespace

extern "C" PLUGINXX_EXPORT const PluginxxInfo* PLUGINXX_CALL musicxx_plugin_get_info(void) {
    return &kBadEntryInfo;
}

/// create 入口存在 (否则失败原因是"缺少 create", 而不是本夹具要验证的 start/stop)
extern "C" PLUGINXX_EXPORT int32_t PLUGINXX_CALL
musicxx_plugin_create(const PluginxxHost* /*host*/, void** plugin_ctx) {
    if (!plugin_ctx) {
        return -1;
    }
    // 宿主在找到 start/stop 之前就应当拒绝装载, 因此正常情况下这里不会被调用
    *plugin_ctx = nullptr;
    return 0;
}

extern "C" PLUGINXX_EXPORT void PLUGINXX_CALL musicxx_plugin_destroy(void* /*plugin_ctx*/) {
    // 没有实例需要释放
}

/* 故意缺失:
 * musicxx_plugin_start
 * musicxx_plugin_stop
 */
