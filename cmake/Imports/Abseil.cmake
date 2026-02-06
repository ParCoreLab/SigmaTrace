option(BUILD_ABSEIL "Build bundled Abseil" ON)
set(ABSEIL_ROOT "" CACHE PATH "Prefix where system Abseil is installed")

# Not an External Project because there are too many targets
# with complex interdependencies and this is easier
# We can't find_package after an ExternalProject_add because it happens
# after the initial configure step and fails because the dependencies
# from the ExternalProject_add don't exist :(
if (BUILD_ABSEIL)
    # WHY ABSEIL WHY
    # We need this to use in shared libs (because of -fPIC)
    # set(BUILD_SHARED_LIBS ON)
    set(ABSL_BUILD_TESTING OFF)
    set(ABSL_USE_GOOGLETEST_HEAD OFF)
    set(ABSL_PROPAGATE_CXX_STD ON)
    set(ABSL_ENABLE_INSTALL ON)
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/Imports/abseil-cpp" "${CMAKE_BINARY_DIR}/abseil-build" EXCLUDE_FROM_ALL SYSTEM)
else ()
    if (ABSEIL_ROOT)
        find_package(absl REQUIRED PATHS "${ABSEIL_ROOT}" NO_DEFAULT_PATH)
    else ()
        find_package(absl REQUIRED)
    endif ()
endif ()
