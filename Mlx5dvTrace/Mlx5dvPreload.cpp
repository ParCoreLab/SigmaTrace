#include <dlfcn.h>

#include "Dlsym.hpp"

#include "Mlx5dvEventSource.hpp"

// ================================================================
namespace mlx5dv {
    /*
    Profiler::Profiler() {
        std::cout << "mlx5dv Profiler Created" << std::endl;

        this->register_all_pre([]<typename Tag>(auto&&...) {
            std::cout << "mlx5dv hit something!" << std::endl;
        });
    }

    Profiler::~Profiler() {
        std::cout << "mlx5dv Profiler Destroyed" << std::endl;
    }
    */
} // namespace mlx5dv
// ================================================================

// Accessor for late-loaded libraries to get the singleton instance via dlsym
extern "C" mlx5dv::Profiler* sigma_provider_instance() {
    return &mlx5dv::Profiler::instance();
}

// TODO: this should be in a helper module in HookLib or done automatically
extern "C" int mlx5dv_init_obj(mlx5dv_obj* obj, uint64_t obj_type) {
    auto& profiler = mlx5dv::Profiler::instance();
    auto& real_func = profiler.real_func<mlx5dv::init_obj_tag>();
    if (!real_func) {
        auto* libmlx5_handle = smart_dlsym::real_dlopen("libmlx5.so", RTLD_NOW);
        profiler.resolve_all(libmlx5_handle, smart_dlsym::real_dlsym);
    }

    return mlx5dv::Profiler::invoke_preload<mlx5dv::init_obj_tag>(obj, obj_type);
}

extern "C" mlx5dv_devx_obj* mlx5dv_devx_obj_create(ibv_context* context, const void* in, size_t inlen, void* out,
                                                   size_t outlen) {
    return mlx5dv::Profiler::invoke_preload<mlx5dv::obj_create_tag>(context, in, inlen, out, outlen);
}

// TODO: this should be in a helper module in HookLib or be done automatically
template<typename Tag>
static void InitMoveThisSomewhereElse() {
    auto& profiler = mlx5dv::Profiler::instance();
    auto& real_func = profiler.real_func<Tag>();
    if (!real_func) {
        auto* libmlx5_handle = smart_dlsym::real_dlopen("libmlx5.so", RTLD_NOW);
        profiler.resolve_all(libmlx5_handle, smart_dlsym::real_dlsym);
    }
}

extern "C" mlx5dv_devx_umem* mlx5dv_devx_umem_reg(ibv_context* ctx, void* addr, size_t size, uint32_t access) {
    InitMoveThisSomewhereElse<mlx5dv::devx_umem_reg_tag>();
    return mlx5dv::Profiler::invoke_preload<mlx5dv::devx_umem_reg_tag>(ctx, addr, size, access);
}

extern "C" int mlx5dv_devx_umem_dereg(mlx5dv_devx_umem* umem) {
    return mlx5dv::Profiler::invoke_preload<mlx5dv::devx_umem_dereg_tag>(umem);
}

extern "C" mlx5dv_devx_uar* mlx5dv_devx_alloc_uar(ibv_context* context, uint32_t flags) {
    InitMoveThisSomewhereElse<mlx5dv::devx_alloc_uar_tag>();
    return mlx5dv::Profiler::invoke_preload<mlx5dv::devx_alloc_uar_tag>(context, flags);
}

extern "C" void mlx5dv_devx_free_uar(mlx5dv_devx_uar* devx_uar) {
    /* return */ mlx5dv::Profiler::invoke_preload<mlx5dv::devx_free_uar_tag>(devx_uar);
}

// ================================================================
static void register_symbols() {
    static const std::unordered_map<std::string, void*> hook_map = {
            {"mlx5dv_init_obj", reinterpret_cast<void*>(mlx5dv_init_obj)},
            {"mlx5dv_devx_umem_reg", reinterpret_cast<void*>(mlx5dv_devx_umem_reg)},
            {"mlx5dv_devx_umem_dereg", reinterpret_cast<void*>(mlx5dv_devx_umem_dereg)},
            {"mlx5dv_devx_alloc_uar", reinterpret_cast<void*>(mlx5dv_devx_alloc_uar)},
            {"mlx5dv_devx_free_uar", reinterpret_cast<void*>(mlx5dv_devx_free_uar)},

    };

    auto& profiler = mlx5dv::Profiler::instance();

    for (auto& [symbol_name, symbol_hook]: hook_map) {
        smart_dlsym::register_symbol(symbol_name, [&symbol_name, &profiler, symbol_hook](void* real_func, auto&&...) {
            // Update the real function pointer in the profiler
            void** real_func_ptr = profiler.real_func_by_name(symbol_name);
            if (real_func_ptr) {
                *real_func_ptr = real_func;
            }

            return symbol_hook;
        });
    }
}

static void __attribute__((constructor)) init() {
    register_symbols();

    // Initialize profiler
    mlx5dv::Profiler::instance();
}
// ================================================================
