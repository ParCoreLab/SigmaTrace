add_library(NvbitMemoryTracker SHARED
        common.h
        inject_funcs.cu
        mem_trace.cu
)
target_link_libraries(NvbitMemoryTracker PRIVATE nvbit)

# Compile options adopted from upstream NVBIT Makefile
target_compile_options(NvbitMemoryTracker PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:
        -D_FORCE_INLINES
        -Xptxas=-cloning=no
        -Xcompiler=-Wall
        -Xcompiler=-fPIC
        >
)

# Note: -Xptxas=-astoolspatch conflicts with separable compilation
set_source_files_properties(inject_funcs.cu PROPERTIES
        COMPILE_OPTIONS "$<$<COMPILE_LANGUAGE:CUDA>:-maxrregcount=24;-Xptxas=-astoolspatch;--keep-device-functions>"
)

set_target_properties(NvbitMemoryTracker PROPERTIES
        OUTPUT_NAME "nvbit_memory_tracker"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

target_link_libraries(NvbitMemoryTracker PRIVATE CUDA::cuda_driver)
target_link_libraries(NvbitMemoryTracker PRIVATE CUDA::cudart)