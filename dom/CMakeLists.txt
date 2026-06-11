cmake_minimum_required(VERSION 3.25)

add_library(malibu_dom OBJECT
    node_table.cpp
    wrapper_cache.cpp
    document.cpp
    dom_command_buffer.cpp
)
target_include_directories(malibu_dom PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_dom PUBLIC cxx_std_20)
target_compile_options(malibu_dom PRIVATE -Wall -Wextra -Wpedantic -Werror)