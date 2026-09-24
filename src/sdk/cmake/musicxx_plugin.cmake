# musicxx 外部插件 SDK 的 CMake 助手（plan §7.1）
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
# 说明：这里只做构建便利，不改变插件契约（入口符号名、清单字段见 `docs/plugin-hooks.md`
# 与 `README.md`）。入口符号前缀默认 `musicxx_plugin`，与宿主 `entrySymbols()` 一致。

if (COMMAND musicxx_plugin_add_target)
  return() # 已引入（宿主工程里可能被多次 include）
endif ()

set(MUSICXX_PLUGIN_ENTRY_PREFIX "musicxx_plugin"
    CACHE STRING "插件入口符号前缀（与宿主 entrySymbols() 一致，不要改）")

# musicxx_plugin_add_target(<目标> SOURCES ... [MANIFEST plugin.yaml]
#                           [ASSETS <文件或目录> ...] [OUTPUT_DIR <目录>])
#
# SOURCES  插件源码（至少一个）
# MANIFEST 清单文件（会被复制到产物目录旁的 plugin.yaml）
# ASSETS   需要随插件分发的资源（目录或文件）：复制到产物目录下**同名**位置，
#          例如 ASSETS shader 会把 `shader/` 整个目录放到产物目录里，
#          插件清单里的相对路径（如 `shader/bg.shaderbundle`）就能直接解析
# OUTPUT_DIR 产物目录（默认当前构建目录）
function(musicxx_plugin_add_target target)
  cmake_parse_arguments(ARG "" "MANIFEST;OUTPUT_DIR" "SOURCES;LIBRARIES;ASSETS"
                        ${ARGN})
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
              "${ARG_MANIFEST}" "$<TARGET_FILE_DIR:${target}>/plugin.yaml"
      COMMENT "复制插件清单 plugin.yaml 到产物目录")
  endif ()

  # ---- 资源（例如 shader bundle 目录）：按原名复制到产物目录 ----
  foreach (_asset ${ARG_ASSETS})
    if (NOT EXISTS "${_asset}")
      message(FATAL_ERROR "musicxx_plugin_add_target(${target}): 资源不存在: ${_asset}")
    endif ()
    get_filename_component(_asset_name "${_asset}" NAME)
    if (IS_DIRECTORY "${_asset}")
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${_asset}" "$<TARGET_FILE_DIR:${target}>/${_asset_name}"
        COMMENT "复制插件资源目录 ${_asset_name}/ 到产物目录")
    else ()
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_asset}" "$<TARGET_FILE_DIR:${target}>/${_asset_name}"
        COMMENT "复制插件资源 ${_asset_name} 到产物目录")
    endif ()
  endforeach ()
  unset(_asset)
  unset(_asset_name)
endfunction()
