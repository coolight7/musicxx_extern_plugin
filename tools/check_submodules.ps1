# 依赖子模块校验 (plan §3.3)
#
# 作用: 打印每个 submodule 的 URL 与 commit, 并与 third_party_versions.json 比对;
#       URL 仍是本地路径时给出"切换为上游 URL"的命令提示 (上游推送后执行一次即可)。
#
# 用法: pwsh -NoProfile -File tools/check_submodules.ps1
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path $Root).Path
$versionsFile = Join-Path $Root 'third_party_versions.json'
$versions = Get-Content -Raw $versionsFile -Encoding UTF8 | ConvertFrom-Json

$failed = 0
Write-Output "=== 依赖子模块校验 ($Root) ==="
foreach ($sub in $versions.submodules) {
    $path = Join-Path $Root $sub.path
    if (-not (Test-Path $path)) {
        Write-Output ("[缺失] {0,-32} 未初始化; 请执行 git submodule update --init --recursive" -f $sub.path)
        $failed++
        continue
    }
    $head = (git -C $path rev-parse HEAD).Trim()
    $url = ''
    try { $url = (git -C $path config --get remote.origin.url).Trim() } catch {}
    $commitOk = ($head -eq $sub.commit)
    $isLocal = ($url -notmatch '^https?://')
    $flag = if ($commitOk) { 'ok' } else { 'COMMIT 不符' }
    if (-not $commitOk) { $failed++ }
    Write-Output ("[{0}] {1,-32} {2} @ {3}" -f $flag, $sub.path, $head.Substring(0, 12), $url)
    if ($isLocal) {
        Write-Output ("      ↑ URL 为本地路径; 上游推送后执行: git submodule set-url {0} {1}; git submodule sync {0}" -f $sub.path, $sub.url)
    }
}
Write-Output ""
if ($failed -gt 0) {
    Write-Output "校验失败项: $failed"
    exit 1
}
Write-Output "全部子模块 commit 与 third_party_versions.json 一致"
