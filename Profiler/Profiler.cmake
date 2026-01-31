#Sigma_source_disable_warnings(mlx5/mlx5_ifc.h mlx5/mlx5_prm.h)
#Sigma_source_disable_errors(mlx5/mlx5_ifc.h mlx5/mlx5_prm.h)

# ================================================================
# Profiler launcher executable
Sigma_add_executable(Profiler Profiler.cpp ProfilerArgs.hpp ProfilerProviders.hpp)
set_target_properties(Profiler PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${SIGMA_TARGET_DIR_FULL}")

target_link_libraries(Profiler PRIVATE Launcher)
# ================================================================

# ================================================================
# Profiler Modules
add_subdirectory(Modules/CudaExportTableOverwrite)
add_subdirectory(Modules/MemoryHijack)
# ================================================================

# ================================================================
# Master LD_PRELOAD Shim for the Profiler
Sigma_add_library(ProfilerShim SHARED ProfilerShim.cpp ProfilerArgs.hpp ProfilerProviders.hpp)

foreach (ProfilerShimTarget IN ITEMS ProfilerShim)
    # Internal headers coped from NVSHMEM/rdma-core tree
    target_include_directories(${ProfilerShimTarget} SYSTEM PRIVATE "mlx5/include")

    set_target_properties(${ProfilerShimTarget} PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            OUTPUT_NAME "${ProfilerShimTarget}"
    )

    # For dlopen/dlsym/dlvsym. Providers make use of the library but only one unit can have the symbols
    target_link_libraries(${ProfilerShimTarget} PUBLIC
            -Wl,--whole-archive
            SmartDlsym
            ProfilerExportTableOverwrite
            -Wl,--no-whole-archive
    )
    set_target_properties(${ProfilerShimTarget} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${SIGMA_TARGET_DIR_FULL}")

    # Profiler modules
    target_link_libraries(${ProfilerShimTarget} PRIVATE ProfilerMemoryRepeater)

    # Runtime dependencies
    target_link_libraries(${ProfilerShimTarget} PRIVATE CUDA::cuda_driver)
    target_link_libraries(${ProfilerShimTarget} PRIVATE CUDA::cudart)
    target_link_libraries(${ProfilerShimTarget} PRIVATE SigmaHelpers)

    # Shim providers
    Sigma_profiler_link_provider(${ProfilerShimTarget} IbverbsTraceShared)
    Sigma_profiler_link_provider(${ProfilerShimTarget} NvbitMemoryTrackerShared)
    Sigma_profiler_link_provider(${ProfilerShimTarget} Mlx5dvTraceShared)

    # Tracing and Logging
    target_link_libraries(${ProfilerShimTarget} PRIVATE EventsLib)
    target_include_directories(${ProfilerShimTarget} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Events")

    # For infiniband/mlx5dv.h headers
    target_include_directories(${ProfilerShimTarget} PRIVATE "${RDMA_INCLUDE_DIR}")

    # Make sure we build before the profiler
    add_dependencies(Profiler ${ProfilerShimTarget})
    target_link_libraries(Profiler PRIVATE ${SIGMA_PROVIDER_PATHS_TARGET})
endforeach ()
# ================================================================

# ================================================================
# Trace results reader
Sigma_add_executable(ProfilerEventReader "TraceFile/ProfilerEventReader.cpp")
set_target_properties(ProfilerEventReader PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${SIGMA_TARGET_DIR_FULL}")

target_link_libraries(ProfilerEventReader PRIVATE EventsLib)
target_include_directories(ProfilerEventReader PRIVATE "${CMAKE_SOURCE_DIR}/Events")

target_link_libraries(ProfilerEventReader PRIVATE PerfettoProto)
target_link_libraries(ProfilerEventReader PRIVATE
        absl::hash
        absl::flat_hash_map
        absl::flat_hash_set)
# ================================================================

