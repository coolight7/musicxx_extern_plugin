# musicxx_extern_plugin 示例插件构建（与打包）脚本
#
# 示例插件是"给插件作者照抄的样板 + 给插件框架保护机制做对照的坏样本"，插件框架本身不需要它们：
# 因此把它们从框架构建脚本里分出来，单独一个脚本负责构建、并按需要打成可安装的 .zip。
#
# 做的事:
#   1. 先构建插件框架(插件框架库 + 插件 SDK): 复用 .native/env/environment.json 缓存，已是最新就有几秒钟的
#      增量构建开销 —— 示例插件的库文件要链接插件框架库与内核，必须有它;
#   2. 构建示例插件(plugins/ 下的动态库插件 + JS 插件); 默认连原生测试与对照示例一起构建;
#   3. 把构建产物里的每个插件目录打包成安装包 .zip (顶层就是插件目录内容，可直接「从压缩包安装」)。
#      包名带上插件版本号: <插件 id>-<清单 version 里的 . 换成 _>.zip，例 example_js_shader-2026_1010_1.zip
#      (版本号约定 `<年>.<月日>.<小版本号>`，例 2026.1010.1 = 2026-10-10 的第 1 版)。
#
# 用法:
#   pwsh -NoProfile -File tools/build_examples.ps1                 # 构建示例插件 + 打包 (已是最新则直接打包)
#   pwsh -NoProfile -File tools/build_examples.ps1 -Force          # 不跳过, 强制重新构建
#   pwsh -NoProfile -File tools/build_examples.ps1 -NoPack         # 只构建，不打包
#   pwsh -NoProfile -File tools/build_examples.ps1 -NoTests        # 不构建原生测试与对照示例
#   pwsh -NoProfile -File tools/build_examples.ps1 -RunTests       # 构建后跑原生测试
#   pwsh -NoProfile -File tools/build_examples.ps1 -NoBuild        # 只用已有产物打包
#   pwsh -NoProfile -File tools/build_examples.ps1 -PackOut D:/out # 指定 .zip 的输出目录
#   pwsh -NoProfile -File tools/build_examples.ps1 -Only example_native,example_js   # 只打包指定插件
#   pwsh -NoProfile -File tools/build_examples.ps1 -Config Debug -Jobs 8
#   pwsh -NoProfile -File tools/build_examples.ps1 -Android -Abi arm64-v8a           # Android 交叉编译
#
# 装到主程序里试:
#   musicxx-cli plugin install "<包>/.native/output/<平台>-<配置>/packages/<插件id>-<版本号>.zip" --enable
#
# 说明: [CmdletBinding()] 让写错的参数名直接报错, 而不是被当成多余参数悄悄忽略。
[CmdletBinding()]
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [ValidateSet('Release', 'Debug')][string]$Config = 'Release',
    [switch]$NoBuild,
    [switch]$Force,
    [switch]$NoTests,
    [switch]$RunTests,
    [switch]$NoPack,
    [string]$PackOut,
    [string[]]$Only = @(),
    [switch]$Clean,
    [int]$Jobs = 0,
    # Android 交叉编译: 与 tools/build_native.ps1 同名参数
    [switch]$Android,
    [ValidateSet('arm64-v8a', 'armeabi-v7a', 'x86_64', 'x86')][string]$Abi = 'arm64-v8a'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'native_common.ps1')

$Root = (Resolve-Path $Root).Path
Initialize-MxBuildEnvironment
$paths = Get-MxBuildPaths -Root $Root -Config $Config -Android:$Android -Abi $Abi
$packDir = if ($PackOut) { [System.IO.Path]::GetFullPath($PackOut) } else { Join-Path $paths.OutputDir 'packages' }

# 用当前这个 PowerShell 进程再开子进程跑别的脚本: 隔离作用域(子脚本里的 exit 不会带走本脚本),
# 也让"构建失败"以退出码的形式回到这里。
$pwshExe = Join-Path $PSHOME $(if ($IsWindows -or $env:OS -eq 'Windows_NT') { 'pwsh.exe' } else { 'pwsh' })
if (-not (Test-Path $pwshExe)) {
    $pwshExe = Join-Path $PSHOME $(if ($IsWindows -or $env:OS -eq 'Windows_NT') { 'powershell.exe' } else { 'powershell' })
}
if (-not (Test-Path $pwshExe)) { $pwshExe = 'pwsh' }

Write-Host '=== musicxx_extern_plugin 示例插件构建 ==='
Write-MxInfo "包目录:   $Root"
Write-MxInfo "配置:     $Config   目标: $(if ($Android) { "android-$Abi" } else { "$($paths.Os)-$($paths.Arch)" })"
Write-MxInfo "插件目录: $($paths.PluginsDir)"
Write-MxInfo "打包目录: $(if ($NoPack) { '(本次不打包)' } else { $packDir })"

# ==================== 1. 构建框架 + 示例插件 ====================

if (-not $NoBuild) {
    $buildArgs = @(
        '-NoProfile', '-File', (Join-Path $PSScriptRoot 'build_native.ps1'),
        '-Root', $Root,
        '-Config', $Config,
        '-WithExamples',
        # 已是最新就跳过构建, 直接去打包 (要强制重建用 -Force)
        '-SkipIfFresh'
    )
    if (-not $NoTests) { $buildArgs += '-WithTests' }
    if ($RunTests) { $buildArgs += '-RunTests' }
    if ($Force) { $buildArgs += '-Force' }
    if ($Clean) { $buildArgs += '-Clean' }
    if ($Jobs -gt 0) { $buildArgs += @('-Jobs', "$Jobs") }
    if ($Android) { $buildArgs += @('-Android', '-Abi', $Abi) }

    Write-Host ''
    Write-Host "==> 构建插件框架 + 示例插件$(if (-not $NoTests) { ' + 原生测试' })$(if ($Force) { ' (强制重建)' } else { ' (已是最新则跳过)' })"
    & $pwshExe @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "构建失败 (退出码 $LASTEXITCODE); 单独重跑可看完整日志: pwsh -File tools/build_native.ps1 -WithExamples" }
}

if (-not (Test-Path $paths.PluginsDir)) {
    throw "没有找到构建出来的示例插件目录: $($paths.PluginsDir)`n  请先不带 -NoBuild 跑一次本脚本"
}

# ==================== 2. 列出构建产物里的插件 ====================

# 插件目录的判据与插件框架一致: 目录里要有 plugin.yaml
$plugins = @()
foreach ($dir in (Get-ChildItem $paths.PluginsDir -Directory | Sort-Object -Property Name)) {
    if (-not (Test-Path (Join-Path $dir.FullName 'plugin.yaml'))) { continue }
    if ($Only.Count -gt 0) {
        $matched = $false
        foreach ($pattern in $Only) {
            if ($dir.Name -like $pattern) { $matched = $true; break }
        }
        if (-not $matched) { continue }
    }
    $files = Get-ChildItem $dir.FullName -Recurse -File
    $plugins += [pscustomobject]@{
        Id      = $dir.Name
        Path    = $dir.FullName
        Version = Read-MxPluginVersion $dir.FullName
        Count   = $files.Count
        Bytes   = ($files | Measure-Object -Property Length -Sum).Sum
        Broken  = $dir.Name -match '(_fail|_bad_entry|_broken)$'
    }
}

Write-MxStep '示例插件'
if ($plugins.Count -eq 0) {
    throw "在 $($paths.PluginsDir) 下没有找到插件目录 (目录里要有 plugin.yaml)"
}
foreach ($plugin in $plugins) {
    $mark = if ($plugin.Broken) { ' [对照示例: 故意做坏的]' } else { '' }
    $ver = if ($plugin.Version -ne '') { $plugin.Version } else { '未声明版本' }
    Write-MxInfo ("{0,-24} {1,4} 个文件 {2,9:N0} 字节  版本 {3}{4}" -f $plugin.Id, $plugin.Count, $plugin.Bytes, $ver, $mark)
}
if ($RunTests) { Write-MxInfo '原生测试已在构建阶段执行 (见上方输出)' }

# ==================== 3. 打包成安装包 (.zip) ====================

if ($NoPack) {
    Write-Host ''
    Write-Host "构建完成 (未打包); 需要安装包时去掉 -NoPack"
    exit 0
}

Write-MxStep "打包 (输出到 $packDir)"
New-Item -ItemType Directory -Force -Path $packDir | Out-Null
$packScript = Join-Path $PSScriptRoot 'pack_plugin.ps1'
$zips = @()
foreach ($plugin in $plugins) {
    # 包名 = <插件 id>-<清单 version 里的 . 换成 _>.zip (与 pack_plugin.ps1 的默认名一致)
    $zipName = Get-MxPluginPackageFileName -PluginId $plugin.Id -Version $plugin.Version
    if ($plugin.Version -eq '') { Write-MxWarn "$($plugin.Id): 清单没写 version，包名不带版本号" }
    $zip = Join-Path $packDir $zipName
    & $pwshExe -NoProfile -File $packScript -PluginDir $plugin.Path -Out $zip
    if ($LASTEXITCODE -ne 0) { throw "打包失败: $($plugin.Id) (退出码 $LASTEXITCODE)" }
    if (Test-Path $zip) {
        $zips += [pscustomobject]@{
            Id    = $plugin.Id
            Name  = $zipName
            Path  = $zip
            Bytes = (Get-Item $zip).Length
            Sha   = (Get-FileHash -Algorithm SHA256 $zip).Hash.ToLower()
        }
    }
}

Write-Host ''
Write-Host "示例插件构建与打包完成: $($zips.Count) 个安装包"
foreach ($zip in $zips) {
    Write-MxInfo ("{0,-40} {1,9:N2} MiB  sha256={2}" -f $zip.Name, ($zip.Bytes / 1MB), $zip.Sha.Substring(0, 16))
}
Write-Host ''
Write-Host '装到主程序里试 (主程序需已开启『拟声++』):'
Write-Host "  musicxx-cli plugin install `"$($zips[0].Path)`" --enable"
Write-Host '  也可以直接把插件目录放进 <应用支持目录>/musicxx/extern_plugin/plugins/ 再在管理页重新扫描'
