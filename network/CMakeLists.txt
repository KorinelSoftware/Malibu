cmake_minimum_required(VERSION 3.25)

find_package(PkgConfig REQUIRED)
pkg_check_modules(OPENSSL REQUIRED libcrypto)

add_library(malibu_network OBJECT
    fetch_engine.cpp
    http_cache.cpp
    websocket.cpp
)
target_include_directories(malibu_network PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_include_directories(malibu_network SYSTEM PRIVATE ${OPENSSL_INCLUDE_DIRS})
target_compile_features(malibu_network PUBLIC cxx_std_20)
target_compile_options(malibu_network PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(malibu_network PUBLIC ${OPENSSL_LIBRARIES})
target_link_directories(malibu_network PUBLIC ${OPENSSL_LIBRARY_DIRS})
