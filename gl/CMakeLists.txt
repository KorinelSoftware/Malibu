cmake_minimum_required(VERSION 3.25)

# MalibuGL: WebGL-1 state machine + rasterizer (uses MalibuShader). No GPU deps.
add_library(malibu_gl STATIC
    gl.cpp
)
target_include_directories(malibu_gl PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_gl PUBLIC cxx_std_20)
target_compile_options(malibu_gl PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(malibu_gl PUBLIC malibu_shader)
