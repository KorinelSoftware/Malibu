cmake_minimum_required(VERSION 3.25)

add_subdirectory(parser)
add_subdirectory(bytecode)
add_subdirectory(vm)
add_subdirectory(heap)
add_subdirectory(compiler)
add_subdirectory(runtime)

# MalibuJS engine facade (source -> parse -> compile -> execute).
add_library(malibu_js_engine OBJECT
    engine.cpp
)
target_include_directories(malibu_js_engine PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_js_engine PUBLIC cxx_std_20)
target_compile_options(malibu_js_engine PRIVATE -Wall -Wextra -Wpedantic -Werror)

# DOM <-> JS binding: routes DOM access from JS through the WebCall ABI.
add_library(malibu_js_dom OBJECT
    dom_binding.cpp
)
target_include_directories(malibu_js_dom PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_js_dom PUBLIC cxx_std_20)
target_compile_options(malibu_js_dom PRIVATE -Wall -Wextra -Wpedantic -Werror)
