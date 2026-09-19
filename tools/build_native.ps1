# 原生依赖与宿主库构建 (plan §11.2 的两阶段构建)
#
# 阶段一: 从 src/third_party 的子模块源码编译依赖 (fmt / yaml-cpp / cxx_utilxx_base / cxx_pluginxx)
#         安装到 .native/<platform>-<arch>/   —— 该目录是"本地构建产物", 不入版本库
# 阶段二: 用该前缀配置宿主库、示例插件与原生测试
#
# 用法:
#   pwsh -NoProfile -File tools/build_native.ps1                     # 阶段一 + 阶段二 (Release)
#   pwsh -NoProfile -File tools/build_native.ps1 -DepsOnly           # 只构建依赖
#   pwsh -NoProfile -File tools/build_native.ps1 -HostOnly           # 只构建宿主 (依赖已就绪)
#   pwsh -NoProfile -File tools/build_native.ps1 -BoostDir <path> -BoostInclude <path>
#
# 关于 Boost: 目前未子模块化 (见 third_party_versions.json 的 external 段)。脚本按以下顺序找 Boost:
#   1) -BoostDir / -BoostInclude 显式参数
#   2) 环境变量 BOOST_DIR / BOOST_INCLUDE
#   3) 同机既有的 Boost 构建 (agentxx 的 boost-windows-build-release) —— 仅作为临时回退
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [string]$Config = 'Release',
    [switch]$DepsOnly,
    [switch]$HostOnly,
    [string]$BoostDir = $env:BOOST_DIR,
    [string]$BoostInclude = $env:BOOST_INCLUDE,
    [string]$AgentxxFallback = 'D:/0Acoolight/Program/cpp/agentxx/agent/third_party'
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path $Root).Path
$thirdParty = Join-Path $Root 'src/third_party'

# 平台标签 (与 third_party_versions.json / 文档一致)
$os = if ($IsWindows -or $env:OS -eq 'Windows_NT') { 'windows' } elseif ($IsMacOS) { 'macos' } else { 'linux' }
$arch = switch ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()) {
    'X64'   { 'x64' }
    'Arm64' { 'arm64' }
    'X86'   { 'x86' }
    default { 'unknown' }
}
$prefix = Join-Path $Root ".native/$os-$arch"
$buildRoot = Join-Path $Root '.native/build'

# Boost 回退解析
if (-not $BoostDir -and -not $BoostInclude) {
    $fb = Join-Path $AgentxxFallback 'boost-windows-build-release'
    if (Test-Path (Join-Path $fb 'lib/cmake/Boost-1.92.0')) {
        $BoostDir = "$fb/lib/cmake/Boost-1.92.0"
        $BoostInclude = "$fb/include"
        Write-Output "[deps] 未指定 Boost, 回退使用既有 Boost 构建: $fb"
    }
}
if ($BoostDir) { Write-Output "[deps] Boost_DIR = $BoostDir" }
if ($BoostInclude) { Write-Output "[deps] Boost include = $BoostInclude" }

New-Item -ItemType Directory -Force -Path $prefix, $buildRoot | Out-Null

function Invoke-CmakeStep {
    param([string]$Name, [string]$Source, [string[]]$ConfigureArgs)
    $buildDir = Join-Path $buildRoot $Name
    Write-Output ""
    Write-Output "==> [$Name] configure"
    & cmake -S $Source -B $buildDir @ConfigureArgs
    if ($LASTEXITCODE -ne 0) { throw "[$Name] configure 失败" }
    Write-Output "==> [$Name] build"
    & cmake --build $buildDir --config $Config --parallel
    if ($LASTEXITCODE -ne 0) { throw "[$Name] build 失败" }
    Write-Output "==> [$Name] install"
    & cmake --install $buildDir --config $Config
    if ($LASTEXITCODE -ne 0) { throw "[$Name] install 失败" }
}

function Get-CommonPrefixArgs {
    $args = @("`"-DCMAKE_INSTALL_PREFIX=$prefix`"")
    # 依赖之间互相 find_package: 本前缀 + Boost
    $prefixPath = @($prefix)
    if ($BoostDir) { $prefixPath += (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $BoostDir))) }
    $args += "`"-DCMAKE_PREFIX_PATH=$($prefixPath -join ';')`""
    return $args
}

if (-not $HostOnly) {
    # 依赖按依赖顺序构建 (fmt → yaml-cpp → utilxx_base → pluginxx)
    $common = @(
        "-G", 'Visual Studio 18 2026', '-A', 'x64',
        "-DCMAKE_INSTALL_PREFIX=$prefix",
        "-DCMAKE_PREFIX_PATH=$prefix",
        "-DBUILD_SHARED_LIBS=OFF"
    )
    if ($BoostDir) { $common += "-DBoost_DIR=$BoostDir" }
    if ($BoostInclude) { $common += "-DBOOST_INCLUDE_DIR=$BoostInclude" }

    Invoke-CmakeStep -Name 'fmt' -Source (Join-Path $thirdParty 'fmt') -ConfigureArgs ($common + @('-DFMT_TEST=OFF', '-DFMT_DOC=OFF'))
    Invoke-CmakeStep -Name 'yaml-cpp' -Source (Join-Path $thirdParty 'yaml-cpp') -ConfigureArgs ($common + @('-DYAML_CPP_BUILD_TESTS=OFF', '-DYAML_CPP_BUILD_TOOLS=OFF'))
    Invoke-CmakeStep -Name 'cxx_utilxx_base' -Source (Join-Path $thirdParty 'cxx_utilxx_base') -ConfigureArgs ($common + @(
        '-DXX_IS_WIN_D=1', '-DXX_IS_MSVC_D=1', '-DCXX_UTILXX_BASE_LINUX_IO_URING_SUPPORTED=OFF'
    ))
    Invoke-CmakeStep -Name 'cxx_pluginxx' -Source (Join-Path $thirdParty 'cxx_pluginxx') -ConfigureArgs ($common + @(
        '-DXX_IS_WIN_D=1', '-DXX_IS_MSVC_D=1', '-DXX_IS_RELEASE_D=1', '-DXX_IS_DEBUG_D=0',
        '-DXX_IS_LINUX_D=0', '-DXX_IS_MACOS_D=0', '-DXX_IS_ANDROID_D=0', '-DXX_IS_IOS_D=0',
        '-DXX_IS_GCC_D=0', '-DXX_IS_CLANG_D=0', '-DXX_IS_MINGW_D=0'
    ))
}

if (-not $DepsOnly) {
    Write-Output ""
    Write-Output "==> [host] configure"
    $hostArgs = @(
        "-G", 'Visual Studio 18 2026', '-A', 'x64',
        "-DMUSICXX_EXTERN_PLUGIN_NATIVE_PREFIX=$prefix",
        "-DMUSICXX_EXTERN_PLUGIN_DEP_PREFIX=$prefix"
    )
    if ($BoostDir) { $hostArgs += "-DBoost_DIR=$BoostDir" }
    & cmake -S (Join-Path $Root 'src') -B (Join-Path $buildRoot 'host') @hostArgs
    if ($LASTEXITCODE -ne 0) { throw '[host] configure 失败' }
    Write-Output "==> [host] build"
    & cmake --build (Join-Path $buildRoot 'host') --config $Config --parallel
    if ($LASTEXITCODE -ne 0) { throw '[host] build 失败' }
}

Write-Output ""
Write-Output "完成: 前缀=$prefix  构建目录=$buildRoot"
