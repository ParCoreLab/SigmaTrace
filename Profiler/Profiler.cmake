# Profiler trace, same config as above
add_library(ProfilerShim SHARED
        ProfilerShim.cpp
)

#set_property(TARGET ProfilerShim PROPERTY ENABLE_EXPORTS ON)

set_target_properties(ProfilerShim PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        OUTPUT_NAME "profiler_shim"
)

# Link against dl for dlsym()
target_link_libraries(ProfilerShim PUBLIC dl)

# This makes it so we can propagate overridden functions I think?
target_link_libraries(ProfilerShim PUBLIC
        -Wl,--whole-archive
        IbverbsTraceStatic
        NvbitMemoryTrackerStatic
        -Wl,--no-whole-archive
)
target_include_directories(ProfilerShim PRIVATE ${CMAKE_SOURCE_DIR}/Ibtrace)

target_link_libraries(ProfilerShim PUBLIC CUDATrace)

set_target_properties(ProfilerShim PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

add_executable(Profiler Profiler.cpp)
target_link_libraries(Profiler PRIVATE Launcher)

target_include_directories(Profiler PRIVATE ${CMAKE_SOURCE_DIR}/Ibtrace)
#target_link_libraries(Profiler PRIVATE ProfilerTrace)

set_property(TARGET Profiler PROPERTY ENABLE_EXPORTS ON)

target_link_libraries(Profiler PRIVATE CUDATrace)
#target_link_libraries(Profiler PRIVATE NvbitMemoryTracker)
