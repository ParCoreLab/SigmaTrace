#pragma once

#include <string_view>

#include "CompositeHookManager.hpp"

#include "LibfabricHooks.hpp"

namespace libfabric {
    constexpr std::string_view LibPath{"libfabric.so"};

    // ================================================================
    // LibfabricEventSource - Unified hook manager for libfabric
    //
    // Combines preload hooks (fi_fabric, fi_getinfo) with object-ops hooks
    // for data-path operations that go through ep->msg/rma ops tables.
    //
    // ObjectType is fid_ep for the endpoint data-path hooks.
    // ================================================================
    template<typename Derived>
    class LibfabricEventSource : public hook::CompositeHookManager<Derived, hook::DefaultHookManagerConfig, fid_ep,
                                                                   // Object-ops hooks (data-path)
                                                                   fi::ep_msg_sendmsg_tag, fi::ep_msg_recvmsg_tag,
                                                                   fi::ep_rma_readmsg_tag, fi::ep_rma_writemsg_tag,
                                                                   // Preload hooks
                                                                   fi::fi_fabric_tag, fi::fi_getinfo_tag> {

        using Base = hook::CompositeHookManager<Derived, hook::DefaultHookManagerConfig, fid_ep, fi::ep_msg_sendmsg_tag,
                                                fi::ep_msg_recvmsg_tag, fi::ep_rma_readmsg_tag, fi::ep_rma_writemsg_tag,
                                                fi::fi_fabric_tag, fi::fi_getinfo_tag>;

    public:
        using Context = Base::Context;

        static void* GetLibHandle() noexcept {
            // return smart_dlsym::DL_RTLD_NEXT;
            return nullptr;
        }

        // ================================================================
        // Object-ops hook support: get_op and get_object_from_args
        // Maps tags to the correct function pointer in the ops tables
        // ================================================================
        template<hook::ValidCallbackTag Tag>
        auto& get_op(fid_ep* ep) const noexcept {
            if constexpr (std::is_same_v<Tag, fi::ep_msg_sendmsg_tag>) {
                return ep->msg->sendmsg;
            } else if constexpr (std::is_same_v<Tag, fi::ep_msg_recvmsg_tag>) {
                return ep->msg->recvmsg;
            } else if constexpr (std::is_same_v<Tag, fi::ep_rma_readmsg_tag>) {
                return ep->rma->readmsg;
            } else if constexpr (std::is_same_v<Tag, fi::ep_rma_writemsg_tag>) {
                return ep->rma->writemsg;
            }
        }

        template<hook::ValidCallbackTag Tag, typename... Args>
        fid_ep* get_object_from_args(Args&&... args) noexcept {
            // For EP ops, the first argument is fid_ep*
            if constexpr (std::is_same_v<Tag, fi::ep_msg_sendmsg_tag> || std::is_same_v<Tag, fi::ep_msg_recvmsg_tag> ||
                          std::is_same_v<Tag, fi::ep_rma_readmsg_tag> || std::is_same_v<Tag, fi::ep_rma_writemsg_tag>) {
                return std::get<0>(std::make_tuple(args...));
            }
            return nullptr;
        }

    protected:
        LibfabricEventSource() = default;
    };

    // ================================================================
    struct Profiler : LibfabricEventSource<Profiler> {
        Profiler();
        ~Profiler() = default;
    };

    extern template class LibfabricEventSource<Profiler>;

} // namespace libfabric
