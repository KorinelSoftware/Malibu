cmake_minimum_required(VERSION 3.25)

# MalibuShader: GLSL ES interpreter (lexer/parser/tree-walk). No deps.
add_library(malibu_shader STATIC
    glsl.cpp
)
target_include_directories(malibu_shader PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_shader PUBLIC cxx_std_20)
target_compile_options(malibu_shader PRIVATE -Wall -Wextra -Wpedantic -Werror)
