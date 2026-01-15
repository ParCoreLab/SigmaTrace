#pragma once

#include <cstdint>

#include <string>

#include "HookTrait.hpp"

namespace cuda_trace {}

// ================================================================
// CUDA Hooks. These aren't really hooks honestly
// ================================================================

// DEFINE_CUSTOM_HOOK(cuda_trace, cuda_trace, int);

// TODO: rename this to cuda_mem_op or something
DEFINE_CUSTOM_HOOK(cuda_trace, cuda_mem_op, int gpu_id, std::uint64_t addr, std::string_view debug_sass);

DEFINE_CUSTOM_HOOK(cuda_trace, cuda_kernel_launch, std::int32_t gpu_id, const char *);
DEFINE_CUSTOM_HOOK(cuda_trace, cuda_kernel_exit, std::int32_t gpu_id, const char *);

// DEFINE_PRELOAD_HOOK(cuda_trace, cuda_trace, "cuda_trace", int, int);


namespace cuda_trace {}
