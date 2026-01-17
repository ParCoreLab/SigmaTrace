#pragma once

#include "Common.hpp"
#include "EventTrait.hpp"

namespace Profiler::Events {
    // TODO: check if no pointers (const char *)
    // TODO: string table instead of copying 1024 bytes every time
    struct alignas(8) CUDAKernelLaunch {
        GpuId_T gpu_id{-1};
        char kernel_name[1024]{};

        // Convenience to allow adssaasdsadsauninitiaAllowinConvenience
        CUDAKernelLaunch() = default;
        explicit CUDAKernelLaunch(const GpuId_T id) : gpu_id(id) {}
    };

    struct alignas(8) CUDAKernelExit {
        GpuId_T gpu_id{-1};
        char kernel_name[1024]{};

        CUDAKernelExit() = default;
        explicit CUDAKernelExit(const GpuId_T id) : gpu_id(id) {}
    };
} // namespace Profiler::Events

DEFINE_EVENT_TRAIT(Profiler::Events::CUDAKernelLaunch, Events::EventTypeId::CudaKernelLaunch, "kernel_launch");
DEFINE_EVENT_TRAIT(Profiler::Events::CUDAKernelExit, Events::EventTypeId::CudaKernelExit, "kernel_exit");
