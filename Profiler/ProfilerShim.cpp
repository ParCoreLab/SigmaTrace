#include <tuple>
#include <vector>

#include "CUDAEventSource.hpp"
#include "IbverbsEventSource.hpp"

struct ProfilerCtx {
    // <addr, length>
    std::vector<std::tuple<uint64_t, size_t>> m_ibv_allocations;

    ProfilerCtx() {
        m_ibv_profiler = &ibv::Profiler::instance();
        m_cuda_profiler = &cuda_trace::NvbitProfiler::instance();

        m_ibv_profiler->register_pre<ibv::open_device_tag>([](auto &&...) {
            std::cout << "[Profiler] ibv_open_device received" << std::endl;
        });

        m_ibv_profiler->register_post<ibv::reg_mr_tag>(
                [this](ibv_mr *out_mr, ibv_pd *pd, void *addr, size_t length, int access) {
                    auto alloc_addr = reinterpret_cast<uint64_t>(addr);
                    std::cout << "[Profiler] ibv_reg_mr received at " << std::hex << alloc_addr << std::endl;
                    m_ibv_allocations.emplace_back(alloc_addr, length);
                });

        m_ibv_profiler->register_post<ibv::reg_mr_iova2_tag>(
                [this](ibv_mr *out_mr, ibv_pd *pd, void *addr, size_t length, auto &&...) {
                    auto alloc_addr = reinterpret_cast<uint64_t>(addr);
                    std::cout << "[Profiler] ibv_reg_mr_iova1232 received at " << std::hex << alloc_addr << std::endl;
                    m_ibv_allocations.emplace_back(alloc_addr, length);
                });

        m_cuda_profiler->register_pre<cuda_trace::cuda_trace_tag>(
                [this](uint64_t addr, std::string_view sass_str, auto &&...) {
                    // std::cout << "[Profiler] CUDA memory event received at " << std::hex << addr << std::endl;

                    for (auto [alloc_addr, alloc_length]: m_ibv_allocations) {
                        if (alloc_addr <= addr && addr < (alloc_addr + alloc_length)) {
                            std::cout << "yes @ " << sass_str << std::endl;
                            return;
                        }
                    }

                    // std::cout << "no @ " << sass_str << std::endl;
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
