#include <endian.h>

#include <atomic>
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

// Profiler Event definitions
#include "Events/CUDAEvents.hpp"
#include "Events/IbvEvents.hpp"
#include "Events/Mlx5DvEvents.hpp"

#ifdef SIGMA_PROVIDER_ucx_trace
#include "Events/UcxEvents.hpp"
#endif
#ifdef SIGMA_PROVIDER_libfabric_trace
#include "Events/LibfabricEvents.hpp"
#endif

#include <infiniband/mlx5dv.h>
#include <infiniband/verbs.h>

// Internal headers for mlx5 type definitions
// TODO: this is inside NVSHMEM. May be NVSHMEM or RDMA-CORE version dependent
#include <mlx5/mlx5_ifc.h>
#include <mlx5/mlx5_prm.h>

// Event providers
#include "CUDAEventSource.hpp"
#include "IbverbsEventSource.hpp"
#include "Mlx5dvEventSource.hpp"

#ifdef SIGMA_PROVIDER_ucx_trace
#include "UcxEventSource.hpp"
#endif
#ifdef SIGMA_PROVIDER_libfabric_trace
#include "LibfabricEventSource.hpp"
#endif

// Profiler-specific modules
#include "Modules/CudaExportTableOverwrite/ExportTableOverwrite.hpp"
#include "Modules/MemoryHijack/MemoryRepeater.hpp"

#include "ProfilerArgs.hpp"
#include "ProfilerProviders.hpp"

// ================================================
using AddrType = uint64_t;
using UmemIdType = uint32_t;
using UarPageIdType = std::remove_extent_t<decltype(mlx5dv_devx_uar::page_id)>;
static_assert(sizeof(UarPageIdType) == sizeof(uint32_t));
using MemoryRepeaterHandle = Profiler::MemoryRepeater::MemoryRepeaterHandle;
// ================================================

// Forward declarations here
namespace Profiler::hijack {
    std::mutex m_profiler_hijack_mutex;

    // ================================================
    // NIC UAR regaddresses allocated with mlx5dv_devx_alloc_uar
    // uar->reg_addr -> uar->page_id
    std::unordered_map<void*, UarPageIdType> Mlx5UarAllocations{};

    // Host addresses registered with cuMemHostRegister with CU_MEMHOSTREGISTER_DEVICEMAP
    // These are the uar->reg_addr from above
    std::unordered_set<void*> CuMemHostRegisterAddrs{};

    // Fake host pointers we create with repeaters
    // Real UAR address (uar->reg_addr) -> Fake UAR address (cudaHostAlloc-ed page)
    std::unordered_map<void*, void*> CuFakeUarPtrs{};
    // ================================================

    // ================================================
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-declarations"
    CUresult Fake_cuMemAlloc(CUdeviceptr* dptr, size_t bytesize) {
        auto* real_func = reinterpret_cast<decltype(cuMemAlloc)*>(CudaExportTable::get_real_cu_func("cuMemAlloc"));
        return real_func(dptr, bytesize);
    }
    static_assert(std::is_same_v<decltype(cuMemAlloc), decltype(Fake_cuMemAlloc)>);

    CUresult Fake_cuLaunchKernel(CUfunction f, unsigned int gridDimX, unsigned int gridDimY, unsigned int gridDimZ,
                                 unsigned int blockDimX, unsigned int blockDimY, unsigned int blockDimZ,
                                 unsigned int sharedMemBytes, CUstream hStream, void** kernelParams, void** extra);
    static_assert(std::is_same_v<decltype(cuLaunchKernel), decltype(Fake_cuLaunchKernel)>);
#pragma GCC diagnostic pop

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

// Convenience
using Helpers::PtrToUint;
using Profiler::Events::GpuId_T;

struct QpTrafficStats {
    std::atomic<uint64_t> bytes_sent{0};
    std::atomic<uint64_t> bytes_recv{0};
    uint32_t send_count{0};
    uint32_t recv_count{0};
};

struct PendingDoorbell {
    uint32_t prod_id;
    Helpers::TS::Timestamp timestamp;
};

struct QpInfo {
    CUcontext cuda_ctx;   // captured during umem_reg
    void* sq_base;        // host-accessible SQ buffer address
    uint32_t log_wq_size; // num WQEs = 1 << log_wq_size
    uint32_t qpn;
    GpuId_T gpu_id;
    cudaMemoryType gpu_mem_type;
    uint8_t service_type{0};
    // on_doorbell
    uint32_t _last_prod_id{0};
    uint32_t _oldest_uncopied_prod_id{0};
    bool _has_pending{false};
    // Remote endpoint info (populated during QP modify)
    uint32_t dest_qpn{0};
    uint64_t dest_interface_id{0};
    uint16_t dest_lid{0};
    // QP transport service type from QPC (0x5 = DCI)
    // Pending doorbells awaiting byte count resolution after batch memcpy+parse
    std::vector<PendingDoorbell> pending_doorbells{};
};

struct ProfilerCtx {
    // ================================================
    // GPU memory allocations
    // addr -> { size, gpu_id }
    // TODO: Make the value a proper struct so we don't have to do std::get<whatever->second>
    // std::map<uintptr_t, std::tuple<size_t, int>> m_gpu_allocations{};
    // ================================================

    std::mutex m_mlx5_umem_mutex;
    // TODO: boost::multi_index is better than this mess
    // Address registered with umem_reg -> gpu id
    // TODO: address should be void*
    std::unordered_map<AddrType, std::tuple<GpuId_T, cudaMemoryType, CUcontext>> m_mlx5_umem_addr_to_gpu;
    // Umem ID -> entry in the above map
    std::unordered_map<UmemIdType, decltype(m_mlx5_umem_addr_to_gpu)::iterator> m_mlx5_umem_to_entry;

    using MemoryRepeaterHandle = Profiler::MemoryRepeater::MemoryRepeaterHandle;
    std::unordered_map<UarPageIdType, std::vector<MemoryRepeaterHandle>> m_mlx5_uar_hijack_handles;

    // QP tracking
    std::mutex m_mlx5_qp_mutex;
    using QpId = uint32_t;
    std::unordered_map<QpId, QpInfo> m_mlx5_qp_info;
    // ================================================

    // ================================================
    // ibv memory tracking
    // address -> (size, device_id)
    // TODO: put this in a module with more info (memory type, device type) and better (binary) search
    std::map<uint64_t, std::tuple<uint64_t, GpuId_T>> m_ibv_memory_registrations;

    // Helper function for debugging
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
    // ================================================

    // ================================================
    // QP traffic statistics for communication graph
    std::mutex m_qp_traffic_mutex;
    std::unordered_map<uint32_t /*qpn*/, QpTrafficStats> m_qp_traffic;

    // QP connection info: src_qpn -> {dest_qpn, context_addr, node_guid}
    struct QpConnectionEntry {
        uint64_t context_addr;
        uint32_t dest_qpn;
        uint64_t dest_interface_id;
        uint64_t dest_subnet_prefix;
        uint16_t dest_lid;
        uint8_t port_num;
    };
    std::unordered_map<uint32_t /*src_qpn*/, QpConnectionEntry> m_qp_connections;
    // ================================================

    // ================================================
    // Stats
    std::atomic<int> num_nic_accesses{0};
    std::atomic<int> num_nic_doorbell_accesses{0};
    std::atomic<int> num_fake_nic_accesses{0};

    std::atomic<int> doorbell_submitted{0};
    std::atomic<int> doorbell_skipped{0};
    // ================================================

    ~ProfilerCtx() {
        std::cout << "[Profiler] num_nic_doorbell_accesses: " << std::dec << num_nic_doorbell_accesses << std::endl;
        std::cout << "[Profiler] num_fake_nic_accesses: " << std::dec << num_fake_nic_accesses << std::endl;

        std::cout << "[Profiler] doorbell_submitted: " << std::dec << doorbell_submitted << std::endl;
        std::cout << "[Profiler] doorbell_skipped: " << std::dec << doorbell_skipped << std::endl;
    }

    // Emit CommunicationSummary events - must be called before EventRecorder destruction
    void emit_communication_summaries() {
        if (!m_event_recorder) {
            return;
        }

        std::scoped_lock lock(m_qp_traffic_mutex);
        for (const auto& [qpn, stats]: m_qp_traffic) {
            uint32_t dest_qpn = 0;
            uint16_t dest_lid = 0;
            GpuId_T gpu_id = Profiler::Events::UnknownDeviceId;

            const auto conn_it = m_qp_connections.find(qpn);
            if (conn_it != m_qp_connections.end()) {
                dest_qpn = conn_it->second.dest_qpn;
                dest_lid = conn_it->second.dest_lid;
            }

            {
                std::scoped_lock qp_lock(m_mlx5_qp_mutex);
                const auto qp_it = m_mlx5_qp_info.find(qpn);
                if (qp_it != m_mlx5_qp_info.end()) {
                    gpu_id = qp_it->second.gpu_id;
                }
            }

            const Profiler::Events::CommunicationSummary summary{
                    .src_qpn = qpn,
                    .dest_qpn = dest_qpn,
                    .bytes_sent = stats.bytes_sent.load(),
                    .bytes_recv = stats.bytes_recv.load(),
                    .send_count = stats.send_count,
                    .recv_count = stats.recv_count,
                    .src_node_guid = 0,
                    .dest_node_guid = 0,
                    .gpu_id = gpu_id,
                    .dest_lid = dest_lid,
            };
            m_event_recorder->Record(summary, 0);
        }
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
        // ================================================================================================
        // Register CUDA API overwrites
        // We only register _v2 versions to avoid issues, but it should be fine
        Profiler::CudaExportTable::register_overwrite("cuLaunchKernel",
                                                      reinterpret_cast<void*>(Profiler::hijack::Fake_cuLaunchKernel));

        Profiler::CudaExportTable::register_overwrite("cuMemAlloc",
                                                      reinterpret_cast<void*>(Profiler::hijack::Fake_cuMemAlloc));

        Profiler::CudaExportTable::register_overwrite(
                "cuMemHostRegister_v2", reinterpret_cast<void*>(Profiler::hijack::Fake_cuMemHostRegister_v2));

        Profiler::CudaExportTable::register_overwrite(
                "cuMemHostUnregister", reinterpret_cast<void*>(Profiler::hijack::Fake_cuMemHostUnregister));

        Profiler::CudaExportTable::register_overwrite(
                "cuMemHostGetDevicePointer_v2",
                reinterpret_cast<void*>(Profiler::hijack::Fake_cuMemHostGetDevicePointer_v2));
        // ================================================================================================

        m_args = Profiler::ProfilerArgs::from_env();

        // Will be populated with things from profiler_args in the future
        m_event_recorder_config = {};

        if (m_args.LoadIbtrace) {
            load_ibv(m_args.ShimDir);
        }

        if (m_args.LoadMlx5dv) {
            load_mlx5dv(m_args.ShimDir);
        }

        // if (m_args.LoadSanitizer) {
        //     load_sanitizer(m_args.ShimDir);
        // }

        if (m_args.LoadCupti) {
            load_cupti(m_args.ShimDir);
        }

#ifdef SIGMA_PROVIDER_ucx_trace
        if (m_args.LoadUcx) {
            load_ucx(m_args.ShimDir);
        }
#endif

#ifdef SIGMA_PROVIDER_libfabric_trace
        if (m_args.LoadLibfabric) {
            load_libfabric(m_args.ShimDir);
        }
#endif
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

    // cuda_trace::SanitizerEventSource* m_sanitizer_profiler{nullptr};
    cuda_trace::CUPTIEventSource* m_cupti_profiler{nullptr};

#ifdef SIGMA_PROVIDER_ucx_trace
    ucx::Profiler* m_ucx_profiler{nullptr};
#endif
#ifdef SIGMA_PROVIDER_libfabric_trace
    libfabric::Profiler* m_libfabric_profiler{nullptr};
#endif

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
                [](ibv_context* out_context, ibv_device* device, const auto ts_start, const auto ts_end) {
            const auto event = instance().register_nic(out_context, device);
            instance().m_event_recorder->Record(event, ts_start, ts_end);
        });

        m_ibv_profiler->register_post<ibv::close_device_tag>(
                [](int, ibv_context* context, const auto ts_start, const auto ts_end) {
            const Profiler::Events::IbvCloseDevice event{
                    .context_addr = reinterpret_cast<uint64_t>(context),
            };
            instance().m_event_recorder->Record(event, ts_start, ts_end);
        });

        m_ibv_profiler->register_post<ibv::create_qp_tag>(
                [](ibv_qp* out_qp, ibv_pd*, ibv_qp_init_attr*, const auto ts_start, const auto ts_end) {
            const Profiler::Events::IbvCreateQp event{
                    .context_addr = reinterpret_cast<uint64_t>(out_qp->context),
                    .qp_num = out_qp->qp_num,
            };
            instance().m_event_recorder->Record(event, ts_start, ts_end);
        });

        m_ibv_profiler->register_pre<ibv::destroy_qp_tag>([](ibv_qp* qp, const auto ts_start) {
            const Profiler::Events::IbvDestroyQp event{
                    // WARNING: not qp->context. Why? I have no clue
                    .context_addr = reinterpret_cast<uint64_t>(qp->pd->context),
                    .qp_num = qp->qp_num,
            };
            instance().m_event_recorder->Record(event, ts_start);
        });

        m_ibv_profiler->register_post<ibv::modify_qp_tag>(
                [](int, ibv_qp* qp, ibv_qp_attr* attr, int attr_mask, const auto ts_start, const auto ts_end) {
            if (!(attr_mask & IBV_QP_DEST_QPN)) {
                return;
            }

            const auto context_addr = reinterpret_cast<uint64_t>(qp->context);

            const Profiler::Events::IbvModifyQp event{
                    .context_addr = context_addr,
                    .qp_num = qp->qp_num,
                    .attr = *attr,
            };
            instance().m_event_recorder->Record(event, ts_start, ts_end);

            // Extract and record QP connection info
            const auto dest_qpn = attr->dest_qp_num;
            const auto& dgid = attr->ah_attr.grh.dgid;
            const auto dest_interface_id = be64toh(dgid.global.interface_id);
            const auto dest_subnet_prefix = be64toh(dgid.global.subnet_prefix);
            const auto dest_lid = attr->ah_attr.dlid;
            const auto port_num = attr->ah_attr.port_num;

            const Profiler::Events::QpConnectionInfo conn_event{
                    .context_addr = context_addr,
                    .src_qpn = qp->qp_num,
                    .dest_qpn = dest_qpn,
                    .dest_interface_id = dest_interface_id,
                    .dest_subnet_prefix = dest_subnet_prefix,
                    .dest_lid = dest_lid,
                    .port_num = port_num,
            };
            instance().m_event_recorder->Record(conn_event, ts_start + 1, ts_end - 1);

            // Store connection info for shutdown summary
            auto& profiler = instance();
            std::scoped_lock lock(profiler.m_qp_traffic_mutex);
            profiler.m_qp_connections[qp->qp_num] = ProfilerCtx::QpConnectionEntry{
                    .context_addr = context_addr,
                    .dest_qpn = dest_qpn,
                    .dest_interface_id = dest_interface_id,
                    .dest_subnet_prefix = dest_subnet_prefix,
                    .dest_lid = dest_lid,
                    .port_num = port_num,
            };
        });

        // ================================================
        // Memory registration
        // TODO: this can be done automatically/declaratively by declaring which arg is addr, which one is length, etc.
        m_ibv_profiler->register_post<ibv::reg_mr_tag>(
                [](ibv_mr*, ibv_pd* pd, void* addr, size_t length, int, const auto ts_start, const auto ts_end) {
            const Profiler::Events::IbvMemoryReg event{
                    .context_addr = PtrToUint(pd->context),
                    .addr = PtrToUint(addr),
                    .size = length,
                    .type = Profiler::Events::IbvMemoryRegType::ibv_reg_mr_iova2,
            };
            instance().m_event_recorder->Record(event, ts_start, ts_end);
        });

        m_ibv_profiler->register_post<ibv::reg_mr_iova2_tag>([](ibv_mr*, ibv_pd* pd, void* addr, size_t length,
                                                                uint64_t, unsigned int, const auto ts_start,
                                                                const auto ts_end) {
            const Profiler::Events::IbvMemoryReg event{
                    .context_addr = PtrToUint(pd->context),
                    .addr = PtrToUint(addr),
                    .size = length,
                    .type = Profiler::Events::IbvMemoryRegType::ibv_reg_mr_iova2,
            };
            instance().m_event_recorder->Record(event, ts_start, ts_end);
        });

        m_ibv_profiler->register_post<ibv::post_send_tag>(
                [](int, ibv_qp* qp, ibv_send_wr* wr, ibv_send_wr**, const auto ts_start, const auto ts_end) {
            const auto context_addr = PtrToUint(qp->context);

            Profiler::Events::IbvPostSend post_send_event{
                    .context_addr = context_addr,
                    .num_wrs = 0,
                    .qp_num = qp->qp_num,
            };

            uint64_t total_bytes_sent = 0;

            // TODO: probably an ibv lib function for this
            for (const auto* wr_it = wr; wr_it != nullptr; wr_it = wr_it->next) {
                post_send_event.num_wrs += 1;
                const Profiler::Events::IbvPostSendWr wr_event{
                        .context_addr = context_addr,
                        .wr_id = wr_it->wr_id,
                        .qp_num = qp->qp_num,
                        .num_sge = wr_it->num_sge,
                        .send_flags = wr_it->send_flags,
                        .opcode = wr_it->opcode,
                };
                instance().m_event_recorder->Record(wr_event, ts_start + 1, ts_end - 1);

                // Sum scatter-gather lengths for traffic accounting
                for (auto sg_idx = 0; sg_idx < wr_it->num_sge; sg_idx++) {
                    total_bytes_sent += wr_it->sg_list[sg_idx].length;
                }
            }

            // TODO: eliminate lock
            // Accumulate send bytes for communication graph
            if (total_bytes_sent > 0) {
                auto& profiler = instance();
                std::scoped_lock lock(profiler.m_qp_traffic_mutex);
                auto& stats = profiler.m_qp_traffic[qp->qp_num];
                stats.bytes_sent += total_bytes_sent;
                stats.send_count += post_send_event.num_wrs;
            }

            instance().m_event_recorder->Record(post_send_event, ts_start, ts_end);
        });

        m_ibv_profiler->register_post<ibv::post_recv_tag>(
                [](int, ibv_qp* qp, ibv_recv_wr* wr, ibv_recv_wr**, const auto ts_start, const auto ts_end) {
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
                instance().m_event_recorder->Record(recv_wr_event, ts_start + 1, ts_end - 1);
            }

            instance().m_event_recorder->Record(recv_event, ts_start, ts_end);
        });

        m_ibv_profiler->register_post<ibv::poll_cq_tag>(
                [](int out_ret, ibv_cq* cq, int num_entries, ibv_wc* wc, const auto ts_start, const auto ts_end) {
            sigma_ignore_unused(num_entries);

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
            if (out_ret < 1) [[likely]] {
                return;
            }

            for (auto i = 0; i < out_ret; i++) {
                const auto& wc_entry = wc[i];

                const Profiler::Events::IbvCompletion completion_event{
                        .context_addr = PtrToUint(cq->context),
                        .wr_id = wc_entry.wr_id,
                        .qp_num = wc_entry.qp_num,
                        .opcode = wc_entry.opcode,
                        .status = wc_entry.status,
                        .byte_len = wc_entry.byte_len,
                };
                instance().m_event_recorder->Record(completion_event, ts_start + 1, ts_end - 1);

                // Accumulate recv bytes from completions
                if (wc_entry.opcode & IBV_WC_RECV) {
                    auto& profiler = instance();
                    std::scoped_lock lock(profiler.m_qp_traffic_mutex);
                    auto& stats = profiler.m_qp_traffic[wc_entry.qp_num];
                    stats.bytes_recv += wc_entry.byte_len;
                    stats.recv_count++;
                }
            }
        });
    }

    void load_mlx5dv(const std::filesystem::path& shim_dir) {
        m_mlx5dv_profiler = get_instance<mlx5dv::Profiler>(shim_dir / Profiler::SIGMA_MLX5DV_PROVIDER_PATH);

        m_mlx5dv_profiler->register_post<mlx5dv::open_device_tag>([](ibv_context* out_context, ibv_device* device,
                                                                     mlx5dv_context_attr*, const auto ts_start,
                                                                     const auto ts_end) {
            const auto event = instance().register_nic(out_context, device);
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
                        .cuda_ctx = gpu_ctx, // = cuda_ctx
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

        // 1. Fake UAR hijack
        m_mlx5dv_profiler->register_post<mlx5dv::devx_alloc_uar_tag>(
                [](mlx5dv_devx_uar* out_uar, ibv_context*, uint32_t, auto...) {
            std::scoped_lock lock(Profiler::hijack::m_profiler_hijack_mutex);
            Profiler::hijack::Mlx5UarAllocations.insert_or_assign(out_uar->reg_addr, out_uar->page_id);
        });

        // 2. Memory tracking
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

        // 2. For memory tracking
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
    }

    void load_cupti(const std::filesystem::path& shim_dir) {
        m_cupti_profiler = get_instance<cuda_trace::CUPTIEventSource>(shim_dir / Profiler::SIGMA_CUPTI_PROVIDER_PATH);

        m_cupti_profiler->register_pre<cuda_trace::cuda_kernel_launch_tag>(
                [](int32_t gpu_id, const char* kernel_name, auto ts_start) {
            std::cout << "[Profiler] [ " << gpu_id << "] CUPTI launched kernel " << kernel_name << std::endl;

            Profiler::Events::CUDAKernelLaunch event{
                    .gpu_id = static_cast<GpuId_T>(gpu_id),
            };
            strncpy(event.kernel_name, kernel_name, sizeof event.kernel_name);

            instance().m_event_recorder->Record(event, ts_start);
        });

        std::cout << "Profiler loaded CUPTI" << std::endl;
    }

#ifdef SIGMA_PROVIDER_ucx_trace
    // ================================================================================================
    // Thread-local correlation: set during UCX operations for cross-layer mapping
    // TODO: not really used properly right now
    static inline thread_local uint64_t tls_active_ucp_ep{0};

    void load_ucx(const std::filesystem::path& shim_dir) {
        m_ucx_profiler = get_instance<ucx::Profiler>(shim_dir / Profiler::SIGMA_UCX_PROVIDER_PATH);

        m_ucx_profiler->register_post<ucx::ucp_init_version_tag>(
                [](ucs_status_t status, unsigned, unsigned, const ucp_params_t*, const ucp_config_t*,
                   ucp_context_h* context_p, const auto ts_start, const auto ts_end) {
            std::cout << "[Profiler] ucp_init_version_tag called!" << std::endl;

            if (status != 0 || !context_p || !*context_p) {
                return;
            }

            const Profiler::Events::UcpInit event{
                    .context_addr = reinterpret_cast<uint64_t>(*context_p),
            };
            instance().m_event_recorder->Record(event, ts_start, ts_end);
        });

        m_ucx_profiler->register_post<ucx::ucp_worker_create_tag>([](ucs_status_t status, ucp_context_h context,
                                                                     const ucp_worker_params_t*, ucp_worker_h* worker_p,
                                                                     const auto ts_start, const auto ts_end) {
            if (status != 0 || !worker_p || !*worker_p) {
                return;
            }

            const Profiler::Events::UcpWorkerCreate event{
                    .context_addr = reinterpret_cast<uint64_t>(context),
                    .worker_addr = reinterpret_cast<uint64_t>(*worker_p),
            };
            instance().m_event_recorder->Record(event, ts_start, ts_end);
        });

        m_ucx_profiler->register_pre<ucx::ucp_worker_destroy_tag>([](ucp_worker_h worker, const auto ts_start) {
            const Profiler::Events::UcpWorkerDestroy event{
                    .worker_addr = reinterpret_cast<uint64_t>(worker),
            };

            instance().m_event_recorder->Record(event, ts_start);
        });

        m_ucx_profiler->register_post<ucx::ucp_ep_create_tag>([](ucs_status_t status, ucp_worker_h worker,
                                                                 const ucp_ep_params_t*, ucp_ep_h* ep_p,
                                                                 const auto ts_start, const auto ts_end) {
            if (status != 0 || !ep_p || !*ep_p) {
                return;
            }

            const Profiler::Events::UcpEpCreate event{
                    .worker_addr = reinterpret_cast<uint64_t>(worker),
                    .ep_addr = reinterpret_cast<uint64_t>(*ep_p),
            };
            instance().m_event_recorder->Record(event, ts_start, ts_end);
        });

        m_ucx_profiler->register_pre<ucx::ucp_ep_close_nbx_tag>(
                [](ucp_ep_h ep, const ucp_request_param_t*, const auto ts_start) {
            const Profiler::Events::UcpEpClose event{
                    .ep_addr = reinterpret_cast<uint64_t>(ep),
            };
            instance().m_event_recorder->Record(event, ts_start);
        });

        m_ucx_profiler->register_post<ucx::ucp_tag_send_nbx_tag>(
                [](ucs_status_ptr_t request, ucp_ep_h ep, const void*, size_t count, ucp_tag_t tag,
                   const ucp_request_param_t*, const auto ts_start, const auto ts_end) {
            tls_active_ucp_ep = reinterpret_cast<uint64_t>(ep);
            const Profiler::Events::UcpTagSend event{
                    .ep_addr = reinterpret_cast<uint64_t>(ep),
                    .tag = tag,
                    .size = count,
                    .request_addr = reinterpret_cast<uint64_t>(request),
            };
            instance().m_event_recorder->Record(event, ts_start, ts_end);
            tls_active_ucp_ep = 0;
        });

        m_ucx_profiler->register_post<ucx::ucp_tag_recv_nbx_tag>(
                [](ucs_status_ptr_t request, ucp_worker_h worker, void*, size_t count, ucp_tag_t tag,
                   ucp_tag_t tag_mask, const ucp_request_param_t*, const auto ts_start, const auto ts_end) {
            const Profiler::Events::UcpTagRecv event{
                    .worker_addr = reinterpret_cast<uint64_t>(worker),
                    .tag = tag,
                    .tag_mask = tag_mask,
                    .size = count,
                    .request_addr = reinterpret_cast<uint64_t>(request),
            };
            instance().m_event_recorder->Record(event, ts_start, ts_end);
        });

        m_ucx_profiler->register_post<ucx::ucp_put_nbx_tag>(
                [](ucs_status_ptr_t request, ucp_ep_h ep, const void*, size_t count, uint64_t remote_addr, ucp_rkey_h,
                   const ucp_request_param_t*, const auto ts_start, const auto ts_end) {
            tls_active_ucp_ep = reinterpret_cast<uint64_t>(ep);
            const Profiler::Events::UcpPut event{
                    .ep_addr = reinterpret_cast<uint64_t>(ep),
                    .size = count,
                    .remote_addr = remote_addr,
                    .request_addr = reinterpret_cast<uint64_t>(request),
            };
            instance().m_event_recorder->Record(event, ts_start, ts_end);
            tls_active_ucp_ep = 0;
        });

        m_ucx_profiler->register_post<ucx::ucp_get_nbx_tag>(
                [](ucs_status_ptr_t request, ucp_ep_h ep, void*, size_t count, uint64_t remote_addr, ucp_rkey_h,
                   const ucp_request_param_t*, const auto ts_start, const auto ts_end) {
            tls_active_ucp_ep = reinterpret_cast<uint64_t>(ep);
            const Profiler::Events::UcpGet event{
                    .ep_addr = reinterpret_cast<uint64_t>(ep),
                    .size = count,
                    .remote_addr = remote_addr,
                    .request_addr = reinterpret_cast<uint64_t>(request),
            };
            instance().m_event_recorder->Record(event, ts_start, ts_end);
            tls_active_ucp_ep = 0;
        });

        m_ucx_profiler->register_post<ucx::ucp_am_send_nbx_tag>(
                [](ucs_status_ptr_t request, ucp_ep_h ep, unsigned int am_id, const void*, size_t header_length,
                   const void*, size_t count, const ucp_request_param_t*, const auto ts_start, const auto ts_end) {
            tls_active_ucp_ep = reinterpret_cast<uint64_t>(ep);
            const Profiler::Events::UcpAmSend event{
                    .ep_addr = reinterpret_cast<uint64_t>(ep),
                    .am_id = am_id,
                    .header_size = header_length,
                    .payload_size = count,
                    .request_addr = reinterpret_cast<uint64_t>(request),
            };
            instance().m_event_recorder->Record(event, ts_start, ts_end);
            tls_active_ucp_ep = 0;
        });

        m_ucx_profiler->register_post<ucx::ucp_mem_map_tag>([](ucs_status_t status, ucp_context_h context,
                                                               const ucp_mem_map_params_t*, ucp_mem_h*,
                                                               const auto ts_start, const auto ts_end) {
            if (status != 0) {
                return;
            }

            const Profiler::Events::UcpMemMap event{
                    .context_addr = reinterpret_cast<uint64_t>(context),
                    .addr = 0, // Would need to dereference params to get addr
                    .size = 0,
            };
            instance().m_event_recorder->Record(event, ts_start, ts_end);
        });

        m_ucx_profiler->register_post<ucx::ucp_mem_unmap_tag>(
                [](ucs_status_t, ucp_context_h context, ucp_mem_h memh, const auto ts_start, const auto ts_end) {
            const Profiler::Events::UcpMemUnmap event{
                    .context_addr = reinterpret_cast<uint64_t>(context),
                    .mem_addr = reinterpret_cast<uint64_t>(memh),
            };
            instance().m_event_recorder->Record(event, ts_start, ts_end);
        });

        // TODO: this might spam
        m_ucx_profiler->register_post<ucx::ucp_worker_progress_tag>(
                [](unsigned int num_progressed, ucp_worker_h worker, const auto ts_start, const auto ts_end) {
            if (num_progressed == 0) {
                return;
            }

            const Profiler::Events::UcpWorkerProgress event{
                    .worker_addr = reinterpret_cast<uint64_t>(worker),
                    .num_progressed = num_progressed,
            };
            instance().m_event_recorder->Record(event, ts_start, ts_end);
        });

        std::cout << "[Profiler] Loaded UCX" << std::endl;
    }
    // ================================================================================================
#endif

#ifdef SIGMA_PROVIDER_libfabric_trace
    // ================================================================================================
    // Thread-local correlation: set during libfabric operations for cross-layer mapping
    // TODO: not really used properly right now
    static inline thread_local uint64_t tls_active_fi_ep{0};

    void load_libfabric(const std::filesystem::path& shim_dir) {
        m_libfabric_profiler = get_instance<libfabric::Profiler>(shim_dir / Profiler::SIGMA_LIBFABRIC_PROVIDER_PATH);

        // Object-ops: ep->msg->sendmsg (captures fi_send/fi_sendv/fi_sendmsg/fi_inject)
        m_libfabric_profiler->register_post<fi::ep_msg_sendmsg_tag>(
                [](ssize_t, fid_ep* ep, const fi_msg* msg, uint64_t flags, const auto ts_start, const auto ts_end) {
            tls_active_fi_ep = reinterpret_cast<uint64_t>(ep);
            uint64_t total_len = 0;
            if (msg && msg->msg_iov) {
                for (size_t i = 0; i < msg->iov_count; i++) {
                    total_len += msg->msg_iov[i].iov_len;
                }
            }
            const Profiler::Events::FiSend event{
                    .ep_addr = reinterpret_cast<uint64_t>(ep),
                    .context = msg ? reinterpret_cast<uint64_t>(msg->context) : 0,
                    .total_iov_len = total_len,
                    .flags = flags,
                    .dest_addr = msg ? msg->addr : 0,
            };
            instance().m_event_recorder->Record(event, ts_start, ts_end);
            tls_active_fi_ep = 0;
        });

        m_libfabric_profiler->register_post<fi::ep_msg_recvmsg_tag>(
                [](ssize_t, fid_ep* ep, const fi_msg* msg, uint64_t flags, const auto ts_start, const auto ts_end) {
            uint64_t total_len = 0;
            if (msg && msg->msg_iov) {
                for (size_t i = 0; i < msg->iov_count; i++) {
                    total_len += msg->msg_iov[i].iov_len;
                }
            }
            const Profiler::Events::FiRecv event{
                    .ep_addr = reinterpret_cast<uint64_t>(ep),
                    .context = msg ? reinterpret_cast<uint64_t>(msg->context) : 0,
                    .total_iov_len = total_len,
                    .flags = flags,
            };
            instance().m_event_recorder->Record(event, ts_start, ts_end);
        });

        m_libfabric_profiler->register_post<fi::ep_rma_readmsg_tag>(
                [](ssize_t, fid_ep* ep, const fi_msg_rma* msg, uint64_t, const auto ts_start, const auto ts_end) {
            tls_active_fi_ep = reinterpret_cast<uint64_t>(ep);
            uint64_t total_len = 0;
            uint64_t remote_addr = 0;
            uint64_t remote_key = 0;
            if (msg) {
                if (msg->msg_iov) {
                    for (size_t i = 0; i < msg->iov_count; i++) {
                        total_len += msg->msg_iov[i].iov_len;
                    }
                }
                if (msg->rma_iov && msg->rma_iov_count > 0) {
                    remote_addr = msg->rma_iov[0].addr;
                    remote_key = msg->rma_iov[0].key;
                }
            }

            const Profiler::Events::FiRdmaRead event{
                    .ep_addr = reinterpret_cast<uint64_t>(ep),
                    .context = msg ? reinterpret_cast<uint64_t>(msg->context) : 0,
                    .total_iov_len = total_len,
                    .remote_addr = remote_addr,
                    .remote_key = remote_key,
            };
            instance().m_event_recorder->Record(event, ts_start, ts_end);
            tls_active_fi_ep = 0;
        });

        m_libfabric_profiler->register_post<fi::ep_rma_writemsg_tag>(
                [](ssize_t, fid_ep* ep, const fi_msg_rma* msg, uint64_t, const auto ts_start, const auto ts_end) {
            tls_active_fi_ep = reinterpret_cast<uint64_t>(ep);
            uint64_t total_len = 0;
            uint64_t remote_addr = 0;
            uint64_t remote_key = 0;
            if (msg) {
                if (msg->msg_iov) {
                    for (size_t i = 0; i < msg->iov_count; i++) {
                        total_len += msg->msg_iov[i].iov_len;
                    }
                }
                if (msg->rma_iov && msg->rma_iov_count > 0) {
                    remote_addr = msg->rma_iov[0].addr;
                    remote_key = msg->rma_iov[0].key;
                }
            }

            const Profiler::Events::FiRdmaWrite event{
                    .ep_addr = reinterpret_cast<uint64_t>(ep),
                    .context = msg ? reinterpret_cast<uint64_t>(msg->context) : 0,
                    .total_iov_len = total_len,
                    .remote_addr = remote_addr,
                    .remote_key = remote_key,
            };
            instance().m_event_recorder->Record(event, ts_start, ts_end);
            tls_active_fi_ep = 0;
        });

        std::cout << "[Profiler] Loaded libfabric tracing" << std::endl;
    }
    // ================================================================================================
#endif
};

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

        auto& profiler = ProfilerCtx::instance();

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

        auto& profiler = ProfilerCtx::instance();
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

// See Note[late_load]
static void __attribute__((constructor)) init() {
    ProfilerCtx::instance();
}

static void __attribute__((destructor)) finalize() {
    // Emit CommunicationSummary events before the EventRecorder is destroyed
    // (static destruction order would destroy the recorder before ProfilerCtx)
    auto& profiler = ProfilerCtx::instance();
    profiler.emit_communication_summaries();
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
