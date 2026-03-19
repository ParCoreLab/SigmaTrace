#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <cuda.h>

#include "../ProfilerShim.hpp"

#include "Helpers/CudaHelpers.hpp"

#include "../Events/CUDAEvents.hpp"
#include "../Events/Mlx5DvEvents.hpp"

#include "../Modules/CudaExportTableOverwrite/ExportTableOverwrite.hpp"

#include "Common.hpp"

namespace Profiler::hijack {
    // TODO: move these into ProfilerCtx
    inline std::mutex m_profiler_hijack_mutex;
    // ================================================================

    // NIC UAR regaddresses allocated with mlx5dv_devx_alloc_uar
    // uar->reg_addr -> uar->page_id
    inline std::unordered_map<void*, UarPageIdType> Mlx5UarAllocations{};

    // Host addresses registered with cuMemHostRegister with CU_MEMHOSTREGISTER_DEVICEMAP
    // These are the uar->reg_addr from above
    inline std::unordered_set<void*> CuMemHostRegisterAddrs{};

    // Fake host pointers we create with repeaters
    // Real UAR address (uar->reg_addr) -> Fake UAR address (cudaHostAlloc-ed page)
    inline std::unordered_map<void*, void*> CuFakeUarPtrs{};
    // ================================================================

    // Per-handle worker tracking for drain-all (sync hooks / shutdown)
    inline std::mutex g_workers_mutex;
    inline std::vector<std::weak_ptr<class DoorbellParseWorker>> g_workers;

    // Called from pthread_atfork child handler.
    // Reinitializes the mutex (POSIX: mutex state is undefined after fork)
    // and clears stale parent-process data from the maps.
    // Forward-declared in ProfilerShim.cpp.
    void reset();
    void reset() {
        new (&m_profiler_hijack_mutex) std::mutex();
        new (&g_workers_mutex) std::mutex();
        Mlx5UarAllocations.clear();
        CuMemHostRegisterAddrs.clear();
        CuFakeUarPtrs.clear();
        g_workers.clear();
    }

    // ================================================================
    // WQE parsing helpers
    // ================================================================
    constexpr uint32_t WQEBB_SIZE = 64;

    struct ParseResult {
        uint64_t total_bytes{0};
        uint32_t send_count{0};
        std::unordered_map<uint32_t, uint64_t> bytes_by_end_prod_id;
    };

    static ParseResult parse_wqe_buffer(const uint8_t* host_buf, uint32_t prod_id_start, uint32_t prod_id_end,
                                        uint32_t qpn, uint8_t service_type) {
        ParseResult result;

        for (uint32_t i = 0, prod_id = prod_id_start; prod_id < prod_id_end;) {
            const uint8_t* wqe = host_buf + i * WQEBB_SIZE;
            const auto* ctrl = reinterpret_cast<const mlx5_wqe_ctrl_seg*>(wqe);

            const uint32_t wqe_qpn_ds = be32toh(ctrl->qpn_ds);
            const uint32_t wqe_ds = wqe_qpn_ds & 0xFF;
            const uint32_t wqe_qpn = wqe_qpn_ds >> 8;

            if (wqe_qpn != qpn) {
                std::cerr << "[i=" << i << " prod_id = " << prod_id << "] WRONG QPN " << wqe_qpn << " vs " << qpn
                          << std::endl;
                std::abort();
            }

            // Parse WQE segments to extract byte count
            // ctrl_seg is 16B (1 ds unit). Remaining ds units are data/raddr segments.
            const uint32_t opmod_idx_opcode = be32toh(ctrl->opmod_idx_opcode);
            const uint8_t opcode = opmod_idx_opcode & 0xFF;
            // Only parse data_segs for opcodes with known WQE layouts
            // RDMA_WRITE (0x08), RDMA_READ (0x10): ctrl + [datagram] + raddr + data_segs
            // SEND (0x0A): ctrl + [datagram] + data_segs
            // Skip NOP (0x00), UMR (0x14), and other opcodes with different layouts
            const bool has_raddr = (opcode == 0x08 || opcode == 0x10);
            const bool has_data_segs = (opcode == 0x08 || opcode == 0x10 || opcode == 0x0A);

            uint64_t wqe_bytes = 0;
            if (has_data_segs && wqe_ds > 1) {
                const uint8_t* seg_ptr = wqe + 16;  // past ctrl_seg (16B)
                uint32_t remaining_ds = wqe_ds - 1; // ds units after ctrl

                // DCI QPs (st=0x5) have a 64B datagram/AV segment (4 ds units) after ctrl_seg
                if (service_type == 0x5 && remaining_ds >= 4) {
                    seg_ptr += 64;
                    remaining_ds -= 4;
                }

                if (has_raddr && remaining_ds >= 1) {
                    // Skip mlx5_wqe_raddr_seg (16B = 1 ds unit)
                    seg_ptr += 16;
                    remaining_ds -= 1;
                }

                // Parse data segments (each 16B): byte_count(4B) + lkey(4B) + addr(8B)
                // Bit 31 of byte_count is the inline data flag in MLX5; mask it off
                while (remaining_ds >= 1) {
                    const uint32_t byte_count = be32toh(*reinterpret_cast<const uint32_t*>(seg_ptr)) & 0x7FFFFFFF;
                    wqe_bytes += byte_count;
                    seg_ptr += 16;
                    remaining_ds -= 1;
                }
            }

            result.total_bytes += wqe_bytes;
            if (has_data_segs) {
                ++result.send_count;
            }

            // ds is in 16B units; advance by whole WQEBBs (64B each = 4 ds units)
            const uint32_t wqebbs = (wqe_ds + 3) / 4;
            i += wqebbs;
            prod_id += wqebbs;

            // Map end prod_id to byte count for doorbell correlation
            result.bytes_by_end_prod_id[prod_id] = wqe_bytes;
        }

        return result;
    }

    static CUresult copy_wqes_from_gpu(uint8_t* dst, const QpInfo& qp_info, uint32_t prod_id_start,
                                       uint32_t prod_id_end, CUstream stream) {
        const uint32_t num_wqebbs_total = prod_id_end - prod_id_start;
        const auto wq_mask = (1u << qp_info.log_wq_size) - 1u;
        const uint32_t start_slot = prod_id_start & wq_mask;
        const uint32_t wq_slots = 1u << qp_info.log_wq_size;

        if (start_slot + num_wqebbs_total <= wq_slots) {
            return cuMemcpyDtoHAsync(dst, reinterpret_cast<CUdeviceptr>(qp_info.sq_base) + start_slot * WQEBB_SIZE,
                                     num_wqebbs_total * WQEBB_SIZE, stream);
        }

        const uint32_t tail = wq_slots - start_slot;
        const uint32_t head = num_wqebbs_total - tail;
        const CUresult err =
                cuMemcpyDtoHAsync(dst, reinterpret_cast<CUdeviceptr>(qp_info.sq_base) + start_slot * WQEBB_SIZE,
                                  tail * WQEBB_SIZE, stream);
        if (err != CUDA_SUCCESS) {
            return err;
        }
        return cuMemcpyDtoHAsync(dst + tail * WQEBB_SIZE, reinterpret_cast<CUdeviceptr>(qp_info.sq_base),
                                 head * WQEBB_SIZE, stream);
    }

    /*
     * Background parse worker
     * Wanted to offload WQE parsing to a background thread after we copy the data
     * to avoid delaying the doorbell forwarding too much.
     * I'm unsure at this point if this really makes any difference because the small
     * machine I'm testing on is too small.
     *
     * TODO:
     *   - Consider using BackgroundProcessor. The code here is messy for no good reason
     *   - Check if delayed cuStreamSynchronize is OK. It's probably not
     *   - We can probably offload the memcpy to the background thread too...
     *   - Need to do preemptive doorbell flush on known high-latency paths
     *     such as context or stream synchronize
     */
    struct ParseWorkItem {
        // std::vector<uint8_t> wqe_data;
        uint8_t* wqe_data;
        std::vector<PendingDoorbell> pending_doorbells;
        uint32_t prod_id_start;
        uint32_t prod_id_end;
        uint32_t qpn;
        GpuId_T gpu_id;
        uint8_t service_type;
        CUstream cu_stream;
    };

    class DoorbellParseWorker {
        ProfilerCtx& m_profiler;
        std::mutex m_mutex;
        std::condition_variable m_cv;
        std::condition_variable m_drain_cv;
        std::deque<ParseWorkItem> m_queue;
        uint64_t m_enqueue_count{0};
        uint64_t m_process_count{0};
        bool m_shutdown{false};
        std::thread m_thread;

        void process_item(const ParseWorkItem& item) {
            CU_CHECK(cuStreamSynchronize(item.cu_stream));

            const auto result =
                    parse_wqe_buffer(item.wqe_data, item.prod_id_start, item.prod_id_end, item.qpn, item.service_type);

            {
                auto& traffic = m_profiler.m_traffic_registry.get_thread_local()[item.qpn];
                traffic.bytes_sent += result.total_bytes;
                traffic.send_count += result.send_count;
            }

            for (const auto& pending: item.pending_doorbells) {
                uint64_t doorbell_bytes = 0;
                const auto it = result.bytes_by_end_prod_id.find(pending.prod_id);
                if (it != result.bytes_by_end_prod_id.end()) {
                    doorbell_bytes = it->second;
                }

                const Events::DoorbellRing event{
                        .addr = 0,
                        .prod_index = pending.prod_id,
                        .qp_num = item.qpn,
                        .gpu_id = item.gpu_id,
                        .is_cpu_hijack = true,
                        .bytes_sent = doorbell_bytes,
                };
                m_profiler.m_event_recorder->Record(event, pending.timestamp);
            }
        }

        void run() {
            while (true) {
                ParseWorkItem item;
                {
                    std::unique_lock lock(m_mutex);
                    m_cv.wait(lock, [this] {
                        return m_shutdown || !m_queue.empty();
                    });

                    if (m_shutdown && m_queue.empty()) {
                        return;
                    }

                    item = std::move(m_queue.front());
                    m_queue.pop_front();
                }

                process_item(item);

                {
                    std::scoped_lock lock(m_mutex);
                    ++m_process_count;
                    m_drain_cv.notify_all();
                }
            }
        }

    public:
        explicit DoorbellParseWorker(ProfilerCtx& profiler) :
            m_profiler(profiler), m_thread([this] {
                run();
            }) {}

        ~DoorbellParseWorker() {
            {
                std::scoped_lock lock(m_mutex);
                m_shutdown = true;
            }
            m_cv.notify_all();
            if (m_thread.joinable()) {
                m_thread.join();
            }
        }

        void enqueue(ParseWorkItem&& item) {
            {
                std::scoped_lock lock(m_mutex);
                m_queue.push_back(std::move(item));
                ++m_enqueue_count;
            }
            m_cv.notify_one();
        }

        // Wait for all items enqueued up to this point to be processed.
        void drain() {
            std::unique_lock lock(m_mutex);
            const auto target = m_enqueue_count;
            m_drain_cv.wait(lock, [this, target] {
                return m_process_count >= target;
            });
        }
    };

    static void drain_all_workers() {
        std::scoped_lock lock(g_workers_mutex);
        std::erase_if(g_workers, [](const auto& wp) {
            return wp.expired();
        });
        for (auto& wp: g_workers) {
            if (auto sp = wp.lock()) {
                sp->drain();
            }
        }
    }

    // ================================================================
    // CUDA API hooks
    // ================================================================
    CUresult Fake_cuMemAlloc(CUdeviceptr* dptr, size_t bytesize);
    static_assert(std::is_same_v<decltype(cuMemAlloc), decltype(Fake_cuMemAlloc)>);

    CUresult Fake_cuLaunchKernel(CUfunction f, unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
                                 unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
                                 unsigned int sharedMemBytes, CUstream hStream, void** kernelParams, void** extra);
    static_assert(std::is_same_v<decltype(cuLaunchKernel), decltype(Fake_cuLaunchKernel)>);

    CUresult Fake_cuMemHostGetDevicePointer_v2(CUdeviceptr* pdptr, void* p, unsigned int Flags);
    static_assert(std::is_same_v<decltype(cuMemHostGetDevicePointer_v2), decltype(Fake_cuMemHostGetDevicePointer_v2)>);

    decltype(auto) CuMemHostRegisterOverwrite(auto* real_func, void* p, size_t bytesize, unsigned int Flags);

    CUresult Fake_cuMemHostRegister_v2(void* p, size_t bytesize, unsigned int Flags);
    static_assert(std::is_same_v<decltype(cuMemHostRegister_v2), decltype(Fake_cuMemHostRegister_v2)>);

    // https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__MEM.html#group__CUDA__MEM_1g63f450c8125359be87b7623b1c0b2a14
    CUresult Fake_cuMemHostUnregister(void* p);
    static_assert(std::is_same_v<decltype(cuMemHostUnregister), decltype(Fake_cuMemHostUnregister)>);

    // TODO Unused for now
    CUresult Fake_cuStreamSynchronize(CUstream hStream);
    static_assert(std::is_same_v<decltype(cuStreamSynchronize), decltype(Fake_cuStreamSynchronize)>);

    CUresult Fake_cuCtxSynchronize();
    static_assert(std::is_same_v<decltype(cuCtxSynchronize), decltype(Fake_cuCtxSynchronize)>);
    // ================================================
} // namespace Profiler::hijack

namespace Profiler::hijack {
    CUresult Fake_cuLaunchKernel(CUfunction f, unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
                                 unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
                                 unsigned int sharedMemBytes, CUstream hStream, void** kernelParams, void** extra) {
        auto* real_func =
                reinterpret_cast<decltype(cuLaunchKernel)*>(CudaExportTable::get_real_cu_func("cuLaunchKernel"));

        return real_func(f, gridDimX, gridDimY, gridDimZ, blockDimX, blockDimY, blockDimZ, sharedMemBytes, hStream,
                         kernelParams, extra);
    }

    /*
     * What happens here:
     * 0. User registers a UAR address with `mlx5dv_devx_alloc_uar`. We save it to `Mlx5UarAllocations`
     * 1. User registers the address with `cuMemHostRegister` + `CU_MEMHOSTREGISTER_DEVICEMAP`. We save it to
     * `CuMemHostRegisterAddrs`
     * 2. User calls `cuMemHostGetDevicePointer(device_ptr**, host_ptr, Flags)`.
     *      We check if host_ptr is saved from steps 0 and 1
     *      We allocate a fake HOST page and register a repeater for it. The page is allocated with cudaHostAlloc
     *      We call the original cuMemHostGetDevicePointer with the fake host page
     */
    static void on_doorbell_ring(const uint64_t new_val, DoorbellParseWorker& worker) {
        /*
         * UAR Doorbell NVSHMEM writes only has the first two fields
         * See ibgda_device.cuh
         *
         * nvshmemi_ibgda_device_qp_t *qp, uint16_t prod_idx) {
         *     uint64_t *bf_ptr = (uint64_t *)qp->tx_wq.bf;
         *     ibgda_ctrl_seg_t ctrl_seg = {
         *         .opmod_idx_opcode = HTOBE32(prod_idx << 8),
         *         .qpn_ds = HTOBE32(qp->qpn << 8)
         *     };
         *     ibgda_store_release(bf_ptr, *((uint64_t *)&ctrl_seg));
         * }
         */

        // https://network.nvidia.com/files/doc-2020/ethernet-adapters-programming-manual.pdf
        // Probably better to save qp numbers in network order from creation..........
        /*
         * Here we parse
         * struct mlx5_wqe_ctrl_seg {
         *     __be32		opmod_idx_opcode;
         *     __be32		qpn_ds;
         *     ...
         * }
         *
         * Alternative
         * const auto* db = static_cast<const uint32_t*>(fake_reg_addr);
         * uint32_t qpn_ds = be32toh(db[0]);
         * uint32_t opmod_idx_opcode = be32toh(db[1]);
         */
        // First and Second 32 bits
        const uint32_t dbr_qpn_ds = be32toh(static_cast<uint32_t>(new_val >> 32));
        const uint32_t dbr_opmod_idx_opcode = be32toh(static_cast<uint32_t>(new_val));

        /*
         * `ds` and `opcode` are not set
         * See ibgda_write_rdma_write_wqe
         * See doca_gpunetio_dev_verbs_qp.cuh also
         */
        const uint32_t dbr_qpn = dbr_qpn_ds >> 8;
        const uint32_t dbr_prod_id = dbr_opmod_idx_opcode >> 8 & 0xFFFF;

        auto& profiler = instance();

        // Should be guaranteed to exist
        auto& qp_info = profiler.m_mlx5_qp_info.at(dbr_qpn);

        // Producer IDs
        const auto last_prod_id = qp_info._last_prod_id;
        qp_info._last_prod_id = dbr_prod_id;

        // wqe_buf/qp_info.sq_base is inaccessible from host, we need to copy it over
        if (qp_info.gpu_mem_type == cudaMemoryTypeDevice) {
            // Capture timestamp and save pending doorbell for deferred byte count resolution
            const auto ts = profiler.m_event_recorder->timestamp_source().now();
            qp_info.pending_doorbells.push_back({dbr_prod_id, ts});

            if (!qp_info._has_pending) {
                qp_info._oldest_uncopied_prod_id = last_prod_id;
                qp_info._has_pending = true;
            }

            constexpr auto Batch_Threshold{512};
            constexpr auto Flush_Margin{64};
            const auto wq_size = 1u << qp_info.log_wq_size;
            const auto distance = dbr_prod_id - qp_info._oldest_uncopied_prod_id;

            const bool should_submit = (distance + Flush_Margin >= wq_size) // safety
                                       || (distance >= Batch_Threshold);    // batching

            if (should_submit) {
                const auto prod_id_start = qp_info._oldest_uncopied_prod_id;
                const auto prod_id_end = dbr_prod_id;
                // const uint32_t num_wqebbs_total = prod_id_end - prod_id_start;

                thread_local CUstream cu_stream = std::invoke([&qp_info]() {
                    CUstream s;
                    CU_CHECK(cuCtxPushCurrent(qp_info.gpu_ctx));
                    CU_CHECK(cuStreamCreate(&s, CU_STREAM_NON_BLOCKING));
                    return s;
                });

                // Auto-cleaning thread_local buffer
                struct CuMemHostDeleter {
                    void operator()(uint8_t* ptr) const {
                        if (ptr) {
                            CU_CHECK(cuMemFreeHost(ptr));
                        }
                    }
                };
                using CuHostMemPtr = std::unique_ptr<uint8_t[], CuMemHostDeleter>;
                thread_local CuHostMemPtr host_wqe_buf{nullptr};
                if (!host_wqe_buf) {
                    void* raw_ptr = nullptr;
                    CU_CHECK(cuMemAllocHost(&raw_ptr, 1024 * WQEBB_SIZE * 64));
                    host_wqe_buf.reset(static_cast<uint8_t*>(raw_ptr));
                }

                CU_CHECK(copy_wqes_from_gpu(host_wqe_buf.get(), qp_info, prod_id_start, prod_id_end, cu_stream));

                // May be unsafe to not call this here
                // TODO: if something breaks, enable this here and remove the same line from process_item
                // CU_CHECK(cuStreamSynchronize(cu_stream));

                // Offload parsing to background worker
                ParseWorkItem work_item;
                work_item.cu_stream = cu_stream;
                work_item.wqe_data = host_wqe_buf.get();

                work_item.pending_doorbells = std::move(qp_info.pending_doorbells);
                work_item.prod_id_start = prod_id_start;
                work_item.prod_id_end = prod_id_end;
                work_item.qpn = dbr_qpn;
                work_item.gpu_id = qp_info.gpu_id;
                work_item.service_type = qp_info.service_type;

                qp_info.pending_doorbells.clear();
                qp_info._oldest_uncopied_prod_id = dbr_prod_id;
                qp_info._has_pending = false;

                worker.enqueue(std::move(work_item));

                profiler.doorbell_submitted.fetch_add(1, std::memory_order_relaxed);
            } else {
                profiler.doorbell_skipped.fetch_add(1, std::memory_order_relaxed);
            }
        }
        assert(qp_info.gpu_id >= 0 && "qp_info GPU ID is not valid in a GPU Doorbell hook");

        profiler.num_fake_nic_accesses.fetch_add(1, std::memory_order_relaxed);
    }

    CUresult CuMemHostGetDevicePointerOverwrite(auto* real_func, CUdeviceptr* pdptr, void* p, unsigned int Flags) {
        if (!real_func) {
            std::cerr << "No real_cuMemHostGetDevicePointer" << std::endl;
            std::abort();
        }

        // TODO: read lock
        // this is a scoped lock to track down recursion bugs with injection functions for now
        std::scoped_lock lock(m_profiler_hijack_mutex);

        if (!CuMemHostRegisterAddrs.contains(p)) {
            return real_func(pdptr, p, Flags);
        }

        // ================================================================================================
        // Fake UAR page replacement
        // ================================================================================================

        auto& profiler = instance();
        // Already registered a repeater — return fake host ptr
        {
            const auto& fake_device_ptr_entry = CuFakeUarPtrs.find(p);
            if (fake_device_ptr_entry != CuFakeUarPtrs.end()) {
                void* fake_addr = fake_device_ptr_entry->second;
                return real_func(pdptr, fake_addr, Flags);
            }
        }

        // First time: allocate fake host page
        constexpr auto PageSize{4096};
        void* uar_fake_page_addr;
        const auto cu_err = CU_LOG(
                cuMemHostAlloc(&uar_fake_page_addr, PageSize, CU_MEMHOSTALLOC_PORTABLE | CU_MEMHOSTALLOC_DEVICEMAP));
        if (cu_err != CUDA_SUCCESS) {
            return cu_err;
        }
        std::memset(uar_fake_page_addr, 0, PageSize);

        void* reg_addr_real = p;
        void* fake_reg_addr = uar_fake_page_addr;

        auto worker = std::make_shared<DoorbellParseWorker>(profiler);
        {
            std::scoped_lock wlock(g_workers_mutex);
            g_workers.push_back(worker);
        }

        MemoryRepeaterHandle handle{static_cast<uint64_t*>(fake_reg_addr), reg_addr_real, [worker](uint64_t new_val) {
            on_doorbell_ring(new_val, *worker);
        }};

        const auto page_id = Mlx5UarAllocations.at(p);
        auto& hijack_handles = profiler.m_mlx5_uar_hijack_handles[page_id];
        auto& inserted_handle = hijack_handles.emplace_back(std::move(handle));
        inserted_handle.start();

        const auto& [fake_ptr_entry, _] = CuFakeUarPtrs.insert_or_assign(p, fake_reg_addr);
        return real_func(pdptr, fake_ptr_entry->second, Flags);
    }

    CUresult Fake_cuMemHostGetDevicePointer_v2(CUdeviceptr* pdptr, void* p, unsigned int Flags) {
        auto* real_cuMemHostGetDevicePointer = reinterpret_cast<decltype(cuMemHostGetDevicePointer_v2)*>(
                CudaExportTable::get_real_cu_func("cuMemHostGetDevicePointer_v2"));

        return CuMemHostGetDevicePointerOverwrite(real_cuMemHostGetDevicePointer, pdptr, p, Flags);
    }

    decltype(auto) CuMemHostRegisterOverwrite(auto* real_func, void* p, size_t bytesize, unsigned int Flags) {
        if (!real_func) {
            std::cerr << "No real_cuMemHostRegister" << std::endl;
            std::abort();
        }

        // UAR registered and IO memory
        if (Mlx5UarAllocations.contains(p) && (Flags & CU_MEMHOSTREGISTER_DEVICEMAP)) {
            std::scoped_lock lock(m_profiler_hijack_mutex);
            // Fake page, skip register
            CuMemHostRegisterAddrs.emplace(p);
            return CUDA_SUCCESS;
        }

        return real_func(p, bytesize, Flags);
    }

    CUresult Fake_cuMemHostRegister_v2(void* p, size_t bytesize, unsigned int Flags) {
        auto* real_cuMemHostRegister = reinterpret_cast<decltype(cuMemHostRegister_v2)*>(
                CudaExportTable::get_real_cu_func("cuMemHostRegister_v2"));

        return CuMemHostRegisterOverwrite(real_cuMemHostRegister, p, bytesize, Flags);
    }

    // https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__MEM.html#group__CUDA__MEM_1g63f450c8125359be87b7623b1c0b2a14
    CUresult Fake_cuMemHostUnregister(void* p) {
        auto* real_cuMemHostUnregister = reinterpret_cast<decltype(cuMemHostUnregister)*>(
                CudaExportTable::get_real_cu_func("cuMemHostUnregister"));

        if (!real_cuMemHostUnregister) {
            std::cerr << "No real_cuMemHostUnregister" << std::endl;
            std::abort();
        }

        if (Mlx5UarAllocations.contains(p)) {
            // We skipped register, so skip unregister
            return CUDA_SUCCESS;
        }

        return real_cuMemHostUnregister(p);
    }

    // Eager flush in know safe stages
    // If we're here we're likely not in a performance-critical area
    // TODO: unused for now
    CUresult Fake_cuStreamSynchronize(CUstream hStream) {
        // Drain all per-handle workers, then flush any sub-threshold pending doorbells
        instance().flush_pending_doorbells();

        auto* real_func = reinterpret_cast<decltype(cuStreamSynchronize)*>(
                CudaExportTable::get_real_cu_func("cuStreamSynchronize"));
        return real_func(hStream);
    }

    CUresult Fake_cuCtxSynchronize() {
        // Drain all per-handle workers, then flush any sub-threshold pending doorbells
        instance().flush_pending_doorbells();

        auto* real_func =
                reinterpret_cast<decltype(cuCtxSynchronize)*>(CudaExportTable::get_real_cu_func("cuCtxSynchronize"));
        return real_func();
    }
} // namespace Profiler::hijack

void ProfilerCtx::load_doorbell_hijack() {
    // ================================================================================================
    // Register CUDA API overwrites
    // We only register _v2 versions to avoid issues, but it should be fine
    Profiler::CudaExportTable::register_overwrite("cuLaunchKernel",
                                                  reinterpret_cast<void*>(Profiler::hijack::Fake_cuLaunchKernel));

    // Profiler::CudaExportTable::register_overwrite("cuMemAlloc",
    //                                               reinterpret_cast<void*>(Profiler::hijack::Fake_cuMemAlloc));

    Profiler::CudaExportTable::register_overwrite("cuMemHostRegister_v2",
                                                  reinterpret_cast<void*>(Profiler::hijack::Fake_cuMemHostRegister_v2));

    Profiler::CudaExportTable::register_overwrite("cuMemHostUnregister",
                                                  reinterpret_cast<void*>(Profiler::hijack::Fake_cuMemHostUnregister));

    Profiler::CudaExportTable::register_overwrite(
            "cuMemHostGetDevicePointer_v2",
            reinterpret_cast<void*>(Profiler::hijack::Fake_cuMemHostGetDevicePointer_v2));

    // Profiler::CudaExportTable::register_overwrite("cuStreamSynchronize",
    //                                               reinterpret_cast<void*>(Profiler::hijack::Fake_cuStreamSynchronize));

    // Profiler::CudaExportTable::register_overwrite("cuCtxSynchronize",
    //                                               reinterpret_cast<void*>(Profiler::hijack::Fake_cuCtxSynchronize));
    // ================================================================================================

    // Fake UAR hijack
    m_mlx5dv_profiler->register_post<mlx5dv::devx_alloc_uar_tag>(
            [](mlx5dv_devx_uar* out_uar, ibv_context*, uint32_t, auto...) {
        std::scoped_lock lock(Profiler::hijack::m_profiler_hijack_mutex);
        Profiler::hijack::Mlx5UarAllocations.insert_or_assign(out_uar->reg_addr, out_uar->page_id);
    });

    // For hijack
    m_mlx5dv_profiler->register_pre<mlx5dv::devx_free_uar_tag>([](mlx5dv_devx_uar* devx_uar, auto...) {
        std::scoped_lock lock(Profiler::hijack::m_profiler_hijack_mutex);

        // TODO (big): this should really be a dependency injection *into* this module
        // We shouldn't be pulling ProfilerShim.hpp
        auto& profiler = instance();

        // Stop repeater threads and destroy per-handle workers.
        // signal_finish() stops polling, erase joins the polling thread and
        // destroys the callback lambda
        // The worker destructor processes remaining items and joins its thread.
        // GPU SQ buffers are still valid (UAR free happens before umem_dereg).
        const auto handles = profiler.m_mlx5_uar_hijack_handles.find(devx_uar->page_id);
        if (handles != profiler.m_mlx5_uar_hijack_handles.end()) {
            for (auto& h: handles->second) {
                h.signal_finish();
            }
            profiler.m_mlx5_uar_hijack_handles.erase(handles);
        }

        // Flush any remaining sub-threshold pending doorbells
        profiler.flush_pending_doorbells();

        Profiler::hijack::Mlx5UarAllocations.erase(devx_uar->reg_addr);
    });

    std::cout << "[Profiler] Loaded Doorbell Hijack Trace" << std::endl;
}

// Flush all pending doorbell batches so bytes aren't lost.
// Called from sync hooks and before emit_communication_summaries() at shutdown.
void ProfilerCtx::flush_pending_doorbells() {
    // Drain all per-handle background workers first
    Profiler::hijack::drain_all_workers();

    std::scoped_lock qp_lock(m_mlx5_qp_mutex);

    for (auto& [qpn, qp_info]: m_mlx5_qp_info) {
        if (!qp_info._has_pending || qp_info.pending_doorbells.empty()) {
            continue;
        }

        if (qp_info.gpu_mem_type != cudaMemoryTypeDevice) {
            continue;
        }

        const auto prod_id_start = qp_info._oldest_uncopied_prod_id;
        const auto prod_id_end = qp_info._last_prod_id;

        if (prod_id_end <= prod_id_start) {
            continue;
        }

        const uint32_t num_wqebbs_total = prod_id_end - prod_id_start;

        CUstream cu_stream;
        if (cuCtxPushCurrent(qp_info.gpu_ctx) != CUDA_SUCCESS) {
            continue;
        }
        if (cuStreamCreate(&cu_stream, CU_STREAM_NON_BLOCKING) != CUDA_SUCCESS) {
            CUcontext dummy;
            CU_CHECK(cuCtxPopCurrent(&dummy));
            continue;
        }

        std::vector<uint8_t> host_wqe_buf(num_wqebbs_total * Profiler::hijack::WQEBB_SIZE);

        const CUresult err_copy = Profiler::hijack::copy_wqes_from_gpu(host_wqe_buf.data(), qp_info, prod_id_start,
                                                                       prod_id_end, cu_stream);

        if (err_copy != CUDA_SUCCESS || cuStreamSynchronize(cu_stream) != CUDA_SUCCESS) {
            CU_CHECK(cuStreamDestroy(cu_stream));
            CUcontext dummy;
            CU_CHECK(cuCtxPopCurrent(&dummy));
            continue;
        }

        const auto result = Profiler::hijack::parse_wqe_buffer(host_wqe_buf.data(), prod_id_start, prod_id_end, qpn,
                                                               qp_info.service_type);

        {
            auto& traffic = m_traffic_registry.get_thread_local()[qpn];
            traffic.bytes_sent += result.total_bytes;
            traffic.send_count += result.send_count;
        }

        qp_info.pending_doorbells.clear();
        qp_info._has_pending = false;

        CU_CHECK(cuStreamDestroy(cu_stream));
        CUcontext dummy;
        CU_CHECK(cuCtxPopCurrent(&dummy));
    }
}
