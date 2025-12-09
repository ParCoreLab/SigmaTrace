#include <cassert>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

#include <cuda_runtime.h>
#include <nccl.h>

#include "cuda_helper.hpp"

constexpr size_t DATA_SIZE = 4096;

int main(int argc, char *argv[]) {
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

    int available_devices = 0;
    CUDA_CHECK(cudaGetDeviceCount(&available_devices));
    assert(available_devices >= 1 && "Need at least one GPU");

    int num_devices = available_devices;
    if (argc > 1) {
        num_devices = std::atoi(argv[1]);
        if (num_devices < 1 || num_devices > available_devices) {
            std::cerr << "Invalid number of GPUs: " << num_devices << " (available: " << available_devices << ")"
                      << std::endl;
            return 1;
        }
    }

    std::cout << "Using " << num_devices << " of " << available_devices << " GPUs" << std::endl;

    cudaDeviceProp prop{};
    CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));

    std::vector<ncclComm_t> nccl_comms(num_devices);
    std::vector<int> nccl_devs(num_devices);
    for (int i = 0; i < num_devices; i++) {
        nccl_devs[i] = i;
    }
    NCCL_CHECK(ncclCommInitAll(nccl_comms.data(), num_devices, nccl_devs.data()));

    // GPU buffers
    std::vector<void *> send_buffs(num_devices);
    std::vector<void *> recv_buffs(num_devices);

    for (int i = 0; i < num_devices; i++) {
        NCCL_MALLOC(&send_buffs[i], DATA_SIZE * sizeof(float));
        NCCL_MALLOC(&recv_buffs[i], DATA_SIZE * sizeof(float));
    }

    for (auto _ = 0; _ < 1; _++) {
        NCCL_CHECK(ncclGroupStart());
        for (int i = 0; i < num_devices; i++) {
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
