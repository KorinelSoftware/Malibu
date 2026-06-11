cmake_minimum_required(VERSION 3.25)

add_library(malibu_view STATIC
    view.cpp
    malibu_view_c_api.cpp
)
target_include_directories(malibu_view PUBLIC
    "${CMAKE_SOURCE_DIR}/malibu-view/include"
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_view PUBLIC cxx_std_20)
target_compile_options(malibu_view PRIVATE -Wall -Wextra -Wpedantic -Werror)

# The view orchestrates the entire engine; propagate every subsystem it links.
target_link_libraries(malibu_view PUBLIC
    malibu_html
    malibu_css
    malibu_layout
    malibu_render
    malibu_text
    malibu_js_engine malibu_js_dom malibu_js_runtime malibu_js_compiler
    malibu_js_heap malibu_js_parser malibu_js_vm malibu_js_bytecode
    malibu_webcall
    malibu_wasm
    malibu_canvas
    malibu_gl
    malibu_shader
    malibu_image
    malibu_dom
    malibu_storage
    malibu_network
    malibu_event_loop
    malibu_security
    malibu_diagnostics
)
