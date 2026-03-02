set(SmartDlsymSources SymbolResolver.h
        SymbolResolver.cpp
        Dlsym.hpp
        Dlsym.cpp)

Sigma_add_library(SmartDlsym STATIC ${SmartDlsymSources})
Sigma_add_library(SmartDlsymShared SHARED ${SmartDlsymSources})

foreach (SmartdlsymTarget IN ITEMS SmartDlsym SmartDlsymShared)
    set_target_properties(${SmartdlsymTarget} PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_NAME "smart_dlsym"
    )

    # TODO: unsure if this is really needed
    set_property(TARGET ${SmartdlsymTarget} PROPERTY ENABLE_EXPORTS ON)

    target_link_libraries(${SmartdlsymTarget} PRIVATE dl)
endforeach ()
