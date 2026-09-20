# musicxx_extern_plugin 的 Linux 平台工程（Flutter FFI 插件）
#
# 与 windows/CMakeLists.txt 同一套路：宿主库**不在 Flutter 构建里编译**。
# libmusicxx_extern_plugin.so 由包内 tools/build_native.sh 的 superbuild 独立构建
# （依赖链含 fmt / yaml-cpp / simdjson / libiconv / uchardet / QuickJS，见方案 §11.2），
# 因此本文件只做两件事：
#   1. 按约定目录找到"已经构建好的"宿主库；
#   2. 把它交给 Flutter 的 PLUGIN_BUNDLED_LIBRARIES，随应用一起分发到 <bundle>/lib/
#      （应用侧 `set(CMAKE_INSTALL_RPATH "$ORIGIN/lib")`，Dart 侧会去可执行文件旁的
#      `lib/` 目录找库，见 lib/src/native_library.dart 的候选顺序）。
#
# 找不到宿主库时只打警告、不中断构建：外部插件是可选功能，缺失时应用照常启动，
# 由 Dart 侧给出"宿主库缺失"的提示（ExternPluginStore 的加载失败路径）。
# 需要"必须打包"的场景（发布流水线）请显式传 -DMUSICXX_EXTERN_PLUGIN_HOST_LIBRARY=<路径>，
# 缺失时由 CI 自行判定失败。

cmake_minimum_required(VERSION 3.14)

project(musicxx_extern_plugin LANGUAGES CXX)

# 显式指定的宿主库路径（最高优先级：便于 CI 与自定义目录）
set(MUSICXX_EXTERN_PLUGIN_HOST_LIBRARY "" CACHE FILEPATH
    "已构建好的 libmusicxx_extern_plugin 宿主库路径（留空则按约定目录自动查找）")

get_filename_component(_mx_pkg_dir "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
get_filename_component(_mx_repo_root "${_mx_pkg_dir}/../.." ABSOLUTE)

set(_mx_lib_name "libmusicxx_extern_plugin.so")

# 架构标签（与 tools/build_native.sh 的 <平台>-<架构>-<配置> 目录命名一致）
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  set(_mx_arch_tag "arm64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(armv7|armv7l|armhf)$")
  set(_mx_arch_tag "arm")
else()
  set(_mx_arch_tag "x64")
endif()

# 约定目录（与 tools/build_native.sh、Dart 侧 native_library.dart 同一套顺序）：
#   1. 环境变量 MUSICXX_EXTERN_PLUGIN_LIBRARY（与 Dart 侧同一个变量名）
#   2. <包>/.native/output/linux-<架构>-<配置>/bin/                       便携输出目录
#   3. <包>/.native/build/linux-<配置>/musicxx-extern-plugin-install/bin/   安装前缀
#   4. <仓库>/resource/libs/linux/lib/                                    与 libmediaxx.so 同一约定
set(_mx_env_library "$ENV{MUSICXX_EXTERN_PLUGIN_LIBRARY}")
set(_mx_out_debug "${_mx_pkg_dir}/.native/output/linux-${_mx_arch_tag}-debug/bin/${_mx_lib_name}")
set(_mx_out_release "${_mx_pkg_dir}/.native/output/linux-${_mx_arch_tag}-release/bin/${_mx_lib_name}")
set(_mx_install_debug "${_mx_pkg_dir}/.native/build/linux-debug/musicxx-extern-plugin-install/bin/${_mx_lib_name}")
set(_mx_install_release "${_mx_pkg_dir}/.native/build/linux-release/musicxx-extern-plugin-install/bin/${_mx_lib_name}")
set(_mx_repo_library "${_mx_repo_root}/resource/libs/linux/lib/${_mx_lib_name}")

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

# 该包没有自己的编译目标：宿主库是外部构建产物，只参与"随包分发"
if(_mx_host_library)
  set(musicxx_extern_plugin_bundled_libraries "${_mx_host_library}" PARENT_SCOPE)
  message(STATUS "[musicxx_extern_plugin] 宿主库将随应用分发: ${_mx_host_library}")
else()
  message(WARNING
      "[musicxx_extern_plugin] 未找到外部插件宿主库（libmusicxx_extern_plugin.so），"
      "本次构建不会把它打包进应用：外部插件功能在运行期不可用。\n"
      "  构建宿主库: bash ${_mx_pkg_dir}/tools/build_native.sh\n"
      "  或显式指定: -DMUSICXX_EXTERN_PLUGIN_HOST_LIBRARY=<库路径>\n"
      "  查找过的位置: ${_mx_out_debug} | ${_mx_out_release} | ${_mx_install_debug} | ${_mx_install_release} | ${_mx_repo_library}")
  set(musicxx_extern_plugin_bundled_libraries "" PARENT_SCOPE)
endif()
