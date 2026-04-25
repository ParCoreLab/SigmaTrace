#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <unordered_map>
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

using Timestamp = Helpers::TS::Timestamp;

struct QpTrafficStats {
    uint64_t bytes_sent{0};
    uint64_t bytes_recv{0};
    uint32_t send_count{0};
    uint32_t recv_count{0};

    QpTrafficStats& operator+=(const QpTrafficStats& o) {
        bytes_sent += o.bytes_sent;
        bytes_recv += o.bytes_recv;
        send_count += o.send_count;
        recv_count += o.recv_count;
        return *this;
    }
};

struct EndpointTrafficStats {
    uint64_t bytes_sent{0};
    uint64_t bytes_recv{0};
    uint32_t send_count{0};
    uint32_t recv_count{0};

    EndpointTrafficStats& operator+=(const EndpointTrafficStats& o) {
        bytes_sent += o.bytes_sent;
        bytes_recv += o.bytes_recv;
        send_count += o.send_count;
        recv_count += o.recv_count;
        return *this;
    }
};

// Sentinel for "no destination known" (recv side). fi_addr_t=0 is a valid AV address.
static constexpr uint64_t FI_DEST_UNKNOWN = UINT64_MAX; // matches FI_ADDR_UNSPEC

// Composite key for per-destination libfabric traffic tracking
struct FiTrafficKey {
    uint64_t ep_addr;
    uint64_t dest_addr; // fi_addr_t for sends, FI_DEST_UNKNOWN for recvs

    bool operator==(const FiTrafficKey&) const = default;

    // TODO: use absl
    struct Hash {
        size_t operator()(const FiTrafficKey& k) const noexcept {
            auto h1 = std::hash<uint64_t>{}(k.ep_addr);
            auto h2 = std::hash<uint64_t>{}(k.dest_addr);
            return h1 ^ (h2 << 1);
        }
    };
};

// Per-thread traffic accumulators merged at shutdown.
// The mutex is only taken on thread registration (first access per thread)
template<typename Key, typename Stats, typename Hash = std::hash<Key>>
struct ThreadLocalRegistry {
    // TODO: use absl
    using Map = std::unordered_map<Key, Stats, Hash>;

    Map& get_thread_local() {
        thread_local Map* cached = nullptr;
        if (cached) [[likely]] {
            return *cached;
        }

        // First call on this thread
        auto map = std::make_shared<Map>();
        cached = map.get();
        std::scoped_lock lock(mutex);
        accumulators.push_back(std::move(map));
        return *cached;
    }

    Map merge_all() {
        Map merged;
        std::scoped_lock lock(mutex);
        for (const auto& acc: accumulators) {
            for (const auto& [key, stats]: *acc) {
                merged[key] += stats;
            }
        }
        return merged;
    }

    void reset() {
        std::scoped_lock lock(mutex);
        accumulators.clear();
    }

    std::mutex mutex;
    std::vector<std::shared_ptr<Map>> accumulators;
};

using ThreadTrafficRegistry = ThreadLocalRegistry<uint32_t, QpTrafficStats>;

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
    // ================================================================
    // GPU memory allocations
    // addr -> { size, gpu_id }
    // TODO: Make the value a proper struct so we don't have to do std::get<whatever->second>
    // std::map<uintptr_t, std::tuple<size_t, int>> m_gpu_allocations{};
    // ================================================================

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
    // ================================================================

    // ================================================================
    // ibv memory tracking
    // address -> (size, device_id)
    // TODO: put this in a module with more info (memory type, device type) and better (binary) search
    std::map<uint64_t, std::tuple<uint64_t, GpuId_T>> m_ibv_memory_registrations;
    // ================================================================

    // ================================================================
    // QP traffic statistics for communication graph
    // TODO: make this more generic. It shouldn't be just ibv
    ThreadTrafficRegistry m_traffic_registry;

    // QP connection info: src_qpn -> {dest_qpn, context_addr, node_guid}
    // Low-frequency updates (only during QP setup), separate mutex is fine
    std::mutex m_qp_connections_mutex;
    struct QpConnectionEntry {
        uint64_t context_addr;
        uint32_t dest_qpn;
        uint64_t dest_interface_id;
        uint64_t dest_subnet_prefix;
        uint16_t dest_lid;
        uint8_t port_num;
    };
    std::unordered_map<uint32_t /*src_qpn*/, QpConnectionEntry> m_qp_connections;
    // ================================================================

    // ================================================================
    // Stats
    std::atomic<int> num_nic_accesses{0};
    std::atomic<int> num_nic_doorbell_accesses{0};
    std::atomic<int> num_fake_nic_accesses{0};

    std::atomic<int> doorbell_submitted{0};
    std::atomic<int> doorbell_skipped{0};
    // ================================================================

    // Emit CommunicationSummary events - must be called before EventRecorder destruction
    void emit_communication_summaries();
#endif // SIGMA_HAS_RDMA_CORE

    // ================================================================
    // UCX/Libfabric endpoint traffic statistics
#if SIGMA_HAS_UCX
    ThreadLocalRegistry<uint64_t, EndpointTrafficStats> m_ucx_send_registry; // keyed by ep_addr
    ThreadLocalRegistry<uint64_t, EndpointTrafficStats> m_ucx_recv_registry; // keyed by worker_addr
    void emit_ucx_summaries();
#endif
#if SIGMA_HAS_LIBFABRIC
    ThreadLocalRegistry<FiTrafficKey, EndpointTrafficStats, FiTrafficKey::Hash> m_fi_traffic_registry;
    void emit_fi_summaries();

    // EP address resolution: populated eagerly at data-path time from handler callbacks.
    // Stored here in the ProfilerCtx lifetime so it survives past provider static destruction.
    std::mutex m_fi_addr_mutex;
    std::unordered_map<uint64_t /*ep_addr*/, uint32_t /*own_hash*/> m_fi_ep_own_hash;
    std::unordered_map<uint64_t /*ep_addr*/, std::unordered_map<uint64_t /*fi_addr_t*/, uint32_t>> m_fi_dest_hash;
#endif
    // ================================================================

    // See Note[late_load]
    // Event module will interface with the filesystem and may print to stderr
    void LateLoad();

#if SIGMA_HAS_CUDA
    std::atomic<bool> is_cuda_loaded{false};
    void LateLoadCUDA();
    void* m_libcuda_handle{nullptr};
#endif

#if SIGMA_HAS_HIP
    std::atomic<bool> is_hip_loaded{false};
    void LateLoadHIP();
#endif

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
    void load_ibv(const std::filesystem::path& shim_dir, const Profiler::ProfilerArgs& args) noexcept;
    void load_mlx5dv(const std::filesystem::path& shim_dir);
#if SIGMA_HAS_CUDA
    void load_doorbell_hijack();
    void flush_pending_doorbells();
#endif
#endif

#if SIGMA_HAS_CUDA
    void load_cupti(const std::filesystem::path& shim_dir);
#endif

#if SIGMA_HAS_UCX
    // Thread-local correlation: set during UCX operations for cross-layer mapping
    // TODO: not really used properly right now
    void load_ucx(const std::filesystem::path& shim_dir, const Profiler::ProfilerArgs& args);
#endif

#if SIGMA_HAS_LIBFABRIC
    // Thread-local correlation: set during libfabric operations for cross-layer mapping
    // TODO: not really used properly right now
    void load_libfabric(const std::filesystem::path& shim_dir);
#endif
};

// Pointer to better control the lifetime
extern ProfilerCtx* g_ProfilerInstance;

static inline ProfilerCtx& instance() {
    return *g_ProfilerInstance;
}
