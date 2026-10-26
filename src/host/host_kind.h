/// 插件形态（kind）注册表
///
/// 背景：插件框架自己只实现**动态库插件**，JS 只是它内置的一种运行器。运行器不该写死在
/// 代码里 —— 把 JS 运行时做成动态库插件、以后接 Python 运行器，都不应该改扫描 / 加载 /
/// 判定的分支。因此 `plugin.yaml` 里的 `kind` 一律先查这张表：
/// - 每一项声明：谁提供（插件框架自己 / 插件实例名）、这个形态自己的**接口代次对**
///   （当前代次 + 兼容下限，清单的 `api_version` 按它判）、交给提供者加载 / 卸载 / 校验的
///   能力全名、一行说明；
/// - 框架自带两项：`native`（插件框架自己加载动态库，按 C 接口代次判：当前 3、下限 3）与
///   `js`（内置 JS 运行器，按 JS 接口代次判：当前 2、**下限仍 1** —— 至今只增不改）；
/// - 插件通过接口表 `musicxx.kind` 登记自己的形态；**内置形态可以被插件接管**
///   （`overrideBuiltin`，例如把 JS 运行器搬到插件里），撤销后自动回到内置实现；
/// - 跟着插件生命周期撤销（禁用 / 卸载 / 关闭『拟声++』），不写磁盘。
///
/// 判定"由谁实现"只看 `provider`：空 = 插件框架自己，非空 = 由这个插件托管。
///
/// "这个包能不能作为这种形态跑"也由提供者回答（`validateCapability`，可选）：
/// 框架不按形态名写分支，只负责把目录交给提供者，拿到拒绝理由就原样告诉用户。
///
/// 线程约定：只在插件框架 IO 线程读写（与其它插件注册同一串行上下文）。
#pragma once

#include "musicxx/plugin/api/plugin_api.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace musicxx {
namespace extern_plugin {

/// 内置 JS 运行器的接口代次对（与 C 接口代次是**两套独立编号**，不要互相比较）
///
/// - 当前代次 `jsInterfaceVersion = 2`：2026-10 的增补（变量通道、网络能力、功能点调用、
///   HTTP 静态路由、界面描述层 v2 …）—— **全是新增**，所以下限不动；
/// - 兼容下限 `jsMinInterfaceVersion = 1`：最初的 JS 接口。只有发生**破坏性变更**
///   （改名 / 删接口 / 改语义）时才把它抬到当时的当前代次，与 C 接口的 `PLUGINXX_MIN_API_VERSION`
///   是同一套规则。
inline constexpr int32_t jsInterfaceVersion = 2;
inline constexpr int32_t jsMinInterfaceVersion = 1;

/// 一种插件形态的声明
struct MusicxxPluginKindSpec {
  /// 形态名（清单里的 `kind`；小写字母、数字、`_` `-` `.`）
  std::string kind;

  /// 是否是框架自带的形态名（`native` / `js`）。接管内置形态时仍为 true，
  /// 但 `provider` 会写上接管它的插件。
  bool builtin = false;

  /// 该形态的插件是否按 C ABI 编译
  /// - true：插件是动态库，会调通用接口表（接口代次对直接用框架的 C 接口那一对）；
  /// - false：插件由某个运行器解释执行（JS / Python …），不碰 C 接口，用自己声明的代次对。
  ///
  /// 它同时决定要不要按动态库的样子解析多目标分支（见 `host_inspect.h`）。
  bool requiresPluginAbi = true;

  /// 该形态的**接口代次**（框架 / 运行器上报给插件；`0` = 未声明）
  int32_t interfaceVersion = 0;

  /// 该形态能接受的**最低**接口代次（清单 `api_version` 低于它 = 这个插件跑不了；
  /// `0` = 不判）。只有破坏性变更才抬它。
  int32_t minInterfaceVersion = 0;

  /// 提供者（插件实例名；空 = 插件框架自己实现）
  std::string provider;

  /// 交给提供者加载 / 卸载一个该形态插件的能力全名
  /// （必须在提供者自己的命名空间 `plugin.<插件id>.` 下；空 = 未声明，加载会被拒绝）
  std::string loadCapability;
  std::string unloadCapability;

  /// 交给提供者的**校验**能力全名（可选；同样要在提供者自己的命名空间下）
  ///
  /// 加载一个该形态的插件之前先问它一次：回答 `{"ok":true}` = 可以跑，
  /// `{"ok":false,"error":"..."}` = 拒绝（理由直接给用户）。空 = 不做这一步。
  ///
  /// 安装预检也读这里：`plugin_inspect` 是同步 C ABI，回答不了"要问插件"的问题，
  /// 所以由主程序拿报出的能力名自己去问提供者（见 `host_inspect.h` 的
  /// `kindValidateCapability`）。
  std::string validateCapability;

  /// 一行说明（自省与管理页展示）
  std::string describe;

  /// 是否由插件框架自己实现（`provider` 为空）
  bool frameworkOwned() const { return provider.empty(); }
};

/// 形态注册表（一个插件框架实例一份）
class MusicxxPluginKindRegistry {
public:
  MusicxxPluginKindRegistry() {
    // 内置形态：native 由插件框架自己加载，按 C 接口代次判（当前 3 / 下限 3）
    MusicxxPluginKindSpec native;
    native.kind = "native";
    native.builtin = true;
    native.requiresPluginAbi = true;
    native.interfaceVersion = MUSICXX_PLUGIN_API_VERSION;
    native.minInterfaceVersion = MUSICXX_PLUGINXX_MIN_API_VERSION;
    native.describe = "动态库插件：由插件框架自己加载，调用通用接口表";
    builtins_.push_back(std::move(native));
    // 内置形态：js 由内置 JS 运行器解释执行，按 JS 接口代次判
    // （当前 2 / 下限 1：至今只增不改，破坏性变更时才抬下限）
    MusicxxPluginKindSpec js;
    js.kind = "js";
    js.builtin = true;
    js.requiresPluginAbi = false;
    js.interfaceVersion = jsInterfaceVersion;
    js.minInterfaceVersion = jsMinInterfaceVersion;
    js.describe = "JS 插件：由内置 JS 运行器解释执行，不调用 C 接口";
    builtins_.push_back(std::move(js));
  }

  /// 登记一种形态（插件侧）
  ///
  /// - `overrideBuiltin = true` 时允许接管框架自带的形态（例如把 JS 运行器搬进插件）：
  ///   `requiresPluginAbi` 与接口代次对沿用内置声明（解释执行的形态不让接管者改代次），
  ///   撤销后回到内置实现；
  /// - 解释执行的形态自己声明 `interfaceVersion` / `minInterfaceVersion`（不声明 = 不判）；
  ///   会调 C 接口表的形态（`requiresPluginAbi`）一律用框架的 C 接口那一对；
  /// - 同一个提供者重复登记 = 幂等覆盖；别的插件已占用同名形态则拒绝。
  bool registerKind(const MusicxxPluginKindSpec &spec, bool overrideBuiltin,
                    std::string &err) {
    if (!isValidKindName(spec.kind)) {
      err = "插件形态名非法 (只允许小写字母、数字、下划线、中划线、点, 长度 1~32)";
      return false;
    }
    if (spec.provider.empty()) {
      err = "插件形态 `" + spec.kind + "` 缺少提供者";
      return false;
    }
    if (spec.loadCapability.empty()) {
      err = "插件形态 `" + spec.kind + "` 缺少 loadCapability";
      return false;
    }
    MusicxxPluginKindSpec entry = spec;
    if (const MusicxxPluginKindSpec *builtin = findBuiltin(spec.kind)) {
      if (!overrideBuiltin) {
        err = "插件形态 `" + spec.kind +
              "` 是插件框架内置形态, 接管它要把 overrideBuiltin 写成 true";
        return false;
      }
      entry.builtin = true;
      // 形态本身是否按 C ABI 编译、接口代次对都由框架决定，不让接管者改
      entry.requiresPluginAbi = builtin->requiresPluginAbi;
      entry.interfaceVersion = builtin->interfaceVersion;
      entry.minInterfaceVersion = builtin->minInterfaceVersion;
      // 只能接管“解释执行”的内置形态（js）：native 的动态库加载与生命周期
      // 是插件框架自己的职责，换出去就没法保证注入/卸载纪律
      if (builtin->requiresPluginAbi) {
        err = "插件形态 `" + spec.kind +
              "` 是动态库形态, 不能由插件接管";
        return false;
      }
    } else if (entry.requiresPluginAbi) {
      // 会调 C 接口表的形态（运行器自己编插件）：接口代次对就是 C 接口那一对，
      // 不让登记方随便写（写了也不采信）
      entry.interfaceVersion = MUSICXX_PLUGIN_API_VERSION;
      entry.minInterfaceVersion = MUSICXX_PLUGINXX_MIN_API_VERSION;
    }
    // 解释执行的形态自己声明代次对：下限不能高于当前代次（未声明 = 不判）
    if (entry.interfaceVersion < 0 || entry.minInterfaceVersion < 0) {
      err = "插件形态 `" + spec.kind + "` 的接口代次不能是负数";
      return false;
    }
    if (entry.interfaceVersion > 0 &&
        entry.minInterfaceVersion > entry.interfaceVersion) {
      err = "插件形态 `" + spec.kind +
            "` 的 minInterfaceVersion 不能大于 interfaceVersion";
      return false;
    }
    if (const MusicxxPluginKindSpec *existing = findOverride(spec.kind)) {
      if (existing->provider != spec.provider) {
        err = "插件形态 `" + spec.kind + "` 已由插件 `" + existing->provider +
              "` 提供";
        return false;
      }
    }
    for (auto &item : overrides_) {
      if (item.kind == spec.kind) {
        item = std::move(entry);
        return true;
      }
    }
    overrides_.push_back(std::move(entry));
    return true;
  }

  /// 撤销一种形态（只能撤销自己登记的；接管内置形态的撤销后自动回到内置实现）
  bool unregisterKind(std::string_view kind, std::string_view provider,
                      std::string &err) {
    const auto it = std::find_if(overrides_.begin(), overrides_.end(),
                                 [&](const MusicxxPluginKindSpec &item) {
                                   return item.kind == kind;
                                 });
    if (it == overrides_.end() || it->provider != provider) {
      err = "插件形态 `" + std::string{kind} + "` 不是本插件登记的";
      return false;
    }
    overrides_.erase(it);
    return true;
  }

  /// 插件停用 / 卸载：撤销它登记的全部形态
  void unregisterAllOf(std::string_view provider) {
    overrides_.erase(std::remove_if(overrides_.begin(), overrides_.end(),
                                    [&](const MusicxxPluginKindSpec &item) {
                                      return item.provider == provider;
                                    }),
                     overrides_.end());
  }

  /// 查一种形态（未登记返回 nullptr）；插件接管了内置形态时给出接管后的那一项
  const MusicxxPluginKindSpec *find(std::string_view kind) const {
    if (const MusicxxPluginKindSpec *override = findOverride(kind)) {
      return override;
    }
    return findBuiltin(kind);
  }

  /// 全部形态（框架自带的在前；被接管的显示接管后的那一项）
  std::vector<MusicxxPluginKindSpec> all() const {
    std::vector<MusicxxPluginKindSpec> result;
    result.reserve(builtins_.size() + overrides_.size());
    for (const MusicxxPluginKindSpec &builtin : builtins_) {
      const MusicxxPluginKindSpec *override = findOverride(builtin.kind);
      result.push_back(override ? *override : builtin);
    }
    for (const MusicxxPluginKindSpec &item : overrides_) {
      if (!item.builtin) {
        result.push_back(item);
      }
    }
    return result;
  }

  /// 该形态的插件是否按 C ABI 编译
  /// - **未登记的形态按 true 算**：宁可要求插件声明够新的接口版本，也不要放进来一个
  ///   我们不知道它怎么编译的二进制。
  bool requiresPluginAbi(std::string_view kind) const {
    const MusicxxPluginKindSpec *spec = find(kind);
    return spec ? spec->requiresPluginAbi : true;
  }

  /// 该形态能接受的最低接口代次（清单 `api_version` 低于它 = 跑不了）
  ///
  /// - 已登记但没声明（`0`）= 不判；
  /// - **未登记的形态按 C 接口下限算**，与 [requiresPluginAbi] 同一个理由。
  int32_t minInterfaceVersion(std::string_view kind) const {
    const MusicxxPluginKindSpec *spec = find(kind);
    return spec ? spec->minInterfaceVersion : MUSICXX_PLUGINXX_MIN_API_VERSION;
  }

  /// 该形态当前上报的接口代次（插件读 `host.info().apiVersion` / `config().apiVersion` 看到的值）
  ///
  /// 未登记或未声明的形态回退到 C 接口代次。
  int32_t interfaceVersion(std::string_view kind) const {
    const MusicxxPluginKindSpec *spec = find(kind);
    return (spec && spec->interfaceVersion > 0) ? spec->interfaceVersion
                                                : MUSICXX_PLUGIN_API_VERSION;
  }

  /// 形态名是否合法（清单字段与接口表共用这一份规则）
  static bool isValidKindName(std::string_view kind) {
    if (kind.empty() || kind.size() > 32) {
      return false;
    }
    for (const char c : kind) {
      const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                      c == '_' || c == '-' || c == '.';
      if (!ok) {
        return false;
      }
    }
    return true;
  }

private:
  const MusicxxPluginKindSpec *findBuiltin(std::string_view kind) const {
    const auto it = std::find_if(builtins_.begin(), builtins_.end(),
                                 [&](const MusicxxPluginKindSpec &item) {
                                   return item.kind == kind;
                                 });
    return it == builtins_.end() ? nullptr : &*it;
  }

  const MusicxxPluginKindSpec *findOverride(std::string_view kind) const {
    const auto it = std::find_if(overrides_.begin(), overrides_.end(),
                                 [&](const MusicxxPluginKindSpec &item) {
                                   return item.kind == kind;
                                 });
    return it == overrides_.end() ? nullptr : &*it;
  }

  /// 框架自带的形态（`native` / `js`）
  std::vector<MusicxxPluginKindSpec> builtins_;
  /// 插件登记的形态（含接管内置形态的那一项）
  std::vector<MusicxxPluginKindSpec> overrides_;
};

} // namespace extern_plugin
} // namespace musicxx
