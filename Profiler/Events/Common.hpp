#pragma once

#include <cstdint>

#include "EventTrait.hpp"
#include "Events.hpp"

namespace Profiler::Events {
    // TODO: can be -1
    // TODO: should be int32_t. Make sure all ids everywhere are int32_t
    using GpuId_T = std::int16_t;
    using GpuId_T = std::int16_t;
    constexpr GpuId_T UnknownDeviceId{-1};

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
DEFINE_EVENT_TRAIT(Profiler::Events::CommunicationSummary, Events::EventTypeId::CommunicationSummary, Events::EventCategory::NIC, "Communication Summary");
// clang-format on
