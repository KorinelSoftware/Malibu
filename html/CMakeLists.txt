cmake_minimum_required(VERSION 3.25)

add_library(malibu_html OBJECT
    html_parser.cpp
)
target_include_directories(malibu_html PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_html PUBLIC cxx_std_20)
target_compile_options(malibu_html PRIVATE -Wall -Wextra -Wpedantic -Werror)
