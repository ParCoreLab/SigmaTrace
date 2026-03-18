# ================================================================
# Feature Flags
# ================================================================
set(SIGMA_FEATURE_LIST "" CACHE INTERNAL "List of all Sigma features")

# ================================================================
# Register a feature. Creates SIGMA_HAS_<NAME> defaulting to OFF.
# Features must be defined here before they're used
macro(Sigma_add_feature NAME)
    set(SIGMA_HAS_${NAME} OFF CACHE INTERNAL "Whether ${NAME} support is available")
    list(APPEND SIGMA_FEATURE_LIST ${NAME})
    set(SIGMA_FEATURE_LIST "${SIGMA_FEATURE_LIST}" CACHE INTERNAL "")
endmacro()

# Enables a feature
macro(Sigma_enable_feature NAME)
    if ("SIGMA_HAS_${NAME}" IN_LIST SIGMA_FEATURE_LIST)
        message(FATAL_ERROR "SIGMA_HAS_${NAME} not a known feature. Declare it with Sigma_add_feature first")
    endif ()

    message(STATUS "[Sigma] Enabled feature ${NAME}")
    set(SIGMA_HAS_${NAME} ON CACHE INTERNAL "" FORCE)
endmacro()

# Applies feature flags to a target
function(Sigma_apply_feature_flags target)
    foreach (FEATURE IN LISTS SIGMA_FEATURE_LIST)
        if (SIGMA_HAS_${FEATURE})
            target_compile_definitions(${target} INTERFACE SIGMA_HAS_${FEATURE}=1)
        else ()
            target_compile_definitions(${target} INTERFACE SIGMA_HAS_${FEATURE}=0)
        endif ()
    endforeach ()
endfunction()

# Create an interface and load all
function(Sigma_load_feature_flags)
    add_library(sigma_feature_flags INTERFACE)
    Sigma_apply_feature_flags(sigma_feature_flags)
endfunction()

# ================================================================
# All Feature Flags definitions
# ================================================================
Sigma_add_feature(CUDA)
Sigma_add_feature(HIP)

# Libraries
Sigma_add_feature(RDMA_CORE)
Sigma_add_feature(NCCL)
Sigma_add_feature(NVSHMEM)
Sigma_add_feature(NVBIT)
Sigma_add_feature(UCX)
Sigma_add_feature(LIBFABRIC)
Sigma_add_feature(LIBURING)
# ================================================================

