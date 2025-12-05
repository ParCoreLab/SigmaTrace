#include <cassert>
#include <deque>
#include <iostream>
#include <map>

#include <dlfcn.h>

#include <cuda_runtime.h>

#include "cuda_helper.hpp"

#include "ibverbs_trace.hpp"

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

    mutable std::mutex m_call_logs_mutex;
    std::deque<IbvCallLog> m_call_logs;

    std::atomic<size_t> m_total_send_bytes{0};

    Profiler() {
        std::cout << "Profiler Initialized" << std::endl;

        // TODO: maybe this should receive context and other stuff too?
        this->register_pre<post_send_tag>([this](ibv_qp *qp, ibv_send_wr *wr, ibv_send_wr **bad_wr) {
            std::cout << "==== [ibv_post_send] Pre Hook" << std::endl;

            // assert(wr->next == nullptr && "TODO: only 1 WR for now");
            assert(wr->num_sge == 1 && "TODO: only 1 SGE for now");

            for (const auto *wr_it = wr; wr_it != nullptr; wr_it = wr_it->next) {
                // TODO: Add total bytes for debug
                for (auto sg_idx = 0; sg_idx < wr_it->num_sge; sg_idx++) {
                    const auto &sg = wr_it->sg_list[sg_idx];
                    m_total_send_bytes.fetch_add(sg.length, std::memory_order_relaxed);

                    // TODO: probably an ibv lib function for this
                    // TODO: only 1 WR and 1 SG for now
                    const auto sg_addr = sg.addr;

                    const auto gpu_idx = this->find_gpu_index(sg_addr);

                    if (gpu_idx == -1) {
                        std::cout << "  ==== [ibv_post_send] on host memory" << std::endl;
                        return;
                    }

                    std::cout << "  ==== [ibv_post_send] from addr on GPU " << gpu_idx << std::endl;
                }
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

    // void post(post_send_tag, int ret, ibv_qp *qp, ibv_send_wr *wr, ibv_send_wr **bad_wr) {
    //     std::cout << "========= Pre post_send hook called 2 ========= " << std::endl;
    // }

    // Global pre
    template<typename Tag, typename... Args>
    void pre(Tag, Contexts & /*ctx*/, Args...) {
        // std::cout << "========= Calling " << HookTrait<Tag>::op_name << std::endl;

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
        /*
        std::cout << "  ========= Called " << HookTrait<Tag>::op_name
                  << " (calls: " << stats.calls.load(std::memory_order_relaxed) << ", ctx: " << ctx.ctx << ")"
                  << std::endl;
        */

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
                std::cout << "==== find_gpu_index found direct match" << std::endl;
                // entry = (length, device_id)
                const auto &entry = mr->second;
                return std::get<1>(entry);
            }
        }

        for (const auto [mr_addr, mr_info]: m_registered_mrs) {
            const auto [length, device_id] = mr_info;

            if (addr >= mr_addr && addr < (mr_addr + length)) {
                std::cout << "==== find_gpu_index found range match" << std::endl;
                return device_id;
            }
        }

        std::cout << "==== find_gpu_index no match found" << std::endl;

        return -1;
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

    cudaPointerAttributes attrs;
    CUDA_CHECK(cudaPointerGetAttributes(&attrs, addr));

    // TODO: defer exit function caller
    // TODO: WHAT ABOUT HOST REGISTERED MEMORY
    if (attrs.type == cudaMemoryTypeUnregistered) {
        return real_reg_mr_iova2(pd, addr, length, iova, access);
    }

    // std::cout << "It is device memory!!!!! " << attrs.device << std::endl;

    // TODO: for future
    assert(cuda_dev == attrs.device && "TODO: Registered device memory on a different GPU device?");

    auto &instance = Profiler::instance();
    auto &mrs_map = instance.m_registered_mrs;

    // TODO: dereg
    if (mrs_map.contains(reinterpret_cast<uintptr_t>(addr))) {
        assert(false && "TODO: Memory region already registered?");
    }

    // TODO: why reinterpret_cast?
    mrs_map.insert({reinterpret_cast<uintptr_t>(addr), {length, attrs.device}});

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

// TODO: for future
// // Hook ibv_reg_mr with NVTX markers
// {
//     pthread_once(&init_once, init_real_functions);
//
//     int gpu_device = get_gpu_device(addr);
//
//     nvtxEventAttributes_t eventAttrib = {0};
//     eventAttrib.version = NVTX_VERSION;
//     eventAttrib.size = NVTX_EVENT_ATTRIB_STRUCT_SIZE;
//     eventAttrib.colorType = NVTX_COLOR_ARGB;
//     eventAttrib.color = gpu_device >= 0 ? 0xFFFFAA00 : 0xFFAAAAAA;
//     eventAttrib.messageType = NVTX_MESSAGE_TYPE_ASCII;
//
//     char msg[256];
//     snprintf(msg, sizeof(msg), "ibv_reg_mr GPU%d %p len=%zu", gpu_device, addr, length);
//     eventAttrib.message.ascii = msg;
//
//     nvtxRangePushEx(&eventAttrib);
//     struct ibv_mr *mr = real_ibv_reg_mr(pd, addr, length, access);
//     nvtxRangePop();
//
//     return mr;
// }
