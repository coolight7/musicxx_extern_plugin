# musicxx_extern_plugin 原生宿主 (CMake)
#
# 依赖来源 (两阶段构建, plan §11.2):
#   阶段一: tools/build_native.ps1 构建 cxx_pluginxx 并安装到 .native/<platform>-<arch>
#   阶段二: 本文件用 find_package 取内核 + 依赖 (fmt / yaml-cpp / cxx_utilxx_base)
#
# 命令行:
#   cmake -S src -B build -G "Visual Studio 18 2026" -A x64 \
#         -DMUSICXX_EXTERN_PLUGIN_NATIVE_PREFIX=<...>/.native/windows-x64 \
#         -DMUSICXX_EXTERN_PLUGIN_DEP_PREFIX=<cxx_utilxx_base/fmt/yaml-cpp 安装前缀>
cmake_minimum_required(VERSION 3.20)
project(musicxx_extern_plugin_host LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

if (NOT DEFINED MUSICXX_EXTERN_PLUGIN_NATIVE_PREFIX)
  set(MUSICXX_EXTERN_PLUGIN_NATIVE_PREFIX
      "${CMAKE_CURRENT_SOURCE_DIR}/../.native/windows-x64")
endif ()
list(APPEND CMAKE_PREFIX_PATH "${MUSICXX_EXTERN_PLUGIN_NATIVE_PREFIX}")

# cxx_utilxx_base / fmt / yaml-cpp 的安装前缀 (当前阶段复用 agentxx 构建产物;
# 后续随 .gitmodules 子模块化后可直接由 build_native 脚本产出)
if (DEFINED MUSICXX_EXTERN_PLUGIN_DEP_PREFIX)
  list(APPEND CMAKE_PREFIX_PATH "${MUSICXX_EXTERN_PLUGIN_DEP_PREFIX}")
endif ()

option(MUSICXX_EXTERN_PLUGIN_BUILD_EXAMPLE "构建示例插件" ON)
option(MUSICXX_EXTERN_PLUGIN_BUILD_TESTS "构建原生测试" ON)

find_package(cxx_pluginxx REQUIRED)
find_package(fmt REQUIRED)
find_package(yaml-cpp REQUIRED)

message(STATUS "[musicxx_ext] native prefix: ${MUSICXX_EXTERN_PLUGIN_NATIVE_PREFIX}")
message(STATUS "[musicxx_ext] dep prefix:    ${MUSICXX_EXTERN_PLUGIN_DEP_PREFIX}")

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
if (NOT XX_IS_MSVC_D OR NOT XX_IS_MSVC_D)
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
