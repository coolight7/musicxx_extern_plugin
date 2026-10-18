/// musicxx 外部插件原生插件框架: 插件本地 HTTP 文件服务
///
/// 为什么要有它: QuickJS 里没有任何 socket API, 而"本地 HTTP 代理音源"这类演示
/// (把本地文件 / 插件自己取到的数据变成播放器能打开的 `http://` 地址) 必须有人监听端口。
/// 这个类就是那"唯一的一小块原生能力": 插件登记文件或一段内容, 拿到一个带随机 token 的 URL。
///
/// 安全边界 (改这里之前先读):
/// - **只监听回环** `127.0.0.1`, 端口由系统分配 (不写固定端口、不打局域网);
/// - URL 里只有随机 token, 服务端按 token 查表, **不做路径解析** —— 构造 URL 读不到别的文件;
/// - 服务属于**插件实例**: 插件停用 / 卸载 / 插件框架停机时一起关掉 (端口释放);
/// - 同时处理的连接数有上限 (`maxConnections`), 超出的直接关掉;
/// - 登记条数有上限 (`maxEntries`), 满了按登记顺序淘汰最旧的。
///
/// 线程: 一条 accept 线程 (asio io_context) + 一个连接线程池; 都只在内部使用,
/// 对外接口 (`serveFile` / `serveBytes` / `infoJson` / `stop`) 可在任意线程调用。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace musicxx {
namespace extern_plugin {

/// 插件本地 HTTP 文件服务 (每个 JS 插件实例最多一个)
class PluginHttpFileServer {
 public:
  /// 一次登记的结果
  struct ServedEntry {
    std::string url;      ///< `http://127.0.0.1:<port>/f/<token>/<名字>`
    int64_t size = 0;     ///< 字节数
    int64_t mtimeMs = 0;  ///< 文件登记时的修改时间 (内存内容为 0)
    bool reused = false;  ///< true = 命中已有登记, URL 与上次相同
  };

  /// 创建一个服务 (此时还没有监听; 第一次登记时惰性启动)
  ///
  /// `ownerLabel` 只用于日志 (插件 id)。
  static std::shared_ptr<PluginHttpFileServer> create(std::string ownerLabel);

  ~PluginHttpFileServer();
  PluginHttpFileServer(const PluginHttpFileServer &) = delete;
  PluginHttpFileServer &operator=(const PluginHttpFileServer &) = delete;

  /// 登记一个本地文件 (第一次调用时启动监听)
  ///
  /// 同一个路径 + 同样的 size/mtime 重复登记返回同一个 URL。
  /// 路径按 UTF-8 解释, Windows 上会转成宽字符打开 (支持中文与空格)。
  bool serveFile(const std::string &path, const std::string &contentType,
                 const std::string &fileName, ServedEntry &out,
                 std::string &err);

  /// 登记一段内存内容 (适合 HLS 播放列表这类小文件)
  ///
  /// 内容超过 [maxInlineBytes] 时拒绝 (大文件请用 [serveFile])。
  bool serveBytes(std::string bytes, const std::string &contentType,
                  const std::string &fileName, ServedEntry &out,
                  std::string &err);

  /// 服务状态 (JSON; 键与 JS 侧 `musicxx.net.serverInfo()` 一致)
  std::string infoJson() const;

  /// 关闭监听并释放端口 (幂等; 可重复调用)
  void stop();

  /// 单段内存内容的上限 (8 MiB)
  static constexpr int64_t maxInlineBytes = 8 * 1024 * 1024;
  /// 登记条数上限
  static constexpr size_t maxEntries = 1024;
  /// 同时处理的连接上限 (超出直接关连接)
  static constexpr size_t maxConnections = 32;
  /// 单次登记的文件名长度上限 (只用于 URL 尾段展示)
  static constexpr size_t maxFileNameLength = 120;

 private:
  struct Impl;
  explicit PluginHttpFileServer(std::string ownerLabel);
  std::shared_ptr<Impl> impl_;
};

}  // namespace extern_plugin
}  // namespace musicxx
