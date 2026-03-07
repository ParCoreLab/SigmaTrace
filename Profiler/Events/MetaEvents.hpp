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
} // namespace Profiler::Events

// clang-format off
DEFINE_EVENT_TRAIT(Profiler::Events::TscCalibration, Events::EventTypeId::TscCalibration, Events::EventCategory::Meta, "tsc_calibration");
// clang-format on
