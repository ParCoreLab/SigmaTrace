#pragma once

#include "Common.hpp"
#include "EventTrait.hpp"
#include "Events.hpp"

namespace Profiler::Events {
    // TODO: check if no pointers (const char *)
    // TODO: string table instead of copying 1024 bytes every time
    EVENT_STRUCT CUDAKernelLaunch {
        GpuId_T gpu_id{-1};
        char kernel_name[1024]{};
    };

    EVENT_STRUCT CUDAKernelExit {
        GpuId_T gpu_id{-1};
        char kernel_name[1024]{};
    };
} // namespace Profiler::Events

// clang-format off
DEFINE_EVENT_TRAIT(Profiler::Events::CUDAKernelLaunch, Events::EventTypeId::CudaKernelLaunch, Events::EventCategory::GPU, "kernel_launch");
DEFINE_EVENT_TRAIT(Profiler::Events::CUDAKernelExit, Events::EventTypeId::CudaKernelExit, Events::EventCategory::GPU, "kernel_exit");
// clang-format on
