cmake_minimum_required(VERSION 3.25)

# Single CSS engine library: parser + selectors + cascade/style resolver +
# invalidation. (computed_style and cascade are header-only data contracts.)
add_library(malibu_css OBJECT
    parser/css_parser.cpp
    selector/selector.cpp
    style/style_resolver.cpp
    invalidation/invalidation.cpp
)
target_include_directories(malibu_css PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_css PUBLIC cxx_std_20)
target_compile_options(malibu_css PRIVATE -Wall -Wextra -Wpedantic -Werror)
