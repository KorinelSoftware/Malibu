cmake_minimum_required(VERSION 3.25)

# malibu-render: headless HTML -> PNG screenshot tool (the visible demo of the
# full HTML/CSS/JS/layout/text rendering pipeline).
add_executable(malibu_render_cli
    malibu_render.cpp
)
set_target_properties(malibu_render_cli PROPERTIES OUTPUT_NAME malibu-render)
target_include_directories(malibu_render_cli PRIVATE
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_render_cli PRIVATE cxx_std_20)
target_compile_options(malibu_render_cli PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(malibu_render_cli PRIVATE
    malibu_view
    malibu_image
    malibu_host_network
)

# libjpeg/libpng live in the multiarch dir, not always on the default runtime
# linker path here — embed an RPATH so the tools find them directly.
set_target_properties(malibu_render_cli PROPERTIES
    BUILD_RPATH "/usr/lib/x86_64-linux-gnu;/usr/lib"
    INSTALL_RPATH "/usr/lib/x86_64-linux-gnu;/usr/lib")

# malibu_wpt: headless Web Platform Tests runner (web-compat thermometer).
add_executable(malibu_wpt malibu_wpt.cpp)
target_include_directories(malibu_wpt PRIVATE "${CMAKE_SOURCE_DIR}/core/include")
target_compile_features(malibu_wpt PRIVATE cxx_std_20)
target_compile_options(malibu_wpt PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(malibu_wpt PRIVATE malibu_view malibu_image)

# malibu_js: minimal JS CLI runner (drives the Test262 conformance suite).
add_executable(malibu_js malibu_js.cpp)
target_include_directories(malibu_js PRIVATE "${CMAKE_SOURCE_DIR}/core/include")
target_compile_features(malibu_js PRIVATE cxx_std_20)
target_compile_options(malibu_js PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(malibu_js PRIVATE
    malibu_js_engine malibu_js_runtime malibu_js_compiler malibu_js_parser
    malibu_js_vm malibu_js_heap malibu_js_bytecode malibu_event_loop malibu_diagnostics)
