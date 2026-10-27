/*
 * ftp.js —— FTP 客户端（基于插件框架的 TCP 能力）
 *
 * 这个文件只做一件事：把 FTP 协议包成几个 Promise 方法 —— 建连、登录、列目录、
 * 取文件大小、开一条下载数据连接。界面与转流都在别的脚本里。
 *
 * 为什么用插件的 socket 能力：QuickJS 没有 socket，而播放器只认 http(s) 与本地文件，
 * 所以 FTP 必须由插件自己连、自己转。`musicxx.net.socket` 由插件框架实现（见
 * docs/plugin-api.md §3.2.6），JS 侧只需要处理事件与字节。
 *
 * 写这个文件时要记住的几件事：
 * - socket 的 `data` 事件是**分片**的：控制连接要自己攒行（按 \r\n），数据连接要把
 *   base64 片段攒起来最后一次性解码（中途解码会把多字节字符切断）；
 * - 文件名可能是 GBK：登录后先 OPTS UTF8 ON，再按账号里的 encoding 转；
 *   转换失败（老插件框架没有 musicxx.util.textEncode）时退化成“只认 UTF-8”；
 * - 每个请求各开一条控制连接（v1 简单可靠），并发由调用方（proxy.js）卡住。
 */

var FtpSource = globalThis.FtpSource || (globalThis.FtpSource = {});

(function (ns) {
  "use strict";

  /* ==================== socket 事件分发 ==================== */

  /// 连接 / 服务 id → 处理函数（同一个 id 上的事件都从这里进）
  ///
  /// 插件框架只有一个全局事件入口（`musicxx.net.socket.on(...)`），所以这里做一次分发：
  /// 客户端连接按 `id` 找处理器，服务端按 `serverId` 找（`accepted` 事件）。
  var handlers = {};

  function fire(targetId, name, event) {
    var handler = handlers[targetId];
    if (!handler || typeof handler[name] !== "function") {
      return;
    }
    try {
      handler[name](event);
    } catch (e) {
      // 处理函数里的异常不能冒到 JS 线程外面（会把插件的其它事件一起带崩）
      musicxx.host.log(4, "[ftp] " + name + " 处理异常: " + (e && e.message ? e.message : String(e)));
    }
  }

  ns.net = {
    handlers: handlers,
    /// 登记一个处理器（客户端连接与服务端共用；`id` 给服务端时按 serverId 收 accepted）
    register: function (id, handler) {
      if (!id) {
        return;
      }
      handlers[id] = handler;
    },
    unregister: function (id) {
      delete handlers[id];
    },
    has: function (id) {
      return !!handlers[id];
    }
  };

  musicxx.net.socket.on("connected", function (event) {
    fire(event.id, "onConnected", event);
  });
  musicxx.net.socket.on("data", function (event) {
    fire(event.id, "onData", event);
  });
  musicxx.net.socket.on("closed", function (event) {
    var handler = handlers[event.id];
    delete handlers[event.id];
    if (handler && typeof handler.onClose === "function") {
      try {
        handler.onClose(event);
      } catch (e) {
        musicxx.host.log(4, "[ftp] onClose 处理异常: " + (e && e.message ? e.message : String(e)));
      }
    }
  });
  musicxx.net.socket.on("accepted", function (event) {
    fire(event.serverId, "onAccepted", event);
  });

  /* ==================== 小工具 ==================== */

  /// 文本工具在不在（老插件框架没有 textEncode / textDecode：只能按 UTF-8 走）
  ns.hasCodec = !!(musicxx.util && typeof musicxx.util.textEncode === "function" &&
                   typeof musicxx.util.textDecode === "function");

  ns.describe = function (result) {
    if (!result) {
      return "没有结果";
    }
    var text = String(result.error || "unknown");
    if (result.message) {
      text += " - " + result.message;
    }
    return text;
  };

  /// 文本 → 账号编码的 base64（失败返回 null）
  ns.encodeText = function (text, encoding) {
    if (!ns.hasCodec) {
      return null;
    }
    var res = musicxx.util.textEncode({ text: text, encoding: encoding || "utf-8" });
    return (res && res.ok === true) ? res.base64 : null;
  };

  /// base64 片段（字符串或数组）→ 文本；失败返回 null
  ns.decodeText = function (chunks, encoding) {
    if (!ns.hasCodec) {
      return null;
    }
    var res = musicxx.util.textDecode({ base64: chunks, encoding: encoding || "utf-8" });
    return (res && res.ok === true) ? res.text : null;
  };

  /// 目录 / 文件路径：统一成“以 / 开头、不含 . 与 ..、不以 / 结尾”（根目录就是 "/"）
  ns.normalizePath = function (path) {
    var text = String(path === undefined || path === null ? "" : path).replace(/\\/g, "/");
    var parts = text.split("/");
    var out = [];
    for (var i = 0; i < parts.length; i++) {
      var item = parts[i];
      if (item === "" || item === ".") {
        continue;
      }
      if (item === "..") {
        out.pop();
        continue;
      }
      out.push(item);
    }
    return "/" + out.join("/");
  };

  /// 拼一个子路径（path 是目录；name 是名字）
  ns.joinPath = function (path, name) {
    var base = ns.normalizePath(path);
    if (base === "/") {
      return ns.normalizePath("/" + name);
    }
    return ns.normalizePath(base + "/" + name);
  };

  /// 上一级目录（已经在根上就还是根）
  ns.parentPath = function (path) {
    var base = ns.normalizePath(path);
    if (base === "/") {
      return "/";
    }
    var index = base.lastIndexOf("/");
    return index <= 0 ? "/" : base.substring(0, index);
  };

  ns.basename = function (path) {
    var base = ns.normalizePath(path);
    if (base === "/") {
      return "";
    }
    return base.substring(base.lastIndexOf("/") + 1);
  };

  /// 看起来像音频文件（“只看音频”过滤用）
  var audioExt = {};
  ("mp3 flac wav aac m4a ogg opus wma ape aiff aif alac dsd dff m4b mp4 oga wv tta")
    .split(" ").forEach(function (ext) { audioExt[ext] = true; });
  ns.isAudioName = function (name) {
    var text = String(name || "");
    var index = text.lastIndexOf(".");
    if (index < 0) {
      return false;
    }
    return !!audioExt[text.substring(index + 1).toLowerCase()];
  };

  ns.formatSize = function (bytes) {
    var value = Number(bytes) || 0;
    if (value <= 0) {
      return "";
    }
    if (value < 1024) {
      return value + " B";
    }
    if (value < 1024 * 1024) {
      return (value / 1024).toFixed(1) + " KB";
    }
    if (value < 1024 * 1024 * 1024) {
      return (value / (1024 * 1024)).toFixed(1) + " MB";
    }
    return (value / (1024 * 1024 * 1024)).toFixed(2) + " GB";
  };

  /// 建一条 TCP 连接（`connected` 事件到了才算连上）
  ns.openTcp = function (host, port, timeoutMs) {
    return new Promise(function (resolve, reject) {
      var res = musicxx.net.socket.connect({
        host: host,
        port: port,
        timeoutMs: timeoutMs || 10000
      });
      if (!res || res.ok !== true) {
        reject(new Error("连接 " + host + ":" + port + " 失败: " + ns.describe(res)));
        return;
      }
      var id = res.id;
      var timer = musicxx.timer.setTimeout(function () {
        ns.net.unregister(id);
        musicxx.net.socket.close(id);
        reject(new Error("连接 " + host + ":" + port + " 超时"));
      }, (timeoutMs || 10000) + 3000);
      ns.net.register(id, {
        onConnected: function () {
          musicxx.timer.clear(timer);
          resolve(id);
        },
        onClose: function (event) {
          musicxx.timer.clear(timer);
          reject(new Error("连接 " + host + ":" + port + " 被关闭: " +
                           (event.error || event.by || "")));
        }
      });
    });
  };

  /* ==================== 错误文本 ==================== */

  /// 常见应答码 → 给人看的原因（不记密码）
  var replyText = {
    421: "服务器暂时不可用（可能连接数已满）",
    425: "无法建立数据连接（被动模式端口可能被拦截）",
    426: "传输被中断",
    430: "登录信息无效",
    500: "服务器不接受这条命令",
    501: "命令参数有误",
    502: "服务器不支持这条命令",
    504: "服务器不支持这个参数",
    530: "登录失败（用户名或密码错误）",
    532: "保存文件需要权限",
    550: "文件不存在或没有权限",
    551: "不是文件",
    552: "空间不足",
    553: "文件名不合法"
  };

  function fail(cmd, reply) {
    var readable = replyText[reply.code] || "服务器拒绝了请求";
    var detail = String(reply.text || "").trim();
    var error = new Error(readable + "（" + reply.code +
                          (detail ? " " + detail : "") + "）");
    error.ftpCode = reply.code;
    error.ftpCommand = cmd;
    return error;
  }

  /* ==================== 控制连接 ==================== */

  /// 一条 FTP 控制连接
  ///
  /// - 按 `\r\n` 攒行（socket 事件是分片的）；
  /// - 应答可能是多行（`226-...` 一直到 `226 ...`）；
  /// - `command()` 发一条命令并等一条应答，应答码不在期望里就抛可读错误。
  function Control(account, socketId) {
    this.account = account;
    this.encoding = String(account.encoding || "utf-8").toLowerCase();
    this.timeoutMs = Number(account.timeoutMs) > 0 ? Number(account.timeoutMs) : 10000;
    this.id = socketId;
    this.buffer = "";
    this.lines = [];
    this.replies = [];
    this.waiters = [];
    this.multiCode = "";
    this.multiBody = [];
    this.closed = false;
    this.features = {};
    this.featureText = "";
    var self = this;
    ns.net.register(socketId, {
      onData: function (event) {
        self.onData(event);
      },
      onClose: function () {
        self.onClose();
      }
    });
  }

  Control.prototype.onData = function (event) {
    var text = (typeof event.text === "string") ? event.text : null;
    if (text === null) {
      // 不是合法 UTF-8（例如 PWD 回了一个 GBK 路径）：按账号编码解这一段
      text = ns.decodeText([event.dataBase64], this.encoding);
    }
    if (!text) {
      return;
    }
    this.buffer += text.replace(/\r\n/g, "\n");
    var index = this.buffer.indexOf("\n");
    while (index >= 0) {
      this.lines.push(this.buffer.substring(0, index));
      this.buffer = this.buffer.substring(index + 1);
      index = this.buffer.indexOf("\n");
    }
    this.pump();
  };

  Control.prototype.onClose = function () {
    this.closed = true;
    var waiters = this.waiters;
    this.waiters = [];
    for (var i = 0; i < waiters.length; i++) {
      waiters[i].reject(new Error("控制连接被关闭"));
    }
  };

  /// 尽量多地把攒到的行拼成完整应答
  Control.prototype.pump = function () {
    for (;;) {
      var reply = this.takeReply();
      if (!reply) {
        return;
      }
      var waiter = this.waiters.shift();
      if (waiter) {
        waiter.resolve(reply);
      } else if (this.replies.length < 8) {
        this.replies.push(reply);
      }
    }
  };

  Control.prototype.takeReply = function () {
    if (this.multiCode) {
      if (!this.lines.length) {
        return null;
      }
      var line = this.lines.shift();
      if (line === this.multiCode || line.indexOf(this.multiCode + " ") === 0) {
        var text = this.multiBody.join("\n");
        this.multiCode = "";
        this.multiBody = [];
        return { code: Number(line.substring(0, 3)), text: text, lines: [line] };
      }
      this.multiBody.push(line);
      return this.takeReply();
    }
    if (!this.lines.length) {
      return null;
    }
    var first = this.lines.shift();
    var matched = /^(\d{3})([ -])(.*)$/.exec(first);
    if (!matched) {
      // 不是应答行（有些服务器会夹欢迎横幅）：丢掉继续
      return this.takeReply();
    }
    if (matched[2] === "-") {
      this.multiCode = matched[1];
      this.multiBody = [matched[3]];
      return this.takeReply();
    }
    return { code: Number(matched[1]), text: matched[3], lines: [first] };
  };

  Control.prototype.awaitReply = function () {
    var self = this;
    return new Promise(function (resolve, reject) {
      if (self.replies.length) {
        resolve(self.replies.shift());
        return;
      }
      if (self.closed) {
        reject(new Error("控制连接已关闭"));
        return;
      }
      var waiter = { resolve: resolve, reject: reject };
      var timer = musicxx.timer.setTimeout(function () {
        var index = self.waiters.indexOf(waiter);
        if (index >= 0) {
          self.waiters.splice(index, 1);
        }
        reject(new Error("服务器没有在规定时间内应答（超时 " + self.timeoutMs + " ms）"));
      }, self.timeoutMs);
      var rawResolve = waiter.resolve;
      var rawReject = waiter.reject;
      waiter.resolve = function (value) {
        musicxx.timer.clear(timer);
        rawResolve(value);
      };
      waiter.reject = function (error) {
        musicxx.timer.clear(timer);
        rawReject(error);
      };
      self.waiters.push(waiter);
    });
  };

  Control.prototype.send = function (line) {
    var result;
    if (ns.hasCodec) {
      var payload = ns.encodeText(line + "\r\n", this.encoding);
      if (payload === null) {
        throw new Error("文本转 " + this.encoding + " 失败: " + line);
      }
      result = musicxx.net.socket.send({ id: this.id, dataBase64: payload });
    } else {
      result = musicxx.net.socket.send({ id: this.id, data: line + "\r\n" });
    }
    if (!result || result.ok !== true) {
      throw new Error("发送命令失败: " + ns.describe(result));
    }
  };

  /// 发命令 + 等应答；`expect` 给了就要求应答码在里
  Control.prototype.command = function (line, expect) {
    this.send(line);
    var read = this.awaitReply();
    if (!expect || !expect.length) {
      return read;
    }
    return read.then(function (reply) {
      if (expect.indexOf(reply.code) < 0) {
        throw fail(line, reply);
      }
      return reply;
    });
  };

  Control.prototype.close = function () {
    if (this.closed) {
      return;
    }
    this.closed = true;
    ns.net.unregister(this.id);
    musicxx.net.socket.close(this.id);
  };

  /// 礼貌地退出（QUIT 失败也无所谓：连接反正要关）
  Control.prototype.quit = function () {
    var self = this;
    if (this.closed) {
      return Promise.resolve();
    }
    return new Promise(function (resolve) {
      try {
        self.send("QUIT");
      } catch (e) {
        // 已经断开：直接收尾
      }
      musicxx.timer.setTimeout(function () {
        self.close();
        resolve();
      }, 50);
    });
  };

  ns.Control = Control;

  /* ==================== 登录 ==================== */

  ns.connect = function (account) {
    var timeoutMs = Number(account.timeoutMs) > 0 ? Number(account.timeoutMs) : 10000;
    return ns.openTcp(account.host, Number(account.port) || 21, timeoutMs)
      .then(function (socketId) {
        var control = new Control(account, socketId);
        return control.awaitReply().then(function (reply) {
          if (reply.code !== 220) {
            throw fail("(欢迎)", reply);
          }
          return control.command("OPTS UTF8 ON", [200]).then(null, function () {
            // 有的服务器不支持 OPTS：不算错误
            return null;
          }).then(function () {
            var user = String(account.user || "");
            if (user === "") {
              return control.command("USER anonymous", [230, 331]).then(function (reply) {
                if (reply.code === 230) {
                  return null;
                }
                return control.command("PASS anonymous@", [230, 202]);
              });
            }
            return control.command("USER " + user, [230, 331]).then(function (reply) {
              if (reply.code === 230) {
                return null;
              }
              var password = String(account.password || "");
              if (password === "") {
                // 空密码：协议上仍然要发一次 PASS
                return control.command("PASS ", [230, 202]);
              }
              return control.command("PASS " + password, [230, 202]);
            });
          }).then(function () {
            return control.command("TYPE I", [200]).then(null, function () {
              return null;
            });
          }).then(function () {
            return control.command("FEAT", [211]).then(function (reply) {
              control.featureText = String(reply.text || "");
              return null;
            }, function () {
              return null;
            });
          }).then(function () {
            var text = control.featureText.toLowerCase();
            control.features.mlsd = text.indexOf("mlsd") >= 0;
            control.features.size = text.indexOf("size") >= 0;
            control.features.rest = text.indexOf("rest") >= 0 || text === "";
            return control;
          });
        });
      });
  };

  /// 用一条控制连接做完一件事，然后 QUIT（失败也抛原错误）
  ns.withControl = function (account, action) {
    return ns.connect(account).then(function (control) {
      return Promise.resolve()
        .then(function () {
          return action(control);
        })
        .then(function (value) {
          return control.quit().then(function () {
            return value;
          });
        }, function (error) {
          return control.quit().then(function () {
            throw error;
          }, function () {
            throw error;
          });
        });
    });
  };

  /* ==================== 被动模式 ==================== */

  function isPrivateAddress(host) {
    var text = String(host || "");
    if (/^127\./.test(text) || /^10\./.test(text) || /^192\.168\./.test(text)) {
      return true;
    }
    var matched = /^172\.(\d+)\./.exec(text);
    if (matched) {
      var second = Number(matched[1]);
      return second >= 16 && second <= 31;
    }
    return false;
  }

  /// 进入被动模式，返回 {host, port}
  ///
  /// EPSV 优先（不受 NAT 改写 IP 的影响），失败再退回 PASV。PASV 给出的地址如果是私网
  /// 而控制连接连的是公网，按 NAT 的常见情况改用控制连接的地址。
  ns.enterPassive = function (control, account) {
    return control.command("EPSV", [229]).then(function (reply) {
      var matched = /\(([^)]*)\)/.exec(String(reply.text || ""));
      if (!matched) {
        throw new Error("EPSV 应答看不懂: " + reply.text);
      }
      var body = matched[1];
      var delimiter = body.charAt(0);
      var parts = body.split(delimiter);
      var port = Number(parts[3]);
      if (!(port > 0)) {
        throw new Error("EPSV 没有给出端口: " + reply.text);
      }
      return { host: account.host, port: port };
    }).then(null, function () {
      return control.command("PASV", [227]).then(function (reply) {
        var matched = /(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)/.exec(String(reply.text || ""));
        if (!matched) {
          throw new Error("PASV 应答看不懂: " + reply.text);
        }
        var host = [matched[1], matched[2], matched[3], matched[4]].join(".");
        var port = Number(matched[5]) * 256 + Number(matched[6]);
        if (isPrivateAddress(host) && !isPrivateAddress(account.host)) {
          host = account.host;
        }
        return { host: host, port: port };
      });
    });
  };

  /* ==================== 列目录 ==================== */

  /// 解析 MLSD 的一行：`type=file;size=123; name`
  function parseMlsdLine(line) {
    var index = line.indexOf(" ");
    if (index <= 0) {
      return null;
    }
    var factsText = line.substring(0, index);
    var name = line.substring(index + 1).trim();
    if (!name) {
      return null;
    }
    var facts = {};
    factsText.split(";").forEach(function (item) {
      var at = item.indexOf("=");
      if (at > 0) {
        facts[item.substring(0, at).toLowerCase()] = item.substring(at + 1);
      }
    });
    var type = String(facts.type || "").toLowerCase();
    if (type === "cdir" || type === "pdir") {
      return null;
    }
    return {
      name: name,
      isDir: type === "dir",
      size: Number(facts.size) || 0,
      modified: String(facts.modify || "")
    };
  }

  var monthIndex = {
    jan: 1, feb: 2, mar: 3, apr: 4, may: 5, jun: 6,
    jul: 7, aug: 8, sep: 9, oct: 10, nov: 11, dec: 12
  };

  /// 解析 UNIX 风格 LIST 的一行（`-rw-r--r-- 1 user group 1234 Jan 01 12:00 名字`）
  ///
  /// 名字里可能有空格，所以不按固定列切：找到月份那一列，再跳过一个时间 / 年份，
  /// 剩下的整段就是名字。
  function parseListLine(line) {
    var matched = /^([\-dlbcps])([rwxsStT-]{9})[.+@]?\s+/.exec(line);
    if (!matched) {
      return null;
    }
    var rest = line.substring(matched[0].length);
    var fields = rest.split(/\s+/);
    var monthAt = -1;
    for (var i = 0; i < fields.length && i < 5; i++) {
      if (monthIndex[fields[i].substring(0, 3).toLowerCase()]) {
        monthAt = i;
        break;
      }
    }
    if (monthAt < 0 || fields.length < monthAt + 4) {
      return null;
    }
    var size = Number(fields[monthAt - 1]);
    // 月份后面是 “日” 与 “时间 / 年份”，名字从再后面开始
    var nameStart = 0;
    var seen = 0;
    for (var at = 0; at < rest.length; at++) {
      if (/\s/.test(rest.charAt(at))) {
        if (at > 0 && !/\s/.test(rest.charAt(at - 1))) {
          seen++;
          if (seen === monthAt + 3) {
            nameStart = at + 1;
            break;
          }
        }
      }
    }
    var name = nameStart > 0 ? rest.substring(nameStart).trim() : "";
    if (!name || name === "." || name === "..") {
      return null;
    }
    var kind = matched[1];
    return {
      name: name,
      isDir: kind === "d",
      size: isNaN(size) ? 0 : size,
      modified: fields.slice(monthAt, monthAt + 3).join(" ")
    };
  }

  /// 读一条数据连接，把内容按账号编码解成文本
  ///
  /// 数据连接用 base64 收：中途解码会把多字节字符切断，所以攒到最后一次性解。
  /// 老插件框架没有 `musicxx.util.textDecode` 时退化成“拼 text 片段”（只对 UTF-8 正确）。
  ns.readDataText = function (control, account, dataSocketId, limitBytes) {
    var limit = Number(limitBytes) > 0 ? Number(limitBytes) : 8 * 1024 * 1024;
    return new Promise(function (resolve, reject) {
      var chunks = [];
      var textParts = [];
      var bytes = 0;
      var done = false;
      function finish(error) {
        if (done) {
          return;
        }
        done = true;
        ns.net.unregister(dataSocketId);
        if (error) {
          reject(error);
          return;
        }
        var text = ns.decodeText(chunks, control.encoding);
        if (text === null) {
          text = textParts.join("");
        }
        resolve(text);
      }
      ns.net.register(dataSocketId, {
        onData: function (event) {
          if (done) {
            return;
          }
          if (event.dataBase64) {
            chunks.push(event.dataBase64);
            bytes += Number(event.bytes) || 0;
          }
          if (typeof event.text === "string") {
            textParts.push(event.text);
          }
          if (bytes > limit) {
            musicxx.net.socket.close(dataSocketId);
            finish(new Error("目录内容过大（超过 " + ns.formatSize(limit) + "）"));
          }
        },
        onClose: function () {
          finish(null);
        }
      });
    });
  };

  /// 列目录：返回 {path, entries:[{name, path, isDir, size}], format}
  ns.list = function (account, path) {
    var target = ns.normalizePath(path || account.rootPath || "/");
    return ns.withControl(account, function (control) {
      return ns.enterPassive(control, account).then(function (passive) {
        return ns.openTcp(passive.host, passive.port, control.timeoutMs).then(function (dataId) {
          var useMlsd = !!control.features.mlsd;
          var command = useMlsd ? ("MLSD " + target) : ("LIST " + target);
          var reading = ns.readDataText(control, account, dataId);
          return control.command(command, [125, 150, 226]).then(function () {
            return reading;
          }).then(function (text) {
            // 读完后服务器还会发一条 226：不主动等（它就在会话里，QUIT 会顺带抽完）
            return { text: text, format: useMlsd ? "mlsd" : "list" };
          }).then(null, function (error) {
            ns.net.unregister(dataId);
            musicxx.net.socket.close(dataId);
            throw error;
          });
        });
      });
    }).then(function (payload) {
      var lines = String(payload.text || "").split(/\r?\n/);
      var entries = [];
      for (var i = 0; i < lines.length; i++) {
        var line = lines[i];
        if (!line || !line.trim()) {
          continue;
        }
        var entry = payload.format === "mlsd" ? parseMlsdLine(line) : parseListLine(line);
        if (!entry) {
          continue;
        }
        entry.path = ns.joinPath(target, entry.name);
        entries.push(entry);
      }
      entries.sort(function (left, right) {
        if (left.isDir !== right.isDir) {
          return left.isDir ? -1 : 1;
        }
        return left.name.localeCompare(right.name);
      });
      return { path: target, entries: entries, format: payload.format };
    });
  };

  /* ==================== 文件大小 ==================== */

  /// 取文件大小（服务器不支持 SIZE 时给 null，不影响播放，只是缓存与拖动会差一些）
  ns.size = function (account, path) {
    var target = ns.normalizePath(path);
    return ns.withControl(account, function (control) {
      return control.command("SIZE " + target, [213]).then(function (reply) {
        var value = Number(String(reply.text || "").trim());
        return value > 0 ? value : null;
      }, function () {
        return null;
      });
    });
  };

  /* ==================== 下载数据连接 ==================== */

  /// 开一条 RETR 数据连接，返回 {control, dataId, start, path, abort()}
  ///
  /// 这里只负责：建连 → 登录 → 被动模式 → 可选 REST → RETR，并确认服务器开始发送。
  /// `sink`（可选）在**发 RETR 之前**就登记到数据连接上：服务器发完 150 就立刻推数据，
  /// 等 Promise 结算再登记会丢掉开头那几个分片。sink 拿到的是原样的 `data` / `closed` 事件。
  ///
  /// `handle.start` 是服务器**实际**开始的偏移：服务器不支持 REST 时是 0（调用方要据此
  /// 重写响应头，否则播放器会在错误的位置读）。
  ns.openRetr = function (account, path, offset, sink) {
    var target = ns.normalizePath(path);
    var handle = { control: null, dataId: "", start: 0, path: target };
    handle.abort = function () {
      if (handle.dataId) {
        ns.net.unregister(handle.dataId);
        musicxx.net.socket.close(handle.dataId);
        handle.dataId = "";
      }
      if (handle.control) {
        handle.control.quit();
        handle.control = null;
      }
    };
    return ns.connect(account).then(function (control) {
      handle.control = control;
      return ns.enterPassive(control, account).then(function (passive) {
        return ns.openTcp(passive.host, passive.port, control.timeoutMs).then(function (dataId) {
          handle.dataId = dataId;
          if (sink) {
            ns.net.register(dataId, {
              onData: function (event) {
                if (typeof sink.onData !== "function") {
                  return;
                }
                try {
                  sink.onData(event);
                } catch (e) {
                  musicxx.host.log(4, "[ftp] 转发数据出错: " +
                                   (e && e.message ? e.message : String(e)));
                }
              },
              onClose: function (event) {
                if (typeof sink.onClose !== "function") {
                  return;
                }
                try {
                  sink.onClose(event);
                } catch (e) {
                  musicxx.host.log(4, "[ftp] 收尾数据连接出错: " +
                                   (e && e.message ? e.message : String(e)));
                }
              }
            });
          }
          function begin() {
            if (!(offset > 0)) {
              return Promise.resolve();
            }
            return control.command("REST " + offset, [350]).then(function () {
              handle.start = offset;
              return null;
            }, function () {
              // 服务器不支持断点续传：从头开始（调用方据此重写响应头）
              handle.start = 0;
              return null;
            });
          }
          return begin().then(function () {
            return control.command("RETR " + target, [125, 150]).then(function () {
              return handle;
            });
          }).then(null, function (error) {
            handle.abort();
            throw error;
          });
        });
      });
    });
  };

  /* ==================== 账号与资源身份 ==================== */

  /// 账号字段的缺省值（界面、存储、解析都用这一份）
  ns.defaultAccount = {
    port: 21,
    encoding: "utf-8",
    rootPath: "/",
    timeoutMs: 10000,
    passive: true,
    audioOnly: true
  };

  /// 把存储里读到的 / 界面提交的数据补成完整账号（端口与超时转数字、路径规范化）
  ns.normalizeAccount = function (raw) {
    var src = raw || {};
    var account = {};
    account.host = String(src.host === undefined || src.host === null ? "" : src.host).trim();
    var port = Number(src.port);
    account.port = (port > 0 && port <= 65535) ? Math.round(port) : ns.defaultAccount.port;
    account.user = String(src.user === undefined || src.user === null ? "" : src.user);
    account.password = String(src.password === undefined || src.password === null ? "" : src.password);
    var encoding = String(src.encoding || ns.defaultAccount.encoding).toLowerCase();
    account.encoding = (encoding === "gbk" || encoding === "gb18030") ? encoding : "utf-8";
    account.rootPath = ns.normalizePath(src.rootPath || "/");
    var timeout = Number(src.timeoutMs);
    account.timeoutMs = (timeout >= 1000 && timeout <= 120000)
      ? Math.round(timeout) : ns.defaultAccount.timeoutMs;
    account.passive = (src.passive === undefined || src.passive === null)
      ? ns.defaultAccount.passive : !!src.passive;
    account.audioOnly = (src.audioOnly === undefined || src.audioOnly === null)
      ? ns.defaultAccount.audioOnly : !!src.audioOnly;
    return account;
  };

  /// 配好主机就算“配置过”（用户名可以为空 = 匿名）
  ns.accountReady = function (account) {
    return !!(account && account.host);
  };

  /// 账号指纹：fnv1a(host(小写) + ":" + port + "/" + user) 取 8 位十六进制
  ///
  /// 只用来认出“这首歌是用哪个账号加的”，**不含密码**；非加密哈希，不需要更安全。
  ns.accountFingerprint = function (account) {
    if (!account) {
      return "";
    }
    var text = String(account.host || "").toLowerCase() + ":" +
               (Number(account.port) || ns.defaultAccount.port) + "/" +
               String(account.user || "");
    var hash = 0x811c9dc5;
    for (var i = 0; i < text.length; i++) {
      hash ^= text.charCodeAt(i) & 0xff;
      hash = (hash + ((hash << 1) + (hash << 4) + (hash << 7) + (hash << 8) + (hash << 24))) >>> 0;
      // 非 ASCII 字符（中文用户名）按两个字节算，避免不同名字撞成同一串
      if (text.charCodeAt(i) > 0xff) {
        hash ^= (text.charCodeAt(i) >> 8) & 0xff;
        hash = (hash + ((hash << 1) + (hash << 4) + (hash << 7) + (hash << 8) + (hash << 24))) >>> 0;
      }
    }
    var hex = (hash >>> 0).toString(16);
    while (hex.length < 8) {
      hex = "0" + hex;
    }
    return hex;
  };

  /// 歌曲里的资源 id：`<指纹>/<去掉开头 / 的路径>`
  ns.resourceIdOf = function (account, path) {
    var fingerprint = ns.accountFingerprint(account);
    var target = ns.normalizePath(path);
    return fingerprint + (target === "/" ? "/" : "/" + target.substring(1));
  };

  /// 资源 id → {fingerprint, path}；格式不对返回 null
  ns.parseResourceId = function (id) {
    var text = String(id || "");
    var index = text.indexOf("/");
    if (index !== 8) {
      return null;
    }
    var fingerprint = text.substring(0, 8).toLowerCase();
    if (!/^[0-9a-f]{8}$/.test(fingerprint)) {
      return null;
    }
    return { fingerprint: fingerprint, path: ns.normalizePath(text.substring(index)) };
  };
})(FtpSource);
