#pragma once

#include <cstdint>

#include "Common.hpp"
#include "EventTrait.hpp"

// TODO: compile these into an .so or something so future readers can reliable read back maybe

namespace Profiler::Events {
    struct alignas(8) Mlx5MemoryRegistration {
        GpuId_T gpu_id;
        int16_t nic_id;
        uint32_t mem_id;
        uint64_t addr;
        uint64_t size;
    };

    struct alignas(8) DoorbellRing {
        GpuId_T gpu_id;
        int16_t nic_id;
        uint64_t addr;
        uint64_t wqe_index;
        uint32_t qp_num;
    };
} // namespace Profiler::Events

DEFINE_EVENT_TRAIT(Profiler::Events::Mlx5MemoryRegistration, Events::EventTypeId::Mlx5MemoryRegistration,
                   "Memory Registration");
DEFINE_EVENT_TRAIT(Profiler::Events::DoorbellRing, Events::EventTypeId::Mlx5dvDoorbellRing, "Doorbell Ring");
