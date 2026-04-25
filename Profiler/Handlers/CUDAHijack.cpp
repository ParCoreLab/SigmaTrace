#include <condition_variable>
#include <memory>
#include <mutex>
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
    inline std::unordered_map<void*, std::pair<AddrType, UarPageIdType>> Mlx5UarAllocations{};

    // Host addresses registered with cuMemHostRegister with CU_MEMHOSTREGISTER_DEVICEMAP
    // These are the uar->reg_addr from above
    inline std::unordered_set<void*> CuMemHostRegisterAddrs{};

    // Fake host pointers we create with repeaters
    // Real UAR address (uar->reg_addr) -> Fake UAR address (cudaHostAlloc-ed page)
    inline std::unordered_map<void*, void*> CuFakeUarPtrs{};
    // ================================================================

    // Called from pthread_atfork child handler.
    // Reinitializes the mutex (POSIX: mutex state is undefined after fork)
    // and clears stale parent-process data from the maps.
    // Forward-declared in ProfilerShim.cpp.
    void reset();
    void reset() {
        new (&m_profiler_hijack_mutex) std::mutex();
        Mlx5UarAllocations.clear();
        CuMemHostRegisterAddrs.clear();
        CuFakeUarPtrs.clear();
    }

    // ================================================================
    // WQE parsing helpers
    // ================================================================
    constexpr uint32_t WQEBB_SIZE = 64;
    constexpr uint32_t PROD_ID_MASK = 0xFFFF;

    static uint32_t prod_id_diff_16(const uint32_t end, const uint32_t start) {
        return (end - start) & PROD_ID_MASK;
    }

    // Opcode classification via bit-set lookup (avoids branch chains on the hot path).
    // Bit N is set if opcode N has that property.
    // has_raddr:     RDMA_WRITE(0x08) RDMA_READ(0x10) ATOMIC_CS(0x11) ATOMIC_FA(0x12)
    //               ATOMIC_MASKED_CS(0x14) ATOMIC_MASKED_FA(0x15)
    // has_data_segs: RDMA_WRITE(0x08) SEND(0x0A) RDMA_READ(0x10)
    // is_data:       RDMA_WRITE(0x08) SEND(0x0A) RDMA_READ(0x10)
    static constexpr uint32_t OPCODE_HAS_RADDR =
            (1u << 0x08) | (1u << 0x10) | (1u << 0x11) | (1u << 0x12) | (1u << 0x14) | (1u << 0x15);
    static constexpr uint32_t OPCODE_HAS_DATA = (1u << 0x08) | (1u << 0x0A) | (1u << 0x10);

    static bool opcode_has_raddr(uint8_t op) {
        return op < 32 && (OPCODE_HAS_RADDR >> op) & 1;
    }
    static bool opcode_has_data_segs(uint8_t op) {
        return op < 32 && (OPCODE_HAS_DATA >> op) & 1;
    }
    [[maybe_unused]] static bool opcode_is_data(uint8_t op) {
        return op < 32 && (OPCODE_HAS_DATA >> op) & 1;
    }

    struct TrafficAccum {
        uint64_t total_bytes{0};
        uint32_t send_count{0};
    };

    // When true, only emit DoorbellRingControl events for masked atomics
    // (ATOMIC_MASKED_CS 0x14 / ATOMIC_MASKED_FA 0x15).  These are the opcodes
    // NVSHMEM uses for both barrier signaling AND application-level atomics
    // (e.g. nvshmem_int_atomic_compare_swap).  Each atomic is emitted
    // individually (not aggregated per doorbell) so that filtering by
    // remote_addr in SQL can isolate barrier events from application atomics.
    // Data doorbells and NOPs are skipped. Traffic accounting is still maintained.
    static constexpr bool BarrierOnly = false;

    // When false, will copy every WQE as they come without
    // deferring batching for later. This will be slower
    // but will catch every doorbell write
    static constexpr bool BatchUar = true;

    // Masked-atomic opcodes: ATOMIC_MASKED_CS (0x14) and ATOMIC_MASKED_FA (0x15).
    static constexpr uint32_t OPCODE_IS_MASKED_ATOMIC = (1u << 0x14) | (1u << 0x15);
    static bool opcode_is_masked_atomic(uint8_t op) {
        return op < 32 && (OPCODE_IS_MASKED_ATOMIC >> op) & 1;
    }

    static void parse_and_emit(const uint8_t* host_buf, uint32_t prod_id_start, uint32_t prod_id_end, uint32_t qpn,
                               uint8_t service_type, Events::GpuId_T gpu_id,
                               const std::vector<PendingDoorbell>& pending_doorbells,
                               ::Events::EventRecorder<>& recorder, TrafficAccum& traffic) {
        if (pending_doorbells.empty()) {
            return;
        }

        const uint32_t num_wqebbs = prod_id_diff_16(prod_id_end, prod_id_start);
        const bool is_dci = (service_type == 0x5);

        uint32_t i = 0;
        uint32_t prod_id = prod_id_start;
        size_t db_idx = 0;

        // Per-doorbell accumulators — reset on each flush_doorbell
        uint64_t data_bytes = 0;
        uint8_t data_opcode = 0;
        uint8_t ctrl_opcode = 0;
        uint64_t ctrl_raddr = 0;

        auto flush_doorbell = [&](size_t idx) {
            const auto& db = pending_doorbells[idx];
            if constexpr (BarrierOnly) {
                // if (ctrl_opcode == 20 && (ctrl_raddr - 8 * gpu_id) == 26317160960) {
                if (ctrl_opcode == 20) {
                    recorder.Record(
                            Events::DoorbellRingControl{
                                    .prod_index = db.prod_id,
                                    .qp_num = qpn,
                                    .gpu_id = gpu_id,
                                    .opcode = ctrl_opcode,
                                    .remote_addr = ctrl_raddr,
                            },
                            db.timestamp);
                }
            } else {
                if (data_bytes > 0 || data_opcode != 0) {
                    recorder.Record(
                            Events::DoorbellRingData{
                                    .prod_index = db.prod_id,
                                    .qp_num = qpn,
                                    .gpu_id = gpu_id,
                                    .bytes_sent = data_bytes,
                                    .opcode = data_opcode,
                            },
                            db.timestamp);
                }
                // if (ctrl_opcode == 20 && (ctrl_raddr - 8 * gpu_id) == 26317160960) {
                if (ctrl_opcode == 20) {
                    recorder.Record(
                            Events::DoorbellRingControl{
                                    .prod_index = db.prod_id,
                                    .qp_num = qpn,
                                    .gpu_id = gpu_id,
                                    .opcode = ctrl_opcode,
                                    .remote_addr = ctrl_raddr,
                            },
                            db.timestamp);
                }
            }
            data_bytes = 0;
            data_opcode = 0;
            ctrl_opcode = 0;
            ctrl_raddr = 0;
        };

        while (i < num_wqebbs) {
            const uint8_t* wqe = host_buf + i * WQEBB_SIZE;
            const uint32_t wqe_qpn_ds = be32toh(reinterpret_cast<const mlx5_wqe_ctrl_seg*>(wqe)->qpn_ds);
            const uint32_t wqe_ds = wqe_qpn_ds & 0xFF;

            if ((wqe_qpn_ds >> 8) != qpn || wqe_ds == 0) {
                ++i;
                prod_id = (prod_id + 1) & PROD_ID_MASK;
                // Advance doorbell cursor past skipped prod_id
                while (db_idx < pending_doorbells.size()) {
                    const auto diff = prod_id_diff_16(prod_id, pending_doorbells[db_idx].prod_id);
                    if (diff == 0 || diff > 0x7FFF) {
                        break;
                    }
                    flush_doorbell(db_idx);
                    ++db_idx;
                }
                if (db_idx >= pending_doorbells.size()) {
                    break;
                }
                continue;
            }

            const uint8_t opcode = be32toh(reinterpret_cast<const mlx5_wqe_ctrl_seg*>(wqe)->opmod_idx_opcode) & 0xFF;
            const uint32_t wqebbs = (wqe_ds + 3) >> 2;
            i += wqebbs;
            prod_id = (prod_id + wqebbs) & PROD_ID_MASK;

            const bool has_raddr = opcode_has_raddr(opcode);
            const bool has_data = opcode_has_data_segs(opcode);

            // In BarrierOnly mode, only parse segments for masked atomics
            const bool need_parse =
                    BarrierOnly ? (has_raddr && opcode_is_masked_atomic(opcode)) : (has_raddr || has_data);

            uint64_t wqe_bytes = 0;
            uint64_t wqe_raddr = 0;

            if (need_parse && wqe_ds > 1) {
                const uint8_t* seg_ptr = wqe + 16;
                uint32_t remaining_ds = wqe_ds - 1;

                if (is_dci && remaining_ds >= 4) {
                    seg_ptr += 64;
                    remaining_ds -= 4;
                }

                if (has_raddr && remaining_ds >= 1) {
                    wqe_raddr = be64toh(*reinterpret_cast<const uint64_t*>(seg_ptr));
                    seg_ptr += 16;
                    --remaining_ds;
                }

                if constexpr (!BarrierOnly) {
                    if (has_data) {
                        for (; remaining_ds; --remaining_ds, seg_ptr += 16)
                            wqe_bytes += be32toh(*reinterpret_cast<const uint32_t*>(seg_ptr)) & 0x7FFFFFFF;
                    }
                }
            }

            traffic.total_bytes += wqe_bytes;
            traffic.send_count += has_data;

            // Advance doorbell cursor: flush when we cross a boundary
            while (db_idx < pending_doorbells.size()) {
                const auto diff = prod_id_diff_16(prod_id, pending_doorbells[db_idx].prod_id);
                if (diff == 0 || diff > 0x7FFF) {
                    break;
                }
                flush_doorbell(db_idx);
                ++db_idx;
            }
            if (db_idx >= pending_doorbells.size())
                break;

            // Accumulate this WQE into per-doorbell state
            if constexpr (BarrierOnly) {
                if (opcode_is_masked_atomic(opcode)) {
                    if (ctrl_opcode != 20) {
                        ctrl_opcode = opcode;
                        ctrl_raddr = wqe_raddr;
                    }
                }
            } else {
                if (has_data) {
                    data_bytes += wqe_bytes;
                    data_opcode = opcode;
                } else if (has_raddr) {
                    ctrl_opcode = opcode;
                    ctrl_raddr = wqe_raddr;
                }
            }
        }

        // Flush last doorbell
        if (db_idx < pending_doorbells.size()) {
            flush_doorbell(db_idx);
        }
    }

    static CUresult copy_wqes_from_gpu(uint8_t* dst, const QpInfo& qp_info, uint32_t prod_id_start,
                                       uint32_t prod_id_end, CUstream stream) {
        const uint32_t num_wqebbs_total = prod_id_diff_16(prod_id_end, prod_id_start);
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
    static void on_doorbell_ring(const uint64_t new_val) {
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

            // Lazy-init per-QP copy stream and host buffer
            if (!qp_info.copy_stream) {
                CU_CHECK(cuCtxPushCurrent(qp_info.gpu_ctx));
                CUstream s;
                CU_CHECK(cuStreamCreate(&s, CU_STREAM_NON_BLOCKING));
                qp_info.copy_stream = s;
                void* raw_ptr = nullptr;
                CU_CHECK(cuMemAllocHost(&raw_ptr, (1u << qp_info.log_wq_size) * WQEBB_SIZE * 4));
                qp_info.host_wqe_buf = static_cast<uint8_t*>(raw_ptr);
            }

            // Async-copy this doorbell's delta WQEBBs into host buffer.
            // This runs BEFORE the NIC write (callback-before-forward in
            // MemoryRepeater), so the SQ data is guaranteed valid.
            const auto delta_wqebbs = prod_id_diff_16(dbr_prod_id, last_prod_id);
            if (delta_wqebbs > 0) {
                const auto buf_offset = prod_id_diff_16(last_prod_id, qp_info._oldest_uncopied_prod_id);
                copy_wqes_from_gpu(qp_info.host_wqe_buf + buf_offset * WQEBB_SIZE, qp_info, last_prod_id, dbr_prod_id,
                                   static_cast<CUstream>(qp_info.copy_stream));
                // No sync here — just enqueue. Sync only at batch time.
            }

            constexpr auto Batch_Threshold{128};
            const auto distance = prod_id_diff_16(dbr_prod_id, qp_info._oldest_uncopied_prod_id);

            if (!BatchUar || distance >= Batch_Threshold) {
                const auto prod_id_start = qp_info._oldest_uncopied_prod_id;
                const auto prod_id_end = dbr_prod_id;
                auto* cu_stream = static_cast<CUstream>(qp_info.copy_stream);

                // All per-doorbell async copies are on this stream — one sync gets them all
                CU_CHECK(cuStreamSynchronize(cu_stream));

                TrafficAccum accum;
                parse_and_emit(qp_info.host_wqe_buf, prod_id_start, prod_id_end, dbr_qpn, qp_info.service_type,
                               qp_info.gpu_id, qp_info.pending_doorbells, *profiler.m_event_recorder, accum);
                {
                    auto& traffic = profiler.m_traffic_registry.get_thread_local()[dbr_qpn];
                    traffic.bytes_sent += accum.total_bytes;
                    traffic.send_count += accum.send_count;
                }

                qp_info.pending_doorbells.clear();
                qp_info._oldest_uncopied_prod_id = dbr_prod_id;
                qp_info._has_pending = false;

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

        MemoryRepeaterHandle handle{static_cast<uint64_t*>(fake_reg_addr), reg_addr_real, [](uint64_t new_val) {
            on_doorbell_ring(new_val);
        }};

        const auto [uar_addr, page_id] = Mlx5UarAllocations.at(p);
        auto& hijack_handles = profiler.m_mlx5_uar_hijack_handles[{p, page_id}];
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
        std::pair<AddrType, UarPageIdType> val{PtrToUint(out_uar), out_uar->page_id};
        Profiler::hijack::Mlx5UarAllocations.insert_or_assign(out_uar->reg_addr, std::move(val));
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

        const auto uar_alloation_entry = Profiler::hijack::Mlx5UarAllocations.find(devx_uar->reg_addr);
        if (uar_alloation_entry == Profiler::hijack::Mlx5UarAllocations.end()) {
            // std::abort();
            return;
        }

        if (uar_alloation_entry->second.first != PtrToUint(devx_uar) ||
            uar_alloation_entry->second.second != devx_uar->page_id) {
            return;
        }

        // Signal the MemoryRepeater to stop polling, then flush remaining
        // pending doorbells. We use signal_finish() (non-blocking) rather
        // than erasing the handle here because erasing joins the thread,
        // and the thread may be blocked in on_doorbell_ring ->
        // cuStreamSynchronize, which would deadlock.
        const auto handles_it = profiler.m_mlx5_uar_hijack_handles.find({devx_uar->reg_addr, devx_uar->page_id});
        if (handles_it != profiler.m_mlx5_uar_hijack_handles.end()) {
            for (auto& h: handles_it->second) {
                h.signal_finish();
            }
        }

        // The polling thread has been told to stop.  It will exit after its
        // current iteration (possibly one last on_doorbell_ring callback).
        // Flush whatever is pending - this picks up everything including
        // any final doorbell the thread may have just delivered.
        profiler.flush_pending_doorbells();

        Profiler::hijack::Mlx5UarAllocations.erase(devx_uar->reg_addr);
    });

    std::cout << "[Profiler] Loaded Doorbell Hijack Trace" << std::endl;
}

// Flush all pending doorbell batches so bytes aren't lost.
// Called from sync hooks and before emit_communication_summaries() at shutdown.
void ProfilerCtx::flush_pending_doorbells() {
    std::scoped_lock qp_lock(m_mlx5_qp_mutex);

    for (auto& [qpn, qp_info]: m_mlx5_qp_info) {
        if (!qp_info._has_pending || qp_info.pending_doorbells.empty()) {
            continue;
        }

        if (qp_info.gpu_mem_type != cudaMemoryTypeDevice) {
            // TODO
            continue;
        }

        const auto prod_id_start = qp_info._oldest_uncopied_prod_id;
        const auto prod_id_end = qp_info._last_prod_id;

        if (prod_id_start == prod_id_end) {
            continue;
        }

        // Per-doorbell async copies already landed in qp_info.host_wqe_buf.
        // Just sync and parse.
        if (!qp_info.copy_stream || !qp_info.host_wqe_buf) {
            continue;
        }

        auto* cu_stream = static_cast<CUstream>(qp_info.copy_stream);
        if (cuStreamSynchronize(cu_stream) != CUDA_SUCCESS) {
            continue;
        }

        Profiler::hijack::TrafficAccum accum;
        Profiler::hijack::parse_and_emit(qp_info.host_wqe_buf, prod_id_start, prod_id_end, qpn, qp_info.service_type,
                                         qp_info.gpu_id, qp_info.pending_doorbells, *m_event_recorder, accum);
        {
            auto& traffic = m_traffic_registry.get_thread_local()[qpn];
            traffic.bytes_sent += accum.total_bytes;
            traffic.send_count += accum.send_count;
        }

        qp_info.pending_doorbells.clear();
        qp_info._has_pending = false;
    }
}
