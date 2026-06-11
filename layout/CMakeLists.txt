cmake_minimum_required(VERSION 3.25)

add_library(malibu_layout OBJECT
    layout_engine.cpp
)
target_include_directories(malibu_layout PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_layout PUBLIC cxx_std_20)
target_compile_options(malibu_layout PRIVATE -Wall -Wextra -Wpedantic -Werror)