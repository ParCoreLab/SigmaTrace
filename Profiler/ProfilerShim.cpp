#include <atomic>
#include <limits>
#include <map>
#include <tuple>
#include <type_traits>
#include <unordered_set>
#include <vector>

#include <cuda.h>
#include <cuda_runtime.h>
#include "Helpers/CudaHelpers.hpp"

#include "Helpers/Utilities.hpp"

#include "Dlsym.hpp"

#include "EventRecorder.hpp"
#include "EventTrait.hpp"
#include "Events.hpp"

// Profiler Event definitions
#include "Events/CUDAEvents.hpp"
#include "Events/IbvEvents.hpp"
#include "Events/Mlx5DvEvents.hpp"

#include "infiniband/mlx5dv.h"
#include "infiniband/verbs.h"

// Internal headers for mlx5 type definitions
// TODO: this is inside NVSHMEM. May be NVSHMEM or RDMA-CORE version dependent
#include <mlx5/mlx5_ifc.h>
#include <mlx5/mlx5_prm.h>

#include "CUDAEventSource.hpp"
#include "IbverbsEventSource.hpp"
#include "Mlx5dvEventSource.hpp"

#include "Modules/CudaExportTableOverwrite/ExportTableOverwrite.hpp"
#include "Modules/MemoryHijack/MemoryRepeater.hpp"

#include "ProfilerArgs.hpp"

// Intel specific
#include <immintrin.h>
// Both
#include <dlfcn.h>
#include <x86intrin.h>

// Build-system defined args to provider .so files
static constexpr std::string_view SIGMA_IBVERBS_PROVIDER_PATH{SIGMA_PROVIDER_ibverbs_trace};
static constexpr std::string_view SIGMA_NVBIT_PROVIDER_PATH{SIGMA_PROVIDER_nvbit_memory_tracker};
static constexpr std::string_view SIGMA_MLX5DV_PROVIDER_PATH{SIGMA_PROVIDER_mlx5dv_trace};

namespace Profiler::hack {
    // Addresses upstream asked to register. Filled in below functions
    // Accesses to this *shouldn't* need synchronization
    std::unordered_set<void*> CuMemHostRegisterAddrs{};

    // Fake addresses we ignore. Filled in mlx5dv hooks
    std::unordered_set<void*> CuMemHostRegisterIgnoredAddrs{};

    // https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__MEM.html#group__CUDA__MEM_1gf0a9fe11544326dabd743b7aa6b54223
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-declarations"
    CUresult Fake_cuMemHostRegister_v2(void* p, size_t bytesize, unsigned int Flags) {
        static auto* real_cuMemHostRegister = reinterpret_cast<decltype(cuMemHostRegister_v2)*>(
                CudaExportTable::get_real_cu_func("cuMemHostRegister_v2"));

        if (!real_cuMemHostRegister) {
            std::cerr << "No real_cuMemHostRegister" << std::endl;
            std::abort();
        }

        // massive hack
        if (CuMemHostRegisterIgnoredAddrs.contains(p)) {
            CuMemHostRegisterAddrs.emplace(p);
            return CUDA_SUCCESS;
        }

        return real_cuMemHostRegister(p, bytesize, Flags);
    }
    static_assert(std::is_same_v<decltype(cuMemHostRegister), decltype(Fake_cuMemHostRegister_v2)>);
    static_assert(std::is_same_v<decltype(cuMemHostRegister_v2), decltype(Fake_cuMemHostRegister_v2)>);

    // https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__MEM.html#group__CUDA__MEM_1g63f450c8125359be87b7623b1c0b2a14
    CUresult Fake_cuMemHostUnregister(void* p) {
        static auto* real_cuMemHostUnregister = reinterpret_cast<decltype(cuMemHostUnregister)*>(
                CudaExportTable::get_real_cu_func("cuMemHostUnregister"));

        if (!real_cuMemHostUnregister) {
            std::cerr << "No real_cuMemHostUnregister" << std::endl;
            std::abort();
        }

        // massive hack
        if (CuMemHostRegisterIgnoredAddrs.contains(p)) {
            return CUDA_SUCCESS;
        }

        return cuMemHostUnregister(p);
    }
    static_assert(std::is_same_v<decltype(cuMemHostUnregister), decltype(Fake_cuMemHostUnregister)>);
#pragma GCC diagnostic pop
} // namespace Profiler::hack

// Convenience
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
[[maybe_unused]] static std::string_view alloc_type_as_string(EnumT type) {
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
    // GPU memory allocaations
    // addr -> { size, gpu_id }
    // TODO: Make the value a proper struct so we don't have to do std::get<whatever->second>
    std::map<uintptr_t, std::tuple<size_t, int>> m_gpu_allocations{};
    // ================================================

    // ================================================
    using AddrType = uint64_t;

    // see mlx5_ifc.h
    // using UmemIdType = std::remove_extent_t<decltype(mlx5_ifc_create_qp_in_bits::wq_umem_id)>;
    // static_assert(sizeof(UmemIdType) == sizeof(uint8_t));
    using UmemIdType = uint32_t;

    using UarPageIdType = std::remove_extent_t<decltype(mlx5dv_devx_uar::page_id)>;
    static_assert(sizeof(UarPageIdType) == sizeof(uint32_t));

    // Registered umem and uar IDs to its buffer type
    std::map<UmemIdType, std::tuple<UmemAllocationType, QueueType>> m_umem_id_to_type;
    std::map<UarPageIdType, std::tuple<UarAllocationType, QueueType>> m_uar_id_to_type;

    // <addr, length, umem ID>
    std::vector<std::tuple<AddrType, size_t, UmemIdType>> m_mlx5_umem_allocations;
    // TODO: boost multimap
    std::map<AddrType, UarPageIdType> m_mlx5_uar_allocations;
    std::map<UarPageIdType, AddrType> m_mlx5_uar_page_id_to_addr;

    std::map<UarPageIdType, std::shared_ptr<bool>> m_mlx5_fake_uar_runner_signals;

    using MemoryRepeaterHandle = Profiler::MemoryRepeater::MemoryRepeaterHandle;
    std::unordered_map<UarPageIdType, std::vector<MemoryRepeaterHandle>> m_mlx5_uar_hijack_handles;
    // ================================================

    // ================================================
    // Stats
    std::atomic<int> num_nic_accesses{0};
    std::atomic<int> num_nic_doorbell_accesses{0};
    std::atomic<int> num_fake_nic_accesses{0};
    std::atomic<int> num_kernel_launches{0};
    // ================================================

    ~ProfilerCtx() {
        // std::cout << "[Profiler] num_nic_accesses: " << std::dec << num_nic_accesses << std::endl;
        std::cout << "[Profiler] num_nic_accesses: " << std::dec << num_nic_doorbell_accesses << std::endl;
        std::cout << "[Profiler] num_fake_nic_accesses: " << std::dec << num_fake_nic_accesses << std::endl;
        // std::cout << "[Profiler] num_kernel_launches: " << std::dec << num_kernel_launches << std::endl;
    }

    ProfilerCtx() {
        Profiler::CudaExportTable::register_overwrite(
                "cuMemHostRegister_v2", reinterpret_cast<void*>(Profiler::hack::Fake_cuMemHostRegister_v2));

        Profiler::CudaExportTable::register_overwrite(
                "cuMemHostUnregister", reinterpret_cast<void*>(Profiler::hack::Fake_cuMemHostUnregister));

        Events::RuntimeConfig eventConfig{};
        m_event_recorder = &Events::EventRecorder<>::instance(std::move(eventConfig));

        const auto profiler_args = Profiler::ProfilerArgs::from_env();

        if (profiler_args.LoadNvbit) {
            load_nvbit(profiler_args.ShimDir);
        }

        if (profiler_args.LoadIbtrace) {
            load_ibv(profiler_args.ShimDir);
        }

        if (profiler_args.LoadMlx5dv) {
            load_mlx5dv(profiler_args.ShimDir);
        }
    }

    static ProfilerCtx& instance() {
        static ProfilerCtx instance;
        return instance;
    }

    Events::EventRecorder<>* m_event_recorder{nullptr};

    ibv::Profiler* m_ibv_profiler{nullptr};
    cuda_trace::NvbitProfiler* m_cuda_profiler{nullptr};
    mlx5dv::Profiler* m_mlx5dv_profiler{nullptr};

    static void* internal_load_lib(std::string_view lib_name) {
        const auto mode = smart_dlsym::DL_RTLD_NOW | smart_dlsym::DL_RTLD_GLOBAL;
        void* ibv_shim = smart_dlsym::real_dlopen(lib_name.data(), mode);
        if (!ibv_shim) {
            std::cerr << "Unable to load " << lib_name << std::endl;
            std::abort();
        }

        return ibv_shim;
    }

    template<typename T>
    static T* get_instance(const std::filesystem::path& lib_name) {
        if (!std::filesystem::exists(lib_name)) {
            std::cerr << "Unable to load " << lib_name << " (file doesn't exist)" << std::endl;
            std::abort();
        }

        auto* handle = internal_load_lib(lib_name.c_str());
        auto* get_instance = reinterpret_cast<T* (*) ()>(smart_dlsym::real_dlsym(handle, "sigma_provider_instance"));
        if (!get_instance) {
            std::cerr << "Unable to load " << lib_name << " (no sigma_provider_instance)" << std::endl;
            std::abort();
        }

        return get_instance();
    }

    void load_ibv(const std::filesystem::path& shim_dir) noexcept {
        m_ibv_profiler = get_instance<ibv::Profiler>(shim_dir / SIGMA_IBVERBS_PROVIDER_PATH);

        m_ibv_profiler->register_post<ibv::open_device_tag>(
                [this](ibv_context* out_context, ibv_device* device, auto ts_start, auto ts_end) {
            std::cout << "[Profiler] ibv_open_device received" << std::endl;

            auto event = Profiler::Events::IbvOpenDevice{
                    .context_addr = reinterpret_cast<uint64_t>(out_context),
                    .node_type = device->node_type,
                    .transport_type = device->transport_type,
            };

            snprintf(event.name, sizeof device->name, "%s", device->name);
            snprintf(event.dev_name, sizeof device->dev_name, "%s", device->dev_name);
            snprintf(event.dev_path, sizeof device->dev_path, "%s", device->dev_path);
            snprintf(event.ibdev_path, sizeof device->ibdev_path, "%s", device->ibdev_path);

            m_event_recorder->Record(event, ts_start, ts_end);
        });

        m_ibv_profiler->register_post<ibv::close_device_tag>(
                [this](int, ibv_context* context, auto ts_start, auto ts_end) {
            std::cout << "[Profiler] ibv_open_device received" << std::endl;

            auto event = Profiler::Events::IbvCloseDevice{
                    .context_addr = reinterpret_cast<uint64_t>(context),
            };

            m_event_recorder->Record(event, ts_start, ts_end);
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
    }

    void load_nvbit(const std::filesystem::path& shim_dir) noexcept {
        m_cuda_profiler = get_instance<cuda_trace::NvbitProfiler>(shim_dir / SIGMA_NVBIT_PROVIDER_PATH);
        // m_cuda_profiler->enable_memop();

        // TODO: pass this along
        m_cuda_profiler->register_pre<cuda_trace::cuda_mem_alloc_tag>(
                [this](uint64_t addr, std::size_t size, auto&&...) {
            sigma_ignore_unused(this);
            sigma_ignore_unused(addr);
            sigma_ignore_unused(size);
        });

        // TODO: pass this along
        m_cuda_profiler->register_pre<cuda_trace::cuda_mem_register_tag>(
                [this](uint64_t addr, std::size_t size, [[maybe_unused]] unsigned int flags, auto&&...) {
            sigma_ignore_unused(this);
            sigma_ignore_unused(addr);
            sigma_ignore_unused(size);
        });

        m_cuda_profiler->register_pre<cuda_trace::cuda_kernel_launch_tag>(
                [this](GpuId_T gpu_id, const char* kernel_name, auto&&...) {
            // TODO: :(
            Profiler::Events::CUDAKernelLaunch kernel_launch_data{gpu_id};
            snprintf(kernel_launch_data.kernel_name, sizeof kernel_launch_data.kernel_name, "%s", kernel_name);

            m_event_recorder->Record(kernel_launch_data);

            num_kernel_launches.fetch_add(1, std::memory_order_relaxed);
        });

        m_cuda_profiler->register_pre<cuda_trace::cuda_kernel_exit_tag>(
                [this](GpuId_T gpu_id, const char* kernel_name, auto&&...) {
            // TODO: :(
            Profiler::Events::CUDAKernelExit kernel_exit_data{gpu_id};
            snprintf(kernel_exit_data.kernel_name, sizeof kernel_exit_data.kernel_name, "%s", kernel_name);

            m_event_recorder->Record(kernel_exit_data);
        });

        m_cuda_profiler->register_pre<cuda_trace::cuda_mem_op_tag>(
                [this](int gpu_id, uint64_t addr, [[maybe_unused]] std::string_view sass_str, auto&&...) {
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
                        const Profiler::Events::DoorbellRing event{
                                .gpu_id = static_cast<int16_t>(gpu_id),
                                .nic_id = 0,
                                .addr = addr,
                                .wqe_index = 0,
                                .qp_num = 0,
                        };
                        m_event_recorder->Record(event);
                        num_nic_doorbell_accesses.fetch_add(1, std::memory_order_relaxed);
                    } else if (alloc_type == UmemAllocationType::BlueField) {
                        std::cout << "TODO: BlueField" << std::endl;
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
                    std::cout << "TODO: BlueField UAR" << std::endl;
                    // TODO
                    // const Profiler::Events::DoorbellRing event{.addr = addr};
                    // m_event_recorder->Record(event);
                    num_nic_doorbell_accesses.fetch_add(1, std::memory_order_relaxed);
                }
                num_nic_accesses.fetch_add(1, std::memory_order_relaxed);
            }
            // ================================================
        });
    }

    void load_mlx5dv(const std::filesystem::path& shim_dir) {
        m_mlx5dv_profiler = get_instance<mlx5dv::Profiler>(shim_dir / SIGMA_MLX5DV_PROVIDER_PATH);

        m_mlx5dv_profiler->register_post<mlx5dv::init_obj_tag>(
                [](auto&&, mlx5dv_obj* obj, uint64_t obj_type, auto&&...) {
            sigma_ignore_unused(obj);

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
            sigma_ignore_unused(out_obj);
            sigma_ignore_unused(context);
            sigma_ignore_unused(inlen);
            sigma_ignore_unused(out);
            sigma_ignore_unused(outlen);

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
                auto* qp_context = DEVX_ADDR_OF(create_qp_in, const_cast<void*>(cmd_in), qpc);

                // BlueField register. TODO: what is BlueField?
                // TODO: We get this from mlx5dv_devx_alloc_uar too
                auto bf_register_uar_id = DEVX_GET(qpc, qp_context, uar_page);
                m_uar_id_to_type[bf_register_uar_id] = {UarAllocationType::BlueField, queue_type};

                // Doorbell register
                auto doorbell_register_umem_id = DEVX_GET(qpc, qp_context, dbr_umem_id);
                m_umem_id_to_type[doorbell_register_umem_id] = {UmemAllocationType::Doorbell, queue_type};

                // TODO: Doorbell Offset
                auto wq_doorbell_offset = DEVX_GET64(qpc, qp_context, dbr_addr);
                sigma_ignore_unused(wq_doorbell_offset);
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
                const auto* cq_context = DEVX_ADDR_OF(create_cq_in, const_cast<void*>(cmd_in), cq_context);

                // BlueFlame
                const auto bf_register_uar_id = DEVX_GET(cqc, cq_context, uar_page);
                m_uar_id_to_type[bf_register_uar_id] = {UarAllocationType::BlueField, queue_type};

                // Doorbell
                const auto doorbell_register_umem_id = DEVX_GET(cqc, cq_context, dbr_umem_id);
                m_umem_id_to_type[doorbell_register_umem_id] = {UmemAllocationType::Doorbell, queue_type};

                // TODO: Doorbell Offset
                const auto cq_doorbell_offset = DEVX_GET64(cqc, cq_context, dbr_addr);
                sigma_ignore_unused(cq_doorbell_offset);
                // ================================================
            }
        });

        // 1. Fake UAR hijack
        m_mlx5dv_profiler->register_post<mlx5dv::devx_alloc_uar_tag>(
                [this](mlx5dv_devx_uar* out_uar, ibv_context*, uint32_t flags, auto&&...) {
            sigma_ignore_unused(flags);

            std::scoped_lock lock(m_ibv_allocations_mutex);

            // TODO: why are there duplicate allocations?
            // returning here breaks things...
            // if (m_mlx5_uar_page_id_to_addr.contains(out_uar->page_id)) {
            //     return;
            // }

            constexpr auto page_size{4096};
            void* uar_fake_page_addr;
            CUDA_CHECK(cudaHostAlloc(&uar_fake_page_addr, page_size, cudaHostAllocMapped));
            memset(uar_fake_page_addr, 0, page_size);

            auto base_addr_real = out_uar->base_addr;
            auto reg_addr_real = out_uar->reg_addr;

            const ptrdiff_t doorbell_offset = static_cast<char*>(reg_addr_real) - static_cast<char*>(base_addr_real);
            if (doorbell_offset < 64) {
                std::cerr << "doorbell_offset (" << doorbell_offset << ") should not be smaller than 64 bytes"
                          << std::endl;
                std::abort();
            }

            // Not fully sure if this is really necessarily the doorbell addr
            void* fake_reg_addr = static_cast<char*>(uar_fake_page_addr) + doorbell_offset;

            // We really only need reg_addr but not setting both causes issues later. See doorbell_offset
            out_uar->base_addr = uar_fake_page_addr;
            out_uar->reg_addr = fake_reg_addr;

            // This shouldn't need synchronization but it'll grow infinitely
            Profiler::hack::CuMemHostRegisterIgnoredAddrs.insert(out_uar->reg_addr);

            MemoryRepeaterHandle handle{static_cast<uint64_t*>(fake_reg_addr), reg_addr_real,
                                        [this, reg_addr_real, fake_reg_addr](const uint64_t new_val) {
                sigma_ignore_unused(new_val);

                const Profiler::Events::DoorbellRing event{
                        .gpu_id = static_cast<int16_t>(0), // TODO: track per-UAR
                        .nic_id = 1,
                        .addr = reinterpret_cast<uint64_t>(reg_addr_real),
                        .fake_addr = reinterpret_cast<uint64_t>(fake_reg_addr),
                        .wqe_index = 0, // TODO
                        .qp_num = 0,    // TODO
                        .is_cpu_hijack = true,
                };
                m_event_recorder->Record(event);

                num_fake_nic_accesses.fetch_add(1, std::memory_order_relaxed);
            }};

            auto& hijack_handles = m_mlx5_uar_hijack_handles[out_uar->page_id];
            auto& inserted_handle = hijack_handles.emplace_back(std::move(handle));
            inserted_handle.start();
        });

        // 2. Memory tracking
        m_mlx5dv_profiler->register_post<mlx5dv::devx_alloc_uar_tag>(
                [this](mlx5dv_devx_uar* out_uar, ibv_context*, uint32_t flags, auto&&...) {
            std::scoped_lock lock(m_ibv_allocations_mutex);

            if (m_mlx5_uar_page_id_to_addr.contains(out_uar->page_id)) {
                // TODO :(*
                // std::cerr << "WARNING: Duplicate page id in mlx5dv_devx_alloc_uar " << out_uar->page_id << std::endl;
                return;
            }

            const auto uar_reg_addr = reinterpret_cast<uint64_t>(out_uar->reg_addr);
            // auto uar_base_addr = reinterpret_cast<uint64_t>(out_uar->base_addr);

            // TODO: not checking existence
            m_mlx5_uar_allocations[uar_reg_addr] = out_uar->page_id;
            m_mlx5_uar_page_id_to_addr[out_uar->page_id] = uar_reg_addr;

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

        // 1. For hijack
        m_mlx5dv_profiler->register_pre<mlx5dv::devx_free_uar_tag>([this](mlx5dv_devx_uar* devx_uar, auto&&...) {
            std::scoped_lock lock(m_ibv_allocations_mutex);

            const auto& handles = m_mlx5_uar_hijack_handles.find(devx_uar->page_id);
            if (handles == m_mlx5_uar_hijack_handles.end()) {
                return;
            }

            for (auto& h: handles->second) {
                h.signal_finish();
            }

            m_mlx5_uar_hijack_handles.erase(handles);
        });

        // 2. For memory tracking
        m_mlx5dv_profiler->register_pre<mlx5dv::devx_free_uar_tag>([this](mlx5dv_devx_uar* devx_uar, auto&&...) {
            std::scoped_lock lock(m_ibv_allocations_mutex);

            // Signal our runner thread to stop
            // We can erase here because it's a shared_ptr
            if (m_mlx5_fake_uar_runner_signals.contains(devx_uar->page_id)) {
                *m_mlx5_fake_uar_runner_signals[devx_uar->page_id].get() = false;
                m_mlx5_fake_uar_runner_signals.erase(devx_uar->page_id);
            }

            const auto& uar_alloc = m_mlx5_uar_allocations.find(devx_uar->page_id);
            if (uar_alloc == m_mlx5_uar_allocations.end()) {
                // std::cerr << "[Profiler] mlx5dv_devx_free_uar called with nonexistent page_id (" << devx_uar->page_id
                // << ")" << std::endl;
                return;
            }

            m_mlx5_uar_allocations.erase(uar_alloc);
            m_mlx5_uar_page_id_to_addr.erase(devx_uar->page_id);
        });

        m_mlx5dv_profiler->register_post<mlx5dv::devx_umem_reg_tag>(
                [this]<typename TS>(mlx5dv_devx_umem* out_umem, [[maybe_unused]] ibv_context* ctx, void* addr,
                                    size_t size, [[maybe_unused]] uint32_t access, TS ts_start, TS ts_end) {
            auto umem_addr = reinterpret_cast<uint64_t>(addr);

            // TODO: extra copy because Record(const T&)
            m_event_recorder->Record<Profiler::Events::Mlx5MemoryRegistration>(
                    {
                            .gpu_id = 0,
                            .nic_id = 0,
                            // TODO
                            .mem_id = out_umem->umem_id,
                            .addr = umem_addr,
                            .size = size,
                    },
                    ts_start, ts_end);

            std::scoped_lock lock(m_ibv_allocations_mutex);

            m_mlx5_umem_allocations.emplace_back(umem_addr, size, out_umem->umem_id);
        });

        // We can't do register_pre because the pointer will be gone
        m_mlx5dv_profiler->register_pre<mlx5dv::devx_umem_dereg_tag>([this](mlx5dv_devx_umem* umem, auto ts_start) {
            m_event_recorder->Record<Profiler::Events::Mlx5MemoryDereg>(
                    {
                            .nic_id = 0,
                            .mem_id = umem->umem_id,
                    },
                    ts_start);

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
    }
};

static void __attribute__((constructor)) init() {
    ProfilerCtx::instance();
}
