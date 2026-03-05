#include "../ProfilerShim.hpp"
#include "Common.hpp"

#include "../Events/CUDAEvents.hpp"
#include "../ProfilerProviders.hpp"
#include "CUDAEventSource.hpp"

void ProfilerCtx::load_cupti(const std::filesystem::path& shim_dir) {
    m_cupti_profiler = load_provider<cuda_trace::CUPTIEventSource>(shim_dir / Profiler::SIGMA_CUPTI_PROVIDER_PATH);

    m_cupti_profiler->register_pre<cuda_trace::cuda_kernel_launch_tag>(
            [](uint32_t gpu_id, const char* kernel_name, auto ts_start) {
        Profiler::Events::CUDAKernelLaunch event{
                .gpu_id = static_cast<GpuId_T>(gpu_id),
        };
        strncpy(event.kernel_name, kernel_name, sizeof event.kernel_name);

        instance().m_event_recorder->Record(event, ts_start);
    });

    m_cupti_profiler->register_pre<cuda_trace::cuda_kernel_exit_tag>(
            [](uint32_t gpu_id, const char* kernel_name, auto ts_start) {
        Profiler::Events::CUDAKernelExit event{
                .gpu_id = static_cast<GpuId_T>(gpu_id),
        };
        strncpy(event.kernel_name, kernel_name, sizeof event.kernel_name);

        instance().m_event_recorder->Record(event, ts_start);
    });

    std::cout << "Profiler loaded CUPTI" << std::endl;
}
