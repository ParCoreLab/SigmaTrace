set(IbverbsTraceSources
        IbverbsHooks.hpp
        IbverbsEventSource.hpp
        IbverbsPreload.cpp
        IbverbsEvents.hpp
        dl_symbol_resolver.cpp)

# Main Ibverbs trace libraries
# Why do we have both static and shared? TODO: explain
add_library(IbverbsTraceShared SHARED ${IbverbsTraceSources})
add_library(IbverbsTraceStatic STATIC ${IbverbsTraceSources})

## Very light test library
#add_library(IbverbsTraceTest SHARED
#        ibverbs_trace.hpp
#        ibverbs_preload.cpp
#        IbverbsEvents.hpp
#)

foreach (IbverbsTraceTarget IN ITEMS IbverbsTraceShared IbverbsTraceStatic)
    set_target_properties(${IbverbsTraceTarget} PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            OUTPUT_NAME "ibverbs_trace"
    )

    set_property(TARGET ${IbverbsTraceTarget} PROPERTY ENABLE_EXPORTS ON)

    target_include_directories(${IbverbsTraceTarget} PRIVATE "${RDMA_INCLUDE_DIR}")

    # Link against dl for dlsym()
    target_link_libraries(${IbverbsTraceTarget} PUBLIC dl)
    # This breaks things
    # Should load symbols dynamically
    # TODO: not having it here breaks other things...
    target_link_libraries(${IbverbsTraceTarget} PRIVATE ibverbs)
    target_link_libraries(${IbverbsTraceTarget} PRIVATE cuda_helper)
    target_link_libraries(${IbverbsTraceTarget} PRIVATE CUDA::cudart)
    target_link_libraries(${IbverbsTraceTarget} PUBLIC CUDA::nvtx3)

    target_link_libraries(${IbverbsTraceTarget} PRIVATE Events)

    target_link_libraries(${IbverbsTraceTarget} PUBLIC HookLib)

    set_target_properties(${IbverbsTraceTarget} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
endforeach ()

