cmake_minimum_required(VERSION 4.0)

# Global CXX and CUDA defaults
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 20)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

if (NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES "native")
endif ()

project(Thing LANGUAGES C CXX CUDA)

# CUDA Toolkit
find_package(CUDAToolkit REQUIRED)

# This is so we don't have to LD_LIBRARY_PATH the cudart library location
# May break ld for other dependencies
function(set_cuda_rpath target)
    set_target_properties(${target} PROPERTIES LINK_FLAGS "-Wl,-rpath,${CUDAToolkit_LIBRARY_DIR}")
endfunction()

# Dependencies
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(RDMA_CORE)
include(NCCL)
include(NVSHMEM)

# Helper Library
add_library(cuda_helper INTERFACE)
target_include_directories(cuda_helper INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/common")

# "Main" Test Executable
add_executable(Thing main.cu)
target_link_libraries(Thing PRIVATE cuda_helper)
#target_link_libraries(Thing PRIVATE CUDA::cudart)

# Link Dependencies defined in modules
target_link_libraries(Thing PRIVATE ibverbs)
target_link_libraries(Thing PRIVATE nccl)

add_executable(ThingNVSHMEM main_nvshmem.cu)
set_cuda_rpath(ThingNVSHMEM)

set_target_properties(ThingNVSHMEM PROPERTIES CUDA_SEPARABLE_COMPILATION ON)

target_compile_options(ThingNVSHMEM PRIVATE $<$<COMPILE_LANGUAGE:CUDA>:
        --generate-line-info
        -g
        >)

target_link_libraries(ThingNVSHMEM PRIVATE cuda_helper)
target_link_libraries(ThingNVSHMEM PRIVATE CUDA::cudart)
target_link_libraries(ThingNVSHMEM PRIVATE CUDA::nvml)
target_link_libraries(ThingNVSHMEM PRIVATE nvshmem::nvshmem_host)
target_link_libraries(ThingNVSHMEM PRIVATE nvshmem::nvshmem_device)

# Events library
add_subdirectory(Events)

# Trace library
add_subdirectory(Ibtrace)

# Sample apps
add_subdirectory(Apps)

# Ensure Thing builds *after* the trace libraries are ready, if it depends on them or if they need to exist.
#add_dependencies(Thing IbtraceTest)
#add_dependencies(Thing ProfilerTrace)
