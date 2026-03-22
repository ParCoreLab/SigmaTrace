#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdlib>

#include <optional>

namespace Profiler {
    struct ProcessRankInfo {
        int32_t rank{-1};
        int32_t local_rank{-1};
        int32_t world_size{-1};
    };

    inline int32_t env_int32(const char* name) {
        const char* val = std::getenv(name);
        if (!val || val[0] == '\0') {
            return -1;
        }

        char* endptr{};
        errno = 0;
        const long result = std::strtol(val, &endptr, 10);

        if (endptr == val || *endptr != '\0' || errno == ERANGE) {
            return -1;
        }

        return static_cast<int32_t>(result);
    }

    inline ProcessRankInfo GetRankInfo(const std::optional<std::string>& env_hint = std::nullopt) {
        ProcessRankInfo info;

        // Try the hint if we have one
        if (env_hint.has_value()) {
            if (const auto v = env_int32(env_hint.value().c_str()); v >= 0) {
                info.rank = v;
                info.local_rank = v;
                info.world_size = v;
                return info;
            }
        }

        // Open MPI
        if (auto v = env_int32("OMPI_COMM_WORLD_RANK"); v >= 0) {
            info.rank = v;
            info.local_rank = env_int32("OMPI_COMM_WORLD_LOCAL_RANK");
            info.world_size = env_int32("OMPI_COMM_WORLD_SIZE");
            return info;
        }

        // PMI (Intel MPI, MPICH)
        if (auto v = env_int32("PMI_RANK"); v >= 0) {
            info.rank = v;
            info.world_size = env_int32("PMI_SIZE");
            return info;
        }

        // SLURM
        if (const auto v = env_int32("SLURM_PROCID"); v >= 0) {
            info.rank = v;
            info.local_rank = env_int32("SLURM_LOCALID");
            info.world_size = env_int32("SLURM_NTASKS");
            return info;
        }

        // PyTorch
        if (const auto v = env_int32("RANK"); v >= 0) {
            info.rank = v;
            info.local_rank = env_int32("LOCAL_RANK");
            info.world_size = env_int32("WORLD_SIZE");
            return info;
        }

        return info;
    }

} // namespace Profiler
