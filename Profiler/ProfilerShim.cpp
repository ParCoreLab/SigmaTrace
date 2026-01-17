#include <atomic>
#include <map>
#include <tuple>
#include <type_traits>
#include <vector>

#include <cuda_runtime.h>
#include "Helpers/CudaHelpers.hpp"

#include "EventRecorder.hpp"
#include "EventTrait.hpp"
#include "Events.hpp"

// Profiler Event definitions
#include "Events/Mlx5DvEvents.hpp"

#include "infiniband/mlx5dv.h"
#include "infiniband/verbs.h"

// TODO: this is inside NVSHMEM. May be NVSHMEM or RDMA-CORE version dependent
#include "mlx5/mlx5_ifc.h"
#include "mlx5/mlx5_prm.h"

#include "CUDAEventSource.hpp"
#include "CUDAEvents.hpp"
#include "IbverbsEventSource.hpp"
#include "Mlx5dvEventSource.hpp"

using Profiler::Events::GpuId_T;

enum class QueueType {
    Unknown,
    WQ,
    CQ,
};

enum class UmemAllocationType {
    Unknown = 0,
    WQE,
    // TODO: BlueFlame, not BlueField
    BlueField,
    Doorbell,
};

enum class UarAllocationType {
    Unknown = 0,
    WQE, // Invalid
    BlueField,
    Doorbell, // Invalid
};

template<typename EnumT>
static std::string_view alloc_type_as_string(EnumT type) {
    switch (type) {
        case EnumT::WQE:
            return "WQE";
        case EnumT::BlueField:
            return "BlueField";
        case EnumT::Doorbell:
            return "Doorbell";
        default:
            return "Unknown";
    }
}

// TODO: for debugging
static int find_gpu_index(const uint64_t addr) {
    cudaPointerAttributes attrs{};
    CUDA_CHECK(cudaPointerGetAttributes(&attrs, reinterpret_cast<const void*>(addr)));

    if (attrs.type == cudaMemoryTypeUnregistered) {
        return -1;
    }

    assert(attrs.device > 0 && "Returning invalid device ID from get_gpu_index");
    return attrs.device;
}

struct ProfilerCtx {
    // TODO: rename
    std::mutex m_ibv_allocations_mutex;

    // ================================================
    // GPU memory allocaations
    // addr -> { size, gpu_id }
    // TODO: Make the value a proper struct so we don't have to do std::get<whatever->second>
    std::map<uintptr_t, std::tuple<size_t, int>> m_gpu_allocations;
    // ================================================

    // ================================================
    using AddrType = uint64_t;

    // see mlx5_ifc.h
    using UmemIdType = std::remove_extent_t<decltype(mlx5_ifc_create_qp_in_bits::wq_umem_id)>;
    static_assert(sizeof(UmemIdType) == sizeof(uint8_t));

    using UarPageIdType = std::remove_extent_t<decltype(mlx5dv_devx_uar::page_id)>;
    static_assert(sizeof(UarPageIdType) == sizeof(uint32_t));

    // Registered umem and uar IDs to its buffer type
    std::map<UmemIdType, std::tuple<UmemAllocationType, QueueType>> m_umem_id_to_type;
    std::map<UarPageIdType, std::tuple<UarAllocationType, QueueType>> m_uar_id_to_type;

    // <addr, length, umem ID>
    std::vector<std::tuple<AddrType, size_t, UmemIdType>> m_mlx5_umem_allocations;
    std::map<AddrType, UarPageIdType> m_mlx5_uar_allocations;
    // ================================================

    // ================================================
    // Stats
    std::atomic<int> num_nic_accesses{0};

    std::atomic<int> num_kernel_launches{0};
    // ================================================

    ~ProfilerCtx() {
        std::cout << "[Profiler] num_nic_accesses: " << std::dec << num_nic_accesses << std::endl;
        std::cout << "[Profiler] num_kernel_launches: " << std::dec << num_kernel_launches << std::endl;
    }

    ProfilerCtx() {
        m_ibv_profiler = &ibv::Profiler::instance();
        m_cuda_profiler = &cuda_trace::NvbitProfiler::instance();
        m_mlx5dv_profiler = &mlx5dv::Profiler::instance();

        m_event_recorder = &Events::EventRecorder<>::instance();

        // TODO: pass this along
        m_cuda_profiler->register_pre<cuda_trace::cuda_mem_alloc_tag>(
                [this](uint64_t addr, std::size_t size, auto&&...) {
            // std::cout << "GPU Mem allocation hit at " << std::hex << addr << std::dec << std::endl;

            // auto gpu_id = find_gpu_index(addr);
            // m_gpu_allocations[addr] = {size, gpu_id};
        });

        // TODO: pass this along
        m_cuda_profiler->register_pre<cuda_trace::cuda_mem_register_tag>(
                [this](uint64_t addr, std::size_t size, unsigned int flags, auto&&...) {
            // std::cout << "GPU Mem registration hit at " << std::hex << addr << std::dec << std::endl;

            // auto gpu_id = find_gpu_index(addr);
            // m_gpu_allocations[addr] = {size, gpu_id};
        });


        m_mlx5dv_profiler->register_post<mlx5dv::init_obj_tag>(
                [](auto&&, mlx5dv_obj* obj, uint64_t obj_type, auto&&...) {
            if (obj_type & MLX5DV_OBJ_QP) {
                std::cout << "TODO: QP created" << std::endl;
            }

            if (obj_type & MLX5DV_OBJ_CQ) {
                std::cout << "TODO: CQ created" << std::endl;
            }
        });

        m_mlx5dv_profiler->register_post<mlx5dv::obj_create_tag>([this](mlx5dv_devx_obj* out_obj, ibv_context* context,
                                                                        const void* cmd_in, size_t inlen, void* out,
                                                                        size_t outlen, auto&&...) {
            assert(out_obj && "mlx5dv::obj_create_tag resulted in null");

            // TODO
            // std::scoped_lock lock(m_ibv_allocations_mutex);

            auto mlx5_cmd_opcode = DEVX_GET(create_qp_in, cmd_in, opcode);

            if (mlx5_cmd_opcode == MLX5_CMD_OP_CREATE_QP) {
                // ================================================
                constexpr auto queue_type = QueueType::WQ;

                // WQ buffer
                auto wg_umem_id = DEVX_GET(create_qp_in, cmd_in, wq_umem_id);
                m_umem_id_to_type[wg_umem_id] = {UmemAllocationType::WQE, queue_type};

                // TODO: what is this
                auto* qp_context = DEVX_ADDR_OF(create_qp_in, cmd_in, qpc);

                // BlueField register. TODO: what is BlueField?
                // TODO: We get this from mlx5dv_devx_alloc_uar too
                auto bf_register_uar_id = DEVX_GET(qpc, qp_context, uar_page);
                m_uar_id_to_type[bf_register_uar_id] = {UarAllocationType::BlueField, queue_type};

                // Doorbell register
                auto doorbell_register_umem_id = DEVX_GET(qpc, qp_context, dbr_umem_id);
                m_umem_id_to_type[doorbell_register_umem_id] = {UmemAllocationType::Doorbell, queue_type};

                // TODO: Doorbell Offset
                auto wq_doorbell_offset = DEVX_GET64(qpc, qp_context, dbr_addr);
                // ================================================
                return;
            }

            if (mlx5_cmd_opcode == MLX5_CMD_OP_CREATE_CQ) {
                // ================================================
                constexpr auto queue_type = QueueType::CQ;

                // CQ buffer
                const auto cq_umem_id = DEVX_GET(create_cq_in, cmd_in, cq_umem_id);
                m_umem_id_to_type[cq_umem_id] = {UmemAllocationType::WQE, queue_type};

                // Context
                const auto* cq_context = DEVX_ADDR_OF(create_cq_in, cmd_in, cq_context);

                // BlueFlame
                const auto bf_register_uar_id = DEVX_GET(cqc, cq_context, uar_page);
                m_uar_id_to_type[bf_register_uar_id] = {UarAllocationType::BlueField, queue_type};

                // Doorbell
                const auto doorbell_register_umem_id = DEVX_GET(cqc, cq_context, dbr_umem_id);
                m_umem_id_to_type[doorbell_register_umem_id] = {UmemAllocationType::Doorbell, queue_type};

                // TODO: Doorbell Offset
                const auto cq_doorbell_offset = DEVX_GET64(cqc, cq_context, dbr_addr);
                // ================================================
            }
        });

        m_mlx5dv_profiler->register_post<mlx5dv::devx_alloc_uar_tag>(
                [this](mlx5dv_devx_uar* out_uar, ibv_context* context, uint32_t flags, auto&&...) {
            auto uar_reg_addr = reinterpret_cast<uint64_t>(out_uar->reg_addr);
            // auto uar_base_addr = reinterpret_cast<uint64_t>(out_uar->base_addr);

            // TODO: not checking existence
            m_mlx5_uar_allocations[uar_reg_addr] = out_uar->page_id;

            /*
             * flags -> https://man7.org/linux/man-pages/man3/mlx5dv_devx_alloc_uar.3.html
             *       Allocation flags for the UAR.
             *       MLX5DV_UAR_ALLOC_TYPE_BF: Allocate UAR with Blueflame properties.
             *       MLX5DV_UAR_ALLOC_TYPE_NC: Allocate UAR with non-cache properties.
             *       MLX5DV_UAR_ALLOC_TYPE_NC_DEDICATED: Allocate a dedicated UAR with non-cache properties.
             */

            if (!(flags & MLX5DV_UAR_ALLOC_TYPE_BF)) {
                // std::cerr << "[Profiler] mlx5dv_devx_alloc_uar with non-BlueField flag" << std::endl;
            }
        });

        m_mlx5dv_profiler->register_pre<mlx5dv::devx_free_uar_tag>([this](mlx5dv_devx_uar* devx_uar, auto&&...) {
            std::scoped_lock lock(m_ibv_allocations_mutex);

            const auto& uar_alloc = m_mlx5_uar_allocations.find(devx_uar->page_id);
            if (uar_alloc == m_mlx5_uar_allocations.end()) {
                // std::cerr << "[Profiler] mlx5dv_devx_free_uar called with nonexistent page_id (" << devx_uar->page_id
                // << ")" << std::endl;
                return;
            }

            m_mlx5_uar_allocations.erase(uar_alloc);
        });

        m_mlx5dv_profiler->register_post<mlx5dv::devx_umem_reg_tag>(
                [this]<typename TS>(mlx5dv_devx_umem* out_umem, ibv_context* ctx, void* addr, size_t size,
                                    uint32_t access, TS ts_start, TS ts_end) {
            auto umem_addr = reinterpret_cast<uint64_t>(addr);

            // TODO: extra copy because Record(const T&)
            m_event_recorder->Record<Profiler::Events::Mlx5MemoryRegistration>(
                    {
                            .gpu_id = 0,
                            .nic_id = 0,
                            .addr = umem_addr,
                            .size = size,
                    },
                    ts_start, ts_end);

            std::scoped_lock lock(m_ibv_allocations_mutex);

            m_mlx5_umem_allocations.emplace_back(umem_addr, size, out_umem->umem_id);
        });

        m_mlx5dv_profiler->register_pre<mlx5dv::devx_umem_dereg_tag>([this](mlx5dv_devx_umem* umem, auto&&...) {
            std::scoped_lock lock(m_ibv_allocations_mutex);

            for (auto it = m_mlx5_umem_allocations.begin(); it != m_mlx5_umem_allocations.end(); ++it) {
                if (std::get<2>(*it) == umem->umem_id) {
                    m_mlx5_umem_allocations.erase(it);
                    return;
                }
            }

            std::cerr << "[Profiler] mlx5dv_devx_umem_dereg called with nonexistent umem_id (" << umem->umem_id << ")"
                      << std::endl;
        });


        m_ibv_profiler->register_post<ibv::open_device_tag>([](auto&&, ibv_device*, auto&&...) {
            std::cout << "[Profiler] ibv_open_device received" << std::endl;
        });

        /* IBV stuff unused for now
        m_ibv_profiler->register_post<ibv::reg_mr_tag>(
                [this](ibv_mr *out_mr, ibv_pd *pd, void *addr, size_t length, int access) {
                    auto alloc_addr = reinterpret_cast<uint64_t>(addr);
                    std::cout << "[Profiler] ibv_reg_mr received at " << std::hex << alloc_addr << std::endl;
                    // m_ibv_allocations.emplace_back(alloc_addr, length);
                });

        m_ibv_profiler->register_post<ibv::reg_mr_iova2_tag>(
                [this](ibv_mr *out_mr, ibv_pd *pd, void *addr, size_t length, auto &&...) {
                    auto alloc_addr = reinterpret_cast<uint64_t>(addr);
                    std::cout << "[Profiler] ibv_reg_mr_iova1232 received at " << std::hex << alloc_addr << std::endl;
                    // m_ibv_allocations.emplace_back(alloc_addr, length);
                });
        */

        m_cuda_profiler->register_pre<cuda_trace::cuda_kernel_launch_tag>(
                [this](GpuId_T gpu_id, const char* kernel_name, auto&&...) {
            // std::cout << "[Profiler] Kernel launched -> " << gpu_id << std::endl;

            // TODO: :(
            Profiler::Events::CUDAKernelLaunch kernel_launch_data{.gpu_id = gpu_id};
            snprintf(kernel_launch_data.kernel_name, sizeof kernel_launch_data.kernel_name, "%s", kernel_name);

            m_event_recorder->Record(kernel_launch_data);

            num_kernel_launches.fetch_add(1, std::memory_order_relaxed);
        });

        m_cuda_profiler->register_pre<cuda_trace::cuda_kernel_exit_tag>(
                [this](GpuId_T gpu_id, const char* kernel_name, auto&&...) {
            // std::cout << "[Profiler] Kernel finished -> " << gpu_id << std::endl;

            // TODO: :(
            Profiler::Events::CUDAKernelExit kernel_exit_data{.gpu_id = gpu_id};
            snprintf(kernel_exit_data.kernel_name, sizeof kernel_exit_data.kernel_name, "%s", kernel_name);

            m_event_recorder->Record(kernel_exit_data);
        });

        m_cuda_profiler->register_pre<cuda_trace::cuda_mem_op_tag>(
                [this](int gpu_id, uint64_t addr, std::string_view sass_str, auto&&...) {
            // ================================================
            // Umem
            for (auto [alloc_addr, alloc_length, umem_id]: m_mlx5_umem_allocations) {
                if (alloc_addr <= addr && addr < (alloc_addr + alloc_length)) {
                    const auto umem_allocation_entry = m_umem_id_to_type.find(umem_id);
                    if (umem_allocation_entry == m_umem_id_to_type.end()) {
                        assert(false && "Empty m_umem_id_to_type");
                        break;
                    }

                    const auto [alloc_type, queue_type] = umem_allocation_entry->second;

                    if (alloc_type == UmemAllocationType::Doorbell) {
                        std::cout << "Doorbell" << std::endl;
                        const Profiler::Events::DoorbellRing event{
                                .gpu_id = static_cast<int16_t>(gpu_id),
                                .nic_id = 0,
                                .addr = addr,
                                .wqe_index = 0,
                                .qp_num = 0,
                        };
                        m_event_recorder->Record(event);
                    }

                    num_nic_accesses.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
            }
            // ================================================

            // ================================================
            // Uar
            {
                const auto uar_allocation_entry = m_mlx5_uar_allocations.find(addr);
                if (uar_allocation_entry == m_mlx5_uar_allocations.end()) {
                    return;
                }

                const auto page_id = uar_allocation_entry->second;

                // TODO: not checking existence
                const auto [alloc_type, queue_type] = m_uar_id_to_type[page_id];

                if (alloc_type == UarAllocationType::BlueField) {
                    // const Profiler::Events::DoorbellRing event{.addr = addr};
                    // m_event_recorder->Record(event);
                }
                num_nic_accesses.fetch_add(1, std::memory_order_relaxed);
            }
            // ================================================
        });
    }

    static ProfilerCtx& instance() {
        static ProfilerCtx instance;
        return instance;
    }

    ibv::Profiler* m_ibv_profiler;
    cuda_trace::NvbitProfiler* m_cuda_profiler;
    mlx5dv::Profiler* m_mlx5dv_profiler;

    Events::EventRecorder<>* m_event_recorder;
};

static void __attribute__((constructor)) init() {
    ProfilerCtx::instance();
}
