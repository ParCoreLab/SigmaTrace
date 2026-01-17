set(Mlx5dvTraceSources
        Mlx5dvHooks.hpp
        Mlx5dvEventSource.hpp
        Mlx5dvPreload.cpp)

Sigma_add_library(Mlx5dvTraceSourcesShared SHARED ${Mlx5dvTraceSources})
Sigma_add_library(Mlx5dvTraceSourcesStatic STATIC ${Mlx5dvTraceSources})

# Import dlsym/dlvsym
target_link_libraries(Mlx5dvTraceSourcesShared PUBLIC
        -Wl,--whole-archive
        SmartDlsym
        -Wl,--no-whole-archive
)

foreach (Mlx5dvTraceTarget IN ITEMS Mlx5dvTraceSourcesShared Mlx5dvTraceSourcesStatic)
    set_target_properties(${Mlx5dvTraceTarget} PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_NAME "mlx5dv_trace"
    )

    set_property(TARGET ${Mlx5dvTraceTarget} PROPERTY ENABLE_EXPORTS ON)

    target_link_libraries(${Mlx5dvTraceTarget} INTERFACE mlx5)

    # Headers without linking
    target_include_directories(
            ${Mlx5dvTraceTarget}
            PRIVATE
            $<TARGET_PROPERTY:SmartDlsym,INTERFACE_INCLUDE_DIRECTORIES>)

    # target_link_libraries(${Mlx5dvTraceTarget} PRIVATE ibverbs)
    # target_link_libraries(${Mlx5dvTraceTarget} PRIVATE cuda_helper)
    # target_link_libraries(${Mlx5dvTraceTarget} PRIVATE CUDA::cudart)
    # target_link_libraries(${Mlx5dvTraceTarget} PUBLIC CUDA::nvtx3)

    # target_link_libraries(${Mlx5dvTraceTarget} PRIVATE Events)

    target_link_libraries(${Mlx5dvTraceTarget} PUBLIC HookLib)

    set_target_properties(${Mlx5dvTraceTarget} PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
endforeach ()

