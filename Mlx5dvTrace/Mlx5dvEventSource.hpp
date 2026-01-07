#pragma once

#include "Mlx5dvHooks.hpp"

#include "CompositeHookManager.hpp"

namespace mlx5dv {
    template<typename Derived>
    class Mlx5dvEventSource : public hook::CompositeHookManager<Derived, int, init_obj_tag> {
        using Base = hook::CompositeHookManager<Derived, int, init_obj_tag>;

    public:
        using Context = Base::Context;

    protected:
        Mlx5dvEventSource() = default;
    };
    // ================================================================
    struct Profiler : Mlx5dvEventSource<Profiler> {
        Profiler();
        virtual ~Profiler();
    };

} // namespace mlx5dv
