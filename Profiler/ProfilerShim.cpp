#include <csignal>
#include <pthread.h>

#include <atomic>
#include <future>

#include <absl/container/flat_hash_set.h>

#include "Dlsym.hpp"

#include "EventRecorder.hpp"
#include "Events/MetaEvents.hpp"
#include "Helpers/TscCalibration.hpp"

#include "Args/ProfilerArgs.hpp"
#include "ProfilerFeatures.hpp"

#if SIGMA_HAS_CUDA
#include "ExportTableOverwrite.hpp"
#endif

#if SIGMA_HAS_RDMA_CORE
#include "Events/IbvEvents.hpp"
#endif

#include "Modules/RankDetector/RankDetector.hpp"

#include "ProfilerShim.hpp"

// Global Profiler instance
ProfilerCtx* g_ProfilerInstance;

ProfilerCtx::~ProfilerCtx() {
#if SIGMA_HAS_RDMA_CORE
    std::cout << "[Profiler] num_nic_doorbell_accesses: " << std::dec << num_nic_doorbell_accesses << std::endl;
    std::cout << "[Profiler] num_fake_nic_accesses: " << std::dec << num_fake_nic_accesses << std::endl;

    std::cout << "[Profiler] doorbell_submitted: " << std::dec << doorbell_submitted << std::endl;
    std::cout << "[Profiler] doorbell_skipped: " << std::dec << doorbell_skipped << std::endl;
#endif
}

#if SIGMA_HAS_RDMA_CORE
// Emit CommunicationSummary events on exit
void ProfilerCtx::emit_communication_summaries() {
    if (!m_event_recorder) {
        return;
    }

    using Profiler::Events::CommLayer;

    // Merge all per-thread traffic accumulators into one map
    auto merged_traffic = m_traffic_registry.merge_all();

    // Track which QPs we've already emitted summaries for
    absl::flat_hash_set<uint32_t> emitted_qpns;

    for (const auto& [qpn, stats]: merged_traffic) {
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
                .bytes_sent = stats.bytes_sent,
                .bytes_recv = stats.bytes_recv,
                .send_count = stats.send_count,
                .recv_count = stats.recv_count,
                .src_node_guid = 0,
                .dest_node_guid = 0,
                .gpu_id = gpu_id,
                .dest_lid = dest_lid,
                .ep_addr = 0,
                .layer = CommLayer::IBV,
        };
        m_event_recorder->Record(summary, 0);
        emitted_qpns.insert(qpn);
    }

    // Emit zero-traffic summaries for QPs known from connection info but with no
    // traffic. This happens for passive receive-side QPs in one-sided RDMA (e.g.
    // NVSHMEM RDMA writes) where the remote QP never calls ibv_post_recv/poll_cq.
    // Without these entries, the dest_qpn in edges won't resolve to any node.
    for (const auto& [qpn, conn]: m_qp_connections) {
        if (emitted_qpns.contains(qpn)) {
            continue;
        }

        GpuId_T gpu_id = Profiler::Events::UnknownDeviceId;
        {
            std::scoped_lock qp_lock(m_mlx5_qp_mutex);
            const auto qp_it = m_mlx5_qp_info.find(qpn);
            if (qp_it != m_mlx5_qp_info.end()) {
                gpu_id = qp_it->second.gpu_id;
            }
        }

        const Profiler::Events::CommunicationSummary summary{
                .src_qpn = qpn,
                .dest_qpn = conn.dest_qpn,
                .bytes_sent = 0,
                .bytes_recv = 0,
                .send_count = 0,
                .recv_count = 0,
                .src_node_guid = 0,
                .dest_node_guid = 0,
                .gpu_id = gpu_id,
                .dest_lid = conn.dest_lid,
                .ep_addr = 0,
                .layer = CommLayer::IBV,
        };
        m_event_recorder->Record(summary, 0);
    }
}
#endif // SIGMA_HAS_RDMA_CORE

#if SIGMA_HAS_UCX
void ProfilerCtx::emit_ucx_summaries() {
    if (!m_event_recorder) {
        return;
    }

    using Profiler::Events::CommLayer;

    auto merged_send = m_ucx_send_registry.merge_all();
    for (const auto& [ep_addr, stats]: merged_send) {
        const Profiler::Events::CommunicationSummary summary{
                .bytes_sent = stats.bytes_sent,
                .send_count = stats.send_count,
                .gpu_id = Profiler::Events::UnknownDeviceId,
                .ep_addr = ep_addr,
                .layer = CommLayer::UCX,
        };
        m_event_recorder->Record(summary, 0);
    }

    auto merged_recv = m_ucx_recv_registry.merge_all();
    for (const auto& [worker_addr, stats]: merged_recv) {
        const Profiler::Events::CommunicationSummary summary{
                .bytes_recv = stats.bytes_recv,
                .recv_count = stats.recv_count,
                .gpu_id = Profiler::Events::UnknownDeviceId,
                .ep_addr = worker_addr,
                .layer = CommLayer::UCX,
        };
        m_event_recorder->Record(summary, 0);
    }
}
#endif // SIGMA_HAS_UCX

#if SIGMA_HAS_LIBFABRIC
void ProfilerCtx::emit_fi_summaries() {
    if (!m_event_recorder) {
        return;
    }

    using Profiler::Events::CommLayer;

    auto merged = m_fi_traffic_registry.merge_all();
    for (const auto& [ep_addr, stats]: merged) {
        const Profiler::Events::CommunicationSummary summary{
                .bytes_sent = stats.bytes_sent,
                .bytes_recv = stats.bytes_recv,
                .send_count = stats.send_count,
                .recv_count = stats.recv_count,
                .gpu_id = Profiler::Events::UnknownDeviceId,
                .ep_addr = ep_addr,
                .layer = CommLayer::Libfabric,
        };
        m_event_recorder->Record(summary, 0);
    }
}
#endif // SIGMA_HAS_LIBFABRIC

/*
 * See Note[late_load]
 * Event module will interface with the filesystem and may print to stderr
 */
void ProfilerCtx::LateLoad() {
    m_event_recorder = std::make_unique<Events::EventRecorder<>>(std::move(m_event_recorder_config));

    // Check for invariant TSC and warn if not available
    if (!Helpers::TS::check_invariant_tsc()) {
        std::cerr << "[Profiler] WARNING: Invariant TSC not detected (constant_tsc + nonstop_tsc). "
                  << "RDTSC timestamps may be unreliable across cores." << std::endl;
    }

    // Record initial TSC calibration event
    const auto snapshot = Helpers::TS::capture_tsc_snapshot();
    const Profiler::Events::TscCalibration calibration{
            .tsc_value = snapshot.tsc_value,
            .wall_ns = snapshot.wall_ns,
            .monotonic_ns = snapshot.monotonic_ns,
            .tsc_freq_hz = snapshot.tsc_freq_hz,
    };

    m_event_recorder->Record(calibration, 0);

    // Get rank from user args or the environment and record ProcessInfo event
    Profiler::Events::ProcessInfo process_info{};
    if (m_args.RankHint.has_value()) {
        const auto rank_hint = m_args.RankHint.value();
        process_info = {
                .rank = rank_hint,
                .local_rank = rank_hint,
                .world_size = rank_hint,
        };
    } else {
        const auto rank_info = Profiler::GetRankInfo(m_args.RankHintEnv);
        process_info = {
                .rank = rank_info.rank,
                .local_rank = rank_info.local_rank,
                .world_size = rank_info.world_size,
        };
    }
    m_event_recorder->Record(process_info, 0);

    if constexpr (Profiler::Features::HasCUDA) {
        smart_dlsym::real_dlopen("libcuda.so", RTLD_LAZY | RTLD_GLOBAL);
    }
}

// ================================================================================================
// Late loading dynamic libs

// Promote a library that is already loaded (RTLD_LOCAL) into global symbol scope
// so that our lazy-bound references to its symbols resolve correctly.
static void* promote_to_global(const char* name) {
    // Just try
    return smart_dlsym::real_dlopen(name, RTLD_LAZY | RTLD_GLOBAL | RTLD_NOLOAD);
}

#if SIGMA_HAS_CUDA
void ProfilerCtx::LateLoadCUDA() {
    if (is_cuda_loaded) {
        return;
    }

    // The target process has libcuda/libcudart loaded (possibly with RTLD_LOCAL).
    // Promote them to RTLD_GLOBAL so our direct symbol references resolve.
    auto* libcuda_handle = promote_to_global("libcuda.so");
    promote_to_global("libcudart.so");

    if (libcuda_handle) {
        m_libcuda_handle = libcuda_handle;
    }

    is_cuda_loaded.store(true);

    if (m_args.LoadCupti) {
        load_cupti(m_args.ShimDirectory);
    }
}

// TODO: this might miss the real first initialization
// However, an actual symbol override can break things
// This should also be done on cuGetExportTable or something
extern "C" CUresult Fake_cuInit(unsigned int Flags);
extern "C" CUresult Fake_cuInit(unsigned int Flags) {
    auto* real_cuInit = reinterpret_cast<decltype(cuInit)*>(Profiler::CudaExportTable::get_real_cu_func("cuInit"));
    if (!real_cuInit) {
        std::cerr << "No real_cuInit" << std::endl;
        std::abort();
    }
    const auto res = real_cuInit(Flags);

    g_ProfilerInstance->LateLoadCUDA();

    return res;
}
#endif

#if SIGMA_HAS_HIP
void ProfilerCtx::LateLoadHIP() {
    if (is_hip_loaded) {
        return;
    }

    promote_to_global("libamdhip64.so");

    is_hip_loaded.store(true);
}
#endif
// ================================================================================================

/*
 * WARNING: Some calls are unsafe in this function.
 * Do not use std::cout or related functions that may depend on libc
 * See Note[late_load]
 */
ProfilerCtx::ProfilerCtx() {
    m_args = Profiler::ProfilerArgs::from_env();

    // Will be populated with things from profiler_args in the future
    m_event_recorder_config = {
            .OutputDirectory = m_args.OutputDirectory,
            .OverwriteOutputDir = m_args.OutputOverwrite,
    };

#if SIGMA_HAS_CUDA
    // We need to wait for the target application to load cuda to bootstrap our own libs
    Profiler::CudaExportTable::register_overwrite("cuInit", reinterpret_cast<void*>(Fake_cuInit));
#endif

#if SIGMA_HAS_RDMA_CORE
    if (m_args.LoadIbtrace) {
        load_ibv(m_args.ShimDirectory, m_args);
    }

    if (m_args.LoadMlx5dv) {
        load_mlx5dv(m_args.ShimDirectory);
    }

    // Must be after mlx5dv
    if (m_args.EnableDoorbellHijack) {
        if (!m_args.LoadMlx5dv) {
            std::cerr << "[Profiler] mlx5dv hooks must be enabled for doorbell hijack support" << std::endl;
            std::abort();
        }

#if SIGMA_HAS_CUDA
        load_doorbell_hijack();
#endif
    }
#endif

#if SIGMA_HAS_UCX
    if (m_args.LoadUcx) {
        load_ucx(m_args.ShimDirectory, m_args);
    }
#endif

#if SIGMA_HAS_LIBFABRIC
    if (m_args.LoadLibfabric) {
        load_libfabric(m_args.ShimDirectory);
    }
#endif
}

/*
 * The move constructor is for reinitializing the profiler on fork
 * We selectively move some things and destroy the rest
 */
ProfilerCtx::ProfilerCtx([[maybe_unused]] ProfilerCtx&& o) noexcept {
    m_args = std::move(o.m_args);
    m_event_recorder_config = {};

#if SIGMA_HAS_RDMA_CORE
    this->m_ibv_profiler = std::move(o.m_ibv_profiler);
    this->m_mlx5dv_profiler = std::move(o.m_mlx5dv_profiler);
#endif

#if SIGMA_HAS_CUDA
    this->m_cuda_profiler = std::move(o.m_cuda_profiler);
    this->m_cupti_profiler = std::move(o.m_cupti_profiler);

    // *Some* things need a little push
    if (m_cupti_profiler) {
        m_cupti_profiler->subscribe_cupti();
    }
#endif

#if SIGMA_HAS_UCX
    this->m_ucx_profiler = std::move(o.m_ucx_profiler);
#endif

#if SIGMA_HAS_LIBFABRIC
    this->m_libfabric_profiler = std::move(o.m_libfabric_profiler);
#endif
}

#if SIGMA_HAS_CUDA
// Forward declaration — defined in CUDAHijack.cpp
// Not very clean but hack fix needs hack
namespace Profiler::hijack {
    void reset();
}
#endif

// ================================================================================================
// Init and Exit
static void profiler_atfork_child();

static std::atomic<bool> g_finalized{false};
static void profiler_finalize() {
    // like std::call_once but works with fork
    bool expected = false;
    if (!g_finalized.compare_exchange_strong(expected, true)) {
        return;
    }

    if (!g_ProfilerInstance) {
        return;
    }

    // We do it earlier here instead of the destructor to avoid segfaults
    const auto snapshot = Helpers::TS::capture_tsc_snapshot();
    const Profiler::Events::TscCalibration calibration{
            .tsc_value = snapshot.tsc_value,
            .wall_ns = snapshot.wall_ns,
            .monotonic_ns = snapshot.monotonic_ns,
            .tsc_freq_hz = snapshot.tsc_freq_hz,
    };
    g_ProfilerInstance->m_event_recorder->Record(calibration, 0);

    // TODO: this is ugly...
#if SIGMA_HAS_RDMA_CORE
#if SIGMA_HAS_CUDA
    g_ProfilerInstance->flush_pending_doorbells();
#endif
    g_ProfilerInstance->emit_communication_summaries();
#endif
#if SIGMA_HAS_UCX
    g_ProfilerInstance->emit_ucx_summaries();
#endif
#if SIGMA_HAS_LIBFABRIC
    g_ProfilerInstance->emit_fi_summaries();
#endif

    // Flush and truncate all mmap'd event files before destroying the profiler.
    // thread_local destructors won't run for threads still alive at shutdown.
    Events::ThreadBufferRegistry::finalize_all();

    delete g_ProfilerInstance;
    g_ProfilerInstance = nullptr;

    std::cout << "[Profiler] Exited Normally!" << std::endl;

    // Terminate immediately to prevent shared library destructors
    // from crashing during _dl_fini(). The profiler has already flushed all data.
    _exit(0);
}

static void __attribute__((constructor)) init() {
    g_ProfilerInstance = new ProfilerCtx();
    const auto res = pthread_atfork(nullptr, nullptr, profiler_atfork_child);
    if (res) {
        std::cerr << "[Profiled] Failed setting pthread_atfork" << std::endl;
        std::abort();
    }
}

static void __attribute__((destructor)) fini() {
    profiler_finalize();
}

static void profiler_at_exit() {
    profiler_finalize();
}

static void profiler_at_signal(int signum) {
    std::cerr << "[Profiler] Interrupt signal (" << signum << ") received." << std::endl;
    profiler_finalize();
    std::exit(signum);
}

/*
 * Things get weird when the process we're profiling forks a child.
 * Most of everything is cloned but we can't safely clone everything
 * especially files in EventRecorder.
 */
static void profiler_atfork_child() {
    /*
     * Orphan the EventRecorder instance. It's a memory leak but there's
     * no good cleaner way to avoid having its destructor run and truncate
     * and close the files
     */
    if (g_ProfilerInstance) {
        std::ignore = g_ProfilerInstance->m_event_recorder.release();
    }

    // Release buffers without flushing
    Events::ThreadBufferRegistry::reset_for_fork();

#if SIGMA_HAS_CUDA
    // Reset CUDA doorbell hijack namespace globals
    // They should probably not be globals to begin with
    Profiler::hijack::reset();
#endif

    // Abandon thread_local stuff. They don't survive fork
#if SIGMA_HAS_RDMA_CORE
    g_ProfilerInstance->m_traffic_registry.reset();
#endif
#if SIGMA_HAS_UCX
    g_ProfilerInstance->m_ucx_send_registry.reset();
    g_ProfilerInstance->m_ucx_recv_registry.reset();
#endif
#if SIGMA_HAS_LIBFABRIC
    g_ProfilerInstance->m_fi_traffic_registry.reset();
#endif

    /*
     * Here we soft reinitialize the Profiler
     * We make use of the move constructor above to move some things and reset the rest
     *Notably, provider handles that are opened with dlopen are kept as is
     */
    auto* new_profiler_instance = new ProfilerCtx(std::move(*g_ProfilerInstance));
    // TODO: might be a better idea to leak than delete
    delete g_ProfilerInstance;
    g_ProfilerInstance = new_profiler_instance;

    // Reset finalization guard so this child can finalize independently
    g_finalized.store(false);

    // Initialize a new EventRecorder and emit meta events
    instance().LateLoad();

    // Exit handler is not inherited on fork
    std::atexit(profiler_at_exit);
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
extern "C" int __libc_start_main(int (*main)(int, char**, char**), int argc, char** argv, void (*init)(),
                                 void (*fini)(), void (*rtld_fini)(), void* stack_end) {
    static auto* real_func = reinterpret_cast<libc_start_main_type>(
            smart_dlsym::real_dlsym(smart_dlsym::DL_RTLD_NEXT, "__libc_start_main"));

    // At this point all libraries should be loaded
    instance().LateLoad();

    // Set up exit handler
    std::atexit(profiler_at_exit);

    // Set up signals
    constexpr std::array handled_signals{
            SIGINT,
            SIGABRT,
    };
    for (const auto sig: handled_signals) {
        std::signal(sig, profiler_at_signal);
    }

    // We need to call this from the same thread the main function runs because some libraries
    // and launchers don't like it when you call dlopen from a different thread
    const auto res = real_func(main, argc, argv, init, fini, rtld_fini, stack_end);

    // We do it here because destructors have reverse order and will break providers
    profiler_finalize();

    return res;
}
#pragma GCC diagnostic pop
// ================================================================================================
