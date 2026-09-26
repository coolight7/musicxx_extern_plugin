# musicxx_extern_plugin 原生构建脚本
#
# 对应 agentxx 的 agent/script/windows_debug_build.bat: 负责"环境准备 + 调 cmake",
# 具体的依赖库编译与宿主工程构建全部交给 src/CMakeLists.txt (superbuild + ExternalProject_Add):
#
#   src/CMakeLists.txt       顶层 superbuild: 推导 XX_IS_* 宏 → 生成 BoostConfig.cmake →
#                            ExternalProject_Add 逐个构建 third_party 依赖 → 构建宿主工程
#   src/host/CMakeLists.txt  嵌套宿主工程: find_package 取依赖 → 宿主库 / 插件 SDK / 示例插件 / 原生测试
#
# 本脚本负责的只有 cmake 不方便做的事:
#   - 环境预检 (cmake / git / Visual Studio), UTF-8 控制台、关闭 vcpkg 集成、MSVC 英文输出
#   - Boost 头文件的解析/复用/下载解包 (只用头文件, 不链接 Boost 编译库)
#   - 按 -Config 传 XX_IS_RELEASE_D / CMAKE_BUILD_TYPE / CMAKE_CONFIGURATION_TYPES / 平台
#   - 构建完成后把产物复制到稳定的输出目录, 可选跑原生测试
#
# 用法:
#   pwsh -NoProfile -File tools/build_native.ps1                    # Release 全量 (依赖 + 宿主)
#   pwsh -NoProfile -File tools/build_native.ps1 -Config Debug      # Debug (独立构建目录, 依赖也重编)
#   pwsh -NoProfile -File tools/build_native.ps1 -DepsOnly          # 只构建依赖库
#   pwsh -NoProfile -File tools/build_native.ps1 -ConfigureOnly     # 只做预检/Boost/configure
#   pwsh -NoProfile -File tools/build_native.ps1 -Clean             # 先清空构建目录
#   pwsh -NoProfile -File tools/build_native.ps1 -RunTests          # 构建后跑原生测试
#   pwsh -NoProfile -File tools/build_native.ps1 -Jobs 8            # 指定并行度
#   pwsh -NoProfile -File tools/build_native.ps1 -BoostRoot <目录>       # 指定 Boost 头文件根 (含 boost/)
#   pwsh -NoProfile -File tools/build_native.ps1 -BoostArchive <压缩包>  # 用本地 Boost 发布包解包
#   pwsh -NoProfile -File tools/build_native.ps1 -Android -Abi arm64-v8a # 用 NDK 交叉编译 Android 宿主库
#
# Android 说明:
#   - 产出与桌面端同一套布局: <包>/.native/output/android-<abi>-<配置>/bin/libmusicxx_extern_plugin.so
#     (Android 的 ABI 名直接用作目录里的架构段: arm64-v8a / armeabi-v7a / x86_64 / x86)
#   - 宿主库不是靠本脚本进 APK 的: 包的 android/build.gradle 会把它按 ABI 收集到 jniLibs,
#     随 Flutter 的 Android 构建打进去 (见 android/README.md)
#   - NDK 位置优先取 -AndroidNdk, 其次 $env:ANDROID_NDK_HOME / $env:ANDROID_NDK_ROOT,
#     最后在 $env:ANDROID_HOME|$env:ANDROID_SDK_ROOT\ndk 下挑版本号最大的一个
#   - 统一用 Ninja 生成器与 c++_static (C++ 运行库静态链进宿主库, APK 不需要额外带 libc++_shared.so)
#   - -RunTests 在 Android 上不执行: 测试可执行文件是给设备编的, 宿主上跑不起来 (需要 adb 手工跑)
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [ValidateSet('Release', 'Debug')][string]$Config = 'Release',
    [switch]$DepsOnly,
    [switch]$ConfigureOnly,
    [switch]$Clean,
    [switch]$RunTests,
    [int]$Jobs = 0,
    [string]$BuildDir,
    [string]$OutputDir,
    [string]$BoostRoot,
    [string]$BoostArchive,
    [string]$Generator,
    [switch]$NoDownloadBoost,
    # Android 交叉编译: 目标平台/ABI/NDK 与最低系统版本
    [switch]$Android,
    [ValidateSet('arm64-v8a', 'armeabi-v7a', 'x86_64', 'x86')][string]$Abi = 'arm64-v8a',
    [string]$AndroidNdk,
    [string]$AndroidPlatform = 'android-24',
    [ValidateSet('c++_static', 'c++_shared')][string]$AndroidStl = 'c++_static'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Root = (Resolve-Path $Root).Path
$srcDir = Join-Path $Root 'src'
$thirdParty = Join-Path $srcDir 'third_party'
$nativeRoot = Join-Path $Root '.native'

# ==================== 构建环境 (对应 .bat 里的 chcp / VSLANG / 关闭 vcpkg) ====================

# 控制台按 UTF-8: 顶层 CMakeLists 会据此写 Directory.Build.targets,
# 强制 MSBuild 的 CustomBuild 任务按 UTF-8 解码嵌套构建的输出 (修中文乱码)
$env:MUSICXX_EXT_BUILD_CONSOLE_CP = '65001'
try { [Console]::OutputEncoding = [System.Text.Encoding]::UTF8 } catch {}
# MSVC 输出英文诊断
$env:VSLANG = '1033'
# 完全停用 vcpkg: 依赖全部来自 third_party 源码构建, 不需要 vcpkg;
# VCPKG_ROOT 置空停用 CMake 工具链集成, VCPkgLocalAppDataDisabled=1 停用 MSBuild 的
# 用户级集成 (否则每次构建都会跑 applocal.ps1 收集 DLL 并打印 "Failed to gather ..." 警告)
$env:VCPKG_ROOT = ''
$env:VCPkgLocalAppDataDisabled = '1'

# ==================== 平台 / 目录 / 生成器 ====================

# Android NDK 位置解析 (显式参数 → 环境变量 → SDK 下版本号最大的一个)
function Resolve-AndroidNdk([string]$Explicit) {
    $marker = 'build/cmake/android.toolchain.cmake'
    if ($Explicit) {
        if (-not (Test-Path (Join-Path $Explicit $marker))) {
            throw "-AndroidNdk 指向的目录里没有 $marker : $Explicit"
        }
        return (Resolve-Path $Explicit).Path
    }
    foreach ($envName in @('ANDROID_NDK_HOME', 'ANDROID_NDK_ROOT')) {
        $value = [Environment]::GetEnvironmentVariable($envName)
        if ($value -and (Test-Path (Join-Path $value $marker))) {
            # 说明: 只返回路径, 提示走 Write-Host —— 函数里的 Write-Output 会被当成返回值
            Write-Host "   NDK: 取自 `$env:$envName ($value)"
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
            Write-Host "   NDK: 取自 `$env:$sdkName\ndk ($($pick.Path))"
            return $pick.Path
        }
    }
    throw ("未找到 Android NDK (需要 build/cmake/android.toolchain.cmake)。`n" +
        "  请安装 NDK 并指定: -AndroidNdk <NDK 根目录>, 或设置环境变量 ANDROID_NDK_HOME")
}

$os = if ($IsWindows -or $env:OS -eq 'Windows_NT') { 'windows' }
elseif ($IsMacOS) { 'macos' }
else { 'linux' }

$arch = switch ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()) {
    'X64' { 'x64' }
    'Arm64' { 'arm64' }
    'X86' { 'x86' }
    default { 'unknown' }
}

# Android: 目标平台与宿主平台无关, 平台/架构段一律按 ABI 推导 (与桌面端的目录命名保持一致)
$androidToolchain = ''
$androidAbi = ''
if ($Android) {
    $os = 'android'
    $androidAbi = $Abi
    $arch = switch ($Abi) {
        'arm64-v8a' { 'arm64' }
        'armeabi-v7a' { 'arm' }
        'x86_64' { 'x64' }
        'x86' { 'x86' }
        default { 'unknown' }
    }
    $androidNdkRoot = Resolve-AndroidNdk -Explicit $AndroidNdk
    $androidToolchain = Join-Path $androidNdkRoot 'build/cmake/android.toolchain.cmake'
    Write-Output "   Android: ABI=$androidAbi platform=$AndroidPlatform stl=$AndroidStl"
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
if ($Android) {
    # Android 的目标处理器由 NDK 工具链按 ANDROID_ABI 推导, 显式传值只会造成两边不一致
    $systemProcessor = ''
}

if (-not $BuildDir) {
    # Android 一个 ABI 一个构建目录 (桌面端沿用 <平台>-<配置>)
    $BuildDir = if ($Android) { Join-Path $nativeRoot "build/android-$androidAbi-$($Config.ToLower())" }
    else { Join-Path $nativeRoot "build/$os-$($Config.ToLower())" }
}
if (-not $OutputDir) {
    # Android 用 ABI 名做架构段 (android-arm64-v8a-release), 桌面端沿用 <平台>-<架构>-<配置>
    $OutputDir = if ($Android) { Join-Path $nativeRoot "output/android-$androidAbi-$($Config.ToLower())" }
    else { Join-Path $nativeRoot "output/$os-$arch-$($Config.ToLower())" }
}
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)
$installDir = Join-Path $BuildDir 'musicxx-extern-plugin-install'

if (-not $Generator) {
    if ($os -eq 'windows') {
        $help = (& cmake --help 2>$null) -join "`n"
        foreach ($candidate in @('Visual Studio 18 2026', 'Visual Studio 17 2022')) {
            if ($help -match [regex]::Escape($candidate)) { $Generator = $candidate; break }
        }
    }
    if (-not $Generator) {
        if (Get-Command ninja -ErrorAction SilentlyContinue) { $Generator = 'Ninja' } else { $Generator = 'Unix Makefiles' }
    }
}
if ($Android) {
    # 交叉编译固定用 Ninja: 与 NDK 工具链配合最稳, 也不依赖宿主平台的 IDE 生成器
    if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
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
            if (Get-Command ninja -ErrorAction SilentlyContinue) { break }
        }
    }
    if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
        throw "Android 交叉编译需要 ninja (未在 PATH 上找到); 请安装 ninja 或使用 Android SDK 自带的 cmake/bin/ninja.exe"
    }
    $Generator = 'Ninja'
}
$isMultiConfig = $Generator -like 'Visual Studio*'
$generatorArgs = @('-G', $Generator)
if ($isMultiConfig) { $generatorArgs += @('-A', $generatorPlatform) }

$isRelease = $Config -in @('Release', 'MinSizeRel')
$xxIsRelease = if ($isRelease) { '1' } else { '0' }

function Write-Step([string]$Text) {
    Write-Output ""
    Write-Output "==> $Text"
}

Write-Output "=== musicxx_extern_plugin 原生构建 ==="
Write-Output "  源目录:     $Root"
Write-Output "  构建目录:   $BuildDir"
Write-Output "  安装目录:   $installDir"
Write-Output "  输出目录:   $OutputDir"
Write-Output "  配置:       $Config (XX_IS_RELEASE_D=$xxIsRelease)   生成器: $Generator$(if ($isMultiConfig) { " ($generatorPlatform)" })"
if ($Android) {
    Write-Output "  目标:       android-$androidAbi ($AndroidPlatform, $AndroidStl)"
    Write-Output "  工具链:     $androidToolchain"
}

# ==================== 环境预检 (失败就给明确原因, 对应 .bat 的 env 检查) ====================

Write-Step '环境预检'
foreach ($tool in @('cmake', 'git')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "未找到 $tool; 请先安装并加入 PATH"
    }
}
if ($Android) {
    # 交叉编译不依赖 Visual Studio / pkg-config: 编译器与依赖查找全由 NDK 工具链接管
    if (-not (Test-Path $androidToolchain)) {
        throw "找不到 NDK 工具链文件: $androidToolchain"
    }
    Write-Output "  ninja: $(Get-Command ninja | Select-Object -ExpandProperty Source)"
}
if ($os -eq 'windows') {
    $vsFound = $false
    foreach ($pf in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if ($pf -and (Test-Path (Join-Path $pf 'Microsoft Visual Studio/Installer/vswhere.exe'))) { $vsFound = $true }
    }
    if (-not $vsFound) {
        foreach ($pf in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
            if ($pf -and (Test-Path (Join-Path $pf 'Microsoft Visual Studio'))) { $vsFound = $true }
        }
    }
    if (-not $vsFound) {
        Write-Output '  [警告] 未检测到 Visual Studio (vswhere 与安装目录都不存在); 若使用其它工具链请忽略'
    }
}
if ($os -eq 'windows' -and -not (Get-Command pkg-config -ErrorAction SilentlyContinue)) {
    Write-Output '  [警告] 未找到 pkg-config; cxx_utilxx_base / cxx_pluginxx 配置阶段会失败 (find_package(PkgConfig REQUIRED))'
}
Write-Output "  cmake: $((& cmake --version)[0])"

# 子模块预检 (commit 由仓库 gitlink 记录, 这里只确认已初始化)
Write-Output '  子模块:'
$submoduleStatus = & git -C $Root submodule status 2>$null
$missing = @()
foreach ($line in $submoduleStatus) {
    if (-not $line) { continue }
    $trimmed = $line.TrimStart()
    if ($trimmed.StartsWith('-')) { $missing += ($trimmed -split '\s+')[1] }
}
if ($missing.Count -gt 0) {
    throw ("以下子模块未初始化:`n  " + ($missing -join "`n  ") + "`n请先执行: git submodule update --init --recursive")
}
Write-Output ("    已就绪: {0} 个 (git submodule status)" -f $submoduleStatus.Count)

# ==================== Boost (仅头文件) ====================
# 只用 Boost.Asio 头, 不链接任何 Boost 编译库。cmake 侧只负责生成 BoostConfig.cmake 与校验版本,
# 头文件的获取(复用/下载/解包/校验)放在这里。版本、下载地址与校验值一同维护在下面的常量里。
$boostVersion = '1.92.0'    # 与 src/third_party/boost/include 中的 boost/version.hpp 一致
$boostSourceUrl = 'https://archives.boost.io/release/@VERSION@/source/boost_@UNDERSCORE@.tar.gz'
$boostSha256 = 'c4a3b310ddd2472416e091067166b0713be97c63f38c212c484ada022fd296ce'

function Test-BoostRoot([string]$Path) {
    return [bool]($Path -and (Test-Path (Join-Path $Path 'boost/version.hpp')))
}

function Get-BoostVersionCode([string]$HeaderRoot) {
    $hit = Select-String -Path (Join-Path $HeaderRoot 'boost/version.hpp') -Pattern '^#define BOOST_VERSION ' |
        Select-Object -First 1
    if (-not $hit) { return 0 }
    return [int]($hit.Line -replace '^#define BOOST_VERSION\s+', '')
}

function Expand-BoostArchive([string]$ArchivePath, [string]$Version, [string]$DestRoot) {
    # Boost 发布包 (archives.boost.io) 顶层是 boost_<下划线版本>/boost/...
    $dirName = 'boost_' + ($Version -replace '\.', '_')
    $downloadRoot = Join-Path $nativeRoot 'downloads'
    $tmp = Join-Path $downloadRoot ("extract-" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    Write-Host "  解包 $ArchivePath"
    & tar -xf $ArchivePath -C $tmp "$dirName/boost"
    if ($LASTEXITCODE -ne 0) { throw "解包失败: $ArchivePath" }
    $headerRoot = Join-Path $tmp $dirName
    if (-not (Test-BoostRoot $headerRoot)) { throw "解包结果里没有 $dirName/boost/version.hpp" }
    if (Test-Path $DestRoot) { Remove-Item $DestRoot -Recurse -Force }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $DestRoot) | Out-Null
    Move-Item $headerRoot $DestRoot
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    return $DestRoot
}

function Resolve-BoostHeaderRoot {
    $cached = Join-Path $nativeRoot ('boost/boost_' + ($boostVersion -replace '\.', '_'))

    # 1) 显式参数
    if ($BoostRoot) {
        if (-not (Test-BoostRoot $BoostRoot)) { throw "-BoostRoot 下没有 boost/version.hpp: $BoostRoot" }
        Write-Host "  Boost: 使用 -BoostRoot 指定的头文件 $BoostRoot"
        return (Resolve-Path $BoostRoot).Path
    }

    # 2) 显式指定的发布包 (优先于内置头文件, 便于离线/自备包)
    if ($BoostArchive) {
        if (-not (Test-Path $BoostArchive)) { throw "-BoostArchive 指向的文件不存在: $BoostArchive" }
        $expanded = @(Expand-BoostArchive -ArchivePath $BoostArchive -Version $boostVersion -DestRoot $cached)
        return $expanded[-1]
    }

    # 3) 本包内置的头文件 (src/third_party/boost/include)
    $vendored = Join-Path $thirdParty 'boost/include'
    if (Test-BoostRoot $vendored) {
        Write-Host "  Boost: 使用本包内置头文件 $vendored"
        return $vendored
    }

    # 4) 之前下载解包过的缓存
    if (Test-BoostRoot $cached) {
        Write-Host "  Boost: 使用已解包缓存 $cached"
        return $cached
    }

    # 5) 按固定版本下载官方发布包
    if ($NoDownloadBoost) {
        throw ("未提供 Boost 头文件: 请用 -BoostRoot <含 boost/ 的目录> 或 -BoostArchive <发布包>, " +
            "或去掉 -NoDownloadBoost 让脚本自动下载 $boostSourceUrl")
    }
    $dirName = 'boost_' + ($boostVersion -replace '\.', '_')
    $downloadRoot = Join-Path $nativeRoot 'downloads'
    New-Item -ItemType Directory -Force -Path $downloadRoot | Out-Null
    $archivePath = Join-Path $downloadRoot "$dirName.tar.gz"
    if (Test-Path $archivePath) {
        Write-Host "  Boost: 复用已下载的发布包 $archivePath"
    }
    else {
        $url = $boostSourceUrl.Replace('@VERSION@', $boostVersion).Replace('@UNDERSCORE@', ($boostVersion -replace '\.', '_'))
        Write-Host "  Boost: 下载 $url"
        & curl.exe -L --fail --retry 3 --retry-delay 2 -o $archivePath $url
        if ($LASTEXITCODE -ne 0) { throw "下载 Boost 发布包失败: $url" }
    }
    if ($boostSha256) {
        $actual = (Get-FileHash -Algorithm SHA256 $archivePath).Hash.ToLower()
        if ($actual -ne $boostSha256.ToLower()) {
            throw "Boost 发布包校验失败: $archivePath`n  期望 $boostSha256`n  实际 $actual"
        }
        Write-Host "  Boost: 发布包 SHA256 校验通过"
    }
    $expanded = @(Expand-BoostArchive -ArchivePath $archivePath -Version $boostVersion -DestRoot $cached)
    return $expanded[-1]
}

Write-Step 'Boost (仅头文件)'
$boostHeaderRoot = Resolve-BoostHeaderRoot
$boostParts = $boostVersion.Split('.')
$expectCode = ([int]$boostParts[0] * 100000) + ([int]$boostParts[1] * 100) + [int]$boostParts[2]
$boostCode = Get-BoostVersionCode $boostHeaderRoot
if ($boostCode -ne $expectCode) {
    throw ("Boost 版本不符: 脚本期望 $boostVersion (BOOST_VERSION=$expectCode), " +
        "实际 BOOST_VERSION=$boostCode ($boostHeaderRoot)")
}
Write-Host "  Boost 头文件根: $boostHeaderRoot (版本 $boostVersion)"

if ($Clean) {
    Write-Step '清理构建目录'
    foreach ($p in @($BuildDir, $OutputDir)) {
        if (Test-Path $p) {
            Write-Output "  删除 $p"
            Remove-Item $p -Recurse -Force
        }
    }
}

# ==================== configure (对应 .bat 的 cmake -D... -B -S) ====================

Write-Step 'configure (superbuild: 依赖 + 宿主)'
# 构建目录签名: 配置/生成器/平台/Boost 位置变化时重置目录, 避免 CMakeCache 里残留
# 上一次的参数 (例如旧的 CMAKE_CONFIGURATION_TYPES) 让新配置报出难以定位的错。
$signature = "config=$Config;generator=$Generator;platform=$generatorPlatform;boost=$boostHeaderRoot;xxRelease=$xxIsRelease;target=$os;abi=$androidAbi;ndk=$androidToolchain;androidPlatform=$AndroidPlatform;stl=$AndroidStl"
$marker = Join-Path $BuildDir '.build_signature'
if (Test-Path $BuildDir) {
    $old = if (Test-Path $marker) { (Get-Content -Raw $marker).Trim() } else { '' }
    if ($old -ne $signature) {
        Write-Output "  构建目录的配置已变化, 重置: $BuildDir"
        Remove-Item $BuildDir -Recurse -Force
    }
}
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
Set-Content -Path $marker -Value $signature -Encoding UTF8

$configureArgs = @(
    "-DXX_IS_RELEASE_D=$xxIsRelease"
    "-DCMAKE_BUILD_TYPE=$Config"
    # 收窄可用配置到本次配置 (与 agentxx 的 windows_*_build.bat 一致): 一个构建目录只构建一个配置,
    # 依赖安装前缀在构建目录内, 因此不会混入其它配置的产物
    "-DCMAKE_CONFIGURATION_TYPES=$Config"
    # Android: 目标处理器由 NDK 工具链按 ANDROID_ABI 推导, 显式传值会造成两边不一致
    if (-not $Android) { "-DCMAKE_SYSTEM_PROCESSOR=$systemProcessor" }
    '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'
    "-DMUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE=$boostHeaderRoot"
)
if ($Android) {
    # 交叉编译参数交给 NDK 工具链 (superbuild 的 src/CMakeLists.txt 会自动补 XX_IS_ANDROID_D=1,
    # 并把 ANDROID_ABI/ANDROID_PLATFORM/ANDROID_STL 原样传给各依赖与宿主工程的嵌套构建)
    $configureArgs += @(
        "-DCMAKE_TOOLCHAIN_FILE=$androidToolchain"
        "-DANDROID_ABI=$androidAbi"
        "-DANDROID_PLATFORM=$AndroidPlatform"
        "-DANDROID_STL=$AndroidStl"
    )
}
if ($DepsOnly) { $configureArgs += '-DMUSICXX_EXTERN_PLUGIN_BUILD_HOST=OFF' }

& cmake @configureArgs @generatorArgs -B $BuildDir -S $srcDir | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'cmake configure 失败' }

if ($ConfigureOnly) {
    Write-Output ""
    Write-Output "ConfigureOnly: 配置完成 (未构建)"
    exit 0
}

# ==================== build ====================

$parallel = if ($Jobs -gt 0) { "$Jobs" } else { "$([Environment]::ProcessorCount)" }
$buildArgs = @('--build', $BuildDir, '--config', $Config, '--parallel', $parallel)
# 只构建依赖: 直接以 cxx_pluginxx_repo 为目标 (host 工程不在依赖链上)
if ($DepsOnly) { $buildArgs += @('--target', 'cxx_pluginxx_repo') }

Write-Step "build (--config $Config --parallel $parallel)"
& cmake @buildArgs | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'cmake build 失败' }

# 顶层 superbuild 自身没有安装规则 (安装都由各 ExternalProject 完成); 这里保持一致行为
& cmake --install $BuildDir --config $Config | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'cmake install 失败' }

# 宿主工程是嵌套 ExternalProject 构建: 生成器有时会判定上层的“构建/安装步骤已是最新”而跳过它,
# 于是源码改动不会真正触发重编译 (表现为"构建成功但产物时间戳/体积不变")。这里显式再跑一次
# 嵌套工程的增量构建与安装 (已经是新的就秒级结束), 保证脚本的"构建成功"与产物一致。
if (-not $DepsOnly) {
    $hostBuildDir = Join-Path (Join-Path $BuildDir 'e') 'host'
    if (Test-Path (Join-Path $hostBuildDir 'CMakeCache.txt')) {
        Write-Step 'build 宿主工程 (嵌套工程增量构建)'
        & cmake --build $hostBuildDir --config $Config --parallel $parallel | Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'cmake build (宿主工程) 失败' }
        & cmake --install $hostBuildDir --config $Config | Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'cmake install (宿主工程) 失败' }
    }
}

# ==================== 产物 ====================

Write-Step '产物'
$artifacts = @()
if (Test-Path (Join-Path $installDir 'bin')) {
    $artifacts += Get-ChildItem (Join-Path $installDir 'bin') -File |
        Where-Object { $_.Extension -in @('.dll', '.so', '.dylib', '.exe') }
}
if (Test-Path (Join-Path $installDir 'plugins')) {
    $artifacts += Get-ChildItem (Join-Path $installDir 'plugins') -Recurse -File |
        Where-Object { $_.Extension -in @('.dll', '.so', '.dylib') }
}
foreach ($f in $artifacts) {
    Write-Output ("  {0,-36} {1,10:N0} 字节" -f $f.Name, $f.Length)
}
if ($artifacts.Count -eq 0) { Write-Output '  (bin/plugins 下没有产物)' }

# 复制到稳定输出目录 (对应 .bat 里 xcopy exec → output); 失败只提示不阻断,
# 因为安装目录里的产物本身已经可用 (例如文件被正在运行的进程占用)
if (-not $DepsOnly) {
    Write-Output ""
    Write-Output "  复制产物到输出目录: $OutputDir"
    New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
    foreach ($sub in @('bin', 'plugins')) {
        # 注意: `Copy-Item <源目录> <目标目录> -Recurse` 在目标已存在时会把源目录
        # 整体嵌进目标 (得到 <目标>/<源>), 旧产物不会被覆盖 —— 必须按"内容合并"复制。
        $to = Join-Path $OutputDir $sub
        $nested = Join-Path $to $sub
        if (Test-Path $nested) { Remove-Item $nested -Recurse -Force }   # 清掉历史误拷贝
        $from = Join-Path $installDir $sub
        if (Test-Path $from) {
            New-Item -ItemType Directory -Force -Path $to | Out-Null
            Copy-Item (Join-Path $from '*') $to -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

# ==================== 原生测试 (可选) ====================

if ($RunTests) {
    Write-Step '原生测试'
    if ($DepsOnly) { throw '-RunTests 不能与 -DepsOnly 同时使用' }
    if ($Android) {
        # 测试可执行文件是给设备编的 (目标 ABI 与宿主 Windows 机不同), 宿主上跑不起来。
        # 需要在设备上跑时: adb push bin/ + plugins/ 到设备目录, 用 LD_LIBRARY_PATH 指向 bin/ 再执行。
        Write-Output "  [跳过] Android 目标是交叉编译产物, 不能在当前主机运行; 产物在: $(Join-Path $installDir 'bin')"
    } else {
        $testName = if ($os -eq 'windows') { 'musicxx_extern_plugin_test.exe' } else { 'musicxx_extern_plugin_test' }
        $testExe = Join-Path $installDir "bin/$testName"
        if (-not (Test-Path $testExe)) { throw "未找到测试可执行文件: $testExe" }

        # 宿主把"父目录下的每个子目录"当作一个插件, 因此测试参数传插件目录的父目录
        $pluginRoot = Join-Path $installDir 'plugins'
        Write-Output "  插件目录: $pluginRoot"
        Write-Output "  运行: $testExe $pluginRoot"
        & $testExe $pluginRoot | Out-Host
        Write-Output "  退出码: $LASTEXITCODE"
    }
}

Write-Output ""
if ($Android) {
    Write-Output "README: 宿主库由 android/build.gradle 按 ABI 收集进 APK (jniLibs), 重新打包安装后生效"
}
Write-Output "构建完成"
