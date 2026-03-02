#pragma once

#include "CUDAHooks.hpp"

#include "CompositeHookManager.hpp"

namespace cuda_trace {
    template<typename Derived>
    class CUDAEventSource
        : public hook::CompositeHookManager<Derived, hook::DefaultHookManagerConfig, int, cuda_mem_op_tag,
                                            cuda_mem_alloc_tag, cuda_mem_register_tag, cuda_kernel_launch_tag,
                                            cuda_kernel_exit_tag> {

        // TODO: too verbose
        using Base = hook::CompositeHookManager<Derived, hook::DefaultHookManagerConfig, int, cuda_mem_op_tag,
                                                cuda_mem_alloc_tag, cuda_mem_register_tag, cuda_kernel_launch_tag,
                                                cuda_kernel_exit_tag>;

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

        void enable_memop() noexcept {
            m_trace_memop = true;
        }

        bool memop_enabled() const noexcept {
            return m_trace_memop;
        }

    private:
        // Memory operation tracing is too expensive
        bool m_trace_memop{false};
    };

    template<typename Derived>
    class CUDAAPIEventSource : public hook::CompositeHookManager<Derived, hook::DefaultHookManagerConfig, int,
                                                                 cuda_kernel_launch_tag, cuda_kernel_exit_tag> {

        // TODO: too verbose
        using Base = hook::CompositeHookManager<Derived, hook::DefaultHookManagerConfig, int, cuda_kernel_launch_tag,
                                                cuda_kernel_exit_tag>;

    public:
        using Context = Base::Context;

    protected:
        CUDAAPIEventSource() = default;
    };

    struct SanitizerEventSource : CUDAAPIEventSource<SanitizerEventSource> {
        SanitizerEventSource();
    };

    struct CUPTIEventSource : CUDAAPIEventSource<CUPTIEventSource> {
        CUPTIEventSource();
    };

} // namespace cuda_trace
