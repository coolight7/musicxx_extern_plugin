# 对照示例动态库插件 (musicxx_extern_plugin/plugins/example_native_bad_entry)
#
# 这个示例刻意**不用** `musicxx_plugin_add_target`：它要的导出面是「有 get_info / create /
# destroy，没有 start / stop」，而构建助手给 Apple 的导出符号表会把 start/stop 一起列上。
# 这里手工建库，只依赖 SDK 的包含目录（头文件里的 C ABI），不链接内核库。
cmake_minimum_required(VERSION 3.20)
project(example_native_bad_entry LANGUAGES CXX)

add_library(example_native_bad_entry SHARED example_native_bad_entry.cpp)
set_target_properties(example_native_bad_entry PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED OFF
  PREFIX ""
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
)

# 只用 SDK 的包含目录与平台宏（内核 kit 是 header-only，本示例不调用它）
if (TARGET musicxx_extern_plugin_sdk)
  target_link_libraries(example_native_bad_entry PRIVATE musicxx_extern_plugin_sdk)
else ()
  target_include_directories(example_native_bad_entry PRIVATE
    "${MUSICXX_EXTERN_PLUGIN_SRC_DIR}/include"
    "${MUSICXX_EXTERN_PLUGIN_SRC_DIR}/sdk/include"
    "${MUSICXX_EXTERN_PLUGIN_DEP_PREFIX}/include"
  )
  target_compile_definitions(example_native_bad_entry PRIVATE
    BOOST_ASIO_STANDALONE=1 ASIO_STANDALONE=1 UTILXX_USE_BOOST_ASIO=1
  )
endif ()

if (MSVC)
  target_compile_options(example_native_bad_entry PRIVATE /utf-8)
endif ()
if (WIN32)
  target_compile_definitions(example_native_bad_entry PRIVATE _WIN32_WINNT=0x0A00)
endif ()

# 产物与清单放在同一层（多配置生成器也不分子目录），使该目录可以直接作为"插件目录"使用
set_target_properties(example_native_bad_entry PROPERTIES
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
foreach (_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL PROFILE)
  set_target_properties(example_native_bad_entry PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_CURRENT_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_CURRENT_BINARY_DIR}"
  )
endforeach ()
unset(_cfg)

add_custom_command(TARGET example_native_bad_entry POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/plugin.yaml"
          "$<TARGET_FILE_DIR:example_native_bad_entry>/plugin.yaml"
  COMMENT "复制插件清单 plugin.yaml 到产物目录")
