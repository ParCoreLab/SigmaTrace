cmake_minimum_required(VERSION 4.0)

# CXX standard, CUDA version, compiler flags
include(cmake/CompilerConfig.cmake)

project(Thing LANGUAGES C CXX CUDA)

# CUDA Toolkit
find_package(CUDAToolkit REQUIRED)

# This is so we don't have to LD_LIBRARY_PATH the cudart library location
# May break ld for other dependencies
function(set_cuda_rpath target)
    set_target_properties(${target} PROPERTIES LINK_FLAGS "-Wl,-rpath,${CUDAToolkit_LIBRARY_DIR}")
endfunction()

# Dependencies
# TODO: don't pollute CMAKE_MODULE_PATH
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Imports")
include(Abseil)
include(RDMA_CORE)
include(NCCL)
include(NVSHMEM)
include(NVBIT)
include(Perfetto)

# Modules
add_subdirectory(Modules)

# "Main" Test Executable
# Excluded because ncclMemAlloc is broken on some systems
Sigma_add_executable(Thing EXCLUDE_FROM_ALL main.cu)
target_link_libraries(Thing PRIVATE SigmaHelpers)
#target_link_libraries(Thing PRIVATE CUDA::cudart)

# Link Dependencies defined in modules
target_link_libraries(Thing PRIVATE ibverbs)
target_link_libraries(Thing PRIVATE nccl)

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

# Dlsym shims
add_subdirectory(SmartDlsym)

# Events library
add_subdirectory(Events)

# Launcher library
add_subdirectory(Launcher)

# Events library
add_subdirectory(Hook)

# IB Trace library
add_subdirectory(Ibtrace)

# mlx5dv Trace library
add_subdirectory(Mlx5dvTrace)

# CUDA Trace libraries
add_subdirectory(CUDATrace)

# Unified Profiler
add_subdirectory(Profiler)

# Sample apps
add_subdirectory(Apps)
