/*
 * search.js —— 搜索页里的「FTP 音乐」搜索类型
 *
 * 搜索页顶部那排标签（本地歌曲 / B站 / 网盘……）里可以多一个插件自己的类型：在槽位
 * `musicxx.ui.search.type` 上注册一份内容就是。用户切到这个标签、输入关键词、点搜索之后，
 * 主程序把关键词与请求序号写进作用域变量，再让 `view()` 生成结果界面。
 *
 * 两条必须记住的规则（见包内 docs/plugin-ui.md §1.11）：
 * - 生成函数**不能等网络**：这里只读缓存，没搜过就返回"正在搜索…"，同时在后台发起搜索；
 *   搜完调 `musicxx.ui.slot.update` 让主程序再问一次。
 * - 关键词变了才重新搜：用 `keyword + request` 当这次的凭据，回来的结果对不上就丢掉
 *   （用户可能又点了搜索或换了词）。
 *
 * 结果直接写 `musicxx.SongListView`：插件只给 items 里的 data.song，行样式、点行播放、
 * 内置歌曲菜单都由主程序画。
 */

var FtpSearch = globalThis.FtpSearch || (globalThis.FtpSearch = {});

(function (ns) {
  "use strict";

  /// 搜索类型的内容短 id（同时也是 `slot.update` 里用的 id）
  var CONTENT_ID = "search";
  /// 本插件声明的音源类型（不引用 plugin.js 里的常量：脚本按顺序执行，这里先加载）
  var TYPE_ID = "plugin." + musicxx.pluginId + ".audio";
  /// 往下搜几层（示例只演示形状：整棵目录树可能很大，不适合一次全读）
  var MAX_DEPTH = 3;
  /// 最多读几个目录 / 返回几首
  var MAX_DIRS = 40;
  var MAX_FILES = 200;
  /// 认作音频的后缀（FTP 上也可能放字幕 / 封面，别把它们都当歌）
  var AUDIO_EXT = {
    mp3: 1, flac: 1, wav: 1, aac: 1, m4a: 1, m4b: 1, ogg: 1, oga: 1, opus: 1,
    wma: 1, ape: 1, wv: 1, tta: 1, aiff: 1, aif: 1, dsf: 1, dff: 1, alac: 1
  };

  /// 上一次搜索的结果（生成函数只读这里）
  var cache = {
    keyword: "",
    request: -1,
    /// idle = 还没搜过；running = 正在搜；ok = 有结果；error = 失败
    state: "idle",
    items: [],
    error: "",
    at: 0
  };

  /// 账号来源由 plugin.js 注入（它才是账号状态的属主）
  var accountProvider = null;

  ns.setAccountProvider = function (provider) {
    accountProvider = (typeof provider === "function") ? provider : null;
  };

  function accountNow() {
    if (!accountProvider) {
      return { loaded: false, account: null };
    }
    try {
      return accountProvider() || { loaded: false, account: null };
    } catch (e) {
      return { loaded: false, account: null };
    }
  }

  /* ==================== 搜索 ==================== */

  function extOf(name) {
    var text = String(name || "");
    var at = text.lastIndexOf(".");
    if (at < 0 || at === text.length - 1) {
      return "";
    }
    return text.substring(at + 1).toLowerCase();
  }

  function isAudioName(name) {
    return AUDIO_EXT[extOf(name)] === 1;
  }

  /// 在 `dir` 下面按文件名找（递归到 [MAX_DEPTH] 层，目录与结果都有上限）
  ///
  /// 读不了的子目录直接跳过：FTP 上一个权限不够的目录不应该让整次搜索失败。
  function collect(account, dir, depth, keyword, out, budget) {
    if (budget.dirs >= MAX_DIRS || out.length >= MAX_FILES) {
      return Promise.resolve(out);
    }
    budget.dirs += 1;
    return FtpSource.list(account, dir).then(function (result) {
      var chain = Promise.resolve(null);
      var entries = (result && result.entries) ? result.entries : [];
      for (var i = 0; i < entries.length; i++) {
        var entry = entries[i];
        if (entry.isDir) {
          if (depth < MAX_DEPTH) {
            chain = chain.then(function (sub) {
              return collect(account, sub.path, depth + 1, keyword, out, budget);
            }.bind(null, entry));
          }
          continue;
        }
        if (!isAudioName(entry.name)) {
          continue;
        }
        if (String(entry.name).toLowerCase().indexOf(keyword) < 0) {
          continue;
        }
        if (out.length >= MAX_FILES) {
          break;
        }
        out.push(entry);
      }
      return chain.then(function () {
        return out;
      });
    }, function () {
      // 这个目录读不了：已经有结果照常返回（不要让整次搜索失败 / 拿到 null）
      return out;
    });
  }

  function refresh() {
    try {
      musicxx.ui.slot.update("musicxx.ui.search.type", [CONTENT_ID]);
    } catch (e) {
      musicxx.host.log(3, "[ftp-search] 请求重生成失败: " +
        ((e && e.message) ? e.message : String(e)));
    }
  }

  /// 发起一次搜索（结果回来以后让主程序再问一次；过期的结果丢掉）
  function startSearch(account, keyword, request) {
    cache.keyword = keyword;
    cache.request = request;
    cache.state = "running";
    cache.items = [];
    cache.error = "";
    cache.at = Date.now();
    var root = FtpSource.normalizePath(account.rootPath || "/");
    collect(account, root, 1, keyword, [], { dirs: 0 }).then(function (items) {
      if (cache.keyword !== keyword || cache.request !== request) {
        return;
      }
      cache.items = Array.isArray(items) ? items : [];
      cache.state = "ok";
      cache.at = Date.now();
      refresh();
    }, function (err) {
      if (cache.keyword !== keyword || cache.request !== request) {
        return;
      }
      cache.state = "error";
      cache.error = (err && err.message) ? err.message : String(err);
      cache.at = Date.now();
      refresh();
    });
  }

  /* ==================== 视图 ==================== */

  function tip(text, color) {
    return {
      blocks: [{
        kind: "Text",
        text: text,
        type: "caption",
        color: color || "hint"
      }]
    };
  }

  function sizeText(size) {
    var value = Number(size) || 0;
    if (value <= 0) {
      return "";
    }
    if (value >= 1024 * 1024) {
      return (Math.round(value / (1024 * 1024) * 10) / 10) + " MB";
    }
    return Math.max(1, Math.round(value / 1024)) + " KB";
  }

  /// 结果列表：块 `musicxx.SongListView`（行样式与点击播放都由主程序画）
  function listView(account, items, keyword) {
    var rows = [];
    for (var i = 0; i < items.length; i++) {
      var entry = items[i];
      var src = FtpSource.resourceIdOf(account, entry.path);
      if (!src) {
        continue;
      }
      rows.push({
        id: src,
        title: entry.name,
        subtitle: entry.path,
        trailing: sizeText(entry.size),
        data: {
          song: {
            type: TYPE_ID,
            src: src,
            fileName: entry.name,
            size: Number(entry.size) || 0
          }
        }
      });
    }
    return {
      blocks: [
        {
          kind: "Text",
          text: "在 FTP 上找到 " + rows.length + " 首（关键词「" + keyword + "」，最多看 " +
            MAX_DEPTH + " 层目录）",
          type: "caption",
          color: "hint"
        },
        {
          kind: "musicxx.SongListView",
          emptyText: "没有找到文件名带这个词的音频",
          items: rows
        }
      ]
    };
  }

  /// 生成函数的正文（外面包一层 try/catch：生成函数抛异常会让整份内容失败）
  function buildView(ctx) {
    var keywordRaw = (ctx && ctx["scope:musicxx.search.keyword"]) || "";
    var keyword = String(keywordRaw).trim();
    var request = Number(ctx && ctx["scope:musicxx.search.request"]) || 0;
    var current = accountNow();
    if (!current.loaded) {
      return tip("正在读取 FTP 账号…");
    }
    var account = current.account;
    if (!FtpSource.accountReady(account)) {
      return tip("还没有配置 FTP 账号：先在「FTP 音乐」里填服务器（或用下面的测试服务端）");
    }
    if (!keyword) {
      return tip("输入歌名里的一个词，再点搜索：插件会按文件名在 FTP 上找（最多 " +
        MAX_DEPTH + " 层目录）");
    }
    var sameSearch = (cache.keyword === keyword && cache.request === request);
    if (!sameSearch) {
      startSearch(account, keyword, request);
      return tip("正在 FTP 上搜索「" + keyword + "」…");
    }
    if (cache.state === "running") {
      return tip("正在 FTP 上搜索「" + keyword + "」…");
    }
    if (cache.state === "error") {
      return tip("搜索失败：" + cache.error, "error");
    }
    if (cache.state !== "ok") {
      return tip("正在 FTP 上搜索「" + keyword + "」…");
    }
    return listView(account, Array.isArray(cache.items) ? cache.items : [], keyword);
  }

  /// 生成函数：只读缓存，不发请求（规则见文件头）
  ns.view = function (ctx) {
    try {
      return buildView(ctx);
    } catch (e) {
      musicxx.host.log(4, "[ftp-search] 生成失败: " +
        ((e && e.stack) ? e.stack : String(e)));
      return tip("搜索界面生成失败：" + ((e && e.message) ? e.message : String(e)), "error");
    }
  };

  /// 账号换了 / 清掉时把缓存作废（plugin.js 在保存账号后调用）
  ns.reset = function () {
    cache.keyword = "";
    cache.request = -1;
    cache.state = "idle";
    cache.items = [];
    cache.error = "";
    cache.at = 0;
  };

  /// 给 `probe` 用的摘要
  ns.stats = function () {
    return {
      state: cache.state,
      keyword: cache.keyword,
      request: cache.request,
      count: cache.items.length,
      error: cache.error,
      at: cache.at
    };
  };

})(FtpSearch);

/* ==================== 注册搜索类型 ==================== */

/// 一份内容 = 一个搜索类型：`title` 是标签名，`order` 决定排在内置类型之后的哪个位置。
/// `deps` 里的两个变量缺一不可：少了哪个，点搜索时插件都收不到新关键词。
musicxx.ui.content.register({
  id: "search",
  slot: "musicxx.ui.search.type",
  title: "FTP 音乐",
  depict: "在 FTP 账号的目录里按文件名搜索（最多 3 层，只认音频后缀）",
  order: 30,
  deps: [
    "scope:musicxx.search.keyword",
    "scope:musicxx.search.request",
    "scope:musicxx.search.type"
  ]
}, function (ctx) {
  return FtpSearch.view(ctx);
});
