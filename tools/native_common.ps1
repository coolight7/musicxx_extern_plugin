# musicxx_extern_plugin 构建脚本公共库
#
# 这个文件不是给用户直接执行的: 它被下面几个脚本点源引入(dot-source) —— 放的是它们必须保持
# 一致的判定与实现, 免得"配置环境的脚本"和"构建的脚本"各写一份, 时间一长就走样:
#
#   tools/setup_native_env.ps1   配置开发编译环境 (工具预检 / 子模块 / Boost / NDK → 环境描述文件)
#   tools/build_native.ps1       构建插件框架 (插件框架库 + 插件 SDK)
#   tools/build_examples.ps1     构建示例插件 (可选打包成安装包 .zip)
#   tools/pack_plugin.ps1        把一个插件目录打成安装包 (包名里的版本号取自清单 version)
#
# 依赖库的"从哪来"另放在 tools/deps_openssl.ps1 (OpenSSL / 客户端 TLS; 由构建脚本点源引入)。
#
# 环境描述文件: <包>/.native/env/environment.json
#   配置环境时解析出来的东西(生成器 / 各工具路径 / Boost 头文件根 / NDK)写在这里, 构建时直接复用,
#   于是"重复构建不再重新探测工具链、不再重复解析 Boost" —— 这就是缓存复用的落点。
#   文件全部可重建: 删掉它(或 -Force)会让下次构建重新解析一遍。
#
# 约定:
#   - 函数名统一 Mx 前缀; 需要回传多项内容的地方用 [ordered]@{} 一次性返回;
#   - 所有进度/提示一律 Write-Host (函数里的 Write-Output 会混进返回值);
#   - 数据访问一律走 Get-MxProp / Contains: 本库在 Set-StrictMode -Version Latest 下运行,
#     直接读不存在的属性会抛异常。

Set-StrictMode -Version Latest

# ==================== 常量 ====================

# Boost (仅头文件)。版本、下载地址与校验值与 tools/build_native.sh 里的常量必须一致。
$script:MxBoostVersion = '1.92.0'
$script:MxBoostSourceUrl = 'https://archives.boost.io/release/@VERSION@/source/boost_@UNDERSCORE@.tar.gz'
$script:MxBoostSha256 = 'c4a3b310ddd2472416e091067166b0713be97c63f38c212c484ada022fd296ce'

# 环境描述文件的格式版本 (字段含义变化时 +1, 旧文件会被当成过期重新解析)
$script:MxEnvSchema = 1

# ==================== 输出 ====================

function Write-MxStep([string]$Text) {
    Write-Host ""
    Write-Host "==> $Text"
}

function Write-MxInfo([string]$Text) {
    Write-Host "  $Text"
}

function Write-MxWarn([string]$Text) {
    Write-Host "  [警告] $Text" -ForegroundColor Yellow
}

function Write-MxOk([string]$Text) {
    Write-Host "  $Text" -ForegroundColor Green
}

# ==================== 插件安装包命名 ====================

# 插件版本号的约定是 `<年>.<月日>.<小版本号>`: 例 2026.1010.1 = 2026-10-10 的第 1 版
# (月日是 4 位 MMDD, 1—9 月补 0)。插件框架自己的版本号用同一套格式, 见 docs/version-history.md。
# 安装包文件名要带上版本号, 所以打包脚本自己读一次清单 —— 这里只做轻量正则, 清单的权威解析在插件框架
# (src/host/host_manifest.cpp)。
function Read-MxPluginVersion([string]$PluginDir) {
    $manifestPath = Join-Path $PluginDir 'plugin.yaml'
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { return '' }
    $text = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8
    $match = [regex]::Match($text, '(?m)^\s*version\s*:\s*(.+?)\s*$')
    if (-not $match.Success) { return '' }
    return $match.Groups[1].Value.Trim().Trim('"').Trim("'")
}

# 安装包文件名: <插件 id>-<版本号, 其中的 . 换成 _>.zip
# 例: example_js_shader (version: 2026.1010.1) → example_js_shader-2026_1010_1.zip
# 清单没写 version 时退回 <插件 id>.zip (只是打包脚本的命名, 不影响插件框架怎么读清单)。
function Get-MxPluginPackageFileName([string]$PluginId, [string]$Version) {
    $tag = $Version.Trim().Trim('"').Trim("'") -replace '\.', '_'
    if ($tag -eq '') { return "$PluginId.zip" }
    return "$PluginId-$tag.zip"
}

# 构建环境变量 (与 build_native.sh / 旧版 build_native.ps1 里同样的几项)
#
# [ConsoleEncoding] 说明: 直接用 pwsh 跑脚本时用 utf8 (控制台能正常显示中文);
# 被构建系统(CMake / Gradle)当子进程调起来时它们按**系统 ANSI 代码页**解码子进程输出,
# 这时要用 'system' 写, 否则 Flutter / Gradle 的日志里中文会变成乱码。
function Initialize-MxBuildEnvironment {
    param([ValidateSet('utf8', 'system')][string]$ConsoleEncoding = 'utf8')

    # 控制台按 UTF-8: 顶层 CMakeLists 会据此写 Directory.Build.targets,
    # 强制 MSBuild 的 CustomBuild 任务按 UTF-8 解码嵌套构建的输出 (修中文乱码)
    $env:MUSICXX_EXT_BUILD_CONSOLE_CP = '65001'
    try {
        if ($ConsoleEncoding -eq 'utf8') {
            [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
        }
        else {
            $ansiCodePage = [System.Globalization.CultureInfo]::CurrentCulture.TextInfo.ANSICodePage
            [Console]::OutputEncoding = [System.Text.Encoding]::GetEncoding($ansiCodePage)
        }
    }
    catch { }
    # MSVC 输出英文诊断
    $env:VSLANG = '1033'
    # 完全停用 vcpkg: 依赖全部来自 third_party 源码构建, 不需要 vcpkg;
    # VCPKG_ROOT 置空停用 CMake 工具链集成, VCPkgLocalAppDataDisabled=1 停用 MSBuild 的
    # 用户级集成 (否则每次构建都会跑 applocal.ps1 收集 DLL 并打印 "Failed to gather ..." 警告)
    $env:VCPKG_ROOT = ''
    $env:VCPkgLocalAppDataDisabled = '1'
}

# ==================== 平台 / 架构 / 生成器 ====================

# 当前(或目标)平台与架构标签: 目录命名、VS 的 -A 平台名与 CMAKE_SYSTEM_PROCESSOR 不是一回事,
# 这里一次算清楚, 供构建目录/输出目录与 cmake 参数共用。
function Get-MxPlatformInfo {
    param(
        [switch]$Android,
        [ValidateSet('arm64-v8a', 'armeabi-v7a', 'x86_64', 'x86')][string]$Abi = 'arm64-v8a'
    )

    $os = if ($IsWindows -or $env:OS -eq 'Windows_NT') { 'windows' }
    elseif ($IsMacOS) { 'macos' }
    else { 'linux' }

    $arch = switch ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()) {
        'X64' { 'x64' }
        'Arm64' { 'arm64' }
        'X86' { 'x86' }
        default { 'unknown' }
    }

    # VS 生成器的 -A 平台名 (x64/ARM64/Win32) 与 CMAKE_SYSTEM_PROCESSOR (AMD64/ARM64/x86) 不是一回事
    $generatorPlatform = switch ($arch) {
        'x64' { 'x64' }
        'arm64' { 'ARM64' }
        'x86' { 'Win32' }
        default { 'x64' }
    }
    $systemProcessor = switch ($arch) {
        'x64' { 'AMD64' }
        'arm64' { 'ARM64' }
        'x86' { 'x86' }
        default { 'AMD64' }
    }

    $androidAbi = ''
    if ($Android) {
        # Android: 目标平台与开发机平台无关, 平台/架构段一律按 ABI 推导 (与桌面端的目录命名保持一致)
        $os = 'android'
        $androidAbi = $Abi
        $arch = switch ($Abi) {
            'arm64-v8a' { 'arm64' }
            'armeabi-v7a' { 'arm' }
            'x86_64' { 'x64' }
            'x86' { 'x86' }
            default { 'unknown' }
        }
        # Android 的目标处理器由 NDK 工具链按 ANDROID_ABI 推导, 显式传值只会造成两边不一致
        $systemProcessor = ''
    }

    return [ordered]@{
        Os                = $os
        Arch              = $arch
        GeneratorPlatform = $generatorPlatform
        SystemProcessor   = $systemProcessor
        Android           = [bool]$Android
        Abi               = $androidAbi
    }
}

# Android NDK 位置解析 (显式参数 → 环境变量 → SDK 下版本号最大的一个)
function Resolve-MxAndroidNdk {
    param([string]$AndroidNdk)

    $marker = 'build/cmake/android.toolchain.cmake'
    if ($AndroidNdk) {
        if (-not (Test-Path (Join-Path $AndroidNdk $marker))) {
            throw "指定的 NDK 目录里没有 $marker : $AndroidNdk"
        }
        return (Resolve-Path $AndroidNdk).Path
    }
    foreach ($envName in @('ANDROID_NDK_HOME', 'ANDROID_NDK_ROOT')) {
        $value = [Environment]::GetEnvironmentVariable($envName)
        if ($value -and (Test-Path (Join-Path $value $marker))) {
            Write-MxInfo "NDK: 取自 `$env:$envName ($value)"
            return (Resolve-Path $value).Path
        }
    }
    foreach ($sdkName in @('ANDROID_HOME', 'ANDROID_SDK_ROOT')) {
        $sdk = [Environment]::GetEnvironmentVariable($sdkName)
        if (-not $sdk) { continue }
        $ndkRoot = Join-Path $sdk 'ndk'
        if (-not (Test-Path $ndkRoot)) { continue }
        $found = @()
        foreach ($dir in Get-ChildItem $ndkRoot -Directory -ErrorAction SilentlyContinue) {
            if (Test-Path (Join-Path $dir.FullName $marker)) {
                # 目录名就是 NDK 版本号 (例如 29.0.14206865); 解析失败按 0.0.0 参与比较
                $parsed = $null
                try { $parsed = [version](($dir.Name -split '[^0-9.]')[0]) } catch { $parsed = $null }
                if ($null -eq $parsed) { $parsed = [version]'0.0.0' }
                $found += [pscustomobject]@{ Path = $dir.FullName; Version = $parsed }
            }
        }
        if ($found.Count -gt 0) {
            $pick = ($found | Sort-Object -Property Version -Descending)[0]
            Write-MxInfo "NDK: 取自 `$env:$sdkName\ndk ($($pick.Path))"
            return $pick.Path
        }
    }
    throw ("未找到 Android NDK (需要 build/cmake/android.toolchain.cmake)。`n" +
        "  请安装 NDK 并指定: -AndroidNdk <NDK 根目录>, 或设置环境变量 ANDROID_NDK_HOME")
}

# 确保 ninja 可用 (Android 交叉编译必须有它); 找不到时在 Android SDK 自带的 cmake 目录里找
function Ensure-MxNinja {
    if (Get-Command ninja -ErrorAction SilentlyContinue) { return }
    # ninja 随 Android SDK 的 cmake 一起分发 (sdk/cmake/<版本>/bin/ninja.exe)
    foreach ($sdkName in @('ANDROID_HOME', 'ANDROID_SDK_ROOT')) {
        $sdk = [Environment]::GetEnvironmentVariable($sdkName)
        if (-not $sdk) { continue }
        foreach ($cmakeDir in Get-ChildItem (Join-Path $sdk 'cmake') -Directory -ErrorAction SilentlyContinue |
                Sort-Object -Property Name -Descending) {
            $ninjaCandidate = Join-Path $cmakeDir.FullName 'bin/ninja.exe'
            if (Test-Path $ninjaCandidate) {
                $env:PATH = "$(Split-Path -Parent $ninjaCandidate);$env:PATH"
                break
            }
        }
        if (Get-Command ninja -ErrorAction SilentlyContinue) { return }
    }
    throw "Android 交叉编译需要 ninja (未在 PATH 上找到); 请安装 ninja 或使用 Android SDK 自带的 cmake/bin/ninja.exe"
}

# 生成器解析: 显式参数 → Windows 上挑已安装的 VS → ninja → Unix Makefiles
# (Android 交叉编译固定 Ninja: 与 NDK 工具链配合最稳, 也不依赖开发机平台的 IDE 生成器)
function Resolve-MxGenerator {
    param(
        [string]$Generator,
        [string]$Os = 'windows',
        [switch]$Android
    )

    if ($Android) {
        [void](Ensure-MxNinja)
        return 'Ninja'
    }
    if ($Generator) { return $Generator }
    if ($Os -eq 'windows') {
        $help = (& cmake --help 2>$null) -join "`n"
        foreach ($candidate in @('Visual Studio 18 2026', 'Visual Studio 17 2022')) {
            if ($help -match [regex]::Escape($candidate)) { return $candidate }
        }
    }
    if (Get-Command ninja -ErrorAction SilentlyContinue) { return 'Ninja' }
    return 'Unix Makefiles'
}

# ==================== 目录约定 ====================

# 构建目录 / 安装前缀 / 便携输出目录 / 产物文件名
#   <包>/.native/build/<平台>-<配置>                     构建目录 (一个配置一个, 依赖也装在里面)
#   <包>/.native/build/<平台>-<配置>/musicxx-extern-plugin-install   安装前缀 (依赖 + 插件框架 + SDK)
#   <包>/.native/output/<平台>-<架构>-<配置>/{bin,plugins}            稳定输出目录 (供打包/手工取用)
# Android 的"平台-架构"段是 android-<abi>: build/android-arm64-v8a-release
function Get-MxBuildPaths {
    param(
        [Parameter(Mandatory)][string]$Root,
        [ValidateSet('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel')][string]$Config = 'Release',
        [switch]$Android,
        [string]$Abi = 'arm64-v8a'
    )

    $nativeRoot = Join-Path $Root '.native'
    $info = Get-MxPlatformInfo -Android:$Android -Abi $Abi
    $configLower = $Config.ToLower()
    if ($Android) {
        $buildDir = Join-Path $nativeRoot "build/android-$($info.Abi)-$configLower"
        $outputDir = Join-Path $nativeRoot "output/android-$($info.Abi)-$configLower"
    }
    else {
        $buildDir = Join-Path $nativeRoot "build/$($info.Os)-$configLower"
        $outputDir = Join-Path $nativeRoot "output/$($info.Os)-$($info.Arch)-$configLower"
    }
    $libraryName = if ($info.Os -eq 'windows') { 'musicxx_extern_plugin.dll' }
    elseif ($info.Os -eq 'macos') { 'libmusicxx_extern_plugin.dylib' }
    else { 'libmusicxx_extern_plugin.so' }
    $testName = if ($info.Os -eq 'windows') { 'musicxx_extern_plugin_test.exe' } else { 'musicxx_extern_plugin_test' }

    return [ordered]@{
        Os            = $info.Os
        Arch          = $info.Arch
        Android       = $info.Android
        Abi           = $info.Abi
        Config        = $Config
        ConfigLower   = $configLower
        GeneratorPlatform = $info.GeneratorPlatform
        SystemProcessor   = $info.SystemProcessor
        NativeRoot    = $nativeRoot
        BuildDir      = [System.IO.Path]::GetFullPath($buildDir)
        OutputDir     = [System.IO.Path]::GetFullPath($outputDir)
        InstallDir    = [System.IO.Path]::GetFullPath((Join-Path $buildDir 'musicxx-extern-plugin-install'))
        LibraryName   = $libraryName
        TestName      = $testName
        HostLibrary   = [System.IO.Path]::GetFullPath((Join-Path $outputDir "bin/$libraryName"))
        PluginsDir    = [System.IO.Path]::GetFullPath((Join-Path $outputDir 'plugins'))
    }
}

# 构建目录签名: 配置/生成器/平台/Boost 位置变化时重置目录, 避免 CMakeCache 里残留上一次的
# 参数 (例如旧的 CMAKE_CONFIGURATION_TYPES) 让新配置报出难以定位的错。
function Get-MxBuildSignature {
    param(
        [Parameter(Mandatory)]$Paths,
        [Parameter(Mandatory)][string]$Generator,
        [string]$BoostHeaderRoot,
        [string]$AndroidToolchain = '',
        [string]$AndroidPlatform = '',
        [string]$AndroidStl = '',
        [string]$OpenSslRoot = '',
        [string]$TlsState = ''
    )
    $xxIsRelease = if ($Paths.Config -eq 'Debug') { '0' } else { '1' }
    return ("config=$($Paths.Config);generator=$Generator;platform=$($Paths.GeneratorPlatform);" +
        "boost=$BoostHeaderRoot;xxRelease=$xxIsRelease;target=$($Paths.Os);abi=$($Paths.Abi);" +
        "ndk=$AndroidToolchain;androidPlatform=$AndroidPlatform;stl=$AndroidStl;" +
        "openssl=$OpenSslRoot;tls=$TlsState")
}

# ==================== Boost (仅头文件) ====================

function Test-MxBoostRoot([string]$Path) {
    return [bool]($Path -and (Test-Path (Join-Path $Path 'boost/version.hpp')))
}

function Get-MxBoostVersionCode([string]$HeaderRoot) {
    $hit = Select-String -Path (Join-Path $HeaderRoot 'boost/version.hpp') -Pattern '^#define BOOST_VERSION ' |
        Select-Object -First 1
    if (-not $hit) { return 0 }
    return [int]($hit.Line -replace '^#define BOOST_VERSION\s+', '')
}

# 版本号 1.92.0 → BOOST_VERSION 109200 (唯一版本来源是 boost/version.hpp)
function Get-MxExpectedBoostVersionCode([string]$Version) {
    $parts = $Version.Split('.')
    return ([int]$parts[0] * 100000) + ([int]$parts[1] * 100) + [int]$parts[2]
}

function Expand-MxBoostArchive {
    param(
        [Parameter(Mandatory)][string]$ArchivePath,
        [Parameter(Mandatory)][string]$Version,
        [Parameter(Mandatory)][string]$DestRoot,
        [Parameter(Mandatory)][string]$NativeRoot
    )
    # Boost 发布包 (archives.boost.io) 顶层是 boost_<下划线版本>/boost/...
    $dirName = 'boost_' + ($Version -replace '\.', '_')
    $tmp = Join-Path (Join-Path $NativeRoot 'downloads') ("extract-" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    Write-MxInfo "解包 $ArchivePath"
    & tar -xf $ArchivePath -C $tmp "$dirName/boost"
    if ($LASTEXITCODE -ne 0) { throw "解包失败: $ArchivePath" }
    $headerRoot = Join-Path $tmp $dirName
    if (-not (Test-MxBoostRoot $headerRoot)) { throw "解包结果里没有 $dirName/boost/version.hpp" }
    if (Test-Path $DestRoot) { Remove-Item $DestRoot -Recurse -Force }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $DestRoot) | Out-Null
    Move-Item $headerRoot $DestRoot
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    return $DestRoot
}

# Boost 头文件解析顺序 (只用 Boost.Asio 头, 不链接任何 Boost 编译库):
#   显式参数 → 显式发布包 → 本包内置头文件 → 已解包缓存 → 按固定版本下载
# -Check 时只报告"会用什么", 不下载也不解包 (给"配置环境前的看一眼"用)。
function Resolve-MxBoostHeaderRoot {
    param(
        [Parameter(Mandatory)][string]$Root,
        [string]$BoostRoot,
        [string]$BoostArchive,
        [switch]$NoDownloadBoost,
        [switch]$Check
    )

    $nativeRoot = Join-Path $Root '.native'
    $cached = Join-Path $nativeRoot ('boost/boost_' + ($script:MxBoostVersion -replace '\.', '_'))
    $vendored = Join-Path $Root 'src/third_party/boost/include'

    # 1) 显式参数
    if ($BoostRoot) {
        if (-not (Test-MxBoostRoot $BoostRoot)) { throw "-BoostRoot 下没有 boost/version.hpp: $BoostRoot" }
        return [ordered]@{ Root = (Resolve-Path $BoostRoot).Path; Source = 'explicit'; Version = $script:MxBoostVersion }
    }
    # 2) 显式指定的发布包 (优先于内置头文件, 便于离线/自备包)
    if ($BoostArchive) {
        if (-not (Test-Path $BoostArchive)) { throw "-BoostArchive 指向的文件不存在: $BoostArchive" }
        if ($Check) {
            return [ordered]@{ Root = ''; Source = 'archive(未解包)'; Version = $script:MxBoostVersion }
        }
        $expanded = Expand-MxBoostArchive -ArchivePath $BoostArchive -Version $script:MxBoostVersion `
            -DestRoot $cached -NativeRoot $nativeRoot
        return [ordered]@{ Root = $expanded; Source = 'archive'; Version = $script:MxBoostVersion }
    }
    # 3) 本包内置的头文件 (src/third_party/boost/include)
    if (Test-MxBoostRoot $vendored) {
        return [ordered]@{ Root = (Resolve-Path $vendored).Path; Source = 'vendored'; Version = $script:MxBoostVersion }
    }
    # 4) 之前下载解包过的缓存
    if (Test-MxBoostRoot $cached) {
        return [ordered]@{ Root = (Resolve-Path $cached).Path; Source = 'cache'; Version = $script:MxBoostVersion }
    }
    # 5) 按固定版本下载官方发布包
    if ($NoDownloadBoost) {
        throw ("未提供 Boost 头文件: 请用 -BoostRoot <含 boost/ 的目录> 或 -BoostArchive <发布包>, " +
            "或去掉 -NoDownloadBoost 让脚本自动下载 $script:MxBoostSourceUrl")
    }
    if ($Check) {
        return [ordered]@{ Root = ''; Source = 'download(未下载)'; Version = $script:MxBoostVersion }
    }
    $dirName = 'boost_' + ($script:MxBoostVersion -replace '\.', '_')
    $downloadRoot = Join-Path $nativeRoot 'downloads'
    New-Item -ItemType Directory -Force -Path $downloadRoot | Out-Null
    $archivePath = Join-Path $downloadRoot "$dirName.tar.gz"
    if (Test-Path $archivePath) {
        Write-MxInfo "Boost: 复用已下载的发布包 $archivePath"
    }
    else {
        $url = $script:MxBoostSourceUrl.Replace('@VERSION@', $script:MxBoostVersion).
            Replace('@UNDERSCORE@', ($script:MxBoostVersion -replace '\.', '_'))
        Write-MxInfo "Boost: 下载 $url"
        & curl.exe -L --fail --retry 3 --retry-delay 2 -o $archivePath $url
        if ($LASTEXITCODE -ne 0) { throw "下载 Boost 发布包失败: $url" }
    }
    if ($script:MxBoostSha256) {
        $actual = (Get-FileHash -Algorithm SHA256 $archivePath).Hash.ToLower()
        if ($actual -ne $script:MxBoostSha256.ToLower()) {
            throw "Boost 发布包校验失败: $archivePath`n  期望 $script:MxBoostSha256`n  实际 $actual"
        }
        Write-MxInfo 'Boost: 发布包 SHA256 校验通过'
    }
    $expanded = Expand-MxBoostArchive -ArchivePath $archivePath -Version $script:MxBoostVersion `
        -DestRoot $cached -NativeRoot $nativeRoot
    return [ordered]@{ Root = $expanded; Source = 'download'; Version = $script:MxBoostVersion }
}

# 校验头文件里的 BOOST_VERSION 与脚本锁定的版本一致 (版本不符时给明确原因, 而不是让编译期报错)
function Assert-MxBoostVersion([string]$HeaderRoot) {
    if (-not (Test-MxBoostRoot $HeaderRoot)) { throw "Boost 头文件缺失: $HeaderRoot" }
    $expect = Get-MxExpectedBoostVersionCode $script:MxBoostVersion
    $actual = Get-MxBoostVersionCode $HeaderRoot
    if ($actual -ne $expect) {
        throw ("Boost 版本不符: 脚本期望 $script:MxBoostVersion (BOOST_VERSION=$expect), " +
            "实际 BOOST_VERSION=$actual ($HeaderRoot)")
    }
}

# ==================== 环境描述文件 ====================

function Get-MxEnvFile([string]$Root) {
    return (Join-Path $Root '.native/env/environment.json')
}

# 安全读取 PSCustomObject 的字段 (Set-StrictMode 下读不存在的属性会抛异常)
function Get-MxProp($Object, [string]$Name, $Default = $null) {
    if ($null -eq $Object) { return $Default }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property -or $null -eq $property.Value) { return $Default }
    return $property.Value
}

function Read-MxEnvironment([string]$Root) {
    $file = Get-MxEnvFile $Root
    if (-not (Test-Path $file)) { return $null }
    try {
        return (Get-Content -Raw -Encoding UTF8 $file | ConvertFrom-Json)
    }
    catch {
        Write-MxWarn "环境描述文件读不出来(将重新解析): $file ($($_.Exception.Message))"
        return $null
    }
}

# 把 JSON 对象整理成脚本内部统一使用的形状 (键名固定, 免得调用点读错大小写)
function ConvertTo-MxEnvironmentResult {
    param($Object, [bool]$Reused, [string]$File)

    if ($null -eq $Object) { return $null }
    $tools = Get-MxProp $Object 'tools'
    $boost = Get-MxProp $Object 'boost'
    $submodules = Get-MxProp $Object 'submodules'
    $android = Get-MxProp $Object 'android'
    return [ordered]@{
        Schema            = [int](Get-MxProp $Object 'schema' 0)
        WrittenAt         = [string](Get-MxProp $Object 'writtenAt' '')
        Reused            = $Reused
        File              = $File
        Platform          = [string](Get-MxProp $Object 'platform' '')
        Arch              = [string](Get-MxProp $Object 'arch' '')
        Generator         = [string](Get-MxProp $Object 'generator' '')
        GeneratorPlatform = [string](Get-MxProp $Object 'generatorPlatform' '')
        IsMultiConfig     = [bool](Get-MxProp $Object 'isMultiConfig' $false)
        Tools             = [ordered]@{
            Cmake            = [string](Get-MxProp $tools 'cmake' '')
            CmakeVersion     = [string](Get-MxProp $tools 'cmakeVersion' '')
            Git              = [string](Get-MxProp $tools 'git' '')
            Ninja            = [string](Get-MxProp $tools 'ninja' '')
            Pwsh             = [string](Get-MxProp $tools 'pwsh' '')
            Vswhere          = [string](Get-MxProp $tools 'vswhere' '')
            PkgConfig        = [string](Get-MxProp $tools 'pkgConfig' '')
            CompilerLauncher = [string](Get-MxProp $tools 'compilerLauncher' '')
        }
        Boost             = [ordered]@{
            Root    = [string](Get-MxProp $boost 'root' '')
            Version = [string](Get-MxProp $boost 'version' '')
            Source  = [string](Get-MxProp $boost 'source' '')
        }
        Submodules        = [ordered]@{
            Total = [int](Get-MxProp $submodules 'total' 0)
            Ready = [bool](Get-MxProp $submodules 'ready' $false)
        }
        Android           = [ordered]@{
            Ndk       = [string](Get-MxProp $android 'ndk' '')
            Toolchain = [string](Get-MxProp $android 'toolchain' '')
            Platform  = [string](Get-MxProp $android 'platform' '')
            Stl       = [string](Get-MxProp $android 'stl' '')
        }
    }
}

function Write-MxEnvironment([string]$Root, $Environment) {
    $file = Get-MxEnvFile $Root
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $file) | Out-Null
    $payload = [ordered]@{
        schema            = $script:MxEnvSchema
        writtenAt         = (Get-Date).ToString('o')
        platform          = $Environment.Platform
        arch              = $Environment.Arch
        generator         = $Environment.Generator
        generatorPlatform = $Environment.GeneratorPlatform
        isMultiConfig     = $Environment.IsMultiConfig
        tools             = [ordered]@{
            cmake            = $Environment.Tools.Cmake
            cmakeVersion     = $Environment.Tools.CmakeVersion
            git              = $Environment.Tools.Git
            ninja            = $Environment.Tools.Ninja
            pwsh             = $Environment.Tools.Pwsh
            vswhere          = $Environment.Tools.Vswhere
            pkgConfig        = $Environment.Tools.PkgConfig
            compilerLauncher = $Environment.Tools.CompilerLauncher
        }
        boost             = [ordered]@{
            root    = $Environment.Boost.Root
            version = $Environment.Boost.Version
            source  = $Environment.Boost.Source
        }
        submodules        = [ordered]@{
            total = $Environment.Submodules.Total
            ready = $Environment.Submodules.Ready
        }
        android           = [ordered]@{
            ndk       = $Environment.Android.Ndk
            toolchain = $Environment.Android.Toolchain
            platform  = $Environment.Android.Platform
            stl       = $Environment.Android.Stl
        }
    }
    # 环境描述文件是"可重建的本地缓存", 不带 BOM: 便于人工查看与脚本比较
    $json = $payload | ConvertTo-Json -Depth 6
    [System.IO.File]::WriteAllText($file, $json, (New-Object System.Text.UTF8Encoding($false)))
    return $file
}

# 缓存的环境描述文件是否还能用于本次请求 (平台一致 + 引用的路径都还在 + 模式版本一致)
function Test-MxEnvironmentUsable {
    param(
        [Parameter(Mandatory)]$Environment,
        [Parameter(Mandatory)][string]$Root,
        [switch]$Android,
        [string]$AndroidPlatform = '',
        [string]$AndroidStl = ''
    )

    if ($null -eq $Environment) { return $false }
    if ([int]$Environment.Schema -ne $script:MxEnvSchema) { return $false }
    # 同一份仓库可能被同步到别的系统 (或环境文件是从别处复制来的): 平台不符就重新解析
    $hostInfo = Get-MxPlatformInfo
    if ([string]$Environment.Platform -notin @('windows', 'linux', 'macos')) { return $false }
    if ([string]$Environment.Platform -ne $hostInfo.Os) { return $false }
    if ($Android) {
        # Android 目标: 记录里必须有可用的 NDK 工具链
        if (-not $Environment.Android.Toolchain) { return $false }
        if (-not (Test-Path $Environment.Android.Toolchain)) { return $false }
        if ($AndroidPlatform -and $Environment.Android.Platform -ne $AndroidPlatform) { return $false }
        if ($AndroidStl -and $Environment.Android.Stl -ne $AndroidStl) { return $false }
    }
    # Boost 头文件必须还在 (可能被清理掉)
    if (-not (Test-MxBoostRoot ([string]$Environment.Boost.Root))) { return $false }
    # 工具都还在 PATH 上 (换了机器/卸了工具时重新解析)
    if (-not $Environment.Tools.Cmake) { return $false }
    if (-not (Test-Path $Environment.Tools.Cmake)) { return $false }
    return $true
}

# ==================== 环境解析 (配置开发编译环境) ====================

# 只读缓存的环境描述文件: 可用就返回 (Reused=$true), 否则返回 $null。
# 构建脚本用它在"已是最新"的情况下秒级返回 —— 不必先跑一遍完整的环境解析。
function Get-MxEnvironmentCached {
    param(
        [Parameter(Mandatory)][string]$Root,
        [switch]$Android,
        [string]$AndroidPlatform = '',
        [string]$AndroidStl = ''
    )

    $envFile = Get-MxEnvFile $Root
    $cached = ConvertTo-MxEnvironmentResult (Read-MxEnvironment $Root) $true $envFile
    if ($cached -and (Test-MxEnvironmentUsable -Environment $cached -Root $Root -Android:$Android `
                -AndroidPlatform $AndroidPlatform -AndroidStl $AndroidStl)) {
        return $cached
    }
    return $null
}

# 解析/复用开发编译环境:
#   已缓存且仍然可用 → 直接返回 (Reused=$true), 不重新探测工具链、不重新解析 Boost;
#   否则做一遍完整解析 (预检 / 子模块 / Boost / NDK) 并写回环境描述文件。
# -Check 只解析不写入磁盘 (缺 Boost 时不下载, 只报告"需要下载")。
function Invoke-MxEnsureEnvironment {
    param(
        [Parameter(Mandatory)][string]$Root,
        [string]$BoostRoot,
        [string]$BoostArchive,
        [switch]$NoDownloadBoost,
        [switch]$Check,
        [switch]$Force,
        [switch]$Quiet,
        [switch]$Android,
        [string]$AndroidNdk,
        [string]$AndroidPlatform = 'android-24',
        [string]$AndroidStl = 'c++_static',
        [string]$Generator
    )

    $envFile = Get-MxEnvFile $Root
    # 缓存复用: 上次解析的结果仍然适用就直接用 (工具位置/Boost 位置没变)。
    # 显式指定 Boost / 生成器时说明调用方要换一套, 不走缓存。
    if (-not $Force -and -not $Check -and -not $BoostRoot -and -not $BoostArchive -and -not $Generator) {
        $cached = Get-MxEnvironmentCached -Root $Root -Android:$Android -AndroidPlatform $AndroidPlatform `
            -AndroidStl $AndroidStl
        if ($cached) { return $cached }
    }

    if (-not $Quiet) { Write-Host "" ; Write-Host '==> 解析开发编译环境' }

    # ---- 工具预检 ----
    foreach ($tool in @('cmake', 'git')) {
        if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
            throw "未找到 $tool; 请先安装并加入 PATH"
        }
    }
    $cmakePath = (Get-Command cmake).Source
    $cmakeVersion = (& cmake --version)[0]
    $gitPath = (Get-Command git).Source
    $pwshPath = ''
    foreach ($candidate in @('pwsh', 'powershell')) {
        $found = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($found) { $pwshPath = $found.Source; break }
    }

    $info = Get-MxPlatformInfo -Android:$Android -Abi 'arm64-v8a'
    $os = $info.Os
    $arch = $info.Arch
    $isMultiConfig = $false

    $vswhere = ''
    $pkgConfig = ''
    if ($os -eq 'windows') {
        foreach ($pf in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
            if ($pf -and (Test-Path (Join-Path $pf 'Microsoft Visual Studio/Installer/vswhere.exe'))) {
                $vswhere = Join-Path $pf 'Microsoft Visual Studio/Installer/vswhere.exe'
            }
        }
        if (-not $vswhere) {
            foreach ($pf in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
                if ($pf -and (Test-Path (Join-Path $pf 'Microsoft Visual Studio'))) {
                    $vswhere = '(未找到 vswhere, 但存在 Visual Studio 安装目录)'
                }
            }
        }
        if (-not $vswhere) {
            Write-MxWarn '未检测到 Visual Studio (vswhere 与安装目录都不存在); 若使用其它工具链请忽略'
        }
        if (-not (Get-Command pkg-config -ErrorAction SilentlyContinue)) {
            Write-MxWarn '未找到 pkg-config; cxx_utilxx_base / cxx_pluginxx 配置阶段会失败 (find_package(PkgConfig REQUIRED))'
        }
        else {
            $pkgConfig = (Get-Command pkg-config).Source
        }
    }
    else {
        $pkgConfig = (Get-Command pkg-config -ErrorAction SilentlyContinue).Source
        if (-not $pkgConfig -and -not $Android) {
            Write-MxWarn '未找到 pkg-config; cxx_utilxx_base / cxx_pluginxx 配置阶段会失败 (find_package(PkgConfig REQUIRED))'
        }
    }

    # ---- 生成器 ----
    $generator = Resolve-MxGenerator -Generator $Generator -Os $os -Android:$Android
    if ($generator -like 'Visual Studio*') { $isMultiConfig = $true }
    $generatorPlatform = if ($generator -like 'Visual Studio*') { $info.GeneratorPlatform } else { '' }
    $ninjaPath = (Get-Command ninja -ErrorAction SilentlyContinue).Source

    # ---- 子模块 (commit 由仓库 gitlink 记录, 这里只确认已初始化) ----
    $submoduleTotal = 0
    $submoduleReady = $true
    $submoduleStatus = & git -C $Root submodule status 2>$null
    $missing = @()
    foreach ($line in $submoduleStatus) {
        if (-not $line) { continue }
        $submoduleTotal++
        $trimmed = $line.TrimStart()
        if ($trimmed.StartsWith('-')) { $missing += ($trimmed -split '\s+')[1] }
    }
    if ($missing.Count -gt 0) {
        $submoduleReady = $false
        throw ("以下子模块未初始化:`n  " + ($missing -join "`n  ") +
            "`n请先执行: git submodule update --init --recursive")
    }

    # ---- Boost (仅头文件) ----
    $boost = Resolve-MxBoostHeaderRoot -Root $Root -BoostRoot $BoostRoot -BoostArchive $BoostArchive `
        -NoDownloadBoost:$NoDownloadBoost -Check:$Check
    if ($boost.Root) { Assert-MxBoostVersion $boost.Root }

    # ---- Android NDK (只在目标为 Android 时解析) ----
    $androidNdk = ''
    $androidToolchain = ''
    if ($Android) {
        $androidNdk = Resolve-MxAndroidNdk -AndroidNdk $AndroidNdk
        $androidToolchain = Join-Path $androidNdk 'build/cmake/android.toolchain.cmake'
    }

    $result = [ordered]@{
        Schema            = $script:MxEnvSchema
        WrittenAt         = (Get-Date).ToString('o')
        Reused            = $false
        File              = $envFile
        Platform          = $os
        Arch              = $arch
        Generator         = $generator
        GeneratorPlatform = $generatorPlatform
        IsMultiConfig     = $isMultiConfig
        Tools             = [ordered]@{
            Cmake            = $cmakePath
            CmakeVersion     = $cmakeVersion
            Git              = $gitPath
            Ninja            = $ninjaPath
            Pwsh             = $pwshPath
            Vswhere          = $vswhere
            PkgConfig        = $pkgConfig
            CompilerLauncher = ''
        }
        Boost             = [ordered]@{
            Root    = $boost.Root
            Version = $boost.Version
            Source  = $boost.Source
        }
        Submodules        = [ordered]@{
            Total = $submoduleTotal
            Ready = $submoduleReady
        }
        Android           = [ordered]@{
            Ndk       = $androidNdk
            Toolchain = $androidToolchain
            Platform  = if ($Android) { $AndroidPlatform } else { '' }
            Stl       = if ($Android) { $AndroidStl } else { '' }
        }
    }

    if (-not $Check) { [void](Write-MxEnvironment -Root $Root -Environment $result) }
    return $result
}

# ==================== 构建新鲜度判定 ====================

# 构建标记: 每次成功构建后写一个, 记录"这次构建覆盖了哪部分内容" (framework / examples / tests)。
# 用标记文件的时间做新鲜度比较, 而不是产物文件的时间 —— 有些构建输入 (例如 CMakeLists.txt)
# 变化后产物二进制并不会重新链接, 按产物时间比较会永远判定"需要重建"。
function Get-MxBuildStampPath {
    param(
        [Parameter(Mandatory)][string]$BuildDir,
        [Parameter(Mandatory)][ValidateSet('framework', 'examples', 'tests')][string]$Kind
    )
    return (Join-Path $BuildDir ".build_stamp_$Kind")
}

function Set-MxBuildStamp {
    param(
        [Parameter(Mandatory)][string]$BuildDir,
        [Parameter(Mandatory)][string[]]$Kinds
    )
    $now = (Get-Date).ToString('o')
    foreach ($kind in $Kinds) {
        $path = Get-MxBuildStampPath -BuildDir $BuildDir -Kind $kind
        [System.IO.File]::WriteAllText($path, "$now", (New-Object System.Text.UTF8Encoding($false)))
    }
}

# 一组路径里最新的文件写入时间 (目录按递归处理)。用于"源码有没有比构建标记更新"的判断。
# 用 .NET 的枚举而不是 Get-ChildItem: 三千多个文件时前者快一个数量级。
function Get-MxNewestWriteTime {
    param(
        [Parameter(Mandatory)][string[]]$Paths,
        [string[]]$SkipDirectoryNames = @('boost')
    )

    $newest = [datetime]::MinValue
    foreach ($path in $Paths) {
        if (-not $path) { continue }
        if (-not (Test-Path -LiteralPath $path)) { continue }
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $time = (Get-Item -LiteralPath $path).LastWriteTime
            if ($time -gt $newest) { $newest = $time }
            continue
        }
        $queue = [System.Collections.Generic.Queue[string]]::new()
        $queue.Enqueue($path)
        while ($queue.Count -gt 0) {
            $dir = $queue.Dequeue()
            try {
                foreach ($file in [System.IO.Directory]::EnumerateFiles($dir)) {
                    $time = [System.IO.File]::GetLastWriteTime($file)
                    if ($time -gt $newest) { $newest = $time }
                }
                foreach ($sub in [System.IO.Directory]::EnumerateDirectories($dir)) {
                    if ($SkipDirectoryNames -contains (Split-Path $sub -Leaf)) { continue }
                    $queue.Enqueue($sub)
                }
            }
            catch {
                # 枚举中途遇到没权限/被删除的目录: 这一层跳过即可 (不影响其它路径)
            }
        }
    }
    return $newest
}

# 影响插件框架库与示例插件的源码集合。
# third_party 全部算在内 (依赖升级/本地改动都会让插件框架库过期); 只有本机内置的 Boost 头文件不算 ——
# 它有几万个文件, 而且不参与编译, 只被 include。
# 构建脚本本身不算源码: 改脚本不改变已编好的库, 算进去会让"已是最新"永远不成立 (脚本一动就要重建);
# 脚本里的参数变化由构建目录签名负责 (配置/生成器/Boost 位置变化会重置构建目录)。
function Get-MxSourcePaths([string]$Root) {
    $paths = @(
        (Join-Path $Root 'src/CMakeLists.txt'),
        (Join-Path $Root 'src/host'),
        (Join-Path $Root 'src/include'),
        (Join-Path $Root 'src/sdk'),
        (Join-Path $Root 'src/third_party'),
        (Join-Path $Root 'plugins'),
        (Join-Path $Root 'tools/cmake')
    )
    return $paths
}

# 产物是否已是最新: 需要的构建标记都在 → 要求存在的产物都在 → 构建目录已 configure 且签名一致
# → 最早的标记比最新源码新。
# [Stamps] 本次请求要求覆盖到的构建标记 (framework / examples / tests 各一个);
# [RequiredPaths] 只要求存在 (目录要求非空), 不参与时间比较。
function Test-MxBuildFresh {
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$BuildDir,
        [Parameter(Mandatory)][string]$Signature,
        [string[]]$Stamps = @(),
        [string[]]$RequiredPaths = @()
    )

    foreach ($stamp in $Stamps) {
        if (-not $stamp -or -not (Test-Path -LiteralPath $stamp)) { return $false }
    }
    foreach ($required in $RequiredPaths) {
        if (-not $required) { return $false }
        if (-not (Test-Path -LiteralPath $required)) { return $false }
        if (Test-Path -LiteralPath $required -PathType Container) {
            $items = @(Get-ChildItem -LiteralPath $required -Recurse -File -ErrorAction SilentlyContinue)
            if ($items.Count -eq 0) { return $false }
        }
    }
    if (-not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) { return $false }
    $marker = Join-Path $BuildDir '.build_signature'
    if (-not (Test-Path $marker)) { return $false }
    if ((Get-Content -Raw $marker).Trim() -ne $Signature) { return $false }
    if ($Stamps.Count -eq 0) { return $false }

    $sourceTime = Get-MxNewestWriteTime -Paths (Get-MxSourcePaths $Root)
    $stampTime = [datetime]::MaxValue
    foreach ($stamp in $Stamps) {
        $time = (Get-Item -LiteralPath $stamp).LastWriteTime
        if ($time -lt $stampTime) { $stampTime = $time }
    }
    return ($stampTime -gt $sourceTime)
}

# ==================== 产物复制 ====================

# 把安装前缀里的 bin/ 与 plugins/ 复制到稳定输出目录。
# 被占用的文件不阻断构建: 先试普通覆盖; 被正在运行的应用/插件加载着 (Windows 会锁住映像)
# 时改成"改名为 *.old 再复制" —— 旧文件留在旁边可以人工删, 新产物无论如何都会到位
# (静默跳过会让人以为"构建成功"但产物还是旧的, 排查时很费时间)。
function Copy-MxBuildArtifacts {
    param(
        [Parameter(Mandatory)][string]$InstallDir,
        [Parameter(Mandatory)][string]$OutputDir
    )

    New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
    foreach ($sub in @('bin', 'plugins')) {
        # 注意: `Copy-Item <源目录> <目标目录> -Recurse` 在目标已存在时会把源目录
        # 整体嵌进目标 (得到 <目标>/<源>), 旧产物不会被覆盖 —— 必须按"内容合并"复制。
        $from = Join-Path $InstallDir $sub
        if (-not (Test-Path $from)) { continue }
        $to = Join-Path $OutputDir $sub
        $nested = Join-Path $to $sub
        if (Test-Path $nested) { Remove-Item $nested -Recurse -Force }   # 清掉历史误拷贝
        New-Item -ItemType Directory -Force -Path $to | Out-Null
        Copy-Item (Join-Path $from '*') $to -Recurse -Force -ErrorAction SilentlyContinue

        # 逐个核对: 复制失败 (目标被占用) 的文件改用"改名 + 复制"
        foreach ($file in (Get-ChildItem -LiteralPath $from -Recurse -File)) {
            $relative = $file.FullName.Substring($from.Length).TrimStart('\', '/')
            $target = Join-Path $to $relative
            if ((Test-Path $target) -and
                ((Get-Item -LiteralPath $target).LastWriteTime -eq $file.LastWriteTime)) {
                continue
            }
            $targetDir = Split-Path -Parent $target
            if ($targetDir) { New-Item -ItemType Directory -Force -Path $targetDir | Out-Null }
            try {
                Copy-Item $file.FullName $target -Force -ErrorAction Stop
            }
            catch {
                # 目标被正在运行的进程锁着: 改名让它脱锁, 再复制新文件
                $backup = "$target.old"
                Remove-Item $backup -Force -ErrorAction SilentlyContinue
                try {
                    Move-Item $target $backup -Force -ErrorAction Stop
                    Copy-Item $file.FullName $target -Force -ErrorAction Stop
                    Write-MxWarn "旧产物被占用, 已改名为 $(Split-Path -Leaf $backup) 后替换: $target"
                }
                catch {
                    Write-MxWarn "产物复制失败 (可能被运行中的应用占用), 请关闭应用后重跑: $target"
                }
            }
        }
    }
}
