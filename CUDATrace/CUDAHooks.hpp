#pragma once

#include <cstdint>

#include <string>

#include "HookTrait.hpp"

namespace cuda_trace {}

// ================================================================
// CUDA Hooks. These aren't really hooks honestly
// ================================================================

// DEFINE_CUSTOM_HOOK(cuda_trace, cuda_trace, int);

DEFINE_CUSTOM_HOOK(cuda_trace, cuda_trace, uint64_t, std::string_view);

// DEFINE_PRELOAD_HOOK(cuda_trace, cuda_trace, "cuda_trace", int, int);


namespace cuda_trace {}
