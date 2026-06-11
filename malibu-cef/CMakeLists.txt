cmake_minimum_required(VERSION 3.25)

# MalibuCEF: CEF3-compatible embedding subset (Task 34), backed by MalibuView.
add_library(malibu_cef STATIC
    src/cef.cpp
)
target_include_directories(malibu_cef PUBLIC
    "${CMAKE_SOURCE_DIR}/malibu-cef/include"
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_cef PUBLIC cxx_std_20)
target_compile_options(malibu_cef PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(malibu_cef PUBLIC
    malibu_view        # brings the whole engine
    malibu_diagnostics
)
