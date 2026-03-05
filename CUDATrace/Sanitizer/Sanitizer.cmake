find_package(CUDAToolkit REQUIRED)

# ================================================================================================
# Sanitizer memory tracker (unused)
set(SANITIZER_ROOT "${CUDAToolkit_LIBRARY_ROOT}/compute-sanitizer")
if (NOT EXISTS "${SANITIZER_ROOT}")
    message(FATAL_ERROR "Could not find compute-sanitizer at ${SANITIZER_ROOT}")
endif ()
add_library(SANITIZER SHARED IMPORTED GLOBAL)
set_target_properties(SANITIZER PROPERTIES
        IMPORTED_LOCATION "${SANITIZER_ROOT}/libsanitizer-public.so"
        INTERFACE_INCLUDE_DIRECTORIES "${SANITIZER_ROOT}/include"
)
set(SANITIZER_INCLUDE_DIR "${SANITIZER_ROOT}/include")

# Not using Sigma_add_library for the flags below
add_library(SanitizerMemoryTracker SHARED
        MemoryTracker.cpp
)
target_link_libraries(SanitizerMemoryTracker PRIVATE SANITIZER)

set_target_properties(SanitizerMemoryTracker PROPERTIES
        SanitizerMemoryTracker ON
        OUTPUT_NAME "sanitizer_memory_tracker"
)
set_target_properties(SanitizerMemoryTracker PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
target_include_directories(SanitizerMemoryTracker PRIVATE ${SANITIZER_INCLUDE_DIR})

# Link against dl for dlsym()
target_link_libraries(SanitizerMemoryTracker PRIVATE dl)
target_link_libraries(SanitizerMemoryTracker PRIVATE CUDA::cudart)

add_library(SanitizerMemoryTrackerFatbin OBJECT MemoryTrackerPatches.cu)
set_property(TARGET SanitizerMemoryTrackerFatbin PROPERTY CUDA_FATBIN_COMPILATION ON)
# If there's a bug in the build it's this
target_compile_options(SanitizerMemoryTrackerFatbin PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:--fatbin --compile-as-tools-patch>)
target_link_libraries(SanitizerMemoryTrackerFatbin PRIVATE SANITIZER)

# ================================================================================================
# API Trace Provider (also unused)
#Sigma_add_provider_library(SanitizerEventProvider SHARED SanitizerTrace.cpp)
#
#target_link_libraries(SanitizerEventProvider PRIVATE SANITIZER)
#target_include_directories(SanitizerEventProvider PRIVATE ${SANITIZER_INCLUDE_DIR})
#target_link_libraries(SanitizerEventProvider PUBLIC CUDATrace)
#
#set_target_properties(SanitizerEventProvider PROPERTIES
#        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}"
#        OUTPUT_NAME "sanitizer_trace"
#)
#
#target_link_libraries(SanitizerEventProvider PRIVATE CUDA::cudart)

