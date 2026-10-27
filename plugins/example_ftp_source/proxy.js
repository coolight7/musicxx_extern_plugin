/*
 * proxy.js —— 本机 HTTP 转流服务
 *
 * 播放器只认 http(s) 与本地文件，而 FTP 谁也打不开（见 docs/plugin-api.md §8.6）。
 * 这个文件在 127.0.0.1 上开一个很小的 HTTP 服务，把播放器发来的请求变成一次 FTP 下载，
 * 再把数据接回播放器的连接。播放器看到的始终是干净的 `http://127.0.0.1:<port>/s/<token>`：
 * 没有账号、没有密码、地址里也没有真实路径。
 *
 * 为什么字节要经过 JS 转发（框架没有 socket 桥接）：
 * 插件框架的 socket 能力只给了 connect / listen / send / pause，没有"两端对接"。所以这里用
 * 发送队列长度做背压：`socket.send` 的结果里有 `queuedBytes`，队列长了就 `socket.pause` 暂停
 * FTP 那一路的读，缓过来再恢复。音频数据确实会过一遍 JS 线程（每片一个 base64 字符串），
 * 这是示例能做到的最简单写法；需要更快时应当由插件框架提供桥接（见 README 的“限制”）。
 *
 * 几条必须守住的规则：
 * - 每条连接只服务一个请求，响应总是 `Connection: close`（播放器与缓存流都会重连）；
 * - `Range` 只处理单区间；服务器不支持 REST 时退回 200 从头发（播放器能接受）；
 * - 不知道文件大小时不写 `Content-Length`，靠关连接结束（缓存与拖动会差一些，播放不受影响）；
 * - 服务器发完 150 就开始推数据，所以数据连接的处理器要在发 RETR 之前登记
 *   （`FtpSource.openRetr` 的 `sink` 参数就是为这个留的），请求头则要等拿到真实起始偏移再写。
 */

var FtpSource = globalThis.FtpSource || (globalThis.FtpSource = {});

(function (ns) {
  "use strict";

  /* ==================== 常量 ==================== */

  /// 转流服务只监听回环：端口由系统分配，只有本机能访问
  var LISTEN_HOST = "127.0.0.1";
  /// 同时进行的转发（播放 + 缓存流 + 拖动一般不超过 3 条）
  var MAX_PARALLEL = 6;
  /// token 表上限：超过就丢最久没用的（一个用户的歌单也到不了这么多）
  var MAX_TOKENS = 4096;
  /// 发送队列高 / 低水位：超过高水位暂停 FTP 读，降到低水位恢复
  var HIGH_WATER = 1024 * 1024;
  var LOW_WATER = 256 * 1024;
  /// 背压轮询间隔
  var POLL_MS = 50;
  /// 请求头最多收这么多（超了回 431）
  var HEAD_LIMIT = 16 * 1024;

  /* ==================== 状态 ==================== */

  var state = {
    /// 当前账号（由 plugin.js 在读到 / 保存账号时同步过来，用来校验 token 属于哪个账号）
    account: null,
    /// 监听的服务 id（空 = 还没启动）
    serverId: "",
    port: 0,
    /// token -> {key, fingerprint, path, size, name, at}
    tokens: {},
    /// `指纹|路径` -> token（同一个文件重复解析拿同一个地址）
    tokenOfKey: {},
    /// token 的分配顺序（超上限时从最旧的开始丢）
    order: [],
    /// 收到的 HTTP 请求数
    requests: 0,
    /// 正在转发的响应数
    active: 0,
    lastError: ""
  };

  /// 正在服务的客户端连接：id -> {id, buffer, streaming}
  var clients = {};

  /* ==================== 内容类型 ==================== */

  var contentTypes = {
    mp3: "audio/mpeg", flac: "audio/flac", wav: "audio/wav", aac: "audio/aac",
    m4a: "audio/mp4", m4b: "audio/mp4", alac: "audio/mp4", mp4: "video/mp4",
    ogg: "audio/ogg", oga: "audio/ogg", opus: "audio/ogg",
    wma: "audio/x-ms-wma", ape: "audio/x-ape", wv: "audio/x-wavpack",
    tta: "audio/x-tta", aiff: "audio/aiff", aif: "audio/aiff",
    dsf: "audio/dsf", dff: "audio/dff"
  };

  function contentTypeOf(name) {
    var text = String(name || "");
    var at = text.lastIndexOf(".");
    if (at < 0 || at === text.length - 1) {
      return "application/octet-stream";
    }
    return contentTypes[text.substring(at + 1).toLowerCase()] || "application/octet-stream";
  }

  /* ==================== token 表 ==================== */

  function randomToken() {
    var text = "";
    while (text.length < 32) {
      var part = Math.floor(Math.random() * 0x100000000).toString(16);
      while (part.length < 8) {
        part = "0" + part;
      }
      text += part;
    }
    return text.substring(0, 32);
  }

  function forgetToken(token) {
    var entry = state.tokens[token];
    if (!entry) {
      return;
    }
    delete state.tokens[token];
    if (state.tokenOfKey[entry.key] === token) {
      delete state.tokenOfKey[entry.key];
    }
  }

  /// 超过上限时把最旧的 token 丢掉（正在转发的响应自己拿着 entry，不受影响）
  function trimTokens() {
    while (state.order.length > MAX_TOKENS) {
      forgetToken(state.order.shift());
    }
  }

  function clearTokens() {
    state.tokens = {};
    state.tokenOfKey = {};
    state.order = [];
  }

  /* ==================== 监听 ==================== */

  /// 启动转流服务（只启动一次；之后一直开着，端口不变才能让已经发出去的地址继续有效）
  function ensureServer() {
    if (state.serverId && state.port > 0) {
      return Promise.resolve({ id: state.serverId, port: state.port });
    }
    var res = musicxx.net.socket.listen({
      id: "ftp-proxy",
      host: LISTEN_HOST,
      port: 0,
      maxConnections: 16
    });
    if (!res || res.ok !== true) {
      state.lastError = "启动本机转流服务失败：" + ns.describe(res);
      return Promise.reject(new Error(state.lastError));
    }
    state.serverId = res.id;
    state.port = Number(res.port) || 0;
    if (!(state.port > 0)) {
      state.lastError = "本机转流服务没有拿到端口";
      return Promise.reject(new Error(state.lastError));
    }
    ns.net.register(res.id, { onAccepted: onAccepted });
    musicxx.host.log(2, "[proxy] 本机转流服务已启动：" + LISTEN_HOST + ":" + state.port);
    return Promise.resolve({ id: state.serverId, port: state.port });
  }

  /* ==================== 对外接口 ==================== */

  /// 换账号：指纹变了就丢掉旧 token（旧歌的地址本来也就不该再能播）
  ns.setAccount = function (account) {
    var previous = state.account ? ns.accountFingerprint(state.account) : "";
    state.account = account || null;
    var current = state.account ? ns.accountFingerprint(state.account) : "";
    if (previous !== current) {
      clearTokens();
    }
  };

  /// 给 `(账号, 路径)` 一个播放地址（重复调用返回同一个地址）
  ///
  /// `size` 知道就给：它决定能不能写 `Content-Length`（缓存与拖动都要）。
  ns.serve = function (account, path, size) {
    var target = ns.normalizePath(path);
    return ensureServer().then(function (info) {
      var fingerprint = ns.accountFingerprint(account);
      var key = fingerprint + "|" + target;
      var token = state.tokenOfKey[key];
      var entry = token ? state.tokens[token] : null;
      if (!entry) {
        token = randomToken();
        entry = {
          key: key,
          fingerprint: fingerprint,
          path: target,
          size: Number(size) > 0 ? Number(size) : 0,
          name: ns.basename(target),
          at: Date.now()
        };
        state.tokens[token] = entry;
        state.tokenOfKey[key] = token;
        state.order.push(token);
        trimTokens();
      } else if (Number(size) > 0) {
        entry.size = Number(size);
      }
      entry.at = Date.now();
      return "http://" + LISTEN_HOST + ":" + info.port + "/s/" + token;
    });
  };

  /// 自省数据（`probe` 能力用）
  ns.stats = function () {
    return {
      running: !!(state.serverId && state.port > 0),
      port: state.port,
      tokens: state.order.length,
      requests: state.requests,
      active: state.active,
      lastError: state.lastError
    };
  };

  /// 插件停止时收尾：关掉所有客户端连接与监听（框架也会关，这里主动做是为了不留计时器）
  ns.stop = function () {
    for (var id in clients) {
      if (!Object.prototype.hasOwnProperty.call(clients, id)) {
        continue;
      }
      var ctx = clients[id];
      if (ctx && ctx.streaming && ctx.streaming.stop) {
        ctx.streaming.stop();
      }
      ns.net.unregister(id);
      musicxx.net.socket.close(id);
    }
    clients = {};
    if (state.serverId) {
      musicxx.net.socket.stop(state.serverId);
      state.serverId = "";
    }
    state.port = 0;
    state.active = 0;
    clearTokens();
  };

  /* ==================== 请求解析 ==================== */

  var reasons = {
    200: "OK",
    206: "Partial Content",
    404: "Not Found",
    405: "Method Not Allowed",
    416: "Range Not Satisfiable",
    431: "Request Header Fields Too Large",
    502: "Bad Gateway",
    503: "Service Unavailable"
  };

  function headText(status, headers) {
    var lines = ["HTTP/1.1 " + status + " " + (reasons[status] || "OK")];
    for (var i = 0; i < headers.length; i++) {
      lines.push(headers[i]);
    }
    lines.push("Connection: close");
    lines.push("");
    lines.push("");
    return lines.join("\r\n");
  }

  function sendText(clientId, text) {
    var res = musicxx.net.socket.send({ id: clientId, data: text });
    return !!(res && res.ok === true);
  }

  /// 回一个短文本响应并关连接（错误路径都走这里）
  function respondText(ctx, status, text, extra) {
    var body = String(text || "") + "\n";
    var bytes = [];
    for (var i = 0; i < body.length; i++) {
      var code = body.charCodeAt(i);
      bytes.push(code < 0x80 ? code : 0x3f); // 非 ASCII 一律写成 '?'：错误文本只给人看
    }
    var headers = [
      "Content-Type: text/plain; charset=utf-8",
      "Content-Length: " + bytes.length,
      "Cache-Control: no-store"
    ];
    if (extra) {
      for (var key in extra) {
        if (Object.prototype.hasOwnProperty.call(extra, key)) {
          headers.push(key + ": " + extra[key]);
        }
      }
    }
    var text2 = "";
    for (var at = 0; at < bytes.length; at++) {
      text2 += String.fromCharCode(bytes[at]);
    }
    sendText(ctx.id, headText(status, headers) + text2);
    ns.net.unregister(ctx.id);
    musicxx.net.socket.close(ctx.id);
  }

  function onAccepted(event) {
    var ctx = { id: event.id, buffer: "", streaming: null };
    clients[event.id] = ctx;
    ns.net.register(event.id, {
      onData: function (dataEvent) {
        onClientData(ctx, dataEvent);
      },
      onClose: function () {
        onClientClose(ctx);
      }
    });
  }

  function onClientClose(ctx) {
    delete clients[ctx.id];
    if (ctx.streaming && ctx.streaming.stop) {
      ctx.streaming.stop();
      ctx.streaming = null;
    }
  }

  function onClientData(ctx, event) {
    if (ctx.streaming) {
      // 已经进入转发：这个请求不会再发东西进来，多出来的字节直接丢掉
      return;
    }
    var text = (typeof event.text === "string") ? event.text : null;
    if (text === null) {
      text = ns.decodeText([event.dataBase64], "utf-8") || "";
    }
    ctx.buffer += text;
    var end = ctx.buffer.indexOf("\r\n\r\n");
    if (end < 0) {
      if (ctx.buffer.length > HEAD_LIMIT) {
        respondText(ctx, 431, "请求头太大");
      }
      return;
    }
    var head = ctx.buffer.substring(0, end);
    ctx.buffer = "";
    handleHead(ctx, head);
  }

  function handleHead(ctx, head) {
    var lines = head.split("\r\n");
    var requestLine = String(lines[0] || "").split(" ");
    var method = String(requestLine[0] || "").toUpperCase();
    var target = String(requestLine[1] || "");
    var headers = {};
    for (var i = 1; i < lines.length; i++) {
      var at = lines[i].indexOf(":");
      if (at > 0) {
        headers[lines[i].substring(0, at).trim().toLowerCase()] = lines[i].substring(at + 1).trim();
      }
    }
    state.requests++;

    if (method !== "GET" && method !== "HEAD") {
      respondText(ctx, 405, "只支持 GET / HEAD", { Allow: "GET, HEAD" });
      return;
    }
    if (state.active >= MAX_PARALLEL) {
      respondText(ctx, 503, "同时播放的请求太多，请稍后再试", { "Retry-After": "1" });
      return;
    }

    var path = target.split("?")[0];
    var segments = path.split("/");
    var parts = [];
    for (var index = 0; index < segments.length; index++) {
      if (segments[index]) {
        parts.push(segments[index]);
      }
    }
    if (parts.length < 2 || parts[0] !== "s") {
      respondText(ctx, 404, "路径不对");
      return;
    }
    var token = parts[1];
    var entry = state.tokens[token];
    if (!entry) {
      respondText(ctx, 404, "播放地址已失效，请重新从歌单打开这首歌");
      return;
    }
    var account = state.account;
    if (!ns.accountReady(account)) {
      respondText(ctx, 503, "还没有配置 FTP 账号");
      return;
    }
    if (ns.accountFingerprint(account) !== entry.fingerprint) {
      respondText(ctx, 404, "这首歌不是当前账号添加的");
      return;
    }
    entry.at = Date.now();

    var name = parts.length > 2 ? decodeSafe(parts[2]) : entry.name;
    startTransfer(ctx, entry, account, headers.range, method === "HEAD", name);
  }

  function decodeSafe(text) {
    try {
      return decodeURIComponent(String(text || ""));
    } catch (e) {
      return String(text || "");
    }
  }

  /* ==================== Range ==================== */

  /// 解析单区间 Range：返回 null（不分段）/ {start, end} / {unsatisfiable:true}
  ///
  /// `size` 未知（0）时只处理 `bytes=a-`：结尾要等数据发完才知道，所以 end 给 -1。
  function parseRange(value, size) {
    var text = String(value || "").trim();
    if (!text) {
      return null;
    }
    var matched = /^bytes=(.*)$/i.exec(text);
    if (!matched) {
      return null;
    }
    var spec = matched[1].trim();
    if (spec.indexOf(",") >= 0) {
      // 多区间：主程序的缓存流不会发，这里按整段回（合法且简单）
      return null;
    }
    var at = spec.indexOf("-");
    if (at < 0) {
      return null;
    }
    var total = Number(size) > 0 ? Number(size) : 0;
    var fromText = spec.substring(0, at).trim();
    var toText = spec.substring(at + 1).trim();
    if (fromText === "") {
      var suffix = Number(toText);
      if (!(suffix > 0) || total <= 0) {
        return null;
      }
      var from = total - suffix;
      if (from < 0) {
        from = 0;
      }
      return { start: from, end: total - 1 };
    }
    var start = Number(fromText);
    if (!isFinite(start) || start < 0) {
      return null;
    }
    if (total > 0 && start >= total) {
      return { unsatisfiable: true };
    }
    if (toText === "") {
      return { start: start, end: total > 0 ? total - 1 : -1 };
    }
    var end = Number(toText);
    if (!isFinite(end)) {
      return null;
    }
    if (total > 0 && end >= total) {
      end = total - 1;
    }
    if (end < start) {
      return { unsatisfiable: true };
    }
    return { start: start, end: end };
  }

  /* ==================== 转发 ==================== */

  /// 转发一个请求：算范围 → 开 FTP 下载 → 写响应头 → 把数据片送到播放器
  ///
  /// 返回的 transfer 上有 `stop()`：客户端断开 / 插件停止 / 出错时都由它收尾（幂等）。
  function startTransfer(ctx, entry, account, rangeHeader, headOnly, name) {
    state.active++;
    var transfer = {
      finished: false,
      /** waiting = 等响应头（数据先攒着）→ streaming = 正常转发 → done */
      phase: "waiting",
      pending: [],
      handle: null,
      bytes: 0,
      expected: -1,
      poll: 0,
      paused: false,
      /// 数据连接在响应头发出之前就关了（零字节文件 / 服务器立刻断开）
      dataClosed: false
    };

    function stopPoll() {
      if (transfer.poll) {
        musicxx.timer.clear(transfer.poll);
        transfer.poll = 0;
      }
    }

    transfer.stop = function () {
      if (transfer.finished) {
        return;
      }
      transfer.finished = true;
      stopPoll();
      if (transfer.handle) {
        transfer.handle.abort();
        transfer.handle = null;
      }
      state.active = Math.max(0, state.active - 1);
    };

    function closeClient() {
      ns.net.unregister(ctx.id);
      delete clients[ctx.id];
      musicxx.net.socket.close(ctx.id);
    }

    /// 结束这次转发：先关 FTP 那边，再关客户端（客户端看到连接关闭 = 数据结束）
    function finish() {
      transfer.stop();
      closeClient();
    }

    function fail(status, message) {
      state.lastError = message;
      if (transfer.phase === "waiting") {
        respondText(ctx, status, message);
        ctx.streaming = null;
        transfer.stop();
        return;
      }
      // 头已经发出去了：只能关连接（播放器会按错误重试或换下一首）
      finish();
    }

    /// 把一片数据写给播放器；队列长了就暂停 FTP 那一路
    function pump(event) {
      if (transfer.finished || !event || !event.dataBase64) {
        return;
      }
      var res = musicxx.net.socket.send({ id: ctx.id, dataBase64: event.dataBase64 });
      if (!res || res.ok !== true) {
        // 播放器那头没了（拖动、切歌、暂停后关流都会这样）
        transfer.stop();
        closeClient();
        return;
      }
      transfer.bytes += Number(event.bytes) || 0;
      var queued = Number(res.queuedBytes) || 0;
      if (!transfer.paused && queued > HIGH_WATER) {
        transfer.paused = true;
        if (transfer.handle && transfer.handle.dataId) {
          musicxx.net.socket.pause(transfer.handle.dataId, true);
        }
        startPoll();
      }
    }

    /// 发送队列的水位
    function queuedBytesOf(id) {
      var info = musicxx.net.socket.info();
      var rows = (info && info.connections) ? info.connections : [];
      for (var i = 0; i < rows.length; i++) {
        if (rows[i].id === id) {
          return Number(rows[i].queuedBytes) || 0;
        }
      }
      return -1;
    }

    /// 队列降到低水位就恢复 FTP 读（插件框架没有“队列排空”事件，只能轮询）
    function startPoll() {
      if (transfer.poll) {
        return;
      }
      transfer.poll = musicxx.timer.setInterval(function () {
        if (transfer.finished) {
          stopPoll();
          return;
        }
        var queued = queuedBytesOf(ctx.id);
        if (queued < 0) {
          transfer.stop();
          closeClient();
          return;
        }
        if (queued <= LOW_WATER) {
          stopPoll();
          transfer.paused = false;
          if (transfer.handle && transfer.handle.dataId) {
            musicxx.net.socket.pause(transfer.handle.dataId, false);
          }
        }
      }, POLL_MS);
    }

    /// 数据连接收完（服务器发完文件 / 断开）
    var sink = {
      onData: function (event) {
        if (transfer.phase === "waiting") {
          transfer.pending.push(event);
          return;
        }
        pump(event);
      },
      onClose: function () {
        if (transfer.phase === "waiting") {
          // 响应头还没发（可能是一开始就结束的空文件）：先记下来，发完头再收尾
          transfer.dataClosed = true;
          return;
        }
        if (transfer.expected >= 0 && transfer.bytes < transfer.expected) {
          state.lastError = "FTP 数据连接提前结束（" + transfer.bytes + "/" + transfer.expected + " 字节）";
        }
        finish();
      }
    };

    /// 响应头 + 开始转发（服务器如果不支持 REST，这里退回 200 从头发）
    function startRetr(total, start) {
      FtpSource.openRetr(account, entry.path, start, sink).then(function (handle) {
        if (transfer.finished) {
          handle.abort();
          return;
        }
        transfer.handle = handle;
        var actualStart = handle.start > 0 ? handle.start : 0;
        var headers = [
          "Content-Type: " + contentTypeOf(name || entry.name),
          "Accept-Ranges: bytes",
          "Cache-Control: no-store"
        ];
        var status = 200;
        if (total > 0 && actualStart > 0 && actualStart < total) {
          status = 206;
          transfer.expected = total - actualStart;
          headers.push("Content-Range: bytes " + actualStart + "-" + (total - 1) + "/" + total);
          headers.push("Content-Length: " + transfer.expected);
        } else if (total > 0) {
          transfer.expected = total;
          headers.push("Content-Length: " + total);
        } else {
          // 大小未知：不写 Content-Length，靠关连接结束
          transfer.expected = -1;
        }
        if (!sendText(ctx.id, headText(status, headers))) {
          transfer.stop();
          closeClient();
          return;
        }
        if (headOnly) {
          finish();
          return;
        }
        transfer.phase = "streaming";
        var pending = transfer.pending;
        transfer.pending = [];
        for (var i = 0; i < pending.length; i++) {
          if (transfer.finished) {
            break;
          }
          pump(pending[i]);
        }
        if (transfer.dataClosed && !transfer.finished) {
          // 数据连接在发头之前就结束了：头已经发出，直接收尾
          finish();
        }
      }, function (error) {
        fail(502, "连接 FTP 服务器失败：" + (error && error.message ? error.message : String(error)));
      });
    }

    /// 拿到文件大小后决定从哪儿开始
    function proceed(range, size) {
      var total = Number(size) > 0 ? Number(size) : 0;
      var start = 0;
      if (range && range.start > 0) {
        if (total <= 0) {
          // 大小未知时不能给「部分内容」：200 必须是从头开始的完整内容。
          // 这种服务器不支持 SIZE，也就没法拖动（跟 README 里写的一致）。
          start = 0;
        } else if (range.start >= total) {
          fail(416, "请求的范围超出文件大小");
          return;
        } else {
          start = range.start;
        }
      }
      startRetr(total, start);
    }

    function begin() {
      var range = parseRange(rangeHeader, entry.size);
      if (range && range.unsatisfiable) {
        fail(416, "请求的范围超出文件大小");
        return;
      }
      if (entry.size > 0) {
        proceed(range, entry.size);
        return;
      }
      // 目录列表没带大小（LIST 格式）：问一次 SIZE；服务器不支持就按未知处理
      FtpSource.size(account, entry.path).then(function (value) {
        if (transfer.finished) {
          return;
        }
        if (value > 0) {
          entry.size = value;
        }
        proceed(range, value > 0 ? value : 0);
      }, function () {
        if (!transfer.finished) {
          proceed(range, 0);
        }
      });
    }

    ctx.streaming = transfer;
    begin();
    return transfer;
  }
})(FtpSource);
