cmake_minimum_required(VERSION 3.25)

# ---------------------------------------------------------------------------
# Embed capability_manifest.json as a compile-time resource.
# Re-runs configure if the JSON changes.
# ---------------------------------------------------------------------------
set(_manifest_json "${CMAKE_CURRENT_SOURCE_DIR}/capability_manifest.json")
file(READ "${_manifest_json}" MALIBU_MANIFEST_JSON)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_manifest_json}")

set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/malibu/capability_manifest")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/capability_manifest_data.h.in"
    "${_gen_dir}/capability_manifest_data.h"
    @ONLY
)

add_library(malibu_capability_manifest OBJECT
    capability_manifest.cpp
)
target_include_directories(malibu_capability_manifest PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_include_directories(malibu_capability_manifest PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
# simdjson headers as SYSTEM so -Wpedantic -Werror does not flag them.
target_include_directories(malibu_capability_manifest SYSTEM PRIVATE
    $<TARGET_PROPERTY:simdjson,INTERFACE_INCLUDE_DIRECTORIES>
)
target_link_libraries(malibu_capability_manifest PRIVATE simdjson)
target_compile_features(malibu_capability_manifest PUBLIC cxx_std_20)
target_compile_options(malibu_capability_manifest PRIVATE -Wall -Wextra -Wpedantic -Werror)

# Newer GCC (15+) flags CRTP patterns inside simdjson's header via -Wtemplate-body.
# That header is vendored and out of our control; disable the diagnostic for this TU only.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-Wno-template-body" MALIBU_HAS_WNO_TEMPLATE_BODY)
if(MALIBU_HAS_WNO_TEMPLATE_BODY)
    target_compile_options(malibu_capability_manifest PRIVATE -Wno-template-body)
endif()
