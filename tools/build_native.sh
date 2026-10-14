#!/usr/bin/env bash
# musicxx_extern_plugin 原生构建脚本 (Linux / macOS)
#
# 与 Windows 的 tools/build_native.ps1 一一对应: 只负责"环境准备 + 调 cmake",
# 依赖库编译与插件框架工程构建全部交给 src/CMakeLists.txt (superbuild + ExternalProject_Add):
#
#   src/CMakeLists.txt       顶层 superbuild: 推导 XX_IS_* 宏 → 生成 BoostConfig.cmake →
#                            ExternalProject_Add 逐个构建 third_party 依赖 → 构建插件框架工程
#   src/host/CMakeLists.txt  嵌套插件框架工程: find_package 取依赖 → 插件框架库 / 插件 SDK / 示例插件 / 原生测试
#
# 本脚本负责的只有 cmake 不方便做的事:
#   - 环境预检 (cmake / git / 编译器版本 / pkg-config), 子模块是否已初始化
#   - Boost 头文件的解析/复用/下载解包 (只用头文件, 不链接 Boost 编译库)
#   - 按 --config 传 XX_IS_RELEASE_D / CMAKE_BUILD_TYPE / CMAKE_CONFIGURATION_TYPES
#   - 按平台传最低版本 (macOS 用 --macos-target, iOS 用 --ios-target), 让产物与主程序的最低版本一致
#   - 构建完成后把产物复制到稳定的输出目录, 可选跑原生测试
#
# 用法 (选项名不区分大小写, --deps-only 与 -DepsOnly 等价):
#   tools/build_native.sh                            # 插件框架: Release 插件框架库 + 插件 SDK
#   tools/build_native.sh --with-examples            # 连示例插件一起构建 (只构建; 构建 + 打包用 tools/build_examples.sh)
#   tools/build_native.sh --with-tests               # 连原生测试一起构建 (含对照示例)
#   tools/build_native.sh --config Debug             # Debug (独立构建目录, 依赖也重编)
#   tools/build_native.sh --deps-only                # 只构建依赖库
#   tools/build_native.sh --configure-only           # 只做预检/Boost/configure
#   tools/build_native.sh --clean                    # 先清空构建目录
#   tools/build_native.sh --run-tests                # 构建后跑原生测试 (失败返回非 0; 隐含示例与测试)
#   tools/build_native.sh --jobs 8                   # 指定并行度
#   tools/build_native.sh --auto                     # 自动模式(应用构建调用): 已是最新则秒退, 失败不阻断
#   tools/build_native.sh --skip-if-fresh            # 已是最新就跳过(失败仍然报错; 示例插件脚本用)
#   tools/build_native.sh --boost-root <目录>        # 指定 Boost 头文件根 (含 boost/)
#   tools/build_native.sh --boost-archive <压缩包>   # 用本地 Boost 发布包解包
#   tools/build_native.sh --no-download-boost        # 禁止自动下载 Boost 头文件
#   tools/build_native.sh --build-dir/--output-dir/--generator <值>   # 覆盖目录与生成器
#   tools/build_native.sh --android --abi arm64-v8a  # 用 NDK 交叉编译 Android 插件框架库
#   tools/build_native.sh --ios                      # 交叉编译 iOS 设备 (arm64) 插件框架库
#   tools/build_native.sh --ios-simulator            # 交叉编译 iOS 模拟器 (开发机架构) 插件框架库
#   tools/build_native.sh --ios-target 15.0          # iOS 最低版本 (默认 15.0, 与主程序一致)
#   tools/build_native.sh --macos-target 12.0        # macOS 最低版本 (默认 12.0, 与主程序一致)
#
# 关于自动模式 (--auto): Windows 侧的 tools/build_native.ps1 与它同等地位, 都是给
# "flutter build" 触发的入口 —— 环境/Boost 复用已有缓存, 已是最新就立即返回, 构建失败只警告
# (外部插件是可选功能, 不该让应用构建失败)。
#
# Android 说明 (与 .ps1 的 -Android 一一对应):
#   - 产出: <包>/.native/output/android-<abi>-<配置>/{bin,plugins} (ABI 名直接用作架构段)
#   - NDK 位置: --ndk > $ANDROID_NDK_HOME / $ANDROID_NDK_ROOT > $ANDROID_SDK_ROOT|$ANDROID_HOME/ndk 下版本最大者
#   - 统一 Ninja 生成器与 c++_static (C++ 运行库静态链进插件框架库, APK 不需要带 libc++_shared.so)
#   - --run-tests 在 Android 上跳过 (测试可执行文件是给设备编的)
#
# iOS 说明 (只在 macOS 上可用; Windows 侧的 .ps1 不支持 —— 它没有 iOS 工具链):
#   - 由 dart 侧/cmake 的 CMAKE_SYSTEM_NAME=iOS 交叉编译, sysroot 用 --ios (iphoneos) 或
#     --ios-simulator (iphonesimulator), 架构: 设备固定 arm64, 模拟器用开发机架构
#   - 最低版本用 --ios-target 给 (默认 15.0), 主程序构建时由构建阶段传自己的 IPHONEOS_DEPLOYMENT_TARGET
#   - 产出: <包>/.native/output/ios-arm64-<配置>/{bin,plugins} 与
#           <包>/.native/output/ios-simulator-<架构>-<配置>/{bin,plugins}
#   - 插件框架库随 iOS 应用包分发: 主程序 ios/ 的 Runner 构建阶段 (musicxx-extern-plugin →
#     ios/tool/embed_extern_plugin_dylib.sh) 调本脚本的自动模式, 把它复制进
#     Runner.app/Frameworks/ 并补签名 (iOS 只允许加载应用包内、与主程序同签名身份的动态库)
#   - --run-tests 在 iOS 设备目标上跳过 (测试可执行文件是给设备编的); 模拟器目标可以直接在
#     开发机上跑 (同一个 CPU 架构), 因此照常执行
#
# macOS 说明:
#   - 最低版本用 --macos-target 给 (默认 12.0), 主程序构建时由构建阶段传自己的
#     MACOSX_DEPLOYMENT_TARGET; 不传的话 CMake 会用"构建机当前的系统版本",
#     编出来的库会按"只在同样新的系统上运行"编译
#
# 产物布局 (与 .ps1 一致):
#   <包>/.native/build/<平台>-<配置>/musicxx-extern-plugin-install/{bin,include,lib,plugins}
#   <包>/.native/output/<平台>-<架构>-<配置>/{bin,plugins}     稳定输出目录
#   Android 的平台-架构段用 android-<abi>: build/android-arm64-v8a-release、output/android-arm64-v8a-release
#   iOS 的平台段是 ios (设备) / ios-simulator (模拟器): build/ios-release、output/ios-simulator-arm64-release
set -euo pipefail

step() { printf '\n==> %s\n' "$1"; }
info() { printf '  %s\n' "$1"; }
# 说明: 警告与错误一律走 stderr; 供"用 stdout 返回字符串"的函数内部也用 stderr 报进度,
# 否则调用方 $(...) 捕获到的值会被提示文字污染。
warn() { printf '  [警告] %s\n' "$1" >&2; }
die() {
    printf '%s\n' "$1" >&2
    exit 1
}

usage() {
    # 打印文件头部的注释块 (第一行 shebang 之后的连续注释行)
    awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0"
}

# ==================== 平台 / 架构 ====================

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${script_dir}/.." && pwd)"

case "$(uname -s)" in
Linux) os=linux ;;
Darwin) os=macos ;;
*) die "不支持的系统: $(uname -s) (本脚本只覆盖 Linux / macOS)" ;;
esac

machine_arch="$(uname -m)"
case "$machine_arch" in
x86_64 | amd64) arch=x64 ;;
arm64 | aarch64) arch=arm64 ;;
*) die "不支持的架构: $machine_arch" ;;
esac

# ==================== 选项 ====================

config=Release
deps_only=0
configure_only=0
clean=0
run_tests=0
with_examples=0
with_tests=0
auto=0
skip_if_fresh=0
jobs=0
build_dir=""
output_dir=""
boost_root=""
boost_archive=""
generator=""
no_download_boost=0
android=0
android_abi="arm64-v8a"
android_ndk=""
android_platform="android-24"
android_stl="c++_static"
ios=0
ios_simulator=0
# iOS 最低版本: 与主程序 ios/Runner.xcodeproj 的 IPHONEOS_DEPLOYMENT_TARGET (15.0) 保持一致,
# 插件框架库的最低版本不能低于主程序 (低了接口随时可能被裁掉), 也不能高于主程序 (高了会在
# 主程序支持的旧系统上按新系统编译, 用到新符号就失败)。主程序构建时会用 --ios-target 传自己的值。
ios_target="15.0"
# macOS 最低版本: 与主程序 macos/Runner.xcodeproj 的 MACOSX_DEPLOYMENT_TARGET (12.0) 保持一致。
# 不传时 CMake 会用"构建机当前的系统版本", 编出来的库按"只在同样新的系统上运行"编译。
# 主程序构建时会用 --macos-target 传自己的值。
macos_target="12.0"

# 自动模式要再起一个子进程跑"不带 --auto 的普通流程" (失败只警告), 这里先把 --auto 摘掉
forward_args=()
for forward_arg in "$@"; do
    case "$(printf '%s' "$forward_arg" | tr '[:upper:]' '[:lower:]' | tr -d '-')" in
    auto) ;;
    *) forward_args+=("$forward_arg") ;;
    esac
done

while [ $# -gt 0 ]; do
    arg="$1"
    shift
    # 三种写法都接受: --name value / --name=value / -Name value (与 PowerShell 的 -Config 一致)
    name="${arg%%=*}"
    name_lower="$(printf '%s' "$name" | tr '[:upper:]' '[:lower:]' | tr -d '-')"
    value=""
    if [ "$name" != "$arg" ]; then
        value="${arg#*=}"
    fi
    # 需要取值的选项: 没写 = 形式时取下一个参数
    case "$name_lower" in
    config | jobs | builddir | outputdir | boostroot | boostarchive | generator | root | abi | ndk | androidplatform | androidstl | iostarget | macostarget)
        if [ -z "$value" ]; then
            [ $# -gt 0 ] || die "选项 $name 缺少取值"
            value="$1"
            shift
        fi
        ;;
    esac
    case "$name_lower" in
    config) config="$value" ;;
    jobs) jobs="$value" ;;
    builddir) build_dir="$value" ;;
    outputdir) output_dir="$value" ;;
    boostroot) boost_root="$value" ;;
    boostarchive) boost_archive="$value" ;;
    generator) generator="$value" ;;
    root) root="$value" ;;
    abi) android_abi="$value" ;;
    ndk) android_ndk="$value" ;;
    androidplatform) android_platform="$value" ;;
    androidstl) android_stl="$value" ;;
    ios) ios=1 ;;
    iossimulator)
        ios=1
        ios_simulator=1
        ;;
    iostarget) ios_target="$value" ;;
    macostarget) macos_target="$value" ;;
    clean) clean=1 ;;
    depsonly) deps_only=1 ;;
    configureonly) configure_only=1 ;;
    runtests) run_tests=1 ;;
    withexamples | examples) with_examples=1 ;;
    noexamples) with_examples=0 ;;
    withtests) with_tests=1 ;;
    notests) with_tests=0 ;;
    auto) auto=1 ;;
    # 归一化后是 skipiffresh（去掉 `-` 后 "skip" + "if" + "fresh" 连在一起）
    skipiffresh) skip_if_fresh=1 ;;
    nodownloadboost) no_download_boost=1 ;;
    android) android=1 ;;
    help | h)
        usage
        exit 0
        ;;
    *) die "未知选项: $arg (用 --help 查看用法)" ;;
    esac
done

case "$config" in
Release | Debug | RelWithDebInfo | MinSizeRel) ;;
*) die "不支持的配置: $config (取 Release / Debug / RelWithDebInfo / MinSizeRel)" ;;
esac
case "$android_abi" in
arm64-v8a | armeabi-v7a | x86_64 | x86) ;;
*) die "不支持的 ABI: $android_abi (取 arm64-v8a / armeabi-v7a / x86_64 / x86)" ;;
esac
case "$android_stl" in
c++_static | c++_shared) ;;
*) die "不支持的 STL: $android_stl (取 c++_static / c++_shared)" ;;
esac
if ! [[ "$jobs" =~ ^[0-9]+$ ]]; then
    die "--jobs 需要是非负整数, 收到: $jobs"
fi
if [ "$deps_only" = 1 ] && [ "$run_tests" = 1 ]; then
    die "--run-tests 不能与 --deps-only 同时使用"
fi
# --run-tests 需要测试可执行文件与对照示例插件, 因此隐含示例与测试 (与 Windows 侧一致)
if [ "$run_tests" = 1 ]; then
    with_examples=1
    with_tests=1
fi

# Android: 目标平台与开发机平台无关, 平台/架构段按 ABI 推导 (与 .ps1 的 -Android 一致)
android_toolchain=""
if [ "$android" = 1 ]; then
    os=android
    case "$android_abi" in
    arm64-v8a) arch=arm64 ;;
    armeabi-v7a) arch=arm ;;
    x86_64) arch=x64 ;;
    x86) arch=x86 ;;
    esac
fi

# iOS: 目标平台同样与开发机无关, 平台/架构段按 --ios / --ios-simulator 推导。
# 设备固定 arm64; 模拟器用开发机架构 (Intel 上是 x64) —— 架构写成别的, 模拟器就加载不了。
# 这两条只会由 macOS 上的主程序构建阶段 (ios/tool/embed_extern_plugin_dylib.sh) 或开发者手工调用。
ios_sdk=""
if [ "$ios" = 1 ]; then
    [ "$android" = 1 ] && die '--android 与 --ios/--ios-simulator 不能同时使用'
    [ "$os" = macos ] || die "iOS 目标只能在 macOS 上构建 (当前系统: $(uname -s)); 需要 Xcode 与 iPhoneOS/iPhoneSimulator SDK"
    case "$ios_target" in
    '' | *[!0-9.]* | .* | *.) die "--ios-target 需要是版本号 (例: 15.0), 收到: ${ios_target}" ;;
    esac
    if [ "$ios_simulator" = 1 ]; then
        os=ios-simulator
        ios_sdk=iphonesimulator
    else
        os=ios
        ios_sdk=iphoneos
        arch=arm64
    fi
fi

# macOS 最低版本只用于本机的 macOS 构建 (iOS 目标走 --ios-target, Linux 上这条不生效)
if [ "$os" = macos ]; then
    case "$macos_target" in
    '' | *[!0-9.]* | .* | *.) die "--macos-target 需要是版本号 (例: 12.0), 收到: ${macos_target}" ;;
    esac
fi

root="$(cd "$root" && pwd)"
src_dir="${root}/src"
third_party="${src_dir}/third_party"
native_root="${root}/.native"

# NDK 位置解析 (显式参数 → 环境变量 → SDK 下版本号最大者); 只输出路径, 提示走 stderr。
# 找不到时返回 1 (调用方决定是报错还是跳过), 因此这里的失败信息只写 stderr。
resolve_android_ndk() {
    local marker='build/cmake/android.toolchain.cmake'
    if [ -n "$android_ndk" ]; then
        if [ ! -f "${android_ndk}/${marker}" ]; then
            printf '  --ndk 指向的目录里没有 %s : %s\n' "$marker" "$android_ndk" >&2
            return 1
        fi
        (cd "$android_ndk" && pwd)
        return 0
    fi
    local env_name env_value
    for env_name in ANDROID_NDK_HOME ANDROID_NDK_ROOT; do
        env_value="$(eval "printf '%s' \"\${${env_name}:-}\"")"
        if [ -n "$env_value" ] && [ -f "${env_value}/${marker}" ]; then
            printf '   NDK: 取自 $%s (%s)\n' "$env_name" "$env_value" >&2
            (cd "$env_value" && pwd)
            return 0
        fi
    done
    local sdk_name sdk_value ndk_root best="" best_version=""
    for sdk_name in ANDROID_SDK_ROOT ANDROID_HOME; do
        sdk_value="$(eval "printf '%s' \"\${${sdk_name}:-}\"")"
        [ -n "$sdk_value" ] || continue
        ndk_root="${sdk_value}/ndk"
        [ -d "$ndk_root" ] || continue
        for candidate in "$ndk_root"/*; do
            [ -f "${candidate}/${marker}" ] || continue
            # 目录名就是 NDK 版本号 (例如 29.0.14206865); 用 sort -V 比较大小
            local version
            version="$(basename "$candidate")"
            if [ -z "$best_version" ] || [ "$version" = "$(printf '%s\n%s\n' "$best_version" "$version" | sort -V | tail -1)" ]; then
                best="$candidate"
                best_version="$version"
            fi
        done
        if [ -n "$best" ]; then
            printf '   NDK: 取自 $%s/ndk (%s)\n' "$sdk_name" "$best" >&2
            (cd "$best" && pwd)
            return 0
        fi
    done
    printf '  未找到 Android NDK (需要 %s)\n' "$marker" >&2
    return 1
}

# 模拟器运行时根目录 (iOS 模拟器产物要在模拟器环境里跑: 需要把 DYLD_ROOT_PATH 指向它)
# 优先取与当前 iPhoneSimulator SDK 版本一致的那个运行时, 没有就用版本号最大的。
# 只用 stdout 输出路径; 找不到时返回 1 (调用方决定跳过还是提示)。
resolve_ios_simulator_root() {
    command -v python3 >/dev/null 2>&1 || return 1
    local sdk_version
    sdk_version="$(xcrun --sdk "${ios_sdk:-iphonesimulator}" --show-sdk-version 2>/dev/null || true)"
    xcrun simctl list runtimes -j 2>/dev/null | python3 -c '
import json, sys

try:
    runtimes = json.load(sys.stdin).get("runtimes", [])
except Exception:
    raise SystemExit(1)

wanted = sys.argv[1] if len(sys.argv) > 1 else ""
items = [r for r in runtimes if r.get("isAvailable") and r.get("runtimeRoot")]
if not items:
    raise SystemExit(1)


def version_key(item):
    parts = [int(p) for p in str(item.get("version", "0")).split(".") if p.isdigit()]
    return parts or [0]


match = [r for r in items if wanted and str(r.get("version", "")) == wanted]
print((match or max(items, key=version_key))[0]["runtimeRoot"])
' "$sdk_version" 2>/dev/null || return 1
}

if [ "$android" = 1 ]; then
    if ! android_ndk_root_path="$(resolve_android_ndk)"; then
        if [ "$auto" = 1 ]; then
            warn '未找到 Android NDK, 跳过自动构建 (手工构建: tools/build_native.sh --android --abi <abi>)'
            exit 0
        fi
        die "未找到 Android NDK (需要 build/cmake/android.toolchain.cmake)。
  请安装 NDK 并指定: --ndk <NDK 根目录>, 或设置环境变量 ANDROID_NDK_HOME"
    fi
    android_toolchain="${android_ndk_root_path}/build/cmake/android.toolchain.cmake"
    printf '   Android: ABI=%s platform=%s stl=%s\n' "$android_abi" "$android_platform" "$android_stl"
fi

config_lower="$(printf '%s' "$config" | tr '[:upper:]' '[:lower:]')"
# Android 一个 ABI 一个构建目录; 桌面端与 iOS 沿用 <平台>-<配置> (iOS 的 os 是 ios / ios-simulator)
if [ "$android" = 1 ]; then
    [ -n "$build_dir" ] || build_dir="${native_root}/build/android-${android_abi}-${config_lower}"
    [ -n "$output_dir" ] || output_dir="${native_root}/output/android-${android_abi}-${config_lower}"
else
    [ -n "$build_dir" ] || build_dir="${native_root}/build/${os}-${config_lower}"
    [ -n "$output_dir" ] || output_dir="${native_root}/output/${os}-${arch}-${config_lower}"
fi
# 转成绝对路径 (目录可能还不存在, 因此先建父目录再规范化)
mkdir -p "$(dirname "$build_dir")"
build_dir="$(cd "$(dirname "$build_dir")" && pwd)/$(basename "$build_dir")"
mkdir -p "$(dirname "$output_dir")"
output_dir="$(cd "$(dirname "$output_dir")" && pwd)/$(basename "$output_dir")"
install_dir="${build_dir}/musicxx-extern-plugin-install"
# 插件框架库与测试可执行文件的文件名 (供自动模式判断产物是否在位; 与安装前缀/输出目录里的名字一致)
lib_name='libmusicxx_extern_plugin.so'
test_name='musicxx_extern_plugin_test'
if [ "$os" = macos ] || [ "$os" = ios ] || [ "$os" = ios-simulator ]; then
    lib_name='libmusicxx_extern_plugin.dylib'
fi

if [ "$android" = 1 ]; then
    # 交叉编译固定用 Ninja: 与 NDK 工具链配合最稳, 也不依赖开发机平台的 IDE 生成器
    command -v ninja >/dev/null 2>&1 ||
        die "Android 交叉编译需要 ninja (未在 PATH 上找到); 请安装 ninja"
    generator=Ninja
fi

if [ -z "$generator" ]; then
    if command -v ninja >/dev/null 2>&1; then
        generator=Ninja
    else
        generator='Unix Makefiles'
    fi
fi

case "$config" in
Debug) xx_is_release=0 ;;
*) xx_is_release=1 ;;
esac

printf '=== musicxx_extern_plugin 原生构建 ===\n'
info "源目录:     ${root}"
info "构建目录:   ${build_dir}"
info "安装目录:   ${install_dir}"
info "输出目录:   ${output_dir}"
info "配置:       ${config} (XX_IS_RELEASE_D=${xx_is_release})   生成器: ${generator}"
if [ "$android" = 1 ]; then
    info "目标:       android-${android_abi} (${android_platform}, ${android_stl})"
    info "工具链:     ${android_toolchain}"
fi
if [ "$ios" = 1 ]; then
    info "目标:       ${os} (sdk=${ios_sdk}, 架构=${arch}, 最低版本=${ios_target})"
elif [ "$os" = macos ]; then
    info "目标:       macos (架构=${arch}, 最低版本=${macos_target})"
fi

# ==================== 环境预检 ====================

step '环境预检'
for tool in cmake git; do
    command -v "$tool" >/dev/null 2>&1 || die "未找到 $tool; 请先安装并加入 PATH"
done
info "cmake: $(cmake --version | head -1)"

if [ "$android" = 1 ]; then
    # 交叉编译用 NDK 自带的 clang, 开发机编译器与 pkg-config 都不参与
    [ -f "$android_toolchain" ] || die "找不到 NDK 工具链文件: ${android_toolchain}"
    info "ninja: $(command -v ninja)"
elif [ "$ios" = 1 ]; then
    # iOS 交叉编译用 SDK 里的 clang (CMake 的 CMAKE_SYSTEM_NAME=iOS 会自动选), 开发机编译器不参与
    command -v xcrun >/dev/null 2>&1 ||
        die 'iOS 构建需要 xcrun; 请安装 Xcode 并执行 sudo xcode-select --switch /Applications/Xcode.app'
    ios_sdk_path="$(xcrun --sdk "$ios_sdk" --show-sdk-path 2>/dev/null || true)"
    if [ -z "$ios_sdk_path" ] || [ ! -d "$ios_sdk_path" ]; then
        die "找不到 ${ios_sdk} SDK (xcrun --sdk ${ios_sdk} --show-sdk-path); 请安装 Xcode 或用 xcode-select 指向它"
    fi
    info "iOS SDK:    ${ios_sdk_path}"
    info "xcodebuild: $(xcodebuild -version 2>/dev/null | head -1 || printf '未知')"
else
    # 插件框架要求 C++26: GCC >= 14 / Clang >= 18
    cxx_compiler="$(command -v c++ || true)"
    if [ -z "$cxx_compiler" ]; then
        warn '未找到 c++ 编译器 (请安装 g++ >= 14 或 clang++ >= 18)'
    else
        cxx_version_line="$("$cxx_compiler" --version | head -1)"
        info "编译器: ${cxx_version_line}"
        compiler_major="$(printf '%s' "$cxx_version_line" | grep -oE '[0-9]+' | head -1)"
        if printf '%s' "$cxx_version_line" | grep -qi 'gcc\|g++\|gnu'; then
            if [ -n "$compiler_major" ] && [ "$compiler_major" -lt 14 ]; then
                warn "GCC ${compiler_major} < 14: 插件框架要求 C++26 (CMAKE_CXX_STANDARD 26), 编译会失败"
            fi
        elif printf '%s' "$cxx_version_line" | grep -qi 'clang'; then
            if [ -n "$compiler_major" ] && [ "$compiler_major" -lt 18 ]; then
                warn "Clang ${compiler_major} < 18: 插件框架要求 C++26, 编译可能失败"
            fi
        fi
    fi

    # cxx_utilxx_base / cxx_pluginxx 的配置阶段有 find_package(PkgConfig REQUIRED)
    command -v pkg-config >/dev/null 2>&1 ||
        warn '未找到 pkg-config; cxx_utilxx_base / cxx_pluginxx 配置阶段会失败 (find_package(PkgConfig REQUIRED))'
fi

# 子模块预检 (commit 由仓库 gitlink 记录, 这里只确认已初始化)
info '子模块:'
submodule_status="$(git -C "$root" submodule status 2>/dev/null || true)"
missing_submodules="$(printf '%s\n' "$submodule_status" | awk '/^-/ { print $2 }')"
if [ -n "$missing_submodules" ]; then
    die "以下子模块未初始化:
  ${missing_submodules}
请先执行: git submodule update --init --recursive"
fi
info "    已就绪: $(printf '%s\n' "$submodule_status" | grep -c . || true) 个 (git submodule status)"

# ==================== Boost (仅头文件) ====================
# 只用 Boost.Asio 头, 不链接任何 Boost 编译库。cmake 侧只负责生成 BoostConfig.cmake 与校验版本,
# 头文件的获取(复用/下载/解包/校验)放在这里。版本、下载地址与校验值一同维护在下面的常量里
# (与 tools/build_native.ps1 的常量保持一致)。

boost_version='1.92.0'
boost_source_url='https://archives.boost.io/release/@VERSION@/source/boost_@UNDERSCORE@.tar.gz'
boost_sha256='c4a3b310ddd2472416e091067166b0713be97c63f38c212c484ada022fd296ce'

test_boost_root() {
    [ -n "${1:-}" ] && [ -f "$1/boost/version.hpp" ]
}

boost_version_code() {
    sed -n 's/^#define BOOST_VERSION[[:space:]]\{1,\}\([0-9]\{1,\}\).*/\1/p' "$1/boost/version.hpp" | head -1
}

# 指定文件的 sha256 (Linux 用 sha256sum, macOS 用 shasum)
sha256_of_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{ print $1 }'
    else
        shasum -a 256 "$1" | awk '{ print $1 }'
    fi
}

# 解包 Boost 发布包 (只取 <boost_版本>/boost 头文件子树), 返回头文件根目录
expand_boost_archive() {
    local archive_path="$1"
    local dest_root="$2"
    local dir_name="boost_$(printf '%s' "$boost_version" | tr '.' '_')"
    local tmp="${native_root}/downloads/extract-$$-$(date +%s)"
    mkdir -p "$tmp"
    printf '  解包 %s\n' "$archive_path" >&2
    tar -xf "$archive_path" -C "$tmp" "$dir_name/boost" || die "解包失败: $archive_path"
    local header_root="${tmp}/${dir_name}"
    test_boost_root "$header_root" || die "解包结果里没有 ${dir_name}/boost/version.hpp"
    rm -rf "$dest_root"
    mkdir -p "$(dirname "$dest_root")"
    mv "$header_root" "$dest_root"
    rm -rf "$tmp"
    printf '%s' "$dest_root"
}

# Boost 头文件解析顺序: 显式参数 → 显式发布包 → 本包内置头文件 → 已解包缓存 → 按版本下载
# 返回头文件根目录 (进度信息走 stderr, 只有路径走 stdout)
resolve_boost_header_root() {
    local cached="${native_root}/boost/boost_$(printf '%s' "$boost_version" | tr '.' '_')"

    # 1) 显式参数
    if [ -n "$boost_root" ]; then
        test_boost_root "$boost_root" || die "--boost-root 下没有 boost/version.hpp: $boost_root"
        printf '  Boost: 使用 --boost-root 指定的头文件 %s\n' "$boost_root" >&2
        (cd "$boost_root" && pwd)
        return 0
    fi

    # 2) 显式指定的发布包 (优先于内置头文件, 便于离线/自备包)
    if [ -n "$boost_archive" ]; then
        [ -f "$boost_archive" ] || die "--boost-archive 指向的文件不存在: $boost_archive"
        expand_boost_archive "$boost_archive" "$cached"
        return 0
    fi

    # 3) 本包内置的头文件 (src/third_party/boost/include)
    local vendored="${third_party}/boost/include"
    if test_boost_root "$vendored"; then
        printf '  Boost: 使用本包内置头文件 %s\n' "$vendored" >&2
        printf '%s' "$vendored"
        return 0
    fi

    # 4) 之前下载解包过的缓存
    if test_boost_root "$cached"; then
        printf '  Boost: 使用已解包缓存 %s\n' "$cached" >&2
        printf '%s' "$cached"
        return 0
    fi

    # 5) 按固定版本下载官方发布包
    if [ "$no_download_boost" = 1 ]; then
        die "未提供 Boost 头文件: 请用 --boost-root <含 boost/ 的目录> 或 --boost-archive <发布包>,
或去掉 --no-download-boost 让脚本自动下载 ${boost_source_url}"
    fi
    local dir_name="boost_$(printf '%s' "$boost_version" | tr '.' '_')"
    local download_root="${native_root}/downloads"
    mkdir -p "$download_root"
    local archive_path="${download_root}/${dir_name}.tar.gz"
    if [ -f "$archive_path" ]; then
        printf '  Boost: 复用已下载的发布包 %s\n' "$archive_path" >&2
    else
        local url
        url="$(printf '%s' "$boost_source_url" | sed "s/@VERSION@/${boost_version}/; s/@UNDERSCORE@/${dir_name#boost_}/")"
        printf '  Boost: 下载 %s\n' "$url" >&2
        curl -L --fail --retry 3 --retry-delay 2 -o "$archive_path" "$url" || die "下载 Boost 发布包失败: $url"
    fi
    if [ -n "$boost_sha256" ]; then
        local actual
        actual="$(sha256_of_file "$archive_path" | tr '[:upper:]' '[:lower:]')"
        [ "$actual" = "$(printf '%s' "$boost_sha256" | tr '[:upper:]' '[:lower:]')" ] ||
            die "Boost 发布包校验失败: ${archive_path}
  期望 ${boost_sha256}
  实际 ${actual}"
        printf '  Boost: 发布包 SHA256 校验通过\n' >&2
    fi
    expand_boost_archive "$archive_path" "$cached"
}

step 'Boost (仅头文件)'
boost_header_root="$(resolve_boost_header_root)"
boost_parts_a="$(printf '%s' "$boost_version" | cut -d. -f1)"
boost_parts_b="$(printf '%s' "$boost_version" | cut -d. -f2)"
boost_parts_c="$(printf '%s' "$boost_version" | cut -d. -f3)"
expect_code=$((boost_parts_a * 100000 + boost_parts_b * 100 + boost_parts_c))
boost_code="$(boost_version_code "$boost_header_root")"
[ -n "$boost_code" ] || die "无法从 ${boost_header_root}/boost/version.hpp 读出 BOOST_VERSION"
if [ "$boost_code" != "$expect_code" ]; then
    die "Boost 版本不符: 脚本期望 ${boost_version} (BOOST_VERSION=${expect_code}), 实际 BOOST_VERSION=${boost_code} (${boost_header_root})"
fi
printf '  Boost 头文件根: %s (版本 %s)\n' "$boost_header_root" "$boost_version"

# 构建目录签名 (配置/生成器/架构/Boost 位置/Android 目标变化时重置构建目录) 与
# 本次构建覆盖的内容 (framework / examples / tests) —— 自动模式与构建标记都要用。
# Android 之外的构建不写 ABI 段 (与 Windows 侧的签名格式保持一致, 便于人工对照)。
android_abi_tag=""
if [ "$android" = 1 ]; then
    android_abi_tag="$android_abi"
fi
# iOS: SDK 与最低版本变化时也要重建 (模拟器/设备、最低版本不同 → 依赖库 ABI 与可用 API 不同)
ios_tag=""
if [ "$ios" = 1 ]; then
    ios_tag="${ios_sdk}/ios${ios_target}"
fi
# macOS: 最低版本变化时同样要重建 (依赖库与插件框架库的 LC_BUILD_VERSION 会跟着变)
macos_tag=""
if [ "$os" = macos ]; then
    macos_tag="macos${macos_target}"
fi
signature="config=${config};generator=${generator};arch=${arch};boost=${boost_header_root};xxRelease=${xx_is_release};target=${os};abi=${android_abi_tag};ndk=${android_toolchain};androidPlatform=${android_platform};stl=${android_stl};ios=${ios_tag};macos=${macos_tag}"
stamp_kinds='framework'
if [ "$with_examples" = 1 ]; then stamp_kinds="${stamp_kinds} examples"; fi
if [ "$with_tests" = 1 ]; then stamp_kinds="${stamp_kinds} tests"; fi

# ==================== 已是最新 (--auto / --skip-if-fresh) ====================
#
# 两个开关都按构建标记判断"要不要真构建"，区别只在构建失败时怎么处理：
#   --auto            给"主程序构建时自动触发插件框架库构建"用（Windows/Linux 的 CMake 配置阶段、
#                     主程序 macos/ 的 Runner 构建阶段、Android 的 Gradle）：已是最新则秒退，
#                     否则起子进程跑完整流程，失败只警告并返回 0 —— 外部插件是可选功能，
#                     不该让应用构建失败；
#   --skip-if-fresh   给"示例插件构建脚本"这类调用方用（tools/build_examples.sh）：已是最新同样秒退，
#                     但真要构建时继续走下面的普通流程，失败照常报错。
#
# 判据（与 Windows 侧 tools/native_common.ps1 的 Test-MxBuildFresh 同一套）：
#   本次要覆盖的构建标记都在 → 要求存在的产物都在 → 构建目录已 configure 且签名一致 →
#   最早的标记比最新源码新。
build_is_fresh() {
    # 1) 本次要覆盖的构建标记都在, 并找出最早的那个 (每个标记都要比源码新)
    local stamp_kind stamp_file oldest_stamp=""
    for stamp_kind in $stamp_kinds; do
        stamp_file="${build_dir}/.build_stamp_${stamp_kind}"
        [ -f "$stamp_file" ] || return 1
        if [ -z "$oldest_stamp" ] || [ "$stamp_file" -ot "$oldest_stamp" ]; then
            oldest_stamp="$stamp_file"
        fi
    done

    # 2) 产物在位
    [ -f "${output_dir}/bin/${lib_name}" ] || return 1
    if [ "$with_examples" = 1 ] && [ -z "$(ls -A "${output_dir}/plugins" 2>/dev/null)" ]; then
        return 1
    fi
    if [ "$with_tests" = 1 ] && [ ! -f "${output_dir}/bin/${test_name}" ]; then
        return 1
    fi

    # 3) 构建目录已 configure 且签名一致
    [ -f "${build_dir}/CMakeCache.txt" ] || return 1
    [ -f "${build_dir}/.build_signature" ] || return 1
    [ "$(tr -d '\r\n' <"${build_dir}/.build_signature")" = "$signature" ] || return 1

    # 4) 源码没有比标记更新的 (third_party 里的 Boost 头文件不算: 它不参与编译, 只被 include)
    local source_path
    for source_path in "${src_dir}/CMakeLists.txt" "${src_dir}/host" "${src_dir}/include" \
        "${src_dir}/sdk" "${src_dir}/third_party" "${root}/plugins" "${root}/tools/cmake"; do
        [ -e "$source_path" ] || continue
        if [ -n "$(find "$source_path" -type d -name boost -prune -o -type f -newer "$oldest_stamp" -print -quit 2>/dev/null)" ]; then
            return 1
        fi
    done
    return 0
}

if [ "$auto" = 1 ] || [ "$skip_if_fresh" = 1 ]; then
    if [ "$clean" = 0 ] && [ "$configure_only" = 0 ] && [ "$deps_only" = 0 ] && [ "$run_tests" = 0 ] &&
        build_is_fresh; then
        printf '  已是最新, 跳过构建: %s\n' "${output_dir}/bin/${lib_name}"
        exit 0
    fi

    if [ "$auto" = 1 ]; then
        printf '\n==> 自动构建: 起子进程执行完整流程 (失败只警告)\n'
        set +e
        bash "${script_dir}/build_native.sh" ${forward_args[@]+"${forward_args[@]}"}
        auto_rc=$?
        set -e
        if [ "$auto_rc" -ne 0 ]; then
            printf '\n' >&2
            warn "插件框架自动构建失败 (退出码 ${auto_rc}), 本次主程序构建不打包外部插件框架库。"
            warn '主程序照常构建与运行 (外部插件功能不可用); 手工重跑看完整日志:'
            warn "  tools/build_native.sh --config ${config}"
            exit 0
        fi
        exit 0
    fi
fi

if [ "$clean" = 1 ]; then
    step '清理构建目录'
    for p in "$build_dir" "$output_dir"; do
        if [ -e "$p" ]; then
            printf '  删除 %s\n' "$p"
            rm -rf "$p"
        fi
    done
fi

# ==================== configure ====================

step 'configure (superbuild: 依赖 + 插件框架)'
# 构建目录签名在 Boost 之后就已经算好 (自动模式要用它判断"是否已是最新"); 这里只写入标记文件。
# 配置/生成器/架构/Boost 位置变化时重置目录, 避免 CMakeCache 里残留上一次的参数
# (例如旧的 CMAKE_CONFIGURATION_TYPES) 让新配置报出难以定位的错。
marker="${build_dir}/.build_signature"
if [ -d "$build_dir" ]; then
    old_signature=""
    if [ -f "$marker" ]; then
        old_signature="$(tr -d '\r\n' <"$marker")"
    fi
    if [ "$old_signature" != "$signature" ]; then
        printf '  构建目录的配置已变化, 重置: %s\n' "$build_dir"
        rm -rf "$build_dir"
    fi
fi
mkdir -p "$build_dir"
printf '%s\n' "$signature" >"$marker"

configure_args=(
    "-DXX_IS_RELEASE_D=${xx_is_release}"
    "-DCMAKE_BUILD_TYPE=${config}"
    # 收窄可用配置到本次配置 (与 Windows 侧一致): 一个构建目录只构建一个配置,
    # 依赖安装前缀在构建目录内, 因此不会混入其它配置的产物
    "-DCMAKE_CONFIGURATION_TYPES=${config}"
)
# Android / iOS: 目标处理器由工具链推导 (ANDROID_ABI / CMAKE_OSX_ARCHITECTURES),
# 显式传开发机架构会造成两边不一致 (iOS 设备目标是 arm64, 开发机可能是 x64)
if [ "$android" != 1 ] && [ "$ios" != 1 ]; then
    configure_args+=("-DCMAKE_SYSTEM_PROCESSOR=${machine_arch}")
fi
configure_args+=(
    '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'
    "-DMUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE=${boost_header_root}"
    # 示例插件与原生测试: 默认关 (示例插件单独构建; Windows 侧见 tools/build_examples.ps1)
    "-DMUSICXX_EXTERN_PLUGIN_BUILD_EXAMPLE=${with_examples}"
    "-DMUSICXX_EXTERN_PLUGIN_BUILD_TESTS=${with_tests}"
)
if [ "$android" = 1 ]; then
    # 交叉编译参数交给 NDK 工具链 (superbuild 的 src/CMakeLists.txt 自动补 XX_IS_ANDROID_D=1,
    # 并把 ANDROID_ABI/ANDROID_PLATFORM/ANDROID_STL 原样传给各依赖与插件框架工程的嵌套构建)
    configure_args+=(
        "-DCMAKE_TOOLCHAIN_FILE=${android_toolchain}"
        "-DANDROID_ABI=${android_abi}"
        "-DANDROID_PLATFORM=${android_platform}"
        "-DANDROID_STL=${android_stl}"
    )
fi
if [ "$ios" = 1 ]; then
    # 走 CMake 内置的 iOS 支持 (CMAKE_SYSTEM_NAME=iOS): 编译器/sysroot 由 CMake 按 CMAKE_OSX_* 选,
    # src/CMakeLists.txt 会把这几项原样转发给各依赖与插件框架工程的嵌套构建 (否则嵌套构建会退回
    # 默认 sysroot=iphoneos、架构=开发机架构, 模拟器目标就编出加载不了的依赖库)
    configure_args+=(
        '-DCMAKE_SYSTEM_NAME=iOS'
        "-DCMAKE_SYSTEM_PROCESSOR=${arch}" # CMAKE_OSX_ARCHITECTURES 不会填它: 多目标打包的 TARGET_TAG auto 要用
        "-DCMAKE_OSX_SYSROOT=${ios_sdk}"   # iphoneos / iphonesimulator
        "-DCMAKE_OSX_ARCHITECTURES=${arch}"
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=${ios_target}"
    )
elif [ "$os" = macos ]; then
    # 本机 macOS 构建: 最低版本必须显式给, 否则 CMake 用构建机当前的系统版本
    # (产物按"只在同样新的系统上运行"编译), src/CMakeLists.txt 会把它转发给嵌套构建
    configure_args+=("-DCMAKE_OSX_DEPLOYMENT_TARGET=${macos_target}")
fi
if [ "$deps_only" = 1 ]; then
    configure_args+=('-DMUSICXX_EXTERN_PLUGIN_BUILD_HOST=OFF')
fi

if ! cmake "${configure_args[@]}" -G "$generator" -B "$build_dir" -S "$src_dir"; then
    die 'cmake configure 失败'
fi

if [ "$configure_only" = 1 ]; then
    printf '\nConfigureOnly: 配置完成 (未构建)\n'
    exit 0
fi

# ==================== build ====================

if [ "$jobs" -gt 0 ]; then
    parallel="$jobs"
elif command -v nproc >/dev/null 2>&1; then
    parallel="$(nproc)"
else
    parallel="$(sysctl -n hw.ncpu 2>/dev/null || printf '4')"
fi

build_args=(--build "$build_dir" --config "$config" --parallel "$parallel")
# 只构建依赖: 直接以 cxx_pluginxx_repo 为目标 (host 工程不在依赖链上)
if [ "$deps_only" = 1 ]; then
    build_args+=(--target cxx_pluginxx_repo)
fi

step "build (--config ${config} --parallel ${parallel})"
if ! cmake "${build_args[@]}"; then
    die 'cmake build 失败'
fi

# 顶层 superbuild 自身没有安装规则 (安装都由各 ExternalProject 完成); 这里保持一致行为
if ! cmake --install "$build_dir" --config "$config"; then
    die 'cmake install 失败'
fi

# 插件框架工程是嵌套 ExternalProject 构建: 生成器有时会判定上层的"构建/安装步骤已是最新"而跳过它,
# 于是源码改动不会真正触发重编译 (表现为"构建成功但产物时间戳/体积不变")。这里显式再跑一次
# 嵌套工程的增量构建与安装 (已经是新的就几秒钟结束), 保证脚本的"构建成功"与产物一致。
if [ "$deps_only" = 0 ]; then
    host_build_dir="${build_dir}/e/host"
    if [ -f "${host_build_dir}/CMakeCache.txt" ]; then
        step 'build 插件框架工程 (嵌套工程增量构建)'
        if ! cmake --build "$host_build_dir" --config "$config" --parallel "$parallel"; then
            die 'cmake build (插件框架工程) 失败'
        fi
        if ! cmake --install "$host_build_dir" --config "$config"; then
            die 'cmake install (插件框架工程) 失败'
        fi
    fi
fi

# ==================== 产物 ====================

step '产物'
artifact_count=0
list_artifacts() {
    local dir="$1"
    [ -d "$dir" ] || return 0
    while IFS= read -r artifact; do
        [ -n "$artifact" ] || continue
        printf '  %-36s %10s 字节\n' "$(basename "$artifact")" "$(wc -c <"$artifact" | tr -d ' ')"
        artifact_count=$((artifact_count + 1))
    done < <(find "$dir" -type f \( -name '*.so' -o -name '*.dylib' -o -name '*.dll' -o -name '*.exe' \
        -o -name 'musicxx_extern_plugin_test' \) | sort)
}
list_artifacts "${install_dir}/bin"
list_artifacts "${install_dir}/plugins"
[ "$artifact_count" -gt 0 ] || printf '  (bin/plugins 下没有产物)\n'

# 复制到稳定输出目录; 失败只提示不阻断, 因为安装目录里的产物本身已经可用
if [ "$deps_only" = 0 ]; then
    printf '\n  复制产物到输出目录: %s\n' "$output_dir"
    mkdir -p "$output_dir"
    for sub in bin plugins; do
        from="${install_dir}/${sub}"
        [ -d "$from" ] || continue
        to="${output_dir}/${sub}"
        # 注意: 必须按"内容合并"复制 (`cp -R <源目录> <目标目录>` 会在目标里嵌一层同名目录)
        rm -rf "${to:?}/${sub:?}"
        mkdir -p "$to"
        cp -R "${from}/." "${to}/"
    done
fi

# ==================== 原生测试 (可选) ====================

if [ "$run_tests" = 1 ]; then
    step '原生测试'
    if [ "$android" = 1 ]; then
        # 测试可执行文件是给设备编的 (目标 ABI 与开发机不同), 开发机上跑不起来。
        # 需要在设备上跑时: adb push bin/ + plugins/ 到设备目录, 用 LD_LIBRARY_PATH 指向 bin/ 再执行。
        printf '  [跳过] Android 目标是交叉编译产物, 不能在当前主机运行; 产物在: %s\n' "${install_dir}/bin"
    elif [ "$ios" = 1 ] && [ "$ios_simulator" = 0 ]; then
        # iOS 设备目标是 iOS 平台的交叉编译产物, 开发机上跑不起来 (模拟器目标可以, 见下面的分支)。
        # 需要在设备上跑时: 用 Xcode/ios-deploy 把 bin/ 与 plugins/ 装进一个测试应用再执行。
        printf '  [跳过] iOS 设备目标是交叉编译产物, 不能在当前主机运行; 产物在: %s\n' "${install_dir}/bin"
    else
        test_exe="${install_dir}/bin/musicxx_extern_plugin_test"
        [ -x "$test_exe" ] || die "未找到测试可执行文件: ${test_exe}"

        # 插件框架把"父目录下的每个子目录"当作一个插件, 因此测试参数传插件目录的父目录
        plugin_root="${install_dir}/plugins"
        printf '  插件目录: %s\n' "$plugin_root"
        printf '  运行: %s %s\n' "$test_exe" "$plugin_root"
        # 测试可执行文件与插件框架库同目录 (安装布局 bin/), 运行时按平台补库搜索路径
        case "$os" in
        macos | ios | ios-simulator)
            # iOS 模拟器产物也是 Mach-O + dyld, 与 macOS 同一套环境变量
            export DYLD_LIBRARY_PATH="${install_dir}/bin${DYLD_LIBRARY_PATH:+:${DYLD_LIBRARY_PATH}}"
            ;;
        *)
            export LD_LIBRARY_PATH="${install_dir}/bin${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
            ;;
        esac

        set +e
        skip_reason=''
        if [ "$os" = ios-simulator ]; then
            # 模拟器产物必须在模拟器环境里跑: dyld 要求 DYLD_ROOT_PATH 指向模拟器运行时根目录
            # (开发机是同一个 CPU 架构, 所以能直接执行, 不需要启动模拟器)
            simulator_root="$(resolve_ios_simulator_root || true)"
            if [ -n "$simulator_root" ] && [ -d "$simulator_root" ]; then
                export DYLD_ROOT_PATH="$simulator_root"
                printf '  模拟器运行时: %s\n' "$simulator_root"
            else
                skip_reason='没找到模拟器运行时根目录 (xcrun simctl list runtimes 里没有可用运行时)'
            fi
        fi
        if [ -n "$skip_reason" ]; then
            printf '  [跳过] %s\n' "$skip_reason"
            printf '         手工在模拟器里跑: xcrun simctl spawn booted %s %s\n' "$test_exe" "$plugin_root"
            test_rc=0
        else
            "$test_exe" "$plugin_root"
            test_rc=$?
            printf '  退出码: %s\n' "$test_rc"
        fi
        set -e
        [ "$test_rc" -eq 0 ] || die "原生测试失败 (exit=${test_rc}); 日志见上方输出"
    fi
fi

# ==================== 构建标记 ====================
#
# 全部做完 (含测试) 之后才写标记: 失败时没有标记, 下一次构建就不会被误判成"已是最新"。
if [ "$configure_only" = 0 ] && [ "$deps_only" = 0 ]; then
    for stamp_kind in $stamp_kinds; do
        : >"${build_dir}/.build_stamp_${stamp_kind}"
    done
    printf '  构建标记: %s → %s\n' "$stamp_kinds" "$build_dir"
fi

printf '\n构建完成\n'
if [ "$android" = 1 ]; then
    printf 'README: 插件框架库由 android/build.gradle 按 ABI 收集进 APK (jniLibs), 重新打包安装后生效\n'
fi
if [ "$ios" = 1 ]; then
    printf 'iOS: 插件框架库由主程序 ios/ 的 Runner 构建阶段复制进 Runner.app/Frameworks (%s)\n' \
        'ios/tool/embed_extern_plugin_dylib.sh; 手工检查用 MUSICXX_EXTERN_PLUGIN_APP_DIR'
fi
printf '插件框架构建完成: %s\n' "${output_dir}/bin/${lib_name}"
if [ "$with_examples" = 1 ]; then
    printf '示例插件在: %s (打包成安装包: tools/pack_plugin.sh --plugin-dir <插件目录>)\n' "${output_dir}/plugins"
else
    printf '示例插件未构建; 要"构建 + 打包成安装包"用: tools/build_examples.sh\n'
fi
