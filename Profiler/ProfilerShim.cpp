#include <atomic>
#include <map>
#include <tuple>
#include <type_traits>
#include <vector>

#include "EventRecorder.hpp"
#include "EventTrait.hpp"
#include "Events.hpp"

// Profiler Event definitions
#include "Events/Mlx5DvEvents.hpp"

#include "infiniband/mlx5dv.h"
#include "infiniband/verbs.h"

#include "mlx5/mlx5_ifc.h"
#include "mlx5/mlx5_prm.h"
// TODO: this is inside NVSHMEM. May be NVSHMEM or RDMA-CORE version dependent
// constexpr auto MLX5_CMD_OP_CREATE_QP { 0x500 };

#include "CUDAEventSource.hpp"
#include "CUDAEvents.hpp"
#include "IbverbsEventSource.hpp"
#include "Mlx5dvEventSource.hpp"

enum class QueueType {
    Unknown,
    WQ,
    CQ,
};

static std::string_view queue_type_as_string(QueueType type) {
    switch (type) {
        case QueueType::WQ:
            return "WQ";
        case QueueType::CQ:
            return "CQ";
        default:
            return "Unknown";
    }
}

enum class UmemAllocationType {
    Unknown = 0,
    WQE,
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

struct ProfilerCtx {
    // TODO: rename
    std::mutex m_ibv_allocations_mutex;

    // ================================================
    using AddrType = uint64_t;

    // see mlx5_ifc.h
    using UmemIdType = std::remove_extent_t<decltype(mlx5_ifc_create_qp_in_bits::wq_umem_id)>;
    static_assert(sizeof(UmemIdType) == sizeof(uint8_t));

    using UarPageIdType = std::remove_extent_t<decltype(mlx5dv_devx_uar::page_id)>;
    static_assert(sizeof(UarPageIdType) == sizeof(uint32_t));

    // Registered umem IDs to its buffer typsdas
    std::map<UmemIdType, std::tuple<UmemAllocationType, QueueType>> m_umem_id_to_type;

    std::map<UarPageIdType, std::tuple<UarAllocationType, QueueType>> m_uar_id_to_type;

    // <addr, length, umem ID>
    std::vector<std::tuple<AddrType, size_t, UmemIdType>> m_mlx5_umem_allocations;
    std::map<AddrType, UarPageIdType> m_mlx5_uar_allocations;
    // ================================================

    // ================================================
    // Stats
    std::atomic<int> num_nic_accesses{0};
    // ================================================

    ~ProfilerCtx() { std::cout << "[Profiler] num_nic_accesses: " << std::dec << num_nic_accesses << std::endl; }

    ProfilerCtx() {
        m_ibv_profiler = &ibv::Profiler::instance();
        m_cuda_profiler = &cuda_trace::NvbitProfiler::instance();
        m_mlx5dv_profiler = &mlx5dv::Profiler::instance();

        m_event_recorder = &Events::EventRecorder<>::instance();

        m_mlx5dv_profiler->register_post<mlx5dv::init_obj_tag>([](auto &&, mlx5dv_obj *obj, uint64_t obj_type) {
            std::cout << "[Profiler] mlx5dv is alive!" << std::endl;

            if (obj_type & MLX5DV_OBJ_QP) {
                std::cout << "QP created" << std::endl;
            }

            if (obj_type & MLX5DV_OBJ_CQ) {
                std::cout << "CQ created" << std::endl;
            }
        });

        m_mlx5dv_profiler->register_post<mlx5dv::obj_create_tag>([this](mlx5dv_devx_obj *out_obj, ibv_context *context,
                                                                        const void *cmd_in, size_t inlen, void *out,
                                                                        size_t outlen) {
            assert(out_obj && "mlx5dv::obj_create_tag resulted in null");

            // TODO
            // std::scoped_lock lock(m_ibv_allocations_mutex);

            auto mlx5_cmd_opcode = DEVX_GET(create_qp_in, cmd_in, opcode);

            if (mlx5_cmd_opcode == MLX5_CMD_OP_CREATE_QP) {
                // ================================================
                const auto queue_type = QueueType::WQ;

                // WQ buffer
                auto wg_umem_id = DEVX_GET(create_qp_in, cmd_in, wq_umem_id);
                m_umem_id_to_type[wg_umem_id] = {UmemAllocationType::WQE, queue_type};

                // TODO: what is this
                auto *qp_context = DEVX_ADDR_OF(create_qp_in, cmd_in, qpc);

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

                // DEBUG
                std::cout << "WQ Buffer ID: " << wg_umem_id << std::endl;
                std::cout << "WQ BF Register ID: " << bf_register_uar_id << std::endl;
                std::cout << "WQ Doorbell Register ID: " << doorbell_register_umem_id << std::endl;
                std::cout << "WQ Doorbell Offset: " << wq_doorbell_offset << std::endl;

                return;
            }

            if (mlx5_cmd_opcode == MLX5_CMD_OP_CREATE_CQ) {
                // ================================================
                const auto queue_type = QueueType::CQ;

                // CQ buffer
                auto cq_umem_id = DEVX_GET(create_cq_in, cmd_in, cq_umem_id);
                m_umem_id_to_type[cq_umem_id] = {UmemAllocationType::WQE, queue_type};

                // Context
                auto *cq_context = DEVX_ADDR_OF(create_cq_in, cmd_in, cq_context);

                // BlueField
                auto bf_register_uar_id = DEVX_GET(cqc, cq_context, uar_page);
                m_uar_id_to_type[bf_register_uar_id] = {UarAllocationType::BlueField, queue_type};

                // Doorbell
                auto doorbell_register_umem_id = DEVX_GET(cqc, cq_context, dbr_umem_id);
                m_umem_id_to_type[doorbell_register_umem_id] = {UmemAllocationType::Doorbell, queue_type};

                // TODO: Doorbell Offset
                auto cq_doorbell_offset = DEVX_GET64(cqc, cq_context, dbr_addr); // DBR offset
                // ================================================

                // DEBUG
                std::cout << "CQ Buffer ID: " << cq_umem_id << std::endl;
                std::cout << "CQ BF Register ID: " << bf_register_uar_id << std::endl;
                std::cout << "CQ Doorbell Register ID: " << doorbell_register_umem_id << std::endl;
                std::cout << "CQ Doorbell Offset: " << cq_doorbell_offset << std::endl;

                return;
            }
        });

        m_mlx5dv_profiler->register_post<mlx5dv::devx_alloc_uar_tag>(
                [this](mlx5dv_devx_uar *out_uar, ibv_context *context, uint32_t flags) {
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
                        std::cerr << "[Profiler] mlx5dv_devx_alloc_uar with non-BlueField flag" << std::endl;
                    }
                });

        m_mlx5dv_profiler->register_pre<mlx5dv::devx_free_uar_tag>([this](mlx5dv_devx_uar *devx_uar) {
            std::scoped_lock lock(m_ibv_allocations_mutex);

            const auto &uar_alloc = m_mlx5_uar_allocations.find(devx_uar->page_id);
            if (uar_alloc == m_mlx5_uar_allocations.end()) {
                // std::cerr << "[Profiler] mlx5dv_devx_free_uar called with nonexistent page_id (" << devx_uar->page_id
                // << ")" << std::endl;
                return;
            }

            m_mlx5_uar_allocations.erase(uar_alloc);
        });

        m_mlx5dv_profiler->register_post<mlx5dv::devx_umem_reg_tag>(
                [this](mlx5dv_devx_umem *out_umem, ibv_context *ctx, void *addr, size_t size, uint32_t access) {
                    // std::cout << "[profiler] umem_reg: " << out_umem->umem_id << std::endl;
                    auto umem_addr = reinterpret_cast<uint64_t>(addr);
                    std::scoped_lock lock(m_ibv_allocations_mutex);
                    m_mlx5_umem_allocations.emplace_back(umem_addr, size, out_umem->umem_id);
                });

        m_mlx5dv_profiler->register_pre<mlx5dv::devx_umem_dereg_tag>([this](mlx5dv_devx_umem *umem) {
            std::scoped_lock lock(m_ibv_allocations_mutex);

            // std::cout << "umem_dereg " << std::dec << umem->umem_id << std::endl;

            for (auto it = m_mlx5_umem_allocations.begin(); it != m_mlx5_umem_allocations.end(); ++it) {
                if (std::get<2>(*it) == umem->umem_id) {
                    m_mlx5_umem_allocations.erase(it);
                    return;
                }
            }

            std::cerr << "[Profiler] mlx5dv_devx_umem_dereg called with nonexistent umem_id (" << umem->umem_id << ")"
                      << std::endl;
        });

        /* IBV stuff unused for now
        m_ibv_profiler->register_pre<ibv::open_device_tag>([](auto &&...) {
            std::cout << "[Profiler] ibv_open_device received" << std::endl;
        });

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

        m_cuda_profiler->register_pre<cuda_trace::cuda_kernel_launch_tag>([this](const char *kernel_name) {
            std::cout << "[Profiler] Kernel launched -> " << kernel_name << std::endl;

            // TODO: :(
            Profiler::Events::CUDAKernelLaunch kernel_launch_data{};
            snprintf(kernel_launch_data.kernel_name, sizeof kernel_launch_data.kernel_name, "%s", kernel_name);

            m_event_recorder->Record(kernel_launch_data);
        });

        m_cuda_profiler->register_pre<cuda_trace::cuda_kernel_exit_tag>([this](const char *kernel_name) {
            std::cout << "[Profiler] Kernel finished -> " << kernel_name << std::endl;

            // TODO: :(
            Profiler::Events::CUDAKernelExit kernel_exit_data{};
            snprintf(kernel_exit_data.kernel_name, sizeof kernel_exit_data.kernel_name, "%s", kernel_name);

            m_event_recorder->Record(kernel_exit_data);
        });

        m_cuda_profiler->register_pre<cuda_trace::cuda_trace_tag>([this](uint64_t addr, std::string_view sass_str,
                                                                         auto &&...) {
            // ================================================
            // Umem
            for (auto [alloc_addr, alloc_length, umem_id]: m_mlx5_umem_allocations) {
                if (alloc_addr == addr) {
                    std::cout << "UMEM aaaaa" << std::endl;
                }

                if (alloc_addr <= addr && addr < (alloc_addr + alloc_length)) {
                    const auto [alloc_type, queue_type] = m_umem_id_to_type[umem_id];

                    // TODO: can be string_view
                    std::string alloc_type_str{alloc_type_as_string(alloc_type)};

                    // TODO: for debugging
                    if (alloc_type == UmemAllocationType::Unknown) {
                        alloc_type_str += " (ID " + std::to_string(umem_id) + ")";
                    }

                    const std::string_view queue_type_str{queue_type_as_string(queue_type)};


                    std::cout << queue_type_str << " | UMEM yes | " << sass_str << " @ " << std::hex << addr;
                    std::cout << " -> " << alloc_type_str << " + " << std::dec << addr - alloc_addr << std::endl;

                    if (alloc_type == UmemAllocationType::Doorbell) {
                        Profiler::Events::DoorbellRing event{.addr = addr};
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

                // TODO: can be string_view
                std::string alloc_type_str{alloc_type_as_string(alloc_type)};

                // TODO: for debugging
                if (alloc_type == UarAllocationType::Unknown) {
                    alloc_type_str += " (ID " + std::to_string(page_id) + ")";
                }

                const std::string_view queue_type_str{queue_type_as_string(queue_type)};

                std::cout << queue_type_str << " | UAR yes | " << sass_str << " @ " << std::hex << addr;
                std::cout << " -> " << alloc_type_str << std::endl;

                num_nic_accesses.fetch_add(1, std::memory_order_relaxed);
            }
            // ================================================
        });
    }

    static ProfilerCtx *instance() {
        static std::unique_ptr<ProfilerCtx> instance;

        if (!instance) {
            instance = std::make_unique<ProfilerCtx>();

            // TODO:
            printf("Created ProfilerCtx @ %p\n", instance.get());
        }

        return instance.get();
    }

    ibv::Profiler *m_ibv_profiler;
    cuda_trace::NvbitProfiler *m_cuda_profiler;
    mlx5dv::Profiler *m_mlx5dv_profiler;

    Events::EventRecorder<> *m_event_recorder;
};

static void __attribute__((constructor)) init() { ProfilerCtx::instance(); }
