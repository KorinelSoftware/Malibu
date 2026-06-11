cmake_minimum_required(VERSION 3.25)

# MalibuCanvas: from-scratch 2D raster surface (the <canvas> 2D backend).
add_library(malibu_canvas STATIC
    canvas2d.cpp
)
target_include_directories(malibu_canvas PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_canvas PUBLIC cxx_std_20)
target_compile_options(malibu_canvas PRIVATE -Wall -Wextra -Wpedantic -Werror)
