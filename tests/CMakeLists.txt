cmake_minimum_required(VERSION 3.25)

# Test infrastructure
# googletest is provided via FetchContent in top-level CMakeLists.txt

# Placeholder test to verify test infrastructure works
add_executable(test_infrastructure test_infrastructure.cpp)
target_link_libraries(test_infrastructure PRIVATE gtest_main)
add_test(NAME test_infrastructure COMMAND test_infrastructure)

# M2: NodeTable tests
add_executable(test_nodetable test_nodetable.cpp)
target_link_libraries(test_nodetable PRIVATE malibu_dom malibu_diagnostics malibu_js_vm gtest_main)
add_test(NAME test_nodetable COMMAND test_nodetable)

# M2: UnifiedObjectGraph tests
add_executable(test_uog test_uog.cpp)
target_link_libraries(test_uog PRIVATE malibu_unified_object_graph malibu_diagnostics gtest_main)
add_test(NAME test_uog COMMAND test_uog)

# M2: WrapperCache tests
add_executable(test_wrappercache test_wrappercache.cpp)
target_link_libraries(test_wrappercache PRIVATE malibu_dom malibu_js_vm malibu_diagnostics gtest_main)
add_test(NAME test_wrappercache COMMAND test_wrappercache)

# Task 4: Capability manifest tests
add_executable(test_capability_manifest test_capability_manifest.cpp)
target_link_libraries(test_capability_manifest PRIVATE malibu_capability_manifest gtest_main)
add_test(NAME test_capability_manifest COMMAND test_capability_manifest)

# Task 21: Event loop tests
add_executable(test_event_loop test_event_loop.cpp)
target_link_libraries(test_event_loop PRIVATE malibu_event_loop gtest_main)
add_test(NAME test_event_loop COMMAND test_event_loop)

# Task 17 (async): Promises, async/await, timers
add_executable(test_js_async test_js_async.cpp)
target_link_libraries(test_js_async PRIVATE
    malibu_js_engine malibu_js_runtime malibu_js_compiler malibu_js_heap
    malibu_js_parser malibu_js_vm malibu_event_loop malibu_diagnostics gtest_main)
add_test(NAME test_js_async COMMAND test_js_async)

# Task 9: DOM tree tests
add_executable(test_domtree test_domtree.cpp)
target_link_libraries(test_domtree PRIVATE malibu_dom malibu_js_vm malibu_diagnostics gtest_main)
add_test(NAME test_domtree COMMAND test_domtree)

# Task 8: DOM command buffer tests
add_executable(test_command_buffer test_command_buffer.cpp)
target_link_libraries(test_command_buffer PRIVATE malibu_dom malibu_js_vm malibu_diagnostics gtest_main)
add_test(NAME test_command_buffer COMMAND test_command_buffer)

# Task 19: WebCall ABI guards / deopt tests
add_executable(test_webcall test_webcall.cpp)
target_link_libraries(test_webcall PRIVATE
    malibu_webcall malibu_dom malibu_js_bytecode malibu_js_vm malibu_diagnostics gtest_main)
add_test(NAME test_webcall COMMAND test_webcall)

# Task 16: Value NaN-boxing tests
add_executable(test_value test_value.cpp)
target_link_libraries(test_value PRIVATE malibu_js_vm malibu_diagnostics gtest_main)
add_test(NAME test_value COMMAND test_value)

# Tasks 14-18: full MalibuJS engine (parser -> compiler -> interpreter -> GC)
add_executable(test_js_engine test_js_engine.cpp)
target_link_libraries(test_js_engine PRIVATE
    malibu_js_engine malibu_js_runtime malibu_js_compiler malibu_js_heap
    malibu_js_parser malibu_js_vm malibu_event_loop malibu_diagnostics gtest_main)
add_test(NAME test_js_engine COMMAND test_js_engine)

# JS <-> DOM via WebCall ABI: real JavaScript manipulating the live DOM
add_executable(test_js_dom test_js_dom.cpp)
target_link_libraries(test_js_dom PRIVATE
    malibu_js_dom malibu_js_engine malibu_js_runtime malibu_js_compiler malibu_js_heap
    malibu_js_parser malibu_js_vm malibu_js_bytecode malibu_webcall malibu_dom malibu_html
    malibu_event_loop malibu_diagnostics gtest_main)
add_test(NAME test_js_dom COMMAND test_js_dom)

# Task 20: Static Pre-Profiler + Expectation Layer tests
add_executable(test_compat test_compat.cpp)
target_link_libraries(test_compat PRIVATE malibu_compat malibu_diagnostics gtest_main)
add_test(NAME test_compat COMMAND test_compat)

# Task 13: Security (Origin / SOP / CSP) tests
add_executable(test_security test_security.cpp)
target_link_libraries(test_security PRIVATE malibu_security malibu_diagnostics gtest_main)
add_test(NAME test_security COMMAND test_security)

# Task 24: Storage engine (localStorage/sessionStorage/IndexedDB/Cache/Cookies)
add_executable(test_storage test_storage.cpp)
target_link_libraries(test_storage PRIVATE malibu_storage malibu_security malibu_diagnostics gtest_main)
add_test(NAME test_storage COMMAND test_storage)

# Task 22: Network engine (Fetch / CORS / HTTP cache / CSP)
add_executable(test_network test_network.cpp)
target_link_libraries(test_network PRIVATE malibu_network malibu_security malibu_diagnostics gtest_main)
add_test(NAME test_network COMMAND test_network)

# Task 23: WebSocket protocol (RFC 6455)
add_executable(test_websocket test_websocket.cpp)
target_link_libraries(test_websocket PRIVATE malibu_network malibu_security malibu_diagnostics gtest_main)
add_test(NAME test_websocket COMMAND test_websocket)

# Task 14 (subset): ECMAScript parser tests
add_executable(test_parser test_parser.cpp)
target_link_libraries(test_parser PRIVATE malibu_js_parser gtest_main)
add_test(NAME test_parser COMMAND test_parser)

# Task 28: HTML parser tests
add_executable(test_html test_html.cpp)
target_link_libraries(test_html PRIVATE malibu_html malibu_dom malibu_js_vm malibu_diagnostics gtest_main)
add_test(NAME test_html COMMAND test_html)

# MalibuWASM: decoder + interpreter
add_executable(test_wasm test_wasm.cpp)
target_link_libraries(test_wasm PRIVATE malibu_wasm gtest_main)
add_test(NAME test_wasm COMMAND test_wasm)

# MalibuGL + MalibuShader: the triangle + GLSL interpreter
add_executable(test_gl test_gl.cpp)
target_link_libraries(test_gl PRIVATE malibu_gl malibu_shader gtest_main)
add_test(NAME test_gl COMMAND test_gl)

# Tasks 10-11: CSS engine (parser/selector/cascade/style/invalidation)
add_executable(test_css test_css.cpp)
target_link_libraries(test_css PRIVATE malibu_css malibu_dom malibu_js_vm malibu_diagnostics gtest_main)
add_test(NAME test_css COMMAND test_css)

# Task 12: Layout engine (block/inline/flex)
add_executable(test_layout test_layout.cpp)
target_link_libraries(test_layout PRIVATE malibu_layout malibu_css malibu_dom malibu_js_vm malibu_diagnostics gtest_main)
add_test(NAME test_layout COMMAND test_layout)

# Task 26: CPU rendering (display list, software rasterizer, glyph atlas) — full LSan.
add_executable(test_render test_render.cpp)
target_link_libraries(test_render PRIVATE
    malibu_render malibu_layout malibu_css malibu_dom
    malibu_js_vm malibu_diagnostics gtest_main)
add_test(NAME test_render COMMAND test_render)

# Real text engine (fontconfig + FreeType + HarfBuzz)
add_executable(test_text test_text.cpp)
target_link_libraries(test_text PRIVATE malibu_text malibu_render gtest_main)
add_test(NAME test_text COMMAND test_text)
# fontconfig keeps a process-global config (parsed via expat) that LSan flags;
# suppress those third-party frames only.
set_tests_properties(test_text PROPERTIES
    ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan_suppressions.txt")

# Task 25: Vulkan device. Leak detection off — the loader/ICD leaks via D-Bus
# (third-party, unsymbolized so unsuppressible by name).
add_executable(test_vulkan test_vulkan.cpp)
target_link_libraries(test_vulkan PRIVATE malibu_render_vulkan malibu_diagnostics gtest_main)
add_test(NAME test_vulkan COMMAND test_vulkan)
set_tests_properties(test_vulkan PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")

# Task 31: MalibuView embedding API (load page, render, eval, messaging, sandbox)
add_executable(test_view test_view.cpp)
target_link_libraries(test_view PRIVATE malibu_view gtest_main)
add_test(NAME test_view COMMAND test_view)
set_tests_properties(test_view PROPERTIES
    ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan_suppressions.txt")

# Empirical "run Discord" harness: boot a Discord-shaped client in MalibuView and
# let the engine surface the next missing web API (error-driven implementation).
add_executable(test_discord test_discord.cpp)
target_link_libraries(test_discord PRIVATE malibu_view gtest_main)
add_test(NAME test_discord COMMAND test_discord)
set_tests_properties(test_discord PROPERTIES
    ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan_suppressions.txt")

add_executable(test_discord_phase2 test_discord_phase2.cpp)
target_link_libraries(test_discord_phase2 PRIVATE malibu_view gtest_main)
add_test(NAME test_discord_phase2 COMMAND test_discord_phase2)
set_tests_properties(test_discord_phase2 PROPERTIES
    ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan_suppressions.txt")

add_executable(test_discord_phase3 test_discord_phase3.cpp)
target_link_libraries(test_discord_phase3 PRIVATE malibu_view gtest_main)
add_test(NAME test_discord_phase3 COMMAND test_discord_phase3)
set_tests_properties(test_discord_phase3 PROPERTIES
    ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan_suppressions.txt")

add_executable(test_discord_phase4 test_discord_phase4.cpp)
target_link_libraries(test_discord_phase4 PRIVATE malibu_view gtest_main)
add_test(NAME test_discord_phase4 COMMAND test_discord_phase4)
set_tests_properties(test_discord_phase4 PROPERTIES
    ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan_suppressions.txt")

add_executable(test_discord_phase5 test_discord_phase5.cpp)
target_link_libraries(test_discord_phase5 PRIVATE malibu_view gtest_main)
add_test(NAME test_discord_phase5 COMMAND test_discord_phase5)
set_tests_properties(test_discord_phase5 PROPERTIES
    ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan_suppressions.txt")

# Task 32: App Runtime (PWA manifest + Service Worker lifecycle + fetch intercept)
add_executable(test_app_runtime test_app_runtime.cpp)
target_link_libraries(test_app_runtime PRIVATE malibu_app_runtime gtest_main)
add_test(NAME test_app_runtime COMMAND test_app_runtime)
set_tests_properties(test_app_runtime PROPERTIES
    ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan_suppressions.txt")

# DOM Event model (addEventListener / dispatchEvent / capture+bubble) via JS
add_executable(test_dom_events test_dom_events.cpp)
target_link_libraries(test_dom_events PRIVATE malibu_view gtest_main)
add_test(NAME test_dom_events COMMAND test_dom_events)
set_tests_properties(test_dom_events PROPERTIES
    ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan_suppressions.txt")

# DOM API coverage (classList / getElementById / traversal / style CSSOM)
add_executable(test_dom_api test_dom_api.cpp)
target_link_libraries(test_dom_api PRIVATE malibu_view gtest_main)
add_test(NAME test_dom_api COMMAND test_dom_api)
set_tests_properties(test_dom_api PROPERTIES
    ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan_suppressions.txt")

# Headless render-to-PNG (the visible page-rendering demo)
add_executable(test_screenshot test_screenshot.cpp)
target_link_libraries(test_screenshot PRIVATE malibu_view malibu_image gtest_main)
add_test(NAME test_screenshot COMMAND test_screenshot)
set_tests_properties(test_screenshot PROPERTIES
    ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan_suppressions.txt")

# Task 29: Linux Platform Backend (POSIX file/clock/thread + display detection)
add_executable(test_linux_platform test_linux_platform.cpp)
target_link_libraries(test_linux_platform PRIVATE malibu_platform_linux malibu_platform_null malibu_diagnostics gtest_main)
add_test(NAME test_linux_platform COMMAND test_linux_platform)

# Tasks 33/35: Malibu Electron Runtime (Electron-style app on Malibu, no Node/Chromium)
add_executable(test_electron test_electron.cpp)
target_link_libraries(test_electron PRIVATE malibu_electron gtest_main)
add_test(NAME test_electron COMMAND test_electron)
set_tests_properties(test_electron PROPERTIES
    ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan_suppressions.txt")

# Task 34: MalibuCEF (CEF3-compatible embedding backed by MalibuView)
add_executable(test_cef test_cef.cpp)
target_link_libraries(test_cef PRIVATE malibu_cef gtest_main)
add_test(NAME test_cef COMMAND test_cef)
set_tests_properties(test_cef PROPERTIES
    ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan_suppressions.txt")

# Full-engine integration: HTML -> DOM -> CSS -> JS (WebCall) -> layout -> raster
add_executable(test_integration test_integration.cpp)
target_link_libraries(test_integration PRIVATE
    malibu_html malibu_css malibu_layout malibu_render
    malibu_js_dom malibu_js_engine malibu_js_runtime malibu_js_compiler malibu_js_heap
    malibu_js_parser malibu_js_vm malibu_js_bytecode malibu_webcall malibu_dom
    malibu_event_loop malibu_diagnostics gtest_main)
add_test(NAME test_integration COMMAND test_integration)

# M2: Property-based tests (Rapidcheck) - disabled for now due to rapidcheck integration issues
# add_executable(test_pbt test_pbt.cpp)
# target_include_directories(test_pbt PRIVATE
#     "${CMAKE_SOURCE_DIR}/build/debug/_deps/rapidcheck-src/include"
#     "${CMAKE_SOURCE_DIR}/build/debug/_deps/rapidcheck-src/extras/gtest/include"
# )
# target_link_libraries(test_pbt PRIVATE malibu_dom malibu_unified_object_graph rapidcheck_gtest gtest_main)
# add_test(NAME test_pbt COMMAND test_pbt)
