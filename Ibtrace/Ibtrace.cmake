set(IbverbsTraceSources
        IbverbsHooks.hpp
        IbverbsEventSource.hpp
        IbverbsPreload.cpp
        # IbverbsEvents.hpp
)

# Main Ibverbs trace libraries
Sigma_add_provider_library(IbverbsTraceShared SHARED ${IbverbsTraceSources})
Sigma_add_provider_library(IbverbsTraceStatic STATIC ${IbverbsTraceSources})

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

    # Headers only - no runtime link dependency on libibverbs
    target_link_libraries(${IbverbsTraceTarget} PRIVATE ibverbs_headers)

    target_link_libraries(${IbverbsTraceTarget} PUBLIC HookLib)
endforeach ()
