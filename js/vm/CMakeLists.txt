cmake_minimum_required(VERSION 3.25)

# Value + JSRealm: no external subsystem dependencies, so DOM / WrapperCache
# tests can link this without pulling in the runtime/compiler.
add_library(malibu_js_vm OBJECT
    value.cpp
    realm.cpp
)
target_include_directories(malibu_js_vm PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_js_vm PUBLIC cxx_std_20)
target_compile_options(malibu_js_vm PRIVATE -Wall -Wextra -Wpedantic -Werror)
