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

// smart_dlsym
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

// Event providers
#include "CUDAEventSource.hpp"
#include "IbverbsEventSource.hpp"
#include "Mlx5dvEventSource.hpp"

// Profiler-specific modules
#include "Modules/CudaExportTableOverwrite/ExportTableOverwrite.hpp"
#include "Modules/MemoryHijack/MemoryRepeater.hpp"

#include "ProfilerArgs.hpp"
#include "ProfilerProviders.hpp"

namespace Profiler::hack {
    // Addresses upstream asked to register. Filled in below functions
    // Accesses to this *shouldn't* need synchronization
    std::unordered_set<void*> CuMemHostRegisterAddrs{};

    // Fake addresses we ignore. Filled in mlx5dv hooks
    std::unordered_set<void*> CuMemHostRegisterIgnoredAddrs{};

    // https://docs.nvidia.com/cuda/cuda-driver-api/group__CUDA__MEM.html#group__CUDA__MEM_1gf0a9fe11544326dabd743b7aa6b54223
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-declarations"
    decltype(auto) CuMemHostRegisterOverwrite(auto* real_func, void* p, size_t bytesize, unsigned int Flags) {
        if (!real_func) {
            std::cerr << "No real_cuMemHostRegister" << std::endl;
            std::abort();
        }

        // massive hack
        if (CuMemHostRegisterIgnoredAddrs.contains(p)) {
            CuMemHostRegisterAddrs.emplace(p);
            return CUDA_SUCCESS;
        }

        return real_func(p, bytesize, Flags);
    }

    CUresult Fake_cuMemHostRegister(void* p, size_t bytesize, unsigned int Flags) {
        static auto* real_cuMemHostRegister =
                reinterpret_cast<decltype(cuMemHostRegister)*>(CudaExportTable::get_real_cu_func("cuMemHostRegister"));

        return CuMemHostRegisterOverwrite(real_cuMemHostRegister, p, bytesize, Flags);
    }
    static_assert(std::is_same_v<decltype(cuMemHostRegister), decltype(Fake_cuMemHostRegister)>);

    CUresult Fake_cuMemHostRegister_v2(void* p, size_t bytesize, unsigned int Flags) {
        static auto* real_cuMemHostRegister = reinterpret_cast<decltype(cuMemHostRegister_v2)*>(
                CudaExportTable::get_real_cu_func("cuMemHostRegister_v2"));

        return CuMemHostRegisterOverwrite(real_cuMemHostRegister, p, bytesize, Flags);
    }
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
using Helpers::PtrToUint;
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
    // GPU memory allocations
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
    // std::vector<std::tuple<AddrType, size_t, UmemIdType>> m_mlx5_umem_allocations;
    // TODO: boost multimap
    // std::map<AddrType, UarPageIdType> m_mlx5_uar_allocations;
    // std::map<UarPageIdType, AddrType> m_mlx5_uar_page_id_to_addr;

    using MemoryRepeaterHandle = Profiler::MemoryRepeater::MemoryRepeaterHandle;
    std::unordered_map<UarPageIdType, std::vector<MemoryRepeaterHandle>> m_mlx5_uar_hijack_handles;
    // ================================================

    // ================================================
    // ibv memory tracking
    // address -> (size, device_id)
    // TODO: put this in a module with more info (memory type, device type) and better (binary) search
    std::map<uint64_t, std::tuple<uint64_t, GpuId_T>> m_ibv_memory_registrations;

    GpuId_T find_gpu_id(const uint64_t addr) {
        const auto& entry = m_ibv_memory_registrations.find(addr);

        // Exact match
        if (entry != m_ibv_memory_registrations.end()) {
            return std::get<1>(entry->second);
        }

        for (const auto& [mem_addr, mem_info]: m_ibv_memory_registrations) {
            const auto [length, device_id] = mem_info;

            if (mem_addr <= addr && addr < (mem_addr + length)) {
                return device_id;
            }
        }

        return Profiler::Events::UnknownDeviceId;
    }

    static GpuId_T get_addr_gpu_id(uint64_t addr) {
        int cuda_dev;
        auto cuda_err = CUDA_LOG(cudaGetDevice(&cuda_dev));
        if (cuda_err != cudaSuccess) {
            return Profiler::Events::UnknownDeviceId;
        }

        cudaPointerAttributes attrs{};
        cuda_err = CUDA_LOG(cudaPointerGetAttributes(&attrs, reinterpret_cast<void*>(addr)));
        if (cuda_err != cudaSuccess) {
            return Profiler::Events::UnknownDeviceId;
        }

        GpuId_T gpu_id{static_cast<GpuId_T>(attrs.device)};
        if (attrs.type == cudaMemoryTypeUnregistered) {
            // Making sure it's a well defined value
            return Profiler::Events::UnknownDeviceId;
        }

        return gpu_id;
    }
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

    // See Note[late_load]
    // Event module will interface with the filesystem and may print to stderr
    void LateLoad() {
        m_event_recorder = &Events::EventRecorder<>::instance(std::move(m_event_recorder_config));
        std::cout << "[Profiler] Initialized!" << std::endl;
    }

    /*
     * WARNING: Some calls are unsafe in this function.
     * Do not use std::cout or related functions that may depend on libc
     * See Note[late_load]
     */
    // WARNING: Some calls are unsafe in this function, such as std::cout. We need to wait until __libc_start_main
    // See LateLoad
    ProfilerCtx() {
        Profiler::CudaExportTable::register_overwrite("cuMemHostRegister",
                                                      reinterpret_cast<void*>(Profiler::hack::Fake_cuMemHostRegister));

        Profiler::CudaExportTable::register_overwrite(
                "cuMemHostRegister_v2", reinterpret_cast<void*>(Profiler::hack::Fake_cuMemHostRegister_v2));

        Profiler::CudaExportTable::register_overwrite(
                "cuMemHostUnregister", reinterpret_cast<void*>(Profiler::hack::Fake_cuMemHostUnregister));

        m_args = Profiler::ProfilerArgs::from_env();

        // Will be populated with things from profiler_args in the future
        m_event_recorder_config = {};

        if (m_args.LoadNvbit) {
            load_nvbit(m_args.ShimDir);
        }

        if (m_args.LoadIbtrace) {
            load_ibv(m_args.ShimDir);
        }

        if (m_args.LoadMlx5dv) {
            load_mlx5dv(m_args.ShimDir);
        }
    }

    static ProfilerCtx& instance() {
        static ProfilerCtx instance;
        return instance;
    }

    Profiler::ProfilerArgs m_args;

    Events::RuntimeConfig m_event_recorder_config;
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

    /*
     * Query NIC info and record on a device open
     *
     * We use context addr + node_guid to uniquely identify NICs in a given node
     * This function is meant to be called on ibv_device_open or mx5dv_device_open
     *
     * It first calls ibv_query_device to get node_guid alongside other attributes
     * If NIC data is already saved, it constructs the IbvOpenDevice event nd returns early
     * Otherwise, it queries each port and saves info.
     *
     * It turns out some applications call ibv_open_device a lot... so we don't want to
     * fetch and record all the data here unnecessarily
     *
     * Note the dynamic symbol resolution: we want this function to be callable from
     * both ibv and mx5dv contexts with any combination of providers loaded. It's possible
     * that the user disables ibv but enables mlx5dv to get a reduced set of events, meaning
     * ibv symbols won't be loaded globally. That being said, the current implementation is jank
     * and should be in a symbol table in a helper module, not this mega function.
     *
     * node_guid is unique per HCA in the node
     *
     * Node (server)
     * ├── HCA 0 (mlx5_0)
     * │   ├── node_guid: 0x1234...      (per-HCA)
     * │   ├── sys_image_guid: 0xABCD... (can span HCAs)
     * │   ├── Port 1
     * │   │   ├── LID: 15               (assigned by SM)
     * │   │   ├── GID[0]: fe80::1234    (link-local, derived from port_guid)
     * │   │   ├── GID[1]: ff02::5678    (if RoCE: IP-based)
     * │   │   └── state: ACTIVE
     * │   └── Port 2
     * │       ├── LID: 16               (different LID)
     * │       ├── GID[0]: fe80::1235
     * │       └── state: ACTIVE
     * └── HCA 1 (mlx5_1)
     *     ├── node_guid: 0x5678...      (different HCA)
     *     ├── sys_image_guid: 0xABCD... (same system)
     *     └── Port 1
     *     ├── LID: 23
     *     └── GID[0]: fe80::5678
     */
    Profiler::Events::IbvOpenDevice register_nic(ibv_context* ctx, ibv_device* device) const {
        using fn_query_port_t = int (*)(ibv_context* context, uint8_t port_num, ibv_port_attr* port_attr);

        static auto* libibverbs_handle = smart_dlsym::real_dlopen("libibverbs.so", smart_dlsym::DL_RTLD_NOW);
        if (!libibverbs_handle) {
            std::cerr << "Could not find dlopen libibverbs.so" << std::endl;
            std::abort();
        }

        static auto* fn_query_device = reinterpret_cast<decltype(ibv_query_device)*>(
                smart_dlsym::real_dlsym(libibverbs_handle, "ibv_query_device"));

        static auto* fn_query_port = reinterpret_cast<fn_query_port_t>(
                smart_dlsym::real_dlvsym(libibverbs_handle, "ibv_query_port", "IBVERBS_1.1"));

        static auto* fn_query_gid =
                reinterpret_cast<decltype(ibv_query_gid)*>(smart_dlsym::real_dlsym(libibverbs_handle, "ibv_query_gid"));

        if (!fn_query_device || !fn_query_port || !fn_query_gid) {
            std::cerr << "Could not find ibv_query function symbols" << std::endl;
            std::abort();
        }

        ibv_device_attr attrs{};
        const auto res_device = fn_query_device(ctx, &attrs);
        if (res_device != 0) {
            std::cerr << "Failed to query device of ctx " << ctx << std::endl;
            std::abort();
        }

        const Profiler::Events::IbvOpenDevice event{
                .context_addr = PtrToUint(ctx),
                .node_guid = attrs.node_guid,
        };

        // TODO: this is probably not a good way to check.
        // A better approach is waiting for the user to call the functions below
        // And only query ourselves before Profiler exit if not all NICs get queried
        static std::unordered_set<uint64_t> fetched_nics{};
        if (fetched_nics.contains(attrs.node_guid)) {
            return event;
        }
        fetched_nics.emplace(attrs.node_guid);

        const uint8_t num_ports{attrs.phys_port_cnt};
        for (uint8_t p = 1; p <= num_ports; p++) {
            ibv_port_attr port_attrs{};
            const auto res_port = fn_query_port(ctx, p, &port_attrs);
            if (res_port != 0) {
                std::cerr << "Failed to query port " << p << " of ctx " << ctx << std::endl;
                std::abort();
            }

            Profiler::Events::IbvPortRegistration port_event{
                    .port_num = p,
                    .lid = port_attrs.lid,
                    .state = port_attrs.state,
                    .link_layer = port_attrs.link_layer,
                    .gid = {},
            };

            // GID index 0 is typically the default (RoCEv2: index 1+ may be different)
            const auto res_gid = fn_query_gid(ctx, p, 0, &port_event.gid);
            if (res_gid != 0) {
                std::cerr << "Failed to query gid of port " << p << " of ctx " << ctx << std::endl;
                std::abort();
            }

            m_event_recorder->Record(port_event, 1);
        }

        // This isn't traced and should precede other ibv events
        Profiler::Events::IbvDeviceRegistration device_registration_event{
                .context_addr = PtrToUint(ctx),
                .node_guid = attrs.node_guid,
                .sys_image_guid = attrs.sys_image_guid,
                .node_type = device->node_type,
                .transport_type = device->transport_type,
        };

        snprintf(device_registration_event.name, sizeof device->name, "%s", device->name);
        snprintf(device_registration_event.dev_name, sizeof device->dev_name, "%s", device->dev_name);
        snprintf(device_registration_event.dev_path, sizeof device->dev_path, "%s", device->dev_path);
        snprintf(device_registration_event.ibdev_path, sizeof device->ibdev_path, "%s", device->ibdev_path);

        m_event_recorder->Record(device_registration_event, 0);

        return event;
    }

    void load_ibv(const std::filesystem::path& shim_dir) noexcept {
        m_ibv_profiler = get_instance<ibv::Profiler>(shim_dir / Profiler::SIGMA_IBVERBS_PROVIDER_PATH);

        m_ibv_profiler->register_post<ibv::open_device_tag>(
                [this](ibv_context* out_context, ibv_device* device, auto ts_start, auto ts_end) {
            // std::cout << "[Profiler] ibv_open_device received " << out_context << std::endl;

            const auto event = register_nic(out_context, device);
            m_event_recorder->Record(event, ts_start, ts_end);
        });

        m_ibv_profiler->register_post<ibv::close_device_tag>(
                [this](int, ibv_context* context, auto ts_start, auto ts_end) {
            // std::cout << "[Profiler] ibv_close_device received " << context << std::endl;

            auto event = Profiler::Events::IbvCloseDevice{
                    .context_addr = reinterpret_cast<uint64_t>(context),
            };

            m_event_recorder->Record(event, ts_start, ts_end);
        });

        m_ibv_profiler->register_post<ibv::create_qp_tag>(
                [this](ibv_qp* out_qp, ibv_pd* pd, ibv_qp_init_attr* qp_init_attr, auto ts_start, auto ts_end) {
            sigma_ignore_unused(pd);
            sigma_ignore_unused(qp_init_attr);

            Profiler::Events::IbvCreateQp event{
                    .context_addr = reinterpret_cast<uint64_t>(out_qp->context),
                    .qp_num = out_qp->qp_num,
            };
            m_event_recorder->Record(event, ts_start, ts_end);
        });

        m_ibv_profiler->register_pre<ibv::destroy_qp_tag>([this](ibv_qp* qp, auto ts_start) {
            Profiler::Events::IbvDestroyQp event{
                    // WARNING: not qp->context. Why? I have no clue
                    .context_addr = reinterpret_cast<uint64_t>(qp->pd->context),
                    .qp_num = qp->qp_num,
            };
            m_event_recorder->Record(event, ts_start);
        });

        m_ibv_profiler->register_post<ibv::modify_qp_tag>(
                [this](int, ibv_qp* qp, ibv_qp_attr* attr, int attr_mask, auto ts_start, auto ts_end) {
            if (!(attr_mask & IBV_QP_DEST_QPN)) {
                return;
            }

            Profiler::Events::IbvModifyQp event{
                    .context_addr = reinterpret_cast<uint64_t>(qp->context),
                    .qp_num = qp->qp_num,
                    .attr = *attr,
            };
            m_event_recorder->Record(event, ts_start, ts_end);
        });

        // ================================================
        // Memory registration
        // TODO: this can be done automatically/declaratively by declaring which arg is addr, which one is length, etc.
        m_ibv_profiler->register_post<ibv::reg_mr_tag>([this]<typename TS>(ibv_mr* out_mr, ibv_pd* pd, void* addr,
                                                                           size_t length, int access, TS ts_start,
                                                                           TS ts_end) {
            sigma_ignore_unused(out_mr);
            sigma_ignore_unused(pd);
            sigma_ignore_unused(access);

            const auto alloc_addr = reinterpret_cast<uint64_t>(addr);

            /*
            const auto gpu_id = get_addr_gpu_id(alloc_addr);
            if (gpu_id != Profiler::Events::UnknownDeviceId) {
                const auto [_, inserted] = m_ibv_memory_registrations.insert({alloc_addr, {length, gpu_id}});
                if (!inserted) {
                    // std::cerr << "reg_mr_tag addr " << addr << " already registered" << std::endl;
                    // std::abort();
                    return;
                }
            }
            */

            const Profiler::Events::IbvMemoryReg event{
                    .context_addr = reinterpret_cast<uint64_t>(pd->context),
                    .addr = alloc_addr,
                    .size = length,
                    .type = Profiler::Events::IbvMemoryRegType::ibv_reg_mr_iova2,
            };
            m_event_recorder->Record(event, ts_start, ts_end);
        });

        m_ibv_profiler->register_post<ibv::reg_mr_iova2_tag>(
                [this]<typename TS>(ibv_mr* out_mr, ibv_pd* pd, void* addr, size_t length, uint64_t iova,
                                    unsigned int access, TS ts_start, TS ts_end) {
            sigma_ignore_unused(out_mr);
            sigma_ignore_unused(pd);
            sigma_ignore_unused(iova);
            sigma_ignore_unused(access);

            const auto alloc_addr = reinterpret_cast<uint64_t>(addr);

            const Profiler::Events::IbvMemoryReg event{
                    .context_addr = reinterpret_cast<uint64_t>(pd->context),
                    .addr = alloc_addr,
                    .size = length,
                    .type = Profiler::Events::IbvMemoryRegType::ibv_reg_mr_iova2,
            };
            m_event_recorder->Record(event, ts_start, ts_end);
        });

        m_ibv_profiler->register_post<ibv::post_send_tag>(
                [this]<typename TS>(auto&&, ibv_qp* qp, ibv_send_wr* wr, ibv_send_wr** bad_wr, TS ts_start, TS ts_end) {
            sigma_ignore_unused(bad_wr);

            const auto context_addr = PtrToUint(qp->context);

            Profiler::Events::IbvPostSend post_send_event{
                    .context_addr = context_addr,
                    .qp_num = qp->qp_num,
                    .num_wrs = 0,
            };

            // TODO: probably an ibv lib function for this
            for (const auto* wr_it = wr; wr_it != nullptr; wr_it = wr_it->next) {
                post_send_event.num_wrs += 1;
                const Profiler::Events::IbvPostSendWr wr_event{
                        .context_addr = context_addr,
                        .qp_num = qp->qp_num,
                        .wr_id = wr_it->wr_id,
                        .num_sge = wr_it->num_sge,
                        .opcode = wr_it->opcode,
                        .send_flags = wr_it->send_flags,
                };
                m_event_recorder->Record(wr_event, ts_start + 1, ts_end - 1);

                // TODO: sg stuff isn't really needed...
                /*
                for (auto sg_idx = 0; sg_idx < wr_it->num_sge; sg_idx++) {
                    const auto& sg = wr_it->sg_list[sg_idx];

                    const auto sg_addr = sg.addr;
                    const auto sg_length = static_cast<size_t>(sg.length);

                    const Profiler::Events::IbvPostSendSg wr_sg_event{
                            .context_addr = context_addr,
                            .qp_num = qp->qp_num,
                            .wr_id = wr_it->wr_id,
                            .addr = sg_addr,
                            .length = sg_length,
                            .lkey = sg.lkey,
                    };
                    m_event_recorder->Record(wr_sg_event, ts_start + 2, ts_end - 2);
                }
                */
            }

            m_event_recorder->Record(post_send_event, ts_start, ts_end);
        });

        m_ibv_profiler->register_post<ibv::post_recv_tag>(
                [this]<typename TS>(auto&&, ibv_qp* qp, ibv_recv_wr* wr, ibv_recv_wr** bad_wr, TS ts_start, TS ts_end) {
            sigma_ignore_unused(bad_wr);

            if (wr->num_sge > 1) {
                std::cerr << "TODO: More than 1 SGE found" << std::endl;
            }

            const auto context_addr = reinterpret_cast<uint64_t>(qp->context);

            Profiler::Events::IbvPostRecv recv_event{
                    .context_addr = context_addr,
                    .qp_num = qp->qp_num,
                    .num_wrs = 0,
            };

            for (const auto* wr_it = wr; wr_it != nullptr; wr_it = wr_it->next) {
                recv_event.num_wrs += 1;

                const Profiler::Events::IbvPostRecvWr recv_wr_event{
                        .context_addr = context_addr,
                        .qp_num = qp->qp_num,
                        .wr_id = wr_it->wr_id,
                        .num_sge = wr_it->num_sge,
                };
                m_event_recorder->Record(recv_wr_event, ts_start + 1, ts_end - 1);
            }

            m_event_recorder->Record(recv_event, ts_start, ts_end);
        });

        m_ibv_profiler->register_post<ibv::poll_cq_tag>(
                [this]<typename TS>(int out_ret, ibv_cq* cq, int num_entries, ibv_wc* wc, TS ts_start, TS ts_end) {
            sigma_ignore_unused(num_entries);

            const auto context_addr = reinterpret_cast<uint64_t>(cq->context);

            // This is too expensive to log every time
            // {
            //     const Profiler::Events::IbvPollCq poll_cq_event{
            //             .context_addr = context_addr,
            //             .cq_addr = reinterpret_cast<uint64_t>(cq),
            //             // TODO: might be wrong. Look at cq->
            //             .num_requested = num_entries,
            //             .num_completed = out_ret,
            //     };
            //     m_event_recorder->Record(poll_cq_event, ts_start, ts_end);
            // }

            // Most polls won't have anything
            if (out_ret == 0) [[likely]] {
                return;
            }

            for (auto i = 0; i < out_ret; i++) {
                const auto& wc_entry = wc[i];

                const Profiler::Events::IbvCompletion completion_event{
                        .context_addr = context_addr,
                        .qp_num = wc_entry.qp_num,
                        .wr_id = wc_entry.wr_id,
                        .opcode = wc_entry.opcode,
                        .status = wc_entry.status,
                        .byte_len = wc_entry.byte_len,
                        .pkey_index = wc_entry.pkey_index,
                };
                m_event_recorder->Record(completion_event, ts_start + 1, ts_end - 1);
            }
        });
    }

    void load_nvbit(const std::filesystem::path& shim_dir) noexcept {
        m_cuda_profiler = get_instance<cuda_trace::NvbitProfiler>(shim_dir / Profiler::SIGMA_NVBIT_PROVIDER_PATH);
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

        /*
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
        */
    }

    void load_mlx5dv(const std::filesystem::path& shim_dir) {
        m_mlx5dv_profiler = get_instance<mlx5dv::Profiler>(shim_dir / Profiler::SIGMA_MLX5DV_PROVIDER_PATH);

        m_mlx5dv_profiler->register_post<mlx5dv::open_device_tag>(
                [this](ibv_context* out_context, ibv_device* device, mlx5dv_context_attr*, auto ts_start, auto ts_end) {
            // std::cout << "[Profiler] mlx5dv_open_device received " << out_context << std::endl;

            const auto event = register_nic(out_context, device);
            m_event_recorder->Record(event, ts_start, ts_end);
        });

        m_mlx5dv_profiler->register_post<mlx5dv::init_obj_tag>(
                [](auto&&, mlx5dv_obj* obj, uint64_t obj_type, auto&&...) {
            sigma_ignore_unused(obj);

            if (obj_type & MLX5DV_OBJ_QP) {
                // std::cout << "TODO: QP created" << std::endl;
            }

            if (obj_type & MLX5DV_OBJ_CQ) {
                // std::cout << "TODO: CQ created" << std::endl;
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

            if (!out_obj) {
                std::cerr << "mlx5dv::obj_create_tag resulted in null" << std::endl;
                return;
            }

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
            const auto cuda_err = CUDA_LOG(cudaHostAlloc(&uar_fake_page_addr, page_size, cudaHostAllocMapped));
            if (cuda_err != cudaSuccess) {
                return;
            }
            memset(uar_fake_page_addr, 0, page_size);

            // TODO: don't do this if buffers can't be set in the first place (no PeerMappingOverride)
            // Causes a segfault later
            const auto base_addr_real = out_uar->base_addr;
            const auto reg_addr_real = out_uar->reg_addr;

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
                [this](mlx5dv_devx_uar* out_uar, ibv_context* ctx, uint32_t flags, auto ts_start, auto ts_end) {
            Profiler::Events::Mlx5dvUarAlloc event{
                    .context_addr = PtrToUint(ctx),
                    .uar_addr = PtrToUint(out_uar),
                    .page_id = out_uar->page_id,
                    .base_addr = PtrToUint(out_uar->base_addr),
                    .reg_addr = PtrToUint(out_uar->reg_addr),
                    .flags = flags,
            };
            m_event_recorder->Record(event, ts_start, ts_end);

            std::scoped_lock lock(m_ibv_allocations_mutex);
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
        m_mlx5dv_profiler->register_pre<mlx5dv::devx_free_uar_tag>([this](mlx5dv_devx_uar* devx_uar, auto ts_start) {
            Profiler::Events::Mlx5dvUarFree event{
                    .uar_addr = PtrToUint(devx_uar),
                    .page_id = devx_uar->page_id,
            };
            m_event_recorder->Record(event, ts_start);
        });

        m_mlx5dv_profiler->register_post<mlx5dv::devx_umem_reg_tag>(
                [this]<typename TS>(mlx5dv_devx_umem* out_umem, [[maybe_unused]] ibv_context* ctx, void* addr,
                                    size_t size, [[maybe_unused]] uint32_t access, TS ts_start, TS ts_end) {
            m_event_recorder->Record<Profiler::Events::Mlx5dvMemoryRegistration>(
                    {
                            .context_addr = PtrToUint(ctx),
                            .umem_addr = PtrToUint(out_umem),
                            .mem_id = out_umem->umem_id,
                            .addr = PtrToUint(addr),
                            .size = size,
                    },
                    ts_start, ts_end);
        });

        // We can't do register_pre because the pointer will be gone
        m_mlx5dv_profiler->register_pre<mlx5dv::devx_umem_dereg_tag>([this](mlx5dv_devx_umem* umem, auto ts_start) {
            m_event_recorder->Record<Profiler::Events::Mlx5dvMemoryDereg>(
                    {
                            .umem_addr = PtrToUint(umem),
                            .mem_id = umem->umem_id,
                    },
                    ts_start);
        });
    }
};

// See Note[late_load]
static void __attribute__((constructor)) init() {
    ProfilerCtx::instance();
}

static void __attribute__((destructor)) finalize() {
    std::cout << "[Profiler] Exited Normally!" << std::endl;
}

/*
 * Note[late_load]: We delay full ProfilerCtx initialization until after __libc_start_main to prevent
 * segfaults in libraries. ProfilerCtx::instance() should only be used to initialize
 * modules with no symbol dependencies, mainly provider setup.
 */
using libc_start_main_type = int (*)(int (*)(int, char**, char**), int, char**, void (*)(), void (*)(), void (*)(),
                                     void*);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-declarations"
extern "C" int __libc_start_main(int (*main)(int, char**, char**), int argc, char** argv, void (*init)(void),
                                 void (*fini)(void), void (*rtld_fini)(void), void* stack_end) {

    static auto* real_func = reinterpret_cast<libc_start_main_type>(
            smart_dlsym::real_dlsym(smart_dlsym::DL_RTLD_NEXT, "__libc_start_main"));

    // At this point all libraries should be loaded
    ProfilerCtx::instance().LateLoad();

    return real_func(main, argc, argv, init, fini, rtld_fini, stack_end);
}
#pragma GCC diagnostic pop
