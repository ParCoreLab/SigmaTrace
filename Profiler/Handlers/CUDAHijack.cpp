#include <mutex>
#include <unordered_map>

#include <cuda.h>

#include "../ProfilerProviders.hpp"
#include "../ProfilerShim.hpp"
#include "Common.hpp"

#include "../Events/CUDAEvents.hpp"
#include "../Events/Mlx5DvEvents.hpp"
#include "CUDAEventSource.hpp"

#include "../Modules/CudaExportTableOverwrite/ExportTableOverwrite.hpp"

// Forward declarations here
namespace Profiler::hijack {
    // TODO: move these into ProfilerCtx
    inline std::mutex m_profiler_hijack_mutex;

    // ================================================
    // NIC UAR regaddresses allocated with mlx5dv_devx_alloc_uar
    // uar->reg_addr -> uar->page_id
    inline std::unordered_map<void*, UarPageIdType> Mlx5UarAllocations{};

    // Host addresses registered with cuMemHostRegister with CU_MEMHOSTREGISTER_DEVICEMAP
    // These are the uar->reg_addr from above
    inline std::unordered_set<void*> CuMemHostRegisterAddrs{};

    // Fake host pointers we create with repeaters
    // Real UAR address (uar->reg_addr) -> Fake UAR address (cudaHostAlloc-ed page)
    inline std::unordered_map<void*, void*> CuFakeUarPtrs{};
    // ================================================

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

    // ================================================
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

        // const auto ts = TimestampSource::now()...

        // Should always be 64 bytes in mlx5
        constexpr auto WQEBB_SIZE{64};
        const auto wq_mask = (1u << qp_info.log_wq_size) - 1u;

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

                const uint32_t num_wqebbs_total = prod_id_end - prod_id_start;
                const uint32_t start_slot = prod_id_start & wq_mask;
                const uint32_t wq_slots = 1u << qp_info.log_wq_size;

                thread_local CUstream cu_stream = std::invoke([&qp_info]() {
                    CUstream s;
                    CU_CHECK(cuCtxPushCurrent(qp_info.cuda_ctx));
                    CU_CHECK(cuStreamCreate(&s, CU_STREAM_NON_BLOCKING));

                    CUcontext empty;
                    CU_CHECK(cuCtxPopCurrent(&empty));
                    return s;
                });

                // TODO: better size calculation
                // this is kinda random
                thread_local uint8_t host_wqe_buf[1024 * WQEBB_SIZE * 64]{};

                // Handle ring wrap-around with two copies
                if (start_slot + num_wqebbs_total <= wq_slots) {
                    CU_CHECK(cuMemcpyDtoHAsync(host_wqe_buf,
                                               reinterpret_cast<CUdeviceptr>(qp_info.sq_base) + start_slot * WQEBB_SIZE,
                                               num_wqebbs_total * WQEBB_SIZE, cu_stream));
                } else {
                    const uint32_t tail = wq_slots - start_slot;
                    const uint32_t head = num_wqebbs_total - tail;
                    CU_CHECK(cuMemcpyDtoHAsync(host_wqe_buf,
                                               reinterpret_cast<CUdeviceptr>(qp_info.sq_base) + start_slot * WQEBB_SIZE,
                                               tail * WQEBB_SIZE, cu_stream));
                    CU_CHECK(cuMemcpyDtoHAsync(host_wqe_buf + tail * WQEBB_SIZE,
                                               reinterpret_cast<CUdeviceptr>(qp_info.sq_base), head * WQEBB_SIZE,
                                               cu_stream));
                }
                CU_CHECK(cuStreamSynchronize(cu_stream));

                uint64_t batch_bytes_sent = 0;

                // Per-WQE byte counts keyed by end prod_id (for matching with pending doorbells)
                std::unordered_map<uint32_t, uint64_t> wqe_bytes_by_end_prod_id;

                for (uint32_t i = 0, prod_id = prod_id_start; prod_id < prod_id_end;) {
                    const uint8_t* wqe = host_wqe_buf + i * WQEBB_SIZE;
                    const auto* ctrl = reinterpret_cast<const mlx5_wqe_ctrl_seg*>(wqe);

                    const uint32_t wqe_qpn_ds = be32toh(ctrl->qpn_ds);
                    const uint32_t wqe_ds = wqe_qpn_ds & 0xFF;
                    const uint32_t wqe_qpn = wqe_qpn_ds >> 8;

                    if (wqe_qpn != dbr_qpn) {
                        std::cerr << "[i=" << i << " prod_id = " << prod_id << "] WRONG QPN " << wqe_qpn << " vs "
                                  << dbr_qpn << std::endl;
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
                        if (qp_info.service_type == 0x5 && remaining_ds >= 4) {
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
                            const uint32_t byte_count =
                                    be32toh(*reinterpret_cast<const uint32_t*>(seg_ptr)) & 0x7FFFFFFF;
                            wqe_bytes += byte_count;
                            seg_ptr += 16;
                            remaining_ds -= 1;
                        }
                    }

                    batch_bytes_sent += wqe_bytes;

                    // ds is in 16B units; advance by whole WQEBBs (64B each = 4 ds units)
                    const uint32_t wqebbs = (wqe_ds + 3) / 4;
                    i += wqebbs;
                    prod_id += wqebbs;

                    // Map end prod_id to byte count for doorbell correlation
                    wqe_bytes_by_end_prod_id[prod_id] = wqe_bytes;
                }

                // Accumulate doorbell-path bytes
                if (batch_bytes_sent > 0) {
                    std::scoped_lock lock(profiler.m_qp_traffic_mutex);
                    profiler.m_qp_traffic[dbr_qpn].bytes_sent += batch_bytes_sent;
                }

                // Emit per-doorbell DoorbellRing events with their original timestamps and computed bytes
                for (const auto& pending: qp_info.pending_doorbells) {
                    uint64_t doorbell_bytes = 0;
                    const auto it = wqe_bytes_by_end_prod_id.find(pending.prod_id);
                    if (it != wqe_bytes_by_end_prod_id.end()) {
                        doorbell_bytes = it->second;
                    }

                    const Profiler::Events::DoorbellRing event{
                            .addr = 0,
                            .prod_index = pending.prod_id,
                            .qp_num = dbr_qpn,
                            .gpu_id = qp_info.gpu_id,
                            .is_cpu_hijack = true,
                            .bytes_sent = doorbell_bytes,
                    };
                    profiler.m_event_recorder->Record(event, pending.timestamp);
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

        MemoryRepeaterHandle handle{static_cast<uint64_t*>(fake_reg_addr), reg_addr_real, on_doorbell_ring};

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
    // ================================================================================================

    // 1. Fake UAR hijack
    m_mlx5dv_profiler->register_post<mlx5dv::devx_alloc_uar_tag>(
            [](mlx5dv_devx_uar* out_uar, ibv_context*, uint32_t, auto...) {
        std::scoped_lock lock(Profiler::hijack::m_profiler_hijack_mutex);
        Profiler::hijack::Mlx5UarAllocations.insert_or_assign(out_uar->reg_addr, out_uar->page_id);
    });

    // 1. For hijack
    m_mlx5dv_profiler->register_pre<mlx5dv::devx_free_uar_tag>([](mlx5dv_devx_uar* devx_uar, auto...) {
        std::scoped_lock lock(Profiler::hijack::m_profiler_hijack_mutex);

        auto& profiler = instance();
        // CPU path: signal finish on MemoryRepeaterHandles (existing behavior)
        const auto& handles = profiler.m_mlx5_uar_hijack_handles.find(devx_uar->page_id);
        if (handles != profiler.m_mlx5_uar_hijack_handles.end()) {
            for (auto& h: handles->second) {
                h.signal_finish();
            }
            profiler.m_mlx5_uar_hijack_handles.erase(handles);
        }

        Profiler::hijack::Mlx5UarAllocations.erase(devx_uar->reg_addr);
    });
}
