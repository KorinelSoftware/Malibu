cmake_minimum_required(VERSION 3.25)

# Real text engine: fontconfig (resolution) + FreeType (rasterization) +
# HarfBuzz (shaping). These are cross-platform libraries, linked here (not in
# core/) so the OS-agnostic core stays clean.
find_package(PkgConfig REQUIRED)
pkg_check_modules(FONTCONFIG REQUIRED fontconfig)

add_library(malibu_text OBJECT
    font.cpp
    glyph_drawer.cpp
)
target_include_directories(malibu_text PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_include_directories(malibu_text SYSTEM PRIVATE
    ${FREETYPE_INCLUDE_DIRS}
    ${HARFBUZZ_INCLUDE_DIRS}
    ${FONTCONFIG_INCLUDE_DIRS}
)
target_compile_features(malibu_text PUBLIC cxx_std_20)
target_compile_options(malibu_text PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(malibu_text PUBLIC
    ${FREETYPE_LIBRARIES}
    ${HARFBUZZ_LIBRARIES}
    ${FONTCONFIG_LIBRARIES}
)
target_link_directories(malibu_text PUBLIC
    ${FREETYPE_LIBRARY_DIRS}
    ${HARFBUZZ_LIBRARY_DIRS}
    ${FONTCONFIG_LIBRARY_DIRS}
)
