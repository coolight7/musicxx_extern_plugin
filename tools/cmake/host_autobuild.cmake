# musicxx_extern_plugin 插件框架库自动构建 (被 windows/CMakeLists.txt 与 linux/CMakeLists.txt include)
#
# 作用: 让 `flutter build <平台>` / `flutter run -d <平台>` 不必先手工跑构建脚本 —— 平台工程在
# configure 期调用包内构建脚本的自动模式 (`-Auto` / `--auto`), 由脚本自己判断
# "产物已是最新就秒退、环境没配置过就现场配置", 因此重复构建几乎没有额外开销。
#
# 为什么放在 configure 期: 平台工程要把"已经存在的插件框架库路径"交给 Flutter 的
# PLUGIN_BUNDLED_LIBRARIES, 而主程序侧的 install(FILES ...) 在构建末尾要求文件已经在位;
# 放到 build 期的自定义目标会来不及 (装的时候文件还不存在)。
#
# 关掉自动构建 (任一即可):
#   -DMUSICXX_EXTERN_PLUGIN_AUTO_BUILD=OFF      平台工程声明的 CMake 变量
#   $env:MUSICXX_EXTERN_PLUGIN_AUTO_BUILD=0     环境变量 (CI 常用)
# 目标配置: 默认 Release; 需要 Debug 插件框架库时设 $env:MUSICXX_EXTERN_PLUGIN_CONFIG=Debug。
# 已经显式指定插件框架库 (-DMUSICXX_EXTERN_PLUGIN_HOST_LIBRARY 或环境变量
# MUSICXX_EXTERN_PLUGIN_LIBRARY) 时不自动构建: 那种情况下用哪份库已经由使用者决定。
#
# 构建失败不中断应用构建 (外部插件是可选功能): 脚本自己在自动模式下就是"失败只警告"，
# 这里再兜一层, 确认库到底有没有产出。

# musicxx_extern_plugin_autobuild_host(PACKAGE_DIR <包目录> PLATFORM <windows|linux|macos>
#                                      ARCH_TAG <x64|arm64|x86|arm> LIBRARY_NAME <库文件名>
#                                      [DISPLAY_NAME <提示用名字>] [EXPLICIT_LIBRARY <显式路径>])
function(musicxx_extern_plugin_autobuild_host)
  cmake_parse_arguments(MX "" "PACKAGE_DIR;PLATFORM;ARCH_TAG;LIBRARY_NAME;DISPLAY_NAME;EXPLICIT_LIBRARY" "" ${ARGN})

  set(_mx_display "${MX_DISPLAY_NAME}")
  if(NOT _mx_display)
    set(_mx_display "${MX_LIBRARY_NAME}")
  endif()

  if(NOT MX_PACKAGE_DIR OR NOT EXISTS "${MX_PACKAGE_DIR}/tools")
    return()
  endif()

  # 已经有人指定了用哪份库: 不再自动构建
  if(MX_EXPLICIT_LIBRARY OR "$ENV{MUSICXX_EXTERN_PLUGIN_LIBRARY}")
    message(STATUS "[musicxx_extern_plugin] 插件框架库已显式指定, 跳过自动构建")
    return()
  endif()

  # 开关: CMake 变量 (平台工程里的 option) 与环境变量
  set(_mx_enabled "${MUSICXX_EXTERN_PLUGIN_AUTO_BUILD}")
  if(DEFINED ENV{MUSICXX_EXTERN_PLUGIN_AUTO_BUILD})
    string(TOLOWER "$ENV{MUSICXX_EXTERN_PLUGIN_AUTO_BUILD}" _mx_env_switch)
    if(_mx_env_switch MATCHES "^(0|off|false|no|n)$")
      set(_mx_enabled "OFF")
    endif()
  endif()
  if(_mx_enabled MATCHES "^(OFF|FALSE|0|NO|N)$")
    message(STATUS "[musicxx_extern_plugin] 自动构建已关闭 (MUSICXX_EXTERN_PLUGIN_AUTO_BUILD=OFF), "
      "需要 ${_mx_display} 时请手工跑包内 tools/build_native.ps1")
    return()
  endif()

  # 原生配置: 默认 Release (应用构建拿不到本次 Flutter 构建配置; 多配置生成器在 configure 期
  # 无法分辨 Debug/Release), 需要 Debug 时用环境变量指定
  set(_mx_config "Release")
  if(DEFINED ENV{MUSICXX_EXTERN_PLUGIN_CONFIG} AND NOT "$ENV{MUSICXX_EXTERN_PLUGIN_CONFIG}" STREQUAL "")
    set(_mx_config "$ENV{MUSICXX_EXTERN_PLUGIN_CONFIG}")
  endif()
  if(NOT _mx_config STREQUAL "Debug")
    set(_mx_config "Release")
  endif()
  string(TOLOWER "${_mx_config}" _mx_config_lower)

  set(_mx_library "${MX_PACKAGE_DIR}/.native/output/${MX_PLATFORM}-${MX_ARCH_TAG}-${_mx_config_lower}/bin/${MX_LIBRARY_NAME}")

  # 选择脚本与解释器
  if(MX_PLATFORM STREQUAL "windows")
    find_program(MUSICXX_EXTERN_PLUGIN_PWSH NAMES pwsh.exe pwsh powershell.exe)
    if(NOT MUSICXX_EXTERN_PLUGIN_PWSH)
      message(WARNING
        "[musicxx_extern_plugin] 没有找到 PowerShell, 跳过插件框架库自动构建; "
        "需要 ${_mx_display} 时请手工跑 tools/build_native.ps1")
      return()
    endif()
    set(_mx_command "${MUSICXX_EXTERN_PLUGIN_PWSH}" -NoProfile -ExecutionPolicy Bypass -File
        "${MX_PACKAGE_DIR}/tools/build_native.ps1" -Auto -Config "${_mx_config}" -ConsoleEncoding system)
  else()
    find_program(MUSICXX_EXTERN_PLUGIN_BASH NAMES bash)
    if(NOT MUSICXX_EXTERN_PLUGIN_BASH)
      message(WARNING
        "[musicxx_extern_plugin] 没有找到 bash, 跳过插件框架库自动构建; "
        "需要 ${_mx_display} 时请手工跑 tools/build_native.sh")
      return()
    endif()
    set(_mx_command "${MUSICXX_EXTERN_PLUGIN_BASH}" "${MX_PACKAGE_DIR}/tools/build_native.sh"
        --auto --config "${_mx_config}")
  endif()

  message(STATUS "[musicxx_extern_plugin] 确保插件框架库就绪 (${_mx_config}): 调用包内构建脚本的自动模式")
  # 子进程输出直接透传 (构建过程很长, 要能在 Flutter 的日志里实时看到)
  execute_process(COMMAND ${_mx_command} RESULT_VARIABLE _mx_result)
  if(EXISTS "${_mx_library}")
    message(STATUS "[musicxx_extern_plugin] 插件框架库就绪: ${_mx_library}")
  elseif(_mx_result EQUAL 0)
    # 脚本退 0 但库仍不在: 已是最新判定命中了别处的库, 或自动构建被跳过
    message(STATUS "[musicxx_extern_plugin] 未产出 ${_mx_library} (脚本判定无需构建或环境尚未配置)")
  else()
    message(WARNING
      "[musicxx_extern_plugin] 插件框架库自动构建失败 (退出码 ${_mx_result}), 本次构建不会打包 ${_mx_display};\n"
      "  应用照常构建与运行, 外部插件功能不可用。手工排查:\n"
      "  包内 tools/build_native.ps1 -Config ${_mx_config}   (Windows)\n"
      "  包内 tools/build_native.sh --config ${_mx_config}   (Linux / macOS)")
  endif()
endfunction()
