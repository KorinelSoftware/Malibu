cmake_minimum_required(VERSION 3.25)

add_library(malibu_webcall OBJECT
    webcall_dispatch.cpp
)
target_include_directories(malibu_webcall PUBLIC
    "${CMAKE_SOURCE_DIR}/core/include"
)
target_compile_features(malibu_webcall PUBLIC cxx_std_20)
target_compile_options(malibu_webcall PRIVATE -Wall -Wextra -Wpedantic -Werror)

# Apply WebCall ABI version (link-time --require-defined symbol check)
malibu_apply_webcall_abi_version(malibu_webcall)
