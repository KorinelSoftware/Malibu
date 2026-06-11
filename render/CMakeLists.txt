cmake_minimum_required(VERSION 3.25)

# CPU rendering pipeline: display list + software rasterizer + glyph atlas.
add_library(malibu_render OBJECT
    display_list/display_list.cpp
    raster/software_rasterizer.cpp
    atlas/glyph_cache.cpp
    renderer.cpp
)
target_include_directories(malibu_render PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_render PUBLIC cxx_std_20)
target_compile_options(malibu_render PRIVATE -Wall -Wextra -Wpedantic -Werror)

# GPU backend (real Vulkan device bring-up; consumes the same display list).
add_library(malibu_render_vulkan OBJECT
    vulkan/vulkan_device.cpp
)
target_include_directories(malibu_render_vulkan PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
# Use the Vulkan headers for includes, but link the loader by name so we are not
# tied to find_package's cached (and on this host, stale) Vulkan_LIBRARY path.
target_include_directories(malibu_render_vulkan SYSTEM PUBLIC "${Vulkan_INCLUDE_DIR}")
target_compile_features(malibu_render_vulkan PUBLIC cxx_std_20)
target_compile_options(malibu_render_vulkan PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(malibu_render_vulkan PUBLIC vulkan)
