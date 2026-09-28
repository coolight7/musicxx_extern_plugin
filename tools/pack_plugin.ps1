# 把插件目录打成安装包（.zip），并列出包内的系统/架构分支
#
# 用途：应用侧「从压缩包安装」要求压缩包**顶层就是插件目录的内容**（plugin.yaml 在根）。
# 多目标包（`lib/<系统>-<架构>/`）可以一次打进同一个 zip：宿主运行时按当前系统与 CPU
# 架构选分支（选择规则见 docs/plugin-native-api.md §1.3）。
#
# 用法：
#   pwsh -NoProfile -File tools/pack_plugin.ps1 -PluginDir <插件目录>
#   pwsh -NoProfile -File tools/pack_plugin.ps1 -PluginDir <插件目录> -Out <输出.zip>
#   pwsh -NoProfile -File tools/pack_plugin.ps1 -PluginDir <插件目录> -Check    # 只检查，不打包
#
# 例（打包构建产物里的多目标示例插件）：
#   pwsh -NoProfile -File tools/pack_plugin.ps1 `
#     -PluginDir .native/output/windows-x64-release/plugins/example_native_multi
param(
  [Parameter(Mandatory = $true)][string]$PluginDir,
  [string]$Out = "",
  [switch]$Check
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $PluginDir -PathType Container)) {
  throw "插件目录不存在: $PluginDir"
}
$pluginDirFull = (Resolve-Path -LiteralPath $PluginDir).Path
$manifestPath = Join-Path $pluginDirFull "plugin.yaml"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
  throw "插件目录里没有 plugin.yaml（压缩包顶层必须是插件目录的内容）: $pluginDirFull"
}

# ---- 读清单里的关键字段（只做提示与校验，权威解析在宿主）----
$manifestText = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8
function Read-ManifestScalar([string]$key, [string]$default) {
  $m = [regex]::Match($manifestText, "(?m)^\s*" + [regex]::Escape($key) + "\s*:\s*(.+?)\s*$")
  if (-not $m.Success) { return $default }
  return $m.Groups[1].Value.Trim().Trim('"').Trim("'")
}
$name = Read-ManifestScalar "name" ""
$kind = Read-ManifestScalar "kind" ""
$entry = Read-ManifestScalar "entry" ""
$targetsDir = Read-ManifestScalar "targets_dir" "lib"
if ($name -eq "") {
  throw "清单里缺少 name（插件 id）: $manifestPath"
}
if ($kind -eq "") { $kind = if ($entry -eq "") { "js" } else { "native" } }

# ---- 分支情况（仅多目标包）----
$branches = @()
if ($targetsDir -ne "") {
  $branchRoot = Join-Path $pluginDirFull $targetsDir
  if (Test-Path -LiteralPath $branchRoot -PathType Container) {
    $branches = Get-ChildItem -LiteralPath $branchRoot -Directory |
      ForEach-Object {
        $libs = Get-ChildItem -LiteralPath $_.FullName -File |
          Where-Object { $_.Name -match '\.(dll|so|dylib)$' -and -not $_.Name.StartsWith('.') }
        [pscustomobject]@{
          Tag   = $_.Name
          Lib   = if ($libs.Count -gt 0) { ($libs | ForEach-Object { $_.Name }) -join ", " } else { "(缺库文件)" }
          Count = $libs.Count
        }
      } | Sort-Object Tag
  }
}

$allFiles = Get-ChildItem -LiteralPath $pluginDirFull -Recurse -File
$totalBytes = ($allFiles | Measure-Object -Property Length -Sum).Sum
if (-not $totalBytes) { $totalBytes = 0 }

Write-Output "插件: $name ($kind)"
if ($entry -ne "") { Write-Output "清单 entry: $entry" }
Write-Output "文件数: $($allFiles.Count)（约 $([math]::Round($totalBytes / 1MB, 2)) MiB）"
if ($branches.Count -gt 0) {
  Write-Output "分支目录 ${targetsDir}/:"
  foreach ($item in $branches) {
    $mark = if ($item.Count -eq 1) { "ok" } elseif ($item.Count -eq 0) { "缺库文件" } else { "多个库文件（需要清单 entry 指明）" }
    Write-Output "  - $($item.Tag): $($item.Lib) [$mark]"
  }
} elseif ($kind -eq "native") {
  Write-Output "分支目录 ${targetsDir}/: 无（单目标包：库文件在插件目录根）"
} else {
  Write-Output "分支目录: 不适用（JS 插件跨平台）"
}

if ($Check) {
  Write-Output "（-Check：只检查，不打包）"
  exit 0
}

if ($Out -eq "") {
  $Out = Join-Path (Split-Path -Parent $pluginDirFull) "$name.zip"
}
$outFull = [System.IO.Path]::GetFullPath($Out)
$outParent = Split-Path -Parent $outFull
if ($outParent -ne "" -and -not (Test-Path -LiteralPath $outParent)) {
  New-Item -ItemType Directory -Path $outParent -Force | Out-Null
}
if (Test-Path -LiteralPath $outFull) { Remove-Item -LiteralPath $outFull -Force }

# 关键：includeBaseDirectory = $false —— 压缩包顶层就是插件目录的内容
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory(
  $pluginDirFull, $outFull, [System.IO.Compression.CompressionLevel]::Optimal, $false)

$zipBytes = (Get-Item -LiteralPath $outFull).Length
$sha = (Get-FileHash -LiteralPath $outFull -Algorithm SHA256).Hash.ToLower()
Write-Output "已打包: $outFull（$([math]::Round($zipBytes / 1MB, 2)) MiB）"
Write-Output "sha256: $sha"
