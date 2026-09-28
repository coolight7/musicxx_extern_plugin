# musicxx 外部插件 SDK 的 CMake 助手
#
# 用法（插件作者的 CMakeLists.txt）：
# ```cmake
# cmake_minimum_required(VERSION 3.20)
# project(my_plugin LANGUAGES CXX)
#
# # 方式一：用已安装的 SDK 前缀（推荐）
# find_package(musicxx_extern_plugin CONFIG REQUIRED)
# # 方式二：在宿主工程内部（musicxx_extern_plugin_sdk 目标已存在）
# include("${MUSICXX_EXTERN_PLUGIN_SRC_DIR}/sdk/cmake/musicxx_plugin.cmake")
#
# musicxx_plugin_add_target(my_plugin SOURCES my_plugin.cpp MANIFEST plugin.yaml)
# ```
#
# 它负责三件事（插件作者不用自己拼）：
# 1. 建 SHARED 库并链接 SDK（`musicxx_extern_plugin_sdk`）与内核/工具库（只链接存在的目标）；
# 2. **只导出入口符号**：MSVC 靠 `__declspec(dllexport)`（SDK 的导出宏负责），
#    GNU/Clang 用"默认隐藏 + version script"，Apple 用 `-exported_symbols_list`；
# 3. 把 `plugin.yaml` 复制到产物目录旁边，使该目录可以直接作为"插件目录"使用。
#
# 说明：这里只做构建便利，不改变插件契约（入口符号名见 `docs/plugin-native-api.md`，
# 清单字段与构建流程也在那一篇）。入口符号前缀默认 `musicxx_plugin`，与宿主 `entrySymbols()` 一致。

if (COMMAND musicxx_plugin_add_target)
  return() # 已引入（宿主工程里可能被多次 include）
endif ()

set(MUSICXX_PLUGIN_ENTRY_PREFIX "musicxx_plugin"
    CACHE STRING "插件入口符号前缀（与宿主 entrySymbols() 一致，不要改）")

# 多目标打包：分支目录名（默认 lib，与清单 targets_dir 的缺省值一致）
set(MUSICXX_PLUGIN_LIB_DIR_NAME "lib"
    CACHE STRING "多目标打包时分支目录的名字（缺省 lib）")

# 当前构建目标的标签（如 windows-x64 / linux-arm64 / android-arm64-v8a）
#
# 与宿主的标签解析（alias 表，见 src/host/host_target.h）对得上：
# - 系统名用规范名（windows / linux / macos / android / ios）；
# - Android 直接用 ABI 名（arm64-v8a / armeabi-v7a / x86_64 / x86），与 APK 的
#   `lib/<abi>/` 写法一致；其余平台用规范架构名（x64 / x86 / arm64 / armv7 …）。
function(musicxx_plugin_target_tag_auto out_var)
  set(_os "")
  if (ANDROID)
    set(_os "android")
  elseif (WIN32)
    set(_os "windows")
  elseif (APPLE AND CMAKE_SYSTEM_NAME STREQUAL "iOS")
    set(_os "ios")
  elseif (APPLE)
    set(_os "macos")
  elseif (UNIX)
    set(_os "linux")
  endif ()

  set(_arch "")
  if (ANDROID AND ANDROID_ABI)
    set(_arch "${ANDROID_ABI}")
  else ()
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _proc)
    if (_proc MATCHES "^(x86_64|amd64|x64)$")
      set(_arch "x64")
    elseif (_proc MATCHES "^(i[3-6]86|x86|ia32)$")
      set(_arch "x86")
    elseif (_proc MATCHES "^(aarch64|arm64|armv8[al]?)$")
      set(_arch "arm64")
    elseif (_proc MATCHES "^(riscv64.*)$")
      set(_arch "riscv64")
    elseif (_proc MATCHES "^(loongarch64|loong64)$")
      set(_arch "loongarch64")
    elseif (_proc MATCHES "^arm")
      set(_arch "armv7")
    elseif (CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(_arch "x64")
    elseif (CMAKE_SIZEOF_VOID_P EQUAL 4)
      set(_arch "x86")
    endif ()
    unset(_proc)
  endif ()

  if (_os STREQUAL "" OR _arch STREQUAL "")
    set(${out_var} "" PARENT_SCOPE)
  else ()
    set(${out_var} "${_os}-${_arch}" PARENT_SCOPE)
  endif ()
endfunction()

# musicxx_plugin_add_target(<目标> SOURCES ... [MANIFEST plugin.yaml]
#                           [ASSETS <文件或目录> ...] [OUTPUT_DIR <目录>]
#                           [TARGET_TAG <标签|auto>] [PACKAGE_DIR <目录>])
#
# SOURCES  插件源码（至少一个）
# MANIFEST 清单文件（会被复制到产物目录旁的 plugin.yaml）
# ASSETS   需要随插件分发的资源（目录或文件）：复制到产物目录下**同名**位置，
#          例如 ASSETS shader 会把 `shader/` 整个目录放到产物目录里，
#          插件清单里的相对路径（如 `shader/bg.shaderbundle`）就能直接解析
# OUTPUT_DIR 产物目录（默认当前构建目录）
#
# TARGET_TAG / PACKAGE_DIR：**多目标打包**（仿 APK 的 lib/<系统>-<架构>/）
#   - 传 TARGET_TAG（`auto` = 按当前构建目标推导，如 windows-x64 / android-arm64-v8a）时，
#     库文件放进 `<PACKAGE_DIR>/lib/<标签>/`，清单与 ASSETS 放进 `<PACKAGE_DIR>/`；
#     同一个 PACKAGE_DIR 可以被多次构建（不同系统/架构）共用，产出一个含多个分支的
#     插件包（每个分支一个子目录），运行时宿主按当前系统/架构选分支加载；
#   - PACKAGE_DIR 缺省为 `<当前构建目录>/package`；不传 TARGET_TAG 时行为与以前一致
#     （库与清单放在同一层，直接作为插件目录）。
function(musicxx_plugin_add_target target)
  cmake_parse_arguments(ARG "" "MANIFEST;OUTPUT_DIR;TARGET_TAG;PACKAGE_DIR"
                        "SOURCES;LIBRARIES;ASSETS" ${ARGN})
  if (NOT ARG_SOURCES)
    message(FATAL_ERROR "musicxx_plugin_add_target(${target}): 必须提供 SOURCES")
  endif ()

  add_library(${target} SHARED ${ARG_SOURCES})

  # ---- 依赖：SDK（包含目录 + 平台宏）与内核/工具库 ----
  #
  # 内核 kit 是 header-only，但示例与多数插件会用 `utilxx_base::Json` 解析载荷，
  # 因此把这些库作为"存在就链接"的可选依赖，插件作者不需要知道具体名字。
  if (TARGET musicxx_extern_plugin_sdk)
    target_link_libraries(${target} PRIVATE musicxx_extern_plugin_sdk)
  elseif (TARGET musicxx_extern_plugin::sdk)
    target_link_libraries(${target} PRIVATE musicxx_extern_plugin::sdk)
  elseif (DEFINED MUSICXX_EXTERN_PLUGIN_SDK_INCLUDE_DIRS)
    target_include_directories(${target} PRIVATE ${MUSICXX_EXTERN_PLUGIN_SDK_INCLUDE_DIRS})
    target_compile_definitions(${target} PRIVATE
      BOOST_ASIO_STANDALONE=1 ASIO_STANDALONE=1 UTILXX_USE_BOOST_ASIO=1)
  else ()
    message(FATAL_ERROR
      "musicxx_plugin_add_target(${target}): 找不到插件 SDK（先 find_package(musicxx_extern_plugin CONFIG) 或传入 MUSICXX_EXTERN_PLUGIN_SDK_INCLUDE_DIRS）")
  endif ()

  foreach (_optional cxx_pluginxx_static cxx_utilxx_base_static fmt::fmt)
    if (TARGET ${_optional})
      target_link_libraries(${target} PRIVATE ${_optional})
    endif ()
  endforeach ()
  foreach (_extra ${ARG_LIBRARIES})
    target_link_libraries(${target} PRIVATE ${_extra})
  endforeach ()
  unset(_optional)
  unset(_extra)

  # ---- 编译设置（内核要求 C++26；源码为 UTF-8）----
  set_target_properties(${target} PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED OFF
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
  )
  if (MSVC)
    target_compile_options(${target} PRIVATE /utf-8)
  endif ()
  if (WIN32)
    target_compile_definitions(${target} PRIVATE _WIN32_WINNT=0x0A00)
  endif ()

  # ---- 只导出入口符号（非 MSVC）----
  #
  # MSVC 不需要额外处理：`__declspec(dllexport)` 已经限定了导出面。
  # 这里给 GNU/Clang 加 version script、给 Apple 加导出符号表，避免把内核/工具库的
  # 符号（或 C++ 运行时符号）暴露出去，也避免与宿主/其它插件互相干扰。
  if (UNIX AND NOT APPLE)
    set(_map "${CMAKE_CURRENT_BINARY_DIR}/${target}.map")
    file(WRITE "${_map}"
      "{\n  global:\n    ${MUSICXX_PLUGIN_ENTRY_PREFIX}_*;\n  local:\n    *;\n};\n")
    target_link_options(${target} PRIVATE "-Wl,--version-script=${_map}")
  elseif (APPLE)
    set(_exp "${CMAKE_CURRENT_BINARY_DIR}/${target}.exp")
    file(WRITE "${_exp}"
      "_${MUSICXX_PLUGIN_ENTRY_PREFIX}_get_info\n"
      "_${MUSICXX_PLUGIN_ENTRY_PREFIX}_create\n"
      "_${MUSICXX_PLUGIN_ENTRY_PREFIX}_start\n"
      "_${MUSICXX_PLUGIN_ENTRY_PREFIX}_stop\n"
      "_${MUSICXX_PLUGIN_ENTRY_PREFIX}_destroy\n")
    target_link_options(${target} PRIVATE "-Wl,-exported_symbols_list,${_exp}")
  endif ()

  # ---- 产物与清单放在同一层（多配置生成器也不分子目录），便于直接作为插件目录 ----
  set(_out "${ARG_OUTPUT_DIR}")
  if (NOT _out)
    set(_out "${CMAKE_CURRENT_BINARY_DIR}")
  endif ()

  # 多目标打包：库文件进 <PACKAGE_DIR>/lib/<标签>/，清单与资源留在 <PACKAGE_DIR>/
  set(_tag "${ARG_TARGET_TAG}")
  if (_tag STREQUAL "auto")
    musicxx_plugin_target_tag_auto(_tag)
    if (_tag STREQUAL "")
      message(FATAL_ERROR
        "musicxx_plugin_add_target(${target}): TARGET_TAG auto 推导失败 "
        "(未知的系统 ${CMAKE_SYSTEM_NAME} / 架构 ${CMAKE_SYSTEM_PROCESSOR})")
    endif ()
  endif ()
  set(_packageDir "")
  set(_manifestDir "${_out}")
  if (NOT _tag STREQUAL "")
    if (ARG_PACKAGE_DIR)
      set(_packageDir "${ARG_PACKAGE_DIR}")
    else ()
      set(_packageDir "${CMAKE_CURRENT_BINARY_DIR}/package")
    endif ()
    set(_out "${_packageDir}/${MUSICXX_PLUGIN_LIB_DIR_NAME}/${_tag}")
    set(_manifestDir "${_packageDir}")
    # 让宿主/父工程知道"这个插件的产物是一个多目标包"（安装整包而不是单个库）
    set_property(GLOBAL PROPERTY "musicxx_plugin_package_${target}"
                 "${_packageDir}")
    # 插件可以据此知道"本份构建属于哪个分支"（日志/自检用）
    target_compile_definitions(${target} PRIVATE
      MUSICXX_PLUGIN_BUILD_TAG="${_tag}")
    message(STATUS
      "[musicxx_plugin] ${target}: 多目标打包, 分支 ${_tag} → ${_out} (包目录 ${_packageDir})")
  endif ()

  set_target_properties(${target} PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${_out}"
    RUNTIME_OUTPUT_DIRECTORY "${_out}"
  )
  foreach (_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL PROFILE)
    set_target_properties(${target} PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${_out}"
      RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${_out}"
    )
  endforeach ()
  unset(_cfg)

  if (ARG_MANIFEST)
    if (NOT EXISTS "${ARG_MANIFEST}")
      message(FATAL_ERROR "musicxx_plugin_add_target(${target}): 清单不存在: ${ARG_MANIFEST}")
    endif ()
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND "${CMAKE_COMMAND}" -E copy_if_different
              "${ARG_MANIFEST}" "${_manifestDir}/plugin.yaml"
      COMMENT "复制插件清单 plugin.yaml 到插件目录")
  endif ()

  # ---- 资源（例如 shader bundle 目录）：按原名复制到插件目录 ----
  #
  # 多目标打包时资源放在包目录（与分支平级，所有分支共用），清单里的相对路径照旧。
  foreach (_asset ${ARG_ASSETS})
    if (NOT EXISTS "${_asset}")
      message(FATAL_ERROR "musicxx_plugin_add_target(${target}): 资源不存在: ${_asset}")
    endif ()
    get_filename_component(_asset_name "${_asset}" NAME)
    if (IS_DIRECTORY "${_asset}")
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${_asset}" "${_manifestDir}/${_asset_name}"
        COMMENT "复制插件资源目录 ${_asset_name}/ 到插件目录")
    else ()
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_asset}" "${_manifestDir}/${_asset_name}"
        COMMENT "复制插件资源 ${_asset_name} 到插件目录")
    endif ()
  endforeach ()
  unset(_asset)
  unset(_asset_name)
endfunction()
