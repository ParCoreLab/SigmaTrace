#pragma once

#include <cstdint>

#include "Common.hpp"
#include "EventTrait.hpp"

namespace Profiler::Events {
    EVENT_STRUCT UcpInit {
        uint64_t context_addr;
    };

    EVENT_STRUCT UcpWorkerCreate {
        uint64_t context_addr;
        uint64_t worker_addr;
    };

    EVENT_STRUCT UcpWorkerDestroy {
        uint64_t worker_addr;
    };

    EVENT_STRUCT UcpEpCreate {
        uint64_t worker_addr;
        uint64_t ep_addr;
    };

    EVENT_STRUCT UcpEpClose {
        uint64_t ep_addr;
    };

    EVENT_STRUCT UcpTagSend {
        uint64_t ep_addr;
        uint64_t tag;
        uint64_t size;
        uint64_t request_addr;
    };

    EVENT_STRUCT UcpTagRecv {
        uint64_t worker_addr;
        uint64_t tag;
        uint64_t tag_mask;
        uint64_t size;
        uint64_t request_addr;
    };

    EVENT_STRUCT UcpPut {
        uint64_t ep_addr;
        uint64_t size;
        uint64_t remote_addr;
        uint64_t request_addr;
    };

    EVENT_STRUCT UcpGet {
        uint64_t ep_addr;
        uint64_t size;
        uint64_t remote_addr;
        uint64_t request_addr;
    };

    EVENT_STRUCT UcpAmSend {
        uint64_t ep_addr;
        uint32_t am_id;
        uint32_t _pad0{0};
        uint64_t header_size;
        uint64_t payload_size;
        uint64_t request_addr;
    };

    EVENT_STRUCT UcpMemMap {
        uint64_t context_addr;
        uint64_t addr;
        uint64_t size;
    };

    EVENT_STRUCT UcpMemUnmap {
        uint64_t context_addr;
        uint64_t mem_addr;
    };

    EVENT_STRUCT UcpWorkerProgress {
        uint64_t worker_addr;
        uint32_t num_progressed;
    };

    EVENT_STRUCT UcpEpQpMapping {
        uint64_t ep_addr;
        uint32_t qp_num;
    };
} // namespace Profiler::Events

// clang-format off
DEFINE_EVENT_TRAIT(Profiler::Events::UcpInit, Events::EventTypeId::UcpInit, Events::EventCategory::NIC, "ucp_init");
DEFINE_EVENT_TRAIT(Profiler::Events::UcpWorkerCreate, Events::EventTypeId::UcpWorkerCreate, Events::EventCategory::NIC, "ucp_worker_create");
DEFINE_EVENT_TRAIT(Profiler::Events::UcpWorkerDestroy, Events::EventTypeId::UcpWorkerDestroy, Events::EventCategory::NIC, "ucp_worker_destroy");
DEFINE_EVENT_TRAIT(Profiler::Events::UcpEpCreate, Events::EventTypeId::UcpEpCreate, Events::EventCategory::NIC, "ucp_ep_create");
DEFINE_EVENT_TRAIT(Profiler::Events::UcpEpClose, Events::EventTypeId::UcpEpClose, Events::EventCategory::NIC, "ucp_ep_close");
DEFINE_EVENT_TRAIT(Profiler::Events::UcpTagSend, Events::EventTypeId::UcpTagSend, Events::EventCategory::NIC, "ucp_tag_send_nbx");
DEFINE_EVENT_TRAIT(Profiler::Events::UcpTagRecv, Events::EventTypeId::UcpTagRecv, Events::EventCategory::NIC, "ucp_tag_recv_nbx");
DEFINE_EVENT_TRAIT(Profiler::Events::UcpPut, Events::EventTypeId::UcpPut, Events::EventCategory::NIC, "ucp_put_nbx");
DEFINE_EVENT_TRAIT(Profiler::Events::UcpGet, Events::EventTypeId::UcpGet, Events::EventCategory::NIC, "ucp_get_nbx");
DEFINE_EVENT_TRAIT(Profiler::Events::UcpAmSend, Events::EventTypeId::UcpAmSend, Events::EventCategory::NIC, "ucp_am_send_nbx");
DEFINE_EVENT_TRAIT(Profiler::Events::UcpMemMap, Events::EventTypeId::UcpMemMap, Events::EventCategory::NIC, "ucp_mem_map");
DEFINE_EVENT_TRAIT(Profiler::Events::UcpMemUnmap, Events::EventTypeId::UcpMemUnmap, Events::EventCategory::NIC, "ucp_mem_unmap");
DEFINE_EVENT_TRAIT(Profiler::Events::UcpWorkerProgress, Events::EventTypeId::UcpWorkerProgress, Events::EventCategory::NIC, "ucp_worker_progress");
DEFINE_EVENT_TRAIT(Profiler::Events::UcpEpQpMapping, Events::EventTypeId::UcpEpQpMapping, Events::EventCategory::NIC, "UCX EP-QP Mapping");
// clang-format on
