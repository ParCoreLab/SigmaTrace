cmake_minimum_required(VERSION 4.0)

# Global CXX and CUDA defaults
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 20)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_ARCHITECTURES native)

project(Thing LANGUAGES C CXX CUDA)

# CUDA Toolkit
find_package(CUDAToolkit REQUIRED)

# Dependencies
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(RDMA_CORE)
include(NCCL)

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

# Events library
add_subdirectory(Events)

# Trace library
add_subdirectory(Ibtrace)

# Ensure Thing builds *after* the trace libraries are ready, if it depends on them or if they need to exist.
#add_dependencies(Thing IbtraceTest)
#add_dependencies(Thing ProfilerTrace)
