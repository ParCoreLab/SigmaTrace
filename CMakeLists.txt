cmake_minimum_required(VERSION 4.0)
project(Thing C CXX CUDA)

set(CMAKE_VERBOSE_MAKEFILE ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CUDA_STANDARD 20)
set(CMAKE_CUDA_ARCHITECTURES "90")

add_executable(Thing main.cu)

# TODO: ExternalProject_Add with INSTALL might make more sense here
add_subdirectory(rdma-core)
target_link_libraries(Thing PRIVATE ibverbs)
target_link_libraries(Thing PRIVATE mlx5)

include(ExternalProject)
ExternalProject_Add(nccl_external
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/nccl
        BINARY_DIR ${CMAKE_BINARY_DIR}/nccl-build

        # CMake configuration
        CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=Release
        # -DCUDA_TOOLKIT_ROOT_DIR=${CUDAToolkit_ROOT}
        # -DCMAKE_CUDA_FLAGS=-G -g -lineinfo
        # -DCMAKE_CXX_FLAGS=-g -O0
        # -DCMAKE_C_FLAGS=-g -O0
        -DVERBOSE=ON
        -DDEBUG=ON
        -DTRACE=ON
        -DNVTX=ON
        -DRDMA_CORE=ON
        -DNET_PROFILER=ON
        -DMLX5DV=ON

        -DVERBS_LIBRARY=${rdma-core_BINARY_DIR}/lib/libibverbs.so

        INSTALL_COMMAND ""
        BUILD_ALWAYS ON
)

add_library(nccl SHARED IMPORTED)
ExternalProject_Get_Property(nccl_external BINARY_DIR)
set_target_properties(nccl PROPERTIES
        IMPORTED_LOCATION ${BINARY_DIR}/lib/libnccl.so
        INTERFACE_INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}/nccl/src/include"
)
add_dependencies(nccl nccl_external)
add_dependencies(nccl PRIVATE ibverbs)
add_dependencies(nccl PRIVATE mlx5)

target_link_libraries(Thing PRIVATE nccl)

add_subdirectory(Ibtrace)
add_dependencies(Thing Ibtrace)

add_subdirectory(libibprof)
add_dependencies(Thing libibprof)

#get_cmake_property(_variableNames VARIABLES)
#list(SORT _variableNames)
#foreach (_variableName ${_variableNames})
#    message(STATUS "${_variableName}=${${_variableName}}")
#endforeach ()
