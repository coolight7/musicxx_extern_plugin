/// musicxx 外部插件原生插件框架: 连接读写层的 TLS 实现 (客户端)
///
/// 设计与边界见 host_net.h / host_net_tls.h。要点:
/// - 信任库: 先按 OpenSSL 的默认路径 (`SSL_CERT_FILE` / 编译期 openssldir) 加载, 再补上
///   平台的根证书 (Windows: 系统 ROOT 证书库; Linux/macOS/Android: 常见 bundle 文件与
///   证书目录)。插件给的 `caFile` / `caPem` 是**追加**信任, 不会换掉系统根证书;
/// - 证书校验: 每个连接单独设置 SNI 与主机名校验 (`SSL_set1_host`; IP 字面量走
///   `SSL_set1_ip_asc`), 所以同一个 SSL_CTX 可以被不同主机复用;
/// - 上下文按 (是否校验 + 额外 CA) 缓存, 单条连接不需要重复付 SSL_CTX_new 的开销;
/// - 协议版本与安全等级的取值与 agentxx 一致: 通用 client 方法 + `SSL_CTX_set_security_level(1)`,
///   对 TLS 1.0 ~ 1.3 自动协商 (只支持旧版本的服务也能连)。
///
/// 这个文件**无条件**编译 OpenSSL 实现: TLS 是插件框架的必备能力, 不存在"某个构建里没有
/// https / wss"的版本。缺依赖时在 CMake 配置阶段就报错, 而不是编一个功能残缺的库。
#include "host_net_tls.h"

#include "utilxx_base/log.h"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

#include <asio/buffer.hpp>
#include <asio/ip/address.hpp>

#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

namespace musicxx {
namespace extern_plugin {

namespace {

/// 平台根证书: Windows 用系统 ROOT 证书库 (证书库随系统更新, 不需要随包带 bundle)
#ifdef _WIN32
int loadSystemRoots(X509_STORE *store) {
  if (!store) {
    return 0;
  }
  HCERTSTORE raw = ::CertOpenSystemStoreW(0, L"ROOT");
  if (!raw) {
    XX_LOGW("[musicxx_ext] 打不开 Windows 根证书库 (错误 {})", ::GetLastError());
    return 0;
  }
  int added = 0;
  PCCERT_CONTEXT context = nullptr;
  while ((context = ::CertEnumCertificatesInStore(raw, context)) != nullptr) {
    const unsigned char *der = context->pbCertEncoded;
    X509 *certificate =
        ::d2i_X509(nullptr, &der, static_cast<long>(context->cbCertEncoded));
    if (!certificate) {
      continue;
    }
    // 重复添加会返回 0, 属于正常情况 (同一个根证书在库里出现多次)
    if (::X509_STORE_add_cert(store, certificate) == 1) {
      ++added;
    }
    ::X509_free(certificate);
  }
  ::CertCloseStore(raw, 0);
  return added;
}
#else
int loadSystemRoots(X509_STORE *store) {
  if (!store) {
    return 0;
  }
  // 各发行版/系统的根证书位置 (文件与目录两种形式; 找不到就跳过)
  static const char *kBundleFiles[] = {
      "/etc/ssl/cert.pem",
      "/etc/ssl/certs/ca-certificates.crt",
      "/etc/pki/tls/certs/ca-bundle.crt",
      "/etc/ssl/ca-bundle.pem",
  };
  static const char *kBundleDirs[] = {
      "/etc/ssl/certs",
      // Android: 系统根证书目录 (hash 命名的 PEM)
      "/system/etc/security/cacerts",
  };
  int added = 0;
  for (const char *file : kBundleFiles) {
    if (::access(file, R_OK) != 0) {
      continue;
    }
    if (::X509_STORE_load_locations(store, file, nullptr) == 1) {
      ++added;
    }
  }
  for (const char *dir : kBundleDirs) {
    if (::access(dir, R_OK) != 0) {
      continue;
    }
    if (::X509_STORE_load_locations(store, nullptr, dir) == 1) {
      ++added;
    }
  }
  return added;
}
#endif

/// 把 PEM 文本里的证书逐张加进信任库
bool addPemToStore(X509_STORE *store, const std::string &pem,
                   std::string &error) {
  if (!store || pem.empty()) {
    return true;
  }
  BIO *bio = ::BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
  if (!bio) {
    error = "CA 内容读不出来";
    return false;
  }
  int added = 0;
  for (;;) {
    X509 *certificate = ::PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    if (!certificate) {
      break;
    }
    if (::X509_STORE_add_cert(store, certificate) == 1) {
      ++added;
    }
    ::X509_free(certificate);
  }
  ::BIO_free(bio);
  ::ERR_clear_error();
  if (added == 0) {
    error = "CA 内容里没有可用的 PEM 证书";
    return false;
  }
  return true;
}

/// TLS 上下文缓存 (网络线程单线程访问, 但仍加锁: 允许将来在别的线程查询)
std::shared_ptr<asio::ssl::context>
tlsContextFor(const TlsClientOptions &options, std::string &error) {
  static std::mutex mutex;
  static std::map<std::string, std::shared_ptr<asio::ssl::context>> cache;

  std::string key = options.verify ? "verify=1;" : "verify=0;";
  key += options.caFile;
  key += ';';
  key += std::to_string(std::hash<std::string>{}(options.caPem));
  {
    std::lock_guard<std::mutex> lock(mutex);
    const auto it = cache.find(key);
    if (it != cache.end()) {
      return it->second;
    }
  }

  auto context =
      std::make_shared<asio::ssl::context>(asio::ssl::context::tls_client);
  AsioErrorCode ec;
  context->set_options(asio::ssl::context::default_workarounds |
                           asio::ssl::context::no_sslv2 |
                           asio::ssl::context::no_sslv3,
                       ec);
  SSL_CTX *raw = context->native_handle();
  // 与 agentxx 一致: 下限 TLS 1.0, 上限交给 OpenSSL 支持的最高版本
  ::SSL_CTX_set_min_proto_version(raw, TLS1_VERSION);
  ::SSL_CTX_set_max_proto_version(raw, 0);
  ::SSL_CTX_set_security_level(raw, 1);
  context->set_verify_mode(options.verify ? asio::ssl::verify_peer
                                           : asio::ssl::verify_none);
  if (options.verify) {
    // 默认路径 (SSL_CERT_FILE / SSL_CERT_DIR / 编译期 openssldir) 可能一个都没有:
    // 失败不算错, 真正的信任来自下面的平台根证书与插件给的 CA
    context->set_default_verify_paths(ec);
    ::ERR_clear_error();
    X509_STORE *store = ::SSL_CTX_get_cert_store(raw);
    if (loadSystemRoots(store) == 0) {
      XX_LOGW("[musicxx_ext] 没有加载到平台根证书, https 站点很可能校验失败");
    }
    if (!options.caFile.empty() &&
        ::SSL_CTX_load_verify_locations(raw, options.caFile.c_str(), nullptr) !=
            1) {
      error = "CA 文件读不出来: " + options.caFile;
      return nullptr;
    }
    if (!addPemToStore(store, options.caPem, error)) {
      return nullptr;
    }
  }

  {
    std::lock_guard<std::mutex> lock(mutex);
    // 缓存最多留 8 份 (插件换 CA 的场景有限, 避免无界增长)
    if (cache.size() >= 8) {
      cache.clear();
    }
    cache[key] = context;
  }
  return context;
}

} // namespace

bool NetStream::tlsSupported() { return true; }

bool NetStream::prepareTls(const TlsClientOptions &options) {
  tlsErrorCode_.clear();
  tlsErrorMessage_.clear();
  if (!options.enable) {
    return true;
  }
  std::string error;
  auto context = tlsContextFor(options, error);
  if (!context) {
    tlsErrorCode_ = "tls_config";
    tlsErrorMessage_ = error.empty() ? "TLS 配置不可用" : error;
    return false;
  }
  tls_ = std::make_unique<asio::ssl::stream<asio::ip::tcp::socket>>(
      std::move(plain_), *context);
  SSL *ssl = tls_->native_handle();
  const std::string &name =
      options.serverName.empty() ? options.host : options.serverName;
  if (!name.empty()) {
    ::SSL_set_tlsext_host_name(ssl, name.c_str());
    if (options.verify) {
      AsioErrorCode ec;
      const auto address = asio::ip::make_address(name, ec);
      if (!ec) {
        // IP 字面量: 证书里要有对应的 IP SAN (DNS 名校验对 IP 不适用)
        ::X509_VERIFY_PARAM_set1_ip_asc(::SSL_get0_param(ssl), name.c_str());
      } else {
        ::SSL_set1_host(ssl, name.c_str());
      }
    }
  }
  return true;
}

void NetStream::describeHandshakeFailure(const AsioErrorCode &ec) {
  long verify = X509_V_OK;
  if (tls_) {
    verify = ::SSL_get_verify_result(tls_->native_handle());
  }
  if (verify != X509_V_OK) {
    tlsErrorCode_ = "tls_cert_invalid";
    tlsErrorMessage_ = ::X509_verify_cert_error_string(verify);
    return;
  }
  tlsErrorCode_ = "tls_handshake_failed";
  tlsErrorMessage_ = ec.message();
}

void NetStream::close() {
  AsioErrorCode ignored;
  if (tls_) {
    tls_->lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    tls_->lowest_layer().close(ignored);
    return;
  }
  plain_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
  plain_.close(ignored);
}

bool NetStream::isPeerClosed(const AsioErrorCode &ec) {
  if (!ec) {
    return false;
  }
  if (ec == asio::error::eof || ec == asio::error::connection_reset ||
      ec == asio::error::operation_aborted) {
    return true;
  }
  // 对端没发 close_notify 就断开: 已收到的数据仍然有效
  return ec == asio::ssl::error::stream_truncated;
}

} // namespace extern_plugin
} // namespace musicxx
