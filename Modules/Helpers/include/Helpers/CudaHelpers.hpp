#pragma once

#include <cstdio>
#include <cstdlib>

#include <cuda.h>
#include <cuda_runtime.h>

#define CU_CHECK(cmd)                                                                                                  \
    do {                                                                                                               \
        CUresult err = cmd;                                                                                            \
        if (err != CUDA_SUCCESS) {                                                                                     \
            const char *str;                                                                                           \
            cuGetErrorString(err, &str);                                                                               \
            std::printf("CU error %s:%d '%s'\n", __FILE__, __LINE__, str);                                             \
            std::exit(EXIT_FAILURE);                                                                                   \
        }                                                                                                              \
    } while (0)

#define CUDA_CHECK(cmd)                                                                                                \
    do {                                                                                                               \
        cudaError_t err = cmd;                                                                                         \
        if (err != cudaSuccess) {                                                                                      \
            std::printf("CUDA error %s:%d '%s'\n", __FILE__, __LINE__, cudaGetErrorString(err));                       \
            std::exit(EXIT_FAILURE);                                                                                   \
        }                                                                                                              \
    } while (0)

#ifdef NVML_API_VERSION
#define NVML_CHECK(cmd)                                                                                                \
    do {                                                                                                               \
        nvmlReturn_t err = cmd;                                                                                        \
        if (err != NVML_SUCCESS) {                                                                                     \
            std::printf("NVML error %s:%d '%s'\n", __FILE__, __LINE__, nvmlErrorString(err));                          \
            std::exit(EXIT_FAILURE);                                                                                   \
        }                                                                                                              \
    } while (0)
#endif

#ifdef NCCL_MINOR
#define NCCL_CHECK(cmd)                                                                                                \
    do {                                                                                                               \
        ncclResult_t res = cmd;                                                                                        \
        if (res != ncclSuccess) {                                                                                      \
            std::printf("NCCL error %s:%d '%s'\n", __FILE__, __LINE__, ncclGetErrorString(res));                       \
            std::exit(EXIT_FAILURE);                                                                                   \
        }                                                                                                              \
    } while (0)

#if NCCL_VERSION_CODE >= NCCL_VERSION(2, 19, 0)
#define NCCL_MALLOC(addr, size) NCCL_CHECK(ncclMemAlloc((addr), (size)))
#else
#define NCCL_MALLOC(addr, size) CUDA_CHECK(cudaMalloc((addr), (size)))
#endif
#endif

#ifdef NVTX_VERSION
const uint32_t colors[] = {0xff00ff00, 0xff0000ff, 0xffffff00, 0xffff00ff, 0xff00ffff, 0xffff0000, 0xffffffff};
const int num_colors = sizeof(colors) / sizeof(uint32_t);

#define PUSH_RANGE(name, cid)                                                                                          \
    {                                                                                                                  \
        int color_id = cid;                                                                                            \
        color_id = color_id % num_colors;                                                                              \
        nvtxEventAttributes_t eventAttrib = {0};                                                                       \
        eventAttrib.version = NVTX_VERSION;                                                                            \
        eventAttrib.size = NVTX_EVENT_ATTRIB_STRUCT_SIZE;                                                              \
        eventAttrib.colorType = NVTX_COLOR_ARGB;                                                                       \
        eventAttrib.color = colors[color_id];                                                                          \
        eventAttrib.messageType = NVTX_MESSAGE_TYPE_ASCII;                                                             \
        eventAttrib.message.ascii = name;                                                                              \
        nvtxRangePushEx(&eventAttrib);                                                                                 \
    }

#define POP_RANGE nvtxRangePop();
#endif

namespace Helpers::Cuda {
    // From cuda samples
    // This subroutine does no real work but runs for at least the specified number
    // of clock ticks.
    __device__ inline void Clock_Block(const clock_t clock_count = 1000000000) {
        const auto start_clock = static_cast<unsigned int>(clock());

        clock_t clock_offset = 0;

        while (clock_offset < clock_count) {
            const auto end_clock = static_cast<unsigned int>(clock());

            // The code below should work like
            // this (thanks to modular arithmetics):
            //
            // clock_offset = (clock_t) (end_clock > start_clock ?
            //                           end_clock - start_clock :
            //                           end_clock + (0xffffffffu - start_clock));
            //
            // Indeed, let m = 2^32 then
            // end - start = end + m - start (mod m).

            clock_offset = static_cast<clock_t>(end_clock - start_clock);
        }
    }
} // namespace Helpers::Cuda
