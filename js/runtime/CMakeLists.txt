cmake_minimum_required(VERSION 3.25)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GMPXX REQUIRED IMPORTED_TARGET gmpxx)
pkg_check_modules(ICU_I18N REQUIRED IMPORTED_TARGET icu-i18n)

add_library(malibu_js_runtime OBJECT
    objects.cpp
    interpreter.cpp
    builtins.cpp
    typed_arrays.cpp
    regex.cpp
    intl.cpp
)
target_include_directories(malibu_js_runtime PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_js_runtime PUBLIC cxx_std_20)
target_compile_options(malibu_js_runtime PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(malibu_js_runtime PUBLIC
    PkgConfig::GMPXX
    PkgConfig::ICU_I18N
)
