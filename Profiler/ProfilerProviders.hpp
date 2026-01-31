#pragma once

#include <string_view>

// Build-system defined args to provider .so files
namespace Profiler {
    static constexpr std::string_view SIGMA_IBVERBS_PROVIDER_PATH{SIGMA_PROVIDER_ibverbs_trace};
    static constexpr std::string_view SIGMA_NVBIT_PROVIDER_PATH{SIGMA_PROVIDER_nvbit_memory_tracker};
    static constexpr std::string_view SIGMA_MLX5DV_PROVIDER_PATH{SIGMA_PROVIDER_mlx5dv_trace};
} // namespace Profiler
