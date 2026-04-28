cmake_minimum_required(VERSION 3.31)

# For whatever reason project() sets CMAKE_CUDA_ARCHITECTURES to 75
if (NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES "native")
endif ()

project(Thing LANGUAGES C CXX)

# Feature flag system must be loaded before other config
include(cmake/Features.cmake)

# CXX standard, GPU backend, compiler flags
include(cmake/CompilerConfig.cmake)

# GPU Toolkit
if (SIGMA_HAS_CUDA)
    find_package(CUDAToolkit REQUIRED)

    # This is so we don't have to LD_LIBRARY_PATH the cudart library location
    # May break ld for other dependencies
    function(set_cuda_rpath target)
        set_target_properties(${target} PROPERTIES LINK_FLAGS "-Wl,-rpath,${CUDAToolkit_LIBRARY_DIR}")
    endfunction()
endif ()

if (SIGMA_HAS_HIP)
    find_package(hip REQUIRED)
endif ()

# Dependencies
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Imports")
include(Abseil)
include(Boost)
include(RDMA_CORE)
include(NCCL)
include(NVSHMEM)
include(NVBIT)
include(Protobuf)
include(Perfetto)
include(UCX)
include(Libfabric)
include(Liburing)

# ================================================================
# Load feature flags that are enabled by this point
Sigma_load_feature_flags()
# ================================================================

# Modules
add_subdirectory(Modules)

# Dlsym shims
add_subdirectory(SmartDlsym)

# Events library
add_subdirectory(Events)

# Launcher library
add_subdirectory(Launcher)

# Events library
add_subdirectory(Hook)

# IB Trace library
if (SIGMA_HAS_RDMA_CORE)
    add_subdirectory(Ibtrace)
    add_subdirectory(Mlx5dvTrace)
endif ()

# UCX Trace library
if (SIGMA_HAS_UCX)
    add_subdirectory(UcxTrace)
endif ()

# libfabric Trace library
if (SIGMA_HAS_LIBFABRIC)
    add_subdirectory(LibfabricTrace)
endif ()

# CUDA Trace libraries
if (SIGMA_HAS_CUDA)
    add_subdirectory(CUDATrace)
endif ()

# Unified Profiler
add_subdirectory(Profiler)

# Sample apps
add_subdirectory(Apps)

# ================================================================
# Configuration Summary
# ================================================================
message(STATUS "")
message(STATUS "===== SigmaTrace Configuration =====")
message(STATUS "  GPU backend:  ${SIGMA_GPU_BACKEND}")
message(STATUS "  RDMA-core:    ${SIGMA_HAS_RDMA_CORE}")
message(STATUS "  NCCL:         ${SIGMA_HAS_NCCL}")
message(STATUS "  NVSHMEM:      ${SIGMA_HAS_NVSHMEM}")
message(STATUS "  NVBit:        ${SIGMA_HAS_NVBIT}")
message(STATUS "  UCX:          ${SIGMA_HAS_UCX}")
message(STATUS "  libfabric:    ${SIGMA_HAS_LIBFABRIC}")
message(STATUS "  liburing:     ${SIGMA_HAS_LIBURING}")
message(STATUS "====================================")
message(STATUS "")
