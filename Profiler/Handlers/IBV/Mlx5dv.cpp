#include "../../ProfilerShim.hpp"
#include "../Common.hpp"
#include "../ProfilerProviders.hpp"

#if SIGMA_HAS_CUDA
#include "Helpers/CudaHelpers.hpp"
#endif

#include "../../Events/Mlx5DvEvents.hpp"
#include "Mlx5dvEventSource.hpp"

#include <infiniband/mlx5dv.h>

// Internal headers for mlx5 type definitions
// TODO: this is inside NVSHMEM. May be NVSHMEM or RDMA-CORE version dependent
#include <mlx5/mlx5_ifc.h>
#include <mlx5/mlx5_prm.h> // Internal headers for mlx5 type definitions

#include "IBVCommon.hpp"

void ProfilerCtx::load_mlx5dv(const std::filesystem::path& shim_dir) {
    m_mlx5dv_profiler = load_provider<mlx5dv::Profiler>(shim_dir / Profiler::SIGMA_MLX5DV_PROVIDER_PATH);

    m_mlx5dv_profiler->register_post<mlx5dv::open_device_tag>([](ibv_context* out_context, ibv_device* device,
                                                                 mlx5dv_context_attr*, const auto ts_start,
                                                                 const auto ts_end) {
        const auto event = register_nic(instance().m_event_recorder.get(), out_context, device);
        instance().m_event_recorder->Record(event, ts_start, ts_end);
    });

    m_mlx5dv_profiler->register_post<mlx5dv::init_obj_tag>([](int, mlx5dv_obj* obj, uint64_t obj_type, auto...) {
        sigma_ignore_unused(obj);

        if (obj_type & MLX5DV_OBJ_QP) {
            // std::cout << "TODO: QP created" << std::endl;
        }

        if (obj_type & MLX5DV_OBJ_CQ) {
            // std::cout << "TODO: CQ created" << std::endl;
        }
    });


#if SIGMA_HAS_CUDA
    m_mlx5dv_profiler->register_post<mlx5dv::obj_create_tag>(
            [](mlx5dv_devx_obj* out_obj, ibv_context*, const void* cmd_in, size_t, void* cmd_out, size_t, auto...) {
        if (!out_obj) {
            return;
        }

        // TODO
        // std::scoped_lock lock(m_ibv_allocations_mutex);

        auto mlx5_cmd_opcode = DEVX_GET(create_qp_in, cmd_in, opcode);

        if (mlx5_cmd_opcode == MLX5_CMD_OP_CREATE_QP) {
            // ================================================
            // const auto wg_qp_id = DEVX_GET(create_qp_in, cmd_in, uid);

            auto& profiler = instance();

            // TODO: move
            const auto out_qpn = DEVX_GET(create_qp_out, cmd_out, qpn);

            // We check here if the UMEM was registered previously with a known GPU ID
            // If not, we don't need to parse all this since they there won't be doorbells
            // TODO: this is probably wrong, UMEM reg GPU discovery can fail
            const auto wq_umem_id = DEVX_GET(create_qp_in, cmd_in, wq_umem_id);
            [[maybe_unused]] const auto wq_umem_offset = DEVX_GET(create_qp_in, cmd_in, wq_umem_offset);

            GpuId_T gpu_id{};
            cudaMemoryType gpu_mem_type{};
            CUcontext gpu_ctx{nullptr};
            void* sq_base{nullptr};
            {
                // TODO: read lock
                std::scoped_lock lock(profiler.m_mlx5_umem_mutex);
                const auto& umem_reg_entry = profiler.m_mlx5_umem_to_entry.find(wq_umem_id);

                // Not an UMEM associated with a GPU. We can ignore this
                if (umem_reg_entry == profiler.m_mlx5_umem_to_entry.end()) {
                    return;
                }

                const auto& umem_gpu_addr = umem_reg_entry->second->first;
                const auto& [umem_gpu_id, umem_gpu_mem_type, umem_gpu_ctx] = umem_reg_entry->second->second;
                gpu_id = umem_gpu_id;
                gpu_mem_type = umem_gpu_mem_type;
                gpu_ctx = umem_gpu_ctx;
                sq_base = reinterpret_cast<void*>(umem_gpu_addr);
            }

            const auto* in_qp_context = DEVX_ADDR_OF(create_qp_in, const_cast<void*>(cmd_in), qpc);
            [[maybe_unused]] const auto* out_qp_context = DEVX_ADDR_OF(create_qp_in, cmd_out, qpc);
            [[maybe_unused]] const auto wq_umem_offset_raw = DEVX_GET64(create_qp_in, cmd_in, wq_umem_offset);

#ifdef SIGMA_DEBUG
            fprintf(stderr, "[create_qp] wq_umem_offset (DEVX_GET64) = %lu (0x%lx)\n", wq_umem_offset_raw,
                    wq_umem_offset_raw);
#endif

            // const auto out_qpn =  DEVX_GET(create_qp_out, cmd_out, qpn);
            const auto log_sq_size = DEVX_GET(qpc, in_qp_context, log_sq_size);
            [[maybe_unused]] const auto log_rq_size = DEVX_GET(qpc, in_qp_context, log_rq_size);
            const auto st = DEVX_GET(qpc, in_qp_context, st); // transport service type (0x5 = DCI)

#ifdef SIGMA_DEBUG
            fprintf(stderr,
                    "[create_qp] qpn=%u wq_umem_id=%u wq_umem_offset=%u "
                    "log_sq_size=%u log_rq_size=%u sq_base=%p\n",
                    out_qpn, wq_umem_id, wq_umem_offset, log_sq_size, log_rq_size, sq_base);
#endif

            CUcontext cuda_ctx{nullptr};
            CU_CHECK(cuCtxGetCurrent(&cuda_ctx));
            if (!cuda_ctx) {
                std::cerr << "CUDA context not found in mlx5dv_obj_create_tag hook" << std::endl;
                std::abort();
            }

            if (cuda_ctx != gpu_ctx) {
                std::cerr << "CONTEXT IS NOT THE SAME" << std::endl;
                std::abort();
            }

            CUdevice dev{-1};
            CU_CHECK(cuCtxGetDevice(&dev));

#ifdef SIGMA_DEBUG
            if (gpu_id != dev) {
                std::cerr << "cuCtxGetDevice device ID differs from previously found umem reg ID" << std::endl;
                std::abort();
            }
#endif

            const QpInfo qp_info{
                    .gpu_ctx = gpu_ctx, // = cuda_ctx
                    .sq_base = static_cast<uint8_t*>(sq_base) + wq_umem_offset_raw,
                    .log_wq_size = log_sq_size,
                    .qpn = out_qpn,
                    .gpu_id = gpu_id,
                    .gpu_mem_type = gpu_mem_type,
                    .service_type = static_cast<uint8_t>(st),
            };
            // TODO:
            {
                const std::scoped_lock lock(profiler.m_mlx5_qp_mutex);
                profiler.m_mlx5_qp_info.insert_or_assign(out_qpn, qp_info);
            }

            // ================================================
            return;
        }

        if (mlx5_cmd_opcode == MLX5_CMD_OP_CREATE_CQ) {
            // ================================================

            // CQ buffer
            const auto cq_umem_id = DEVX_GET(create_cq_in, cmd_in, cq_umem_id);
            sigma_ignore_unused(cq_umem_id);

            // Context
            const auto* cq_context = DEVX_ADDR_OF(create_cq_in, const_cast<void*>(cmd_in), cq_context);

            // BlueFlame
            const auto bf_register_uar_id = DEVX_GET(cqc, cq_context, uar_page);
            sigma_ignore_unused(bf_register_uar_id);

            // Doorbell
            const auto doorbell_register_umem_id = DEVX_GET(cqc, cq_context, dbr_umem_id);
            sigma_ignore_unused(doorbell_register_umem_id);

            // TODO: Doorbell Offset
            const auto cq_doorbell_offset = DEVX_GET64(cqc, cq_context, dbr_addr);
            sigma_ignore_unused(cq_doorbell_offset);
            // ================================================
        }
    });
#endif

    m_mlx5dv_profiler->register_post<mlx5dv::obj_modify_tag>(
            [](int, mlx5dv_devx_obj*, const void* cmd_in, size_t, void*, size_t, auto...) {
        const auto mlx5_cmd_opcode = DEVX_GET(init2rtr_qp_in, cmd_in, opcode);

        if (mlx5_cmd_opcode == MLX5_CMD_OP_INIT2RTR_QP) {
            auto* qp_context = DEVX_ADDR_OF(init2rtr_qp_in, const_cast<void*>(cmd_in), qpc);
            const auto qp_remote_qpn = DEVX_GET(qpc, qp_context, remote_qpn);
            const auto qp_qpn = DEVX_GET(init2rtr_qp_in, cmd_in, qpn);

            // Extract GID and LID from QP context
            // primary_address_path contains dgid, dlid, port
            const auto* pri_path = DEVX_ADDR_OF(qpc, qp_context, primary_address_path);
            const auto dest_lid = static_cast<uint16_t>(DEVX_GET(ads, pri_path, rlid));
            const auto port_num = static_cast<uint8_t>(DEVX_GET(ads, pri_path, vhca_port_num));

            // Extract destination GID (16 bytes at rgid_rip offset)
            const auto* rgid_bytes =
                    reinterpret_cast<const uint8_t*>(DEVX_ADDR_OF(ads, const_cast<uint8_t*>(pri_path), rgid_rip));
            uint64_t dest_subnet_prefix = 0;
            uint64_t dest_interface_id = 0;
            std::memcpy(&dest_subnet_prefix, rgid_bytes, 8);
            std::memcpy(&dest_interface_id, rgid_bytes + 8, 8);
            dest_subnet_prefix = be64toh(dest_subnet_prefix);
            dest_interface_id = be64toh(dest_interface_id);

            // Emit QpConnectionInfo event
            const Profiler::Events::QpConnectionInfo conn_event{
                    .context_addr = 0, // no ibv_context available in mlx5dv path
                    .src_qpn = qp_qpn,
                    .dest_qpn = qp_remote_qpn,
                    .dest_interface_id = dest_interface_id,
                    .dest_subnet_prefix = dest_subnet_prefix,
                    .dest_lid = dest_lid,
                    .port_num = port_num,
            };
            instance().m_event_recorder->Record(conn_event, 0);

            // Store in QpInfo for doorbell correlation
            auto& profiler = instance();
            {
                std::scoped_lock lock(profiler.m_mlx5_qp_mutex);
                auto it = profiler.m_mlx5_qp_info.find(qp_qpn);
                if (it != profiler.m_mlx5_qp_info.end()) {
                    it->second.dest_qpn = qp_remote_qpn;
                    it->second.dest_interface_id = dest_interface_id;
                    it->second.dest_lid = dest_lid;
                }
            }

            // Store connection for shutdown summary
            {
                std::scoped_lock lock(profiler.m_qp_traffic_mutex);
                profiler.m_qp_connections[qp_qpn] = ProfilerCtx::QpConnectionEntry{
                        .context_addr = 0,
                        .dest_qpn = qp_remote_qpn,
                        .dest_interface_id = dest_interface_id,
                        .dest_subnet_prefix = dest_subnet_prefix,
                        .dest_lid = dest_lid,
                        .port_num = port_num,
                };
            }
        }
    });

    // Memory tracking
    m_mlx5dv_profiler->register_post<mlx5dv::devx_alloc_uar_tag>(
            [](mlx5dv_devx_uar* out_uar, ibv_context* ctx, uint32_t flags, const auto ts_start, const auto ts_end) {
        Profiler::Events::Mlx5dvUarAlloc event{
                .context_addr = PtrToUint(ctx),
                .uar_addr = PtrToUint(out_uar),
                .base_addr = PtrToUint(out_uar->base_addr),
                .reg_addr = PtrToUint(out_uar->reg_addr),
                .page_id = out_uar->page_id,
                .flags = flags,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
    });

    // For memory tracking
    m_mlx5dv_profiler->register_pre<mlx5dv::devx_free_uar_tag>([](mlx5dv_devx_uar* devx_uar, const auto ts_start) {
        Profiler::Events::Mlx5dvUarFree event{
                .uar_addr = PtrToUint(devx_uar),
                .page_id = devx_uar->page_id,
        };
        instance().m_event_recorder->Record(event, ts_start);
    });

    m_mlx5dv_profiler->register_post<mlx5dv::devx_umem_reg_tag>(
            [](mlx5dv_devx_umem* out_umem, [[maybe_unused]] ibv_context* ctx, void* addr, size_t size,
               [[maybe_unused]] uint32_t access, const auto ts_start, const auto ts_end) {
#if SIGMA_HAS_CUDA
        cudaPointerAttributes addr_attrs{};
        CUDA_CHECK(cudaPointerGetAttributes(&addr_attrs, addr));

#ifdef SIGMA_DEBUG
        fprintf(stderr, "[umem_reg] umem_id=%u addr=%p size=%zu gpu_mem_type=%d\n", out_umem->umem_id, addr, size,
                addr_attrs.type);
#endif

        if (addr_attrs.type != cudaMemoryTypeUnregistered) {
            std::scoped_lock lock(instance().m_mlx5_umem_mutex);

            CUcontext cu_ctx;
            CU_CHECK(cuCtxGetCurrent(&cu_ctx));

            const auto& [entry, inserted] = instance().m_mlx5_umem_addr_to_gpu.insert_or_assign(
                    PtrToUint(addr), std::make_tuple(addr_attrs.device, addr_attrs.type, cu_ctx));

            if (!inserted) {
                std::cerr << "Duplicate mlx5dv_umem_reg address" << std::endl;
                std::abort();
            }

            instance().m_mlx5_umem_to_entry.insert_or_assign(out_umem->umem_id, entry);
        }
#endif

        instance().m_event_recorder->Record<Profiler::Events::Mlx5dvMemoryRegistration>(
                {
                        .context_addr = PtrToUint(ctx),
                        .umem_addr = PtrToUint(out_umem),
                        .addr = PtrToUint(addr),
                        .size = size,
                        .mem_id = out_umem->umem_id,
                },
                ts_start, ts_end);
    });

    // We can't do register_pre because the pointer will be gone
    m_mlx5dv_profiler->register_pre<mlx5dv::devx_umem_dereg_tag>([](mlx5dv_devx_umem* umem, const auto ts_start) {
        {
            std::scoped_lock lock(instance().m_mlx5_umem_mutex);
            const auto& entry = instance().m_mlx5_umem_to_entry.find(umem->umem_id);
            if (entry != instance().m_mlx5_umem_to_entry.end()) {
                auto addr_entry = entry->second;
                instance().m_mlx5_umem_to_entry.erase(entry);
                instance().m_mlx5_umem_addr_to_gpu.erase(addr_entry);
            }
        }

        instance().m_event_recorder->Record<Profiler::Events::Mlx5dvMemoryDereg>(
                {
                        .umem_addr = PtrToUint(umem),
                        .mem_id = umem->umem_id,
                },
                ts_start);
    });

    std::cout << "[Profiler] loaded mlx5dv Trace" << std::endl;
}
