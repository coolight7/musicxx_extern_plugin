# musicxx_extern_plugin 的 Windows 平台工程（Flutter FFI 插件）
#
# 这个包与普通 FFI 插件不同：插件框架库不是普通的"插件源码编译"，而是一条独立的依赖链
# （fmt / yaml-cpp / simdjson / libiconv / uchardet / QuickJS 都由包内构建脚本从
# src/third_party 的源码构建，见 tools/build_native.ps1）。因此本文件做三件事：
#   1. 按约定目录自动触发插件框架库构建（已是最新则秒退，见 tools/cmake/host_autobuild.cmake）；
#   2. 按约定目录找到"已经构建好的"插件框架库；
#   3. 把它交给 Flutter 的 PLUGIN_BUNDLED_LIBRARIES，随应用一起分发到可执行文件旁边。
#
# 找不到插件框架库时只打警告、不中断构建：外部插件是可选功能，缺失时主程序照常启动，
# 由 Dart 侧给出"插件框架库缺失"的提示（ExternPluginStore 的加载失败路径）。
# 需要"必须打包"的场景（发布流水线）请显式传 -DMUSICXX_EXTERN_PLUGIN_HOST_LIBRARY=<路径>，
# 并把 -DMUSICXX_EXTERN_PLUGIN_AUTO_BUILD=OFF 关掉自动构建，缺失时由 CI 自行判定失败。

cmake_minimum_required(VERSION 3.14)

project(musicxx_extern_plugin LANGUAGES CXX)

# 显式指定的插件框架库路径（最高优先级：便于 CI 与自定义目录）
set(MUSICXX_EXTERN_PLUGIN_HOST_LIBRARY "" CACHE FILEPATH
    "已构建好的 musicxx_extern_plugin 插件框架库路径（留空则按约定目录自动查找）")

get_filename_component(_mx_pkg_dir "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
get_filename_component(_mx_repo_root "${_mx_pkg_dir}/../.." ABSOLUTE)

set(_mx_lib_name "musicxx_extern_plugin.dll")

# 生成器平台名（VS 的 -A 值：x64 / ARM64 / Win32）→ 构建脚本使用的架构标签
if(CMAKE_GENERATOR_PLATFORM)
  string(TOLOWER "${CMAKE_GENERATOR_PLATFORM}" _mx_gen_platform)
else()
  set(_mx_gen_platform "")
endif()
if(_mx_gen_platform STREQUAL "arm64")
  set(_mx_arch_tag "arm64")
elseif(_mx_gen_platform STREQUAL "win32")
  set(_mx_arch_tag "x86")
else()
  set(_mx_arch_tag "x64")
endif()

# 约定目录（与 tools/build_native.ps1、Dart 侧 native_library.dart 同一套顺序）：
#   1. 环境变量 MUSICXX_EXTERN_PLUGIN_LIBRARY（与 Dart 侧同一个变量名）
#   2. <包>/.native/output/windows-<架构>-<配置>/bin/            便携输出目录
#   3. <包>/.native/build/windows-<配置>/musicxx-extern-plugin-install/bin/   安装前缀
#   4. <仓库>/resource/libs/windows/lib/                        与 libmediaxx.dll 同一约定
set(_mx_env_library "$ENV{MUSICXX_EXTERN_PLUGIN_LIBRARY}")
set(_mx_out_debug "${_mx_pkg_dir}/.native/output/windows-${_mx_arch_tag}-debug/bin/${_mx_lib_name}")
set(_mx_out_release "${_mx_pkg_dir}/.native/output/windows-${_mx_arch_tag}-release/bin/${_mx_lib_name}")
set(_mx_install_debug "${_mx_pkg_dir}/.native/build/windows-debug/musicxx-extern-plugin-install/bin/${_mx_lib_name}")
set(_mx_install_release "${_mx_pkg_dir}/.native/build/windows-release/musicxx-extern-plugin-install/bin/${_mx_lib_name}")
set(_mx_repo_library "${_mx_repo_root}/resource/libs/windows/lib/${_mx_lib_name}")

# 构建主程序时自动触发插件框架库构建 (已是最新则秒级返回; 关掉: -DMUSICXX_EXTERN_PLUGIN_AUTO_BUILD=OFF)
# 必须在下面的"查找插件框架库"之前: 查找结果要被交给 PLUGIN_BUNDLED_LIBRARIES, 装包时文件必须在位。
option(MUSICXX_EXTERN_PLUGIN_AUTO_BUILD "构建主程序时自动构建外部插件框架库（已是最新则跳过）" ON)
include("${_mx_pkg_dir}/tools/cmake/host_autobuild.cmake")
musicxx_extern_plugin_autobuild_host(
  PACKAGE_DIR      "${_mx_pkg_dir}"
  PLATFORM         windows
  ARCH_TAG         "${_mx_arch_tag}"
  LIBRARY_NAME     "${_mx_lib_name}"
  EXPLICIT_LIBRARY "${MUSICXX_EXTERN_PLUGIN_HOST_LIBRARY}"
)

set(_mx_host_library "")

# 1) 显式路径（写错就直接报错：CI 里"必须打包"的场景不要静默降级成"没打包"）
if(MUSICXX_EXTERN_PLUGIN_HOST_LIBRARY)
  if(EXISTS "${MUSICXX_EXTERN_PLUGIN_HOST_LIBRARY}")
    set(_mx_host_library "${MUSICXX_EXTERN_PLUGIN_HOST_LIBRARY}")
  else()
    message(FATAL_ERROR
      "[musicxx_extern_plugin] MUSICXX_EXTERN_PLUGIN_HOST_LIBRARY 指定的库不存在: "
      "${MUSICXX_EXTERN_PLUGIN_HOST_LIBRARY}")
  endif()
endif()
# 2) 环境变量（与 Dart 侧一致）
if(NOT _mx_host_library AND _mx_env_library AND EXISTS "${_mx_env_library}")
  set(_mx_host_library "${_mx_env_library}")
endif()
# 3) 便携输出目录 / 安装前缀：Debug 与 Release 都在时按当前构建配置选择
if(NOT _mx_host_library)
  set(_mx_have_debug FALSE)
  set(_mx_have_release FALSE)
  if(EXISTS "${_mx_out_debug}" OR EXISTS "${_mx_install_debug}")
    set(_mx_have_debug TRUE)
  endif()
  if(EXISTS "${_mx_out_release}" OR EXISTS "${_mx_install_release}")
    set(_mx_have_release TRUE)
  endif()
  if(_mx_have_debug AND _mx_have_release)
    # Profile 不是原生构建的配置：与 Release 同一份（构建脚本只产出 Debug / Release）
    set(_mx_host_library
        "$<IF:$<CONFIG:Debug>,${_mx_out_debug},${_mx_out_release}>")
  elseif(_mx_have_release)
    if(EXISTS "${_mx_out_release}")
      set(_mx_host_library "${_mx_out_release}")
    else()
      set(_mx_host_library "${_mx_install_release}")
    endif()
  elseif(_mx_have_debug)
    if(EXISTS "${_mx_out_debug}")
      set(_mx_host_library "${_mx_out_debug}")
    else()
      set(_mx_host_library "${_mx_install_debug}")
    endif()
  endif()
endif()
# 4) 仓库内的预置库目录
if(NOT _mx_host_library AND EXISTS "${_mx_repo_library}")
  set(_mx_host_library "${_mx_repo_library}")
endif()

# 该包没有自己的编译目标：插件框架库是外部构建产物，只参与"随包分发"
if(_mx_host_library)
  set(musicxx_extern_plugin_bundled_libraries "${_mx_host_library}" PARENT_SCOPE)
  message(STATUS "[musicxx_extern_plugin] 插件框架库将随主程序分发: ${_mx_host_library}")
else()
  message(WARNING
      "[musicxx_extern_plugin] 未找到外部插件框架库（musicxx_extern_plugin.dll），"
      "本次构建不会把它打包进主程序：外部插件功能在运行期不可用。\n"
      "  构建插件框架库: pwsh -NoProfile -File ${_mx_pkg_dir}/tools/build_native.ps1\n"
      "  (主程序构建默认会自动触发它; 若已用 -DMUSICXX_EXTERN_PLUGIN_AUTO_BUILD=OFF 关掉, 请手工跑)\n"
      "  或显式指定: -DMUSICXX_EXTERN_PLUGIN_HOST_LIBRARY=<库路径>\n"
      "  查找过的位置: ${_mx_out_debug} | ${_mx_out_release} | ${_mx_install_debug} | ${_mx_install_release} | ${_mx_repo_library}")
  set(musicxx_extern_plugin_bundled_libraries "" PARENT_SCOPE)
endif()
