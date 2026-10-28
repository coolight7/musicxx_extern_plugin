/*
 * plugin.js —— 注册、能力、钩子、账号状态
 *
 * 这个示例插件演示"插件怎么提供一个自己的音源"，整条链路分四段（另见 README.md）：
 *
 * 1. **配置账号**：主页入口「FTP 音乐」→ `accounts` 能力 → 账号写进插件私有 KV
 *    （`musicxx.storage`，密码只在本机，界面不回显）。
 * 2. **挑歌加进歌单**（两种写法，推荐第一种）：
 *    - 主程序画界面：`musicxx.source.browse` 声明 + `drive.list` 等能力 —— 「添加歌曲」菜单里
 *      自动多一行，点开就是与内置网盘一样的弹窗（路径栏 / 搜索 / 勾选 / 整目录递归 / 加歌单）；
 *    - 自己画：UI 项 `addsong.entry` 指向 `musicxx.ui.openOverlaySheet` → 浮层里的 `browse`
 *      能力列目录，`browseAdd` 调 `musicxx.playlist.addSong`（老写法，留着做对照）。
 * 3. **播放**：歌曲的音源类型是 `plugin.example_ftp_source.audio`，`src` 是
 *    `<账号指纹>/<FTP 路径>`（见 ftp.js 的 `resourceIdOf`）。播放时主程序问
 *    `musicxx.player.source.resolve`，这里把路径换成 `http://127.0.0.1:<port>/s/<token>`
 *    （proxy.js 起的本机转流服务），`kind` 写 `url`。
 * 4. **收尾**：插件停止 / 卸载 / 关『拟声++』时框架会关掉它的全部 socket；脚本里只在
 *    `musicxx.plugin.beforeUnload`（如果框架提供）里主动关一次监听，避免端口多占一会儿。
 *
 * 几条约束（写插件时都要记住）：
 * - 脚本顶层必须**同步**完成注册：账号读取是异步动作，只能在顶层发起、在回调里填状态；
 * - 能力处理器返回 Promise 也能生效（插件框架等它结算），所以列目录可以 `await` 自己的 FTP 客户端；
 * - 但**不能 `await` 主程序动作再依赖它**：`browseAdd` 里发起 `playlist.addSong` 后立刻返回视图，
 *   结果在回调里提示并关浮层；
 * - 钩子 `musicxx.player.source.resolve` 是异步派发的，主程序最多等 5 秒，所以这里只做内存操作
 *   （查账号 + 登记 token + 第一次启动监听），不登录、不查文件大小。
 */

/// 本插件 id（插件框架注入）
const PLUGIN_ID = musicxx.pluginId;

/// 本插件声明的音源类型 id
const TYPE_ID = "plugin." + PLUGIN_ID + ".audio";

/// 账号在插件私有 KV 里的键
const STORAGE_KEY = "account";

/// 浏览页一次显示多少项；"再显示 N 项"按这个步长加
const PAGE_SIZE = 50;

/// 一次最多选多少首（主程序 `musicxx.playlist.addSong` 的上限也是 200）
const MAX_SELECTED = 200;

/// 插件框架有没有 socket 能力（老版本没有：整个例子会明确报错，而不是静默不工作）
const NET_OK = !!(musicxx.net && musicxx.net.socket &&
    typeof musicxx.net.socket.listen === "function" &&
    typeof musicxx.net.socket.connect === "function");

/// 运行计数与最近错误（`probe` 能力回读，排查用；不写磁盘）
const stats = {
    resolveCalls: 0,
    resolveOk: 0,
    browseCalls: 0,
    browseOk: 0,
    addCalls: 0,
    playCalls: 0,
    lastError: ""
};

/// 账号状态：`account` 是解析好的账号（null = 没配置），`extras` 是最近测试结果
const accountState = {
    account: null,
    extras: { lastTestAt: 0, lastOk: false, lastMessage: "" },
    loaded: false,
    loading: null,
    returnTo: "",
    message: "",
    messageTone: "",
    busy: false
};

/// 账号表单草稿（只在编辑时存在；密码放内存里，不进动作参数、不进日志）
let draft = null;

/// 浏览页的当前状态（内存里；浮层关掉就丢，重新打开从根路径开始）
const browseState = {
    pid: 0,
    playlistName: "",
    songNum: 0,
    path: "/",
    keyword: "",
    selected: {},
    sizes: {},
    limit: PAGE_SIZE,
    entries: [],
    error: "",
    busy: false
};

/* ==================== 小工具 ==================== */

/// 记一条错误：插件日志 + `probe` 里的最后一条（不打印密码）
function noteError(text) {
    const line = String(text || "").slice(0, 240);
    stats.lastError = line;
    musicxx.host.log(3, PLUGIN_ID + ": " + line);
}

/// 提示一句（fire-and-forget）
function toast(text) {
    musicxx.ui.toast({ text: String(text) }).then(function () {
        return null;
    }, function (err) {
        noteError("弹提示失败：" + err.message);
    });
}

/// 取整数字段（非法值回退缺省）
function intOf(value, fallback) {
    const number = Number(value);
    return isFinite(number) ? Math.round(number) : fallback;
}

/* ==================== 账号 ==================== */

/// 存储里的原始对象 → 账号（缺字段补缺省；主机为空等于没配置）
function accountFromRaw(raw) {
    if (!raw || typeof raw !== "object") {
        return null;
    }
    const account = FtpSource.normalizeAccount(raw);
    return FtpSource.accountReady(account) ? account : null;
}

function extrasFromRaw(raw) {
    if (!raw || typeof raw !== "object") {
        return { lastTestAt: 0, lastOk: false, lastMessage: "" };
    }
    return {
        lastTestAt: intOf(raw.lastTestAt, 0),
        lastOk: raw.lastOk === true,
        lastMessage: (typeof raw.lastMessage === "string") ? raw.lastMessage : ""
    };
}

/// 读账号（只读一次；并发调用共用同一个 Promise）
function loadAccount() {
    if (accountState.loading) {
        return accountState.loading;
    }
    const pending = musicxx.storage.get(STORAGE_KEY, null).then(function (raw) {
        accountState.account = accountFromRaw(raw);
        accountState.extras = extrasFromRaw(raw);
        accountState.loaded = true;
        // 换账号会让旧歌的播放地址失效（指纹对不上就拒绝服务）
        FtpSource.setAccount(accountState.account);
        musicxx.host.log(2, accountState.account
            ? ("已读到 FTP 账号：" + accountState.account.host + ":" + accountState.account.port)
            : "还没有配置 FTP 账号");
        return accountState.account;
    }, function (err) {
        noteError("读取账号失败：" + err.message);
        accountState.loaded = true;
        return accountState.account;
    });
    accountState.loading = pending;
    pending.then(function () {
        accountState.loading = null;
    }, function () {
        accountState.loading = null;
    });
    return pending;
}

/// 账号读完了就直接给，没读完就等这次读取（解析钩子里会用到）
function ensureAccount() {
    if (accountState.loaded) {
        return Promise.resolve(accountState.account);
    }
    return loadAccount();
}

/// 写账号（连同最近测试结果）
function persistAccount(account) {
    const raw = {};
    for (const key in account) {
        if (Object.prototype.hasOwnProperty.call(account, key)) {
            raw[key] = account[key];
        }
    }
    raw.lastTestAt = accountState.extras.lastTestAt || 0;
    raw.lastOk = !!accountState.extras.lastOk;
    raw.lastMessage = accountState.extras.lastMessage || "";
    accountState.account = account;
    accountState.loaded = true;
    FtpSource.setAccount(account);
    return musicxx.storage.set(STORAGE_KEY, raw);
}

/// 只更新最近测试结果（账号本身不动）
function persistExtras() {
    if (!accountState.account) {
        return Promise.resolve(null);
    }
    return persistAccount(accountState.account);
}

/// 表单草稿（密码从空开始：已保存的密码不回显，留空 = 不修改）
function draftFromStored() {
    const account = accountState.account;
    return {
        host: account ? account.host : "",
        port: account ? account.port : FtpSource.defaultAccount.port,
        user: account ? account.user : "",
        password: "",
        rootPath: account ? account.rootPath : "/",
        encoding: account ? account.encoding : "utf-8",
        timeoutSec: account ? Math.round(account.timeoutMs / 1000) : 10
    };
}

/// 把草稿变成账号；不合法时给一句可读原因
function accountFromDraft() {
    const value = draft || draftFromStored();
    const previous = accountState.account;
    const account = FtpSource.normalizeAccount({
        host: value.host,
        port: value.port,
        user: value.user,
        // 密码留空 = 保留原来那个（新账号时就是空密码）
        password: value.password ? value.password : (previous ? previous.password : ""),
        encoding: value.encoding,
        rootPath: value.rootPath,
        timeoutMs: intOf(value.timeoutSec, 10) * 1000
    });
    if (!account.host) {
        return { error: "请先填服务器地址" };
    }
    if (!(account.port > 0) || account.port > 65535) {
        return { error: "端口要在 1 ~ 65535 之间" };
    }
    return { account: account };
}

/* ==================== 列目录 ==================== */

/// 把浏览状态回到"刚打开"的样子（`args` 来自浮层入口：带 `pid` / `name` / `songNum`）
function resetBrowse(args) {
    const account = accountState.account;
    browseState.pid = intOf(args && args.pid, 0);
    browseState.playlistName = (args && typeof args.name === "string") ? args.name : "";
    browseState.songNum = intOf(args && args.songNum, 0);
    browseState.keyword = "";
    browseState.selected = {};
    browseState.sizes = {};
    browseState.limit = PAGE_SIZE;
    browseState.error = "";
    browseState.busy = false;
    const fromArgs = (args && typeof args.path === "string") ? args.path : "";
    browseState.path = FtpSource.normalizePath(fromArgs || (account ? account.rootPath : "/"));
    browseState.entries = [];
}

/// 读当前目录；失败只把原因写进视图（不 reject，页面还能继续用）
function listNow() {
    const account = accountState.account;
    browseState.error = "";
    if (!FtpSource.accountReady(account)) {
        browseState.entries = [];
        return Promise.resolve();
    }
    return FtpSource.list(account, browseState.path).then(function (result) {
        browseState.path = result.path;
        browseState.entries = result.entries;
        for (let i = 0; i < result.entries.length; i++) {
            const entry = result.entries[i];
            if (!entry.isDir) {
                browseState.sizes[entry.path] = entry.size;
            }
        }
        stats.browseOk += 1;
    }, function (err) {
        browseState.entries = [];
        browseState.error = (err && err.message) ? err.message : String(err);
        noteError("列目录失败：" + browseState.error);
    });
}

/// 当前目录里"该显示"的条目（只按关键字筛，目录与文件都列）
function visibleEntries() {
    const keyword = browseState.keyword.toLowerCase();
    const rows = [];
    for (let i = 0; i < browseState.entries.length; i++) {
        const entry = browseState.entries[i];
        if (keyword && entry.name.toLowerCase().indexOf(keyword) < 0) {
            continue;
        }
        rows.push(entry);
    }
    return rows;
}

/// 选中的路径（保持勾选顺序）
function selectedPaths() {
    const rows = [];
    for (const path in browseState.selected) {
        if (Object.prototype.hasOwnProperty.call(browseState.selected, path) &&
            browseState.selected[path]) {
            rows.push(path);
        }
    }
    return rows;
}

/// 勾选 / 取消一个文件（`value` 没给就取反）
function toggleSelected(path, value) {
    const target = FtpSource.normalizePath(path || "");
    if (!target || target === "/") {
        return;
    }
    const next = (value === undefined || value === null) ? !browseState.selected[target] : !!value;
    if (next) {
        if (selectedPaths().length >= MAX_SELECTED && !browseState.selected[target]) {
            toast("一次最多选 " + MAX_SELECTED + " 首");
            return;
        }
        browseState.selected[target] = true;
        return;
    }
    delete browseState.selected[target];
}

/// 视图数据（交给 ui.js 拼）
function browseViewData(env) {
    const rows = visibleEntries();
    const limit = Math.max(PAGE_SIZE, browseState.limit);
    return {
        env: env,
        configured: FtpSource.accountReady(accountState.account),
        account: accountState.account || { host: "", port: FtpSource.defaultAccount.port, user: "" },
        path: browseState.path,
        // 已在服务器根目录：没有上一级，目录列表块就不出那个按钮（返回键回到“退出浮层”）
        atRoot: browseState.path === "/",
        keyword: browseState.keyword,
        entries: rows.slice(0, limit).map(function (entry) {
            const row = {
                path: entry.path,
                name: entry.name,
                isDir: !!entry.isDir,
                size: entry.size
            };
            if (!entry.isDir) {
                // 目录列表看到 song 就把这一行画成歌曲样式（封面 / 歌手 / 音源标签）；
                // 真正加进歌单时 browseAdd 会自己再算一遍（那边还带文件名识别）
                row.song = {
                    type: TYPE_ID,
                    src: FtpSource.resourceIdOf(accountState.account, entry.path),
                    fileName: entry.name,
                    size: entry.size || 0
                };
            }
            return row;
        }),
        total: rows.length,
        limit: limit,
        selected: browseState.selected,
        selectedCount: selectedPaths().length,
        pid: browseState.pid,
        playlistName: browseState.playlistName,
        error: browseState.error,
        busy: browseState.busy
    };
}

function browseAnswer(env) {
    return { view: FtpSource.browseView(browseViewData(env)) };
}

/* ==================== 账号页的视图数据 ==================== */

function accountsAnswer(env) {
    // 没配置过、也没草稿：只能给表单（摘要页没有可显示的内容）
    if (!accountState.account && !draft) {
        draft = draftFromStored();
    }
    return {
        view: FtpSource.accountsView({
            env: env,
            stored: accountState.account,
            draft: draft,
            message: accountState.message,
            messageTone: accountState.messageTone,
            busy: accountState.busy,
            returnTo: accountState.returnTo,
            lastTestAt: accountState.extras.lastTestAt || 0,
            lastOk: !!accountState.extras.lastOk,
            lastMessage: accountState.extras.lastMessage || ""
        })
    };
}

/// 保存后回哪儿：从浏览页进来就回浏览页（顺便按新账号重读目录）
function returnFromAccounts(env) {
    if (accountState.returnTo !== "browse") {
        accountState.returnTo = "";
        return Promise.resolve(accountsAnswer(env));
    }
    accountState.returnTo = "";
    browseState.selected = {};
    browseState.keyword = "";
    browseState.entries = [];
    browseState.error = "";
    browseState.limit = PAGE_SIZE;
    const account = accountState.account;
    browseState.path = account ? account.rootPath : "/";
    return listNow().then(function () {
        return browseAnswer(env);
    });
}

/// 测一次连接：连上并读根路径（结果写进账号的最近测试字段）
function testAccount() {
    const account = accountState.account;
    if (!FtpSource.accountReady(account)) {
        accountState.message = "还没配置账号";
        accountState.messageTone = "error";
        return Promise.resolve(null);
    }
    accountState.busy = true;
    return FtpSource.list(account, account.rootPath).then(function (result) {
        accountState.busy = false;
        accountState.extras = {
            lastTestAt: Date.now(),
            lastOk: true,
            lastMessage: "读到 " + result.entries.length + " 项"
        };
        accountState.message = "连接成功，根路径下读到 " + result.entries.length + " 项。";
        accountState.messageTone = "accent";
        return persistExtras().then(function () {
            return null;
        }, function (err) {
            noteError("写入测试结果失败：" + err.message);
            return null;
        });
    }, function (err) {
        accountState.busy = false;
        const text = (err && err.message) ? err.message : String(err);
        accountState.extras = {
            lastTestAt: Date.now(),
            lastOk: false,
            lastMessage: text.slice(0, 160)
        };
        accountState.message = "连接失败：" + text;
        accountState.messageTone = "error";
        return persistExtras().then(function () {
            return null;
        }, function (writeErr) {
            noteError("写入测试结果失败：" + writeErr.message);
            return null;
        });
    });
}

/* ==================== 能力：账号 ==================== */

/// 账号页 / 表单的一个字段变化
function applyAccountField(args) {
    if (!draft) {
        draft = draftFromStored();
    }
    const id = (args && typeof args.id === "string") ? args.id : "";
    const value = args ? args.value : undefined;
    switch (id) {
        case "host":
            draft.host = String(value === undefined || value === null ? "" : value).trim();
            break;
        case "port":
            draft.port = intOf(value, draft.port);
            break;
        case "user":
            draft.user = String(value === undefined || value === null ? "" : value);
            break;
        case "password":
            draft.password = String(value === undefined || value === null ? "" : value);
            break;
        case "rootPath":
            draft.rootPath = FtpSource.normalizePath(String(value || "/"));
            break;
        case "encoding":
            draft.encoding = (value === "gbk") ? "gbk" : "utf-8";
            break;
        case "timeoutSec":
            draft.timeoutSec = intOf(value, draft.timeoutSec);
            break;
        default:
            break;
    }
}

/// 保存草稿；成功返回 true（草稿清掉、账号已写入）
function saveDraft() {
    const parsed = accountFromDraft();
    if (parsed.error) {
        accountState.message = parsed.error;
        accountState.messageTone = "error";
        return Promise.resolve(false);
    }
    accountState.busy = true;
    return persistAccount(parsed.account).then(function () {
        accountState.busy = false;
        draft = null;
        accountState.message = "已保存到本机（" + parsed.account.host + ":" +
            parsed.account.port + "）。";
        accountState.messageTone = "accent";
        return true;
    }, function (err) {
        accountState.busy = false;
        accountState.message = "保存失败：" + err.message;
        accountState.messageTone = "error";
        return false;
    });
}

musicxx.capability.register("accounts", function (args) {
    const env = FtpSource.viewEnv(args);
    const op = (args && typeof args.op === "string") ? args.op : "view";

    if (!NET_OK) {
        accountState.message = "当前插件框架没有 TCP socket 能力：这个示例需要较新的插件框架版本。";
        accountState.messageTone = "error";
        return accountsAnswer(env);
    }

    return ensureAccount().then(function () {
        switch (op) {
            case "edit":
                if (draft) {
                    break;
                }
                draft = draftFromStored();
                accountState.returnTo = (args && args.from === "browse") ? "browse" : "";
                accountState.message = "密码不显示：留空表示不修改已保存的那个。";
                accountState.messageTone = "";
                break;
            case "field":
                applyAccountField(args);
                break;
            case "save":
                return saveDraft().then(function (saved) {
                    return saved ? returnFromAccounts(env) : accountsAnswer(env);
                });
            case "saveTest": {
                const cameFromBrowse = accountState.returnTo === "browse";
                return saveDraft().then(function (saved) {
                    if (!saved) {
                        return accountsAnswer(env);
                    }
                    return testAccount().then(function () {
                        if (!cameFromBrowse) {
                            return accountsAnswer(env);
                        }
                        // 从浏览页来的：回到浏览页，测试结果用提示说出来
                        toast(accountState.message);
                        return returnFromAccounts(env);
                    });
                });
            }
            case "test":
                return testAccount().then(function () {
                    return accountsAnswer(env);
                });
            case "clear":
                draft = null;
                accountState.busy = true;
                return musicxx.storage.remove(STORAGE_KEY).then(function () {
                    accountState.account = null;
                    accountState.extras = { lastTestAt: 0, lastOk: false, lastMessage: "" };
                    accountState.busy = false;
                    FtpSource.setAccount(null);
                    accountState.message = "已清除本机保存的账号。";
                    accountState.messageTone = "accent";
                    return accountsAnswer(env);
                }, function (err) {
                    accountState.busy = false;
                    accountState.message = "清除失败：" + err.message;
                    accountState.messageTone = "error";
                    return accountsAnswer(env);
                });
            case "cancel":
                draft = null;
                accountState.message = "";
                accountState.messageTone = "";
                accountState.returnTo = "";
                break;
            case "view":
            default:
                if (!accountState.account && !draft) {
                    draft = draftFromStored();
                }
                break;
        }
        return accountsAnswer(env);
    });
});

/* ==================== 能力：浏览 ==================== */

musicxx.capability.register("browse", function (args) {
    const env = FtpSource.viewEnv(args);
    const op = (args && typeof args.op === "string" && args.op) ? args.op : "open";

    if (!NET_OK) {
        return { view: FtpSource.accountsView({
            env: env,
            stored: null,
            draft: draftFromStored(),
            message: "当前插件框架没有 TCP socket 能力：这个示例需要较新的插件框架版本。",
            messageTone: "error"
        }) };
    }

    return ensureAccount().then(function () {
        switch (op) {
            case "open":
                resetBrowse(args);
                return listNow();
            case "resume":
                return listNow();
            case "enter":
                browseState.path = FtpSource.normalizePath((args && args.path) || "/");
                browseState.limit = PAGE_SIZE;
                return listNow();
            case "up":
                browseState.path = FtpSource.parentPath(browseState.path);
                browseState.limit = PAGE_SIZE;
                return listNow();
            case "refresh":
                return listNow();
            case "more":
                browseState.limit = browseState.limit + PAGE_SIZE;
                return null;
            case "keyword":
                browseState.keyword = (args && typeof args.value === "string") ? args.value : "";
                browseState.limit = PAGE_SIZE;
                return null;
            case "select":
                toggleSelected(args && args.path, args ? args.value : undefined);
                return null;
            case "toggle":
                toggleSelected(args && args.path, undefined);
                return null;
            default:
                return null;
        }
    }).then(function () {
        stats.browseCalls += 1;
        return browseAnswer(env);
    }, function (err) {
        noteError("浏览页出错：" + err.message);
        browseState.error = err.message;
        return browseAnswer(env);
    });
});

/* ==================== 能力：加入歌单 / 播放选中 ==================== */

/// 选中的文件 → `musicxx.playlist.addSong` 的 songs 载荷
function songsOfSelected(account) {
    const paths = selectedPaths();
    const songs = [];
    for (let i = 0; i < paths.length; i++) {
        const path = paths[i];
        const name = FtpSource.basename(path);
        if (!name) {
            continue;
        }
        songs.push({
            type: TYPE_ID,
            src: FtpSource.resourceIdOf(account, path),
            fileName: name,
            // 歌名与歌手留给主程序按文件名识别（与本地扫描、B 站列表同一套规则）
            size: browseState.sizes[path] || 0
        });
    }
    return songs;
}

musicxx.capability.register("browseAdd", function (args) {
    const env = FtpSource.viewEnv(args);
    const account = accountState.account;
    if (!FtpSource.accountReady(account)) {
        return browseAnswer(env);
    }
    const songs = songsOfSelected(account);
    if (!songs.length) {
        return browseAnswer(env);
    }
    if (!(browseState.pid > 0)) {
        toast("没有目标歌单：请从歌单页的「添加歌曲」进来");
        return browseAnswer(env);
    }
    browseState.busy = true;
    stats.addCalls += 1;
    // 不 await：能力处理器要尽快回视图，结果在回调里提示并关浮层
    musicxx.call("musicxx.playlist.addSong", {
        pid: browseState.pid,
        songs: songs
    }).then(function (result) {
        browseState.busy = false;
        if (!result || result.ok !== true) {
            const reason = (result && result.error) ? result.error : "unknown";
            noteError("加入歌单失败：" + reason);
            toast("加入歌单失败：" + reason);
            return;
        }
        browseState.selected = {};
        let text = "已添加 " + result.added + " 首到『" +
            (result.name || String(browseState.pid)) + "』";
        if (result.skipped > 0) {
            text += "，" + result.skipped + " 首已存在";
        }
        toast(text);
        musicxx.host.log(2, PLUGIN_ID + ": " + text);
        // 关掉这一层浮层（歌单页那边 await 的入口调用因此返回，列表随之刷新）
        musicxx.ui.overlayClose().then(function () {
            return null;
        }, function (err) {
            noteError("关闭浮层失败：" + err.message);
        });
    }, function (err) {
        browseState.busy = false;
        noteError("加入歌单失败：" + err.message);
        toast("加入歌单失败：" + err.message);
    });
    return browseAnswer(env);
});

musicxx.capability.register("browsePlay", function (args) {
    // 界面上不放这个按钮（挑歌时用不到），留着给脚本 / 自动化调：直接播第一个勾选的
    const env = FtpSource.viewEnv(args);
    const account = accountState.account;
    const paths = selectedPaths();
    if (!FtpSource.accountReady(account) || !paths.length) {
        return browseAnswer(env);
    }
    const path = paths[0];
    stats.playCalls += 1;
    musicxx.call("musicxx.player.playSrc", {
        src: FtpSource.resourceIdOf(account, path),
        type: TYPE_ID,
        name: FtpSource.basename(path)
    }).then(function (result) {
        if (!result || result.ok !== true) {
            noteError("播放失败：" + ((result && result.error) ? result.error : "unknown"));
        }
    }, function (err) {
        noteError("播放失败：" + err.message);
    });
    return browseAnswer(env);
});

/* ==================== 能力：自检 ==================== */

musicxx.capability.register("probe", function () {
    const account = accountState.account;
    const proxy = FtpSource.stats();
    return {
        ok: true,
        pluginId: PLUGIN_ID,
        sourceType: TYPE_ID,
        netOk: NET_OK,
        hasCodec: FtpSource.hasCodec,
        configured: FtpSource.accountReady(account),
        fingerprint: account ? FtpSource.accountFingerprint(account) : "",
        account: account ? {
            host: account.host,
            port: account.port,
            user: account.user,
            rootPath: account.rootPath,
            encoding: account.encoding
        } : null,
        proxy: {
            running: proxy.running,
            port: proxy.port,
            tokens: proxy.tokens,
            requests: proxy.requests,
            active: proxy.active,
            lastError: proxy.lastError
        },
        browse: {
            path: browseState.path,
            entries: browseState.entries.length,
            selected: selectedPaths().length,
            pid: browseState.pid
        },
        counts: {
            resolve: stats.resolveCalls,
            resolveOk: stats.resolveOk,
            browse: stats.browseCalls,
            browseOk: stats.browseOk,
            addSong: stats.addCalls,
            play: stats.playCalls
        },
        lastError: stats.lastError
    };
});

/* ==================== 音源解析钩子 ==================== */

/// 主程序要把音源换成"播放器能打开的地址"时会问一次（异步派发，最多等 5 秒）
///
/// 只做三件事：确认是自己的类型 → 指纹对得上 → 登记 token 拿一个本机地址。
/// 不登录、不查大小（那些都在真正播放的 HTTP 请求里做）。
musicxx.hooks.register("musicxx.player.source.resolve", {
    mode: "decision",
    ownerTag: PLUGIN_ID + ".ftp"
}, function (ctx) {
    if (!ctx || ctx.pluginType !== TYPE_ID) {
        return null;
    }
    stats.resolveCalls += 1;
    const parsed = FtpSource.parseResourceId(ctx.src);
    if (!parsed) {
        noteError("资源 id 格式不对（要 <8 位指纹>/<路径>）：" + String(ctx.src || "").slice(0, 120));
        return null;
    }
    return ensureAccount().then(function (account) {
        if (!FtpSource.accountReady(account)) {
            noteAccountProblem("还没有配置账号");
            return null;
        }
        if (FtpSource.accountFingerprint(account) !== parsed.fingerprint) {
            noteAccountProblem("这首歌不是当前账号添加的");
            return null;
        }
        return FtpSource.serve(account, parsed.path, 0).then(function (url) {
            stats.resolveOk += 1;
            // 回答必须包一层 patch：firstNonNull 只在收到 patch 时停止询问后面的实现
            return { patch: { src: { path: url, kind: "url" } } };
        });
    }, function (err) {
        noteError("解析失败：" + err.message);
        return null;
    });
});

/// 解析不出来的原因要让人看见，但别每首歌都弹一次
let lastProblemAt = 0;
function noteAccountProblem(reason) {
    noteError("解析跳过：" + reason);
    const now = Date.now();
    if (now - lastProblemAt < 30000) {
        return;
    }
    lastProblemAt = now;
    toast("FTP 音源不可用：" + reason + "（打开「FTP 音乐」重新配置）");
}

/* ==================== 音源类型声明 ==================== */

/// 声明本插件自己的音源类型
///
/// 只用于自省与更清楚的报错（`musicxx-cli call musicxx.source.list` / MCP `ListMediaSources`
/// 读这份清单）；能不能播看的是上面那个解析处理器。声明跟着插件生命周期撤销，不写磁盘。
musicxx.call("musicxx.source.define", {
    id: TYPE_ID,
    title: "FTP 文件",
    depict: "地址里写「账号指纹/路径」，播放时由插件把文件转成本机 http 地址",
    srcDoc: "3f9c1d7b/Music/Album/a.flac"
}).then(function (result) {
    if (!result || result.ok !== true) {
        noteError("声明音源类型失败：" + ((result && result.error) || "unknown"));
    }
}, function (err) {
    noteError("声明音源类型失败：" + err.message);
});

/* ==================== 网盘：浏览与挑歌（主程序画界面） ==================== */

/// 一次 `drive.resolveSongs` 最多读多少首（超出截断，界面会提示）
const MAX_DRIVE_SONGS = 2000;

/// 声明"这个音源类型可以浏览"
///
/// 声明之后主程序把它当成一个网盘来源：「添加歌曲」菜单里自动多一行（内置网盘风格：路径栏 /
/// 排序 / 搜索 / 勾选 / 整目录递归，最多 5 层），数据由下面几个能力提供。
/// `capabilities` 里出现某个键 = 这个能力实现了（名字也可以改成别的）：
/// `list` 必填，`search` / `resolveSongs` 可选（没给就没搜索按钮、退回逐层列目录）。
/// 声明跟着插件生命周期撤销，不写磁盘。
musicxx.call("musicxx.source.browse", {
    id: TYPE_ID,
    title: "FTP 文件",
    root: "/",
    icon: "resource/svg/ftp.svg",
    search: true,
    write: true,
    capabilities: {
        list: "drive.list",
        search: "drive.search",
        resolveSongs: "drive.resolveSongs",
        mkdir: "drive.mkdir",
        rename: "drive.rename",
        remove: "drive.remove",
        info: "drive.info"
    }
}).then(function (result) {
    if (!result || result.ok !== true) {
        noteError("声明可浏览来源失败：" + ((result && result.error) || "unknown"));
    }
}, function (err) {
    noteError("声明可浏览来源失败：" + err.message);
});

/// 一个 FTP 目录项 → `drive.list` 的条目
///
/// 文件要带 `song`（类型 + 资源 id），主程序才按"歌曲行"画、才能加进歌单；
/// `name` 当 `fileName` 用，歌名与歌手由主程序按文件名识别（与本地扫描同一套规则）。
function driveEntryOf(account, entry) {
    const item = {
        name: entry.name,
        path: entry.path,
        isDir: !!entry.isDir,
        size: entry.size || 0
    };
    if (!item.isDir) {
        item.song = {
            type: TYPE_ID,
            src: FtpSource.resourceIdOf(account, entry.path),
            fileName: entry.name,
            size: item.size
        };
    }
    return item;
}

/// `drive.list`：一页目录
///
/// `offset` 为空串 = 第一页，`next` 为空串 = 没有下一页；主程序拿到后还会本地排一次，
/// 所以这里只按偏移切片（FTP 的 LIST 没有分页，一次就能读完）。
musicxx.capability.register("drive.list", function (args) {
    const account = accountState.account;
    if (!FtpSource.accountReady(account)) {
        return { ok: false, error: "还没有配置 FTP 账号（打开「FTP 音乐」填服务器）" };
    }
    const path = FtpSource.normalizePath(args && args.path ? args.path : "/");
    const offset = Math.max(0, intOf(args && args.offset, 0));
    const limit = Math.max(1, intOf(args && args.limit, PAGE_SIZE));
    return FtpSource.list(account, path).then(function (result) {
        const entries = [];
        for (let i = 0; i < result.entries.length; i += 1) {
            entries.push(driveEntryOf(account, result.entries[i]));
        }
        const page = entries.slice(offset, offset + limit);
        const next = (offset + limit < entries.length) ? String(offset + limit) : "";
        stats.browseCalls += 1;
        stats.browseOk += 1;
        return { ok: true, entries: page, next: next, total: entries.length };
    }, function (err) {
        const reason = (err && err.message) ? err.message : String(err);
        noteError("drive.list 失败：" + reason);
        return { ok: false, error: reason };
    });
});

/// `drive.search`：在当前目录里按名字找（FTP 没有搜索接口，这里只搜一层）
musicxx.capability.register("drive.search", function (args) {
    const account = accountState.account;
    if (!FtpSource.accountReady(account)) {
        return { ok: false, error: "还没有配置 FTP 账号（打开「FTP 音乐」填服务器）" };
    }
    const keyword = String((args && args.keyword) || "").trim().toLowerCase();
    if (!keyword) {
        return { ok: true, entries: [], next: "" };
    }
    const path = FtpSource.normalizePath(args && args.path ? args.path : "/");
    return FtpSource.list(account, path).then(function (result) {
        const entries = [];
        for (let i = 0; i < result.entries.length; i += 1) {
            const entry = result.entries[i];
            if (String(entry.name).toLowerCase().indexOf(keyword) >= 0) {
                entries.push(driveEntryOf(account, entry));
            }
        }
        return { ok: true, entries: entries, next: "" };
    }, function (err) {
        return { ok: false, error: (err && err.message) ? err.message : String(err) };
    });
});

/// `drive.resolveSongs`：把一个文件夹里的歌一次读完（勾整目录时比逐层列目录快）
///
/// 失败时返回 `ok: false`；主程序会退回逐层 `drive.list`，所以这里不用自己重试。
/// 每个文件都当音频（FTP 上放的一般就是音乐），超过 [MAX_DRIVE_SONGS] 截断。
musicxx.capability.register("drive.resolveSongs", function (args) {
    const account = accountState.account;
    if (!FtpSource.accountReady(account)) {
        return { ok: false, error: "还没有配置 FTP 账号（打开「FTP 音乐」填服务器）" };
    }
    const paths = (args && Array.isArray(args.paths)) ? args.paths : [];
    const maxDepth = Math.min(Math.max(1, intOf(args && args.maxDepth, 5)), 5);
    const songs = [];
    const seen = {};
    function walk(path, depth) {
        if (depth > maxDepth || songs.length >= MAX_DRIVE_SONGS) {
            return Promise.resolve(null);
        }
        return FtpSource.list(account, path).then(function (result) {
            let chain = Promise.resolve(null);
            for (let i = 0; i < result.entries.length; i += 1) {
                const entry = result.entries[i];
                if (entry.isDir) {
                    chain = chain.then(function () {
                        return walk(entry.path, depth + 1);
                    });
                } else {
                    const src = FtpSource.resourceIdOf(account, entry.path);
                    if (!src || seen[src]) {
                        continue;
                    }
                    seen[src] = true;
                    songs.push({
                        type: TYPE_ID,
                        src: src,
                        fileName: entry.name,
                        size: entry.size || 0
                    });
                }
            }
            return chain;
        }, function () {
            // 某个子目录读不了就跳过，别让整个文件夹失败
            return null;
        });
    }
    let chain = Promise.resolve(null);
    for (let i = 0; i < paths.length; i += 1) {
        const target = paths[i];
        chain = chain.then(function () {
            return walk(FtpSource.normalizePath(target), 1);
        });
    }
    return chain.then(function () {
        return {
            ok: true,
            songs: songs,
            truncated: songs.length >= MAX_DRIVE_SONGS
        };
    });
});

/// `drive.mkdir`：新建文件夹（FTP `MKD`）
///
/// 参数 `{parent, name}`，成功回新路径（主程序拿它刷新列表）。
musicxx.capability.register("drive.mkdir", function (args) {
    const account = accountState.account;
    if (!FtpSource.accountReady(account)) {
        return { ok: false, error: "还没有配置 FTP 账号（打开「FTP 音乐」填服务器）" };
    }
    const parent = FtpSource.normalizePath(args && args.parent ? args.parent : "/");
    const name = String((args && args.name) || "").trim();
    if (!name || name.indexOf("/") >= 0) {
        return { ok: false, error: "名字不能为空，也不能带 /" };
    }
    return FtpSource.mkdir(account, FtpSource.joinPath(parent, name)).then(function (path) {
        return { ok: true, path: path };
    }, function (err) {
        return { ok: false, error: (err && err.message) ? err.message : String(err) };
    });
});

/// `drive.rename`：重命名 / 移动（FTP `RNFR` + `RNTO`）
///
/// 参数 `{path, newName}`；新名字相对同一层目录。成功回新路径。
musicxx.capability.register("drive.rename", function (args) {
    const account = accountState.account;
    if (!FtpSource.accountReady(account)) {
        return { ok: false, error: "还没有配置 FTP 账号（打开「FTP 音乐」填服务器）" };
    }
    const source = FtpSource.normalizePath(args && args.path ? args.path : "");
    const newName = String((args && args.newName) || "").trim();
    if (!source || source === "/") {
        return { ok: false, error: "路径不对：" + source };
    }
    if (!newName || newName.indexOf("/") >= 0) {
        return { ok: false, error: "名字不能为空，也不能带 /" };
    }
    const target = FtpSource.joinPath(FtpSource.parentOf(source), newName);
    return FtpSource.rename(account, source, target).then(function (path) {
        return { ok: true, path: path };
    }, function (err) {
        return { ok: false, error: (err && err.message) ? err.message : String(err) };
    });
});

/// `drive.remove`：删除（文件走 `DELE`，目录走 `RMD`）
///
/// 参数 `{paths:[…]}`；一条失败就停下并报错（已删掉的那几个主程序会以为还在，
/// 用户刷新列表就对了）。
musicxx.capability.register("drive.remove", function (args) {
    const account = accountState.account;
    if (!FtpSource.accountReady(account)) {
        return { ok: false, error: "还没有配置 FTP 账号（打开「FTP 音乐」填服务器）" };
    }
    const paths = (args && Array.isArray(args.paths)) ? args.paths : [];
    if (!paths.length) {
        return { ok: false, error: "没有要删除的路径" };
    }
    let chain = Promise.resolve(0);
    for (let i = 0; i < paths.length; i += 1) {
        const target = FtpSource.normalizePath(paths[i]);
        chain = chain.then(function (done) {
            return FtpSource.remove(account, target).then(function () {
                return done + 1;
            });
        });
    }
    return chain.then(function (removed) {
        return { ok: true, removed: removed };
    }, function (err) {
        return { ok: false, error: (err && err.message) ? err.message : String(err) };
    });
});

/// `drive.info`：条目信息（大小 / 类型 / 修改时间）
///
/// 回答 `{ok, title, pairs:[{k,v}]}`：主程序把它们拼成一段文本显示。
musicxx.capability.register("drive.info", function (args) {
    const account = accountState.account;
    if (!FtpSource.accountReady(account)) {
        return { ok: false, error: "还没有配置 FTP 账号（打开「FTP 音乐」填服务器）" };
    }
    const target = FtpSource.normalizePath(args && args.path ? args.path : "");
    return FtpSource.stat(account, target).then(function (entry) {
        if (!entry) {
            return { ok: false, error: "服务器上没有这一项：" + target };
        }
        const pairs = [
            { k: "路径", v: entry.path },
            { k: "类型", v: entry.isDir ? "文件夹" : "文件" }
        ];
        if (!entry.isDir) {
            pairs.push({ k: "大小", v: FtpSource.formatSize(entry.size || 0) });
        }
        if (entry.modified) {
            pairs.push({ k: "修改时间", v: String(entry.modified) });
        }
        return { ok: true, title: entry.name, pairs: pairs };
    }, function (err) {
        return { ok: false, error: (err && err.message) ? err.message : String(err) };
    });
});

/* ==================== UI 项 ==================== */

/// 主页入口：账号页
musicxx.ui.registerEntry({
    name: "accounts",
    type: "home.entry",
    order: 130,
    data: {
        title: "FTP 音乐",
        subtitle: "浏览 FTP 服务器上的歌曲",
        icon: "resource/svg/ftp.svg",
        action: { kind: "route", route: "ext://" + PLUGIN_ID + "/accounts" }
    }
});

/// 主页入口：整个来源的**整页浏览**（与内置网盘页一样）
///
/// 动作是 `command` → `musicxx.ui.browseDrive`（主程序执行：打开这个来源的整页目录列表，
/// 立即返回，不等用户关页面）。插件自己不用画界面，列表、菜单、搜索都由主程序给。
musicxx.ui.registerEntry({
    name: "browseDrive",
    type: "home.entry",
    order: 131,
    data: {
        title: "FTP 文件",
        subtitle: "整页浏览与下载",
        icon: "resource/svg/ftp.svg",
        action: { kind: "command", name: "musicxx.ui.browseDrive", args: { type: TYPE_ID } }
    }
});

/// 歌单「添加歌曲」里的入口：打开插件自己的挑歌浮层
///
/// 动作是 `command` → `musicxx.ui.openOverlaySheet`（主程序执行：打开这个插件视图的底部浮层，
/// 等用户关掉它再返回）。主程序会在点击时把当前歌单的 `pid` / `name` / `songNum` 合进参数。
musicxx.ui.registerEntry({
    name: "addSong",
    type: "addsong.entry",
    order: 930,
    data: {
        title: "添加FTP歌曲",
        subtitle: "从 FTP 服务器选择",
        icon: "resource/svg/ftp.svg",
        action: {
            kind: "command",
            name: "musicxx.ui.openOverlaySheet",
            args: {
                route: "ext://" + PLUGIN_ID + "/browse",
                title: "添加FTP歌曲",
                // 可用宽高的 90%：两种布局里都是“左侧 / 顶部固定、列表占满剩下的地方”
                // （见 ui.js 的 browseView）
                size: "full"
            }
        }
    }
});

/* ==================== 启动 ==================== */

/// 顶层只发起读取（动作是异步的，顶层不能 await）
loadAccount();

if (!NET_OK) {
    noteError("插件框架没有 TCP socket 能力：FTP 例子无法工作（主页入口仍在，页面会说明原因）");
}
if (!FtpSource.hasCodec) {
    musicxx.host.log(3, PLUGIN_ID + ": 插件框架没有 musicxx.util.textEncode/textDecode，" +
        "GBK 服务器上的中文名会乱码（UTF-8 服务器不受影响）");
}

console.log(PLUGIN_ID + " 已加载（音源类型 " + TYPE_ID + "）");
