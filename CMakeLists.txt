cmake_minimum_required(VERSION 3.29)

set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "Minimum macOS version")
project(hugin_munin VERSION 0.1.0 LANGUAGES CXX)

if(NOT APPLE)
  message(FATAL_ERROR "Only the macOS backend is implemented so far")
endif()

# The pasteboard backend under core/src/platform/macos/ is Objective-C++.
enable_language(OBJCXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(hm_core
  core/src/clipboard.cpp
  core/src/metrics.cpp
  core/src/sampler.cpp
  core/src/coordinator.cpp
  core/src/platform/macos/coordinator_ipc.cpp
  core/src/platform/macos/collectors.cpp
  core/src/platform/macos/pasteboard.mm
)
target_include_directories(hm_core PUBLIC core/include third_party)
target_link_libraries(hm_core PUBLIC "-framework CoreFoundation" "-framework IOKit" "-framework AppKit")
target_compile_options(hm_core PRIVATE -Wall -Wextra -Wpedantic)

add_library(hm_bridge core/bridge/bridge.cpp)
target_include_directories(hm_bridge PUBLIC core/bridge/include)
target_link_libraries(hm_bridge PUBLIC hm_core)
target_compile_options(hm_bridge PRIVATE -Wall -Wextra -Wpedantic)

add_executable(hm-service utilities/service.cpp)
target_link_libraries(hm-service PRIVATE hm_core)
add_executable(hm-run utilities/run.cpp)
target_link_libraries(hm-run PRIVATE hm_core)
add_executable(hm-mcp utilities/mcp.cpp)
target_link_libraries(hm-mcp PRIVATE hm_core)

# The macOS UI. CMake only supports Swift with the Ninja and Xcode generators.
# Without an explicit target, swiftc ignores the deployment target and builds for the host's macOS.
set(CMAKE_Swift_COMPILER_TARGET ${CMAKE_SYSTEM_PROCESSOR}-apple-macosx${CMAKE_OSX_DEPLOYMENT_TARGET})
enable_language(Swift)
set(CMAKE_Swift_LANGUAGE_VERSION 6)
# Incremental mode fails on relink with swiftc's cannotResolveTempPath error.
set(CMAKE_Swift_COMPILATION_MODE wholemodule)
add_executable(HuginMunin MACOSX_BUNDLE
  ui/macos/main.swift
  ui/macos/Monitor.swift
  ui/macos/Clipboard.swift
  ui/macos/ClipboardBezel.swift
  ui/macos/HotKey.swift
  ui/macos/FileShelf.swift
  ui/macos/Notch.swift
  ui/macos/Settings.swift
  ui/macos/Style.swift
)
set_target_properties(HuginMunin PROPERTIES
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/ui/macos/Info.plist.in
)
target_link_libraries(HuginMunin PRIVATE hm_bridge)
# Linking AppKit through hm_core makes ld see libc++ twice, once from Swift and once from the
# framework's re-exported interface; the duplicate is harmless and the warning is noise.
target_link_options(HuginMunin PRIVATE "SHELL:-Xlinker -no_warn_duplicate_libraries")
add_dependencies(HuginMunin hm-service hm-mcp hm-run)
# swiftc writes dependency files next to the executable, named by build type, and codesign rejects
# stray files in a bundle.
add_custom_command(TARGET HuginMunin POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E rm -f $<TARGET_FILE:HuginMunin>.d $<TARGET_FILE:HuginMunin>.emit-module.d
  COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:hm-service> $<TARGET_FILE:hm-mcp> $<TARGET_FILE:hm-run> $<TARGET_FILE_DIR:HuginMunin>
  COMMAND codesign --force --sign - $<TARGET_FILE_DIR:HuginMunin>/hm-run
  COMMAND codesign --force --sign - $<TARGET_FILE_DIR:HuginMunin>/hm-service
  COMMAND codesign --force --sign - $<TARGET_FILE_DIR:HuginMunin>/hm-mcp
  COMMAND codesign --force --sign - $<TARGET_BUNDLE_DIR:HuginMunin>
)

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME mcp_integration COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/mcp_integration.py $<TARGET_FILE:hm-mcp>)
  set_tests_properties(mcp_integration PROPERTIES TIMEOUT 30)
  add_dependencies(hm_bridge hm-service)
  foreach(test metrics_test collectors_test sampler_test clipboard_test bridge_test file_shelf_test coordinator_test)
    add_executable(${test} tests/${test}.cpp)
    target_link_libraries(${test} PRIVATE hm_bridge)
    target_compile_options(${test} PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME ${test} COMMAND ${test})
    list(APPEND hm_test_targets ${test})
  endforeach()
  # The sanitizer presets build this rather than `all`, because the Swift app does not link against
  # sanitized C++ objects.
  add_custom_target(hm_tests DEPENDS ${hm_test_targets} hm-mcp hm-run)
endif()
