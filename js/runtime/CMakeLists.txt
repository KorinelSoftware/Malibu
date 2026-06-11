cmake_minimum_required(VERSION 3.25)

add_library(malibu_js_runtime OBJECT
    objects.cpp
    interpreter.cpp
    builtins.cpp
    typed_arrays.cpp
    regex.cpp
)
target_include_directories(malibu_js_runtime PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_js_runtime PUBLIC cxx_std_20)
target_compile_options(malibu_js_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)
