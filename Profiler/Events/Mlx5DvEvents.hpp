#pragma once

#include <cstdint>

#include "Common.hpp"
#include "EventTrait.hpp"
#include "Events.hpp"

// TODO: compile these into an .so or something so future readers can reliable read back maybe

namespace Profiler::Events {
    struct alignas(8) Mlx5dvMemoryRegistration {
        // TODO: should be GpuId_T (or something similar)
        uint64_t context_addr;
        uint64_t umem_addr;
        uint32_t mem_id;
        uint64_t addr;
        uint64_t size;
    };

    struct alignas(8) Mlx5dvMemoryDereg {
        // Curiously, no context_addr here
        uint64_t umem_addr;
        uint32_t mem_id;
    };

    struct alignas(8) Mlx5dvUarAlloc {
        uint64_t context_addr;
        uint64_t uar_addr;
        uint32_t page_id;
        uint64_t base_addr;
        uint64_t reg_addr;
        uint32_t flags;
    };

    struct alignas(8) Mlx5dvUarFree {
        // no context_addr here either
        uint64_t uar_addr;
        uint32_t page_id;
    };

    struct alignas(8) DoorbellRing {
        GpuId_T gpu_id;
        uint64_t addr;
        uint64_t fake_addr{0};
        uint64_t wqe_index;
        uint32_t qp_num;
        bool is_cpu_hijack{false};
    };
} // namespace Profiler::Events

// clang-format off
DEFINE_EVENT_TRAIT(Profiler::Events::Mlx5dvMemoryRegistration, Events::EventTypeId::Mlx5dvMemoryRegistration, Events::EventCategory::NIC, "mlx5dv_devx_umem_reg");
DEFINE_EVENT_TRAIT(Profiler::Events::Mlx5dvMemoryDereg, Events::EventTypeId::Mlx5dvMemoryDereg, Events::EventCategory::NIC, "mlx5dv_devx_umem_dereg");

DEFINE_EVENT_TRAIT(Profiler::Events::Mlx5dvUarAlloc, Events::EventTypeId::Mlx5dvUarAlloc, Events::EventCategory::NIC, "mlx5dv_devx_alloc_uar");
DEFINE_EVENT_TRAIT(Profiler::Events::Mlx5dvUarFree, Events::EventTypeId::Mlx5dvUarFree, Events::EventCategory::NIC, "mlx5dv_devx_free_uar");

DEFINE_EVENT_TRAIT(Profiler::Events::DoorbellRing, Events::EventTypeId::Mlx5dvDoorbellRing, Events::EventCategory::NIC, "Doorbell Ring");
// clang-format on
