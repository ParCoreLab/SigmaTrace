#include <cassert>
#include <iostream>

#include <cuda_runtime.h>

#include <nvshmem.h>
#include <nvshmemx.h>

#include "cuda_helper.hpp"

constexpr size_t DATA_SIZE = 4096;

__global__ void ring_bcast(int *data, size_t nelem, int root, uint64_t *psync) {
    int mype = nvshmem_my_pe();
    int npes = nvshmem_n_pes();
    int peer = (mype + 1) % npes;

    if (mype == root)
        *psync = 1;

    nvshmem_signal_wait_until(psync, NVSHMEM_CMP_NE, 0);

    if (mype == npes - 1)
        return;

    nvshmem_int_put(data, data, nelem, peer);
    nvshmem_fence();
    nvshmemx_signal_op(psync, 1, NVSHMEM_SIGNAL_SET, peer);

    *psync = 0;
}

int main(int argc, char *argv[]) {
    setenv("NVSHMEM_DEBUG", "TRACE", 1);

    // TODO: move to a helper module
    int available_devices = 0;
    // CUDA_CHECK(cudaGetDeviceCount(&available_devices));
    cudaGetDeviceCount(&available_devices);
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

    nvshmem_init();

    int my_pe = nvshmem_my_pe();
    int n_pes = nvshmem_n_pes();

    std::cout << "Hello from PE " << my_pe << " of " << n_pes << std::endl;

    return 0;
}
