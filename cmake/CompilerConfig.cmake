# Use Sigma_add_executable/Sigma_add_library to apply these settings

# ================================================================
# Language Standards
# ================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 20)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

if (NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES "native")
endif ()

# ================================================================
# Build Type
# ================================================================
if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type (Debug, Release, RelWithDebInfo)" FORCE)
    message(STATUS "Build type not specified, defaulting to Debug")
endif ()

set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")

# ================================================================
# Compiler Identification
# ================================================================
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(SIGMA_COMPILER_IS_GCC TRUE)
    message(STATUS "Compiler: GCC ${CMAKE_CXX_COMPILER_VERSION}")
elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(SIGMA_COMPILER_IS_CLANG TRUE)
    message(STATUS "Compiler: Clang ${CMAKE_CXX_COMPILER_VERSION}")
endif ()

# ================================================================
# Warning Flags
# ================================================================

set(SIGMA_CXX_WARNINGS
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wcast-align
        # -Wunused
        # No easy way to suppress
        -Wno-unused-local-typedefs
        -Wconversion
        -Wsign-conversion
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough
        -Wmissing-include-dirs
        -Wundef
        -Wcast-qual
        -Wwrite-strings
        -Wdisabled-optimization
        -Wredundant-decls
        -Wmissing-declarations
)

set(SIGMA_CXX_WARNINGS_GCC
        -Wduplicated-cond
        -Wduplicated-branches
        -Wlogical-op
        -Wuseless-cast
        -Wno-maybe-uninitialized
)

set(SIGMA_CXX_WARNINGS_CLANG
        -Wshadow-all
        -Wno-unknown-warning-option
)

set(SIGMA_CUDA_WARNINGS
        -Werror=all-warnings
        -Wreorder
        # -Wdefault-stream-launch
        -Wext-lambda-captures-this
)

# ================================================================
# Build Configuration Flags
# ================================================================
# Host compiler
set(SIGMA_CXX_FLAGS_DEBUG -O0 -g3 -fno-omit-frame-pointer)
set(SIGMA_CXX_FLAGS_RELEASE -Werror -O3 -march=native -ffast-math)
set(SIGMA_CXX_FLAGS_RELWITHDEB -O2 -g -fno-omit-frame-pointer)

# CUDA
set(SIGMA_CUDA_FLAGS_DEBUG -g -O0 --generate-line-info)
set(SIGMA_CUDA_FLAGS_RELEASE -O3 --use_fast_math --extra-device-vectorization)
set(SIGMA_CUDA_FLAGS_RELWITHDEB -O2 -g --generate-line-info)

# ================================================================
# Optional Features
# ================================================================
option(SIGMA_ENABLE_LTO "Enable Link Time Optimization for Release builds" OFF)
option(SIGMA_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(SIGMA_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(SIGMA_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

# ================================================================
# Semi-internal: Apply compile options to a target
# ================================================================
function(Sigma_apply_compile_options target)
    # C/CXX warnings
    target_compile_options(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:C,CXX>:${SIGMA_CXX_WARNINGS}>"
    )

    # Compiler-specific warnings
    if (SIGMA_COMPILER_IS_GCC)
        target_compile_options(${target} PRIVATE
                "$<$<COMPILE_LANGUAGE:C,CXX>:${SIGMA_CXX_WARNINGS_GCC}>"
        )
    elseif (SIGMA_COMPILER_IS_CLANG)
        target_compile_options(${target} PRIVATE
                "$<$<COMPILE_LANGUAGE:C,CXX>:${SIGMA_CXX_WARNINGS_CLANG}>"
        )
    endif ()

    # C/CXX build type flags
    target_compile_options(${target} PRIVATE
            "$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<CONFIG:Debug>>:${SIGMA_CXX_FLAGS_DEBUG}>"
            "$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<CONFIG:Release>>:${SIGMA_CXX_FLAGS_RELEASE}>"
            "$<$<AND:$<COMPILE_LANGUAGE:C,CXX>,$<CONFIG:RelWithDebInfo>>:${SIGMA_CXX_FLAGS_RELWITHDEB}>"
    )

    # CUDA warnings and build type flags
    target_compile_options(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:CUDA>:${SIGMA_CUDA_WARNINGS}>"
            "$<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:Debug>>:${SIGMA_CUDA_FLAGS_DEBUG}>"
            "$<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:Release>>:${SIGMA_CUDA_FLAGS_RELEASE}>"
            "$<$<AND:$<COMPILE_LANGUAGE:CUDA>,$<CONFIG:RelWithDebInfo>>:${SIGMA_CUDA_FLAGS_RELWITHDEB}>"
    )

    # Compile definitions
    target_compile_definitions(${target} PRIVATE
            $<$<CONFIG:Debug>:DEBUG>
            $<$<CONFIG:Release>:NDEBUG>
            $<$<CONFIG:RelWithDebInfo>:NDEBUG>
    )

    # LTO
    if (SIGMA_ENABLE_LTO)
        include(CheckIPOSupported)
        check_ipo_supported(RESULT lto_supported)
        if (lto_supported)
            set_target_properties(${target} PROPERTIES
                    INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE
                    INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE
            )
        endif ()
    endif ()

    # Sanitizers
    if (SIGMA_ENABLE_ASAN)
        target_compile_options(${target} PRIVATE -fsanitize=address)
        target_link_options(${target} PRIVATE -fsanitize=address)
    endif ()

    if (SIGMA_ENABLE_UBSAN)
        target_compile_options(${target} PRIVATE -fsanitize=undefined)
        target_link_options(${target} PRIVATE -fsanitize=undefined)
    endif ()

    if (SIGMA_ENABLE_TSAN)
        target_compile_options(${target} PRIVATE -fsanitize=thread)
        target_link_options(${target} PRIVATE -fsanitize=thread)
    endif ()
endfunction()

# ================================================================
# Wrapper Functions
# ================================================================
# Sigma_add_executable(name [EXCLUDE_FROM_ALL] source1 ...)
function(Sigma_add_executable name)
    add_executable(${name} ${ARGN})
    Sigma_apply_compile_options(${name})
endfunction()

# Sigma_add_library(name [STATIC|SHARED|MODULE|OBJECT|INTERFACE] [EXCLUDE_FROM_ALL] source1 ...)
function(Sigma_add_library name)
    add_library(${name} ${ARGN})
    get_target_property(type ${name} TYPE)
    if (NOT type STREQUAL "INTERFACE_LIBRARY")
        Sigma_apply_compile_options(${name})
    endif ()
endfunction()

# Sigma_add_cuda_library(name [STATIC|SHARED] source1 ...)
# Convenience for CUDA libraries with separable compilation
function(Sigma_add_cuda_library name)
    add_library(${name} ${ARGN})
    Sigma_apply_compile_options(${name})
    set_target_properties(${name} PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
endfunction()

# ================================================================
# Per-Source File Overrides
# ================================================================
# Disable all warnings for specific source files
function(Sigma_source_disable_warnings)
    set_source_files_properties(${ARGN} PROPERTIES COMPILE_OPTIONS "-w")
endfunction()

# Disable -Werror for specific source files
function(Sigma_source_disable_errors)
    set_source_files_properties(${ARGN} PROPERTIES COMPILE_OPTIONS "-Wno-error")
endfunction()
