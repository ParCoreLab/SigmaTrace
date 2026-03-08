option(SIGMA_ENABLE_NVSHMEM "Enable NVSHMEM" ON)
option(BUILD_NVSHMEM "Build bundled NVSHMEM" OFF)
set(NVSHMEM_ROOT "" CACHE PATH "Prefix where system NVSHMEM is installed")

if (NOT SIGMA_ENABLE_NVSHMEM)
    message(STATUS "[Sigma] NVSHMEM: disabled by user")
    return()
endif ()

if (NOT SIGMA_HAS_CUDA)
    message(STATUS "[Sigma] NVSHMEM: disabled (no CUDA backend)")
    return()
endif ()

if (BUILD_NVSHMEM)
    set(NVSHMEM_DEPENDS_LIST "")
    if (BUILD_RDMA_CORE)
        list(APPEND NVSHMEM_DEPENDS_LIST rdma-core ibverbs verbs mlx5)
    endif ()

    include(ExternalProject)
    ExternalProject_Add(nvshmem_external
            SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Imports/nvshmem"
            BINARY_DIR "${CMAKE_BINARY_DIR}/nvshmem-build"
            CMAKE_ARGS
            # Performance killers
            # Release build also kills compile time.
            # Debug is too slow to run, Release takes too long to compile with ...DEVICE_INLINING=ON
            -DCMAKE_BUILD_TYPE=RelWithDebInfo
            -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
            -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DCUDA_HOME=${CUDAToolkit_LIBRARY_ROOT}
            -DCMAKE_VERBOSE_MAKEFILE=ON
            -DNVSHMEM_DEBUG=OFF
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
            -DNVSHMEM_IBDEVX_SUPPORT=ON
            -DNVSHMEM_IBGDA_SUPPORT=ON
            -DNVSHMEM_IBGDA_SUPPORT_GPUMEM_ONLY=ON   # For compatibility with system NVSHMEM. Enable for optimizations
            -DNVSHMEM_DEVEL=OFF
            # Compile time killer
            -DNVSHMEM_ENABLE_ALL_DEVICE_INLINING=OFF  # Allows sources to show on ncu
            # -DNVSHMEM_GPU_COLL_USE_LDST=ON          # TODO
            -DNVSHMEM_TRACE=OFF
            -DNVSHMEM_USE_GDRCOPY=ON                  # TODO
            -DNVSHMEM_USE_MLX5DV=ON
            -DNVSHMEM_MPI_SUPPORT=ON
            -DNVSHMEM_PMIX_SUPPORT=OFF
            -DNVSHMEM_UCX_SUPPORT=OFF                 # TODO
            -DNVSHMEM_NVTX=ON
            -DNVSHMEM_VERBOSE=ON                      # PTX compiler output
            -DNVSHMEM_USE_NCCL=OFF
            -DVERBS_LIBRARY=${IBVERBS_LIBRARY_PATH}
            -DMLX5_lib=${MLX5_LIBRARY_PATH}
            # INSTALL_COMMAND "install"
            DEPENDS ${NVSHMEM_DEPENDS_LIST}
            BUILD_BYPRODUCTS
            # "${CMAKE_BINARY_DIR}/nvshmem-build/lib/libnvshmem.a"
            "${CMAKE_BINARY_DIR}/nvshmem-build/lib/libnvshmem_host.so"
            "${CMAKE_BINARY_DIR}/nvshmem-build/lib/libnvshmem_device.a"

            BUILD_ALWAYS OFF
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


    Sigma_enable_feature(NVSHMEM)
else ()
    # TODO: Version might break it
    # Bit of a hack to get NVSHMEM from nvhpc toolkit
    find_package(NVSHMEM QUIET PATHS "${NVSHMEM_ROOT}" "${CUDAToolkit_LIBRARY_ROOT}/../../comm_libs/nvshmem")
    if (NVSHMEM_FOUND)
        Sigma_enable_feature(NVSHMEM)
    endif ()
endif ()

if (SIGMA_HAS_NVSHMEM)
    message(STATUS "[Sigma] NVSHMEM: found")
else ()
    message(STATUS "[Sigma] NVSHMEM: not found, disabling NVSHMEM support")
endif ()
