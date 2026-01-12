#pragma once

#include <chrono>
#include <concepts>
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

    // High-resolution timestamp source concept
    // Zero-overhead compile-time polymorphism using C++20 concepts
    // TODO: correction mechanism
    template<typename T>
    concept TimestampSource = requires(const T source) {
        { source.now() } noexcept -> std::same_as<Timestamp>;
    };

    // Steady clock-based timestamp source
    // Monotonic, portable timing with nanosecond resolution
    class ChronoTimestampSource {
    public:
        using Clock = std::chrono::steady_clock;

        ChronoTimestampSource() noexcept : m_epoch(Clock::now()) {}

        [[nodiscard]] Timestamp now() const noexcept {
            const auto duration = Clock::now() - m_epoch;
            return static_cast<Timestamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
        }

    private:
        Clock::time_point m_epoch;
    };

    // RDTSC-based timestamp source for minimal latency
    // x86-64 only - measures CPU cycles, not wall time
    // WARNING: Not portable, affected by frequency scaling
    class RdtscTimestampSource {
    public:
        [[nodiscard]] Timestamp now() const noexcept {
            uint32_t lo, hi;
            __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
            return (static_cast<uint64_t>(hi) << 32) | lo;
        }
    };

    // Returns 0 for debugging convenience
    class FakeTimestampSource {
    public:
        [[nodiscard]] Timestamp now() const noexcept { return 0; }
    };

    // Verify implementations satisfy the concept
    static_assert(TimestampSource<ChronoTimestampSource>);
    static_assert(TimestampSource<RdtscTimestampSource>);
    static_assert(TimestampSource<FakeTimestampSource>);

} // namespace Events
