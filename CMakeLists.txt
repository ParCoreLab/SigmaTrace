cmake_minimum_required(VERSION 4.0)
project(Thing C CXX CUDA)

set(CMAKE_VERBOSE_MAKEFILE ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CUDA_STANDARD 20)
set(CMAKE_CUDA_ARCHITECTURES "90")

find_package(CUDAToolkit REQUIRED)

add_library(cuda_helper INTERFACE)
target_include_directories(cuda_helper INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/common")

add_executable(Thing main.cu)

#target_link_libraries(Thing PRIVATE CUDA::cudart)

include(ExternalProject)

ExternalProject_Add(rdma-core
        SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/rdma-core
        BINARY_DIR ${CMAKE_BINARY_DIR}/rdma-core

        CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DIN_PLACE=1

        INSTALL_COMMAND ""
)

# Create a custom target that ensures rdma-core is built
add_custom_target(rdma-core-build ALL
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}/rdma-core
        DEPENDS rdma-core
)

ExternalProject_Get_Property(rdma-core BINARY_DIR)
set(RDMA_CORE_BINARY_DIR ${BINARY_DIR})
file(MAKE_DIRECTORY "${BINARY_DIR}/include")

add_library(ibverbs SHARED IMPORTED)
set_target_properties(ibverbs PROPERTIES
        IMPORTED_LOCATION ${BINARY_DIR}/lib/libibverbs.so
        INTERFACE_INCLUDE_DIRECTORIES
        "${BINARY_DIR}/include"
)
add_dependencies(ibverbs rdma-core-build)

add_library(mlx5 SHARED IMPORTED)
set_target_properties(mlx5 PROPERTIES
        IMPORTED_LOCATION ${BINARY_DIR}/lib/libmlx5.so
        INTERFACE_INCLUDE_DIRECTORIES
        "${BINARY_DIR}/include"
)
add_dependencies(mlx5 rdma-core-build)

target_link_libraries(Thing PRIVATE cuda_helper)
target_link_libraries(Thing PRIVATE ibverbs)

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

        -DVERBS_LIBRARY=${RDMA_CORE_BINARY_DIR}/lib/libibverbs.so
        -DMLX5_LIBRARY=${RDMA_CORE_BINARY_DIR}/lib/libmlx5.so

        INSTALL_COMMAND ""
        DEPENDS rdma-core
)

# Create a custom target that ensures nccl is built
add_custom_target(nccl-build-target ALL
        COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR}/nccl-build
        DEPENDS nccl_external
)

add_library(nccl SHARED IMPORTED)
ExternalProject_Get_Property(nccl_external BINARY_DIR)
set_target_properties(nccl PROPERTIES
        IMPORTED_LOCATION ${BINARY_DIR}/lib/libnccl.so
        INTERFACE_INCLUDE_DIRECTORIES
        "${CMAKE_CURRENT_SOURCE_DIR}/nccl/src/include"
)
add_dependencies(nccl nccl-build-target)
add_dependencies(nccl PRIVATE ibverbs)
add_dependencies(nccl PRIVATE mlx5)

target_link_libraries(Thing PRIVATE nccl)

add_subdirectory(Ibtrace)
add_dependencies(Thing IbtraceTest)
add_dependencies(Thing ProfilerTrace)

#add_dependencies(Ibtrace rdma-core-build)

#add_subdirectory(libibprof)
#add_dependencies(Thing libibprof)

#get_cmake_property(_variableNames VARIABLES)
#list(SORT _variableNames)
#foreach (_variableName ${_variableNames})
#    message(STATUS "${_variableName}=${${_variableName}}")
#endforeach ()
