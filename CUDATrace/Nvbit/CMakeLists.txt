set(NvbitMemoryTrackerSources
        common.h
        inject_funcs.cu
        mem_trace.cu
)

set_source_files_properties(mem_trace.cu PROPERTIES
        COMPILE_OPTIONS "$<$<COMPILE_LANGUAGE:CUDA>:-g>"
)

# Note: -Xptxas=-astoolspatch conflicts with separable compilation
set_source_files_properties(inject_funcs.cu PROPERTIES
        COMPILE_OPTIONS "$<$<COMPILE_LANGUAGE:CUDA>:-maxrregcount=24;-Xptxas=-astoolspatch;--keep-device-functions>"
)

Sigma_add_library(NvbitMemoryTrackerShared SHARED ${NvbitMemoryTrackerSources})
Sigma_add_library(NvbitMemoryTrackerStatic STATIC ${NvbitMemoryTrackerSources})

foreach (NvbitMemoryTrackerTarget IN ITEMS NvbitMemoryTrackerShared NvbitMemoryTrackerStatic)
    set_property(TARGET ${NvbitMemoryTrackerTarget} PROPERTY ENABLE_EXPORTS ON)

    set_target_properties(${NvbitMemoryTrackerTarget} PROPERTIES POSITION_INDEPENDENT_CODE ON)

    target_link_libraries(${NvbitMemoryTrackerTarget} PRIVATE nvbit)
    target_link_libraries(${NvbitMemoryTrackerTarget} PRIVATE CUDATrace)

    # Compile options adopted from upstream NVBIT Makefile
    target_compile_options(${NvbitMemoryTrackerTarget} PRIVATE
            $<$<COMPILE_LANGUAGE:CUDA>:
            -D_FORCE_INLINES
            -Xptxas=-cloning=no
            -Xcompiler=-Wall
            -Xcompiler=-fPIC
            >
    )

    set_target_properties(${NvbitMemoryTrackerTarget} PROPERTIES
            OUTPUT_NAME "nvbit_memory_tracker"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    )

    target_link_libraries(${NvbitMemoryTrackerTarget} PRIVATE CUDA::cuda_driver)
    target_link_libraries(${NvbitMemoryTrackerTarget} PRIVATE CUDA::cudart)
endforeach ()
