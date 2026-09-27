# 把两个 kit 文件（基础 kit + musicxx 扩展 kit）复制进随包分发的 **JS 插件**目录
#
# 用法（在包目录下）：
#   pwsh -NoProfile -File tools/sync_ui_kit.ps1                       # 同步 plugins/ 下所有 JS 插件
#   pwsh -NoProfile -File tools/sync_ui_kit.ps1 -Plugin example_js    # 只同步一个插件目录
#
# 说明：
# - 宿主**不提供** kit（否则改 kit 会影响已经装好的插件），所以 kit 是随插件目录一起分发的
#   普通脚本文件，由清单 `scripts: [pluginxx_ui_kit.js, musicxx_ui_kit.js, plugin.js]` 按顺序装载；
# - 只有 JS 插件需要 kit（动态库插件用 C++ 头文件里的 kit，见 src/sdk/include/…/musicxx_ui_kit.g.h）；
# - 基础 kit 来自子模块 `src/third_party/cxx_pluginxx_ui/js/pluginxx_ui_kit.js`；
#   扩展 kit 来自本包 `js/musicxx_ui_kit.js`（由 tools/gen_ui_kit.ps1 生成）；
# - 插件作者改 kit（比如按自己的留白口径改基础组件）时，直接改插件目录里的副本即可。
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [string]$Plugin = ''
)

$ErrorActionPreference = 'Stop'

$baseKit = Join-Path $Root 'src/third_party/cxx_pluginxx_ui/js/pluginxx_ui_kit.js'
$extKit = Join-Path $Root 'js/musicxx_ui_kit.js'
foreach ($p in @($baseKit, $extKit)) {
    if (-not (Test-Path $p)) {
        throw "找不到 $p （先跑 tools/gen_ui_kit.ps1 生成扩展 kit；基础 kit 来自子模块）"
    }
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
    $kind = ''
    if (Test-Path $manifest) {
        foreach ($line in Get-Content $manifest) {
            if ($line -match '^\s*kind\s*:\s*(\S+)') {
                $kind = $Matches[1].Trim()
                break
            }
        }
    }
    if ($kind -ne 'js') {
        Write-Output "  跳过（kind=$kind，不是 JS 插件）: $dir"
        continue
    }
    Copy-Item $baseKit (Join-Path $dir 'pluginxx_ui_kit.js') -Force
    Copy-Item $extKit (Join-Path $dir 'musicxx_ui_kit.js') -Force
    $count += 1
    Write-Output "  已同步 kit: $dir"
}
Write-Output "共 $count 个 JS 插件目录"
Write-Output '提示: 清单里要按顺序写上 scripts: [pluginxx_ui_kit.js, musicxx_ui_kit.js, plugin.js]'
