cmake_minimum_required(VERSION 3.25)

add_library(malibu_diagnostics OBJECT
    diagnostic_log.cpp
)
target_include_directories(malibu_diagnostics PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_diagnostics PUBLIC cxx_std_20)
target_compile_options(malibu_diagnostics PRIVATE -Wall -Wextra -Wpedantic -Werror)