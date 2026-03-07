#pragma once

// Abstract GPU types so the profiler compiles without a GPU backend.
// When CUDA is available, these are the real CUDA types.
// Otherwise, they are opaque stand-ins that preserve struct layout.

namespace Helpers::GPU {};

#ifdef SIGMA_HAS_CUDA
#include <cuda.h>
#include <cuda_runtime.h>

namespace Helpers::GPU {
    using Context = CUcontext;
    using MemoryType = cudaMemoryType;
} // namespace Helpers::GPU

using GpuContext = CUcontext;
using GpuMemoryType = cudaMemoryType;
#elif defined(SIGMA_HAS_HIP)
// TODO: HIP equivalents
#include <hip/hip_runtime.h>
using GpuContext = hipCtx_t;
using GpuMemoryType = hipMemoryType;
#else
using GpuContext = void*;
using GpuMemoryType = int;
#endif
