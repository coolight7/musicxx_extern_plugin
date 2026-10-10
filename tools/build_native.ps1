# musicxx_extern_plugin 插件框架构建脚本 (插件框架库 + 插件 SDK)
#
# 只构建"主程序要用、插件作者要链接"的那份东西: 插件框架库 musicxx_extern_plugin.<dll|so|dylib>
# 与插件 SDK (include/ + lib/cmake/)。示例插件不在这里构建 —— 它们由
# tools/build_examples.ps1 负责, 这样应用构建不必为示例插件花时间。
#
# 具体的依赖库编译与插件框架工程构建全部交给 src/CMakeLists.txt (superbuild + ExternalProject_Add):
#
#   src/CMakeLists.txt       顶层 superbuild: 推导 XX_IS_* 宏 → 生成 BoostConfig.cmake →
#                            ExternalProject_Add 逐个构建 third_party 依赖 → 构建插件框架工程
#   src/host/CMakeLists.txt  嵌套插件框架工程: find_package 取依赖 → 插件框架库 / 插件 SDK / 原生测试
#
# 本脚本负责的只有 cmake 不方便做的事:
#   - 先确保开发编译环境就绪 (与 tools/setup_native_env.ps1 同一份实现: 复用
#     .native/env/environment.json 缓存, 已配置过就不重新探测工具链、不重新解析 Boost)
#   - 按 -Config 传 XX_IS_RELEASE_D / CMAKE_BUILD_TYPE / CMAKE_CONFIGURATION_TYPES / 平台
#   - 构建完成后把产物复制到稳定的输出目录, 可选跑原生测试
#
# 用法:
#   pwsh -NoProfile -File tools/build_native.ps1                    # Release 插件框架库 + SDK
#   pwsh -NoProfile -File tools/build_native.ps1 -Config Debug      # Debug (独立构建目录, 依赖也重编)
#   pwsh -NoProfile -File tools/build_native.ps1 -WithExamples      # 连示例插件一起构建
#   pwsh -NoProfile -File tools/build_native.ps1 -WithTests         # 连原生测试一起构建(含对照示例)
#   pwsh -NoProfile -File tools/build_native.ps1 -RunTests          # 构建后跑原生测试(隐含示例与测试)
#   pwsh -NoProfile -File tools/build_native.ps1 -Auto              # 自动模式(应用构建调用): 已是最新则
#                                                                   # 秒退, 失败不阻断应用构建只给警告
#   pwsh -NoProfile -File tools/build_native.ps1 -Auto -ConsoleEncoding system   # 构建系统当子进程调用时的输出编码
#   pwsh -NoProfile -File tools/build_native.ps1 -SkipIfFresh       # 已是最新就跳过(失败仍然报错)
#   pwsh -NoProfile -File tools/build_native.ps1 -Force             # 不跳过, 强制走一遍构建
#   pwsh -NoProfile -File tools/build_native.ps1 -DepsOnly          # 只构建依赖库
#   pwsh -NoProfile -File tools/build_native.ps1 -ConfigureOnly     # 只做预检/Boost/configure
#   pwsh -NoProfile -File tools/build_native.ps1 -Clean             # 先清空构建目录
#   pwsh -NoProfile -File tools/build_native.ps1 -Jobs 8            # 指定并行度
#   pwsh -NoProfile -File tools/build_native.ps1 -NoEnvSetup        # 跳过环境解析(只用已有缓存)
#   pwsh -NoProfile -File tools/build_native.ps1 -Android -Abi arm64-v8a   # 用 NDK 交叉编译 Android 插件框架库
#
# Android 说明:
#   - 产出与桌面端同一套布局: <包>/.native/output/android-<abi>-<配置>/bin/libmusicxx_extern_plugin.so
#     (Android 的 ABI 名直接用作目录里的架构段: arm64-v8a / armeabi-v7a / x86_64 / x86)
#   - 插件框架库不是靠本脚本进 APK 的: 包的 android/build.gradle 会把它按 ABI 收集到 jniLibs,
#     随 Flutter 的 Android 构建打进去 (见 android/README.md)
#   - NDK 位置优先取 -AndroidNdk, 其次 $env:ANDROID_NDK_HOME / $env:ANDROID_NDK_ROOT,
#     最后在 $env:ANDROID_HOME|$env:ANDROID_SDK_ROOT\ndk 下挑版本号最大的一个
#   - 统一用 Ninja 生成器与 c++_static (C++ 运行库静态链进插件框架库, APK 不需要额外带 libc++_shared.so)
#   - -RunTests 在 Android 上不执行: 测试可执行文件是给设备编的, 开发机上跑不起来 (需要 adb 手工跑)
#
# 说明: [CmdletBinding()] 让写错的参数名直接报错, 而不是被当成多余参数悄悄忽略。
[CmdletBinding()]
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot),
    [ValidateSet('Release', 'Debug')][string]$Config = 'Release',
    [switch]$DepsOnly,
    [switch]$ConfigureOnly,
    [switch]$Clean,
    [switch]$RunTests,
    [switch]$WithExamples,
    [switch]$WithTests,
    [switch]$Auto,
    [switch]$Force,
    [switch]$SkipIfFresh,
    [switch]$NoEnvSetup,
    # 被构建系统当子进程调起来时用 system: CMake / Gradle 按系统代码页解码子进程输出
    # (Flutter 的构建日志里中文才不会变成乱码); 直接用 pwsh 跑时保持默认的 utf8
    [ValidateSet('utf8', 'system')][string]$ConsoleEncoding = 'utf8',
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
. (Join-Path $PSScriptRoot 'native_common.ps1')

$Root = (Resolve-Path $Root).Path
Initialize-MxBuildEnvironment -ConsoleEncoding $ConsoleEncoding

# ==================== 目录约定 ====================

$paths = Get-MxBuildPaths -Root $Root -Config $Config -Android:$Android -Abi $Abi
# 显式指定的目录优先级最高 (CI/自定义目录); 派生路径跟着一起改
if ($BuildDir) {
    $paths.BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
    $paths.InstallDir = [System.IO.Path]::GetFullPath((Join-Path $paths.BuildDir 'musicxx-extern-plugin-install'))
}
if ($OutputDir) {
    $paths.OutputDir = [System.IO.Path]::GetFullPath($OutputDir)
    $paths.HostLibrary = [System.IO.Path]::GetFullPath((Join-Path $paths.OutputDir "bin/$($paths.LibraryName)"))
    $paths.PluginsDir = [System.IO.Path]::GetFullPath((Join-Path $paths.OutputDir 'plugins'))
}

# 构建哪些部分: 框架默认只出插件框架库与 SDK; -RunTests 需要测试可执行文件与对照示例插件
$buildExamples = [bool]($WithExamples -or $RunTests)
$buildTests = [bool]($WithTests -or $RunTests)
if ($DepsOnly -and $RunTests) { throw '-RunTests 不能与 -DepsOnly 同时使用' }

# 本次构建覆盖的内容对应的构建标记 (framework / examples / tests): 构建成功后写,
# 自动模式按它们判断"已是最新"。放在这里算: 自动模式与普通构建都要用同一个列表。
$stampKinds = @('framework')
if ($buildExamples) { $stampKinds += 'examples' }
if ($buildTests) { $stampKinds += 'tests' }

Write-Host '=== musicxx_extern_plugin 插件框架构建 ==='
if ($Auto) {
    Write-MxInfo ("自动模式: {0} / {1} → {2}" -f
        $(if ($Android) { "android-$($paths.Abi)" } else { "$($paths.Os)-$($paths.Arch)" }),
        $paths.Config, $paths.OutputDir)
}
else {
    Write-MxInfo "源目录:     $Root"
    Write-MxInfo "构建目录:   $($paths.BuildDir)"
    Write-MxInfo "安装目录:   $($paths.InstallDir)"
    Write-MxInfo "输出目录:   $($paths.OutputDir)"
    $extra = ''
    if ($buildExamples) { $extra += ' + 示例插件' }
    if ($buildTests) { $extra += ' + 原生测试' }
    Write-MxInfo ("配置:       {0}   目标: {1}   内容: 插件框架库 + 插件 SDK{2}" -f $paths.Config,
        $(if ($Android) { "android-$($paths.Abi)" } else { "$($paths.Os)-$($paths.Arch)" }), $extra)
}

# ==================== 环境 (复用 .native/env/environment.json 缓存) ====================

# 自动模式(应用构建调用)先只看缓存: 环境已配置过就不必重新解析 —— "已是最新"的情况下整段逻辑
# 只花一次脚本启动的开销 (这正是"应用构建不再要求先手工跑构建脚本"的前提)。
$environment = $null
if ($NoEnvSetup -or $Auto) {
    $environment = Get-MxEnvironmentCached -Root $Root -Android:$Android `
        -AndroidPlatform $AndroidPlatform -AndroidStl $AndroidStl
    if (-not $environment -and $NoEnvSetup) {
        throw '环境未配置(或已失效)且指定了 -NoEnvSetup; 请先执行 tools/setup_native_env.ps1'
    }
}

if (-not $environment) {
    # 正常模式(或自动模式下缓存不可用): 解析一遍, 缺 Boost 时该下载就下载
    $environment = Invoke-MxEnsureEnvironment -Root $Root -BoostRoot $BoostRoot -BoostArchive $BoostArchive `
        -NoDownloadBoost:$NoDownloadBoost -Android:$Android -AndroidNdk $AndroidNdk `
        -AndroidPlatform $AndroidPlatform -AndroidStl $AndroidStl -Generator $Generator -Quiet:$Auto
}
elseif (-not $Auto) {
    Write-MxInfo "环境:       复用缓存 $($environment.File)"
}

$generator = $environment.Generator
$isMultiConfig = [bool]$environment.IsMultiConfig
$boostHeaderRoot = $environment.Boost.Root
$androidToolchain = $environment.Android.Toolchain
$generatorArgs = @('-G', $generator)
if ($isMultiConfig) { $generatorArgs += @('-A', $paths.GeneratorPlatform) }

if (-not $Auto) {
    Write-MxInfo "生成器:     $generator$(if ($isMultiConfig) { " ($($paths.GeneratorPlatform))" })"
    Write-MxInfo "Boost:      $boostHeaderRoot (版本 $($environment.Boost.Version), 来源 $($environment.Boost.Source))"
    if ($Android) {
        Write-MxInfo "目标:       android-$($paths.Abi) ($AndroidPlatform, $AndroidStl)"
        Write-MxInfo "工具链:     $androidToolchain"
    }
}

# 构建目录签名: 配置/生成器/平台/Boost 位置变化时重置目录, 避免 CMakeCache 里残留上一次的参数
# (例如旧的 CMAKE_CONFIGURATION_TYPES) 让新配置报出难以定位的错。
$signature = Get-MxBuildSignature -Paths $paths -Generator $generator -BoostHeaderRoot $boostHeaderRoot `
    -AndroidToolchain $androidToolchain -AndroidPlatform $(if ($Android) { $AndroidPlatform } else { '' }) `
    -AndroidStl $(if ($Android) { $AndroidStl } else { '' })

# ==================== 已是最新: 直接返回 ====================

# -Auto (应用构建调用) 与 -SkipIfFresh (示例插件脚本等) 都按构建标记判断"要不要真构建";
# 两者的区别只在失败处理: -Auto 失败不阻断调用方, -SkipIfFresh 失败照常报错。
if (($Auto -or $SkipIfFresh) -and -not $Force -and -not $Clean -and -not $ConfigureOnly `
        -and -not $DepsOnly -and -not $RunTests) {
    # 本次要构建的东西都覆盖过、且都发生在源码最后一次改动之后 → 直接跳过
    $required = @($paths.HostLibrary)
    if ($buildExamples) { $required += $paths.PluginsDir }
    if ($buildTests) { $required += (Join-Path $paths.OutputDir "bin/$($paths.TestName)") }
    $stamps = @($stampKinds | ForEach-Object { Get-MxBuildStampPath -BuildDir $paths.BuildDir -Kind $_ })
    if (Test-MxBuildFresh -Root $Root -BuildDir $paths.BuildDir -Signature $signature `
            -Stamps $stamps -RequiredPaths $required) {
        Write-MxOk "已是最新, 跳过构建: $($paths.HostLibrary)"
        exit 0
    }
}

# ==================== 构建 ====================
#
# 下面整段是构建主体: 自动模式下它被包在 try/catch 里 —— 插件框架构建失败不该让应用构建失败
# (插件框架库缺失时主程序照常启动, 只是没有外部插件), 但要在构建日志里给出明确的警告与下一步。
$buildBody = {
    if ($Clean) {
        Write-MxStep '清理构建目录'
        foreach ($p in @($paths.BuildDir, $paths.OutputDir)) {
            if (Test-Path $p) {
                Write-MxInfo "删除 $p"
                Remove-Item $p -Recurse -Force
            }
        }
    }

    # ==================== configure (superbuild: 依赖 + 插件框架) ====================

    Write-MxStep 'configure (superbuild: 依赖 + 插件框架)'
    $marker = Join-Path $paths.BuildDir '.build_signature'
    if (Test-Path $paths.BuildDir) {
        $old = if (Test-Path $marker) { (Get-Content -Raw $marker).Trim() } else { '' }
        if ($old -ne $signature) {
            Write-MxInfo "构建目录的配置已变化, 重置: $($paths.BuildDir)"
            Remove-Item $paths.BuildDir -Recurse -Force
        }
    }
    New-Item -ItemType Directory -Force -Path $paths.BuildDir | Out-Null
    Set-Content -Path $marker -Value $signature -Encoding UTF8

    $xxIsRelease = if ($Config -eq 'Debug') { '0' } else { '1' }
    $configureArgs = @(
        "-DXX_IS_RELEASE_D=$xxIsRelease"
        "-DCMAKE_BUILD_TYPE=$Config"
        # 收窄可用配置到本次配置 (与 agentxx 的 windows_*_build.bat 一致): 一个构建目录只构建一个
        # 配置, 依赖安装前缀在构建目录内, 因此不会混入其它配置的产物
        "-DCMAKE_CONFIGURATION_TYPES=$Config"
        # Android: 目标处理器由 NDK 工具链按 ANDROID_ABI 推导, 显式传值会造成两边不一致
        if (-not $Android) { "-DCMAKE_SYSTEM_PROCESSOR=$($paths.SystemProcessor)" }
        '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'
        "-DMUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE=$boostHeaderRoot"
        # 示例插件与原生测试: 默认关 (示例插件交给 tools/build_examples.ps1)
        "-DMUSICXX_EXTERN_PLUGIN_BUILD_EXAMPLE=$(if ($buildExamples) { '1' } else { '0' })"
        "-DMUSICXX_EXTERN_PLUGIN_BUILD_TESTS=$(if ($buildTests) { '1' } else { '0' })"
    )
    if ($Android) {
        # 交叉编译参数交给 NDK 工具链 (superbuild 的 src/CMakeLists.txt 会自动补 XX_IS_ANDROID_D=1,
        # 并把 ANDROID_ABI/ANDROID_PLATFORM/ANDROID_STL 原样传给各依赖与插件框架工程的嵌套构建)
        $configureArgs += @(
            "-DCMAKE_TOOLCHAIN_FILE=$androidToolchain"
            "-DANDROID_ABI=$($paths.Abi)"
            "-DANDROID_PLATFORM=$AndroidPlatform"
            "-DANDROID_STL=$AndroidStl"
        )
    }
    if ($DepsOnly) { $configureArgs += '-DMUSICXX_EXTERN_PLUGIN_BUILD_HOST=OFF' }

    & cmake @configureArgs @generatorArgs -B $paths.BuildDir -S (Join-Path $Root 'src') | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'cmake configure 失败' }

    if ($ConfigureOnly) { return }

    # ==================== build ====================

    $parallel = if ($Jobs -gt 0) { "$Jobs" } else { "$([Environment]::ProcessorCount)" }
    $buildArgs = @('--build', $paths.BuildDir, '--config', $Config, '--parallel', $parallel)
    # 只构建依赖: 直接以 cxx_pluginxx_repo 为目标 (host 工程不在依赖链上)
    if ($DepsOnly) { $buildArgs += @('--target', 'cxx_pluginxx_repo') }

    Write-MxStep "build (--config $Config --parallel $parallel)"
    & cmake @buildArgs | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'cmake build 失败' }

    # 顶层 superbuild 自身没有安装规则 (安装都由各 ExternalProject 完成); 这里保持一致行为
    & cmake --install $paths.BuildDir --config $Config | Out-Host
    if ($LASTEXITCODE -ne 0) { throw 'cmake install 失败' }

    # 插件框架工程是嵌套 ExternalProject 构建: 生成器有时会判定上层的"构建/安装步骤已是最新"而跳过它,
    # 于是源码改动不会真正触发重编译 (表现为"构建成功但产物时间戳/体积不变")。这里显式再跑一次
    # 嵌套工程的增量构建与安装 (已经是新的就几秒钟结束), 保证脚本的"构建成功"与产物一致。
    if (-not $DepsOnly) {
        $hostBuildDir = Join-Path (Join-Path $paths.BuildDir 'e') 'host'
        if (Test-Path (Join-Path $hostBuildDir 'CMakeCache.txt')) {
            Write-MxStep 'build 插件框架工程 (嵌套工程增量构建)'
            & cmake --build $hostBuildDir --config $Config --parallel $parallel | Out-Host
            if ($LASTEXITCODE -ne 0) { throw 'cmake build (插件框架工程) 失败' }
            & cmake --install $hostBuildDir --config $Config | Out-Host
            if ($LASTEXITCODE -ne 0) { throw 'cmake install (插件框架工程) 失败' }
        }
    }

    # ==================== 产物 ====================

    Write-MxStep '产物'
    $artifacts = @()
    if (Test-Path (Join-Path $paths.InstallDir 'bin')) {
        $artifacts += Get-ChildItem (Join-Path $paths.InstallDir 'bin') -File |
            Where-Object { $_.Extension -in @('.dll', '.so', '.dylib', '.exe') }
    }
    if (Test-Path (Join-Path $paths.InstallDir 'plugins')) {
        $artifacts += Get-ChildItem (Join-Path $paths.InstallDir 'plugins') -Recurse -File |
            Where-Object { $_.Extension -in @('.dll', '.so', '.dylib') }
    }
    foreach ($f in $artifacts) {
        Write-MxInfo ("{0,-38} {1,10:N0} 字节" -f $f.Name, $f.Length)
    }
    if ($artifacts.Count -eq 0) { Write-MxInfo '(bin/plugins 下没有产物)' }

    # 复制到稳定输出目录 (主程序侧与打包脚本都按这个目录找库)
    if (-not $DepsOnly) {
        Write-MxInfo "复制产物到输出目录: $($paths.OutputDir)"
        Copy-MxBuildArtifacts -InstallDir $paths.InstallDir -OutputDir $paths.OutputDir
    }

    # ==================== 原生测试 (可选) ====================

    if ($RunTests) {
        Write-MxStep '原生测试'
        if ($Android) {
            # 测试可执行文件是给设备编的 (目标 ABI 与开发机 Windows 不同), 开发机上跑不起来。
            # 需要在设备上跑时: adb push bin/ + plugins/ 到设备目录, 用 LD_LIBRARY_PATH 指向 bin/ 再执行。
            Write-MxInfo "[跳过] Android 目标是交叉编译产物, 不能在当前主机运行; 产物在: $(Join-Path $paths.InstallDir 'bin')"
        }
        else {
            $testExe = Join-Path $paths.InstallDir "bin/$($paths.TestName)"
            if (-not (Test-Path $testExe)) { throw "未找到测试可执行文件: $testExe" }

            # 插件框架把"父目录下的每个子目录"当作一个插件, 因此测试参数传插件目录的父目录
            $pluginRoot = Join-Path $paths.InstallDir 'plugins'
            Write-MxInfo "插件目录: $pluginRoot"
            Write-MxInfo "运行: $testExe $pluginRoot"
            & $testExe $pluginRoot | Out-Host
            Write-MxInfo "退出码: $LASTEXITCODE"
            if ($LASTEXITCODE -ne 0) { throw "原生测试失败 (退出码 $LASTEXITCODE)" }
        }
    }

    # ==================== 构建标记 ====================
    #
    # 全部做完 (含测试) 之后才写标记: 失败时没有标记, 下一次构建就不会被误判成"已是最新"。
    # -DepsOnly 没有产出插件框架库, 不能写标记。
    if (-not $DepsOnly) {
        Set-MxBuildStamp -BuildDir $paths.BuildDir -Kinds $stampKinds
        Write-MxInfo "构建标记: $($stampKinds -join ', ') → $($paths.BuildDir)"
    }
}

if ($Auto) {
    try {
        & $buildBody
    }
    catch {
        Write-Host ''
        Write-MxWarn "插件框架自动构建失败, 本次主程序构建不打包外部插件框架库: $($_.Exception.Message)"
        Write-MxInfo '主程序照常构建与运行 (外部插件功能不可用); 手工重跑看完整日志:'
        Write-MxInfo "  pwsh -NoProfile -File tools/build_native.ps1 -Config $Config"
        exit 0
    }
}
else {
    & $buildBody
}

if ($ConfigureOnly) {
    Write-Host ''
    Write-Host 'ConfigureOnly: 配置完成 (未构建)'
    exit 0
}

Write-Host ''
if ($DepsOnly) {
    Write-Host "DepsOnly: 依赖库构建完成 (未构建插件框架库); 安装前缀: $($paths.InstallDir)"
}
else {
    if ($Android) {
        Write-Host 'README: 插件框架库由 android/build.gradle 按 ABI 收集进 APK (jniLibs), 重新打包安装后生效'
    }
    Write-Host "插件框架构建完成: $($paths.HostLibrary)"
    if ($buildExamples) {
        Write-Host "示例插件在: $($paths.PluginsDir) (打包: pwsh -NoProfile -File tools/build_examples.ps1)"
    }
}
