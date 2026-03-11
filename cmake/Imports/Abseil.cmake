include(Common)

sigma_import_options(ABSEIL)

# Not an ExternalProject because there are too many targets
# with complex interdependencies and this is easier.
# We can't find_package after an ExternalProject_add because it happens
# after the initial configure step and fails because the dependencies
# from the ExternalProject_add don't exist :(

# Settings required when building from source
# Neded for shared libs and install targets
set(ABSL_BUILD_TESTING OFF)
set(ABSL_USE_GOOGLETEST_HEAD OFF)
set(ABSL_PROPAGATE_CXX_STD ON)
set(ABSL_ENABLE_INSTALL ON)

if (BUILD_ABSEIL)
    # Prefer building from source; fall back to system if source unavailable
    sigma_source_available("${CMAKE_CURRENT_SOURCE_DIR}/Imports/abseil-cpp" _can_build)
    if (_can_build)
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/Imports/abseil-cpp" "${CMAKE_BINARY_DIR}/abseil-build" EXCLUDE_FROM_ALL SYSTEM)
        message(STATUS "[Sigma] Abseil: building from source (Imports/abseil-cpp)")
    else ()
        message(WARNING "[Sigma] Abseil: source not available (Imports/abseil-cpp missing), trying system")
        if (ABSEIL_ROOT)
            find_package(absl QUIET PATHS "${ABSEIL_ROOT}" NO_DEFAULT_PATH)
        else ()
            find_package(absl QUIET)
        endif ()
        if (absl_FOUND)
            message(STATUS "[Sigma] Abseil: found system installation")
        else ()
            message(FATAL_ERROR "[Sigma] Abseil: source unavailable and system not found")
        endif ()
    endif ()
else ()
    if (ABSEIL_ROOT)
        find_package(absl QUIET PATHS "${ABSEIL_ROOT}" NO_DEFAULT_PATH)
    else ()
        find_package(absl QUIET)
    endif ()
    if (absl_FOUND)
        message(STATUS "[Sigma] Abseil: found system installation")
    else ()
        sigma_source_available("${CMAKE_CURRENT_SOURCE_DIR}/Imports/abseil-cpp" _can_build)
        if (_can_build)
            add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/Imports/abseil-cpp" "${CMAKE_BINARY_DIR}/abseil-build" EXCLUDE_FROM_ALL SYSTEM)
            message(STATUS "[Sigma] Abseil: building from source (Imports/abseil-cpp)")
        else ()
            message(FATAL_ERROR "[Sigma] Abseil: not found in system and source not available (Imports/abseil-cpp missing)")
        endif ()
    endif ()
endif ()
