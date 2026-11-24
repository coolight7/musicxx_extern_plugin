/*
 * testserver.js —— 内置的测试用 FTP 服务端（只监听回环，只读）
 *
 * 为什么有这个东西：这个示例插件是 FTP **客户端**，要手动试一整条链路
 * （列目录 → 加歌单 → 播放 → 搜索）就得先有一台 FTP 服务器。原来靠
 * `python -m pyftpdlib` 手搭，现在插件自己起一个：在账号页打开开关、选一个本地目录，
 * 它就是一台只读的 FTP 服务器，拿 `127.0.0.1:<端口>` + 页面上的用户名密码当账号即可。
 *
 * 它实现的是 FTP 的一个小子集（够这个示例自己以及常见客户端用）：
 *
 *   USER / PASS / SYST / FEAT / OPTS / NOOP / TYPE / PWD / CWD / CDUP / QUIT
 *   PASV / EPSV（被动模式）；LIST / NLST / MLSD（列目录）；SIZE / MDTM；REST / RETR（下载）
 *
 * 明确不做：主动模式（PORT / EPRT）、写操作（MKD / RMD / DELE / RNFR / RNTO 一律 502）、
 * TLS（FTPS）、多用户与权限。目录树被限制在开关里选的那个目录内：虚拟路径里的 `..`
 * 在根上就停住，所以拿它读不到外层文件。
 *
 * 文件内容从主程序的只读能力 `musicxx.fs.list` / `musicxx.fs.stat` / `musicxx.fs.read`
 * 读：JS 插件自己没有任何文件系统接口（见 docs/plugin-js-api.md §3），这几个能力是主程序
 * 提供的那一层。一次读 256 KiB，`socket.send` 的 `queuedBytes` 超过高水位就等队列降下来
 * （与 proxy.js 的背压同一套写法），所以大文件也能陆续发出去。
 *
 * 生命周期：开关打开时启动，关掉 / 插件停用 / 卸载时由 `ns.stop()`（以及插件框架）关掉，
 * 端口随即释放。只在本机回环上监听，局域网里的设备连不上。
 */

var FtpTestServer = globalThis.FtpTestServer || (globalThis.FtpTestServer = {});

(function (ns) {
  "use strict";

  /* ==================== 常量 ==================== */

  /// 只监听回环：这是"自己测试自己"用的服务，不应该暴露到局域网
  var LISTEN_HOST = "127.0.0.1";
  /// 一次从 fs.read 读多少（再大就是一次几百 KB 的 base64 在 Dart 与 JS 之间来回拷）
  var CHUNK = 256 * 1024;
  /// 发送队列高 / 低水位：超过高水位先不发下一片，降到低水位继续
  var HIGH_WATER = 1024 * 1024;
  var LOW_WATER = 256 * 1024;
  var POLL_MS = 50;
  /// 控制连接最多同时几条（数据连接另算，每条控制连接最多开一个）
  var MAX_CONNECTIONS = 8;
  /// 客户端发了 PASV 却迟迟不连数据连接时的等待时间
  var DATA_WAIT_MS = 5000;
  /// 一次控制命令最多攒这么多字节（正常一行就几十字节）
  var CONTROL_LIMIT = 8 * 1024;

  /* ==================== 状态 ==================== */

  var state = {
    /// 控制监听的 socket id（空 = 没在跑）
    serverId: "",
    port: 0,
    /// 挂载的本机目录（绝对路径，'/' 分隔）
    rootPath: "",
    user: "test",
    password: "123456",
    running: false,
    startedAt: 0,
    /// 统计（`probe` / 账号页显示，排查用）
    /// `sessions` 是**当前**还活着的控制连接数（与 MAX_CONNECTIONS 比较的就是它），
    /// `totalSessions` 是开服以来的累计条数：客户端每做一件事开一条、做完 QUIT，
    /// 累计值增长很快，不要拿它去比上限。
    sessions: 0,
    totalSessions: 0,
    commands: 0,
    transfers: 0,
    bytesSent: 0,
    lastError: ""
  };

  /// 控制连接：id → ctx
  var clients = {};
  /// 数据监听：serverId → ctx（每条控制连接最多一个）
  var listeners = {};
  /// 自增 id：数据监听的 socket id 用
  var nextId = 0;

  function log(text) {
    musicxx.host.log(2, "[ftp-test] " + text);
  }

  function noteError(text) {
    var line = String(text || "").slice(0, 240);
    state.lastError = line;
    musicxx.host.log(3, "[ftp-test] " + line);
  }

  function describe(result) {
    if (FtpSource && typeof FtpSource.describe === "function") {
      return FtpSource.describe(result);
    }
    return result ? JSON.stringify(result) : "null";
  }

  /* ==================== 文件能力（只读） ==================== */

  /// 调一条主程序的文件能力；失败也回对象（不复用异常）
  function fsCall(name, args) {
    return musicxx.capability.call(name, args).then(function (result) {
      if (!result || typeof result !== "object") {
        return { ok: false, error: "no_result" };
      }
      return result;
    }, function (err) {
      return { ok: false, error: (err && err.message) ? err.message : String(err) };
    });
  }

  /* ==================== 路径 ==================== */

  /// 虚拟路径（以 / 开头、不含 . 与 ..）→ 本机真实路径
  ///
  /// 只做拼接：虚拟路径在 [normalizeVirtual] 里已经收进根目录内，这里再加一道兜底。
  function realPathOf(virtualPath) {
    var base = String(state.rootPath || "").replace(/\\/g, "/");
    while (base.length > 1 && base.charAt(base.length - 1) === "/") {
      base = base.substring(0, base.length - 1);
    }
    return (virtualPath === "/") ? base : (base + virtualPath);
  }

  /// 命令里的路径 → 以 / 开头、不含 . 与 .. 的虚拟路径（以 cwd 为基准）
  function normalizeVirtual(input, cwd) {
    var text = String(input === undefined || input === null ? "" : input).replace(/\\/g, "/");
    var base = (text.charAt(0) === "/") ? "" : String(cwd || "/");
    var parts = (base + "/" + text).split("/");
    var out = [];
    for (var i = 0; i < parts.length; i++) {
      var item = parts[i];
      if (item === "" || item === ".") {
        continue;
      }
      if (item === "..") {
        // 到根就停住：不允许靠 .. 走出挂载目录
        if (out.length > 0) {
          out.pop();
        }
        continue;
      }
      out.push(item);
    }
    return "/" + out.join("/");
  }

  /// 名字里的控制字符会让 FTP 行断开：列目录时把 CR/LF 换成空格
  function safeName(name) {
    return String(name === undefined || name === null ? "" : name).replace(/[\r\n]/g, " ");
  }

  /* ==================== 控制应答 ==================== */

  /// 发一行或多行应答（多行以 250- 开头、250 结尾，调用方自己给完整文本）
  function reply(ctx, text) {
    if (!ctx || !ctx.id) {
      return;
    }
    musicxx.net.socket.send({ id: ctx.id, data: text + "\r\n" });
  }

  function replyCode(ctx, code, text) {
    reply(ctx, code + " " + text);
  }

  /* ==================== 数据连接 ==================== */

  /// 关掉这条控制连接的数据监听（不影响已经建立的数据连接）
  function closeDataListener(listenerId) {
    if (!listenerId) {
      return;
    }
    var entry = listeners[listenerId];
    if (entry) {
      delete listeners[listenerId];
      if (FtpSource.net && FtpSource.net.unregister) {
        FtpSource.net.unregister(listenerId);
      }
      musicxx.net.socket.stop(listenerId);
    }
  }

  /// 开一个被动模式的数据监听，返回端口
  function openDataListener(ctx) {
    closeDataListener(ctx.dataListenerId);
    var id = "ftp-test-data-" + (++nextId);
    var res = musicxx.net.socket.listen({
      id: id,
      host: LISTEN_HOST,
      port: 0,
      maxConnections: 2
    });
    if (!res || res.ok !== true) {
      noteError("数据监听失败：" + describe(res));
      return 0;
    }
    var port = Number(res.port) || 0;
    if (!(port > 0)) {
      noteError("数据监听没有拿到端口");
      closeDataListener(id);
      return 0;
    }
    log("数据监听：" + LISTEN_HOST + ":" + port);
    ctx.dataListenerId = id;
    ctx.dataClientId = "";
    ctx.dataWaiters = [];
    listeners[id] = ctx;
    FtpSource.net.register(id, {
      onAccepted: function (event) {
        onDataAccepted(ctx, event);
      }
    });
    return port;
  }

  function onDataAccepted(ctx, event) {
    if (!ctx || ctx.closed) {
      musicxx.net.socket.close(event.id);
      return;
    }
    ctx.dataClientId = event.id;
    // 客户端可能在 RETR / LIST 之前就连上，也可能之后才连：两种都在这条路径上处理
    FtpSource.net.register(event.id, {
      onData: function () {
        // 数据连接上客户端不会再发东西（ABOR 走控制连接），收到的字节直接丢掉
      },
      onClose: function () {
        if (ctx.dataClientId === event.id) {
          ctx.dataClientId = "";
        }
      }
    });
    var waiters = ctx.dataWaiters;
    ctx.dataWaiters = [];
    for (var i = 0; i < waiters.length; i++) {
      waiters[i](event.id);
    }
  }

  /// 等数据连接就绪（超时给错误，调用方回 425）
  function waitForData(ctx) {
    if (ctx.dataClientId) {
      return Promise.resolve(ctx.dataClientId);
    }
    return new Promise(function (resolve, reject) {
      var timer = 0;
      var done = false;
      function finish(id) {
        if (done) {
          return;
        }
        done = true;
        if (timer) {
          musicxx.timer.clear(timer);
          timer = 0;
        }
        resolve(id);
      }
      ctx.dataWaiters.push(finish);
      timer = musicxx.timer.setTimeout(function () {
        if (done) {
          return;
        }
        done = true;
        reject(new Error("客户端没有连上数据连接"));
      }, DATA_WAIT_MS);
    });
  }

  /// 收尾一条数据连接：`close` 会先把排队的字节写完再关（框架保证）
  function finishData(ctx) {
    var dataId = ctx.dataClientId;
    ctx.dataClientId = "";
    closeDataListener(ctx.dataListenerId);
    ctx.dataListenerId = "";
    if (dataId) {
      if (FtpSource.net && FtpSource.net.unregister) {
        FtpSource.net.unregister(dataId);
      }
      musicxx.net.socket.close(dataId);
    }
  }

  /// 发送队列的水位（拿不到连接时返回 -1 = 连接没了）
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

  /// 等发送队列降到低水位（大文件把队列堆满时用）
  function waitDrain(id) {
    return new Promise(function (resolve, reject) {
      var timer = musicxx.timer.setInterval(function () {
        var queued = queuedBytesOf(id);
        if (queued < 0) {
          musicxx.timer.clear(timer);
          reject(new Error("数据连接已经关闭"));
          return;
        }
        if (queued <= LOW_WATER) {
          musicxx.timer.clear(timer);
          resolve(null);
        }
      }, POLL_MS);
    });
  }

  /* ==================== 传输 ==================== */

  /// 把一段文本发到数据连接（`150` 已经回过之后调用）
  function sendDataText(dataId, text) {
    var res = musicxx.net.socket.send({ id: dataId, data: text });
    return !!(res && res.ok === true);
  }

  /// 列目录（LIST / NLST / MLSD），`mlsd` 为真时按机器可读格式
  function transferList(ctx, virtualPath, mlsd, namesOnly) {
    var real = realPathOf(virtualPath);
    return fsCall("musicxx.fs.list", { path: real }).then(function (result) {
      if (!result.ok) {
        finishData(ctx);
        replyCode(ctx, 550, "打不开目录：" + String(result.error || ""));
        return null;
      }
      var entries = result.entries || [];
      var lines = [];
      for (var i = 0; i < entries.length; i++) {
        var entry = entries[i];
        var name = safeName(entry.name);
        if (!name) {
          continue;
        }
        if (namesOnly) {
          lines.push(name);
          continue;
        }
        if (mlsd) {
          lines.push(mlsdLineOf(entry, name));
        } else {
          lines.push(listLineOf(entry, name));
        }
      }
      return waitForData(ctx).then(function (dataId) {
        if (!sendDataText(dataId, lines.length ? (lines.join("\r\n") + "\r\n") : "")) {
          finishData(ctx);
          replyCode(ctx, 426, "数据连接断了");
          return null;
        }
        state.transfers += 1;
        finishData(ctx);
        replyCode(ctx, 226, "列目录完成（" + lines.length + " 项）");
        return null;
      });
    });
  }

  /// MLSD 的一行：`type=file;size=123;modify=20260101120000; name`
  function mlsdLineOf(entry, name) {
    var facts = entry.isDir ? "type=dir" : "type=file;size=" + String(entry.size || 0);
    var modified = Number(entry.modifiedMs) || 0;
    if (modified > 0) {
      facts += ";modify=" + stampOf(modified);
    }
    return facts + "; " + name;
  }

  /// LIST 的一行：`-rw-r--r-- 1 ftp ftp <大小> <时间> <名字>`
  function listLineOf(entry, name) {
    var mode = entry.isDir ? "drwxr-xr-x" : "-rw-r--r--";
    var size = String(entry.isDir ? 0 : (Number(entry.size) || 0));
    var modified = Number(entry.modifiedMs) || 0;
    var at = modified > 0 ? new Date(modified) : new Date();
    return mode + " 1 ftp ftp " + size + " " + monthName(at) + " " +
      (at.getDate() < 10 ? "0" : "") + at.getDate() + " " +
      (at.getHours() < 10 ? "0" : "") + at.getHours() + ":" +
      (at.getMinutes() < 10 ? "0" : "") + at.getMinutes() + " " + name;
  }

  var MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"];

  function monthName(date) {
    return MONTHS[date.getMonth()] || "Jan";
  }

  /// `20260101120000`（MLSD 的 modify 用的是 UTC）
  function stampOf(milliseconds) {
    var date = new Date(milliseconds);
    function pad(value) {
      return value < 10 ? ("0" + value) : String(value);
    }
    return String(date.getUTCFullYear()) + pad(date.getUTCMonth() + 1) +
      pad(date.getUTCDate()) + pad(date.getUTCHours()) +
      pad(date.getUTCMinutes()) + pad(date.getUTCSeconds());
  }

  /// 下载（RETR）：从 `ctx.rest` 开始，一片一片读到数据连接上
  function transferRetr(ctx, virtualPath) {
    var real = realPathOf(virtualPath);
    return fsCall("musicxx.fs.stat", { path: real }).then(function (stat) {
      if (!stat.ok || stat.exists !== true || stat.isFile !== true) {
        finishData(ctx);
        replyCode(ctx, 550, "文件不存在：" + virtualPath);
        return null;
      }
      var offset = Math.max(0, Number(ctx.rest) || 0);
      ctx.rest = 0;
      state.transfers += 1;
      return waitForData(ctx).then(function (dataId) {
        replyCode(ctx, 150, "开始发送 " + virtualPath);
        return pumpChunks(ctx, real, offset, dataId);
      });
    });
  }

  /// 一片一片读 → 发 → 队列长了就等（递归靠 Promise，不会把栈压深）
  function pumpChunks(ctx, real, offset, dataId) {
    return fsCall("musicxx.fs.read", {
      path: real,
      offset: offset,
      size: CHUNK
    }).then(function (chunk) {
      if (!chunk.ok) {
        finishData(ctx);
        replyCode(ctx, 451, "读文件失败：" + String(chunk.error || ""));
        return null;
      }
      if (chunk.bytes > 0) {
        var res = musicxx.net.socket.send({ id: dataId, dataBase64: chunk.data });
        if (!res || res.ok !== true) {
          finishData(ctx);
          return null;
        }
        state.bytesSent += Number(chunk.bytes) || 0;
        var queued = Number(res.queuedBytes) || 0;
        if (queued > HIGH_WATER) {
          return waitDrain(dataId).then(function () {
            return chunk.eof ? finishRetr(ctx) : pumpChunks(ctx, real, offset + chunk.bytes, dataId);
          }, function () {
            finishData(ctx);
            return null;
          });
        }
      }
      if (chunk.eof || chunk.bytes <= 0) {
        return finishRetr(ctx);
      }
      return pumpChunks(ctx, real, offset + chunk.bytes, dataId);
    });
  }

  function finishRetr(ctx) {
    finishData(ctx);
    replyCode(ctx, 226, "发送完成");
    return null;
  }

  /* ==================== 命令 ==================== */

  function replyNotImplemented(ctx, command) {
    replyCode(ctx, 502, "这个测试服务端不实现 " + command);
  }

  /// 处理一行控制命令
  function handleLine(ctx, line) {
    if (!line) {
      return;
    }
    state.commands += 1;
    var at = line.indexOf(" ");
    var command = (at < 0 ? line : line.substring(0, at)).toUpperCase();
    var argument = at < 0 ? "" : line.substring(at + 1);
    switch (command) {
      case "USER":
        ctx.pendingUser = argument.trim();
        replyCode(ctx, 331, "请输入密码");
        return;
      case "PASS":
        if (argument === state.password) {
          ctx.authenticated = true;
          replyCode(ctx, 230, "登录成功");
        } else {
          ctx.authenticated = false;
          replyCode(ctx, 530, "用户名或密码不对");
        }
        return;
      case "SYST":
        replyCode(ctx, 215, "UNIX Type: L8");
        return;
      case "FEAT":
        reply(ctx, "211-Features:");
        reply(ctx, " UTF8");
        reply(ctx, " MLSD");
        reply(ctx, " MLST type*;size*;modify*;");
        reply(ctx, " SIZE");
        reply(ctx, " MDTM");
        reply(ctx, " REST STREAM");
        reply(ctx, " EPSV");
        replyCode(ctx, 211, "End");
        return;
      case "OPTS":
        replyCode(ctx, 200, "OK");
        return;
      case "NOOP":
        replyCode(ctx, 200, "OK");
        return;
      case "TYPE": {
        var type = argument.trim().toUpperCase();
        if (type === "I" || type === "A" || type === "L 8") {
          replyCode(ctx, 200, "已切换传输类型");
        } else {
          replyCode(ctx, 504, "不支持的传输类型：" + type);
        }
        return;
      }
      case "PWD":
      case "XPWD":
        replyCode(ctx, 257, "\"" + ctx.cwd + "\"");
        return;
      case "CWD":
      case "XCWD": {
        if (!ctx.authenticated) {
          replyCode(ctx, 530, "请先登录");
          return;
        }
        var target = normalizeVirtual(argument, ctx.cwd);
        var real = realPathOf(target);
        fsCall("musicxx.fs.stat", { path: real }).then(function (stat) {
          if (stat.ok && stat.exists === true && stat.isDir === true) {
            ctx.cwd = target;
            replyCode(ctx, 250, "目录已切换");
          } else {
            replyCode(ctx, 550, "目录不存在：" + target);
          }
        });
        return;
      }
      case "CDUP":
      case "XCUP": {
        var parent = normalizeVirtual("..", ctx.cwd);
        var parentReal = realPathOf(parent);
        fsCall("musicxx.fs.stat", { path: parentReal }).then(function (stat) {
          if (stat.ok && stat.exists === true && stat.isDir === true) {
            ctx.cwd = parent;
            replyCode(ctx, 250, "目录已切换");
          } else {
            replyCode(ctx, 550, "已经在根目录");
          }
        });
        return;
      }
      case "PASV": {
        var port = openDataListener(ctx);
        if (!(port > 0)) {
          replyCode(ctx, 425, "开不了数据端口");
          return;
        }
        replyCode(ctx, 227, "Entering Passive Mode (127,0,0,1," +
          Math.floor(port / 256) + "," + (port % 256) + ")");
        return;
      }
      case "EPSV": {
        var port2 = openDataListener(ctx);
        if (!(port2 > 0)) {
          replyCode(ctx, 425, "开不了数据端口");
          return;
        }
        replyCode(ctx, 229, "Entering Extended Passive Mode (|||" + port2 + "|)");
        return;
      }
      case "PORT":
      case "EPRT":
        replyCode(ctx, 502, "这个测试服务端只支持被动模式（PASV / EPSV）");
        return;
      case "LIST":
      case "NLST":
      case "MLSD": {
        if (!ctx.authenticated) {
          replyCode(ctx, 530, "请先登录");
          return;
        }
        var listPath = normalizeVirtual(argument.trim(), ctx.cwd);
        var real2 = realPathOf(listPath);
        fsCall("musicxx.fs.stat", { path: real2 }).then(function (stat) {
          if (!stat.ok || stat.exists !== true) {
            finishData(ctx);
            replyCode(ctx, 550, "目录不存在：" + listPath);
            return null;
          }
          var dirPath = stat.isDir === true ? listPath : normalizeVirtual("..", listPath);
          replyCode(ctx, 150, "开始列目录");
          return transferList(ctx, dirPath, command === "MLSD", command === "NLST");
        });
        return;
      }
      case "SIZE": {
        if (!ctx.authenticated) {
          replyCode(ctx, 530, "请先登录");
          return;
        }
        var sizePath = normalizeVirtual(argument.trim(), ctx.cwd);
        fsCall("musicxx.fs.stat", { path: realPathOf(sizePath) }).then(function (stat) {
          if (stat.ok && stat.exists === true && stat.isFile === true) {
            replyCode(ctx, 213, String(stat.size || 0));
          } else {
            replyCode(ctx, 550, "取不到大小：" + sizePath);
          }
        });
        return;
      }
      case "MDTM": {
        var timePath = normalizeVirtual(argument.trim(), ctx.cwd);
        fsCall("musicxx.fs.stat", { path: realPathOf(timePath) }).then(function (stat) {
          if (stat.ok && stat.exists === true) {
            replyCode(ctx, 213, stampOf(Number(stat.modifiedMs) || Date.now()));
          } else {
            replyCode(ctx, 550, "取不到修改时间：" + timePath);
          }
        });
        return;
      }
      case "REST": {
        var value = Number(argument.trim());
        if (isFinite(value) && value >= 0) {
          ctx.rest = Math.floor(value);
          replyCode(ctx, 350, "从第 " + ctx.rest + " 字节开始");
        } else {
          replyCode(ctx, 501, "REST 需要一个非负整数");
        }
        return;
      }
      case "RETR": {
        if (!ctx.authenticated) {
          replyCode(ctx, 530, "请先登录");
          return;
        }
        var target = normalizeVirtual(argument.trim(), ctx.cwd);
        transferRetr(ctx, target);
        return;
      }
      case "ABOR":
        finishData(ctx);
        replyCode(ctx, 226, "已取消");
        return;
      case "QUIT":
        replyCode(ctx, 221, "再见");
        closeClient(ctx);
        return;
      case "MKD":
      case "XDMD":
      case "RMD":
      case "XRMD":
      case "DELE":
      case "RNFR":
      case "RNTO":
      case "STOR":
      case "APPE":
        replyCode(ctx, 502, "测试服务端只读，不改文件");
        return;
      default:
        replyNotImplemented(ctx, command);
    }
  }

  /* ==================== 连接 ==================== */

  function onAccepted(event) {
    if (!state.serverId || event.serverId !== state.serverId) {
      // 不是这个监听来的连接（例如别的插件）——不理会
      return;
    }
    if (state.sessions >= MAX_CONNECTIONS) {
      musicxx.net.socket.send({ id: event.id, data: "421 连接太多，稍后再试\r\n" });
      musicxx.net.socket.close(event.id);
      return;
    }
    var ctx = {
      id: event.id,
      buffer: "",
      authenticated: false,
      pendingUser: "",
      cwd: "/",
      rest: 0,
      dataListenerId: "",
      dataClientId: "",
      dataWaiters: [],
      closed: false
    };
    clients[event.id] = ctx;
    state.sessions += 1;
    state.totalSessions += 1;
    FtpSource.net.register(event.id, {
      onData: function (dataEvent) {
        onControlData(ctx, dataEvent);
      },
      onClose: function () {
        closeClient(ctx);
      }
    });
    replyCode(ctx, 220, "musicxx FTP 测试服务端（只读）");
  }

  function onControlData(ctx, event) {
    if (ctx.closed) {
      return;
    }
    var text = (typeof event.text === "string") ? event.text : null;
    if (text === null) {
      text = FtpSource.decodeText([event.dataBase64], "utf-8") || "";
    }
    ctx.buffer += text;
    if (ctx.buffer.length > CONTROL_LIMIT && ctx.buffer.indexOf("\n") < 0) {
      replyCode(ctx, 500, "命令行太长");
      ctx.buffer = "";
      return;
    }
    var index = ctx.buffer.indexOf("\n");
    while (index >= 0) {
      var line = ctx.buffer.substring(0, index);
      ctx.buffer = ctx.buffer.substring(index + 1);
      if (line.charAt(line.length - 1) === "\r") {
        line = line.substring(0, line.length - 1);
      }
      handleLine(ctx, line);
      index = ctx.buffer.indexOf("\n");
    }
  }

  function closeClient(ctx) {
    if (!ctx || ctx.closed) {
      return;
    }
    ctx.closed = true;
    delete clients[ctx.id];
    // 连接关掉就把占用的名额还回去（`sessions` 是“现在有几条”）：
    // 漏了这一步的话，累计开满 MAX_CONNECTIONS 条以后新连接全被 421 拒掉
    if (state.sessions > 0) {
      state.sessions -= 1;
    }
    closeDataListener(ctx.dataListenerId);
    ctx.dataListenerId = "";
    if (ctx.dataClientId) {
      if (FtpSource.net && FtpSource.net.unregister) {
        FtpSource.net.unregister(ctx.dataClientId);
      }
      musicxx.net.socket.close(ctx.dataClientId);
      ctx.dataClientId = "";
    }
    if (FtpSource.net && FtpSource.net.unregister) {
      FtpSource.net.unregister(ctx.id);
    }
    musicxx.net.socket.close(ctx.id);
  }

  /* ==================== 对外接口 ==================== */

  /// 启动服务：`config = {rootPath, port, user, password}`
  ///
  /// `port` 为 0（或没给）时由系统分配；账号密码为空时用 test / 123456。
  ns.start = function (config) {
    var options = config || {};
    if (state.running) {
      return Promise.resolve(ns.status());
    }
    var rootPath = String(options.rootPath || "").trim();
    if (!rootPath) {
      return Promise.reject(new Error("没有设置要挂载的目录"));
    }
    state.rootPath = rootPath.replace(/\\/g, "/");
    state.user = String(options.user || "test");
    state.password = String(options.password || "123456");
    var wanted = Math.max(0, Math.min(65535, intOf(options.port, 0)));
    var res = musicxx.net.socket.listen({
      id: "ftp-test-server",
      host: LISTEN_HOST,
      port: wanted,
      maxConnections: MAX_CONNECTIONS
    });
    if (!res || res.ok !== true) {
      state.lastError = "启动测试 FTP 服务端失败：" + describe(res);
      return Promise.reject(new Error(state.lastError));
    }
    state.serverId = res.id;
    state.port = Number(res.port) || 0;
    state.running = true;
    state.startedAt = Date.now();
    state.lastError = "";
    FtpSource.net.register(res.id, { onAccepted: onAccepted });
    log("测试 FTP 服务端已启动：" + LISTEN_HOST + ":" + state.port +
      "，根目录 " + state.rootPath);
    return Promise.resolve(ns.status());
  };

  /// 停掉服务（幂等）：关掉所有控制 / 数据连接与监听
  ns.stop = function () {
    var ids = [];
    for (var id in clients) {
      if (Object.prototype.hasOwnProperty.call(clients, id)) {
        ids.push(id);
      }
    }
    for (var i = 0; i < ids.length; i++) {
      closeClient(clients[ids[i]]);
    }
    clients = {};
    var listenerIds = [];
    for (var key in listeners) {
      if (Object.prototype.hasOwnProperty.call(listeners, key)) {
        listenerIds.push(key);
      }
    }
    for (var j = 0; j < listenerIds.length; j++) {
      closeDataListener(listenerIds[j]);
    }
    listeners = {};
    if (state.serverId) {
      if (FtpSource.net && FtpSource.net.unregister) {
        FtpSource.net.unregister(state.serverId);
      }
      musicxx.net.socket.stop(state.serverId);
      state.serverId = "";
    }
    if (state.running) {
      log("测试 FTP 服务端已停止");
    }
    state.running = false;
    state.port = 0;
    state.startedAt = 0;
    // 上面的 closeClient 会逐个减，这里再置 0 兑底（迟到的 onClose 不再重复减）
    state.sessions = 0;
    return Promise.resolve(ns.status());
  };

  /// 当前状态（账号页与 `probe` 都用它）
  ns.status = function () {
    return {
      running: !!state.running,
      port: state.port,
      host: LISTEN_HOST,
      rootPath: state.rootPath,
      user: state.user,
      passwordSet: !!state.password,
      startedAt: state.startedAt,
      sessions: state.sessions,
      totalSessions: state.totalSessions,
      commands: state.commands,
      transfers: state.transfers,
      bytesSent: state.bytesSent,
      lastError: state.lastError
    };
  };

  /// 改挂载目录 / 账号（服务在跑时重启一次让新配置生效）
  ns.apply = function (config) {
    var options = config || {};
    var wasRunning = state.running;
    return ns.stop().then(function () {
      if (wasRunning) {
        return ns.start(options);
      }
      state.rootPath = String(options.rootPath || "").replace(/\\/g, "/");
      state.user = String(options.user || "test");
      state.password = String(options.password || "123456");
      return ns.status();
    });
  };

  /// 取整（非法值回退缺省）
  function intOf(value, fallback) {
    var number = Number(value);
    return isFinite(number) ? Math.round(number) : fallback;
  }

})(FtpTestServer);
