#!/usr/bin/env bash
# 把一个插件目录打成安装包（.zip），并列出包内的系统/架构分支
# —— Windows 侧 tools/pack_plugin.ps1 的等价实现（Linux / macOS 用）。
#
# 用途：主程序「从压缩包安装」要求压缩包**顶层就是插件目录的内容**（plugin.yaml 在根）。
# 多目标包（`lib/<系统>-<架构>/`）可以一次打进同一个 zip：插件框架运行时按当前系统与 CPU
# 架构选分支（选择规则见 docs/plugin-native-api.md §1.3）。
#
# 用法（选项名不区分大小写，--plugin-dir 与 -PluginDir 等价）：
#   tools/pack_plugin.sh --plugin-dir <插件目录>
#   tools/pack_plugin.sh --plugin-dir <插件目录> --out <输出.zip>
#   tools/pack_plugin.sh --plugin-dir <插件目录> --check      # 只检查，不打包
#
# 包名：不给 --out 时是 <插件 id>-<清单 version 里的 . 换成 _>.zip，与插件目录同级
# （例：example_js_shader 的 version 是 2026.1010.1 → example_js_shader-2026_1010_1.zip）。
#
# 例（打包构建产物里的多目标示例插件）：
#   tools/pack_plugin.sh --plugin-dir .native/output/macos-arm64-release/plugins/example_native_multi
#
# 打包实现：优先用 zip（macOS 自带，常见 Linux 发行版都有），没有 zip 时退回 python3 的 zipfile；
# 两者都跳过 macOS 的 `.DS_Store` / `._*` 垃圾文件，且**顶层目录不进包**（与 PowerShell 侧的
# includeBaseDirectory = $false 一致）。
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
pkg_root="$(cd "${script_dir}/.." && pwd)"

# ==================== 选项 ====================

plugin_dir=""
out_zip=""
check_only=0

while [ $# -gt 0 ]; do
    arg="$1"
    shift
    # 两种写法都接受: --name value / --name=value
    name="${arg%%=*}"
    name_lower="$(printf '%s' "$name" | tr '[:upper:]' '[:lower:]' | tr -d '-')"
    value=""
    if [ "$name" != "$arg" ]; then
        value="${arg#*=}"
    fi
    case "$name_lower" in
    plugindir | dir)
        if [ -z "$value" ]; then
            [ $# -gt 0 ] || die "选项 $name 缺少取值"
            value="$1"
            shift
        fi
        plugin_dir="$value"
        ;;
    out | output)
        if [ -z "$value" ]; then
            [ $# -gt 0 ] || die "选项 $name 缺少取值"
            value="$1"
            shift
        fi
        out_zip="$value"
        ;;
    check) check_only=1 ;;
    help | h)
        usage
        exit 0
        ;;
    *) die "未知选项: $arg (用 --help 查看用法)" ;;
    esac
done

[ -n "$plugin_dir" ] || die "缺少 --plugin-dir <插件目录> (用 --help 查看用法)"
[ -d "$plugin_dir" ] || die "插件目录不存在: $plugin_dir"
plugin_dir="$(cd "$plugin_dir" && pwd)"
manifest="${plugin_dir}/plugin.yaml"
[ -f "$manifest" ] || die "插件目录里没有 plugin.yaml（压缩包顶层必须是插件目录的内容）: $plugin_dir"

# ==================== 读清单里的关键字段（只做提示与校验，权威解析在插件框架） ====================

# 读一个标量字段（`<key>: <值>`，首个匹配行；取不到时输出空串）
manifest_scalar() {
    sed -n "s/^[[:space:]]*$1[[:space:]]*:[[:space:]]*//p" "$manifest" | head -1 |
        sed -E 's/[[:space:]]+$//' | sed -E 's/^["'"'"']+//; s/["'"'"']+$//'
}

# 清单里有没有这个键（`targets_dir: ''` 这种显式空值与"没写"不是一回事）
manifest_has_key() {
    grep -qE "^[[:space:]]*$1[[:space:]]*:" "$manifest"
}

name="$(manifest_scalar name)"
[ -n "$name" ] || die "清单里缺少 name（插件 id）: $manifest"
kind="$(manifest_scalar kind)"
entry="$(manifest_scalar entry)"
version="$(manifest_scalar version)"
if manifest_has_key targets_dir; then
    targets_dir="$(manifest_scalar targets_dir)"
else
    targets_dir='lib'
fi
if [ -z "$kind" ]; then
    if [ -z "$entry" ]; then kind='js'; else kind='native'; fi
fi

# ==================== example_js_font 示例字体支持 ====================

# 与 pack_plugin.ps1 一致：字体文件存在就一起打进包（仓库里没有就算了）
if [ "$name" = 'example_js_font' ]; then
    font_file="${plugin_dir}/resource/font/example-font.ttf"
    if [ ! -f "$font_file" ]; then
        src_font="${pkg_root}/resource/font/example-font.ttf"
        if [ -f "$src_font" ]; then
            mkdir -p "$(dirname "$font_file")"
            cp -f "$src_font" "$font_file"
            info "已将示例字体文件打包包含: $font_file"
        fi
    fi
fi

# ==================== 统计与分支 ====================

file_count="$(find "$plugin_dir" -type f | wc -l | tr -d ' ')"
total_bytes="$(find "$plugin_dir" -type f -exec wc -c {} + | tail -1 | awk '{ print $1 }')"
[ -n "$total_bytes" ] || total_bytes=0
total_mib="$(awk -v bytes="$total_bytes" 'BEGIN { printf "%.2f", bytes / 1048576 }')"

printf '插件: %s (%s)\n' "$name" "$kind"
if [ -n "$version" ]; then
    printf '清单 version: %s\n' "$version"
else
    printf '清单 version: 未声明（包名不带版本号）\n'
fi
[ -n "$entry" ] && printf '清单 entry: %s\n' "$entry"
printf '文件数: %s（约 %s MiB）\n' "$file_count" "$total_mib"

branch_root="${plugin_dir}/${targets_dir}"
branch_count=0
if [ -n "$targets_dir" ] && [ -d "$branch_root" ]; then
    printf '分支目录 %s/:\n' "$targets_dir"
    while IFS= read -r branch; do
        [ -n "$branch" ] || continue
        branch_name="$(basename "$branch")"
        libs="$(find "$branch" -maxdepth 1 -type f \( -name '*.dll' -o -name '*.so' -o -name '*.dylib' \) \
            ! -name '.*' -exec basename {} \; | sort | paste -sd ', ' -)"
        lib_count="$(find "$branch" -maxdepth 1 -type f \( -name '*.dll' -o -name '*.so' -o -name '*.dylib' \) \
            ! -name '.*' | wc -l | tr -d ' ')"
        case "$lib_count" in
        1) mark='ok' ;;
        0) mark='缺库文件' ;;
        *) mark='多个库文件（需要清单 entry 指明）' ;;
        esac
        [ -n "$libs" ] || libs='(缺库文件)'
        printf '  - %s: %s [%s]\n' "$branch_name" "$libs" "$mark"
        branch_count=$((branch_count + 1))
    done < <(find "$branch_root" -mindepth 1 -maxdepth 1 -type d | sort)
elif [ "$kind" = 'native' ]; then
    printf '分支目录 %s/: 无（单目标包：库文件在插件目录根）\n' "$targets_dir"
else
    printf '分支目录: 不适用（JS 插件跨平台）\n'
fi

if [ "$check_only" = 1 ]; then
    printf '（--check：只检查，不打包）\n'
    exit 0
fi

# ==================== 打包 ====================

# 打成顶层就是插件目录内容的 zip；macOS 的 .DS_Store / 资源分叉文件不进包
make_zip() {
    local src="$1"
    local out="$2"
    rm -f "$out"
    if command -v zip >/dev/null 2>&1; then
        (
            cd "$src" && zip -r -X -q "$out" . \
                -x '.DS_Store' -x '*/.DS_Store' -x '._*' -x '*/._*'
        )
    elif command -v python3 >/dev/null 2>&1; then
        python3 - "$src" "$out" <<'PY'
import os
import sys
import zipfile

src, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
    for root, dirs, files in os.walk(src):
        dirs.sort()
        files.sort()
        for name in files:
            # 跳过 macOS 垃圾文件（顶层要与插件目录内容一致）
            if name == ".DS_Store" or name.startswith("._"):
                continue
            full = os.path.join(root, name)
            rel = os.path.relpath(full, src).replace(os.sep, "/")
            info = zipfile.ZipInfo(rel)
            info.compress_type = zipfile.ZIP_DEFLATED
            mode = 0o755 if os.access(full, os.X_OK) else 0o644
            info.external_attr = mode << 16
            with open(full, "rb") as handle:
                zf.writestr(info, handle.read())
PY
    else
        die "打包需要 zip 或 python3，两者都没找到；装一个再重试（macOS 自带 zip，Debian/Ubuntu: apt install zip）"
    fi
    [ -f "$out" ] || die "打包失败: 没有生成 $out"
}

if [ -z "$out_zip" ]; then
    tag="$(printf '%s' "$version" | tr '.' '_')"
    if [ -z "$tag" ]; then
        out_zip="$(dirname "$plugin_dir")/${name}.zip"
    else
        out_zip="$(dirname "$plugin_dir")/${name}-${tag}.zip"
    fi
fi
if [ "${out_zip#/}" = "$out_zip" ]; then
    # 相对路径按当前工作目录解析（与命令行给人的直觉一致）
    out_zip="$(pwd)/${out_zip}"
fi
out_parent="$(dirname "$out_zip")"
[ -d "$out_parent" ] || mkdir -p "$out_parent"

step "打包"
make_zip "$plugin_dir" "$out_zip"

zip_bytes="$(wc -c <"$out_zip" | tr -d ' ')"
zip_mib="$(awk -v bytes="$zip_bytes" 'BEGIN { printf "%.2f", bytes / 1048576 }')"
if command -v shasum >/dev/null 2>&1; then
    sha="$(shasum -a 256 "$out_zip" | awk '{ print $1 }')"
elif command -v sha256sum >/dev/null 2>&1; then
    sha="$(sha256sum "$out_zip" | awk '{ print $1 }')"
else
    sha=''
fi
printf '已打包: %s（%s MiB）\n' "$out_zip" "$zip_mib"
if [ -n "$sha" ]; then
    printf 'sha256: %s\n' "$sha"
else
    warn '没有 shasum/sha256sum，跳过 sha256'
fi
