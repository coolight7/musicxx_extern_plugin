# musicxx_extern_plugin 开发编译环境配置脚本
#
# 做一次就够: 把"编译插件、编译插件框架(插件框架库)"需要的东西全部准备好并记到缓存里, 之后
# tools/build_native.ps1 与 tools/build_examples.ps1 直接复用这份缓存, 不再重复探测工具链、
# 不再重复解析 Boost (重复执行本脚本只是读一遍缓存并打印结果, 秒级返回)。
#
# 具体做这些事:
#   - 工具预检: cmake / git / Visual Studio (或其它生成器) / ninja (Android) / pkg-config
#   - 依赖子模块是否已初始化 (未初始化时给出 git submodule update --init --recursive)
#   - Boost 头文件 (只用 Boost.Asio 头, 不链接任何 Boost 编译库) 的解析与缓存:
#       本包内置(src/third_party/boost/include) → .native/boost 缓存 → 按固定版本下载并校验 SHA256
#   - Android 交叉编译目标: NDK 位置与工具链文件
#   - 把结果写进 <包>/.native/env/environment.json (可重建的本地缓存; 删掉它就会重新解析)
#
# 用法:
#   pwsh -NoProfile -File tools/setup_native_env.ps1              # 解析并写缓存 (已就绪时秒返回)
#   pwsh -NoProfile -File tools/setup_native_env.ps1 -Check       # 只检查并报告, 不下载也不写缓存
#   pwsh -NoProfile -File tools/setup_native_env.ps1 -Force       # 忽略缓存重新解析
#   pwsh -NoProfile -File tools/setup_native_env.ps1 -Android     # 顺带解析 Android NDK (交叉编译用)
#   pwsh -NoProfile -File tools/setup_native_env.ps1 -BoostRoot <含 boost/ 的目录>
#   pwsh -NoProfile -File tools/setup_native_env.ps1 -BoostArchive <Boost 发布包>   # 离线/自备包
#   pwsh -NoProfile -File tools/setup_native_env.ps1 -NoDownloadBoost               # 禁止自动下载
#
# 环境就绪之后:
#   pwsh -NoProfile -File tools/build_native.ps1     构建插件框架 (插件框架库 + 插件 SDK)
#   pwsh -NoProfile -File tools/build_examples.ps1   构建(并打包)示例插件
#   flutter build windows / flutter build apk ...    应用构建会自动触发插件框架构建(已是最新则跳过)
#
# 说明: 下面用了 [CmdletBinding()] —— 参数名写错时直接报错, 而不是被当成多余参数悄悄忽略
# (本脚本有几十个开关, 静默忽略一个拼错的开关会表现成"改了参数却没生效")。
[CmdletBinding()]
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [string]$BoostRoot,
    [string]$BoostArchive,
    [switch]$NoDownloadBoost,
    # OpenSSL (客户端 TLS: https / wss): 打印/缓存解析结果
    [string]$OpenSslRoot,
    [switch]$NoDownloadOpenSsl,
    [switch]$NoTls,
    [switch]$Check,
    [switch]$Force,
    [switch]$Quiet,
    [string]$Generator,
    # Android 交叉编译环境 (NDK 位置); 桌面端不需要
    [switch]$Android,
    [string]$AndroidNdk,
    [string]$AndroidPlatform = 'android-24',
    [ValidateSet('c++_static', 'c++_shared')][string]$AndroidStl = 'c++_static'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'native_common.ps1')
. (Join-Path $PSScriptRoot 'deps_openssl.ps1')

$Root = (Resolve-Path $Root).Path
Initialize-MxBuildEnvironment

Write-Host '=== musicxx_extern_plugin 开发编译环境 ==='
Write-MxInfo "包目录:     $Root"
if ($Check) { Write-MxInfo '模式:       只检查 (-Check): 不下载、不写缓存' }

$environment = Invoke-MxEnsureEnvironment -Root $Root -BoostRoot $BoostRoot -BoostArchive $BoostArchive `
    -NoDownloadBoost:$NoDownloadBoost -Check:$Check -Force:$Force -Android:$Android `
    -AndroidNdk $AndroidNdk -AndroidPlatform $AndroidPlatform -AndroidStl $AndroidStl -Generator $Generator

Write-MxStep '环境就绪情况'
Write-MxInfo "平台/架构:  $($environment.Platform) / $($environment.Arch)"
Write-MxInfo "生成器:     $($environment.Generator)"
Write-MxInfo "cmake:      $($environment.Tools.CmakeVersion)"
if ($environment.Tools.Ninja) { Write-MxInfo "ninja:      $($environment.Tools.Ninja)" }
if ($environment.Tools.PkgConfig) { Write-MxInfo "pkg-config: $($environment.Tools.PkgConfig)" }

$boostSourceText = switch ($environment.Boost.Source) {
    'explicit' { '使用 -BoostRoot 指定的头文件' }
    'archive' { '使用 -BoostArchive 解包出的头文件' }
    'vendored' { '复用本包内置头文件 src/third_party/boost/include' }
    'cache' { '复用已解包缓存 .native/boost' }
    'download' { '已下载并校验官方发布包' }
    'download(未下载)' { '缺失: 正式执行本脚本时会自动下载(需要网络, 约 150 MB)' }
    'archive(未解包)' { '将使用 -BoostArchive 指定的发布包(尚未解包)' }
    default { $environment.Boost.Source }
}
if ($environment.Boost.Root) {
    Write-MxInfo "Boost:      $boostSourceText -> $($environment.Boost.Root) (版本 $($environment.Boost.Version))"
}
else {
    Write-MxWarn "Boost:      $boostSourceText"
}
Write-MxInfo "子模块:     $($environment.Submodules.Total) 个已初始化"

# OpenSSL (客户端 TLS): 提前解析/下载好, 构建时就不用再等
$openssl = [ordered]@{ Root = ''; Version = ''; Source = '' }
if ($NoTls) {
    Write-MxInfo 'OpenSSL:    -NoTls: 构建出来的插件框架不带 TLS (https / wss 不可用)'
}
else {
    $openssl = Resolve-MxOpenSslRoot -Root $Root -OpenSslRoot $OpenSslRoot `
        -NoDownloadOpenSsl:$NoDownloadOpenSsl -Check:$Check
    if ($openssl.Root) {
        Write-MxInfo "OpenSSL:    $($openssl.Root) (版本 $($openssl.Version), 来源 $($openssl.Source))"
    }
    elseif ($openssl.Source) {
        Write-MxWarn "OpenSSL:    $($openssl.Source) (正式执行本脚本时会自动下载, 完整包约 260 MB)"
    }
    else {
        Write-MxWarn 'OpenSSL:    缺失 (构建出来的插件框架不带 TLS; https / wss 报 tls_unavailable)'
    }
}
if ($Android -or $environment.Android.Toolchain) {
    Write-MxInfo "Android NDK: $($environment.Android.Ndk)"
    Write-MxInfo "  工具链:    $($environment.Android.Toolchain)"
    Write-MxInfo "  目标:      $($environment.Android.Platform), $($environment.Android.Stl)"
}

if ($Check) {
    Write-Host ''
    Write-Host '检查完成: 没有写缓存 (去掉 -Check 就会把上面的解析结果写进缓存)'
    exit 0
}

if ($environment.Reused) {
    Write-MxStep '缓存复用'
    Write-MxOk "环境描述文件仍然有效, 未重新探测: $($environment.File)"
}
else {
    Write-MxStep '缓存已更新'
    Write-MxOk "环境描述文件: $($environment.File)"
    Write-MxInfo '下次构建会直接复用这份结果 (-Force 可强制重新解析)'
}

if (-not $Quiet) {
    Write-Host ''
    Write-Host '下一步:'
    Write-Host '  pwsh -NoProfile -File tools/build_native.ps1     构建插件框架(插件框架库 + 插件 SDK)'
    Write-Host '  pwsh -NoProfile -File tools/build_examples.ps1   构建(并打包)示例插件'
    Write-Host '  也可以直接 flutter build windows / flutter build apk: 应用构建会自动触发插件框架的构建'
}
Write-Host '完成'
