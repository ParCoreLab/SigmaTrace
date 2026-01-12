find_package(Flatbuffers REQUIRED)

set(FLATBUFFERS_SOURCES
        Simple.fbs
)

build_flatbuffers(
        "${FLATBUFFERS_SOURCES}"
        ""
        FlatBuffersGenerated
        ""
        "${CMAKE_CURRENT_BINARY_DIR}"
        ""
        "")
