#
# musicxx_extern_plugin 依赖库: OpenSSL (客户端 TLS)
#
# 被 tools/setup_native_env.ps1 与 tools/build_native.ps1 点源引入(dot-source), 只放
# "OpenSSL 装在哪 / 没有就怎么弄到"这一件事, 构建流程本身不写在这里。
#
# 为什么要 OpenSSL: 插件网络能力的客户端要能连 https / wss (服务端仍然只有 http / ws,
# 插件自己监听时不做 TLS)。做法与 agentxx 一致: 静态链接, 不随包分发 DLL。
#   - Windows 上没有 perl / nasm, 无法从源码编译 OpenSSL, 因此用 slproweb 的预编译包
#     (https://slproweb.com/products/Win32OpenSSL.html): 从它的官方哈希清单里挑最新的 4.x
#     Win64 完整包, 下载后**解包**到 .native/openssl/windows-<arch>, 不执行安装程序。
#     完整包约 260 MB (头文件 + 全部静态库 + 工具); Light 包没有静态库, 不能用。
#   - Linux / macOS / Android / iOS 由 tools/build_native.sh 从源码编译到同一套目录。
#
# 目录约定: 解析结果是一棵 CMake 能直接用的安装树
#   Windows (slproweb 布局): include/openssl/ssl.h + lib/VC/x64/MD{,d}/libssl_static.lib
#   源码编译布局:            include/openssl/ssl.h + lib/libssl.a (或 lib64/libssl.a)
#
# 用法 (由调用方 dot-source 本文件后调用):
#   $openssl = Resolve-MxOpenSslRoot -Root <包目录> [-OpenSslRoot <目录>] [-NoDownloadOpenSsl]
#   if ($openssl.Root) { ... }   # Root 为空 = 这份构建不带 TLS (CMake 里关掉)

Set-StrictMode -Version Latest

# 版本固定为 4.x; 空 = 取 slproweb 清单里最新的 4.x (与 agentxx 的做法一致)。
# 需要别的版本时设环境变量 MUSICXX_EXTERN_PLUGIN_OPENSSL_VERSION (例: 4.0.3)。
$script:MxOpenSslHashUrl = 'https://slproweb.com/download/win32_openssl_hashes.json'
# 清单取不到时的兜底 (与清单里的 Win64 完整包一致; 升级时一起改)
$script:MxOpenSslFallbackVersion = '4.0.3'
$script:MxOpenSslFallbackUrl = 'https://slproweb.com/download/Win64OpenSSL-4_0_3.msi'
$script:MxOpenSslFallbackSha256 = '43f6895ace409b9bed115431221a6ed17d0754dcb2d8d8a149bafa20fa29c7ad'

function Test-MxOpenSslRoot([string]$Path) {
    if (-not $Path) { return $false }
    if (-not (Test-Path (Join-Path $Path 'include/openssl/ssl.h'))) { return $false }
    foreach ($relative in @('lib/VC/x64/MD/libssl_static.lib', 'lib/VC/x64/MDd/libssl_static.lib',
            'lib/libssl.a', 'lib64/libssl.a', 'lib/libssl.lib')) {
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
        if (-not (Test-MxOpenSslRoot $root)) {
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

# OpenSSL 安装树解析顺序:
#   显式参数 → 环境变量 MUSICXX_EXTERN_PLUGIN_OPENSSL_ROOT → .native/openssl/windows-<arch> → 下载并解包
# -AllowMissing: 拿不到就返回空 Root (调用方关掉 TLS 继续构建), 不抛异常 ——
#   应用构建(自动模式)用这个, 免得一个下载问题把整个应用构建打断。
function Resolve-MxOpenSslRoot {
    param(
        [Parameter(Mandatory)][string]$Root,
        [string]$OpenSslRoot,
        [switch]$NoDownloadOpenSsl,
        [switch]$AllowMissing,
        [switch]$Check
    )

    $nativeRoot = Join-Path $Root '.native'
    $arch = if ([Environment]::Is64BitProcess) { 'x64' } else { 'x86' }
    $cached = Join-Path $nativeRoot ('openssl/windows-' + $arch)

    if ($OpenSslRoot) {
        if (-not (Test-MxOpenSslRoot $OpenSslRoot)) {
            throw "-OpenSslRoot 下没有 include/openssl/ssl.h 与静态库: $OpenSslRoot"
        }
        $resolved = (Resolve-Path $OpenSslRoot).Path
        return [ordered]@{ Root = $resolved; Version = (Get-MxOpenSslVersionText $resolved); Source = 'explicit' }
    }
    if ($env:MUSICXX_EXTERN_PLUGIN_OPENSSL_ROOT) {
        $fromEnv = $env:MUSICXX_EXTERN_PLUGIN_OPENSSL_ROOT
        if (Test-MxOpenSslRoot $fromEnv) {
            $resolved = (Resolve-Path $fromEnv).Path
            return [ordered]@{ Root = $resolved; Version = (Get-MxOpenSslVersionText $resolved); Source = 'env' }
        }
        Write-MxWarn "环境变量 MUSICXX_EXTERN_PLUGIN_OPENSSL_ROOT 指向的目录不可用: $fromEnv"
    }
    if (Test-MxOpenSslRoot $cached) {
        return [ordered]@{ Root = (Resolve-Path $cached).Path; Version = (Get-MxOpenSslVersionText $cached); Source = 'cache' }
    }
    if ($NoDownloadOpenSsl -or $AllowMissing -or $Check) {
        if (-not $AllowMissing -and -not $Check) {
            throw ('未提供 OpenSSL: 请用 -OpenSslRoot <安装目录> 或设置环境变量 ' +
                'MUSICXX_EXTERN_PLUGIN_OPENSSL_ROOT, 或去掉 -NoDownloadOpenSsl 让脚本自动下载')
        }
        return [ordered]@{ Root = ''; Version = ''; Source = 'download(未下载)' }
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
