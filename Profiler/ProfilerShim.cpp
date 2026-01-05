#include "CUDAEventSource.hpp"
#include "IbverbsEventSource.hpp"

struct ProfilerCtx {
    ProfilerCtx() {
        m_ibv_profiler = &ibv::Profiler::instance();
        m_cuda_profiler = &cuda_trace::NvbitProfiler::instance();

        m_ibv_profiler->register_pre<ibv::open_device_tag>([](auto &&...) {
            std::cout << "[Profiler] ibv_open_device received" << std::endl;
        });

        m_ibv_profiler->register_pre<ibv::reg_mr_tag>([](auto &&...) {
            std::cout << "[Profiler] ibv_reg_mr received" << std::endl;
        });

        m_cuda_profiler->register_pre<cuda_trace::cuda_trace_tag>([](auto &&...) {
            std::cout << "[Profiler] CUDA memory trace event received" << std::endl;
        });
    }

    static ProfilerCtx *instance() {
        static ProfilerCtx *instance;

        if (!instance) {
            instance = new ProfilerCtx();

            // TODO:
            printf("Created ProfilerCtx @ %p\n", instance);
        }

        return instance;
    }

    ibv::Profiler *m_ibv_profiler;
    cuda_trace::NvbitProfiler *m_cuda_profiler;
};

static void __attribute__((constructor)) init() { ProfilerCtx::instance(); }
