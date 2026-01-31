#pragma once

#include "IbverbsHooks.hpp"

#include "CompositeHookManager.hpp"

namespace ibv {

    // ================================================================
    // IbverbsEventSource - Unified hook manager for ibverbs
    //
    // Combines object-ops hooks (post_send, poll_cq, etc.) with
    // preload hooks (reg_mr, open_device, etc.) into a single interface.
    //
    // Usage:
    //   struct MyProfiler : IbverbsEventSource<MyProfiler> {
    //       MyProfiler() {
    //           // Register callbacks
    //           register_post<post_send_tag>([](int ret, ibv_qp*, ...) { ... });
    //           register_pre<reg_mr_tag>([](ibv_pd*, void*, ...) { ... });
    //       }
    //
    //       // Optional: override pre/post for object-ops (with context)
    //       void pre(post_send_tag, Context& ctx, ibv_qp*, ...) { ... }
    //
    //       // Optional: override pre/post for preload (no context)
    //       void pre(reg_mr_tag, ibv_pd*, ...) { ... }
    //   };
    // ================================================================
    template<typename Derived>
    class IbverbsEventSource
        : public hook::CompositeHookManager<
                  Derived, hook::DefaultHookManagerConfig, ibv_context, post_send_tag, post_recv_tag, poll_cq_tag,
                  req_notify_cq_tag, post_srq_recv_tag, open_device_tag, close_device_tag, query_device_tag, reg_mr_tag,
                  dereg_mr_tag, reg_mr_iova2_tag, reg_dmabuf_mr_tag, alloc_pd_tag, create_cq_tag, create_qp_tag,
                  destroy_qp_tag, modify_qp_tag, create_ah_tag> {

        // TODO: too verbose
        using Base =
                hook::CompositeHookManager<Derived, hook::DefaultHookManagerConfig, ibv_context, post_send_tag,
                                           post_recv_tag, poll_cq_tag, req_notify_cq_tag, post_srq_recv_tag,
                                           open_device_tag, close_device_tag, query_device_tag, reg_mr_tag,
                                           dereg_mr_tag, reg_mr_iova2_tag, reg_dmabuf_mr_tag, alloc_pd_tag,
                                           create_cq_tag, create_qp_tag, destroy_qp_tag, modify_qp_tag, create_ah_tag>;

    public:
        using Context = Base::Context;

        // ================================================================
        // Object-ops hook support: get_op and get_object_from_args
        // These are required by CompositeHookManager for object-ops hooks
        // ================================================================

        // TODO: the two functions below might need rework
        template<hook::ValidCallbackTag Tag>
        auto& get_op(ibv_context* ctx) const noexcept {
            if constexpr (std::is_same_v<Tag, post_send_tag>) {
                return ctx->ops.post_send;
            } else if constexpr (std::is_same_v<Tag, post_recv_tag>) {
                return ctx->ops.post_recv;
            } else if constexpr (std::is_same_v<Tag, poll_cq_tag>) {
                return ctx->ops.poll_cq;
            } else if constexpr (std::is_same_v<Tag, req_notify_cq_tag>) {
                return ctx->ops.req_notify_cq;
            } else if constexpr (std::is_same_v<Tag, post_srq_recv_tag>) {
                return ctx->ops.post_srq_recv;
            }
        }

        template<hook::ValidCallbackTag Tag, typename... Args>
        ibv_context* get_object_from_args(Args&&... args) noexcept {
            // Extract ibv_context from the first argument
            // post_send: (ibv_qp*, ...) -> qp->context
            // post_recv: (ibv_qp*, ...) -> qp->context
            // poll_cq: (ibv_cq*, ...) -> cq->context
            // req_notify_cq: (ibv_cq*, ...) -> cq->context
            // post_srq_recv: (ibv_srq*, ...) -> srq->context

            auto first_arg = std::get<0>(std::make_tuple(args...));

            if constexpr (std::is_same_v<Tag, post_send_tag> || std::is_same_v<Tag, post_recv_tag>) {
                // First arg is ibv_qp*
                return reinterpret_cast<ibv_qp*>(first_arg)->context;
            } else if constexpr (std::is_same_v<Tag, poll_cq_tag> || std::is_same_v<Tag, req_notify_cq_tag>) {
                // First arg is ibv_cq*
                return reinterpret_cast<ibv_cq*>(first_arg)->context;
            } else if constexpr (std::is_same_v<Tag, post_srq_recv_tag>) {
                // First arg is ibv_srq*
                return reinterpret_cast<ibv_srq*>(first_arg)->context;
            } else {
                return nullptr;
            }
        }

    protected:
        IbverbsEventSource() = default;
    };
    // ================================================================
    struct Profiler : IbverbsEventSource<Profiler> {
        Profiler();
    };

    // Prevent other translation units from instantiating their own singleton.
    // The explicit instantiation is in IbverbsPreload.cpp.
    extern template class IbverbsEventSource<Profiler>;

    // ================================================================
    // LiteEventSource - Pre-built lightweight version
    // No custom hooks, just pure callback dispatch
    // ================================================================
    // struct LiteEventSource : IbverbsEventSource<LiteEventSource> {};

} // namespace ibv
