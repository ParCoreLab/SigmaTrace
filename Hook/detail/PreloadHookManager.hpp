#pragma once

#include <cassert>

#include "Dlsym.hpp"

#include "CallbackManager.hpp"
#include "HookTrait.hpp"

namespace hook::detail {
    // ================================================================
    // Real function pointer storage for preload-style hooks
    // TODO: Should this be here?
    // ================================================================
    template<typename... Tags>
    class RealFunctionStorage {
    public:
        template<typename Tag>
        HookTrait<Tag>::FuncPtr& get() {
            return std::get<typename HookTrait<Tag>::FuncPtr>(m_funcs);
        }

        // TODO: remove
        template<typename Tag>
        HookTrait<Tag>::FuncPtr get() const {
            return std::get<typename HookTrait<Tag>::FuncPtr>(m_funcs);
        }

    private:
        std::tuple<typename HookTrait<Tags>::FuncPtr...> m_funcs{};
    };

    // ================================================================
    // PreloadHookManager - Base for LD_PRELOAD style function interposition
    //
    // Usage:
    //   1. Define hooks using DEFINE_PRELOAD_HOOK macro
    //   2. Create a derived class inheriting from PreloadHookManager
    //   3. In your LD_PRELOAD library, define extern "C" functions that call invoke<Tag>()
    //
    // Example:
    //   struct MyHooks : PreloadHookManager<MyHooks, my_func_tag, other_func_tag> {
    //       // Optional: override pre/post for specific tags
    //       void pre(my_func_tag, int arg) { ... }
    //   };
    //
    //   extern "C" int my_func(int arg) {
    //       return MyHooks::invoke<my_func_tag>(arg);
    //   }
    //
    // TODO: We should probably check for ValidHookTag
    // but instead we accept all tags here to avoid having to
    // partition template args...
    // ================================================================
    template<typename... Tags>
    class PreloadHookManager {
        using RealFuncs = RealFunctionStorage<Tags...>;

    public:
        // ================================================================
        // Real function pointer management
        // ================================================================

        template<ValidCallbackTag Tag>
            requires is_preload_hook_v<Tag>
        HookTrait<Tag>::FuncPtr& real_func() {
            return m_real_funcs.template get<Tag>();
        }

        template<ValidCallbackTag Tag>
            requires is_preload_hook_v<Tag>
        void resolve_real(void* handle, HookTrait<Tag>::FuncPtr default_func = nullptr) noexcept {
            using Trait = HookTrait<Tag>;

            // Preload in case downstream users don't use dlsym
            auto** real_fn_ptr = &m_real_funcs.template get<Tag>();
            if (!*real_fn_ptr) {
                *real_fn_ptr = reinterpret_cast<Trait::FuncPtr>(smart_dlsym::real_dlsym(handle, Trait::symbol_name));
                if (!*real_fn_ptr) {
                    std::cerr << "[hook] Failed to resolve " << Trait::symbol_name << std::endl;
                    return;
                }
            }

            auto* hook_fn = smart_dlsym::real_dlsym(smart_dlsym::DL_RTLD_DEFAULT, Trait::symbol_name);
            if (!hook_fn || hook_fn == reinterpret_cast<void*>(*real_fn_ptr)) {
                if (!hook_fn) {
                    std::cerr << "[hook] WARN: No hook function for " << Trait::symbol_name << std::endl;
                }

                // Default to passing through to invoke_preload
                hook_fn = reinterpret_cast<void*>(default_func);
            }

            // Replace the function if downstream calls dlvsym. This ensures we has
            smart_dlsym::register_symbol(Trait::symbol_name, [real_fn_ptr, hook_fn](void* real_func, auto&&...) {
                // Switch to the dlsym version
                if (*real_fn_ptr) {
                    *real_fn_ptr = reinterpret_cast<Trait::FuncPtr>(real_func);
                }

                return hook_fn;
            });
        }

        template<typename Tag>
        void resolve_one(auto&&... args) noexcept {
            if constexpr (is_preload_hook_v<Tag>) {
                resolve_real<Tag>(std::forward<decltype(args)>(args)...);
            }
        }

        // Resolve all preload hooks
        void resolve_all(auto&&... args) noexcept {
            (resolve_one<Tags>(std::forward<decltype(args)>(args)...), ...);
        }

        // ================================================================
        // Static wrapper - called from LD_PRELOAD hook functions
        // ================================================================
        template<ValidCallbackTag Tag, typename... Args>
            requires is_preload_hook_v<Tag>
        auto invoke(Args... args) noexcept -> HookTrait<Tag>::RetType {
            auto real = this->real_func<Tag>();
            assert(real && "Real function not resolved - call resolve_real<Tag>() first");
            auto ret = real(args...);
            return ret;
        }

    protected:
        RealFuncs m_real_funcs;
    };

} // namespace hook::detail
