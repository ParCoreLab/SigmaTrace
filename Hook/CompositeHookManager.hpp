#pragma once

#include <type_traits>

#include "Helpers/Timestamp.hpp"

#include "CallbackManager.hpp"
#include "HookTrait.hpp"

#include "detail/ObjectOpsHookManager.hpp"
#include "detail/PreloadHookManager.hpp"

namespace hook {
    // ================================================================
    namespace TS = Helpers::TS;

    // Concept requires TimestampSource, other fields have defaults
    template<typename T>
    concept HookManagerConfig = requires {
        // Must specify a TimestampSource type
        typename T::TimestampSourceType;
        requires TS::TimestampSource<typename T::TimestampSourceType>;
    };

    struct DefaultHookManagerConfig {
        using TimestampSourceType = TS::RdtscTimestampSource;
    };
    static_assert(HookManagerConfig<DefaultHookManagerConfig>);
    // ================================================================

    // ================================================================
    // Concepts for better error messages

    // Check if Derived implements get_op
    template<typename D, typename Tag, typename Obj>
    concept has_get_op_for = requires(D &d, Obj *obj) { d.template get_op<Tag>(obj); };
    // ===============================================================

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
    template<typename Derived, HookManagerConfig Config, typename ObjectType, typename... Tags>
    class CompositeHookManager : public CallbackManager<Derived, Tags...> {
        static_assert(!ValidHookTag<ObjectType>,
                      "Missing ObjectType. Did you mistakenly define as <Derived, ...Tags>?");

        using Base = CallbackManager<Derived, Tags...>;

        using TimestampSource = Config::TimestampSourceType;
        TimestampSource m_timestamp{};

        /*
         *TODO: Right now both implementations get all the tags.
         *This is OK for now, but should use detail/PartitionTag.hpp in the future
         */
        using PreloadBase = detail::PreloadHookManager<Tags...>;
        using ObjectOpsBase = detail::ObjectOpsHookManager<Tags...>;

    public:
        using Context = detail::ObjectContext<Tags...>;

        // Singleton
        // TODO: unique_ptr and return a pointer
        static Derived &instance() {
            static auto *mgr = new Derived();
            return static_cast<Derived &>(*mgr);
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
        // Helper to register pre-callbacks for all tags automatically
        // Callback signature: void(const char* symbol_name, Args...)
        // ================================================================
        template<typename Callback>
        void register_all_pre(Callback &&callback) {
            (register_pre_for_tag<Tags>(std::forward<Callback>(callback)), ...);
        }

    private:
        template<typename Tag, typename Callback>
        void register_pre_for_tag(Callback &&callback) {
            this->template register_pre<Tag>([callback](auto &&...args) {
                callback.template operator()<Tag>(std::forward<decltype(args)>(args)...);
            });
        }

    public:
        // ================================================================
        // 1. Custom event hooks
        // ================================================================
        template<ValidEventTag Tag, typename... Args>
            requires is_custom_hook_v<Tag>
        void invoke_custom(Args &&...args) noexcept {
            // TODO: pre only
            auto ts_start = m_timestamp.now();
            this->template invoke_all_pre<Tag>(std::forward<Args>(args)..., ts_start);
        }

        // ================================================================
        // 1. Preload hook management
        // ================================================================
        // using PreloadBase::real_func;
        void resolve_all(auto &&...args) { m_preload_impl.resolve_all(std::forward<decltype(args)>(args)...); }

        template<ValidCallbackTag Tag>
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
        template<ValidCallbackTag Tag, typename... Args>
            requires is_preload_hook_v<Tag>
        static auto invoke_preload(Args... args) -> HookTrait<Tag>::RetType {
            auto &mgr = instance();

            // Start timestamp as soon as the function hook is invoked
            auto ts_start = mgr.m_timestamp.now();

            mgr.template invoke_all_pre<Tag>(args..., ts_start);

            auto real = mgr.template real_func<Tag>();

            // It can die silently otherwise
            if (!real) {
                std::cerr << "ERROR: Real function for Tag " << HookTrait<Tag>::name << " is empty!" << std::endl;
                assert(false);
            }

            auto ret = real(args...);

            // End timestamp
            auto ts_end = mgr.m_timestamp.now();

            mgr.template invoke_all_post<Tag>(ret, std::forward<Args>(args)..., ts_start, ts_end);

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
        template<ValidCallbackTag Tag, typename... Args>
            requires is_object_ops_hook_v<Tag>
        static auto invoke_object_ops(Args... args) -> typename HookTrait<Tag>::RetType {
            auto &mgr = instance();

            // Start timestamp as soon as the function hook is invoked
            auto ts_start = mgr.m_timestamp.now();

            // We can't std::forward multiple times
            ObjectType *obj = static_cast<Derived &>(mgr).template get_object_from_args<Tag>(args...);
            Context *ctx = mgr.m_object_ops_impl.find(obj);

            if (!ctx) {
                // Fallback - shouldn't happen in normal operation
                // TODO: maybe the callbacks should get an std::optional or std::expected
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

            // End timestamp
            auto ts_end = mgr.m_timestamp.now();

            // Post-hooks with context
            mgr.template invoke_post_with_context<Tag>(*ctx, ret, args..., ts_start, ts_end);

            return ret;
        }

    protected:
        CompositeHookManager() = default;

        // Expose base class methods
        using Base::invoke_all_post;
        using Base::invoke_all_pre;

        // Context-aware invocation for object-ops
        template<ValidCallbackTag Tag, typename... Args>
        void invoke_pre_with_context(Context &ctx, Args... args) {
            if constexpr (requires(Derived &d, Tag t, Context &c, Args... a) { d.pre(t, c, a...); }) {
                static_cast<Derived &>(*this).pre(Tag{}, ctx, args...);
            }
            Base::template invoke_pre_callbacks<Tag>(args...);
        }

        template<ValidCallbackTag Tag, typename Ret, typename... Args>
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

        template<ValidCallbackTag Tag>
        void install_if_object_ops(ObjectType *obj, Context &ctx) {
            if constexpr (is_object_ops_hook_v<Tag>) {
                static_assert(has_get_op_for<Derived, Tag, ObjectType>,
                              "Derived must provide: `template <...> get_op<Tag>(ObjectType*)` for object-ops hooks");

                auto &op = static_cast<Derived &>(*this).template get_op<Tag>(obj);
                ctx.template original<Tag>() = op;
                op = &Derived::template invoke_object_ops<Tag>;
            }
        }

        template<size_t... Is>
        void uninstall_object_ops_hooks(ObjectType *obj, Context &ctx, std::index_sequence<Is...>) {
            (uninstall_if_object_ops<std::tuple_element_t<Is, std::tuple<Tags...>>>(obj, ctx), ...);
        }

        template<ValidCallbackTag Tag>
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
