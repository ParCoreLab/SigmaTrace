# Protobuf
add_subdirectory(Protobuf)

# FlatBuffers
find_package(Flatbuffers REQUIRED)
add_subdirectory(FlatBuffers)

# Write speed test application
add_executable(Writer EXCLUDE_FROM_ALL writer.cpp "${PROTO_SOURCES}")
target_link_libraries(Writer PRIVATE EventsLib)

target_link_libraries(Writer PUBLIC Simple_obj)
target_link_libraries(Writer PRIVATE flatbuffers::flatbuffers)
add_dependencies(Writer FlatBuffersGenerated)
target_include_directories(Writer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/FlatBuffers")

# Read speed and correctness test application
add_executable(Reader EXCLUDE_FROM_ALL Reader.cpp)
target_link_libraries(Reader PRIVATE EventsLib)