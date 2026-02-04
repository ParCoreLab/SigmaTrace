#pragma once

#include <cstdint>
#include <limits>

namespace Events {
    // CRITICAL: Do not EVER change this. It's for a very basic checksum to validate each event
    constexpr uint64_t MAGIC_VALUE = std::numeric_limits<decltype(MAGIC_VALUE)>::max();

    using EventId_T = std::uint16_t;

    // CRITICAL: Event IDs are stable and MUST NEVER change once assigned lest you break backwards compatibility
    // Always append new events at the end of each range with the next available ID.
    // Deprecated events should be marked but NEVER removed or reused.
    enum class EventTypeId : EventId_T {
        Unknown = 0,

        // IBVerbs events (100-199)
        IbvDeviceRegistration = 100,
        IbvOpenDevice,
        IbvCloseDevice,
        // ================================================================
        IbvMemoryReg,
        // ================================================================
        IbvCreateQp,
        IbvDestroyQp,
        IbvModifyQp,
        // ================================================================
        IbvPostSend,
        IbvPostSendWr,
        IbvPostSendSg,
        // ================================================================
        IbvPostRecv,
        IbvPostRecvWr,
        // ================================================================
        IbvPollCq,
        IbvPollCqCompletion,
        // ================================================================
        IbvPostSrqRecv,
        IbvReqNotifyCq,
        // IbvCompletion = 112, // Individual work completion
        // ================================================================

        // mlx5dv specific events
        Mlx5dvDoorbellRing = 200,
        Mlx5dvMemoryRegistration,
        Mlx5dvMemoryDereg,

        // CUDA events
        CudaKernelLaunch = 300,
        CudaKernelExit,
        CudaMemcpy,
        CudaMemset,
        CudaSync,

        // NCCL events
        NcclAllReduce = 400,
        NcclAllGather,
        NcclBroadcast,
        NcclReduceScatter,

        // User-defined events start at 1000
        UserDefined = 1000,

        MAX_EVENT_TYPE = 65535
    };
    static_assert(static_cast<uint16_t>(EventTypeId::MAX_EVENT_TYPE) <= std::numeric_limits<EventId_T>::max());

    enum class EventCategory {
        Unknown = 0,
        CPU,
        GPU,
        NIC,
        // TODO: need to differentiate between CPU and GPU RDMA calls as well as non-RDMA ops
        // GPU_RDMA,
        // CPU_RDM
    };
} // namespace Events
