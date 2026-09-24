# 打包示例插件的 shader bundle（Windows 用 pwsh）
#
# 用法：
#   pwsh -NoProfile -File .\build_bundle.ps1 [-FlutterRoot <Flutter SDK 根目录>]
#
# 产物：bg.shaderbundle（与 bundle.json 同目录），随插件一起分发。
#
# Linux / macOS 用同样的参数调用对应平台的 impellerc（无 .exe 后缀）：
#   "$FLUTTER_ROOT/bin/cache/artifacts/engine/linux-x64/impellerc" \
#     --shader-bundle="$(tr -d '\n' < bundle.json)" --sl=bg.shaderbundle
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
    throw '找不到 Flutter SDK：请用 -FlutterRoot 指定（例如 C:\Users\me\fvm\versions\3.47.5）'
}

$engineDir = Join-Path $FlutterRoot 'bin\cache\artifacts\engine'
$impellerc = Get-ChildItem -Path $engineDir -Recurse -Filter 'impellerc.exe' -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty FullName
if ([string]::IsNullOrEmpty($impellerc)) {
    throw "找不到 impellerc.exe（$engineDir）"
}

# bundle 描述必须以「单行」传给 --shader-bundle：带换行时会被当成多个参数，
# impellerc 会报 "Target shading language file name was empty"
$spec = ((Get-Content (Join-Path $here 'bundle.json') -Raw) -replace "`r?`n", ' ').Trim()
$out = Join-Path $here 'bg.shaderbundle'

& $impellerc --shader-bundle="$spec" --sl="$out"
if ($LASTEXITCODE -ne 0) {
    throw "impellerc 编译失败（exit=$LASTEXITCODE）"
}
Write-Output "已生成: $out ($((Get-Item $out).Length) 字节)"
