#pragma once

#include <cstdint>

#include "EventTrait.hpp"
#include "Events.hpp"

namespace Profiler::Events {
    EVENT_STRUCT TscCalibration {
        uint64_t tsc_value;    // RDTSC at calibration point
        uint64_t wall_ns;      // CLOCK_REALTIME ns (cross-node alignment)
        uint64_t monotonic_ns; // CLOCK_MONOTONIC_RAW ns (validation)
        uint64_t tsc_freq_hz;  // TSC frequency in Hz
    };

    EVENT_STRUCT ProcessInfo {
        int32_t rank;       // -1 if unknown
        int32_t local_rank; // -1 if unknown
        int32_t world_size; // -1 if unknown
    };
} // namespace Profiler::Events

// clang-format off
DEFINE_EVENT_TRAIT(Profiler::Events::TscCalibration, Events::EventTypeId::TscCalibration, Events::EventCategory::Meta, "tsc_calibration");
DEFINE_EVENT_TRAIT(Profiler::Events::ProcessInfo, Events::EventTypeId::ProcessInfo, Events::EventCategory::Meta, "process_info");
// clang-format on
