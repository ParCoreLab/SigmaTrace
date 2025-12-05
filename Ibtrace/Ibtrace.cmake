# TODO: Remove duplicate library definition
add_library(IbtraceTest SHARED
        ibverbs_trace.hpp
        ibverbs_preload.cpp
)

target_include_directories(IbtraceTest PRIVATE "${RDMA_CORE_BINARY_DIR}/include")

set_target_properties(IbtraceTest PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        OUTPUT_NAME "ibverbs_trace_test"
)

# Link against dl for dlsym()
target_link_libraries(IbtraceTest PRIVATE dl)
target_link_libraries(IbtraceTest PRIVATE ibverbs)

set_target_properties(IbtraceTest PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

add_executable(test_ibtrace test_ibtrace.cpp)
target_link_libraries(test_ibtrace PRIVATE IbtraceTest ibverbs)

# Profiler trace, same config as above
add_library(ProfilerTrace SHARED
        ibverbs_trace.hpp
        profiler_preload.cpp
)

target_include_directories(ProfilerTrace PRIVATE "${RDMA_CORE_BINARY_DIR}/include")

set_target_properties(ProfilerTrace PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        OUTPUT_NAME "profiler_trace"
)

# Link against dl for dlsym()
target_link_libraries(ProfilerTrace PRIVATE dl)
target_link_libraries(ProfilerTrace PRIVATE ibverbs)
target_link_libraries(ProfilerTrace PRIVATE cuda_helper)
target_link_libraries(ProfilerTrace PRIVATE CUDA::cudart)

set_target_properties(ProfilerTrace PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

add_executable(ibverbs_profiler profiler.cpp)
target_link_libraries(ibverbs_profiler PRIVATE ProfilerTrace ibverbs)
target_link_libraries(ibverbs_profiler PRIVATE CUDA::cudart)

# Ensure recompilation on change
add_dependencies(ibverbs_profiler Thing)
