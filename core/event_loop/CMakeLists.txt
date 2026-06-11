cmake_minimum_required(VERSION 3.25)

add_library(malibu_event_loop OBJECT
    event_loop.cpp
)
target_include_directories(malibu_event_loop PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_event_loop PUBLIC cxx_std_20)
target_compile_options(malibu_event_loop PRIVATE -Wall -Wextra -Wpedantic -Werror)