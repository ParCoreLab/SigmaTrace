set(Mlx5dvTraceSources
        Mlx5dvHooks.hpp
        Mlx5dvEventSource.hpp
        Mlx5dvPreload.cpp)

Sigma_add_provider_library(Mlx5dvTraceShared SHARED ${Mlx5dvTraceSources})
Sigma_add_provider_library(Mlx5dvTraceStatic STATIC ${Mlx5dvTraceSources})

# Import dlsym/dlvsym
target_link_libraries(Mlx5dvTraceShared PRIVATE
        -Wl,--whole-archive
        # SmartDlsym
        -Wl,--no-whole-archive
)

foreach (Mlx5dvTraceTarget IN ITEMS Mlx5dvTraceShared Mlx5dvTraceStatic)
    set_target_properties(${Mlx5dvTraceTarget} PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_NAME "mlx5dv_trace"
    )

    # Headers without linking
    target_include_directories(
            ${Mlx5dvTraceTarget}
            PRIVATE
            $<TARGET_PROPERTY:SmartDlsym,INTERFACE_INCLUDE_DIRECTORIES>)

    target_link_libraries(${Mlx5dvTraceTarget} PUBLIC HookLib)

    # TODO: Do INTERFACE_INCLUDE_DIRECTORIES in the future
    # Headers only - link-time dependency on libibverbs/libmlx5 breaks dlsym hooks
    target_include_directories(${Mlx5dvTraceTarget} PRIVATE "${RDMA_INCLUDE_DIR}")
endforeach ()

