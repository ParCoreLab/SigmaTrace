#pragma once

// This unit both ensures the feature flags we use are actually defined
// and converts them to actual C++ constructs
namespace Profiler::Features {

    // GPU backends
    inline constexpr bool HasCUDA{SIGMA_HAS_CUDA};
    inline constexpr bool HasHIP{SIGMA_HAS_HIP};

    // Libraries
    inline constexpr bool HasRdmaCore{SIGMA_HAS_RDMA_CORE};
    inline constexpr bool HasNCCL{SIGMA_HAS_NCCL};
    inline constexpr bool HasNVSHMEM{SIGMA_HAS_NVSHMEM};
    inline constexpr bool HasNVBit{SIGMA_HAS_NVBIT};
    inline constexpr bool HasUCX{SIGMA_HAS_UCX};
    inline constexpr bool HasLibfabric{SIGMA_HAS_LIBFABRIC};

} // namespace Profiler::Features
