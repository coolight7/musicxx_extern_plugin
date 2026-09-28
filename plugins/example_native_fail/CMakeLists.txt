# 对照示例动态库插件 (musicxx_extern_plugin/plugins/example_native_fail)
#
# 与 example_native 用同一个构建助手：建库、链接 SDK 与内核/工具库、只导出
# `musicxx_plugin_*` 入口符号、把 `plugin.yaml` 复制到产物目录旁边。
cmake_minimum_required(VERSION 3.20)
project(example_native_fail LANGUAGES CXX)

if (DEFINED MUSICXX_EXTERN_PLUGIN_SRC_DIR)
  include("${MUSICXX_EXTERN_PLUGIN_SRC_DIR}/sdk/cmake/musicxx_plugin.cmake")
else ()
  include("${CMAKE_CURRENT_LIST_DIR}/../../src/sdk/cmake/musicxx_plugin.cmake")
endif ()

musicxx_plugin_add_target(example_native_fail
  SOURCES example_native_fail.cpp
  MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
)
