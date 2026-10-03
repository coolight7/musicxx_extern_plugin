# 把两个 kit 文件（基础 kit + musicxx 扩展 kit）复制进随包分发的 **JS 插件**目录
#
# 用法（在包目录下）：
#   pwsh -NoProfile -File tools/sync_ui_kit.ps1                       # 同步 plugins/ 下所有用 kit 的 JS 插件
#   pwsh -NoProfile -File tools/sync_ui_kit.ps1 -Plugin example_js    # 只同步一个插件目录
#
# 说明：
# - 宿主**不提供** kit（否则改 kit 会影响已经装好的插件），所以 kit 是随插件目录一起分发的
#   普通脚本文件，由清单 `scripts: [pluginxx_ui_kit.js, musicxx_ui_kit.js, plugin.js]` 按顺序装载；
# - 只给「清单 `scripts` 里真的声明了 kit 文件」的 JS 插件复制：宿主只执行清单里列出的脚本，
#   没声明的插件（例如只演示多脚本装载的 `example_js_multi_script`）不需要这两个文件，
#   复制过去只会变成多余文件；
# - 只有 JS 插件需要 kit（动态库插件用 C++ 头文件里的 kit，见 src/sdk/include/…/musicxx_ui_kit.g.h）；
# - 基础 kit 来自子模块 `src/third_party/cxx_pluginxx_ui/js/pluginxx_ui_kit.js`；
#   扩展 kit 来自本包 `js/musicxx_ui_kit.js`（由 tools/gen_ui_kit.ps1 生成）；
# - 插件作者改 kit（比如按自己的留白规则改基础组件）时，直接改插件目录里的副本即可。
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [string]$Plugin = ''
)

$ErrorActionPreference = 'Stop'

$baseKitName = 'pluginxx_ui_kit.js'
$extKitName = 'musicxx_ui_kit.js'
$baseKit = Join-Path $Root "src/third_party/cxx_pluginxx_ui/js/$baseKitName"
$extKit = Join-Path $Root "js/$extKitName"
foreach ($p in @($baseKit, $extKit)) {
    if (-not (Test-Path $p)) {
        throw "找不到 $p （先跑 tools/gen_ui_kit.ps1 生成扩展 kit；基础 kit 来自子模块）"
    }
}

# 读清单里的一项：kind 与 scripts（scripts 支持 `[...]` 内联写法与 `- 值` 的块写法）
function Read-Manifest([string]$path) {
    $text = Get-Content $path -Raw
    $result = @{ kind = ''; scripts = @() }
    if ($text -match '(?m)^\s*kind\s*:\s*(\S+)') {
        $result.kind = $Matches[1].Trim()
    }
    if ($text -match '(?m)^\s*scripts\s*:\s*\[(.+?)\]') {
        $result.scripts = @($Matches[1] -split ',' |
            ForEach-Object { $_.Trim().Trim('"').Trim("'") } |
            Where-Object { $_ })
    } elseif ($text -match '(?m)^\s*scripts\s*:\s*\r?\n((?:\s*-\s*.+\r?\n?)+)') {
        $result.scripts = @([regex]::Matches($Matches[1], '(?m)^\s*-\s*(.+)$') |
            ForEach-Object { $_.Groups[1].Value.Trim().Trim('"').Trim("'") })
    }
    return $result
}

$pluginsDir = Join-Path $Root 'plugins'
$targets = @()
if ($Plugin) {
    $targets = @(Join-Path $pluginsDir $Plugin)
} else {
    $targets = Get-ChildItem $pluginsDir -Directory |
        Where-Object { Test-Path (Join-Path $_.FullName 'plugin.yaml') } |
        ForEach-Object { $_.FullName }
}

$count = 0
foreach ($dir in $targets) {
    if (-not (Test-Path $dir)) {
        throw "插件目录不存在: $dir"
    }
    # 只给 JS 插件发 kit（native 插件的界面用 C++ 头文件里的 kit）
    $manifest = Join-Path $dir 'plugin.yaml'
    $meta = @{ kind = ''; scripts = @() }
    if (Test-Path $manifest) {
        $meta = Read-Manifest $manifest
    }
    if ($meta.kind -ne 'js') {
        Write-Output "  跳过（kind=$($meta.kind)，不是 JS 插件）: $dir"
        continue
    }
    $copied = 0
    foreach ($kit in @(
            @{ name = $baseKitName; source = $baseKit },
            @{ name = $extKitName; source = $extKit })) {
        if ($meta.scripts -notcontains $kit.name) {
            continue
        }
        Copy-Item $kit.source (Join-Path $dir $kit.name) -Force
        $copied += 1
    }
    if ($copied -gt 0) {
        $count += 1
        Write-Output "  已同步 $copied 个 kit: $dir"
    } else {
        Write-Output "  跳过（清单 scripts 里没有 kit 文件）: $dir"
    }
}
Write-Output "共同步 $count 个 JS 插件"
Write-Output '提示: 清单里要按顺序写上 scripts: [pluginxx_ui_kit.js, musicxx_ui_kit.js, plugin.js]'
