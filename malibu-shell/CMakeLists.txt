cmake_minimum_required(VERSION 3.25)

# malibu-shell: an interactive SDL2 mini-browser driving the Malibu engine.
# Lives outside core/ — it uses OS APIs (SDL2 window/input) and libcurl (TLS),
# which the engine itself never touches (host provides bytes via request_handler).
find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 sdl2)

if(NOT SDL2_FOUND)
    message(STATUS "malibu-shell: SDL2 not found — skipping the mini-browser")
    return()
endif()

add_executable(malibu_shell src/main.cpp)
target_include_directories(malibu_shell PRIVATE
    "${CMAKE_SOURCE_DIR}/malibu-view/include"
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_include_directories(malibu_shell SYSTEM PRIVATE ${SDL2_INCLUDE_DIRS})
target_compile_features(malibu_shell PRIVATE cxx_std_20)
target_compile_options(malibu_shell PRIVATE -Wall -Wextra)
target_link_libraries(malibu_shell PRIVATE
    malibu_view
    malibu_image
    malibu_host_network
    ${SDL2_LIBRARIES}
)
target_link_directories(malibu_shell PRIVATE ${SDL2_LIBRARY_DIRS})
set_target_properties(malibu_shell PROPERTIES OUTPUT_NAME malibu-browser)

# libjpeg/libpng live in the multiarch dir which isn't always on the default
# runtime linker path here — embed an RPATH so the binary finds them directly.
set_target_properties(malibu_shell PROPERTIES
    BUILD_RPATH   "/usr/lib/x86_64-linux-gnu;/usr/lib"
    INSTALL_RPATH "/usr/lib/x86_64-linux-gnu;/usr/lib")
