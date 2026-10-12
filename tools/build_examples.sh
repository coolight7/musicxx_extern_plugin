#!/usr/bin/env bash
# musicxx_extern_plugin 示例插件构建（与打包）脚本 —— Windows 侧 tools/build_examples.ps1 的等价实现
#
# 示例插件是"给插件作者照抄的样板 + 给插件框架保护机制做对照的坏样本"，插件框架本身不需要它们：
# 所以它们不在框架构建脚本里默认构建（tools/build_native.sh 要显式 --with-examples），也不参与主程序构建。
#
# 做的事:
#   1. 构建插件框架（插件框架库 + 插件 SDK）+ 示例插件 + 原生测试 —— 复用 tools/build_native.sh，
#      构建标记已有就跳过（要强制重建用 --force），原生测试可以用 --run-tests 顺手跑一遍;
#   2. 列出构建产物里的每个插件（文件数 / 体积 / 清单版本）;
#   3. 把每个插件目录打包成安装包 .zip（顶层就是插件目录内容，可直接「从压缩包安装」）。
#      包名带上插件版本号: <插件 id>-<清单 version 里的 . 换成 _>.zip，例 example_js_shader-2026_1010_1.zip
#      （版本号约定 `<年>.<月日>.<小版本号>`，例 2026.1010.1 = 2026-10-10 的第 1 版）。
#
# 用法（选项名不区分大小写，--no-pack 与 -NoPack 等价）:
#   tools/build_examples.sh                        # 构建示例插件 + 打包（已是最新则直接打包）
#   tools/build_examples.sh --force                # 不跳过，强制走一遍构建
#   tools/build_examples.sh --no-pack              # 只构建，不打包
#   tools/build_examples.sh --no-tests             # 不构建原生测试与对照示例
#   tools/build_examples.sh --run-tests            # 构建后跑原生测试（隐含示例与测试）
#   tools/build_examples.sh --no-build             # 只用已有产物打包
#   tools/build_examples.sh --pack-out <目录>      # 指定 .zip 的输出目录
#   tools/build_examples.sh --only example_native,example_js   # 只打包指定插件（支持 * 通配）
#   tools/build_examples.sh --config Debug --jobs 8
#   tools/build_examples.sh --clean                # 先清空构建目录
#   tools/build_examples.sh --android --abi arm64-v8a          # Android 交叉编译
#
# 产物:
#   <包>/.native/output/<平台>-<架构>-<配置>/plugins/   插件目录（包内 flutter test 会用它）
#   <包>/.native/output/<平台>-<架构>-<配置>/packages/  安装包 .zip（本脚本的输出）
#
# 装到主程序里试:
#   musicxx-cli plugin install "<包>/.native/output/<平台>-<架构>-<配置>/packages/<插件id>-<版本号>.zip" --enable
set -euo pipefail

step() { printf '\n==> %s\n' "$1"; }
info() { printf '  %s\n' "$1"; }
warn() { printf '  [警告] %s\n' "$1" >&2; }
die() {
    printf '%s\n' "$1" >&2
    exit 1
}
usage() {
    # 打印文件头部的注释块 (第一行 shebang 之后的连续注释行)
    awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0"
}

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "${script_dir}/.." && pwd)"

# ==================== 选项 ====================

config=Release
no_build=0
force=0
no_tests=0
run_tests=0
no_pack=0
pack_out=""
only_patterns=""
clean=0
jobs=0
android=0
android_abi='arm64-v8a'

while [ $# -gt 0 ]; do
    arg="$1"
    shift
    # 三种写法都接受: --name value / --name=value / -Name value
    name="${arg%%=*}"
    name_lower="$(printf '%s' "$name" | tr '[:upper:]' '[:lower:]' | tr -d '-')"
    value=""
    if [ "$name" != "$arg" ]; then
        value="${arg#*=}"
    fi
    case "$name_lower" in
    config | jobs | packout | only | root | abi | androidabi)
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
    packout) pack_out="$value" ;;
    only) only_patterns="${only_patterns:+${only_patterns},}${value}" ;;
    root) root="$value" ;;
    abi | androidabi) android_abi="$value" ;;
    nobuild) no_build=1 ;;
    force | rebuild) force=1 ;;
    notests) no_tests=1 ;;
    runtests) run_tests=1 ;;
    nopack) no_pack=1 ;;
    clean) clean=1 ;;
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
if ! [ "$jobs" -ge 0 ] 2>/dev/null; then
    die "--jobs 需要是非负整数, 收到: $jobs"
fi
if [ "$run_tests" = 1 ]; then
    # --run-tests 在 tools/build_native.sh 里同样隐含示例与测试
    no_tests=0
fi

root="$(cd "$root" && pwd)"
config_lower="$(printf '%s' "$config" | tr '[:upper:]' '[:lower:]')"
native_root="${root}/.native"

# ==================== 输出目录（与 tools/build_native.sh 的命名一致） ====================

machine_arch="$(uname -m)"
case "$machine_arch" in
x86_64 | amd64) arch=x64 ;;
arm64 | aarch64) arch=arm64 ;;
arm | armv7 | armv7l | armhf) arch=arm ;;
*) arch="$machine_arch" ;;
esac

if [ "$android" = 1 ]; then
    target_tag="android-${android_abi}"
else
    case "$(uname -s)" in
    Linux) os=linux ;;
    Darwin) os=macos ;;
    *) die "不支持的系统: $(uname -s) (本脚本只覆盖 Linux / macOS; Windows 侧用 tools/build_examples.ps1)" ;;
    esac
    target_tag="${os}-${arch}"
fi

output_dir="${native_root}/output/${target_tag}-${config_lower}"
plugins_dir="${output_dir}/plugins"
if [ -n "$pack_out" ]; then
    pack_dir="$pack_out"
else
    pack_dir="${output_dir}/packages"
fi
# 转成绝对路径 (目录可能还不存在)
if [ "${pack_dir#/}" = "$pack_dir" ]; then
    pack_dir="$(pwd)/${pack_dir}"
fi

printf '=== musicxx_extern_plugin 示例插件构建 ===\n'
info "包目录:   ${root}"
info "配置:     ${config}   目标: ${target_tag}"
info "插件目录: ${plugins_dir}"
if [ "$no_pack" = 1 ]; then
    info '打包目录: (本次不打包)'
else
    info "打包目录: ${pack_dir}"
fi

# ==================== 1. 构建框架 + 示例插件 ====================

if [ "$no_build" = 0 ]; then
    build_script="${script_dir}/build_native.sh"
    [ -f "$build_script" ] || die "缺少构建脚本: ${build_script}"

    build_args=(--with-examples)
    if [ "$no_tests" = 0 ]; then
        build_args+=(--with-tests)
    fi
    if [ "$run_tests" = 1 ]; then
        build_args+=(--run-tests)
    fi
    # 已是最新就跳过构建, 直接去打包 (要强制重建用 --force)
    if [ "$force" = 0 ]; then
        build_args+=(--skip-if-fresh)
    else
        info '(强制重建)'
    fi
    if [ "$clean" = 1 ]; then
        build_args+=(--clean)
    fi
    if [ "$jobs" -gt 0 ]; then
        build_args+=(--jobs "$jobs")
    fi
    if [ "$android" = 1 ]; then
        build_args+=(--android --abi "$android_abi")
    fi

    printf '\n==> 构建插件框架 + 示例插件'
    if [ "$no_tests" = 0 ]; then
        printf ' + 原生测试'
    fi
    if [ "$force" = 0 ]; then
        printf ' (已是最新则跳过)\n'
    else
        printf '\n'
    fi
    bash "$build_script" --config "$config" ${build_args[@]+"${build_args[@]}"} ||
        die "构建失败; 单独重跑可看完整日志: tools/build_native.sh --with-examples --config ${config}"
fi

[ -d "$plugins_dir" ] ||
    die "没有找到构建出来的示例插件目录: ${plugins_dir}
  请先不带 --no-build 跑一次本脚本"

# ==================== 2. 列出构建产物里的插件 ====================

# 插件目录的判据与插件框架一致: 目录里要有 plugin.yaml
plugins=()
for dir in "$plugins_dir"/*; do
    [ -d "$dir" ] || continue
    [ -f "${dir}/plugin.yaml" ] || continue
    plugin_id="$(basename "$dir")"
    if [ -n "$only_patterns" ]; then
        matched=0
        old_ifs="$IFS"
        IFS=', '
        for pattern in $only_patterns; do
            [ -n "$pattern" ] || continue
            # shellcheck disable=SC2254  # 这里就是要让模式展开
            case "$plugin_id" in
            $pattern)
                matched=1
                break
                ;;
            esac
        done
        IFS="$old_ifs"
        [ "$matched" = 1 ] || continue
    fi
    plugins+=("$dir")
done

step '示例插件'
if [ "${#plugins[@]}" -eq 0 ]; then
    die "在 ${plugins_dir} 下没有找到插件目录 (目录里要有 plugin.yaml)"
fi

for dir in "${plugins[@]}"; do
    plugin_id="$(basename "$dir")"
    version="$(sed -n 's/^[[:space:]]*version[[:space:]]*:[[:space:]]*//p' "${dir}/plugin.yaml" | head -1 |
        sed -E 's/[[:space:]]+$//' | sed -E 's/^["'"'"']+//; s/["'"'"']+$//')"
    count="$(find "$dir" -type f | wc -l | tr -d ' ')"
    bytes="$(find "$dir" -type f -exec wc -c {} + | tail -1 | awk '{ print $1 }')"
    [ -n "$bytes" ] || bytes=0
    mark=''
    case "$plugin_id" in
    *_fail | *_bad_entry | *_broken) mark=' [对照示例: 故意做坏的]' ;;
    esac
    if [ -n "$version" ]; then
        version_text="$version"
    else
        version_text='未声明版本'
    fi
    printf '  %-24s %4s 个文件 %10s 字节  版本 %s%s\n' "$plugin_id" "$count" "$bytes" "$version_text" "$mark"
done
if [ "$run_tests" = 1 ]; then
    info '原生测试已在构建阶段执行 (见上方输出)'
fi

# ==================== 3. 打包成安装包 (.zip) ====================

if [ "$no_pack" = 1 ]; then
    printf '\n构建完成 (未打包); 需要安装包时去掉 --no-pack\n'
    exit 0
fi

pack_script="${script_dir}/pack_plugin.sh"
[ -f "$pack_script" ] || die "缺少打包脚本: ${pack_script}"
mkdir -p "$pack_dir"

step "打包 (输出到 ${pack_dir})"
packed_zips=()
packed_shas=()
for dir in "${plugins[@]}"; do
    plugin_id="$(basename "$dir")"
    version="$(sed -n 's/^[[:space:]]*version[[:space:]]*:[[:space:]]*//p' "${dir}/plugin.yaml" | head -1 |
        sed -E 's/[[:space:]]+$//' | sed -E 's/^["'"'"']+//; s/["'"'"']+$//')"
    # 包名 = <插件 id>-<清单 version 里的 . 换成 _>.zip (与 pack_plugin.sh 的默认名一致)
    tag="$(printf '%s' "$version" | tr '.' '_')"
    if [ -z "$tag" ]; then
        zip_name="${plugin_id}.zip"
        warn "${plugin_id}: 清单没写 version，包名不带版本号"
    else
        zip_name="${plugin_id}-${tag}.zip"
    fi
    zip_path="${pack_dir}/${zip_name}"
    if ! pack_output="$(bash "$pack_script" --plugin-dir "$dir" --out "$zip_path" 2>&1)"; then
        printf '%s\n' "$pack_output" >&2
        die "打包失败: ${plugin_id}"
    fi
    printf '%s\n' "$pack_output"
    packed_zips+=("$zip_path")
    packed_shas+=("$(printf '%s\n' "$pack_output" | sed -n 's/^sha256: //p' | head -1)")
done

printf '\n示例插件构建与打包完成: %s 个安装包\n' "${#packed_zips[@]}"
index=0
while [ "$index" -lt "${#packed_zips[@]}" ]; do
    zip_path="${packed_zips[$index]}"
    zip_bytes="$(wc -c <"$zip_path" | tr -d ' ')"
    zip_mib="$(awk -v bytes="$zip_bytes" 'BEGIN { printf "%.2f", bytes / 1048576 }')"
    printf '  %-40s %9s MiB  sha256=%s\n' "$(basename "$zip_path")" "$zip_mib" \
        "$(printf '%s' "${packed_shas[$index]}" | cut -c1-16)"
    index=$((index + 1))
done
printf '\n装到主程序里试 (主程序需已开启『拟声++』):\n'
printf '  musicxx-cli plugin install "%s" --enable\n' "${packed_zips[0]}"
printf '  也可以直接把插件目录放进 <应用支持目录>/musicxx/extern_plugin/plugins/ 再在管理页重新扫描\n'
printf '  （示例插件目录本身在 %s）\n' "$plugins_dir"
