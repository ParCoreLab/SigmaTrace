Sigma_add_library(ProfilerExportTableOverwrite STATIC ExportTablePreload.cpp ExportTableOverwrite.hpp)
set_target_properties(ProfilerExportTableOverwrite PROPERTIES
        ENABLE_EXPORTS ON
        POSITION_INDEPENDENT_CODE ON
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")

target_include_directories(ProfilerExportTableOverwrite PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

Sigma_add_headers(ProfilerExportTableOverwrite SmartDlsym)

Sigma_add_headers(ProfilerExportTableOverwrite CUDA::cudart SYSTEM)
#target_link_libraries(ProfilerExportTableOverwrite PRIVATE CUDA::cuda_driver)
