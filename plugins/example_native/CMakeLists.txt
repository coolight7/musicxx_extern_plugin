# 示例原生插件 (musicxx_extern_plugin/plugins/example_native)
#
# 这是插件作者的最小参考：CMakeLists 只写几行 ——
#   1) 引入 SDK 构建助手（宿主工程内部直接 include；外部作者用 find_package 后再拿）；
#   2) 调 musicxx_plugin_add_target 建库并声明清单。
#
# 助手负责：链接 SDK 与内核/工具库、只导出 `musicxx_plugin_*` 入口符号（GNU/Clang 用
# version script、Apple 用导出符号表）、把 `plugin.yaml` 复制到产物目录旁边
# （因此构建目录可以直接当作"插件目录"）。详见 `src/sdk/cmake/musicxx_plugin.cmake`。
cmake_minimum_required(VERSION 3.20)
project(example_native LANGUAGES CXX)

if (DEFINED MUSICXX_EXTERN_PLUGIN_SRC_DIR)
  include("${MUSICXX_EXTERN_PLUGIN_SRC_DIR}/sdk/cmake/musicxx_plugin.cmake")
else ()
  include("${CMAKE_CURRENT_LIST_DIR}/../../src/sdk/cmake/musicxx_plugin.cmake")
endif ()

musicxx_plugin_add_target(example_native
  SOURCES example_native.cpp
  MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
)
