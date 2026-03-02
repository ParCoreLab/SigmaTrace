option(BUILD_UCX_TRACE "Build UCX tracing" ON)

set(UCX_FOUND FALSE)
set(UCX_INCLUDE_DIR "")
set(UCX_LIBRARY "")

if (BUILD_UCX_TRACE)
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
            message(STATUS "Found UCX: ${UCX_LIBRARY}")
            message(STATUS "  Include: ${UCX_INCLUDE_DIR}")
        else ()
            message(STATUS "UCX pkg-config found but libucp not located, disabling UCX tracing")
        endif ()
    else ()
        # Try finding without pkg-config
        find_path(UCX_INCLUDE_DIR NAMES ucp/api/ucp.h
                HINTS /usr/include /usr/local/include)
        find_library(UCX_LIBRARY NAMES ucp
                HINTS /usr/lib /usr/lib64 /usr/local/lib /usr/local/lib64)

        if (UCX_INCLUDE_DIR AND UCX_LIBRARY)
            set(UCX_FOUND TRUE)
            message(STATUS "Found UCX: ${UCX_LIBRARY}")
            message(STATUS "  Include: ${UCX_INCLUDE_DIR}")
        else ()
            message(STATUS "UCX not found, disabling UCX tracing")
        endif ()
    endif ()
else ()
    message(STATUS "UCX tracing disabled")
endif ()
