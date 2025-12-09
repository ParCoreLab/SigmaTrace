#pragma once

#include <cstdio>
#include <cstdlib>

// TODO: maybe don't include these
#include <cuda_runtime.h>
#include <nccl.h>

#include "nvtx3/nvToolsExt.h"

#define CUDA_CHECK(cmd)                                                                                                \
    do {                                                                                                               \
        cudaError_t err = cmd;                                                                                         \
        if (err != cudaSuccess) {                                                                                      \
            std::printf("CUDA error %s:%d '%s'\n", __FILE__, __LINE__, cudaGetErrorString(err));                       \
            std::exit(EXIT_FAILURE);                                                                                   \
        }                                                                                                              \
    } while (0)

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
