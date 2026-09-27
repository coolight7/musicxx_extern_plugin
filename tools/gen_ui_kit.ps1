# 生成 musicxx 扩展 kit（界面描述层的 C++ 头与 JS 文件）
#
# 用法（在包目录下，需要 Dart SDK）：
#   pwsh -NoProfile -File tools/gen_ui_kit.ps1
#
# 说明：
# - 定义文件是本包的 `schema/musicxx-ui-kit.def.json`（`extends` 的是插件包里子模块
#   `src/third_party/cxx_pluginxx_ui` 的基础 kit：基础组件会被合并进来，同名组件以本包的为准）；
# - 生成器属于描述层库，必须在库根目录下运行（它按相对路径读 `schema/ui.def.json`）；
# - 产物：
#     src/sdk/include/musicxx/plugin/api/musicxx_ui_kit.g.h   插件 SDK 的 C++ kit（命名空间 musicxx::ui::kit）
#     js/musicxx_ui_kit.js                                    随插件分发的 JS kit（全局 pluginxx.ui.kit）
#     docs/musicxx-ui-kit.md                                  生成出来的组件说明
#   前两个是生成物，改 kit 后重跑本脚本并一起提交。
param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'

$libRoot = Join-Path $Root 'src/third_party/cxx_pluginxx_ui'
$defPath = Join-Path $Root 'schema/musicxx-ui-kit.def.json'
$sdkDir = Join-Path $Root 'src/sdk/include/musicxx/plugin/api'
$jsDir = Join-Path $Root 'js'
$docsDir = Join-Path $Root 'docs'

foreach ($p in @($libRoot, $defPath)) {
    if (-not (Test-Path $p)) {
        throw "找不到 $p （子模块没初始化？跑一次 git submodule update --init --recursive）"
    }
}
if (-not (Get-Command dart -ErrorAction SilentlyContinue)) {
    throw 'PATH 里没有 dart（描述层库的生成器是 Dart 写的）'
}

# 生成到临时目录，再按本包的目录结构摆放（生成器只会往一个 --out 目录里写）
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("musicxx-ui-kit-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
try {
    Push-Location $libRoot
    try {
        & dart run tools/gen_ui.dart --ext-kit $defPath --prefix musicxx `
            --namespace 'musicxx::ui::kit' --targets cpp,js --out $tmp `
            --source-note 'schema/musicxx-ui-kit.def.json'
        if ($LASTEXITCODE -ne 0) { throw "生成器退出码 $LASTEXITCODE" }
    } finally {
        Pop-Location
    }

    New-Item -ItemType Directory -Force -Path $sdkDir, $jsDir, $docsDir | Out-Null
    Copy-Item (Join-Path $tmp 'musicxx_ui_kit.g.h') (Join-Path $sdkDir 'musicxx_ui_kit.g.h') -Force
    Copy-Item (Join-Path $tmp 'musicxx_ui_kit.js') (Join-Path $jsDir 'musicxx_ui_kit.js') -Force
    Copy-Item (Join-Path $tmp 'musicxx_ui_kit.md') (Join-Path $docsDir 'musicxx-ui-kit.md') -Force

    Write-Output '生成完成：'
    Write-Output ("  " + (Join-Path $sdkDir 'musicxx_ui_kit.g.h'))
    Write-Output ("  " + (Join-Path $jsDir 'musicxx_ui_kit.js'))
    Write-Output ("  " + (Join-Path $docsDir 'musicxx-ui-kit.md'))
    Write-Output ''
    Write-Output '提示：把 js/ 下的两个 kit 文件复制进要发布的 JS 插件目录（见 docs/plugin-ui.md）'
    Write-Output '      pwsh -NoProfile -File tools/sync_ui_kit.ps1   # 一次同步到 plugins/ 下的示例插件'
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
