#include <deque>
#include <iostream>
#include <sstream>
#include <string_view>

#include "Dlsym.hpp"

#include "IbverbsEventSource.hpp"

constexpr bool DebugPrint = false;

// TODO: odd implementation
static void LogDebug(auto&& ss) {
    if constexpr (DebugPrint) {
        std::cout << "=== [ibv_hook] " << ss << std::endl;
    }
}

template<typename Tag, typename Tuple, std::size_t... Is>
static void LogCallImpl(Tuple&& args, std::index_sequence<Is...>) {
    std::stringstream ss;
    // clang-format off
    ss << "=== [ibv_hook] Called "
              << hook::HookTrait<Tag>::symbol_name
              << " (";

    ((ss
          << (Is == 0 ? "" : ", ")
          << std::get<Is>(hook::HookTrait<Tag>::arg_names)
          << ": "
          << std::get<Is>(args)), ...);
    // clang-format on

    ss << ')';

    LogDebug(ss.str());
}

template<typename Tag, typename... Args>
static void LogCall(Args&&... args) {
    static_assert(sizeof...(Args) == std::tuple_size_v<decltype(hook::HookTrait<Tag>::arg_names)>,
                  "Argument count does not match arg_names");

    LogCallImpl<Tag>(std::forward_as_tuple(std::forward<Args>(args)...), std::index_sequence_for<Args...>{});
}

// ================================================================
// Ibv profiler class with the hooks we care about
// ================================================================
namespace ibv {
    constexpr std::string_view LibPath{"libibverbs.so"};

    Profiler::Profiler() {
        LogDebug("Profiler Initialized");

        auto* libibverbs_handle = smart_dlsym::real_dlopen(LibPath.data(), smart_dlsym::DL_RTLD_NOW);
        resolve_all_preload(libibverbs_handle);

        // ================================================================
        // DEBUG: Log al calls
        // ================================================================
        if constexpr (DebugPrint) {
            this->register_all_pre([]<typename Tag>(auto&&... args) {
                LogCall<Tag>(std::forward<decltype(args)>(args)...);
            });
        }

        // Set up for installing hooks into ctx
        this->register_post<open_device_tag>([this](ibv_context* ctx, ibv_device*, auto&&...) {
            if (ctx) {
                this->install(ctx);
                LogDebug("Hooks installed");
            }
        });

        this->register_post<close_device_tag>([this](int, ibv_context* ctx, auto&&...) {
            if (ctx) {
                this->uninstall(ctx);
                LogDebug("Hooks uninstalled");
            }
        });
    }
} // namespace ibv

// Explicit instantiation - this is the ONE place the template is instantiated
template class ibv::IbverbsEventSource<ibv::Profiler>;

static void __attribute__((destructor)) finalize() {
    LogDebug("Unloaded");
}

// Accessor for late-loaded libraries to get the singleton instance via dlsym
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-declarations"
extern "C" ibv::Profiler* sigma_provider_instance() {
    return &ibv::Profiler::instance();
}
#pragma GCC diagnostic pop

extern "C" ibv_context* ibv_open_device(ibv_device* device) {
    return ibv::Profiler::invoke_preload<ibv::open_device_tag>(device);
}

extern "C" int ibv_close_device(ibv_context* context) {
    return ibv::Profiler::invoke_preload<ibv::close_device_tag>(context);
}

#ifdef ibv_reg_mr
#undef ibv_reg_mr
#endif
extern "C" ibv_mr* ibv_reg_mr(ibv_pd* pd, void* addr, size_t length, int access) {
    return ibv::Profiler::invoke_preload<ibv::reg_mr_tag>(pd, addr, length, access);
}

extern "C" int ibv_dereg_mr(ibv_mr* mr) {
    return ibv::Profiler::invoke_preload<ibv::dereg_mr_tag>(mr);
}

extern "C" ibv_mr* ibv_reg_mr_iova2(ibv_pd* pd, void* addr, size_t length, uint64_t iova, unsigned int access) {
    return ibv::Profiler::invoke_preload<ibv::reg_mr_iova2_tag>(pd, addr, length, iova, access);
}

extern "C" ibv_mr* ibv_reg_dmabuf_mr(ibv_pd* pd, uint64_t offset, size_t length, uint64_t iova, int fd, int access) {
    return ibv::Profiler::invoke_preload<ibv::reg_dmabuf_mr_tag>(pd, offset, length, iova, fd, access);
}

extern "C" ibv_pd* ibv_alloc_pd(ibv_context* context) {
    return ibv::Profiler::invoke_preload<ibv::alloc_pd_tag>(context);
}

extern "C" ibv_cq* ibv_create_cq(ibv_context* context, int cqe, void* cq_context, ibv_comp_channel* channel,
                                 int comp_vector) {
    return ibv::Profiler::invoke_preload<ibv::create_cq_tag>(context, cqe, cq_context, channel, comp_vector);
}

extern "C" ibv_qp* ibv_create_qp(ibv_pd* pd, ibv_qp_init_attr* qp_init_attr) {
    return ibv::Profiler::invoke_preload<ibv::create_qp_tag>(pd, qp_init_attr);
}

extern "C" int ibv_modify_qp(ibv_qp* qp, ibv_qp_attr* attr, int attr_mask) {
    return ibv::Profiler::invoke_preload<ibv::modify_qp_tag>(qp, attr, attr_mask);
}

extern "C" ibv_ah* ibv_create_ah(ibv_pd* pd, ibv_ah_attr* attr) {
    return ibv::Profiler::invoke_preload<ibv::create_ah_tag>(pd, attr);
}

// ================================================================
static void __attribute__((constructor /* (201) */)) init() {
    ibv::Profiler::instance();

    LogDebug("Loaded");
}
// ================================================================
