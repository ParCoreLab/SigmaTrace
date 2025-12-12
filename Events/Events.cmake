add_library(Events INTERFACE)

target_sources(Events INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/Events.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/EventTrait.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/EventStorage.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ThreadLocalBuffer.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ThreadLocalRingBuffer.hpp
        ${CMAKE_CURRENT_SOURCE_DIR}/EventRecorder.hpp
)

target_include_directories(Events INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Require C++20 for concepts
target_compile_features(Events INTERFACE cxx_std_20)
