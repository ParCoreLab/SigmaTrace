#pragma once

#include <cstdint>

#include "Common.hpp"
#include "EventTrait.hpp"

namespace Profiler::Events {
    EVENT_STRUCT FiDomainOpen {
        uint64_t fabric_addr;
        uint64_t domain_addr;
        char provider_name[32]{};
    };

    EVENT_STRUCT FiEndpointCreate {
        uint64_t domain_addr;
        uint64_t ep_addr;
        uint32_t ep_type;
    };

    EVENT_STRUCT FiEndpointClose {
        uint64_t ep_addr;
    };

    EVENT_STRUCT FiMemoryReg {
        uint64_t domain_addr;
        uint64_t addr;
        uint64_t size;
        uint64_t requested_key;
    };

    EVENT_STRUCT FiAvInsert {
        uint64_t av_addr;
        uint32_t count;
    };

    EVENT_STRUCT FiSend {
        uint64_t ep_addr;
        uint64_t context;
        uint64_t total_iov_len;
        uint64_t flags;
        uint64_t dest_addr;
    };

    EVENT_STRUCT FiRecv {
        uint64_t ep_addr;
        uint64_t context;
        uint64_t total_iov_len;
        uint64_t flags;
    };

    EVENT_STRUCT FiRdmaRead {
        uint64_t ep_addr;
        uint64_t context;
        uint64_t total_iov_len;
        uint64_t remote_addr;
        uint64_t remote_key;
    };

    EVENT_STRUCT FiRdmaWrite {
        uint64_t ep_addr;
        uint64_t context;
        uint64_t total_iov_len;
        uint64_t remote_addr;
        uint64_t remote_key;
    };

    EVENT_STRUCT FiCompletion {
        uint64_t cq_addr;
        uint64_t context;
        int32_t num_completions;
        uint32_t _pad0{0};
        uint64_t flags;
        uint64_t src_addr;
    };

    EVENT_STRUCT FiInject {
        uint64_t ep_addr;
        uint64_t len;
        uint64_t dest_addr;
    };

    EVENT_STRUCT FiEndpointQpMapping {
        uint64_t ep_addr;
        uint32_t qp_num;
    };
} // namespace Profiler::Events

// clang-format off
DEFINE_EVENT_TRAIT(Profiler::Events::FiDomainOpen, Events::EventTypeId::FiDomainOpen, Events::EventCategory::NIC, "fi_domain");
DEFINE_EVENT_TRAIT(Profiler::Events::FiEndpointCreate, Events::EventTypeId::FiEndpointCreate, Events::EventCategory::NIC, "fi_endpoint");
DEFINE_EVENT_TRAIT(Profiler::Events::FiEndpointClose, Events::EventTypeId::FiEndpointClose, Events::EventCategory::NIC, "fi_close(EP)");
DEFINE_EVENT_TRAIT(Profiler::Events::FiMemoryReg, Events::EventTypeId::FiMemoryReg, Events::EventCategory::NIC, "fi_mr_reg");
DEFINE_EVENT_TRAIT(Profiler::Events::FiAvInsert, Events::EventTypeId::FiAvInsert, Events::EventCategory::NIC, "fi_av_insert");
DEFINE_EVENT_TRAIT(Profiler::Events::FiSend, Events::EventTypeId::FiSend, Events::EventCategory::NIC, "fi_sendmsg");
DEFINE_EVENT_TRAIT(Profiler::Events::FiRecv, Events::EventTypeId::FiRecv, Events::EventCategory::NIC, "fi_recvmsg");
DEFINE_EVENT_TRAIT(Profiler::Events::FiRdmaRead, Events::EventTypeId::FiRdmaRead, Events::EventCategory::NIC, "fi_readmsg");
DEFINE_EVENT_TRAIT(Profiler::Events::FiRdmaWrite, Events::EventTypeId::FiRdmaWrite, Events::EventCategory::NIC, "fi_writemsg");
DEFINE_EVENT_TRAIT(Profiler::Events::FiCompletion, Events::EventTypeId::FiCompletion, Events::EventCategory::NIC, "fi_cq_read");
DEFINE_EVENT_TRAIT(Profiler::Events::FiInject, Events::EventTypeId::FiInject, Events::EventCategory::NIC, "fi_inject");
DEFINE_EVENT_TRAIT(Profiler::Events::FiEndpointQpMapping, Events::EventTypeId::FiEndpointQpMapping, Events::EventCategory::NIC, "FI EP-QP Mapping");
// clang-format on
