/// musicxx 外部插件原生插件框架: 插件网络能力 (HTTP / TCP / WebSocket)
///
/// 为什么要有它: JS 运行时里没有任何 socket API, 而"插件自己提供音源 / 自己搭一个小服务"
/// 这类需求都要能联网。这里实现一套**插件框架内置**的网络能力, JS插件与动态库插件共用:
/// - HTTP 客户端（插件发起请求）与 HTTP 服务端（插件回答请求）;
/// - TCP 客户端与 TCP 服务（监听、接受连接、收发字节）;
/// - WebSocket 客户端与服务端（握手、文本/二进制消息、ping/pong、关闭握手）。
///
/// 与 `PluginHttpFileServer` 的分工: 那个是"把文件变成 http 地址"的固定用途小服务;
/// 这里是通用网络能力, 由插件自己决定收发什么。
///
/// 安全边界 (改这里之前先读):
/// - 服务端默认**只监听回环** `127.0.0.1`; 插件要监听局域网必须显式给 `host: "0.0.0.0"`,
///   文档里写明这意味着同网段可访问;
/// - 只支持 `http://` 与 `ws://`: 这个构建没有链接 TLS 库, `https://` / `wss://`
///   返回 `unsupported_scheme`（要 https 用主程序的 `musicxx.net.fetch`）;
/// - 连接数、服务数、单条消息大小、等待队列都有上限 (见实现里的常量),
///   超限按可读错误返回, 不做无界排队;
/// - 网络能力跟着**插件实例**: 停用 / 卸载 / 关『拟声++』/ 插件框架停机时,
///   该插件的全部服务与连接一起关掉 (端口释放)。
///
/// 线程模型:
/// - 全部 socket 状态归**一条独立的网络线程** (自己的 `asio::io_context`), 与插件框架线程分开;
/// - `call` / `callAsync` / `cancel` 可在任意线程调用: 内部投递到网络线程,
///   需要结果时**有界等待** (网络线程不会反过来等插件框架线程, 因此不会互锁);
/// - 事件经 `PublishFn` 发出 (调用方实现成"投递到插件框架线程再广播"),
///   异步完成回调经 `PostFn` 投递回插件框架线程后执行 —— 动态库插件代码只在框架线程跑。
#pragma once

#include "musicxx_extern_plugin_api.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace musicxx {
namespace extern_plugin {

/// 插件网络能力的中心 (进程内一个; 由 `MusicxxHostManager` 持有)
class PluginNetHub {
public:
  /// 发一条插件网络事件 (实现里已带 `kind` 字段)
  ///
  /// 调用方必须把它投递到插件框架线程再广播 (`MusicxxHostManager::publishPluginEventFrom`
  /// 就是这个语义), 不要在网络线程上直接调插件代码。
  using PublishFn = std::function<void(const std::string &pluginId,
                                       const std::string &payloadJson)>;

  /// 把一个函数投递到插件框架线程执行 (异步完成回调用)
  using PostFn = std::function<void(std::function<void()>)>;

  /// 异步操作的完成回调
  ///
  /// - `status` = `PLUGINXX_OPERATOR_OK` 时 `payloadJson` 是结果 JSON;
  /// - `status` = `PLUGINXX_OPERATOR_FAILED` 时 `payloadJson` 是错误 JSON
  ///   (`{ok:false,error,message}`);
  /// - 取消 (插件卸载 / 框架停机) 用 `PLUGINXX_OPERATOR_CANCELLED` + 空载荷。
  using DoneFn = std::function<void(int32_t status, const std::string &payloadJson)>;

  static std::shared_ptr<PluginNetHub> create(PublishFn publish,
                                             PostFn postToFramework);
  ~PluginNetHub();
  PluginNetHub(const PluginNetHub &) = delete;
  PluginNetHub &operator=(const PluginNetHub &) = delete;

  /// 起网络线程 (幂等)
  void start();

  /// 停掉全部服务与连接并释放网络线程 (幂等)
  void stop();

  bool running() const;

  /// 同步操作 (`op` 见 `docs/plugin-api.md` 的网络一节)
  ///
  /// 返回 `MUSICXX_EXTERN_PLUGIN_OK` 表示"操作执行完毕"(结果里的 `ok` 才是业务结论);
  /// 参数非法/未启动等返回负错误码 (不写 outJson)。
  int32_t call(const std::string &pluginId, const std::string &op,
               const std::string &argsJson, std::string &outJson);

  /// 插件私有数据根目录 (`http.download` 落盘到 `<dataDir>/<插件 id>/data/downloads/`)
  ///
  /// 要在 `start()` 之前设好 (主程序在 `host_config.data_dir` 里给); 不设时
  /// `http.download` 返回 `no_data_dir`。
  void setDataDir(const std::string &dir);

  /// 异步操作 (目前是 `http.request`; 其它 op 返回参数错误)
  ///
  /// - 受理成功写出 `outRequestId` 与受理结果 `outJson` (`{ok:true,requestId:N}`),
  ///   结果**恰好一次**经 `done` 给出 (插件框架线程);
  /// - 受理失败返回负错误码, `outJson` 里是可读原因, **不回调 `done`**
  ///   (与 `request_action` 的"受理失败不推事件"同一约定)。
  int32_t callAsync(const std::string &pluginId, const std::string &op,
                    const std::string &argsJson, DoneFn done,
                    std::string &outJson, int64_t *outRequestId);

  /// 取消一个进行中的异步请求 (不存在/已完成返回 `ERR_NOT_FOUND`)
  int32_t cancel(const std::string &pluginId, int64_t requestId,
                 std::string &outJson);

  /// 关掉某个插件的全部服务、连接与未完成请求 (插件停用 / 卸载 / 框架停机)
  ///
  /// 返回需要按"已取消"结算的完成回调 —— **调用方要在插件框架线程上逐个调用**
  /// (网络线程不能执行插件代码)。
  std::vector<DoneFn> shutdownPlugin(const std::string &pluginId);

  /// 网络能力现状 (`net.info` 的数据源; pluginId 为空 = 全部插件)
  std::string infoJson(const std::string &pluginId);

  /// 实现细节 (仅供 host_net.cpp 里的连接类使用; 不要从外部引用)
  struct Impl;

private:
  explicit PluginNetHub(std::shared_ptr<Impl> impl);

  std::shared_ptr<Impl> impl_;
};

} // namespace extern_plugin
} // namespace musicxx
