#pragma once

#include <string_view>

// Build-system defined args to provider .so files
namespace Profiler {
    static constexpr std::string_view SIGMA_IBVERBS_PROVIDER_PATH{SIGMA_PROVIDER_ibverbs_trace};
    static constexpr std::string_view SIGMA_MLX5DV_PROVIDER_PATH{SIGMA_PROVIDER_mlx5dv_trace};
    // static constexpr std::string_view SIGMA_SANITIZER_PROVIDER_PATH{SIGMA_PROVIDER_sanitizer_trace};
    static constexpr std::string_view SIGMA_CUPTI_PROVIDER_PATH{SIGMA_PROVIDER_cupti_trace};

#ifdef SIGMA_PROVIDER_libfabric_trace
    static constexpr std::string_view SIGMA_LIBFABRIC_PROVIDER_PATH{SIGMA_PROVIDER_libfabric_trace};
#endif

#ifdef SIGMA_PROVIDER_ucx_trace
    static constexpr std::string_view SIGMA_UCX_PROVIDER_PATH{SIGMA_PROVIDER_ucx_trace};
#endif
} // namespace Profiler
