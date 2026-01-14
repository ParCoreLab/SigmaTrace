add_library(ProfilerShim SHARED
        mlx5/mlx5_ifc.h
        mlx5/mlx5_prm.h
        ProfilerShim.cpp
)

#set_property(TARGET ProfilerShim PROPERTY ENABLE_EXPORTS ON)

set_target_properties(ProfilerShim PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        OUTPUT_NAME "profiler_shim"
)

# This makes it so we can propagate overridden functions I think?
target_link_libraries(ProfilerShim PUBLIC
        -Wl,--whole-archive
        SmartDlsym
        IbverbsTraceStatic
        NvbitMemoryTrackerStatic
        Mlx5dvTraceSourcesStatic
        -Wl,--no-whole-archive
)
set_target_properties(ProfilerShim PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Shim dependencies
target_include_directories(ProfilerShim PRIVATE ${CMAKE_SOURCE_DIR}/Ibtrace)
target_link_libraries(ProfilerShim PUBLIC CUDATrace)

# Tracing and Logging
target_link_libraries(ProfilerShim PRIVATE EventsLib)

# Not using protobuf for now
#add_subdirectory(Events)
#target_link_libraries(ProfilerShim PRIVATE EventsLibProtobuf ProfilerProtoSimple)
#target_link_libraries(ProfilerShim PRIVATE EventsLibProtobuf ProfilerProtoMlx5dv)
target_include_directories(ProfilerShim PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Events")

# For infiniband/mlx5dv.h
target_include_directories(ProfilerShim PRIVATE "${RDMA_INCLUDE_DIR}")

add_executable(Profiler Profiler.cpp)
target_link_libraries(Profiler PRIVATE Launcher)

target_include_directories(Profiler PRIVATE ${CMAKE_SOURCE_DIR}/Ibtrace)
#target_link_libraries(Profiler PRIVATE ProfilerTrace)

set_property(TARGET Profiler PROPERTY ENABLE_EXPORTS ON)

target_link_libraries(Profiler PRIVATE CUDATrace)
#target_link_libraries(Profiler PRIVATE NvbitMemoryTracker)

add_executable(ProfilerEventReader "TraceFile/ProfilerEventReader.cpp")
target_link_libraries(ProfilerEventReader PRIVATE EventsLib)
target_include_directories(ProfilerEventReader PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Events")

target_link_libraries(ProfilerEventReader PRIVATE PerfettoProto)
