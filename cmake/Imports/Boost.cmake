option(BUILD_BOOST "Build bundled Boost" OFF)
set(BOOST_ROOT "" CACHE PATH "Prefix where system Boost is installed")

# Minimum required Boost version
set(BOOST_MIN_VERSION "1.65.0")

# Required Boost components
set(BOOST_REQUIRED_COMPONENTS program_options)

if (BUILD_BOOST)
    # For bundled Boost, use add_subdirectory if Boost source is available
    # This assumes Boost is in Imports/boost
    set(BOOST_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Imports/boost")
    
    if (EXISTS "${BOOST_INCLUDE_DIR}")
        # If using bundled Boost as header-only or with built libraries
        # Note: Boost program_options needs to be built as it's not header-only
        message(STATUS "Using bundled Boost from ${BOOST_INCLUDE_DIR}")
        
        # Create imported targets manually for bundled Boost
        # This would require building Boost libraries separately
        message(FATAL_ERROR "Bundled Boost build not yet implemented. Please use system Boost with -DBUILD_BOOST=OFF")
    else ()
        message(FATAL_ERROR "Bundled Boost source not found at ${BOOST_INCLUDE_DIR}")
    endif ()
else ()
    # Use system Boost
    if (BOOST_ROOT)
        set(Boost_NO_SYSTEM_PATHS ON)
        set(BOOST_ROOT "${BOOST_ROOT}")
    endif ()
    
    # Find Boost with required components
    find_package(Boost ${BOOST_MIN_VERSION} REQUIRED COMPONENTS ${BOOST_REQUIRED_COMPONENTS})
    
    if (Boost_FOUND)
        message(STATUS "Found Boost ${Boost_VERSION} at ${Boost_INCLUDE_DIRS}")
        message(STATUS "Boost libraries: ${Boost_LIBRARIES}")
    else ()
        message(FATAL_ERROR "System Boost not found. Install Boost or use -DBUILD_BOOST=ON")
    endif ()
endif ()

# Create interface targets for easy linking
# Boost::program_options is created automatically by find_package(Boost)
# For future-proofing, we ensure the targets are available

if (NOT TARGET Boost::boost)
    message(WARNING "Boost::boost target not found - using legacy Boost variables")
endif ()

# Note: Modern CMake's find_package(Boost) automatically creates:
# - Boost::boost (header-only interface)
# - Boost::program_options (compiled library)
# - Boost::filesystem, Boost::system, etc. for other components
