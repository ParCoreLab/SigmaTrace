#pragma once

#include <chrono>
#include <cstdint>

namespace Events {
    // Timestamp in nanoseconds since recorder epoch
    using Timestamp = uint64_t;

    // Common header for all events
    struct alignas(8) EventHeader {
        Timestamp timestamp;
    };

    // Storage wrapper that combines header + event data
    // Aligned to 8 bytes for cache-friendly access
    template<typename EventT>
    struct alignas(8) StoredEvent {
        EventHeader header;
        EventT data;

        // Calculate padded size to ensure 8-byte alignment for next event
        static constexpr size_t padded_size = ((sizeof(EventHeader) + sizeof(EventT) + 7) / 8) * 8;
    };

    // High-resolution timestamp source
    // Uses steady_clock for monotonic, portable timing
    // TODO: maybe an interface in the future
    // TODO: correction mechanism
    /*
     * TODO: consider using RDTSC
     * __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
     * return ((uint64_t)hi << 32) | lo;
     */
    class TimestampSource {
    public:
        using Clock = std::chrono::steady_clock;

        TimestampSource() : m_epoch(Clock::now()) {}

        [[nodiscard]] Timestamp now() const {
            const auto duration = Clock::now() - m_epoch;
            return static_cast<Timestamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
        }

    private:
        Clock::time_point m_epoch;
    };

} // namespace Events
