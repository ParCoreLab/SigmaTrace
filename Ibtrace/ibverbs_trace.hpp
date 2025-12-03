#pragma once

#include <chrono>
#include <iostream>
#include <mutex>
#include <tuple>
#include <typeindex>
#include <unordered_map>

#include "infiniband/verbs.h"

namespace ibv_hook {
    constexpr auto IBV_HOOK_VERSION = "IBVERBS_1.1";
    // ============================================================================
    // Statistics
    // ============================================================================

    struct Stats {
        std::uint64_t calls = 0;
        std::uint64_t total_ns = 0;

        void record(const std::uint64_t ns) {
            ++calls;
            total_ns += ns;
        }

        void print(const char *name, std::ostream &os = std::cout) const {
            if (calls == 0)
                return;

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
        static constexpr const char *op_name = #name;                                                                  \
                                                                                                                       \
        static FuncPtr &get_op(ibv_context *ctx) { return ctx->ops.name; }                                             \
        static ibv_context *get_ctx(first_arg_type arg) { return arg->context; }                                       \
    }

    // Supported obv functions are defined here as Traits
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

        template<typename Tag>
        typename HookTrait<Tag>::FuncPtr get_original() const {
            return std::get<typename HookTrait<Tag>::FuncPtr>(originals);
        }

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

    // ============================================================================
    // Hook Manager
    // ============================================================================

    template<typename Derived, typename... Tags>
    class HookManager {
    public:
        using Storage = ContextStorage<Tags...>;

        static Derived &instance() {
            static Derived mgr;
            return mgr;
        }

        Storage *install(ibv_context *ctx) {
            std::lock_guard lock(m_mutex);

            auto [it, inserted] = m_contexts.emplace(ctx, Storage{ctx});
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

        Storage *find(ibv_context *ctx) {
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

    protected:
        HookManager() = default;

        // Generic wrapper - calls derived class hooks
        template<typename Tag, typename... Args>
        static auto wrapper(Args... args) {
            using Trait = HookTrait<Tag>;
            auto &mgr = instance();

            auto *ctx = Trait::get_ctx(std::get<0>(std::tie(args...)));
            auto *storage = mgr.find(ctx);

            // Pre-hook (in derived class)
            mgr.template pre<Tag>(args...);

            // Time and call original
            const auto start = std::chrono::steady_clock::now();
            auto ret = storage->template get_original<Tag>()(args...);
            const auto elapsed = std::chrono::steady_clock::now() - start;

            // Record stats
            storage->template get_stats<Tag>().record(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());

            // Post-hook (in derived class)
            mgr.template post<Tag>(ret, args...);

            return ret;
        }

        // Default hooks - do nothing (override in derived)
        template<typename Tag, typename... Args>
        void pre(Args...) {}

        template<typename Tag, typename Ret, typename... Args>
        void post(Ret, Args...) {}

        mutable std::mutex m_mutex;
        std::unordered_map<ibv_context *, Storage> m_contexts;
    };


    // ============================================================================
    // Default hook set
    // ============================================================================
    // Simple manager with no custom hooks - just stats
    struct DefaultHookManager : HookManager<DefaultHookManager, post_send_tag, post_recv_tag, poll_cq_tag,
                                            req_notify_cq_tag, post_srq_recv_tag> {};
} // namespace ibv_hook
