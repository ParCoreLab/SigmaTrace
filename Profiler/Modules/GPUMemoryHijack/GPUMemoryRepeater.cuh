#pragma once

#include <cassert>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <vector>

#include <cuda.h>
#include <cuda_runtime.h>

#include "ExportTableOverwrite.hpp"
#include "Helpers/CudaHelpers.hpp"

namespace Profiler::GpuRepeater {
    namespace detail {
        cudaError_t launch_kernel(dim3 gridDim, dim3 blockDim, void** args, size_t sharedMem, cudaStream_t stream);
        CUresult launch_test_kernel(cudaStream_t stream);
    } // namespace detail

    // ============================================================
    // Shared structures (host + device)
    // ============================================================

    struct GpuWqeRecord {
        uint64_t timestamp; // clock64() on GPU
        uint64_t raddr;     // remote address (RDMA ops)
        uint64_t laddr;     // local address
        uint32_t qpn;
        uint32_t rkey;
        uint32_t lkey;
        uint32_t byte_count;
        uint16_t wqe_idx;
        uint16_t doorbell_prod_idx;
        uint8_t opcode;
        uint8_t ds;
        int16_t gpu_id;
    };
    static_assert(sizeof(GpuWqeRecord) == 48, "GpuWqeRecord packing unexpected");

    struct GpuUarEntry {
        uint64_t* fake_addr; // cudaMalloc'd GPU memory (NVSHMEM writes here)
        uint64_t* real_addr; // NIC BAR device pointer (kernel forwards here)
    };

    struct GpuQpEntry {
        void* sq_base; // device pointer to SQ buffer in GPU HBM
        uint32_t qpn;
        uint32_t log_wq_size;
        int qp_type; // 0=RC, 1=DCI
        int gpu_id;
    };

    // Pinned host memory, accessible from both CPU and GPU.
    // Host writes UAR/QP tables, GPU reads them.
    // GPU writes result ring, host reads it.
    struct GpuRepeaterChannel {
        static constexpr int MAX_UARS = 64;
        static constexpr int MAX_QPS = 256;
        static constexpr uint32_t RESULT_RING_SIZE = 16384;
        static constexpr uint32_t RESULT_RING_MASK = RESULT_RING_SIZE - 1;
        static_assert((RESULT_RING_SIZE & RESULT_RING_MASK) == 0);

        // --- UAR table: host writes, GPU reads ---
        GpuUarEntry uars[MAX_UARS];
        alignas(64) volatile int num_uars;

        // --- QP table: host writes, GPU reads ---
        GpuQpEntry qps[MAX_QPS];
        alignas(64) volatile int num_qps;

        // --- Result ring: GPU writes, host reads ---
        // Not volatile — synchronized via head/tail with fences.
        GpuWqeRecord result_ring[RESULT_RING_SIZE];
        alignas(64) volatile uint32_t result_head; // GPU producer
        alignas(64) volatile uint32_t result_tail; // CPU consumer

        // --- Control ---
        alignas(64) volatile int shutdown;
    };

    // ============================================================
    // mlx5 constants (duplicated here to avoid header dependency)
    // ============================================================
    namespace mlx5_opcodes {
        constexpr uint8_t SEND = 0x0a;
        constexpr uint8_t SEND_IMM = 0x0b;
        constexpr uint8_t RDMA_WRITE = 0x08;
        constexpr uint8_t RDMA_READ = 0x10;
        constexpr uint8_t ATOMIC_CS = 0x11;
        constexpr uint8_t ATOMIC_FA = 0x12;
    } // namespace mlx5_opcodes

    constexpr int QP_TYPE_DCI = 1;
    constexpr size_t WQEBB_SIZE = 64;

    // ================================================================
    // Kernels
    // ================================================================
    namespace detail {
        __global__ void test_kernel();
        __global__ void gpu_doorbell_intercept_kernel();
        __global__ void parse_and_record_oneshot(volatile void* sq_base, uint32_t qpn, uint32_t log_wq_size,
                                                 int qp_type, uint32_t prod_id_start, uint32_t prod_id_end,
                                                 GpuRepeaterChannel* channel);

    } // namespace detail

    // ================================================================
    // Host interface
    // ================================================================
    class GpuMemoryRepeater {
    public:
        using DrainCallback = std::function<void(const GpuWqeRecord* records, size_t count)>;

        enum class State {
            Idle,
            Running,
            ShuttingDown
        };

        explicit GpuMemoryRepeater(int gpu_id, CUcontext app_ctx) : m_gpu_id(gpu_id), m_app_ctx(app_ctx) {
            CU_CHECK(cuCtxPushCurrent(m_app_ctx));

            CUdevice cu_dev;
            CU_CHECK(cuCtxGetDevice(&cu_dev));

            if (m_use_green_context) {
                // Check green context support
                int sm_count{0};
                CU_CHECK(cuDeviceGetAttribute(&sm_count, CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, cu_dev));

                // Get SM resources
                CUdevResource sm_resource;
                CU_CHECK(cuDeviceGetDevResource(cu_dev, &sm_resource, CU_DEV_RESOURCE_TYPE_SM));

                // Split SM resource
                // CUdevSmResource sm_res{.smCount = 1};
                CUdevResource res_resources;
                unsigned int resource_groups{1};
                CU_CHECK(cuDevSmResourceSplitByCount(&res_resources, &resource_groups, &sm_resource, nullptr, 0, 1));

                // Generate resource description
                CUdevResourceDesc res_desc;
                CU_CHECK(cuDevResourceGenerateDesc(&res_desc, &res_resources, 1));

                // Create the green context
                CU_CHECK(cuGreenCtxCreate(&m_green_ctx, res_desc, cu_dev, CU_GREEN_CTX_DEFAULT_STREAM));

                // Create the stream
                // CU_CHECK(cuGreenCtxCreate(&m_green_ctx, res_desc, cu_dev, CU_GREEN_CTX_DEFAULT_STREAM));
                int stream_priority_least{};
                int stream_priority_greatest{};
                CU_CHECK(cuCtxGetStreamPriorityRange(&stream_priority_least, &stream_priority_greatest));
                CU_CHECK(cuGreenCtxStreamCreate(&m_stream, m_green_ctx, CU_STREAM_NON_BLOCKING,
                                                stream_priority_greatest));
            } else {
                // CUcontext prev;
                // CU_CHECK(cuCtxPopCurrent(&prev));

                // Separate context — device-wide syncs in app_ctx won't reach this
                // This switches the thread to the ctx
                CUctxCreateParams ctx_params{};
                CU_CHECK(cuCtxCreate(&m_persistent_ctx, &ctx_params, CU_CTX_SCHED_SPIN, cu_dev));
                // CU_CHECK(cuStreamCreate(&m_stream, CU_STREAM_NON_BLOCKING));
                CU_CHECK(cuStreamCreate(&m_stream, CU_STREAM_DEFAULT));

                m_app_ctx = m_persistent_ctx;
            }

            // ===================================================
            // Channel and other memory allocation
            // ===================================================
            // Pinned memory for host and GPU access
            CUDA_CHECK(cudaHostAlloc(reinterpret_cast<void**>(&m_channel), sizeof(GpuRepeaterChannel),
                                     cudaHostAllocMapped | cudaHostAllocPortable));
            std::memset(m_channel, 0, sizeof(GpuRepeaterChannel));

            // TODO: hack to avoid recursion back to our hook. May not be necessary
            static auto* real_cuMemHostGetDevicePointer = reinterpret_cast<decltype(cuMemHostGetDevicePointer)*>(
                    CudaExportTable::get_real_cu_func("cuMemHostGetDevicePointer_v2"));
            assert(real_cuMemHostGetDevicePointer);

            // Device-visible pointer to the channel
            // CUDA_CHECK(cudaHostGetDevicePointer(reinterpret_cast<void**>(&m_d_channel), m_channel, 0));

            // Device-visible pointer to the channel
            CU_CHECK(real_cuMemHostGetDevicePointer(reinterpret_cast<CUdeviceptr*>(&m_d_channel), m_channel, 0));
            // ===================================================

            // ===================================================
            // Ready the kernel
            // ===================================================
            // Get the kernel function
            // CUDA_CHECK(cudaGetFuncBySymbol(&m_kernel_func, reinterpret_cast<void*>(detail::test_kernel)));
            CUDA_CHECK(cudaGetFuncBySymbol(&m_kernel_func, reinterpret_cast<void*>(detail::parse_and_record_oneshot)));

            if (m_use_graph_launch) {
                // Capture into graph
                CU_CHECK(cuStreamBeginCapture(m_stream, CU_STREAM_CAPTURE_MODE_RELAXED));

                void* args[] = {&m_d_channel};
                // void* args = nullptr;
                CU_CHECK(cuLaunchKernel(m_kernel_func, 1, 1, 1, // 1 block
                                        1, 1, 1,                // 1 thread
                                        0, m_stream, args, nullptr));

                CU_CHECK(cuStreamEndCapture(m_stream, &m_graph));
                CU_CHECK(cuGraphInstantiate(&m_graph_exec, m_graph, 0));
            }
            // ===================================================

            // CUcontext prev;
            // CU_CHECK(cuCtxPopCurrent(&prev));
        }

        void switch_context() const noexcept {
            CU_CHECK(cuCtxPushCurrent(m_app_ctx));
        }

        ~GpuMemoryRepeater() {
            if (m_launched && !m_shut_down) {
                shutdown();
            }

            return;

            // Free fake UAR allocations
            // CUcontext prev;
            // CU_CHECK(cuCtxPushCurrent(m_ctx));
            for (auto* ptr: m_fake_uar_allocs) {
                CUDA_CHECK(cudaFree(ptr));
            }
            CUDA_CHECK(cudaFreeHost(m_channel));
            CU_CHECK(cuStreamDestroy(m_stream));
            // CU_CHECK(cuCtxPopCurrent(&prev));
        }

        // Non-copyable, non-movable
        GpuMemoryRepeater(const GpuMemoryRepeater&) = delete;
        GpuMemoryRepeater& operator=(const GpuMemoryRepeater&) = delete;

        // --------------------------------------------------------
        // Launch persistent kernel. Safe to call before any UARs/QPs
        // are registered — kernel will pick them up dynamically.
        // --------------------------------------------------------
        void launch(void* sq_base, uint32_t qpn, uint32_t log_wq_size, int qp_type, uint32_t prod_id_start,
                    uint32_t prod_id_end) {
            // void launch() {
            // assert(!(m_launched && m_state == State::Running) && "GpuMemoryRepeater already launched");

            // CU_CHECK(cuCtxPushCurrent(m_app_ctx));

            if (m_use_graph_launch) {
                CU_CHECK(cuGraphLaunch(m_graph_exec, m_stream));
            } else {
                // void* args[] = {&m_d_channel};
                // CU_CHECK(cuLaunchKernel(m_kernel_func, 1, 1, 1, // 1 block
                //                         1, 1, 1,                // 1 thread
                //                         0, m_stream, args, nullptr));

                CU_CHECK(cuStreamSynchronize(m_stream));

                void* args[] = {
                        &sq_base, &qpn, &log_wq_size, &qp_type, &prod_id_start, &prod_id_end, &m_d_channel,
                };
                // void* args = nullptr;
                CU_CHECK(cuLaunchKernel(m_kernel_func, 1, 1, 1, // 1 block
                                        1, 1, 1,                // 1 thread
                                        0, m_stream, args, nullptr));
            }

            // CU_CHECK(cuStreamSynchronize(m_stream));

            // CUcontext prev;
            // CU_CHECK(cuCtxPopCurrent(&prev));

            // m_launched = true;
            // m_state = State::Running;
        }

        // --------------------------------------------------------
        // Stop the persistent kernel.
        // --------------------------------------------------------
        void shutdown() {
            if (!m_launched || m_shut_down) {
                return;
            }

            return;

            m_channel->shutdown = 1;
            __atomic_thread_fence(__ATOMIC_RELEASE);

            // CUcontext prev;
            // CU_CHECK(cuCtxPushCurrent(m_ctx));
            // CU_CHECK(cuStreamSynchronize(m_stream));
            // CU_CHECK(cuCtxPopCurrent(&prev));

            m_shut_down = true;
            std::cout << "[GpuMemoryRepeater] Shut down on GPU " << m_gpu_id << std::endl;
        }

        void ensure_running() {
            if (m_state == State::Running) {
                return;
            }

            return;

            m_channel->shutdown = 0;
            __atomic_thread_fence(__ATOMIC_RELEASE);

            CUcontext prev;
            CU_CHECK(cuCtxPushCurrent(m_app_ctx));

            // Set early to prevent recursion. TODO: use export table
            m_state = State::Running;
            // launch();

            // CU_CHECK(cuStreamSynchronize(nullptr));

            CU_CHECK(cuCtxPopCurrent(&prev));
        }

        void ensure_stopped() {
            if (m_state != State::Running) {
                return;
            }

            return;

            m_channel->shutdown = 1;
            __atomic_thread_fence(__ATOMIC_RELEASE);

            // TODO: don't really need to sync here
            CUcontext prev;
            CU_CHECK(cuCtxPushCurrent(m_app_ctx));
            CU_CHECK(cuStreamSynchronize(m_stream));
            CU_CHECK(cuCtxPopCurrent(&prev));

            m_state = State::Idle;
        }

        // --------------------------------------------------------
        // Register a UAR for interception. Allocates a fake doorbell
        // page in GPU memory and returns its device pointer.
        // Caller should give the returned pointer to NVSHMEM
        // (via cuMemHostGetDevicePointer substitution).
        //
        // real_uar_device_ptr: device-accessible pointer to the real
        //   NIC BAR (from the original cuMemHostGetDevicePointer).
        //
        // Returns: device pointer to fake doorbell in GPU HBM.
        // --------------------------------------------------------
        CUdeviceptr add_uar(CUdeviceptr real_uar_device_ptr) {
            std::scoped_lock lock(m_table_mutex);

            const int idx = m_channel->num_uars;
            assert(idx < GpuRepeaterChannel::MAX_UARS && "Too many UARs");

            // Allocate fake doorbell in GPU memory (fast L2 polling for kernel)
            uint64_t* fake_dev = nullptr;
            CUcontext prev;
            // CU_CHECK(cuCtxPushCurrent(m_ctx));
            CUDA_CHECK(cudaMalloc(&fake_dev, 256)); // over-allocate to avoid false sharing
            CUDA_CHECK(cudaMemset(fake_dev, 0, 256));
            // CU_CHECK(cuCtxPopCurrent(&prev));

            m_fake_uar_allocs.push_back(fake_dev);

            // Write entry to table (host side of pinned memory)
            m_channel->uars[idx].fake_addr = fake_dev;
            m_channel->uars[idx].real_addr = reinterpret_cast<uint64_t*>(real_uar_device_ptr);

            // Publish: fence then increment count
            __atomic_thread_fence(__ATOMIC_RELEASE);
            m_channel->num_uars = idx + 1;

            std::cout << "[GpuMemoryRepeater] GPU " << m_gpu_id << " added UAR #" << idx << " fake=" << fake_dev
                      << " real=0x" << std::hex << real_uar_device_ptr << std::dec << std::endl;

            return reinterpret_cast<CUdeviceptr>(fake_dev);
        }

        // --------------------------------------------------------
        // Register a QP for WQE parsing.
        // --------------------------------------------------------
        void add_qp(uint32_t qpn, void* sq_base, uint32_t log_wq_size, int qp_type, int gpu_id) {
            std::scoped_lock lock(m_table_mutex);

            const int idx = m_channel->num_qps;
            assert(idx < GpuRepeaterChannel::MAX_QPS && "Too many QPs");

            m_channel->qps[idx] = GpuQpEntry{
                    .sq_base = sq_base,
                    .qpn = qpn,
                    .log_wq_size = log_wq_size,
                    .qp_type = qp_type,
                    .gpu_id = gpu_id,
            };

            __atomic_thread_fence(__ATOMIC_RELEASE);
            m_channel->num_qps = idx + 1;

            std::cout << "[GpuMemoryRepeater] GPU " << m_gpu_id << " added QP " << qpn << " log_sq=" << log_wq_size
                      << " type=" << (qp_type == QP_TYPE_DCI ? "DCI" : "RC") << std::endl;
        }

        // --------------------------------------------------------
        // Set the callback invoked during drain().
        // Callback receives a contiguous batch of records.
        // May be called twice per drain if ring wraps.
        // --------------------------------------------------------
        void set_drain_callback(DrainCallback cb) {
            m_callback = std::move(cb);
        }

        // --------------------------------------------------------
        // Drain all pending WQE records from the GPU.
        // Calls the registered callback (if any) with batches.
        // Returns the number of records drained.
        // --------------------------------------------------------
        size_t drain() {
            const uint32_t head = m_channel->result_head;
            __atomic_thread_fence(__ATOMIC_ACQUIRE);

            const uint32_t tail = m_local_tail;
            const uint32_t count = head - tail;
            if (count == 0)
                return 0;

            if (m_callback) {
                const uint32_t start = tail & GpuRepeaterChannel::RESULT_RING_MASK;
                const uint32_t end = head & GpuRepeaterChannel::RESULT_RING_MASK;

                // Cast away volatile — data is synchronized by the head/tail fence
                const auto* ring = const_cast<const GpuWqeRecord*>(m_channel->result_ring);

                if (start < end || count <= GpuRepeaterChannel::RESULT_RING_SIZE - start) {
                    // No wrap (or exactly fills to end)
                    m_callback(ring + start, count);
                } else {
                    // Wraps: two contiguous segments
                    const uint32_t first = GpuRepeaterChannel::RESULT_RING_SIZE - start;
                    m_callback(ring + start, first);
                    m_callback(ring, count - first);
                }
            }

            m_local_tail = head;
            __atomic_thread_fence(__ATOMIC_RELEASE);
            m_channel->result_tail = head;

            return count;
        }

        // --------------------------------------------------------
        // Accessors
        // --------------------------------------------------------
        [[nodiscard]] int gpu_id() const {
            return m_gpu_id;
        }
        [[nodiscard]] bool is_launched() const {
            return m_launched;
        }

        [[nodiscard]] uint32_t pending_count() const {
            return m_channel->result_head - m_local_tail;
        }

    private:
        // Config
        bool m_use_green_context{false};
        bool m_use_graph_launch{false};

        State m_state = State::Idle;

        int m_gpu_id;
        CUcontext m_app_ctx;
        CUcontext m_persistent_ctx{nullptr};
        CUgreenCtx m_green_ctx{nullptr};
        CUstream m_stream;

        CUgraph m_graph;
        CUgraphExec m_graph_exec;
        CUfunction m_kernel_func;

        // Pinned host memory — shared with GPU kernel
        GpuRepeaterChannel* m_channel = nullptr;
        GpuRepeaterChannel* m_d_channel = nullptr; // device-visible alias

        // Tracking for cleanup
        std::vector<uint64_t*> m_fake_uar_allocs;

        // Drain state
        uint32_t m_local_tail = 0;
        DrainCallback m_callback;

        // For thread-safe add_uar / add_qp from host hooks
        std::mutex m_table_mutex;

        bool m_launched = false;
        bool m_shut_down = false;
    };

} // namespace Profiler::GpuRepeater
