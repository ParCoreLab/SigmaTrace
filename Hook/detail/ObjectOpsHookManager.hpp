#pragma once

#include <cassert>

#include <iostream>
#include <memory>
#include <unordered_map>

#include "HookTrait.hpp"

namespace hook::detail {
    // ================================================================
    // Per-object context storage for object-ops hooks
    // Stores original function pointers and per-object state
    // ================================================================
    template<typename... Tags>
    class ObjectContext {
    public:
        // Original function pointers
        // TODO: remove
        template<ValidCallbackTag Tag>
        HookTrait<Tag>::FuncPtr& original() noexcept {
            return std::get<typename HookTrait<Tag>::FuncPtr>(m_originals);
        }

        template<ValidCallbackTag Tag>
        HookTrait<Tag>::FuncPtr original() const noexcept {
            return std::get<typename HookTrait<Tag>::FuncPtr>(m_originals);
        }

        // The raw object pointer (for reverse lookup)
        // TODO: remove
        [[nodiscard]] void* object_ptr() const noexcept {
            return m_object_ptr;
        }

        void set_object_ptr(void* ptr) noexcept {
            m_object_ptr = ptr;
        }

    private:
        std::tuple<typename HookTrait<Tags>::FuncPtr...> m_originals{};
        // std::tuple<Stats<Tags>...> m_stats{};
        void* m_object_ptr{nullptr};
    };

    // ================================================================
    // ObjectOpsHookManager - Base for object->ops style hooks
    //
    // This is a partial implementation - derived classes must provide:
    //   - get_op<Tag>(object): returns reference to function pointer in object
    //   - get_object_from_args<Tag>(args...): extracts object from call args
    //
    // Usage:
    //   struct MyHooks : ObjectOpsHookManager<MyHooks, MyContext, my_tag, other_tag> {
    //       template<typename Tag>
    //       auto& get_op(my_object* obj) { return obj->ops.XXX; }
    //
    //       template<typename Tag, typename... Args>
    //       my_object* get_object_from_args(Args... args) { ... }
    //   };
    // ================================================================
    // TODO: This depends on ObjectType
    template<typename... Tags>
    class ObjectOpsHookManager {
    public:
        using Context = ObjectContext<Tags...>;

        // ================================================================
        // Context management
        // ================================================================
        // Install hooks on an object
        // TODO
        // template<typename ObjectType>
        // Context *install(ObjectType *obj) {
        //
        // }

        template<typename ObjectType>
        Context* get_context(ObjectType* obj) {
            assert(obj);

            auto key = reinterpret_cast<uintptr_t>(obj);
            if (m_contexts.contains(key)) {
                return m_contexts[key].get();
            }

            // TODO: shared_ptr?
            auto ctx = std::make_unique<Context>();
            ctx->set_object_ptr(obj);

            auto* ctx_ptr = ctx.get();
            m_contexts[key] = std::move(ctx);
            return ctx_ptr;
        }

        // TODO
        // // Uninstall hooks from an object
        // template <typename ObjectType>
        // void uninstall(ObjectType *obj) {
        // }

        // Find context for an object
        template<typename ObjectType>
        Context* find(ObjectType* obj) noexcept {
            if (!obj) {
                return nullptr;
            }

            auto key = reinterpret_cast<uintptr_t>(obj);
            auto it = m_contexts.find(key);
            return it != m_contexts.end() ? it->second.get() : nullptr;
        }

    protected:
        // TODO
        // ObjectOpsHookManager() = default;

        // ================================================================
        // wrapper template
        // Called by the installed wrapper functions
        // ================================================================
        template<ValidCallbackTag Tag, typename ObjectType, typename... Args>
            requires is_object_ops_hook_v<Tag>
        [[maybe_unused]] auto wrapper(ObjectType* obj, Args...) noexcept -> HookTrait<Tag>::RetType {
            // Get object from arguments (derived class must implement this)
            Context* ctx = find(obj);

            if (!ctx) {
                std::cerr << "[hook] Warning: wrapper called for untracked object\n";
                // Can't call original - we don't have it
                if constexpr (std::is_void_v<typename HookTrait<Tag>::RetType>) {
                    return;
                } else {
                    return typename HookTrait<Tag>::RetType{};
                }
            }

            return nullptr;

            // TODO
            // // Pre-hooks (with context)
            // mgr.template invoke_all_pre_with_context<Tag>(*ctx, args...);
            //
            // // Call original function
            // auto original = ctx->template original<Tag>();
            // auto ret = original(args...);
            //
            // // Post-hooks (with context)
            // mgr.template invoke_all_post_with_context<Tag>(*ctx, ret, args...);

            // return ret;
        }

        // ================================================================
        // Hook invocation with context
        // ================================================================
        // TODO
        // template<ValidCallbackTag Tag, typename... Args>
        // void invoke_all_pre_with_context(Context &ctx, Args... args) {
        //     // Call derived class pre hook if it exists
        //     if constexpr (requires(Derived &d, Tag t, Context &c, Args... a) { d.pre(t, c, a...); }) {
        //         static_cast<Derived &>(*this).pre(Tag{}, ctx, args...);
        //     }
        //
        //     // Call registered callbacks (without context - they're library-agnostic)
        //     Base::template invoke_pre_callbacks<Tag>(args...);
        // }

        // TODO
        // template<ValidCallbackTag Tag, typename Ret, typename... Args>
        // void invoke_all_post_with_context(Context &ctx, Ret ret, Args... args) {
        //     // Call derived class post hook if it exists
        //     if constexpr (requires(Derived &d, Tag t, Context &c, Ret r, Args... a) { d.post(t, c, r, a...); }) {
        //         static_cast<Derived &>(*this).post(Tag{}, ctx, ret, args...);
        //     }
        //
        //     // Call registered callbacks (without context)
        //     Base::template invoke_post_callbacks<Tag>(ret, args...);
        // }

    public:
        // TODO: hack! This is public because it wasn't
        // possible/easy to implement some of the functionality here
        std::unordered_map<uintptr_t, std::unique_ptr<Context>> m_contexts;
    };

} // namespace hook::detail
