set(PERFETTO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Imports/perfetto" CACHE PATH "Path where system Perfetto is installed")

set(PERFETTO_SDK_ROOT "${PERFETTO_ROOT}/sdk")

add_library(perfetto STATIC "${PERFETTO_SDK_ROOT}/perfetto.cc")
set_target_properties(perfetto PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        INTERFACE_INCLUDE_DIRECTORIES "${PERFETTO_SDK_ROOT}")
