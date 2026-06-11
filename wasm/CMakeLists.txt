cmake_minimum_required(VERSION 3.25)

# MalibuWASM: from-scratch WebAssembly decoder + interpreter. No JS dependency.
add_library(malibu_wasm STATIC
    wasm.cpp
)
target_include_directories(malibu_wasm PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_wasm PUBLIC cxx_std_20)
target_compile_options(malibu_wasm PRIVATE -Wall -Wextra -Wpedantic -Werror)
