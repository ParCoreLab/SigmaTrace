#pragma once

#include "EventTrait.hpp"

namespace Profiler::Events {
    // TODO: check if no pointers (const char *)
    // RIODSIhasiodusahd
    struct alignas(8) CUDAKernelLaunch {
        char kernel_name[1024];
    };

    struct alignas(8) CUDAKernelExit {
        char kernel_name[1024];
    };
} // namespace Profiler::Events

DEFINE_EVENT_TRAIT(Profiler::Events::CUDAKernelLaunch, Events::EventTypeId::CudaKernelLaunch, "kernel_launch");
DEFINE_EVENT_TRAIT(Profiler::Events::CUDAKernelExit, Events::EventTypeId::CudaKernelExit, "kernel_exit");
