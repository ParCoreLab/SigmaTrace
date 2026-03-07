option(SIGMA_ENABLE_UCX "Enable UCX tracing" ON)

set(UCX_FOUND FALSE)
set(UCX_INCLUDE_DIR "")
set(UCX_LIBRARY "")

if (NOT SIGMA_ENABLE_UCX)
    message(STATUS "[Sigma] UCX: disabled by user")
    return()
endif ()

find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
    pkg_check_modules(UCX_PKG QUIET ucx)
endif ()

if (UCX_PKG_FOUND)
    set(UCX_INCLUDE_DIR ${UCX_PKG_INCLUDE_DIRS})

    # pkg_check_modules resolves each -l flag to a full path in LINK_LIBRARIES.
    # Extract the libucp.so entry directly from there.
    foreach (_lib IN LISTS UCX_PKG_LINK_LIBRARIES)
        if (_lib MATCHES "libucp")
            set(UCX_LIBRARY "${_lib}")
            break()
        endif ()
    endforeach ()

    if (UCX_LIBRARY)
        set(UCX_FOUND TRUE)
    else ()
        message(STATUS "[Sigma] UCX: pkg-config found but libucp not located, disabling")
    endif ()
else ()
    # Try finding without pkg-config
    find_path(UCX_INCLUDE_DIR NAMES ucp/api/ucp.h
            HINTS /usr/include /usr/local/include)
    find_library(UCX_LIBRARY NAMES ucp
            HINTS /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64)

    if (UCX_INCLUDE_DIR AND UCX_LIBRARY)
        set(UCX_FOUND TRUE)
    endif ()
endif ()

if (UCX_FOUND)
    message(STATUS "[Sigma] UCX: found (${UCX_LIBRARY})")
else ()
    message(STATUS "[Sigma] UCX: not found, disabling UCX tracing")
endif ()
