#pragma once

#include <cstdint>

#include "EventTrait.hpp"

namespace Events::Ibv {
    // Packed struct for ibv_post_send events
    struct alignas(8) SimpleEvent {
        uint64_t hello; // Some number
    };

    // Packed struct for ibv_post_send events
    struct alignas(8) PostSendEvent {
        uint32_t qp_num;      // Queue pair number
        uint64_t wr_id;       // Work request ID
        uint32_t num_sge;     // Number of scatter-gather entries
        uint32_t opcode;      // IBV_WR_* opcode
        uint32_t send_flags;  // IBV_SEND_* flags
        uint64_t total_bytes; // Total bytes across all SGEs
        int32_t gpu_device;   // GPU device index (-1 for host memory)
        uint32_t _padding;    // Explicit padding for alignment
    };

    // Packed struct for ibv_post_recv events
    struct alignas(8) PostRecvEvent {
        uint32_t qp_num;    // Queue pair number
        uint64_t wr_id;     // Work request ID
        uint32_t num_sge;   // Number of scatter-gather entries
        int32_t gpu_device; // GPU device index (-1 for host memory)
    };

    // Packed struct for ibv_poll_cq events (the poll call itself)
    struct alignas(8) PollCqEvent {
        uint64_t cq_handle;    // CQ pointer/identifier
        int32_t num_requested; // How many entries requested
        int32_t num_completed; // How many actually completed
    };

    // Individual completion event (one per work completion returned)
    struct alignas(8) CompletionEvent {
        uint32_t qp_num;   // Queue pair number
        uint64_t wr_id;    // Work request ID
        uint32_t status;   // IBV_WC_* status code
        uint32_t opcode;   // IBV_WC_* opcode
        uint32_t byte_len; // Bytes transferred (for recv)
        uint32_t flags;    // Completion flags
    };

    // Packed struct for ibv_post_srq_recv events
    struct alignas(8) PostSrqRecvEvent {
        uint64_t srq_handle; // SRQ pointer/identifier
        uint64_t wr_id;      // Work request ID
        uint32_t num_sge;    // Number of scatter-gather entries
        int32_t gpu_device;  // GPU device index (-1 for host memory)
    };

    // Packed struct for ibv_req_notify_cq events
    struct alignas(8) ReqNotifyCqEvent {
        uint64_t cq_handle;     // CQ pointer/identifier
        int32_t solicited_only; // Whether only solicited completions trigger
        uint32_t _padding;      // Explicit padding
    };

} // namespace Events::Ibv


// Event trait definitions (must be at global/namespace scope after struct definitions)
DEFINE_EVENT_TRAIT(Events::Ibv::SimpleEvent, Events::EventTypeId::IbvPostSend, "simple_event");
DEFINE_EVENT_TRAIT(Events::Ibv::PostSendEvent, Events::EventTypeId::IbvPostSend, "ibv_post_send");
DEFINE_EVENT_TRAIT(Events::Ibv::PostRecvEvent, Events::EventTypeId::IbvPostRecv, "ibv_post_recv");
DEFINE_EVENT_TRAIT(Events::Ibv::PollCqEvent, Events::EventTypeId::IbvPollCq, "ibv_poll_cq");
DEFINE_EVENT_TRAIT(Events::Ibv::CompletionEvent, Events::EventTypeId::IbvCompletion, "ibv_completion");
DEFINE_EVENT_TRAIT(Events::Ibv::PostSrqRecvEvent, Events::EventTypeId::IbvPostSrqRecv, "ibv_post_srq_recv");
DEFINE_EVENT_TRAIT(Events::Ibv::ReqNotifyCqEvent, Events::EventTypeId::IbvReqNotifyCq, "ibv_req_notify_cq");
