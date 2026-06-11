# cmake/webcall_abi_version.cmake — WebCall ABI versioning and link-time symbol enforcement.
#
# Defines WEBCALL_ABI_VERSION and, on Linux/GCC/Clang, adds a linker option to
# malibu_core that requires the symbol `malibu_webcall_abi_version_1` to be
# defined. This causes a link error if the ABI version mismatches.

cmake_minimum_required(VERSION 3.25)

# ---------------------------------------------------------------------------
# ABI version constant — increment this on every breaking ABI change.
# ---------------------------------------------------------------------------
set(WEBCALL_ABI_VERSION 1)

# Derived symbol name used for the link-time check.
set(WEBCALL_ABI_SYMBOL "malibu_webcall_abi_version_${WEBCALL_ABI_VERSION}")

# Propagate the version as a compile definition so C++ code can use it.
# Call malibu_apply_webcall_abi_version(target) for any target that needs it.
function(malibu_apply_webcall_abi_version target)
    target_compile_definitions("${target}" PUBLIC
        MALIBU_WEBCALL_ABI_VERSION=${WEBCALL_ABI_VERSION}
    )

    # Add --require-defined linker option only on Linux with GCC or Clang.
    # This ensures the ABI version symbol is present at final link, catching
    # mismatched library versions at build time rather than at runtime.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_link_options("${target}" PRIVATE
                "-Wl,--require-defined=${WEBCALL_ABI_SYMBOL}"
            )
            message(STATUS
                "WebCall ABI: --require-defined=${WEBCALL_ABI_SYMBOL} applied to ${target}")
        endif()
    endif()
endfunction()
