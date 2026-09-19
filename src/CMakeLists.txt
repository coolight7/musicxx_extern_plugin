# musicxx_extern_plugin 原生构建顶层工程 (superbuild)
#
# 结构仿照 agentxx (agent/CMakeLists.txt + agent/lib/CMakeLists.txt):
#   1) 本文件 (superbuild): 推导 XX_IS_* 平台/编译器/配置宏, 准备 Boost 头文件与
#      BoostConfig.cmake, 构造 _COMMON_CMAKE_ARGS / _COMMON_CMAKE_CACHE_ARGS,
#      再用 ExternalProject_Add 把 third_party 的依赖库逐个 configure+build+install
#      到统一安装目录;
#   2) src/host/CMakeLists.txt (嵌套工程): 用 find_package 从安装目录取依赖,
#      编译宿主库 / 插件 SDK / 示例插件 / 原生测试。
# 依赖只来自 src/third_party 下的 git 子模块源码与 Boost 头文件, 不需要任何预编译库。
#
# 命令行 (一般由 tools/build_native.ps1 调用):
#   cmake -DXX_IS_RELEASE_D=1 -DCMAKE_BUILD_TYPE=Release -DCMAKE_CONFIGURATION_TYPES=Release \
#         -DMUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE=<含 boost/ 的目录> \
#         -B <build 目录> -S src -G "Visual Studio 18 2026" -A x64
#   cmake --build <build 目录> --config Release --parallel
cmake_minimum_required(VERSION 3.20)
# find_package 使用 <PackageName>_ROOT 变量
cmake_policy(SET CMP0074 NEW)
# MSVC 运行时库标志由抽象层选择
cmake_policy(SET CMP0091 NEW)
# find_package 使用大写的 _ROOT 变量
cmake_policy(SET CMP0144 NEW)
# 不使用 CMake 内置 FindBoost 模块, 统一走 BoostConfig.cmake (见下方生成逻辑)
cmake_policy(SET CMP0167 OLD)

project(musicxx_extern_plugin_superbuild LANGUAGES C CXX)

# 说明: CMAKE_CONFIGURATION_TYPES 由构建脚本在命令行收窄成"本次构建的配置" (多配置生成器只保留一种配置),
# 但**不往嵌套构建传** —— 原因见下方 _COMMON_CMAKE_CACHE_ARGS 处的注释 (嵌套编译器探测固定用 Debug)。
# 多配置生成器统一用 `cmake --build --config <配置>` 指定要构建的配置。

# ===== 编译标准 =====
# 内核要求 C++26 (MSVC 用 /std:c++latest); 嵌套构建沿用同一标准
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ===== Debug / Release 开关 (口径与 agentxx 的 XX_IS_RELEASE_D 一致) =====
# 仅表示"是否按调试用途编译": XX_IS_DEBUG_D=1 时库内打开更多日志/断言, 不代表编译类型。
# 构建脚本按 -Config 传 -DXX_IS_RELEASE_D=0|1。
option(XX_IS_RELEASE_D "Release Build." ON)
if (XX_IS_RELEASE_D)
  set(XX_IS_DEBUG_D   0)
  set(XX_IS_RELEASE_D 1)
else ()
  set(XX_IS_DEBUG_D   1)
  set(XX_IS_RELEASE_D 0)
endif ()

# ===== 目标平台 (XX_IS_*_D) =====
set(XX_IS_LINUX_D   0)
set(XX_IS_WIN_D     0)
set(XX_IS_MACOS_D   0)
set(XX_IS_ANDROID_D 0)
set(XX_IS_IOS_D     0)

if (CMAKE_SYSTEM_NAME STREQUAL "Windows")
  set(XX_IS_WIN_D 1)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Android")
  set(XX_IS_ANDROID_D 1)
elseif (APPLE AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(XX_IS_MACOS_D 1)
elseif (APPLE AND CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(XX_IS_IOS_D 1)
elseif (CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(XX_IS_LINUX_D 1)
else ()
  message(FATAL_ERROR "[musicxx_ext] 不支持的平台: ${CMAKE_SYSTEM_NAME}")
endif ()

# ===== 工具链 (XX_IS_*_D) =====
set(XX_IS_MSVC_D  0)
set(XX_IS_GCC_D   0)
set(XX_IS_CLANG_D 0)
set(XX_IS_MINGW_D 0)

if (MSVC)
  set(XX_IS_MSVC_D 1)
elseif (CMAKE_C_COMPILER_ID MATCHES "GNU")
  set(XX_IS_GCC_D 1)
elseif (CMAKE_C_COMPILER_ID MATCHES "Clang")
  set(XX_IS_CLANG_D 1)
else ()
  message(FATAL_ERROR "[musicxx_ext] 不支持的编译器: ${CMAKE_C_COMPILER_ID}")
endif ()

# MinGW: Windows 目标 + 非 MSVC 编译器
if (XX_IS_WIN_D AND NOT XX_IS_MSVC_D)
  set(XX_IS_MINGW_D 1)
endif ()

message("[musicxx_ext] platform: linux=${XX_IS_LINUX_D} win=${XX_IS_WIN_D} macos=${XX_IS_MACOS_D} "
        "android=${XX_IS_ANDROID_D} ios=${XX_IS_IOS_D} | toolchain: msvc=${XX_IS_MSVC_D} "
        "gcc=${XX_IS_GCC_D} clang=${XX_IS_CLANG_D} mingw=${XX_IS_MINGW_D} | debug=${XX_IS_DEBUG_D}")
message("[musicxx_ext] generator=${CMAKE_GENERATOR} configs=${CMAKE_CONFIGURATION_TYPES} build_type=${CMAKE_BUILD_TYPE}")

# ===== MSVC 运行时库 (统一动态链接标准库) =====
# 宿主库与插件、依赖库必须一致, 否则跨 DLL 传递 CRT 对象会崩
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
# 优先使用静态库
if (XX_IS_MSVC_D)
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .lib .a)
else ()
  list(PREPEND CMAKE_FIND_LIBRARY_SUFFIXES .a)
endif ()

# ===== Windows/MSVC 嵌套构建输出中文乱码修复 (做法与 agentxx 一致) =====
# ExternalProject 的步骤跑在 MSBuild 的 CustomBuild 任务里, 该任务未显式指定
# StdOutEncoding/StdErrEncoding 时按 OEM 代码页 (中文系统 936/GBK) 解码子进程输出,
# 于是嵌套构建里编译器/内层 MSBuild 的中文诊断会显示成乱码。这里在构建目录写下
# Directory.Build.targets (MSBuild 会自动向上查找并导入), 当构建环境声明控制台为
# UTF-8 (构建脚本设置 MUSICXX_EXT_BUILD_CONSOLE_CP=65001) 时强制按 UTF-8 解码。
if (XX_IS_WIN_D AND XX_IS_MSVC_D AND CMAKE_GENERATOR MATCHES "Visual Studio")
  file(WRITE "${CMAKE_BINARY_DIR}/Directory.Build.targets"
"<Project>\n"
"  <!-- musicxx_extern_plugin: 强制 MSBuild CustomBuild 任务按 UTF-8 解码子进程输出 (配合构建脚本的 UTF-8 控制台) -->\n"
"  <Target Name=\"MusicxxExt_ForceUtf8CustomBuildOutput\" BeforeTargets=\"CustomBuild\" Condition=\"'$(MusicxxExtBuildConsoleCP)' == '65001'\">\n"
"    <ItemGroup>\n"
"      <CustomBuild>\n"
"        <StdOutEncoding>UTF-8</StdOutEncoding>\n"
"        <StdErrEncoding>UTF-8</StdErrEncoding>\n"
"      </CustomBuild>\n"
"    </ItemGroup>\n"
"  </Target>\n"
"</Project>\n"
  )
endif ()

# ===== 目录布局 =====
# third_party: 依赖子模块源码 (commit 由仓库 gitlink 记录)
# 安装目录  : 依赖与宿主产物统一装到这里 (位于构建目录内, 因此 Release/Debug 天然隔离)
set(MUSICXX_EXTERN_PLUGIN_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/")
set(MUSICXX_EXTERN_PLUGIN_INSTALL_DIR
    "${CMAKE_BINARY_DIR}/musicxx-extern-plugin-install"
    CACHE PATH "依赖与宿主产物的安装目录 (find_package 的来源)")
set(XX_INSTALL_DIR "${MUSICXX_EXTERN_PLUGIN_INSTALL_DIR}")

option(MUSICXX_EXTERN_PLUGIN_BUILD_HOST  "构建宿主库/示例插件/原生测试 (关闭则只构建依赖)" ON)
option(MUSICXX_EXTERN_PLUGIN_BUILD_TESTS "构建原生测试" ON)

# ===== Boost (仅头文件) =====
# 内核与 cxx_utilxx_base 用 find_package(Boost CONFIG) 且只取头文件根 (Boost_INCLUDE_DIRS):
# 源码写 <asio/...> 时靠 <头文件根>/boost/ 命中 Boost.Asio; 不链接任何 Boost 编译库。
#
# 头文件来源 (优先级):
#   1) -DMUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE=<含 boost/ 的目录>  (tools/build_native.ps1 传入)
#   2) src/third_party/boost/include   (本机内置的 Boost 头文件)
#   3) .native/boost/boost_<版本>/      (构建脚本按固定版本下载解包后的缓存)
# 分工: 脚本负责"获取头文件"(下载/解包/校验), 本文件负责"生成 BoostConfig.cmake + 校验版本"。
if (NOT MUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE)
  set(_musicxx_ext_boost_candidates "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost/include")
  file(GLOB _musicxx_ext_boost_cached "${CMAKE_CURRENT_SOURCE_DIR}/../.native/boost/boost_*")
  list(APPEND _musicxx_ext_boost_candidates ${_musicxx_ext_boost_cached})
  foreach (_cand ${_musicxx_ext_boost_candidates})
    if (EXISTS "${_cand}/boost/version.hpp")
      set(MUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE "${_cand}")
      break()
    endif ()
  endforeach ()
  unset(_musicxx_ext_boost_candidates)
  unset(_musicxx_ext_boost_cached)
endif ()

if (NOT MUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE)
  message(FATAL_ERROR
    "[musicxx_ext] 缺少 Boost 头文件 (本框架只用头文件, 不链接 Boost 编译库)\n"
    "  请先执行: pwsh -NoProfile -File tools/build_native.ps1   (会按固定版本下载/解包 Boost)\n"
    "  或显式指定: -DMUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE=<含 boost/ 的目录>")
elseif (NOT EXISTS "${MUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE}/boost/version.hpp")
  message(FATAL_ERROR
    "[musicxx_ext] ${MUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE} 下没有 boost/version.hpp")
endif ()
file(TO_CMAKE_PATH "${MUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE}" MUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE)

# 从 boost/version.hpp 读出版本号 (BOOST_VERSION 形如 109200 = 1.92.0), 作为唯一版本来源
file(STRINGS "${MUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE}/boost/version.hpp" _musicxx_ext_boost_ver_line
     REGEX "^#define BOOST_VERSION ")
string(REGEX MATCH "[0-9]+" _musicxx_ext_boost_code "${_musicxx_ext_boost_ver_line}")
if (NOT _musicxx_ext_boost_code)
  message(FATAL_ERROR
    "[musicxx_ext] 无法从 ${MUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE}/boost/version.hpp 读出 BOOST_VERSION")
endif ()
math(EXPR _musicxx_ext_boost_major "${_musicxx_ext_boost_code} / 100000")
math(EXPR _musicxx_ext_boost_minor "(${_musicxx_ext_boost_code} / 100) % 1000")
math(EXPR _musicxx_ext_boost_patch "${_musicxx_ext_boost_code} % 100")
set(MUSICXX_EXTERN_PLUGIN_BOOST_VERSION
    "${_musicxx_ext_boost_major}.${_musicxx_ext_boost_minor}.${_musicxx_ext_boost_patch}")
unset(_musicxx_ext_boost_ver_line)
unset(_musicxx_ext_boost_code)
unset(_musicxx_ext_boost_major)
unset(_musicxx_ext_boost_minor)
unset(_musicxx_ext_boost_patch)

message("[musicxx_ext] Boost 头文件: ${MUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE} (版本 ${MUSICXX_EXTERN_PLUGIN_BOOST_VERSION})")

# 生成 BoostConfig.cmake: b2 生成的那套配置用不上 (我们只要头文件根), 而且预生成的配置会把
# 绝对路径烘焙进导出目标 (曾因此把别的 Boost 安装目录带进下游编译)。
set(BOOST_INCLUDE_DIR "${MUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE}")
set(BOOST_VERSION "${MUSICXX_EXTERN_PLUGIN_BOOST_VERSION}")
set(_musicxx_ext_boost_cfg_dir
    "${MUSICXX_EXTERN_PLUGIN_INSTALL_DIR}/lib/cmake/Boost-${BOOST_VERSION}")
file(MAKE_DIRECTORY "${_musicxx_ext_boost_cfg_dir}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/../tools/cmake/BoostConfig.cmake.in"
               "${_musicxx_ext_boost_cfg_dir}/BoostConfig.cmake" @ONLY)
unset(_musicxx_ext_boost_cfg_dir)

# 依赖与宿主工程都从安装目录取依赖
list(PREPEND CMAKE_PREFIX_PATH "${MUSICXX_EXTERN_PLUGIN_INSTALL_DIR}")

# 交叉编译: 隔离搜索, 避免引用宿主机的库
if (CMAKE_CROSSCOMPILING)
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY "ONLY")
  set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE "ONLY")
  set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE "ONLY")
  list(PREPEND CMAKE_FIND_ROOT_PATH "${MUSICXX_EXTERN_PLUGIN_INSTALL_DIR}")
  list(APPEND CMAKE_FIND_ROOT_PATH "${MUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE}")
endif ()

# ===== 嵌套构建公共参数 (对应 agentxx 的 _AGENTXX_COMMON_CMAKE_*_ARGS) =====
# 嵌套构建是独立的 cmake configure, 不继承本工程的变量, 因此编译器/启动器/链接参数/
# 运行时库/平台宏/安装目录必须显式传下去, 否则依赖库与宿主库的 ABI 与运行库会不一致。
#
# 注意: 值里带分号的 (列表型: CMAKE_PREFIX_PATH / CMAKE_FIND_ROOT_PATH) 只能放
# CMAKE_CACHE_ARGS (会写进嵌套构建的 initial cache 文件, 分号不会丢);
# CMAKE_ARGS 是命令行参数, 值里的分号会被当成列表分隔符拆开, 传给嵌套 cmake 就变成
# 多个杂散参数 (曾出现 "Ignoring extra path from command line: RelWithDebInfo")。
function(_musicxx_ext_add_arg var_name value)
  if (NOT "${value}" STREQUAL "")
    list(APPEND ${var_name} "${value}")
    set(${var_name} "${${var_name}}" PARENT_SCOPE)
  endif ()
endfunction()

set(_COMMON_CMAKE_CACHE_ARGS
    "-DCMAKE_INSTALL_PREFIX:PATH=${MUSICXX_EXTERN_PLUGIN_INSTALL_DIR}"
    "-DCMAKE_PREFIX_PATH:STRING=${CMAKE_PREFIX_PATH}"
    "-DBOOST_ROOT:PATH=${MUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE}"
    "-DBoost_ROOT:PATH=${MUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE}"
    "-DBoost_INCLUDE_DIR:PATH=${MUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE}"
    "-DMUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE:PATH=${MUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE}"
)
# 配置类型不往嵌套构建传:
# - 嵌套 cmake 的编译器探测固定用 Debug 配置生成测试工程, 若把可用配置收窄成 Release,
#   探测会因为找不到 Debug 而失败 ("Check for working C compiler ... broken");
# - 嵌套构建保留全部配置也无害: 构建时由 ExternalProject 自动带上 `--config <顶层配置>`,
#   安装进前缀的只有本次配置的产物。
_musicxx_ext_add_arg(_COMMON_CMAKE_CACHE_ARGS "-DCMAKE_FIND_ROOT_PATH:STRING=${CMAKE_FIND_ROOT_PATH}")

set(_COMMON_CMAKE_ARGS
    # 安装与工程布局
    "-DCMAKE_INSTALL_PREFIX=${MUSICXX_EXTERN_PLUGIN_INSTALL_DIR}"
    "-DMUSICXX_EXTERN_PLUGIN_DEP_PREFIX=${MUSICXX_EXTERN_PLUGIN_INSTALL_DIR}"
    "-DXX_INSTALL_DIR=${XX_INSTALL_DIR}"
    "-DMUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE=${MUSICXX_EXTERN_PLUGIN_BOOST_INCLUDE}"
    "-DMUSICXX_EXTERN_PLUGIN_BOOST_VERSION=${MUSICXX_EXTERN_PLUGIN_BOOST_VERSION}"

    # 配置类型 (单值)
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"

    # 交叉编译 / 工具链
    "-DCMAKE_CROSSCOMPILING=${CMAKE_CROSSCOMPILING}"
    "-DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}"
    "-DCMAKE_SYSTEM_PROCESSOR=${CMAKE_SYSTEM_PROCESSOR}"
    "-DCMAKE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"

    # 编译器
    "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
    "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"

    # 链接与编译参数 (嵌套工程需要与顶层一致, 例如 MSVC 的 /machine:x64)
    "-DCMAKE_EXE_LINKER_FLAGS=${CMAKE_EXE_LINKER_FLAGS}"
    "-DCMAKE_SHARED_LINKER_FLAGS=${CMAKE_SHARED_LINKER_FLAGS}"
    "-DCMAKE_EXE_LINKER_FLAGS_DEBUG=${CMAKE_EXE_LINKER_FLAGS_DEBUG}"
    "-DCMAKE_SHARED_LINKER_FLAGS_DEBUG=${CMAKE_SHARED_LINKER_FLAGS_DEBUG}"
    "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=${CMAKE_EXE_LINKER_FLAGS_RELEASE}"
    "-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=${CMAKE_SHARED_LINKER_FLAGS_RELEASE}"
    "-DCMAKE_C_FLAGS=${CMAKE_C_FLAGS}"
    "-DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS}"
    "-DCMAKE_C_FLAGS_RELEASE=${CMAKE_C_FLAGS_RELEASE}"
    "-DCMAKE_CXX_FLAGS_RELEASE=${CMAKE_CXX_FLAGS_RELEASE}"
    "-DCMAKE_C_FLAGS_DEBUG=${CMAKE_C_FLAGS_DEBUG}"
    "-DCMAKE_CXX_FLAGS_DEBUG=${CMAKE_CXX_FLAGS_DEBUG}"
    "-DCMAKE_C_FLAGS_RELWITHDEBINFO=${CMAKE_C_FLAGS_RELWITHDEBINFO}"
    "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=${CMAKE_CXX_FLAGS_RELWITHDEBINFO}"
    "-DCMAKE_C_FLAGS_MINSIZEREL=${CMAKE_C_FLAGS_MINSIZEREL}"
    "-DCMAKE_CXX_FLAGS_MINSIZEREL=${CMAKE_CXX_FLAGS_MINSIZEREL}"
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DCMAKE_EXPORT_COMPILE_COMMANDS=${CMAKE_EXPORT_COMPILE_COMMANDS}

    # MSVC 运行时库 (宿主库与依赖库必须一致)
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}"

    # 策略 (嵌套工程里同样生效)
    -DCMAKE_POLICY_DEFAULT_CMP0074=NEW
    -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
    -DCMAKE_POLICY_DEFAULT_CMP0144=NEW
    -DCMAKE_POLICY_DEFAULT_CMP0167=OLD

    # 平台/编译器宏 (依赖库的 cmake/xx_platform_macros.cmake 会沿用上层判定)
    -DXX_IS_DEBUG_D=${XX_IS_DEBUG_D}
    -DXX_IS_RELEASE_D=${XX_IS_RELEASE_D}
    -DXX_IS_LINUX_D=${XX_IS_LINUX_D}
    -DXX_IS_WIN_D=${XX_IS_WIN_D}
    -DXX_IS_MACOS_D=${XX_IS_MACOS_D}
    -DXX_IS_ANDROID_D=${XX_IS_ANDROID_D}
    -DXX_IS_IOS_D=${XX_IS_IOS_D}
    -DXX_IS_MINGW_D=${XX_IS_MINGW_D}
    -DXX_IS_MSVC_D=${XX_IS_MSVC_D}
    -DXX_IS_GCC_D=${XX_IS_GCC_D}
    -DXX_IS_CLANG_D=${XX_IS_CLANG_D}

    # 宿主工程开关
    -DMUSICXX_EXTERN_PLUGIN_BUILD_TESTS=${MUSICXX_EXTERN_PLUGIN_BUILD_TESTS}
)

# 只在有值时传递 (空值传下去会覆盖嵌套构建里的默认值, 徒增噪音与歧义)
_musicxx_ext_add_arg(_COMMON_CMAKE_ARGS "-DCMAKE_TOOLCHAIN_FILE:PATH=${CMAKE_TOOLCHAIN_FILE}")
_musicxx_ext_add_arg(_COMMON_CMAKE_ARGS "-DCMAKE_C_COMPILER_LAUNCHER=${CMAKE_C_COMPILER_LAUNCHER}")
_musicxx_ext_add_arg(_COMMON_CMAKE_ARGS "-DCMAKE_CXX_COMPILER_LAUNCHER=${CMAKE_CXX_COMPILER_LAUNCHER}")
_musicxx_ext_add_arg(_COMMON_CMAKE_ARGS "-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=${CMAKE_MSVC_DEBUG_INFORMATION_FORMAT}")
if (CMAKE_FIND_ROOT_PATH_MODE_LIBRARY)
  list(APPEND _COMMON_CMAKE_ARGS -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=${CMAKE_FIND_ROOT_PATH_MODE_LIBRARY})
endif ()
if (CMAKE_FIND_ROOT_PATH_MODE_INCLUDE)
  list(APPEND _COMMON_CMAKE_ARGS -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=${CMAKE_FIND_ROOT_PATH_MODE_INCLUDE})
endif ()
if (CMAKE_FIND_ROOT_PATH_MODE_PACKAGE)
  list(APPEND _COMMON_CMAKE_ARGS -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=${CMAKE_FIND_ROOT_PATH_MODE_PACKAGE})
endif ()
if (ANDROID_PLATFORM)
  list(APPEND _COMMON_CMAKE_ARGS
      "-DCMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}"
      -DANDROID_ABI=${ANDROID_ABI}
      -DANDROID_PLATFORM=${ANDROID_PLATFORM}
      -DANDROID_STL=${ANDROID_STL}
  )
endif ()

message("[musicxx_ext] _COMMON_CMAKE_ARGS: ${_COMMON_CMAKE_ARGS}")
message("[musicxx_ext] _COMMON_CMAKE_CACHE_ARGS: ${_COMMON_CMAKE_CACHE_ARGS}")
message("[musicxx_ext] 安装目录: ${MUSICXX_EXTERN_PLUGIN_INSTALL_DIR}")

# ===== 依赖库: 源码全部来自 third_party, 统一 ExternalProject_Add =====
#
# 嵌套构建目录刻意用短路径 (<构建目录>/e/<短名>):
# ExternalProject 默认布局是 <项目名>-prefix/src/<项目名>-build, 叠加 CMake 探测编译器时
# 生成的 CMakeFiles/CMakeScratch/TryCompile-xxxxxx/cmTC_xxx.dir/Debug/.../*.tlog 会超过
# Windows 的 MAX_PATH(260), 触发 MSBuild FileTracker 的 "FTK1011: could not create the new
# file tracking log file" (本包位于较深的目录, 更容易命中)。
include(ExternalProject)
set(MUSICXX_EXTERN_PLUGIN_BUILD_ROOT "${CMAKE_BINARY_DIR}/e")

# fmt
ExternalProject_Add(
  fmt_repo
  SOURCE_DIR "${MUSICXX_EXTERN_PLUGIN_THIRD_PARTY_DIR}/fmt/"
  BINARY_DIR "${MUSICXX_EXTERN_PLUGIN_BUILD_ROOT}/fmt"
  INSTALL_DIR "${MUSICXX_EXTERN_PLUGIN_INSTALL_DIR}"
  CMAKE_ARGS
    ${_COMMON_CMAKE_ARGS}
    -DFMT_INSTALL=ON
    -DFMT_DOC=OFF
    -DFMT_TEST=OFF
    -DBUILD_SHARED_LIBS=OFF
  CMAKE_CACHE_ARGS
    ${_COMMON_CMAKE_CACHE_ARGS}
  TEST_COMMAND ""
)

# yaml-cpp (plugin.yaml 清单解析)
ExternalProject_Add(
  yaml_cpp_repo
  SOURCE_DIR "${MUSICXX_EXTERN_PLUGIN_THIRD_PARTY_DIR}/yaml-cpp/"
  BINARY_DIR "${MUSICXX_EXTERN_PLUGIN_BUILD_ROOT}/yaml-cpp"
  INSTALL_DIR "${MUSICXX_EXTERN_PLUGIN_INSTALL_DIR}"
  CMAKE_ARGS
    ${_COMMON_CMAKE_ARGS}
    -DYAML_CPP_BUILD_CONTRIB=OFF
    -DYAML_CPP_BUILD_TOOLS=OFF
    -DYAML_CPP_BUILD_TESTS=OFF
    -DYAML_CPP_INSTALL=ON
    -DYAML_ENABLE_PIC=ON
    -DBUILD_SHARED_LIBS=OFF
  CMAKE_CACHE_ARGS
    ${_COMMON_CMAKE_CACHE_ARGS}
  TEST_COMMAND ""
)

# simdjson (cxx_utilxx_base 的 JSON 后端)
ExternalProject_Add(
  simdjson_repo
  SOURCE_DIR "${MUSICXX_EXTERN_PLUGIN_THIRD_PARTY_DIR}/simdjson/"
  BINARY_DIR "${MUSICXX_EXTERN_PLUGIN_BUILD_ROOT}/simdjson"
  INSTALL_DIR "${MUSICXX_EXTERN_PLUGIN_INSTALL_DIR}"
  CMAKE_ARGS
    ${_COMMON_CMAKE_ARGS}
    -DSIMDJSON_JUST_LIBRARY=ON
    -DSIMDJSON_BUILD_STATIC=ON
    -DSIMDJSON_DEVELOPER_MODE=OFF
    -DSIMDJSON_ENABLE_THREADS=OFF
    -DBUILD_SHARED_LIBS=OFF
  CMAKE_CACHE_ARGS
    ${_COMMON_CMAKE_CACHE_ARGS}
  TEST_COMMAND ""
)

# libiconv (C, 提供 iconv.h 与 iconvConfig.cmake; cxx_utilxx_base 的字符集转换必需)
ExternalProject_Add(
  libiconv_repo
  SOURCE_DIR "${MUSICXX_EXTERN_PLUGIN_THIRD_PARTY_DIR}/libiconv-native/"
  BINARY_DIR "${MUSICXX_EXTERN_PLUGIN_BUILD_ROOT}/libiconv"
  INSTALL_DIR "${MUSICXX_EXTERN_PLUGIN_INSTALL_DIR}"
  CMAKE_ARGS
    ${_COMMON_CMAKE_ARGS}
    -DICONV_ENABLE_EXTRA=OFF
    -DICONV_EMBED_MODE=OFF
    -DBUILD_SHARED_LIBS=OFF
  CMAKE_CACHE_ARGS
    ${_COMMON_CMAKE_CACHE_ARGS}
  TEST_COMMAND ""
)

# uchardet (字符集探测; 命令行工具依赖 Windows 没有的 getopt.h, 故 BUILD_BINARY=OFF)
ExternalProject_Add(
  uchardet_repo
  SOURCE_DIR "${MUSICXX_EXTERN_PLUGIN_THIRD_PARTY_DIR}/uchardet/"
  BINARY_DIR "${MUSICXX_EXTERN_PLUGIN_BUILD_ROOT}/uchardet"
  INSTALL_DIR "${MUSICXX_EXTERN_PLUGIN_INSTALL_DIR}"
  CMAKE_ARGS
    ${_COMMON_CMAKE_ARGS}
    -DCHECK_SSE2=ON
    -DBUILD_BINARY=OFF
    -DBUILD_SHARED_LIBS=OFF
  CMAKE_CACHE_ARGS
    ${_COMMON_CMAKE_CACHE_ARGS}
  TEST_COMMAND ""
)

# cxx_utilxx_base (日志/JSON/字符串/取消令牌/系统探测)
# - 字符集开关固定 ON: 该库的 string_util 无条件使用 <iconv.h> 与 uchardet
# - BUILD_ALWAYS/INSTALL_ALWAYS: 本仓维护的依赖, 源码改动后重新构建并安装
ExternalProject_Add(
  cxx_utilxx_base_repo
  DEPENDS
    fmt_repo
    simdjson_repo
    libiconv_repo
    uchardet_repo
  SOURCE_DIR "${MUSICXX_EXTERN_PLUGIN_THIRD_PARTY_DIR}/cxx_utilxx_base/"
  BINARY_DIR "${MUSICXX_EXTERN_PLUGIN_BUILD_ROOT}/utilxx_base"
  INSTALL_DIR "${MUSICXX_EXTERN_PLUGIN_INSTALL_DIR}"
  BUILD_ALWAYS TRUE
  INSTALL_ALWAYS TRUE
  CMAKE_ARGS
    ${_COMMON_CMAKE_ARGS}
    -DCXX_UTILXX_BASE_ENABLE_CHARSET=ON
    -DCXX_UTILXX_BASE_USE_BOOST_ASIO=ON
    -DCXX_UTILXX_BASE_LINUX_IO_URING_SUPPORTED=OFF
  CMAKE_CACHE_ARGS
    ${_COMMON_CMAKE_CACHE_ARGS}
  TEST_COMMAND ""
)

# cxx_pluginxx (插件框架内核)
ExternalProject_Add(
  cxx_pluginxx_repo
  DEPENDS
    cxx_utilxx_base_repo
    fmt_repo
    yaml_cpp_repo
  SOURCE_DIR "${MUSICXX_EXTERN_PLUGIN_THIRD_PARTY_DIR}/cxx_pluginxx/"
  BINARY_DIR "${MUSICXX_EXTERN_PLUGIN_BUILD_ROOT}/pluginxx"
  INSTALL_DIR "${MUSICXX_EXTERN_PLUGIN_INSTALL_DIR}"
  BUILD_ALWAYS TRUE
  INSTALL_ALWAYS TRUE
  CMAKE_ARGS
    ${_COMMON_CMAKE_ARGS}
  CMAKE_CACHE_ARGS
    ${_COMMON_CMAKE_CACHE_ARGS}
  TEST_COMMAND ""
)

# ===== 宿主工程 (嵌套构建, 内部用 find_package 取上面的依赖) =====
if (MUSICXX_EXTERN_PLUGIN_BUILD_HOST)
  ExternalProject_Add(
    musicxx_extern_plugin_host_repo
    DEPENDS
      cxx_pluginxx_repo
      fmt_repo
      yaml_cpp_repo
    SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/host/"
    BINARY_DIR "${MUSICXX_EXTERN_PLUGIN_BUILD_ROOT}/host"
    INSTALL_DIR "${MUSICXX_EXTERN_PLUGIN_INSTALL_DIR}"
    BUILD_ALWAYS TRUE
    INSTALL_ALWAYS TRUE
    CMAKE_ARGS
      ${_COMMON_CMAKE_ARGS}
    CMAKE_CACHE_ARGS
      ${_COMMON_CMAKE_CACHE_ARGS}
    TEST_COMMAND ""
  )
else ()
  message("[musicxx_ext] MUSICXX_EXTERN_PLUGIN_BUILD_HOST=OFF: 只构建依赖库")
endif ()
