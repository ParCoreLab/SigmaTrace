#pragma once

#include <cstdint>

#include "Common.hpp"
#include "EventTrait.hpp"
#include "Events.hpp"

// TODO: compile these into an .so or something so future readers can reliable read back maybe

namespace Profiler::Events {
    struct alignas(8) Mlx5MemoryRegistration {
        GpuId_T gpu_id;
        // TODO: should be GpuId_T (or something similar)
        int16_t nic_id;
        uint32_t mem_id;
        uint64_t addr;
        uint64_t size;
    };

    struct alignas(8) Mlx5MemoryDereg {
        int16_t nic_id;
        uint32_t mem_id;
    };

    struct alignas(8) DoorbellRing {
        GpuId_T gpu_id;
        int16_t nic_id;
        uint64_t addr;
        uint64_t fake_addr{0};
        uint64_t wqe_index;
        uint32_t qp_num;
        bool is_cpu_hijack{false};
    };
} // namespace Profiler::Events

// clang-format off
DEFINE_EVENT_TRAIT(Profiler::Events::Mlx5MemoryRegistration, Events::EventTypeId::Mlx5MemoryRegistration, Events::EventCategory::NIC, "mlx5dv_devx_umem_reg");

DEFINE_EVENT_TRAIT(Profiler::Events::Mlx5MemoryDereg, Events::EventTypeId::Mlx5MemoryDereg, Events::EventCategory::NIC, "mlx5dv_devx_umem_dereg");
DEFINE_EVENT_TRAIT(Profiler::Events::DoorbellRing, Events::EventTypeId::Mlx5dvDoorbellRing, Events::EventCategory::NIC, "Doorbell Ring");
// clang-format on
