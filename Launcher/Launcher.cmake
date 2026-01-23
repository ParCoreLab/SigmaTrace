Sigma_add_library(Launcher STATIC Launcher.cpp Launcher.hpp)
target_link_libraries(Launcher PUBLIC SigmaHelpers)
target_link_libraries(Launcher PRIVATE CUDA::cudart)

target_include_directories(Launcher INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# >:(
#target_compile_features(Launcher PUBLIC cxx_std_23)
