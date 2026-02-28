#include <array>
#include <cassert>
#include <iostream>
#include <sys/wait.h>
#include <thread>
#include <vector>

#include <cuda_runtime.h>
#include <nvml.h>

#include <nvshmem.h>
#include <nvshmemx.h>

#include "Helpers/CudaHelpers.hpp"

constexpr auto NumElements = 8192;
constexpr auto ThreadsPerBlock = 1024;
[[maybe_unused]] constexpr auto NumBlocks = NumElements / ThreadsPerBlock;
static_assert(NumElements % ThreadsPerBlock == 0, "NumElements must be multiple of ThreadsPerBlock");

[[maybe_unused]] constexpr auto NumRepeats = 1000;
[[maybe_unused]] constexpr auto ReferenceTimeNs = 31262269; // nanoseconds; it was revealed to me in a dream

__global__ void set_and_shift_kernel(float* send_data, float* recv_data, int num_elems, int mype, int npes) {
    int thread_idx = blockIdx.x * blockDim.x + threadIdx.x;
    /* set the corresponding element of send_data */
    if (thread_idx < num_elems) {
        send_data[thread_idx] = mype;
    }

    // asm volatile("st.release.gpu.global.L1::no_allocate.b32 [%0], %1;" : : "l"((uint32_t*)0), "r"(0));
    // asm volatile("st.release.gpu.global.L1::no_allocate.b64 [%0], %1;" : : "l"((uint64_t*)0), "l"((uint64_t)0));

    // for (int i = 0; i < 100000; i++) {
    int peer = (mype + 1) % npes;
    /* Every thread in block 0 calls nvshmemx_float_put_block. Alternatively,
       every thread can call shmem_float_p, but shmem_float_p has a disadvantage
       that when the destination GPU is connected via IB, there will be one rma
       message for every single element which can be detrimental to performance.
       And the disadvantage with shmem_float_put is that when the destination GPU is p2p
       connected, it cannot leverage multiple threads to copy the data to the destination
       GPU. */
    int block_offset = blockIdx.x * blockDim.x;

    // All threads in a block call the API with the same arguments
    auto* dest = recv_data + block_offset;
    const auto* src = send_data + block_offset;
    nvshmemx_float_put_block(dest, src, min(blockDim.x, num_elems - block_offset), peer);
    // }
}

__global__ void simple_shift(int* destination) {
    int mype = nvshmem_my_pe();
    int npes = nvshmem_n_pes();
    int peer = (mype + 1) % npes;

    nvshmem_int_p(destination, mype, peer);
}

// NVSHMEM program start point
void program(nvshmemx_uniqueid_t id, const int device_id, const int num_devices) {
    // ================================================================
    // NVSHMEM Initialization
    // ================================================================
    std::cout << "=== Initializing " << device_id << std::endl;

    CUDA_CHECK(cudaSetDevice(device_id));

    nvshmemx_init_attr_t attr = NVSHMEMX_INIT_ATTR_INITIALIZER;
    nvshmemx_set_attr_uniqueid_args(device_id, num_devices, &id, &attr);
    nvshmemx_init_attr(NVSHMEMX_INIT_WITH_UNIQUEID, &attr);

    std::cout << "=== Initialized " << device_id << std::endl;

    const int my_pe = nvshmem_my_pe();
    const int n_pes = nvshmem_n_pes();

    std::cout << "=== Hello from PE " << my_pe << " of " << n_pes << std::endl;

    nvshmem_barrier_all();
    // ================================================================

    // ================================================================
    // Small debug kernel
    int* destination = static_cast<int*>(nvshmem_malloc(sizeof(int)));

    simple_shift<<<1, 1, 0>>>(destination);
    CUDA_CHECK(cudaDeviceSynchronize());
    nvshmem_barrier_all();
    // ================================================================

    // /*
    // ================================================================
    // Kernel prep and launch
    // ================================================================
    auto* send_data = static_cast<float*>(nvshmem_malloc(sizeof(float) * NumElements));
    auto* recv_data = static_cast<float*>(nvshmem_malloc(sizeof(float) * NumElements));
    assert(send_data != nullptr && recv_data != nullptr);

    // Warmup
    for (auto _ = 0; _ < NumRepeats; ++_) {
        set_and_shift_kernel<<<NumBlocks, ThreadsPerBlock>>>(send_data, recv_data, NumElements, my_pe, n_pes);
    }

    CUDA_CHECK(cudaDeviceSynchronize());
    nvshmem_barrier_all();

    const auto start = std::chrono::high_resolution_clock::now();
    for (auto _ = 0; _ < NumRepeats; ++_) {
        set_and_shift_kernel<<<NumBlocks, ThreadsPerBlock>>>(send_data, recv_data, NumElements, my_pe, n_pes);
    }
    CUDA_CHECK(cudaDeviceSynchronize());
    nvshmem_barrier_all();
    const auto end = std::chrono::high_resolution_clock::now();

    const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    const auto performance_delta =
            static_cast<double>(duration - ReferenceTimeNs) / static_cast<double>(ReferenceTimeNs) * 100.0;

    std::cout << "=== PE " << my_pe << " took " << duration << " ns ";
    std::cout << "(" << std::showpos << (duration - ReferenceTimeNs) << "ns/" << performance_delta << "%)" << std::endl;

    nvshmem_barrier_all();

    // Data validation
    std::array<float, NumElements> recv_data_host{};
    CUDA_CHECK(cudaMemcpy(recv_data_host.data(), recv_data, NumElements * sizeof(float), cudaMemcpyDefault));
    const auto ref = (my_pe - 1 + n_pes) % n_pes;
    for (auto i = 0; i < NumElements; ++i) {
        assert(recv_data_host[i] == ref && "Host data validation failed");
    }

    nvshmem_free(send_data);
    nvshmem_free(recv_data);
    */

    nvshmem_finalize();

    std::cout << "Goodbye from PE " << my_pe << std::endl;
}

int main(int argc, char* argv[]) {
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

    // This is the most important flag. Default is ibrc which uses host-deside asdasA
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
    setenv("NVSHMEM_IB_DISABLE_DMABUF", "1", 1);

    // TODO: Set to enable multiple NICs per PE if available.
    // setenv("NVSHMEM_IBGDA_ENABLE_MULTI_PORT", "1", 1);

    setenv("NVSHMEM_NVTX", "all", 1);
    // ================================================================

    // TODO: move to a helper module
    // Can't initialize cuda here if we're forking
    NVML_CHECK(nvmlInit());

    unsigned int available_devices{};
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

    // TODO: is this ok? The process calling this won't be initializing nvshmem
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
