#include <cassert>
#include <deque>
#include <iostream>
#include <map>
#include <queue>
#include <unordered_map>

#include <dlfcn.h>

#include <cuda_runtime.h>
#include <nvtx3/nvtx3.hpp>

#include "cuda_helper.hpp"

#include "ibverbs_trace.hpp"

// TODO: events
#include "EventRecorder.hpp"
#include "IbverbsEvents.hpp"

using Events::EventRecorder;
using namespace Events::Ibv;

// TODO
#ifdef ibv_reg_mr
#undef ibv_reg_mr
#endif

using namespace ibv_hook;

using Timestamp_t = std::chrono::steady_clock::time_point;

enum class IbvCallLogType {
    None,
    Pre,
    Post,
};

struct IbvCallLog {
    IbvCallLogType type{IbvCallLogType::None};
    Timestamp_t time;
    const char *op_name{nullptr};
};

// NVTX
// TODO: what is going on here? Can I create this dynamically?
// Categories start from 99 because idk someone has a conflict somewhere
static const nvtx3::named_category NvtxHostCategory{99, "HOST"};
static const std::array<nvtx3::named_category, 8> NvtxGpuCategories = {{
        {100, "GPU 0"},
        {101, "GPU 1"},
        {102, "GPU 2"},
        {103, "GPU 3"},
        {104, "GPU 4"},
        {105, "GPU 5"},
        {106, "GPU 6"},
        {107, "GPU 7"},
}};

// TODO: unused
struct NvtxRDMACategories {
    static constexpr uint32_t SEND = 1;
    static constexpr uint32_t RECV = 2;
    static constexpr uint32_t RDMA_WRITE = 3;
    static constexpr uint32_t RDMA_READ = 4;
    static constexpr uint32_t MR_REG = 5;
    static constexpr uint32_t CQ_POLL = 6;
};

struct NvtxPendingOperation {
    nvtx3::unique_range range;
    int gpu_device;
    uint32_t qp_num;
    // ibv_wr_opcode opcode;

    // We can only move the range otherwise it'll leave weird empty traces
    NvtxPendingOperation(nvtx3::unique_range &&_range, const int _gpu_device, const uint32_t _qp_num) :
        range(std::move(_range)), gpu_device(_gpu_device), qp_num(_qp_num) {}

    NvtxPendingOperation(NvtxPendingOperation &&) = default;
    NvtxPendingOperation &operator=(NvtxPendingOperation &&) = default;

    NvtxPendingOperation(const NvtxPendingOperation &) = delete;
    NvtxPendingOperation &operator=(const NvtxPendingOperation &) = delete;
};

struct Profiler : HookManager<Profiler, post_send_tag, post_recv_tag, poll_cq_tag
                              // req_notify_cq_tag,
                              // post_srq_recv_tag
                              > {
    // TODO: THIS SHOULD NOT BE A VECTOR OR A MAP (probably)
    // TODO: What happens if two GPUs on the same system have the same address range? (no UVA?)
    // TODO: mutex
    // Vector/maps of (address, size) or (address, (device_id, size))
    // std::vector<std::tuple<uintptr_t, size_t>> m_registered_mrs;
    std::map<uintptr_t, std::tuple<size_t, int>> m_registered_mrs;

    // pd -> device_id
    std::map<uintptr_t, int> m_registered_mrs_pd;

    mutable std::mutex m_call_logs_mutex;
    std::deque<IbvCallLog> m_call_logs;

    std::atomic<size_t> m_total_send_bytes{0};

    // NVTX
    // (QP, WR_ID, opcode_type) -> NvtxPendingOperation
    // (QP, WR_ID, is_send) -> NvtxPendingOperation
    struct PendingOpKey {
        uint32_t qp_num;
        uint64_t wr_id;
        bool is_send_queue;

        std::size_t operator()(const PendingOpKey &k) const {
            return std::hash<uint32_t>()(k.qp_num) ^ (std::hash<uint64_t>()(k.wr_id) << 1) ^
                   (std::hash<bool>()(k.is_send_queue) << 2);
        }

        bool operator==(const PendingOpKey &other) const {
            return qp_num == other.qp_num && wr_id == other.wr_id && is_send_queue == other.is_send_queue;
        }
    };
    std::mutex m_nvtx_pending_ops_mutex;
    std::unordered_map<PendingOpKey, std::queue<NvtxPendingOperation>, PendingOpKey> m_nvtx_pending_ops;

    Profiler() {
        std::cout << "Profiler Initialized" << std::endl;

        // TODO: maybe this should receive context and other stuff too?
        this->register_pre<post_send_tag>([this](ibv_qp *qp, ibv_send_wr *wr, ibv_send_wr **bad_wr) {
            // TODO: probably an ibv lib function for this
            for (const auto *wr_it = wr; wr_it != nullptr; wr_it = wr_it->next) {
                // TODO: Add total bytes for debug
                for (auto sg_idx = 0; sg_idx < wr_it->num_sge; sg_idx++) {
                    const auto &sg = wr_it->sg_list[sg_idx];
                    m_total_send_bytes.fetch_add(sg.length, std::memory_order_relaxed);

                    const auto sg_addr = sg.addr;
                    const auto gpu_idx = this->find_gpu_index(sg_addr);
                    const auto sg_length = static_cast<size_t>(sg.length);

                    // TODO: Build NVTX message
                    char msg[256];
                    if (gpu_idx == -1) {
                        snprintf(msg, sizeof(msg), "SEND QP%u HOST wr_id=%lu len=%zu", qp->qp_num, wr->wr_id,
                                 sg_length);
                    } else {
                        snprintf(msg, sizeof(msg), "SEND QP%u GPU%d wr_id=%lu len=%zu", qp->qp_num, gpu_idx, wr->wr_id,
                                 sg_length);
                    }
                    std::cout << msg << std::endl;

                    const nvtx3::event_attributes nvtx_attr{
                            nvtx3::message(msg),
                            gpu_idx == -1 ? NvtxHostCategory : NvtxGpuCategories[gpu_idx],
                    };

                    // TODO: pain
                    // Upstream program won't ibv_poll_cq for non signaled sends, so maybe nvtx3::mark it here
                    // if wr->send_flags & IBV_SEND_SIGNALED
                    // TODO: very late initialization of unique_range
                    NvtxPendingOperation pending_op{
                            nvtx3::unique_range{nvtx_attr},
                            gpu_idx,
                            qp->qp_num,
                    };

                    std::lock_guard lock(m_nvtx_pending_ops_mutex);

                    PendingOpKey pending_op_key{qp->qp_num, wr->wr_id, true};
                    auto &queue_ref = m_nvtx_pending_ops[pending_op_key];
                    if (!queue_ref.empty()) {
                        std::cout << "WARNING: Duplicate SEND wr_id=" << wr->wr_id << " on QP" << qp->qp_num
                                  << std::endl;
                    }

                    queue_ref.push(std::move(pending_op));
                }
            }
        });

        this->register_pre<post_recv_tag>([this](ibv_qp *qp, ibv_recv_wr *wr, ibv_recv_wr **bad_wr) {
            assert(wr->num_sge <= 1 && "TODO: More than 1 SGE found");

            // TODO: put this in a function (see ibv_post_send)
            for (const auto *wr_it = wr; wr_it != nullptr; wr_it = wr_it->next) {
                const auto gpu_idx = this->find_gpu_index_pd(reinterpret_cast<uintptr_t>(qp->pd));

                // TODO: Build NVTX message
                char msg[256];
                if (gpu_idx == -1) {
                    snprintf(msg, sizeof(msg), "RECV QP%u HOST wr_id=%lu", qp->qp_num, wr->wr_id);
                } else {
                    snprintf(msg, sizeof(msg), "RECV QP%u GPU%d wr_id=%lu", qp->qp_num, gpu_idx, wr->wr_id);
                }
                std::cout << msg << std::endl;

                // TODO: debugging
                {
                    EventRecorder::instance().Record<SimpleEvent>({.hello = wr_it->wr_id});
                }

                const nvtx3::event_attributes nvtx_attr{
                        nvtx3::message(msg),
                        gpu_idx == -1 ? NvtxHostCategory : NvtxGpuCategories[gpu_idx],
                };

                NvtxPendingOperation pending_op{
                        nvtx3::unique_range{nvtx_attr},
                        gpu_idx,
                        qp->qp_num,
                };

                std::lock_guard lock(m_nvtx_pending_ops_mutex);

                PendingOpKey pending_op_key{qp->qp_num, wr->wr_id, false};
                if (m_nvtx_pending_ops.contains(pending_op_key)) {
                    // TODO: Duplicates SHOULD only appear on sends
                    std::cout << "WARNING: Duplicate RECV wr_id=" << wr->wr_id << " on QP" << qp->qp_num << std::endl;
                    assert(false && "TODO: Duplicate RECV wr_id detected");
                }

                m_nvtx_pending_ops[pending_op_key].push(std::move(pending_op));
            }
        });

        this->register_post<poll_cq_tag>([this](int ret, ibv_cq *cq, int num_entries, ibv_wc *wc) {
            std::lock_guard lock(m_nvtx_pending_ops_mutex);

            // TODO: what is num_entries?
            for (auto i = 0; i < ret; i++) {
                const auto &wc_entry = wc[i];
                const auto wr_id = wc_entry.wr_id;

                bool is_send = (wc_entry.opcode != IBV_WC_RECV && wc_entry.opcode != IBV_WC_RECV_RDMA_WITH_IMM);

                PendingOpKey pending_op_key{wc_entry.qp_num, wr_id, is_send};
                auto it = m_nvtx_pending_ops.find(pending_op_key);
                if (it == m_nvtx_pending_ops.end()) {
                    continue;
                }

                auto &queue_ref = it->second;
                if (queue_ref.empty()) {
                    continue;
                }

                // TODO: is this actually moving?
                const auto &pending_op = std::move(queue_ref.front());

                const auto status = wc_entry.status;

                // Add a completion marker
                char msg[128];
                snprintf(msg, sizeof(msg), "COMPLETE QP%u wr_id=%lu status=%s GPU%d", wc_entry.qp_num, wr_id,
                         ibv_wc_status_str(status), pending_op.gpu_device);
                std::cout << msg << std::endl;

                // TODO: Maybe lockless or per-type buckets
                // Empty the queue
                std::remove_reference_t<decltype(queue_ref)>().swap(queue_ref);
            }
        });
    }

    virtual ~Profiler() {
        std::cout << "Profiler Destroyed" << std::endl;
        for (const auto &log: m_call_logs) {
            std::string type_str;
            switch (log.type) {
                case IbvCallLogType::Pre:
                    type_str = "Pre";
                    break;
                case IbvCallLogType::Post:
                    type_str = "Post";
                    break;
                default:
                    type_str = "None";
                    break;
            }

            // std::cout << "[" << type_str << "] " << log.op_name << " at "
            //           << std::chrono::duration_cast<std::chrono::microseconds>(log.time.time_since_epoch()).count()
            //           << " us" << std::endl;
        }

        std::cout << "Total logs: " << m_call_logs.size() << std::endl;

        // TODO: debug test
        std::cout << "\n\n\n === Total send bytes: " << m_total_send_bytes.load(std::memory_order_relaxed) << std::endl;
    }

    // Global pre
    template<typename Tag, typename... Args>
    void pre(Tag, Contexts & /*ctx*/, Args...) {
        // TODO: not efficient right now
        const IbvCallLog log{
                .type = IbvCallLogType::Pre,
                .time = std::chrono::steady_clock::now(),
                .op_name = HookTrait<Tag>::op_name,
        };

        std::lock_guard lock(m_mutex);
        m_call_logs.push_back(log);
    }

    // Global post
    template<typename Tag, typename Ret, typename... Args>
    void post(Tag, Contexts &ctx, Ret /*ret*/, Args... args) {
        const auto &stats = ctx.get_stats<Tag>();
        // TODO: not efficient right now
        const IbvCallLog log{
                .type = IbvCallLogType::Post,
                .time = std::chrono::steady_clock::now(),
                .op_name = HookTrait<Tag>::op_name,
        };

        std::lock_guard lock(m_mutex);
        m_call_logs.push_back(log);
    }

private:
    int find_gpu_index(const uintptr_t addr) {
        {
            const auto &mr = m_registered_mrs.find(addr);
            if (mr != m_registered_mrs.end()) {
                // entry = (length, device_id)
                const auto &entry = mr->second;
                return std::get<1>(entry);
            }
        }

        for (const auto [mr_addr, mr_info]: m_registered_mrs) {
            const auto [length, device_id] = mr_info;

            if (addr >= mr_addr && addr < (mr_addr + length)) {
                return device_id;
            }
        }

        // TODO: for debugging, remove later
        {
            cudaPointerAttributes attrs{};
            CUDA_CHECK(cudaPointerGetAttributes(&attrs, reinterpret_cast<void *>(addr)));
            assert(attrs.type == cudaMemoryTypeUnregistered && "CUDA Registered Address not previously registered");
        }

        return -1;
    }

    int find_gpu_index_pd(const uintptr_t pd_addr) {
        const auto &mr = m_registered_mrs_pd.find(pd_addr);
        if (mr == m_registered_mrs_pd.end()) {
            return -1;
        }

        return mr->second;
    }
};

static ibv_context *(*real_open)(ibv_device *) = nullptr;
static int (*real_close)(ibv_context *) = nullptr;
static ibv_mr *(*real_reg_mr)(ibv_pd *, void *, size_t, int) = nullptr;
static int (*real_dereg_mr)(ibv_mr *mr) = nullptr;
static ibv_mr *(*real_reg_mr_iova2)(ibv_pd *pd, void *addr, size_t length, uint64_t iova,
                                    unsigned int access) = nullptr;

static void __attribute__((constructor)) init() {
    Profiler::instance();
    std::cout << "[ibv_hook] Loaded\n";
}

static void __attribute__((destructor)) finalize() {
    std::cout << "[ibv_hook] Unloaded\n";
    Profiler::instance().print_stats();
}

extern "C" ibv_context *ibv_open_device(ibv_device *device) {
    // TODO: may not be very efficient but it's probably fine
    if (!real_open) {
        real_open = reinterpret_cast<decltype(real_open)>(dlvsym(RTLD_NEXT, "ibv_open_device", IBV_HOOK_VERSION));
    }

    auto *ctx = real_open(device);
    if (ctx) {
        Profiler::instance().install(ctx);
        std::cout << "[ibv_hook] Hooks installed on " << ctx << "\n";
    }

    return ctx;
}

extern "C" int ibv_close_device(ibv_context *context) {
    Profiler::instance().print_stats();

    if (!real_close) {
        real_close = reinterpret_cast<decltype(real_close)>(dlvsym(RTLD_NEXT, "ibv_close_device", IBV_HOOK_VERSION));
    }

    Profiler::instance().uninstall(context);
    return real_close(context);
}

extern "C" ibv_mr *ibv_reg_mr_iova2(ibv_pd *pd, void *addr, size_t length, uint64_t iova, unsigned int access) {
    if (!real_reg_mr_iova2) {
        // IBVERBS_1.8 taken from nccl/src/misc/ibvsymbols.cc
        real_reg_mr_iova2 =
                reinterpret_cast<decltype(real_reg_mr_iova2)>(dlvsym(RTLD_NEXT, "ibv_reg_mr_iova2", "IBVERBS_1.8"));
    }

    int cuda_dev;
    CUDA_CHECK(cudaGetDevice(&cuda_dev));

    // TODO: Somehow addr and iova are the same? Assert to verify in the future
    assert(addr == reinterpret_cast<decltype(addr)>(iova) && "TODO: addr and iova are not the same");

    cudaPointerAttributes attrs{};
    CUDA_CHECK(cudaPointerGetAttributes(&attrs, addr));

    // TODO: defer exit function caller
    // TODO: WHAT ABOUT HOST REGISTERED MEMORY
    if (attrs.type == cudaMemoryTypeUnregistered) {
        std::cout << "Some sort of unregistered memory" << std::endl;
        return real_reg_mr_iova2(pd, addr, length, iova, access);
    }

    // TODO: for future
    assert(cuda_dev == attrs.device && "TODO: Registered device memory on a different GPU device?");

    auto &instance = Profiler::instance();
    auto &mrs_map = instance.m_registered_mrs;
    auto &mrs_map_pd = instance.m_registered_mrs_pd;

    // TODO: dereg
    if (mrs_map.contains(reinterpret_cast<uintptr_t>(addr))) {
        assert(false && "TODO: Memory region already registered?");
    }

    // TODO: why reinterpret_cast?
    mrs_map.insert({reinterpret_cast<uintptr_t>(addr), {length, attrs.device}});

    // TODO: not chceking for duplicates
    mrs_map_pd.insert({reinterpret_cast<uintptr_t>(pd), attrs.device});

    return real_reg_mr_iova2(pd, addr, length, iova, access);
}

// TODO: actually do something with this
extern "C" int ibv_dereg_mr(ibv_mr *mr) {
    if (!real_dereg_mr) {
        real_dereg_mr = reinterpret_cast<decltype(real_dereg_mr)>(dlvsym(RTLD_NEXT, "ibv_dereg_mr", IBV_HOOK_VERSION));
    }

    return real_dereg_mr(mr);
}

/* TODO: This is a bit suspicious. nccl should be calling a version of ibv_reg_mr, but this func isn't hit
 * The hooks below are for mem reg functions that I'm unsure are ever called in nccl
 */
#define PROFILER_HOOK_FATAL_ERROR assert(false && (std::string(__func__) + " hook called! To be handled").c_str())

extern "C" ibv_mr *ibv_reg_mr(ibv_pd *pd, void *addr, size_t length, int access) {
    PROFILER_HOOK_FATAL_ERROR;

    if (!real_reg_mr) {
        real_reg_mr = reinterpret_cast<decltype(real_reg_mr)>(dlvsym(RTLD_NEXT, "real_reg_mr", IBV_HOOK_VERSION));
    }

    return real_reg_mr(pd, addr, length, access);
}
