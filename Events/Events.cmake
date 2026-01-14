#add_subdirectory(Protobuf)
#add_subdirectory(Flatbuffers)

add_library(EventsLib INTERFACE)

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
)

target_include_directories(EventsLib INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

#add_subdirectory(Tests)
