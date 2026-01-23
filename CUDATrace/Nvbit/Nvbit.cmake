set(NvbitMemoryTrackerSources
        common.h
        inject_funcs.cu
        mem_trace.cu
)

# Note: -Xptxas=-astoolspatch conflicts with separable compilation
set_source_files_properties(inject_funcs.cu PROPERTIES
        COMPILE_OPTIONS "$<$<COMPILE_LANGUAGE:CUDA>:-maxrregcount=24;-Xptxas=-astoolspatch;--keep-device-functions>"
)

Sigma_add_provider_library(NvbitMemoryTrackerShared SHARED ${NvbitMemoryTrackerSources})
Sigma_add_provider_library(NvbitMemoryTrackerStatic STATIC ${NvbitMemoryTrackerSources})

foreach (NvbitMemoryTrackerTarget IN ITEMS NvbitMemoryTrackerShared NvbitMemoryTrackerStatic)
    set_target_properties(${NvbitMemoryTrackerTarget} PROPERTIES OUTPUT_NAME "nvbit_memory_tracker")

    # Public interface
    target_link_libraries(${NvbitMemoryTrackerTarget} PUBLIC CUDATrace)

    # Compile options adopted from upstream NVBIT Makefile
    target_compile_options(${NvbitMemoryTrackerTarget} PRIVATE
            $<$<COMPILE_LANGUAGE:CUDA>:
            -D_FORCE_INLINES
            -Xptxas=-cloning=no
            -Xcompiler=-Wall
            -Xcompiler=-fPIC
            >
    )

    target_link_libraries(${NvbitMemoryTrackerTarget} PRIVATE nvbit)
    target_link_libraries(${NvbitMemoryTrackerTarget} PRIVATE CUDA::cuda_driver)
    target_link_libraries(${NvbitMemoryTrackerTarget} PRIVATE CUDA::cudart)
endforeach ()
