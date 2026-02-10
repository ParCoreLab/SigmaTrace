#pragma once

#include <bit>

#define sigma_ignore_unused(expr) (void) (expr)

namespace Helpers {
    static constexpr uint64_t PtrToUint(void* ptr) {
        return std::bit_cast<uint64_t>(ptr);
    }
} // namespace Helpers
