#pragma once

#include <cstdint>

#include "Common.hpp"
#include "EventTrait.hpp"
#include "Events.hpp"

// TODO: compile these into an .so or something so future readers can reliable read back maybe

namespace Profiler::Events {
    EVENT_STRUCT Mlx5dvMemoryRegistration {
        // TODO: should be GpuId_T (or something similar)
        uint64_t context_addr;
        uint64_t umem_addr;
        uint64_t addr;
        uint64_t size;
        uint32_t mem_id;
    };

    EVENT_STRUCT Mlx5dvMemoryDereg {
        // Curiously, no context_addr here
        uint64_t umem_addr;
        uint32_t mem_id;
    };

    EVENT_STRUCT Mlx5dvUarAlloc {
        uint64_t context_addr;
        uint64_t uar_addr;
        uint64_t base_addr;
        uint64_t reg_addr;
        uint32_t page_id;
        uint32_t flags;
    };

    EVENT_STRUCT Mlx5dvUarFree {
        // no context_addr here either
        uint64_t uar_addr;
        uint32_t page_id;
    };

    // Data doorbell: RDMA_WRITE (0x08), SEND (0x0A), RDMA_READ (0x10)
    EVENT_STRUCT DoorbellRingData {
        uint32_t prod_index;
        uint32_t qp_num;
        GpuId_T gpu_id;
        uint64_t bytes_sent{0};
        uint8_t opcode{0};
    };

    // Control doorbell: barriers, atomics, NOP, fence, etc.
    EVENT_STRUCT DoorbellRingControl {
        uint32_t prod_index;
        uint32_t qp_num;
        GpuId_T gpu_id;
        uint8_t opcode{0};
        uint64_t remote_addr{0}; // Target address from raddr_seg (for atomics/barriers)
    };

    // Helper: returns true if opcode moves data
    inline bool is_data_opcode(uint8_t opcode) {
        return opcode == 0x08 || opcode == 0x0A || opcode == 0x10;
    }
} // namespace Profiler::Events

// clang-format off
DEFINE_EVENT_TRAIT(Profiler::Events::Mlx5dvMemoryRegistration, Events::EventTypeId::Mlx5dvMemoryRegistration, Events::EventCategory::NIC, "mlx5dv_devx_umem_reg");
DEFINE_EVENT_TRAIT(Profiler::Events::Mlx5dvMemoryDereg, Events::EventTypeId::Mlx5dvMemoryDereg, Events::EventCategory::NIC, "mlx5dv_devx_umem_dereg");

DEFINE_EVENT_TRAIT(Profiler::Events::Mlx5dvUarAlloc, Events::EventTypeId::Mlx5dvUarAlloc, Events::EventCategory::NIC, "mlx5dv_devx_alloc_uar");
DEFINE_EVENT_TRAIT(Profiler::Events::Mlx5dvUarFree, Events::EventTypeId::Mlx5dvUarFree, Events::EventCategory::NIC, "mlx5dv_devx_free_uar");

DEFINE_EVENT_TRAIT(Profiler::Events::DoorbellRingData, Events::EventTypeId::Mlx5dvDoorbellRingData, Events::EventCategory::NIC, "Doorbell Data");
DEFINE_EVENT_TRAIT(Profiler::Events::DoorbellRingControl, Events::EventTypeId::Mlx5dvDoorbellRingControl, Events::EventCategory::NIC, "Doorbell Control");
// clang-format on
