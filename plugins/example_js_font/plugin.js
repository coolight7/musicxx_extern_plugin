/// musicxx 字体切换示例插件 (JS, 零编译)
///
/// 演示全局字体更换能力（所有 `XXText*` 组件与输入框生效）：
/// - `微软雅黑`：使用系统字体 "Microsoft YaHei"；
/// - `字体文件示例`：动态加载插件目录自带的 `resource/font/example-font.ttf`；
/// - `拟声默认`：恢复应用默认字体（`default`）。
///
/// ## 这一页是"页面按钮 + 主程序动作"的**正确写法**样板（三条务必照抄）
///
/// 1. **能力处理器必须立刻返回**：页面按钮的 `dispatch` 动作由主程序**同步**进插件框架调用
///    （`plugin_call`，应用 UI 线程全程阻塞等这个返回值）。处理器里做长事情 = 整机卡住。
/// 2. **处理器里不要 `await` 主程序动作**：动作要回到主程序线程才有人处理，而主程序线程正阻塞在
///    第 1 条这次调用里 —— 两边互等到动作超时（5 秒）。表现为：点击后**整个程序卡几秒**，
///    然后插件收到 `action_timeout` 报"切换失败"，可字体其实已经切过去了。
///    正确做法是**先把动作发出去**（`musicxx.ui.setFontFamily(...)` 不 await，只跟 `.then`），
///    界面状态立刻返回视图；动作结果在后台回读。
/// 3. **应用会自己补一次刷新**：`dispatch` 拿到视图后 500ms 会再静默取一次页面（见
///    `ExternPluginViewBody._scheduleQuietReload`），所以后台改好的真实状态会自动显示出来，
///    插件不需要额外做刷新逻辑。
///
/// 约束：脚本顶层必须**同步**完成注册（顶层不能用 await）；异步逻辑放到钩子或 `.then` 里。

/// 界面 kit（随插件目录分发的两个脚本，见 plugin.yaml 的 `scripts`）
const kit = pluginxx.ui.kit;

const FONT_MODE_KEY = "fontMode";
const FONT_FILE_PATH = "resource/font/example-font.ttf";
const FONT_FILE_FAMILY = "ExampleFont";
const YAHEI_FAMILY = "Microsoft YaHei";
/// 官方变量：应用当前生效的全局字体（可同步 peek 读缓存，不必等动作往返）
const FONT_VAR_KEY = "musicxx.ui.fontFamily";

const MODE_LABELS = {
    yahei: "微软雅黑",
    file: "字体文件示例",
    default: "拟声默认",
};

/// 页面显示的模式：点下去先按新值显示，动作失败时回滚
let currentMode = "default";
/// 最近一次确认应用成功的模式（成功回执到达后才更新）
let appliedMode = "default";
/// 最近一次失败原因（页面展示 + 日志）
let lastError = "";

/// 订阅官方变量 `musicxx.ui.fontFamily`，让 `peek` 缓存一直是最新的
///
/// 规则：只有"有人关心（订阅或 watch）"的键，主程序才会推值给插件框架；
/// watch 是保活（不要回调），页面用 `peek` 同步读缓存 —— 不做 FFI 往返，也不会阻塞。
/// 顶层调用会被引擎推迟到"注册回放"阶段执行（与钩子/能力/UI 项同一套做法）。
musicxx.vars.watch(FONT_VAR_KEY);

function normalizeMode(mode) {
    return (mode === "yahei" || mode === "file") ? mode : "default";
}

/// 主程序当前生效字体（同步读变量缓存；缓存还没到就给"未知"）
function appliedFontText() {
    const cached = musicxx.vars.peek(FONT_VAR_KEY);
    const value = (cached && typeof cached.value === "string") ? cached.value : "";
    return value === "" ? "（未知）" : value;
}

/// 应用一个字体模式：**后台执行**，返回 Promise（成功 `{ok:true}`，失败 `{ok:false, error}`）
///
/// 调用方（能力处理器）只跟 `.then(...)`，**绝不 await** —— 见文件头第 2 条。
function applyFontMode(mode) {
    const target = normalizeMode(mode);
    let action;
    if (target === "yahei") {
        action = musicxx.ui.setFontFamily(YAHEI_FAMILY);
    } else if (target === "file") {
        action = musicxx.ui.loadFont({
            family: FONT_FILE_FAMILY,
            path: FONT_FILE_PATH,
            apply: true,
        }).then(function (res) {
            if (!res || res.ok !== true) {
                throw new Error((res && res.error) ? res.error : "字体文件加载失败");
            }
            return res;
        });
    } else {
        action = musicxx.ui.setFontFamily("default");
    }
    return action.then(function () {
        appliedMode = target;
        lastError = "";
        // 配置落盘：失败只记日志（内存里的模式已经切好了，不影响本次运行）
        musicxx.storage.setConfig(FONT_MODE_KEY, target).then(function () {
            return null;
        }, function (err) {
            musicxx.host.log(3, "写字体配置失败: " + err.message);
        });
        musicxx.host.log(2, "全局字体已切换为: " + MODE_LABELS[target]);
        return { ok: true, mode: target };
    }, function (err) {
        lastError = (err && err.message) ? err.message : String(err);
        musicxx.host.log(3, "切换字体模式失败: " + lastError);
        return { ok: false, error: lastError };
    });
}

/// 页面按钮动作：**立刻返回视图**（后台执行）
///
/// 页面拿到的就是"点完的样子"；动作失败时回滚显示，并异步提示（那时调用早就返回了，
/// 用提示/通知这类动作是安全的）。
function selectMode(mode, args) {
    const target = normalizeMode(mode);
    const previous = currentMode;
    currentMode = target;                       // 页面先按新值画
    applyFontMode(target).then(function (res) {
        if (res && res.ok) {
            musicxx.ui.toast("已切换字体：" + MODE_LABELS[target]).then(function () {
                return null;
            }, function () {
                return null;
            });
            return;
        }
        // 失败：回滚显示。应用在 500ms 后会静默再取一次页面，那时读到的是真实状态
        if (currentMode === target) {
            currentMode = previous;
        }
        musicxx.ui.notify({
            text: "切换字体失败：" + ((res && res.error) ? res.error : "未知原因"),
            kind: "error",
        }).then(function () {
            return null;
        }, function () {
            return null;
        });
    });
    return { view: cardView(args) };
}

/// 启动时恢复上次选择的模式（后台执行，不影响装载）
///
/// 装载阶段的脚本顶层只登记，不 await；这里用 `.then` 把配置读回来再切。
musicxx.storage.getConfig(FONT_MODE_KEY, "default").then(function (saved) {
    const target = normalizeMode(saved);
    if (target === "default") {
        return;
    }
    currentMode = target;
    applyFontMode(target);
}, function () {
    return null;
});

/// 主页入口
musicxx.ui.registerEntry({
    name: "card",
    type: "home.entry",
    order: 115,
    data: {
        title: "字体切换示例",
        subtitle: "微软雅黑 / 字体文件 / 拟声默认",
        action: { kind: "route", route: "ext://example_js_font/card" },
    },
});

/// 视图环境量（客户端取页面时会把能力段放进参数）
function viewEnv(args) {
    if (args && typeof args.ui === "object" && args.ui !== null) {
        return args.ui;
    }
    return null;
}

/// 自绘页面
function cardView(args) {
    const env = viewEnv(args);
    const pending = (currentMode !== appliedMode);
    const blocks = [
        kit.hint({
            text: "本插件演示通过外置插件动作切换全局 XXText* 与输入框字体。"
                + "切换是异步的：点完先按新模式显示，主程序生效后再由页面回读确认。",
        }, env),
        kit.divider({}, env),
        kit.listRow({
            title: "选择的模式",
            subtitle: "点按钮后立即显示",
            trailing: (MODE_LABELS[currentMode] || "未知") + (pending ? "（切换中…）" : ""),
        }, env),
        kit.listRow({
            title: "主程序生效字体",
            subtitle: "读官方变量 musicxx.ui.fontFamily 的同步缓存",
            trailing: appliedFontText(),
        }, env),
    ];
    if (lastError !== "") {
        blocks.push(kit.listRow({
            title: "最近一次失败",
            subtitle: "动作被拒绝或字体文件缺失",
            trailing: lastError,
        }, env));
    }
    blocks.push(
        kit.divider({}, env),
        kit.button({
            label: (currentMode === "yahei" ? "✓ " : "") + "切换为「微软雅黑」",
            variant: currentMode === "yahei" ? "primary" : "secondary",
            action: "selectYahei",
        }, env),
        kit.button({
            label: (currentMode === "file" ? "✓ " : "") + "切换为「字体文件示例」",
            variant: currentMode === "file" ? "primary" : "secondary",
            action: "selectFile",
        }, env),
        kit.button({
            label: (currentMode === "default" ? "✓ " : "") + "恢复为「拟声默认」",
            variant: currentMode === "default" ? "primary" : "secondary",
            action: "selectDefault",
        }, env),
        kit.divider({}, env),
        kit.text({ text: "效果预览（切换后以下文字与输入框会立刻变字体）：" }, env),
        kit.text({ text: "中文测试：拟声音乐播放器 · 全局字体预览 0.87.4" }, env),
        kit.text({ text: "English: The quick brown fox jumps over the lazy dog. 0123456789" }, env),
        kit.inputRow({
            id: "fontPreviewInput",
            title: "输入框字体预览",
            depict: "在这里输入文字，看输入框与提示文字用的字体",
            value: "",
        }, env),
    );
    return {
        title: "字体切换示例插件",
        subtitle: "点按钮切换；动作在后台执行，页面稍后自动回读真实状态",
        blocks: blocks,
    };
}

// ==================== 能力（页面 id 同名取视图 + 三个按钮） ====================

musicxx.capability.register("card", function (args) {
    return cardView(args);
});

musicxx.capability.register("selectYahei", function (args) {
    return selectMode("yahei", args);
});

musicxx.capability.register("selectFile", function (args) {
    return selectMode("file", args);
});

musicxx.capability.register("selectDefault", function (args) {
    return selectMode("default", args);
});
