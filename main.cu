#include <cassert>
#include <iostream>
#include <thread>

#include <cuda_runtime.h>
#include <nccl.h>

#include "cuda_helper.hpp"

constexpr size_t DATA_SIZE = 4096;
constexpr int NUM_RANKS = 2;

int main() {
    std::cout << "ok" << std::endl;

    setenv("NCCL_DEBUG", "TRACE", 1);

    setenv("NCCL_IB_HCA", "mlx5_0", 1);
    setenv("NCCL_IB_DISABLE", "0", 1);
    setenv("NCCL_IB_GID_INDEX", "0", 1);
    setenv("NCCL_P2P_DISABLE", "1", 1);
    // setenv("NCCL_P2P_LEVEL", "SYS", 1);
    setenv("NCCL_SHM_DISABLE", "1", 1);
    // setenv("NCCL_NET_PLUGIN", "none", 1);
    setenv("NCCL_SOCKET_NAME", "ibp33s0", 1);
    setenv("NCCL_NET", "IB", 1);

    // Enables GPUDirect RDMA across all dis
    setenv("NCCL_NET_GDR_LEVEL", "SYS", 1);

    setenv("NCCL_NET_SHARED_BUFFERS", "0", 1);
    setenv("NCCL_IGNORE_CPU_AFFINITY", "1", 1);
    setenv("NCCL_CROSS_NIC", "1", 1);
    setenv("NCCL_MIN_NCHANNELS", "1", 1);
    // setenv("NCCL_PLUGIN_P2P", "none", 1);
    // setenv("NCCL_P2P_DIRECT_DISABLE", "1", 1);

    setenv("NCCL_DEBUG_SUBSYS", "INIT,NET", 1);

    setenv("NVSHMEM_NVTX", "common", 1);

    int num_devices = 0;
    cudaGetDeviceCount(&num_devices);
    assert(num_devices >= NUM_RANKS && "Need at least two GPUs");

    cudaDeviceProp prop{};
    CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));

    ncclComm_t nccl_comms[NUM_RANKS];
    constexpr int nccl_devs[NUM_RANKS] = {0, 1};
    NCCL_CHECK(ncclCommInitAll(nccl_comms, NUM_RANKS, nccl_devs));

    // Disable P2P access in case NCCL env doesn't do any
    cudaSetDevice(nccl_devs[0]);
    cudaDeviceDisablePeerAccess(nccl_devs[1]);

    // GPU buffers
    void *send_buffs[NUM_RANKS];
    void *recv_buffs[NUM_RANKS];

    for (auto i = 0; i < NUM_RANKS; i++) {
        ncclMemAlloc(&send_buffs[i], DATA_SIZE * sizeof(float));
        ncclMemAlloc(&recv_buffs[i], DATA_SIZE * sizeof(float));
        NCCL_MALLOC(&send_buffs[i], DATA_SIZE * sizeof(float));
        NCCL_MALLOC(&recv_buffs[i], DATA_SIZE * sizeof(float));
    }

    for (auto _ = 0; _ < 1; _++) {
        NCCL_CHECK(ncclGroupStart());
        for (auto i = 0; i < NUM_RANKS; i++) {
            const auto *send_buff = send_buffs[i];
            auto *recv_buff = recv_buffs[i];
            NCCL_CHECK(ncclAllGather(send_buff, recv_buff, DATA_SIZE, ncclFloat, nccl_comms[i], nullptr));
        }
        NCCL_CHECK(ncclGroupEnd());

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    for (const auto nccl_comm: nccl_comms) {
        ncclCommDestroy(nccl_comm);
    }

    return 0;
}
