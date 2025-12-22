option(BUILD_NCCL "Build bundled NCCL" ON)
set(NCCL_ROOT "" CACHE PATH "Prefix where system NCCL is installed")

if (BUILD_NCCL)
    set(NCCL_DEPENDS_LIST "")
    if (BUILD_RDMA_CORE)
        list(APPEND NCCL_DEPENDS_LIST rdma-core ibverbs verbs mlx5)
    endif ()

    include(ExternalProject)
    ExternalProject_Add(nccl_external
            SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Imports/nccl"
            BINARY_DIR "${CMAKE_BINARY_DIR}/nccl-build"
            CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_CUDA_ARCHITECTURES=${CMAKE_CUDA_ARCHITECTURES}
            -DVERBOSE=ON
            -DDEBUG=ON
            -DTRACE=ON
            -DNVTX=ON
            -DRDMA_CORE=ON
            -DNET_PROFILER=ON
            # Force nccl to use our version
            -DCMAKE_DISABLE_FIND_PACKAGE_verbs=TRUE
            -DCMAKE_C_FLAGS=-I${RDMA_INCLUDE_DIR}
            -DCMAKE_CXX_FLAGS=-I${RDMA_INCLUDE_DIR}
            -DVERBS_LIBRARY=${IBVERBS_LIBRARY_PATH}
            -DMLX5_LIBRARY=${MLX5_LIBRARY_PATH}
            INSTALL_COMMAND ""
            DEPENDS ${NCCL_DEPENDS_LIST}
            BUILD_BYPRODUCTS
            "${CMAKE_BINARY_DIR}/nccl-build/lib/libnccl.so"

            BUILD_ALWAYS ON
    )

    add_library(nccl SHARED IMPORTED)
    target_link_libraries(nccl INTERFACE ibverbs)

    ExternalProject_Get_Property(nccl_external BINARY_DIR)

    # Create directory ahead of time so CMake generic include checks don't fail if they happen early
    file(MAKE_DIRECTORY "${BINARY_DIR}/include")

    set_target_properties(nccl PROPERTIES
            IMPORTED_LOCATION "${BINARY_DIR}/lib/libnccl.so"
            INTERFACE_INCLUDE_DIRECTORIES "${BINARY_DIR}/include"
    )
    # Ensure nccl_external is built before we try to link 'nccl'
    add_dependencies(nccl nccl_external)

    # TODO: unsure if this is proper
    set(CMAKE_DISABLE_FIND_PACKAGE_nccl TRUE)
else ()
    find_path(NCCL_INCLUDE_DIR NAMES nccl.h
            HINTS ${NCCL_ROOT}/include /usr/include /usr/local/include
    )
    find_library(NCCL_LIBRARY NAMES nccl
            HINTS ${NCCL_ROOT}/lib ${NCCL_ROOT}/lib64 /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64
    )
    if (NOT NCCL_INCLUDE_DIR OR NOT NCCL_LIBRARY)
        message(FATAL_ERROR "System NCCL not found; set BUILD_NCCL=ON to build the bundled copy or provide NCCL_ROOT.")
    endif ()

    add_library(nccl SHARED IMPORTED)
    set_target_properties(nccl PROPERTIES
            IMPORTED_LOCATION "${NCCL_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${NCCL_INCLUDE_DIR}"
    )
endif ()
