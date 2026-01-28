#include <string_view>

#include "Dlsym.hpp"

#include "Mlx5dvEventSource.hpp"

// ================================================================
namespace mlx5dv {
    constexpr std::string_view LibPath{"libmlx5.so"};

    Profiler::Profiler() {
        auto* libmlx5_handle = smart_dlsym::real_dlopen(LibPath.data(), smart_dlsym::DL_RTLD_NOW);
        resolve_all(libmlx5_handle, smart_dlsym::real_dlsym);
    }
} // namespace mlx5dv
// ================================================================

// Accessor for late-loaded libraries to get the singleton instance via dlsym
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-declarations"
extern "C" mlx5dv::Profiler* sigma_provider_instance() {
    return &mlx5dv::Profiler::instance();
}
#pragma GCC diagnostic pop

// TODO: this should be in a helper module in HookLib or done automatically
extern "C" int mlx5dv_init_obj(mlx5dv_obj* obj, uint64_t obj_type) {
    return mlx5dv::Profiler::invoke_preload<mlx5dv::init_obj_tag>(obj, obj_type);
}

extern "C" mlx5dv_devx_obj* mlx5dv_devx_obj_create(ibv_context* context, const void* in, size_t inlen, void* out,
                                                   size_t outlen) {
    return mlx5dv::Profiler::invoke_preload<mlx5dv::obj_create_tag>(context, in, inlen, out, outlen);
}

extern "C" mlx5dv_devx_umem* mlx5dv_devx_umem_reg(ibv_context* ctx, void* addr, size_t size, uint32_t access) {
    return mlx5dv::Profiler::invoke_preload<mlx5dv::devx_umem_reg_tag>(ctx, addr, size, access);
}

extern "C" int mlx5dv_devx_umem_dereg(mlx5dv_devx_umem* umem) {
    return mlx5dv::Profiler::invoke_preload<mlx5dv::devx_umem_dereg_tag>(umem);
}

extern "C" mlx5dv_devx_uar* mlx5dv_devx_alloc_uar(ibv_context* context, uint32_t flags) {
    return mlx5dv::Profiler::invoke_preload<mlx5dv::devx_alloc_uar_tag>(context, flags);
}

extern "C" void mlx5dv_devx_free_uar(mlx5dv_devx_uar* devx_uar) {
    /* return */ mlx5dv::Profiler::invoke_preload<mlx5dv::devx_free_uar_tag>(devx_uar);
}

// ================================================================
static void __attribute__((constructor)) init() {
    mlx5dv::Profiler::instance();
}
// ================================================================
