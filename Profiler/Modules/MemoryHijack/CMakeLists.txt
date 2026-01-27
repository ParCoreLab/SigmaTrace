Sigma_add_library(ProfilerMemoryRepeater STATIC MemoryRepeater.cpp MemoryRepeater.hpp)
target_include_directories(ProfilerMemoryRepeater PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
