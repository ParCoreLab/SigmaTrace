#pragma once

#include <cstdint>

namespace Events {
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

        // CUDA events (200-299) - reserved for future
        CudaKernelLaunch = 200,
        CudaMemcpy = 201,
        CudaMemset = 202,
        CudaSync = 203,

        // NCCL events (300-399) - reserved for future
        NcclAllReduce = 300,
        NcclAllGather = 301,
        NcclBroadcast = 302,
        NcclReduceScatter = 303,

        // User-defined events start at 1000
        UserDefined = 1000,

        MAX_EVENT_TYPE = 65535
    };

    static_assert(static_cast<uint16_t>(EventTypeId::MAX_EVENT_TYPE) <= std::numeric_limits<EventId_T>::max());

} // namespace Events
