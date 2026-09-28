# 多目标打包示例插件
#
# 与其它示例的区别只有一处：调 `musicxx_plugin_add_target` 时多传 `TARGET_TAG auto`，
# SDK 助手会把库文件放进 `<构建目录>/package/lib/<系统>-<架构>/`，清单放在
# `<构建目录>/package/`。于是：
#
#   cmake -S . -B build -DMUSICXX_EXTERN_PLUGIN_SRC_DIR=<包目录>/src
#   cmake --build build --config Release
#   → build/package/plugin.yaml
#     build/package/lib/windows-x64/example_native_multi.dll
#
# 换一个平台/架构（或在有 NDK 的机器上交叉编译 Android）各构建一次，把各次的
# `lib/<标签>/` 目录合并进同一个 package 目录，就得到一个通用插件包（压缩成 zip
# 后即可在应用里「从压缩包安装」）；宿主运行时会按当前系统/CPU 架构选分支。
cmake_minimum_required(VERSION 3.20)
project(example_native_multi LANGUAGES CXX)

if (DEFINED MUSICXX_EXTERN_PLUGIN_SRC_DIR)
  include("${MUSICXX_EXTERN_PLUGIN_SRC_DIR}/sdk/cmake/musicxx_plugin.cmake")
else ()
  include("${CMAKE_CURRENT_LIST_DIR}/../../src/sdk/cmake/musicxx_plugin.cmake")
endif ()

musicxx_plugin_add_target(example_native_multi
  SOURCES example_native_multi.cpp
  MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
  # 按当前构建目标推导分支标签，并把库文件放进 <包目录>/lib/<标签>/
  TARGET_TAG auto
)
