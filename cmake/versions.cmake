# cmake/versions.cmake — Pinned dependency versions for Malibu Engine
# All versions must be updated here; never inline them in CMakeLists.txt.

set(MALIBU_GOOGLETEST_VERSION     "1.14.0")
set(MALIBU_VULKAN_SDK_MIN_VERSION "1.3.261")
set(MALIBU_FREETYPE_VERSION       "2.13.2")
set(MALIBU_HARFBUZZ_VERSION       "8.3.0")
set(MALIBU_LIBPNG_VERSION         "1.6.40")
set(MALIBU_LIBJPEG_TURBO_VERSION  "3.0.1")
set(MALIBU_NGHTTP2_VERSION        "1.58.0")
set(MALIBU_SIMDJSON_VERSION       "3.6.3")

# GIT tags / commit hashes for FetchContent targets
set(MALIBU_GOOGLETEST_GIT_TAG     "v${MALIBU_GOOGLETEST_VERSION}")
set(MALIBU_SIMDJSON_GIT_TAG       "v${MALIBU_SIMDJSON_VERSION}")
set(MALIBU_NGHTTP2_GIT_TAG        "v${MALIBU_NGHTTP2_VERSION}")
