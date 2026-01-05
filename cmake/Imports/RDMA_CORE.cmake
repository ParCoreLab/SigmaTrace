option(BUILD_RDMA_CORE "Build bundled rdma-core" ON)
set(RDMA_ROOT "" CACHE PATH "Prefix where system rdma-core is installed")

set(RDMA_INCLUDE_DIR "")
set(IBVERBS_LIBRARY_PATH "")
set(MLX5_LIBRARY_PATH "")

if (BUILD_RDMA_CORE)
    include(ExternalProject)
    ExternalProject_Add(rdma-core
            SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Imports/rdma-core"
            BINARY_DIR "${CMAKE_BINARY_DIR}/rdma-core"
            CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
#            -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
#            -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DIN_PLACE=1
            # TODO: this breaks on some systems but might be important
            -DENABLE_RESOLVE_NEIGH=0
            # Disabled for faster build
            -DNO_MAN_PAGES=1
            # TODO: this will probably be useful in the future
            -DNO_PYVERBS=1
            INSTALL_COMMAND ""
            BUILD_BYPRODUCTS
            "${CMAKE_BINARY_DIR}/rdma-core/lib/libibverbs.so"
            "${CMAKE_BINARY_DIR}/rdma-core/lib/libmlx5.so"
            "${CMAKE_BINARY_DIR}/rdma-core/lib/librdmacm.so"

            BUILD_ALWAYS OFF
    )

    ExternalProject_Get_Property(rdma-core BINARY_DIR)
    set(RDMA_BINARY_DIR "${BINARY_DIR}")
    set(RDMA_INCLUDE_DIR "${RDMA_BINARY_DIR}/include")
    set(IBVERBS_LIBRARY_PATH "${RDMA_BINARY_DIR}/lib/libibverbs.so")
    set(MLX5_LIBRARY_PATH "${RDMA_BINARY_DIR}/lib/libmlx5.so")

    # Create directory ahead of time so CMake generic include checks don't fail if they happen early
    file(MAKE_DIRECTORY "${RDMA_INCLUDE_DIR}")

    # Custom target to force build order if needed by non-imported targets
    add_custom_target(rdma-core-build
            COMMAND ${CMAKE_COMMAND} --build "${RDMA_BINARY_DIR}"
            DEPENDS rdma-core
    )

    # TODO: unsure if this is proper
    set(CMAKE_DISABLE_FIND_PACKAGE_ibverbs TRUE)
    set(CMAKE_DISABLE_FIND_PACKAGE_mlx5 TRUE)
else ()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(RDMA REQUIRED libibverbs libmlx5)

    set(RDMA_INCLUDE_DIR ${RDMA_INCLUDE_DIRS})
    set(IBVERBS_LIBRARY_PATH ${RDMA_LIBRARIES})

    # Try finding full paths for ExternalProject usage (NCCL)
    find_library(IBVERBS_LIB_FULL_PATH NAMES ibverbs HINTS ${RDMA_LIBRARY_DIRS})
    find_library(MLX5_LIB_FULL_PATH NAMES mlx5 HINTS ${RDMA_LIBRARY_DIRS})

    if (NOT IBVERBS_LIB_FULL_PATH)
        find_library(IBVERBS_LIB_FULL_PATH NAMES ibverbs)
    endif ()
    if (NOT MLX5_LIB_FULL_PATH)
        find_library(MLX5_LIB_FULL_PATH NAMES mlx5)
    endif ()

    set(IBVERBS_LIBRARY_PATH "${IBVERBS_LIB_FULL_PATH}")
    set(MLX5_LIBRARY_PATH "${MLX5_LIB_FULL_PATH}")

    if (NOT IBVERBS_LIBRARY_PATH OR NOT MLX5_LIBRARY_PATH)
        message(FATAL_ERROR "System rdma-core (libibverbs/libmlx5) not found.")
    endif ()
endif ()

# Imported Targets for RDMA
add_library(ibverbs SHARED IMPORTED GLOBAL)
set_target_properties(ibverbs PROPERTIES
        IMPORTED_LOCATION "${IBVERBS_LIBRARY_PATH}"
        INTERFACE_INCLUDE_DIRECTORIES "${RDMA_INCLUDE_DIR}"
)

# NCCL looks for verbs
add_library(verbs SHARED IMPORTED GLOBAL)
set_target_properties(verbs PROPERTIES
        IMPORTED_LOCATION "${IBVERBS_LIBRARY_PATH}"
        INTERFACE_INCLUDE_DIRECTORIES "${RDMA_INCLUDE_DIR}"
)

add_library(mlx5 SHARED IMPORTED GLOBAL)
set_target_properties(mlx5 PROPERTIES
        IMPORTED_LOCATION "${MLX5_LIBRARY_PATH}"
        INTERFACE_INCLUDE_DIRECTORIES "${RDMA_INCLUDE_DIR}"
)

if (BUILD_RDMA_CORE)
    add_dependencies(ibverbs rdma-core)
    add_dependencies(verbs rdma-core)
    add_dependencies(mlx5 rdma-core)
endif ()
