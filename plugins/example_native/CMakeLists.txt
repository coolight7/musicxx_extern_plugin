# 示例原生插件 (musicxx_extern_plugin/example/example_native)
#
# 只导出 musicxx_plugin_* 入口符号 (由 MUSICXX_PLUGIN_EXPORT 生成);
# 头文件依赖由宿主构建提供的 INTERFACE 目标 musicxx_extern_plugin_sdk 带给插件。
cmake_minimum_required(VERSION 3.20)
project(example_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED OFF)

add_library(example_native SHARED example_native.cpp)

target_link_libraries(example_native PRIVATE musicxx_extern_plugin_sdk cxx_pluginxx_static fmt::fmt)

if (WIN32)
  target_compile_definitions(example_native PRIVATE _WIN32_WINNT=0x0A00)
endif ()

# 插件只导出入口符号 (其余符号默认隐藏; MSVC 侧靠 __declspec(dllexport) 显式声明)
set_target_properties(example_native PROPERTIES
  PREFIX ""
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)
