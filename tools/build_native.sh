#!/usr/bin/env bash
# musicxx_extern_plugin 原生构建脚本 (Linux / macOS)
#
# 与 Windows 的 tools/build_native.ps1 一一对应: 只负责"环境准备 + 调 cmake",
# 依赖库编译与宿主工程构建全部交给 src/CMakeLists.txt (superbuild + ExternalProject_Add):
#
#   src/CMakeLists.txt       顶层 superbuild: 推导 XX_IS_* 宏 → 生成 BoostConfig.cmake →
#                            ExternalProject_Add 逐个构建 third_party 依赖 → 构建宿主工程
#   src/host/CMakeLists.txt  嵌套宿主工程: find_package 取依赖 → 宿主库 / 插件 SDK / 示例插件 / 原生测试
#
# 本脚本负责的只有 cmake 不方便做的事:
#   - 环境预检 (cmake / git / 编译器版本 / pkg-config), 子模块是否已初始化
#   - Boost 头文件的解析/复用/下载解包 (只用头文件, 不链接 Boost 编译库)
#   - 按 --config 传 XX_IS_RELEASE_D / CMAKE_BUILD_TYPE / CMAKE_CONFIGURATION_TYPES
#   - 构建完成后把产物复制到稳定的输出目录, 可选跑原生测试
#
# 用法 (选项名不区分大小写, --deps-only 与 -DepsOnly 等价):
#   tools/build_native.sh                            # Release 全量 (依赖 + 宿主)
#   tools/build_native.sh --config Debug             # Debug (独立构建目录, 依赖也重编)
#   tools/build_native.sh --deps-only                # 只构建依赖库
#   tools/build_native.sh --configure-only           # 只做预检/Boost/configure
#   tools/build_native.sh --clean                    # 先清空构建目录
#   tools/build_native.sh --run-tests                # 构建后跑原生测试 (失败返回非 0)
#   tools/build_native.sh --jobs 8                   # 指定并行度
#   tools/build_native.sh --boost-root <目录>        # 指定 Boost 头文件根 (含 boost/)
#   tools/build_native.sh --boost-archive <压缩包>   # 用本地 Boost 发布包解包
#   tools/build_native.sh --no-download-boost        # 禁止自动下载 Boost 头文件
#   tools/build_native.sh --build-dir/--output-dir/--generator <值>   # 覆盖目录与生成器
#
# 产物布局 (与 .ps1 一致):
#   <包>/.native/build/<平台>-<配置>/musicxx-extern-plugin-install/{bin,include,lib,plugins}
#   <包>/.native/output/<平台>-<架构>-<配置>/{bin,plugins}     稳定输出目录
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
jobs=0
build_dir=""
output_dir=""
boost_root=""
boost_archive=""
generator=""
no_download_boost=0

while [ $# -gt 0 ]; do
    arg="$1"
    shift
    # 三种写法都接受: --name value / --name=value / -Name value (与 PowerShell 的 -Config 对齐)
    name="${arg%%=*}"
    name_lower="$(printf '%s' "$name" | tr '[:upper:]' '[:lower:]' | tr -d '-')"
    value=""
    if [ "$name" != "$arg" ]; then
        value="${arg#*=}"
    fi
    # 需要取值的选项: 没写 = 形式时取下一个参数
    case "$name_lower" in
    config | jobs | builddir | outputdir | boostroot | boostarchive | generator | root)
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
    clean) clean=1 ;;
    depsonly) deps_only=1 ;;
    configureonly) configure_only=1 ;;
    runtests) run_tests=1 ;;
    nodownloadboost) no_download_boost=1 ;;
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
if ! [[ "$jobs" =~ ^[0-9]+$ ]]; then
    die "--jobs 需要是非负整数, 收到: $jobs"
fi
if [ "$deps_only" = 1 ] && [ "$run_tests" = 1 ]; then
    die "--run-tests 不能与 --deps-only 同时使用"
fi

root="$(cd "$root" && pwd)"
src_dir="${root}/src"
third_party="${src_dir}/third_party"
native_root="${root}/.native"

config_lower="$(printf '%s' "$config" | tr '[:upper:]' '[:lower:]')"
[ -n "$build_dir" ] || build_dir="${native_root}/build/${os}-${config_lower}"
[ -n "$output_dir" ] || output_dir="${native_root}/output/${os}-${arch}-${config_lower}"
# 转成绝对路径 (目录可能还不存在, 因此先建父目录再规范化)
mkdir -p "$(dirname "$build_dir")"
build_dir="$(cd "$(dirname "$build_dir")" && pwd)/$(basename "$build_dir")"
mkdir -p "$(dirname "$output_dir")"
output_dir="$(cd "$(dirname "$output_dir")" && pwd)/$(basename "$output_dir")"
install_dir="${build_dir}/musicxx-extern-plugin-install"

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

# ==================== 环境预检 ====================

step '环境预检'
for tool in cmake git; do
    command -v "$tool" >/dev/null 2>&1 || die "未找到 $tool; 请先安装并加入 PATH"
done
info "cmake: $(cmake --version | head -1)"

# 内核要求 C++26: GCC >= 14 / Clang >= 18 (plan §11.2 的工具链要求)
cxx_compiler="$(command -v c++ || true)"
if [ -z "$cxx_compiler" ]; then
    warn '未找到 c++ 编译器 (请安装 g++ >= 14 或 clang++ >= 18)'
else
    cxx_version_line="$("$cxx_compiler" --version | head -1)"
    info "编译器: ${cxx_version_line}"
    compiler_major="$(printf '%s' "$cxx_version_line" | grep -oE '[0-9]+' | head -1)"
    if printf '%s' "$cxx_version_line" | grep -qi 'gcc\|g++\|gnu'; then
        if [ -n "$compiler_major" ] && [ "$compiler_major" -lt 14 ]; then
            warn "GCC ${compiler_major} < 14: 内核要求 C++26 (CMAKE_CXX_STANDARD 26), 编译会失败"
        fi
    elif printf '%s' "$cxx_version_line" | grep -qi 'clang'; then
        if [ -n "$compiler_major" ] && [ "$compiler_major" -lt 18 ]; then
            warn "Clang ${compiler_major} < 18: 内核要求 C++26, 编译可能失败"
        fi
    fi
fi

# cxx_utilxx_base / cxx_pluginxx 的配置阶段有 find_package(PkgConfig REQUIRED)
command -v pkg-config >/dev/null 2>&1 ||
    warn '未找到 pkg-config; cxx_utilxx_base / cxx_pluginxx 配置阶段会失败 (find_package(PkgConfig REQUIRED))'

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

step 'configure (superbuild: 依赖 + 宿主)'
# 构建目录签名: 配置/生成器/架构/Boost 位置变化时重置目录, 避免 CMakeCache 里残留
# 上一次的参数 (例如旧的 CMAKE_CONFIGURATION_TYPES) 让新配置报出难以定位的错。
signature="config=${config};generator=${generator};arch=${arch};boost=${boost_header_root};xxRelease=${xx_is_release}"
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
    "-DCMAKE_SYSTEM_PROCESSOR=${machine_arch}"
    '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'
    "-DMUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE=${boost_header_root}"
)
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

# 宿主工程是嵌套 ExternalProject 构建: 生成器有时会判定上层的"构建/安装步骤已是最新"而跳过它,
# 于是源码改动不会真正触发重编译 (表现为"构建成功但产物时间戳/体积不变")。这里显式再跑一次
# 嵌套工程的增量构建与安装 (已经是新的就秒级结束), 保证脚本的"构建成功"与产物一致。
if [ "$deps_only" = 0 ]; then
    host_build_dir="${build_dir}/e/host"
    if [ -f "${host_build_dir}/CMakeCache.txt" ]; then
        step 'build 宿主工程 (嵌套工程增量构建)'
        if ! cmake --build "$host_build_dir" --config "$config" --parallel "$parallel"; then
            die 'cmake build (宿主工程) 失败'
        fi
        if ! cmake --install "$host_build_dir" --config "$config"; then
            die 'cmake install (宿主工程) 失败'
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
    test_exe="${install_dir}/bin/musicxx_extern_plugin_test"
    [ -x "$test_exe" ] || die "未找到测试可执行文件: ${test_exe}"

    # 宿主把"父目录下的每个子目录"当作一个插件, 因此测试参数传插件目录的父目录
    plugin_root="${install_dir}/plugins"
    printf '  插件目录: %s\n' "$plugin_root"
    printf '  运行: %s %s\n' "$test_exe" "$plugin_root"
    # 测试可执行文件与宿主库同目录 (安装布局 bin/), 运行时按平台补库搜索路径
    if [ "$os" = macos ]; then
        export DYLD_LIBRARY_PATH="${install_dir}/bin${DYLD_LIBRARY_PATH:+:${DYLD_LIBRARY_PATH}}"
    else
        export LD_LIBRARY_PATH="${install_dir}/bin${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
    fi

    set +e
    "$test_exe" "$plugin_root"
    test_rc=$?
    set -e
    printf '  退出码: %s\n' "$test_rc"
    [ "$test_rc" -eq 0 ] || die "原生测试失败 (exit=${test_rc}); 日志见上方输出"
fi

printf '\n构建完成\n'
