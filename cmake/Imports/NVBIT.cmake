set(NVBIT_ARCH ${CMAKE_SYSTEM_PROCESSOR})

# NVBIT releases are either "x86_64" or "aarch64"
if (NVBIT_ARCH MATCHES "x86_64|amd64|AMD64")
    set(NVBIT_ARCH "x86_64")
elseif (NVBIT_ARCH MATCHES "aarch64|arm64|ARM64")
    set(NVBIT_ARCH "aarch64")
else ()
    message(FATAL_ERROR "Unsupported architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif ()

set(NVBIT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Imports/nvbit_release_${NVBIT_ARCH}" CACHE PATH "Prefix where system NVBIT is installed")

set(NVBIT_VERSION "1.7.6")
set(NVBIT_URL "https://github.com/NVlabs/NVBit/releases/download/v${NVBIT_VERSION}/nvbit-Linux-${NVBIT_ARCH}-${NVBIT_VERSION}.tar.bz2")
set(NVBIT_ARCHIVE "${CMAKE_CURRENT_SOURCE_DIR}/Imports/nvbit-${NVBIT_VERSION}.tar.bz2")

# Download the archive if neither the root nor the archive exist
if (NOT EXISTS ${NVBIT_ROOT} AND NOT EXISTS ${NVBIT_ARCHIVE})
    message(STATUS "Downloading NVBit ${NVBIT_VERSION}...")
    file(DOWNLOAD ${NVBIT_URL} ${NVBIT_ARCHIVE}
            SHOW_PROGRESS
            STATUS download_status
            TLS_VERIFY ON)

    list(GET download_status 0 status_code)
    list(GET download_status 1 status_message)

    if (NOT status_code EQUAL 0)
        message(FATAL_ERROR "Failed to download NVBit: ${status_message}")
    endif ()
endif ()

# Extract the archive
if (NOT EXISTS ${NVBIT_ROOT})
    message(STATUS "Extracting NVBit to ${NVBIT_ROOT}...")
    file(MAKE_DIRECTORY ${NVBIT_ROOT})

    file(ARCHIVE_EXTRACT INPUT ${NVBIT_ARCHIVE} DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/Imports/")
endif ()

message(STATUS "Using NVBIT from ${NVBIT_ROOT}")

add_library(nvbit STATIC IMPORTED)
set_target_properties(nvbit PROPERTIES
        IMPORTED_LOCATION "${NVBIT_ROOT}/core/libnvbit.a"
        INTERFACE_INCLUDE_DIRECTORIES "${NVBIT_ROOT}/core"
)

target_include_directories(nvbit INTERFACE "${NVBIT_ROOT}/core")
