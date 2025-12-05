#define _GNU_SOURCE

#include <deque>
#include <iostream>

#include <dlfcn.h>

#include <cuda_runtime.h>

#include "cuda_helper.hpp"

#include "ibverbs_trace.hpp"

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
    mutable std::mutex m_call_logs_mutex;
    std::deque<IbvCallLog> m_call_logs;

    Profiler() {
        std::cout << "Profiler Initialized" << std::endl;

        // this->register_pre<post_send_tag>([](auto &&...) {
        // std::cout << "========= Pre post_send hook called ========= " << std::endl;
        // });
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

            std::cout << "[" << type_str << "] " << log.op_name << " at "
                      << std::chrono::duration_cast<std::chrono::microseconds>(log.time.time_since_epoch()).count()
                      << " us" << std::endl;
        }
    }

    void post(post_send_tag, int ret, ibv_qp *qp, ibv_send_wr *wr, ibv_send_wr **bad_wr) {
        std::cout << "========= Pre post_send hook called 2 ========= " << std::endl;
    }

    // Global pre
    template<typename Tag, typename... Args>
    void pre(Tag, Contexts & /*ctx*/, Args...) {
        std::cout << "========= Calling " << HookTrait<Tag>::op_name << std::endl;

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
        std::cout << "  ========= Called " << HookTrait<Tag>::op_name
                  << " (calls: " << stats.calls.load(std::memory_order_relaxed) << ", ctx: " << ctx.ctx << ")"
                  << std::endl;

        // TODO: not efficient right now
        const IbvCallLog log{
                .type = IbvCallLogType::Post,
                .time = std::chrono::steady_clock::now(),
                .op_name = HookTrait<Tag>::op_name,
        };

        std::lock_guard lock(m_mutex);
        m_call_logs.push_back(log);
    }
};

static ibv_context *(*real_open)(ibv_device *) = nullptr;

static int (*real_close)(ibv_context *) = nullptr;

static void __attribute__((constructor)) init() {
    Profiler::instance();
    std::cout << "[ibv_hook] Loaded\n";
}

static void __attribute__((destructor)) finalize() {
    std::cout << "[ibv_hook] Unloaded\n";
    Profiler::instance().print_stats();
}

extern "C" ibv_context *ibv_open_device(ibv_device *device) {
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
