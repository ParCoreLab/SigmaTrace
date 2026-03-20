#pragma once

#include <cstdint>

#include "Common.hpp"
#include "EventTrait.hpp"

// TODO: compile these into an .so or something so future readers can reliably read back maybe

namespace Profiler::Events {
    // We want to be able to have these types without pulling verbs.h
    // Might want to compile with no libibverbs available
    namespace types {
        // From verbs.h
        enum ibv_node_type {
            IBV_NODE_UNKNOWN = -1,
            IBV_NODE_CA = 1,
            IBV_NODE_SWITCH,
            IBV_NODE_ROUTER,
            IBV_NODE_RNIC,
            IBV_NODE_USNIC,
            IBV_NODE_USNIC_UDP,
            IBV_NODE_UNSPECIFIED,
        };

        enum ibv_transport_type {
            IBV_TRANSPORT_UNKNOWN = -1,
            IBV_TRANSPORT_IB = 0,
            IBV_TRANSPORT_IWARP,
            IBV_TRANSPORT_USNIC,
            IBV_TRANSPORT_USNIC_UDP,
            IBV_TRANSPORT_UNSPECIFIED,
        };

        // Originally a union
        struct ibv_gid {
            uint64_t subnet_prefix;
            uint64_t interface_id;
        };

        enum ibv_port_state {
            IBV_PORT_NOP = 0,
            IBV_PORT_DOWN = 1,
            IBV_PORT_INIT = 2,
            IBV_PORT_ARMED = 3,
            IBV_PORT_ACTIVE = 4,
            IBV_PORT_ACTIVE_DEFER = 5
        };

        enum ibv_wc_status {
            IBV_WC_SUCCESS,
            IBV_WC_LOC_LEN_ERR,
            IBV_WC_LOC_QP_OP_ERR,
            IBV_WC_LOC_EEC_OP_ERR,
            IBV_WC_LOC_PROT_ERR,
            IBV_WC_WR_FLUSH_ERR,
            IBV_WC_MW_BIND_ERR,
            IBV_WC_BAD_RESP_ERR,
            IBV_WC_LOC_ACCESS_ERR,
            IBV_WC_REM_INV_REQ_ERR,
            IBV_WC_REM_ACCESS_ERR,
            IBV_WC_REM_OP_ERR,
            IBV_WC_RETRY_EXC_ERR,
            IBV_WC_RNR_RETRY_EXC_ERR,
            IBV_WC_LOC_RDD_VIOL_ERR,
            IBV_WC_REM_INV_RD_REQ_ERR,
            IBV_WC_REM_ABORT_ERR,
            IBV_WC_INV_EECN_ERR,
            IBV_WC_INV_EEC_STATE_ERR,
            IBV_WC_FATAL_ERR,
            IBV_WC_RESP_TIMEOUT_ERR,
            IBV_WC_GENERAL_ERR,
            IBV_WC_TM_ERR,
            IBV_WC_TM_RNDV_INCOMPLETE,
        };

        enum ibv_wc_opcode {
            IBV_WC_SEND,
            IBV_WC_RDMA_WRITE,
            IBV_WC_RDMA_READ,
            IBV_WC_COMP_SWAP,
            IBV_WC_FETCH_ADD,
            IBV_WC_BIND_MW,
            IBV_WC_LOCAL_INV,
            IBV_WC_TSO,
            IBV_WC_FLUSH,
            IBV_WC_ATOMIC_WRITE = 9,
            /*
             * Set value of IBV_WC_RECV so consumers can test if a completion is a
             * receive by testing (opcode & IBV_WC_RECV).
             */
            IBV_WC_RECV = 1 << 7,
            IBV_WC_RECV_RDMA_WITH_IMM,

            IBV_WC_TM_ADD,
            IBV_WC_TM_DEL,
            IBV_WC_TM_SYNC,
            IBV_WC_TM_RECV,
            IBV_WC_TM_NO_TAG,
            IBV_WC_DRIVER1,
            IBV_WC_DRIVER2,
            IBV_WC_DRIVER3,
        };

        enum ibv_wr_opcode {
            IBV_WR_RDMA_WRITE,
            IBV_WR_RDMA_WRITE_WITH_IMM,
            IBV_WR_SEND,
            IBV_WR_SEND_WITH_IMM,
            IBV_WR_RDMA_READ,
            IBV_WR_ATOMIC_CMP_AND_SWP,
            IBV_WR_ATOMIC_FETCH_AND_ADD,
            IBV_WR_LOCAL_INV,
            IBV_WR_BIND_MW,
            IBV_WR_SEND_WITH_INV,
            IBV_WR_TSO,
            IBV_WR_DRIVER1,
            IBV_WR_FLUSH = 14,
            IBV_WR_ATOMIC_WRITE = 15,
        };
    } // namespace types

    // Non trace
    EVENT_STRUCT IbvDeviceRegistration {
        uint64_t context_addr;
        uint64_t node_guid;
        uint64_t sys_image_guid;

        char name[64]{};
        char dev_name[64]{};
        char dev_path[256]{};
        char ibdev_path[256]{};

        types::ibv_node_type node_type;
        types::ibv_transport_type transport_type;
    };

    // Non trace
    EVENT_STRUCT IbvPortRegistration {
        uint8_t port_num;
        uint16_t lid;
        types::ibv_port_state state;
        uint8_t link_layer;
        types::ibv_gid gid;
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
        // ibv_qp_attr attr;

        uint32_t dest_qp_num;

        uint16_t ah_attr_dlid;
        types::ibv_gid ah_attr_grh_dgid;

        /*
        uint32_t dest_qp_num;

        ibv_gid dgid;

        // ibv_ah_attr
        uint16_t dlid;
        uint8_t sl;

        uint8_t port_num;
        */
    };

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
        types::ibv_wr_opcode opcode;
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
        types::ibv_wc_opcode opcode;
        types::ibv_wc_status status;
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

    // Layer identifiers for CommunicationSummary
    enum class CommLayer : uint8_t {
        IBV = 0,
        UCX = 1,
        Libfabric = 2
    };

    EVENT_STRUCT CommunicationSummary {
        uint32_t src_qpn{};
        uint32_t dest_qpn{};
        uint64_t bytes_sent{};
        uint64_t bytes_recv{};
        uint32_t send_count{};
        uint32_t recv_count{};
        uint64_t src_node_guid{};  // from IbvModifyQp / device registration
        uint64_t dest_node_guid{}; // 0 if unknown
        GpuId_T gpu_id{-1};        // -1 if CPU-only
        uint16_t dest_lid{};       // for same-subnet identification
        uint64_t ep_addr{};        // endpoint address (UCX/libfabric), 0 for IBV
        CommLayer layer{};         // 0=ibv, 1=ucx, 2=libfabric
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
