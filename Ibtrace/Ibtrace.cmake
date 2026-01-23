set(IbverbsTraceSources
        IbverbsHooks.hpp
        IbverbsEventSource.hpp
        IbverbsPreload.cpp
        # IbverbsEvents.hpp
)

# Main Ibverbs trace libraries
Sigma_add_provider_library(IbverbsTraceShared SHARED ${IbverbsTraceSources})
Sigma_add_provider_library(IbverbsTraceStatic STATIC ${IbverbsTraceSources})

# TODO: haven't found a good way to link against this.
# Main profiler that dlopen's this lib already includes the SmartDlsym symbols
target_link_libraries(IbverbsTraceShared PRIVATE
        -Wl,--whole-archive
        # SmartDlsym
        -Wl,--no-whole-archive
)

foreach (IbverbsTraceTarget IN ITEMS IbverbsTraceShared IbverbsTraceStatic)
    set_target_properties(${IbverbsTraceTarget} PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_NAME "ibverbs_trace"
    )

    # Headers without linking
    target_include_directories(
            ${IbverbsTraceTarget}
            PRIVATE
            $<TARGET_PROPERTY:SmartDlsym,INTERFACE_INCLUDE_DIRECTORIES>)

    # This breaks things
    # Should load symbols dynamically
    # TODO: not having it here breaks other things...
    target_link_libraries(${IbverbsTraceTarget} PRIVATE ibverbs)

    target_link_libraries(${IbverbsTraceTarget} PUBLIC HookLib)

    # verbs.h header
    target_include_directories(${IbverbsTraceTarget} PRIVATE "${RDMA_INCLUDE_DIR}")
endforeach ()
