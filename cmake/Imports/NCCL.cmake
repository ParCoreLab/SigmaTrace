option(SIGMA_ENABLE_NCCL "Enable NCCL" ON)
option(BUILD_NCCL "Build bundled NCCL" OFF)
set(NCCL_ROOT "" CACHE PATH "Prefix where system NCCL is installed")

if (NOT SIGMA_ENABLE_NCCL)
    message(STATUS "[Sigma] NCCL: disabled by user")
    return()
endif ()

if (NOT SIGMA_HAS_CUDA)
    message(STATUS "[Sigma] NCCL: disabled (no CUDA backend)")
    return()
endif ()

if (BUILD_NCCL)
    set(NCCL_DEPENDS_LIST "")
    if (BUILD_RDMA_CORE)
        list(APPEND NCCL_DEPENDS_LIST rdma-core ibverbs verbs mlx5)
    endif ()

    # nccl doesn't compile with clang
    if("${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
        set(SIGMA_NCCL_C_COMPILER "gcc")
        set(SIGMA_NCCL_CXX_COMPILER "g++")
    else()
        set(SIGMA_NCCL_C_COMPILER ${CMAKE_C_COMPILER})
        set(SIGMA_NCCL_CXX_COMPILER ${CMAKE_CXX_COMPILER})
    endif()

    include(ExternalProject)
    ExternalProject_Add(nccl_external
            SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Imports/nccl"
            BINARY_DIR "${CMAKE_BINARY_DIR}/nccl-build"
            CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=RelWithDebInfo
            -DCMAKE_C_COMPILER=${SIGMA_NCCL_C_COMPILER}
            -DCMAKE_CXX_COMPILER=${SIGMA_NCCL_CXX_COMPILER}
            -DCUDA_HOME=${CUDAToolkit_LIBRARY_ROOT}
            -DCMAKE_CUDA_ARCHITECTURES=${CMAKE_CUDA_ARCHITECTURES}
            -DVERBOSE=ON
            -DDEBUG=ON
            -DTRACE=ON
            -DNVTX=ON
            -DRDMA_CORE=ON
            -DNET_PROFILER=ON
            -DCMAKE_DISABLE_FIND_PACKAGE_verbs=TRUE
            -DCMAKE_C_FLAGS=-I${RDMA_INCLUDE_DIR}
            -DCMAKE_CXX_FLAGS=-I${RDMA_INCLUDE_DIR}
            -DVERBS_LIBRARY=${IBVERBS_LIBRARY_PATH}
            -DMLX5_LIBRARY=${MLX5_LIBRARY_PATH}
            INSTALL_COMMAND ""
            DEPENDS ${NCCL_DEPENDS_LIST}
            BUILD_BYPRODUCTS
            "${CMAKE_BINARY_DIR}/nccl-build/lib/libnccl.so"

            BUILD_ALWAYS OFF
    )

    add_library(nccl SHARED IMPORTED)
    target_link_libraries(nccl INTERFACE ibverbs)

    ExternalProject_Get_Property(nccl_external BINARY_DIR)

    file(MAKE_DIRECTORY "${BINARY_DIR}/include")

    set_target_properties(nccl PROPERTIES
            IMPORTED_LOCATION "${BINARY_DIR}/lib/libnccl.so"
            INTERFACE_INCLUDE_DIRECTORIES "${BINARY_DIR}/include"
    )
    add_dependencies(nccl nccl_external)

    set(CMAKE_DISABLE_FIND_PACKAGE_nccl TRUE)
    Sigma_enable_feature(NCCL)
else ()
    find_path(NCCL_INCLUDE_DIR NAMES nccl.h
            HINTS ${NCCL_ROOT}/include /usr/include /usr/local/include
    )
    find_library(NCCL_LIBRARY NAMES nccl
            HINTS ${NCCL_ROOT}/lib ${NCCL_ROOT}/lib64 /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64
    )
    if (NCCL_INCLUDE_DIR AND NCCL_LIBRARY)
        add_library(nccl SHARED IMPORTED)
        set_target_properties(nccl PROPERTIES
                IMPORTED_LOCATION "${NCCL_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${NCCL_INCLUDE_DIR}"
        )
        Sigma_enable_feature(NCCL)
    endif ()
endif ()

if (SIGMA_HAS_NCCL)
    message(STATUS "[Sigma] NCCL: found")
else ()
    message(STATUS "[Sigma] NCCL: not found, disabling NCCL support")
endif ()
