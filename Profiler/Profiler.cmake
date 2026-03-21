# ================================================================
# Profiler launcher executable
Sigma_add_executable(Profiler
        Profiler.cpp
        Args/ProfilerArgsParser.hpp
        Args/ProfilerArgs.hpp
        ProfilerFeatures.hpp
        ProfilerProviders.hpp)
set_target_properties(Profiler PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${SIGMA_TARGET_DIR_FULL}")

target_link_libraries(Profiler PRIVATE Launcher)
target_link_libraries(Profiler PRIVATE Boost::program_options)
# ================================================================

# ================================================================
# Profiler Modules
if (SIGMA_HAS_CUDA)
    add_subdirectory(Modules/CudaExportTableOverwrite)
endif ()
add_subdirectory(Modules/MemoryHijack)
#add_subdirectory(Modules/GPUMemoryHijack)
add_subdirectory(Modules/RankDetector)
# ================================================================

# ================================================================
# Master LD_PRELOAD Shim for the Profiler
# Build the source list conditionally based on available features
set(PROFILER_SHIM_SOURCES
        ProfilerShim.hpp ProfilerShim.cpp
        Handlers/Common.hpp
        Args/ProfilerArgs.hpp
        ProfilerFeatures.hpp
        ProfilerProviders.hpp)

if (SIGMA_HAS_CUDA)
    list(APPEND PROFILER_SHIM_SOURCES Handlers/CUPTI.cpp)
    if (SIGMA_HAS_RDMA_CORE)
        list(APPEND PROFILER_SHIM_SOURCES Handlers/CUDAHijack.cpp)
    endif ()
endif ()

if (SIGMA_HAS_RDMA_CORE)
    list(APPEND PROFILER_SHIM_SOURCES
            Handlers/IBV/IBVCommon.hpp
            Handlers/IBV/Ibverbs.cpp
            Handlers/IBV/Mlx5dv.cpp)
endif ()

if (SIGMA_HAS_LIBFABRIC)
    list(APPEND PROFILER_SHIM_SOURCES Handlers/Libfabric.cpp)
endif ()

if (SIGMA_HAS_UCX)
    list(APPEND PROFILER_SHIM_SOURCES Handlers/UCX.cpp)
endif ()

Sigma_add_library(ProfilerShim SHARED ${PROFILER_SHIM_SOURCES})

foreach (ProfilerShimTarget IN ITEMS ProfilerShim)
    set_target_properties(${ProfilerShimTarget} PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            OUTPUT_NAME "${ProfilerShimTarget}"
    )

    # Might fix something that's not broken yet
    # target_link_options(${ProfilerShimTarget} PRIVATE
    #         "LINKER:--unresolved-symbols=ignore-all"
    # )

    # For dlopen/dlsym/dlvsym. Providers make use of the library but only one unit can have the symbols
    target_link_libraries(${ProfilerShimTarget} PUBLIC
            -Wl,--whole-archive
            SmartDlsym
            -Wl,--no-whole-archive
    )

    if (SIGMA_HAS_CUDA)
        target_link_libraries(${ProfilerShimTarget} PUBLIC
                -Wl,--whole-archive
                ProfilerExportTableOverwrite
                -Wl,--no-whole-archive
        )
    endif ()

    set_target_properties(${ProfilerShimTarget} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${SIGMA_TARGET_DIR_FULL}")

    # Profiler modules
    target_link_libraries(${ProfilerShimTarget} PRIVATE ProfilerMemoryRepeater)
    # target_link_libraries(${ProfilerShimTarget} PRIVATE ProfilerGPUMemoryRepeater)
    target_link_libraries(${ProfilerShimTarget} PRIVATE ProfilerRankDetector)

    if (SIGMA_HAS_CUDA)
        target_link_libraries(${ProfilerShimTarget} INTERFACE CUDATrace)

        Sigma_add_headers(${ProfilerShimTarget} CUDA::cudart SYSTEM)
        Sigma_add_headers(${ProfilerShimTarget} CUDA::cuda_driver SYSTEM)

        Sigma_profiler_link_provider(${ProfilerShimTarget} CuptiEventProvider)
        # Sigma_profiler_link_provider(${ProfilerShimTarget} NvbitMemoryTrackerShared)
        # Sigma_profiler_link_provider(${ProfilerShimTarget} SanitizerMemoryTracker)
    endif ()

    if (SIGMA_HAS_HIP)
        Sigma_add_headers(${ProfilerShimTarget} hip::host SYSTEM)
    endif ()

    if (SIGMA_HAS_RDMA_CORE)
        # Internal headers copied from NVSHMEM/rdma-core tree
        target_include_directories(${ProfilerShimTarget} SYSTEM PRIVATE "mlx5/include")

        # For infiniband/mlx5dv.h headers system includes
        target_link_libraries(${ProfilerShimTarget} PRIVATE ibverbs_headers)

        Sigma_profiler_link_provider(${ProfilerShimTarget} IbverbsTraceShared)
        Sigma_profiler_link_provider(${ProfilerShimTarget} Mlx5dvTraceShared)
    endif ()

    if (SIGMA_HAS_LIBFABRIC)
        Sigma_profiler_link_provider(${ProfilerShimTarget} LibfabricTraceShared)
    endif ()

    if (SIGMA_HAS_UCX)
        Sigma_profiler_link_provider(${ProfilerShimTarget} UcxTraceShared)
    endif ()

    target_link_libraries(${ProfilerShimTarget} PRIVATE SigmaHelpers)

    # Tracing and Logging
    target_link_libraries(${ProfilerShimTarget} PRIVATE EventsLib)
    target_include_directories(${ProfilerShimTarget} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Events")

    # Make sure we build before the profiler
    add_dependencies(Profiler ${ProfilerShimTarget})
    target_link_libraries(Profiler PRIVATE ${SIGMA_PROVIDER_PATHS_TARGET})
endforeach ()
# ================================================================

# ================================================================
# Trace results reader
if (SIGMA_HAS_PERFETTO)
    Sigma_add_executable(ProfilerEventReader "TraceFile/ProfilerEventReader.cpp")
    set_target_properties(ProfilerEventReader PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${SIGMA_TARGET_DIR_FULL}")

    target_link_libraries(ProfilerEventReader PRIVATE EventsLib)
    target_include_directories(ProfilerEventReader PRIVATE "${CMAKE_SOURCE_DIR}/Events")

    target_link_libraries(ProfilerEventReader PRIVATE PerfettoProto)
    target_link_libraries(ProfilerEventReader PRIVATE
            absl::hash
            absl::flat_hash_map
            absl::flat_hash_set)
else ()
    message(WARNING "Did not build ProfilerEventReader. Make sure Imports/perfetto is up to date or PERFETTO_ROOT is set")
endif ()
# ================================================================
