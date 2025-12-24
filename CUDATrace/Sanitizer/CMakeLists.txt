find_package(CUDAToolkit REQUIRED)

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
target_compile_options(SanitizerMemoryTrackerFatbin PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:--compile-as-tools-patch>)
target_link_libraries(SanitizerMemoryTrackerFatbin PRIVATE SANITIZER)

add_custom_target(SanitizerMemoryTrackerFatbinCopy ALL
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_OBJECTS:SanitizerMemoryTrackerFatbin> "${CMAKE_BINARY_DIR}/lib/"
        DEPENDS SanitizerMemoryTrackerFatbin
        COMMENT "Copying fatbin to ${CMAKE_BINARY_DIR}/lib"
)

add_dependencies(SanitizerMemoryTracker SanitizerMemoryTrackerFatbin)
