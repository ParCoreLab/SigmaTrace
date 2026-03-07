cmake_minimum_required(VERSION 4.0)

project(Thing LANGUAGES C CXX)

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

# Dependencies
# TODO: don't pollute CMAKE_MODULE_PATH
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Imports")
include(Abseil)
include(RDMA_CORE)
include(NCCL)
include(NVSHMEM)
include(NVBIT)
include(Perfetto)
include(UCX)
include(Libfabric)

# ================================================================
# Feature flags interface target
# All Sigma targets automatically link this via Sigma_apply_compile_options
add_library(sigma_feature_flags INTERFACE)
if (SIGMA_HAS_CUDA)
    target_compile_definitions(sigma_feature_flags INTERFACE SIGMA_HAS_CUDA)
endif ()
if (SIGMA_HAS_HIP)
    target_compile_definitions(sigma_feature_flags INTERFACE SIGMA_HAS_HIP)
endif ()
if (RDMA_CORE_FOUND)
    target_compile_definitions(sigma_feature_flags INTERFACE SIGMA_HAS_RDMA_CORE)
endif ()
if (NCCL_FOUND)
    target_compile_definitions(sigma_feature_flags INTERFACE SIGMA_HAS_NCCL)
endif ()
if (NVSHMEM_FOUND)
    target_compile_definitions(sigma_feature_flags INTERFACE SIGMA_HAS_NVSHMEM)
endif ()
if (NVBIT_FOUND)
    target_compile_definitions(sigma_feature_flags INTERFACE SIGMA_HAS_NVBIT)
endif ()
if (UCX_FOUND)
    target_compile_definitions(sigma_feature_flags INTERFACE SIGMA_HAS_UCX)
endif ()
if (LIBFABRIC_FOUND)
    target_compile_definitions(sigma_feature_flags INTERFACE SIGMA_HAS_LIBFABRIC)
endif ()
# ================================================================

# Modules
add_subdirectory(Modules)

# "Main" Test Executable
# Excluded because ncclMemAlloc is broken on some systems
# TODO: move this out of here
if (SIGMA_HAS_CUDA AND RDMA_CORE_FOUND AND NCCL_FOUND)
    Sigma_add_executable(Thing EXCLUDE_FROM_ALL main.cu)
    target_link_libraries(Thing PRIVATE SigmaHelpers)
    #target_link_libraries(Thing PRIVATE CUDA::cudart)

    # Link Dependencies defined in modules
    target_link_libraries(Thing PRIVATE ibverbs)
    target_link_libraries(Thing PRIVATE nccl)
endif ()

if (SIGMA_HAS_CUDA AND NVSHMEM_FOUND)
    Sigma_add_executable(ThingNVSHMEM main_nvshmem.cu)
    set_cuda_rpath(ThingNVSHMEM)

    # Preventing some linker issues with clang
    set_target_properties(ThingNVSHMEM PROPERTIES CUDA_SEPARABLE_COMPILATION ON)
    target_link_options(ThingNVSHMEM PRIVATE -no-pie)

    target_compile_options(ThingNVSHMEM PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:
            --generate-line-info
            -g
            >)

    target_link_libraries(ThingNVSHMEM PRIVATE SigmaHelpers)
    target_link_libraries(ThingNVSHMEM PRIVATE CUDA::cudart)
    target_link_libraries(ThingNVSHMEM PRIVATE CUDA::nvml)
    target_link_libraries(ThingNVSHMEM PRIVATE nvshmem::nvshmem_host)
    target_link_libraries(ThingNVSHMEM PRIVATE nvshmem::nvshmem_device)
endif ()

# Dlsym shims
add_subdirectory(SmartDlsym)

# Events library
add_subdirectory(Events)

# Launcher library
add_subdirectory(Launcher)

# Events library
add_subdirectory(Hook)

# IB Trace library
if (RDMA_CORE_FOUND)
    add_subdirectory(Ibtrace)
    add_subdirectory(Mlx5dvTrace)
endif ()

# UCX Trace library
if (UCX_FOUND)
    add_subdirectory(UcxTrace)
endif ()

# libfabric Trace library
if (LIBFABRIC_FOUND)
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
message(STATUS "  RDMA-core:    ${RDMA_CORE_FOUND}")
message(STATUS "  NCCL:         ${NCCL_FOUND}")
message(STATUS "  NVSHMEM:      ${NVSHMEM_FOUND}")
message(STATUS "  NVBit:        ${NVBIT_FOUND}")
message(STATUS "  UCX:          ${UCX_FOUND}")
message(STATUS "  libfabric:    ${LIBFABRIC_FOUND}")
message(STATUS "====================================")
message(STATUS "")
