# Maybe we don't need a static lib
add_library(PerfettoLogger STATIC
        PerfettoLogger.cpp
        PerfettoLogger.hpp
)
set_target_properties(PerfettoLogger PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(PerfettoLogger PUBLIC perfetto)
