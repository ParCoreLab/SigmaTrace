#pragma once

#include <cstdint>

#include <string>

#include "HookTrait.hpp"

namespace cuda_trace {}

// ================================================================
// CUDA Hooks. These aren't really hooks honestly
// ================================================================

// DEFINE_CUSTOM_HOOK(cuda_trace, cuda_trace, int);
DEFINE_CUSTOM_HOOK(cuda_trace, cuda_device_available, std::uint32_t device_id, CUuuid uuid);

// TODO: rename this to cuda_mem_op or something
DEFINE_CUSTOM_HOOK(cuda_trace, cuda_mem_op, int gpu_id, std::uint64_t addr, std::string_view debug_sass);

DEFINE_CUSTOM_HOOK(cuda_trace, cuda_mem_alloc, std::uint64_t, std::size_t);
DEFINE_CUSTOM_HOOK(cuda_trace, cuda_mem_register, std::uint64_t, std::size_t, unsigned int);

DEFINE_CUSTOM_HOOK(cuda_trace, cuda_kernel_launch, std::uint32_t gpu_id, const char* kernel_name);
DEFINE_CUSTOM_HOOK(cuda_trace, cuda_kernel_exit, std::uint32_t gpu_id, const char* kernel_name);

// DEFINE_PRELOAD_HOOK(cuda_trace, cuda_trace, "cuda_trace", int, int);

namespace cuda_trace {}
