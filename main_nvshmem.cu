#include <cassert>
#include <sys/wait.h>
#include <iostream>
#include <thread>
#include <vector>

#include <cuda_runtime.h>
#include <nvml.h>

#include <nvshmem.h>
#include <nvshmemx.h>

#include "cuda_helper.hpp"

__global__ void simple_shift(int *destination) {
    int mype = nvshmem_my_pe();
    int npes = nvshmem_n_pes();
    int peer = (mype + 1) % npes;

    nvshmem_int_p(destination, mype, peer);
}

void program(nvshmemx_uniqueid_t id, const int device_id, const int num_devices) {
    std::cout << "=== Initializing " << device_id << std::endl;

    CUDA_CHECK(cudaSetDevice(device_id));

    nvshmemx_init_attr_t attr = NVSHMEMX_INIT_ATTR_INITIALIZER;
    nvshmemx_set_attr_uniqueid_args(device_id, num_devices, &id, &attr);
    nvshmemx_init_attr(NVSHMEMX_INIT_WITH_UNIQUEID, &attr);

    std::cout << "=== Initialized " << device_id << std::endl;

    int my_pe = nvshmem_my_pe();
    int n_pes = nvshmem_n_pes();

    std::cout << "=== Hello from PE " << my_pe << " of " << n_pes << std::endl;

    nvshmem_barrier_all();

    auto *destination = static_cast<int *>(nvshmem_malloc(sizeof(int)));
    simple_shift<<<1, 1, 0, nullptr>>>(destination);
    nvshmem_barrier_all();

    int msg;
    CUDA_CHECK(cudaMemcpyAsync(&msg, destination, sizeof(int), cudaMemcpyDeviceToHost, nullptr));

    std::cout << "=== PE " << my_pe << " received " << msg << std::endl;

    nvshmem_finalize();
}

int main(int argc, char *argv[]) {
    // ================================================================
    // Env for forcing ibv
    // ================================================================
    // setenv("NVSHMEM_DEBUG", "TRACE", 1);

    // TODO: probably wrong
    // setenv("NVSHMEM_BOOTSTRAP_UID_SOCK_IFNAME", "ibp33s0", 1);

    setenv("NVSHMEM_DISABLE_P2P", "1", 1);

    // TODO: maybe later
    // setenv("NVSHMEM_DISABLE_NVLS", "1", 1);

    setenv("NVSHMEM_DISABLE_NCCL", "1", 1);
    setenv("NVSHMEM_REMOTE_TRANSPORT", "ibdevx", 1);

    // TODO: maybe later
    // setenv("NVSHMEM_DISABLE_GDRCOPY", "1", 1);

    // TODO: future
    // setenv("NVSHMEM_ENABLE_NIC_PE_MAPPING", "0", 1);

    // Probably not needed
    // setenv("NVSHMEM_HCA_LIST", "mlx5_0", 1);

    // TODO: future
    // setenv("NVSHMEM_HCA_PE_MAPPING", "mlx5_0:1:2", 1);

    /* When running on an NVLink-only configuaration (No-IB, No-UCX),
     * completely disable the proxy thread. This will disable device
     * side global exit and device side wait timeout polling
     * (enabled by NVSHMEM_TIMEOUT_DEVICE_POLLING build-time variable)
     * because these are processed by the proxy thread. */
    // setenv("NVSHMEM_DISABLE_LOCAL_ONLY_PROXY", "1", 1);

    // TODO: maybe later
    // setenv("NVSHMEM_IBGDA_NUM_DCI", "1", 1);
    // setenv("NVSHMEM_IBGDA_NUM_SHARED_DCI", "1", 1);
    // setenv("NVSHMEM_IBGDA_DCI_MAP_BY", "sm", 1);
    // setenv("NVSHMEM_IBGDA_NUM_RC_PER_PE", "1", 1);

    // TODO: Force NIC buffer memory type
    // setenv("NVSHMEM_IBGDA_FORCE_NIC_BUF_MEMTYPE", "gpumem", 1);

    // TODO: Number of requests to be batched before submitting to the NIC
    // setenv("NVSHMEM_IBGDA_NUM_REQUESTS_IN_BATCH", "32", 1);

    // Enable GPU NIC doorbell stuff
    setenv("NVSHMEM_IB_ENABLE_IBGDA", "1", 1);

    // Selects the processor used for ringing NIC’s doorbell. Choices are auto, gpu, cpu.
    setenv("NVSHMEM_IBGDA_NIC_HANDLER", "gpu", 1);

    // TODO: Set to disable DMAbuf in any IB based remote transport.
    // Logs say: The system does not support registering the NIC control buffers with DMABUF.
    // Fallback to use either nv_peer_mem or nvidia_peermem
    // setenv("NVSHMEM_IB_DISABLE_DMABUF", "1", 1);

    // TODO: Set to enable multiple NICs per PE if available.
    // setenv("NVSHMEM_IBGDA_ENABLE_MULTI_PORT", "1", 1);

    setenv("NVSHMEM_NVTX", "all", 1);
    // ================================================================

    // TODO: move to a helper module
    // Can't initialize cuda here if we're forking
    NVML_CHECK(nvmlInit());

    unsigned int available_devices;
    NVML_CHECK(nvmlDeviceGetCount(&available_devices));

    NVML_CHECK(nvmlShutdown());

    assert(available_devices >= 1 && "Need at least one GPU");

    int num_devices = static_cast<int>(available_devices);
    if (argc > 1) {
        num_devices = std::atoi(argv[1]);
        if (num_devices < 1 || num_devices > available_devices) {
            std::cerr << "Invalid number of GPUs: " << num_devices << " (available: " << available_devices << ")"
                      << std::endl;
            return 1;
        }
    }

    std::cout << "Using " << num_devices << " of " << available_devices << " GPUs" << std::endl;

    // TODO: is this ok? The process calling this won't be initialiedzedinitializing nvshmeminitializing nvshmem
    nvshmemx_uniqueid_t id = NVSHMEMX_UNIQUEID_INITIALIZER;
    nvshmemx_get_uniqueid(&id);

    // Forking a child for each PE might break NVSHMEM
    // Instead, the parent process becomes PE0 using GPU0
    const auto num_children = num_devices - 1;

    std::vector<pid_t> processes;
    processes.reserve(num_children);

    for (auto i = 0; i < num_devices - 1; i++) {
        auto pid = fork();
        if (pid == 0) {
            // GPU 0 is the parent process, so we do (i + 1)
            program(id, i + 1, num_devices);
            return 0;
        } else {
            processes.push_back(pid);
        }
    }

    program(id, 0, num_devices);

    wait(nullptr);

    return 0;
}
