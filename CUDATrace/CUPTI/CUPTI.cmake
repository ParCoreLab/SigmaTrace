find_package(CUDAToolkit REQUIRED)

#Sigma_add_library(CuptiEventProvider SHARED CUPTITrace.cpp)
Sigma_add_provider_library(CuptiEventProvider SHARED CUPTITrace.cpp)

target_link_libraries(CuptiEventProvider PRIVATE CUDA::cupti)
target_link_libraries(CuptiEventProvider PUBLIC CUDATrace)

set_target_properties(CuptiEventProvider PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_NAME "cupti_trace"
)
