cmake_minimum_required(VERSION 3.25)

add_library(malibu_compat OBJECT
    static_pre_profiler.cpp
    expectation_layer.cpp
)
target_include_directories(malibu_compat PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_compat PUBLIC cxx_std_20)
target_compile_options(malibu_compat PRIVATE -Wall -Wextra -Wpedantic -Werror)
