# musicxx_extern_plugin 原生宿主 (CMake)
#
# 依赖来源 (两阶段构建, plan §11.2):
#   阶段一: tools/build_native.ps1 从 src/third_party 的子模块源码构建依赖
#           (fmt / yaml-cpp / simdjson / libiconv-native / uchardet / cxx_utilxx_base / cxx_pluginxx)
#           并安装到 .native/deps/<平台>-<架构>; Boost 只取头文件, 其 CMake 配置也由脚本生成
#   阶段二: 本文件用 find_package 取内核与依赖, 构建宿主库 / 示例插件 / 原生测试
#
# 命令行:
#   cmake -S src -B build -G "Visual Studio 18 2026" -A x64 \
#         -DMUSICXX_EXTERN_PLUGIN_NATIVE_PREFIX=<...>/.native/deps/windows-x64 \
#         -DMUSICXX_EXTERN_PLUGIN_DEP_PREFIX=<...>/.native/deps/windows-x64 \
#         -DBoost_DIR=<...>/.native/deps/windows-x64/lib/cmake/Boost-1.92.0
cmake_minimum_required(VERSION 3.20)
project(musicxx_extern_plugin_host LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 依赖前缀默认位置与 tools/build_native.ps1 保持一致: .native/deps/<平台>-<架构>
if (NOT DEFINED MUSICXX_EXTERN_PLUGIN_NATIVE_PREFIX)
  if (WIN32)
    set(_musicxx_ext_os "windows")
  elseif (ANDROID)
    set(_musicxx_ext_os "android")
  elseif (IOS)
    set(_musicxx_ext_os "ios")
  elseif (APPLE)
    set(_musicxx_ext_os "macos")
  else ()
    set(_musicxx_ext_os "linux")
  endif ()
  if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "(?i)arm|aarch64")
      set(_musicxx_ext_arch "arm64")
    else ()
      set(_musicxx_ext_arch "x64")
    endif ()
  else ()
    set(_musicxx_ext_arch "x86")
  endif ()
  set(MUSICXX_EXTERN_PLUGIN_NATIVE_PREFIX
      "${CMAKE_CURRENT_SOURCE_DIR}/../.native/deps/${_musicxx_ext_os}-${_musicxx_ext_arch}")
endif ()
list(APPEND CMAKE_PREFIX_PATH "${MUSICXX_EXTERN_PLUGIN_NATIVE_PREFIX}")

# cxx_utilxx_base / fmt / yaml-cpp / Boost 的安装前缀
# 默认取本包自己的依赖前缀 (.native/deps/<平台>-<架构>), 由 tools/build_native.ps1 阶段一产出;
# 仍可用 -DMUSICXX_EXTERN_PLUGIN_DEP_PREFIX=<其它前缀> 覆盖 (例如复用已有依赖安装)。
if (NOT DEFINED MUSICXX_EXTERN_PLUGIN_DEP_PREFIX)
  set(MUSICXX_EXTERN_PLUGIN_DEP_PREFIX "${MUSICXX_EXTERN_PLUGIN_NATIVE_PREFIX}")
endif ()
list(APPEND CMAKE_PREFIX_PATH "${MUSICXX_EXTERN_PLUGIN_DEP_PREFIX}")

option(MUSICXX_EXTERN_PLUGIN_BUILD_EXAMPLE "构建示例插件" ON)
option(MUSICXX_EXTERN_PLUGIN_BUILD_TESTS "构建原生测试" ON)

message(STATUS "[musicxx_ext] native prefix: ${MUSICXX_EXTERN_PLUGIN_NATIVE_PREFIX}")
message(STATUS "[musicxx_ext] dep prefix:    ${MUSICXX_EXTERN_PLUGIN_DEP_PREFIX}")

# 依赖全部来自上面两个前缀: 找不到时直接提示"先跑构建脚本", 不要让人去猜
set(_musicxx_ext_missing "")
find_package(cxx_pluginxx CONFIG QUIET)
if (NOT cxx_pluginxx_FOUND)
  list(APPEND _musicxx_ext_missing "cxx_pluginxx")
endif ()
find_package(fmt CONFIG QUIET)
if (NOT fmt_FOUND)
  list(APPEND _musicxx_ext_missing "fmt")
endif ()
find_package(yaml-cpp CONFIG QUIET)
if (NOT yaml-cpp_FOUND)
  list(APPEND _musicxx_ext_missing "yaml-cpp")
endif ()
find_package(Boost CONFIG QUIET)
if (NOT Boost_FOUND)
  list(APPEND _musicxx_ext_missing "Boost")
endif ()
if (_musicxx_ext_missing)
  string(REPLACE ";" ", " _musicxx_ext_missing_text "${_musicxx_ext_missing}")
  message(FATAL_ERROR
    "[musicxx_ext] 缺少依赖包: ${_musicxx_ext_missing_text}\n"
    "  当前依赖前缀: ${MUSICXX_EXTERN_PLUGIN_DEP_PREFIX}\n"
    "  请先构建依赖:\n"
    "    pwsh -NoProfile -File tools/build_native.ps1")
endif ()
unset(_musicxx_ext_missing)

# ==================== 宿主库 ====================

set(MUSICXX_EXTERN_PLUGIN_HOST_SOURCES
  host/musicxx_host.cpp
  host/musicxx_host_tables.cpp
  host/musicxx_extern_plugin_api.cpp
)

add_library(musicxx_extern_plugin SHARED ${MUSICXX_EXTERN_PLUGIN_HOST_SOURCES})
add_library(musicxx_extern_plugin::host ALIAS musicxx_extern_plugin)

target_include_directories(musicxx_extern_plugin
  PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/host"
          "${CMAKE_CURRENT_SOURCE_DIR}/sdk/include"
)

target_compile_definitions(musicxx_extern_plugin PRIVATE MUSICXX_EXTERN_PLUGIN_BUILD=1)

# 源码与头文件都是 UTF-8 (含中文注释); MSVC 默认按系统代码页(936)解析会吃掉换行导致语法错误
if (MSVC)
  target_compile_options(musicxx_extern_plugin PRIVATE /utf-8)
endif ()

target_link_libraries(musicxx_extern_plugin
  PRIVATE cxx_pluginxx_static fmt::fmt yaml-cpp::yaml-cpp
)

# 导出面: 只导出 musicxx_extern_plugin_* (Windows 靠 __declspec(dllexport) 显式声明;
# 其它平台默认隐藏符号, 只留标注过导出宏的入口)
if (NOT MSVC)
  set_target_properties(musicxx_extern_plugin PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
  )
endif ()

if (WIN32)
  target_compile_definitions(musicxx_extern_plugin PRIVATE _WIN32_WINNT=0x0A00)
endif ()

# ==================== 插件 SDK (header-only 接口) ====================
#
# 示例插件与外部插件作者只需头文件 (内核 kit 是 header-only); 这里把所需 include
# 目录与平台宏集中到一个 INTERFACE 目标, 避免每个插件各自猜路径。
add_library(musicxx_extern_plugin_sdk INTERFACE)
target_include_directories(musicxx_extern_plugin_sdk INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/include"
  "${MUSICXX_EXTERN_PLUGIN_NATIVE_PREFIX}/include"
)
foreach (_sdkInc
         "${MUSICXX_EXTERN_PLUGIN_DEP_PREFIX}/include"
         "${MUSICXX_EXTERN_PLUGIN_DEP_PREFIX}/include/boost"
         "${Boost_INCLUDE_DIRS}"
         "${Boost_INCLUDE_DIRS}/boost")
  if (EXISTS "${_sdkInc}")
    target_include_directories(musicxx_extern_plugin_sdk INTERFACE "${_sdkInc}")
  endif ()
endforeach ()
target_compile_definitions(musicxx_extern_plugin_sdk INTERFACE
  BOOST_ASIO_STANDALONE=1
  ASIO_STANDALONE=1
  UTILXX_USE_BOOST_ASIO=1
)
if (MSVC)
  target_compile_options(musicxx_extern_plugin_sdk INTERFACE /utf-8)
endif ()
unset(_sdkInc)

# ==================== 示例插件 ====================

if (MUSICXX_EXTERN_PLUGIN_BUILD_EXAMPLE)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../example/example_native"
                   "${CMAKE_CURRENT_BINARY_DIR}/example_native")
endif ()

# ==================== 原生测试 ====================

if (MUSICXX_EXTERN_PLUGIN_BUILD_TESTS)
  add_executable(musicxx_extern_plugin_test "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_host.cpp")
  target_include_directories(musicxx_extern_plugin_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(musicxx_extern_plugin_test PRIVATE musicxx_extern_plugin)
  if (MSVC)
    target_compile_options(musicxx_extern_plugin_test PRIVATE /utf-8)
  endif ()
  if (WIN32)
    target_compile_definitions(musicxx_extern_plugin_test PRIVATE _WIN32_WINNT=0x0A00)
  endif ()
endif ()
