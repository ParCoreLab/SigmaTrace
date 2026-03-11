include(Common)

sigma_import_options(PROTOBUF)

# We can't use ExternalProject on Protobuf because it needs
# configure time targets (absl). See cmake/Imports/Abseil.cmake

macro(_sigma_protobuf_build_from_source)
    set(PROTOBUF_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Imports/protobuf")

    # Configure Protobuf to use our Abseil
    set(protobuf_ABSL_PROVIDER "package" CACHE STRING "" FORCE)
    set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "" FORCE)

    # Disable tests and examples
    set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(protobuf_BUILD_PROTOBUF_BINARIES ON CACHE BOOL "" FORCE)
    set(protobuf_BUILD_PROTOC_BINARIES ON CACHE BOOL "" FORCE)
    set(protobuf_BUILD_LIBPROTOC ON CACHE BOOL "" FORCE)
    set(protobuf_BUILD_LIBUPB ON CACHE BOOL "" FORCE)
    set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)

    # Add Protobuf as a subdirectory with SYSTEM to suppress header warnings
    add_subdirectory("${PROTOBUF_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/protobuf-build" EXCLUDE_FROM_ALL SYSTEM)

    # Suppress compile warnings for Protobuf library targets
    foreach (_target libprotobuf libprotobuf-lite libprotoc)
        if (TARGET ${_target})
            target_compile_options(${_target} PRIVATE -w)
        endif ()
    endforeach ()

    # Include the protobuf-generate module to make protobuf_generate() available
    include("${PROTOBUF_SOURCE_DIR}/cmake/protobuf-generate.cmake")

    if (TARGET protobuf::libprotobuf)
        message(STATUS "[Sigma] Protobuf: built from source (using project Abseil)")
    else ()
        message(FATAL_ERROR "[Sigma] Protobuf: failed to create targets")
    endif ()
endmacro()

if (BUILD_PROTOBUF)
    # Prefer building from source; fall back to system if source unavailable
    sigma_source_available("${CMAKE_CURRENT_SOURCE_DIR}/Imports/protobuf" _can_build)
    if (_can_build)
        _sigma_protobuf_build_from_source()
    else ()
        message(WARNING "[Sigma] Protobuf: source not available (Imports/protobuf missing), trying system")
        if (PROTOBUF_ROOT)
            find_package(Protobuf CONFIG QUIET PATHS "${PROTOBUF_ROOT}" NO_DEFAULT_PATH)
        else ()
            find_package(Protobuf CONFIG QUIET)
        endif ()
        if (Protobuf_FOUND)
            message(STATUS "[Sigma] Protobuf: found system installation")
        else ()
            message(FATAL_ERROR "[Sigma] Protobuf: source unavailable and system not found")
        endif ()
    endif ()
else ()
    # Default: try system first, fall back to building from source
    if (PROTOBUF_ROOT)
        find_package(Protobuf CONFIG QUIET PATHS "${PROTOBUF_ROOT}" NO_DEFAULT_PATH)
    else ()
        find_package(Protobuf CONFIG QUIET)
    endif ()
    if (Protobuf_FOUND)
        message(STATUS "[Sigma] Protobuf: found system installation")
    else ()
        sigma_source_available("${CMAKE_CURRENT_SOURCE_DIR}/Imports/protobuf" _can_build)
        if (_can_build)
            _sigma_protobuf_build_from_source()
        else ()
            message(FATAL_ERROR "[Sigma] Protobuf: not found in system and source not available (Imports/protobuf missing)")
        endif ()
    endif ()
endif ()
