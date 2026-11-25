/// musicxx 外部插件框架原生测试 (不依赖 Dart)
///
/// 覆盖:
/// - test_host_lifecycle: create/stop/destroy 幂等;
/// - test_plugin_load: 扫描 + 加载示例插件 + 能力注册生效;
/// - test_capabilities: decision 能力裁决 (cancel) / 观察能力 /
/// 未命中时零处理器;
/// - test_namespace: 官方 id 的命名空间规则 + 自定义
/// id（`plugin.<id>.*`）不受预设表限制;
/// - 卸载后能力注册全部摘除 (无残留)。
///
/// 运行: musicxx_extern_plugin_test <example_native 插件目录>

#include "musicxx_extern_plugin_api.h"

#include "utilxx_base/log.h"
#include "host_text.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#if defined(_WIN32)
// GetACP: 下面的编码用例只在中文 ANSI 代码页下跑
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

int g_failed = 0;
int g_checks = 0;

void check(bool ok, const char *what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("  [FAIL] %s\n", what);
  } else {
    std::printf("  [ ok ] %s\n", what);
  }
}

MusicxxExternPluginStringView view(const std::string &s) {
  MusicxxExternPluginStringView v{};
  v.data = s.data();
  v.size = s.size();
  return v;
}

/// 指针版视图 (C ABI 入参形态)
///
/// 注意: 槽位是滚动复用的静态数组, 因此**一次调用里的视图个数必须 ≤ 槽位数**
/// (C++ 不规定实参求值顺序, 槽位太少会让后写入的值覆盖先写入的视图)。
MusicxxExternPluginStringView *viewP(const std::string &s) {
  static MusicxxExternPluginStringView slots[8];
  static int next = 0;
  auto &v = slots[next++ % 8];
  v.data = s.data();
  v.size = s.size();
  return &v;
}

MusicxxExternPluginStringView viewC(const char *s) {
  MusicxxExternPluginStringView v{};
  v.data = s;
  v.size = std::strlen(s);
  return v;
}

MusicxxExternPluginStringView *viewCP(const char *s) {
  static MusicxxExternPluginStringView slots[8];
  static int next = 0;
  auto &v = slots[next++ % 8];
  v.data = s;
  v.size = std::strlen(s);
  return &v;
}

void freeStr(MusicxxExternPluginString &s) {
  musicxx_extern_plugin_string_free(&s);
}

std::string take(MusicxxExternPluginString &s) {
  std::string out;
  if (s.data) {
    out.assign(s.data, static_cast<size_t>(s.size));
  }
  freeStr(s);
  return out;
}

/// 旧版派发标志 (`DISPATCH_SYNC` / `DISPATCH_ASYNC` 已从 C ABI 删掉)
///
/// 现在只有一种派发: 提交 + 一条结果事件。测试里的同步包装 `t_capability_dispatch*`
/// 一律等结果, 所以这两个标志只用来保留调用点的形状。
#define MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC 0
#define MUSICXX_EXTERN_PLUGIN_DISPATCH_ASYNC 1

// 提交式包装定义在后面 (要用到 [waitReply]); 这里的等待辅助先声明一下
int32_t t_capability_dispatch(MusicxxExternPluginHost *h,
                               const MusicxxExternPluginStringView *id,
                               const MusicxxExternPluginStringView *input,
                               int32_t flags, uint32_t timeoutMs,
                               MusicxxExternPluginString *out,
                               MusicxxExternPluginString *log);
int32_t t_plugin_call_async(MusicxxExternPluginHost *h,
                            const MusicxxExternPluginStringView *id,
                            const MusicxxExternPluginStringView *method,
                            const MusicxxExternPluginStringView *argsJson,
                            int64_t *outRequestId,
                            MusicxxExternPluginString *log);

/* ==================== 能力调用 (测试专用等待包装) ==================== */

/// 取 JSON 里某个字符串字段（定义在下面的工具区；这里先声明，辅助函数要用到）
std::string jsonStringField(const std::string &json, const std::string &key);

/// 一次能力调用的结果 (受理码 + 结果事件)
///
/// 提交成功后结果经 `musicxx.request.result` 事件回填。测试里等结果是允许的 ——
/// 这里是同步测试代码, 等的东西只可能是事件, 不会互锁。
struct CapabilityCallOutcome {
  int32_t acceptRc = PLUGINXX_RESULT_ERR_STATE; ///< 受理码 (0 = 已受理)
  std::string acceptErr;                              ///< 受理失败原因
  int64_t requestId = 0;
  bool gotResult = false; ///< 收到结果事件 (受理成功时一定有)
  bool ok = false;        ///< 结果事件里的 ok
  std::string payload;    ///< ok 时是结果 JSON, 否则是错误说明
  int64_t resultMs = 0;
};

/// 等待能力结果期间取到的**其它**事件数组 (每项形如 `[{...}]`, 由 [pollEvents]
/// 放回)
std::vector<std::string> &stashedEvents() {
  static std::vector<std::string> list;
  return list;
}

/// 插件框架上报的插件 ABI 代次（从 plugin_inspect 取一次，后面的用例据此比对）
std::string g_pluginAbiVersion;

/// 去掉 JSON 数组最外层的方括号 (`[]` 与空串都给空串)
std::string stripArray(const std::string &json) {
  if (json.size() < 2 || json.front() != '[' || json.back() != ']') {
    return json;
  }
  return json.substr(1, json.size() - 2);
}

/// 一次事件读取的结果 (rc + 事件数组 JSON)
struct PolledEvents {
  int32_t rc = 0;
  std::string json = "[]";
};

/// 测试里唯一的事件出口: 先把等待能力结果期间攒下的放回来, 再 poll 一次
///
/// 为什么需要: 结果事件与别的事件在同一条队列里, 等结果的循环只能一条一条取
/// (`max_count = 1`) —— 取到的别的必须攒着放回去, 不能悄悄吃掉。
PolledEvents pollEvents(MusicxxExternPluginHost *host, int maxCount) {
  MusicxxExternPluginString events{};
  MusicxxExternPluginString log{};
  const int32_t rc =
      musicxx_extern_plugin_poll_events(host, maxCount, &events, &log);
  freeStr(log);
  const std::string fresh = stripArray(take(events));
  std::string merged = "[";
  bool first = true;
  for (const std::string &item : stashedEvents()) {
    const std::string body = stripArray(item);
    if (body.empty()) {
      continue;
    }
    if (!first) {
      merged += ",";
    }
    first = false;
    merged += body;
  }
  stashedEvents().clear();
  if (!fresh.empty()) {
    if (!first) {
      merged += ",";
    }
    merged += fresh;
  }
  merged += "]";
  return PolledEvents{rc, merged};
}

/// 让一份内容生成一次, 返回 `musicxx.ui.content` 事件里那一条的 JSON
///
/// 参数 `id` 写内容全名 (`plugin.<插件 id>.<内容 id>`); 返回空串 = 没拿到结果
/// (没有这份内容 / 生成失败时事件里只有 `error`)。
std::string requestUiContent(MusicxxExternPluginHost *host, const char *id,
                             const char *contextJson = "{}") {
  MusicxxExternPluginStringView idView{id, std::strlen(id)};
  MusicxxExternPluginStringView ctxView{contextJson, std::strlen(contextJson)};
  int64_t requestId = 0;
  MusicxxExternPluginString log{};
  const int32_t rc = musicxx_extern_plugin_ui_request(host, &idView, &ctxView,
                                                     &requestId, &log);
  freeStr(log);
  if (rc != PLUGINXX_RESULT_OK) {
    return {};
  }
  for (int i = 0; i < 20; ++i) {
    const PolledEvents polled = pollEvents(host, 500);
    const std::string events = polled.json;
    const std::string needle = "\"id\":\"" + std::string{id} + "\"";
    size_t pos = events.find(needle);
    while (pos != std::string::npos) {
      // 事件是 JSON 数组, 取包含这条 id 的那个对象 (按花括号配对)
      const size_t start = events.rfind('{', pos);
      if (start == std::string::npos) {
        break;
      }
      int depth = 0;
      bool inString = false;
      size_t end = std::string::npos;
      for (size_t at = start; at < events.size(); ++at) {
        const char c = events[at];
        if (inString) {
          if (c == '\\') {
            ++at;
          } else if (c == '"') {
            inString = false;
          }
          continue;
        }
        if (c == '"') {
          inString = true;
        } else if (c == '{') {
          ++depth;
        } else if (c == '}') {
          --depth;
          if (depth == 0) {
            end = at;
            break;
          }
        }
      }
      if (end == std::string::npos) {
        break;
      }
      const std::string body = events.substr(start, end - start + 1);
      // 注册表变化的事件也带这条 id (`rev:0`), 但那是"注册了一条内容", 不是生成结果:
      // 只认带 `json` / `error` 的那一条
      if (body.find("\"json\"") != std::string::npos ||
          body.find("\"error\"") != std::string::npos) {
        return body;
      }
      pos = events.find(needle, pos + needle.size());
    }
    // 没取到就再等一会儿 (生成是异步投递到插件线程的)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return {};
}

/// 从 JSON 文本里取出某个键的**原始值文本** (对象/数组原样给, 字符串去掉引号)
///
/// 用途: 结果事件载荷里的 `result` 是任意 JSON,
/// 不能被"取字符串字段"的写法吃掉。 只做浅扫描: 找到 `"key":`
/// 后按括号配对与字符串转义跳过, 不解析整份 JSON。
std::string jsonRawField(const std::string &json, const std::string &key) {
  const std::string needle = "\"" + key + "\":";
  size_t pos = json.find(needle);
  if (pos == std::string::npos) {
    return {};
  }
  pos += needle.size();
  while (pos < json.size() && json[pos] == ' ') {
    ++pos;
  }
  if (pos >= json.size()) {
    return {};
  }
  const char first = json[pos];
  if (first == '"') {
    std::string out;
    size_t at = pos + 1;
    while (at < json.size()) {
      const char c = json[at];
      if (c == '\\' && at + 1 < json.size()) {
        out.push_back(c);
        out.push_back(json[at + 1]);
        at += 2;
        continue;
      }
      if (c == '"') {
        break;
      }
      out.push_back(c);
      ++at;
    }
    return out;
  }
  if (first == '{' || first == '[') {
    const char close = (first == '{') ? '}' : ']';
    int depth = 0;
    bool inString = false;
    size_t at = pos;
    while (at < json.size()) {
      const char c = json[at];
      if (inString) {
        if (c == '\\' && at + 1 < json.size()) {
          at += 2;
          continue;
        }
        if (c == '"') {
          inString = false;
        }
      } else if (c == '"') {
        inString = true;
      } else if (c == first) {
        ++depth;
      } else if (c == close) {
        --depth;
        if (depth == 0) {
          ++at;
          break;
        }
      }
      ++at;
    }
    return json.substr(pos, at - pos);
  }
  size_t at = pos;
  while (at < json.size() && json[at] != ',' && json[at] != '}' &&
         json[at] != ']') {
    ++at;
  }
  std::string out = json.substr(pos, at - pos);
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

/// 事件文本里的载荷段 (`"payload":` 之后的整段; 事件对象里它是最后一个键)
std::string payloadOfEvent(const std::string &eventJson) {
  const std::string needle = "\"payload\":";
  const size_t pos = eventJson.find(needle);
  if (pos == std::string::npos) {
    return {};
  }
  return eventJson.substr(pos + needle.size());
}

/// 在攒下的事件里找指定 requestId 的结果（找到就取出来并删掉那一条）
///
/// 为什么要找攒下的: 等别的请求结果时（如卸载）把这条结果当成"别的事"攒了起来,
/// 之后再来等它就永远等不到了。
std::string takeStashedResult(int64_t requestId) {
  const std::string needle = "\"requestId\":" + std::to_string(requestId);
  auto &stash = stashedEvents();
  for (auto it = stash.begin(); it != stash.end(); ++it) {
    if (it->find("\"type\":\"musicxx.request.result\"") == std::string::npos) {
      continue;
    }
    if (it->find(needle) == std::string::npos) {
      continue;
    }
    const std::string payload = payloadOfEvent(*it);
    stash.erase(it);
    return payload;
  }
  return {};
}

/// 等一条结果事件并按 `requestId` 取它的载荷（别的攒进 stash）
///
/// 返回空串 = 在时限内没等到（附带把取到的其它事件攒起来）。
std::string waitCapabilityResult(MusicxxExternPluginHost *host,
                                 int64_t requestId, uint32_t waitMs = 3000) {
  if (const std::string stashed = takeStashedResult(requestId);
      !stashed.empty()) {
    return stashed;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds{waitMs};
  while (std::chrono::steady_clock::now() < deadline) {
    MusicxxExternPluginString events{};
    MusicxxExternPluginString pollLog{};
    musicxx_extern_plugin_poll_events(host, 1, &events, &pollLog);
    freeStr(pollLog);
    const std::string body = stripArray(take(events));
    if (body.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds{20});
      continue;
    }
    const bool isResult =
        body.find("\"type\":\"musicxx.request.result\"") !=
            std::string::npos &&
        body.find("\"requestId\":" + std::to_string(requestId)) !=
            std::string::npos;
    if (!isResult) {
      stashedEvents().push_back("[" + body + "]");
      continue;
    }
    return payloadOfEvent(body);
  }
  return {};
}

/// 同步派发一次能力，返回**完整回执**（`{handled, handlers, called, result?}`）
///
/// 测试里同步等是允许的：等的是插件框架线程上的处理器链，不会互锁。
/// 裁决对象（合并后的字段）用 `jsonRawField(ack, "result")` 取 ——
/// 没有处理器给出 `patch` / `action` 时回执里没有 `result` 这一段。
std::string dispatchCapabilitySync(MusicxxExternPluginHost *host,
                                   const char *capabilityId,
                                   const std::string &inputJson) {
  MusicxxExternPluginString out{};
  MusicxxExternPluginString emitLog{};
  const auto rc = t_capability_dispatch(
      host, viewCP(capabilityId), viewP(inputJson),
      MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC, 0, &out, &emitLog);
  freeStr(emitLog);
  return take(out);
}

/// 调一次插件能力并等到结果事件 (受理失败时不推事件, 直接返回受理码)
CapabilityCallOutcome callCapability(MusicxxExternPluginHost *host,
                                     const std::string &id,
                                     const std::string &method,
                                     const std::string &argsJson,
                                     uint32_t waitMs = 5000) {
  CapabilityCallOutcome outcome;
  MusicxxExternPluginString log{};
  int64_t requestId = 0;
  const auto rc = t_plugin_call_async(
      host, viewP(id), viewP(method), viewP(argsJson), &requestId, &log);
  outcome.acceptErr = take(log);
  outcome.acceptRc = rc;
  if (rc != PLUGINXX_RESULT_OK) {
    return outcome; ///< 受理失败: 没有 requestId, 也不会推事件
  }
  outcome.requestId = requestId;

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds{waitMs};
  while (std::chrono::steady_clock::now() < deadline) {
    /// 注意: 这里**直接** `poll_events(1)`, 不经过 [pollEvents] 的合并 ——
    /// 等结果期间取到的每条都按原样攒进 [stashedEvents] (一条一个条目),
    /// 不然"攒下的" 与 "等到的结果" 混在一条 body 里,
    /// 解析出来的载荷会是别人的。
    MusicxxExternPluginString events{};
    MusicxxExternPluginString pollLog{};
    musicxx_extern_plugin_poll_events(host, 1, &events, &pollLog);
    freeStr(pollLog);
    const std::string body = stripArray(take(events));
    if (body.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds{20});
      continue;
    }
    // 一条一条取: 是不是"本次等待的那条结果事件"
    const bool isResult =
        body.find("\"type\":\"musicxx.request.result\"") !=
            std::string::npos &&
        body.find("\"requestId\":" + std::to_string(requestId)) !=
            std::string::npos;
    if (!isResult) {
      stashedEvents().push_back("[" + body + "]");
      continue;
    }
    const std::string payload = payloadOfEvent(body);
    outcome.gotResult = true;
    outcome.ok = payload.find("\"ok\":true") != std::string::npos;
    if (outcome.ok) {
      const std::string data = jsonRawField(payload, "data");
      outcome.payload = (data == "null") ? std::string{} : data;
    } else {
      const std::string errorJson = jsonRawField(payload, "error");
      outcome.payload = jsonStringField(errorJson, "message");
      if (outcome.payload.empty()) {
        outcome.payload = jsonStringField(errorJson, "reason");
      }
    }
    return outcome;
  }
  return outcome; ///< 超时没等到: gotResult 保持 false, 由用例判失败
}

/* ==================== 提交式 C ABI 的同步包装 (测试专用) ==================== */


/// 一次提交的结果（`musicxx.request.result` 的载荷）
struct RequestReply {
  int32_t acceptRc = 0; ///< 受理码（非 0 = 未受理，不会推事件）
  int32_t code = 0;     ///< 结果事件里的 error.code
  bool got = false;     ///< 等到结果事件
  bool ok = false;      ///< 结果事件里的 ok
  std::string data;     ///< ok 时是 data 原文（`null` → 空串）
  std::string error;    ///< 失败原因（给人看的那句）
  std::string reason;   ///< 失败分类名（`not_found` / `host_stopped` …）
};

/// 把文本写进插件框架堆字符串出参（覆盖原值）
void setOutStr(MusicxxExternPluginString *out, const std::string &text) {
  if (!out) {
    return;
  }
  const MusicxxExternPluginStringView view{text.data(), text.size()};
  musicxx_extern_plugin_strdup_n(&view, out);
}

/// 等一条 `musicxx.request.result`（按 `requestId` 配；别的攒进 stash）
///
/// 默认给 30 s: 插件 `create` 里发起的应用能力请求在原生测试宿主下没人回答,
/// 要等能力自带的 5 s 超时才能继续, 加上加载本身后 5 s 不够。
RequestReply waitReply(MusicxxExternPluginHost *host, int64_t requestId,
                       uint32_t waitMs = 30000) {
  RequestReply reply;
  const std::string needle = "\"requestId\":" + std::to_string(requestId);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds{waitMs};
  while (std::chrono::steady_clock::now() < deadline) {
    /// 一条一条取：攒下的与等到的混在一起会解析出别人的载荷
    MusicxxExternPluginString events{};
    MusicxxExternPluginString pollLog{};
    musicxx_extern_plugin_poll_events(host, 1, &events, &pollLog);
    freeStr(pollLog);
    const std::string body = stripArray(take(events));
    if (body.empty()) {
      std::this_thread::sleep_for(std::chrono::milliseconds{20});
      continue;
    }
    const bool isResult =
        body.find("\"type\":\"musicxx.request.result\"") != std::string::npos &&
        body.find(needle) != std::string::npos;
    if (!isResult) {
      stashedEvents().push_back("[" + body + "]");
      continue;
    }
    const std::string payload = payloadOfEvent(body);
    reply.got = true;
    reply.ok = payload.find("\"ok\":true") != std::string::npos;
    if (reply.ok) {
      const std::string data = jsonRawField(payload, "data");
      reply.data = (data == "null") ? std::string{} : data;
    } else {
      const std::string errorJson = jsonRawField(payload, "error");
      const std::string code = jsonRawField(errorJson, "code");
      reply.code = code.empty() ? 0 : static_cast<int32_t>(std::atoi(code.c_str()));
      reply.reason = jsonStringField(errorJson, "reason");
      reply.error = jsonStringField(errorJson, "message");
      if (reply.error.empty()) {
        reply.error = reply.reason;
      }
      if (reply.error.empty()) {
        reply.error = "请求失败";
      }
    }
    return reply;
  }
  reply.error = "等待结果事件超时";
  return reply;
}

/// 提交一次请求并等它的结果（`submit` 只负责受理并写 `requestId`）
template <typename Fn>
RequestReply submitAndWait(MusicxxExternPluginHost *host,
                           MusicxxExternPluginString *log, Fn &&submit) {
  RequestReply reply;
  int64_t requestId = 0;
  const int32_t rc = submit(&requestId);
  reply.acceptRc = rc;
  if (rc != PLUGINXX_RESULT_OK) {
    /// 受理失败：没有 requestId，也不会有结果事件；原因已经在 `log` 里
    const std::string text = log ? take(*log) : std::string{};
    reply.error = text.empty() ? "受理失败" : text;
    reply.reason = "rejected";
    if (log) {
      setOutStr(log, reply.error);
    }
    return reply;
  }
  return waitReply(host, requestId);
}

/// 包装的返回码：受理码 → 结果事件里的错误码 → 状态错 / 超时
int32_t replyCode(const RequestReply &reply) {
  if (reply.acceptRc != PLUGINXX_RESULT_OK) {
    return reply.acceptRc;
  }
  if (reply.ok) {
    return PLUGINXX_RESULT_OK;
  }
  if (!reply.got) {
    return PLUGINXX_RESULT_ERR_TIMEOUT;
  }
  return reply.code != 0 ? reply.code : PLUGINXX_RESULT_ERR_STATE;
}

/// 把结果数据写进出参（失败时不碰出参，把原因写进 log）
int32_t replyToOut(const RequestReply &reply, MusicxxExternPluginString *out,
                   MusicxxExternPluginString *log) {
  const int32_t code = replyCode(reply);
  if (code == PLUGINXX_RESULT_OK) {
    if (out) {
      setOutStr(out, reply.data);
    }
  } else if (log) {
    setOutStr(log, reply.error);
  }
  return code;
}

/// 只要返回码的包装用它: 失败时把结果事件里的原因写进 `log`
///
/// 旧符号是在同一次调用里既回码又写 `log` 的, 调用点习惯直接看 `log`。
int32_t replyLog(const RequestReply &reply, MusicxxExternPluginString *log) {
  const int32_t code = replyCode(reply);
  if (code != PLUGINXX_RESULT_OK && log && !reply.error.empty()) {
    setOutStr(log, reply.error);
  }
  return code;
}

int32_t t_host_start(MusicxxExternPluginHost *h, MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_host_start(h, rid, log);
  });
  return replyLog(reply, log);
}

int32_t t_host_stop(MusicxxExternPluginHost *h, uint32_t timeoutMs,
                    MusicxxExternPluginString *log) {
  (void)timeoutMs; ///< 停机等待上限在插件框架里（5 s），这里只等结果
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_host_shutdown(h, rid, log);
  });
  return replyLog(reply, log);
}

int32_t t_set_language(MusicxxExternPluginHost *h,
                       const MusicxxExternPluginStringView *lang,
                       MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_set_language(h, lang, rid, log);
  });
  return replyLog(reply, log);
}

int32_t t_set_config(MusicxxExternPluginHost *h,
                     const MusicxxExternPluginStringView *json,
                     MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_set_config(h, json, rid, log);
  });
  return replyLog(reply, log);
}

int32_t t_plugin_scan(MusicxxExternPluginHost *h,
                      MusicxxExternPluginString *out,
                      MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_plugin_scan(h, rid, log);
  });
  return replyToOut(reply, out, log);
}

int32_t t_plugin_inspect(MusicxxExternPluginHost *h,
                         const MusicxxExternPluginStringView *dir,
                         const MusicxxExternPluginStringView *os,
                         const MusicxxExternPluginStringView *arch,
                         MusicxxExternPluginString *out,
                         MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_plugin_inspect(h, dir, os, arch, rid, log);
  });
  return replyToOut(reply, out, log);
}

int32_t t_plugin_list(MusicxxExternPluginHost *h,
                      MusicxxExternPluginString *out,
                      MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_plugin_list(h, rid, log);
  });
  return replyToOut(reply, out, log);
}

int32_t t_plugin_load_sync(MusicxxExternPluginHost *h,
                           const MusicxxExternPluginStringView *idOrPath,
                           const MusicxxExternPluginStringView *options,
                           uint32_t timeoutMs,
                           MusicxxExternPluginString *log) {
  (void)timeoutMs; ///< 加载事务的等待上限在插件框架里
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_plugin_load(h, idOrPath, options, rid, log);
  });
  return replyLog(reply, log);
}

int32_t t_plugin_enable(MusicxxExternPluginHost *h,
                        const MusicxxExternPluginStringView *id,
                        MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_plugin_enable(h, id, rid, log);
  });
  return replyLog(reply, log);
}

int32_t t_plugin_disable(MusicxxExternPluginHost *h,
                         const MusicxxExternPluginStringView *id,
                         MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_plugin_disable(h, id, rid, log);
  });
  return replyLog(reply, log);
}

int32_t t_plugin_unload(MusicxxExternPluginHost *h,
                        const MusicxxExternPluginStringView *id,
                        MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_plugin_unload(h, id, rid, log);
  });
  return replyLog(reply, log);
}

int32_t t_plugin_reload(MusicxxExternPluginHost *h,
                        const MusicxxExternPluginStringView *id,
                        MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_plugin_reload(h, id, rid, log);
  });
  return replyLog(reply, log);
}

/// 同上，但只受理不等待（要自己等结果的用例用它）
int32_t t_plugin_call_async(MusicxxExternPluginHost *h,
                            const MusicxxExternPluginStringView *id,
                            const MusicxxExternPluginStringView *method,
                            const MusicxxExternPluginStringView *argsJson,
                            int64_t *outRequestId,
                            MusicxxExternPluginString *log) {
  return musicxx_extern_plugin_plugin_call(h, id, method, argsJson,
                                           outRequestId, log);
}

/// 派发一次能力并等结果（`flags` / `timeoutMs` 只为保留旧调用点，已无意义）
int32_t t_capability_dispatch(MusicxxExternPluginHost *h,
                              const MusicxxExternPluginStringView *id,
                              const MusicxxExternPluginStringView *input,
                              int32_t flags, uint32_t timeoutMs,
                              MusicxxExternPluginString *out,
                              MusicxxExternPluginString *log) {
  (void)flags;
  (void)timeoutMs;
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_capability_dispatch(h, id, input, rid, log);
  });
  return replyToOut(reply, out, log);
}

int32_t t_capability_dispatch_phase(MusicxxExternPluginHost *h,
                                    const MusicxxExternPluginStringView *id,
                                    int32_t phase,
                                    const MusicxxExternPluginStringView *input,
                                    int32_t flags, uint32_t timeoutMs,
                                    MusicxxExternPluginString *out,
                                    MusicxxExternPluginString *log) {
  (void)flags;
  (void)timeoutMs;
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_capability_dispatch_phase(h, id, phase, input,
                                                          rid, log);
  });
  return replyToOut(reply, out, log);
}

// 下面两个查询先声明: 计数的包装要用它们（定义在后面的能力查询区）
int32_t t_capability_handlers(MusicxxExternPluginHost *h,
                              const MusicxxExternPluginStringView *id,
                              MusicxxExternPluginString *out,
                              MusicxxExternPluginString *log);
int32_t t_capability_handlers_phase(MusicxxExternPluginHost *h,
                                    const MusicxxExternPluginStringView *id,
                                    int32_t phase,
                                    MusicxxExternPluginString *out,
                                    MusicxxExternPluginString *log);

/// 处理器列表里的条数（列表是 JSON 数组，每项都带一个 `instance`）
int32_t handlerCountOf(const std::string &listJson) {
  int32_t count = 0;
  const std::string needle = "\"instance\":";
  for (size_t pos = listJson.find(needle); pos != std::string::npos;
       pos = listJson.find(needle, pos + needle.size())) {
    ++count;
  }
  return count;
}

/// `capability_handler_count` 已删除：用 `capability.handlers` 的列表长度顶上
int32_t t_capability_handler_count(MusicxxExternPluginHost *h,
                                   const MusicxxExternPluginStringView *id,
                                   int32_t *outCount,
                                   MusicxxExternPluginString *log) {
  MusicxxExternPluginString list{};
  const int32_t code =
      t_capability_handlers(h, id, &list, log);
  const std::string text = take(list);
  if (code != PLUGINXX_RESULT_OK) {
    return code;
  }
  if (outCount) {
    *outCount = handlerCountOf(text);
  }
  return PLUGINXX_RESULT_OK;
}

int32_t t_capability_handler_count_phase(MusicxxExternPluginHost *h,
                                         const MusicxxExternPluginStringView *id,
                                         int32_t phase, int32_t *outCount,
                                         MusicxxExternPluginString *log) {
  MusicxxExternPluginString list{};
  const int32_t code = t_capability_handlers_phase(h, id, phase, &list, log);
  const std::string text = take(list);
  if (code != PLUGINXX_RESULT_OK) {
    return code;
  }
  if (outCount) {
    *outCount = handlerCountOf(text);
  }
  return PLUGINXX_RESULT_OK;
}

int32_t t_capability_handlers(MusicxxExternPluginHost *h,
                              const MusicxxExternPluginStringView *id,
                              MusicxxExternPluginString *out,
                              MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_capability_handlers(h, id, rid, log);
  });
  return replyToOut(reply, out, log);
}

int32_t t_capability_handlers_phase(MusicxxExternPluginHost *h,
                                    const MusicxxExternPluginStringView *id,
                                    int32_t phase,
                                    MusicxxExternPluginString *out,
                                    MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_capability_handlers_phase(h, id, phase, rid,
                                                          log);
  });
  return replyToOut(reply, out, log);
}

int32_t t_capability_stats(MusicxxExternPluginHost *h,
                           MusicxxExternPluginString *out,
                           MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_capability_stats(h, rid, log);
  });
  return replyToOut(reply, out, log);
}

int32_t t_capability_app_respond(MusicxxExternPluginHost *h,
                                 int64_t requestId, int32_t status,
                                 const MusicxxExternPluginStringView *resultJson,
                                 MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_capability_app_respond(
        h, requestId, status, resultJson, rid, log);
  });
  return replyLog(reply, log);
}

int32_t t_broadcast_publish(MusicxxExternPluginHost *h,
                            const MusicxxExternPluginStringView *topic,
                            const MusicxxExternPluginStringView *payload,
                            MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_broadcast_publish(h, topic, payload, rid, log);
  });
  return replyLog(reply, log);
}

int32_t t_broadcast_read(MusicxxExternPluginHost *h,
                         const MusicxxExternPluginStringView *id,
                         MusicxxExternPluginString *out,
                         MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_broadcast_read(h, id, rid, log);
  });
  return replyToOut(reply, out, log);
}

int32_t t_broadcast_list(MusicxxExternPluginHost *h,
                         const MusicxxExternPluginStringView *prefix,
                         MusicxxExternPluginString *out,
                         MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_broadcast_list(h, prefix, rid, log);
  });
  return replyToOut(reply, out, log);
}

int32_t t_ui_snapshot(MusicxxExternPluginHost *h,
                      MusicxxExternPluginString *out,
                      MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_ui_snapshot(h, rid, log);
  });
  return replyToOut(reply, out, log);
}

int32_t t_ui_contents(MusicxxExternPluginHost *h,
                      MusicxxExternPluginString *out,
                      MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_ui_contents(h, rid, log);
  });
  return replyToOut(reply, out, log);
}

int32_t t_ui_slots(MusicxxExternPluginHost *h, MusicxxExternPluginString *out,
                   MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_ui_slots(h, rid, log);
  });
  return replyToOut(reply, out, log);
}

int32_t t_stats(MusicxxExternPluginHost *h,
                const MusicxxExternPluginStringView *scope,
                MusicxxExternPluginString *out,
                MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_stats(h, scope, rid, log);
  });
  return replyToOut(reply, out, log);
}

int32_t t_stats_config(MusicxxExternPluginHost *h,
                       const MusicxxExternPluginStringView *cfg,
                       MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_stats_config(h, cfg, rid, log);
  });
  return replyLog(reply, log);
}

int32_t t_debug_info(MusicxxExternPluginHost *h,
                     MusicxxExternPluginString *out,
                     MusicxxExternPluginString *log) {
  const RequestReply reply = submitAndWait(h, log, [&](int64_t *rid) {
    return musicxx_extern_plugin_debug_info(h, rid, log);
  });
  return replyToOut(reply, out, log);
}

/// 从扫描结果 JSON (扁平数组) 中取出指定插件 id 的片段 (到下一个条目的 "id"
/// 之前)
///
/// 用途: 断言某个插件的 `valid`/`supported` 字段 ——
/// 入口文件按平台解析后的结果是否正确 (清单按 Linux 写 entry, Windows/macOS
/// 由插件框架修正扩展名)。
std::string scanItemOf(const std::string &scanJson, const std::string &id) {
  const std::string key = "\"id\":\"" + id + "\"";
  const auto pos = scanJson.find(key);
  if (pos == std::string::npos) {
    return {};
  }
  const auto next = scanJson.find("\"id\":\"", pos + key.size());
  return scanJson.substr(pos, next == std::string::npos ? std::string::npos
                                                        : next - pos);
}

/// 取 JSON 字符串字段的裸值 (测试专用极简解析; 不支持转义)
std::string jsonStringField(const std::string &json, const std::string &key) {
  const std::string needle = "\"" + key + "\":\"";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) {
    return {};
  }
  const auto begin = pos + needle.size();
  const auto end = json.find('"', begin);
  if (end == std::string::npos) {
    return {};
  }
  return json.substr(begin, end - begin);
}

/// 取 JSON 整数字段的裸文本 (测试专用)
std::string jsonIntField(const std::string &json, const std::string &key) {
  const std::string needle = "\"" + key + "\":";
  const auto pos = json.find(needle);
  if (pos == std::string::npos) {
    return {};
  }
  auto begin = pos + needle.size();
  auto end = begin;
  while (end < json.size() &&
         (std::isdigit(static_cast<unsigned char>(json[end])) ||
          json[end] == '-')) {
    ++end;
  }
  return json.substr(begin, end - begin);
}

/// 取视图里第一个形如 `1x` / `0.5x` 的文本 (测试专用)
///
/// 设置页里「背景动画速率」那一行的右侧状态就是这个形态。列表块移除后，行由
/// `Block`(内容块) 与布局块组合出来，没有 `right` 字段可读，因此按值的形态取
/// （与 Dart 侧整套流程测试里的 `_rowOf`
/// 同一个目的：读到那一行显示的状态文字）。
std::string jsonFirstRateText(const std::string &json) {
  const std::string needle = "\"text\":\"";
  size_t pos = 0;
  while ((pos = json.find(needle, pos)) != std::string::npos) {
    const size_t begin = pos + needle.size();
    const size_t end = json.find('"', begin);
    if (end == std::string::npos) {
      return {};
    }
    const std::string value = json.substr(begin, end - begin);
    pos = end;
    size_t i = 0;
    bool digits = false;
    while (i < value.size() &&
           std::isdigit(static_cast<unsigned char>(value[i]))) {
      ++i;
      digits = true;
    }
    if (digits && i < value.size() && value[i] == '.') {
      ++i;
      while (i < value.size() &&
             std::isdigit(static_cast<unsigned char>(value[i]))) {
        ++i;
      }
    }
    if (digits && i + 1 == value.size() && value[i] == 'x') {
      return value;
    }
  }
  return {};
}

/* ==================== 多目标打包（分支选择）测试辅助 ==================== */

/// 与 `check` 同义，但允许拼出带变量的说明文本
void checkText(bool ok, const std::string &what) {
  ++g_checks;
  if (!ok) {
    ++g_failed;
    std::printf("  [FAIL] %s\n", what.c_str());
  } else {
    std::printf("  [ ok ] %s\n", what.c_str());
  }
}

/// 当前编译目标的规范系统名（与插件框架上报的平台标识同规则）
std::string testPlatform() {
#if defined(__ANDROID__)
  return "android";
#elif defined(_WIN32)
  return "windows";
#elif defined(__APPLE__) &&                                                    \
    defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__)
  // iOS / iOS 模拟器：clang 会定义这个宏（macOS 目标不会）
  return "ios";
#elif defined(__APPLE__)
  return "macos";
#elif defined(__linux__)
  return "linux";
#else
  return "unknown";
#endif
}

/// 当前编译目标的规范架构名（与插件框架 `currentTargetArch()` / `hostArch()`
/// 同规则）
std::string testArch() {
#if defined(_M_ARM64) || defined(__aarch64__)
  return "arm64";
#elif defined(_M_X64) || defined(__x86_64__)
  return "x64";
#elif defined(_M_ARM) || defined(__arm__)
  return "armv7"; ///< 32 位 ARM (Android armeabi-v7a / Linux armhf)
#elif defined(_M_IX86) || defined(__i386__)
  return "x86";
#elif defined(__riscv) && (__riscv_xlen == 64)
  return "riscv64";
#elif defined(__loongarch64)
  return "loongarch64";
#else
  return "unknown";
#endif
}

/// 当前系统的别名写法（清单 `platforms` 里写别名与写规范名必须等效）
std::string testOsAlias() {
  const std::string os = testPlatform();
  if (os == "windows") {
    return "win32";
  }
  if (os == "linux") {
    return "gnu";
  }
  if (os == "macos") {
    return "osx";
  }
  if (os == "ios") {
    return "iphoneos";
  }
  return os;
}

/// 当前架构的别名写法（清单 `arch` 里写别名与写规范名必须等效）
std::string testArchAlias() {
  const std::string arch = testArch();
  if (arch == "x64") {
    return "amd64";
  }
  if (arch == "arm64") {
    return "aarch64";
  }
  if (arch == "x86") {
    return "i686";
  }
  return arch;
}

/// 按平台修正扩展名后的库文件名（清单按 Linux 写
/// `<名>.so`，插件框架按平台修正扩展名）
std::string testLibFileName(const std::string &base) {
#if defined(_WIN32)
  return base + ".dll";
#elif defined(__APPLE__)
  // 插件库**没有 `lib` 前缀**（SDK 的 musicxx_plugin_add_target 设了 PREFIX
  // ""）：macOS 上是 `example_native.dylib`，与 Windows 的
  // `example_native.dll`、Linux 的 `example_native.so` 对齐；
  // 只有插件框架库本身叫 libmusicxx_extern_plugin.dylib。
  return base + ".dylib";
#else
  return base + ".so";
#endif
}

void writeTextFile(const std::filesystem::path &path, const std::string &text) {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream out(path, std::ios::binary);
  out << text;
}

void removePath(const std::filesystem::path &path) {
  std::error_code ec;
  std::filesystem::remove_all(path, ec);
}

/// 建一个临时"插件根目录"：`<根>/<id>/plugin.yaml` + 参数里给的相对路径文件
///
/// 文件内容随便给（扫描只看文件在不在）；返回插件根目录（扫描入口）。
std::filesystem::path
makeTempPluginRoot(const std::string &id, const std::string &manifest,
                   const std::vector<std::string> &files) {
  static int counter = 0;
  std::error_code ec;
  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path root =
      std::filesystem::temp_directory_path(ec) /
      ("musicxx_ext_pkg_" + std::to_string(stamp) + "_" +
       std::to_string(++counter));
  const std::filesystem::path dir = root / id;
  std::filesystem::create_directories(dir, ec);
  writeTextFile(dir / "plugin.yaml", manifest);
  for (const std::string &relative : files) {
    writeTextFile(dir / relative, "not a real library");
  }
  return root;
}

/// JSON 字符串转义（路径里有反斜杠时也能安全放进 JSON 文本）
std::string jsonEscape(const std::string &text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char ch : text) {
    if (ch == '\\' || ch == '"') {
      out.push_back('\\');
    }
    out.push_back(ch);
  }
  return out;
}

/// 切换插件框架的用户插件目录（扫描入口）并返回扫描结果里某个插件的片段
std::string scanPluginItem(MusicxxExternPluginHost *host,
                           const std::string &pluginRoot,
                           const std::string &id) {
  const std::string cfg =
      "{\"userPluginDir\":\"" + jsonEscape(pluginRoot) + "\"}";
  MusicxxExternPluginString cfgLog{};
  t_set_config(host, viewP(cfg), &cfgLog);
  freeStr(cfgLog);
  MusicxxExternPluginString out{};
  MusicxxExternPluginString scanLog{};
  const auto rc = t_plugin_scan(host, &out, &scanLog);
  freeStr(scanLog);
  const std::string json = take(out);
  if (rc != PLUGINXX_RESULT_OK) {
    return {};
  }
  return scanItemOf(json, id);
}

/// 取选中分支的标签（`"target":"..."`）
std::string targetOf(const std::string &item) {
  return jsonStringField(item, "target");
}

/// 多目标打包（仿 APK 的 `lib/<系统>-<架构>/`）：分支选择与加载
///
/// 用例覆盖：选择优先级（系统+架构 → 系统 → 架构 → 通用 → 根目录）、
/// 不匹配分支的报错、未知标签目录被忽略、`targets_dir` 改名、
/// "分支里唯一的动态库文件"自动识别、真实加载走分支。
void testPluginTargetLayout(MusicxxExternPluginHost *host,
                            const std::string &pluginsRoot) {
  const std::string os = testPlatform();
  const std::string arch = testArch();
  const std::string otherOs = (os == "windows") ? "linux" : "windows";
  const std::string exactTag = os + "-" + arch;
  const std::string osTag = os;
  const std::string archTag = arch;
  const std::string otherTag = otherOs + "-" + arch;
  const std::string libName = testLibFileName("tgt_multi");
  const std::string manifest = "name: tgt_multi\n"
                               "entry: tgt_multi.so\n"
                               "kind: native\n"
                               "version: 0.1.0\n";

  // 1) 选择优先级：包内同时放 不匹配 / 系统+架构 / 只系统 / 只架构 / 通用，
  //    以及一个不是分支的目录与根目录库文件
  const std::filesystem::path root = makeTempPluginRoot(
      "tgt_multi", manifest,
      {"lib/" + otherTag + "/" + libName, "lib/" + exactTag + "/" + libName,
       "lib/" + osTag + "/" + libName, "lib/" + archTag + "/" + libName,
       "lib/universal/" + libName, "lib/notatag/" + libName,
       testLibFileName("tgt_multi")});
  const std::filesystem::path pkg = root / "tgt_multi";
  {
    const std::string item = scanPluginItem(host, root.string(), "tgt_multi");
    checkText(!item.empty(), "多目标包被扫描到");
    checkText(item.find("\"supported\":true") != std::string::npos,
              "多目标包（有匹配分支）判定为可用");
    checkText(targetOf(item) == exactTag,
              "优先选系统+架构分支 (" + exactTag + ")");
    checkText(item.find("\"targetEntry\":\"lib/" + exactTag + "/" + libName +
                        "\"") != std::string::npos,
              "targetEntry 指向分支内的库文件");
    checkText(item.find("\"targets\":[") != std::string::npos &&
                  item.find("\"notatag\"") == std::string::npos,
              "unknown 标签目录不算分支");
    checkText(item.find("\"" + otherTag + "\"") != std::string::npos &&
                  item.find("\"match\":-1") != std::string::npos,
              "不匹配当前环境的分支照常列出（match=-1）");
  }
  // 2) 逐个删掉更高优先级的分支，验证剩下的按 只系统 → 只架构 → 通用 →
  //    根目录 的顺序接管
  removePath(pkg / "lib" / exactTag);
  {
    const std::string item = scanPluginItem(host, root.string(), "tgt_multi");
    checkText(targetOf(item) == osTag, "去掉系统+架构分支后选只系统分支");
  }
  removePath(pkg / "lib" / osTag);
  {
    const std::string item = scanPluginItem(host, root.string(), "tgt_multi");
    checkText(targetOf(item) == archTag, "再去掉只系统分支后选只架构分支");
  }
  removePath(pkg / "lib" / archTag);
  {
    const std::string item = scanPluginItem(host, root.string(), "tgt_multi");
    checkText(targetOf(item) == "universal", "再去掉只架构分支后选通用分支");
  }
  removePath(pkg / "lib" / "universal");
  {
    const std::string item = scanPluginItem(host, root.string(), "tgt_multi");
    checkText(targetOf(item).empty() &&
                  item.find("\"supported\":true") != std::string::npos,
              "分支都不匹配时回退插件根目录的库文件");
  }
  removePath(pkg / testLibFileName("tgt_multi"));
  {
    const std::string item = scanPluginItem(host, root.string(), "tgt_multi");
    checkText(item.find("\"supported\":false") != std::string::npos,
              "包内没有匹配分支且根目录也没有库文件时判为不可用");
    checkText(item.find("包内没有匹配当前系统/架构的分支") != std::string::npos,
              "不可用原因写明没有匹配的分支, 并列出包内分支");
  }
  removePath(root);

  // 3) 清单 targets_dir 改名：lib/ 里的分支不参与，改用 targets/
  {
    const std::filesystem::path customRoot = makeTempPluginRoot(
        "tgt_custom",
        "name: tgt_custom\nentry: tgt_custom.so\nkind: native\n"
        "targets_dir: targets\n",
        {"targets/" + exactTag + "/" + libName,
         "lib/" + otherTag + "/" + libName});
    const std::string item =
        scanPluginItem(host, customRoot.string(), "tgt_custom");
    checkText(targetOf(item) == exactTag,
              "targets_dir 指定的分支目录按清单生效");
    checkText(item.find("\"targetEntry\":\"targets/" + exactTag + "/") !=
                  std::string::npos,
              "targetEntry 指向 targets_dir 里的库文件");
    removePath(customRoot);
  }

  // 4) 分支里的库文件名与清单 entry 不同：目录里只有一个动态库文件时直接用它
  {
    const std::string customLib = testLibFileName("whatever_name");
    const std::filesystem::path uniRoot = makeTempPluginRoot(
        "tgt_uni", "name: tgt_uni\nentry: tgt_uni.so\nkind: native\n",
        {"lib/universal/" + customLib});
    const std::string item = scanPluginItem(host, uniRoot.string(), "tgt_uni");
    checkText(targetOf(item) == "universal", "只有通用分支时选通用分支");
    checkText(item.find("\"targetEntry\":\"lib/universal/" + customLib +
                        "\"") != std::string::npos,
              "分支里唯一的动态库文件被自动识别（名字不必与 entry 相同）");
    removePath(uniRoot);
  }

  // 5) 分支目录存在但没有库文件：报"分支里没有库文件"
  {
    const std::filesystem::path emptyRoot = makeTempPluginRoot(
        "tgt_empty", "name: tgt_empty\nentry: tgt_empty.so\nkind: native\n",
        {"lib/" + exactTag + "/readme.txt"});
    const std::string item =
        scanPluginItem(host, emptyRoot.string(), "tgt_empty");
    checkText(item.find("\"supported\":false") != std::string::npos,
              "分支里没有库文件时判为不可用");
    checkText(item.find("没有可加载的库文件") != std::string::npos,
              "不可用原因说明分支里没有库文件");
    removePath(emptyRoot);
  }

  // 6) 真实加载：把示例动态库插件复制到分支目录里，验证加载走的也是分支
  //    （加载后立即卸载，避免与后面的常规加载重名）
  {
    const std::filesystem::path nativeDir =
        std::filesystem::path(pluginsRoot) / "example_native";
    const std::filesystem::path nativeLib =
        nativeDir / testLibFileName("example_native");
    const std::filesystem::path nativeManifest = nativeDir / "plugin.yaml";
    if (!std::filesystem::exists(nativeLib) ||
        !std::filesystem::exists(nativeManifest)) {
      checkText(false, "示例动态库插件缺失 (跳过真实加载用例)");
      return;
    }
    // 清单原样复制（entry 仍是按 Linux 写的
    // example_native.so），库文件放进分支目录
    std::error_code ec;
    std::ifstream in(nativeManifest, std::ios::binary);
    std::ostringstream oss;
    oss << in.rdbuf();
    const std::filesystem::path loadRoot =
        makeTempPluginRoot("example_native", oss.str(), {});
    const std::filesystem::path branchDir =
        loadRoot / "example_native" / "lib" / exactTag;
    std::filesystem::create_directories(branchDir, ec);
    std::filesystem::copy_file(nativeLib, branchDir / nativeLib.filename(), ec);

    const std::string item =
        scanPluginItem(host, loadRoot.string(), "example_native");
    checkText(targetOf(item) == exactTag, "示例插件放进分支目录后按分支选中");

    MusicxxExternPluginString loadLog{};
    const auto loadRc = t_plugin_load_sync(
        host, viewCP("example_native"), viewCP("{}"), 10000, &loadLog);
    if (loadRc != PLUGINXX_RESULT_OK) {
      std::printf("  [info] 分支加载失败 log=%s\n", take(loadLog).c_str());
      freeStr(loadLog);
    } else {
      freeStr(loadLog);
    }
    checkText(loadRc == PLUGINXX_RESULT_OK, "分支里的库文件能真正加载");
    {
      MusicxxExternPluginString listOut{};
      MusicxxExternPluginString listLog{};
      t_plugin_list(host, &listOut, &listLog);
      freeStr(listLog);
      const std::string loadedItem =
          scanItemOf(take(listOut), "example_native");
      checkText(targetOf(loadedItem) == exactTag,
                "已加载插件报告自己用的是哪个分支");
      checkText(loadedItem.find("\"targetEntry\":\"lib/" + exactTag + "/") !=
                    std::string::npos,
                "已加载插件报告分支里的库文件");
    }
    {
      MusicxxExternPluginString unloadLog{};
      t_plugin_unload(host, viewCP("example_native"),
                                          &unloadLog);
      freeStr(unloadLog);
    }
    removePath(loadRoot);
    // 还原插件目录（后面的用例还要用常规目录里的插件）
    scanPluginItem(host, pluginsRoot, "example_native");
  }
}

/// 插件目录静态判定（`plugin_inspect`）：安装预检入口，与扫描/加载同源
///
/// 用例覆盖：清单字段、按当前或**指定**目标环境选分支、没有匹配分支的原因、
/// 清单 `platforms` / `arch` 的声明（含别名与 32 位 ARM）、JS 插件的脚本清单、
/// 目录不存在，以及"inspect 与 scan 对同一目录结论一致"。
void testPluginInspect(MusicxxExternPluginHost *host,
                       const std::string &pluginsRoot) {
  auto inspect = [&](const std::string &dir, const std::string &os,
                     const std::string &arch) -> std::string {
    MusicxxExternPluginString out{};
    MusicxxExternPluginString log{};
    const auto rc = t_plugin_inspect(
        host, viewP(dir), viewP(os), viewP(arch), &out, &log);
    freeStr(log);
    const std::string json = take(out);
    return rc == PLUGINXX_RESULT_OK ? json : std::string{};
  };

  const std::string os = testPlatform();
  const std::string arch = testArch();
  const std::string exactTag = os + "-" + arch;
  const std::string libName = testLibFileName("tgt_inspect");
  const std::string manifest = "name: tgt_inspect\n"
                               "entry: tgt_inspect.so\n"
                               "kind: native\n"
                               "version: 0.2.0\n"
                               "author: \"probe\"\n";
  const std::filesystem::path root = makeTempPluginRoot(
      "tgt_inspect", manifest,
      {"lib/" + exactTag + "/" + libName, "lib/linux-arm64/" + libName});
  const std::filesystem::path pkg = root / "tgt_inspect";

  // 1) 用插件框架当前环境判定（os/arch 传空）
  const std::string info = inspect(pkg.string(), "", "");
  checkText(!info.empty(), "plugin_inspect 有出参");
  checkText(info.find("\"valid\":true") != std::string::npos,
            "plugin_inspect 判定清单可解析");
  checkText(jsonStringField(info, "id") == "tgt_inspect",
            "plugin_inspect 给出插件 id");
  checkText(jsonStringField(info, "kind") == "native",
            "plugin_inspect 给出形态");
  checkText(info.find("\"kindKnown\":true") != std::string::npos &&
                info.find("\"kindRequiresPluginAbi\":true") !=
                    std::string::npos,
            "plugin_inspect 报出形态事实：已登记且按 C ABI 编译");
  checkText(jsonIntField(info, "kindInterfaceVersion") ==
                    jsonIntField(info, "kindMinInterfaceVersion") &&
                !jsonIntField(info, "kindInterfaceVersion").empty(),
            "动态库形态的接口代次两处同值");
  g_pluginAbiVersion = jsonIntField(info, "kindInterfaceVersion");
  checkText(jsonStringField(info, "kindProviderId").empty() &&
                jsonStringField(info, "kindValidateCapability").empty(),
            "内置形态没有运行器提供者与校验能力");
  checkText(jsonStringField(info, "version") == "0.2.0",
            "plugin_inspect 给出清单字段");
  checkText(info.find("\"supported\":true") != std::string::npos,
            "plugin_inspect 判定为可用");
  checkText(jsonStringField(info, "target") == exactTag,
            "plugin_inspect 按当前系统/架构选中分支");

  // 2) 与扫描同源：同一个目录，两边报的分支与库文件必须一致
  {
    const std::string scanItem =
        scanPluginItem(host, root.string(), "tgt_inspect");
    checkText(jsonStringField(scanItem, "target") ==
                  jsonStringField(info, "target"),
              "plugin_inspect 与扫描选中的分支一致");
    checkText(jsonStringField(scanItem, "targetEntry") ==
                  jsonStringField(info, "targetEntry"),
              "plugin_inspect 与扫描的库文件一致");
    checkText(jsonStringField(scanItem, "reason") ==
                  jsonStringField(info, "reason"),
              "plugin_inspect 与扫描的可用性原因一致（均没有原因）");
  }

  // 3) 指定目标环境：按传进来的 os/arch 选分支（不需要在那种机器上）
  checkText(jsonStringField(inspect(pkg.string(), "linux", "arm64"),
                            "target") == "linux-arm64",
            "plugin_inspect 按指定目标环境选分支");

  // 4) 目标环境没有匹配分支：标明不可用并给出原因
  //
  // 查询的环境必须**肯定不在**这个临时包里（包里只有当前环境分支与
  // linux-arm64）： 写死 "macos/arm64" 在 macOS
  // 上跑测试时会命中"当前环境分支"（用例本身依赖宿主平台）。
  const std::string missingOs = (os == "macos") ? "windows" : "macos";
  const std::string missingArch = (missingOs == "windows") ? "x64" : "arm64";
  {
    const std::string missing = inspect(pkg.string(), missingOs, missingArch);
    checkText(missing.find("\"supported\":false") != std::string::npos,
              "plugin_inspect 对没有匹配分支的环境判为不可用");
    checkText(missing.find("没有匹配当前系统/架构的分支") != std::string::npos,
              "plugin_inspect 给出没有匹配分支的原因");
  }
  removePath(root);

  // 5) 清单声明了 platforms / arch：按目标环境比较（传空 = 插件框架当前环境）
  //
  // 回归：主程序安装预检（`host.plugins.inspect(dir)`）**不传**
  // os/arch，早期实现 拿空串去比对清单里的 platforms，于是声明了 platforms
  // 的插件一律被判成 "当前平台不在清单声明内" ——
  // 这类插件永远装不上（示例插件几乎都声明了 platforms）。
  {
    const std::string libName2 = testLibFileName("tgt_declared");
    const std::filesystem::path declaredRoot = makeTempPluginRoot(
        "tgt_declared",
        "name: tgt_declared\nentry: tgt_declared.so\nkind: native\n"
        "platforms: [" +
            testOsAlias() + "]\narch: [" + testArchAlias() + "]\n",
        {libName2});
    const std::string pkgDir = (declaredRoot / "tgt_declared").string();

    const std::string hostEnv = inspect(pkgDir, "", "");
    checkText(
        hostEnv.find("\"supported\":true") != std::string::npos,
        "清单声明了 platforms / arch 时，按插件框架当前环境（传空）判定为可用");
    checkText(jsonStringField(hostEnv, "reason").empty(),
              "按插件框架当前环境判定时没有不可用原因");

    const std::string scanItem =
        scanPluginItem(host, declaredRoot.string(), "tgt_declared");
    checkText(scanItem.find("\"supported\":true") != std::string::npos,
              "扫描对声明了别名平台/架构的插件同样判为可用");

    // 换个平台：明确报"当前平台不在清单声明内"，并带上当前环境与声明内容
    const std::string otherOs =
        (testPlatform() == "windows") ? "linux" : "windows";
    const std::string mismatch = inspect(pkgDir, otherOs, testArch());
    checkText(mismatch.find("\"supported\":false") != std::string::npos,
              "换成别的平台后判为不可用");
    checkText(mismatch.find("当前平台不在清单声明内") != std::string::npos &&
                  mismatch.find(otherOs) != std::string::npos,
              "不可用原因写明平台不在声明内，并给出目标环境");
    removePath(declaredRoot);
  }

  // 6) 32 位 ARM：`android-armeabi-v7a` 分支与清单里的别名写法（arm / armv7 /
  // armeabi-v7a）
  //
  // 本用例在 x64 机上跑：目标环境由参数注入（`plugin_inspect` 的 os/arch
  // 参数就是为这种 验证准备的）。编译目标本身的架构判定（32 位 ARM 上报
  // armv7）由 `debug_info` 的 `arch` 断言覆盖（见 main
  // 里的"插件框架上报的架构"）。
  {
    const std::string armLib = testLibFileName("tgt_armv7");
    const std::filesystem::path armRoot = makeTempPluginRoot(
        "tgt_armv7",
        "name: tgt_armv7\nentry: tgt_armv7.so\nkind: native\n"
        "platforms: [android, linux]\narch: [arm]\n",
        {"lib/android-armeabi-v7a/" + armLib, "lib/android-arm64-v8a/" + armLib,
         "lib/linux-armv7/" + armLib});
    const std::string armPkg = (armRoot / "tgt_armv7").string();

    const std::string androidArm = inspect(armPkg, "android", "armv7");
    checkText(androidArm.find("\"supported\":true") != std::string::npos,
              "32 位 ARM（armv7）判定为可用");
    checkText(jsonStringField(androidArm, "target") == "android-armeabi-v7a",
              "选中 32 位 ARM 分支 android-armeabi-v7a");
    checkText(androidArm.find("\"arch\":[\"arm\"]") != std::string::npos,
              "清单 arch 写别名 arm 照常解析");

    // Linux armhf：同一份包按 linux-armv7 分支
    const std::string linuxArm = inspect(armPkg, "linux", "armv7");
    checkText(jsonStringField(linuxArm, "target") == "linux-armv7",
              "Linux 32 位 ARM 选 linux-armv7 分支");

    // 换个架构（arm64）：清单 arch 只声明了 32 位 ARM，应判为不可用并说明原因
    const std::string arm64Info = inspect(armPkg, "android", "arm64");
    checkText(arm64Info.find("\"supported\":false") != std::string::npos,
              "只声明 32 位 ARM 的插件在 arm64 环境判为不可用");
    checkText(arm64Info.find("当前架构不在清单声明内") != std::string::npos &&
                  arm64Info.find("当前 arm64") != std::string::npos,
              "架构不匹配的原因写明当前架构与清单声明");
    removePath(armRoot);
  }

  // 7) JS 插件：形态与脚本清单；动态库分支字段不出现
  {
    const std::filesystem::path jsDir =
        std::filesystem::path(pluginsRoot) / "example_js";
    const std::string jsInfo = inspect(jsDir.string(), "", "");
    checkText(jsonStringField(jsInfo, "kind") == "js",
              "plugin_inspect 识别 JS 插件");
    checkText(jsInfo.find("\"scripts\":[\"") != std::string::npos,
              "plugin_inspect 给出 JS 插件的脚本清单");
    // 主程序靠这一项决定要不要卡 api_version（不自己按形态名写分支）
    checkText(jsInfo.find("\"kindRequiresPluginAbi\":false") !=
                  std::string::npos,
              "JS 形态报出不按 C ABI 编译");
    // JS 有自己的接口代次对：广播只剩读与订阅后当前代次与兼容下限都是 3
    checkText(jsonIntField(jsInfo, "kindInterfaceVersion") == "3" &&
                  jsonIntField(jsInfo, "kindMinInterfaceVersion") == "3",
              "JS 形态的接口代次对是 3/3 (删接口后下限一起抬)");
    checkText(jsInfo.find("\"targetsDir\"") == std::string::npos &&
                  jsInfo.find("\"targetEntry\"") == std::string::npos,
              "JS 插件不出现动态库分支字段");
    // 示例插件都声明了
    // platforms：按插件框架当前环境判定必须是可用（安装预检的调用形状）
    checkText(jsInfo.find("\"supported\":true") != std::string::npos,
              "声明了 platforms 的 JS 示例插件按插件框架当前环境判定为可用");
    // 示例 JS 插件清单写 3：与自己的形态代次一致（不跟着插件 ABI 代次抬高）
    checkText(jsonIntField(jsInfo, "apiVersion") == "3",
              "JS 示例插件清单里的 api_version 为 3 (与插件 ABI 代次无关)");
  }

  // 8) 形态自己的接口下限也要生效：JS 接口下限是 3，清单写 0 就跑不了
  //     （与动态库插件判 7 是同一套规则，只是下限来自形态注册表）
  {
    const std::filesystem::path jsLowRoot = makeTempPluginRoot(
        "tgt_js_low",
        "name: tgt_js_low\nkind: js\napi_version: 0\nscripts: [plugin.js]\n",
        {"plugin.js"});
    const std::string lowInfo =
        inspect((jsLowRoot / "tgt_js_low").string(), "", "");
    checkText(lowInfo.find("\"supported\":false") != std::string::npos,
              "JS 插件声明 api_version 0 (低于形态下限) 判为不可用");
    checkText(lowInfo.find("最低 3") != std::string::npos,
              "不可用原因给出该运行环境的最低接口代次");
    removePath(jsLowRoot);
  }

  // 9) 没人声明的形态：判为不可用，并说清要先装提供它的运行器插件
  {
    const std::filesystem::path kindRoot = makeTempPluginRoot(
        "tgt_kind", "name: tgt_kind\nkind: python\nentry: main.py\n",
        {"main.py"});
    const std::string kindInfo =
        inspect((kindRoot / "tgt_kind").string(), "", "");
    checkText(kindInfo.find("\"kindKnown\":false") != std::string::npos,
              "未登记的形态被标成 kindKnown=false");
    checkText(kindInfo.find("\"supported\":false") != std::string::npos,
              "未登记的形态判为不可用");
    checkText(kindInfo.find("没有运行器声明支持插件形态") != std::string::npos,
              "未登记的形态说明要先安装提供它的插件");
    removePath(kindRoot);
  }

  // 10) 目录不存在：valid=false + 可读原因（调用本身仍然成功）
  {
    const std::filesystem::path missing =
        std::filesystem::path(pluginsRoot) / "definitely_missing_plugin";
    const std::string missingInfo = inspect(missing.string(), "", "");
    checkText(!missingInfo.empty(), "plugin_inspect 对不存在的目录也有出参");
    checkText(missingInfo.find("\"valid\":false") != std::string::npos,
              "plugin_inspect 对不存在的目录判 valid=false");
    checkText(missingInfo.find("插件目录不存在") != std::string::npos,
              "plugin_inspect 说明目录不存在");
  }

  // 还原插件目录（本用例中途把扫描入口指向了临时包，后面的用例要用常规目录里的插件）
  scanPluginItem(host, pluginsRoot, "example_native");
}

} // namespace

/// 把框架内部日志打到 stderr (只在 `MUSICXX_TEST_LOG=1` 时注册)
class TestStderrLogSink final : public utilxx_base::ThreadedLogSink {
protected:
  void onLog(const utilxx_base::LogEntry &entry) override {
    std::fprintf(stderr, "[%s] %s\n", entry.module.c_str(),
                 entry.message.c_str());
  }
};

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IONBF, 0); // debug: unbuffered stdout

  // 排查加载 / 生命周期问题时看框架内部日志: `MUSICXX_TEST_LOG=1` 时把
  // utilxx_base 的日志打到 stderr。默认不注册 sink, 框架日志被丢弃。
  std::shared_ptr<TestStderrLogSink> debugLogSink;
  if (const char *logFlag = std::getenv("MUSICXX_TEST_LOG");
      logFlag != nullptr && std::strcmp(logFlag, "1") == 0) {
    debugLogSink = std::make_shared<TestStderrLogSink>();
    utilxx_base::LogDispatcher::instance().addSink(debugLogSink);
  }
  const std::string pluginDir = (argc > 1) ? argv[1] : "";
  std::printf("musicxx_extern_plugin native test (pluginDir=%s)\n",
              pluginDir.c_str());

  check(musicxx_extern_plugin_api_version() ==
            MUSICXX_EXTERN_PLUGIN_API_VERSION,
        "api_version 匹配");
  check((musicxx_extern_plugin_feature_bits() &
         MUSICXX_EXTERN_PLUGIN_FEATURE_NET) != 0,
        "feature_bits 包含插件网络能力 (musicxx.net)");

  // 系统错误文本的编码: 系统给的是本机 ANSI 编码 (中文 Windows 上是 GBK),
  // 框架按 UTF-8 交给插件。这里只校验 osTextToUtf8 的两条性质: UTF-8 / ASCII
  // 原样返回; 系统文本转一次之后保持稳定 (说明转出来的确实是 UTF-8,
  // 不会转第二次又变一次)
  {
    using musicxx::extern_plugin::osTextToUtf8;
    const std::string utf8Text = "外部插件: 连接失败";
    checkText(osTextToUtf8(utf8Text) == utf8Text, "UTF-8 文本原样返回");
    checkText(osTextToUtf8("connection refused") == "connection refused",
              "ASCII 文本原样返回");
    const std::string systemText =
        std::error_code(10061, std::system_category()).message();
    const std::string converted = osTextToUtf8(systemText);
    checkText(osTextToUtf8(converted) == converted,
              "系统错误文本转成 UTF-8 后保持稳定");
    std::printf("  [info] 系统错误文本: %s\n", converted.c_str());
#if defined(_WIN32)
    // 中文 Windows 上 Boost.System 给的「连接被拒绝」是 `FormatMessageA`
    // 的结果， 也就是 ANSI（936 / GBK）字节。直接拿那串字节验证转换；代码页不是
    // 936 的 机器上系统文本本来就不同，跳过这一条。
    if (936 == ::GetACP()) {
      const std::string ansiText =
          "\xD3\xC9\xD3\xDA\xC4\xBF\xB1\xEA\xBC\xC6\xCB\xE3\xBB\xFA\xBB\xFD"
          "\xBC\xAB\xBE\xDC\xBE\xF8\xA3\xAC\xCE\xDE\xB7\xA8\xC1\xAC\xBD\xD3"
          "\xA1\xA3";
      checkText(osTextToUtf8(ansiText) == "由于目标计算机积极拒绝，无法连接。",
                "ANSI(GBK) 系统文本转成 UTF-8");
    }
#endif
  }

  MusicxxExternPluginString log{};
  MusicxxExternPluginString version{};
  check(musicxx_extern_plugin_version(&version) == PLUGINXX_RESULT_OK &&
            version.size > 0,
        "version 可读");
  freeStr(version);

  MusicxxExternPluginHostConfig cfg{};
  cfg.struct_size = sizeof(MusicxxExternPluginHostConfig);
  const std::string appV = "0.0.0-test";
  /// 平台标识与编译目标一致（插件框架按它选多目标分支，见
  /// testPluginTargetLayout）
  const std::string plat = testPlatform();
  const std::string lang = "zh-cn";
  /// 客户端界面能力段 (正常情况下由界面描述层库按客户端实现生成)
  const std::string uiCaps =
      R"({"apiVersion":1,"kind":"gui","blocks":["Text","Row","Column"],)"
      R"("controls":["checkbox"],"gap":12})";
  cfg.app_version = view(appV);
  cfg.platform = view(plat);
  cfg.language = view(lang);
  cfg.user_plugin_dir = view(pluginDir);
  // 插件私有数据目录 (http.download 落盘到 <data_dir>/<插件id>/data/downloads)
  std::filesystem::path dataRootPath = std::filesystem::temp_directory_path() /
                                       "musicxx-extern-plugin-test-data";
  std::error_code dataRootEc;
  std::filesystem::create_directories(dataRootPath, dataRootEc);
  const std::string dataRoot = dataRootPath.string();
  cfg.data_dir = view(dataRoot);
  cfg.ui_capabilities = view(uiCaps);
  cfg.log_level = 2;
  cfg.flags = MUSICXX_EXTERN_PLUGIN_FLAG_DEBUG_OBSERVE_EVENTS;

  auto *host = musicxx_extern_plugin_host_create(&cfg, &log);
  if (!host) {
    std::printf("host_create failed: %s\n", take(log).c_str());
    return 2;
  }
  check(host != nullptr, "host_create");

  check(t_host_start(host, &log) ==
            PLUGINXX_RESULT_OK,
        "host_start");

  // 扫描
  MusicxxExternPluginString scan{};
  const auto scanRc = t_plugin_scan(host, &scan, &log);
  const std::string scanJson = take(scan);
  check(scanRc == PLUGINXX_RESULT_OK, "plugin_scan 成功");
  std::printf("  [info] scanJson=%s\n", scanJson.substr(0, 400).c_str());
  // 扫描应同时发现原生与 JS 示例插件 (JS 插件零编译)
  check(scanJson.find("example_native") != std::string::npos,
        "扫描发现 example_native");
  check(scanJson.find("example_js") != std::string::npos,
        "扫描发现 example_js (JS 插件)");
  check(scanJson.find("example_js_playing_bg") != std::string::npos,
        "扫描发现 example_js_playing_bg (JS 插件)");
  check(scanJson.find("\"kind\":\"js\"") != std::string::npos,
        "JS 插件的 kind 为 js");
  check(scanJson.find("本次运行未启用 JS 运行时") == std::string::npos,
        "JS 运行时可用 (未报未启用)");

  // 动态库插件的入口解析: 清单按 Linux 写 entry (`example_native.so`),
  // Windows/macOS 上由插件框架
  // 修正扩展名。扫描阶段的校验必须与加载阶段用同一套解析,
  // 否则会出现"扫描判缺库文件、 加载却能命中"的不一致
  // (本用例就是那次缺陷的回归防护)。
  {
    const std::string nativeItem = scanItemOf(scanJson, "example_native");
    check(!nativeItem.empty(), "扫描结果含 example_native 条目");
    check(nativeItem.find("\"supported\":true") != std::string::npos,
          "动态库插件按平台解析 entry 后仍为可用 (未被判库文件缺失)");
    check(nativeItem.find("库文件缺失") == std::string::npos,
          "动态库插件未被判为库文件缺失");
  }

  // 多目标打包（仿 APK 的 lib/<系统>-<架构>/）：分支选择与加载
  testPluginTargetLayout(host, pluginDir);

  // 插件目录静态判定（plugin_inspect）：安装预检用的入口，与扫描同源
  testPluginInspect(host, pluginDir);

  // 加载 (同步)
  MusicxxExternPluginString empty{};
  const auto loadRc = t_plugin_load_sync(
      host, viewCP("example_native"), viewCP(R"({"enabled":true})"), 10000,
      &log);
  if (loadRc != PLUGINXX_RESULT_OK) {
    std::printf("  [info] load rc=%d log=%s\n", loadRc, take(log).c_str());
  }
  check(loadRc == PLUGINXX_RESULT_OK, "plugin_load_sync 成功");

  // 能力处理器数量
  int32_t count = 0;
  check(t_capability_handler_count(
            host, viewCP("musicxx.player.beforePlaySong"), &count, &log) ==
                PLUGINXX_RESULT_OK &&
            count == 1,
        "beforePlaySong 处理器数为 1");
  check(t_capability_handler_count(
            host, viewCP("musicxx.player.completed"), &count, &log) ==
                PLUGINXX_RESULT_OK &&
            count == 0,
        "未注册能力的处理器数为 0");

  // 分阶段查询: 旧符号是全部阶段的总数, 新符号只看一个阶段
  {
    int32_t implCount = -1;
    int32_t beforeCount = -1;
    int32_t afterCount = -1;
    check(t_capability_handler_count_phase(
              host, viewCP("musicxx.player.beforePlaySong"),
              MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_IMPL, &implCount,
              &log) == PLUGINXX_RESULT_OK &&
              implCount == 1,
          "beforePlaySong 的 impl 阶段处理器数为 1");
    check(t_capability_handler_count_phase(
              host, viewCP("musicxx.player.beforePlaySong"),
              MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_BEFORE, &beforeCount,
              &log) == PLUGINXX_RESULT_OK &&
              beforeCount == 0,
          "beforePlaySong 的 before 阶段处理器数为 0");
    check(t_capability_handler_count_phase(
              host, viewCP("musicxx.player.beforePlaySong"),
              MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_AFTER, &afterCount,
              &log) == PLUGINXX_RESULT_OK &&
              afterCount == 0,
          "beforePlaySong 的 after 阶段处理器数为 0");
    check(t_capability_handler_count_phase(
              host, viewCP("musicxx.player.beforePlaySong"), 9, &afterCount,
              &log) == PLUGINXX_RESULT_ERR_ARG,
          "非法阶段编号被拒绝");
  }

  // 包装点 `musicxx.lyric.load` 上的阶段示例: 前链改输入 / 拦下, 后链改输出 / 丢弃
  //
  // 示例只在载荷带 `"example_native_demo":true` 时动手, 其余调用原样返回 ——
  // 装上这个示例插件的用户不会因此丢歌词。(两段示例原先在 `example_native_multi`
  // 里无条件改写结果, 已迁移到这里并加上标记。)
  {
    int32_t lyricBefore = -1;
    int32_t lyricAfter = -1;
    check(t_capability_handler_count_phase(
              host, viewCP("musicxx.lyric.load"),
              MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_BEFORE, &lyricBefore,
              &log) == PLUGINXX_RESULT_OK &&
              lyricBefore == 1,
          "包装点声明了 before 阶段且已有处理器");
    check(t_capability_handler_count_phase(
              host, viewCP("musicxx.lyric.load"),
              MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_AFTER, &lyricAfter,
              &log) == PLUGINXX_RESULT_OK &&
              lyricAfter == 1,
          "包装点声明了 after 阶段且已有处理器");

    // 旧符号是全阶段的总数（包装点两个阶段都已登记）
    int32_t lyricTotal = 0;
    check(t_capability_handler_count(
              host, viewCP("musicxx.lyric.load"), &lyricTotal, &log) ==
                  PLUGINXX_RESULT_OK &&
              lyricTotal == 2,
          "旧符号报告全部阶段的处理器总数");
    // 没有 impl 阶段: 包装点的实现固定是应用自己
    int32_t lyricImpl = -1;
    check(t_capability_handler_count_phase(
              host, viewCP("musicxx.lyric.load"),
              MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_IMPL, &lyricImpl,
              &log) == PLUGINXX_RESULT_OK &&
              lyricImpl == 0,
          "包装点没有 impl 阶段");

    // 不带演示标记: 处理器照常被调用, 但不改任何东西
    {
      MusicxxExternPluginString plainOut{};
      const auto rcPlain = t_capability_dispatch_phase(
          host, viewCP("musicxx.lyric.load"),
          MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_BEFORE,
          viewP(R"({"pid":"p1","cancel":true})"),
          MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC, 0, &plainOut, &log);
      const std::string plainResult = take(plainOut);
      check(rcPlain == PLUGINXX_RESULT_OK,
            "不带演示标记的前链派发成功");
      check(plainResult.find("\"action\"") == std::string::npos &&
                plainResult.find("\"patch\"") == std::string::npos,
            "不带演示标记时前链不改输入 (也不拦下)");

      MusicxxExternPluginString plainAfterOut{};
      const auto rcPlainAfter =
          t_capability_dispatch_phase(
              host, viewCP("musicxx.lyric.load"),
              MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_AFTER,
              viewP(R"({"pid":"p1","value":{"lyric":{"content":"x"}}})"),
              MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC, 0, &plainAfterOut,
              &log);
      const std::string plainAfterResult = take(plainAfterOut);
      check(rcPlainAfter == PLUGINXX_RESULT_OK,
            "不带演示标记的后链派发成功");
      check(plainAfterResult.find("\"items\":3") == std::string::npos,
            "不带演示标记时后链不改输出 (真实歌词结果不被换成固定值)");
    }

    // 前链: 串行、正序, 累积的 patch 交回调用方
    {
      MusicxxExternPluginString beforeOut{};
      const auto rcBefore = t_capability_dispatch_phase(
          host, viewCP("musicxx.lyric.load"),
          MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_BEFORE,
          viewP(R"({"pid":"p1","example_native_demo":true})"),
          MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC, 0, &beforeOut, &log);
      const std::string beforeResult = take(beforeOut);
      check(rcBefore == PLUGINXX_RESULT_OK, "前链派发成功");
      check(beforeResult.find("\"patch\":{\"limit\":50}") != std::string::npos,
            "前链改过的输入交回调用方");
    }

    // 前链 cancel: 回执带 action=cancel
    {
      MusicxxExternPluginString cancelOut{};
      const auto rcCancel = t_capability_dispatch_phase(
          host, viewCP("musicxx.lyric.load"),
          MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_BEFORE,
          viewP(R"({"pid":"p1","example_native_demo":true,"cancel":true})"),
          MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC, 0, &cancelOut, &log);
      const std::string cancelResult = take(cancelOut);
      check(rcCancel == PLUGINXX_RESULT_OK &&
                cancelResult.find("\"action\":\"cancel\"") != std::string::npos,
            "前链 cancel 结束整条链");
    }

    // 后链: 值被替换
    {
      MusicxxExternPluginString afterOut{};
      const auto rcAfter = t_capability_dispatch_phase(
          host, viewCP("musicxx.lyric.load"),
          MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_AFTER,
          viewP(R"({"pid":"p1","example_native_demo":true,"value":{"items":1}})"),
          MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC, 0, &afterOut, &log);
      const std::string afterResult = take(afterOut);
      check(rcAfter == PLUGINXX_RESULT_OK &&
                afterResult.find("\"items\":3") != std::string::npos,
            "后链改掉了输出");
      check(afterResult.find("example_native") != std::string::npos,
            "后链拿到的是上一步的输出 (不是原始载荷)");
    }

    // 后链 drop: 回执带 action=drop
    {
      MusicxxExternPluginString dropOut{};
      const auto rcDrop = t_capability_dispatch_phase(
          host, viewCP("musicxx.lyric.load"),
          MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_AFTER,
          viewP(R"({"pid":"p1","example_native_demo":true,"drop":true,"value":{"items":1}})"),
          MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC, 0, &dropOut, &log);
      const std::string dropResult = take(dropOut);
      check(rcDrop == PLUGINXX_RESULT_OK &&
                dropResult.find("\"action\":\"drop\"") != std::string::npos,
            "后链 drop 结束整条链");
    }

    // 处理器列表能看出每个处理器在哪个阶段 (自省)
    {
      MusicxxExternPluginString handlersOut{};
      MusicxxExternPluginString handlersLog{};
      check(t_capability_handlers_phase(
                host, viewCP("musicxx.lyric.load"),
                MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_AFTER, &handlersOut,
                &handlersLog) == PLUGINXX_RESULT_OK,
            "分阶段处理器列表可读");
      const std::string handlers = take(handlersOut);
      check(handlers.find("\"phase\":\"after\"") != std::string::npos,
            "分阶段处理器列表带 phase");
    }
  }

  // capability_info 带 phases / phaseCount; 处理器列表每项带 phase
  {
    MusicxxExternPluginString out{};
    MusicxxExternPluginString capabilityInfoLog{};
    check(t_capability_handlers(
              host, viewCP("musicxx.player.beforePlaySong"), &out,
              &capabilityInfoLog) == PLUGINXX_RESULT_OK,
          "capability_handlers 可调用");
    const std::string handlers = take(out);
    check(handlers.find("\"phase\":\"impl\"") != std::string::npos,
          "处理器列表带 phase 字段");

    MusicxxExternPluginString phaseOut{};
    MusicxxExternPluginString phaseLog{};
    check(t_capability_handlers_phase(
              host, viewCP("musicxx.player.beforePlaySong"),
              MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_AFTER, &phaseOut,
              &phaseLog) == PLUGINXX_RESULT_OK,
          "capability_handlers_phase 可调用");
    const std::string afterHandlers = take(phaseOut);
    check(afterHandlers.find("\"count\":0") != std::string::npos,
          "after 阶段当前没有处理器");
  }

  // 分阶段派发: impl 与旧符号一致; 没有处理器的阶段不派发
  {
    MusicxxExternPluginString out{};
    const std::string payload =
        R"({"sid":"s0","song":{"name":"广告插播 - 阶段测试"}})";
    const auto rc = t_capability_dispatch_phase(
        host, viewCP("musicxx.player.beforePlaySong"),
        MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_IMPL, viewP(payload),
        MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC, 200, &out, &log);
    const std::string result = take(out);
    check(rc == PLUGINXX_RESULT_OK,
          "capability_dispatch_phase(impl) 返回成功");
    check(result.find("\"skip\"") != std::string::npos,
          "impl 阶段与旧符号行为一致 (skip)");

    MusicxxExternPluginString beforeOut{};
    const auto rcBefore = t_capability_dispatch_phase(
        host, viewCP("musicxx.player.beforePlaySong"),
        MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_BEFORE, viewP(payload),
        MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC, 200, &beforeOut, &log);
    const std::string beforeResult = take(beforeOut);
    check(rcBefore == PLUGINXX_RESULT_OK &&
              beforeResult.find("\"handled\":false") != std::string::npos,
          "没有 before 处理器时不派发");

    MusicxxExternPluginString badOut{};
    check(t_capability_dispatch_phase(
              host, viewCP("musicxx.player.beforePlaySong"), 7, viewP(payload),
              MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC, 200, &badOut,
              &log) == PLUGINXX_RESULT_ERR_ARG,
          "非法阶段派发被拒绝");
    freeStr(badOut);
  }

  // 事件泵: 应能取到 host.ready / plugin.loaded / capability.changed
  {
    const PolledEvents pumped = pollEvents(host, 200);
    const std::string eventsJson = pumped.json;
    check(pumped.rc == PLUGINXX_RESULT_OK ||
              pumped.rc == PLUGINXX_RESULT_ERR_TIMEOUT,
          "poll_events 可调用");
    check(eventsJson.find("musicxx.plugin.loaded") != std::string::npos ||
              eventsJson.find("musicxx.host.ready") != std::string::npos,
          "事件包含 host.ready / plugin.loaded");
    // UI 项注册/更新会推送 musicxx.ui.changed; 提示走动作请求通道
    // (musicxx.ui.toast)
    check(eventsJson.find("musicxx.ui.changed") != std::string::npos,
          "UI 项变化推送 musicxx.ui.changed 事件");
    check(eventsJson.find("musicxx.ui.toast") != std::string::npos,
          "提示走动作请求通道 (musicxx.ui.toast)");
  }

  // decision 能力: 广告曲目 → cancel
  {
    MusicxxExternPluginString out{};
    const std::string payload =
        R"({"sid":"s1","song":{"name":"广告插播 - 测试","artist":"x"},"mode":"normal"})";
    const auto rc = t_capability_dispatch(
        host, viewCP("musicxx.player.beforePlaySong"), viewP(payload),
        MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC, 200, &out, &log);
    const std::string result = take(out);
    check(rc == PLUGINXX_RESULT_OK, "capability_dispatch(sync) 返回成功");
    check(result.find("\"skip\"") != std::string::npos,
          "广告曲目被裁决为 skip");
  }

  // decision 能力: 普通曲目 → 无裁决
  {
    MusicxxExternPluginString out{};
    const std::string payload =
        R"({"sid":"s2","song":{"name":"普通歌曲","artist":"y"}})";
    const auto rc = t_capability_dispatch(
        host, viewCP("musicxx.player.beforePlaySong"), viewP(payload),
        MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC, 200, &out, &log);
    const std::string result = take(out);
    check(rc == PLUGINXX_RESULT_OK,
          "capability_dispatch(sync) 普通曲目成功");
    check(result.find("\"skip\"") == std::string::npos, "普通曲目无 skip 裁决");
  }

  // 通知型能力: 同样是“提交 + 一条结果”, 只是回执里没有裁决
  {
    MusicxxExternPluginString out{};
    const std::string payload = R"({"sid":"s3","song":{"name":"观察目标"}})";
    const auto rc = t_capability_dispatch(
        host, viewCP("musicxx.song.changed"), viewP(payload),
        MUSICXX_EXTERN_PLUGIN_DISPATCH_ASYNC, 0, &out, &log);
    const std::string ack = take(out);
    check(rc == PLUGINXX_RESULT_OK, "通知型能力派发成功");
    check(ack.find("\"handled\"") != std::string::npos,
          "通知型能力回执带 handled");
  }

  // 裁决型能力: 结果就在同一次回执里 (协议里已经没有 callId / 独立结果事件)
  {
    MusicxxExternPluginString out{};
    const std::string payload =
        R"({"sid":"s4","song":{"name":"广告插播 - 测试","artist":"z"}})";
    const auto rc = t_capability_dispatch(
        host, viewCP("musicxx.player.beforePlaySong"), viewP(payload),
        MUSICXX_EXTERN_PLUGIN_DISPATCH_ASYNC, 200, &out, &log);
    const std::string ack = take(out);
    check(rc == PLUGINXX_RESULT_OK, "裁决型能力派发成功");
    check(ack.find("\"async\"") == std::string::npos,
          "回执里没有 async 标记 (只有一种形状)");
    check(ack.find("callId") == std::string::npos,
          "回执里没有 callId (结果不再走独立事件)");
    check(ack.find("\"skip\"") != std::string::npos,
          "裁决结果直接在回执里");

    // 连续两次派发各有独立回执
    MusicxxExternPluginString out2{};
    const auto rc2 = t_capability_dispatch(
        host, viewCP("musicxx.player.beforePlaySong"),
        viewCP(R"({"sid":"s5","song":{"name":"普通歌曲"}})"),
        MUSICXX_EXTERN_PLUGIN_DISPATCH_ASYNC, 200, &out2, &log);
    const std::string ack2 = take(out2);
    check(rc2 == PLUGINXX_RESULT_OK, "连续派发成功");
    check(ack2.find("\"skip\"") == std::string::npos, "普通曲目无 skip 裁决");
  }

  // 未知能力: 处理器数为 0 且派发不报错
  {
    MusicxxExternPluginString out{};
    const auto rc = t_capability_dispatch(
        host, viewCP("musicxx.plugin.unknownCapability"), viewCP("{}"),
        MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC, 50, &out, &log);
    const std::string result = take(out);
    check(rc == PLUGINXX_RESULT_OK, "未知能力派发不报错");
    check(result.find("\"handled\":false") != std::string::npos,
          "未知能力无处理器");
  }

  // 带当前值的广播
  {
    const std::string key = "musicxx.state.song";
    const std::string val = R"({"sid":"s1","name":"镜像歌曲"})";
    check(t_broadcast_publish(
              host, viewP(key), viewP(val), &log) == PLUGINXX_RESULT_OK,
          "broadcast_publish 成功");
    MusicxxExternPluginString listed{};
    check(t_plugin_list(host, &listed, &log) ==
              PLUGINXX_RESULT_OK,
          "plugin_list 成功");
    const std::string listJson = take(listed);
    check(listJson.find("example_native") != std::string::npos,
          "plugin_list 含已加载插件");
  }

  // ==================== 能力调用: 异步受理 + 结果事件 ====================
  //
  // 语义 (见 `musicxx_extern_plugin_api.h`):
  // - 受理成功 ⇒ 一定有终局: 结果 / 错误 / 插件卸载 (`plugin_unloaded`) /
  // 插件框架停机
  //   (`host_stopped`) 四条路都会推一条 `musicxx.request.result`;
  // - 受理失败 (没加载 / 没声明该能力 / 归属不对 / 未及时受理)
  // 只在返回值上体现, 不推事件;
  // - requestId 是唯一的配对依据。
  {
    // 1) 受理成功 + 结果事件 (requestId 配对)
    {
      const CapabilityCallOutcome outcome = callCapability(
          host, "example_native", "plugin.example_native.probe", "{}", 8000);
      check(outcome.acceptRc == PLUGINXX_RESULT_OK,
            "能力调用受理成功 (返回 requestId)");
      check(outcome.requestId > 0, "受理成功给出非零 requestId");
      check(outcome.gotResult, "结果事件按 requestId 回传");
      check(outcome.ok, "结果事件 ok=true");
      check(outcome.payload.find("\"callThread\"") != std::string::npos ||
                outcome.payload.find("\"stateLen\"") != std::string::npos,
            "结果事件带插件给出的结果");
      check(outcome.resultMs >= 0, "结果事件带耗时 (ms)");
    }

    // 2) 受理失败: 返回负错误码且不推事件
    {
      const CapabilityCallOutcome gone = callCapability(
          host, "example_native", "noSuchCapability", "{}", 1500);
      check(gone.acceptRc == PLUGINXX_RESULT_ERR_NOT_FOUND,
            "未声明能力: 受理阶段返回未找到");
      check(!gone.gotResult, "受理失败不推结果事件");
    }
  }

  // 统计与调试信息
  {
    MusicxxExternPluginString stats{};
    check(t_stats(host, nullptr, &stats, &log) ==
              PLUGINXX_RESULT_OK,
          "stats 可读");
    const std::string statsJson = take(stats);
    check(statsJson.find("example_native") != std::string::npos,
          "stats 含插件条目");

    // 事件计数与速率: 插件发布的 plugin.<id>.* 事件归属到该插件
    // (example_native 的 create 事务里发布过一次 plugin.example_native.hello)
    const auto at = statsJson.find(R"("id":"example_native")");
    const std::string entry =
        (at == std::string::npos) ? std::string{} : statsJson.substr(at, 600);
    const std::string eventsText = jsonIntField(entry, "events");
    check(!eventsText.empty() && std::stoll(eventsText) >= 1,
          "插件发布的事件计入统计 (plugin.<id>.* 归属)");
    check(entry.find("\"eventsPerSec\":") != std::string::npos,
          "stats 含事件速率字段");
    check(entry.find("\"eventsAvgPerSec\":") != std::string::npos,
          "stats 含平均事件速率字段");

    // 能力统计按处理器给出调用次数/失败次数/耗时 (只观测不限制:
    // 没有"暂停派发"这个概念)
    MusicxxExternPluginString statsOut{};
    check(t_capability_stats(host, &statsOut, &log) ==
              PLUGINXX_RESULT_OK,
          "capability_stats 可读");
    const std::string capabilityStatsJson = take(statsOut);
    check(capabilityStatsJson.find("\"failures\":") != std::string::npos,
          "capability_stats 含失败次数");
    check(capabilityStatsJson.find("\"paused\"") == std::string::npos,
          "capability_stats 不再含暂停派发标记");
    check(capabilityStatsJson.find("\"timeouts\"") == std::string::npos,
          "capability_stats 不再含超时统计");

    MusicxxExternPluginString info{};
    const auto infoRc = t_debug_info(host, &info, &log);
    const std::string infoJson = take(info);
    check(infoRc == PLUGINXX_RESULT_OK, "debug_info 可读");

    // 架构标识: 上报给插件的 `arch` 必须是规范名, 而且与"选分支/判清单 arch
    // 用的架构" 是同一个名字 (32 位 ARM 上曾经报 unknown, 于是 armeabi-v7a
    // 分支与 `arch: [arm]` 声明谁都匹配不上)
    const std::string reportedArch = jsonStringField(infoJson, "arch");
    checkText(reportedArch != "unknown",
              "插件框架上报的架构不是 unknown (32 位 ARM 应报 armv7)");
    checkText(reportedArch == testArch(),
              "插件框架上报的架构与用例期望一致 (" + reportedArch + ")");
  }

  // 插件能力调用 (Dart → 插件) + 线程模型 / 命名空间自检
  {
    // 先让插件框架发布一个测试主题 (插件已订阅), 再读探针 ——
    // 两者都投递到插件框架线程, 因此顺序确定 (先进先出),
    // 探针一定看到已处理的事件
    MusicxxExternPluginString pubLog{};
    const auto pubRc = t_broadcast_publish(
        host, viewCP("musicxx.test.ping"), viewCP(R"({"n":1})"), &pubLog);
    check(pubRc == PLUGINXX_RESULT_OK, "broadcast_publish 合法主题成功");
    freeStr(pubLog);

    MusicxxExternPluginString badPubLog{};
    const auto badPubRc = t_broadcast_publish(
        host, viewCP("foo.bar"), viewCP("{}"), &badPubLog);
    check(badPubRc == PLUGINXX_RESULT_ERR_PERMISSION,
          "broadcast_publish 非法主题被拒绝");
    freeStr(badPubLog);

    const CapabilityCallOutcome probeCall = callCapability(
        host, "example_native", "plugin.example_native.probe", "{}", 8000);
    const std::string probe = probeCall.payload;
    check(probeCall.acceptRc == PLUGINXX_RESULT_OK && probeCall.ok,
          "plugin_call(probe) 成功 (异步受理 + 结果事件)");
    std::printf("  [info] plugin_call requestId=%lld ms=%lld\n",
                static_cast<long long>(probeCall.requestId),
                static_cast<long long>(probeCall.resultMs));
    std::printf("  [info] probe=%s\n", probe.c_str());

    // ==================== 流结束 (广播表 v3) ====================
    //
    // 属主结束自己的一条流: 带结束回调的订阅 → 发布一个值 → 结束 → 再发布。
    // 期望: 订阅/发布/结束都是 0; 结束回调恰好一次且拿到 reason; 结束之后再发布
    // 返回 `PLUGINXX_RESULT_ERR_ENDED` (-5)。
    {
      const CapabilityCallOutcome endCall = callCapability(
          host, "example_native", "plugin.example_native.probe",
          R"({"streamEndCheck":1})", 8000);
      const std::string result = endCall.payload;
      check(endCall.acceptRc == PLUGINXX_RESULT_OK && endCall.ok,
            "plugin_call(probe, streamEndCheck) 成功");
      check(jsonIntField(result, "streamEndSubRc") == "0",
            "带结束回调订阅成功 (rc=0)");
      check(jsonIntField(result, "streamEndValueRc") == "0",
            "结束前发布值成功 (rc=0)");
      check(jsonIntField(result, "streamEndValues") == "1",
            "订阅在结束前收到发布的值");
      check(jsonIntField(result, "streamEndRc") == "0",
            "属主结束自己的流成功 (rc=0)");
      check(jsonIntField(result, "streamEndEnds") == "1",
            "结束回调恰好一次");
      check(jsonIntField(result, "streamEndReasonOk") == "1",
            "结束回调拿到 reason JSON");
      check(jsonIntField(result, "streamEndPublishAfterRc") ==
                std::to_string(PLUGINXX_RESULT_ERR_ENDED),
            "流结束之后再发布返回 ended");
    }

    // 插件框架按本插件的形态报接口代次：动态库插件看到插件 ABI 代次，
    // 与 JS 插件的 JS 接口代次（3）是两套编号。这里与 plugin_inspect 报的
    // 那一对相等（不写死数字：改插件 ABI 代次时不用改这两处断言）。
    check(!g_pluginAbiVersion.empty() &&
              jsonIntField(probe, "hostApiVersion") == g_pluginAbiVersion,
          "动态库插件看到的接口代次与 plugin_inspect 报的一致");

    // ==================== 声明式界面 (动态库插件): 声明项 / 内容 / 槽位
    // ====================
    //
    // 插件只做声明 (声明项 + 挂在槽位上的内容), 主程序负责渲染;
    // 这里校验注册 / 拒绝 / 覆盖 / 推送 / 槽位声明与撤销 / 快照 / 事件。
    {
      check(jsonIntField(probe, "uiHomeRc") == "0",
            "注册主页入口声明项成功 (rc=0)");
      check(jsonIntField(probe, "uiSongRc") == "0",
            "注册歌曲菜单项成功 (rc=0)");
      check(jsonIntField(probe, "uiForeignRc") == "-6",
            "他人命名空间的项被拒绝 (-6)");
      check(jsonIntField(probe, "uiBadTypeRc") == "-4",
            "未知槽位 / 声明项被拒绝 (-4)");
      check(jsonIntField(probe, "uiBadDataRc") == "-1",
            "缺少 title 的声明项被拒绝 (-1)");
      check(jsonIntField(probe, "uiPageBackgroundRc") == "0",
            "注册普通页面背景成功 (rc=0)");
      check(jsonIntField(probe, "uiPageBackgroundBadRoutesRc") == "-1",
            "页面背景 routes 不是字符串数组时被拒绝 (-1)");
      check(jsonIntField(probe, "uiNotifyRc") == "0",
            "通知受理成功 (fire-and-forget)");

      // ---- 内容 ----
      check(jsonIntField(probe, "uiContentRc") == "0", "注册内容成功 (rc=0)");
      check(jsonIntField(probe, "uiContentDupSlotRc") == "0",
            "同一槽位再注册一份内容成功 (多份并存)");
      check(jsonIntField(probe, "uiContentOverwriteRc") == "0",
            "同 id 再注册 = 覆盖 (rc=0)");
      check(jsonIntField(probe, "uiContentBadSlotRc") == "-4",
            "注册到未知槽位被拒绝 (-4)");
      check(jsonIntField(probe, "uiContentBadDepsRc") == "-1",
            "deps 不是字符串数组时被拒绝 (-1)");
      check(jsonIntField(probe, "uiContentUnregisterRc") == "0",
            "注销一份内容成功 (rc=0)");
      check(jsonIntField(probe, "uiContentPushedRc") == "0",
            "注册一份只从 push 取内容的那份成功 (rc=0)");
      {
        const std::string entries = jsonRawField(probe, "uiEntries");
        check(entries.find("plugin.example_native.card") != std::string::npos,
              "声明项表含主页入口");

        const std::string contents = jsonRawField(probe, "uiContents");
        check(contents.find("plugin.example_native.playingBg") !=
                  std::string::npos,
              "内容表含播放页背景样式");
        check(contents.find("musicxx.ui.playing.background") !=
                  std::string::npos,
              "播放页背景的槽位是 musicxx.ui.playing.background");
        check(contents.find("plugin.example_native.pageBg") !=
                  std::string::npos &&
              contents.find("/setting/theme") != std::string::npos,
              "页面背景内容带注册时给的 routes");
        check(contents.find("plugin.example_native.row1") != std::string::npos,
              "内容表里有覆盖后的 row1");
        check(contents.find("\"order\":30") != std::string::npos,
              "同 id 覆盖后 order 换成新值 (30)");
        check(contents.find("musicxx.ui.playing.songlist.top") !=
                  std::string::npos,
              "内容表带槽位全名");
        check(contents.find("plugin.example_native.row2") == std::string::npos,
              "注销后的 row2 不在内容表里");
        check(contents.find("plugin.example_native.badSlot") ==
                  std::string::npos,
              "未知槽位的内容没进表");
        check(contents.find("plugin.example_native.badDeps") ==
                  std::string::npos,
              "deps 非法的内容没进表");
      }

      // ---- 插件声明的槽位 ----
      check(jsonIntField(probe, "uiSlotDefineRc") == "0",
            "插件声明自己的槽位成功 (rc=0)");
      check(jsonIntField(probe, "uiSlotDefineAppRc") == "-6",
            "声明应用预留的槽位被拒绝 (-6)");
      check(jsonIntField(probe, "uiSlotDefineBadKindRc") == "-1",
            "槽位 kind 不合法被拒绝 (-1)");
      check(jsonIntField(probe, "uiSlotUndefineRc") == "0",
            "撤销自己声明的槽位成功 (rc=0)");
      check(jsonIntField(probe, "uiSlotUpdateRc") == "0",
            "要求重新生成 (按槽位) 受理成功 (rc=0)");
      {
        const std::string slots = jsonRawField(probe, "uiSlotList");
        check(slots.find("musicxx.ui.playing.songlist.top") !=
                  std::string::npos,
              "槽位表含应用预留的插入位置");
        check(slots.find("musicxx.ui.home.entry") != std::string::npos,
              "槽位表含应用预留的声明项类型");
        check(slots.find("example_native") == std::string::npos,
              "撤销后的插件槽位不在槽位表里");
      }

      // ---- 快照 ----
      MusicxxExternPluginString snap{};
      const auto snapRc = t_ui_snapshot(host, &snap, &log);
      const std::string snapshot = take(snap);
      check(snapRc == PLUGINXX_RESULT_OK, "ui_snapshot 成功");
      check(snapshot.find("plugin.example_native.card") != std::string::npos,
            "快照含主页入口项");
      check(snapshot.find("plugin.example_native.songInfo") !=
                std::string::npos,
            "快照含歌曲菜单项");
      // 播放页背景 / 页面背景现在都是**内容**（声明项只给入口 / 菜单 / 音效这类列表项），
      // 所以它们的 id 在内容快照里，不在声明项快照里。
      MusicxxExternPluginString contentSnap{};
      const auto contentSnapRc =
          t_ui_contents(host, &contentSnap, &log);
      const std::string contentSnapshot = take(contentSnap);
      check(contentSnapRc == PLUGINXX_RESULT_OK &&
                contentSnapshot.find("plugin.example_native.playingBg") !=
                    std::string::npos &&
                contentSnapshot.find("musicxx.ui.playing.background") !=
                    std::string::npos,
            "内容快照含播放页背景样式");
      check(contentSnapshot.find("plugin.example_native.pageBg") !=
                    std::string::npos &&
                contentSnapshot.find("musicxx.ui.page.background") !=
                    std::string::npos &&
                contentSnapshot.find("/setting/theme") != std::string::npos,
            "内容快照含普通页面背景（routes 声明）");
      check(snapshot.find("musicxx.ui.home.entry") != std::string::npos,
            "快照含官方声明项类型");
      check(snapshot.find("plugin") != std::string::npos,
            "快照项带所属插件 id");
      check(snapshot.find("plugin.other_plugin.card") == std::string::npos,
            "他人命名空间的项不在快照里");
      check(snapshot.find("musicxx.ui.unknown") == std::string::npos,
            "未知类型不在快照里");
      check(snapshot.find("nodata") == std::string::npos, "非法声明不在快照里");

      // 内容注册表快照
      MusicxxExternPluginString contentsOut{};
      const auto contentsRc =
          t_ui_contents(host, &contentsOut, &log);
      check(contentsRc == PLUGINXX_RESULT_OK, "ui_contents 成功");
      const std::string contentsSnapshot = take(contentsOut);
      check(contentsSnapshot.find("plugin.example_native.row1") !=
                std::string::npos,
            "内容注册表含插件注册的内容");
      check(contentsSnapshot.find("\"title\":\"原生示例行\"") !=
                std::string::npos,
            "内容注册表带注册时给的展示名");
      check(contentsSnapshot.find("\"depict\":\"插在播放列表顶部\"") !=
                std::string::npos,
            "内容注册表带注册时给的一句话说明");

      // 槽位表快照
      MusicxxExternPluginString slotsOut{};
      const auto slotsRc = t_ui_slots(host, &slotsOut, &log);
      check(slotsRc == PLUGINXX_RESULT_OK, "ui_slots 成功");
      const std::string slotsSnapshot = take(slotsOut);
      check(slotsSnapshot.find("musicxx.ui.playing.lyric.top") !=
                std::string::npos,
            "槽位表快照含插入位置");
    }

    // 内容生成 / 内容注册 / 槽位变化各有自己的事件
    //
    // 触发点放在插件里（probe 带 `uiRefresh`）：注册与声明都在 `start` 事务里做完，
    // 那些事件早就被前面的用例取走了，所以这里让插件再做一遍全部动作。
    {
      const CapabilityCallOutcome refresh = callCapability(
          host, "example_native", "probe", R"({"uiRefresh":1})", 8000);
      check(refresh.acceptRc == PLUGINXX_RESULT_OK && refresh.ok,
            "probe 触发一次内容 / 槽位刷新");
      const std::string uiEvents = pollEvents(host, 500).json;
      check(uiEvents.find("musicxx.ui.content") != std::string::npos,
            "内容生成结果推 musicxx.ui.content 事件");
      check(uiEvents.find("plugin.example_native.row1") != std::string::npos,
            "事件里带按槽位重新生成的那份内容");
      check(uiEvents.find("plugin.example_native.pushed") != std::string::npos,
            "生成函数里 push 的内容也推了事件");
      check(uiEvents.find("musicxx.ui.contents") != std::string::npos,
            "内容注册表变化推 musicxx.ui.contents 事件");
      check(uiEvents.find("musicxx.ui.slots") != std::string::npos,
            "槽位声明 / 撤销推 musicxx.ui.slots 事件");
    }

    // 单框架线程模型: create 事务与能力处理器必须在同一条线程上执行
    const std::string startThread = jsonStringField(probe, "startThread");
    const std::string callThread = jsonStringField(probe, "callThread");
    check(!startThread.empty() && startThread == callThread,
          "插件代码全部在插件框架线程执行");

    // 能力 / 事件命名空间校验 (官方 id 归应用: 应用没声明的不能注册)
    check(jsonIntField(probe, "unknownCapabilityRc") ==
              std::to_string(PLUGINXX_RESULT_ERR_NOT_DECLARED),
          "应用没声明的官方能力注册被拒绝（not_declared）");
    check(jsonIntField(probe, "foreignActionRc") ==
              std::to_string(PLUGINXX_RESULT_ERR_NOT_DECLARED),
          "没声明过的动作名（能力）被拒绝");
    check(jsonIntField(probe, "dupImplementRc") == "0",
          "同一能力覆盖式重复注册成功");
    check(jsonIntField(probe, "fixedImplRc") != "0",
          "固定实现的能力拒绝注册 impl (只开放前链 / 后链)");
    // 事件命名空间与订阅
    check(jsonIntField(probe, "badTopicSubscribeRc") == "-1",
          "非法事件主题订阅被拒绝");
    check(jsonIntField(probe, "ownPublishRc") == "0",
          "本插件命名空间事件可发布");
    check(jsonIntField(probe, "foreignPublishRc") != "0",
          "他人命名空间事件发布被拒绝");
    check(jsonIntField(probe, "pingEvents") == "1",
          "插件收到插件框架发布的事件");
    check(jsonIntField(probe, "stateEvents") != "0",
          "带当前值的频道变化会通知订阅插件");
    // 带当前值的频道可被插件同步读到 (上一段推送过 musicxx.state.song)
    check(jsonIntField(probe, "stateLen") != "0", "插件可同步读带当前值的频道");
  }

  // 动作请求超时保护: 插件请求 → Dart 不回复 → 插件框架到点终结并推
  // cancel 事件
  {
    const CapabilityCallOutcome neverRespond = callCapability(
        host, "example_native", "probe",
        R"({"appCapabilityCall":"musicxx.test.neverRespond"})", 8000);
    check(neverRespond.acceptRc == PLUGINXX_RESULT_OK && neverRespond.ok,
          "probe 触发插件动作请求成功");

    // 动作超时: 插件给的时间就是生效的超时 (不再钳制区间);
    // 等它过期后取事件与探针
    std::this_thread::sleep_for(std::chrono::milliseconds{1600});

    const PolledEvents events = pollEvents(host, 500);
    const std::string eventsJson = events.json;
    check(events.rc == PLUGINXX_RESULT_OK ||
              events.rc == PLUGINXX_RESULT_ERR_TIMEOUT,
          "poll_events 可调用");
    check(eventsJson.find("musicxx.capability.app.request") !=
              std::string::npos,
          "Dart 侧收到动作请求事件");
    check(eventsJson.find("musicxx.capability.app.cancel") != std::string::npos,
          "动作超时后收到取消事件");

    const CapabilityCallOutcome probeAgain =
        callCapability(host, "example_native", "probe", "{}", 8000);
    const std::string probe2 = probeAgain.payload;
    check(probeAgain.acceptRc == PLUGINXX_RESULT_OK && probeAgain.ok,
          "超时后探针可读");
    // PLUGINXX_OPERATOR_CANCELED == 1: 超时按取消终结,
    // 插件侧收到恰好一次完成通知
    check(jsonIntField(probe2, "lastActionStatus") == "1",
          "动作超时按取消终结并回调插件");
  }

  // 能力调用错误路径: 未加载插件 / 未声明能力
  {
    const CapabilityCallOutcome notLoaded =
        callCapability(host, "no_such_plugin", "probe", "{}", 2000);
    check(notLoaded.acceptRc == PLUGINXX_RESULT_ERR_NOT_FOUND,
          "plugin_call 未加载插件返回未找到 (受理阶段, 不推事件)");
    check(notLoaded.gotResult == false, "受理失败不推结果事件");

    const CapabilityCallOutcome noCapability =
        callCapability(host, "example_native", "noSuchCapability", "{}", 2000);
    check(noCapability.acceptRc == PLUGINXX_RESULT_ERR_NOT_FOUND,
          "plugin_call 未声明能力返回未找到");
    check(noCapability.gotResult == false, "未声明能力也不推结果事件");
  }

  // 加载失败安全降级: 不崩溃、明确失败、插件框架继续可用
  {
    MusicxxExternPluginString loadLog{};
    const auto rc = t_plugin_load_sync(
        host, viewCP("definitely_missing_plugin"), viewCP("{}"), 3000,
        &loadLog);
    const std::string loadErr = take(loadLog);
    check(rc != PLUGINXX_RESULT_OK, "加载不存在的插件返回失败");
    check(rc != PLUGINXX_RESULT_ERR_TIMEOUT,
          "失败是明确失败而非挂住超时");
    check(!loadErr.empty(), "失败原因有可读文本");
    std::printf("  [info] load-fail rc=%d log=%s\n", rc, loadErr.c_str());

    MusicxxExternPluginString again{};
    const auto rc2 = t_plugin_load_sync(
        host, viewCP("definitely_missing_plugin"), viewCP("{}"), 3000, &again);
    take(again);
    check(rc2 != PLUGINXX_RESULT_OK, "失败后不会静默重试成功");

    // 插件框架仍可正常工作
    MusicxxExternPluginString listed{};
    check(t_plugin_list(host, &listed, &log) ==
                  PLUGINXX_RESULT_OK &&
              take(listed).find("example_native") != std::string::npos,
          "加载失败后插件框架仍可用");
  }

  // 禁用 → 能力摘除, 启用 → 重新注册
  {
    check(t_plugin_disable(
              host, viewCP("example_native"), &log) == PLUGINXX_RESULT_OK,
          "plugin_disable 成功");
    int32_t afterDisable = 0;
    t_capability_handler_count(
        host, viewCP("musicxx.player.beforePlaySong"), &afterDisable, &log);
    check(afterDisable == 0, "禁用后能力处理器被摘除");
  }

  // 卸载
  {
    check(t_plugin_unload(host, viewCP("example_native"),
                                              &log) == PLUGINXX_RESULT_OK,
          "plugin_unload 成功");
    int32_t after = 0;
    t_capability_handler_count(
        host, viewCP("musicxx.player.beforePlaySong"), &after, &log);
    check(after == 0, "卸载后无能力残留");
    // 幂等: 重复卸载视为已达目标状态 (Dart 侧状态不同步/重复点击都会走到这里)
    check(t_plugin_unload(host, viewCP("example_native"),
                                              &log) == PLUGINXX_RESULT_OK,
          "重复卸载幂等成功");
    // 卸载后能力调用应受理失败 (注册已随实例摘除)
    const CapabilityCallOutcome afterUnload =
        callCapability(host, "example_native", "probe", "{}", 2000);
    check(afterUnload.acceptRc != PLUGINXX_RESULT_OK,
          "卸载后能力调用失败");
  }

  // 未完成的能力调用必须有终局: 插件卸载 ⇒ `plugin_unloaded` 结果事件
  //
  // "受理成功 ⇒ 一定有终局"是这套异步调用的硬约束: 调用方 (Dart) 只认
  // `musicxx.request.result` 事件, 插件卸载时若不给终局, 它就会一直悬挂。
  {
    MusicxxExternPluginString reloadLog{};
    const auto reloadRc = t_plugin_load_sync(
        host, viewCP("example_native"), viewCP("{}"), 10000, &reloadLog);
    if (reloadRc != PLUGINXX_RESULT_OK) {
      std::printf("  [info] 终局用例加载 rc=%d log=%s\n", reloadRc,
                  take(reloadLog).c_str());
    } else {
      freeStr(reloadLog);
    }
    check(reloadRc == PLUGINXX_RESULT_OK, "为终局用例加载动态库插件");

    // 让插件发起一个"永远不回复"的主程序动作请求, 把这通能力调用卡住
    MusicxxExternPluginString callLog{};
    int64_t requestId = 0;
    const auto rc = t_plugin_call_async(
        host, viewCP("example_native"), viewCP("asyncProbe"),
        viewCP(R"({"waitMs":1200})"), &requestId, &callLog);
    freeStr(callLog);
    check(rc == PLUGINXX_RESULT_OK, "异步完成的能力已受理 (协程挂起中)");
    check(requestId > 0, "异步能力拿到 requestId");

    // 协程还在睡: 插件框架线程是空闲的,
    // 所以卸载能立刻开始并先给这通调用一个终局
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    MusicxxExternPluginString unloadLog{};
    const auto unloadRc = t_plugin_unload(
        host, viewCP("example_native"), &unloadLog);
    freeStr(unloadLog);
    check(unloadRc == PLUGINXX_RESULT_OK, "卸载异步完成的能力所在插件");

    // 卸载先给这通调用一个终局: 事件里 error 说的是 plugin_unloaded
    const std::string unloadPayload =
        waitCapabilityResult(host, requestId, 3000);
    const bool sawUnloaded =
        unloadPayload.find("plugin_unloaded") != std::string::npos &&
        unloadPayload.find("\"ok\":false") != std::string::npos;
    if (!sawUnloaded) {
      std::printf("  [info] unload payload=%s\n", unloadPayload.c_str());
    }
    check(sawUnloaded, "插件卸载给未完成的调用终局 (plugin_unloaded)");
  }

  // ==================== JS 插件 (零编译) ====================
  //
  // 同一套用例在原生插件与 JS 插件上都跑: decision 裁决、observe
  // 通知、能力探针、当前值读取、广播订阅、定时器、禁用/卸载摘除。
  {
    MusicxxExternPluginString loadLog{};
    const auto jsLoadRc = t_plugin_load_sync(
        host, viewCP("example_js"), viewCP(R"({"enabled":true})"), 15000,
        &loadLog);
    if (jsLoadRc != PLUGINXX_RESULT_OK) {
      std::printf("  [info] js load rc=%d log=%s\n", jsLoadRc,
                  take(loadLog).c_str());
    } else {
      freeStr(loadLog);
    }
    check(jsLoadRc == PLUGINXX_RESULT_OK, "JS 插件加载成功 (example_js)");

    int32_t jsImplementationCount = 0;
    check(t_capability_handler_count(
              host, viewCP("musicxx.player.beforePlaySong"),
              &jsImplementationCount, &log) == PLUGINXX_RESULT_OK &&
              jsImplementationCount == 1,
          "JS 能力处理器已注册 (beforePlaySong = 1)");

    // decision 能力: 广告曲目 → cancel
    {
      MusicxxExternPluginString out{};
      const std::string payload =
          R"({"sid":"js1","song":{"name":"广告插播 - JS 测试","artist":"x"}})";
      const auto rc = t_capability_dispatch(
          host, viewCP("musicxx.player.beforePlaySong"), viewP(payload),
          MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC, 300, &out, &log);
      const std::string result = take(out);
      check(rc == PLUGINXX_RESULT_OK, "JS 能力派发返回成功");
      check(result.find("\"skip\"") != std::string::npos,
            "JS 裁决: 广告曲目 skip");
    }

    // decision 能力: 普通曲目 → 无裁决
    {
      MusicxxExternPluginString out{};
      const std::string payload =
          R"({"sid":"js2","song":{"name":"普通歌曲 JS"}})";
      const auto rc = t_capability_dispatch(
          host, viewCP("musicxx.player.beforePlaySong"), viewP(payload),
          MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC, 300, &out, &log);
      const std::string result = take(out);
      check(rc == PLUGINXX_RESULT_OK, "JS 能力 (普通曲目) 派发成功");
      check(result.find("\"skip\"") == std::string::npos,
            "JS 裁决: 普通曲目无 skip");
    }

    // decision 能力: 播放错误 → patch.tryNextSrc
    {
      MusicxxExternPluginString out{};
      const std::string payload =
          R"({"sid":"js3","srcKey":"k3","errorCode":-1})";
      const auto rc = t_capability_dispatch(
          host, viewCP("musicxx.player.error"), viewP(payload),
          MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC, 300, &out, &log);
      const std::string result = take(out);
      check(rc == PLUGINXX_RESULT_OK, "JS 错误能力派发成功");
      check(result.find("tryNextSrc") != std::string::npos,
            "JS 裁决: 首个错误建议换源");
    }

    // 广播频道 (异步): 切歌通知 + 脚本日志
    //
    // 通知型能力在框架 API 2 之后就是广播频道, 所以这里发布广播
    // （与 Dart 侧 app 推 `musicxx.song.changed` 走的是同一条路）。
    {
      const std::string payload =
          R"({"sid":"js4","song":{"name":"观察目标 JS"}})";
      const auto rc = t_broadcast_publish(
          host, viewCP("musicxx.song.changed"), viewP(payload), &log);
      check(rc == PLUGINXX_RESULT_OK, "切歌广播发布成功");

      // 等 JS 线程执行完 (含脚本定时器 50ms)
      std::this_thread::sleep_for(std::chrono::milliseconds{500});
      const std::string eventsJson = pollEvents(host, 500).json;
      check(eventsJson.find("musicxx.plugin.log") != std::string::npos,
            "JS 日志经事件回传");
      check(eventsJson.find("切歌") != std::string::npos,
            "JS 广播订阅已执行 (切歌通知)");
    }

    // 能力探针 (Dart → JS): 自检信息 + 频道当前值同步读
    {
      const CapabilityCallOutcome probeCall =
          callCapability(host, "example_js", "probe", "{}", 8000);
      const std::string probe = probeCall.payload;
      check(probeCall.acceptRc == PLUGINXX_RESULT_OK && probeCall.ok,
            "JS 能力调用成功 (probe)");
      check(jsonStringField(probe, "pluginId") == "example_js",
            "JS 能力读到插件 id");
      check(jsonIntField(probe, "implementationCount") == "4",
            "JS 注册了 4 个能力 (3 个裁决 + 功能埋点通用通道)");
      check(jsonIntField(probe, "songChangedCount") == "1",
            "JS 广播订阅计数为 1 (订阅真的收到了)");
      check(jsonStringField(probe, "currentSongName") == "镜像歌曲",
            "JS 同步读频道当前值 (musicxx.state.song)");
      check(jsonStringField(probe, "hostPlatform") == testPlatform(),
            "JS 读到插件框架信息 (平台)");
      // JS 侧的声明式 UI 项 (顶层注册 → 插件框架线程回放)
      // 2 项 = 主页入口 + 歌曲菜单 (设置界面是插件自己的页面, 不是 UI 项)
      check(jsonIntField(probe, "uiEntries") == "2",
            "JS 注册了 2 个 UI 项 (脚本侧登记)");
      check(jsonIntField(probe, "selfStatsImplementations") == "4",
            "JS 能读自己的统计 (stats.getSelf 的能力计数, 含功能埋点通道)");
      // 客户端界面能力段: 插件框架配置里给的 ui 段原样出现在 host.info() 里
      check(jsonStringField(probe, "hostUiKind") == "gui",
            "JS 从 host.info().ui 读到客户端能力段 (渲染类型)");
      check(jsonIntField(probe, "hostUiBlocks") == "3",
            "JS 从 host.info().ui 读到客户端支持的组件数");
    }

    // 流结束 (广播表 v3): JS 侧同样能订阅结束回调、结束自己的流
    //
    // 分两次 probe: 第一次触发自检（返回值与「结束之后再发布」的异常是同步的），
    // 结束回调与值回调在之后的 JS 任务里执行，第二次 probe 才能读到。
    {
      const CapabilityCallOutcome trigger =
          callCapability(host, "example_js", "probe",
                         R"({"streamEndCheck":1})", 8000);
      const std::string result = trigger.payload;
      check(trigger.acceptRc == PLUGINXX_RESULT_OK && trigger.ok,
            "JS probe(streamEndCheck) 成功");
      check(jsonIntField(result, "streamEndSubscribed") == "1",
            "JS 带结束回调订阅成功");
      check(jsonIntField(result, "streamEndPublishOk") == "1",
            "JS 结束前发布值成功");
      check(jsonIntField(result, "streamEndEndOk") == "1",
            "JS 属主结束自己的流成功");
      check(!jsonStringField(result, "streamEndAfterEndError").empty(),
            "JS 流结束之后再发布抛错 (ERR_ENDED)");

      const CapabilityCallOutcome after =
          callCapability(host, "example_js", "probe", "{}", 8000);
      const std::string state = after.payload;
      check(after.acceptRc == PLUGINXX_RESULT_OK && after.ok,
            "JS probe() 二次读取成功");
      check(jsonIntField(state, "streamEndValues") == "1",
            "JS 订阅在结束前收到发布的值");
      check(jsonIntField(state, "streamEndEnds") == "1",
            "JS 结束回调恰好一次");
      check(jsonIntField(state, "streamEndReasonOk") == "1",
            "JS 结束回调拿到 reason JSON");
    }

    // JS 插件的 UI 项进入插件框架快照 (与动态库插件同一注册表)
    {
      MusicxxExternPluginString snap{};
      const auto rc = t_ui_snapshot(host, &snap, &log);
      const std::string snapshot = take(snap);
      check(rc == PLUGINXX_RESULT_OK, "ui_snapshot 可读 (含 JS 项)");
      check(snapshot.find("plugin.example_js.card") != std::string::npos,
            "JS 插件的主页入口项进入快照");
      check(snapshot.find("plugin.example_js.songInfo") != std::string::npos,
            "JS 插件的菜单项进入快照");
      // 框架没有"插件设置页"类型: 设置界面改成插件自己的页面 (同名能力),
      // 不再注册 UI 项, 快照里也不该出现 settings.page
      check(snapshot.find("musicxx.ui.settings.page") == std::string::npos,
            "快照里没有设置页类型 (框架不管理插件设置入口)");
      check(snapshot.find("plugin.example_js.settings") == std::string::npos,
            "JS 插件不再注册设置页入口项");
      check(snapshot.find("plugin.example_js.bg") == std::string::npos,
            "播放页背景示例已拆成独立插件 (example_js 不再注册背景项)");
    }

    // 插件自己的设置界面: 现在是 `musicxx.ui.page` 上的内容
    // (框架不管理设置入口、也不渲染设置控件, 页面内容全部由插件给)
    {
      const std::string view =
          requestUiContent(host, "plugin.example_js.settings");
      check(!view.empty(), "设置页内容能生成 (插件自绘页面)");
      check(view.find("\"blocks\"") != std::string::npos,
            "设置界面返回声明式块 (由插件给出结构)");
      check(view.find("config.json") != std::string::npos,
            "设置界面内容由插件给出 (说明自己的 config.json 用法)");
    }

    // 播放页背景示例 `example_js_playing_bg`: 六种背景样式（封面三种 + 图案三种）
    // + 歌曲图接管 + 封面取色钩子 + 说明页 / 设置页 / 频谱页。
    //
    // 这个块同时验证"一个插件多个脚本"：探针里的计数来自不同脚本声明的常量与函数，
    // 少执行一个脚本就对不上（原先单独用一个 27 行的插件教这件事，现已并入这里）。
    {
      MusicxxExternPluginString loadLog{};
      const auto bgLoadRc = t_plugin_load_sync(
          host, viewCP("example_js_playing_bg"), viewCP(R"({"enabled":true})"),
          8000, &loadLog);
      if (bgLoadRc != PLUGINXX_RESULT_OK) {
        std::printf("  [info] playing_bg js load rc=%d log=%s\n", bgLoadRc,
                    take(loadLog).c_str());
      } else {
        freeStr(loadLog);
      }
      check(bgLoadRc == PLUGINXX_RESULT_OK,
            "播放页背景示例插件加载成功 (example_js_playing_bg)");

      // 多脚本: 六个脚本都执行过
      const CapabilityCallOutcome bgProbe =
          callCapability(host, "example_js_playing_bg", "probe", "{}", 8000);
      check(bgProbe.acceptRc == PLUGINXX_RESULT_OK && bgProbe.ok,
            "背景示例的多脚本探针可读");
      check(jsonIntField(bgProbe.payload, "styles") == "6",
            "六种背景样式都在 (backgrounds.js 执行过)");
      check(jsonIntField(bgProbe.payload, "uiEntries") == "1",
            "声明项只剩主页入口 (样式与歌曲图接管已是内容)");
      check(jsonIntField(bgProbe.payload, "uiContents") == "10",
            "内容 10 份 = 六种背景 + 歌曲图接管 + 三个页面");
      check(jsonIntField(bgProbe.payload, "paletteHook") == "1",
            "取色钩子已注册 (palette.js 执行过)");
      check(jsonIntField(bgProbe.payload, "hasSettingsView") == "1" &&
                jsonIntField(bgProbe.payload, "hasSpectrumView") == "1",
            "页面函数都在 (views.js 执行过)");

      MusicxxExternPluginString snap{};
      const auto snapRc = t_ui_contents(host, &snap, &log);
      const std::string snapshot = take(snap);
      check(snapRc == PLUGINXX_RESULT_OK &&
                snapshot.find("plugin.example_js_playing_bg.lattice") !=
                    std::string::npos &&
                snapshot.find("musicxx.ui.playing.background") !=
                    std::string::npos,
            "背景样式作为内容进注册表 (六种都在)");
      check(snapshot.find("plugin.example_js_playing_bg.settings") !=
                    std::string::npos &&
                snapshot.find("musicxx.ui.page") != std::string::npos,
            "三个页面都是 musicxx.ui.page 上的内容");
      // 注册名是 `home`（入口打开的页面 id 才是 `card`），快照里的 id 取注册名
      // 主页入口仍是**声明项**，所以它在声明项快照里
      {
        MusicxxExternPluginString items{};
        const auto itemsRc =
            t_ui_snapshot(host, &items, &log);
        const std::string itemSnapshot = take(items);
        check(itemsRc == PLUGINXX_RESULT_OK &&
                  itemSnapshot.find("plugin.example_js_playing_bg.home") !=
                      std::string::npos,
              "背景示例的主页入口项进入声明项快照");
      }

      // 速率设置: 页面现在是 `musicxx.ui.page` 上的内容, 让插件生成一份
      // (设置页的行由布局块组合, 右侧状态按值的形态读: 见 jsonFirstRateText)
      const std::string beforeView =
          requestUiContent(host, "plugin.example_js_playing_bg.settings");
      check(beforeView.find("背景动画速率") != std::string::npos,
            "背景示例的设置页含『背景动画速率』设置项");
      const std::string beforeRate = jsonFirstRateText(beforeView);

      const CapabilityCallOutcome cycleCall = callCapability(
          host, "example_js_playing_bg", "cycleBackgroundRate",
          R"({"view":"settings"})", 8000);
      check(cycleCall.acceptRc == PLUGINXX_RESULT_OK && cycleCall.ok,
            "切换背景动画速率的能力可调用");
      const std::string cycleView =
          requestUiContent(host, "plugin.example_js_playing_bg.settings");
      const std::string afterRate = jsonFirstRateText(cycleView);
      check(!beforeRate.empty() && !afterRate.empty() &&
                beforeRate != afterRate,
            "切换后设置页显示的速率随之变化");

      // 卸载 → UI 项摘除
      check(t_plugin_unload(
                host, viewCP("example_js_playing_bg"), &log) ==
                PLUGINXX_RESULT_OK,
            "卸载背景示例插件");
      MusicxxExternPluginString after{};
      const auto afterRc =
          t_ui_contents(host, &after, &log);
      const std::string afterSnapshot = take(after);
      check(afterRc == PLUGINXX_RESULT_OK &&
                afterSnapshot.find("plugin.example_js_playing_bg.lattice") ==
                    std::string::npos,
            "卸载后背景示例的内容无残留");
    }

    // ============ 播放页插槽示例 (example_js_playing_slots)
    // ============
    //
    // 三类位置各验一遍：插入块（歌词页 / 播放列表页 / 更多信息页）、
    // 按钮级 / 整栏替换（controlBar）、音频动效（playing.visualizer）与
    // 普通页面背景（歌曲页面 /songs 的歌单封面背景）。
    {
      MusicxxExternPluginString loadLog{};
      const auto slotsRc = t_plugin_load_sync(
          host, viewCP("example_js_playing_slots"),
          viewCP(R"({"enabled":true})"), 8000, &loadLog);
      if (slotsRc != PLUGINXX_RESULT_OK) {
        std::printf("  [info] slots js load rc=%d log=%s\n", slotsRc,
                    take(loadLog).c_str());
      } else {
        freeStr(loadLog);
      }
      check(slotsRc == PLUGINXX_RESULT_OK,
            "播插槽示例插件加载成功 (example_js_playing_slots)");

      const CapabilityCallOutcome slotsProbe = callCapability(
          host, "example_js_playing_slots", "probe", "{}", 8000);
      check(slotsProbe.acceptRc == PLUGINXX_RESULT_OK && slotsProbe.ok,
            "插槽示例的探针可读");
      check(jsonIntField(slotsProbe.payload, "uiEntries") == "1",
            "声明项只剩主页入口 (位置上的界面全是内容)");
      check(jsonIntField(slotsProbe.payload, "uiContents") == "9",
            "内容 9 份 = 3 个插入 + 2 个按钮 + 1 个动效 + 1 个页面背景 + 2 个页面");
      check(jsonIntField(slotsProbe.payload, "hasControlBar") == "1" &&
                jsonIntField(slotsProbe.payload, "visualizer") == "1" &&
                jsonIntField(slotsProbe.payload, "pageBackground") == "1",
            "控制栏视图、动效与页面背景声明都在 (四个业务脚本都执行过)");

      MusicxxExternPluginString snap{};
      const auto snapRc = t_ui_contents(host, &snap, &log);
      const std::string snapshot = take(snap);
      // 插入位置 / 替换位置 / 可选样式槽位只能由**内容**填充:
      // 插件用 `ui.item.register` 往这些槽位注册声明项会被框架直接拒绝。
      check(snapRc == PLUGINXX_RESULT_OK &&
                snapshot.find("musicxx.ui.playing.lyric.top") !=
                    std::string::npos &&
                snapshot.find("musicxx.ui.playing.songlist.header") !=
                    std::string::npos &&
                snapshot.find("musicxx.ui.playing.moreInfo.footer") !=
                    std::string::npos,
            "三个插入位置都注册了内容");
      check(snapshot.find("musicxx.ui.playing.controlBar.menu") !=
                    std::string::npos &&
                snapshot.find("musicxx.ui.playing.controlBar.favorite") !=
                    std::string::npos,
            "按钮级替换位置注册了内容");
      check(snapshot.find("musicxx.ui.playing.visualizer") !=
                    std::string::npos,
            "音频动效是可选样式槽位上的内容");
      check(snapshot.find("musicxx.ui.page.background") != std::string::npos &&
                snapshot.find("/songs") != std::string::npos,
            "歌曲页面的页面背景带 routes 声明");
      check(snapshot.find("musicxx.ui.page") != std::string::npos,
            "两个插件页面是 musicxx.ui.page 上的内容");

      // 整栏替换：开启后多出一份 `wholeBar` 内容（按钮级内容保留，主程序会忽略它们）
      const CapabilityCallOutcome barCall = callCapability(
          host, "example_js_playing_slots", "toggleWholeBar",
          R"({"view":"settings"})", 8000);
      check(barCall.acceptRc == PLUGINXX_RESULT_OK && barCall.ok,
            "切换整栏替换的能力可调用");
      MusicxxExternPluginString barSnap{};
      const auto barSnapRc =
          t_ui_contents(host, &barSnap, &log);
      const std::string barSnapshot = take(barSnap);
      check(barSnapRc == PLUGINXX_RESULT_OK &&
                barSnapshot.find("plugin.example_js_playing_slots.wholeBar") !=
                    std::string::npos,
            "整栏替换是一份内容 (开启后进注册表)");

      // 整栏内容的生成函数：带状态参数生成一棵块树
      const std::string barView = requestUiContent(
          host, "plugin.example_js_playing_slots.wholeBar",
          R"({"musicxx.player.playing":true,"musicxx.song.hasSong":true,"musicxx.song.favorite":true})");
      check(barView.find("\"blocks\"") != std::string::npos,
            "整栏内容生成出块描述");

      // 关回去：把 config.json 里的整栏开关恢复默认，重复跑同一个构建目录也不会改变计数
      const CapabilityCallOutcome barReset = callCapability(
          host, "example_js_playing_slots", "toggleWholeBar",
          R"({"view":"settings"})", 8000);
      check(barReset.acceptRc == PLUGINXX_RESULT_OK && barReset.ok,
            "整栏替换可以关回去");

      check(t_plugin_unload(
                host, viewCP("example_js_playing_slots"), &log) ==
                PLUGINXX_RESULT_OK,
            "卸载插槽示例插件");
      MusicxxExternPluginString afterSlots{};
      const auto afterSlotsRc =
          t_ui_contents(host, &afterSlots, &log);
      const std::string afterSlotsSnapshot = take(afterSlots);
      check(afterSlotsRc == PLUGINXX_RESULT_OK &&
                afterSlotsSnapshot.find(
                    "musicxx.ui.playing.lyric.top") == std::string::npos,
            "卸载后插槽示例的内容无残留");
    }

    // ============ 网络能力 (example_js_net 的 selftest 跑完整一轮)
    // ============
    //
    // 这一段是插件网络层唯一的端到端回归: 起 HTTP 服务并自请求、静态路由与
    // Range、 TCP 服务端/客户端、WebSocket 两端、UDP 通道都在 selftest
    // 里走一遍。 换网络实现（或改 op 分发）之后这里必须仍然是 ok。
    {
      MusicxxExternPluginString netLoadLog{};
      const auto netLoadRc = t_plugin_load_sync(
          host, viewCP("example_js_net"), viewCP(R"({"enabled":true})"), 8000,
          &netLoadLog);
      if (netLoadRc != PLUGINXX_RESULT_OK) {
        std::printf("  [info] net example load rc=%d log=%s\n", netLoadRc,
                    take(netLoadLog).c_str());
      } else {
        freeStr(netLoadLog);
      }
      check(netLoadRc == PLUGINXX_RESULT_OK,
            "网络示例插件加载成功 (example_js_net)");

      const CapabilityCallOutcome selfTest =
          callCapability(host, "example_js_net", "selftest", "{}", 60000);
      if (!selfTest.ok) {
        std::printf("  [info] net selftest rc=%d payload=%s\n",
                    selfTest.acceptRc, selfTest.payload.c_str());
      }
      check(selfTest.acceptRc == PLUGINXX_RESULT_OK && selfTest.ok &&
                selfTest.payload.find("\"ok\":true") != std::string::npos,
            "网络能力自检通过 (HTTP 服务/客户端、静态路由与 "
            "Range、TCP、WebSocket、UDP)");

      check(t_plugin_unload(host, viewCP("example_js_net"),
                                                &log) ==
                PLUGINXX_RESULT_OK,
            "卸载网络示例插件");
    }

    // ============ 多脚本按顺序加载 ============
    //
    // 清单 `scripts` 里的脚本在**同一个 JS 上下文**里依次执行，前一个脚本声明的常量与
    // 函数在后一个里可见。这条已经由 `example_js_playing_bg` 的探针覆盖
    // （背景示例上面的 `probe`: styles / uiEntries / hasSettingsView 分别来自
    // backgrounds.js、palette.js 与 views.js；清单漏一个脚本就对不上），
    // 原先那个只教多脚本的示例插件（`example_js_multi_script`）已删除。

    // ============ 插件框架内置网络能力 (example_js_net) ============
    //
    // 六条角色一次跑完: HTTP / TCP / WebSocket 各自"起服务 → 本机客户端连上去 →
    // 比对回包"。全部经由 `musicxx.net` 表 (JS 侧是
    // `musicxx.net.http/socket/ws`)， 服务只监听回环，端口由系统分配。
    {
      MusicxxExternPluginString loadLog{};
      const auto loadRc = t_plugin_load_sync(
          host, viewCP("example_js_net"), viewCP("{}"), 8000, &loadLog);
      if (loadRc != PLUGINXX_RESULT_OK) {
        std::printf("  [info] example_js_net load rc=%d log=%s\n", loadRc,
                    take(loadLog).c_str());
      } else {
        freeStr(loadLog);
      }
      check(loadRc == PLUGINXX_RESULT_OK, "网络示例插件加载成功");

      // 插件载入 1.5 s 后会自动跑一遍; 这里主动调一次（同一时间只跑一份，
      // 不会互相抢固定的服务 id）。
      const CapabilityCallOutcome netTest =
          callCapability(host, "example_js_net", "selftest", "{}", 20000);
      const std::string report = netTest.payload;
      std::printf("  [info] example_js_net selftest=%s\n", report.c_str());
      check(netTest.acceptRc == PLUGINXX_RESULT_OK && netTest.ok,
            "网络示例的自检能力可调用");
      check(jsonRawField(report, "ok") == "true",
            "网络示例自检通过 (HTTP / TCP / WebSocket 客户端 + 服务端)");
      check(report.find("\"http\":{") != std::string::npos &&
                report.find("\"socket\":{") != std::string::npos &&
                report.find("\"ws\":{") != std::string::npos,
            "自检报告包含 HTTP / TCP / WebSocket 三段结果");
      check(report.find("\"udp\":{") != std::string::npos &&
                report.find("udp:ping") != std::string::npos,
            "UDP 通道能收到发给它的包 (文本与二进制)");
      check(report.find("\"binaryBytes\":4") != std::string::npos,
            "UDP 二进制包按字节数收妥");
      check(report.find("\"broadcast\":true") != std::string::npos,
            "UDP 能发给广播地址 255.255.255.255");
      check(report.find("echo:ping") != std::string::npos,
            "TCP 与 WebSocket 都拿到了服务端回写");
      check(report.find("\"chunked\":{") != std::string::npos &&
                report.find("hello world") != std::string::npos,
            "HTTP 客户端能解码分块（chunked）响应");
      check(report.find("\"fetch\":{") != std::string::npos &&
                report.find("\"downloadBytes\":") != std::string::npos,
            "fetch / download 由插件框架自己实现并能正常返回");
      check(report.find("\"static\":{") != std::string::npos &&
                report.find("\"rangeStatus\":206") != std::string::npos &&
                report.find("\"beyondStatus\":416") != std::string::npos &&
                report.find("\"goneStatus\":404") != std::string::npos &&
                report.find("\"prefixedStatus\":200") != std::string::npos,
            "静态路由 / 范围请求 / 摘掉路由后的 404 / 带路径前缀 都对");

      // 自省: 自检结束会主动关掉服务与连接
      const CapabilityCallOutcome netInfo =
          callCapability(host, "example_js_net", "info", "{}", 8000);
      check(netInfo.acceptRc == PLUGINXX_RESULT_OK && netInfo.ok,
            "网络自省能力可调用");
      check(netInfo.payload.find("\"connections\":[]") != std::string::npos,
            "自检结束后没有残留连接");
      check(netInfo.payload.find("\"udp\":[]") != std::string::npos,
            "自检结束后没有残留 UDP 通道");
      // 客户端 TLS 能力: https / wss 能不能用 (构建时关掉 TLS 就是 tls=false)
      check(netInfo.payload.find("\"tls\":") != std::string::npos,
            "网络自省报告了客户端 TLS 能力");
      const bool tlsBuilt =
          netInfo.payload.find("\"tls\":true") != std::string::npos;
      if (tlsBuilt) {
        check(netInfo.payload.find("\"wss\"") != std::string::npos,
              "带 TLS 的构建声明支持 https / wss");

        // 真实 https 需要一个可用的公网/自建地址, 默认不跑
        // (与真实音频用例同一套做法):
        //   set MUSICXX_TEST_HTTPS_URL=https://example.com/
        const char *httpsUrl = std::getenv("MUSICXX_TEST_HTTPS_URL");
        if (httpsUrl && *httpsUrl) {
          const std::string args =
              std::string("{\"httpsUrl\":\"") + httpsUrl + "\"}";
          const CapabilityCallOutcome httpsTest =
              callCapability(host, "example_js_net", "selftest", args, 30000);
          std::printf("  [info] https selftest=%s\n",
                      httpsTest.payload.c_str());
          check(
              httpsTest.acceptRc == PLUGINXX_RESULT_OK && httpsTest.ok &&
                  httpsTest.payload.find("\"https\":{") != std::string::npos &&
                  httpsTest.payload.find("\"status\":200") != std::string::npos,
              "https 客户端能完成 TLS 握手并拿到 200 (证书链与主机名校验通过)");
        }
      }

      check(t_plugin_unload(host, viewCP("example_js_net"),
                                                &log) ==
                PLUGINXX_RESULT_OK,
            "网络示例插件卸载成功");
    }

    // ============ 跨插件能力调用 (capability.call) ============
    //
    // JS 插件调用其它插件的能力: JS 目标同线程直接调用;
    // 原生目标投递到插件框架线程执行, 脚本侧不等待 (返回
    // Promise)。这里用"最后一次结果"记录, 由 probe 能力回读。
    {
      // 原生示例插件在前面的用例里被卸载了, 这里临时再装一次作为被调用方
      MusicxxExternPluginString reloadLog{};
      const auto reloadRc = t_plugin_load_sync(
          host, viewCP("example_native"), viewCP("{}"), 8000, &reloadLog);
      check(reloadRc == PLUGINXX_RESULT_OK,
            "临时加载动态库插件 (跨插件调用被调用方)");

      const CapabilityCallOutcome crossCall = callCapability(
          host, "example_js", "crossCall",
          R"({"target":"example_native","method":"probe"})", 8000);
      const std::string callJson = crossCall.payload;
      check(crossCall.acceptRc == PLUGINXX_RESULT_OK && crossCall.ok,
            "触发跨插件调用 (JS → 原生)");
      check(jsonIntField(callJson, "accepted") == "1", "跨插件调用被受理");

      // 等回执回到 JS 线程 (插件框架线程执行 + 投递回 JS 线程)
      std::this_thread::sleep_for(std::chrono::milliseconds{800});
      const CapabilityCallOutcome probe3Call =
          callCapability(host, "example_js", "probe", "{}", 8000);
      const std::string probe3Json = probe3Call.payload;
      check(probe3Call.acceptRc == PLUGINXX_RESULT_OK && probe3Call.ok,
            "回读跨插件调用结果");
      check(jsonIntField(probe3Json, "crossPending") == "0",
            "跨插件调用已结束 (无挂起)");
      check(jsonIntField(probe3Json, "crossOk") == "1",
            "跨插件调用成功 (JS → 原生能力)");
      check(jsonIntField(probe3Json, "crossKeys") != "0",
            "跨插件调用拿到结果内容");

      // JS → JS 目标 + **异步能力处理器** (返回 Promise): 同一条 JS
      // 线程上就地调用, 处理器返回 Promise 时按等待号结算后回执 ——
      // 这条路以前直接失败
      // (`capability_async_without_wait`), 现在与 JS → 原生一样能拿到结果。
      const CapabilityCallOutcome jsToJs = callCapability(
          host, "example_js", "crossCall",
          R"({"target":"example_js","method":"slowProbe","args":{"waitMs":120}})",
          8000);
      check(jsToJs.acceptRc == PLUGINXX_RESULT_OK && jsToJs.ok,
            "触发跨插件调用 (JS → JS 的异步能力)");
      std::this_thread::sleep_for(std::chrono::milliseconds{600});
      const CapabilityCallOutcome probe4Call =
          callCapability(host, "example_js", "probe", "{}", 8000);
      const std::string probe4Json = probe4Call.payload;
      check(probe4Call.acceptRc == PLUGINXX_RESULT_OK && probe4Call.ok,
            "回读 JS → JS 异步能力的调用结果");
      if (jsonIntField(probe4Json, "crossOk") != "1") {
        std::printf("  [info] js->js cross error=%s\n",
                    jsonStringField(probe4Json, "crossError").c_str());
      }
      check(jsonIntField(probe4Json, "crossPending") == "0",
            "JS → JS 异步调用已结束 (无挂起)");
      check(jsonIntField(probe4Json, "crossOk") == "1",
            "JS → JS 的异步能力处理器能拿到结果 (返回 Promise 也算回答)");

      check(t_plugin_unload(host, viewCP("example_native"),
                                                &log) ==
                PLUGINXX_RESULT_OK,
            "卸载临时加载的动态库插件");
    }

    // ============ 通用通道: 插件声明的功能埋点 ============
    //
    // 插件点共用**一条固定能力** `musicxx.feature.point.call`（点是运行期数据，
    // 插件框架的预设表不可能逐条登记；插件点走这一条通道，靠 `point`
    // 参数区分）:
    // - 处理器按 `point` 过滤，回答要包一层 `patch`（firstNonNull 只在收到
    // patch 时才停）；
    // - `ownerTag` = 点 id（只用于自省"这个处理器声称实现哪个点"）；
    // - 不回答自己发起的调用（`caller` 等于自己的插件 id）。
    {
      // 1) JS 示例声明并实现了 `plugin.example_js.beat`
      const CapabilityCallOutcome featureProbe =
          callCapability(host, "example_js", "featureProbe", "{}", 8000);
      check(featureProbe.acceptRc == PLUGINXX_RESULT_OK &&
                featureProbe.ok,
            "功能埋点示例能力可调用 (featureProbe)");
      std::printf("  [info] featureProbe=%s\n", featureProbe.payload.c_str());
      check(jsonStringField(featureProbe.payload, "declared") ==
                "plugin.example_js.beat",
            "插件声明了功能埋点 (plugin.<插件id>.beat)");

      // 2) 处理器登记成功 + ownerTag = 点 id（还能在自省里看到）
      {
        MusicxxExternPluginString out{};
        const auto rc = t_capability_handlers(
            host, viewCP("musicxx.feature.point.call"), &out, &log);
        const std::string json = take(out);
        check(rc == PLUGINXX_RESULT_OK,
              "通用通道能力存在 (插件注册处理器不是 ERR_NOT_FOUND)");
        check(jsonIntField(json, "count") != "0",
              "通用通道上有处理器 (示例插件登记了实现)");
        check(json.find("\"ownerTag\":\"plugin.example_js.beat\"") !=
                  std::string::npos,
              "处理器的 ownerTag 是它实现的功能埋点 id");
      }

      // 3) 派发一次: 处理器的回答按 `patch` 合并到顶层 (`value` 就是结果)
      {
        const std::string ack = dispatchCapabilitySync(
            host, "musicxx.feature.point.call",
            R"({"point":"plugin.example_js.beat","args":{"bpm":60,"beats":3},"caller":""})");
        check(jsonIntField(ack, "called") == "1", "通用通道派发到示例插件");
        check(ack.find("\"bpm\":60") != std::string::npos,
              "处理器的结果经 patch 合并到裁决顶层");
        check(ack.find("\"beats\":[0,1000,2000]") != std::string::npos,
              "处理器按自己的语义算结果 (60 BPM → 每拍 1000 ms)");
      }

      // 4) 不是自己的点: 处理器不回答（链条继续问别人）
      {
        const std::string ack = dispatchCapabilitySync(
            host, "musicxx.feature.point.call",
            R"({"point":"plugin.nobody.other","args":{},"caller":""})");
        check(ack.find("\"bpm\"") == std::string::npos,
              "不是自己实现的功能埋点不回答");
      }

      // 5) 不回答自己发起的调用（`caller` 等于自己的插件 id）
      {
        const std::string ack = dispatchCapabilitySync(
            host, "musicxx.feature.point.call",
            R"({"point":"plugin.example_js.beat","args":{},"caller":"example_js"})");
        check(ack.find("\"confident\"") == std::string::npos,
              "不回答自己发起的调用 (caller === 自己)");
      }

      // 6) 摘除实现后不再回答（`unprovide`）
      {
        const CapabilityCallOutcome unprovide =
            callCapability(host, "example_js", "featureUnprovide", "{}", 8000);
        check(unprovide.acceptRc == PLUGINXX_RESULT_OK && unprovide.ok,
              "示例插件能摘除自己的实现");
        const std::string ack = dispatchCapabilitySync(
            host, "musicxx.feature.point.call",
            R"({"point":"plugin.example_js.beat","args":{},"caller":""})");
        check(ack.find("\"confident\"") == std::string::npos,
              "摘除实现后不再回答 (unprovide)");
      }

      // 7) native 侧糖: 同一个约定在动态库插件上一样成立
      //    (处理函数返回的是**值**, 糖负责过滤 point / caller 并包 patch)
      {
        MusicxxExternPluginString nativeLog{};
        const auto nativeRc = t_plugin_load_sync(
            host, viewCP("example_native"), viewCP("{}"), 8000, &nativeLog);
        freeStr(nativeLog);
        check(nativeRc == PLUGINXX_RESULT_OK,
              "加载动态库插件 (native 糖用例)");

        const CapabilityCallOutcome nativeProbe = callCapability(
            host, "example_native", "plugin.example_native.probe", "{}", 8000);
        check(nativeProbe.acceptRc == PLUGINXX_RESULT_OK &&
                  nativeProbe.ok,
              "动态库插件的探针可读");
        check(jsonStringField(nativeProbe.payload, "featurePointId") ==
                  "plugin.example_native.sample",
              "动态库插件读了 host.info().pluginId 并拼出自己的点 id");
        check(jsonIntField(nativeProbe.payload, "featurePointRc") == "0",
              "native 糖登记功能埋点实现成功 (ownerTag = 点 id)");

        const std::string nativeAck = dispatchCapabilitySync(
            host, "musicxx.feature.point.call",
            R"({"point":"plugin.example_native.sample","args":{"a":2,"b":3},"caller":""})");
        check(jsonIntField(nativeAck, "called") == "1",
              "通用通道派发到动态库插件");
        if (nativeAck.find("\"sum\":5") == std::string::npos) {
          std::printf("  [info] native ack=%s\n", nativeAck.c_str());
        }
        check(nativeAck.find("\"sum\":5") != std::string::npos,
              "native 糖把返回值包成 patch.value (处理函数只返回值)");

        // 不是自己的点: 同一个插件不回答（说明糖做了 point 过滤）
        const std::string foreignPoint = dispatchCapabilitySync(
            host, "musicxx.feature.point.call",
            R"({"point":"plugin.nobody.other","args":{"a":2,"b":3},"caller":""})");
        check(foreignPoint.find("\"sum\"") == std::string::npos,
              "native 糖按 point 过滤 (不是自己的点不回答)");

        // caller 是自己: 不回答（避免把自己算的结果再问回去）
        const std::string selfCall = dispatchCapabilitySync(
            host, "musicxx.feature.point.call",
            R"({"point":"plugin.example_native.sample","args":{"a":2,"b":3},"caller":"example_native"})");
        check(selfCall.find("\"sum\"") == std::string::npos,
              "native 糖不回答自己发起的调用 (caller === 自己)");

        check(t_plugin_unload(
                  host, viewCP("example_native"), &log) ==
                  PLUGINXX_RESULT_OK,
              "卸载动态库插件 (native 糖用例结束)");
      }
    }

    // ============ JS 执行上限已移除 ============
    //
    // 早期版本有一个可选的
    // `jsExecGuardMs`（超时中断超长脚本）。现在插件框架不限制脚本执行
    // 时长、也不提供中断开关：死循环脚本会一直占住共享 JS 线程（对照示例
    // example_js_spin 因此不再加载 —— 它会真的把线程占死，连卸载都做不了）。
    {
      check(t_set_config(host,
                                             viewP(R"({"jsExecGuardMs":400})"),
                                             &log) == PLUGINXX_RESULT_OK,
            "执行上限配置项被忽略 (不再报错)");
      MusicxxExternPluginString stats{};
      const auto statsRc =
          t_stats(host, nullptr, &stats, &log);
      const std::string statsJson = take(stats);
      check(statsRc == PLUGINXX_RESULT_OK, "stats 可读 (无执行上限)");
      check(statsJson.find("execGuardHits") == std::string::npos,
            "统计不再含执行上限中断计数");
      check(statsJson.find("execGuardMs") == std::string::npos,
            "统计不再含执行上限配置");
    }

    // 禁用 → 能力摘除, 启用 → 重新注册 (脚本重跑)
    {
      check(t_plugin_disable(
                host, viewCP("example_js"), &log) == PLUGINXX_RESULT_OK,
            "JS 插件禁用成功");
      int32_t afterDisable = 0;
      t_capability_handler_count(
          host, viewCP("musicxx.player.beforePlaySong"), &afterDisable, &log);
      check(afterDisable == 0, "JS 插件禁用后能力被摘除");

      check(t_plugin_enable(
                host, viewCP("example_js"), &log) == PLUGINXX_RESULT_OK,
            "JS 插件重新启用成功");
      std::this_thread::sleep_for(std::chrono::milliseconds{300});
      int32_t afterEnable = 0;
      t_capability_handler_count(
          host, viewCP("musicxx.player.beforePlaySong"), &afterEnable, &log);
      check(afterEnable == 1, "JS 插件启用后能力重新注册");
    }

    // 调试信息含 JS 运行时状态
    {
      MusicxxExternPluginString debug{};
      const auto rc = t_debug_info(host, &debug, &log);
      const std::string debugJson = take(debug);
      check(rc == PLUGINXX_RESULT_OK, "debug_info 可读");
      check(debugJson.find("\"available\":true") != std::string::npos,
            "调试信息含 JS 运行时");
      check(debugJson.find("example_js") != std::string::npos,
            "调试信息含 JS 插件");
      // 共享 JS 线程的排队观测 (只观测不限制)
      check(debugJson.find("\"queueDepth\":") != std::string::npos,
            "调试信息含 JS 任务队列深度");
      check(debugJson.find("\"queueWaitMaxMs\":") != std::string::npos,
            "调试信息含 JS 排队等待时长");

      MusicxxExternPluginString stats{};
      check(t_stats(host, nullptr, &stats, &log) ==
                PLUGINXX_RESULT_OK,
            "stats 可读 (含 JS 段)");
      const std::string statsJson = take(stats);
      check(statsJson.find("\"queueDepth\":") != std::string::npos,
            "stats 含 JS 任务队列深度");
      check(statsJson.find("\"queueWaitLastMs\":") != std::string::npos,
            "stats 含 JS 最近一次排队等待时长");
    }

    // 卸载 → 注册全部摘除
    {
      check(t_plugin_unload(
                host, viewCP("example_js"), &log) == PLUGINXX_RESULT_OK,
            "JS 插件卸载成功");
      int32_t after = 0;
      t_capability_handler_count(
          host, viewCP("musicxx.player.beforePlaySong"), &after, &log);
      check(after == 0, "JS 插件卸载后无能力残留");
      const CapabilityCallOutcome jsAfterUnload =
          callCapability(host, "example_js", "probe", "{}", 2000);
      check(jsAfterUnload.acceptRc != PLUGINXX_RESULT_OK,
            "JS 插件卸载后能力调用失败");
    }
  }

  // ==================== JavaScript 异步裁决 (裁决处理器返回 Promise)
  // ====================
  //
  // 语义 (无等待超时):
  // - 处理器返回 Promise 时, 插件框架一直等到它结算, 裁决照常生效;
  // - 没有"等太久就按无裁决继续"这回事, 也没有迟到丢弃。
  {
    MusicxxExternPluginString loadLog{};
    const auto jsAsyncRc = t_plugin_load_sync(
        host, viewCP("example_js_async"), viewCP(R"({"enabled":true})"), 8000,
        &loadLog);
    check(jsAsyncRc == PLUGINXX_RESULT_OK,
          "异步裁决对照示例加载成功 (example_js_async)");
    if (jsAsyncRc != PLUGINXX_RESULT_OK) {
      std::printf("  [info] example_js_async rc=%d log=%s\n", jsAsyncRc,
                  take(loadLog).c_str());
    } else {
      freeStr(loadLog);
    }

    int32_t seekHandlers = 0;
    t_capability_handler_count(
        host, viewCP("musicxx.player.seek"), &seekHandlers, &log);
    check(seekHandlers == 1, "异步裁决处理器已注册 (player.seek = 1)");

    // 1) Promise 30 ms 后结算 → 前链改过的目标位置生效
    {
      MusicxxExternPluginString out{};
      const auto rc = t_capability_dispatch_phase(
          host, viewCP("musicxx.player.seek"),
          MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_BEFORE,
          viewP(R"({"sid":"async1","from":1000,"to":2000})"),
          MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC, 300, &out, &log);
      const std::string result = take(out);
      std::printf("  [info] async seek result=%s\n", result.c_str());
      check(rc == PLUGINXX_RESULT_OK, "异步裁决能力派发成功");
      check(jsonIntField(result, "to") == "12345",
            "Promise 结算的 patch 生效 (to)");
    }

    // 2) Promise 1.5 s 后才结算 → 插件框架一直等, 裁决照样生效 (没有等待超时)
    {
      MusicxxExternPluginString out{};
      const auto t0 = std::chrono::steady_clock::now();
      const auto rc = t_capability_dispatch_phase(
          host, viewCP("musicxx.player.volume"),
          MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_BEFORE,
          viewP(R"({"sid":"async2","from":0.5,"to":0.9})"),
          MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC, 300, &out, &log);
      const std::string result = take(out);
      const auto elapsedMs =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - t0)
              .count();
      std::printf("  [info] async volume result=%s (%lld ms)\n", result.c_str(),
                  static_cast<long long>(elapsedMs));
      check(rc == PLUGINXX_RESULT_OK, "长 Promise 的派发返回成功");
      check(result.find("\"cancel\"") != std::string::npos,
            "插件框架等到 Promise 结算, 裁决生效 (cancel)");
      check(elapsedMs >= 1200, "插件框架等满了 Promise 的等待时间");
    }

    // 3) 结算之后可以继续派发 (处理器没有被跳过)
    {
      MusicxxExternPluginString out{};
      const auto rc = t_capability_dispatch_phase(
          host, viewCP("musicxx.player.volume"),
          MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_BEFORE,
          viewP(R"({"sid":"async3","from":0.1,"to":0.2})"),
          MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC, 300, &out, &log);
      freeStr(out);
      check(rc == PLUGINXX_RESULT_OK, "再次派发成功 (处理器未被跳过)");
    }

    // 4) 统计: 三次派发的 Promise 都结算 (没有超时/迟到这两个概念)
    {
      const CapabilityCallOutcome asyncProbe =
          callCapability(host, "example_js_async", "probe", "{}", 8000);
      const std::string probe = asyncProbe.payload;
      std::printf("  [info] async probe=%s\n", probe.c_str());
      check(asyncProbe.acceptRc == PLUGINXX_RESULT_OK && asyncProbe.ok,
            "异步裁决对照示例能力调用成功 (probe)");
      check(jsonIntField(probe, "asyncDispatchSettled") == "3",
            "统计: 三次派发都在结算后拿到结果");
      check(jsonIntField(probe, "runs") == "3",
            "处理器每次都被调用 (未被跳过)");
      check(probe.find("asyncDispatchTimeouts") == std::string::npos,
            "统计不再有超时字段");
      check(probe.find("asyncDispatchLateDrops") == std::string::npos,
            "统计不再有迟到丢弃字段");
    }

    // 5) 能力统计里没有失败 (慢不等于失败)
    {
      MusicxxExternPluginString stats{};
      const auto rc =
          t_capability_stats(host, &stats, &log);
      const std::string statsJson = take(stats);
      check(rc == PLUGINXX_RESULT_OK, "capability_stats 可读");
      const auto at = statsJson.find("musicxx.player.volume");
      const std::string entry =
          (at == std::string::npos)
              ? std::string{}
              : statsJson.substr(at, statsJson.find(']', at) - at);
      check(entry.find("\"failures\":0") != std::string::npos,
            "慢的异步裁决不计处理器失败");
    }

    // 6) 卸载 → 无残留 (含等待条目)
    {
      check(
          t_plugin_unload(host, viewCP("example_js_async"),
                                              &log) == PLUGINXX_RESULT_OK,
          "异步裁决对照示例卸载成功");
      int32_t seekAfter = 0;
      int32_t volumeAfter = 0;
      t_capability_handler_count(
          host, viewCP("musicxx.player.seek"), &seekAfter, &log);
      t_capability_handler_count(
          host, viewCP("musicxx.player.volume"), &volumeAfter, &log);
      check(seekAfter == 0 && volumeAfter == 0,
            "异步裁决对照示例卸载后无能力残留");
    }
  }

  // 脚本错误安全降级: 非法脚本的插件不应加载成功, 插件框架继续可用
  {
    MusicxxExternPluginString loadLog{};
    const auto rc = t_plugin_load_sync(
        host, viewCP("example_js_broken"), viewCP("{}"), 5000, &loadLog);
    const std::string errText = take(loadLog);
    check(rc != PLUGINXX_RESULT_OK, "脚本错误的 JS 插件加载失败");
    check(rc != PLUGINXX_RESULT_ERR_TIMEOUT,
          "脚本错误是明确失败而非超时");
    std::printf("  [info] example_js_broken rc=%d log=%s\n", rc,
                errText.c_str());
    MusicxxExternPluginString listed{};
    check(t_plugin_list(host, &listed, &log) ==
              PLUGINXX_RESULT_OK,
          "脚本错误后插件框架仍可用");
    freeStr(listed);
  }

  // 插件全部卸载后: UI 项应全部摘除 (无残留)
  {
    MusicxxExternPluginString snap{};
    const auto snapRc = t_ui_snapshot(host, &snap, &log);
    const std::string snapshot = take(snap);
    check(snapRc == PLUGINXX_RESULT_OK, "ui_snapshot 可读 (全部卸载后)");
    check(snapshot.find("plugin.example_native.card") == std::string::npos,
          "动态库插件卸载后 UI 项无残留");
    check(snapshot.find("plugin.example_js.card") == std::string::npos,
          "JS 插件卸载后 UI 项无残留");
  }

  // ==================== 入口符号约定 (对应用例 test_entry_symbols)
  // ====================
  //
  // 对照示例 example_native_bad_entry 的库文件导出了 get_info/create/stop,
  // 但**没有** destroy。 约定要求 create/stop/destroy 三个入口都齐 (create 就是
  // "构造 + 注册事务"), 插件框架必须在 "查找入口符号"阶段就拒绝加载: 明确失败
  // (不是超时)、有可读原因、不留注册残留。
  {
    MusicxxExternPluginString loadLog{};
    const auto rc = t_plugin_load_sync(
        host, viewCP("example_native_bad_entry"), viewCP("{}"), 5000, &loadLog);
    const std::string errText = take(loadLog);
    check(rc != PLUGINXX_RESULT_OK, "缺 destroy 入口的库被拒绝加载");
    check(rc != PLUGINXX_RESULT_ERR_TIMEOUT,
          "拒绝是明确失败而非挂住超时");
    check(errText.find("入口符号") != std::string::npos,
          "失败原因指向缺失的入口符号");
    std::printf("  [info] bad-entry rc=%d log=%s\n", rc, errText.c_str());

    // 未加载 + 未注册任何能力 (此时其它插件都已卸载)
    MusicxxExternPluginString listed{};
    const std::string listJson = [&] {
      t_plugin_list(host, &listed, &log);
      return take(listed);
    }();
    check(listJson.find("example_native_bad_entry") == std::string::npos,
          "缺入口的库不出现在已加载列表");
    int32_t badEntryIds = -1;
    t_capability_handler_count(
        host, viewCP("musicxx.song.changed"), &badEntryIds, &log);
    check(badEntryIds == 0, "缺入口的库没有注册任何能力");
  }

  // ==================== 多目标打包示例 (lib/<系统>-<架构>/)
  // ====================
  //
  // 示例插件 example_native_multi 的库文件放在 `lib/<构建目标标签>/` 里（SDK
  // 助手按 TARGET_TAG auto
  // 摆放）。这里验证：扫描/加载都按当前系统与架构选中分支，并且
  // 加载的确实是那一份构建（插件自报的构建标签与插件框架选中的分支一致）。
  {
    const std::string multiItem = scanItemOf(scanJson, "example_native_multi");
    check(!multiItem.empty(), "扫描发现 example_native_multi (多目标包)");
    check(multiItem.find("\"supported\":true") != std::string::npos,
          "多目标包按当前系统/架构选中分支后判定为可用");
    const std::string selectedTag = jsonStringField(multiItem, "target");
    check(!selectedTag.empty(), "多目标包报告了选中的分支标签");
    check(multiItem.find("\"targetEntry\":\"lib/") != std::string::npos,
          "多目标包的 targetEntry 指向分支目录里的库文件");

    MusicxxExternPluginString loadLog{};
    const auto loadRc = t_plugin_load_sync(
        host, viewCP("example_native_multi"), viewCP("{}"), 10000, &loadLog);
    if (loadRc != PLUGINXX_RESULT_OK) {
      std::printf("  [info] example_native_multi load rc=%d log=%s\n", loadRc,
                  take(loadLog).c_str());
    } else {
      freeStr(loadLog);
    }
    check(loadRc == PLUGINXX_RESULT_OK, "多目标包按分支加载成功");

    const CapabilityCallOutcome whichCall =
        callCapability(host, "example_native_multi",
                       "plugin.example_native_multi.which", "{}", 8000);
    const std::string cap = whichCall.payload;
    check(whichCall.acceptRc == PLUGINXX_RESULT_OK && whichCall.ok,
          "多目标包的能力可调用");
    check(jsonStringField(cap, "tag") == selectedTag && !selectedTag.empty(),
          "加载的正是插件框架选中的分支 (插件自报构建标签与分支标签一致)");

    // 多目标包只讲"打包"一件事: 前链 / 后链的示例已经搬到 `example_native`
    // (那里带演示标记, 不会误伤真实调用)。

    MusicxxExternPluginString unloadLog{};
    t_plugin_unload(host, viewCP("example_native_multi"),
                                        &unloadLog);
    freeStr(unloadLog);
  }

  // ==================== 处理器失败只记统计 (不暂停派发)
  // ====================
  //
  // 对照示例 example_native_fail
  // 在前链注册三个处理器（“改播放值”的三个能力实现固定， 只能注册前链 / 后链）:
  //   musicxx.player.volume   → 每次失败 (返回非 0)
  //   musicxx.player.speed    → 每次成功
  //   musicxx.player.pitch    → 走 musicxx.capabilities 注册的前链
  // 期望: 失败只累计到统计 (capability_stats 的 failures),
  // 处理器每次都照常被调用; 没有"连续失败暂停派发"这回事, 插件也不会被卸载。
  {
    MusicxxExternPluginString loadLog{};
    const auto loadRc = t_plugin_load_sync(
        host, viewCP("example_native_fail"), viewCP("{}"), 8000, &loadLog);
    if (loadRc != PLUGINXX_RESULT_OK) {
      std::printf("  [info] example_native_fail load rc=%d log=%s\n", loadRc,
                  take(loadLog).c_str());
    } else {
      freeStr(loadLog);
    }
    check(loadRc == PLUGINXX_RESULT_OK,
          "失败对照示例加载成功 (example_native_fail)");

    // 同一个插件还有一条**走 `musicxx.capabilities` 注册**的能力（阶段 2c
    // 的新入口）： 注册结果与派发要和 `musicxx.capabilities` 那套完全一样
    {
      int32_t pitchIds = -1;
      t_capability_handler_count(
          host, viewCP("musicxx.player.pitch"), &pitchIds, &log);
      check(pitchIds == 1, "能力表注册的能力出现在处理器计数里");

      MusicxxExternPluginString pitchOut{};
      const auto pitchRc = t_capability_dispatch_phase(
          host, viewCP("musicxx.player.pitch"),
          MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_BEFORE,
          viewP(R"({"from":1.0,"to":0.9})"),
          MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC, 200, &pitchOut, &log);
      const std::string pitchResult = take(pitchOut);
      check(pitchRc == PLUGINXX_RESULT_OK, "能力表注册的能力可派发");
      check(jsonIntField(pitchResult, "called") == "1",
            "能力表注册的处理器被调用");
      check(pitchResult.find("\"pitch\"") != std::string::npos,
            "能力处理器的回答进入合并结果");
      check(pitchResult.find("1.25") != std::string::npos,
            "合并结果就是能力处理器给的值");
    }

    int32_t fixtureIds = -1;
    t_capability_handler_count(
        host, viewCP("musicxx.player.volume"), &fixtureIds, &log);
    check(fixtureIds == 1, "失败处理器的处理器数为 1 (其余插件已卸载)");

    // 连续 5 次派发: 每次都真的调用了处理器 (called=1), 不做任何跳过
    for (int i = 1; i <= 5; ++i) {
      MusicxxExternPluginString out{};
      const auto rc = t_capability_dispatch_phase(
          host, viewCP("musicxx.player.volume"),
          MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_BEFORE,
          viewP(R"({"sid":"brk","from":1.0,"to":0.8})"),
          MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC, 200, &out, &log);
      const std::string result = take(out);
      check(rc == PLUGINXX_RESULT_OK, "失败处理器派发可调用");
      check(jsonIntField(result, "called") == "1",
            "失败处理器每次都被调用 (不跳过)");
    }

    // 处理器级明细来自能力统计接口 `capability_stats`: 只有次数与失败次数
    {
      MusicxxExternPluginString stats{};
      const auto statsRc =
          t_capability_stats(host, &stats, &log);
      const std::string statsJson = take(stats);
      check(statsRc == PLUGINXX_RESULT_OK,
            "capability_stats 可读 (失败统计)");
      check(statsJson.find("\"calls\":5") != std::string::npos,
            "失败处理器被调用 5 次 (每次都派发)");
      check(statsJson.find("\"failures\":5") != std::string::npos,
            "失败次数累计为 5");
      check(statsJson.find("\"paused\"") == std::string::npos,
            "capability_stats 不再有暂停派发标记");
      check(statsJson.find("\"failures\":0") != std::string::npos,
            "同插件的正常处理器无失败记录");
    }

    // 聚合统计 (插件框架/插件维度) 仍然可读: 记录观测数据, 不做任何惩罚
    {
      MusicxxExternPluginString stats{};
      const auto statsRc =
          t_stats(host, nullptr, &stats, &log);
      const std::string statsJson = take(stats);
      check(statsRc == PLUGINXX_RESULT_OK, "stats 可读 (失败统计)");
      check(statsJson.find("example_native_fail") != std::string::npos,
            "聚合统计含失败对照示例插件");
      check(statsJson.find("\"calls\":") != std::string::npos,
            "聚合统计含能力调用次数");
    }

    // 同一插件的另一个处理器照常工作
    {
      MusicxxExternPluginString out{};
      const auto rc = t_capability_dispatch_phase(
          host, viewCP("musicxx.player.speed"),
          MUSICXX_EXTERN_PLUGIN_CAPABILITY_PHASE_BEFORE,
          viewP(R"({"from":1.0,"to":1.25})"),
          MUSICXX_EXTERN_PLUGIN_DISPATCH_SYNC, 200, &out, &log);
      const std::string result = take(out);
      check(rc == PLUGINXX_RESULT_OK, "同插件的另一个处理器派发成功");
      check(jsonIntField(result, "called") == "1", "另一个处理器不受影响");
    }

    // 插件本身仍在加载状态
    {
      const CapabilityCallOutcome failProbe =
          callCapability(host, "example_native_fail",
                         "plugin.example_native_fail.probe", "{}", 8000);
      const std::string probeJson = failProbe.payload;
      check(failProbe.acceptRc == PLUGINXX_RESULT_OK && failProbe.ok,
            "失败之后插件仍可响应能力调用 (未被卸载)");
      check(jsonIntField(probeJson, "failingCalls") == "5",
            "失败处理器被调用 5 次 (全部派发)");
      check(jsonIntField(probeJson, "goodCalls") == "1",
            "正常处理器按预期被调用 1 次");
    }

    // 卸载后无残留
    {
      check(t_plugin_unload(
                host, viewCP("example_native_fail"), &log) ==
                PLUGINXX_RESULT_OK,
            "失败对照示例卸载成功");
      int32_t after = -1;
      t_capability_handler_count(
          host, viewCP("musicxx.player.volume"), &after, &log);
      check(after == 0, "失败对照示例卸载后无能力残留");
      int32_t pitchAfter = -1;
      t_capability_handler_count(
          host, viewCP("musicxx.player.pitch"), &pitchAfter, &log);
      check(pitchAfter == 0, "能力表注册的能力随插件卸载一起摘除");
    }
  }

  // ==================== 广播 (发布 / 读当前值 / 列频道) ====================
  //
  // 广播只有四件事: 声明 (宿主按生成表补)、发布、读当前值、订阅 (插件侧)。
  // 应用侧能用的入口是 broadcast_publish / broadcast_read / broadcast_list。
  {
    // 能力位: 本次构建必须带广播通道
    {
      const int32_t bits = musicxx_extern_plugin_feature_bits();
      check((bits & MUSICXX_EXTERN_PLUGIN_FEATURE_BROADCASTS) != 0,
            "feature_bits 含广播通道");
      check((bits & MUSICXX_EXTERN_PLUGIN_FEATURE_CAPABILITIES) != 0,
            "feature_bits 含能力通道");
    }

    // 未声明的频道: 读要明确报"未声明"
    {
      MusicxxExternPluginString out{};
      const int32_t readRc = t_broadcast_read(
          host, viewCP("musicxx.test.missing"), &out, &log);
      take(out);
      check(readRc == PLUGINXX_RESULT_ERR_NOT_FOUND,
            "读未声明的频道返回未找到");
    }

    // 生成表里有当前值的频道: 发布之后读回同一份
    {
      const std::string topic = "musicxx.player.position";
      const std::string value = R"({"positionMs":1234})";
      const int32_t pubRc = t_broadcast_publish(
          host, viewP(topic), viewP(value), &log);
      check(pubRc == PLUGINXX_RESULT_OK, "发布官方频道成功");
      MusicxxExternPluginString out{};
      const int32_t readRc =
          t_broadcast_read(host, viewP(topic), &out, &log);
      check(readRc == PLUGINXX_RESULT_OK, "读官方频道当前值成功");
      check(take(out) == value, "读回的当前值是最后一次发布的内容");
    }

    // 表外 id: 默认没有当前值 (cache: none), 发布成功但读不到
    {
      const std::string topic = "musicxx.test.plain";
      check(t_broadcast_publish(
                host, viewP(topic), viewCP(R"({"v":1})"), &log) ==
                PLUGINXX_RESULT_OK,
            "发布表外频道时按默认属性补声明");
      MusicxxExternPluginString out{};
      const int32_t readRc =
          t_broadcast_read(host, viewP(topic), &out, &log);
      take(out);
      check(readRc == PLUGINXX_RESULT_ERR_ARG,
            "没有当前值的频道读值返回参数错误");
    }

    // 插件命名空间的频道由插件自己发: 应用侧发布要被拒
    {
      const int32_t rc = t_broadcast_publish(
          host, viewCP("plugin.someone.tip"), viewCP("{}"), &log);
      check(rc == PLUGINXX_RESULT_ERR_PERMISSION,
            "应用侧不能发布插件命名空间的频道");
    }

    // 列出来: 声明字段与当前值都在, 前缀能过滤
    {
      MusicxxExternPluginString out{};
      const int32_t rc = t_broadcast_list(
          host, viewCP("musicxx.player.position"), &out, &log);
      const std::string json = take(out);
      check(rc == PLUGINXX_RESULT_OK, "broadcast_list 受理");
      check(json.find("\"broadcasts\"") != std::string::npos,
            "列表带 broadcasts 段");
      check(json.find("musicxx.player.position") != std::string::npos,
            "前缀过滤命中该频道");
      check(json.find("musicxx.test.plain") == std::string::npos,
            "前缀过滤排除别的频道");
      check(json.find("\"cache\":\"latest\"") != std::string::npos,
            "列表给出缓存属性");
      check(json.find("\"publish\":\"owner\"") != std::string::npos,
            "列表给出谁能发布");
      check(json.find("\"hasValue\":true") != std::string::npos,
            "列表给出有没有当前值");
      check(json.find("\"subscribers\":") != std::string::npos,
            "列表给出订阅数");
      check(json.find("\"value\"") != std::string::npos, "列表带上当前值");
    }

    // 调试信息里能看到广播表 (排障用)
    {
      MusicxxExternPluginString debug{};
      t_debug_info(host, &debug, &log);
      const std::string json = take(debug);
      check(json.find("\"broadcasts\"") != std::string::npos,
            "调试信息含广播表");
    }
  }

  // ==================== 能力处理器列表 (自省查询 capability_handlers)
  // ====================
  //
  // 自省要回答"这个能力现在有谁在提供、谁先执行": 列表按**调用顺序**给出
  // (priority 升序, 同优先级按注册顺序), 只暴露 plugin / instance / kind /
  // priority / ownerTag ——
  // 不暴露处理器地址、载荷与内部状态。主程序把它写进功能埋点自省, 插件侧用动作
  // `musicxx.capability.subscribers` 读同一份数据。
  {
    // 先归一状态: 这两个插件在前面的用例里可能还装着
    t_plugin_unload(host, viewCP("example_native"), &log);
    t_plugin_unload(host, viewCP("example_js"), &log);

    MusicxxExternPluginString nativeLoadLog{};
    const auto nativeRc = t_plugin_load_sync(
        host, viewCP("example_native"), viewCP("{}"), 8000, &nativeLoadLog);
    if (nativeRc != PLUGINXX_RESULT_OK) {
      std::printf("  [info] example_native load rc=%d log=%s\n", nativeRc,
                  take(nativeLoadLog).c_str());
    } else {
      freeStr(nativeLoadLog);
    }
    check(nativeRc == PLUGINXX_RESULT_OK, "自省用例加载动态库插件");

    MusicxxExternPluginString jsLoadLog{};
    const auto jsRc = t_plugin_load_sync(
        host, viewCP("example_js"), viewCP(R"({"enabled":true})"), 15000,
        &jsLoadLog);
    if (jsRc != PLUGINXX_RESULT_OK) {
      std::printf("  [info] example_js load rc=%d log=%s\n", jsRc,
                  take(jsLoadLog).c_str());
    } else {
      freeStr(jsLoadLog);
    }
    check(jsRc == PLUGINXX_RESULT_OK, "自省用例加载 JS 插件");

    // 预设表里没有的能力 id：查询得到 0 个处理器（不自报“没找到”）
    {
      MusicxxExternPluginString out{};
      const auto rc = t_capability_handlers(
          host, viewCP("musicxx.no.such.capability2"), &out, &log);
      const std::string json = take(out);
      check(rc == PLUGINXX_RESULT_OK, "自定义能力查询返回成功");
      check(jsonIntField(json, "count") == "0", "自定义能力无处理器");
    }

    // 两种形态同时在: 列表给出两个处理器, 并带上 kind 与实例名
    {
      MusicxxExternPluginString out{};
      const auto rc = t_capability_handlers(
          host, viewCP("musicxx.player.beforePlaySong"), &out, &log);
      const std::string json = take(out);
      check(rc == PLUGINXX_RESULT_OK, "capability_handlers 可读");
      check(jsonStringField(json, "capability") ==
                "musicxx.player.beforePlaySong",
            "回报的是同一个能力 id");
      check(jsonIntField(json, "count") == "2", "两种形态的处理器都在列表里");
      check(json.find("\"kind\":\"native\"") != std::string::npos,
            "列表含原生处理器");
      check(json.find("\"kind\":\"js\"") != std::string::npos,
            "列表含 JS 处理器");
      check(json.find("\"plugin\":\"example_native\"") != std::string::npos,
            "原生处理器给出插件 id");
      check(json.find("\"instance\":\"js:example_js\"") != std::string::npos,
            "JS 处理器用插件框架的实例名");
      check(json.find("\"ownerTag\":") != std::string::npos,
            "列表带 ownerTag 字段");

      // 列表按调用顺序: priority 必须单调不减
      const std::string priorityKey = "\"priority\":";
      std::vector<int> priorities;
      for (size_t pos = json.find(priorityKey); pos != std::string::npos;
           pos = json.find(priorityKey, pos + priorityKey.size())) {
        size_t at = pos + priorityKey.size();
        const bool negative = at < json.size() && json[at] == '-';
        if (negative) {
          ++at;
        }
        int value = 0;
        for (; at < json.size() && json[at] >= '0' && json[at] <= '9'; ++at) {
          value = value * 10 + (json[at] - '0');
        }
        priorities.push_back(negative ? -value : value);
      }
      bool ordered = priorities.size() == 2;
      for (size_t i = 1; i < priorities.size(); ++i) {
        if (priorities[i] < priorities[i - 1]) {
          ordered = false;
        }
      }
      check(ordered, "处理器按调用顺序给出 (priority 升序)");

      // 与计数接口一致: 列表长度 = capability_handler_count
      int32_t counted = -1;
      t_capability_handler_count(
          host, viewCP("musicxx.player.beforePlaySong"), &counted, &log);
      check(counted == 2, "capability_handler_count 与列表长度一致");
    }

    // 禁用 JS 插件 → 列表只剩原生那一条; 重新启用 → 又回来
    {
      check(t_plugin_disable(
                host, viewCP("example_js"), &log) == PLUGINXX_RESULT_OK,
            "禁用 JS 插件");
      MusicxxExternPluginString out{};
      t_capability_handlers(
          host, viewCP("musicxx.player.beforePlaySong"), &out, &log);
      const std::string json = take(out);
      check(jsonIntField(json, "count") == "1", "禁用后列表只剩一个处理器");
      check(json.find("\"kind\":\"js\"") == std::string::npos,
            "禁用后 JS 处理器从列表里消失");

      check(t_plugin_enable(
                host, viewCP("example_js"), &log) == PLUGINXX_RESULT_OK,
            "重新启用 JS 插件");
      std::this_thread::sleep_for(std::chrono::milliseconds{300});
      MusicxxExternPluginString out2{};
      t_capability_handlers(
          host, viewCP("musicxx.player.beforePlaySong"), &out2, &log);
      const std::string json2 = take(out2);
      check(jsonIntField(json2, "count") == "2", "启用后列表恢复两个处理器");
    }

    // 收尾: 两个插件都放掉 (后面还有"卸载要释放动态库"的用例)
    t_plugin_unload(host, viewCP("example_js"), &log);
    t_plugin_unload(host, viewCP("example_native"), &log);
    MusicxxExternPluginString empty{};
    t_capability_handlers(
        host, viewCP("musicxx.player.beforePlaySong"), &empty, &log);
    const std::string emptyJson = take(empty);
    check(jsonIntField(emptyJson, "count") == "0",
          "都卸载后能力上没有处理器 (列表为空)");
  }

  // ============ 卸载 = 释放动态库: 卸载后的插件文件要能删能换 (回归)
  // ============
  //
  // 卸载要把实例与动态库一起放掉: 之后插件目录必须能整个删掉
  // (管理页的"彻底卸载") 或把库文件换成新构建的那一份 (改完代码
  // reload/更新插件都靠这一步)。Windows 上
  // 插件框架没关掉动态库句柄时文件一直被映射着, 删除/替换只会拿到"拒绝访问"。
  {
    std::error_code ec;
    const std::filesystem::path source =
        std::filesystem::path(pluginDir) / "example_native";
    const std::filesystem::path probeRoot =
        std::filesystem::temp_directory_path(ec) / "musicxx_ext_unload_probe";
    const std::filesystem::path probe = probeRoot / "example_native";
    removePath(probeRoot);
    std::filesystem::create_directories(probe, ec);
    if (!ec) {
      std::filesystem::copy(
          source, probe,
          std::filesystem::copy_options::recursive |
              std::filesystem::copy_options::overwrite_existing,
          ec);
    }
    check(!ec, "准备卸载用插件副本");
    if (ec) {
      std::printf("  [info] 复制失败原因: %s\n", ec.message().c_str());
    }
    if (!ec) {
      /// 同名实例只有一个: 先放下常规目录里那一份, 免得按 id 卸载打到别的实例
      t_plugin_unload(host, viewCP("example_native"), &log);

      const std::string probePath = probe.string();
      MusicxxExternPluginString probeLog{};
      const auto probeRc = t_plugin_load_sync(
          host, viewCP(probePath.c_str()), viewCP("{}"), 8000, &probeLog);
      check(probeRc == PLUGINXX_RESULT_OK, "副本插件加载成功");
      if (probeRc != PLUGINXX_RESULT_OK) {
        std::printf("  [info] 副本加载失败原因: %s\n", take(probeLog).c_str());
      } else {
        freeStr(probeLog);
      }
      check(t_plugin_unload(host, viewCP("example_native"),
                                                &log) ==
                PLUGINXX_RESULT_OK,
            "副本插件卸载成功");

      ec.clear();
      std::filesystem::remove_all(probe, ec);
      if (ec) {
        checkText(false, "卸载后插件目录能删除被拒绝 (动态库句柄没释放): " +
                             ec.message());
      } else {
        checkText(true, "卸载后插件目录能删除");
      }
      removePath(probeRoot);
    }
  }

  // 插件框架停机 ⇒ 还没回来的能力调用按 `host_stopped` 收尾 (同样不能悬挂)
  //
  // 这一段必须放在最后: 它之后插件框架就停了。
  // 用的能力是 `asyncProbe`(协程异步完成): 等待时长给得比停机预算长 ——
  // 停机等不到它, 于是这条调用在停机时仍然是"未完成", 正好验证收尾那一步。
  // (能力的等待由协程自己的计时器驱动, 不占住插件框架线程;
  // 停机收尾之后它随插件框架一起收场)
  {
    MusicxxExternPluginString reloadLog{};
    const auto reloadRc = t_plugin_load_sync(
        host, viewCP("example_native"), viewCP("{}"), 10000, &reloadLog);
    freeStr(reloadLog);
    check(reloadRc == PLUGINXX_RESULT_OK, "为停机用例加载动态库插件");

    MusicxxExternPluginString callLog{};
    int64_t requestId = 0;
    const auto rc = t_plugin_call_async(
        host, viewCP("example_native"), viewCP("asyncProbe"),
        viewCP(R"({"waitMs":20000})"), &requestId, &callLog);
    freeStr(callLog);
    check(rc == PLUGINXX_RESULT_OK, "停机前受理一个异步完成的能力调用");
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    // 停机预算给得比能力的等待短: 停机不会一直等它 (日志里会记一条)
    check(t_host_stop(host, 1500, &log) ==
              PLUGINXX_RESULT_OK,
          "host_stop");

    // 停机时推的那条终局事件: error 说的是 host_stopped
    const std::string stopPayload = waitCapabilityResult(host, requestId, 3000);
    const bool sawStopped =
        stopPayload.find("host_stopped") != std::string::npos;
    if (!sawStopped) {
      std::printf("  [info] stop payload=%s\n", stopPayload.c_str());
    }
    check(sawStopped, "插件框架停机给未完成的调用终局 (host_stopped)");
  }

  check(t_host_stop(host, 1000, &log) ==
            PLUGINXX_RESULT_OK,
        "host_stop 幂等");
  musicxx_extern_plugin_host_destroy(host);

  std::printf("\nchecks=%d failed=%d\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
