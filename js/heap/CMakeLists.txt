cmake_minimum_required(VERSION 3.25)

add_library(malibu_js_heap OBJECT
    heap.cpp
)
target_include_directories(malibu_js_heap PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_js_heap PUBLIC cxx_std_20)
target_compile_options(malibu_js_heap PRIVATE -Wall -Wextra -Wpedantic -Werror)
