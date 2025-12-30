#pragma once

#include <type_traits>

#include "CallbackManager.hpp"
#include "HookTrait.hpp"
#include "detail/ObjectOpsHookManager.hpp"
#include "detail/PreloadHookManager.hpp"

namespace hook {
    // ================================================================
    // CompositeHookManager - Combines preload and object-ops hooks
    //
    // This provides a unified interface for registering callbacks
    // regardless of the underlying hook mechanism.
    //
    // Usage:
    //   struct MyHooks : CompositeHookManager<MyHooks, MyObjectType,
    //       preload_tag1, preload_tag2,  // Preload hooks
    //       objops_tag1, objops_tag2     // Object-ops hooks
    //   > {
    //       // Must implement for object-ops:
    //       template<typename Tag>
    //       auto& get_op(MyObjectType* obj) { ... }
    //
    //       template<typename Tag, typename... Args>
    //       MyObjectType* get_object_from_args(Args... args) { ... }
    //   };
    // ================================================================
    // TODO: Get rid of ObjectType
    template<typename Derived, typename ObjectType, typename... Tags>
    class CompositeHookManager : public CallbackManager<Derived, Tags...> {
        using Base = CallbackManager<Derived, Tags...>;

        /*
         *TODO: Right now both implementations get all the tags.
         *This is OK for now, but should use detail/PartitionTag.hpp in the future
         */
        using PreloadBase = detail::PreloadHookManager<Tags...>;
        using ObjectOpsBase = detail::ObjectOpsHookManager<Tags...>;

    public:
        using Context = detail::ObjectContext<Tags...>;

        // Singleton
        static Derived &instance() {
            static Derived mgr;
            return mgr;
        }

        // ================================================================
        // Unified callback registration
        // These just forward to the base CallbackManager
        // ================================================================
        using Base::clear_all_callbacks;
        using Base::clear_callbacks;
        using Base::register_post;
        using Base::register_pre;

        // ================================================================
        // 1. Preload hook management
        // ================================================================
        // using PreloadBase::real_func;
        void resolve_all(auto&&... args) { m_preload_impl.resolve_all(std::forward<decltype(args)>(args)...); }

        template<ValidHookTag Tag>
            requires is_preload_hook_v<Tag>
        HookTrait<Tag>::FuncPtr &real_func() {
            return m_preload_impl.template real_func<Tag>();
        }

        // TODO: this function is really odd but we need to somehow
        // resolve to the correct real_func<Tag> with a runtime parameter...
        void **real_func_by_name(std::string_view name) {
            void **func_ptr = nullptr;

            ([&]() -> bool {
                if constexpr (is_preload_hook_v<Tags>) {
                    if (HookTrait<Tags>::symbol_name == name) {
                        func_ptr = reinterpret_cast<void **>(&real_func<Tags>());
                        return true;
                    }
                }
                return false;
            }() || ...);

            return func_ptr;
        }

        // Static wrapper for preload hooks
        // TODO: remove, possibly. We can already call these through the instance
        template<ValidHookTag Tag, typename... Args>
            requires is_preload_hook_v<Tag>
        static auto invoke_preload(Args... args) -> HookTrait<Tag>::RetType {
            auto &mgr = instance();

            mgr.template invoke_all_pre<Tag>(args...);

            auto real = mgr.template real_func<Tag>();
            auto ret = real(args...);

            mgr.template invoke_all_post<Tag>(ret, args...);

            return ret;
        }

        // ================================================================
        // 2. Object-ops hook management
        // ================================================================
        Context *install(ObjectType *obj) {
            assert(obj && "HookManager ctx obj install called with null obj");

            std::lock_guard lock(m_contexts_mutex);

            // auto key = reinterpret_cast<uintptr_t>(obj);
            // if (m_contexts.contains(key)) {
            //     return m_contexts[key].get();
            // }
            //
            // auto ctx = std::make_unique<Context>();
            // ctx->set_object_ptr(obj);

            auto *ctx = m_object_ops_impl.get_context(obj);

            // Install object-ops hooks only
            // TODO: why pointer dereference?
            install_object_ops_hooks(obj, *ctx, std::make_index_sequence<sizeof...(Tags)>{});

            return ctx;
        }

        void uninstall(ObjectType *obj) {
            // TODO: maybe it's fine if it's null
            assert(obj && "HookManager ctx obj uninstalled called with null obj");

            std::lock_guard lock(m_contexts_mutex);

            auto &contexts = m_object_ops_impl.m_contexts;

            auto key = reinterpret_cast<uintptr_t>(obj);
            auto it = contexts.find(key);
            if (it == contexts.end()) {
                return;
            }

            auto *ctx = m_object_ops_impl.get_context(obj);
            uninstall_object_ops_hooks(obj, *ctx, std::make_index_sequence<sizeof...(Tags)>{});

            contexts.erase(it);
        }

        // Context *find(ObjectType *obj) {
        //     if (!obj) {
        //         return nullptr;
        //     }
        //
        //     std::lock_guard lock(m_contexts_mutex);
        //
        //     auto key = reinterpret_cast<uintptr_t>(obj);
        //     auto it = m_contexts.find(key);
        //     return it != m_contexts.end() ? it->second.get() : nullptr;
        // }

        // Static wrapper for object-ops hooks
        template<ValidHookTag Tag, typename... Args>
            requires is_object_ops_hook_v<Tag>
        static auto invoke_object_ops(Args... args) -> typename HookTrait<Tag>::RetType {
            auto &mgr = instance();

            ObjectType *obj = static_cast<Derived &>(mgr).template get_object_from_args<Tag>(args...);
            Context *ctx = mgr.m_object_ops_impl.find(obj);

            if (!ctx) {
                // Fallback - shouldn't happen in normal operation
                if constexpr (std::is_void_v<typename HookTrait<Tag>::RetType>) {
                    return;
                } else {
                    return typename HookTrait<Tag>::RetType{};
                }
            }

            // Pre-hooks with context
            mgr.template invoke_pre_with_context<Tag>(*ctx, args...);

            auto original = ctx->template original<Tag>();
            auto ret = original(args...);

            // Post-hooks with context
            mgr.template invoke_post_with_context<Tag>(*ctx, ret, args...);

            return ret;
        }

    protected:
        CompositeHookManager() = default;

        // Expose base class methods
        using Base::invoke_all_post;
        using Base::invoke_all_pre;

        // Context-aware invocation for object-ops
        template<ValidHookTag Tag, typename... Args>
        void invoke_pre_with_context(Context &ctx, Args... args) {
            if constexpr (requires(Derived &d, Tag t, Context &c, Args... a) { d.pre(t, c, a...); }) {
                static_cast<Derived &>(*this).pre(Tag{}, ctx, args...);
            }
            Base::template invoke_pre_callbacks<Tag>(args...);
        }

        template<ValidHookTag Tag, typename Ret, typename... Args>
        void invoke_post_with_context(Context &ctx, Ret ret, Args... args) {
            if constexpr (requires(Derived &d, Tag t, Context &c, Ret r, Args... a) { d.post(t, c, r, a...); }) {
                static_cast<Derived &>(*this).post(Tag{}, ctx, ret, args...);
            }
            Base::template invoke_post_callbacks<Tag>(ret, args...);
        }

    private:
        // TODO: this is ugly and should be in ObjectOpsHookManager
        // but since we really need Derived, it's here for now
        template<size_t... Is>
        void install_object_ops_hooks(ObjectType *obj, Context &ctx, std::index_sequence<Is...>) {
            (install_if_object_ops<std::tuple_element_t<Is, std::tuple<Tags...>>>(obj, ctx), ...);
        }

        template<ValidHookTag Tag>
        void install_if_object_ops(ObjectType *obj, Context &ctx) {
            if constexpr (is_object_ops_hook_v<Tag>) {
                auto &op = static_cast<Derived &>(*this).template get_op<Tag>(obj);
                ctx.template original<Tag>() = op;
                op = &Derived::template invoke_object_ops<Tag>;
            }
        }

        template<size_t... Is>
        void uninstall_object_ops_hooks(ObjectType *obj, Context &ctx, std::index_sequence<Is...>) {
            (uninstall_if_object_ops<std::tuple_element_t<Is, std::tuple<Tags...>>>(obj, ctx), ...);
        }

        template<ValidHookTag Tag>
        void uninstall_if_object_ops(ObjectType *obj, Context &ctx) {
            if constexpr (is_object_ops_hook_v<Tag>) {
                auto &op = static_cast<Derived &>(*this).template get_op<Tag>(obj);
                op = ctx.template original<Tag>();
            }
        }

        // Implementations
        PreloadBase m_preload_impl;
        mutable std::mutex m_contexts_mutex;
        ObjectOpsBase m_object_ops_impl;
    };

} // namespace hook
