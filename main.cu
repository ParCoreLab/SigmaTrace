#include <cassert>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

#include <cuda_runtime.h>
#include <nccl.h>

#include "Helpers/CudaHelpers.hpp"

constexpr size_t DATA_SIZE = 4096;

__global__ void dummy_kernel(void* data, const size_t size) {
    const size_t idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < size) {
        static_cast<float*>(data)[idx] = static_cast<float>(idx);
    }

    Helpers::Cuda::Clock_Block();
}

int main(int argc, char* argv[]) {
    // setenv("NCCL_DEBUG", "TRACE", 1);

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

    setenv("NCCL_NVTX", "common", 1);

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
    std::vector<void*> send_buffs(num_devices);
    std::vector<void*> recv_buffs(num_devices);

    auto memory_type_to_string = [](cudaMemoryType type) -> const char* {
        switch (type) {
            case cudaMemoryTypeUnregistered:
                return "cudaMemoryTypeUnregistered";
            case cudaMemoryTypeHost:
                return "cudaMemoryTypeHost";
            case cudaMemoryTypeDevice:
                return "cudaMemoryTypeDevice";
            case cudaMemoryTypeManaged:
                return "cudaMemoryTypeManaged";
            default:
                return "Unknown";
        }
    };

    for (int i = 0; i < num_devices; i++) {
        auto send_buff = &send_buffs[i];
        auto recv_buff = &recv_buffs[i];

        NCCL_MALLOC(send_buff, DATA_SIZE * sizeof(float));
        NCCL_MALLOC(recv_buff, DATA_SIZE * sizeof(float));

        cudaPointerAttributes send_attrs{};
        cudaPointerAttributes recv_attrs{};
        CUDA_CHECK(cudaPointerGetAttributes(&send_attrs, *send_buff));
        CUDA_CHECK(cudaPointerGetAttributes(&recv_attrs, *recv_buff));

        std::cout << "Send buffer on device " << i << ": " << memory_type_to_string(send_attrs.type) << std::endl;
        std::cout << "Recv buffer on device " << i << ": " << memory_type_to_string(recv_attrs.type) << std::endl;
    }

    for (auto _ = 0; _ < 1; _++) {
        NCCL_CHECK(ncclGroupStart());
        for (int i = 0; i < num_devices; i++) {
            const auto* send_buff = send_buffs[i];
            auto* recv_buff = recv_buffs[i];
            NCCL_CHECK(ncclAllGather(send_buff, recv_buff, DATA_SIZE, ncclFloat, nccl_comms[i], nullptr));
        }
        NCCL_CHECK(ncclGroupEnd());

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "Launching dummy kernel" << std::endl;
    for (int i = 0; i < num_devices; i++) {
        cudaSetDevice(i);
        CUDA_CHECK(cudaDeviceSynchronize());
        dummy_kernel<<<(DATA_SIZE + 255) / 256, 256>>>(send_buffs[i], DATA_SIZE);
        CUDA_CHECK(cudaDeviceSynchronize());
    }

    for (const auto nccl_comm: nccl_comms) {
        ncclCommDestroy(nccl_comm);
    }

    std::cout << "Finished" << std::endl;

    return 0;
}
