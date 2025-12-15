set(NVSHMEM_DEPENDS_LIST "")
if (BUILD_RDMA_CORE)
    list(APPEND NVSHMEM_DEPENDS_LIST rdma-core)
endif ()

include(ExternalProject)
ExternalProject_Add(nvshmem_external
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Imports/nvshmem"
        BINARY_DIR "${CMAKE_BINARY_DIR}/nvshmem-build"
        CMAKE_ARGS
        -DCMAKE_VERBOSE_MAKEFILE=ON
        -DCMAKE_BUILD_TYPE=Debug
        -DCMAKE_CUDA_COMPILER=${CMAKE_CUDA_COMPILER}
        -DCUDA_HOME=${CUDAToolkit_ROOT}
        # -DNCCL_HOME=                           # TODO
        -DCMAKE_CUDA_ARCHITECTURES=${CMAKE_CUDA_ARCHITECTURES}
        -DNVSHMEM_PREFIX=${CMAKE_BINARY_DIR}/nvshmem-build
        -DNVSHMEM_BUILD_TESTS=OFF
        -DNVSHMEM_BUILD_EXAMPLES=ON
        # -D CUTLASS
        # -DNVSHMEM_BUILD_BITCODE_LIBRARY=OFF
        -DNVSHMEM_BUILD_PACKAGES=OFF
        -DNVSHMEM_BUILD_PYTHON_DEVICE_LIB=OFF
        -DNVSHMEM_BUILD_PYTHON_LIB=OFF
        -DNVSHMEM_BUILD_RPM_PACKAGE=OFF
        -DNVSHMEM_BUILD_TGZ_PACKAGE=OFF           # TODO
        -DNVSHMEM_BUILD_TXZ_PACKAGE=OFF           # TODO
        -DNVSHMEM_DEBUG=ON
        -DNVSHMEM_IBDEVX_SUPPORT=ON
        -DNVSHMEM_IBGDA_SUPPORT=ON
        -DNVSHMEM_IBGDA_SUPPORT_GPUMEM_ONLY=ON    # TODO
        -DNVSHMEM_DEVEL=OFF
        # -DNVSHMEM_ENABLE_ALL_DEVICE_INLINING=ON # TODO
        # -DNVSHMEM_GPU_COLL_USE_LDST=ON          # TODO
        -DNVSHMEM_TRACE=ON
        -DNVSHMEM_USE_GDRCOPY=ON                  # TODO
        -DNVSHMEM_USE_MLX5DV=ON
        -DNVSHMEM_MPI_SUPPORT=ON
        -DNVSHMEM_PMIX_SUPPORT=OFF
        -DNVSHMEM_UCX_SUPPORT=OFF                 # TODO
        -DNVSHMEM_NVTX=ON
        -DNVSHMEM_VERBOSE=OFF                     # PTX compiler output
        -DNVSHMEM_USE_NCCL=OFF
        -DVERBS_LIBRARY=${IBVERBS_LIBRARY_PATH}
        -DMLX5_lib=${MLX5_LIBRARY_PATH}
        # INSTALL_COMMAND "install"
        DEPENDS ${NVSHMEM_DEPENDS_LIST}
        BUILD_BYPRODUCTS
        # "${CMAKE_BINARY_DIR}/nvshmem-build/lib/libnvshmem.a"
        "${CMAKE_BINARY_DIR}/nvshmem-build/lib/libnvshmem_host.so"
        "${CMAKE_BINARY_DIR}/nvshmem-build/lib/libnvshmem_device.a"

        BUILD_ALWAYS ON
)

ExternalProject_Get_Property(nvshmem_external BINARY_DIR)

# CMake checks fail without these
file(MAKE_DIRECTORY "${BINARY_DIR}/include")
file(MAKE_DIRECTORY "${BINARY_DIR}/lib")

add_library(nvshmem::nvshmem_device STATIC IMPORTED GLOBAL)
add_library(nvshmem::nvshmem_host SHARED IMPORTED GLOBAL)

set_target_properties(nvshmem::nvshmem_device PROPERTIES
        IMPORTED_LOCATION "${BINARY_DIR}/lib/libnvshmem_device.a"
        INTERFACE_INCLUDE_DIRECTORIES "${BINARY_DIR}/include"
)

set_target_properties(nvshmem::nvshmem_host PROPERTIES
        IMPORTED_LOCATION "${BINARY_DIR}/lib/libnvshmem_host.so"
        INTERFACE_INCLUDE_DIRECTORIES "${BINARY_DIR}/include"
)

add_dependencies(nvshmem::nvshmem_device nvshmem_external)
add_dependencies(nvshmem::nvshmem_host nvshmem_external)

# 2. Link CUDA Runtime and MPI (if used)
#set(_nvshmem_interface_libs CUDA::cudart)
#if(MPI_FOUND)
#    list(APPEND _nvshmem_interface_libs MPI::MPI_CXX)
#endif()

#set_target_properties(nvshmem::nvshmem PROPERTIES
#        INTERFACE_LINK_LIBRARIES "${_nvshmem_interface_libs}"
#)