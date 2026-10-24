/// musicxx 外部插件原生插件框架: 连接读写层 (纯 TCP / 客户端 TLS)
///
/// 为什么单独一层: HTTP 客户端与 WebSocket 客户端都要"先连 TCP, 再决定走不走 TLS",
/// 之后的读写对上层没有区别。把这一层收在一起, 两处客户端就只需要 `async_read_some` /
/// `async_write_some` / `async_write` 这一套, 不必各自处理两套流类型。
///
/// 只做**客户端** TLS: 插件自己监听的服务 (`http.serve` / `ws.listen`) 仍然只有 http / ws。
/// 实现用 OpenSSL (静态链接, 见 src/CMakeLists.txt); 这个构建没有链接 OpenSSL 时
/// (`MUSICXX_EXTERN_PLUGIN_HAS_TLS` 未定义) `asyncStartTls` 直接以 `tls_unsupported` 结束。
#pragma once

#include "utilxx_base/asio_error.h"

#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>

#include <memory>
#include <string>
#include <utility>

#if MUSICXX_EXTERN_PLUGIN_HAS_TLS
#include <asio/ssl.hpp>
#endif

namespace musicxx {
namespace extern_plugin {

using utilxx_base::AsioErrorCode;

/// 一次客户端连接的 TLS 参数 (https / wss)
struct TlsClientOptions {
  bool enable = false;  ///< true = 这条连接走 TLS
  bool verify = true;   ///< 校验证书链与主机名 (默认开; 关掉只用于自签名/调试)
  std::string host;     ///< 地址里的主机名 (校验用; IPv6 字面量不带方括号)
  std::string serverName; ///< SNI / 主机名校验的覆盖值 (空 = 用 host)
  std::string caFile;   ///< 额外信任的 CA 文件 (PEM)
  std::string caPem;    ///< 额外信任的 CA 内容 (PEM)
};

/// 一条连接的读写层
///
/// - 默认是纯 TCP; `asyncStartTls` 成功后读写都走 TLS, 上层代码不用区分;
/// - 只允许在**网络线程**上使用 (与插件的 socket 状态同一线程);
/// - `tlsErrorCode()` / `tlsErrorMessage()` 给出 TLS 失败的原因
///   (`tls_unsupported` / `tls_config` / `tls_cert_invalid` / `tls_handshake_failed`)。
class NetStream : public std::enable_shared_from_this<NetStream> {
public:
  /// 接管一条已经连上的 socket (TCP 连接由调用方建立)
  explicit NetStream(asio::ip::tcp::socket socket)
      : executor_(socket.get_executor()), plain_(std::move(socket)) {
    AsioErrorCode ignored;
    plain_.set_option(asio::ip::tcp::no_delay(true), ignored);
  }

  NetStream(const NetStream &) = delete;
  NetStream &operator=(const NetStream &) = delete;

  using executor_type = asio::any_io_executor;

  /// 这个构建的插件框架是否带 TLS
  static bool tlsSupported();

  executor_type get_executor() const { return executor_; }

  /// 底层 TCP socket (发起连接、读取对端地址用)
  ///
  /// 类型是 socket 的 lowest_layer_type (`basic_socket<tcp, executor>`): TLS 层的
  /// lowest_layer() 返回的也是它, 两边类型一致才能同一个函数返回
  using lowest_layer_type = asio::ip::tcp::socket::lowest_layer_type;

  lowest_layer_type &lowest_layer() {
#if MUSICXX_EXTERN_PLUGIN_HAS_TLS
    if (tls_) {
      return tls_->lowest_layer();
    }
#endif
    return plain_.lowest_layer();
  }

  bool usingTls() const {
#if MUSICXX_EXTERN_PLUGIN_HAS_TLS
    return tls_ != nullptr;
#else
    return false;
#endif
  }

  const std::string &tlsErrorCode() const { return tlsErrorCode_; }
  const std::string &tlsErrorMessage() const { return tlsErrorMessage_; }

  /// 开始 TLS 握手 (TCP 已连上, 还没收发任何应用数据)
  ///
  /// handler 形如 `void(AsioErrorCode ec, std::string code, std::string message)`:
  /// ec 为空表示握手与证书校验都通过; 否则 code/message 给出原因。
  template <class Handler>
  void asyncStartTls(const TlsClientOptions &options, Handler &&handler) {
#if MUSICXX_EXTERN_PLUGIN_HAS_TLS
    if (!prepareTls(options)) {
      auto self = shared_from_this();
      asio::post(executor_,
                 [self, handler = std::forward<Handler>(handler)]() mutable {
                   handler(asio::error::operation_not_supported,
                           self->tlsErrorCode_, self->tlsErrorMessage_);
                 });
      return;
    }
    auto self = shared_from_this();
    tls_->async_handshake(
        asio::ssl::stream_base::client,
        [self, handler = std::forward<Handler>(handler)](
            const AsioErrorCode &ec) mutable {
          if (ec) {
            self->describeHandshakeFailure(ec);
          }
          handler(ec, self->tlsErrorCode_, self->tlsErrorMessage_);
        });
#else
    (void)options;
    auto self = shared_from_this();
    asio::post(executor_,
               [self, handler = std::forward<Handler>(handler)]() mutable {
                 handler(asio::error::operation_not_supported,
                         std::string{"tls_unsupported"},
                         std::string{"这个插件框架构建没有链接 TLS 库 (https / wss 不可用)"});
               });
#endif
  }

  template <class MutableBufferSequence, class Handler>
  void async_read_some(const MutableBufferSequence &buffers, Handler &&handler) {
#if MUSICXX_EXTERN_PLUGIN_HAS_TLS
    if (tls_) {
      tls_->async_read_some(buffers, std::forward<Handler>(handler));
      return;
    }
#endif
    plain_.async_read_some(buffers, std::forward<Handler>(handler));
  }

  template <class ConstBufferSequence, class Handler>
  void async_write_some(const ConstBufferSequence &buffers, Handler &&handler) {
#if MUSICXX_EXTERN_PLUGIN_HAS_TLS
    if (tls_) {
      tls_->async_write_some(buffers, std::forward<Handler>(handler));
      return;
    }
#endif
    plain_.async_write_some(buffers, std::forward<Handler>(handler));
  }

  /// 关掉连接 (TLS 下直接关底层 TCP: 上层总是发完响应再关, 不等 close_notify)
  void close();

  /// 读到的错误算不算"对端正常关闭"
  ///
  /// TLS 下对端不发 close_notify 就走 (很常见) 会得到 `ssl::error::stream_truncated`;
  /// 已经收完数据的情况下这与 TCP 的 EOF 同义, 按正常关闭处理。
  static bool isPeerClosed(const AsioErrorCode &ec);

private:
  /// 建 TLS 层; 失败时填 tlsErrorCode_/tlsErrorMessage_ 并返回 false
  bool prepareTls(const TlsClientOptions &options);
  void describeHandshakeFailure(const AsioErrorCode &ec);

  asio::any_io_executor executor_;
  asio::ip::tcp::socket plain_;
#if MUSICXX_EXTERN_PLUGIN_HAS_TLS
  std::unique_ptr<asio::ssl::stream<asio::ip::tcp::socket>> tls_;
#endif
  std::string tlsErrorCode_;
  std::string tlsErrorMessage_;
};

/// 运行期 OpenSSL 版本 (info / 自省用); 不带 TLS 的构建返回空串
std::string tlsRuntimeVersion();

} // namespace extern_plugin
} // namespace musicxx
