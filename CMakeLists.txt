cmake_minimum_required(VERSION 3.25)
project(malibu LANGUAGES CXX C)

# ---------------------------------------------------------------------------
# Dependency Management Options
# ---------------------------------------------------------------------------
option(MALIBU_USE_SYSTEM_DEPS      "Prefer system packages for large deps"  ON)
option(MALIBU_VENDOR_TEST_DEPS     "Vendor googletest via FetchContent"     ON)
option(MALIBU_VENDOR_SMALL_DEPS    "Vendor simdjson, vulkan-headers"        ON)
option(MALIBU_VENDOR_ALL           "Vendor everything except Vulkan loader" OFF)
option(MALIBU_ENABLE_PBT           "Enable Rapidcheck PBT"                  ON)

# ---------------------------------------------------------------------------
# Version definitions
# ---------------------------------------------------------------------------
include(cmake/versions.cmake)

# ---------------------------------------------------------------------------
# Platform check (enforces no OS calls in core/)
# ---------------------------------------------------------------------------
include(cmake/platform_check.cmake)

# ---------------------------------------------------------------------------
# WebCall ABI versioning
# ---------------------------------------------------------------------------
include(cmake/webcall_abi_version.cmake)

# ---------------------------------------------------------------------------
# FetchContent for small/test dependencies
# ---------------------------------------------------------------------------
if(MALIBU_VENDOR_TEST_DEPS)
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest
        GIT_TAG        ${MALIBU_GOOGLETEST_GIT_TAG}
    )
    FetchContent_MakeAvailable(googletest)
    message(STATUS "googletest: FetchContent (MALIBU_VENDOR_TEST_DEPS=ON)")
endif()

if(MALIBU_VENDOR_SMALL_DEPS)
    include(FetchContent)
    
    # simdjson
    FetchContent_Declare(simdjson
        GIT_REPOSITORY https://github.com/simdjson/simdjson
        GIT_TAG        ${MALIBU_SIMDJSON_GIT_TAG}
    )
    FetchContent_MakeAvailable(simdjson)
    message(STATUS "simdjson: FetchContent (MALIBU_VENDOR_SMALL_DEPS=ON)")

    # vulkan-headers
    FetchContent_Declare(vulkan-headers
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers
        GIT_TAG        v${MALIBU_VULKAN_SDK_MIN_VERSION}
    )
    FetchContent_MakeAvailable(vulkan-headers)
    message(STATUS "vulkan-headers: FetchContent (MALIBU_VENDOR_SMALL_DEPS=ON)")
endif()

if(MALIBU_VENDOR_ALL)
    include(FetchContent)
    
    # freetype
    FetchContent_Declare(freetype
        GIT_REPOSITORY https://gitlab.freedesktop.org/freetype/freetype.git
        GIT_TAG        VER-${MALIBU_FREETYPE_VERSION}
    )
    
    # harfbuzz
    FetchContent_Declare(harfbuzz
        GIT_REPOSITORY https://github.com/harfbuzz/harfbuzz.git
        GIT_TAG        ${MALIBU_HARFBUZZ_VERSION}
    )
    
    # libpng
    FetchContent_Declare(libpng
        GIT_REPOSITORY https://github.com/pnggroup/libpng.git
        GIT_TAG        v${MALIBU_LIBPNG_VERSION}
    )
    
    # libjpeg-turbo
    FetchContent_Declare(libjpeg-turbo
        GIT_REPOSITORY https://github.com/libjpeg-turbo/libjpeg-turbo.git
        GIT_TAG        ${MALIBU_LIBJPEG_TURBO_VERSION}
    )
    
    # nghttp2
    FetchContent_Declare(nghttp2
        GIT_REPOSITORY https://github.com/nghttp2/nghttp2.git
        GIT_TAG        v${MALIBU_NGHTTP2_VERSION}
    )
    
    FetchContent_MakeAvailable(freetype harfbuzz libpng libjpeg-turbo nghttp2)
    message(STATUS "All large deps: FetchContent (MALIBU_VENDOR_ALL=ON)")
endif()

# ---------------------------------------------------------------------------
# System package discovery (preferred path)
# ---------------------------------------------------------------------------
if(MALIBU_USE_SYSTEM_DEPS)
    find_package(PkgConfig REQUIRED)
    
    pkg_check_modules(FREETYPE REQUIRED freetype2)
    pkg_check_modules(HARFBUZZ REQUIRED harfbuzz)
    pkg_check_modules(LIBPNG REQUIRED libpng)
    pkg_check_modules(LIBJPEG REQUIRED libjpeg)
    pkg_check_modules(NGHTTP2 REQUIRED libnghttp2)
    
    message(STATUS "System deps found via pkg-config: freetype, harfbuzz, libpng, libjpeg, nghttp2")
endif()

# Vulkan: loader from system, headers from FetchContent or system
find_package(Vulkan REQUIRED)
message(STATUS "Vulkan: ${Vulkan_VERSION} (loader from system)")

# Rapidcheck for PBT
if(MALIBU_ENABLE_PBT)
    include(FetchContent)
    FetchContent_Declare(rapidcheck
        GIT_REPOSITORY https://github.com/emil-e/rapidcheck
        GIT_TAG        master
    )
    FetchContent_MakeAvailable(rapidcheck)
    message(STATUS "rapidcheck: FetchContent (MALIBU_ENABLE_PBT=ON)")
endif()

# ---------------------------------------------------------------------------
# Compiler settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic -Werror")
    set(CMAKE_CXX_FLAGS_DEBUG "-g -O0 -fno-omit-frame-pointer")
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
endif()

# ---------------------------------------------------------------------------
# Subdirectories - Core subsystems
# ---------------------------------------------------------------------------
add_subdirectory(core)
add_subdirectory(js)
add_subdirectory(wasm)
add_subdirectory(canvas)
add_subdirectory(shader)
add_subdirectory(gl)
add_subdirectory(webcall)
add_subdirectory(dom)
add_subdirectory(unified_object_graph)
add_subdirectory(html)
add_subdirectory(css)
add_subdirectory(layout)
add_subdirectory(render)
add_subdirectory(text)
add_subdirectory(image)
add_subdirectory(network)
add_subdirectory(storage)
add_subdirectory(compat)
add_subdirectory(security)

# ---------------------------------------------------------------------------
# Platform backends
# ---------------------------------------------------------------------------
add_subdirectory(platform)

# ---------------------------------------------------------------------------
# Consumer surfaces
# ---------------------------------------------------------------------------
add_subdirectory(malibu-view)
add_subdirectory(malibu-app-runtime)
add_subdirectory(malibu-electron)
add_subdirectory(malibu-cef)

# ---------------------------------------------------------------------------
# Tools (CLI: headless HTML -> PNG renderer)
# ---------------------------------------------------------------------------
add_subdirectory(tools)

# ---------------------------------------------------------------------------
# Interactive mini-browser (SDL2 + libcurl). Auto-skips if SDL2/curl missing.
# ---------------------------------------------------------------------------
add_subdirectory(malibu-shell)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)

# ---------------------------------------------------------------------------
# Install (optional)
# ---------------------------------------------------------------------------
# install(TARGETS malibu_core malibu_platform_null ...)