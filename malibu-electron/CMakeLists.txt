cmake_minimum_required(VERSION 3.25)

# Malibu Electron Runtime (Tasks 33/35): runs Electron-style apps with the main
# process on MalibuJS and renderers on MalibuView — no Node, no Chromium.
# (A separate N-API bridge for running packaged Electron apps unmodified would
# require Node.js headers and is out of scope for the Node-less runtime.)
add_library(malibu_electron STATIC
    electron_runtime.cpp
)
target_include_directories(malibu_electron PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_electron PUBLIC cxx_std_20)
target_compile_options(malibu_electron PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(malibu_electron PUBLIC
    malibu_view   # renderer + brings the whole engine
)
