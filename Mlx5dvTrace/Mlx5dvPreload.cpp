#include <dlfcn.h>

#include "Dlsym.hpp"

#include "Mlx5dvEventSource.hpp"

// ================================================================
namespace mlx5dv {
    Profiler::Profiler() {
        std::cout << "mlx5dv Profiler Created" << std::endl;

        this->register_all_pre([]<typename Tag>(auto &&...) {
            std::cout << "mlx5dv hit something!" << std::endl;
        });
    }

    Profiler::~Profiler() { std::cout << "mlx5dv Profiler Destroyed" << std::endl; }
} // namespace mlx5dv
// ================================================================

extern "C" int mlx5dv_init_obj(mlx5dv_obj *obj, uint64_t obj_type) {
    auto &profiler = mlx5dv::Profiler::instance();

    // TODO: this should be in a helper module in
    auto &real_func = profiler.real_func<mlx5dv::init_obj_tag>();
    if (!real_func) {
        auto *libmlx5_handle = smart_dlsym::real_dlopen("libmlx5.so", RTLD_NOW);
        profiler.resolve_all(libmlx5_handle, smart_dlsym::real_dlsym);
    }

    return mlx5dv::Profiler::invoke_preload<mlx5dv::init_obj_tag>(obj, obj_type);
}

// ================================================================
static void register_symbols() {
    static const std::unordered_map<std::string, void *> hook_map = {
            {"mlx5dv_init_obj", reinterpret_cast<void *>(mlx5dv_init_obj)},
    };

    auto &profiler = mlx5dv::Profiler::instance();

    for (auto &[symbol_name, symbol_hook]: hook_map) {
        smart_dlsym::register_symbol(symbol_name, [&symbol_name, &profiler, symbol_hook](void *real_func, auto &&...) {
            // Update the real function pointer in the profiler
            void **real_func_ptr = profiler.real_func_by_name(symbol_name);
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
