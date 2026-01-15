#pragma once

#include <cstdint>
#include <limits>

namespace Events {
    // CRITICAL: Do not EVER change this. It's for a very basic checksum to validate each event
    constexpr uint64_t MAGIC_VALUE = std::numeric_limits<decltype(MAGIC_VALUE)>::max();

    using EventId_T = uint16_t;

    // CRITICAL: Event IDs are stable and MUST NEVER change once assigned lest you break backwards compatibility
    // Always append new events at the end of each range with the next available ID.
    // Deprecated events should be marked but NEVER removed or reused.
    enum class EventTypeId : EventId_T {
        Unknown = 0,

        // IBVerbs events (100-199)
        IbvPostSend = 100,
        IbvPostRecv = 101,
        IbvPollCq = 102,
        IbvPostSrqRecv = 103,
        IbvReqNotifyCq = 104,
        IbvCompletion = 105, // Individual work completion

        Mlx5dvDoorbellRing = 200,

        // CUDA events (200-299) - reserved for future
        CudaKernelLaunch = 300,
        CudaKernelExit = 301,
        CudaMemcpy = 302,
        CudaMemset = 303,
        CudaSync = 304,

        // NCCL events (300-399) - reserved for future
        NcclAllReduce = 400,
        NcclAllGather = 401,
        NcclBroadcast = 402,
        NcclReduceScatter = 403,

        // User-defined events start at 1000
        UserDefined = 1000,

        MAX_EVENT_TYPE = 65535
    };

    static_assert(static_cast<uint16_t>(EventTypeId::MAX_EVENT_TYPE) <= std::numeric_limits<EventId_T>::max());

} // namespace Events
