#pragma once

#include <type_traits>

#include "Helpers/Timestamp.hpp"

#include "CallbackManager.hpp"
#include "Helpers/Utilities.hpp"
#include "HookTrait.hpp"

#include "detail/ObjectOpsHookManager.hpp"
#include "detail/PreloadHookManager.hpp"

namespace hook {
    static constexpr bool DebugPrint{false};

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
        using TimestampSourceType = TS::GettimeTimestampSource;
    };
    static_assert(HookManagerConfig<DefaultHookManagerConfig>);
    // ================================================================

    // ================================================================
    // Dealing with Tags
    template<typename Tag>
    struct TaggedBool {
        bool value{false};
    };
    // ================================================================

    // ================================================================
    // Concepts for better error messages

    // Check if Derived implements get_op
    template<typename D, typename Tag, typename Obj>
    concept has_get_op_for = requires(D& d, Obj* obj) { d.template get_op<Tag>(obj); };

    template<typename D>
    concept has_get_lib_handle = requires {
        { D::GetLibHandle() } -> std::same_as<void*>;
    };
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

        /*
         *TODO: Right now both implementations get all the tags.
         *This is OK for now, but should use detail/PartitionTag.hpp in the future
         */
        using PreloadBase = detail::PreloadHookManager<Tags...>;
        using ObjectOpsBase = detail::ObjectOpsHookManager<Tags...>;

    public:
        using Context = detail::ObjectContext<Tags...>;

        using TimestampSource = Config::TimestampSourceType;
        TimestampSource m_timestamp{};

        // Slot-based context storage ot avoid map find() in every op
        using ContextSlots = detail::ContextSlots<Context>;

        // Singleton
        static Derived& instance() noexcept {
            static Derived mgr;
            return mgr;
        }

        // ================================================================
        // Unified callback registration
        // These just forward to the base CallbackManager
        // ================================================================
        using Base::clear_all_callbacks;
        using Base::clear_callbacks;

        template<ValidCallbackTag Tag>
        void internal_register() noexcept {
            if constexpr (is_preload_hook_v<Tag>) {
                static_assert(has_get_lib_handle<Derived>, "Derived must provide: `void* GetLibHandle()`");

                static bool is_tag_registered{false};
                if (!is_tag_registered) {
                    if constexpr (DebugPrint) {
                        std::cout << "Registering " << HookTrait<Tag>::name << std::endl;
                    }
                    register_preload_one<Tag>();
                }
                is_tag_registered = true;
            }
        }

        template<ValidCallbackTag Tag>
        void register_pre(auto&&... args) noexcept {
            std::get<TaggedBool<Tag>>(m_registered_tags).value = true;
            {
                std::scoped_lock lock(m_registration_mutex);
                internal_register<Tag>();
            }
            return Base::template register_pre<Tag>(std::forward<decltype(args)>(args)...);
        }

        template<ValidCallbackTag Tag>
        void register_post(auto&&... args) noexcept {
            std::get<TaggedBool<Tag>>(m_registered_tags).value = true;
            {
                std::scoped_lock lock(m_registration_mutex);
                internal_register<Tag>();
            }
            return Base::template register_post<Tag>(std::forward<decltype(args)>(args)...);
        }

        // ================================================================
        // Helper to register pre-callbacks for all tags automatically
        // Callback signature: void(const char* symbol_name, Args...)
        // ================================================================
        template<typename Callback>
        void register_all_pre(Callback&& callback) noexcept {
            (register_pre_for_tag<Tags>(std::forward<Callback>(callback)), ...);
        }

        // TODO
        template<typename Callback>
        [[deprecated]] void register_pre_default(Callback&& callback) noexcept {
            // Not deprecated, just not implemented yet
            sigma_ignore_unused(callback);
            std::abort();
        }

    private:
        template<typename Tag, typename Callback>
        void register_pre_for_tag(Callback&& callback) noexcept {
            this->template register_pre<Tag>([callback](auto&&... args) {
                callback.template operator()<Tag>(std::forward<decltype(args)>(args)...);
            });
        }

    public:
        // ================================================================
        // 1. Custom event hooks
        // ================================================================
        template<ValidEventTag Tag, typename... Args>
            requires is_custom_hook_v<Tag>
        void invoke_custom(Args&&... args) noexcept {
            // TODO: pre only
            auto ts_start = m_timestamp.now();
            this->template invoke_all_pre<Tag>(std::forward<Args>(args)..., ts_start);
        }

        template<ValidEventTag Tag, typename... Args>
            requires is_custom_hook_v<Tag>
        void invoke_custom_ts(Timestamp ts_start, Args&&... args) noexcept {
            this->template invoke_all_pre<Tag>(std::forward<Args>(args)..., ts_start);
        }

        // ================================================================
        // 1. Preload hook management
        // ================================================================
        // using PreloadBase::real_func;
        template<typename Tag>
        void register_preload_one(auto&&... args) noexcept {
            if constexpr (is_preload_hook_v<Tag>) {
                auto default_func = InvokePreloadPtr<Tag, typename HookTrait<Tag>::FuncPtr>::get();
                m_preload_impl.template register_hook<Tag>(std::forward<decltype(args)>(args)..., default_func);
            }
        }

        void register_preload_all(auto&&... args) noexcept {
            // m_preload_impl.resolve_all(std::forward<decltype(args)>(args)...);
            (register_preload_one<Tags>(std::forward<decltype(args)>(args)...), ...);
        }

        template<ValidCallbackTag Tag>
            requires is_preload_hook_v<Tag>
        [[nodiscard]] HookTrait<Tag>::FuncPtr& real_func() {
            return m_preload_impl.template real_func<Tag>();
        }

        // TODO: this function is really odd but we need to somehow
        // resolve to the correct real_func<Tag> with a runtime parameter...
        [[nodiscard]] void** real_func_by_name(std::string_view name) noexcept {
            void** func_ptr = nullptr;

            ([&]() -> bool {
                if constexpr (is_preload_hook_v<Tags>) {
                    if (HookTrait<Tags>::symbol_name == name) {
                        func_ptr = reinterpret_cast<void**>(&real_func<Tags>());
                        return true;
                    }
                }
                return false;
            }() || ...);

            return func_ptr;
        }

        // template<typename Tag, typename... Args>
        // static constexpr bool invoke_preload_noexcept_v =
        //         noexcept(std::declval<CompositeHookManager &>().template real_func<Tag>()(std::declval<Args>()...));

        // Static wrapper for preload hooks
        // TODO: remove, possibly. We can already call these through the instance
        template<ValidCallbackTag Tag, typename... Args>
            requires is_preload_hook_v<Tag>
        static auto invoke_preload(Args&&... args) noexcept -> HookTrait<Tag>::RetType {
            auto& mgr = instance();

            // No hooks registered, don't do all the stuff below
            if (!std::get<TaggedBool<Tag>>(mgr.m_registered_tags).value) {
                auto& real = mgr.template real_func<Tag>();

                // If we're here, the injected function is called but no hooks are registered
                // Late load the real function and call it
                if (!real) {
                    auto* handle = Derived::GetLibHandle();
                    mgr.m_preload_impl.template register_real<Tag>(handle);

                    // Lazy resolution fallback for RTLD_NEXT handles
                    if (!real) {
                        std::cerr << "ERROR: Real function for Tag " << HookTrait<Tag>::name << " is empty!"
                                  << std::endl;
                        std::abort();
                    }
                }

                return real(std::forward<Args>(args)...);
            }

            // Start timestamp as soon as the function hook is invoked
            auto ts_start = mgr.m_timestamp.now();

            mgr.template invoke_all_pre<Tag>(args..., ts_start);

            // First time here will always be null
            auto& real = mgr.template real_func<Tag>();

            // Lazy resolution: the real library may not have been loaded at registration time
            // If we're at this stage, the real function has been loaded by the target application
            if (!real) {
                auto* handle = Derived::GetLibHandle();
                mgr.m_preload_impl.template register_real<Tag>(handle);
                if (!real) {
                    std::cerr << "ERROR: Real function for Tag " << HookTrait<Tag>::name << " is empty!" << std::endl;
                    std::abort();
                }
            }

            // if constexpr (std::is_void_v<typename HookTrait<Tag>::RetType>) { ... }
            auto ret = real(args...);

            // End timestamp
            auto ts_end = mgr.m_timestamp.now();

            mgr.template invoke_all_post<Tag>(ret, std::forward<Args>(args)..., ts_start, ts_end);

            return ret;
        }

        // ================================================================
        // 2. Object-ops hook management
        // ================================================================
        Context* install(ObjectType* obj) noexcept {
            assert(obj && "HookManager ctx obj install called with null obj");

            std::lock_guard lock(m_registration_mutex);

            // auto key = reinterpret_cast<uintptr_t>(obj);
            // if (m_contexts.contains(key)) {
            //     return m_contexts[key].get();
            // }
            //
            // auto ctx = std::make_unique<Context>();
            // ctx->set_object_ptr(obj);

            auto* ctx = m_object_ops_impl.get_context(obj);

            // Install object-ops hooks only
            install_object_ops_hooks(obj, ctx, std::make_index_sequence<sizeof...(Tags)>{});

            return ctx;
        }

        void uninstall(ObjectType* obj) noexcept {
            // TODO: maybe it's fine if it's null
            assert(obj && "HookManager ctx obj uninstalled called with null obj");

            std::lock_guard lock(m_registration_mutex);

            auto& contexts = m_object_ops_impl.m_contexts;

            auto key = reinterpret_cast<uintptr_t>(obj);
            auto it = contexts.find(key);
            if (it == contexts.end()) {
                return;
            }

            auto* ctx = m_object_ops_impl.get_context(obj);
            uninstall_object_ops_hooks(obj, ctx, std::make_index_sequence<sizeof...(Tags)>{});

            contexts.erase(it);
        }

        // Static wrapper for object-ops hooks
        // Note: this takes Args... instead of Args&&... because FuncPtr args need to match exactly...
        // See the note in install_if_object_ops
        // Most of these are small value types or pointers. Should be fine.
        // SlotIndex is a compile-time constant that indexes into ContextSlots::slots
        template<ValidCallbackTag Tag, size_t CtxSlotIndex, typename... Args>
            requires is_object_ops_hook_v<Tag>
        static auto invoke_object_ops(Args... args) noexcept -> typename HookTrait<Tag>::RetType {
            auto& mgr = instance();

            // Start timestamp as soon as the function hook is invoked
            // TODO: don't get timestamp if no hooks registered
            auto ts_start = mgr.m_timestamp.now();

            // Get context from pre-allocated slot (no runtime lookup!)
            Context* ctx = ContextSlots::slots[CtxSlotIndex];

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
            mgr.template invoke_pre_with_context<Tag>(ctx, args..., ts_start);

            const auto& original = ctx->template original<Tag>();
            decltype(auto) ret = original(args...);

            // End timestamp
            auto ts_end = mgr.m_timestamp.now();

            // Post-hooks with context
            /* TODO: don't call if no hooks
             * For some reason just calling this function is expensive
             * even if no hooks are registered. Maybe because of arg copies?
             * Need more optimizations.
             * Pack args into a reference-able tuple and don't call if no hooks
             */
            mgr.template invoke_post_with_context<Tag>(ctx, ret, args..., ts_start, ts_end);

            return ret;
        }

    protected:
        CompositeHookManager() = default;

        // Expose base class methods
        using Base::invoke_all_post;
        using Base::invoke_all_pre;

        // Context-aware invocation for object-ops
        template<ValidCallbackTag Tag, typename... Args>
        void invoke_pre_with_context(Context* ctx, Args&&... args) noexcept {
            if constexpr (requires(Derived& d, Tag t, Context* c, Args&&... a) {
                              d.pre(t, c, std::forward<Args>(a)...);
                          }) {
                static_cast<Derived&>(*this).pre(Tag{}, ctx, args...);
            }
            Base::template invoke_pre_callbacks<Tag>(std::forward<Args>(args)...);
        }

        template<ValidCallbackTag Tag, typename Ret, typename... Args>
        void invoke_post_with_context(Context* ctx, Ret ret, Args&&... args) noexcept {
            if constexpr (requires(Derived& d, Tag t, Context* c, Ret r, Args&&... a) {
                              d.post(t, c, r, std::forward<Args>(a)...);
                          }) {
                static_cast<Derived&>(*this).post(Tag{}, ctx, ret, args...);
            }
            Base::template invoke_post_callbacks<Tag>(ret, std::forward<Args>(args)...);
        }

    private:
        // TODO: this is ugly and should be in ObjectOpsHookManager
        // but since we really need Derived, it's here for now
        template<size_t... Is>
        void install_object_ops_hooks(ObjectType* obj, Context* ctx, std::index_sequence<Is...>) noexcept {
            (install_if_object_ops<std::tuple_element_t<Is, std::tuple<Tags...>>>(obj, ctx), ...);
        }

        // Alias for the function pointer table
        template<typename Tag, typename FuncPtr>
        using InvokeObjectOpsTable = detail::InvokeObjectOpsTable<detail::MAX_OBJECT_OPS_SLOTS, Derived, Tag, FuncPtr>;

        template<ValidCallbackTag Tag>
        void install_if_object_ops(ObjectType* obj, Context* ctx) noexcept {
            if constexpr (is_object_ops_hook_v<Tag>) {
                static_assert(has_get_op_for<Derived, Tag, ObjectType>,
                              "Derived must provide: `template <...> get_op<Tag>(ObjectType*)` for object-ops hooks");

                if (!std::get<TaggedBool<Tag>>(m_registered_tags).value) {
                    if constexpr (DebugPrint) {
                        std::cout << "Skipping " << HookTrait<Tag>::name << std::endl;
                    }
                    return;
                }

                auto& op = static_cast<Derived&>(*this).template get_op<Tag>(obj);
                ctx->template original<Tag>() = op;

                // Allocate a slot and store context there
                size_t slot = ContextSlots::allocate(ctx);

                // Get the pre-instantiated function pointer for this slot
                using FuncPtr = typename HookTrait<Tag>::FuncPtr;
                op = InvokeObjectOpsTable<Tag, FuncPtr>::table[slot];
            }
        }

        template<size_t... Is>
        void uninstall_object_ops_hooks(ObjectType* obj, Context* ctx, std::index_sequence<Is...>) noexcept {
            (uninstall_if_object_ops<std::tuple_element_t<Is, std::tuple<Tags...>>>(obj, ctx), ...);
        }

        template<ValidCallbackTag Tag>
        void uninstall_if_object_ops(ObjectType* obj, Context* ctx) noexcept {
            if constexpr (is_object_ops_hook_v<Tag>) {
                auto& op = static_cast<Derived&>(*this).template get_op<Tag>(obj);
                op = ctx->template original<Tag>();
            }
        }

        // Helper to create a function pointer matching HookTrait<Tag>::FuncPtr that calls invoke_preload
        // We're basically specializing the template away so we can use it like a function pointer
        // TODO: should be more generic and moved to a helper module
        template<typename Tag, typename FuncPtr>
        struct InvokePreloadPtr;

        template<typename Tag, typename Ret, typename... Args>
        struct InvokePreloadPtr<Tag, Ret (*)(Args...) noexcept> {
            static Ret call(Args... args) noexcept {
                return invoke_preload<Tag>(std::forward<Args>(args)...);
            }
            static constexpr auto get() noexcept {
                return &call;
            }
        };

        template<typename Tag, typename Ret, typename... Args>
        struct InvokePreloadPtr<Tag, Ret (*)(Args...)> {
            static Ret call(Args... args) {
                return invoke_preload<Tag>(std::forward<Args>(args)...);
            }
            static constexpr auto get() {
                return &call;
            }
        };

        mutable std::mutex m_registration_mutex;
        std::tuple<TaggedBool<Tags>...> m_registered_tags;

        // Implementations
        PreloadBase m_preload_impl;
        ObjectOpsBase m_object_ops_impl;
    };

} // namespace hook
