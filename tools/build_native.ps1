# musicxx_extern_plugin 原生依赖与宿主库构建 (plan §11.2 的两阶段构建)
#
# 阶段一: 从 src/third_party 的子模块源码编译依赖, 安装到 .native/deps/<平台>-<架构>/
#         依赖顺序: fmt → yaml-cpp → simdjson → libiconv-native → uchardet
#                   → cxx_utilxx_base → cxx_pluginxx
# 阶段二: 用该前缀配置并构建宿主库、示例插件与原生测试
#
# 依赖来源只有两处, 都不依赖任何"预编译库":
#   1) src/third_party/ 下的 git 子模块 (commit 锁定见 third_party_versions.json);
#   2) Boost 头文件 (只用 Boost.Asio 头, 不链接 Boost 编译库):
#      src/third_party/boost/include 已内置时直接使用, 否则按固定版本从官方发布包下载解包,
#      并由本脚本生成 <前缀>/lib/cmake/Boost-<版本>/BoostConfig.cmake (不依赖 b2 生成的配置)。
#      字符集转换 (iconv + uchardet) 与上游默认保持一致: cxx_utilxx_base 的源码无条件使用
#      <iconv.h> 与 uchardet, 因此两者都必须从源码构建, 不能靠开关关掉。
#
# 用法:
#   pwsh -NoProfile -File tools/build_native.ps1                 # 依赖 + 宿主 (Release)
#   pwsh -NoProfile -File tools/build_native.ps1 -DepsOnly       # 只构建依赖
#   pwsh -NoProfile -File tools/build_native.ps1 -HostOnly       # 只构建宿主 (依赖已在先前缀)
#   pwsh -NoProfile -File tools/build_native.ps1 -PrepareOnly    # 只做预检与 Boost 准备, 不构建
#   pwsh -NoProfile -File tools/build_native.ps1 -Clean          # 先清空依赖前缀与构建目录
#   pwsh -NoProfile -File tools/build_native.ps1 -RunTests       # 构建后跑原生测试
#   pwsh -NoProfile -File tools/build_native.ps1 -BoostRoot <目录>       # 指定 Boost 头文件根 (含 boost/)
#   pwsh -NoProfile -File tools/build_native.ps1 -BoostArchive <压缩包>  # 用本地 Boost 发布包解包
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')][string]$Config = 'Release',
    [switch]$DepsOnly,
    [switch]$HostOnly,
    [switch]$PrepareOnly,
    [switch]$Clean,
    [switch]$RunTests,
    [int]$Jobs = 0,
    [string]$Prefix,
    [string]$BoostRoot,
    [string]$BoostArchive,
    [string]$Generator,
    [switch]$NoDownloadBoost
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Root = (Resolve-Path $Root).Path
$thirdParty = Join-Path $Root 'src/third_party'
$versionsJson = Get-Content -Raw (Join-Path $Root 'third_party_versions.json') -Encoding UTF8 | ConvertFrom-Json
$boostMeta = $versionsJson.external.boost

# ==================== 平台 / 目录 / 生成器 ====================

$os = if ($IsWindows -or $env:OS -eq 'Windows_NT') { 'windows' }
elseif ($IsMacOS) { 'macos' }
else { 'linux' }

$arch = switch ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()) {
    'X64' { 'x64' }
    'Arm64' { 'arm64' }
    'X86' { 'x86' }
    default { 'unknown' }
}

if (-not $Prefix) { $Prefix = Join-Path $Root ".native/deps/$os-$arch" }
$Prefix = [System.IO.Path]::GetFullPath($Prefix)
$nativeRoot = Join-Path $Root '.native'
$buildRoot = Join-Path $nativeRoot 'build'
$downloadRoot = Join-Path $nativeRoot 'downloads'

function Write-Step([string]$Text) {
    Write-Output ""
    Write-Output "==> $Text"
}

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
$isMultiConfig = $Generator -like 'Visual Studio*'
$generatorArgs = @('-G', $Generator)
if ($isMultiConfig) { $generatorArgs += @('-A', $arch) }

Write-Output "=== musicxx_extern_plugin 原生构建 ==="
Write-Output "  源目录:     $Root"
Write-Output "  依赖前缀:   $Prefix"
Write-Output "  构建目录:   $buildRoot"
Write-Output "  生成器:     $Generator$(if ($isMultiConfig) { " ($arch)" })    配置: $Config"

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw '未找到 cmake; 请先安装 CMake 并加入 PATH' }
if ($os -eq 'windows' -and -not (Get-Command pkg-config -ErrorAction SilentlyContinue)) {
    Write-Output "  [警告] 未找到 pkg-config; cxx_utilxx_base / cxx_pluginxx 在配置阶段会失败 (find_package(PkgConfig REQUIRED))"
}

# ==================== 子模块预检 ====================

Write-Step '子模块预检'
$missing = @()
foreach ($sub in $versionsJson.submodules) {
    $subPath = Join-Path $Root $sub.path
    if (-not (Test-Path $subPath)) { $missing += $sub.path; continue }
    $head = (& git -C $subPath rev-parse HEAD 2>$null)
    if ($head) { $head = $head.Trim() }
    if ($head -and $head -ne $sub.commit) {
        Write-Output ("  [警告] {0} 当前 {1}, third_party_versions.json 记录 {2}" -f `
                $sub.path, $head.Substring(0, 12), $sub.commit.Substring(0, 12))
    }
}
if ($missing.Count -gt 0) {
    throw ("以下子模块未初始化:`n  " + ($missing -join "`n  ") + "`n请先执行: git submodule update --init --recursive")
}
Write-Output ("  已就绪: {0} 个子模块" -f $versionsJson.submodules.Count)

# ==================== Boost (仅头文件) ====================

# 说明: 下面三个函数会返回值, 因此函数体内的日志一律用 Write-Host (Write-Output 会混进返回值)

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
    $cached = Join-Path $nativeRoot ('boost/boost_' + ($boostMeta.version -replace '\.', '_'))

    # 1) 显式参数
    if ($BoostRoot) {
        if (-not (Test-BoostRoot $BoostRoot)) { throw "-BoostRoot 下没有 boost/version.hpp: $BoostRoot" }
        Write-Host "  Boost: 使用 -BoostRoot 指定的头文件 $BoostRoot"
        return (Resolve-Path $BoostRoot).Path
    }

    # 2) 显式指定的发布包 (优先于内置头文件, 便于离线/自备包)
    if ($BoostArchive) {
        if (-not (Test-Path $BoostArchive)) { throw "-BoostArchive 指向的文件不存在: $BoostArchive" }
        $expanded = @(Expand-BoostArchive -ArchivePath $BoostArchive -Version $boostMeta.version -DestRoot $cached)
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
            "或去掉 -NoDownloadBoost 让脚本自动下载 $($boostMeta.source_url)")
    }
    $dirName = 'boost_' + ($boostMeta.version -replace '\.', '_')
    New-Item -ItemType Directory -Force -Path $downloadRoot | Out-Null
    $archivePath = Join-Path $downloadRoot "$dirName.tar.gz"
    if (Test-Path $archivePath) {
        Write-Host "  Boost: 复用已下载的发布包 $archivePath"
    }
    else {
        $url = $boostMeta.source_url.Replace('@VERSION@', $boostMeta.version).Replace('@UNDERSCORE@', ($boostMeta.version -replace '\.', '_'))
        Write-Host "  Boost: 下载 $url"
        & curl.exe -L --fail --retry 3 --retry-delay 2 -o $archivePath $url
        if ($LASTEXITCODE -ne 0) { throw "下载 Boost 发布包失败: $url" }
    }
    if ($boostMeta.archive_sha256) {
        $actual = (Get-FileHash -Algorithm SHA256 $archivePath).Hash.ToLower()
        if ($actual -ne $boostMeta.archive_sha256.ToLower()) {
            throw "Boost 发布包校验失败: $archivePath`n  期望 $($boostMeta.archive_sha256)`n  实际 $actual"
        }
        Write-Host "  Boost: 发布包 SHA256 校验通过"
    }
    $expanded = @(Expand-BoostArchive -ArchivePath $archivePath -Version $boostMeta.version -DestRoot $cached)
    return $expanded[-1]
}

function Write-BoostConfig([string]$HeaderRoot) {
    # 内核与基础库都用 find_package(Boost CONFIG) 且只取头文件根, 因此这里生成一份最小配置
    $cfgDir = Join-Path $Prefix ("lib/cmake/Boost-" + $boostMeta.version)
    New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
    $template = Get-Content -Raw (Join-Path $PSScriptRoot 'cmake/BoostConfig.cmake.in') -Encoding UTF8
    $content = $template.
        Replace('@BOOST_INCLUDE_DIR@', ($HeaderRoot -replace '\\', '/')).
        Replace('@BOOST_VERSION@', $boostMeta.version)
    Set-Content -Path (Join-Path $cfgDir 'BoostConfig.cmake') -Value $content -Encoding UTF8
    Write-Host "  Boost: 生成 $cfgDir/BoostConfig.cmake (头文件根 $($HeaderRoot -replace '\\', '/'))"
}

# ==================== 构建辅助 ====================

New-Item -ItemType Directory -Force -Path $Prefix, $buildRoot | Out-Null

if ($Clean) {
    Write-Step '清理 (依赖前缀与构建目录)'
    foreach ($p in @($Prefix, $buildRoot)) {
        if (Test-Path $p) {
            Write-Output "  删除 $p"
            Remove-Item $p -Recurse -Force
        }
    }
    New-Item -ItemType Directory -Force -Path $Prefix, $buildRoot | Out-Null
}

# 同一个构建目录换前缀/换 Boost 后必须重新配置, 否则 CMake 缓存里的 <包>_DIR 会继续指向旧前缀
# (此前正是这样把依赖悄悄指到了别的预编译前缀上); 因此记录签名, 不一致就重置该构建目录。
function Reset-BuildDirIfStale([string]$Name) {
    $dir = Join-Path $buildRoot $Name
    $signature = "prefix=$Prefix;boost=$script:boostHeaderRoot;config=$Config;generator=$Generator"
    if (Test-Path $dir) {
        $marker = Join-Path $dir '.build_signature'
        $old = if (Test-Path $marker) { (Get-Content -Raw $marker).Trim() } else { '' }
        if ($old -ne $signature) {
            Write-Host "  [$Name] 构建目录的依赖前缀已变化, 重置 $dir"
            Remove-Item $dir -Recurse -Force
        }
    }
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Set-Content -Path (Join-Path $dir '.build_signature') -Value $signature -Encoding UTF8
    return $dir
}

function Invoke-CMakeStep {
    param([string]$Name, [string]$Source, [string[]]$ConfigureArgs)
    $dir = Reset-BuildDirIfStale $Name
    Write-Host "  [$Name] configure"
    & cmake -S $Source -B $dir @generatorArgs @ConfigureArgs | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "[$Name] configure 失败" }
    Write-Host "  [$Name] build"
    $buildArgs = @('--build', $dir, '--config', $Config, '--parallel')
    if ($Jobs -gt 0) { $buildArgs[-1] = "$Jobs" }
    & cmake @buildArgs | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "[$Name] build 失败" }
    Write-Host "  [$Name] install"
    & cmake --install $dir --config $Config | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "[$Name] install 失败" }
}

# ==================== Boost 准备 ====================

Write-Step 'Boost (仅头文件)'
$script:boostHeaderRoot = Resolve-BoostHeaderRoot
$boostCode = Get-BoostVersionCode $script:boostHeaderRoot
$expectedParts = $boostMeta.version.Split('.')
$expectCode = ([int]$expectedParts[0] * 100000) + ([int]$expectedParts[1] * 100) + [int]$expectedParts[2]
if ($boostCode -ne $expectCode) {
    throw ("Boost 版本不符: 期望 $($boostMeta.version) (BOOST_VERSION=$expectCode), " +
        "实际 BOOST_VERSION=$boostCode ($script:boostHeaderRoot)")
}
Write-BoostConfig $script:boostHeaderRoot

if ($PrepareOnly) {
    Write-Output ""
    Write-Output "PrepareOnly: 预检与 Boost 准备完成 (未构建)"
    exit 0
}

# ==================== 阶段一: 依赖 ====================

if (-not $HostOnly) {
    Write-Step '阶段一: 从 src/third_party 构建依赖'
    $common = @(
        "-DCMAKE_INSTALL_PREFIX=$Prefix",
        "-DCMAKE_PREFIX_PATH=$Prefix",
        '-DBUILD_SHARED_LIBS=OFF',
        "-DCMAKE_BUILD_TYPE=$Config"
    )

    Invoke-CMakeStep -Name 'fmt' -Source (Join-Path $thirdParty 'fmt') -ConfigureArgs ($common + @(
            '-DFMT_TEST=OFF', '-DFMT_DOC=OFF', '-DFMT_INSTALL=ON'
        ))

    Invoke-CMakeStep -Name 'yaml-cpp' -Source (Join-Path $thirdParty 'yaml-cpp') -ConfigureArgs ($common + @(
            '-DYAML_CPP_BUILD_TESTS=OFF', '-DYAML_CPP_BUILD_TOOLS=OFF', '-DYAML_CPP_INSTALL=ON'
        ))

    # cxx_utilxx_base 的 JSON 后端依赖 simdjson
    Invoke-CMakeStep -Name 'simdjson' -Source (Join-Path $thirdParty 'simdjson') -ConfigureArgs ($common + @(
            '-DSIMDJSON_JUST_LIBRARY=ON', '-DSIMDJSON_BUILD_STATIC=ON',
            '-DSIMDJSON_DEVELOPER_MODE=OFF', '-DSIMDJSON_ENABLE_THREADS=OFF'
        ))

    # 字符集: libiconv (C, 提供 iconv.h 与 iconvConfig.cmake) + uchardet (C++, 编码探测)
    Invoke-CMakeStep -Name 'libiconv-native' -Source (Join-Path $thirdParty 'libiconv-native') -ConfigureArgs $common

    # uchardet 的命令行工具需要 getopt.h (Windows 无): 只构建库与包配置, 不构建工具与测试
    Invoke-CMakeStep -Name 'uchardet' -Source (Join-Path $thirdParty 'uchardet') -ConfigureArgs ($common + @(
            '-DBUILD_BINARY=OFF', '-DUCHARDET_BUILD_TOOLS=OFF'
        ))

    Invoke-CMakeStep -Name 'cxx_utilxx_base' -Source (Join-Path $thirdParty 'cxx_utilxx_base') -ConfigureArgs ($common + @(
            '-DCXX_UTILXX_BASE_ENABLE_CHARSET=ON', '-DCXX_UTILXX_BASE_USE_BOOST_ASIO=ON',
            '-DCXX_UTILXX_BASE_LINUX_IO_URING_SUPPORTED=OFF'
        ))

    Invoke-CMakeStep -Name 'cxx_pluginxx' -Source (Join-Path $thirdParty 'cxx_pluginxx') -ConfigureArgs $common
}

# ==================== 阶段二: 宿主 ====================

if (-not $DepsOnly) {
    Write-Step '阶段二: 宿主库 / 示例插件 / 原生测试'
    $hostDir = Reset-BuildDirIfStale 'host'
    $boostCfgDir = Join-Path $Prefix ("lib/cmake/Boost-" + $boostMeta.version)
    $hostArgs = @(
        "-DMUSICXX_EXTERN_PLUGIN_NATIVE_PREFIX=$Prefix",
        "-DMUSICXX_EXTERN_PLUGIN_DEP_PREFIX=$Prefix",
        "-DBoost_DIR=$boostCfgDir",
        "-DCMAKE_BUILD_TYPE=$Config"
    )
    Write-Host "  [host] configure"
    & cmake -S (Join-Path $Root 'src') -B $hostDir @generatorArgs @hostArgs | Out-Host
    if ($LASTEXITCODE -ne 0) { throw '[host] configure 失败' }
    Write-Host "  [host] build"
    $buildArgs = @('--build', $hostDir, '--config', $Config, '--parallel')
    if ($Jobs -gt 0) { $buildArgs[-1] = "$Jobs" }
    & cmake @buildArgs | Out-Host
    if ($LASTEXITCODE -ne 0) { throw '[host] build 失败' }
}

# ==================== 产物 ====================

Write-Step '产物'
$hostRelease = Join-Path $buildRoot 'host/Release'
$artifacts = @()
if (Test-Path $hostRelease) {
    $artifacts += Get-ChildItem $hostRelease -File | Where-Object { $_.Extension -in @('.dll', '.so', '.dylib', '.exe', '.lib', '.a') }
}
if ($artifacts.Count -gt 0) {
    foreach ($f in $artifacts) {
        Write-Output ("  {0,-34} {1,10:N0} 字节" -f $f.Name, $f.Length)
    }
}
else {
    Write-Output '  (未找到产物; 上面若已构建成功请检查 Release 目录)'
}
Write-Output ""
Write-Output "  依赖前缀: $Prefix"
Write-Output "  构建目录: $buildRoot"

# ==================== 原生测试 (可选) ====================

if ($RunTests) {
    Write-Step '原生测试'
    if ($DepsOnly) { throw '-RunTests 不能与 -DepsOnly 同时使用' }
    $testExe = Join-Path $hostRelease 'musicxx_extern_plugin_test.exe'
    if (-not (Test-Path $testExe)) { $testExe = Join-Path $hostRelease 'musicxx_extern_plugin_test' }
    if (-not (Test-Path $testExe)) { throw "未找到测试可执行文件: $testExe" }

    # 宿主把"父目录下的每个子目录"当作一个插件, 因此测试参数传插件目录的父目录
    $pluginRoot = Join-Path $buildRoot 'host/test-plugins'
    $exampleDir = Join-Path $pluginRoot 'example_native'
    New-Item -ItemType Directory -Force -Path $exampleDir | Out-Null
    $exampleBuilt = Join-Path $buildRoot 'host/example_native/Release'
    if (-not (Test-Path $exampleBuilt)) { $exampleBuilt = Join-Path $buildRoot 'host/example_native' }
    if (Test-Path $exampleBuilt) {
        Get-ChildItem $exampleBuilt -File | Where-Object { $_.Extension -in @('.dll', '.so', '.dylib') } |
            Copy-Item -Destination $exampleDir -Force
    }
    Copy-Item (Join-Path $Root 'example/example_native/plugin.yaml') $exampleDir -Force
    Write-Output "  插件目录: $pluginRoot"
    Write-Output "  运行: $testExe $pluginRoot"
    & $testExe $pluginRoot | Out-Host
    Write-Output "  退出码: $LASTEXITCODE"
}

Write-Output ""
Write-Output "构建完成"
