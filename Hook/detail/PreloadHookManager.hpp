#pragma once

// TODO: remove
#include <dlfcn.h>
#include <link.h>

#include <cassert>

#include "Dlsym.hpp"

#include "CallbackManager.hpp"
#include "HookTrait.hpp"

namespace hook::detail {
    /*
     * Scan all loaded shared objects to find a symbol, skipping a specific .so.
     * Used as fallback when normal dlsym resolution fails due to either
     * recursion to itself or weird rpath stuff some libraries from some package
     * managers (spack) have.
     * We try our best to resolve to the function the profiled application would
     * normally resolve to by delaying all this as late as possible.
     */
    struct SymbolScanCtx {
        const char* symbol;
        void* skip_base; // base address of .so to skip (our provider)
        void* result;
    };

    inline void* scan_loaded_libraries(const char* symbol, void* skip_base) noexcept {
        SymbolScanCtx ctx{
                symbol,
                skip_base,
                nullptr,
        };

        dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) -> int {
            auto& ctx = *static_cast<SymbolScanCtx*>(data);

            if (!info->dlpi_name || !info->dlpi_name[0])
                return 0;

            // Skip our own provider .so by base address (reliable across
            // symlinks, relative paths, etc. unlike string comparison)
            if (ctx.skip_base && reinterpret_cast<void*>(info->dlpi_addr) == ctx.skip_base) {
                return 0;
            }

            void* h = smart_dlsym::real_dlopen(info->dlpi_name, smart_dlsym::DL_RTLD_NOW | smart_dlsym::DL_RTLD_NOLOAD);
            if (!h) {
                return 0;
            }

            void* sym = smart_dlsym::real_dlsym(h, ctx.symbol);
            smart_dlsym::real_dlclose(h);

            if (sym) {
                ctx.result = sym;
                return 1;
            }
            return 0;
        }, &ctx);

        return ctx.result;
    }

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
        void register_real() noexcept {
            using Trait = HookTrait<Tag>;

            auto** real_fn_ptr = &m_real_funcs.template get<Tag>();
            if (*real_fn_ptr) {
                std::cerr << "[hook] ERROR: Real function for " << Trait::symbol_name << " already registered"
                          << std::endl;
                std::abort();
            }

            // Identify our own provider .so to detect self-resolution.
            // We look up our own hook via RTLD_DEFAULT and use dladdr to find which .so it's in.
            // If any resolution method returns a function in that same .so, it's our own hook.

            // Sunk cost fallacy over here because I wrote all this infrastructure for LD_PRELOAD
            // with dlsym but it looks like there are so many edge cases that it might've been
            // better to use LD_AUDIT and la_symbind64()
            // Are we seriously reading the ELF again? Insane.
            const void* our_hook = smart_dlsym::real_dlsym(smart_dlsym::DL_RTLD_DEFAULT, Trait::symbol_name);
            Dl_info our_info{};
            void* our_base = nullptr;
            if (our_hook && dladdr(our_hook, &our_info)) {
                our_base = our_info.dli_fbase;
            }

            auto is_self = [&](const void* resolved) -> bool {
                if (!our_base || !resolved) {
                    return false;
                }

                Dl_info ri{};
                return dladdr(resolved, &ri) && ri.dli_fbase == our_base;
            };

            // Try to resolve with whatever given handle is and set the pointer
            auto try_resolve = [&](void* h) -> bool {
                auto* resolved = smart_dlsym::real_dlsym(h, Trait::symbol_name);
                if (!resolved || is_self(resolved)) {
                    return false;
                }
                *real_fn_ptr = reinterpret_cast<Trait::FuncPtr>(resolved);
                return true;
            };

            // 1. RTLD_NEXT: searches after the calling .so in the link map.
            //    Preferred because it respects the application's actual library
            //    resolution order (avoids version mismatch with system vs spack libs).
            // if (try_resolve(smart_dlsym::DL_RTLD_NEXT))
            //     return;


            // 2. Specific library handle (from GetLibHandle/dlopen)
            // This get the wrong version
            /*
            if (handle && try_resolve(handle))
                return;
            */

            // 3. RTLD_DEFAULT (NULL): searches all loaded libraries
            // This never works but it may work
            if (try_resolve(nullptr)) {
                return;
            }

            // Give up and scan all loaded shared objects via dl_iterate_phdr,
            // skipping our own provider .so. This handles cases where both
            // RTLD_NEXT and RTLD_DEFAULT fail due to link map ordering.
            if (our_base) {
                auto* found = scan_loaded_libraries(Trait::symbol_name, our_base);
                if (found && !is_self(found)) {
                    *real_fn_ptr = reinterpret_cast<Trait::FuncPtr>(found);
                    return;
                }
            }

            std::cerr << "[hook] Failed to register real function for " << Trait::symbol_name << std::endl;
            std::abort();
        }

        template<ValidCallbackTag Tag>
            requires is_preload_hook_v<Tag>
        void register_hook(HookTrait<Tag>::FuncPtr default_func = nullptr) noexcept {
            using Trait = HookTrait<Tag>;

            /*
             * We no longer preload functions to avoid messing with the symbol resolution order
             * Instead, we wait for the target application to resolve and call the functions
             * and late load them at that poi
            // Preload in case downstream users don't use dlsym
            auto** real_fn_ptr = &m_real_funcs.template get<Tag>();
            if (!*real_fn_ptr) {
                *real_fn_ptr = reinterpret_cast<Trait::FuncPtr>(smart_dlsym::real_dlsym(handle, Trait::symbol_name));
            }
            */

            auto** real_fn_ptr = &m_real_funcs.template get<Tag>();
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
                // Update real function pointer (may be first resolution when using RTLD_NEXT)
                *real_fn_ptr = reinterpret_cast<Trait::FuncPtr>(real_func);

                return hook_fn;
            });
        }

        template<typename Tag>
        void resolve_hook_one(auto&&... args) noexcept {
            if constexpr (is_preload_hook_v<Tag>) {
                register_hook<Tag>(std::forward<decltype(args)>(args)...);
            }
        }

        // Resolve all preload hooks
        void resolve_hook_all(auto&&... args) noexcept {
            (resolve_hook_one<Tags>(std::forward<decltype(args)>(args)...), ...);
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
