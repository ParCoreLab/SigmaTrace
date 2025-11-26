#include <array>
#include <cassert>
#include <iostream>

#include <cuda_runtime.h>

#include <nccl.h>

#define CUDA_CHECK(cmd) do {                        \
    cudaError_t err = cmd;                          \
    if (err != cudaSuccess) {                       \
        printf("CUDA error %s:%d '%s'\n",           \
        __FILE__,__LINE__,cudaGetErrorString(err)); \
        exit(EXIT_FAILURE);                         \
    }                                               \
} while(0)

#define NCCL_CHECK(cmd) do {                         \
    ncclResult_t res = cmd;                         \
    if (res != ncclSuccess) {                       \
        printf("NCCL error %s:%d '%s'\n",           \
        __FILE__,__LINE__,ncclGetErrorString(res)); \
        exit(EXIT_FAILURE);                         \
    }                                               \
} while(0)

constexpr size_t DATA_SIZE = 4096;

int main() {
    std::cout << "ok" << std::endl;

    setenv("NCCL_DEBUG", "INFO", 1);

    int num_devices = 0;
    cudaGetDeviceCount(&num_devices);
    assert(num_devices >= 2 && "Need at least two GPUs");

    cudaDeviceProp prop{};
    CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));

    // Allocate and initialize data on each GPU
    std::array<std::array<float, DATA_SIZE>, 2> send_buffs{};

    for (auto &send_buff: send_buffs) {
        for (auto &elem: send_buff) {
            elem = 1.0f;
        }
    }

    ncclComm_t nccl_comms[2];
    constexpr int nccl_devs[] = {0, 1};
    NCCL_CHECK(ncclCommInitAll(nccl_comms, 2, nccl_devs));

    for (const auto nccl_comm: nccl_comms) {
        ncclCommDestroy(nccl_comm);
    }

    return 0;
}
