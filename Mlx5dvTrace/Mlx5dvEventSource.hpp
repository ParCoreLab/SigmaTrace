#pragma once

#include "Mlx5dvHooks.hpp"

#include "CompositeHookManager.hpp"

namespace mlx5dv {
    template<typename Derived>
    class Mlx5dvEventSource : public hook::CompositeHookManager<Derived, hook::DefaultHookManagerConfig, int,
                                                                init_obj_tag, devx_umem_reg_tag, devx_umem_dereg_tag,
                                                                obj_create_tag, devx_alloc_uar_tag, devx_free_uar_tag> {

    public:
        using Base = hook::CompositeHookManager<Derived, hook::DefaultHookManagerConfig, int, init_obj_tag,
                                                devx_umem_reg_tag, devx_umem_dereg_tag, obj_create_tag,
                                                devx_alloc_uar_tag, devx_free_uar_tag>;

        using Context = Base::Context;

    protected:
        Mlx5dvEventSource() = default;
    };
    // ================================================================
    struct Profiler : Mlx5dvEventSource<Profiler> {
        Profiler();
        ~Profiler() = default;
    };

} // namespace mlx5dv
