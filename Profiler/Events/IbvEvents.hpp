#pragma once

#include <cstdint>

#include "infiniband/verbs.h"

#include "Common.hpp"
#include "EventTrait.hpp"

// TODO: compile these into an .so or something so future readers can reliable read back maybe

namespace Profiler::Events {
    struct alignas(8) IbvOpenDevice {
        uint64_t context_addr;
        char name[64]{};
        char dev_name[64]{};
        char dev_path[256]{};
        char ibdev_path[256]{};

        ibv_node_type node_type;
        ibv_transport_type transport_type;
    };

    struct alignas(8) IbvCloseDevice {
        uint64_t context_addr;
    };

    enum class IbvMemoryRegType : uint16_t {
        Unknown = 0,
        ibv_reg_mr_iova2
    };

    struct alignas(8) IbvMemoryReg {
        uint64_t context_addr;
        GpuId_T gpu_id;
        uint64_t addr;
        uint64_t size;
        IbvMemoryRegType type;
    };

    struct alignas(8) IbvPostSend {
        uint64_t context_addr;
        GpuId_T gpu_id;
        uint32_t qp_num; // Queue pair number
    };

    struct alignas(8) IbvPostSendWr {
        uint64_t context_addr;
        GpuId_T gpu_id;
        uint32_t qp_num;
        uint64_t wr_id;
        int32_t num_sge;
        ibv_wr_opcode opcode;
        uint32_t send_flags;
    };

    struct alignas(8) IbvPostSendSg {
        uint64_t context_addr;
        GpuId_T gpu_id;
        uint32_t qp_num;
        uint64_t wr_id;
        uint64_t addr;
        uint64_t length;
        uint32_t lkey;
    };

    struct alignas(8) IbvPostRecv {
        uint64_t context_addr;
        uint32_t qp_num;
    };

    struct alignas(8) IbvPostRecvWr {
        uint64_t context_addr;
        GpuId_T gpu_id;
        uint32_t qp_num;
        uint64_t wr_id;
        int32_t num_sge;
    };

    struct alignas(8) IbvPollCq {
        uint64_t context_addr;
        uint64_t cq_addr;
        int32_t num_requested;
        int32_t num_completed;
    };

    struct IbvCompletion {
        // uint64_t timestamp_tsc;
        // imm_data
        uint64_t context_addr;
        uint32_t qp_num;
        uint64_t wr_id;
        ibv_wc_opcode opcode;
        ibv_wc_status status;
        uint32_t byte_len;
        uint16_t pkey_index;
    };

} // namespace Profiler::Events

// clang-format off
DEFINE_EVENT_TRAIT(Profiler::Events::IbvOpenDevice, Events::EventTypeId::IbvOpenDevice, Events::EventCategory::NIC, "ibv_open_device");
DEFINE_EVENT_TRAIT(Profiler::Events::IbvCloseDevice, Events::EventTypeId::IbvCloseDevice, Events::EventCategory::NIC, "ibv_close_device");

DEFINE_EVENT_TRAIT(Profiler::Events::IbvMemoryReg, Events::EventTypeId::IbvMemoryReg, Events::EventCategory::NIC, "IBV Memory Registration");
DEFINE_EVENT_TRAIT(Profiler::Events::IbvPostSend, Events::EventTypeId::IbvPostSend, Events::EventCategory::NIC, "ibv_post_send");
DEFINE_EVENT_TRAIT(Profiler::Events::IbvPostSendWr, Events::EventTypeId::IbvPostSendWr, Events::EventCategory::NIC, "ibv_post_send WR");
DEFINE_EVENT_TRAIT(Profiler::Events::IbvPostSendSg, Events::EventTypeId::IbvPostSendSg, Events::EventCategory::NIC, "ibv_post_send WR SG");

DEFINE_EVENT_TRAIT(Profiler::Events::IbvPostRecv, Events::EventTypeId::IbvPostRecv, Events::EventCategory::NIC, "ibv_post_recv");
DEFINE_EVENT_TRAIT(Profiler::Events::IbvPostRecvWr, Events::EventTypeId::IbvPostRecvWr, Events::EventCategory::NIC, "ibv_post_recv WR");

DEFINE_EVENT_TRAIT(Profiler::Events::IbvPollCq, Events::EventTypeId::IbvPollCq, Events::EventCategory::NIC, "ibv_poll_cq");
DEFINE_EVENT_TRAIT(Profiler::Events::IbvCompletion, Events::EventTypeId::IbvPollCqCompletion, Events::EventCategory::NIC, "IBV Completion");

// clang-format on
