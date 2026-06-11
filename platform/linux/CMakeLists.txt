cmake_minimum_required(VERSION 3.25)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "Skipping malibu_platform_linux (not Linux)")
    return()
endif()

# malibu_platform_linux — Linux Platform Backend (Task 29).
# OS-specific code is ALLOWED here; this is a Platform Backend.
add_library(malibu_platform_linux STATIC
    linux_platform.cpp
)
target_include_directories(malibu_platform_linux PUBLIC
    "${CMAKE_SOURCE_DIR}/platform"
    "${CMAKE_SOURCE_DIR}/platform/linux"
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_platform_linux PUBLIC cxx_std_20)
target_compile_options(malibu_platform_linux PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(malibu_platform_linux PUBLIC malibu_diagnostics)
