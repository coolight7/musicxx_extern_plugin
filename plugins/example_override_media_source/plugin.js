/// example_override_media_source —— 插件参与音源解析的示例
///
/// 这个插件演示**插件怎么把歌曲里保存的音源字符串换成播放器能打开的地址**。它注册同一个钩子
/// `musicxx.player.source.resolve`，按载荷里的来源类型分流：
///
/// 1. `type = "bili"`（内置 Bili 来源）且开关打开：用载荷 `crudeSrc` 里的 BV 号查 B 站接口，
///    回答"音频直链 + B 站请求头"，也就是**覆盖主程序内置的 Bili 解析**；
/// 2. `pluginType = plugin.<插件 id>.plugin_bili`（本插件声明的音源）：`src` 里写 BV 号，
///    用同一份逻辑解析；
/// 3. `pluginType = plugin.<插件 id>.plugin_proxy_local`（本插件声明的音源）：`src` 里写本地
///    文件路径，由插件框架的 HTTP 服务（`musicxx.net.http.serve` + `http.route`）给播放器
///    一个 `http://127.0.0.1:...` 地址。
///
/// 三条路各自有开关（插件目录里的 `config.json`，设置页里是三个开关），关掉即回退：
/// 1、2 关掉后不接管（主程序自己的解析照旧），3 关掉后该来源解析失败。
///
/// 约束与写法说明见 plugins/example_override_media_source/README.md；钩子与解析的接口说明见
/// 包内 docs/plugin-hooks.md 的 `musicxx.player.source.resolve` 与 docs/plugin-api.md §3.2.6。

/// 随插件分发的界面 kit（清单 scripts 里按顺序加载了基础 kit 与 musicxx 扩展 kit）
const kit = pluginxx.ui.kit;

const PLUGIN_ID = musicxx.pluginId;

/// 本插件声明的音源类型 id（必须落在自己的命名空间里）
const TYPE_PLUGIN_BILI = "plugin." + PLUGIN_ID + ".plugin_bili";
const TYPE_PROXY_LOCAL = "plugin." + PLUGIN_ID + ".plugin_proxy_local";

/// B 站接口与播放 CDN 需要的请求头（CDN 会检查 Referer，缺了会 404）
const BILI_REFERER = "https://www.bilibili.com";
const BILI_UA =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 " +
    "(KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36";

/// 播放器/缓存流会带 Range 请求，B 站 CDN 也要求带 Referer；这三枚一起给
function biliHeaders() {
    return {
        "Referer": BILI_REFERER,
        "Origin": BILI_REFERER,
        "User-Agent": BILI_UA,
    };
}

/// 设置项默认值（用户改过的值保存在插件目录的 config.json 里）
const CONFIG_DEFAULTS = {
    overrideBili: true,
    pluginBiliEnabled: true,
    proxyLocalEnabled: true,
};

/// 当前生效的设置（脚本启动时先按默认值，读到 config.json 后覆盖）
let config = {
    overrideBili: CONFIG_DEFAULTS.overrideBili,
    pluginBiliEnabled: CONFIG_DEFAULTS.pluginBiliEnabled,
    proxyLocalEnabled: CONFIG_DEFAULTS.proxyLocalEnabled,
};
/// 已经读回来的设置项数量（`probe` 用它说明"设置还没读完"）
let configLoaded = 0;

/// 运行计数与最近一次错误（设置页展示，排查用；不写入磁盘）
const stats = {
    biliCalls: 0,
    biliOk: 0,
    pluginBiliCalls: 0,
    proxyCalls: 0,
    lastServedUrl: "",
    lastError: "",
};

/* ==================== 设置读写 ==================== */

/// 读回全部设置（顶层调用；动作是异步的，不能 await）
function refreshConfig() {
    Object.keys(CONFIG_DEFAULTS).forEach(function (key) {
        musicxx.storage.getConfig(key, CONFIG_DEFAULTS[key]).then(function (value) {
            config[key] = (typeof value === "boolean") ? value : CONFIG_DEFAULTS[key];
            configLoaded += 1;
        }, function (err) {
            noteError("读取设置失败(" + key + "): " + err.message);
        });
    });
}

/// 改一个设置：先改脚本里的值（设置页立刻看到），再异步写 config.json
function saveConfig(key, value) {
    if (!Object.prototype.hasOwnProperty.call(CONFIG_DEFAULTS, key)) {
        return;
    }
    config[key] = value;
    musicxx.storage.setConfig(key, value).then(function () {
        return null;
    }, function (err) {
        noteError("写入设置失败(" + key + "): " + err.message);
    });
}

function settingOf(key) {
    return config[key] === true;
}

/// 记一条错误：日志 + 设置页的"最近错误"一行（只保留最后一条，避免页面越写越长）
function noteError(message) {
    const text = String(message || "").slice(0, 240);
    stats.lastError = text;
    musicxx.host.log(3, "example_override_media_source: " + text);
}

/* ==================== 小工具 ==================== */

/// 从任意文本里取 BV 号（区分大小写的是后 10 位，前缀统一成 BV）
function extractBvid(text) {
    if (typeof text !== "string") {
        return "";
    }
    const match = /[Bb][Vv]([0-9A-Za-z]{10})/.exec(text);
    return match ? ("BV" + match[1]) : "";
}

/// 地址里的本地路径 → 可直接交给文件服务的路径
///
/// - `file:///D:/a.mp3` → `D:/a.mp3`（去 `file://`、解百分号编码）
/// - `/D:/a.mp3` → `D:/a.mp3`（Windows 的路径被写成 URI 时前面会多一个斜杠）
/// - 反斜杠保留（C++ 侧两种分隔符都认）
function normalizeLocalPath(raw) {
    let text = String(raw || "").trim();
    if (!text) {
        return "";
    }
    if (/^file:\/\//i.test(text)) {
        text = text.slice(7);
        try {
            text = decodeURIComponent(text);
        } catch (e) {
            // 百分号写得不规范就不解，按原样交给文件服务
        }
    }
    if (/^\/[A-Za-z]:/.test(text)) {
        text = text.slice(1);
    }
    return text;
}

/// 取文件名（地址里可能是 / 或 \）
function basename(path) {
    const text = String(path || "");
    let index = text.lastIndexOf("/");
    const back = text.lastIndexOf("\\");
    if (back > index) {
        index = back;
    }
    return index >= 0 ? text.slice(index + 1) : text;
}

/// 直链里的 deadline（秒，B 站给的有效期）→ 毫秒时间戳；没有就返回 0
function deadlineMsOf(url) {
    const match = /[?&]deadline=(\d+)/.exec(String(url || ""));
    return match ? (Number(match[1]) * 1000) : 0;
}

/// 发一次 B 站接口请求并把 JSON 解出来；失败返回 null（不抛异常，失败等于"这次没给"）
function biliGetJson(url, timeoutMs) {
    return musicxx.net.fetch({
        url: url,
        timeoutMs: timeoutMs,
        headers: {
            "Referer": BILI_REFERER,
            "User-Agent": BILI_UA,
        },
    }).then(function (response) {
        if (!response || response.ok !== true || response.status !== 200) {
            noteError("B 站接口请求失败: " +
                ((response && response.error) ? response.error : ("status=" + ((response && response.status) || 0))));
            return null;
        }
        let body = null;
        try {
            body = JSON.parse(response.body || "");
        } catch (e) {
            noteError("B 站接口返回的不是 JSON");
            return null;
        }
        if (!body || body.code !== 0) {
            noteError("B 站接口返回错误: " + ((body && body.message) ? body.message : ("code=" + ((body && body.code) || 0))));
            return null;
        }
        return body.data || null;
    }, function (err) {
        noteError("B 站接口请求失败: " + err.message);
        return null;
    });
}

/// 音质枚举名（主程序给的）→ B 站音频档位序号（30216 < 30232 < 30280 < 30250 < 30251）
function audioRankOf(id) {
    switch (id) {
        case 30216: return 0;   // 64k
        case 30232: return 1;   // 132k
        case 30280: return 2;   // 192k
        case 30250: return 3;   // 杜比
        case 30251: return 4;   // Hi-Res
        default: return 2;
    }
}

/// 音质偏好（小写枚举名）→ 想要的档位序号（拿不到更高档位时退回能拿到的最高档）
function wantRankOf(quality) {
    switch (String(quality || "")) {
        case "lower": return 0;
        case "standard": return 1;
        case "hq": return 2;
        case "sq": return 3;
        case "hires": return 4;
        case "upper": return 4;
        default: return 4;      // 主程序默认音质是 upper
    }
}

/// 档位序号 → 主程序用的音质枚举名（结果里的 quality 只用于展示）
function qualityNameOf(id) {
    switch (id) {
        case 30216: return "lower";
        case 30232: return "standard";
        case 30280: return "hq";
        case 30250: return "sq";
        case 30251: return "hires";
        default: return "";
    }
}

/// 从 dash.audio 里挑一路：优先不超过用户偏好、同档位选带宽最大的；一档都没有就选最低档
function pickAudio(list, quality) {
    const want = wantRankOf(quality);
    let best = null;
    let lowest = null;
    for (let i = 0; i < list.length; ++i) {
        const item = list[i];
        const url = item.baseUrl || item.base_url;
        if (!url) {
            continue;
        }
        const rank = audioRankOf(item.id);
        if (!lowest || rank < lowest.rank ||
            (rank === lowest.rank && (item.bandwidth || 0) > (lowest.item.bandwidth || 0))) {
            lowest = { rank: rank, item: item };
        }
        if (rank <= want &&
            (!best || rank > best.rank ||
                (rank === best.rank && (item.bandwidth || 0) > (best.item.bandwidth || 0)))) {
            best = { rank: rank, item: item };
        }
    }
    return best ? best.item : (lowest ? lowest.item : null);
}

/* ==================== Bili 解析 ==================== */

/// 解析结果缓存（同一个 BV 的 preload / 播放 / 封面 / 媒体信息会连着问好几次）
const biliCache = {};
const BILI_CACHE_TTL_MS = 10 * 60 * 1000;

function cacheKeyOf(bvid, quality) {
    return bvid + "|" + String(quality || "");
}

function rememberBili(key, result) {
    biliCache[key] = result;
    const keys = Object.keys(biliCache);
    if (keys.length > 64) {
        let oldestKey = keys[0];
        for (let i = 1; i < keys.length; ++i) {
            if ((biliCache[keys[i]].atMs || 0) < (biliCache[oldestKey].atMs || 0)) {
                oldestKey = keys[i];
            }
        }
        delete biliCache[oldestKey];
    }
}

/// BV 号 → {url, headers, size, durationMs, quality, expireAtMs}；给不出时返回 null
///
/// 两件事要注意：
/// - **多 P 视频不接管**：钩子载荷里没有分 P 序号，插件分不清当前是第几 P，硬接管会把第 2 P
///   播成第 1 P。`x/web-interface/view` 返回多个 `pages` 时直接返回 null，让主程序已经算好的
///   那一份继续生效；
/// - 两次请求各自有超时（钩子侧最多等 5 秒），任一失败都等于"这次不提供"。
function resolveBili(bvid, quality) {
    if (!bvid) {
        return Promise.resolve(null);
    }
    const key = cacheKeyOf(bvid, quality);
    const cached = biliCache[key];
    const now = Date.now();
    if (cached &&
        (now - cached.atMs) < BILI_CACHE_TTL_MS &&
        (!cached.expireAtMs || cached.expireAtMs - now > 60000)) {
        return Promise.resolve(cached);
    }
    return biliGetJson(
        "https://api.bilibili.com/x/web-interface/view?bvid=" + encodeURIComponent(bvid),
        1800,
    ).then(function (view) {
        if (!view || !Array.isArray(view.pages) || view.pages.length === 0) {
            return null;
        }
        if (view.pages.length > 1) {
            musicxx.host.log(2, "多 P 视频不接管（载荷里没有分 P 序号），交回主程序: " + bvid);
            return null;
        }
        const page = view.pages[0];
        const cid = page.cid || view.cid;
        if (!cid) {
            noteError("B 站视频没有 cid: " + bvid);
            return null;
        }
        const query =
            "bvid=" + encodeURIComponent(bvid) +
            "&cid=" + encodeURIComponent(String(cid)) +
            "&fnval=16&fnver=0&fourk=1&platform=pc&high_quality=1";
        return biliGetJson("https://api.bilibili.com/x/player/playurl?" + query, 1800)
            .then(function (data) {
                const audio = (data && data.dash && Array.isArray(data.dash.audio))
                    ? data.dash.audio
                    : [];
                const item = pickAudio(audio, quality);
                if (!item) {
                    noteError("B 站没有返回可用的音频流: " + bvid);
                    return null;
                }
                const url = item.baseUrl || item.base_url;
                const durationMs = page.duration
                    ? (Number(page.duration) * 1000)
                    : Number(data.timelength || 0);
                const size = (item.bandwidth && durationMs > 0)
                    ? Math.floor(Number(item.bandwidth) * (durationMs / 1000) / 8)
                    : 0;
                const result = {
                    url: url,
                    headers: biliHeaders(),
                    size: size,
                    durationMs: durationMs,
                    quality: qualityNameOf(item.id),
                    expireAtMs: deadlineMsOf(url),
                    atMs: Date.now(),
                };
                rememberBili(key, result);
                return result;
            });
    });
}

/// 把解析结果变成钩子的回答（必须用 `patch` 包一层：firstNonNull 只在收到 patch 时停止询问）
function biliAnswer(result) {
    return {
        patch: {
            src: {
                path: result.url,
                headers: result.headers,
                kind: "url",
                size: result.size || 0,
                durationMs: result.durationMs || 0,
                quality: result.quality || "",
                expireAtMs: result.expireAtMs || 0,
            },
        },
    };
}

/* ==================== 本地文件 → HTTP 地址 ==================== */

/// 代理用的 HTTP 服务 id（一个插件一个固定服务，静态路由都挂在它上面）
const PROXY_SERVER_ID = PLUGIN_ID + ".proxy";

/// 错误对象里的一句话（`message` 优先，再退 `error`）
function failureText(result) {
    if (!result) {
        return "unknown";
    }
    const text = result.message || result.error;
    return (typeof text === "string" && text !== "") ? text : "unknown";
}

/// 取代理服务；没有就起一个（`http.info` 里能看到自己起过哪些服务）
function ensureProxyServer() {
    if (!musicxx.net || !musicxx.net.http) {
        return {ok: false, error: "runtime_unsupported", message: "插件框架没有网络能力"};
    }
    const info = musicxx.net.http.info();
    const servers = (info && info.servers) || [];
    for (let i = 0; i < servers.length; i++) {
        const item = servers[i];
        if (item && item.id === PROXY_SERVER_ID) {
            return {ok: true, id: PROXY_SERVER_ID, url: item.url || ""};
        }
    }
    return musicxx.net.http.serve({
        id: PROXY_SERVER_ID,
        host: "127.0.0.1",
        port: 0,
        // 这个服务只用来发文件: 没命中路由就 404, 不用等插件回答
        staticOnly: true,
    });
}

/// 给一个本地文件挂一条静态路由，回答一个 `http://127.0.0.1:...` 地址
///
/// 为什么要绕一道 HTTP：插件框架的 JS 运行时没有 socket，但**播放器只认 http(s) 与本地文件**。
/// 有了这一层，插件就能把"播放器不支持的东西"变成普通 HTTP：
/// - 本地文件（本示例）；
/// - 插件用 `musicxx.net.fetch / download` 取到的远端数据（先落盘再登记，小内容直接给 `bytes`）；
/// - FTP / 自定义协议（插件取流后转发）；
/// - HLS 播放列表与分片、转码后的小段（用 `bytes` 给）。
///
/// 命中静态路由的请求由插件框架直接回答（200 / 206 / 416），不会走到 `http.onRequest`。
/// 旧版插件框架没有网络能力：退化为直接返回本地路径（`kind: "file"`），播放器自己读文件。
function answerProxyLocal(rawPath) {
    const path = normalizeLocalPath(rawPath);
    if (!path) {
        noteError("自定义音源里没有文件路径");
        return null;
    }
    stats.proxyCalls += 1;
    const server = ensureProxyServer();
    if (!server || server.ok !== true) {
        noteError("起本地 HTTP 服务失败: " + failureText(server));
        return {patch: {src: {path: path, kind: "file"}}};
    }
    const served = musicxx.net.http.route({
        serverId: server.id,
        file: path,
        fileName: basename(path),
    });
    if (!served || served.ok !== true) {
        noteError("登记本地文件失败: " + failureText(served));
        return null;
    }
    stats.lastServedUrl = served.url || "";
    return {
        patch: {
            src: {
                path: served.url,
                kind: "url",
                size: served.size || 0,
            },
        },
    };
}

/* ==================== 音源解析钩子 ==================== */

/// 主程序要把一个音源变成"播放器能打开的地址 + 请求头"时问一次。
///
/// 处理器是**异步派发**的（Dart 侧最多等 5 秒），所以这里可以返回 Promise（网络请求）；
/// 同步就能判断"不接管"的分支要立刻返回 null（最省时间）。
musicxx.hooks.register("musicxx.player.source.resolve", {
    mode: "decision",
    ownerTag: "example_override_media_source.bili",
}, function (ctx) {
    const type = (ctx && ctx.type) || "";
    const pluginType = (ctx && ctx.pluginType) || "";
    const src = (ctx && ctx.src) || "";
    const crude = (ctx && ctx.crudeSrc) || "";
    const quality = (ctx && ctx.quality) || "";

    // 1) 内置 Bili 来源：载荷里 src 已经换成临时直链，BV 号在 crudeSrc
    if (type === "bili" && settingOf("overrideBili")) {
        const bvid = extractBvid(crude) || extractBvid(src);
        if (!bvid) {
            return null;
        }
        stats.biliCalls += 1;
        return resolveBili(bvid, quality).then(function (result) {
            if (!result) {
                return null;    // 不接管：主程序已经算好的那份继续用
            }
            stats.biliOk += 1;
            return biliAnswer(result);
        });
    }

    // 2) 本插件声明的音源：src 里写的是 BV 号（插件自己定的资源 id 格式）
    if (pluginType === TYPE_PLUGIN_BILI && settingOf("pluginBiliEnabled")) {
        const bvid = extractBvid(src);
        if (!bvid) {
            noteError("插件音源里没有 BV 号: " + src);
            return null;    // 不接管：主程序会把它当解析不了的资源报错
        }
        stats.pluginBiliCalls += 1;
        return resolveBili(bvid, quality).then(function (result) {
            if (!result) {
                return null;
            }
            stats.biliOk += 1;
            return biliAnswer(result);
        });
    }

    // 3) 本地文件 → 插件 HTTP 服务的静态路由
    if (pluginType === TYPE_PROXY_LOCAL && settingOf("proxyLocalEnabled")) {
        return answerProxyLocal(src);
    }

    return null;
});

/* ==================== 音源类型声明 ==================== */

/// 声明本插件自己的音源类型（只用于自省与更清楚的报错；能不能播看的是上面那个处理器）
///
/// 声明跟着插件生命周期走：停用 / 卸载 / 关『拟声++』时主程序自动撤销，不写入磁盘；
/// 每次启用脚本都会重新声明一遍。命令行 `musicxx-cli call musicxx.source.list`、
/// MCP `ListMediaSources` 与插件动作 `musicxx.source.list` 读的就是这份清单。
function declareSourceTypes() {
    musicxx.call("musicxx.source.define", {
        id: TYPE_PLUGIN_BILI,
        title: "B 站（插件解析）",
        depict: "地址里写 BV 号，插件自己查 B 站接口换播放地址与请求头",
        srcDoc: "BV1GJ411x7h7",
    }).then(function (result) {
        if (!result || result.ok !== true) {
            noteError("声明音源类型失败: " + ((result && result.error) || "unknown"));
        }
    }, function (err) {
        noteError("声明音源类型失败: " + err.message);
    });
    musicxx.call("musicxx.source.define", {
        id: TYPE_PROXY_LOCAL,
        title: "本地文件（HTTP 代理）",
        depict: "地址里写本地文件路径，插件把它挂到自己的 HTTP 服务上，播放器拿到一个 http 地址",
        srcDoc: "D:/music/a.mp3 或 file:///D:/music/a.mp3",
    }).then(function (result) {
        if (!result || result.ok !== true) {
            noteError("声明音源类型失败: " + ((result && result.error) || "unknown"));
        }
    }, function (err) {
        noteError("声明音源类型失败: " + err.message);
    });
}

/* ==================== 界面：主页入口 + 设置页 ==================== */

function viewEnv(args) {
    if (args && typeof args.ui === "object" && args.ui !== null) {
        return args.ui;
    }
    return null;
}

/// 设置页上的本地服务一行（端口 / 挂了几条静态路由）
function proxyServerText() {
    const info = musicxx.net && musicxx.net.http ? musicxx.net.http.info() : null;
    const servers = (info && info.servers) || [];
    for (let i = 0; i < servers.length; i++) {
        const item = servers[i];
        if (!item || item.id !== PROXY_SERVER_ID) {
            continue;
        }
        const routes = item.routes || [];
        return "端口 " + String(item.port) + " · 登记 " + String(routes.length) + " 条";
    }
    return "未启动（还没有人用过本地代理）";
}

/// 设置页（也是主页入口打开的页面）
///
/// `override` 用于在"刚点了开关、异步写入还没回来"时先按新值画一次；
/// 平时直接读脚本里的 config（`saveConfig` 会先改内存值）。
function settingsView(args, override) {
    const env = viewEnv(args);
    const overrideBili = (override && typeof override.overrideBili === "boolean")
        ? override.overrideBili
        : settingOf("overrideBili");
    const pluginBili = (override && typeof override.pluginBiliEnabled === "boolean")
        ? override.pluginBiliEnabled
        : settingOf("pluginBiliEnabled");
    const proxyLocal = (override && typeof override.proxyLocalEnabled === "boolean")
        ? override.proxyLocalEnabled
        : settingOf("proxyLocalEnabled");
    const lastError = stats.lastError === "" ? "没有错误" : stats.lastError;
    const serverText = proxyServerText();
    return {
        title: "音源解析示例",
        subtitle: "插件参与音源解析的三条路",
        blocks: [
            kit.hint({
                text: "插件注册 musicxx.player.source.resolve：主程序要把音源换成播放地址时会先问插件。" +
                    "开了下面的开关，B 站歌曲的 BV 号就由插件自己查接口换成音频直链与请求头。",
            }, env),
            kit.divider({}, env),
            kit.switchRow({
                id: "overrideBili",
                title: "覆盖内置 Bili 解析",
                depict: "用 BV 号查 B 站接口，回答音频直链 + Referer/Origin/UA；多 P 视频自动交回主程序",
                value: overrideBili,
                action: "setSwitch",
            }, env),
            kit.listRow({
                title: "Bili 解析",
                subtitle: "被问到的次数 / 给出地址的次数",
                trailing: String(stats.biliCalls) + " / " + String(stats.biliOk),
            }, env),
            kit.divider({}, env),
            kit.switchRow({
                id: "pluginBiliEnabled",
                title: "自定义音源 plugin_bili",
                depict: "地址写 plugin.<插件 id>.plugin_bili://<BV 号>，播放时同样换成直链（关掉后该来源解析失败）",
                value: pluginBili,
                action: "setSwitch",
            }, env),
            kit.listRow({
                title: "自定义音源",
                subtitle: "被问到的次数",
                trailing: String(stats.pluginBiliCalls),
            }, env),
            kit.divider({}, env),
            kit.switchRow({
                id: "proxyLocalEnabled",
                title: "本地文件走 HTTP（plugin_proxy_local）",
                depict: "地址写 plugin.<插件 id>.plugin_proxy_local://<本地路径>，播放时由插件框架的 HTTP 服务把文件流给播放器（支持 Range）",
                value: proxyLocal,
                action: "setSwitch",
            }, env),
            kit.listRow({
                title: "本地 HTTP 服务",
                subtitle: "只监听 127.0.0.1，路由路径里只有随机 token",
                trailing: serverText,
            }, env),
            kit.listRow({
                title: "本地代理",
                subtitle: "被问到的次数",
                trailing: String(stats.proxyCalls),
            }, env),
            kit.divider({}, env),
            kit.listRow({
                title: "最近错误",
                subtitle: "只保留最后一条，排查用",
                trailing: lastError,
            }, env),
            kit.button({
                label: "播放示例视频（BV1GJ411x7h7）",
                variant: "primary",
                action: "playDemo",
            }, env),
            kit.button({
                label: "播放自定义音源（plugin_bili）",
                action: "playCustomSrc",
            }, env),
            kit.button({label: "刷新本页", action: "settings"}, env),
        ],
    };
}

musicxx.ui.registerEntry({
    name: "settings",
    type: "home.entry",
    order: 120,
    data: {
        title: "音源解析示例",
        subtitle: "插件覆盖 Bili 解析 / 声明自己的音源",
        icon: "resource/svg/source.svg",
        action: {kind: "route", route: "ext://" + PLUGIN_ID + "/settings"},
    },
});

musicxx.capability.register("settings", function (args) {
    return {view: settingsView(args, null)};
});

/// 设置页的开关（`switchRow` 值变化时派发，参数里带 `id` 与 `value`）
musicxx.capability.register("setSwitch", function (args) {
    const key = (args && typeof args.id === "string") ? args.id : "";
    const value = !!(args && args.value);
    saveConfig(key, value);
    musicxx.host.log(2, "设置: " + key + " → " + value);
    const override = {};
    override[key] = value;
    return {view: settingsView(args, override)};
});

/// 用统一入口播放一首歌
///
/// 能力处理器必须同步返回，所以动作发出去就结束，结果只写日志（页面不靠它刷新）。
/// `type` 与歌曲实体的来源类型一致：内置 Bili 是 `bili`，插件自己的音源写类型 id。
function playViaEntry(name, src, type) {
    musicxx.call("musicxx.player.playSrc", {
        src: src,
        type: type,
        name: name,
    }).then(function (result) {
        if (!result || result.ok !== true) {
            noteError("播放失败: " + ((result && result.error) ? result.error : "unknown"));
        }
    }, function (err) {
        noteError("播放失败: " + err.message);
    });
}

/// 设置页按钮：用统一入口播放一首 B 站歌曲（BV 号就是歌曲的音源）
musicxx.capability.register("playDemo", function (args) {
    playViaEntry("Never Gonna Give You Up", "BV1GJ411x7h7", "bili");
    return {view: settingsView(args, null)};
});

/// 设置页按钮：播放一首用本插件音源类型写的歌
///
/// 类型写在 `type` 里，`src` 就是插件自己的资源 id（这里是 BV 号）。
musicxx.capability.register("playCustomSrc", function (args) {
    playViaEntry("Never Gonna Give You Up", "BV1GJ411x7h7", TYPE_PLUGIN_BILI);
    return {view: settingsView(args, null)};
});

/// 自检能力：把一段文本挂到本地 HTTP 服务
///
/// 用处有两个：演示 `http.route` 的 `bytes` 形态（HLS 播放列表、转码后的小段就是这样给的）；
/// 以及在命令行里用 `curl` 验证服务真的在跑（返回的 `url` 直接 GET）。
musicxx.capability.register("serveText", function (args) {
    const text = (args && typeof args.text === "string")
        ? args.text
        : "musicxx plugin http server\n";
    const server = ensureProxyServer();
    if (!server || server.ok !== true) {
        return { ok: false, error: "serve_failed", message: failureText(server) };
    }
    return musicxx.net.http.route({
        serverId: server.id,
        bytes: text,
        contentType: "text/plain; charset=utf-8",
        fileName: "hello.txt",
    });
});

/// 自检能力：测试与命令行用它回读设置、计数与最近错误
musicxx.capability.register("probe", function () {
    const info = musicxx.net && musicxx.net.http ? musicxx.net.http.info() : null;
    const servers = (info && info.servers) || [];
    let proxyServer = {running: false};
    for (let i = 0; i < servers.length; i++) {
        const item = servers[i];
        if (item && item.id === PROXY_SERVER_ID) {
            proxyServer = {
                running: true,
                id: item.id,
                port: item.port,
                url: item.url || "",
                routes: item.routes || [],
            };
        }
    }
    return {
        pluginId: PLUGIN_ID,
        configLoaded: configLoaded,
        config: {
            overrideBili: settingOf("overrideBili"),
            pluginBiliEnabled: settingOf("pluginBiliEnabled"),
            proxyLocalEnabled: settingOf("proxyLocalEnabled"),
        },
        stats: {
            biliCalls: stats.biliCalls,
            biliOk: stats.biliOk,
            pluginBiliCalls: stats.pluginBiliCalls,
            proxyCalls: stats.proxyCalls,
            lastServedUrl: stats.lastServedUrl,
        },
        server: proxyServer,
        lastError: stats.lastError,
        sourceTypes: {
            bili: TYPE_PLUGIN_BILI,
            proxyLocal: TYPE_PROXY_LOCAL,
        },
    };
});

declareSourceTypes();
refreshConfig();

console.log("example_override_media_source 已加载（pid=" + PLUGIN_ID + "）");
