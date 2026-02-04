#pragma once

#include <ctime>

#include <chrono>
#include <concepts>

namespace Helpers::TS {
    using Timestamp = uint64_t;

    // High-resolution timestamp source concept
    // TODO: correction mechanism
    template<typename T>
    concept TimestampSource = requires(const T source) {
        { source.now() } noexcept -> std::same_as<Timestamp>;
    };

    // Steady clock-based timestamp source
    // Monotonic, portable timing with nanosecond resolution
    class SteadyTimestampSource {
    public:
        using Clock = std::chrono::steady_clock;

        [[nodiscard]] Timestamp now() const noexcept {
            const auto duration = Clock::now().time_since_epoch();
            return static_cast<Timestamp>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
        }
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

    // LFENCE + RDTSC: alternative serialization (sometimes lower latency)
    class RdtscFencedTimestampSource {
    public:
        [[nodiscard]] Timestamp now() const noexcept {
            uint32_t lo, hi;
            __asm__ __volatile__("lfence\n\t"
                                 "rdtsc"
                                 : "=a"(lo), "=d"(hi)
                                 :
                                 : "memory");
            return (static_cast<uint64_t>(hi) << 32) | lo;
        }
    };

    // clock_gettime via vDSO — ~20ns on Linux, gives wall-time correlation
    class GettimeTimestampSource {
    public:
        [[nodiscard]] Timestamp now() const noexcept {
            timespec ts{};
            clock_gettime(CLOCK_MONOTONIC_RAW, &ts); // or CLOCK_MONOTONIC
            return static_cast<Timestamp>(ts.tv_sec) * 1'000'000'000ULL + static_cast<unsigned long long>(ts.tv_nsec);
        }
    };

    // Returns 0 for debugging or ignoring the timestamp
    class FakeTimestampSource {
    public:
        [[nodiscard]] Timestamp now() const noexcept {
            return 0;
        }
    };

    // Verify implementations satisfy the concept
    static_assert(TimestampSource<SteadyTimestampSource>);
    static_assert(TimestampSource<RdtscTimestampSource>);
    static_assert(TimestampSource<RdtscFencedTimestampSource>);
    static_assert(TimestampSource<GettimeTimestampSource>);
    static_assert(TimestampSource<FakeTimestampSource>);
} // namespace Helpers::TS
