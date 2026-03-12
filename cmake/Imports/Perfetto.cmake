include(Common)

sigma_import_options(PERFETTO WITH_ENABLE)

if (NOT SIGMA_ENABLE_PERFETTO)
    message(STATUS "[Sigma] Perfetto: disabled by user")
    return()
endif ()

set(PERFETTO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Imports/perfetto")

# Directory where proto files are located
set(PERFETTO_PROTOS_DIR "${PERFETTO_ROOT}/protos")

# The merged proto file contains all Perfetto trace definitions in one file
set(PERFETTO_TRACE_PROTO "${PERFETTO_PROTOS_DIR}/perfetto/trace/perfetto_trace.proto")

if (NOT EXISTS "${PERFETTO_TRACE_PROTO}")
    message(WARNING "${PERFETTO_TRACE_PROTO} does not exist. Make sure to pull the repo or set PERFETTO_ROOT")
    return()
else()
    set(SIGMA_HAS_PERFETTO ON)
endif ()

# Create the library target
add_library(PerfettoProto STATIC)

target_include_directories(PerfettoProto SYSTEM PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(PerfettoProto PUBLIC protobuf::libprotobuf)

# Generate C++ sources from proto files
protobuf_generate(
        TARGET PerfettoProto
        PROTOS ${PERFETTO_TRACE_PROTO}
        IMPORT_DIRS ${PERFETTO_PROTOS_DIR}
)

# Suppress warnings in generated code
target_compile_options(PerfettoProto PRIVATE
        -Wno-deprecated-declarations
        -Wno-unused-parameter
        -Wno-array-bounds
)

set_target_properties(PerfettoProto PROPERTIES
        POSITION_INDEPENDENT_CODE ON
)
