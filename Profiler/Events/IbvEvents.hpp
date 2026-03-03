#pragma once

#include <cstdint>

#include <infiniband/verbs.h>

#include "Common.hpp"
#include "EventTrait.hpp"

// TODO: compile these into an .so or something so future readers can reliably read back maybe

namespace Profiler::Events {
    // Non trace
    EVENT_STRUCT IbvDeviceRegistration {
        uint64_t context_addr;
        uint64_t node_guid;
        uint64_t sys_image_guid;

        char name[64]{};
        char dev_name[64]{};
        char dev_path[256]{};
        char ibdev_path[256]{};

        ibv_node_type node_type;
        ibv_transport_type transport_type;
    };

    // Non trace
    EVENT_STRUCT IbvPortRegistration {
        uint8_t port_num;
        uint16_t lid;
        ibv_port_state state;
        uint8_t link_layer;
        ibv_gid gid;
    };

    EVENT_STRUCT IbvOpenDevice {
        uint64_t context_addr;
        uint64_t node_guid;
    };

    EVENT_STRUCT IbvCloseDevice {
        uint64_t context_addr;
    };

    EVENT_STRUCT IbvCreateQp {
        uint64_t context_addr;
        uint32_t qp_num;
        // ibv_qp_init_attr qp_init_attr
    };

    EVENT_STRUCT IbvDestroyQp {
        uint64_t context_addr;
        uint32_t qp_num;
    };

    // verbs.h
    EVENT_STRUCT IbvModifyQp {
        uint64_t context_addr;
        uint32_t qp_num;

        // Just record everything, why not :)
        ibv_qp_attr attr;

        /*
        uint32_t dest_qp_num;

        ibv_gid dgid;

        // ibv_ah_attr
        uint16_t dlid;
        uint8_t sl;

        uint8_t port_num;
        */
    };
    // TODO: event system should do this
    static_assert(std::is_trivial_v<ibv_qp_attr> && std::is_standard_layout_v<ibv_qp_attr>);

    enum class IbvMemoryRegType : uint16_t {
        Unknown = 0,
        ibv_reg_mr_iova2
    };

    EVENT_STRUCT IbvMemoryReg {
        uint64_t context_addr;
        uint64_t addr;
        uint64_t size;
        IbvMemoryRegType type;
    };

    EVENT_STRUCT IbvMemoryDereg {
        uint64_t context_addr;
        uint64_t addr;
        uint64_t size;
        IbvMemoryRegType type;
    };

    EVENT_STRUCT IbvPostSend {
        uint64_t context_addr;
        uint32_t num_wrs;
        uint32_t qp_num; // Queue pair number
    };

    EVENT_STRUCT IbvPostSendWr {
        uint64_t context_addr;
        uint64_t wr_id;
        uint32_t qp_num;
        int32_t num_sge;
        uint32_t send_flags;
        ibv_wr_opcode opcode;
    };

    EVENT_STRUCT IbvPostSendSg {
        uint64_t context_addr;
        uint32_t qp_num;
        uint64_t wr_id;
        uint64_t addr;
        uint64_t length;
        uint32_t lkey;
    };

    EVENT_STRUCT IbvPostRecv {
        uint64_t context_addr;
        uint32_t qp_num;
        uint64_t num_wrs;
    };

    EVENT_STRUCT IbvPostRecvWr {
        uint64_t context_addr;
        uint32_t qp_num;
        uint64_t wr_id;
        int32_t num_sge;
    };

    EVENT_STRUCT IbvPollCq {
        uint64_t context_addr;
        uint64_t cq_addr;
        int32_t num_requested;
        int32_t num_completed;
    };

    EVENT_STRUCT IbvCompletion {
        // uint64_t timestamp_tsc;
        // imm_data
        uint64_t context_addr;
        uint64_t wr_id;
        uint32_t qp_num;
        ibv_wc_opcode opcode;
        ibv_wc_status status;
        uint32_t byte_len;
    };

    EVENT_STRUCT QpConnectionInfo {
        uint64_t context_addr;
        uint32_t src_qpn;
        uint32_t dest_qpn;
        uint64_t dest_interface_id;  // from dgid.global.interface_id (0 if unavailable)
        uint64_t dest_subnet_prefix; // from dgid.global.subnet_prefix
        uint16_t dest_lid;           // local ID (used when GID unavailable on same subnet)
        uint8_t port_num;
    };

    EVENT_STRUCT CommunicationSummary {
        uint32_t src_qpn;
        uint32_t dest_qpn;
        uint64_t bytes_sent;
        uint64_t bytes_recv;
        uint32_t send_count;
        uint32_t recv_count;
        uint64_t src_node_guid;  // from IbvModifyQp / device registration
        uint64_t dest_node_guid; // 0 if unknown
        GpuId_T gpu_id;          // -1 if CPU-only
        uint16_t dest_lid;       // for same-subnet identification
    };

} // namespace Profiler::Events

// clang-format off
DEFINE_EVENT_TRAIT(Profiler::Events::IbvDeviceRegistration, Events::EventTypeId::IbvDeviceRegistration, Events::EventCategory::NIC, "IBV Device Registration");
DEFINE_EVENT_TRAIT(Profiler::Events::IbvPortRegistration, Events::EventTypeId::IbvPortRegistration, Events::EventCategory::NIC, "IBV Port Registration");

DEFINE_EVENT_TRAIT(Profiler::Events::IbvOpenDevice, Events::EventTypeId::IbvOpenDevice, Events::EventCategory::NIC, "ibv_open_device");
DEFINE_EVENT_TRAIT(Profiler::Events::IbvCloseDevice, Events::EventTypeId::IbvCloseDevice, Events::EventCategory::NIC, "ibv_close_device");

DEFINE_EVENT_TRAIT(Profiler::Events::IbvCreateQp, Events::EventTypeId::IbvCreateQp, Events::EventCategory::NIC, "ibv_create_qp");
DEFINE_EVENT_TRAIT(Profiler::Events::IbvDestroyQp, Events::EventTypeId::IbvDestroyQp, Events::EventCategory::NIC, "ibv_destroy_qp");
DEFINE_EVENT_TRAIT(Profiler::Events::IbvModifyQp, Events::EventTypeId::IbvModifyQp, Events::EventCategory::NIC, "ibv_modify_qp");

DEFINE_EVENT_TRAIT(Profiler::Events::IbvMemoryReg, Events::EventTypeId::IbvMemoryReg, Events::EventCategory::NIC, "IBV Memory Registration");
DEFINE_EVENT_TRAIT(Profiler::Events::IbvMemoryDereg, Events::EventTypeId::IbvMemoryDereg, Events::EventCategory::NIC, "IBV Memory Deregistration");

DEFINE_EVENT_TRAIT(Profiler::Events::IbvPostSend, Events::EventTypeId::IbvPostSend, Events::EventCategory::NIC, "ibv_post_send");
DEFINE_EVENT_TRAIT(Profiler::Events::IbvPostSendWr, Events::EventTypeId::IbvPostSendWr, Events::EventCategory::NIC, "ibv_post_send WR");
DEFINE_EVENT_TRAIT(Profiler::Events::IbvPostSendSg, Events::EventTypeId::IbvPostSendSg, Events::EventCategory::NIC, "ibv_post_send WR SG");

DEFINE_EVENT_TRAIT(Profiler::Events::IbvPostRecv, Events::EventTypeId::IbvPostRecv, Events::EventCategory::NIC, "ibv_post_recv");
DEFINE_EVENT_TRAIT(Profiler::Events::IbvPostRecvWr, Events::EventTypeId::IbvPostRecvWr, Events::EventCategory::NIC, "ibv_post_recv WR");

DEFINE_EVENT_TRAIT(Profiler::Events::IbvPollCq, Events::EventTypeId::IbvPollCq, Events::EventCategory::NIC, "ibv_poll_cq");
DEFINE_EVENT_TRAIT(Profiler::Events::IbvCompletion, Events::EventTypeId::IbvPollCqCompletion, Events::EventCategory::NIC, "IBV Completion");

DEFINE_EVENT_TRAIT(Profiler::Events::QpConnectionInfo, Events::EventTypeId::QpConnectionInfo, Events::EventCategory::NIC, "QP Connection Info");
DEFINE_EVENT_TRAIT(Profiler::Events::CommunicationSummary, Events::EventTypeId::CommunicationSummary, Events::EventCategory::NIC, "Communication Summary");

// clang-format on
