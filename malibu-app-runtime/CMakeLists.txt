cmake_minimum_required(VERSION 3.25)

add_library(malibu_app_runtime STATIC
    manifest.cpp
    app_runtime.cpp
    service_worker.cpp
)
target_include_directories(malibu_app_runtime PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
# simdjson (manifest parser) as a SYSTEM include so -Wpedantic -Werror does not
# flag its headers.
target_include_directories(malibu_app_runtime SYSTEM PRIVATE
    $<TARGET_PROPERTY:simdjson,INTERFACE_INCLUDE_DIRECTORIES>
)
target_compile_features(malibu_app_runtime PUBLIC cxx_std_20)
target_compile_options(malibu_app_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)

include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-Wno-template-body" MALIBU_APP_HAS_WNO_TEMPLATE_BODY)
if(MALIBU_APP_HAS_WNO_TEMPLATE_BODY)
    target_compile_options(malibu_app_runtime PRIVATE -Wno-template-body)  # GCC15 vs simdjson
endif()

target_link_libraries(malibu_app_runtime PUBLIC
    malibu_view       # brings the whole engine (DOM/CSS/JS/layout/render/text)
    malibu_storage
    malibu_diagnostics
    simdjson
)
