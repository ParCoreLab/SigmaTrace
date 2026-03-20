#pragma once

#include <cstdint>
#include <cstdlib>

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

        return static_cast<int32_t>(std::atoi(val));
    }

    inline ProcessRankInfo GetRankInfo() {
        ProcessRankInfo info;

        // Priority 1: Open MPI
        if (auto v = env_int32("OMPI_COMM_WORLD_RANK"); v >= 0) {
            info.rank = v;
            info.local_rank = env_int32("OMPI_COMM_WORLD_LOCAL_RANK");
            info.world_size = env_int32("OMPI_COMM_WORLD_SIZE");
            return info;
        }

        // Priority 2: PMI (Intel MPI, MPICH)
        if (auto v = env_int32("PMI_RANK"); v >= 0) {
            info.rank = v;
            info.world_size = env_int32("PMI_SIZE");
            return info;
        }

        // Priority 3: SLURM
        if (const auto v = env_int32("SLURM_PROCID"); v >= 0) {
            info.rank = v;
            info.local_rank = env_int32("SLURM_LOCALID");
            info.world_size = env_int32("SLURM_NTASKS");
            return info;
        }

        // Priority 4: PyTorch
        if (const auto v = env_int32("RANK"); v >= 0) {
            info.rank = v;
            info.local_rank = env_int32("LOCAL_RANK");
            info.world_size = env_int32("WORLD_SIZE");
            return info;
        }

        return info;
    }

} // namespace Profiler
