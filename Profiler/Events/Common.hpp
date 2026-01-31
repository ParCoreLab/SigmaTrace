#pragma once

#include <cstdint>

namespace Profiler::Events {
    // TODO: can be -1
    // TODO: should be int32_t. Make sure all ids everywhere are int32_t
    using GpuId_T = std::int16_t;
    using GpuId_T = std::int16_t;
    constexpr GpuId_T UnknownDeviceId{-1};
} // namespace Profiler::Events
