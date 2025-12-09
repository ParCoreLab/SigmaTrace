option(BUILD_NCCL "Build bundled NCCL" ON)
set(NCCL_ROOT "" CACHE PATH "Prefix where system NCCL is installed")

if (BUILD_NCCL)
    set(NCCL_DEPENDS_LIST "")
    if (BUILD_RDMA_CORE)
        list(APPEND NCCL_DEPENDS_LIST rdma-core)
    endif ()

    include(ExternalProject)
    ExternalProject_Add(nccl_external
            SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/nccl"
            BINARY_DIR "${CMAKE_BINARY_DIR}/nccl-build"
            CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            -DVERBOSE=ON
            -DDEBUG=ON
            -DTRACE=ON
            -DNVTX=ON
            -DRDMA_CORE=ON
            -DNET_PROFILER=ON
            -DMLX5DV=ON
            -DVERBS_LIBRARY=${IBVERBS_LIBRARY_PATH}
            -DMLX5_LIBRARY=${MLX5_LIBRARY_PATH}
            INSTALL_COMMAND ""
            DEPENDS ${NCCL_DEPENDS_LIST}
            BUILD_BYPRODUCTS
            "${CMAKE_BINARY_DIR}/nccl-build/lib/libnccl.so"
    )

    add_library(nccl SHARED IMPORTED)
    ExternalProject_Get_Property(nccl_external BINARY_DIR)
    set_target_properties(nccl PROPERTIES
            IMPORTED_LOCATION "${BINARY_DIR}/lib/libnccl.so"
            INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/nccl/src/include"
    )
    # Ensure nccl_external is built before we try to link 'nccl'
    add_dependencies(nccl nccl_external)
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
