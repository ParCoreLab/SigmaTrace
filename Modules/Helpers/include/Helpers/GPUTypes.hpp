#pragma once

// Abstract GPU types so the profiler compiles without a GPU backend.
// When CUDA is available, these are the real CUDA types.
// Otherwise, they are opaque stand-ins that preserve struct layout.
namespace Helpers::GPU {}

#if SIGMA_HAS_CUDA
#include <cuda.h>
#include <cuda_runtime.h>

namespace Helpers::GPU {
    using Context = CUcontext;
    using MemoryType = cudaMemoryType;
} // namespace Helpers::GPU

#elif SIGMA_HAS_HIP
// TODO: HIP equivalents
#include <hip/hip_runtime.h>
namespace Helpers::GPU {
    using Context = hipCtx_t;
    using MemoryType = hipMemoryType;
} // namespace Helpers::GPU
#else
// Just in case
namespace Helpers::GPU {
    using Context = void*;
    using MemoryType = int;
} // namespace Helpers::GPU
#endif
