add_library(Ibtrace SHARED
        ibverbs_trace.hpp
        ibverbs_preload.cpp
)

target_include_directories(Ibtrace PRIVATE "${rdma-core_BINARY_DIR}/include")

set_target_properties(Ibtrace PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        OUTPUT_NAME "ibverbs_trace"
)

# Link against dl for dlsym()
target_link_libraries(Ibtrace PRIVATE dl)
target_link_libraries(Ibtrace PRIVATE ibverbs)

set_target_properties(Ibtrace PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

add_executable(test_ibtrace test_ibtrace.cpp)
target_link_libraries(test_ibtrace PRIVATE Ibtrace ibverbs)
