Sigma_add_library(SmartDlsym STATIC
        SymbolResolver.h
        SymbolResolver.cpp
        Dlsym.hpp
        Dlsym.cpp
)

# TODO: may be unnecessary
set_target_properties(SmartDlsym PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_NAME "smart_dlsym"
)

set_property(TARGET SmartDlsym PROPERTY ENABLE_EXPORTS ON)

target_link_libraries(SmartDlsym PRIVATE dl)

set_target_properties(SmartDlsym PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
