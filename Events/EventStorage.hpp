#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>

#include "Helpers/Timestamp.hpp"

#include "Events.hpp"

namespace Events {
    // Timestamp in nanoseconds since recorder epoch
    using Helpers::TS::Timestamp;
    static_assert(std::convertible_to<Timestamp, std::uint64_t>);

    // Common header for all events
    struct alignas(8) EventHeader {
        Timestamp timestamp_start;
        Timestamp timestamp_end;
        // TODO: Might be too big...
        uint64_t _check{MAGIC_VALUE};

        // Constructor is here to ensure we don't accidentally initialize a partial header
        EventHeader() = delete;
        explicit EventHeader(const Timestamp ts_start, const Timestamp ts_end) :
            timestamp_start(ts_start), timestamp_end(ts_end) {}
    };

    // Storage wrapper that combines header + event data
    // Aligned to 8 bytes for cache-friendly access
    template<typename EventT>
    struct alignas(8) StoredEvent {
        EventHeader header{0, 0};
        EventT data;

        // Calculate padded size to ensure 8-byte alignment for next event
        static constexpr size_t padded_size = ((sizeof(EventHeader) + sizeof(EventT) + 7) / 8) * 8;
    };
} // namespace Events
