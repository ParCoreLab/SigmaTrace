#include <cpuid.h>

#include <immintrin.h>
#include <x86intrin.h>

#include <limits>

#include <iostream>

#include "MemoryRepeater.hpp"

namespace Profiler::MemoryRepeater {
    using WaitFn = void (*)(void*);

    static bool internal_has_mwaitx() noexcept {
        unsigned eax, ebx, ecx, edx;
        if (__get_cpuid_max(0x80000000, nullptr) < 0x80000001) {
            return false;
        }

        __cpuid(0x80000001, eax, ebx, ecx, edx);
        // https://github.com/tycho/cpuid/blob/7fc731f21b9a7c1a3688e7d04ac3c61f88ce7766/feature.c#L1012
        return ecx & (0x20000000u);
    }

    static bool internal_has_waitpkg() noexcept {
        unsigned eax, ebx, ecx, edx;
        if (__get_cpuid_max(0, nullptr) < 7) {
            return false;
        }

        __cpuid_count(7, 0, eax, ebx, ecx, edx);
        return ecx & (0x00000020u);
    }

    __attribute__((target("mwaitx"))) static void wait_mem_mwaitx(void* addr) noexcept {
        _mm_monitorx(addr, 0, 0);
        _mm_mwaitx(0, 0, std::numeric_limits<unsigned int>::max());
    }

    __attribute__((target("waitpkg"))) static void wait_mem_waitpkg(void* addr) noexcept {
        _umonitor(addr);
        // state: 0 = C0.2 (slower wakeup, lower power)
        //        1 = C0.1 (faster wakeup, higher power)
        _umwait(0, std::numeric_limits<unsigned int>::max());
    }

    static WaitFn select_wait_fn() noexcept {
        if (internal_has_mwaitx()) {
            return wait_mem_mwaitx;
        }

        if (internal_has_waitpkg()) {
            return wait_mem_waitpkg;
        }

        return [](void*) noexcept {
            _mm_pause();
        };
    }

    void wait_mem(void* addr) noexcept {
        static const WaitFn fn = select_wait_fn();
        fn(addr);
    }
} // namespace Profiler::MemoryRepeater
