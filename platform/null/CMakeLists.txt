cmake_minimum_required(VERSION 3.25)

add_library(malibu_platform_null OBJECT
    null_platform.cpp
)
target_include_directories(malibu_platform_null PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
    "${CMAKE_SOURCE_DIR}/platform"
)
target_compile_features(malibu_platform_null PUBLIC cxx_std_20)
target_compile_options(malibu_platform_null PRIVATE -Wall -Wextra -Wpedantic -Werror)