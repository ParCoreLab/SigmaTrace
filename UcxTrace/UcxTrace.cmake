set(UcxTraceSources
        UcxHooks.hpp
        UcxEventSource.hpp
        UcxPreload.cpp)

Sigma_add_provider_library(UcxTraceShared SHARED ${UcxTraceSources})
Sigma_add_provider_library(UcxTraceStatic STATIC ${UcxTraceSources})

foreach (UcxTraceTarget IN ITEMS UcxTraceShared UcxTraceStatic)
    set_target_properties(${UcxTraceTarget} PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}"
            INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${UCX_INCLUDE_DIR}"
            OUTPUT_NAME "ucx_trace"
    )

    # Headers without linking
    Sigma_add_headers(${UcxTraceTarget} SmartDlsym)

    target_link_libraries(${UcxTraceTarget} PUBLIC HookLib)

    if (UCX_INCLUDE_DIR)
        target_include_directories(${UcxTraceTarget} SYSTEM PUBLIC "${UCX_INCLUDE_DIR}")
    endif ()
endforeach ()
