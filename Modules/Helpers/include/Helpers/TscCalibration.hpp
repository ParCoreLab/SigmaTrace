#pragma once

#include <ctime>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

#include <cpuid.h>

#include "Timestamp.hpp"

namespace Helpers::TS {
    struct TscSnapshot {
        uint64_t tsc_value;
        uint64_t wall_ns;      // CLOCK_REALTIME
        uint64_t monotonic_ns; // CLOCK_MONOTONIC_RAW
        uint64_t tsc_freq_hz;
    };

    // Read TSC frequency from sysfs (Linux-specific)
    inline uint64_t read_tsc_freq_hz() {
        static constexpr auto tsc_freq_path{"/sys/devices/system/cpu/cpu0/tsc_freq_khz"};
        std::ifstream ifs(tsc_freq_path);
        if (!ifs.is_open()) {
            return 0;
        }

        uint64_t khz = 0;
        if (ifs >> khz) {
            return khz * 1000ULL;
        }

        return 0;
    }

    // Measure TSC frequency by busy-waiting ~100ms against CLOCK_MONOTONIC_RAW
    inline uint64_t measure_tsc_freq_hz() {
        static constexpr uint64_t MEASURE_NS = 100'000'000ULL; // 100ms

        timespec ts_start{}, ts_end{};
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts_start);
        const uint64_t tsc_start = RdtscTimestampSource::Now();

        // Busy-wait
        uint64_t elapsed_ns = 0;
        do {
            clock_gettime(CLOCK_MONOTONIC_RAW, &ts_end);
            elapsed_ns = static_cast<uint64_t>(ts_end.tv_sec - ts_start.tv_sec) * 1'000'000'000ULL +
                         static_cast<uint64_t>(ts_end.tv_nsec - ts_start.tv_nsec);
        } while (elapsed_ns < MEASURE_NS);

        const uint64_t tsc_end = RdtscTimestampSource::Now();
        const uint64_t tsc_delta = tsc_end - tsc_start;

        return tsc_delta * 1'000'000'000ULL / elapsed_ns;
    }

    // Get TSC frequency, preferring sysfs, falling back to measurement
    inline uint64_t get_tsc_freq_hz() {
        uint64_t freq = 0; // qread_tsc_freq_hz();
        if (freq == 0) {
            freq = measure_tsc_freq_hz();
        }
        return freq;
    }

    // Capture a synchronized snapshot of TSC, wall clock, and monotonic clock
    inline TscSnapshot capture_tsc_snapshot() {
        const uint64_t freq = get_tsc_freq_hz();

        // Bracket the RDTSC with clock_gettime calls for better accuracy
        timespec ts_wall{}, ts_mono{};
        clock_gettime(CLOCK_REALTIME, &ts_wall);
        const uint64_t tsc = RdtscTimestampSource::Now();
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts_mono);

        const TscSnapshot snapshot{
                .tsc_value = tsc,
                .wall_ns = static_cast<uint64_t>(ts_wall.tv_sec) * 1'000'000'000ULL +
                           static_cast<uint64_t>(ts_wall.tv_nsec),
                .monotonic_ns = static_cast<uint64_t>(ts_mono.tv_sec) * 1'000'000'000ULL +
                                static_cast<uint64_t>(ts_mono.tv_nsec),
                .tsc_freq_hz = freq,
        };
        return snapshot;
    }

    // Check for invariant TSC support (constant_tsc + nonstop_tsc)
    inline bool check_invariant_tsc_fs() {
        std::ifstream cpuinfo("/proc/cpuinfo");
        if (!cpuinfo.is_open()) {
            return false;
        }

        bool has_constant_tsc = false;
        bool has_nonstop_tsc = false;

        std::string line;
        while (std::getline(cpuinfo, line)) {
            if (line.find("flags") == std::string::npos) {
                continue;
            }
            has_constant_tsc = line.find("constant_tsc") != std::string::npos;
            has_nonstop_tsc = line.find("nonstop_tsc") != std::string::npos;
            break;
        }

        return has_constant_tsc && has_nonstop_tsc;
    }

    // Check for invariant TSC support (constant_tsc + nonstop_tsc)
    inline bool check_invariant_tsc() {
        // https://github.com/tycho/cpuid/blob/7fc731f21b9a7c1a3688e7d04ac3c61f88ce7766/feature.c#L1032
        if (__get_cpuid_max(0x80000000, nullptr) < 0x80000001) {
            return false;
        }

        unsigned eax, ebx, ecx, edx;
        __cpuid(0x80000007, eax, ebx, ecx, edx);

        return (edx & (1 << 8)) != 0;
    }

} // namespace Helpers::TS
