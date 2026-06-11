cmake_minimum_required(VERSION 3.25)

add_library(malibu_security OBJECT
    origin.cpp
    same_origin_policy.cpp
    csp_enforcer.cpp
)
target_include_directories(malibu_security PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_security PUBLIC cxx_std_20)
target_compile_options(malibu_security PRIVATE -Wall -Wextra -Wpedantic -Werror)
