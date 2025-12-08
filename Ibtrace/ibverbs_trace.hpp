#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <tuple>
#include <typeindex>
#include <unordered_map>
#include <vector>

#include <nvtx3/nvtx3.hpp>

#include "infiniband/verbs.h"

namespace ibv_hook {
    constexpr auto IBV_HOOK_VERSION = "IBVERBS_1.1";
    // ============================================================================
    // Statistics
    // ============================================================================
    struct Stats {
        std::atomic<std::uint64_t> calls = 0;
        std::atomic<std::uint64_t> total_ns = 0;

        void record(const std::uint64_t ns) {
            calls.fetch_add(1, std::memory_order_relaxed);
            total_ns.fetch_add(ns, std::memory_order_relaxed);
        }

        void print(const char *name, std::ostream &os = std::cout) const {
            if (calls == 0) {
                return;
            }

            // TODO: std::format
            os << "  " << name << ": " << calls << " calls, " << (total_ns / calls) << " ns avg\n";
        }
    };

    // ============================================================================
    // Hook trait - define one per IBV operation
    // ============================================================================
    template<typename Tag>
    struct HookTrait;

#define DEFINE_IBV_HOOK(name, ret_type, first_arg_type, ...)                                                           \
    struct name##_tag {};                                                                                              \
                                                                                                                       \
    template<>                                                                                                         \
    struct HookTrait<name##_tag> {                                                                                     \
        using FuncPtr = ret_type (*)(first_arg_type, ##__VA_ARGS__);                                                   \
        using RetType = ret_type;                                                                                      \
        using PreCallback = std::function<void(first_arg_type, ##__VA_ARGS__)>;                                        \
        using PostCallback = std::function<void(ret_type, first_arg_type, ##__VA_ARGS__)>;                             \
        static constexpr const char *op_name = #name;                                                                  \
                                                                                                                       \
        static FuncPtr &get_op(ibv_context *ctx) { return ctx->ops.name; }                                             \
        static ibv_context *get_ctx(first_arg_type arg) { return arg->context; }                                       \
    }

    // Supported obv functions are defined here as Traits
    // TODO: this might break if two functions have the same signature
    DEFINE_IBV_HOOK(post_send, int, ibv_qp *, ibv_send_wr *, ibv_send_wr **);
    DEFINE_IBV_HOOK(post_recv, int, ibv_qp *, ibv_recv_wr *, ibv_recv_wr **);
    DEFINE_IBV_HOOK(poll_cq, int, ibv_cq *, int, ibv_wc *);
    DEFINE_IBV_HOOK(req_notify_cq, int, ibv_cq *, int);
    DEFINE_IBV_HOOK(post_srq_recv, int, ibv_srq *, ibv_recv_wr *, ibv_recv_wr **);

    // ============================================================================
    // Per-context storage for original function pointers
    // ============================================================================
    template<typename... Tags>
    struct ContextStorage {
        ibv_context *ctx = nullptr;
        std::tuple<typename HookTrait<Tags>::FuncPtr...> originals;
        std::unordered_map<std::type_index, Stats> stats;

        explicit ContextStorage(ibv_context *c) : ctx(c) {
            // Save original function pointers
            ((std::get<typename HookTrait<Tags>::FuncPtr>(originals) = HookTrait<Tags>::get_op(c)), ...);
        }

        // TODO: MAYBE use index-based access
        template<typename Tag>
        HookTrait<Tag>::FuncPtr get_original() const {
            return std::get<typename HookTrait<Tag>::FuncPtr>(originals);
        }

        // TODO: ditto
        template<typename Tag>
        Stats &get_stats() {
            // Default-construct if not present
            return stats[std::type_index(typeid(Tag))];
        }

        void print_stats(std::ostream &os = std::cout) const {
            os << "Context " << ctx << ":\n";
            print_stats_impl(os, std::index_sequence_for<Tags...>{});
        }

    private:
        template<std::size_t... Is>
        void print_stats_impl(std::ostream &os, std::index_sequence<Is...>) const {
            (print_single_tag<Tags>(os), ...);
        }

        template<typename Tag>
        void print_single_tag(std::ostream &os) const {
            auto it = stats.find(std::type_index(typeid(Tag)));
            if (it != stats.end()) {
                it->second.print(HookTrait<Tag>::op_name, os);
            }
        }
    };

    // ================================================================
    // Callback storage for registered hooks
    // ================================================================
    template<typename... Tags>
    struct CallbackStorage {
        std::tuple<std::vector<typename HookTrait<Tags>::PreCallback>...> pre_callbacks;
        std::tuple<std::vector<typename HookTrait<Tags>::PostCallback>...> post_callbacks;

        template<typename Tag>
        std::vector<typename HookTrait<Tag>::PreCallback> &get_pre_callbacks() {
            return std::get<std::vector<typename HookTrait<Tag>::PreCallback>>(pre_callbacks);
        }

        template<typename Tag>
        std::vector<typename HookTrait<Tag>::PostCallback> &get_post_callbacks() {
            return std::get<std::vector<typename HookTrait<Tag>::PostCallback>>(post_callbacks);
        }
    };

    // ================================================================
    // Hook Manager
    // ================================================================
    template<typename Derived, typename... Tags>
    class HookManager {
        struct NvtxDomain {
            static constexpr char const *name{"ibv Hooks"};
        };
        using NvtxDomainScopedRange = nvtx3::scoped_range_in<NvtxDomain>;

    public:
        using Contexts = ContextStorage<Tags...>;
        using Callbacks = CallbackStorage<Tags...>;

        static Derived &instance() {
            static Derived mgr;
            return mgr;
        }

        Contexts *install(ibv_context *ctx) {
            std::lock_guard lock(m_mutex);

            auto [it, inserted] = m_contexts.emplace(ctx, Contexts{ctx});
            if (inserted) {
                // Install our wrappers
                ((HookTrait<Tags>::get_op(ctx) = &wrapper<Tags>), ...);
            }
            return &it->second;
        }

        void uninstall(ibv_context *ctx) {
            std::lock_guard lock(m_mutex);

            if (auto it = m_contexts.find(ctx); it != m_contexts.end()) {
                // Restore originals
                ((HookTrait<Tags>::get_op(ctx) = it->second.template get_original<Tags>()), ...);
                m_contexts.erase(it);
            }
        }

        Contexts *find(ibv_context *ctx) {
            std::lock_guard lock(m_mutex);
            if (auto it = m_contexts.find(ctx); it != m_contexts.end()) {
                return &it->second;
            }
            return nullptr;
        }

        void print_stats(std::ostream &os = std::cout) const {
            std::lock_guard lock(m_mutex);
            os << "\n=== IBV Hook Statistics ===\n";
            for (const auto &[ctx, storage]: m_contexts) {
                storage.print_stats(os);
            }
        }

        // Register a pre-hook callback for a given tag
        // Usage: instance().register_pre<post_send_tag>([](ibv_qp* qp, ibv_send_wr* wr, ibv_send_wr** bad_wr) { ... });
        template<typename Tag>
        void register_pre(HookTrait<Tag>::PreCallback callback) {
            std::lock_guard lock(m_mutex);
            m_callbacks.template get_pre_callbacks<Tag>().push_back(std::move(callback));
        }

        // Register a post-hook callback for a given tag
        // Usage: instance().register_post<post_send_tag>([](int ret, auto&& ...) { ... });
        template<typename Tag>
        void register_post(HookTrait<Tag>::PostCallback callback) {
            std::lock_guard lock(m_mutex);
            m_callbacks.template get_post_callbacks<Tag>().push_back(std::move(callback));
        }

        // Clear all registered callbacks for a specific tag
        template<typename Tag>
        void clear_callbacks() {
            std::lock_guard lock(m_mutex);
            m_callbacks.template get_pre_callbacks<Tag>().clear();
            m_callbacks.template get_post_callbacks<Tag>().clear();
        }

        // Clear all registered callbacks
        void clear_all_callbacks() {
            std::lock_guard lock(m_mutex);
            (m_callbacks.template get_pre_callbacks<Tags>().clear(), ...);
            (m_callbacks.template get_post_callbacks<Tags>().clear(), ...);
        }

        // TODO: need async versions maybe
        /* TODO: There are currently two ways of adding hooks:
         * `register_hook` like above, and `pre/post` like below
         * It is currently unclear which way is better (performance-wise)
         * so I have them both avialable here.
         */
        // Default pre-hooks - do nothing (override specific ones in derived class)
        // Usage: void pre(post_send_tag, Contexts& ctx, ibv_qp* qp, ibv_send_wr* wr, ibv_send_wr** bad_wr) { ... }
        template<typename Tag, typename... Args>
        void pre(Tag, Contexts &, Args...) {}

        // Default post-hooks - do nothing (override specific ones in derived class)
        // Usage: void post(post_send_tag, Contexts& ctx, int ret, ibv_qp* qp, ibv_send_wr* wr, ibv_send_wr** bad_wr) {
        // ... }
        template<typename Tag, typename Ret, typename... Args>
        void post(Tag, Contexts &, Ret, Args...) {}

    protected:
        HookManager() = default;

        // SFINAE helper to detect if Derived has a pre/post() overload for a given Tag
        template<typename Tag, typename... Args>
        static constexpr bool has_pre_v =
                requires(Derived &d, Tag t, Contexts &ctx, Args... args) { d.pre(t, ctx, args...); };

        template<typename Tag, typename Ret, typename... Args>
        static constexpr bool has_post_v =
                requires(Derived &d, Tag t, Contexts &ctx, Ret r, Args... args) { d.post(t, ctx, r, args...); };

        // Helpers to conditionally call pre/post if overload exists
        template<typename Tag, typename... Args>
        void call_pre(Derived &derived, Tag tag, Contexts &ctx, Args... args) {
            if constexpr (has_pre_v<Tag, Args...>) {
                derived.pre(tag, ctx, args...);
            }
        }

        template<typename Tag, typename Ret, typename... Args>
        void call_post(Derived &derived, Tag tag, Contexts &ctx, Ret ret, Args... args) {
            if constexpr (has_post_v<Tag, Ret, Args...>) {
                derived.post(tag, ctx, ret, args...);
            }
        }

        // Call all registered pre/post-callbacks for a tag
        template<typename Tag, typename... Args>
        void invoke_pre_callbacks(Args... args) {
            // Note: m_mutex should already be held or we accept the race for performance
            for (auto &cb: m_callbacks.template get_pre_callbacks<Tag>()) {
                cb(args...);
            }
        }

        template<typename Tag, typename Ret, typename... Args>
        void invoke_post_callbacks(Ret ret, Args... args) {
            for (auto &cb: m_callbacks.template get_post_callbacks<Tag>()) {
                cb(ret, args...);
            }
        }

        // Generic wrapper - calls derived class hooks
        template<typename Tag, typename... Args>
        static auto wrapper(Args... args) {
            // TODO: maybe a mutex here so uninstall doesn't nuke this func
            using Trait = HookTrait<Tag>;
            auto &mgr = instance();

            auto *ctx = Trait::get_ctx(std::get<0>(std::tie(args...)));
            auto *storage = mgr.find(ctx);

            // Encapsulates everything
            NvtxDomainScopedRange r_all(nvtx3::event_attributes{Trait::op_name, nvtx3::rgb{0, 0, 255}});

            // ================================================================
            // Pre-hooks
            {
                // TODO: Don't compute this every time. Save in the Trait
                const auto nvtx_str = std::string(Trait::op_name) + "_profiler_pre_hooks";
                NvtxDomainScopedRange r(nvtx3::event_attributes{nvtx_str, nvtx3::rgb{255, 0, 0}});

                // static_cast<Derived &>(mgr).pre(Tag{}, *storage, args...);
                mgr.template call_pre<Tag>(static_cast<Derived &>(mgr), Tag{}, *storage, args...);

                // Call registered pre-callbacks
                mgr.template invoke_pre_callbacks<Tag>(args...);
            }
            // ================================================================

            int ret;
            const auto start = std::chrono::steady_clock::now();
            // ================================================================
            // Real function execution
            {
                const auto nvtx_str = std::string(Trait::op_name) + "_real";
                NvtxDomainScopedRange r(nvtx3::event_attributes{nvtx_str, nvtx3::rgb{0, 255, 0}});

                // TODO: might break if void return
                ret = storage->template get_original<Tag>()(args...);
            }
            // ================================================================
            const auto elapsed = std::chrono::steady_clock::now() - start;

            // Record stats
            auto &stats = storage->template get_stats<Tag>();
            stats.record(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());

            // ================================================================
            // Post-hooks
            {
                const auto nvtx_str = std::string(Trait::op_name) + "_profiler_post_hooks";
                NvtxDomainScopedRange r(nvtx3::event_attributes{nvtx_str, nvtx3::rgb{255, 0, 0}});

                // Post-hook (in derived class) - only call if overload exists - pass tag as first argument for overload
                mgr.template call_post<Tag>(static_cast<Derived &>(mgr), Tag{}, *storage, ret, args...);

                // Call registered post-callbacks
                mgr.template invoke_post_callbacks<Tag>(ret, args...);
            }
            // ================================================================

            return ret;
        }

        mutable std::mutex m_mutex;
        std::unordered_map<ibv_context *, Contexts> m_contexts;
        // TODO: maybe another template here so we don't have to specify callback type twice like
        // std::get<std::vector<typename HookTrait<Tag>::PreCallback>>(pre_callbacks);
        Callbacks m_callbacks;
    };


    // ================================================================
    // Default hook set - just stats
    // ================================================================
    struct DefaultHookManager : HookManager<DefaultHookManager, post_send_tag, post_recv_tag, poll_cq_tag,
                                            req_notify_cq_tag, post_srq_recv_tag> {};
} // namespace ibv_hook
