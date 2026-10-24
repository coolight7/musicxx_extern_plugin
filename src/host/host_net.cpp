/// musicxx 外部插件原生插件框架: 插件网络能力实现 (HTTP / TCP / WebSocket)
///
/// 设计与安全边界见 host_net.h。文件结构:
/// 1) 公共小工具 (URL / 头解析 / 正文解码 / base64 / SHA-1 / UTF-8 校验);
/// 2) `Stream`: 一条连接的读写与背压 (其余连接类型都继承它);
/// 3) `PluginNetHub::Impl`: 服务表、连接表、HTTP 待回答表与操作分发;
/// 4) HTTP 客户端 / HTTP 服务端 / TCP / WebSocket 四组实现;
/// 5) 对外 (同步 / 异步 / 取消 / 卸载) 入口。
///
/// 线程: 全部 socket 状态只在网络线程上访问; 对外入口投递到网络线程并**有界等待**
/// (网络线程不会等插件框架线程, 所以不会互锁)。

#include "host_net.h"

#include "host_json.h"
#include "host_net_tls.h"

#include "pluginxx/api/abi.h"

#include "utilxx_base/asio_error.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"
#include "utilxx_base/string_util.h"

#include <asio/buffer.hpp>
#include <asio/connect.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/steady_timer.hpp>
#include <asio/write.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace musicxx {
namespace extern_plugin {

using utilxx_base::AsioErrorCode;
using utilxx_base::Json;

namespace {

/* ==================== 上限 (改大之前先想清楚内存) ==================== */

/// 单条连接一次读的字节数
constexpr size_t kReadChunk = 16 * 1024;
/// 请求头 / 响应头上限
constexpr size_t kMaxHeadBytes = 16 * 1024;
/// HTTP 正文上限 (请求与响应共用; 超过按 413 / truncated 处理)
constexpr int64_t kMaxHttpBody = 8 * 1024 * 1024;
/// WebSocket 单条消息上限
constexpr int64_t kMaxWsMessage = 8 * 1024 * 1024;
/// 单条连接的发送队列上限 (背压)
constexpr size_t kMaxOutQueue = 4 * 1024 * 1024;
/// 每个插件最多几个同类服务
constexpr int32_t kMaxServersPerKind = 8;
/// 每个插件最多几条连接 (客户端 + 服务端)
constexpr size_t kMaxConnectionsPerPlugin = 64;
/// 每个插件同时进行的 HTTP 客户端请求
constexpr size_t kMaxHttpClientsPerPlugin = 16;
/// 每个 HTTP 服务同时等待插件回答的请求
constexpr size_t kMaxHttpPendingPerServer = 64;
/// HTTP 客户端默认超时
constexpr int64_t kDefaultHttpTimeoutMs = 15000;
/// 同步入口等网络线程的上界
constexpr uint32_t kCallWaitMs = 5000;

/* ==================== 文本小工具 ==================== */

std::string lowerAscii(std::string text) {
  for (char &c : text) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return text;
}

std::string_view trimAscii(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' ||
                           text.front() == '\r' || text.front() == '\n')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' ||
                           text.back() == '\r' || text.back() == '\n')) {
    text.remove_suffix(1);
  }
  return text;
}

bool startsWith(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); ++i) {
    char x = a[i];
    char y = b[i];
    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
    if (x != y) {
      return false;
    }
  }
  return true;
}

/// 是否是合法 UTF-8 (只用来决定"要不要多给一个 text 字段")
bool isValidUtf8(std::string_view text) {
  size_t i = 0;
  const size_t n = text.size();
  while (i < n) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      ++i;
      continue;
    }
    size_t extra = 0;
    uint32_t code = 0;
    if ((c & 0xE0) == 0xC0) {
      extra = 1;
      code = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      extra = 2;
      code = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      extra = 3;
      code = c & 0x07;
    } else {
      return false;
    }
    if (i + extra >= n) {
      return false;
    }
    for (size_t k = 1; k <= extra; ++k) {
      const unsigned char cc = static_cast<unsigned char>(text[i + k]);
      if ((cc & 0xC0) != 0x80) {
        return false;
      }
      code = (code << 6) | (cc & 0x3F);
    }
    // 过长编码与代理区不算合法
    if ((extra == 1 && code < 0x80) || (extra == 2 && code < 0x800) ||
        (extra == 3 && code < 0x10000) || code > 0x10FFFF ||
        (code >= 0xD800 && code <= 0xDFFF)) {
      return false;
    }
    i += extra + 1;
  }
  return true;
}

/// 把一段字节变成安全的 JSON 文本: 非法 UTF-8 字节换成 '?'
///
/// 用途: HTTP 头值按规范可能是 ISO-8859-1, 直接写进 JSON 会让 LLM/JS 的发解析失败。
std::string toSafeText(std::string_view text) {
  if (isValidUtf8(text)) {
    return std::string{text};
  }
  std::string out;
  out.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
      ++i;
      continue;
    }
    size_t extra = 0;
    if ((c & 0xE0) == 0xC0) {
      extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
      extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
      extra = 3;
    }
    bool valid = extra > 0 && i + extra < text.size();
    if (valid) {
      for (size_t k = 1; k <= extra; ++k) {
        if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) {
          valid = false;
          break;
        }
      }
    }
    if (valid) {
      out.append(text.substr(i, extra + 1));
      i += extra + 1;
    } else {
      out.push_back('?');
      ++i;
    }
  }
  return out;
}

std::string hexEncode(const unsigned char *data, size_t size) {
  static const char *kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    out.push_back(kDigits[(data[i] >> 4) & 0x0F]);
    out.push_back(kDigits[data[i] & 0x0F]);
  }
  return out;
}

std::vector<unsigned char> randomBytes(size_t count) {
  std::vector<unsigned char> bytes(count);
  static std::mutex rngMutex;
  static std::mt19937_64 rng{[] {
    std::random_device rd;
    std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
    std::mt19937_64 engine{seq};
    return engine;
  }()};
  {
    std::lock_guard lock{rngMutex};
    for (size_t i = 0; i < count; i += 8) {
      const uint64_t value = rng();
      for (size_t b = 0; b < 8 && i + b < count; ++b) {
        bytes[i + b] = static_cast<unsigned char>((value >> (8 * b)) & 0xFF);
      }
    }
  }
  return bytes;
}

/// SHA-1 (WebSocket 握手用; 只要一个用途, 不引入加密库)
std::string sha1Raw(std::string_view data) {
  uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                   0xC3D2E1F0u};
  std::string message{data};
  const uint64_t bitLen = static_cast<uint64_t>(message.size()) * 8;
  message.push_back(static_cast<char>(0x80));
  while (message.size() % 64 != 56) {
    message.push_back('\0');
  }
  for (int i = 7; i >= 0; --i) {
    message.push_back(static_cast<char>((bitLen >> (8 * i)) & 0xFF));
  }
  for (size_t offset = 0; offset + 64 <= message.size(); offset += 64) {
    uint32_t w[80] = {};
    for (size_t i = 0; i < 16; ++i) {
      const auto *p = reinterpret_cast<const unsigned char *>(message.data() +
                                                             offset + i * 4);
      w[i] = (static_cast<uint32_t>(p[0]) << 24) |
             (static_cast<uint32_t>(p[1]) << 16) |
             (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
    }
    for (size_t i = 16; i < 80; ++i) {
      w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]);
      w[i] = (w[i] << 1) | (w[i] >> 31);
    }
    uint32_t a = h[0];
    uint32_t b = h[1];
    uint32_t c = h[2];
    uint32_t d = h[3];
    uint32_t e = h[4];
    for (size_t i = 0; i < 80; ++i) {
      uint32_t f = 0;
      uint32_t k = 0;
      if (i < 20) {
        f = (b & c) | ((~b) & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const uint32_t temp = ((a << 5) | (a >> 27)) + f + e + k + w[i];
      e = d;
      d = c;
      c = (b << 30) | (b >> 2);
      b = a;
      a = temp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  }
  unsigned char out[20] = {};
  for (size_t i = 0; i < 5; ++i) {
    out[i * 4 + 0] = static_cast<unsigned char>((h[i] >> 24) & 0xFF);
    out[i * 4 + 1] = static_cast<unsigned char>((h[i] >> 16) & 0xFF);
    out[i * 4 + 2] = static_cast<unsigned char>((h[i] >> 8) & 0xFF);
    out[i * 4 + 3] = static_cast<unsigned char>(h[i] & 0xFF);
  }
  return std::string(reinterpret_cast<const char *>(out), sizeof(out));
}

std::string wsAcceptKey(std::string_view clientKey) {
  static constexpr std::string_view kGuid =
      "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  const std::string digest = sha1Raw(std::string{clientKey} + std::string{kGuid});
  return utilxx_base::base64Encode(digest);
}

std::string wsClientKey() {
  const auto bytes = randomBytes(16);
  return utilxx_base::base64Encode(
      std::string_view{reinterpret_cast<const char *>(bytes.data()), bytes.size()});
}

const char *statusTextOf(int status) {
  switch (status) {
  case 200: return "OK";
  case 201: return "Created";
  case 202: return "Accepted";
  case 204: return "No Content";
  case 206: return "Partial Content";
  case 301: return "Moved Permanently";
  case 302: return "Found";
  case 303: return "See Other";
  case 304: return "Not Modified";
  case 307: return "Temporary Redirect";
  case 308: return "Permanent Redirect";
  case 400: return "Bad Request";
  case 401: return "Unauthorized";
  case 403: return "Forbidden";
  case 404: return "Not Found";
  case 405: return "Method Not Allowed";
  case 408: return "Request Timeout";
  case 411: return "Length Required";
  case 413: return "Payload Too Large";
  case 416: return "Range Not Satisfiable";
  case 426: return "Upgrade Required";
  case 429: return "Too Many Requests";
  case 500: return "Internal Server Error";
  case 501: return "Not Implemented";
  case 502: return "Bad Gateway";
  case 503: return "Service Unavailable";
  case 504: return "Gateway Timeout";
  default: return "";
  }
}

Json errorJson(std::string_view code, std::string_view message) {
  Json out = Json::object();
  out["ok"] = false;
  out["error"] = std::string{code};
  out["message"] = std::string{message};
  return out;
}

/// JSON 值的文本形式 (字符串原样, 其它类型 dump; 头字段值用)
std::string jsonTextOf(const Json &value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_null()) {
    return {};
  }
  return value.dump();
}

/* ==================== URL ==================== */

struct ParsedUrl {
  bool ok = false;
  std::string error;
  std::string scheme; ///< http / https / ws / wss
  std::string host;   ///< 不含端口
  uint16_t port = 0;
  std::string path; ///< 以 / 开头, 含查询串
  std::string target; ///< path + 查询串 (同 path)
};

ParsedUrl parseUrl(const std::string &text) {
  ParsedUrl out;
  const size_t schemeEnd = text.find("://");
  if (schemeEnd == std::string::npos) {
    out.error = "地址必须以 <scheme>:// 开头";
    return out;
  }
  out.scheme = lowerAscii(text.substr(0, schemeEnd));
  const size_t authorityStart = schemeEnd + 3;
  size_t authorityEnd = text.find_first_of("/?#", authorityStart);
  if (authorityEnd == std::string::npos) {
    authorityEnd = text.size();
  }
  const std::string authority = text.substr(authorityStart, authorityEnd - authorityStart);
  if (authority.empty()) {
    out.error = "地址缺少主机名";
    return out;
  }
  std::string hostPort = authority;
  const size_t at = hostPort.find_last_of('@');
  if (at != std::string::npos) {
    hostPort = hostPort.substr(at + 1); ///< 丢弃 userinfo (鉴权走请求头)
  }
  if (!hostPort.empty() && hostPort.front() == '[') {
    const size_t close = hostPort.find(']');
    if (close == std::string::npos) {
      out.error = "IPv6 地址缺少 ]";
      return out;
    }
    out.host = hostPort.substr(1, close - 1);
    if (close + 1 < hostPort.size()) {
      if (hostPort[close + 1] != ':') {
        out.error = "IPv6 地址端口写法不合法";
        return out;
      }
      const std::string portText = hostPort.substr(close + 2);
      if (!portText.empty()) {
        try {
          const int value = std::stoi(portText);
          if (value <= 0 || value > 65535) {
            out.error = "端口超出范围";
            return out;
          }
          out.port = static_cast<uint16_t>(value);
        } catch (...) {
          out.error = "端口不是数字";
          return out;
        }
      }
    }
  } else {
    const size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos) {
      out.host = hostPort.substr(0, colon);
      const std::string portText = hostPort.substr(colon + 1);
      if (!portText.empty()) {
        try {
          const int value = std::stoi(portText);
          if (value <= 0 || value > 65535) {
            out.error = "端口超出范围";
            return out;
          }
          out.port = static_cast<uint16_t>(value);
        } catch (...) {
          out.error = "端口不是数字";
          return out;
        }
      }
    } else {
      out.host = hostPort;
    }
  }
  if (out.host.empty()) {
    out.error = "地址缺少主机名";
    return out;
  }
  if (out.port == 0) {
    out.port = (out.scheme == "https" || out.scheme == "wss") ? 443 : 80;
  }
  std::string path = text.substr(authorityEnd);
  if (path.empty()) {
    path = "/";
  } else if (path.front() == '?') {
    path = "/" + path;
  } else if (path.front() == '#') {
    path = "/";
  }
  // 片段不发给服务端
  const size_t hash = path.find('#');
  if (hash != std::string::npos) {
    path = path.substr(0, hash);
  }
  out.path = path;
  out.target = path;
  out.ok = true;
  return out;
}

/// Host 请求头: 默认端口不写 (与浏览器 / curl 一致), IPv6 字面量要加回方括号
std::string hostHeaderOf(const ParsedUrl &url) {
  const bool isIpv6 = url.host.find(':') != std::string::npos;
  const std::string host = isIpv6 ? "[" + url.host + "]" : url.host;
  const uint16_t defaultPort =
      (url.scheme == "https" || url.scheme == "wss") ? 443 : 80;
  if (url.port == defaultPort) {
    return host;
  }
  return host + ":" + std::to_string(url.port);
}

/// 协议 + 主机 + 端口 (拼绝对地址用; 默认端口不写)
std::string originOf(const ParsedUrl &url) {
  return url.scheme + "://" + hostHeaderOf(url);
}

/// 从参数里取客户端 TLS 选项 (http.request / http.fetch / http.download / ws.connect 共用)
///
/// `tlsVerify` 默认开; `caFile` / `caPem` 是**追加**信任的 CA (不影响系统根证书);
/// `serverName` 可以覆盖 SNI 与主机名校验用的名字 (调试 / IP 访问时用)。
void parseTlsOptions(const Json &args, TlsClientOptions &options) {
  options.verify = args.value("tlsVerify", true);
  options.caFile = args.value("caFile", std::string{});
  options.caPem = args.value("caPem", std::string{});
  options.serverName = args.value("serverName", std::string{});
}

/// 下载文件名规整:
/// - 只接受"纯名字": 路径分隔符与 `..` 一律拒绕, 拿到的文件名拿不到别的目录;
/// - 非法字符 (Windows 不允许的那些) 换成 `_`, 去掉前导点与空名字;
/// - 没给名字时按 URL 的最后一段推。
std::string downloadFileNameOf(const std::string &raw, const std::string &url) {
  std::string name{trimAscii(raw)};
  if (name.empty()) {
    std::string path = url;
    const size_t schemeEnd = path.find("://");
    if (schemeEnd != std::string::npos) {
      const size_t slash = path.find('/', schemeEnd + 3);
      path = slash == std::string::npos ? std::string{} : path.substr(slash);
    }
    const size_t query = path.find_first_of("?#");
    if (query != std::string::npos) {
      path = path.substr(0, query);
    }
    const size_t slash = path.find_last_of('/');
    name = slash == std::string::npos ? path : path.substr(slash + 1);
  }
  std::string safe;
  safe.reserve(name.size());
  for (const char c : name) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' ||
        c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
      safe.push_back('_');
    } else {
      safe.push_back(c);
    }
  }
  size_t start = 0;
  while (start < safe.size() && (safe[start] == '.' || safe[start] == ' ')) {
    ++start;
  }
  safe = safe.substr(start);
  while (!safe.empty() && (safe.back() == '.' || safe.back() == ' ')) {
    safe.pop_back();
  }
  if (safe.empty() || safe == "..") {
    safe = "download.bin";
  }
  if (safe.size() > 128) {
    safe = safe.substr(safe.size() - 128);
  }
  return safe;
}

/* ==================== HTTP 头 / 正文 ==================== */

struct HttpHead {
  bool request = false;
  std::string method;
  std::string target;
  std::string version;
  int status = 0;
  std::string reason;
  std::vector<std::pair<std::string, std::string>> headers;

  std::string header(std::string_view name) const {
    for (const auto &item : headers) {
      if (iequals(item.first, name)) {
        return item.second;
      }
    }
    return {};
  }
  bool hasHeader(std::string_view name) const {
    for (const auto &item : headers) {
      if (iequals(item.first, name)) {
        return true;
      }
    }
    return false;
  }
  Json headersJson() const {
    Json out = Json::object();
    for (const auto &item : headers) {
      const std::string key = lowerAscii(toSafeText(item.first));
      if (out.contains(key)) {
        // 重复头 (Set-Cookie 等) 用换行拼起来; 单值头按规范也可以这样看
        const std::string previous = out.value(key, std::string{});
        out[key] = previous + "\n" + toSafeText(item.second);
      } else {
        out[key] = toSafeText(item.second);
      }
    }
    return out;
  }
};

/// 解析请求头或响应头 (输入不含结尾空行也可以)
bool parseHttpHead(std::string_view text, bool request, HttpHead &out) {
  out = HttpHead{};
  out.request = request;
  size_t pos = 0;
  bool firstLine = true;
  while (pos <= text.size()) {
    size_t lineEnd = text.find('\n', pos);
    std::string_view line = lineEnd == std::string_view::npos
                                ? text.substr(pos)
                                : text.substr(pos, lineEnd - pos);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (firstLine) {
      firstLine = false;
      if (request) {
        const size_t sp1 = line.find(' ');
        const size_t sp2 = sp1 == std::string_view::npos
                              ? std::string_view::npos
                              : line.find(' ', sp1 + 1);
        if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) {
          return false;
        }
        out.method = std::string{line.substr(0, sp1)};
        out.target = std::string{line.substr(sp1 + 1, sp2 - sp1 - 1)};
        out.version = std::string{line.substr(sp2 + 1)};
      } else {
        const size_t sp1 = line.find(' ');
        if (sp1 == std::string_view::npos) {
          return false;
        }
        if (!startsWith(line, "HTTP/")) {
          return false;
        }
        out.version = std::string{line.substr(0, sp1)};
        const std::string_view rest = line.substr(sp1 + 1);
        const size_t sp2 = rest.find(' ');
        const std::string codeText{rest.substr(0, sp2)};
        try {
          out.status = std::stoi(codeText);
        } catch (...) {
          return false;
        }
        if (sp2 != std::string_view::npos) {
          out.reason = std::string{rest.substr(sp2 + 1)};
        }
      }
    } else if (!line.empty()) {
      const size_t colon = line.find(':');
      if (colon == std::string_view::npos) {
        return false;
      }
      out.headers.emplace_back(std::string{line.substr(0, colon)},
                               std::string{trimAscii(line.substr(colon + 1))});
    }
    if (lineEnd == std::string_view::npos) {
      break;
    }
    pos = lineEnd + 1;
  }
  return firstLine == false && (!request || !out.method.empty()) &&
         (request || out.status > 0);
}

/// 请求行里 target 的路径与查询串
void splitTarget(std::string_view target, std::string &path, std::string &query) {
  const size_t mark = target.find('?');
  if (mark == std::string_view::npos) {
    path = std::string{target};
    query.clear();
    return;
  }
  path = std::string{target.substr(0, mark)};
  query = std::string{target.substr(mark + 1)};
}

/// HTTP 正文解码 (Content-Length / chunked / 读到断开)
class HttpBodyDecoder {
public:
  enum class Mode { None, Length, Chunked, UntilClose };

  void init(Mode mode, int64_t length, int64_t maxBytes) {
    mode_ = mode;
    remaining_ = length;
    maxBytes_ = maxBytes > 0 ? maxBytes : kMaxHttpBody;
    done_ = mode == Mode::None || (mode == Mode::Length && length <= 0);
    truncated_ = false;
    body_.clear();
    raw_.clear();
    pos_ = 0;
    phase_ = ChunkPhase::Size;
    chunkRemaining_ = 0;
    received_ = 0;
  }

  /// 正文的落盘回调 (下载用): 设了就不再在内存里留着正文
  using Sink = std::function<void(const char *, size_t)>;
  void setSink(Sink sink) { sink_ = std::move(sink); }

  /// 已经从网络收到并解码出来的正文字节数 (不受 maxBytes 上限影响)
  int64_t received() const { return received_; }

  void feed(const char *data, size_t size) {
    if (done_ || size == 0) {
      return;
    }
    if (mode_ == Mode::Length) {
      const size_t take = static_cast<size_t>(
          (std::min)(static_cast<int64_t>(size), remaining_));
      appendBody(data, take);
      remaining_ -= static_cast<int64_t>(take);
      done_ = remaining_ <= 0;
      return;
    }
    if (mode_ == Mode::UntilClose) {
      appendBody(data, size);
      return;
    }
    raw_.append(data, size);
    processChunked();
  }

  /// 对端断开: 只有 UntilClose 才算正常结束
  void finishOnClose() {
    if (mode_ == Mode::UntilClose) {
      done_ = true;
    }
  }

  bool done() const { return done_; }
  bool truncated() const { return truncated_; }
  const std::string &body() const { return body_; }

private:
  enum class ChunkPhase { Size, Data, DataCrlf, Trailer };

  void appendBody(const char *data, size_t size) {
    if (size == 0) {
      return;
    }
    received_ += static_cast<int64_t>(size);
    if (sink_) {
      // 流式落盘: 正文不进内存
      sink_(data, size);
      return;
    }
    const size_t room =
        body_.size() >= static_cast<size_t>(maxBytes_)
            ? 0
            : static_cast<size_t>(maxBytes_) - body_.size();
    const size_t take = (std::min)(room, size);
    if (take > 0) {
      body_.append(data, take);
    }
    if (take < size) {
      truncated_ = true;
    }
  }

  bool readLine(std::string &line) {
    const size_t end = raw_.find("\r\n", pos_);
    if (end == std::string::npos) {
      return false;
    }
    line.assign(raw_, pos_, end - pos_);
    pos_ = end + 2;
    return true;
  }

  void processChunked() {
    while (!done_) {
      if (phase_ == ChunkPhase::Size) {
        std::string line;
        if (!readLine(line)) {
          return;
        }
        const size_t semicolon = line.find(';');
        const std::string hex{trimAscii(
            std::string_view{line}.substr(0, semicolon))};
        if (hex.empty()) {
          done_ = true; ///< 坏数据: 按结束处理 (正文里已经收到的部分保留)
          return;
        }
        int64_t value = 0;
        try {
          value = std::stoll(hex, nullptr, 16);
        } catch (...) {
          done_ = true;
          return;
        }
        if (value <= 0) {
          phase_ = ChunkPhase::Trailer;
        } else {
          chunkRemaining_ = value;
          phase_ = ChunkPhase::Data;
        }
      } else if (phase_ == ChunkPhase::Data) {
        const size_t available = raw_.size() - pos_;
        if (available == 0) {
          return;
        }
        const size_t take = static_cast<size_t>(
            (std::min)(static_cast<int64_t>(available), chunkRemaining_));
        appendBody(raw_.data() + pos_, take);
        pos_ += take;
        chunkRemaining_ -= static_cast<int64_t>(take);
        if (chunkRemaining_ <= 0) {
          phase_ = ChunkPhase::DataCrlf;
        }
      } else if (phase_ == ChunkPhase::DataCrlf) {
        if (raw_.size() - pos_ < 2) {
          return;
        }
        pos_ += 2;
        phase_ = ChunkPhase::Size;
      } else {
        std::string line;
        if (!readLine(line)) {
          return;
        }
        if (line.empty()) {
          done_ = true;
          return;
        }
      }
    }
  }

  Mode mode_ = Mode::None;
  int64_t remaining_ = 0;
  int64_t maxBytes_ = kMaxHttpBody;
  int64_t received_ = 0;
  Sink sink_;
  bool done_ = true;
  bool truncated_ = false;
  std::string body_;
  std::string raw_;
  size_t pos_ = 0;
  ChunkPhase phase_ = ChunkPhase::Size;
  int64_t chunkRemaining_ = 0;
};

/* ==================== 有界等待槽 (网络线程 → 调用线程) ==================== */

struct SyncSlot {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;

  void set() {
    {
      std::lock_guard lock{mutex};
      done = true;
    }
    cv.notify_all();
  }

  bool wait(uint32_t timeoutMs) {
    std::unique_lock lock{mutex};
    return cv.wait_for(lock, std::chrono::milliseconds{timeoutMs},
                       [this] { return done; });
  }
};

} // namespace

/// 前置声明: 连接类型都从这里派生
struct PluginNetHub::Impl;

/// 一条 TCP 连接 (客户端或服务端接受的那一端)
struct Stream : std::enable_shared_from_this<Stream> {
  Stream(std::shared_ptr<PluginNetHub::Impl> hub, asio::ip::tcp::socket socket);
  virtual ~Stream() = default;

  std::shared_ptr<PluginNetHub::Impl> hub;
  std::string plugin;
  std::string id;
  std::string serverId; ///< 空 = 客户端连接
  std::string remote;
  std::string local;

  /// 连接的读写层: 纯 TCP 或客户端 TLS (建好之后所有读写都经过它)
  std::shared_ptr<NetStream> io;
  std::vector<char> readBuf;
  std::deque<std::string> outQueue;
  size_t outBytes = 0;
  bool writing = false;
  bool closing = false;
  bool closeWhenFlushed = false;

  void start();
  void startRead();
  void send(std::string data, std::string &err);
  void flushThenClose();
  void closeStream(std::string_view reason, std::string_view error);

  /// 收到字节 (网络线程; 实现里可以直接 closeStream)
  virtual void onBytes(const char *data, size_t size) = 0;
  /// 连接关闭事件 (`kind` 由实现决定怎么发布)
  virtual void onClosed(std::string_view reason, std::string_view error) = 0;
  /// 连接类别 (tcp / ws / http), 用于 info 与自省
  virtual const char *kind() const = 0;

private:
  void startWrite();
  void afterWrite(AsioErrorCode ec);
};

} // namespace extern_plugin
} // namespace musicxx

namespace musicxx {
namespace extern_plugin {

struct HttpClient;
struct HttpServerStream;
struct TcpStream;
struct WsStream;

/// HTTP 客户端的三处用法: 同一个实现, 结果 JSON 的字段不同
///   Request  = `http.request` (原始响应: 正文 + headers)
///   Fetch    = `http.fetch`   (老 `musicxx.net.fetch` 的形态)
///   Download = `http.download` (正文流式写文件, 结果里给路径与字节数)
enum class HttpMode : int32_t { Request = 0, Fetch = 1, Download = 2 };

/* ==================== 网络中心 ==================== */

struct PluginNetHub::Impl : std::enable_shared_from_this<PluginNetHub::Impl> {
  PublishFn publish;
  PostFn postToFramework;

  asio::io_context io;
  std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>>
      guard;
  std::thread thread;
  std::atomic<bool> running{false};
  std::atomic<int64_t> nextRequestId{1};
  std::atomic<int64_t> nextConnSeq{1};
  /// 插件私有数据根目录 (http.download 落盘用; 在 start() 之前设)
  std::string dataDir;

  /// 一个服务 (HTTP / TCP / WS)
  struct Server {
    std::string plugin;
    std::string kind; ///< http | tcp | ws
    std::string id;
    std::string host;
    uint16_t port = 0;
    std::string pathPrefix; ///< http / ws 的路径前缀 (空 = 不限制)
    std::shared_ptr<asio::ip::tcp::acceptor> acceptor;
    int64_t maxBody = kMaxHttpBody;
    int32_t requestTimeoutMs = 30000;
    int32_t maxConnections = 32;
    size_t pending = 0;            ///< 正在等插件回答的 HTTP 请求数
    std::set<std::string> conns;   ///< 该服务下的连接 id
    std::string url;               ///< http://host:port (只对 http / ws 有意义)
  };

  std::map<std::string, std::shared_ptr<Server>> servers; ///< plugin|kind|id
  std::map<std::string, std::shared_ptr<Stream>> conns;   ///< plugin|id

  /// 等待插件回答的 HTTP 请求
  struct PendingHttp {
    std::string plugin;
    std::string serverKey;
    std::weak_ptr<Stream> stream;
  };
  std::map<int64_t, PendingHttp> pendingHttp;

  /// 进行中的 HTTP 客户端请求
  struct HttpWait {
    std::string plugin;
    DoneFn done;
    std::shared_ptr<HttpClient> client;
  };
  std::map<int64_t, HttpWait> httpWaits;

  /* ---------- 小工具 ---------- */

  static std::string serverKeyOf(const std::string &plugin,
                                 const std::string &kind,
                                 const std::string &id) {
    return plugin + "|" + kind + "|" + id;
  }
  static std::string connKeyOf(const std::string &plugin,
                               const std::string &id) {
    return plugin + "|" + id;
  }

  /// 发一条插件网络事件 (`payload` 里要带 `kind`)
  void emit(const std::string &pluginId, Json payload) {
    if (publish) {
      publish(pluginId, payload.dump());
    }
  }

  template <typename Fn> bool runSync(Fn &&fn, uint32_t waitMs = kCallWaitMs) {
    if (!running.load(std::memory_order_acquire)) {
      return false;
    }
    auto slot = std::make_shared<SyncSlot>();
    asio::post(io, [fn = std::forward<Fn>(fn), slot]() mutable {
      try {
        fn();
      } catch (const std::exception &e) {
        XX_LOGW("[musicxx_ext] 网络线程任务异常: {}", e.what());
      }
      slot->set();
    });
    return slot->wait(waitMs);
  }

  int64_t allocateRequestId() {
    return nextRequestId.fetch_add(1, std::memory_order_relaxed);
  }

  /* ---------- 连接与服务记账 ---------- */

  size_t connectionCountOf(const std::string &plugin) const {
    size_t count = 0;
    for (const auto &item : conns) {
      if (item.second && item.second->plugin == plugin) {
        ++count;
      }
    }
    return count;
  }

  /// 登记一条连接 (超限返回 false)
  bool registerStream(const std::shared_ptr<Stream> &stream);

  /// 连接关闭后的清理 (`Stream::closeStream` 调用; 网络线程)
  void onStreamClosed(const std::shared_ptr<Stream> &stream);

  /// 取连接 (只能网络线程)
  std::shared_ptr<Stream> findConn(const std::string &plugin,
                                   const std::string &id) const;

  /// 起一个监听; 失败写 err
  bool listenServer(std::shared_ptr<Server> server, std::string &err);

  void acceptNext(const std::shared_ptr<Server> &server);

  /// 接受一条连接并创建对应的 Stream 实现
  void onAccepted(const std::shared_ptr<Server> &server,
                  asio::ip::tcp::socket socket);

  /// 关掉某个插件的全部服务与连接 (网络线程; 卸载 / 停机)
  ///
  /// `cancelled` 非空时收走未完成 HTTP 客户端请求的完成回调 —— 它们要靠调用方
  /// 在插件框架线程上按"已取消"结算 (网络线程不能执行插件代码)。
  void closeAllOf(const std::string &plugin, bool allPlugins,
                  std::vector<DoneFn> *cancelled = nullptr);

  /// 起 / 停网络线程 (幂等)
  void startThread();
  void stopThread();

  /* ---------- HTTP 服务端 (等插件回答的请求) ---------- */

  /// 登记一个等待回答的请求; 超出上限返回 0
  int64_t registerPendingHttp(const std::shared_ptr<Server> &server,
                              const std::shared_ptr<Stream> &stream);
  std::shared_ptr<Stream> takePendingHttp(int64_t requestId,
                                          std::string *plugin);
  void dropPendingHttpOfStream(const Stream *stream);
  void dropPendingHttp(int64_t requestId);

  /* ---------- HTTP 客户端 ---------- */

  /// 开始一个 HTTP 请求 (网络线程); 失败时 out 写错误对象并返回 false
  ///
  /// `mode`: 0 = http.request / 1 = http.fetch / 2 = http.download
  bool startHttpRequest(const std::string &plugin, int64_t requestId,
                        const Json &args, DoneFn done, Json &out,
                        int32_t mode = 0);

  /// 完成一个 HTTP 客户端请求 (成功与失败都走这里)
  void finishHttpRequest(int64_t requestId, bool ok, Json payload);

  void cancelHttpRequest(const std::string &plugin, int64_t requestId,
                         Json &out);

  /* ---------- 服务与连接操作 (网络线程) ---------- */

  int32_t handleCall(const std::string &plugin, const std::string &op,
                     const Json &args, Json &out);

  void stopServers(const std::string &plugin, const std::string &kind,
                   const std::string &id, Json &out);
  void closeConn(const std::string &plugin, const std::string &id, Json &out);
  void sendConn(const std::string &plugin, const Json &args, Json &out);
  std::string infoJsonOnThread(const std::string &plugin, bool allPlugins);

  /// 当前是否在监听这个地址 (服务端存在且监听中)
  std::shared_ptr<Server> findServer(const std::string &plugin,
                                     const std::string &kind,
                                     const std::string &id) const;

  std::string nextServerId(const std::string &plugin,
                           const std::string &kind) const;
  std::string nextConnId(const std::string &plugin,
                         const std::string &prefix) const;
};

namespace {

/// endpoint → `1.2.3.4:5678`
std::string endpointText(const asio::ip::tcp::endpoint &endpoint) {
  return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
}

/// 连接 / 服务 id: 只允许字母数字与 . _ - (拼进事件与 info 里不会串味)
bool isValidId(std::string_view id) {
  if (id.empty() || id.size() > 64) {
    return false;
  }
  for (const char c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    if (!ok) {
      return false;
    }
  }
  return true;
}

/// 取服务器绑定地址 (缺省只监听回环)
bool resolveBindHost(std::string &host, std::string &err) {
  if (host.empty()) {
    host = "127.0.0.1";
  }
  if (host == "localhost") {
    host = "127.0.0.1";
  }
  AsioErrorCode ec;
  (void)asio::ip::make_address(host, ec);
  if (ec) {
    err = "监听地址不是合法的 IP: " + host;
    return false;
  }
  return true;
}

} // namespace

/* ==================== Stream ==================== */

Stream::Stream(std::shared_ptr<PluginNetHub::Impl> hubRef,
               asio::ip::tcp::socket socketRef)
    : hub(std::move(hubRef)) {
  // 已连上的 socket 才有对端/本地地址 (客户端连接建立前是空的, 连上后再取一次)
  if (socketRef.is_open()) {
    AsioErrorCode ec;
    const auto remoteEndpoint = socketRef.remote_endpoint(ec);
    if (!ec) {
      remote = endpointText(remoteEndpoint);
    }
    const auto localEndpoint = socketRef.local_endpoint(ec);
    if (!ec) {
      local = endpointText(localEndpoint);
    }
  }
  io = std::make_shared<NetStream>(std::move(socketRef));
}

void Stream::start() {
  readBuf.resize(kReadChunk);
  startRead();
}

void Stream::startRead() {
  if (closing) {
    return;
  }
  auto self = shared_from_this();
  io->async_read_some(
      asio::buffer(readBuf), [self](AsioErrorCode ec, size_t size) {
        if (self->closing) {
          return;
        }
        if (ec) {
          const bool eof = NetStream::isPeerClosed(ec);
          self->closeStream(eof ? "remote_closed" : "error", ec.message());
          return;
        }
        if (size > 0) {
          self->onBytes(self->readBuf.data(), size);
        }
        if (!self->closing) {
          self->startRead();
        }
      });
}

void Stream::send(std::string data, std::string &err) {
  if (closing || closeWhenFlushed) {
    err = "closed";
    return;
  }
  if (outBytes + data.size() > kMaxOutQueue) {
    err = "queue_full";
    return;
  }
  outBytes += data.size();
  outQueue.push_back(std::move(data));
  startWrite();
}

void Stream::startWrite() {
  if (writing) {
    return;
  }
  if (outQueue.empty()) {
    if (closeWhenFlushed) {
      closeStream("local_close", {});
    }
    return;
  }
  writing = true;
  auto self = shared_from_this();
  asio::async_write(*io, asio::buffer(outQueue.front()),
                    [self](AsioErrorCode ec, size_t) { self->afterWrite(ec); });
}

void Stream::afterWrite(AsioErrorCode ec) {
  writing = false;
  if (closing) {
    return;
  }
  if (ec) {
    closeStream("error", ec.message());
    return;
  }
  if (!outQueue.empty()) {
    outBytes -= outQueue.front().size();
    outQueue.pop_front();
  }
  startWrite();
}

void Stream::flushThenClose() {
  closeWhenFlushed = true;
  if (!writing && outQueue.empty()) {
    closeStream("local_close", {});
  }
}

void Stream::closeStream(std::string_view reason, std::string_view error) {
  if (closing) {
    return;
  }
  closing = true;
  io->close();
  onClosed(reason, error);
  if (hub) {
    hub->onStreamClosed(shared_from_this());
  }
}

/* ==================== 记账 ==================== */

std::shared_ptr<Stream>
PluginNetHub::Impl::findConn(const std::string &plugin,
                             const std::string &id) const {
  const auto it = conns.find(connKeyOf(plugin, id));
  return it == conns.end() ? nullptr : it->second;
}

bool PluginNetHub::Impl::registerStream(const std::shared_ptr<Stream> &stream) {
  if (!stream) {
    return false;
  }
  if (connectionCountOf(stream->plugin) >= kMaxConnectionsPerPlugin) {
    return false;
  }
  conns[connKeyOf(stream->plugin, stream->id)] = stream;
  if (!stream->serverId.empty()) {
    const auto server = findServer(stream->plugin, stream->kind(), stream->serverId);
    if (server) {
      server->conns.insert(stream->id);
    }
  }
  return true;
}

void PluginNetHub::Impl::onStreamClosed(const std::shared_ptr<Stream> &stream) {
  if (!stream) {
    return;
  }
  const auto it = conns.find(connKeyOf(stream->plugin, stream->id));
  if (it != conns.end() && it->second == stream) {
    conns.erase(it);
  }
  if (!stream->serverId.empty()) {
    const auto server = findServer(stream->plugin, stream->kind(), stream->serverId);
    if (server) {
      server->conns.erase(stream->id);
    }
  }
  dropPendingHttpOfStream(stream.get());
}

std::shared_ptr<PluginNetHub::Impl::Server>
PluginNetHub::Impl::findServer(const std::string &plugin,
                               const std::string &kind,
                               const std::string &id) const {
  const auto it = servers.find(serverKeyOf(plugin, kind, id));
  return it == servers.end() ? nullptr : it->second;
}

std::string PluginNetHub::Impl::nextServerId(const std::string &plugin,
                                             const std::string &kind) const {
  for (int i = 1; i < 10000; ++i) {
    const std::string candidate = kind + std::to_string(i);
    if (!findServer(plugin, kind, candidate)) {
      return candidate;
    }
  }
  return kind + "_x";
}

std::string PluginNetHub::Impl::nextConnId(const std::string &plugin,
                                           const std::string &prefix) const {
  for (int i = 1; i < 100000; ++i) {
    const std::string candidate = prefix + std::to_string(i);
    if (!findConn(plugin, candidate)) {
      return candidate;
    }
  }
  return prefix + "_x";
}

/* ==================== 监听与接受 ==================== */

bool PluginNetHub::Impl::listenServer(std::shared_ptr<Server> server,
                                      std::string &err) {
  int32_t sameKind = 0;
  for (const auto &item : servers) {
    if (item.second && item.second->plugin == server->plugin &&
        item.second->kind == server->kind) {
      ++sameKind;
    }
  }
  if (sameKind >= kMaxServersPerKind) {
    err = "同类服务已达上限";
    return false;
  }
  if (!resolveBindHost(server->host, err)) {
    return false;
  }
  try {
    asio::ip::tcp::endpoint endpoint{
        asio::ip::make_address(server->host), server->port};
    auto acceptor = std::make_shared<asio::ip::tcp::acceptor>(io);
    acceptor->open(endpoint.protocol());
    acceptor->set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor->bind(endpoint);
    acceptor->listen(asio::socket_base::max_listen_connections);
    server->port = acceptor->local_endpoint().port();
    server->acceptor = acceptor;
    if (server->kind == "http" || server->kind == "ws") {
      server->url = (server->kind == "http" ? "http://" : "ws://") +
                    server->host + ":" + std::to_string(server->port) +
                    server->pathPrefix;
    }
    servers[serverKeyOf(server->plugin, server->kind, server->id)] = server;
    acceptNext(server);
    return true;
  } catch (const std::exception &e) {
    err = e.what();
    return false;
  }
}

void PluginNetHub::Impl::acceptNext(const std::shared_ptr<Server> &server) {
  if (!server || !server->acceptor || !running.load(std::memory_order_acquire)) {
    return;
  }
  if (!server->acceptor->is_open()) {
    return; ///< 服务已关: 不再接受 (关监听后会回来一次 operation_aborted)
  }
  auto socket = std::make_shared<asio::ip::tcp::socket>(io);
  auto self = shared_from_this();
  server->acceptor->async_accept(*socket,
                                 [self, server, socket](AsioErrorCode ec) {
    if (!ec && socket->is_open()) {
      self->onAccepted(server, std::move(*socket));
    }
    self->acceptNext(server);
  });
}

/* ==================== HTTP 服务端 (等插件回答) ==================== */

int64_t PluginNetHub::Impl::registerPendingHttp(
    const std::shared_ptr<Server> &server,
    const std::shared_ptr<Stream> &stream) {
  if (!server || !stream) {
    return 0;
  }
  if (server->pending >= kMaxHttpPendingPerServer) {
    return 0;
  }
  const int64_t requestId = allocateRequestId();
  PendingHttp pending;
  pending.plugin = server->plugin;
  pending.serverKey = serverKeyOf(server->plugin, server->kind, server->id);
  pending.stream = stream;
  pendingHttp[requestId] = std::move(pending);
  ++server->pending;
  return requestId;
}

std::shared_ptr<Stream>
PluginNetHub::Impl::takePendingHttp(int64_t requestId, std::string *plugin) {
  const auto it = pendingHttp.find(requestId);
  if (it == pendingHttp.end()) {
    return nullptr;
  }
  if (plugin) {
    *plugin = it->second.plugin;
  }
  const auto serverIt = servers.find(it->second.serverKey);
  if (serverIt != servers.end() && serverIt->second &&
      serverIt->second->pending > 0) {
    --serverIt->second->pending;
  }
  auto stream = it->second.stream.lock();
  pendingHttp.erase(it);
  return stream;
}

void PluginNetHub::Impl::dropPendingHttp(int64_t requestId) {
  std::string plugin;
  (void)takePendingHttp(requestId, &plugin);
}

void PluginNetHub::Impl::dropPendingHttpOfStream(const Stream *stream) {
  for (auto it = pendingHttp.begin(); it != pendingHttp.end();) {
    if (it->second.stream.lock().get() == stream) {
      const auto serverIt = servers.find(it->second.serverKey);
      if (serverIt != servers.end() && serverIt->second &&
          serverIt->second->pending > 0) {
        --serverIt->second->pending;
      }
      it = pendingHttp.erase(it);
    } else {
      ++it;
    }
  }
}

/* ==================== 关停某一插件的全部网络资源 ==================== */

void PluginNetHub::Impl::closeAllOf(const std::string &plugin, bool allPlugins,
                                  std::vector<DoneFn> *cancelled) {
  // 1) 服务: 关监听 (之后不再接受新连接)
  for (auto it = servers.begin(); it != servers.end();) {
    if (!it->second ||
        (allPlugins ? true : it->second->plugin == plugin)) {
      if (it->second->acceptor) {
        AsioErrorCode ignored;
        it->second->acceptor->close(ignored);
      }
      it = servers.erase(it);
    } else {
      ++it;
    }
  }
  // 2) 连接: 关 socket (closeStream 会走 onClosed → 事件)
  std::vector<std::shared_ptr<Stream>> closing;
  for (const auto &item : conns) {
    if (!item.second) {
      continue;
    }
    if (allPlugins || item.second->plugin == plugin) {
      closing.push_back(item.second);
    }
  }
  for (const auto &stream : closing) {
    stream->closeStream("plugin_unloaded", {});
  }
  // 3) 未回答的 HTTP 请求与未完成的客户端请求
  for (auto it = pendingHttp.begin(); it != pendingHttp.end();) {
    if (allPlugins || it->second.plugin == plugin) {
      it = pendingHttp.erase(it);
    } else {
      ++it;
    }
  }
  std::vector<std::pair<int64_t, std::string>> waits;
  for (auto it = httpWaits.begin(); it != httpWaits.end();) {
    if (allPlugins || it->second.plugin == plugin) {
      waits.emplace_back(it->first, it->second.plugin);
      if (cancelled && it->second.done) {
        cancelled->push_back(std::move(it->second.done));
      }
      it = httpWaits.erase(it);
    } else {
      ++it;
    }
  }
  for (const auto &item : waits) {
    Json event = errorJson("cancelled", "插件已卸载或插件框架已停止");
    event["kind"] = "http.response";
    event["requestId"] = item.first;
    emit(item.second, std::move(event));
  }
}

/* ==================== HTTP 服务端连接 ==================== */

struct HttpServerStream : Stream {
  explicit HttpServerStream(std::shared_ptr<PluginNetHub::Impl> hubRef,
                            asio::ip::tcp::socket socketRef)
      : Stream(std::move(hubRef), std::move(socketRef)) {}

  std::shared_ptr<PluginNetHub::Impl::Server> server;
  std::string in;
  std::string currentMethod = "GET";
  bool waitingResponse = false;
  int64_t pendingRequestId = 0;
  bool keepAlive = false;
  std::unique_ptr<asio::steady_timer> timeout;

  const char *kind() const override { return "http"; }

  void onBytes(const char *data, size_t size) override {
    if (waitingResponse) {
      // 一次只处理一条请求 (不做流水线): 多余字节先留着, 回答完再解析
      if (in.size() + size > kMaxHeadBytes + static_cast<size_t>(kMaxHttpBody)) {
        closeStream("bad_request", "请求过大");
        return;
      }
    }
    in.append(data, size);
    process();
  }

  void onClosed(std::string_view /*reason*/, std::string_view /*error*/) override {
    if (pendingRequestId != 0 && hub) {
      hub->dropPendingHttp(pendingRequestId);
      pendingRequestId = 0;
    }
    waitingResponse = false;
    if (timeout) {
      AsioErrorCode ignored;
      timeout->cancel();
    }
  }

  void armTimeout() {
    if (!timeout) {
      timeout = std::make_unique<asio::steady_timer>(hub->io);
    }
    AsioErrorCode ignored;
    timeout->cancel();
    const int32_t budget =
        server ? (std::max)(1000, server->requestTimeoutMs) : 30000;
    timeout->expires_after(std::chrono::milliseconds{budget});
    auto self = std::static_pointer_cast<HttpServerStream>(shared_from_this());
    timeout->async_wait([self](AsioErrorCode ec) {
      if (ec || !self->waitingResponse) {
        return;
      }
      // 插件没在时限内回答: 回 504 并关掉连接 (待回答登记同时作废)
      if (self->pendingRequestId != 0) {
        self->hub->dropPendingHttp(self->pendingRequestId);
        self->pendingRequestId = 0;
      }
      self->waitingResponse = false;
      self->writeSimple(504, "插件没有在时限内回答请求");
      self->flushThenClose();
    });
  }

  void process() {
    if (waitingResponse || closing) {
      return;
    }
    const size_t headEnd = in.find("\r\n\r\n");
    if (headEnd == std::string::npos) {
      if (in.size() > kMaxHeadBytes) {
        writeSimple(431, "请求头过大");
        flushThenClose();
      }
      return;
    }
    HttpHead head;
    if (!parseHttpHead(std::string_view{in}.substr(0, headEnd), true, head)) {
      writeSimple(400, "请求头解析失败");
      flushThenClose();
      return;
    }
    const std::string transfer = lowerAscii(head.header("transfer-encoding"));
    if (!transfer.empty() && transfer.find("chunked") != std::string::npos) {
      // 分块请求体不做解码: 直接告诉调用方用 Content-Length
      writeSimple(411, "请求体请用 Content-Length");
      flushThenClose();
      return;
    }
    int64_t bodyLength = 0;
    const std::string lengthText = head.header("content-length");
    if (!lengthText.empty()) {
      try {
        bodyLength = std::stoll(lengthText);
      } catch (...) {
        writeSimple(400, "Content-Length 不是数字");
        flushThenClose();
        return;
      }
      if (bodyLength < 0) {
        writeSimple(400, "Content-Length 为负");
        flushThenClose();
        return;
      }
    }
    const int64_t maxBody = server ? server->maxBody : kMaxHttpBody;
    if (bodyLength > maxBody) {
      writeSimple(413, "请求体超过上限");
      flushThenClose();
      return;
    }
    if (in.size() < headEnd + 4 + static_cast<size_t>(bodyLength)) {
      return; ///< 正文还没到齐
    }
    std::string body = in.substr(headEnd + 4, static_cast<size_t>(bodyLength));
    in.erase(0, headEnd + 4 + static_cast<size_t>(bodyLength));

    const std::string connection = lowerAscii(head.header("connection"));
    keepAlive = (head.version == "HTTP/1.1" && connection != "close") ||
                connection == "keep-alive";
    currentMethod = head.method;

    std::string path;
    std::string query;
    splitTarget(head.target, path, query);
    if (server && !server->pathPrefix.empty() &&
        !startsWith(path, server->pathPrefix)) {
      writeResponseJson([] {
        Json out = Json::object();
        out["status"] = 404;
        out["body"] = "not found";
        out["contentType"] = "text/plain; charset=utf-8";
        return out;
      }());
      return;
    }

    waitingResponse = true;
    pendingRequestId = hub->registerPendingHttp(server, shared_from_this());
    if (pendingRequestId == 0) {
      waitingResponse = false;
      writeSimple(503, "等待回答的请求过多");
      flushThenClose();
      return;
    }
    Json event;
    event["kind"] = "http.request";
    event["serverId"] = serverId;
    event["requestId"] = pendingRequestId;
    event["method"] = toSafeText(head.method);
    event["path"] = toSafeText(path);
    event["query"] = toSafeText(query);
    event["target"] = toSafeText(head.target);
    event["version"] = head.version;
    event["headers"] = head.headersJson();
    event["remote"] = remote;
    event["bodyBytes"] = static_cast<int64_t>(body.size());
    const std::string bodyBase64 = utilxx_base::base64Encode(body);
    event["bodyBase64"] = bodyBase64;
    if (isValidUtf8(body)) {
      event["body"] = body;
      event["text"] = body;
    }
    hub->emit(plugin, std::move(event));
    armTimeout();
  }

  /// 用固定文本回一条简单响应 (解析失败 / 超限等内部错误)
  void writeSimple(int status, std::string_view text) {
    Json out = Json::object();
    out["status"] = status;
    out["contentType"] = "text/plain; charset=utf-8";
    out["body"] = std::string{text} + "\n";
    writeResponseJson(out);
  }

  /// 按插件给的响应写回去 (status / headers / body|bodyBase64|text / contentType)
  void writeResponseJson(const Json &response) {
    int status = 200;
    if (response.is_object() && response.contains("status")) {
      const Json &value = response["status"];
      if (value.is_number_integer()) {
        status = static_cast<int>(value.get<int64_t>());
      }
    }
    if (status < 100 || status > 599) {
      status = 500;
    }
    std::string body;
    bool bodyBase64 = false;
    if (response.is_object()) {
      const std::string base64 = response.value("bodyBase64", std::string{});
      if (!base64.empty()) {
        const auto decoded = utilxx_base::base64Decode(base64);
        if (decoded.has_value()) {
          body = *decoded;
          bodyBase64 = true;
        }
      }
      if (!bodyBase64) {
        body = response.value("body", std::string{});
        if (body.empty()) {
          body = response.value("text", std::string{});
        }
      }
    }

    std::string out = "HTTP/1.1 " + std::to_string(status) + " " +
                      statusTextOf(status) + "\r\n";
    const bool isHead = currentMethod == "HEAD";
    if (status == 204 || status == 304) {
      body.clear();
    }
    bool hasContentType = false;
    bool hasContentLength = false;
    bool hasConnection = false;
    if (response.is_object() && response["headers"].is_object()) {
      const Json &headers = response["headers"];
      for (const auto &item : headers.items()) {
        const std::string name{item.first};
        if (iequals(name, "content-type")) hasContentType = true;
        if (iequals(name, "content-length")) hasContentLength = true;
        if (iequals(name, "connection")) hasConnection = true;
        out += name + ": " + toSafeText(jsonTextOf(item.second)) + "\r\n";
      }
    }
    if (!hasContentType) {
      const std::string contentType =
          response.is_object()
              ? response.value("contentType",
                               std::string{"application/octet-stream"})
              : std::string{"application/octet-stream"};
      out += "Content-Type: " + contentType + "\r\n";
    }
    if (!hasContentLength) {
      out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    if (!hasConnection) {
      out += std::string{"Connection: "} + (keepAlive ? "keep-alive" : "close") +
             "\r\n";
    }
    out += "\r\n";
    if (!isHead) {
      out += body;
    }

    waitingResponse = false;
    pendingRequestId = 0;
    if (timeout) {
      AsioErrorCode ignored;
      timeout->cancel();
    }
    std::string err;
    send(std::move(out), err);
    if (!err.empty()) {
      closeStream("error", err);
      return;
    }
    if (!keepAlive) {
      flushThenClose();
      return;
    }
    // keep-alive: 写完以后继续读下一条 (剩余字节可能已经在缓冲里)
    process();
  }
};

/* ==================== TCP 连接 ==================== */

struct TcpStream : Stream {
  explicit TcpStream(std::shared_ptr<PluginNetHub::Impl> hubRef,
                     asio::ip::tcp::socket socketRef)
      : Stream(std::move(hubRef), std::move(socketRef)) {}

  std::shared_ptr<PluginNetHub::Impl::Server> server;

  /// 客户端连接状态 (服务端接受来的连接不用)
  std::unique_ptr<asio::ip::tcp::resolver> resolver;
  std::unique_ptr<asio::steady_timer> timer;

  /// 发起连接 (网络线程); 成功发 `socket.connected`, 失败发 `socket.closed`
  void connectTo(std::string host, uint16_t port, int32_t timeoutMs);
  void onConnected();

  /// 客户端还没连上 (这期间 send 要拒绝, 否则会写到未连接的 socket 上)
  bool connecting = false;

  const char *kind() const override { return "tcp"; }

  void onBytes(const char *data, size_t size) override {
    Json event;
    event["kind"] = "socket.data";
    event["id"] = id;
    if (!serverId.empty()) {
      event["serverId"] = serverId;
    }
    const std::string_view view{data, size};
    event["dataBase64"] =
        utilxx_base::base64Encode(std::string_view{data, size});
    if (isValidUtf8(view)) {
      event["text"] = std::string{view};
      event["data"] = std::string{view};
    }
    event["bytes"] = static_cast<int64_t>(size);
    hub->emit(plugin, std::move(event));
  }

  void onClosed(std::string_view reason, std::string_view error) override {
    Json event;
    event["kind"] = "socket.closed";
    event["id"] = id;
    if (!serverId.empty()) {
      event["serverId"] = serverId;
    }
    event["reason"] = std::string{reason};
    if (!error.empty()) {
      event["error"] = std::string{error};
    }
    hub->emit(plugin, std::move(event));
  }
};

/* ==================== HTTP 客户端 ==================== */

struct HttpClient : std::enable_shared_from_this<HttpClient> {
  std::shared_ptr<PluginNetHub::Impl> hub;
  std::string plugin;
  int64_t requestId = 0;

  // 参数
  std::string url;
  std::string method = "GET";
  Json headers = Json::object();
  std::string body;
  int64_t timeoutMs = kDefaultHttpTimeoutMs;
  int64_t maxBytes = kMaxHttpBody;
  bool followRedirects = true;
  int32_t maxRedirects = 3;
  TlsClientOptions tls; ///< https 时由 startHttpRequest 填好

  /// 本次调用是哪种形态 (结果 JSON 的字段不同)
  HttpMode mode = HttpMode::Request;
  /// fetch 的 bytes 形态才需要 bodyBase64
  bool wantBase64 = true;

  /// download: 落盘位置 (由 startHttpRequest 填好) 与写入状态
  std::string downloadDir;
  std::string downloadFileName;
  std::string downloadPath;
  std::FILE *downloadFile = nullptr;
  int64_t downloadBytes = 0;
  bool downloadWriteFailed = false;

  // 运行状态
  ParsedUrl target;
  std::unique_ptr<asio::ip::tcp::resolver> resolver;
  std::shared_ptr<NetStream> stream;
  std::unique_ptr<asio::steady_timer> timer;
  std::string requestData;
  std::vector<char> readBuf;
  std::string pending;
  HttpHead head;
  bool gotHead = false;
  HttpBodyDecoder decoder;
  int32_t redirects = 0;
  bool finished = false;
  int64_t startedAtMs = 0;
  int64_t contentLength = -1;
  std::string currentUrl;

  void begin() {
    finished = false;
    gotHead = false;
    pending.clear();
    head = HttpHead{};
    startedAtMs = nowMs();
    attempt();
  }

  void attempt() {
    closeStreamAndFile();
    if (resolver) {
      resolver.reset();
    }
    // 重定向会再次进来: 上一次响应剩下的字节不能带进下一次
    gotHead = false;
    pending.clear();
    head = HttpHead{};
    contentLength = -1;
    currentUrl = url;
    target = parseUrl(url);
    if (!target.ok) {
      fail("bad_url", target.error);
      return;
    }
    const bool secure = target.scheme == "https";
    if (!secure && target.scheme != "http") {
      fail("unsupported_scheme", "HTTP 客户端只支持 http:// 与 https://");
      return;
    }
    tls.enable = secure;
    tls.host = target.host;
    decoder = HttpBodyDecoder{};
    if (!timer) {
      timer = std::make_unique<asio::steady_timer>(hub->io);
    }
    timer->expires_after(std::chrono::milliseconds{(std::max)(int64_t{1000}, timeoutMs)});
    auto self = shared_from_this();
    timer->async_wait([self](AsioErrorCode ec) {
      if (ec) {
        return;
      }
      self->fail("timeout", "请求超时");
    });
    resolver = std::make_unique<asio::ip::tcp::resolver>(hub->io);
    resolver->async_resolve(target.host, std::to_string(target.port),
                            [self](AsioErrorCode ec,
                                   asio::ip::tcp::resolver::results_type results) {
                              if (ec) {
                                self->fail("resolve_failed", ec.message());
                                return;
                              }
                              self->onResolved(std::move(results));
                            });
  }

  void onResolved(asio::ip::tcp::resolver::results_type results) {
    stream = std::make_shared<NetStream>(asio::ip::tcp::socket(hub->io));
    auto self = shared_from_this();
    asio::async_connect(
        stream->lowest_layer(), results,
        [self](AsioErrorCode ec, const asio::ip::tcp::endpoint &) {
          if (ec) {
            self->fail("connect_failed", ec.message());
            return;
          }
          if (!self->tls.enable) {
            self->sendRequest();
            return;
          }
          // https: 先 TLS 握手, 再发请求
          self->stream->asyncStartTls(
              self->tls, [self](AsioErrorCode tlsEc, std::string code,
                                std::string message) {
                if (self->finished) {
                  return;
                }
                if (tlsEc) {
                  self->fail(code.empty() ? "tls_failed" : code, message);
                  return;
                }
                self->sendRequest();
              });
        });
  }

  void sendRequest() {
    std::string headText;
    headText.reserve(256);
    headText += method + " " + target.target + " HTTP/1.1\r\n";
    bool hasHost = false;
    bool hasContentType = false;
    bool hasAccept = false;
    if (headers.is_object()) {
      for (const auto &item : headers.items()) {
        const std::string name{item.first};
        if (iequals(name, "host")) hasHost = true;
        if (iequals(name, "content-type")) hasContentType = true;
        if (iequals(name, "accept")) hasAccept = true;
        // Content-Length / Connection 由这里决定, 不接受覆盖 (避免请求体错位)
        if (iequals(name, "content-length") || iequals(name, "connection") ||
            iequals(name, "transfer-encoding")) {
          continue;
        }
        headText += name + ": " + toSafeText(jsonTextOf(item.second)) + "\r\n";
      }
    }
    if (!hasHost) {
      headText += "Host: " + hostHeaderOf(target) + "\r\n";
    }
    if (!hasAccept) {
      headText += "Accept: */*\r\n";
    }
    if (!body.empty() && !hasContentType) {
      headText += "Content-Type: application/octet-stream\r\n";
    }
    headText += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    headText += "Connection: close\r\n";
    headText += "\r\n";
    requestData = headText + body;
    if (mode == HttpMode::Download && !downloadDir.empty() &&
        !openDownloadFile()) {
      return;
    }
    auto self = shared_from_this();
    asio::async_write(*stream, asio::buffer(requestData),
                      [self](AsioErrorCode ec, size_t) {
                        if (ec) {
                          self->fail("write_failed", ec.message());
                          return;
                        }
                        self->readSome();
                      });
  }

  void readSome() {
    readBuf.resize(kReadChunk);
    auto self = shared_from_this();
    stream->async_read_some(
        asio::buffer(readBuf), [self](AsioErrorCode ec, size_t size) {
          if (size > 0) {
            self->onData(self->readBuf.data(), size);
            if (self->finished) {
              return;
            }
          }
          if (ec) {
            if (NetStream::isPeerClosed(ec)) {
              self->onEof();
            } else {
              self->fail("read_failed", ec.message());
            }
            return;
          }
          self->readSome();
        });
  }

  void onData(const char *data, size_t size) {
    // 先拼进 pending: 头还没读完时它就是响应头缓冲区, 头读完之后它就是尚未解析的正文。
    // (响应可能分很多次读到 —— TLS 下尤其如此; 只拼头不拼后续数据会把后半段正文丢掉,
    //  表现为 "连接在响应正文读完前关闭")
    pending.append(data, size);
    if (!gotHead) {
      const size_t headEnd = pending.find("\r\n\r\n");
      if (headEnd == std::string::npos) {
        if (pending.size() > kMaxHeadBytes) {
          fail("bad_response", "响应头过大");
        }
        return;
      }
      std::string headText = pending.substr(0, headEnd);
      pending.erase(0, headEnd + 4);
      if (!parseHttpHead(headText, false, head)) {
        fail("bad_response", "响应头解析失败");
        return;
      }
      gotHead = true;

      // 重定向: 换地址重来 (正文不读, 直接关掉当前连接)
      const std::string location = head.header("location");
      const bool isRedirect = head.status == 301 || head.status == 302 ||
                              head.status == 303 || head.status == 307 ||
                              head.status == 308;
      if (isRedirect && followRedirects && !location.empty() &&
          redirects < (std::max)(0, static_cast<int>(maxRedirects))) {
        if (head.status == 303 ||
            (head.status == 301 || head.status == 302)) {
          method = "GET";
          body.clear();
        }
        ++redirects;
        const std::string next = absoluteUrl(location);
        AsioErrorCode ignored;
        timer->cancel();
        if (stream) {
          stream->close();
          stream.reset();
        }
        url = next;
        attempt();
        return;
      }

      const std::string transfer = lowerAscii(head.header("transfer-encoding"));
      const std::string lengthText = head.header("content-length");
      const bool noBody = method == "HEAD" || head.status == 204 ||
                          head.status == 304;
      // 下载: 正文直接流式写文件, 内存里不留 (只受下载上限约束)
      const int64_t bodyLimit =
          mode == HttpMode::Download ? (int64_t{8} * 1024 * 1024 * 1024) : maxBytes;
      if (noBody) {
        decoder.init(HttpBodyDecoder::Mode::None, 0, bodyLimit);
      } else if (transfer.find("chunked") != std::string::npos) {
        decoder.init(HttpBodyDecoder::Mode::Chunked, 0, bodyLimit);
      } else if (!lengthText.empty()) {
        int64_t length = 0;
        try {
          length = std::stoll(lengthText);
        } catch (...) {
          length = -1;
        }
        contentLength = length;
        if (length < 0) {
          decoder.init(HttpBodyDecoder::Mode::UntilClose, 0, bodyLimit);
        } else {
          decoder.init(HttpBodyDecoder::Mode::Length, length, bodyLimit);
        }
      } else {
        decoder.init(HttpBodyDecoder::Mode::UntilClose, 0, bodyLimit);
      }
    }
    if (!pending.empty()) {
      decoder.feed(pending.data(), pending.size());
      pending.clear();
      if (downloadWriteFailed) {
        fail("write_failed", "下载文件写入失败: " + downloadPath);
        return;
      }
    }
    if (decoder.done()) {
      complete();
    } else if (decoder.truncated()) {
      complete();
    }
  }

  void onEof() {
    if (!gotHead) {
      fail("bad_response", "连接在响应头读完前关闭");
      return;
    }
    decoder.finishOnClose();
    if (decoder.done() || decoder.truncated()) {
      complete();
    } else {
      std::string detail = "连接在响应正文读完前关闭 (已收到 " +
                           std::to_string(decoder.received()) + " 字节";
      if (contentLength >= 0) {
        detail += " / 声明 " + std::to_string(contentLength);
      }
      detail += ")";
      fail("incomplete", detail);
    }
  }

  void complete() {
    if (finished) {
      return;
    }
    finished = true;
    if (timer) {
      timer->cancel();
    }
    closeStreamAndFile();
    Json out = Json::object();
    out["ok"] = true;
    out["url"] = currentUrl;
    out["status"] = head.status;
    out["statusText"] = head.reason;
    out["headers"] = head.headersJson();
    out["contentType"] = head.header("content-type");
    out["contentLength"] = contentLength;
    out["receivedBytes"] = decoder.received();
    out["truncated"] = decoder.truncated();
    out["redirects"] = redirects;
    out["elapsedMs"] = nowMs() - startedAtMs;
    if (mode == HttpMode::Download) {
      out["path"] = downloadPath;
      out["bytes"] = downloadBytes;
    } else {
      const std::string &bodyText = decoder.body();
      out["bodyBytes"] = static_cast<int64_t>(bodyText.size());
      if (wantBase64) {
        out["bodyBase64"] = utilxx_base::base64Encode(bodyText);
      }
      if (isValidUtf8(bodyText)) {
        out["body"] = bodyText;
      }
    }
    hub->finishHttpRequest(requestId, true, std::move(out));
  }

  void fail(std::string_view code, std::string_view message) {
    if (finished) {
      return;
    }
    finished = true;
    if (timer) {
      timer->cancel();
    }
    closeStreamAndFile();
    Json out = errorJson(code, message);
    out["url"] = currentUrl;
    out["redirects"] = redirects;
    out["elapsedMs"] = nowMs() - startedAtMs;
    hub->finishHttpRequest(requestId, false, std::move(out));
  }

  /// 关连接; 退出时把下载文件也收尾 (失败时把半份文件删掉)
  void closeStreamAndFile() {
    if (stream) {
      stream->close();
      stream.reset();
    }
    if (downloadFile) {
      std::fclose(downloadFile);
      downloadFile = nullptr;
      if (finished && downloadBytes == 0 && !downloadPath.empty()) {
        // 一个字节都没写下来就不要留空文件
        std::remove(downloadPath.c_str());
        downloadPath.clear();
      }
    }
  }

  /// 创建下载文件并给它接上写入回调 (目录由 startHttpRequest 准备好)
  bool openDownloadFile() {
    downloadPath = downloadDir + "/" + downloadFileName;
    downloadFile = std::fopen(downloadPath.c_str(), "wb");
    if (!downloadFile) {
      downloadPath.clear();
      fail("write_failed", "下载文件建不出来: " + downloadDir + "/" + downloadFileName);
      return false;
    }
    downloadBytes = 0;
    downloadWriteFailed = false;
    decoder.setSink([this](const char *data, size_t size) {
      if (!downloadFile || downloadWriteFailed) {
        return;
      }
      if (std::fwrite(data, 1, size, downloadFile) != size) {
        downloadWriteFailed = true;
        return;
      }
      downloadBytes += static_cast<int64_t>(size);
    });
    return true;
  }

  static int64_t nowMs() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }

  /// 相对 Location → 绝对地址 (同协议; 保留原来是哪种 scheme)
  std::string absoluteUrl(const std::string &location) const {
    if (location.find("://") != std::string::npos) {
      return location;
    }
    const std::string origin = originOf(target);
    if (!location.empty() && location.front() == '/') {
      return origin + location;
    }
    std::string base = target.path;
    const size_t slash = base.find_last_of('/');
    if (slash != std::string::npos) {
      base = base.substr(0, slash + 1);
    } else {
      base = "/";
    }
    return origin + base + location;
  }
};

/* ==================== HTTP 客户端: 中心侧入口 ==================== */

bool PluginNetHub::Impl::startHttpRequest(const std::string &plugin,
                                          int64_t requestId, const Json &args,
                                          DoneFn done, Json &out,
                                          int32_t mode) {
  if (!args.is_object()) {
    out = errorJson("bad_args", "参数必须是对象");
    return false;
  }
  const std::string url = args.value("url", std::string{});
  if (url.empty()) {
    out = errorJson("bad_args", "缺少 url");
    return false;
  }
  size_t active = 0;
  for (const auto &item : httpWaits) {
    if (item.second.plugin == plugin) {
      ++active;
    }
  }
  if (active >= kMaxHttpClientsPerPlugin) {
    out = errorJson("too_many_requests", "同时在进行的 HTTP 请求过多");
    return false;
  }

  auto client = std::make_shared<HttpClient>();
  client->hub = shared_from_this();
  client->plugin = plugin;
  client->requestId = requestId;
  client->url = url;
  client->mode = static_cast<HttpMode>(mode);
  client->method = args.value("method", std::string{"GET"});
  for (char &c : client->method) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  if (args["headers"].is_object()) {
    client->headers = args["headers"];
  }
  const std::string bodyBase64 = args.value("bodyBase64", std::string{});
  if (!bodyBase64.empty()) {
    const auto decoded = utilxx_base::base64Decode(bodyBase64);
    if (!decoded.has_value()) {
      out = errorJson("bad_base64", "bodyBase64 解不开");
      return false;
    }
    client->body = *decoded;
  } else {
    const std::string body = args.value("body", std::string{});
    if (!body.empty()) {
      client->body = body;
    } else if (args.contains("json")) {
      client->body = args["json"].dump();
      if (!client->headers.contains("Content-Type")) {
        client->headers["Content-Type"] = "application/json";
      }
    }
  }
  const int64_t timeoutMs = args.value("timeoutMs", int64_t{kDefaultHttpTimeoutMs});
  client->timeoutMs = (std::max)(int64_t{1000}, (std::min)(timeoutMs, int64_t{120000}));
  const int64_t maxBytes = args.value("maxBytes", int64_t{kMaxHttpBody});
  client->maxBytes = (std::max)(int64_t{1024}, (std::min)(maxBytes, int64_t{16 * 1024 * 1024}));
  client->followRedirects = args.value("followRedirects", true);
  client->maxRedirects = static_cast<int32_t>(
      (std::max)(int64_t{0}, (std::min)(args.value("maxRedirects", int64_t{3}),
                                        int64_t{5})));

  // TLS 选项 (https 才用得到)
  parseTlsOptions(args, client->tls);

  // 结果形态
  if (client->mode == HttpMode::Fetch) {
    const std::string responseType = args.value("responseType", std::string{"text"});
    client->wantBase64 = responseType == "bytes";
  } else if (client->mode == HttpMode::Download) {
    // 下载只做 GET 落盘 (与 `musicxx.net.download` 的参数/行为一致)
    if (client->method != "GET") {
      out = errorJson("bad_args", "下载只支持 GET");
      return false;
    }
    if (dataDir.empty()) {
      out = errorJson("no_data_dir",
                      "插件数据目录未就绪 (host_config.data_dir 为空), 不能下载到本地");
      return false;
    }
    const std::string fileName =
        downloadFileNameOf(args.value("fileName", std::string{}), url);
    client->downloadDir = dataDir + "/" + plugin + "/data/downloads";
    client->downloadFileName = fileName;
    // 目录先建出来; 建不出来就让调用方拿到可读原因
    std::error_code dirEc;
    std::filesystem::create_directories(client->downloadDir, dirEc);
    if (dirEc) {
      out = errorJson("write_failed", "下载目录建不出来: " + client->downloadDir +
                                          " (" + dirEc.message() + ")");
      return false;
    }
  }

  HttpWait wait;
  wait.plugin = plugin;
  wait.done = std::move(done);
  wait.client = client;
  httpWaits[requestId] = std::move(wait);

  out = Json::object();
  out["ok"] = true;
  out["requestId"] = requestId;
  client->begin();
  return true;
}

void PluginNetHub::Impl::finishHttpRequest(int64_t requestId, bool ok,
                                           Json payload) {
  const auto it = httpWaits.find(requestId);
  if (it == httpWaits.end()) {
    return;
  }
  HttpWait wait = std::move(it->second);
  httpWaits.erase(it);
  payload["requestId"] = requestId;
  const std::string text = payload.dump();
  if (wait.done) {
    DoneFn done = wait.done;
    postToFramework([done, ok, text] {
      if (done) {
        done(ok ? PLUGINXX_OPERATOR_OK : PLUGINXX_OPERATOR_FAILED, text);
      }
    });
  }
  payload["kind"] = "http.response";
  payload["ok"] = payload.value("ok", ok);
  emit(wait.plugin, std::move(payload));
}

void PluginNetHub::Impl::cancelHttpRequest(const std::string &plugin,
                                           int64_t requestId, Json &out) {
  const auto it = httpWaits.find(requestId);
  if (it == httpWaits.end() || it->second.plugin != plugin) {
    out = errorJson("not_found", "没有这个进行中的请求");
    return;
  }
  auto client = it->second.client;
  if (client) {
    client->fail("cancelled", "请求已取消");
  }
  out = Json::object();
  out["ok"] = true;
  out["requestId"] = requestId;
}

} // namespace extern_plugin
} // namespace musicxx

namespace musicxx {
namespace extern_plugin {

/* ==================== TCP 客户端连接 ==================== */

void TcpStream::connectTo(std::string host, uint16_t port, int32_t timeoutMs) {
  connecting = true;
  timer = std::make_unique<asio::steady_timer>(hub->io);
  timer->expires_after(std::chrono::milliseconds{(std::max)(timeoutMs, 1000)});
  auto self = std::static_pointer_cast<TcpStream>(shared_from_this());
  timer->async_wait([self](AsioErrorCode ec) {
    if (!ec) {
      self->closeStream("timeout", "连接超时");
    }
  });
  resolver = std::make_unique<asio::ip::tcp::resolver>(hub->io);
  resolver->async_resolve(
      host, std::to_string(port),
      [self](AsioErrorCode ec, asio::ip::tcp::resolver::results_type results) {
        if (ec) {
          self->closeStream("connect_failed", "解析失败: " + ec.message());
          return;
        }
        asio::async_connect(
            self->io->lowest_layer(), results,
            [self](AsioErrorCode connectEc, const asio::ip::tcp::endpoint &) {
              if (connectEc) {
                self->closeStream("connect_failed",
                                  "连接失败: " + connectEc.message());
                return;
              }
              self->onConnected();
            });
      });
}

void TcpStream::onConnected() {
  connecting = false;
  if (timer) {
    AsioErrorCode ignored;
    timer->cancel();
  }
  AsioErrorCode ec;
  remote = endpointText(io->lowest_layer().remote_endpoint(ec));
  local = endpointText(io->lowest_layer().local_endpoint(ec));
  Json event;
  event["kind"] = "socket.connected";
  event["id"] = id;
  event["remote"] = remote;
  event["local"] = local;
  hub->emit(plugin, std::move(event));
  start();
}

/* ==================== WebSocket ==================== */

struct WsStream : Stream {
  explicit WsStream(std::shared_ptr<PluginNetHub::Impl> hubRef,
                    asio::ip::tcp::socket socketRef)
      : Stream(std::move(hubRef), std::move(socketRef)) {}

  bool roleClient = true;
  std::shared_ptr<PluginNetHub::Impl::Server> server;

  // 客户端参数
  std::string url;
  std::string clientKey;
  Json extraHeaders = Json::object();
  std::vector<std::string> subprotocols;
  int32_t connectTimeoutMs = 8000;
  TlsClientOptions tlsOptions; ///< wss 时由 ws.connect 的参数填好

  // 双方共用
  std::string subprotocol;
  int64_t maxMessage = kMaxWsMessage;
  bool open = false;
  bool closeSent = false;
  std::string handshakeIn;
  std::string in;
  std::string messageAccum;
  int32_t messageOpcode = 0;
  std::string closeBy; ///< remote / local / error / plugin_unloaded
  int32_t closeCode = 0;
  std::string closeReason;

  std::unique_ptr<asio::ip::tcp::resolver> resolver;
  std::unique_ptr<asio::steady_timer> timer;

  const char *kind() const override { return "ws"; }

  /* ---------- 读 ---------- */

  void onBytes(const char *data, size_t size) override {
    if (!open) {
      handshakeIn.append(data, size);
      if (roleClient) {
        handleClientHandshake();
      } else {
        handleServerHandshake();
      }
      return;
    }
    in.append(data, size);
    drainFrames();
  }

  void onClosed(std::string_view reason, std::string_view error) override {
    if (timer) {
      AsioErrorCode ignored;
      timer->cancel();
    }
    Json event;
    event["kind"] = "ws.close";
    event["id"] = id;
    if (!serverId.empty()) {
      event["serverId"] = serverId;
    }
    if (closeBy.empty()) {
      // closeStream 的原因串直接映射成"谁关的"
      if (reason == "local_close") {
        closeBy = "local";
      } else if (reason == "remote_closed") {
        closeBy = "remote";
      } else if (reason == "plugin_unloaded") {
        closeBy = "plugin";
      } else {
        closeBy = "error";
      }
    }
    event["by"] = closeBy;
    if (closeCode != 0) {
      event["code"] = closeCode;
    }
    if (!closeReason.empty()) {
      event["reason"] = toSafeText(closeReason);
    }
    if (!error.empty()) {
      event["error"] = std::string{error};
    }
    hub->emit(plugin, std::move(event));
  }

  /* ---------- 客户端: 连接 + 握手 ---------- */

  void startClient() {
    ParsedUrl target = parseUrl(url);
    if (!target.ok) {
      closeBy = "error";
      closeStream("error", target.error);
      return;
    }
    const bool secure = target.scheme == "wss";
    if (target.scheme != "ws" && !secure) {
      closeBy = "error";
      closeStream("error", "WebSocket 地址只支持 ws:// 或 wss://");
      return;
    }
    tlsOptions.enable = secure;
    tlsOptions.host = target.host;
    armTimer(connectTimeoutMs);
    resolver = std::make_unique<asio::ip::tcp::resolver>(hub->io);
    auto self = std::static_pointer_cast<WsStream>(shared_from_this());
    resolver->async_resolve(
        target.host, std::to_string(target.port),
        [self, target](AsioErrorCode ec,
                       asio::ip::tcp::resolver::results_type results) {
          if (ec) {
            self->closeBy = "error";
            self->closeStream("connect_failed", "解析失败: " + ec.message());
            return;
          }
          asio::async_connect(
              self->io->lowest_layer(), results,
              [self, target](AsioErrorCode connectEc,
                             const asio::ip::tcp::endpoint &) {
                if (connectEc) {
                  self->closeBy = "error";
                  self->closeStream("connect_failed",
                                    "连接失败: " + connectEc.message());
                  return;
                }
                AsioErrorCode ignored;
                self->remote = endpointText(
                    self->io->lowest_layer().remote_endpoint(ignored));
                self->local = endpointText(
                    self->io->lowest_layer().local_endpoint(ignored));
                if (!self->tlsOptions.enable) {
                  self->sendClientHandshake(target);
                  return;
                }
                // wss: 先完成 TLS 握手, 再在上面跑 WebSocket 握手
                self->io->asyncStartTls(
                    self->tlsOptions,
                    [self, target](AsioErrorCode tlsEc, std::string code,
                                   std::string message) {
                      if (self->closing) {
                        return;
                      }
                      if (tlsEc) {
                        self->closeBy = "error";
                        self->closeStream(code.empty() ? "tls_failed" : code,
                                          message);
                        return;
                      }
                      self->sendClientHandshake(target);
                    });
              });
        });
  }

  void sendClientHandshake(const ParsedUrl &target) {
    clientKey = wsClientKey();
    std::string request = "GET " + target.target + " HTTP/1.1\r\n";
    request += "Host: " + hostHeaderOf(target) + "\r\n";
    request += "Upgrade: websocket\r\n";
    request += "Connection: Upgrade\r\n";
    request += "Sec-WebSocket-Key: " + clientKey + "\r\n";
    request += "Sec-WebSocket-Version: 13\r\n";
    if (!subprotocols.empty()) {
      request += "Sec-WebSocket-Protocol: ";
      for (size_t i = 0; i < subprotocols.size(); ++i) {
        if (i > 0) {
          request += ", ";
        }
        request += subprotocols[i];
      }
      request += "\r\n";
    }
    if (extraHeaders.is_object()) {
      for (const auto &item : extraHeaders.items()) {
        const std::string name{item.first};
        if (iequals(name, "host") || iequals(name, "upgrade") ||
            iequals(name, "connection") || iequals(name, "sec-websocket-key") ||
            iequals(name, "sec-websocket-version") ||
            iequals(name, "sec-websocket-protocol")) {
          continue;
        }
        request += name + ": " + toSafeText(jsonTextOf(item.second)) + "\r\n";
      }
    }
    request += "\r\n";
    armTimer(connectTimeoutMs);
    std::string err;
    send(std::move(request), err);
    if (!err.empty()) {
      closeBy = "error";
      closeStream("error", err);
      return;
    }
    start(); ///< 读循环 (等 101)
  }

  void handleClientHandshake() {
    const size_t headEnd = handshakeIn.find("\r\n\r\n");
    if (headEnd == std::string::npos) {
      if (handshakeIn.size() > kMaxHeadBytes) {
        failHandshake("握手响应头过大");
      }
      return;
    }
    HttpHead head;
    const std::string headText = handshakeIn.substr(0, headEnd);
    handshakeIn.erase(0, headEnd + 4);
    if (!parseHttpHead(headText, false, head) || head.status != 101) {
      failHandshake("握手被拒绝 (状态 " + std::to_string(head.status) + ")");
      return;
    }
    const std::string accept = head.header("sec-websocket-accept");
    if (accept != wsAcceptKey(clientKey)) {
      failHandshake("Sec-WebSocket-Accept 不匹配");
      return;
    }
    subprotocol = head.header("sec-websocket-protocol");
    open = true;
    if (timer) {
      AsioErrorCode ignored;
      timer->cancel();
    }
    Json event;
    event["kind"] = "ws.open";
    event["id"] = id;
    event["url"] = url;
    event["remote"] = remote;
    event["headers"] = head.headersJson();
    if (!subprotocol.empty()) {
      event["subprotocol"] = subprotocol;
    }
    hub->emit(plugin, std::move(event));
    if (!handshakeIn.empty()) {
      in = std::move(handshakeIn);
      handshakeIn.clear();
      drainFrames();
    }
  }

  /* ---------- 服务端: 握手 ---------- */

  void handleServerHandshake() {
    const size_t headEnd = handshakeIn.find("\r\n\r\n");
    if (headEnd == std::string::npos) {
      if (handshakeIn.size() > kMaxHeadBytes) {
        failHandshake("握手请求头过大");
      }
      return;
    }
    HttpHead head;
    const std::string headText = handshakeIn.substr(0, headEnd);
    handshakeIn.erase(0, headEnd + 4);
    if (!parseHttpHead(headText, true, head)) {
      failHandshake("握手请求头解析失败");
      return;
    }
    const std::string key = head.header("sec-websocket-key");
    const std::string upgrade = lowerAscii(head.header("upgrade"));
    const std::string connection = lowerAscii(head.header("connection"));
    if (key.empty() || upgrade.find("websocket") == std::string::npos ||
        connection.find("upgrade") == std::string::npos) {
      failHandshake("不是合法的 WebSocket 握手");
      return;
    }
    std::string path;
    std::string query;
    splitTarget(head.target, path, query);
    if (server && !server->pathPrefix.empty() &&
        !startsWith(path, server->pathPrefix)) {
      std::string err;
      send("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
           err);
      closeBy = "error";
      closeStream("handshake_rejected", "路径不匹配");
      return;
    }
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
        wsAcceptKey(key) + "\r\n";
    const std::string wanted = head.header("sec-websocket-protocol");
    if (!wanted.empty()) {
      // 服务端不声明子协议: 原样回第一个是常见做法, 但更安全的是不回
      // (客户端会按"没选"处理); 这里保持不回。
    }
    response += "\r\n";
    std::string err;
    send(std::move(response), err);
    if (!err.empty()) {
      closeBy = "error";
      closeStream("error", err);
      return;
    }
    open = true;
    Json event;
    event["kind"] = "ws.open";
    event["id"] = id;
    if (!serverId.empty()) {
      event["serverId"] = serverId;
    }
    event["path"] = toSafeText(path);
    event["query"] = toSafeText(query);
    event["remote"] = remote;
    event["headers"] = head.headersJson();
    hub->emit(plugin, std::move(event));
    if (!handshakeIn.empty()) {
      in = std::move(handshakeIn);
      handshakeIn.clear();
      drainFrames();
    }
  }

  void failHandshake(std::string_view message) {
    closeBy = "error";
    // 服务端: 给对端一个可读的拒绝响应 (客户端: 直接关)
    if (!roleClient && !closing) {
      std::string err;
      send("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
           err);
    }
    closeStream("handshake_failed", message);
  }

  void armTimer(int32_t timeoutMs) {
    if (!timer) {
      timer = std::make_unique<asio::steady_timer>(hub->io);
    }
    AsioErrorCode ignored;
    timer->cancel();
    timer->expires_after(
        std::chrono::milliseconds{(std::max)(timeoutMs, 1000)});
    auto self = std::static_pointer_cast<WsStream>(shared_from_this());
    timer->async_wait([self](AsioErrorCode ec) {
      if (!ec && !self->open) {
        self->closeBy = "error";
        self->closeStream("timeout", "握手超时");
      }
    });
  }

  /* ---------- 帧 ---------- */

  void drainFrames() {
    while (open && !closing) {
      if (in.size() < 2) {
        return;
      }
      const auto b0 = static_cast<unsigned char>(in[0]);
      const auto b1 = static_cast<unsigned char>(in[1]);
      const bool fin = (b0 & 0x80) != 0;
      const int32_t opcode = b0 & 0x0F;
      const bool masked = (b1 & 0x80) != 0;
      if ((b0 & 0x70) != 0) {
        protocolError(1002, "保留位不为 0");
        return;
      }
      uint64_t length = b1 & 0x7F;
      size_t offset = 2;
      if (length == 126) {
        if (in.size() < 4) {
          return;
        }
        length = (static_cast<uint64_t>(static_cast<unsigned char>(in[2])) << 8) |
                 static_cast<unsigned char>(in[3]);
        offset = 4;
      } else if (length == 127) {
        if (in.size() < 10) {
          return;
        }
        uint64_t value = 0;
        for (size_t i = 2; i < 10; ++i) {
          value = (value << 8) | static_cast<unsigned char>(in[i]);
        }
        if ((value & 0x8000000000000000ull) != 0) {
          protocolError(1002, "帧长度最高位为 1");
          return;
        }
        length = value;
        offset = 10;
      }
      if (length > static_cast<uint64_t>(maxMessage)) {
        protocolError(1009, "消息超过上限");
        return;
      }
      if (!roleClient && !masked) {
        protocolError(1002, "客户端发来的帧必须带掩码");
        return;
      }
      if (roleClient && masked) {
        protocolError(1002, "服务端发来的帧不能带掩码");
        return;
      }
      const size_t maskLength = masked ? 4 : 0;
      if (in.size() < offset + maskLength + length) {
        return;
      }
      std::string payload = in.substr(offset + maskLength, length);
      if (masked) {
        const unsigned char *mask =
            reinterpret_cast<const unsigned char *>(in.data() + offset);
        for (size_t i = 0; i < payload.size(); ++i) {
          payload[i] = static_cast<char>(
              static_cast<unsigned char>(payload[i]) ^ mask[i % 4]);
        }
      }
      in.erase(0, offset + maskLength + length);
      handleFrame(fin, opcode, std::move(payload));
    }
  }

  void handleFrame(bool fin, int32_t opcode, std::string payload) {
    switch (opcode) {
    case 0x0: { // 分片续帧
      if (messageOpcode == 0) {
        protocolError(1002, "没有开始的分片");
        return;
      }
      appendFragment(std::move(payload), fin);
      return;
    }
    case 0x1:
    case 0x2: { // 文本 / 二进制
      if (messageOpcode != 0) {
        protocolError(1002, "上一条分片消息还没结束");
        return;
      }
      if (!fin) {
        messageOpcode = opcode;
        messageAccum = std::move(payload);
        if (static_cast<int64_t>(messageAccum.size()) > maxMessage) {
          protocolError(1009, "消息超过上限");
        }
        return;
      }
      deliverMessage(opcode, payload);
      return;
    }
    case 0x8:
      handleCloseFrame(payload);
      return;
    case 0x9: { // ping → pong
      std::string err;
      sendFrame(0xA, payload, err);
      return;
    }
    case 0xA: { // pong
      Json event;
      event["kind"] = "ws.pong";
      event["id"] = id;
      if (!serverId.empty()) {
        event["serverId"] = serverId;
      }
      event["bytes"] = static_cast<int64_t>(payload.size());
      hub->emit(plugin, std::move(event));
      return;
    }
    default:
      protocolError(1002, "未知帧类型");
      return;
    }
  }

  void appendFragment(std::string payload, bool fin) {
    messageAccum += payload;
    if (static_cast<int64_t>(messageAccum.size()) > maxMessage) {
      protocolError(1009, "消息超过上限");
      return;
    }
    if (fin) {
      const int32_t opcode = messageOpcode;
      std::string full = std::move(messageAccum);
      messageOpcode = 0;
      messageAccum.clear();
      deliverMessage(opcode, full);
    }
  }

  void deliverMessage(int32_t opcode, const std::string &payload) {
    Json event;
    event["kind"] = "ws.message";
    event["id"] = id;
    if (!serverId.empty()) {
      event["serverId"] = serverId;
    }
    event["binary"] = opcode == 0x2;
    event["dataBase64"] = utilxx_base::base64Encode(payload);
    if (opcode == 0x1) {
      event["text"] = payload;
    }
    event["bytes"] = static_cast<int64_t>(payload.size());
    hub->emit(plugin, std::move(event));
  }

  void handleCloseFrame(const std::string &payload) {
    closeBy = "remote";
    if (payload.size() >= 2) {
      closeCode = (static_cast<unsigned char>(payload[0]) << 8) |
                  static_cast<unsigned char>(payload[1]);
      closeReason = payload.substr(2);
    } else {
      closeCode = 1005; ///< 没有状态码
    }
    if (!closeSent) {
      std::string echo;
      if (payload.size() >= 2) {
        echo = payload.substr(0, 2);
      }
      std::string err;
      sendFrame(0x8, echo, err);
      closeSent = true;
    }
    flushThenClose();
  }

  void protocolError(int32_t code, std::string_view message) {
    closeBy = "error";
    if (!closeSent) {
      std::string err;
      sendFrame(0x8, closePayload(code, std::string{message}), err);
      closeSent = true;
    }
    closeStream("protocol_error", message);
  }

  static std::string closePayload(int32_t code, const std::string &reason) {
    std::string payload;
    payload.push_back(static_cast<char>((code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(code & 0xFF));
    payload += reason.substr(0, 120);
    return payload;
  }

  void sendFrame(int32_t opcode, std::string_view payload, std::string &err) {
    if (!open || closing) {
      err = "not_open";
      return;
    }
    std::string frame;
    frame.reserve(payload.size() + 14);
    frame.push_back(static_cast<char>(0x80 | (opcode & 0x0F)));
    const bool mask = roleClient;
    const uint64_t size = payload.size();
    if (size < 126) {
      frame.push_back(static_cast<char>((mask ? 0x80 : 0x00) | size));
    } else if (size <= 0xFFFF) {
      frame.push_back(static_cast<char>((mask ? 0x80 : 0x00) | 126));
      frame.push_back(static_cast<char>((size >> 8) & 0xFF));
      frame.push_back(static_cast<char>(size & 0xFF));
    } else {
      frame.push_back(static_cast<char>((mask ? 0x80 : 0x00) | 127));
      for (int i = 7; i >= 0; --i) {
        frame.push_back(static_cast<char>((size >> (8 * i)) & 0xFF));
      }
    }
    if (mask) {
      const auto key = randomBytes(4);
      frame.append(reinterpret_cast<const char *>(key.data()), 4);
      const size_t start = frame.size();
      frame.append(payload);
      for (size_t i = 0; i < size; ++i) {
        frame[start + i] = static_cast<char>(
            static_cast<unsigned char>(frame[start + i]) ^ key[i % 4]);
      }
    } else {
      frame.append(payload);
    }
    send(std::move(frame), err);
  }

  /// 插件主动关闭 (`ws.close` / `socket.close`)
  void closeByPlugin(int32_t code, const std::string &reason) {
    if (closeBy.empty()) {
      closeBy = "local";
    }
    closeCode = code;
    closeReason = reason;
    if (!closeSent && open) {
      std::string err;
      sendFrame(0x8, closePayload(code, reason), err);
      closeSent = true;
    }
    flushThenClose();
  }
};

/* ==================== 接受连接 (需要各种 Stream 已定义) ==================== */

void PluginNetHub::Impl::onAccepted(const std::shared_ptr<Server> &server,
                                    asio::ip::tcp::socket socket) {
  if (!server || server->conns.size() >=
                     static_cast<size_t>(server->maxConnections) ||
      connectionCountOf(server->plugin) >= kMaxConnectionsPerPlugin) {
    AsioErrorCode ignored;
    socket.close(ignored);
    return;
  }
  const int64_t seq = nextConnSeq.fetch_add(1, std::memory_order_relaxed);
  const std::string connId = server->id + "#" + std::to_string(seq);

  std::shared_ptr<Stream> stream;
  if (server->kind == "http") {
    auto http = std::make_shared<HttpServerStream>(shared_from_this(),
                                                   std::move(socket));
    http->server = server;
    stream = http;
  } else if (server->kind == "ws") {
    auto ws = std::make_shared<WsStream>(shared_from_this(), std::move(socket));
    ws->server = server;
    ws->roleClient = false;
    // 握手要有上界: 连上不发 (或发坏的) 握手不能一直占着连接
    ws->armTimer(10000);
    stream = ws;
  } else {
    auto tcp = std::make_shared<TcpStream>(shared_from_this(), std::move(socket));
    tcp->server = server;
    stream = tcp;
  }
  stream->plugin = server->plugin;
  stream->serverId = server->id;
  stream->id = connId;
  if (!registerStream(stream)) {
    stream->io->close();
    return;
  }
  if (auto tcp = std::dynamic_pointer_cast<TcpStream>(stream)) {
    Json event;
    event["kind"] = "socket.accepted";
    event["serverId"] = server->id;
    event["id"] = stream->id;
    event["remote"] = stream->remote;
    event["local"] = stream->local;
    emit(server->plugin, std::move(event));
  }
  stream->start();
}

/* ==================== 操作分发 ==================== */

void PluginNetHub::Impl::stopServers(const std::string &plugin,
                                     const std::string &kind,
                                     const std::string &id, Json &out) {
  std::vector<std::shared_ptr<Server>> closing;
  for (auto it = servers.begin(); it != servers.end();) {
    if (it->second && it->second->plugin == plugin &&
        (id.empty() || it->second->id == id)) {
      closing.push_back(it->second);
      it = servers.erase(it);
    } else {
      ++it;
    }
  }
  int32_t stopped = 0;
  for (const auto &server : closing) {
    ++stopped;
    if (server->acceptor) {
      AsioErrorCode ignored;
      server->acceptor->close(ignored);
    }
    std::vector<std::string> connIds(server->conns.begin(), server->conns.end());
    for (const auto &connId : connIds) {
      const auto stream = findConn(plugin, connId);
      if (stream) {
        stream->closeStream("server_stopped", {});
      }
    }
    Json event;
    event["kind"] = kind == "http" ? "http.serverClosed"
                      : kind == "ws" ? "ws.serverClosed"
                                     : "socket.serverClosed";
    event["id"] = server->id;
    emit(plugin, std::move(event));
  }
  out = Json::object();
  out["ok"] = true;
  out["stopped"] = stopped;
}

void PluginNetHub::Impl::closeConn(const std::string &plugin,
                                   const std::string &id, Json &out) {
  const auto stream = findConn(plugin, id);
  if (!stream) {
    out = errorJson("not_found", "没有这条连接");
    return;
  }
  if (auto ws = std::dynamic_pointer_cast<WsStream>(stream)) {
    ws->closeByPlugin(1000, {});
  } else {
    stream->closeStream("local_close", {});
  }
  out = Json::object();
  out["ok"] = true;
  out["id"] = id;
}

void PluginNetHub::Impl::sendConn(const std::string &plugin, const Json &args,
                                  Json &out) {
  const std::string id = args.value("id", std::string{});
  if (id.empty()) {
    out = errorJson("bad_args", "缺少连接 id");
    return;
  }
  const auto stream = findConn(plugin, id);
  if (!stream) {
    out = errorJson("not_found", "没有这条连接");
    return;
  }
  if (stream->kind() != "tcp") {
    out = errorJson("wrong_kind", "这条连接不是 socket 连接 (WS 连接用 ws.send)");
    return;
  }
  if (auto tcp = std::dynamic_pointer_cast<TcpStream>(stream)) {
    if (tcp->connecting) {
      out = errorJson("not_connected", "连接还在建立中 (等 socket.connected 再发)");
      return;
    }
  }
  std::string payload;
  const std::string base64 = args.value("dataBase64", std::string{});
  if (!base64.empty()) {
    const auto decoded = utilxx_base::base64Decode(base64);
    if (!decoded.has_value()) {
      out = errorJson("bad_base64", "dataBase64 解不开");
      return;
    }
    payload = *decoded;
  } else {
    payload = args.value("data", std::string{});
    if (payload.empty()) {
      payload = args.value("text", std::string{});
    }
  }
  if (payload.empty()) {
    out = errorJson("bad_args", "没有要发送的内容 (data / dataBase64)");
    return;
  }
  std::string err;
  stream->send(std::move(payload), err);
  if (!err.empty()) {
    out = errorJson(err, "发送失败");
    return;
  }
  out = Json::object();
  out["ok"] = true;
  out["id"] = id;
}

std::string PluginNetHub::Impl::infoJsonOnThread(const std::string &plugin,
                                                 bool allPlugins) {
  Json out = Json::object();
  out["ok"] = true;
  out["running"] = running.load(std::memory_order_acquire);
  // 客户端 TLS: 这个构建能不能连 https / wss; 服务端仍然只有 http / ws
  out["tls"] = NetStream::tlsSupported();
  const std::string tlsVersion = tlsRuntimeVersion();
  if (!tlsVersion.empty()) {
    out["tlsProvider"] = tlsVersion;
  }
  out["schemes"] = [&] {
    Json schemes = Json::array();
    schemes.push_back("http");
    if (NetStream::tlsSupported()) {
      schemes.push_back("https");
    }
    schemes.push_back("ws");
    if (NetStream::tlsSupported()) {
      schemes.push_back("wss");
    }
    return schemes;
  }();
  Json serversJson = Json::array();
  for (const auto &item : servers) {
    const auto &server = item.second;
    if (!server || (!allPlugins && server->plugin != plugin)) {
      continue;
    }
    Json row = Json::object();
    row["plugin"] = server->plugin;
    row["kind"] = server->kind;
    row["id"] = server->id;
    row["host"] = server->host;
    row["port"] = static_cast<int64_t>(server->port);
    if (!server->url.empty()) {
      row["url"] = server->url;
    }
    if (!server->pathPrefix.empty()) {
      row["path"] = server->pathPrefix;
    }
    row["connections"] = static_cast<int64_t>(server->conns.size());
    row["pending"] = static_cast<int64_t>(server->pending);
    serversJson.push_back(std::move(row));
  }
  out["servers"] = std::move(serversJson);
  Json connsJson = Json::array();
  for (const auto &item : conns) {
    const auto &stream = item.second;
    if (!stream || (!allPlugins && stream->plugin != plugin)) {
      continue;
    }
    Json row = Json::object();
    row["plugin"] = stream->plugin;
    row["id"] = stream->id;
    row["kind"] = stream->kind();
    if (!stream->serverId.empty()) {
      row["serverId"] = stream->serverId;
    }
    row["remote"] = stream->remote;
    row["local"] = stream->local;
    row["secure"] = stream->io && stream->io->usingTls();
    row["queuedBytes"] = static_cast<int64_t>(stream->outBytes);
    connsJson.push_back(std::move(row));
  }
  out["connections"] = std::move(connsJson);
  size_t httpClients = 0;
  for (const auto &item : httpWaits) {
    if (allPlugins || item.second.plugin == plugin) {
      ++httpClients;
    }
  }
  out["httpClients"] = static_cast<int64_t>(httpClients);
  Json limits = Json::object();
  limits["maxServersPerKind"] = kMaxServersPerKind;
  limits["maxConnectionsPerPlugin"] =
      static_cast<int64_t>(kMaxConnectionsPerPlugin);
  limits["maxHttpClientsPerPlugin"] =
      static_cast<int64_t>(kMaxHttpClientsPerPlugin);
  limits["maxHttpPendingPerServer"] =
      static_cast<int64_t>(kMaxHttpPendingPerServer);
  limits["maxHttpBodyBytes"] = kMaxHttpBody;
  limits["maxWsMessageBytes"] = kMaxWsMessage;
  limits["maxOutQueueBytes"] = static_cast<int64_t>(kMaxOutQueue);
  out["limits"] = std::move(limits);
  return out.dump();
}

int32_t PluginNetHub::Impl::handleCall(const std::string &plugin,
                                       const std::string &op, const Json &args,
                                       Json &out) {
  if (!args.is_object()) {
    out = errorJson("bad_args", "参数必须是对象");
    return MUSICXX_EXTERN_PLUGIN_OK;
  }

  /* ---------- 通用 ---------- */
  if (op == "net.info") {
    out = parseJsonSafe(infoJsonOnThread(plugin, false));
    return MUSICXX_EXTERN_PLUGIN_OK;
  }

  /* ---------- HTTP 客户端 (同步形态: 只受理, 结果经事件) ---------- */
  if (op == "http.request" || op == "http.fetch" || op == "http.download") {
    const int32_t mode =
        (op == "http.fetch") ? 1 : ((op == "http.download") ? 2 : 0);
    const int64_t requestId = allocateRequestId();
    if (!startHttpRequest(plugin, requestId, args, DoneFn{}, out, mode)) {
      return MUSICXX_EXTERN_PLUGIN_OK; ///< out 里已经是错误对象
    }
    return MUSICXX_EXTERN_PLUGIN_OK;
  }
  if (op == "http.cancel") {
    cancelHttpRequest(plugin, args.value("requestId", int64_t{0}), out);
    return MUSICXX_EXTERN_PLUGIN_OK;
  }

  /* ---------- HTTP 服务端 ---------- */
  if (op == "http.serve") {
    std::string id = args.value("id", std::string{});
    if (!id.empty() && !isValidId(id)) {
      out = errorJson("bad_id", "id 只能包含字母数字与 . _ -");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    if (id.empty()) {
      id = nextServerId(plugin, "http");
    }
    if (findServer(plugin, "http", id)) {
      out = errorJson("exists", "同名的 HTTP 服务已经存在");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    auto server = std::make_shared<Server>();
    server->plugin = plugin;
    server->kind = "http";
    server->id = id;
    server->host = args.value("host", std::string{"127.0.0.1"});
    const int64_t port = args.value("port", int64_t{0});
    if (port < 0 || port > 65535) {
      out = errorJson("bad_port", "端口超出范围");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    server->port = static_cast<uint16_t>(port);
    server->pathPrefix = args.value("path", std::string{});
    server->maxBody = args.value("maxBodyBytes", int64_t{kMaxHttpBody});
    server->requestTimeoutMs = static_cast<int32_t>(
        args.value("requestTimeoutMs", int64_t{30000}));
    server->maxConnections = static_cast<int32_t>(
        args.value("maxConnections", int64_t{32}));
    if (server->maxConnections <= 0) {
      server->maxConnections = 32;
    }
    std::string err;
    if (!listenServer(server, err)) {
      out = errorJson("listen_failed", err);
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    out = Json::object();
    out["ok"] = true;
    out["id"] = server->id;
    out["host"] = server->host;
    out["port"] = static_cast<int64_t>(server->port);
    out["url"] = server->url;
    Json event;
    event["kind"] = "http.listening";
    event["id"] = server->id;
    event["host"] = server->host;
    event["port"] = static_cast<int64_t>(server->port);
    event["url"] = server->url;
    emit(plugin, std::move(event));
    return MUSICXX_EXTERN_PLUGIN_OK;
  }
  if (op == "http.respond") {
    const int64_t requestId = args.value("requestId", int64_t{0});
    std::string owner;
    const auto stream = takePendingHttp(requestId, &owner);
    if (!stream || owner != plugin) {
      out = errorJson("not_found", "没有这条等待回答的请求");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    const auto http = std::dynamic_pointer_cast<HttpServerStream>(stream);
    if (!http) {
      out = errorJson("wrong_kind", "这条请求不属于 HTTP 服务");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    http->writeResponseJson(args);
    out = Json::object();
    out["ok"] = true;
    out["requestId"] = requestId;
    return MUSICXX_EXTERN_PLUGIN_OK;
  }
  if (op == "http.stop") {
    stopServers(plugin, "http", args.value("id", std::string{}), out);
    return MUSICXX_EXTERN_PLUGIN_OK;
  }
  if (op == "http.info") {
    out = parseJsonSafe(infoJsonOnThread(plugin, false));
    return MUSICXX_EXTERN_PLUGIN_OK;
  }

  /* ---------- TCP 客户端 ---------- */
  if (op == "socket.connect") {
    const std::string host = args.value("host", std::string{});
    const int64_t port = args.value("port", int64_t{0});
    if (host.empty() || port <= 0 || port > 65535) {
      out = errorJson("bad_args", "host / port 不合法");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    std::string id = args.value("id", std::string{});
    if (!id.empty() && !isValidId(id)) {
      out = errorJson("bad_id", "id 只能包含字母数字与 . _ -");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    if (id.empty()) {
      id = nextConnId(plugin, "c");
    }
    if (findConn(plugin, id)) {
      out = errorJson("exists", "同名的连接已经存在");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    auto stream = std::make_shared<TcpStream>(shared_from_this(),
                                              asio::ip::tcp::socket(io));
    stream->plugin = plugin;
    stream->id = id;
    if (!registerStream(stream)) {
      out = errorJson("too_many", "连接数已达上限");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    out = Json::object();
    out["ok"] = true;
    out["id"] = id;
    out["state"] = "connecting";
    const int32_t timeoutMs =
        static_cast<int32_t>(args.value("timeoutMs", int64_t{8000}));
    stream->connectTo(host, static_cast<uint16_t>(port), timeoutMs);
    return MUSICXX_EXTERN_PLUGIN_OK;
  }

  /* ---------- TCP 服务 ---------- */
  if (op == "socket.listen") {
    std::string id = args.value("id", std::string{});
    if (!id.empty() && !isValidId(id)) {
      out = errorJson("bad_id", "id 只能包含字母数字与 . _ -");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    if (id.empty()) {
      id = nextServerId(plugin, "tcp");
    }
    if (findServer(plugin, "tcp", id)) {
      out = errorJson("exists", "同名的服务已经存在");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    auto server = std::make_shared<Server>();
    server->plugin = plugin;
    server->kind = "tcp";
    server->id = id;
    server->host = args.value("host", std::string{"127.0.0.1"});
    const int64_t port = args.value("port", int64_t{0});
    if (port < 0 || port > 65535) {
      out = errorJson("bad_port", "端口超出范围");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    server->port = static_cast<uint16_t>(port);
    server->maxConnections = static_cast<int32_t>(
        args.value("maxConnections", int64_t{32}));
    if (server->maxConnections <= 0) {
      server->maxConnections = 32;
    }
    std::string err;
    if (!listenServer(server, err)) {
      out = errorJson("listen_failed", err);
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    out = Json::object();
    out["ok"] = true;
    out["id"] = server->id;
    out["host"] = server->host;
    out["port"] = static_cast<int64_t>(server->port);
    Json event;
    event["kind"] = "socket.listening";
    event["id"] = server->id;
    event["host"] = server->host;
    event["port"] = static_cast<int64_t>(server->port);
    emit(plugin, std::move(event));
    return MUSICXX_EXTERN_PLUGIN_OK;
  }

  /* ---------- 连接收发 (TCP 服务端连接同一套) ---------- */
  if (op == "socket.send") {
    sendConn(plugin, args, out);
    return MUSICXX_EXTERN_PLUGIN_OK;
  }
  if (op == "socket.close") {
    closeConn(plugin, args.value("id", std::string{}), out);
    return MUSICXX_EXTERN_PLUGIN_OK;
  }
  if (op == "socket.serverStop") {
    stopServers(plugin, "tcp", args.value("id", std::string{}), out);
    return MUSICXX_EXTERN_PLUGIN_OK;
  }
  if (op == "socket.info") {
    out = parseJsonSafe(infoJsonOnThread(plugin, false));
    return MUSICXX_EXTERN_PLUGIN_OK;
  }

  /* ---------- WebSocket 客户端 ---------- */
  if (op == "ws.connect") {
    std::string url = args.value("url", std::string{});
    if (url.empty()) {
      // 也接受 host / port / path 的拆开写法
      const std::string host = args.value("host", std::string{});
      const int64_t port = args.value("port", int64_t{0});
      if (!host.empty()) {
        std::string path = args.value("path", std::string{"/"});
        if (path.empty() || path.front() != '/') {
          path = "/" + path;
        }
        url = "ws://" + host + (port > 0 ? ":" + std::to_string(port) : "") + path;
      }
    }
    if (url.empty()) {
      out = errorJson("bad_args", "缺少 url");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    const ParsedUrl target = parseUrl(url);
    if (!target.ok) {
      out = errorJson("bad_url", target.error);
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    if (target.scheme != "ws" && target.scheme != "wss") {
      out = errorJson("unsupported_scheme", "WebSocket 地址只支持 ws:// 或 wss://");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    if (target.scheme == "wss" && !NetStream::tlsSupported()) {
      out = errorJson("tls_unsupported",
                      "这个插件框架构建没有链接 TLS 库 (wss 不可用)");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    std::string id = args.value("id", std::string{});
    if (!id.empty() && !isValidId(id)) {
      out = errorJson("bad_id", "id 只能包含字母数字与 . _ -");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    if (id.empty()) {
      id = nextConnId(plugin, "w");
    }
    if (findConn(plugin, id)) {
      out = errorJson("exists", "同名的连接已经存在");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    auto ws = std::make_shared<WsStream>(shared_from_this(),
                                         asio::ip::tcp::socket(io));
    ws->plugin = plugin;
    ws->id = id;
    ws->roleClient = true;
    ws->url = url;
    ws->connectTimeoutMs =
        static_cast<int32_t>(args.value("timeoutMs", int64_t{8000}));
    ws->maxMessage = (std::max)(
        int64_t{1024},
        (std::min)(args.value("maxMessageBytes", int64_t{kMaxWsMessage}),
                   int64_t{16 * 1024 * 1024}));
    parseTlsOptions(args, ws->tlsOptions);
    if (args["headers"].is_object()) {
      ws->extraHeaders = args["headers"];
    }
    if (args["subprotocols"].is_array()) {
      for (const auto &item : args["subprotocols"]) {
        if (item.is_string()) {
          ws->subprotocols.push_back(item.get<std::string>());
        }
      }
    }
    if (!registerStream(ws)) {
      out = errorJson("too_many", "连接数已达上限");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    out = Json::object();
    out["ok"] = true;
    out["id"] = id;
    out["state"] = "connecting";
    ws->startClient();
    return MUSICXX_EXTERN_PLUGIN_OK;
  }

  /* ---------- WebSocket 服务 ---------- */
  if (op == "ws.listen") {
    std::string id = args.value("id", std::string{});
    if (!id.empty() && !isValidId(id)) {
      out = errorJson("bad_id", "id 只能包含字母数字与 . _ -");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    if (id.empty()) {
      id = nextServerId(plugin, "ws");
    }
    if (findServer(plugin, "ws", id)) {
      out = errorJson("exists", "同名的服务已经存在");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    auto server = std::make_shared<Server>();
    server->plugin = plugin;
    server->kind = "ws";
    server->id = id;
    server->host = args.value("host", std::string{"127.0.0.1"});
    const int64_t port = args.value("port", int64_t{0});
    if (port < 0 || port > 65535) {
      out = errorJson("bad_port", "端口超出范围");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    server->port = static_cast<uint16_t>(port);
    server->pathPrefix = args.value("path", std::string{});
    server->maxConnections = static_cast<int32_t>(
        args.value("maxConnections", int64_t{32}));
    if (server->maxConnections <= 0) {
      server->maxConnections = 32;
    }
    std::string err;
    if (!listenServer(server, err)) {
      out = errorJson("listen_failed", err);
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    out = Json::object();
    out["ok"] = true;
    out["id"] = server->id;
    out["host"] = server->host;
    out["port"] = static_cast<int64_t>(server->port);
    out["url"] = server->url;
    Json event;
    event["kind"] = "ws.listening";
    event["id"] = server->id;
    event["host"] = server->host;
    event["port"] = static_cast<int64_t>(server->port);
    event["url"] = server->url;
    emit(plugin, std::move(event));
    return MUSICXX_EXTERN_PLUGIN_OK;
  }
  if (op == "ws.send") {
    const std::string id = args.value("id", std::string{});
    const auto stream = findConn(plugin, id);
    if (!stream) {
      out = errorJson("not_found", "没有这条连接");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    const auto ws = std::dynamic_pointer_cast<WsStream>(stream);
    if (!ws) {
      out = errorJson("wrong_kind", "这条连接不是 WebSocket 连接");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    int32_t opcode = 1;
    std::string payload;
    const std::string base64 = args.value("dataBase64", std::string{});
    if (!base64.empty()) {
      const auto decoded = utilxx_base::base64Decode(base64);
      if (!decoded.has_value()) {
        out = errorJson("bad_base64", "dataBase64 解不开");
        return MUSICXX_EXTERN_PLUGIN_OK;
      }
      payload = *decoded;
      opcode = 2;
    } else {
      payload = args.value("text", std::string{});
      if (payload.empty()) {
        payload = args.value("data", std::string{});
      }
      if (payload.empty()) {
        out = errorJson("bad_args", "没有要发送的内容 (text / data / dataBase64)");
        return MUSICXX_EXTERN_PLUGIN_OK;
      }
    }
    std::string err;
    ws->sendFrame(opcode, payload, err);
    if (!err.empty()) {
      out = errorJson(err, "发送失败 (连接可能还没握手完成)");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    out = Json::object();
    out["ok"] = true;
    out["id"] = id;
    return MUSICXX_EXTERN_PLUGIN_OK;
  }
  if (op == "ws.ping") {
    const std::string id = args.value("id", std::string{});
    const auto stream = findConn(plugin, id);
    const auto ws = std::dynamic_pointer_cast<WsStream>(stream);
    if (!ws) {
      out = errorJson("not_found", "没有这条连接");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    std::string err;
    ws->sendFrame(0x9, args.value("text", std::string{}), err);
    out = err.empty() ? Json::object() : errorJson(err, "发送失败");
    out["ok"] = err.empty();
    return MUSICXX_EXTERN_PLUGIN_OK;
  }
  if (op == "ws.close") {
    const std::string id = args.value("id", std::string{});
    const auto stream = findConn(plugin, id);
    const auto ws = std::dynamic_pointer_cast<WsStream>(stream);
    if (!ws) {
      out = errorJson("not_found", "没有这条 WebSocket 连接");
      return MUSICXX_EXTERN_PLUGIN_OK;
    }
    const int32_t code =
        static_cast<int32_t>(args.value("code", int64_t{1000}));
    ws->closeByPlugin(code <= 0 ? 1000 : code,
                      args.value("reason", std::string{}));
    out = Json::object();
    out["ok"] = true;
    out["id"] = id;
    return MUSICXX_EXTERN_PLUGIN_OK;
  }
  if (op == "ws.serverStop") {
    stopServers(plugin, "ws", args.value("id", std::string{}), out);
    return MUSICXX_EXTERN_PLUGIN_OK;
  }
  if (op == "ws.info") {
    out = parseJsonSafe(infoJsonOnThread(plugin, false));
    return MUSICXX_EXTERN_PLUGIN_OK;
  }

  return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
}

void PluginNetHub::Impl::startThread() {
  if (running.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  io.restart();
  guard = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
      asio::make_work_guard(io));
  auto self = shared_from_this();
  thread = std::thread([self] {
    try {
      self->io.run();
    } catch (const std::exception &e) {
      XX_LOGE("[musicxx_ext] 插件网络线程退出: {}", e.what());
    }
  });
  XX_LOGI("[musicxx_ext] 插件网络线程已启动");
}

void PluginNetHub::Impl::stopThread() {
  if (!running.exchange(false, std::memory_order_acq_rel)) {
    if (thread.joinable()) {
      thread.join();
    }
    return;
  }
  auto slot = std::make_shared<SyncSlot>();
  auto self = shared_from_this();
  asio::post(io, [self, slot] {
    self->closeAllOf({}, true, nullptr);
    slot->set();
  });
  slot->wait(5000);
  guard.reset();
  io.stop();
  if (thread.joinable()) {
    thread.join();
  }
  XX_LOGI("[musicxx_ext] 插件网络线程已停止");
}

/* ==================== 对外入口 ==================== */

PluginNetHub::PluginNetHub(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}

PluginNetHub::~PluginNetHub() { stop(); }

std::shared_ptr<PluginNetHub> PluginNetHub::create(PublishFn publish,
                                                   PostFn postToFramework) {
  auto impl = std::make_shared<Impl>();
  impl->publish = std::move(publish);
  impl->postToFramework = std::move(postToFramework);
  return std::shared_ptr<PluginNetHub>(new PluginNetHub(std::move(impl)));
}

void PluginNetHub::start() { impl_->startThread(); }

void PluginNetHub::setDataDir(const std::string &dir) {
  // 在 start() 之前设好 (主程序在 host 配置里给)
  impl_->dataDir = dir;
}

void PluginNetHub::stop() { impl_->stopThread(); }

bool PluginNetHub::running() const {
  return impl_->running.load(std::memory_order_acquire);
}

int32_t PluginNetHub::call(const std::string &pluginId, const std::string &op,
                           const std::string &argsJson, std::string &outJson) {
  if (!running()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  }
  bool parsed = false;
  Json args = parseJsonSafe(argsJson, &parsed);
  if (!parsed) {
    return MUSICXX_EXTERN_PLUGIN_ERR_JSON;
  }
  auto result = std::make_shared<Json>(Json::object());
  auto code = std::make_shared<int32_t>(MUSICXX_EXTERN_PLUGIN_ERR_INTERNAL);
  const bool waited = impl_->runSync([impl = impl_, pluginId, op, args, result,
                                      code] {
    *code = impl->handleCall(pluginId, op, args, *result);
  });
  if (!waited) {
    return MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT;
  }
  if (*code != MUSICXX_EXTERN_PLUGIN_OK) {
    return *code;
  }
  outJson = result->dump();
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t PluginNetHub::callAsync(const std::string &pluginId,
                                const std::string &op,
                                const std::string &argsJson, DoneFn done,
                                std::string &outJson, int64_t *outRequestId) {
  if (!running()) {
    outJson = errorJson("not_running", "插件网络线程没有运行").dump();
    return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  }
  if (op != "http.request" && op != "http.fetch" && op != "http.download") {
    outJson = errorJson("unsupported_op", "这个操作没有异步形态").dump();
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  const int32_t mode =
      (op == "http.fetch") ? 1 : ((op == "http.download") ? 2 : 0);
  bool parsed = false;
  Json args = parseJsonSafe(argsJson, &parsed);
  if (!parsed) {
    outJson = errorJson("bad_json", "参数不是合法 JSON").dump();
    return MUSICXX_EXTERN_PLUGIN_ERR_JSON;
  }
  const int64_t requestId = impl_->allocateRequestId();
  auto result = std::make_shared<Json>(Json::object());
  auto started = std::make_shared<bool>(false);
  const bool waited = impl_->runSync(
      [impl = impl_, pluginId, requestId, args, done = std::move(done), result,
       started, mode]() mutable {
        *started = impl->startHttpRequest(pluginId, requestId, args,
                                          std::move(done), *result, mode);
      });
  if (!waited) {
    outJson = errorJson("timeout", "网络线程没有及时受理").dump();
    return MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT;
  }
  outJson = result->dump();
  if (!*started) {
    return MUSICXX_EXTERN_PLUGIN_ERR_ARG;
  }
  if (outRequestId) {
    *outRequestId = requestId;
  }
  return MUSICXX_EXTERN_PLUGIN_OK;
}

int32_t PluginNetHub::cancel(const std::string &pluginId, int64_t requestId,
                             std::string &outJson) {
  if (!running()) {
    return MUSICXX_EXTERN_PLUGIN_ERR_STATE;
  }
  auto result = std::make_shared<Json>(Json::object());
  const bool waited = impl_->runSync(
      [impl = impl_, pluginId, requestId, result] {
        impl->cancelHttpRequest(pluginId, requestId, *result);
      });
  if (!waited) {
    return MUSICXX_EXTERN_PLUGIN_ERR_TIMEOUT;
  }
  outJson = result->dump();
  return MUSICXX_EXTERN_PLUGIN_OK;
}

std::vector<PluginNetHub::DoneFn>
PluginNetHub::shutdownPlugin(const std::string &pluginId) {
  auto cancelled = std::make_shared<std::vector<DoneFn>>();
  impl_->runSync(
      [impl = impl_, pluginId, cancelled] {
        impl->closeAllOf(pluginId, false, cancelled.get());
      },
      5000);
  return std::move(*cancelled);
}

std::string PluginNetHub::infoJson(const std::string &pluginId) {
  if (!running()) {
    return errorJson("not_running", "插件网络线程没有运行").dump();
  }
  auto out = std::make_shared<std::string>(
      errorJson("timeout", "网络线程没有及时回答").dump());
  impl_->runSync([impl = impl_, pluginId, out] {
    *out = impl->infoJsonOnThread(pluginId, pluginId.empty());
  }, 5000);
  return *out;
}

} // namespace extern_plugin
} // namespace musicxx


