/*
 * example_js_net —— 插件框架内置网络能力示例
 *
 * 演示七件事（全部在本机回环上完成，不访问外网）:
 *   1. HTTP 服务端：`musicxx.net.http.serve` 起服务，`http.onRequest` 收请求，`http.respond` 回答；
 *   2. 静态路由：`http.serve({static})` / `http.route` 挂内容，命中后由框架直接回答；
 *   3. HTTP 客户端：`musicxx.net.http.request` 请求刚起的服务，检查状态码与回包正文；
 *   4. TCP 服务：`musicxx.net.socket.listen` 起服务，`socket.on("accepted"/"data")` 收发；
 *   5. TCP 客户端：`musicxx.net.socket.connect` 连上去，`socket.send` 发字节，等服务端回写；
 *   6. WebSocket 服务：`musicxx.net.ws.listen` 起服务，`ws.on("message")` 收到文本后原样回写；
 *   7. WebSocket 客户端：`musicxx.net.ws.connect` 连上去，`ws.send` 发文本，等回包。
 *
 * 插件载入 1.5 秒后自动跑一遍自检，结果写进日志；也可以通过能力手动触发：
 *   - `selftest`：跑一遍（返回报告 JSON，`ok` 是结论）；
 *   - `status`：上一次自检的报告；
 *   - `info`：`musicxx.net.info()` 的服务 / 连接快照。
 *
 * 要知道的限制：
 * - 客户端支持 http / https / ws / wss（TLS 由插件框架静态链接的 OpenSSL 完成）；
 *   服务端只监听明文（插件自己起服务时不做 TLS）；
 * - `musicxx.net.fetch` / `download` 与这里的 http 客户端是同一套实现，自检里一并跑一遍；
 * - 服务端默认只监听 127.0.0.1（写 `host: "0.0.0.0"` 才会暴露给同网段）；
 * - 服务与连接都跟着插件实例：停用 / 卸载时自动关掉（这里也会主动关，方便重复跑）。
 */
(function () {
  "use strict";

  /// 本示例的固定 id（重复跑自检时用同一组，先关再开）
  var HTTP_ID = "demo-http";
  var TCP_ID = "demo-tcp";
  var TCP_CLIENT_ID = "demo-tcp-client";
  var WS_ID = "demo-ws";
  var WS_CLIENT_ID = "demo-ws-client";

  var net = {
    http: null,
    socketServer: null,
    wsServer: null,
    httpHandled: 0,
    socketServerConn: "",
    wsServerConn: "",
    last: null,
    error: ""
  };

  /// 手写分块响应用: 每条连接只回一次（连接 id → 已回过）
  var chunkedAnswered = {};

  /// 等待中的事件（key → 一次性的处理函数）
  var waiters = {};

  function fireEvent(name, event) {
    var fn = waiters[name];
    if (!fn) {
      return;
    }
    waiters[name] = null;
    fn(event);
  }

  function nextEvent(name, timeoutMs) {
    return new Promise(function (resolve, reject) {
      var timer = setTimeout(function () {
        if (waiters[name] === handler) {
          waiters[name] = null;
        }
        reject(new Error("等待 " + name + " 超时"));
      }, timeoutMs || 5000);

      function handler(event) {
        clearTimeout(timer);
        resolve(event);
      }

      waiters[name] = handler;
    });
  }

  function describe(result) {
    if (!result) {
      return "没有结果";
    }
    var text = String(result.error || "unknown");
    if (result.message) {
      text += " - " + result.message;
    }
    return text;
  }

  /* ==================== 事件处理（只注册一次） ==================== */

  // HTTP 服务端：每个请求回一份 JSON，把方法 / 路径 / 查询串原样带回去
  musicxx.net.http.onRequest(function (event) {
    net.httpHandled++;
    musicxx.net.http.respond(event.requestId, {
      status: 200,
      contentType: "application/json; charset=utf-8",
      body: JSON.stringify({
        server: "example_js_net",
        method: event.method,
        path: event.path,
        query: event.query
      })
    });
  });

  // TCP 客户端连上以后才会有 data 事件
  musicxx.net.socket.on("connected", function (event) {
    fireEvent("socket.connected", event);
  });

  // 服务端接受了连接（服务端连接用 `serverId` 区分：同一个事件两种来源）
  musicxx.net.socket.on("accepted", function (event) {
    net.socketServerConn = event.id;
    fireEvent("socket.accepted", event);
  });

  musicxx.net.socket.on("data", function (event) {
    if (event.serverId === "demo-chunked") {
      // 手写一份分块响应（测试 HTTP 客户端的 chunked 解码）
      if (!chunkedAnswered[event.id]) {
        chunkedAnswered[event.id] = true;
        musicxx.net.socket.send(event.id,
          "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n" +
          "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n" +
          "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
      }
      return;
    }
    if (event.serverId) {
      // 服务端连接：收到什么就回 `echo:` + 原文
      musicxx.net.socket.send(event.id, "echo:" + (event.text || ""));
      return;
    }
    fireEvent("socket.data", event);
  });

  // WebSocket 握手完成（服务端连接带 serverId；客户端连接带 url）
  musicxx.net.ws.on("open", function (event) {
    if (event.serverId) {
      net.wsServerConn = event.id;
      return;
    }
    fireEvent("ws.open", event);
  });

  musicxx.net.ws.on("message", function (event) {
    if (event.serverId) {
      musicxx.net.ws.send(event.id, "echo:" + (event.text || ""));
      return;
    }
    fireEvent("ws.message", event);
  });

  /* ==================== 自检 ==================== */

  async function testHttp() {
    musicxx.net.http.stop(HTTP_ID); // 上一次可能没关干净
    var served = musicxx.net.http.serve({ id: HTTP_ID, port: 0 });
    if (!served.ok) {
      throw new Error("http.serve 失败: " + describe(served));
    }
    net.http = served;

    var before = net.httpHandled;
    var response = await musicxx.net.http.request({
      url: served.url + "/hello?name=musicxx",
      timeoutMs: 5000
    });

    var data = {};
    try {
      data = JSON.parse(response.body || "{}");
    } catch (e) {
      data = {};
    }
    if (response.status !== 200) {
      throw new Error("HTTP 状态码是 " + response.status);
    }
    if (data.query !== "name=musicxx" || data.path !== "/hello") {
      throw new Error("HTTP 回包内容不符合预期: " + (response.body || ""));
    }
    return {
      url: served.url,
      status: response.status,
      method: data.method,
      query: data.query,
      handled: net.httpHandled - before
    };
  }

  /// fetch / download 是把同一个 HTTP 实现再包一层：
  /// `musicxx.net.fetch` 只取正文, `musicxx.net.download` 直接落到插件自己的数据目录。
  async function testFetchDownload() {
    musicxx.net.http.stop(HTTP_ID + "-fetch");
    var served = musicxx.net.http.serve({ id: HTTP_ID + "-fetch", port: 0 });
    if (!served.ok) {
      throw new Error("http.serve 失败: " + describe(served));
    }
    try {
      var fetched = await musicxx.net.fetch({
        url: served.url + "/fetch?name=musicxx",
        timeoutMs: 5000
      });
      if (!fetched.ok || fetched.status !== 200) {
        throw new Error("fetch 状态码不对: " + fetched.status);
      }
      var parsed = {};
      try { parsed = JSON.parse(fetched.body || "{}"); } catch (e) { parsed = {}; }
      if (parsed.path !== "/fetch") {
        throw new Error("fetch 回包内容不符合预期: " + (fetched.body || ""));
      }
      var downloaded = await musicxx.net.download({
        url: served.url + "/download",
        fileName: "selftest.bin",
        timeoutMs: 5000
      });
      if (!downloaded.ok || !downloaded.path || downloaded.bytes <= 0) {
        throw new Error("download 失败: " + describe(downloaded));
      }
      return {
        status: fetched.status,
        contentType: fetched.contentType,
        bodyBytes: fetched.bodyBytes,
        receivedBytes: fetched.receivedBytes,
        downloadBytes: downloaded.bytes,
        downloadPath: downloaded.path
      };
    } finally {
      musicxx.net.http.stop(HTTP_ID + "-fetch");
    }
  }

  /// 静态路由：把内存内容 / 本地文件挂到 HTTP 服务上
  ///
  /// 命中后由插件框架直接回答（200 / 206 / 416），**不进 `http.onRequest`** —— 这条就是
  /// "把播放器打不开的东西变成 http 地址"的短路径。`staticOnly: true` 让没命中的请求直接 404，
  /// 不用等插件回答。
  async function testStatic() {
    var id = HTTP_ID + "-static";
    musicxx.net.http.stop(id);
    var payload = "#EXTM3U\n#EXT-X-VERSION:3\n";
    var served = musicxx.net.http.serve({
      id: id,
      port: 0,
      staticOnly: true,
      static: [{
        bytes: payload,
        contentType: "text/plain; charset=utf-8",
        fileName: "hello.txt"
      }]
    });
    if (!served.ok) {
      throw new Error("http.serve(static) 失败: " + describe(served));
    }
    var url = served.routes[0].url;
    try {
      var full = await musicxx.net.fetch({ url: url, timeoutMs: 5000 });
      if (full.status !== 200 || full.body !== payload) {
        throw new Error("静态内容整段取回不对: " + full.status + " / " +
                        String(full.bodyBytes));
      }
      var ranged = await musicxx.net.http.request({
        url: url,
        headers: { Range: "bytes=0-6" },
        timeoutMs: 5000
      });
      if (ranged.status !== 206 || ranged.body !== "#EXTM3U") {
        throw new Error("范围请求不对: " + ranged.status + " / " + ranged.body);
      }
      var beyond = await musicxx.net.http.request({
        url: url,
        headers: { Range: "bytes=99999-" },
        timeoutMs: 5000
      });
      if (beyond.status !== 416) {
        throw new Error("越界范围应当回 416, 实际 " + beyond.status);
      }

      // 再挂一条（显式路径）再摘掉：摘掉后同一个地址就是 404
      var added = musicxx.net.http.route({
        serverId: id,
        path: "/extra.bin",
        bytes: "abcd",
        fileName: "extra.bin"
      });
      if (!added.ok) {
        throw new Error("http.route 失败: " + describe(added));
      }
      var extra = await musicxx.net.fetch({ url: added.url, timeoutMs: 5000 });
      if (extra.status !== 200 || extra.body !== "abcd") {
        throw new Error("追加的静态路由取回不对: " + extra.status);
      }
      var removed = musicxx.net.http.unroute({
        serverId: id,
        path: added.path
      });
      if (!removed.ok || removed.removed !== true) {
        throw new Error("http.unroute 失败: " + describe(removed));
      }
      var gone = await musicxx.net.fetch({ url: added.url, timeoutMs: 5000 });
      if (gone.status !== 404) {
        throw new Error("摘掉路由后应当回 404, 实际 " + gone.status);
      }

      // 带路径前缀的服务：路由的 path 要带上前缀，拼出来的 URL 不能重复前缀
      var prefixedId = id + "-prefixed";
      musicxx.net.http.stop(prefixedId);
      var prefixedServer = musicxx.net.http.serve({
        id: prefixedId,
        port: 0,
        path: "/api",
        staticOnly: true
      });
      if (!prefixedServer.ok) {
        throw new Error("带前缀的 http.serve 失败: " + describe(prefixedServer));
      }
      var prefixed = { status: 0, url: "" };
      try {
        var autoRoute = musicxx.net.http.route({
          serverId: prefixedId,
          bytes: "prefixed",
          fileName: "p.txt"
        });
        if (!autoRoute.ok || autoRoute.path.indexOf("/api/f/") !== 0) {
          throw new Error("自动生成的路径应当带前缀: " + describe(autoRoute));
        }
        prefixed.url = autoRoute.url;
        var fetchedPrefixed = await musicxx.net.fetch({
          url: autoRoute.url,
          timeoutMs: 5000
        });
        prefixed.status = fetchedPrefixed.status;
        if (fetchedPrefixed.status !== 200 || fetchedPrefixed.body !== "prefixed") {
          throw new Error("带前缀的静态路由取回不对: " + fetchedPrefixed.status +
                          " / " + autoRoute.url);
        }
      } finally {
        musicxx.net.http.stop(prefixedId);
      }
      return {
        url: url,
        status: full.status,
        contentType: full.contentType,
        bodyBytes: full.bodyBytes,
        rangeStatus: ranged.status,
        beyondStatus: beyond.status,
        removed: removed.removed,
        goneStatus: gone.status,
        prefixedStatus: prefixed.status
      };
    } finally {
      musicxx.net.http.stop(id);
    }
  }

  async function testSocket() {
    musicxx.net.socket.stop(TCP_ID);
    var server = musicxx.net.socket.listen({ id: TCP_ID, port: 0 });
    if (!server.ok) {
      throw new Error("socket.listen 失败: " + describe(server));
    }
    net.socketServer = server;

    var connected = nextEvent("socket.connected");
    var accepted = nextEvent("socket.accepted");
    var client = musicxx.net.socket.connect({
      id: TCP_CLIENT_ID,
      host: "127.0.0.1",
      port: server.port,
      timeoutMs: 5000
    });
    if (!client.ok) {
      throw new Error("socket.connect 失败: " + describe(client));
    }
    var clientInfo = await connected;
    var serverInfo = await accepted;

    var dataPromise = nextEvent("socket.data");
    var sent = musicxx.net.socket.send(TCP_CLIENT_ID, "ping");
    if (!sent.ok) {
      throw new Error("socket.send 失败: " + describe(sent));
    }
    var message = await dataPromise;
    if (message.text !== "echo:ping") {
      throw new Error("TCP 回包不是 echo:ping: " + (message.text || ""));
    }
    return {
      port: server.port,
      clientRemote: clientInfo.remote,
      serverConn: serverInfo.id,
      echo: message.text,
      bytes: message.bytes
    };
  }

  /// 用 TCP 服务手写一份分块响应，验证 HTTP 客户端的 chunked 解码
  async function testChunked() {
    musicxx.net.socket.stop("demo-chunked");
    chunkedAnswered = {};
    var server = musicxx.net.socket.listen({ id: "demo-chunked", port: 0 });
    if (!server.ok) {
      throw new Error("socket.listen 失败: " + describe(server));
    }
    var response = await musicxx.net.http.request({
      url: "http://127.0.0.1:" + server.port + "/chunked",
      timeoutMs: 5000
    });
    if (response.status !== 200 || response.body !== "hello world") {
      throw new Error("分块响应解码不对: " + (response.body || ""));
    }
    return { status: response.status, body: response.body, bytes: response.bodyBytes };
  }

  async function testWebSocket() {
    musicxx.net.ws.stop(WS_ID);
    var server = musicxx.net.ws.listen({ id: WS_ID, port: 0, path: "/chat" });
    if (!server.ok) {
      throw new Error("ws.listen 失败: " + describe(server));
    }
    net.wsServer = server;

    var opened = nextEvent("ws.open");
    var client = musicxx.net.ws.connect(
      "ws://127.0.0.1:" + server.port + "/chat",
      { id: WS_CLIENT_ID, timeoutMs: 5000 }
    );
    if (!client.ok) {
      throw new Error("ws.connect 失败: " + describe(client));
    }
    var openEvent = await opened;

    var messagePromise = nextEvent("ws.message");
    var sent = musicxx.net.ws.send(WS_CLIENT_ID, "ping");
    if (!sent.ok) {
      throw new Error("ws.send 失败: " + describe(sent));
    }
    var message = await messagePromise;
    if (message.text !== "echo:ping") {
      throw new Error("WebSocket 回包不是 echo:ping: " + (message.text || ""));
    }
    return {
      url: server.url,
      path: openEvent.path,
      echo: message.text,
      bytes: message.bytes
    };
  }

  function cleanup() {
    musicxx.net.ws.close(WS_CLIENT_ID, 1000, "自检结束");
    musicxx.net.ws.stop(WS_ID);
    musicxx.net.socket.close(TCP_CLIENT_ID);
    musicxx.net.socket.stop(TCP_ID);
    musicxx.net.socket.stop("demo-chunked");
    musicxx.net.http.stop(HTTP_ID);
  }

  /// https 客户端: 公网地址由外部给 (离线环境不跑)
  ///
  /// 传进来的地址不会被重定向到别的主机也能拿到状态码就证明: TLS 握手 + 证书链校验
  /// (系统根证书) + 主机名校验都过了。
  async function testHttps(url) {
    var fetched = await musicxx.net.fetch({ url: url, timeoutMs: 20000 });
    return {
      url: url,
      status: fetched.status,
      contentType: fetched.contentType,
      bodyBytes: fetched.bodyBytes,
      receivedBytes: fetched.receivedBytes
    };
  }

  async function doSelfTest(options) {
    cleanup();
    var startedAt = Date.now();
    var report = { ok: true };
    try {
      report.http = await testHttp();
      report.static = await testStatic();
      report.fetch = await testFetchDownload();
      report.socket = await testSocket();
      report.chunked = await testChunked();
      report.ws = await testWebSocket();
      var httpsUrl = (options && typeof options.httpsUrl === "string") ? options.httpsUrl : "";
      if (httpsUrl) {
        report.https = await testHttps(httpsUrl);
      }
    } catch (e) {
      report.ok = false;
      report.error = String(e && e.message ? e.message : e);
    } finally {
      cleanup();
    }
    report.ms = Date.now() - startedAt;
    return report;
  }

  /// 同一时间只跑一份（自动自检与手动调用会撞上）
  var running = null;

  function runSelfTest(options) {
    if (running) {
      return running;
    }
    running = doSelfTest(options).then(function (report) {
      running = null;
      net.last = report;
      net.error = report.ok ? "" : (report.error || "未知原因");
      return report;
    }, function (err) {
      running = null;
      var text = String(err && err.message ? err.message : err);
      net.last = { ok: false, error: text };
      net.error = text;
      return net.last;
    });
    return running;
  }

  /* ==================== 对外能力 ==================== */

  /// 跑一遍自检：{ok, http:{...}, fetch:{...}, socket:{...}, ws:{...}, ms} / {ok:false, error}
  /// 可选参数 `{httpsUrl}`: 给了就额外跑一次 https 客户端 (公网/自建 TLS 服务)
  musicxx.capability.register("selftest", function (args) {
    return runSelfTest(args || {});
  });

  /// 上一次自检的报告（还没跑过时给 ok:false）
  musicxx.capability.register("status", function () {
    return net.last || { ok: false, error: "还没跑过自检" };
  });

  /// 服务 / 连接 / 上限快照（插件框架的网络能力自省）
  musicxx.capability.register("info", function () {
    return musicxx.net.info();
  });

  /* ==================== 载入后自动跑一遍 ==================== */

  musicxx.timer.setTimeout(function () {
    runSelfTest().then(function (report) {
      if (report.ok) {
        musicxx.host.log(2, "[example_js_net] 自检通过: " + JSON.stringify(report));
      } else {
        musicxx.host.log(4, "[example_js_net] 自检失败: " + (report.error || "未知原因"));
      }
    });
  }, 1500);
})();
