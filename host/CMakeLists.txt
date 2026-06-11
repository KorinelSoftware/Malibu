cmake_minimum_required(VERSION 3.25)

find_package(CURL REQUIRED)

add_library(malibu_host_network STATIC
    curl_resource_loader.cpp
)
target_include_directories(malibu_host_network PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_host_network PUBLIC cxx_std_20)
target_compile_options(malibu_host_network PRIVATE
    -Wall -Wextra -Wpedantic -Werror
)
target_link_libraries(malibu_host_network PUBLIC CURL::libcurl)
