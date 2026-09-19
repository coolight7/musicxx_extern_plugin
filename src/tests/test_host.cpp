/// musicxx 外部插件宿主原生测试 (不依赖 Dart)
///
/// 覆盖 (plan §13.1 的首批用例):
/// - test_host_lifecycle: create/start/stop/destroy 幂等;
/// - test_plugin_load: 扫描 + 装载示例插件 + 钩子注册生效;
/// - test_hooks: decision 钩子裁决 (skip) / 观察钩子 / 未命中时零处理器;
/// - test_namespace: 未知钩子注册被拒 (由宿主实现保证, 这里校验已注册集合);
/// - 卸载后钩子注册全部摘除 (无残留)。
///
/// 运行: musicxx_extern_plugin_test <example_native 插件目录>

#include "musicxx_extern_plugin_api.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failed = 0;
int g_checks = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("  [FAIL] %s\n", what);
    } else {
        std::printf("  [ ok ] %s\n", what);
    }
}

MusicxxExternPluginStringView view(const std::string& s) {
    MusicxxExternPluginStringView v{};
    v.data = s.data();
    v.size = s.size();
    return v;
}

/// 指针版视图 (C ABI 入参形态); 单线程测试用, A/B 两个槽位足够一次调用用两个视图
MusicxxExternPluginStringView* viewP(const std::string& s) {
    static MusicxxExternPluginStringView slots[2];
    static int                          next = 0;
    auto&                               v    = slots[next++ % 2];
    v.data                                   = s.data();
    v.size                                   = s.size();
    return &v;
}

MusicxxExternPluginStringView viewC(const char* s) {
    MusicxxExternPluginStringView v{};
    v.data = s;
    v.size = std::strlen(s);
    return v;
}

MusicxxExternPluginStringView* viewCP(const char* s) {
    static MusicxxExternPluginStringView slots[2];
    static int                          next = 0;
    auto&                               v    = slots[next++ % 2];
    v.data                                   = s;
    v.size                                   = std::strlen(s);
    return &v;
}

void freeStr(MusicxxExternPluginString& s) {
    musicxx_extern_plugin_string_free(&s);
}

std::string take(MusicxxExternPluginString& s) {
    std::string out;
    if (s.data) {
        out.assign(s.data, static_cast<size_t>(s.size));
    }
    freeStr(s);
    return out;
}

} // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0); // debug: unbuffered stdout
    const std::string pluginDir = (argc > 1) ? argv[1] : "";
    std::printf("musicxx_extern_plugin native test (pluginDir=%s)\n", pluginDir.c_str());

    check(musicxx_extern_plugin_api_version() == MUSICXX_EXTERN_PLUGIN_API_VERSION, "api_version 匹配");

    MusicxxExternPluginString log{};
    MusicxxExternPluginString version{};
    check(
        musicxx_extern_plugin_version(&version) == MUSICXX_EXTERN_PLUGIN_OK && version.size > 0,
        "version 可读"
    );
    freeStr(version);

    MusicxxExternPluginHostConfig cfg{};
    cfg.struct_size        = sizeof(MusicxxExternPluginHostConfig);
    const std::string appV = "0.0.0-test";
    const std::string plat = "windows";
    const std::string lang = "zh-cn";
    cfg.app_version        = view(appV);
    cfg.platform           = view(plat);
    cfg.language           = view(lang);
    cfg.user_plugin_dir    = view(pluginDir);
    cfg.log_level          = 2;
    cfg.flags              = MUSICXX_EXTERN_PLUGIN_FLAG_DEBUG_OBSERVE_EVENTS;

    auto* host = musicxx_extern_plugin_host_create(&cfg, &log);
    if (!host) {
        std::printf("host_create failed: %s\n", take(log).c_str());
        return 2;
    }
    check(host != nullptr, "host_create");

    check(musicxx_extern_plugin_host_start(host, &log) == MUSICXX_EXTERN_PLUGIN_OK, "host_start");

    // 扫描
    MusicxxExternPluginString scan{};
    const auto scanRc = musicxx_extern_plugin_plugin_scan(host, &scan, &log);
    const std::string scanJson = take(scan);
    check(scanRc == MUSICXX_EXTERN_PLUGIN_OK, "plugin_scan 成功");
    std::printf("  [info] scanJson=%s\n", scanJson.substr(0, 400).c_str());

    // 装载 (同步)
    MusicxxExternPluginString empty{};
    const auto loadRc = musicxx_extern_plugin_plugin_load_sync(
        host,
        viewCP("example_native"),
        viewCP(R"({"enabled":true})"),
        10000,
        &log
    );
    if (loadRc != MUSICXX_EXTERN_PLUGIN_OK) {
        std::printf("  [info] load rc=%d log=%s\n", loadRc, take(log).c_str());
    }
    check(loadRc == MUSICXX_EXTERN_PLUGIN_OK, "plugin_load_sync 成功");

    // 钩子处理器数量
    int32_t count = 0;
    check(
        musicxx_extern_plugin_hook_count(
            host,
            viewCP("musicxx.player.beforePlaySong"),
            &count,
            &log
        )
            == MUSICXX_EXTERN_PLUGIN_OK
            && count == 1,
        "beforePlaySong 处理器数为 1"
    );
    check(
        musicxx_extern_plugin_hook_count(host, viewCP("musicxx.player.completed"), &count, &log)
                == MUSICXX_EXTERN_PLUGIN_OK
            && count == 0,
        "未注册钩子的处理器数为 0"
    );

    // 事件泵: 应能取到 host.ready / plugin.loaded / hook.changed
    {
        MusicxxExternPluginString events{};
        const auto rc = musicxx_extern_plugin_poll_events(host, 200, &events, &log);
        const std::string eventsJson = take(events);
        check(rc == MUSICXX_EXTERN_PLUGIN_OK || rc == MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT, "poll_events 可调用");
        check(
            eventsJson.find("musicxx.plugin.loaded") != std::string::npos
                || eventsJson.find("musicxx.host.ready") != std::string::npos,
            "事件包含 host.ready / plugin.loaded"
        );
    }

    // decision 钩子: 广告曲目 → skip
    {
        MusicxxExternPluginString out{};
        const std::string        payload
            = R"({"sid":"s1","song":{"name":"广告插播 - 测试","artist":"x"},"mode":"normal"})";
        const auto rc = musicxx_extern_plugin_hook_emit(
            host,
            viewCP("musicxx.player.beforePlaySong"),
            viewP(payload),
            MUSICXX_EXTERN_PLUGIN_HOOK_SYNC,
            200,
            &out,
            &log
        );
        const std::string result = take(out);
        check(rc == MUSICXX_EXTERN_PLUGIN_OK, "hook_emit(sync) 返回成功");
        check(result.find("\"skip\"") != std::string::npos, "广告曲目被裁决为 skip");
    }

    // decision 钩子: 普通曲目 → 无裁决
    {
        MusicxxExternPluginString out{};
        const std::string        payload = R"({"sid":"s2","song":{"name":"普通歌曲","artist":"y"}})";
        const auto               rc      = musicxx_extern_plugin_hook_emit(
            host,
            viewCP("musicxx.player.beforePlaySong"),
            viewP(payload),
            MUSICXX_EXTERN_PLUGIN_HOOK_SYNC,
            200,
            &out,
            &log
        );
        const std::string result = take(out);
        check(rc == MUSICXX_EXTERN_PLUGIN_OK, "hook_emit(sync) 普通曲目成功");
        check(result.find("\"skip\"") == std::string::npos, "普通曲目无 skip 裁决");
    }

    // 观察型钩子 (异步派发)
    {
        MusicxxExternPluginString out{};
        const std::string        payload = R"({"sid":"s3","song":{"name":"观察目标"}})";
        const auto               rc      = musicxx_extern_plugin_hook_emit(
            host,
            viewCP("musicxx.song.changed"),
            viewP(payload),
            MUSICXX_EXTERN_PLUGIN_HOOK_ASYNC,
            0,
            &out,
            &log
        );
        freeStr(out);
        check(rc == MUSICXX_EXTERN_PLUGIN_OK, "hook_emit(async) 入队成功");
    }

    // 未知钩子: 处理器数为 0 且派发不报错
    {
        MusicxxExternPluginString out{};
        const auto                rc = musicxx_extern_plugin_hook_emit(
            host,
            viewCP("musicxx.plugin.unknownHook"),
            viewCP("{}"),
            MUSICXX_EXTERN_PLUGIN_HOOK_SYNC,
            50,
            &out,
            &log
        );
        const std::string result = take(out);
        check(rc == MUSICXX_EXTERN_PLUGIN_OK, "未知钩子派发不报错");
        check(result.find("\"handled\":false") != std::string::npos, "未知钩子无处理器");
    }

    // 状态镜像
    {
        const std::string key = "musicxx.state.song";
        const std::string val = R"({"sid":"s1","name":"镜像歌曲"})";
        check(
            musicxx_extern_plugin_state_update(host, viewP(key), viewP(val), &log)
                == MUSICXX_EXTERN_PLUGIN_OK,
            "state_update 成功"
        );
        MusicxxExternPluginString listed{};
        check(
            musicxx_extern_plugin_plugin_list(host, &listed, &log) == MUSICXX_EXTERN_PLUGIN_OK,
            "plugin_list 成功"
        );
        const std::string listJson = take(listed);
        check(listJson.find("example_native") != std::string::npos, "plugin_list 含已装载插件");
    }

    // 统计与调试信息
    {
        MusicxxExternPluginString stats{};
        check(
            musicxx_extern_plugin_stats(host, nullptr, &stats, &log) == MUSICXX_EXTERN_PLUGIN_OK,
            "stats 可读"
        );
        const std::string statsJson = take(stats);
        check(statsJson.find("example_native") != std::string::npos, "stats 含插件条目");

        MusicxxExternPluginString info{};
        check(
            musicxx_extern_plugin_debug_info(host, &info, &log) == MUSICXX_EXTERN_PLUGIN_OK,
            "debug_info 可读"
        );
        freeStr(info);
    }

    // 禁用 → 钩子摘除, 启用 → 重新注册
    {
        check(
            musicxx_extern_plugin_plugin_disable(host, viewCP("example_native"), &log)
                == MUSICXX_EXTERN_PLUGIN_OK,
            "plugin_disable 成功"
        );
        int32_t afterDisable = 0;
        musicxx_extern_plugin_hook_count(host, viewCP("musicxx.player.beforePlaySong"), &afterDisable, &log);
        check(afterDisable == 0, "禁用后钩子处理器被摘除");
    }

    // 卸载
    {
        check(
            musicxx_extern_plugin_plugin_unload(host, viewCP("example_native"), &log)
                == MUSICXX_EXTERN_PLUGIN_OK,
            "plugin_unload 成功"
        );
        int32_t after = 0;
        musicxx_extern_plugin_hook_count(host, viewCP("musicxx.player.beforePlaySong"), &after, &log);
        check(after == 0, "卸载后无钩子残留");
    }

    check(musicxx_extern_plugin_host_stop(host, 5000, &log) == MUSICXX_EXTERN_PLUGIN_OK, "host_stop");
    check(musicxx_extern_plugin_host_stop(host, 1000, &log) == MUSICXX_EXTERN_PLUGIN_OK, "host_stop 幂等");
    musicxx_extern_plugin_host_destroy(host);

    std::printf("\nchecks=%d failed=%d\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
