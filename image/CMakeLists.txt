cmake_minimum_required(VERSION 3.25)

# PNG encode/decode (libpng). Outside core/ — touches files + third-party libs.
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBPNG REQUIRED libpng)

add_library(malibu_image OBJECT
    png.cpp
    jpeg.cpp
    webp.cpp
    svg.cpp
    gif.cpp
)
target_include_directories(malibu_image PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_include_directories(malibu_image SYSTEM PRIVATE ${LIBPNG_INCLUDE_DIRS})
target_compile_features(malibu_image PUBLIC cxx_std_20)
target_compile_options(malibu_image PRIVATE -Wall -Wextra -Wpedantic -Werror)
# libjpeg is loaded at runtime via dlopen (see jpeg.cpp) — link libdl only, so
# the binary has no hard NEEDED dependency on libjpeg.so and launches even where
# that soname is absent (JPEG is then skipped). libpng stays a normal dep.
target_link_libraries(malibu_image PUBLIC ${LIBPNG_LIBRARIES} ${CMAKE_DL_LIBS})
target_link_directories(malibu_image PUBLIC ${LIBPNG_LIBRARY_DIRS})
target_include_directories(malibu_image SYSTEM PRIVATE /usr/include)  # jpeglib.h (headers only)
# libjpeg's setjmp/longjmp error path trips GCC's -Wclobbered with C++ locals.
set_source_files_properties(jpeg.cpp PROPERTIES COMPILE_OPTIONS "-Wno-clobbered")
# gif.cpp has unused function warning for decode_gif (used via decode_image in jpeg.cpp)
set_source_files_properties(gif.cpp PROPERTIES COMPILE_OPTIONS "-Wno-unused-function")
