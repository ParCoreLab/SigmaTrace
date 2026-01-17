Sigma_add_library(CUDATrace INTERFACE
        CUDAHooks.hpp
        CUDAEventSource.hpp
)

target_link_libraries(CUDATrace INTERFACE HookLib)
set_target_properties(CUDATrace PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")

target_include_directories(CUDATrace INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

add_subdirectory(Sanitizer)
add_subdirectory(Nvbit)
