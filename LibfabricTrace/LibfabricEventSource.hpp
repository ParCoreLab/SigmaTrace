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
    // for ALL data-path operations through ep->msg/rma/tagged ops tables.
    //
    // ObjectType is fid_ep for the endpoint data-path hooks.
    // ================================================================
    template<typename Derived>
    class LibfabricEventSource
        : public hook::CompositeHookManager<Derived, hook::DefaultHookManagerConfig, fid_ep,
                                            // Object-ops hooks
                                            fi::ep_msg_recv_tag, fi::ep_msg_recvv_tag, fi::ep_msg_recvmsg_tag,
                                            fi::ep_msg_send_tag, fi::ep_msg_sendv_tag, fi::ep_msg_sendmsg_tag,
                                            fi::ep_msg_inject_tag, fi::ep_msg_senddata_tag, fi::ep_msg_injectdata_tag,
                                            // Object-ops hooks (RMA)
                                            fi::ep_rma_read_tag, fi::ep_rma_readv_tag, fi::ep_rma_readmsg_tag,
                                            fi::ep_rma_write_tag, fi::ep_rma_writev_tag, fi::ep_rma_writemsg_tag,
                                            fi::ep_rma_inject_tag, fi::ep_rma_writedata_tag, fi::ep_rma_injectdata_tag,
                                            // Object-ops hooks (tagged)
                                            fi::ep_tagged_recv_tag, fi::ep_tagged_recvv_tag, fi::ep_tagged_recvmsg_tag,
                                            fi::ep_tagged_send_tag, fi::ep_tagged_sendv_tag, fi::ep_tagged_sendmsg_tag,
                                            fi::ep_tagged_inject_tag, fi::ep_tagged_senddata_tag,
                                            fi::ep_tagged_injectdata_tag,
                                            // Preload hooks
                                            fi::fi_fabric_tag, fi::fi_getinfo_tag> {

        using Base = hook::CompositeHookManager<
                Derived, hook::DefaultHookManagerConfig, fid_ep, fi::ep_msg_recv_tag, fi::ep_msg_recvv_tag,
                fi::ep_msg_recvmsg_tag, fi::ep_msg_send_tag, fi::ep_msg_sendv_tag, fi::ep_msg_sendmsg_tag,
                fi::ep_msg_inject_tag, fi::ep_msg_senddata_tag, fi::ep_msg_injectdata_tag, fi::ep_rma_read_tag,
                fi::ep_rma_readv_tag, fi::ep_rma_readmsg_tag, fi::ep_rma_write_tag, fi::ep_rma_writev_tag,
                fi::ep_rma_writemsg_tag, fi::ep_rma_inject_tag, fi::ep_rma_writedata_tag, fi::ep_rma_injectdata_tag,
                fi::ep_tagged_recv_tag, fi::ep_tagged_recvv_tag, fi::ep_tagged_recvmsg_tag, fi::ep_tagged_send_tag,
                fi::ep_tagged_sendv_tag, fi::ep_tagged_sendmsg_tag, fi::ep_tagged_inject_tag,
                fi::ep_tagged_senddata_tag, fi::ep_tagged_injectdata_tag, fi::fi_fabric_tag, fi::fi_getinfo_tag>;

    public:
        using Context = Base::Context;

        static void* GetLibHandle() noexcept {
            return nullptr;
        }

        // ================================================================
        // Object-ops hook support: get_op and get_object_from_args
        // Maps tags to the correct function pointer in the ops tables
        // ================================================================
        template<hook::ValidCallbackTag Tag>
        auto& get_op(fid_ep* ep) const noexcept {
            // msg ops
            if constexpr (std::is_same_v<Tag, fi::ep_msg_recv_tag>) {
                return ep->msg->recv;
            } else if constexpr (std::is_same_v<Tag, fi::ep_msg_recvv_tag>) {
                return ep->msg->recvv;
            } else if constexpr (std::is_same_v<Tag, fi::ep_msg_recvmsg_tag>) {
                return ep->msg->recvmsg;
            } else if constexpr (std::is_same_v<Tag, fi::ep_msg_send_tag>) {
                return ep->msg->send;
            } else if constexpr (std::is_same_v<Tag, fi::ep_msg_sendv_tag>) {
                return ep->msg->sendv;
            } else if constexpr (std::is_same_v<Tag, fi::ep_msg_sendmsg_tag>) {
                return ep->msg->sendmsg;
            } else if constexpr (std::is_same_v<Tag, fi::ep_msg_inject_tag>) {
                return ep->msg->inject;
            } else if constexpr (std::is_same_v<Tag, fi::ep_msg_senddata_tag>) {
                return ep->msg->senddata;
            } else if constexpr (std::is_same_v<Tag, fi::ep_msg_injectdata_tag>) {
                return ep->msg->injectdata;
            }
            // rma ops
            else if constexpr (std::is_same_v<Tag, fi::ep_rma_read_tag>) {
                return ep->rma->read;
            } else if constexpr (std::is_same_v<Tag, fi::ep_rma_readv_tag>) {
                return ep->rma->readv;
            } else if constexpr (std::is_same_v<Tag, fi::ep_rma_readmsg_tag>) {
                return ep->rma->readmsg;
            } else if constexpr (std::is_same_v<Tag, fi::ep_rma_write_tag>) {
                return ep->rma->write;
            } else if constexpr (std::is_same_v<Tag, fi::ep_rma_writev_tag>) {
                return ep->rma->writev;
            } else if constexpr (std::is_same_v<Tag, fi::ep_rma_writemsg_tag>) {
                return ep->rma->writemsg;
            } else if constexpr (std::is_same_v<Tag, fi::ep_rma_inject_tag>) {
                return ep->rma->inject;
            } else if constexpr (std::is_same_v<Tag, fi::ep_rma_writedata_tag>) {
                return ep->rma->writedata;
            } else if constexpr (std::is_same_v<Tag, fi::ep_rma_injectdata_tag>) {
                return ep->rma->injectdata;
            }
            // tagged ops
            else if constexpr (std::is_same_v<Tag, fi::ep_tagged_recv_tag>) {
                return ep->tagged->recv;
            } else if constexpr (std::is_same_v<Tag, fi::ep_tagged_recvv_tag>) {
                return ep->tagged->recvv;
            } else if constexpr (std::is_same_v<Tag, fi::ep_tagged_recvmsg_tag>) {
                return ep->tagged->recvmsg;
            } else if constexpr (std::is_same_v<Tag, fi::ep_tagged_send_tag>) {
                return ep->tagged->send;
            } else if constexpr (std::is_same_v<Tag, fi::ep_tagged_sendv_tag>) {
                return ep->tagged->sendv;
            } else if constexpr (std::is_same_v<Tag, fi::ep_tagged_sendmsg_tag>) {
                return ep->tagged->sendmsg;
            } else if constexpr (std::is_same_v<Tag, fi::ep_tagged_inject_tag>) {
                return ep->tagged->inject;
            } else if constexpr (std::is_same_v<Tag, fi::ep_tagged_senddata_tag>) {
                return ep->tagged->senddata;
            } else if constexpr (std::is_same_v<Tag, fi::ep_tagged_injectdata_tag>) {
                return ep->tagged->injectdata;
            }
        }

        template<hook::ValidCallbackTag Tag, typename... Args>
        fid_ep* get_object_from_args(Args&&... args) noexcept {
            // For all EP ops, the first argument is fid_ep*
            return std::get<0>(std::make_tuple(args...));
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
