option(BUILD_BOOST "Build bundled Boost" ON)
set(BOOST_ROOT "" CACHE PATH "Prefix where system Boost is installed")

set(_boost_components program_options)

# Force find_package to use static libs
set(Boost_USE_STATIC_LIBS ON)

if (NOT BUILD_BOOST)
    if (BOOST_ROOT)
        find_package(Boost REQUIRED PATHS "${BOOST_ROOT}" NO_DEFAULT_PATH COMPONENTS ${_boost_components})
    else ()
        find_package(Boost REQUIRED COMPONENTS ${_boost_components})
    endif ()
    message(STATUS "[Sigma] Boost: found system installation (${Boost_INCLUDE_DIRS})")
    return()
endif ()

# Try to find system Boost first
find_package(Boost QUIET COMPONENTS ${_boost_components})

if (Boost_FOUND)
    message(STATUS "[Sigma] Boost: found system installation (${Boost_INCLUDE_DIRS})")
else ()
    message(STATUS "[Sigma] Boost not found. Building from source...")

    set(BOOST_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Imports/boost")
    set(BOOST_BINARY_DIR "${CMAKE_BINARY_DIR}/boost-build")
    set(BOOST_INSTALL_DIR "${CMAKE_BINARY_DIR}/boost-install")

    # Create directories that must exist at configure time
    file(MAKE_DIRECTORY "${BOOST_BINARY_DIR}")
    file(MAKE_DIRECTORY "${BOOST_INSTALL_DIR}/include")
    file(MAKE_DIRECTORY "${BOOST_INSTALL_DIR}/lib")

    include(ExternalProject)
    ExternalProject_Add(boost_external
            SOURCE_DIR "${BOOST_SOURCE_DIR}"
            BINARY_DIR "${BOOST_BINARY_DIR}"
            INSTALL_DIR "${BOOST_INSTALL_DIR}"
            CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_CXX_STANDARD=${CMAKE_CXX_STANDARD}
            -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
            -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
            -DCMAKE_CXX_FLAGS=-w
            -DCMAKE_C_FLAGS=-w
            -DBOOST_INCLUDE_LIBRARIES=program_options
            -DBUILD_SHARED_LIBS=OFF
            -DBUILD_TESTING=OFF
            BUILD_BYPRODUCTS
            "${BOOST_INSTALL_DIR}/lib/libboost_program_options.a"
            BUILD_ALWAYS OFF
    )

    # Create imported target for Boost::program_options
    add_library(Boost::program_options STATIC IMPORTED GLOBAL)
    set_target_properties(Boost::program_options PROPERTIES
            IMPORTED_LOCATION "${BOOST_INSTALL_DIR}/lib/libboost_program_options.a"
            INTERFACE_INCLUDE_DIRECTORIES "${BOOST_INSTALL_DIR}/include"
    )
    add_dependencies(Boost::program_options boost_external)

    # Create Boost::headers interface target
    add_library(Boost::headers INTERFACE IMPORTED GLOBAL)
    set_target_properties(Boost::headers PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${BOOST_INSTALL_DIR}/include"
    )
    add_dependencies(Boost::headers boost_external)

    # Set Boost_FOUND for downstream
    set(Boost_FOUND TRUE)
    set(Boost_INCLUDE_DIRS "${BOOST_INSTALL_DIR}/include")

    message(STATUS "[Sigma] Boost: building from source (${BOOST_SOURCE_DIR})")
endif ()
