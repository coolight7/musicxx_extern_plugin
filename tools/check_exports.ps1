<#
.SYNOPSIS
    核对 "C 头里声明的导出函数" 与 "src/exports.map 这份可读清单" 完全一致。

.DESCRIPTION
    导出面有两份描述：头文件里的声明（决定 dllexport / 可见性）与 `src/exports.map`
    （Linux / Android 的 version script 用它，也是人看的清单）。两份不一致时，宿主可能
    在运行期才发现 "lookup 失败"，所以这里在构建前后都能查：
      - 头里有、清单没有：符号会被 version script 隐藏（Linux / Android 上调不到）；
      - 清单里有、头里没有：清单写错了或代码删了没同步。

    只认 `src/include/musicxx_extern_plugin_api.h` 里带 `MUSICXX_EXTERN_PLUGIN_EXPORT`
    的函数声明；其它头文件与内部符号不参与。

.PARAMETER Quiet
    只输出结论，不打印符号明细。

.EXAMPLE
    pwsh -NoProfile -File tools/check_exports.ps1
#>
[CmdletBinding()]
param(
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

$packageRoot = Split-Path -Parent $PSScriptRoot
$headerPath = Join-Path $packageRoot 'src/include/musicxx_extern_plugin_api.h'
$mapPath = Join-Path $packageRoot 'src/exports.map'

foreach ($path in @($headerPath, $mapPath)) {
    if (-not (Test-Path $path)) {
        Write-Host "[check_exports] 缺文件: $path"
        exit 1
    }
}

# 1) 头文件里的导出函数（声明形如 `MUSICXX_EXTERN_PLUGIN_EXPORT <返回类型> MUSICXX_EXTERN_PLUGIN_CALL
#    musicxx_extern_plugin_xxx(`，中间可能换行）
$headerText = Get-Content -Raw -Encoding UTF8 $headerPath
$declared = [System.Collections.Generic.HashSet[string]]::new()
foreach ($match in [regex]::Matches(
        $headerText,
        'MUSICXX_EXTERN_PLUGIN_EXPORT[\s\S]*?\b(musicxx_extern_plugin_[A-Za-z0-9_]+)\s*\(')) {
    [void]$declared.Add($match.Groups[1].Value)
}

# 2) 清单里的符号（一行一个，`#` 开头与空行忽略）
$listed = [System.Collections.Generic.HashSet[string]]::new()
foreach ($line in Get-Content -Encoding UTF8 $mapPath) {
    $text = $line.Trim()
    if ($text.Length -eq 0 -or $text.StartsWith('#')) { continue }
    [void]$listed.Add($text)
}

$missingInMap = @($declared | Where-Object { -not $listed.Contains($_) } | Sort-Object)
$missingInHeader = @($listed | Where-Object { -not $declared.Contains($_) } | Sort-Object)

if (-not $Quiet) {
    Write-Host ("[check_exports] 头文件声明 " + $declared.Count + " 个, 清单 " + $listed.Count + " 个")
}

if ($missingInMap.Count -eq 0 -and $missingInHeader.Count -eq 0) {
    Write-Host '[check_exports] 一致'
    exit 0
}

foreach ($name in $missingInMap) {
    Write-Host "[check_exports] 头文件里有、src/exports.map 里没有: $name"
}
foreach ($name in $missingInHeader) {
    Write-Host "[check_exports] src/exports.map 里有、头文件里没有: $name"
}
Write-Host ("[check_exports] 失败 (缺 {0} 个, 多 {1} 个)" -f $missingInMap.Count, $missingInHeader.Count)
exit 1
