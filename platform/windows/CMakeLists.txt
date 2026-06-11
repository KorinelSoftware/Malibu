cmake_minimum_required(VERSION 3.25)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(STATUS "Skipping malibu_platform_windows (not Windows)")
    return()
endif()

# malibu_platform_windows — Windows Win32 backend. OS-specific code allowed.
add_library(malibu_platform_windows SHARED
    windows_platform_stub.cpp
)
target_include_directories(malibu_platform_windows PUBLIC
    "${CMAKE_SOURCE_DIR}/platform"
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_platform_windows PUBLIC cxx_std_20)
target_compile_options(malibu_platform_windows PRIVATE /W4 /WX)

# Full Win32 implementation added in Task 30.
