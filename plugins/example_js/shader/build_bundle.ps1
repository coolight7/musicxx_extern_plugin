# 打包示例插件的 shader bundle（Windows 用 pwsh，Linux/macOS 用 bash 版见下一段注释）
#
# 用法：
#   pwsh -NoProfile -File .\build_bundle.ps1 [-FlutterRoot <Flutter SDK 根目录>]
#
# 产物：bg.shaderbundle（与 bundle.json 同目录），随插件一起分发。
param(
    [string]$FlutterRoot = ""
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

if ([string]::IsNullOrEmpty($FlutterRoot)) {
    # 优先用当前 flutter（fvm 环境也能取到）
    $flutterCmd = Get-Command flutter -ErrorAction SilentlyContinue
    if ($null -ne $flutterCmd) {
        $FlutterRoot = Split-Path -Parent (Split-Path -Parent $flutterCmd.Source)
    }
}
if ([string]::IsNullOrEmpty($FlutterRoot) -or -not (Test-Path $FlutterRoot)) {
    throw "找不到 Flutter SDK：请用 -FlutterRoot 指定（例如 C:\Users\me\fvm\versions\3.47.5）"
}

$impellerc = Get-ChildItem -Path (Join-Path $FlutterRoot "bin\cache\artifacts\engine") `
    -Recurse -Filter "impellerc.exe" -ErrorAction SilentlyContinue |
    Select-Object -First 1 -ExpandProperty FullName
if ([string]::IsNullOrEmpty($impellerc)) {
    throw "找不到 impellerc.exe（$FlutterRoot\bin\cache\artifacts\engine）"
}

$spec = Get-Content (Join-Path $here "bundle.json") -Raw
& $impellerc --shader-bundle="$spec" --sl=(Join-Path $here "bg.shaderbundle")
if ($LASTEXITCODE -ne 0) {
    throw "impellerc 编译失败（exit=$LASTEXITCODE）"
}
Write-Output "已生成: $(Join-Path $here 'bg.shaderbundle')"

# Linux / macOS：同一目录下的 impellerc（无 .exe 后缀）用同样的参数调用，或直接跑：
#   "$FLUTTER_ROOT/bin/cache/artifacts/engine/linux-x64/impellerc" \
#     --shader-bundle="$(cat bundle.json)" --sl=bg.shaderbundle
