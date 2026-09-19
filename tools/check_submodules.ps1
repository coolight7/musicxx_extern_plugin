# 依赖子模块检查
#
# 依赖库全部来自 src/third_party 下的 git 子模块。**子模块的 commit 由仓库自己的 gitlink
# 记录**（外层仓库提交时写入），不需要额外的版本清单文件；因此这里只做两件事：
#   1) .gitmodules 里登记的子模块是否都已初始化（未初始化时给出拉取命令）
#   2) 打印 URL 与当前 commit，方便人工对照
#
# 用法: pwsh -NoProfile -File tools/check_submodules.ps1
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path $Root).Path

Write-Output "=== 依赖子模块检查 ($Root) ==="

$status = & git -C $Root submodule status
$failed = 0
foreach ($line in $status) {
    if (-not $line) { continue }
    # 行首标记: ' ' 与记录一致 / '+' 与记录不同 / '-' 未初始化 / 'U' 冲突
    $trimmed = $line.Trim()
    $marker = $trimmed.Substring(0, 1)
    $initialized = $marker -ne '-'
    $fields = ($trimmed -replace '^[-+U\s]+', '') -split '\s+'
    $commit = $fields[0]
    $path = $fields[1]
    $url = ''
    try { $url = (& git -C $Root config -f .gitmodules --get "submodule.$path.url" 2>$null) } catch {}
    if (-not $initialized) {
        $failed++
        Write-Output ("[缺失] {0,-34} 未初始化; 请执行 git submodule update --init --recursive" -f $path)
    }
    elseif ($marker -eq '+') {
        Write-Output ("[注意] {0,-34} {1} @ {2} (工作区 HEAD 与仓库记录不同, 属正常开发状态)" -f $path, $commit.Substring(0, 12), $url)
    }
    else {
        Write-Output ("[ok]   {0,-34} {1} @ {2}" -f $path, $commit.Substring(0, 12), $url)
    }
}

Write-Output ""
if ($failed -gt 0) {
    Write-Output "未初始化子模块: $failed"
    exit 1
}
Write-Output "全部子模块已就绪 (commit 由外层仓库的 gitlink 锁定, 无需额外清单文件)"
