#
# musicxx_extern_plugin 依赖库: OpenSSL (客户端 TLS)
#
# 被 tools/setup_native_env.ps1 与 tools/build_native.ps1 点源引入(dot-source), 只放
# "OpenSSL 装在哪 / 没有就怎么弄到"这一件事, 构建流程本身不写在这里。
#
# 为什么要 OpenSSL: 插件网络能力的客户端要能连 https / wss (服务端仍然只有 http / ws,
# 插件自己监听时不做 TLS)。做法与 agentxx 一致: 静态链接, 不随包分发 DLL。
#
# **TLS 是必须的能力, 不存在"某个构建里没有"的版本** —— 各平台都要准备好安装树:
#   - Windows: 用 slproweb 的预编译包 (https://slproweb.com/products/Win32OpenSSL.html):
#     从它的官方哈希清单里挑最新的 4.x Win64 完整包, 下载后**解包**到
#     .native/openssl/windows-<arch>, 不执行安装程序。完整包约 260 MB (头文件 + 全部静态库
#     + 工具); Light 包没有静态库, 不能用。
#   - Android: 在 **Windows 上用 NDK 自己编** (见下面 "Android: 用本机 NDK 编"), 产物落到
#     .native/openssl/android-<abi>。Linux / macOS 上由 tools/build_native.sh 编同一个位置。
#   - Linux / macOS / iOS: 由 tools/build_native.sh 从源码编到 .native/openssl/<目标标签>。
# 各平台安装树按目标分开存放, 不能混用 —— Windows 的 .lib 在 Android / Linux 上链接不了。
#
# Android: 用本机 NDK 编 (不需要 WSL)
#   Android 目标也能在 Windows 上编出来: OpenSSL 的 Configure 是 perl 脚本, 用 Git for Windows
#   自带的 perl 就能跑; 剩下的 make 从 ezwinports 取一份 make.exe; 编译器用 NDK 的
#   <triple><api>-clang (它就是 clang --target=..., Configure 认 NDK 的 prebuilt 布局)。
#   两处坑 (都已在本文件里处理):
#     1. Git 自带的 perl 是精简装, 缺 IPC::Cmd / Pod::Usage (后者会让每次生成头文件失败,
#        表现是编译到一半出现 0 字节头文件)。tools/openssl_perl_shim/ 里放了这两个模块的
#        极简替代品, 构建时拷进源码树并用**相对** PERL5LIB 传进去 (msys perl 会把
#        "C:\..." 里的冒号当路径分隔符)。
#     2. 编译时 PATH 上要先有 NDK 的 bin 目录, 而且 ANDROID_NDK_ROOT 要指向 NDK 根
#        (Configure 靠这两样确定工具链)。
#   所以整个 ABI 编完不需要 WSL, 也不用手工摆目录。
#
# 目录约定: 解析结果是一棵 CMake 能直接用的安装树
#   Windows (slproweb 布局): include/openssl/ssl.h + lib/VC/x64/MD{,d}/libssl_static.lib
#   源码编译布局:            include/openssl/ssl.h + lib/libssl.a (或 lib64/libssl.a)
#
# 用法 (由调用方 dot-source 本文件后调用):
#   $openssl = Resolve-MxOpenSslRoot -Root <包目录> [-OpenSslRoot <目录>] [-NoDownloadOpenSsl] \
#                                    -TargetTag windows-x64|android-arm64-v8a|... \
#                                    [-AndroidNdk <NDK 根>] [-AndroidApi 24] [-Jobs N]
#   if ($openssl.Root) { ... }   # 拿不到时: 非交叉目标自己编, Android 报错 (TLS 不可缺失)

Set-StrictMode -Version Latest

# 版本固定为 4.x; 空 = 取 slproweb 清单里最新的 4.x (与 agentxx 的做法一致)。
# 需要别的版本时设环境变量 MUSICXX_EXTERN_PLUGIN_OPENSSL_VERSION (例: 4.0.3)。
$script:MxOpenSslHashUrl = 'https://slproweb.com/download/win32_openssl_hashes.json'
# 清单取不到时的兜底 (与清单里的 Win64 完整包一致; 升级时一起改)
$script:MxOpenSslFallbackVersion = '4.0.3'
$script:MxOpenSslFallbackUrl = 'https://slproweb.com/download/Win64OpenSSL-4_0_3.msi'
$script:MxOpenSslFallbackSha256 = '43f6895ace409b9bed115431221a6ed17d0754dcb2d8d8a149bafa20fa29c7ad'

# 目标平台标签 (与 CMake 侧 src/CMakeLists.txt 的候选目录、build_native.sh 的缓存目录同名):
#   windows-x64 / windows-arm64 / linux-x64 / macos-arm64 / android-<abi> / ios-arm64 / ios-simulator-<arch>
# 静态库位置按"Windows 预编译包"与"源码编译"两种布局区分。
function Get-MxOpenSslTargetKind([string]$TargetTag) {
    if ($TargetTag -like 'windows*') { return 'windows' }
    if ($TargetTag -like 'android*') { return 'android' }
    return 'posix'
}

# 这棵安装树能不能给这个目标用: 头文件 + **该目标平台**的静态库
#   -TargetTag 不给时两种布局都认 (只用于"解包出来的目录像不像一棵安装树"这种场合)
function Test-MxOpenSslRoot {
    param(
        [string]$Path,
        [string]$TargetTag = ''
    )

    if (-not $Path) { return $false }
    if (-not (Test-Path (Join-Path $Path 'include/openssl/ssl.h'))) { return $false }

    $windowsLibs = @('lib/VC/x64/MD/libssl_static.lib', 'lib/VC/x64/MDd/libssl_static.lib',
        'lib/VC/ARM64/MD/libssl_static.lib', 'lib/VC/ARM64/MDd/libssl_static.lib',
        'lib/libssl.lib', 'libssl.lib')
    $posixLibs = @('lib/libssl.a', 'lib64/libssl.a')
    $relatives = if (-not $TargetTag) { $windowsLibs + $posixLibs }
    elseif ((Get-MxOpenSslTargetKind $TargetTag) -eq 'windows') { $windowsLibs }
    else { $posixLibs }

    foreach ($relative in $relatives) {
        if (Test-Path (Join-Path $Path $relative)) { return $true }
    }
    return $false
}

# 从 include/openssl/opensslv.h 读版本串 (只用于打印; 读不到就是空)
function Get-MxOpenSslVersionText([string]$Root) {
    if (-not $Root) { return '' }
    $header = Join-Path $Root 'include/openssl/opensslv.h'
    if (-not (Test-Path $header)) { return '' }
    foreach ($line in (Get-Content -Encoding UTF8 $header)) {
        $trimmed = $line.Trim()
        # 头文件里的写法是 "# define OPENSSL_VERSION_STR \"4.0.1\"" (# 与 define 之间有空格)
        if (-not $trimmed.StartsWith('#')) { continue }
        if (-not $trimmed.Contains('OPENSSL_VERSION_STR')) { continue }
        $first = $trimmed.IndexOf('"')
        $last = $trimmed.LastIndexOf('"')
        if ($first -ge 0 -and $last -gt $first) {
            return $trimmed.Substring($first + 1, $last - $first - 1)
        }
    }
    return ''
}

# slproweb 哈希清单: 返回 @{ Version; Url; Sha256 }, 取不到返回 $null
function Get-MxOpenSslPackageMeta {
    param([string]$Version)

    if (-not $Version) { $Version = $env:MUSICXX_EXTERN_PLUGIN_OPENSSL_VERSION }
    try {
        $manifest = Invoke-RestMethod -Uri $script:MxOpenSslHashUrl -UseBasicParsing -TimeoutSec 30
    }
    catch {
        Write-MxWarn "取不到 OpenSSL 版本清单: $($_.Exception.Message)"
        return $null
    }
    $files = $manifest.files
    if (-not $files) { return $null }

    if ($Version) {
        # 指定版本: 优先 MSI (能管理安装解包, 不需要管理员权限), 其次 EXE
        foreach ($ext in @('.msi', '.exe')) {
            $name = 'Win64OpenSSL-' + ($Version -replace '\.', '_') + $ext
            $entry = $files.PSObject.Properties[$name]
            if ($entry -and $entry.Value) {
                return [ordered]@{ Version = $Version; Url = $entry.Value.url; Sha256 = $entry.Value.sha256 }
            }
        }
        Write-MxWarn "清单里没有 OpenSSL $Version 的 Win64 完整包"
        return $null
    }

    # 未指定版本: 清单里最新的 4.x (文件名形如 Win64OpenSSL-4_0_3.msi)
    $best = $null
    $bestVersion = [version]'0.0.0'
    foreach ($property in $files.PSObject.Properties) {
        $name = [string]$property.Name
        if (-not $name.StartsWith('Win64OpenSSL-')) { continue }
        $rest = $name.Substring('Win64OpenSSL-'.Length)
        $dot = $rest.LastIndexOf('.')
        if ($dot -lt 0) { continue }
        $ext = $rest.Substring($dot)
        if ($ext -ne '.msi' -and $ext -ne '.exe') { continue }
        $numberText = $rest.Substring(0, $dot)
        if (-not $numberText.StartsWith('4_')) { continue }
        try { $parsed = [version]($numberText.Replace('_', '.')) }
        catch { continue }
        if ($parsed -gt $bestVersion) {
            $bestVersion = $parsed
            $best = [ordered]@{
                Version = $parsed.ToString()
                Url     = $property.Value.url
                Sha256  = $property.Value.sha256
            }
        }
    }
    return $best
}

# 把已下载的 slproweb 包 (MSI / NSIS EXE) 解包到 $DestDir
#   - MSI: msiexec 管理安装 (/a) 只把文件铺开, 不写注册表、不需要管理员权限
#   - EXE: 7-Zip 解包 NSIS 外壳 (同样不执行安装程序)
# 安装包自己的目录层级不确定, 这里按 include/openssl/opensslv.h 递归找到安装树根。
function Expand-MxOpenSslPackage {
    param(
        [Parameter(Mandatory)][string]$PackagePath,
        [Parameter(Mandatory)][string]$DestDir
    )

    $tmp = Join-Path ([System.IO.Path]::GetTempPath()) ('musicxx-openssl-' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    try {
        if ($PackagePath.EndsWith('.msi')) {
            Write-MxInfo 'OpenSSL: 管理安装解包 (msiexec /a)'
            $log = Join-Path $tmp 'msiexec.log'
            $arguments = @('/a', $PackagePath, '/qn', "TARGETDIR=$tmp", '/L*v', $log)
            $process = Start-Process -FilePath 'msiexec.exe' -ArgumentList $arguments -Wait -PassThru
            # 3010 = 成功但需要重启 (管理安装不会走到这里, 保险起见认下)
            if ($process.ExitCode -ne 0 -and $process.ExitCode -ne 3010) {
                throw "msiexec 管理安装失败 (退出码 $($process.ExitCode)); 日志: $log"
            }
        }
        elseif ($PackagePath.EndsWith('.exe')) {
            $sevenZip = Get-Command 7z -ErrorAction SilentlyContinue
            if (-not $sevenZip) { $sevenZip = Get-Command 7za -ErrorAction SilentlyContinue }
            if (-not $sevenZip) {
                throw ('解包 slproweb 安装包需要 7-Zip (7z.exe); 也可以先用 -OpenSslRoot ' +
                    '<OpenSSL 安装目录> 指定一份已有安装 (含 include/openssl/ssl.h 与静态库)')
            }
            Write-MxInfo "OpenSSL: 用 7-Zip 解包 ($($sevenZip.Source))"
            & $sevenZip.Source x -y "-o$tmp" $PackagePath | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "7-Zip 解包失败 (退出码 $LASTEXITCODE): $PackagePath" }
        }
        else {
            throw "不认识的 OpenSSL 安装包: $PackagePath"
        }

        $header = $null
        foreach ($candidate in (Get-ChildItem $tmp -Recurse -File -Filter 'opensslv.h' -ErrorAction SilentlyContinue)) {
            if ($candidate.DirectoryName.EndsWith('openssl')) { $header = $candidate; break }
        }
        if (-not $header) { throw "解包结果里没有 openssl/opensslv.h: $tmp" }
        # <根>/include/openssl/opensslv.h → 根
        $root = Split-Path -Parent (Split-Path -Parent $header.DirectoryName)
        if (-not (Test-MxOpenSslRoot $root 'windows-x64')) {
            throw ("解包出来的目录不是可用的 OpenSSL 安装树 (缺静态库): $root`n" +
                "  slproweb 的 Light 包不含静态库, 请用完整包")
        }
        if (Test-Path $DestDir) { Remove-Item $DestDir -Recurse -Force }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $DestDir) | Out-Null
        Move-Item $root $DestDir
        return $DestDir
    }
    finally {
        Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# ==================== Android: 用本机 NDK 从源码编 (Windows) ====================
#
# 为什么放在这里而不是靠外面的 WSL: TLS 是必备能力, "某个 ABI 编不出来"就等于功能缺失。
# OpenSSL 的 Configure 就是一个 perl 脚本, make 只负责跑编译器与 perl 生成脚本, 所以 Windows 上
# 只要有 perl + make + NDK clang 就能编出 Android 静态库 —— 不需要类 Unix 环境。

# OpenSSL 源码版本 (与 tools/build_native.sh 的兜底值一致; 需要别的版本时设
# 环境变量 MUSICXX_EXTERN_PLUGIN_OPENSSL_VERSION, 两个脚本都认)
$script:MxOpenSslSourceVersion = if ($env:MUSICXX_EXTERN_PLUGIN_OPENSSL_VERSION) {
    $env:MUSICXX_EXTERN_PLUGIN_OPENSSL_VERSION
}
else { '4.0.1' }
$script:MxOpenSslSourceUrls = @(
    'https://github.com/openssl/openssl/releases/download/openssl-@VERSION@/openssl-@VERSION@.tar.gz',
    'https://www.openssl.org/source/openssl-@VERSION@.tar.gz'
)
# 下载 make 的地方 (ezwinports 的 Windows 版 GNU make; Scoop 的 make 包也是这个源)
$script:MxMakeUrl = 'https://downloads.sourceforge.net/project/ezwinports/make-4.4.1-without-guile-w32-bin.zip'

# Android ABI → OpenSSL Configure 目标
function Get-MxOpenSslAndroidTarget([string]$Abi) {
    switch ($Abi) {
        'arm64-v8a' { return 'android-arm64' }
        'armeabi-v7a' { return 'android-arm' }
        'x86_64' { return 'android-x86_64' }
        'x86' { return 'android-x86' }
        default { throw "不支持的 Android ABI: $Abi" }
    }
}

# 找一个能用的 perl。Windows 上现成的是 Git for Windows 自带的那份。
function Resolve-MxPerlTool {
    $candidates = @()
    foreach ($name in @('perl')) {
        $command = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue
        if ($command) { $candidates += $command.Source }
    }
    # Git for Windows 的 perl 不在 PATH 上也能找到 (它随 Git 一起装, 不单独装)
    $gitCommand = Get-Command git -CommandType Application -ErrorAction SilentlyContinue
    if ($gitCommand) {
        # Git 的 cmd 目录: <Git>/cmd → <Git>/usr/bin/perl.exe
        $gitRoot = Split-Path -Parent (Split-Path -Parent $gitCommand.Source)
        $candidates += (Join-Path $gitRoot 'usr/bin/perl.exe')
    }
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate) { return (Resolve-Path $candidate).Path }
    }
    return ''
}

# 把 Windows 路径转成 msys / Cygwin 写法 (D:\a\b → /d/a/b)
#
# 为什么要转: msys 的 perl 启动时会把 PATH 里的 Windows 路径换成 /d/... 写法, 但**环境变量**
# (例如 ANDROID_NDK_ROOT) 会原样留着。OpenSSL 的 Configurations/15-android.conf 用
# `which("clang") =~ m|^$ndk/.*/prebuilt/...|` 判断 NDK 布局 —— 一边是 /d/...、一边是
# D:/..., 匹配失败它就退回去找早已从 NDK 里移除的 gcc, 报
# "no NDK aarch64-linux-android-gcc on $PATH"。把 NDK 根也转成 msys 写法就好了。
function ConvertTo-MxMsysPath([string]$Path) {
    if (-not $Path) { return $Path }
    if ($Path -match '^([A-Za-z]):[\\/]?(.*)$') {
        $drive = $Matches[1].ToLowerInvariant()
        $rest = $Matches[2] -replace '\\', '/'
        if ($rest) { return "/$drive/$rest" }
        return "/$drive"
    }
    return ($Path -replace '\\', '/')
}

# 这个 perl 是不是 msys / Cygwin 构建 (只有它们需要 msys 写法的路径)
function Test-MxPerlNeedsMsysPaths([string]$PerlPath) {
    if (-not $PerlPath) { return $false }
    try {
        $osname = (& $PerlPath -e 'print $^O' 2>$null | Out-String).Trim()
        return ($osname -eq 'msys' -or $osname -eq 'cygwin')
    }
    catch { return $false }
}

# 找一份能在 Windows 上跑 GNU makefile 的 make
#   OpenSSL 的 Configure 生成的是 GNU makefile (用到 `if [ -r ... ]` 这类 sh 语法),
#   所以必须是真的 GNU make, 且能找到一个 sh —— Git for Windows 的 sh.exe 就够。
function Resolve-MxMakeTool {
    foreach ($name in @('make', 'gmake', 'mingw32-make')) {
        $command = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue
        if ($command) { return $command.Source }
    }
    return ''
}

# 找一个 sh (GNU make 要用它执行配方里的 rm / chmod 之类)
function Resolve-MxShTool {
    foreach ($name in @('sh', 'bash')) {
        $command = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue
        if ($command) { return $command.Source }
    }
    $gitCommand = Get-Command git -CommandType Application -ErrorAction SilentlyContinue
    if ($gitCommand) {
        $gitRoot = Split-Path -Parent (Split-Path -Parent $gitCommand.Source)
        foreach ($relative in @('usr/bin/sh.exe', 'bin/sh.exe', 'bin/bash.exe')) {
            $candidate = Join-Path $gitRoot $relative
            if (Test-Path $candidate) { return (Resolve-Path $candidate).Path }
        }
    }
    return ''
}

# 下载并解包一份 make.exe 到 .native/tools/make (只用于 Windows 上编 Android 的 OpenSSL)
function Install-MxMakeTool {
    param([Parameter(Mandatory)][string]$NativeRoot)

    $toolRoot = Join-Path $NativeRoot 'tools/make'
    $exe = Get-ChildItem $toolRoot -Recurse -File -Filter 'make.exe' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($exe) { return $exe.FullName }

    Write-MxInfo 'OpenSSL: 没有找到 make, 下载一份 Windows 版 GNU make (ezwinports)'
    $downloadRoot = Join-Path $NativeRoot 'downloads'
    New-Item -ItemType Directory -Force -Path $downloadRoot | Out-Null
    $archive = Join-Path $downloadRoot 'make-4.4.1-without-guile-w32-bin.zip'
    if (-not (Test-Path $archive)) {
        & curl.exe -L --fail --retry 5 --retry-delay 3 --connect-timeout 30 -o $archive $script:MxMakeUrl | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw ("下载 make 失败 (curl 退出码 $LASTEXITCODE)。`n" +
                '  可以自己装一份 make (例如 scoop install make) 后重试。')
        }
    }
    if (Test-Path $toolRoot) { Remove-Item $toolRoot -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $toolRoot | Out-Null
    Expand-Archive -Path $archive -DestinationPath $toolRoot -Force
    $exe = Get-ChildItem $toolRoot -Recurse -File -Filter 'make.exe' -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $exe) { throw "make 压缩包里没有 make.exe: $archive" }
    return $exe.FullName
}

# 把 OpenSSL 源码准备到 .native/downloads/openssl-<version> (已存在就直接用)
function Initialize-MxOpenSslSource {
    param(
        [Parameter(Mandatory)][string]$NativeRoot,
        [string]$Version = ''
    )

    if (-not $Version) { $Version = $script:MxOpenSslSourceVersion }
    $downloadRoot = Join-Path $NativeRoot 'downloads'
    $sourceDir = Join-Path $downloadRoot ('openssl-' + $Version)
    if (Test-Path (Join-Path $sourceDir 'Configure')) { return $sourceDir }

    New-Item -ItemType Directory -Force -Path $downloadRoot | Out-Null
    $archive = Join-Path $downloadRoot ('openssl-' + $Version + '.tar.gz')
    if (-not (Test-Path $archive)) {
        $downloaded = $false
        foreach ($template in $script:MxOpenSslSourceUrls) {
            $url = $template.Replace('@VERSION@', $Version)
            Write-MxInfo "OpenSSL: 下载源码 $url"
            & curl.exe -L --fail --retry 5 --retry-delay 3 --connect-timeout 30 -C - -o $archive $url | Out-Null
            if ($LASTEXITCODE -eq 0) { $downloaded = $true; break }
            Write-MxWarn "下载失败, 换下一个地址 (curl 退出码 $LASTEXITCODE)"
        }
        if (-not $downloaded) {
            # 删掉半截文件: 留着会让下一次启动以为源码已经下好了, 而去解一个坏包
            Remove-Item $archive -Force -ErrorAction SilentlyContinue
            throw ("下载 OpenSSL 源码失败。`n" +
                "  可以手工下载 openssl-$Version.tar.gz 放到 $downloadRoot 后重试。")
        }
    }
    Write-MxInfo "OpenSSL: 解包源码 $archive"
    if (Test-Path $sourceDir) { Remove-Item $sourceDir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $sourceDir | Out-Null
    # tar 在 Windows 10+ 自带 (bsdtar), 直接认 .tar.gz
    & tar.exe -xf $archive -C $sourceDir --strip-components=1 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $sourceDir 'Configure'))) {
        throw "解包 OpenSSL 源码失败: $archive"
    }
    return $sourceDir
}

# 用本机 NDK 编一个 Android ABI 的 OpenSSL 静态库, 产物装到 $InstallDir
#   成功返回安装树路径; 缺少必要工具时返回空串 (由调用方统一报错)。
#
# 注意: 这个函数只能返回**一个字符串**。外部命令 (configure / make / curl / tar) 的输出必须
# 全部导进日志文件 —— 一旦落到管道上, 它们会和返回值拼在一起, 调用方拿到一串日志而不是路径。
function Build-MxOpenSslForAndroid {
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string]$Abi,
        [Parameter(Mandatory)][string]$InstallDir,
        [Parameter(Mandatory)][string]$AndroidNdk,
        [string]$AndroidApi = '24',
        [int]$Jobs = 0
    )

    if (-not $AndroidNdk -or -not (Test-Path $AndroidNdk)) {
        Write-MxWarn 'OpenSSL: 没有 Android NDK, 无法编 Android 静态库'
        return ''
    }
    $perl = Resolve-MxPerlTool
    if (-not $perl) {
        Write-MxWarn ('OpenSSL: 没有找到 perl。Windows 上只要装了 Git for Windows 就有 ' +
            '(它在 Git 的 usr/bin 下); 也可以自己装一份 perl 后重试')
        return ''
    }
    $sh = Resolve-MxShTool
    if (-not $sh) {
        Write-MxWarn ('OpenSSL: 没有找到 sh。GNU make 要用它执行配方里的 rm / chmod; ' +
            '装了 Git for Windows 就有, 也可以自己装一份 bash 后重试')
        return ''
    }
    $make = Resolve-MxMakeTool
    if (-not $make) {
        try { $make = Install-MxMakeTool -NativeRoot (Join-Path $Root '.native') }
        catch { Write-MxWarn "OpenSSL: $($_.Exception.Message)"; return '' }
    }

    $nativeRoot = Join-Path $Root '.native'
    $sourceDir = $null
    try { $sourceDir = Initialize-MxOpenSslSource -NativeRoot $nativeRoot }
    catch { Write-MxWarn "OpenSSL: $($_.Exception.Message)"; return '' }

    # perl 兼容模块: 拷进源码树, 用相对 PERL5LIB 传进去
    #   相对路径是必须的: msys perl 会把 "C:\dir" 里的冒号当成列表分隔符。
    $shimSource = Join-Path $PSScriptRoot 'openssl_perl_shim'
    $shimName = 'musicxx_perl_shim'
    $shimTarget = Join-Path $sourceDir $shimName
    if (Test-Path $shimSource) {
        if (Test-Path $shimTarget) { Remove-Item $shimTarget -Recurse -Force }
        Copy-Item $shimSource $shimTarget -Recurse -Force
    }

    $ndkBin = Get-ChildItem (Join-Path $AndroidNdk 'toolchains/llvm/prebuilt') -Directory -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $ndkBin) {
        Write-MxWarn "OpenSSL: NDK 里没有 llvm 预编译工具链: $AndroidNdk"
        return ''
    }
    $ndkBinPath = Join-Path $ndkBin.FullName 'bin'

    $configureTarget = Get-MxOpenSslAndroidTarget $Abi
    if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

    Write-MxInfo "OpenSSL: 用 NDK 编译 $configureTarget (API $AndroidApi) → $InstallDir"

    # 子进程的环境: NDK 工具链在 PATH 最前, plus perl shim 与 NDK 根
    #   msys / Cygwin 的 perl 需要 NDK 根写成 msys 写法, 否则 Configure 认不出 NDK 布局
    $ndkRootForPerl = $AndroidNdk
    if (Test-MxPerlNeedsMsysPaths $perl) {
        $ndkRootForPerl = ConvertTo-MxMsysPath $AndroidNdk
    }
    $savedPath = $env:PATH
    $savedNdkRoot = $env:ANDROID_NDK_ROOT
    $savedPerl5Lib = $env:PERL5LIB
    try {
        $env:PATH = @($ndkBinPath, (Split-Path -Parent $perl), (Split-Path -Parent $sh), (Split-Path -Parent $make)) +
            @($savedPath) -join ';'
        $env:ANDROID_NDK_ROOT = $ndkRootForPerl
        $env:PERL5LIB = $shimName

        Push-Location $sourceDir
        try {
            # Configure / make 的输出很多 (每次都是几千行): 写进日志文件, 失败时告诉用户去哪里看。
            # **不能** 落到管道上: 那样它们会和本函数的返回值 (安装树路径) 拼在一起。
            $logRoot = Join-Path (Join-Path $Root '.native') 'log'
            New-Item -ItemType Directory -Force -Path $logRoot | Out-Null
            $logFile = Join-Path $logRoot ('openssl-' + $Abi + '.log')
            Remove-Item $logFile -Force -ErrorAction SilentlyContinue

            # 重配前清掉上次的产物 (同一棵源码树也能换 ABI 重编)
            & $make 'clean' *>&1 | Out-Null

            # --prefix 会写进 Makefile 并被 sh 使用, 跟 NDK 根一样要转成 msys 写法
            $prefixForPerl = $InstallDir
            if (Test-MxPerlNeedsMsysPaths $perl) {
                $prefixForPerl = ConvertTo-MxMsysPath $InstallDir
            }
            $configureArgs = @(
                $configureTarget,
                "-D__ANDROID_API__=$AndroidApi",
                'no-shared', 'no-apps', 'no-docs', 'no-tests', 'no-ocsp',
                "--prefix=$prefixForPerl", "--openssldir=$prefixForPerl"
            )
            # Configure 本身也是 perl 脚本: 用显式 perl 调用, 不依赖 PATH 上的 perl 名字
            & $perl './Configure' @configureArgs *>&1 | Tee-Object -FilePath $logFile -Append | Out-Null
            if ($LASTEXITCODE -ne 0) {
                Write-MxWarn "OpenSSL: Configure 失败 (退出码 $LASTEXITCODE); 日志: $logFile"
                return ''
            }

            & $make "-j$Jobs" 'build_libs' *>&1 | Tee-Object -FilePath $logFile -Append | Out-Null
            if ($LASTEXITCODE -ne 0) {
                Write-MxWarn "OpenSSL: 编译失败 (退出码 $LASTEXITCODE); 日志: $logFile"
                return ''
            }
            & $make 'install_dev' *>&1 | Tee-Object -FilePath $logFile -Append | Out-Null
            if ($LASTEXITCODE -ne 0) {
                Write-MxWarn "OpenSSL: 安装失败 (退出码 $LASTEXITCODE); 日志: $logFile"
                return ''
            }
        }
        finally { Pop-Location }
    }
    finally {
        $env:PATH = $savedPath
        $env:ANDROID_NDK_ROOT = $savedNdkRoot
        $env:PERL5LIB = $savedPerl5Lib
    }

    if (-not (Test-MxOpenSslRoot $InstallDir ('android-' + $Abi))) {
        Write-MxWarn "OpenSSL: 编译完成但产物不完整: $InstallDir"
        return ''
    }
    return (Resolve-Path $InstallDir).Path
}

# OpenSSL 安装树解析顺序:
#   显式参数 → 环境变量 → .native/openssl/<目标标签> → resource/libs/<目标标签>/openssl
#   -TargetTag: 目标平台标签 (windows-x64 / android-arm64-v8a / ...), 决定缓存目录与"什么算静态库"。
#   交叉编译时必须用目标标签 —— 否则会把 windows-x64 那棵树的 .lib 传给非 Windows 平台。
#   拿不到时一律报错 (除了 -Check): TLS 是必备能力, 不存在"这次构建不带"的情况。
function Resolve-MxOpenSslRoot {
    param(
        [Parameter(Mandatory)][string]$Root,
        [string]$OpenSslRoot,
        [string]$TargetTag = '',
        [switch]$NoDownloadOpenSsl,
        [switch]$Check,
        [string]$AndroidNdk = '',
        [string]$AndroidApi = '24',
        [int]$Jobs = 0
    )

    if (-not $TargetTag) { $TargetTag = 'windows-x64' }
    $targetKind = Get-MxOpenSslTargetKind $TargetTag
    # 本机能不能为这个目标准备安装树: Windows 目标用 slproweb 预编译包,
    # Android 目标用本机 NDK 从源码编
    $canPrepareHere = ($targetKind -eq 'windows' -or $targetKind -eq 'android')

    $nativeRoot = Join-Path $Root '.native'
    $cached = Join-Path $nativeRoot ('openssl/' + $TargetTag)
    # 手工放进仓库、随包分发的安装树。两种目录名都认, 两种层级也都认:
    #   resource/libs/android/arm64-v8a/        (直接摆 lib/ 与 include/, 按平台分层)
    #   resource/libs/android/arm64-v8a/openssl
    #   resource/libs/android-arm64-v8a|...     (平台与变体写一起)
    $packagedDirs = @()
    $pkgBases = @()
    if ($TargetTag -match '^([a-z]+)-(.+)$') {
        $pkgBases += Join-Path $Root ("resource/libs/{0}/{1}" -f $Matches[1], $Matches[2])
    }
    $pkgBases += Join-Path $Root ("resource/libs/{0}" -f $TargetTag)
    foreach ($pkgBase in $pkgBases) {
        $packagedDirs += $pkgBase
        $packagedDirs += (Join-Path $pkgBase 'openssl')
    }

    if ($OpenSslRoot) {
        if (-not (Test-MxOpenSslRoot $OpenSslRoot $TargetTag)) {
            $detail = if ($targetKind -eq 'windows') { 'include/openssl/ssl.h 与静态库 (.lib)' }
            else { 'include/openssl/ssl.h 与静态库 (libssl.a)' }
            throw "-OpenSslRoot 下没有 $detail : $OpenSslRoot (目标 $TargetTag)"
        }
        $resolved = (Resolve-Path $OpenSslRoot).Path
        return [ordered]@{ Root = $resolved; Version = (Get-MxOpenSslVersionText $resolved); Source = 'explicit' }
    }
    if ($env:MUSICXX_EXTERN_PLUGIN_OPENSSL_ROOT) {
        $fromEnv = $env:MUSICXX_EXTERN_PLUGIN_OPENSSL_ROOT
        if (Test-MxOpenSslRoot $fromEnv $TargetTag) {
            $resolved = (Resolve-Path $fromEnv).Path
            return [ordered]@{ Root = $resolved; Version = (Get-MxOpenSslVersionText $resolved); Source = 'env' }
        }
        Write-MxWarn "环境变量 MUSICXX_EXTERN_PLUGIN_OPENSSL_ROOT 指向的目录不可用 (目标 $TargetTag): $fromEnv"
    }
    if (Test-MxOpenSslRoot $cached $TargetTag) {
        return [ordered]@{ Root = (Resolve-Path $cached).Path; Version = (Get-MxOpenSslVersionText $cached); Source = 'cache' }
    }
    foreach ($packaged in $packagedDirs) {
        if (Test-MxOpenSslRoot $packaged $TargetTag) {
            return [ordered]@{ Root = (Resolve-Path $packaged).Path; Version = (Get-MxOpenSslVersionText $packaged); Source = 'packaged' }
        }
    }
    if (-not $canPrepareHere) {
        # 其它交叉目标 (Linux / macOS / iOS) 在 Windows 上没法自动准备 —— 它们的工具链不在本机。
        # TLS 是必备能力, 所以直接报错, 给出放依赖的位置。
        if ($NoDownloadOpenSsl -or $Check) {
            return [ordered]@{ Root = ''; Version = ''; Source = '缺失' }
        }
        $candidateLines = ($packagedDirs | ForEach-Object { "    $_" }) -join [Environment]::NewLine
        throw ((@(
            "找不到目标 $TargetTag 的 OpenSSL 安装树 (客户端 TLS 是必备能力, 不能缺)。",
            '  请自行编译 OpenSSL 静态库 (no-shared) 后放到:',
            $candidateLines,
            '  目录里要有 include/openssl/ssl.h 与 lib/libssl.a (或 lib64/libssl.a)。',
            '  或用 -OpenSslRoot 指向已经编好的安装树。'
        ) -join [Environment]::NewLine))
    }
    if ($NoDownloadOpenSsl -or $Check) {
        if (-not $Check) {
            # 到头了: 手上没有可用的安装树, 而调用方不允许现场准备 (Auto 模式为应用构建调用,
            # 那里不能花半小时编 OpenSSL)
            $candidateLines = ($packagedDirs | ForEach-Object { "    $_" }) -join [Environment]::NewLine
            $hint = if ($targetKind -eq 'android' -and $canPrepareHere) {
                @('  这个脚本能自己编: 去掉 -NoDownloadOpenSsl / -Auto 手工跑一次构建,',
                    '  编好的安装树会留在 .native/openssl/ 里, 之后应用构建直接复用。')
            }
            else {
                @('  请自行编译 OpenSSL 静态库 (no-shared) 后放到上面任一目录。')
            }
            throw ((@(
                "找不到目标 $TargetTag 的 OpenSSL 安装树, 而且本次不允许现场准备。",
                '  可用位置:'
            ) + @($candidateLines) + $hint + @(
                '  也可以用 -OpenSslRoot 指向已经编好的安装树。'
            )) -join [Environment]::NewLine)
        }
        return [ordered]@{ Root = ''; Version = ''; Source = 'download(未准备)' }
    }

    if ($targetKind -eq 'android') {
        # Android: 用本机 NDK 从源码编到缓存目录。编不出来 (缺 NDK / perl / make) 就报错 ——
        # TLS 不可缺失, 不能静默降级成一份没有 TLS 的库。
        $abi = $TargetTag -replace '^android-', ''
        $built = ''
        try {
            $built = Build-MxOpenSslForAndroid -Root $Root -Abi $abi -InstallDir $cached `
                -AndroidNdk $AndroidNdk -AndroidApi $AndroidApi -Jobs $Jobs
        }
        catch { Write-MxWarn "OpenSSL: 自动编译失败: $($_.Exception.Message)" }
        if ($built -and (Test-MxOpenSslRoot $built $TargetTag)) {
            return [ordered]@{ Root = $built; Version = (Get-MxOpenSslVersionText $built); Source = 'build' }
        }
        $candidateLines = ($packagedDirs | ForEach-Object { "    $_" }) -join [Environment]::NewLine
        throw ((@(
            "找不到目标 $TargetTag 的 OpenSSL 安装树, 而且本机没能自动编译出来。",
            '自动编译需要 Android NDK (build/cmake/android.toolchain.cmake) + perl + make;',
            '少哪一个上面已经有提示 (Windows 上 perl / sh 随 Git for Windows 就有, make 会自动下载)。',
            '也可以自己编一份静态库后放到:',
            $candidateLines,
            '  目录里要有 include/openssl/ssl.h 与 lib/libssl.a。',
            '  或用 -OpenSslRoot 指向已经编好的安装树。'
        ) -join [Environment]::NewLine))
    }

    $meta = Get-MxOpenSslPackageMeta
    if (-not $meta) {
        Write-MxWarn 'OpenSSL: 取不到 slproweb 版本清单, 退回脚本里固定的版本'
        $meta = [ordered]@{
            Version = $script:MxOpenSslFallbackVersion
            Url     = $script:MxOpenSslFallbackUrl
            Sha256  = $script:MxOpenSslFallbackSha256
        }
    }
    Write-MxInfo "OpenSSL: $($meta.Version) (slproweb 预编译包, 含头文件与静态库)"
    $downloadRoot = Join-Path $nativeRoot 'downloads'
    New-Item -ItemType Directory -Force -Path $downloadRoot | Out-Null
    $packagePath = Join-Path $downloadRoot ('openssl-' + $meta.Version + '-win64' + [System.IO.Path]::GetExtension($meta.Url))
    $needDownload = $true
    if (Test-Path $packagePath) {
        if ($meta.Sha256) {
            $actual = (Get-FileHash -Algorithm SHA256 $packagePath).Hash.ToLower()
            $needDownload = ($actual -ne $meta.Sha256.ToLower())
            if ($needDownload) { Write-MxInfo 'OpenSSL: 已下载的包校验不符, 重新下载' }
        }
        else {
            $needDownload = $false
        }
    }
    if ($needDownload) {
        if (Test-Path $packagePath) { Remove-Item $packagePath -Force }
        # 这个包很大 (约 260 MB): 断点续传 + 多试几次, 网络中断时不必从 0 开始
        Write-MxInfo "OpenSSL: 下载 $($meta.Url)"
        & curl.exe -L --fail --retry 5 --retry-delay 3 --connect-timeout 30 -C - -o $packagePath $meta.Url
        if ($LASTEXITCODE -ne 0) {
            throw ("下载 OpenSSL 预编译包失败 (curl 退出码 $LASTEXITCODE): $($meta.Url)`n" +
                '  可以手工下载解包后用 -OpenSslRoot 指向解出来的目录')
        }
        if ($meta.Sha256) {
            $actual = (Get-FileHash -Algorithm SHA256 $packagePath).Hash.ToLower()
            if ($actual -ne $meta.Sha256.ToLower()) {
                throw "OpenSSL 预编译包校验失败: $packagePath`n  期望 $($meta.Sha256)`n  实际 $actual"
            }
            Write-MxInfo 'OpenSSL: 安装包 SHA256 校验通过'
        }
    }
    else {
        Write-MxInfo "OpenSSL: 复用已下载的安装包 $packagePath"
    }
    [void](Expand-MxOpenSslPackage -PackagePath $packagePath -DestDir $cached)
    return [ordered]@{ Root = (Resolve-Path $cached).Path; Version = (Get-MxOpenSslVersionText $cached); Source = 'download' }
}

# 给定目标平台与 ABI/架构, 给出安装树目录标签 (与 CMake 侧候选目录同名)
function Get-MxOpenSslTargetTag {
    param(
        [switch]$Android,
        [switch]$IOS,
        [switch]$MacOS,
        [string]$Abi = 'arm64-v8a',
        [string]$Arch = ''
    )

    if (-not $Arch) {
        $Arch = switch ([System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()) {
            'X64' { 'x64' }
            'Arm64' { 'arm64' }
            'X86' { 'x86' }
            default { 'x64' }
        }
    }
    if ($Android) { return 'android-' + $Abi }
    if ($IOS) { return 'ios-arm64' }   # Windows 侧不构建 iOS, 这个分支只为标签完整
    if ($MacOS) { return 'macos-' + $Arch }
    return 'windows-' + $Arch
}
