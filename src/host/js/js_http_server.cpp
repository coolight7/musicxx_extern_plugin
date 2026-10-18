/// musicxx 外部插件原生插件框架: 插件本地 HTTP 文件服务实现
///
/// 设计与安全边界见 js_http_server.h。这里只做三件事:
/// 1. 登记表 (token → 文件或内存内容) 与惰性启动的监听;
/// 2. 极简 HTTP/1.1 响应 (GET / HEAD + 单段 Range), 文件按 64 KiB 边读边写;
/// 3. 停止时关闭监听与连接, 让端口立刻释放。

#include "js_http_server.h"

#include "utilxx_base/asio_error.h"
#include "utilxx_base/json.h"
#include "utilxx_base/log.h"

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read_until.hpp>
#include <asio/streambuf.hpp>
#include <asio/thread_pool.hpp>
#include <asio/write.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <sys/time.h>
#endif

namespace musicxx {
namespace extern_plugin {

namespace fs = std::filesystem;
using utilxx_base::AsioErrorCode;
using utilxx_base::Json;

namespace {

/// 读请求头 / 写响应的单次等待上限 (毫秒)
///
/// 回环传输不会用这么久; 它的作用是让 `stop()` 能在有慢连接时也很快返回
/// (每个阻塞的读写最多等这么久, 期间还会检查停止标志)。
constexpr int kIoTimeoutMs = 1000;
/// 请求头上限 (只解析请求行与 Range, 不需要更大)
constexpr size_t kMaxRequestHead = 16 * 1024;
/// 流式读取文件的分块大小
constexpr size_t kFileChunk = 64 * 1024;

std::string toHex(const unsigned char *data, size_t size) {
  static const char *kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (size_t i = 0; i < size; ++i) {
    out.push_back(kDigits[(data[i] >> 4) & 0x0F]);
    out.push_back(kDigits[data[i] & 0x0F]);
  }
  return out;
}

/// 随机 token (16 字节 → 32 位十六进制)
std::string randomToken() {
  unsigned char bytes[16] = {};
  static std::mutex rngMutex;
  static std::mt19937_64 rng{[] {
    std::random_device rd;
    std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
    std::mt19937_64 engine{seq};
    return engine;
  }()};
  {
    std::lock_guard lock{rngMutex};
    for (size_t i = 0; i < sizeof(bytes); i += 8) {
      const uint64_t value = rng();
      for (size_t b = 0; b < 8 && i + b < sizeof(bytes); ++b) {
        bytes[i + b] = static_cast<unsigned char>((value >> (8 * b)) & 0xFF);
      }
    }
  }
  return toHex(bytes, sizeof(bytes));
}

/// UTF-8 路径 → filesystem 路径 (Windows 上要按 UTF-8 解释, 否则中文路径打不开)
fs::path toFsPath(const std::string &utf8) {
  const std::u8string text{reinterpret_cast<const char8_t *>(utf8.data()),
                           utf8.size()};
  return fs::path{text};
}

/// 修改时间 → 毫秒时间戳 (便携写法: 用两个时钟的当前值换算)
int64_t mtimeMsOf(const fs::path &path) {
  std::error_code ec;
  const auto ftime = fs::last_write_time(path, ec);
  if (ec) {
    return 0;
  }
  const auto sysTime =
      std::chrono::time_point_cast<std::chrono::milliseconds>(
          ftime - fs::file_time_type::clock::now() +
          std::chrono::system_clock::now());
  return static_cast<int64_t>(sysTime.time_since_epoch().count());
}

std::string lowerAscii(std::string text) {
  for (char &c : text) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return text;
}

/// 扩展名 → Content-Type (只覆盖音频/视频/文本这几类常见格式)
std::string contentTypeOfName(const std::string &name) {
  const size_t dot = name.find_last_of('.');
  if (dot == std::string::npos) {
    return {};
  }
  const std::string ext = lowerAscii(name.substr(dot + 1));
  if (ext == "mp3") return "audio/mpeg";
  if (ext == "m4a" || ext == "mp4") return "audio/mp4";
  if (ext == "flac") return "audio/flac";
  if (ext == "wav") return "audio/wav";
  if (ext == "ogg" || ext == "opus") return "audio/ogg";
  if (ext == "aac") return "audio/aac";
  if (ext == "webm") return "video/webm";
  if (ext == "mkv") return "video/x-matroska";
  if (ext == "ts") return "video/mp2t";
  if (ext == "m3u8") return "application/vnd.apple.mpegurl";
  if (ext == "lrc") return "text/plain; charset=utf-8";
  if (ext == "txt" || ext == "json") return "text/plain; charset=utf-8";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "png") return "image/png";
  return {};
}

/// URL 尾段只保留 unreserved 字符, 其余百分号编码 (中文文件名也能用)
std::string urlEncodePathSegment(const std::string &text) {
  static const char *kDigits = "0123456789ABCDEF";
  std::string out;
  out.reserve(text.size());
  for (const unsigned char c : text) {
    const bool unreserved = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                            c == '.' || c == '~';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
      continue;
    }
    out.push_back('%');
    out.push_back(kDigits[(c >> 4) & 0x0F]);
    out.push_back(kDigits[c & 0x0F]);
  }
  return out;
}

std::string fileNameOfPath(const std::string &path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// 文件名只用于 URL 展示: 去掉路径分隔与控制字符, 限长
std::string sanitizeFileName(const std::string &name) {
  std::string out;
  out.reserve(std::min(name.size(),
                       PluginHttpFileServer::maxFileNameLength));
  for (const unsigned char c : name) {
    if (out.size() >= PluginHttpFileServer::maxFileNameLength) {
      break;
    }
    if (c < 0x20 || c == 0x7F || c == '/' || c == '\\') {
      continue;
    }
    out.push_back(static_cast<char>(c));
  }
  return out;
}

/// 当前平台上给 socket 设读写超时 (毫秒)
void setSocketTimeout(asio::ip::tcp::socket &sock, int millis) {
#if defined(_WIN32)
  const DWORD value = static_cast<DWORD>(millis);
  ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char *>(&value), sizeof(value));
  ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char *>(&value), sizeof(value));
#else
  struct timeval tv {};
  tv.tv_sec = millis / 1000;
  tv.tv_usec = (millis % 1000) * 1000;
  ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv,
               sizeof(tv));
  ::setsockopt(sock.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &tv,
               sizeof(tv));
#endif
}

/// 单段 Range (`bytes=start-end` / `bytes=start-` / `bytes=-suffix`)
struct ByteRange {
  bool present = false;       ///< 请求里有没有 Range
  bool satisfiable = true;    ///< 不合法时回 416
  int64_t start = 0;
  int64_t end = 0;            ///< 闭区间
};

int64_t parseNonNegative(const std::string &text) {
  int64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return -1;
    }
    if (value > (INT64_MAX - 9) / 10) {
      return -1;
    }
    value = value * 10 + (c - '0');
  }
  return value;
}

ByteRange parseRange(const std::string &header, int64_t size) {
  ByteRange range;
  if (header.empty()) {
    return range;
  }
  range.present = true;
  std::string text = header;
  // 前缀大小写不敏感
  if (lowerAscii(text.substr(0, 6)) != "bytes=") {
    range.satisfiable = false;
    return range;
  }
  text = text.substr(6);
  // 多段不支持: 当成"没有 Range", 整段返回 (比 416 更宽容, 播放器只用单段)
  if (text.find(',') != std::string::npos) {
    range.present = false;
    return range;
  }
  const size_t dash = text.find('-');
  if (dash == std::string::npos) {
    range.satisfiable = false;
    return range;
  }
  const std::string left = text.substr(0, dash);
  const std::string right = text.substr(dash + 1);
  if (left.empty()) {
    // 后缀形式: 最后 N 字节
    const int64_t suffix = parseNonNegative(right);
    if (suffix <= 0) {
      range.satisfiable = false;
      return range;
    }
    if (size <= 0) {
      range.satisfiable = false;
      return range;
    }
    range.start = (std::max)(static_cast<int64_t>(0), size - suffix);
    range.end = size - 1;
    return range;
  }
  const int64_t start = parseNonNegative(left);
  if (start < 0) {
    range.satisfiable = false;
    return range;
  }
  int64_t end = size - 1;
  if (!right.empty()) {
    const int64_t parsed = parseNonNegative(right);
    if (parsed < 0 || parsed < start) {
      range.satisfiable = false;
      return range;
    }
    end = (std::min)(parsed, size - 1);
  }
  if (size <= 0 || start >= size) {
    range.satisfiable = false;
    return range;
  }
  range.start = start;
  range.end = end;
  return range;
}

}  // namespace

/* ==================== 实现 ==================== */

struct PluginHttpFileServer::Impl {
  /// 一条登记 (文件或内存内容)
  struct Entry {
    std::string token;
    std::string fileName;     ///< URL 尾段 (可空)
    std::string contentType;
    std::string path;         ///< 文件登记时的原始路径 (UTF-8)
    std::string bytes;        ///< 内存登记时的内容
    bool isFile = false;
    int64_t size = 0;
    int64_t mtimeMs = 0;
    std::string dedupeKey;    ///< 重复登记判据
  };

  explicit Impl(std::string label) : ownerLabel(std::move(label)) {}

  std::string ownerLabel;

  /// 启动/停止互斥 (顺序固定: lifecycle → mutex, 不允许反过来)
  std::mutex lifecycle;
  /// 登记表与计数的互斥
  mutable std::mutex mutex;
  std::map<std::string, Entry> entries;
  std::vector<std::string> tokens;  ///< 登记顺序 (淘汰最旧用)
  int64_t requests = 0;
  int64_t bytesServed = 0;
  bool running = false;
  uint16_t port = 0;
  std::string rootUrl;  ///< `http://127.0.0.1:<port>`
  std::string baseUrl;  ///< `http://127.0.0.1:<port>/f/`

  /// 网络部分
  std::unique_ptr<asio::io_context> io;
  std::unique_ptr<asio::ip::tcp::acceptor> acceptor;
  std::unique_ptr<asio::thread_pool> workers;
  std::thread acceptThread;
  std::atomic<bool> stopping{false};
  std::atomic<int> active{0};

  /// 启动监听 (调用方需持有 lifecycle); 失败时 err 说明原因
  bool start(std::string &err);

  /// accept 循环 (io 线程)
  void acceptNext();

  /// 处理一条连接 (连接线程池里跑; 同步读写)
  void handleConnection(asio::ip::tcp::socket sock);

  /// 组装记录 (调用方需持有 mutex)
  bool insert(Entry entry, ServedEntry &out, std::string &err);

  /// 查表 (任意线程)
  bool lookup(const std::string &token, Entry &out) const;
};

bool PluginHttpFileServer::Impl::start(std::string &err) {
  try {
    io = std::make_unique<asio::io_context>();
    acceptor = std::make_unique<asio::ip::tcp::acceptor>(*io);
    const asio::ip::tcp::endpoint endpoint{
        asio::ip::make_address("127.0.0.1"), 0};
    acceptor->open(endpoint.protocol());
    acceptor->set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor->bind(endpoint);
    acceptor->listen(asio::socket_base::max_listen_connections);
    port = acceptor->local_endpoint().port();
    workers = std::make_unique<asio::thread_pool>(4);
    stopping.store(false, std::memory_order_release);
    rootUrl = "http://127.0.0.1:" + std::to_string(port);
    baseUrl = rootUrl + "/f/";
    acceptNext();
    acceptThread = std::thread([this] {
      try {
        io->run();
      } catch (const std::exception &e) {
        XX_LOGW("[musicxx_ext] 插件 HTTP 服务 io 线程异常: {}", e.what());
      }
    });
    running = true;
    XX_LOGI("[musicxx_ext] 插件 HTTP 服务已启动: owner={} url={}", ownerLabel,
            baseUrl);
    return true;
  } catch (const std::exception &e) {
    err = e.what();
    // 起不来就把半成品收掉, 免得留下占着线程的对象
    if (acceptor) {
      AsioErrorCode ignored;
      acceptor->close(ignored);
    }
    if (io) {
      io->stop();
    }
    if (acceptThread.joinable()) {
      acceptThread.join();
    }
    if (workers) {
      workers->stop();
      workers->join();
    }
    acceptor.reset();
    workers.reset();
    io.reset();
    return false;
  }
}

void PluginHttpFileServer::Impl::acceptNext() {
  if (stopping.load(std::memory_order_acquire) || !acceptor || !io) {
    return;
  }
  auto socket = std::make_shared<asio::ip::tcp::socket>(*io);
  acceptor->async_accept(*socket, [this, socket](const AsioErrorCode &ec) {
    if (stopping.load(std::memory_order_acquire)) {
      return;
    }
    if (!ec) {
      if (active.load(std::memory_order_acquire) >=
          static_cast<int>(PluginHttpFileServer::maxConnections)) {
        // 连接数到上限: 直接关掉, 不进队列 (队列无限长没有意义)
        AsioErrorCode ignored;
        socket->close(ignored);
      } else {
        active.fetch_add(1, std::memory_order_acq_rel);
        if (workers) {
          asio::post(*workers,
                     [this, conn = std::move(*socket)]() mutable {
                       handleConnection(std::move(conn));
                       active.fetch_sub(1, std::memory_order_acq_rel);
                     });
        } else {
          active.fetch_sub(1, std::memory_order_acq_rel);
        }
      }
    }
    acceptNext();
  });
}

bool PluginHttpFileServer::Impl::lookup(const std::string &token,
                                        Entry &out) const {
  std::lock_guard lock{mutex};
  const auto it = entries.find(token);
  if (it == entries.end()) {
    return false;
  }
  out = it->second;
  return true;
}

bool PluginHttpFileServer::Impl::insert(Entry entry, ServedEntry &out,
                                        std::string &err) {
  std::lock_guard lock{mutex};
  for (const auto &item : entries) {
    if (item.second.dedupeKey == entry.dedupeKey) {
      out.url = baseUrl + item.second.token +
                (item.second.fileName.empty()
                     ? std::string{}
                     : "/" + urlEncodePathSegment(item.second.fileName));
      out.size = item.second.size;
      out.mtimeMs = item.second.mtimeMs;
      out.reused = true;
      return true;
    }
  }
  if (entries.size() >= PluginHttpFileServer::maxEntries) {
    // 满了: 淘汰最旧的一条 (登记顺序就是 tokens 的顺序)
    if (!tokens.empty()) {
      entries.erase(tokens.front());
      tokens.erase(tokens.begin());
    }
  }
  const std::string token = randomToken();
  entry.token = token;
  const std::string name = sanitizeFileName(entry.fileName);
  entry.fileName = name;
  out.url = baseUrl + token +
            (name.empty() ? std::string{} : "/" + urlEncodePathSegment(name));
  out.size = entry.size;
  out.mtimeMs = entry.mtimeMs;
  out.reused = false;
  tokens.push_back(token);
  entries.emplace(token, std::move(entry));
  return true;
}

void PluginHttpFileServer::Impl::handleConnection(
    asio::ip::tcp::socket sock) {
  AsioErrorCode ec;
  sock.set_option(asio::ip::tcp::no_delay(true), ec);
  setSocketTimeout(sock, kIoTimeoutMs);

  // 1) 读请求头 (上限 16 KiB)
  asio::streambuf buffer{kMaxRequestHead};
  asio::read_until(sock, buffer, "\r\n\r\n", ec);
  if (ec) {
    AsioErrorCode ignored;
    sock.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    sock.close(ignored);
    return;
  }
  const auto begin = asio::buffers_begin(buffer.data());
  const auto end = asio::buffers_end(buffer.data());
  std::string head{begin, end};
  while (head.size() >= 2 &&
         (head.back() == '\n' || head.back() == '\r')) {
    head.pop_back();
  }

  // 2) 解析请求行与 Range
  std::string method;
  std::string target;
  std::string rangeHeader;
  {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= head.size()) {
      const size_t lineEnd = head.find('\n', pos);
      std::string line = head.substr(
          pos, lineEnd == std::string::npos ? std::string::npos : lineEnd - pos);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      lines.push_back(std::move(line));
      if (lineEnd == std::string::npos) {
        break;
      }
      pos = lineEnd + 1;
    }
    if (lines.empty()) {
      return;
    }
    const std::string &requestLine = lines.front();
    const size_t sp1 = requestLine.find(' ');
    const size_t sp2 =
        sp1 == std::string::npos ? std::string::npos : requestLine.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
      return;
    }
    method = requestLine.substr(0, sp1);
    target = requestLine.substr(sp1 + 1, sp2 - sp1 - 1);
    for (size_t i = 1; i < lines.size(); ++i) {
      const size_t colon = lines[i].find(':');
      if (colon == std::string::npos) {
        continue;
      }
      if (lowerAscii(lines[i].substr(0, colon)) == "range") {
        size_t valueStart = colon + 1;
        while (valueStart < lines[i].size() &&
               (lines[i][valueStart] == ' ' || lines[i][valueStart] == '\t')) {
          ++valueStart;
        }
        rangeHeader = lines[i].substr(valueStart);
      }
    }
  }

  const bool isHead = method == "HEAD";
  if (method != "GET" && !isHead) {
    const std::string response =
        "HTTP/1.1 405 Method Not Allowed\r\nAllow: GET, HEAD\r\n"
        "Content-Length: 0\r\nConnection: close\r\n\r\n";
    asio::write(sock, asio::buffer(response), ec);
    AsioErrorCode ignored;
    sock.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    sock.close(ignored);
    return;
  }

  // 3) 路由: /f/<token>[/<名字>]；其它路径只回一行说明
  auto sendSimple = [&sock](int code, const char *reason,
                            const std::string &body) {
    const std::string response =
        "HTTP/1.1 " + std::to_string(code) + " " + reason +
        "\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: " +
        std::to_string(body.size()) +
        "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
    AsioErrorCode writeEc;
    asio::write(sock, asio::buffer(response), writeEc);
    AsioErrorCode ignored;
    sock.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    sock.close(ignored);
  };

  if (target == "/" || target.empty()) {
    sendSimple(200, "OK",
               "musicxx plugin http file server\n" + ownerLabel + "\n");
    return;
  }
  if (target.compare(0, 3, "/f/") != 0) {
    sendSimple(404, "Not Found", "not found\n");
    return;
  }
  std::string rest = target.substr(3);
  const size_t query = rest.find('?');
  if (query != std::string::npos) {
    rest = rest.substr(0, query);
  }
  const size_t slash = rest.find('/');
  const std::string token = slash == std::string::npos ? rest : rest.substr(0, slash);

  Entry entry;
  if (token.empty() || !lookup(token, entry)) {
    sendSimple(404, "Not Found", "token not found\n");
    return;
  }

  // 4) Range → 200 / 206 / 416
  const ByteRange range = parseRange(rangeHeader, entry.size);
  if (range.present && !range.satisfiable) {
    const std::string response =
        "HTTP/1.1 416 Range Not Satisfiable\r\nAccept-Ranges: bytes\r\n"
        "Content-Range: bytes */" +
        std::to_string(entry.size) +
        "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    asio::write(sock, asio::buffer(response), ec);
    AsioErrorCode ignored;
    sock.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    sock.close(ignored);
    return;
  }

  const bool partial = range.present;
  const int64_t bodyStart = partial ? range.start : 0;
  const int64_t bodyEnd = partial ? range.end : entry.size - 1;
  const int64_t bodySize = entry.size <= 0 ? 0 : (bodyEnd - bodyStart + 1);

  std::string response = partial ? "HTTP/1.1 206 Partial Content\r\n"
                                 : "HTTP/1.1 200 OK\r\n";
  response += "Content-Type: " +
              (entry.contentType.empty() ? std::string{"application/octet-stream"}
                                         : entry.contentType) +
              "\r\n";
  response += "Content-Length: " + std::to_string(bodySize) + "\r\n";
  response += "Accept-Ranges: bytes\r\n";
  response += "Cache-Control: no-store\r\n";
  response += "Connection: close\r\n";
  if (partial) {
    response += "Content-Range: bytes " + std::to_string(bodyStart) + "-" +
                std::to_string(bodyEnd) + "/" + std::to_string(entry.size) +
                "\r\n";
  }
  response += "\r\n";
  asio::write(sock, asio::buffer(response), ec);
  if (ec) {
    AsioErrorCode ignored;
    sock.close(ignored);
    return;
  }
  {
    std::lock_guard lock{mutex};
    ++requests;
  }
  if (isHead || bodySize <= 0) {
    AsioErrorCode ignored;
    sock.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    sock.close(ignored);
    return;
  }

  // 5) 写正文: 文件按块读 (不整文件进内存), 内存内容一次写完
  int64_t written = 0;
  bool ok = true;
  if (entry.isFile) {
    std::ifstream in(toFsPath(entry.path), std::ios::binary);
    if (!in) {
      AsioErrorCode ignored;
      sock.close(ignored);
      return;
    }
    in.seekg(static_cast<std::streamoff>(bodyStart), std::ios::beg);
    std::vector<char> chunk(kFileChunk);
    int64_t remaining = bodySize;
    while (remaining > 0 && !stopping.load(std::memory_order_acquire)) {
      const size_t want = static_cast<size_t>(
          (std::min)(static_cast<int64_t>(chunk.size()), remaining));
      in.read(chunk.data(), static_cast<std::streamsize>(want));
      const std::streamsize got = in.gcount();
      if (got <= 0) {
        ok = false;
        break;
      }
      AsioErrorCode writeEc;
      asio::write(sock, asio::buffer(chunk.data(), static_cast<size_t>(got)),
                  writeEc);
      if (writeEc) {
        ok = false;
        break;
      }
      written += got;
      remaining -= got;
    }
  } else {
    const size_t start =
        static_cast<size_t>((std::min)(bodyStart, entry.size));
    const size_t count = static_cast<size_t>(
        (std::max)(static_cast<int64_t>(0), (std::min)(bodySize, entry.size - static_cast<int64_t>(start))));
    AsioErrorCode writeEc;
    asio::write(sock, asio::buffer(entry.bytes.data() + start, count), writeEc);
    if (!writeEc) {
      written = static_cast<int64_t>(count);
    } else {
      ok = false;
    }
  }
  if (written > 0) {
    std::lock_guard lock{mutex};
    bytesServed += written;
  }
  if (!ok) {
    XX_LOGW("[musicxx_ext] 插件 HTTP 服务传输中断: owner={} token={}", ownerLabel,
            entry.token);
  }
  AsioErrorCode ignored;
  sock.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
  sock.close(ignored);
}

/* ==================== 对外接口 ==================== */

PluginHttpFileServer::PluginHttpFileServer(std::string ownerLabel)
    : impl_(std::make_shared<Impl>(std::move(ownerLabel))) {}

PluginHttpFileServer::~PluginHttpFileServer() { stop(); }

std::shared_ptr<PluginHttpFileServer> PluginHttpFileServer::create(
    std::string ownerLabel) {
  return std::shared_ptr<PluginHttpFileServer>(
      new PluginHttpFileServer(std::move(ownerLabel)));
}

bool PluginHttpFileServer::serveFile(const std::string &path,
                                     const std::string &contentType,
                                     const std::string &fileName,
                                     ServedEntry &out, std::string &err) {
  if (path.empty()) {
    err = "路径不能为空";
    return false;
  }
  const fs::path fsPath = toFsPath(path);
  std::error_code ec;
  const auto status = fs::status(fsPath, ec);
  if (ec || !fs::exists(status)) {
    err = "文件不存在";
    return false;
  }
  if (!fs::is_regular_file(status)) {
    err = "不是普通文件";
    return false;
  }
  const int64_t size = static_cast<int64_t>(fs::file_size(fsPath, ec));
  if (ec) {
    err = "读文件大小失败";
    return false;
  }
  const int64_t mtimeMs = mtimeMsOf(fsPath);

  std::lock_guard lifecycleLock{impl_->lifecycle};
  if (!impl_->running) {
    if (!impl_->start(err)) {
      return false;
    }
  }
  Impl::Entry entry;
  entry.fileName = fileName.empty() ? fileNameOfPath(path) : fileName;
  entry.contentType =
      contentType.empty() ? contentTypeOfName(entry.fileName) : contentType;
  entry.path = path;
  entry.isFile = true;
  entry.size = size;
  entry.mtimeMs = mtimeMs;
  entry.dedupeKey = "file|" + path + "|" + std::to_string(size) + "|" +
                    std::to_string(mtimeMs);
  return impl_->insert(std::move(entry), out, err);
}

bool PluginHttpFileServer::serveBytes(std::string bytes,
                                      const std::string &contentType,
                                      const std::string &fileName,
                                      ServedEntry &out, std::string &err) {
  if (static_cast<int64_t>(bytes.size()) > maxInlineBytes) {
    err = "内容超过上限 (8 MiB), 大文件请用 serveFile";
    return false;
  }
  std::lock_guard lifecycleLock{impl_->lifecycle};
  if (!impl_->running) {
    if (!impl_->start(err)) {
      return false;
    }
  }
  // 内存内容的去重判据: 长度 + FNV-1a 哈希 (够用, 不上加密哈希)
  uint64_t hash = 1469598103934665603ull;
  for (const unsigned char c : bytes) {
    hash ^= c;
    hash *= 1099511628211ull;
  }
  Impl::Entry entry;
  entry.fileName = fileName;
  entry.contentType =
      contentType.empty() ? contentTypeOfName(fileName) : contentType;
  entry.isFile = false;
  entry.size = static_cast<int64_t>(bytes.size());
  entry.dedupeKey = "bytes|" + std::to_string(bytes.size()) + "|" +
                    std::to_string(hash);
  entry.bytes = std::move(bytes);
  return impl_->insert(std::move(entry), out, err);
}

std::string PluginHttpFileServer::infoJson() const {
  Json out;
  bool running = false;
  uint16_t port = 0;
  std::string rootUrl;
  size_t files = 0;
  int64_t requests = 0;
  int64_t bytesServed = 0;
  {
    std::lock_guard lock{impl_->mutex};
    running = impl_->running;
    port = impl_->port;
    rootUrl = impl_->rootUrl;
    files = impl_->entries.size();
    requests = impl_->requests;
    bytesServed = impl_->bytesServed;
  }
  out["running"] = running;
  out["url"] = running ? rootUrl : std::string{};
  out["port"] = static_cast<int32_t>(port);
  out["files"] = static_cast<int64_t>(files);
  out["requests"] = requests;
  out["bytesServed"] = bytesServed;
  out["owner"] = impl_->ownerLabel;
  return out.dump();
}

void PluginHttpFileServer::stop() {
  std::lock_guard lifecycleLock{impl_->lifecycle};
  if (!impl_->running &&
      !impl_->acceptor) {
    return;
  }
  impl_->stopping.store(true, std::memory_order_release);
  if (impl_->acceptor) {
    AsioErrorCode ignored;
    impl_->acceptor->close(ignored);
  }
  if (impl_->io) {
    impl_->io->stop();
  }
  if (impl_->acceptThread.joinable()) {
    impl_->acceptThread.join();
  }
  if (impl_->workers) {
    impl_->workers->stop();
    impl_->workers->join();
  }
  impl_->acceptor.reset();
  impl_->workers.reset();
  impl_->io.reset();
  {
    std::lock_guard lock{impl_->mutex};
    const bool wasRunning = impl_->running;
    impl_->running = false;
    impl_->entries.clear();
    impl_->tokens.clear();
    if (wasRunning) {
      XX_LOGI("[musicxx_ext] 插件 HTTP 服务已停止: owner={} requests={} bytes={}",
              impl_->ownerLabel, impl_->requests, impl_->bytesServed);
    }
  }
}

}  // namespace extern_plugin
}  // namespace musicxx
