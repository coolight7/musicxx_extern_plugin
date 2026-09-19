# musicxx_extern_plugin 宿主工程 (嵌套构建)
#
# 由顶层 src/CMakeLists.txt 的 ExternalProject_Add(musicxx_extern_plugin_host_repo) 调用,
# 依赖全部经 find_package 从顶层准备好的安装前缀取 (依赖库由顶层 ExternalProject 先构建安装)。
# 产物: 宿主库 musicxx_extern_plugin / 插件 SDK(头文件接口) / 示例插件 / 原生测试。
cmake_minimum_required(VERSION 3.20)
# find_package 使用 <PackageName>_ROOT 变量
cmake_policy(SET CMP0074 NEW)
cmake_policy(SET CMP0091 NEW)
cmake_policy(SET CMP0144 NEW)
# 统一走 BoostConfig.cmake (由顶层 superbuild 生成)
cmake_policy(SET CMP0167 OLD)

project(musicxx_extern_plugin_host LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(MUSICXX_EXTERN_PLUGIN_BUILD_EXAMPLE "构建示例插件" ON)
option(MUSICXX_EXTERN_PLUGIN_BUILD_TESTS   "构建原生测试" ON)
# JS 插件运行时 (QuickJS): 关闭后宿主只支持原生插件 (内核与 Dart C ABI 不受影响)
option(MUSICXX_EXTERN_PLUGIN_ENABLE_JS     "构建 JS 插件运行时 (QuickJS)" ON)

# ===== 依赖前缀 =====
# 顶层 superbuild 通过 -DMUSICXX_EXTERN_PLUGIN_DEP_PREFIX / -DXX_INSTALL_DIR 传入;
# 手工单独构建本工程时也可以显式指定 (但依赖必须先构建好, 见 README)。
if (NOT MUSICXX_EXTERN_PLUGIN_DEP_PREFIX)
  if (XX_INSTALL_DIR)
    set(MUSICXX_EXTERN_PLUGIN_DEP_PREFIX "${XX_INSTALL_DIR}")
  else ()
    message(FATAL_ERROR
      "[musicxx_ext] 未指定依赖前缀 (找不到 cxx_pluginxx / fmt / yaml-cpp / Boost)\n"
      "  请用顶层工程构建: pwsh -NoProfile -File tools/build_native.ps1\n"
      "  或显式指定: -DMUSICXX_EXTERN_PLUGIN_DEP_PREFIX=<依赖安装前缀>")
  endif ()
endif ()
list(APPEND CMAKE_PREFIX_PATH "${MUSICXX_EXTERN_PLUGIN_DEP_PREFIX}")
message(STATUS "[musicxx_ext] 宿主工程依赖前缀: ${MUSICXX_EXTERN_PLUGIN_DEP_PREFIX}")

# 依赖全部来自上面的前缀: 找不到时直接提示"先跑构建脚本", 不要让人去猜
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
    "  依赖前缀: ${MUSICXX_EXTERN_PLUGIN_DEP_PREFIX}\n"
    "  请先运行: pwsh -NoProfile -File tools/build_native.ps1")
endif ()
unset(_musicxx_ext_missing)

# 上层源码目录 (C ABI 头 / 插件 SDK / 示例插件 / 原生测试)
get_filename_component(MUSICXX_EXTERN_PLUGIN_SRC_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/.."      ABSOLUTE)
get_filename_component(MUSICXX_EXTERN_PLUGIN_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.."   ABSOLUTE)

# ==================== 宿主库 ====================

# JS 插件运行时: QuickJS 以 add_subdirectory 直接编入宿主库 (plan §11.2),
# 不链接它的命令行/标准库模块 (qjs-libc 不参与链接), 也不开 JIT。
set(MUSICXX_EXTERN_PLUGIN_QUICKJS_DIR "${MUSICXX_EXTERN_PLUGIN_SRC_DIR}/third_party/quickjs")
set(MUSICXX_EXTERN_PLUGIN_JS_AVAILABLE OFF)
if (MUSICXX_EXTERN_PLUGIN_ENABLE_JS)
  if (EXISTS "${MUSICXX_EXTERN_PLUGIN_QUICKJS_DIR}/CMakeLists.txt")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(QJS_BUILD_CLI_STATIC OFF CACHE BOOL "" FORCE)
    set(QJS_BUILD_WERROR OFF CACHE BOOL "" FORCE)
    set(QJS_BUILD_LIBC OFF CACHE BOOL "" FORCE)
    set(QJS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    # EXCLUDE_FROM_ALL: QuickJS 自带的命令行工具/测试可执行文件不参与默认构建
    # (qjs 静态库仍会作为本库的依赖被构建)
    add_subdirectory("${MUSICXX_EXTERN_PLUGIN_QUICKJS_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/quickjs" EXCLUDE_FROM_ALL)
    set(MUSICXX_EXTERN_PLUGIN_JS_AVAILABLE ON)
    message(STATUS "[musicxx_ext] JS 插件运行时: QuickJS (${MUSICXX_EXTERN_PLUGIN_QUICKJS_DIR})")
  else ()
    message(WARNING
      "[musicxx_ext] 找不到 QuickJS 子模块 (${MUSICXX_EXTERN_PLUGIN_QUICKJS_DIR}); "
      "本次构建不含 JS 插件运行时 (请执行 git submodule update --init --recursive)")
  endif ()
endif ()

set(MUSICXX_EXTERN_PLUGIN_HOST_SOURCES
  musicxx_host.cpp
  musicxx_host_tables.cpp
  musicxx_host_ui.cpp
  musicxx_extern_plugin_api.cpp
  js/js_engine.cpp
)

add_library(musicxx_extern_plugin SHARED ${MUSICXX_EXTERN_PLUGIN_HOST_SOURCES})
add_library(musicxx_extern_plugin::host ALIAS musicxx_extern_plugin)

target_include_directories(musicxx_extern_plugin
  PUBLIC  "${MUSICXX_EXTERN_PLUGIN_SRC_DIR}/include"
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
          "${MUSICXX_EXTERN_PLUGIN_SRC_DIR}/sdk/include"
)

target_compile_definitions(musicxx_extern_plugin PRIVATE MUSICXX_EXTERN_PLUGIN_BUILD=1)

if (MUSICXX_EXTERN_PLUGIN_JS_AVAILABLE)
  target_compile_definitions(musicxx_extern_plugin PRIVATE MUSICXX_EXTERN_PLUGIN_HAS_JS=1)
  target_link_libraries(musicxx_extern_plugin PRIVATE qjs)
endif ()

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
  "${MUSICXX_EXTERN_PLUGIN_SRC_DIR}/include"
  "${MUSICXX_EXTERN_PLUGIN_SRC_DIR}/sdk/include"
  "${MUSICXX_EXTERN_PLUGIN_DEP_PREFIX}/include"
)
foreach (_sdkInc "${MUSICXX_EXTERN_PLUGIN_DEP_PREFIX}/include/boost" "${Boost_INCLUDE_DIRS}")
  if (_sdkInc AND EXISTS "${_sdkInc}")
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

# ==================== 官方插件 (含示例) ====================
#
# 源码目录 `plugins/` 放官方实现与示例插件 (每个子目录一个插件); 示例插件的 C++ 版
# 需要参与构建, JS 版是纯脚本 (只做安装/复制)。第三方作者可以参考它们的写法。

if (MUSICXX_EXTERN_PLUGIN_BUILD_EXAMPLE)
  add_subdirectory("${MUSICXX_EXTERN_PLUGIN_ROOT_DIR}/plugins/example_native"
                   "${CMAKE_CURRENT_BINARY_DIR}/example_native")
endif ()

# ==================== 原生测试 ====================

if (MUSICXX_EXTERN_PLUGIN_BUILD_TESTS)
  add_executable(musicxx_extern_plugin_test "${MUSICXX_EXTERN_PLUGIN_SRC_DIR}/tests/test_host.cpp")
  target_include_directories(musicxx_extern_plugin_test PRIVATE "${MUSICXX_EXTERN_PLUGIN_SRC_DIR}/include")
  target_link_libraries(musicxx_extern_plugin_test PRIVATE musicxx_extern_plugin)
  if (MSVC)
    target_compile_options(musicxx_extern_plugin_test PRIVATE /utf-8)
  endif ()
  if (WIN32)
    target_compile_definitions(musicxx_extern_plugin_test PRIVATE _WIN32_WINNT=0x0A00)
  endif ()
endif ()

# ==================== 安装 ====================
#
# 统一装进依赖前缀 (即顶层 superbuild 的安装目录), 便于一次性取用:
#   bin/                        宿主库 (及导入库) 与原生测试
#   include/                    Dart C ABI 头 + 插件 SDK 头
#   plugins/example_native/     示例插件 (库文件 + plugin.yaml, 可直接作为插件目录)
install(TARGETS musicxx_extern_plugin
  RUNTIME DESTINATION bin
  LIBRARY DESTINATION bin
  ARCHIVE DESTINATION lib
)
install(FILES "${MUSICXX_EXTERN_PLUGIN_SRC_DIR}/include/musicxx_extern_plugin_api.h"
  DESTINATION include)
install(DIRECTORY "${MUSICXX_EXTERN_PLUGIN_SRC_DIR}/sdk/include/"
  DESTINATION include)

if (TARGET example_native)
  install(TARGETS example_native RUNTIME DESTINATION plugins/example_native LIBRARY DESTINATION plugins/example_native)
  install(FILES "${MUSICXX_EXTERN_PLUGIN_ROOT_DIR}/plugins/example_native/plugin.yaml"
    DESTINATION plugins/example_native)
endif ()

# JS 插件示例: 纯脚本 + 清单, 无需编译
if (EXISTS "${MUSICXX_EXTERN_PLUGIN_ROOT_DIR}/plugins/example_js/plugin.yaml")
  install(DIRECTORY "${MUSICXX_EXTERN_PLUGIN_ROOT_DIR}/plugins/example_js/"
    DESTINATION plugins/example_js)
endif ()

# 原生测试用的插件夹具 (含故意损坏的插件, 用于验证失败降级)
if (EXISTS "${MUSICXX_EXTERN_PLUGIN_SRC_DIR}/tests/plugins/")
  install(DIRECTORY "${MUSICXX_EXTERN_PLUGIN_SRC_DIR}/tests/plugins/"
    DESTINATION plugins)
endif ()

if (TARGET musicxx_extern_plugin_test)
  install(TARGETS musicxx_extern_plugin_test RUNTIME DESTINATION bin)
endif ()
