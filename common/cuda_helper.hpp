#pragma once

#include <cstdio>
#include <cstdlib>

// TODO: maybe don't include these
#include <cuda_runtime.h>
#include <nccl.h>

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
