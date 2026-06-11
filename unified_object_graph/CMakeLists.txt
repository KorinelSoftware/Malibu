cmake_minimum_required(VERSION 3.25)

add_library(malibu_unified_object_graph OBJECT
    uog.cpp
)
target_include_directories(malibu_unified_object_graph PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_unified_object_graph PUBLIC cxx_std_20)
target_compile_options(malibu_unified_object_graph PRIVATE -Wall -Wextra -Wpedantic -Werror)