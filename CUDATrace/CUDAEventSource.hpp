#pragma once

#include "CUDAHooks.hpp"

#include "CompositeHookManager.hpp"

namespace cuda_trace {
    template<typename Derived>
    class CUDAEventSource : public hook::CompositeHookManager<Derived, int, cuda_trace_tag, cuda_kernel_launch_tag,
                                                              cuda_kernel_exit_tag> {

        // TODO: too verbose
        using Base =
                hook::CompositeHookManager<Derived, int, cuda_trace_tag, cuda_kernel_launch_tag, cuda_kernel_exit_tag>;

    public:
        using Context = Base::Context;

    protected:
        CUDAEventSource() = default;
    };

    // ================================================================
    // Specific "profilers"
    // TODO: move ts out of here
    // ================================================================
    struct NvbitProfiler : CUDAEventSource<NvbitProfiler> {
        NvbitProfiler();
    };

} // namespace cuda_trace
