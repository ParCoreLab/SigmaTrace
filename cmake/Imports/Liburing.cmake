option(SIGMA_ENABLE_LIBURING "Enable Liburing FIle IO" ON)

if (NOT SIGMA_ENABLE_LIBURING)
    message(STATUS "[Sigma] Liburing: disabled by user")
    return()
endif ()

find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
    pkg_check_modules(LIBURING QUIET IMPORTED_TARGET liburing)
endif ()

if (NOT LIBURING_FOUND)
    message(STATUS "[Sigma] Liburing: not found. Disabling")
    return()
endif ()

set(LIBURING_LIB_PATH "${LIBURING_LIBRARY_DIRS}/liburing.a")

Sigma_enable_feature(LIBURING)
message(STATUS "[Sigma] Liburing: found (${LIBURING_LIB_PATH})")

add_library(liburing STATIC IMPORTED GLOBAL)
set_target_properties(liburing PROPERTIES
        IMPORTED_LOCATION "${LIBURING_LIB_PATH}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBURING_INCLUDE_DIRS}"
)


