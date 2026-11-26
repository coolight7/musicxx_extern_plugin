<#
.SYNOPSIS
    检查运行期字符串里没有中文（日志、错误原因、事件正文一律英文）。

.DESCRIPTION
    规则见 `AGENTS.md` §9.4：

    - 运行期会被人读到的字符串一律英文：日志宏、错误原因串、事件载荷里的
      `message` / `reason` / `log`、安装预检的结论文字、示例与测试夹具、构建脚本与命令行提示；
    - 中文只留在**代码注释**与走本地化表的**界面文案**里。

    实现方式：逐行扫源码，先按状态机去掉 `//` 与 `/* */` 注释（字符串里的注释符不算），
    再检查字符串字面量里有没有中日韩字符。所以中文注释不会误报，字符串里的中文一定会报。

    **存量清单**（`tools/log_language_allowlist.txt`）：本轮之前就带中文运行期字符串的文件先列入
    清单，脚本对它们只提示不计失败，好让检查能立刻进日常流程；清单只许变短，
    把一个文件里的字符串改完就把那一行删掉。`-Strict` 忽略清单（收尾时用它确认清单已空）。

.PARAMETER Strict
    忽略存量清单：任何文件里的中文运行期字符串都算失败。

.PARAMETER Quiet
    只输出结论。

.EXAMPLE
    pwsh -NoProfile -File tools/check_log_language.ps1
    pwsh -NoProfile -File tools/check_log_language.ps1 -Strict
#>
[CmdletBinding()]
param(
    [switch]$Strict,
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

$packageRoot = Split-Path -Parent $PSScriptRoot
$allowlistPath = Join-Path $PSScriptRoot 'log_language_allowlist.txt'

# 扫哪些文件：框架与宿主实现、SDK 头（运行期字符串都在这里）、Dart 封装、示例插件与 JS。
# 不扫：生成物、子模块、构建产物、文档。
$scanRoots = @(
    (Join-Path $packageRoot 'src/host'),
    (Join-Path $packageRoot 'src/include'),
    (Join-Path $packageRoot 'src/sdk/include'),
    (Join-Path $packageRoot 'lib/src'),
    (Join-Path $packageRoot 'plugins')
)
$skipDirNames = @('third_party', 'build', '.native', '.dart_tool', 'node_modules')
$extensions = @('.cpp', '.h', '.hpp', '.c', '.dart', '.js')
# 生成物（它们是工具从声明表生成的，中文只出现在注释与文档里）
$skipFileNames = @(
    'bindings_generated.dart',
    'capability_ids.g.dart',
    'ui_slots.g.dart',
    'ui_variables.g.dart',
    'capability_ids.g.h',
    'ui_slots.g.h',
    'ui_variables.g.h',
    'musicxx_ui_kit.g.h',
    'musicxx_ui_kit.js'
)

function Test-Cjk([string]$text) {
    foreach ($ch in $text.ToCharArray()) {
        $code = [int][char]$ch
        if (($code -ge 0x4E00 -and $code -le 0x9FFF) -or
            ($code -ge 0x3000 -and $code -le 0x303F) -or
            ($code -ge 0xFF00 -and $code -le 0xFFEF)) {
            return $true
        }
    }
    return $false
}

# 返回该行里"代码中的字符串字面量"列表（去掉注释之后）
function Get-CodeStrings([string]$line, [ref]$state) {
    $strings = [System.Collections.Generic.List[string]]::new()
    $current = [System.Text.StringBuilder]::new()
    $inString = $false
    $inRaw = $false
    $quoteChar = [char]0
    $i = 0
    while ($i -lt $line.Length) {
        $ch = $line[$i]
        $next = if ($i + 1 -lt $line.Length) { $line[$i + 1] } else { [char]0 }
        if ($state.Value -eq 'block') {
            if ($ch -eq '*' -and $next -eq '/') { $state.Value = 'code'; $i += 2; continue }
            $i++
            continue
        }
        if ($inRaw) {
            if ($ch -eq ')' -and $next -eq '"') {
                $strings.Add($current.ToString()); $current.Clear(); $inRaw = $false; $i += 2; continue
            }
            [void]$current.Append($ch); $i++; continue
        }
        if ($inString) {
            if ($ch -eq '\') {
                [void]$current.Append($ch)
                if ($i + 1 -lt $line.Length) { [void]$current.Append($line[$i + 1]); $i += 2; continue }
                $i++; continue
            }
            if ($ch -eq $quoteChar) {
                $strings.Add($current.ToString()); $current.Clear(); $inString = $false; $i++; continue
            }
            [void]$current.Append($ch); $i++; continue
        }
        # 代码态
        if ($ch -eq '/' -and $next -eq '/') { break }
        if ($ch -eq '/' -and $next -eq '*') { $state.Value = 'block'; $i += 2; continue }
        if ($ch -eq 'R' -and $next -eq '"') { $inRaw = $true; $i += 2; continue }
        if ($ch -eq '"' -or $ch -eq "'" -or $ch -eq '`') {
            $inString = $true; $quoteChar = $ch; $i++; continue
        }
        $i++
    }
    if ($inRaw -and $current.Length -gt 0) { $strings.Add($current.ToString()) }
    if ($inString -and $current.Length -gt 0) { $strings.Add($current.ToString()) }
    return $strings
}

$allowlist = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
if (-not $Strict -and (Test-Path $allowlistPath)) {
    foreach ($line in Get-Content -Encoding UTF8 $allowlistPath) {
        $text = $line.Trim()
        if ($text.Length -eq 0 -or $text.StartsWith('#')) { continue }
        [void]$allowlist.Add($text.Replace('/', '\'))
    }
}

$violations = @{}   # 相对路径 → 命中行号列表
$legacy = @{}       # 存量清单里的文件 → 命中行号列表

foreach ($root in $scanRoots) {
    if (-not (Test-Path $root)) { continue }
    $files = Get-ChildItem -Path $root -Recurse -File | Where-Object {
        $extensions -contains $_.Extension -and
        $skipFileNames -notcontains $_.Name -and
        ($_.FullName.Substring($packageRoot.Length + 1).Split('\') | Where-Object { $skipDirNames -contains $_ }).Count -eq 0
    }
    foreach ($file in $files) {
        $rel = $file.FullName.Substring($packageRoot.Length + 1)
        $state = 'code'
        $lines = @()
        $lineNo = 0
        foreach ($line in Get-Content -Encoding UTF8 $file.FullName) {
            $lineNo++
            $strings = Get-CodeStrings $line ([ref]$state)
            foreach ($s in $strings) {
                if (Test-Cjk $s) { $lines += $lineNo; break }
            }
        }
        if ($lines.Count -eq 0) { continue }
        if (-not $Strict -and $allowlist.Contains($rel.Replace('/', '\'))) {
            $legacy[$rel] = $lines
        } else {
            $violations[$rel] = $lines
        }
    }
}

if (-not $Quiet) {
    foreach ($rel in ($legacy.Keys | Sort-Object)) {
        Write-Host ("[log-language] 存量 (清单内): {0}  行 {1}" -f $rel, ($legacy[$rel] -join ','))
    }
}

if ($violations.Count -eq 0) {
    if ($legacy.Count -gt 0) {
        Write-Host ("[log-language] 通过 (清单内还有 {0} 个文件待改；清单只许变短)" -f $legacy.Count)
    } else {
        Write-Host '[log-language] 通过 (没有中文运行期字符串)'
    }
    exit 0
}

foreach ($rel in ($violations.Keys | Sort-Object)) {
    Write-Host ("[log-language] 中文运行期字符串: {0}  行 {1}" -f $rel, ($violations[$rel] -join ','))
}
Write-Host ("[log-language] 失败 ({0} 个文件)" -f $violations.Count)
exit 1
