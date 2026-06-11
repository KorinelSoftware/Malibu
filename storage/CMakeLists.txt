cmake_minimum_required(VERSION 3.25)

add_library(malibu_storage OBJECT
    web_storage.cpp
    indexed_db.cpp
    cache_storage.cpp
    cookie_store.cpp
    storage_engine.cpp
)
target_include_directories(malibu_storage PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_storage PUBLIC cxx_std_20)
target_compile_options(malibu_storage PRIVATE -Wall -Wextra -Wpedantic -Werror)