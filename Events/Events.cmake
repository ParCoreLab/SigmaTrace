#add_subdirectory(Protobuf)
#add_subdirectory(Flatbuffers)

Sigma_add_library(EventsLib INTERFACE)

target_sources(EventsLib INTERFACE
        Events.hpp
        EventTrait.hpp
        EventStorage.hpp
        EventRecorder.hpp
        EventReader.hpp
        ThreadLocalBuffer.hpp
        ThreadLocalRingBuffer.hpp
        FileIO/File.hpp
        FileIO/FileUring.hpp
        FileIO/SerializableTrait.hpp
)

target_include_directories(EventsLib INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(EventsLib INTERFACE SigmaHelpers)

#add_subdirectory(Tests)
