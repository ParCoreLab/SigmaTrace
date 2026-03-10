#pragma once

#include <atomic>
#include <map>
#include <tuple>
#include <unordered_set>
#include <vector>

// Generic GPU types
#include "Helpers/GPUTypes.hpp"
#include "Helpers/Utilities.hpp"

#include "EventRecorder.hpp"

// Profiler Event definitions
#include "Events/Common.hpp"

// Profiler-specific modules
#include "Modules/MemoryHijack/MemoryRepeater.hpp"

// Event providers
#if SIGMA_HAS_CUDA
#include "CUDAEventSource.hpp"
#endif
#if SIGMA_HAS_RDMA_CORE
#include "IbverbsEventSource.hpp"
#include "Mlx5dvEventSource.hpp"
#endif
#if SIGMA_HAS_UCX
#include "UcxEventSource.hpp"
#endif
#if SIGMA_HAS_LIBFABRIC
#include "LibfabricEventSource.hpp"
#endif

#include "Args/ProfilerArgs.hpp"

// ================================================
using AddrType = uint64_t;
using UmemIdType = uint32_t;
using UarPageIdType = uint32_t;
using MemoryRepeaterHandle = Profiler::MemoryRepeater::MemoryRepeaterHandle;
// ================================================

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
    Helpers::GPU::Context gpu_ctx; // captured during umem_reg
    void* sq_base;                 // host-accessible SQ buffer address
    uint32_t log_wq_size;          // num WQEs = 1 << log_wq_size
    uint32_t qpn;
    GpuId_T gpu_id;
    Helpers::GPU::MemoryType gpu_mem_type;
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

template<typename T>
struct ProviderHandle {
    T* ptr{nullptr};
    void* dl_handle{nullptr};

    T* operator->() const noexcept {
        return ptr;
    }
    T& operator*() const noexcept {
        return *ptr;
    }
    explicit operator bool() const noexcept {
        return ptr != nullptr;
    }

    ~ProviderHandle() {
        if (dl_handle) {
            // TODO: this doesn't actually close because of STB_GNU_UNIQUE
            // smart_dlsym::real_dlclose(dl_handle);
            dl_handle = nullptr;
        }
    }

    ProviderHandle() = default;
    ProviderHandle(T* p, void* h) : ptr(p), dl_handle(h) {}
    ProviderHandle(const ProviderHandle&) = delete;
    ProviderHandle& operator=(const ProviderHandle&) = delete;
    ProviderHandle(ProviderHandle&& o) noexcept : ptr(o.ptr), dl_handle(o.dl_handle) {
        o.ptr = nullptr;
        o.dl_handle = nullptr;
    }
    ProviderHandle& operator=(ProviderHandle&& o) noexcept {
        if (this != &o) {
            if (dl_handle) {
                smart_dlsym::real_dlclose(dl_handle);
            }
            ptr = o.ptr;
            dl_handle = o.dl_handle;
            o.ptr = nullptr;
            o.dl_handle = nullptr;
        }
        return *this;
    }
};

struct ProfilerCtx {
    // ================================================
    // GPU memory allocations
    // addr -> { size, gpu_id }
    // TODO: Make the value a proper struct so we don't have to do std::get<whatever->second>
    // std::map<uintptr_t, std::tuple<size_t, int>> m_gpu_allocations{};
    // ================================================

#if SIGMA_HAS_RDMA_CORE
    std::mutex m_mlx5_umem_mutex;
    // TODO: boost::multi_index is better than this mess
    // Address registered with umem_reg -> gpu id
    // TODO: address should be void*
    std::unordered_map<AddrType, std::tuple<GpuId_T, Helpers::GPU::MemoryType, Helpers::GPU::Context>>
            m_mlx5_umem_addr_to_gpu;
    // Umem ID -> entry in the above map
    std::unordered_map<UmemIdType, typename decltype(m_mlx5_umem_addr_to_gpu)::iterator> m_mlx5_umem_to_entry;

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

    // Emit CommunicationSummary events - must be called before EventRecorder destruction
    void emit_communication_summaries();
#endif // SIGMA_HAS_RDMA_CORE

    // See Note[late_load]
    // Event module will interface with the filesystem and may print to stderr
    void LateLoad();

    // They fail if !SIGMA_HAS_X
    std::atomic<bool> is_cuda_loaded{false};
    void LateLoadCUDA();
    std::atomic<bool> is_hip_loaded{false};
    void LateLoadHIP();

    /*
     * WARNING: Some calls are unsafe in this function.
     * Do not use std::cout or related functions that may depend on libc
     * See Note[late_load]
     */
    // WARNING: Some calls are unsafe in this function, such as std::cout. We need to wait until __libc_start_main
    // See LateLoad
    ProfilerCtx();
    ~ProfilerCtx();

    /*
     * We have a move constructor to allow resetting the profiler
     * in the child after fork without reloading providers
     * Ideally we would just reload the providers too due to
     * several very painful reaasons we can't and also don't need to.
     * Instead, we just move what we need and reset the rest
     */
    ProfilerCtx(ProfilerCtx&&) noexcept;

    ProfilerCtx(ProfilerCtx&) = delete;
    ProfilerCtx(const ProfilerCtx&) = delete;

    Profiler::ProfilerArgs m_args;

    Events::RuntimeConfig m_event_recorder_config;
    std::unique_ptr<Events::EventRecorder<>> m_event_recorder{nullptr};

    // Provider handles
#if SIGMA_HAS_RDMA_CORE
    ProviderHandle<ibv::Profiler> m_ibv_profiler;
    ProviderHandle<mlx5dv::Profiler> m_mlx5dv_profiler;
#endif

#if SIGMA_HAS_CUDA
    ProviderHandle<cuda_trace::NvbitProfiler> m_cuda_profiler;
    ProviderHandle<cuda_trace::CUPTIEventSource> m_cupti_profiler;
#endif

#if SIGMA_HAS_UCX
    ProviderHandle<ucx::Profiler> m_ucx_profiler;
#endif
#if SIGMA_HAS_LIBFABRIC
    ProviderHandle<libfabric::Profiler> m_libfabric_profiler;
#endif

    // Handlers and Providers
#if SIGMA_HAS_RDMA_CORE
    void load_ibv(const std::filesystem::path& shim_dir) noexcept;
    void load_mlx5dv(const std::filesystem::path& shim_dir);
#if SIGMA_HAS_CUDA
    void load_doorbell_hijack();
#endif
#endif

#if SIGMA_HAS_CUDA
    void load_cupti(const std::filesystem::path& shim_dir);
#endif

#if SIGMA_HAS_UCX
    // Thread-local correlation: set during UCX operations for cross-layer mapping
    // TODO: not really used properly right now
    static inline thread_local uint64_t tls_active_ucp_ep{0};

    void load_ucx(const std::filesystem::path& shim_dir);
#endif

#if SIGMA_HAS_LIBFABRIC
    // Thread-local correlation: set during libfabric operations for cross-layer mapping
    // TODO: not really used properly right now
    static inline thread_local uint64_t tls_active_fi_ep{0};

    void load_libfabric(const std::filesystem::path& shim_dir);
#endif
};

// Pointer to better control the lifetime
extern ProfilerCtx* g_ProfilerInstance;

static inline ProfilerCtx& instance() {
    return *g_ProfilerInstance;
}
