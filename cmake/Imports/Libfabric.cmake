option(SIGMA_ENABLE_LIBFABRIC "Enable libfabric tracing" ON)

set(LIBFABRIC_FOUND FALSE)
set(LIBFABRIC_INCLUDE_DIR "")
set(LIBFABRIC_LIBRARY "")

if (NOT SIGMA_ENABLE_LIBFABRIC)
    message(STATUS "[Sigma] libfabric: disabled by user")
    return()
endif ()

find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
    pkg_check_modules(LIBFABRIC_PKG QUIET libfabric)
endif ()

if (LIBFABRIC_PKG_FOUND)
    set(LIBFABRIC_INCLUDE_DIR ${LIBFABRIC_PKG_INCLUDE_DIRS})

    # pkg_check_modules resolves each -l flag to a full path in LINK_LIBRARIES.
    # Extract the libfabric.so entry directly from there.
    foreach (_lib IN LISTS LIBFABRIC_PKG_LINK_LIBRARIES)
        if (_lib MATCHES "libfabric")
            set(LIBFABRIC_LIBRARY "${_lib}")
            break()
        endif ()
    endforeach ()

    if (LIBFABRIC_LIBRARY)
        set(LIBFABRIC_FOUND TRUE)
    else ()
        message(STATUS "[Sigma] libfabric: pkg-config found but library not located, disabling")
    endif ()
else ()
    # Try finding without pkg-config
    find_path(LIBFABRIC_INCLUDE_DIR NAMES rdma/fabric.h
            HINTS /usr/include /usr/local/include)
    find_library(LIBFABRIC_LIBRARY NAMES fabric
            HINTS /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64)

    if (LIBFABRIC_INCLUDE_DIR AND LIBFABRIC_LIBRARY)
        set(LIBFABRIC_FOUND TRUE)
    endif ()
endif ()

if (LIBFABRIC_FOUND)
    message(STATUS "[Sigma] libfabric: found (${LIBFABRIC_LIBRARY})")
else ()
    message(STATUS "[Sigma] libfabric: not found, disabling libfabric tracing")
endif ()
