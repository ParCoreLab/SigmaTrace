include_guard(GLOBAL)

# sigma_import_options(<PACKAGE>
#   [BUILD_DEFAULT <ON|OFF>]      # default: OFF
#   [WITH_ENABLE]                 # also declare SIGMA_ENABLE_<PACKAGE>
#   [ENABLE_DEFAULT <ON|OFF>]     # default: ON, only with WITH_ENABLE
# )
# Declares BUILD_<PACKAGE> option and <PACKAGE>_ROOT cache path.
# Optionally declares SIGMA_ENABLE_<PACKAGE>.
function(sigma_import_options _PACKAGE)
    cmake_parse_arguments(arg "WITH_ENABLE" "BUILD_DEFAULT;ENABLE_DEFAULT" "" ${ARGN})
    if (NOT DEFINED arg_BUILD_DEFAULT)
        set(arg_BUILD_DEFAULT OFF)
    endif ()
    option(BUILD_${_PACKAGE} "Build ${_PACKAGE} from source (falls back to system if source unavailable)" ${arg_BUILD_DEFAULT})
    set(${_PACKAGE}_ROOT "" CACHE PATH "Prefix for system ${_PACKAGE} installation")
    if (arg_WITH_ENABLE)
        if (NOT DEFINED arg_ENABLE_DEFAULT)
            set(arg_ENABLE_DEFAULT ON)
        endif ()
        option(SIGMA_ENABLE_${_PACKAGE} "Enable ${_PACKAGE} support" ${arg_ENABLE_DEFAULT})
    endif ()
endfunction()

# sigma_source_available(<SOURCE_DIR> <RESULT_VAR>)
# Sets RESULT_VAR to TRUE if SOURCE_DIR contains a CMakeLists.txt (i.e. submodule is populated).
function(sigma_source_available _SOURCE_DIR _RESULT_VAR)
    if (EXISTS "${_SOURCE_DIR}/CMakeLists.txt")
        set(${_RESULT_VAR} TRUE PARENT_SCOPE)
    else ()
        set(${_RESULT_VAR} FALSE PARENT_SCOPE)
    endif ()
endfunction()
