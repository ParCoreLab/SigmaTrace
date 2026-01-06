#include <deque>
#include <iostream>
#include <sstream>

#include <cassert>
#include <dlfcn.h>

#include "IbverbsEventSource.hpp"

// Helper dl_iterate_phdr walker
extern void *find_symbol_in_lib(const char *lib_pattern, const char *symbol_name);

constexpr bool DebugPrint = false;

// TODO: odd implementation
static void LogDebug(auto &&ss) {
    if constexpr (DebugPrint) {
        std::cout << "=== [ibv_hook] " << ss << std::endl;
    }
}

template<typename Tag, typename Tuple, std::size_t... Is>
static void LogCallImpl(Tuple &&args, std::index_sequence<Is...>) {
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
static void LogCall(Args &&...args) {
    static_assert(sizeof...(Args) == std::tuple_size_v<decltype(hook::HookTrait<Tag>::arg_names)>,
                  "Argument count does not match arg_names");

    LogCallImpl<Tag>(std::forward_as_tuple(std::forward<Args>(args)...), std::index_sequence_for<Args...>{});
}

// ================================================================
// Ibv profiler class with the hooks we care about
// ================================================================
namespace ibv {
    Profiler::Profiler() {
        LogDebug("Profiler Initialized");

        // ================================================================
        // DEBUG: Log al calls
        // ================================================================
        if constexpr (DebugPrint) {
            this->register_all_pre([]<typename Tag>(auto &&...args) {
                LogCall<Tag>(std::forward<decltype(args)>(args)...);
            });
        }

        // ================================================================
        // Set up for installing hooks into ctx
        // ================================================================
        this->register_post<open_device_tag>([this](ibv_context *ctx, ibv_device *device) {
            if (ctx) {
                this->install(ctx);

                LogDebug("Hooks installed");
            }
        });

        this->register_post<close_device_tag>([this](int, ibv_context *ctx) {
            if (ctx) {
                this->uninstall(ctx);

                LogDebug("Hooks uninstalled");
            }
        });
    }

    Profiler::~Profiler() { LogDebug("Profiler Destroyed"); }
} // namespace ibv

// Store the real dlsym/dlvsym
static void *(*real_dlopen)(const char *, int) = nullptr;

static void *(*real_dlsym)(void *, const char *) = nullptr;
static void *(*real_dlvsym)(void *, const char *, const char *) = nullptr;

static void __attribute__((constructor)) init() {
    real_dlopen = reinterpret_cast<decltype(real_dlopen)>(find_symbol_in_lib("libc", "dlopen"));

    real_dlsym = reinterpret_cast<decltype(real_dlsym)>(find_symbol_in_lib("libc", "dlsym"));
    real_dlvsym = reinterpret_cast<decltype(real_dlvsym)>(find_symbol_in_lib("libc", "dlvsym"));

    // Initialize profiler
    ibv::Profiler::instance();

    LogDebug("Loaded");
}

static void __attribute__((destructor)) finalize() { LogDebug("Unloaded"); }

// ================================================================
// dlsym/dlvsym interception for libraries that dynamically load ibv funcs
// TODO: can possibly be in namespace ibv
// TODO: can be generated in Hooks lib instead of here
// ================================================================
extern "C" ibv_context *ibv_open_device(ibv_device *device) {
    // TODO: Hack! Some libraries dlopen, some dlsym and others just use the symbols
    // This makes sure the symbol we use is the same as a link-time symbol
    // the upstream library uses (I think).
    // If we dlopen libibverbs in init(), NVSHMEM breaks
    // If we don't load it, NCCL breaks
    // go figure
    auto &profiler = ibv::Profiler::instance();
    auto &real_func = profiler.real_func<ibv::open_device_tag>();
    if (!real_func) {
        auto *libibverbs_handle = real_dlopen("libibverbs.so", RTLD_NOW);
        profiler.resolve_all(libibverbs_handle, real_dlsym);
    }

    return ibv::Profiler::invoke_preload<ibv::open_device_tag>(device);
}

extern "C" int ibv_close_device(ibv_context *context) {
    return ibv::Profiler::invoke_preload<ibv::close_device_tag>(context);
}

#ifdef ibv_reg_mr
#undef ibv_reg_mr
#endif
extern "C" ibv_mr *ibv_reg_mr(ibv_pd *pd, void *addr, size_t length, int access) {
    return ibv::Profiler::invoke_preload<ibv::reg_mr_tag>(pd, addr, length, access);
}

extern "C" int ibv_dereg_mr(ibv_mr *mr) { return ibv::Profiler::invoke_preload<ibv::dereg_mr_tag>(mr); }

extern "C" ibv_mr *ibv_reg_mr_iova2(ibv_pd *pd, void *addr, size_t length, uint64_t iova, unsigned int access) {
    return ibv::Profiler::invoke_preload<ibv::reg_mr_iova2_tag>(pd, addr, length, iova, access);
}

extern "C" ibv_mr *ibv_reg_dmabuf_mr(ibv_pd *pd, uint64_t offset, size_t length, uint64_t iova, int fd, int access) {
    return ibv::Profiler::invoke_preload<ibv::reg_dmabuf_mr_tag>(pd, offset, length, iova, fd, access);
}

extern "C" ibv_pd *ibv_alloc_pd(ibv_context *context) {
    return ibv::Profiler::invoke_preload<ibv::alloc_pd_tag>(context);
}

extern "C" ibv_cq *ibv_create_cq(ibv_context *context, int cqe, void *cq_context, ibv_comp_channel *channel,
                                 int comp_vector) {
    return ibv::Profiler::invoke_preload<ibv::create_cq_tag>(context, cqe, cq_context, channel, comp_vector);
}

extern "C" ibv_qp *ibv_create_qp(ibv_pd *pd, ibv_qp_init_attr *qp_init_attr) {
    return ibv::Profiler::invoke_preload<ibv::create_qp_tag>(pd, qp_init_attr);
}

extern "C" int ibv_modify_qp(ibv_qp *qp, ibv_qp_attr *attr, int attr_mask) {
    return ibv::Profiler::invoke_preload<ibv::modify_qp_tag>(qp, attr, attr_mask);
}

extern "C" ibv_ah *ibv_create_ah(ibv_pd *pd, ibv_ah_attr *attr) {
    return ibv::Profiler::invoke_preload<ibv::create_ah_tag>(pd, attr);
}

// Map of <ibv func name -> hook function ptr>
// Note: We use the new architecture where real funcs are stored in the manager
static void *get_hook_for_symbol(std::string_view symbol) {
    // Map symbol names to our hook functions
    static const std::unordered_map<std::string_view, void *> hook_map = {
            {"ibv_open_device", reinterpret_cast<void *>(ibv_open_device)},
            {"ibv_close_device", reinterpret_cast<void *>(ibv_close_device)},
            {"ibv_reg_mr", reinterpret_cast<void *>(ibv_reg_mr)},
            {"ibv_dereg_mr", reinterpret_cast<void *>(ibv_dereg_mr)},
            {"ibv_reg_mr_iova2", reinterpret_cast<void *>(ibv_reg_mr_iova2)},
            {"ibv_reg_dmabuf_mr", reinterpret_cast<void *>(ibv_reg_dmabuf_mr)},
            {"ibv_alloc_pd", reinterpret_cast<void *>(ibv_alloc_pd)},
            {"ibv_create_cq", reinterpret_cast<void *>(ibv_create_cq)},
            {"ibv_create_qp", reinterpret_cast<void *>(ibv_create_qp)},
            {"ibv_modify_qp", reinterpret_cast<void *>(ibv_modify_qp)},
            {"ibv_create_ah", reinterpret_cast<void *>(ibv_create_ah)},
    };

    auto it = hook_map.find(symbol);
    return it != hook_map.end() ? it->second : nullptr;
}

// extern "C" void *dlopen(const char *filename, int flags) {
//     assert(real_dlopen);
//
//     std::cout << "dlopen: " << filename << std::endl;
//
//     void* handle = real_dlopen(filename, RTLD_LAZY | RTLD_GLOBAL | flags);
//     return handle;
// }

extern "C" void *dlsym(void *handle, const char *symbol) {
    assert(real_dlsym);

    // std::cout << "Loading " << symbol << std::endl;

    auto *hook = get_hook_for_symbol(symbol);

    if (!hook) {
        return real_dlsym(handle, symbol);
    }

    // Resolve the real function and store it in the manager
    void *real_fn = real_dlsym(handle, symbol);

    // Update the real function pointer in the profiler
    auto &profiler = ibv::Profiler::instance();
    std::string_view sym{symbol};

    void **real_func_ptr = profiler.real_func_by_name(sym);
    if (real_func_ptr) {
        // std::cout << "=== [ibv_hook] dlsym replaced " << sym << std::endl;
        *real_func_ptr = real_fn;
    }

    return hook;
}

extern "C" void *dlvsym(void *handle, const char *symbol, const char *version) {
    assert(real_dlvsym);

    auto *hook = get_hook_for_symbol(symbol);

    if (!hook) {
        return real_dlvsym(handle, symbol, version);
    }

    // Resolve the real function and store it in the manager
    void *real_fn = real_dlvsym(handle, symbol, version);

    // Update the real function pointer in the profiler
    auto &profiler = ibv::Profiler::instance();
    std::string_view sym{symbol};

    void **real_func_ptr = profiler.real_func_by_name(sym);
    if (real_func_ptr) {
        // std::cout << "=== [ibv_hook] dlvsym replaced " << sym << std::endl;
        *real_func_ptr = real_fn;
    }

    return hook;
}
// ================================================================
