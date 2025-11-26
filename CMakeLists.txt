cmake_minimum_required(VERSION 4.0)
project(Thing C CXX CUDA)

# nccl is unusable when built in Debug mode
# Needed when built with add_subdirectory, not ExternalProject_Add
#set(CMAKE_BUILD_TYPE RelWithDebInfo)

set(CMAKE_VERBOSE_MAKEFILE ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CUDA_STANDARD 20)
set(CMAKE_CUDA_ARCHITECTURES "90")

add_executable(Thing main.cu)

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
        # -DRDMA_CORE=ON
        # -DNET_PROFILER=ON
        # -DMLX5DV=ON

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

target_link_libraries(Thing PRIVATE nccl)