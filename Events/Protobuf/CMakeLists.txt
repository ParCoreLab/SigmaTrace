find_package(Protobuf CONFIG REQUIRED)

set(PROTO_SOURCES "Simple.proto")

function(EventsGenerateProtobuf TARGET SOURCE)
    find_package(Protobuf CONFIG REQUIRED)
    add_library("${TARGET}" OBJECT "${SOURCE}")
    target_link_libraries("${TARGET}" PUBLIC protobuf::libprotobuf)

    # TODO: Required for linking into shared libraries
    set_target_properties("${TARGET}" PROPERTIES POSITION_INDEPENDENT_CODE ON)

    # Add parent binary directory as include path so consumers can use
    # subdirectory-prefixed includes like "Events/Simple.pb.h"
    get_filename_component(PARENT_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}" DIRECTORY)
    target_include_directories("${TARGET}" PUBLIC "${PARENT_BINARY_DIR}")

    protobuf_generate(
            TARGET "${TARGET}"
            LANGUAGE cpp
    )
endfunction()

# For testing
foreach (ProtoSource ${PROTO_SOURCES})
    get_filename_component(ProtoSourceName "${ProtoSource}" NAME_WE)
    set(ProtoSourceObj "${ProtoSourceName}_obj")

    EventsGenerateProtobuf("${ProtoSourceObj}" ${ProtoSource})
endforeach ()

add_library(EventsLibProtobuf INTERFACE ProtobufEventTrait.hpp)
target_link_libraries(EventsLibProtobuf INTERFACE EventsLib)
set_target_properties(EventsLibProtobuf PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}"
        INTERFACE_LINK_LIBRARIES "protobuf::libprotobuf"
)
