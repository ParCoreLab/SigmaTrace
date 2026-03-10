set(LibfabricTraceSources
        LibfabricHooks.hpp
        LibfabricEventSource.hpp
        LibfabricPreload.cpp)

Sigma_add_provider_library(LibfabricTraceShared SHARED ${LibfabricTraceSources})
Sigma_add_provider_library(LibfabricTraceStatic STATIC ${LibfabricTraceSources})

foreach (LibfabricTraceTarget IN ITEMS LibfabricTraceShared LibfabricTraceStatic)
    set_target_properties(${LibfabricTraceTarget} PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}"
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${LIBFABRIC_INCLUDE_DIR}"
            OUTPUT_NAME "libfabric_trace"
    )

    # Headers without linking
    Sigma_add_headers(${LibfabricTraceTarget} SmartDlsym)

    target_link_libraries(${LibfabricTraceTarget} PUBLIC HookLib)

    if (LIBFABRIC_INCLUDE_DIR)
        target_include_directories(${LibfabricTraceTarget} SYSTEM PUBLIC "${LIBFABRIC_INCLUDE_DIR}")
    endif ()
endforeach ()
