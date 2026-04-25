#include "../ProfilerProviders.hpp"
#include "../ProfilerShim.hpp"
#include "Common.hpp"

#include "../Events/LibfabricEvents.hpp"
#include "LibfabricEventSource.hpp"

// Defined in LibfabricPreload.cpp - resolve fi_addr_t to raw address hash.
// These access the provider's statics which are alive during data-path operations.
extern "C" uint32_t sigma_fi_get_own_addr_hash(fid_ep* ep);
extern "C" uint32_t sigma_fi_resolve_dest(fid_ep* ep, fi_addr_t dest_addr);

// Cache own_hash and dest_hash into ProfilerCtx at data-path time.
// Called from handler callbacks where provider statics are guaranteed alive.
// Uses thread_local fast-path to avoid mutex on repeat calls.
static void cache_fi_addr(uint64_t ep_addr, uint64_t dest_addr) {
    // Thread-local fast path: skip if this (ep, dest) pair was already cached
    thread_local std::unordered_map<uint64_t, std::unordered_set<uint64_t>> seen;
    auto& ep_set = seen[ep_addr];
    if (ep_set.contains(dest_addr)) [[likely]] {
        return;
    }
    ep_set.insert(dest_addr);

    auto& ctx = instance();
    std::scoped_lock lock(ctx.m_fi_addr_mutex);

    if (!ctx.m_fi_ep_own_hash.contains(ep_addr)) {
        uint32_t h = sigma_fi_get_own_addr_hash(reinterpret_cast<fid_ep*>(ep_addr));
        if (h != 0) {
            ctx.m_fi_ep_own_hash[ep_addr] = h;
        }
    }

    if (dest_addr != FI_DEST_UNKNOWN) {
        auto& ep_dests = ctx.m_fi_dest_hash[ep_addr];
        if (!ep_dests.contains(dest_addr)) {
            uint32_t h = sigma_fi_resolve_dest(reinterpret_cast<fid_ep*>(ep_addr), static_cast<fi_addr_t>(dest_addr));
            if (h != 0) {
                ep_dests[dest_addr] = h;
            }
        }
    }
}

static void fi_track_send(uint64_t ep_addr, uint64_t dest_addr, uint64_t len) {
    cache_fi_addr(ep_addr, dest_addr);
    if (len > 0) {
        auto& stats = instance().m_fi_traffic_registry.get_thread_local()[FiTrafficKey{ep_addr, dest_addr}];
        stats.bytes_sent += len;
        stats.send_count++;
    }
}

static void fi_track_recv(uint64_t ep_addr, uint64_t len) {
    cache_fi_addr(ep_addr, FI_DEST_UNKNOWN);
    if (len > 0) {
        auto& stats = instance().m_fi_traffic_registry.get_thread_local()[FiTrafficKey{ep_addr, FI_DEST_UNKNOWN}];
        stats.bytes_recv += len;
        stats.recv_count++;
    }
}

// ================================================================================================
void ProfilerCtx::load_libfabric(const std::filesystem::path& shim_dir) {
    m_libfabric_profiler = load_provider<libfabric::Profiler>(shim_dir / Profiler::SIGMA_LIBFABRIC_PROVIDER_PATH);

    // ================================================================
    // ep->msg->send (fi_send)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_msg_send_tag>([](ssize_t, fid_ep* ep, const void*, size_t len, void*,
                                                                fi_addr_t dest_addr, void* context, const auto ts_start,
                                                                const auto ts_end) {
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiSend event{
                .ep_addr = ep_addr,
                .context = reinterpret_cast<uint64_t>(context),
                .total_iov_len = len,
                .flags = 0,
                .dest_addr = dest_addr,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_send(ep_addr, dest_addr, len);
    });

    // ================================================================
    // ep->msg->sendv (fi_sendv)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_msg_sendv_tag>([](ssize_t, fid_ep* ep, const struct iovec* iov, void**,
                                                                 size_t count, fi_addr_t dest_addr, void* context,
                                                                 const auto ts_start, const auto ts_end) {
        uint64_t total_len = 0;
        if (iov) {
            for (size_t i = 0; i < count; i++) {
                total_len += iov[i].iov_len;
            }
        }
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiSend event{
                .ep_addr = ep_addr,
                .context = reinterpret_cast<uint64_t>(context),
                .total_iov_len = total_len,
                .flags = 0,
                .dest_addr = dest_addr,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_send(ep_addr, dest_addr, total_len);
    });

    // ================================================================
    // ep->msg->sendmsg (fi_sendmsg)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_msg_sendmsg_tag>(
            [](ssize_t, fid_ep* ep, const fi_msg* msg, uint64_t flags, const auto ts_start, const auto ts_end) {
        uint64_t total_len = 0;
        if (msg && msg->msg_iov) {
            for (size_t i = 0; i < msg->iov_count; i++) {
                total_len += msg->msg_iov[i].iov_len;
            }
        }
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const fi_addr_t dest = msg ? msg->addr : 0;
        const Profiler::Events::FiSend event{
                .ep_addr = ep_addr,
                .context = msg ? reinterpret_cast<uint64_t>(msg->context) : 0,
                .total_iov_len = total_len,
                .flags = flags,
                .dest_addr = dest,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_send(ep_addr, dest, total_len);
    });

    // ================================================================
    // ep->msg->inject (fi_inject)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_msg_inject_tag>([](ssize_t, fid_ep* ep, const void*, size_t len,
                                                                  fi_addr_t dest_addr, const auto ts_start,
                                                                  const auto ts_end) {
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiInject event{
                .ep_addr = ep_addr,
                .len = len,
                .dest_addr = static_cast<uint64_t>(dest_addr),
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_send(ep_addr, dest_addr, len);
    });

    // ================================================================
    // ep->msg->senddata (fi_senddata)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_msg_senddata_tag>([](ssize_t, fid_ep* ep, const void*, size_t len, void*,
                                                                    uint64_t, fi_addr_t dest_addr, void* context,
                                                                    const auto ts_start, const auto ts_end) {
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiSend event{
                .ep_addr = ep_addr,
                .context = reinterpret_cast<uint64_t>(context),
                .total_iov_len = len,
                .flags = 0,
                .dest_addr = dest_addr,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_send(ep_addr, dest_addr, len);
    });

    // ================================================================
    // ep->msg->injectdata (fi_injectdata)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_msg_injectdata_tag>([](ssize_t, fid_ep* ep, const void*, size_t len,
                                                                      uint64_t, fi_addr_t dest_addr,
                                                                      const auto ts_start, const auto ts_end) {
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiInject event{
                .ep_addr = ep_addr,
                .len = len,
                .dest_addr = static_cast<uint64_t>(dest_addr),
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_send(ep_addr, dest_addr, len);
    });

    // ================================================================
    // ep->msg->recv (fi_recv) — dest=0 (recv side, source unknown)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_msg_recv_tag>([](ssize_t, fid_ep* ep, void*, size_t len, void*,
                                                                fi_addr_t, void* context, const auto ts_start,
                                                                const auto ts_end) {
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiRecv event{
                .ep_addr = ep_addr,
                .context = reinterpret_cast<uint64_t>(context),
                .total_iov_len = len,
                .flags = 0,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_recv(ep_addr, len);
    });

    // ================================================================
    // ep->msg->recvv (fi_recvv)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_msg_recvv_tag>([](ssize_t, fid_ep* ep, const struct iovec* iov, void**,
                                                                 size_t count, fi_addr_t, void* context,
                                                                 const auto ts_start, const auto ts_end) {
        uint64_t total_len = 0;
        if (iov) {
            for (size_t i = 0; i < count; i++) {
                total_len += iov[i].iov_len;
            }
        }
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiRecv event{
                .ep_addr = ep_addr,
                .context = reinterpret_cast<uint64_t>(context),
                .total_iov_len = total_len,
                .flags = 0,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_recv(ep_addr, total_len);
    });

    // ================================================================
    // ep->msg->recvmsg (fi_recvmsg)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_msg_recvmsg_tag>(
            [](ssize_t, fid_ep* ep, const fi_msg* msg, uint64_t flags, const auto ts_start, const auto ts_end) {
        uint64_t total_len = 0;
        if (msg && msg->msg_iov) {
            for (size_t i = 0; i < msg->iov_count; i++) {
                total_len += msg->msg_iov[i].iov_len;
            }
        }
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiRecv event{
                .ep_addr = ep_addr,
                .context = msg ? reinterpret_cast<uint64_t>(msg->context) : 0,
                .total_iov_len = total_len,
                .flags = flags,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_recv(ep_addr, total_len);
    });

    // ================================================================
    // ep->rma->read (fi_read) — reading FROM remote, dest = src_addr
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_rma_read_tag>(
            [](ssize_t, fid_ep* ep, void*, size_t len, void*, fi_addr_t src_addr, uint64_t remote_addr,
               uint64_t remote_key, void* context, const auto ts_start, const auto ts_end) {
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiRdmaRead event{
                .ep_addr = ep_addr,
                .context = reinterpret_cast<uint64_t>(context),
                .total_iov_len = len,
                .remote_addr = remote_addr,
                .remote_key = remote_key,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_send(ep_addr, src_addr, len);
    });

    // ================================================================
    // ep->rma->readv (fi_readv)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_rma_readv_tag>(
            [](ssize_t, fid_ep* ep, const struct iovec* iov, void**, size_t count, fi_addr_t src_addr,
               uint64_t remote_addr, uint64_t remote_key, void* context, const auto ts_start, const auto ts_end) {
        uint64_t total_len = 0;
        if (iov) {
            for (size_t i = 0; i < count; i++) {
                total_len += iov[i].iov_len;
            }
        }
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiRdmaRead event{
                .ep_addr = ep_addr,
                .context = reinterpret_cast<uint64_t>(context),
                .total_iov_len = total_len,
                .remote_addr = remote_addr,
                .remote_key = remote_key,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_send(ep_addr, src_addr, total_len);
    });

    // ================================================================
    // ep->rma->readmsg (fi_readmsg)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_rma_readmsg_tag>(
            [](ssize_t, fid_ep* ep, const fi_msg_rma* msg, uint64_t, const auto ts_start, const auto ts_end) {
        uint64_t total_len = 0;
        uint64_t remote_addr = 0;
        uint64_t remote_key = 0;
        fi_addr_t dest = msg ? msg->addr : 0;
        if (msg) {
            if (msg->msg_iov) {
                for (size_t i = 0; i < msg->iov_count; i++) {
                    total_len += msg->msg_iov[i].iov_len;
                }
            }
            if (msg->rma_iov && msg->rma_iov_count > 0) {
                remote_addr = msg->rma_iov[0].addr;
                remote_key = msg->rma_iov[0].key;
            }
        }
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiRdmaRead event{
                .ep_addr = ep_addr,
                .context = msg ? reinterpret_cast<uint64_t>(msg->context) : 0,
                .total_iov_len = total_len,
                .remote_addr = remote_addr,
                .remote_key = remote_key,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_send(ep_addr, dest, total_len);
    });

    // ================================================================
    // ep->rma->write (fi_write)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_rma_write_tag>(
            [](ssize_t, fid_ep* ep, const void*, size_t len, void*, fi_addr_t dest_addr, uint64_t remote_addr,
               uint64_t remote_key, void* context, const auto ts_start, const auto ts_end) {
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiRdmaWrite event{
                .ep_addr = ep_addr,
                .context = reinterpret_cast<uint64_t>(context),
                .total_iov_len = len,
                .remote_addr = remote_addr,
                .remote_key = remote_key,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_send(ep_addr, dest_addr, len);
    });

    // ================================================================
    // ep->rma->writev (fi_writev)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_rma_writev_tag>(
            [](ssize_t, fid_ep* ep, const struct iovec* iov, void**, size_t count, fi_addr_t dest_addr,
               uint64_t remote_addr, uint64_t remote_key, void* context, const auto ts_start, const auto ts_end) {
        uint64_t total_len = 0;
        if (iov) {
            for (size_t i = 0; i < count; i++) {
                total_len += iov[i].iov_len;
            }
        }
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiRdmaWrite event{
                .ep_addr = ep_addr,
                .context = reinterpret_cast<uint64_t>(context),
                .total_iov_len = total_len,
                .remote_addr = remote_addr,
                .remote_key = remote_key,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_send(ep_addr, dest_addr, total_len);
    });

    // ================================================================
    // ep->rma->writemsg (fi_writemsg)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_rma_writemsg_tag>(
            [](ssize_t, fid_ep* ep, const fi_msg_rma* msg, uint64_t, const auto ts_start, const auto ts_end) {
        uint64_t total_len = 0;
        uint64_t remote_addr = 0;
        uint64_t remote_key = 0;
        fi_addr_t dest = msg ? msg->addr : 0;
        if (msg) {
            if (msg->msg_iov) {
                for (size_t i = 0; i < msg->iov_count; i++) {
                    total_len += msg->msg_iov[i].iov_len;
                }
            }
            if (msg->rma_iov && msg->rma_iov_count > 0) {
                remote_addr = msg->rma_iov[0].addr;
                remote_key = msg->rma_iov[0].key;
            }
        }
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiRdmaWrite event{
                .ep_addr = ep_addr,
                .context = msg ? reinterpret_cast<uint64_t>(msg->context) : 0,
                .total_iov_len = total_len,
                .remote_addr = remote_addr,
                .remote_key = remote_key,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_send(ep_addr, dest, total_len);
    });

    // ================================================================
    // ep->rma->inject (fi_inject_write)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_rma_inject_tag>(
            [](ssize_t, fid_ep* ep, const void*, size_t len, fi_addr_t dest_addr, uint64_t remote_addr,
               uint64_t remote_key, const auto ts_start, const auto ts_end) {
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiRdmaWrite event{
                .ep_addr = ep_addr,
                .context = 0,
                .total_iov_len = len,
                .remote_addr = remote_addr,
                .remote_key = remote_key,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_send(ep_addr, dest_addr, len);
    });

    // ================================================================
    // ep->rma->writedata (fi_writedata)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_rma_writedata_tag>(
            [](ssize_t, fid_ep* ep, const void*, size_t len, void*, uint64_t, fi_addr_t dest_addr, uint64_t remote_addr,
               uint64_t remote_key, void* context, const auto ts_start, const auto ts_end) {
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiRdmaWrite event{
                .ep_addr = ep_addr,
                .context = reinterpret_cast<uint64_t>(context),
                .total_iov_len = len,
                .remote_addr = remote_addr,
                .remote_key = remote_key,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_send(ep_addr, dest_addr, len);
    });

    // ================================================================
    // ep->rma->injectdata (fi_injectwritedata)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_rma_injectdata_tag>(
            [](ssize_t, fid_ep* ep, const void*, size_t len, uint64_t, fi_addr_t dest_addr, uint64_t remote_addr,
               uint64_t remote_key, const auto ts_start, const auto ts_end) {
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiRdmaWrite event{
                .ep_addr = ep_addr,
                .context = 0,
                .total_iov_len = len,
                .remote_addr = remote_addr,
                .remote_key = remote_key,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_send(ep_addr, dest_addr, len);
    });

    // ================================================================
    // ep->tagged->send (fi_tsend)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_tagged_send_tag>([](ssize_t, fid_ep* ep, const void*, size_t len, void*,
                                                                   fi_addr_t dest_addr, uint64_t tag, void* context,
                                                                   const auto ts_start, const auto ts_end) {
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiSend event{
                .ep_addr = ep_addr,
                .context = reinterpret_cast<uint64_t>(context),
                .total_iov_len = len,
                .flags = tag,
                .dest_addr = dest_addr,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_send(ep_addr, dest_addr, len);
    });

    // ================================================================
    // ep->tagged->sendv (fi_tsendv)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_tagged_sendv_tag>(
            [](ssize_t, fid_ep* ep, const struct iovec* iov, void**, size_t count, fi_addr_t dest_addr, uint64_t tag,
               void* context, const auto ts_start, const auto ts_end) {
        uint64_t total_len = 0;
        if (iov) {
            for (size_t i = 0; i < count; i++) {
                total_len += iov[i].iov_len;
            }
        }
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiSend event{
                .ep_addr = ep_addr,
                .context = reinterpret_cast<uint64_t>(context),
                .total_iov_len = total_len,
                .flags = tag,
                .dest_addr = dest_addr,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_send(ep_addr, dest_addr, total_len);
    });

    // ================================================================
    // ep->tagged->sendmsg (fi_tsendmsg)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_tagged_sendmsg_tag>(
            [](ssize_t, fid_ep* ep, const fi_msg_tagged* msg, uint64_t flags, const auto ts_start, const auto ts_end) {
        uint64_t total_len = 0;
        if (msg && msg->msg_iov) {
            for (size_t i = 0; i < msg->iov_count; i++) {
                total_len += msg->msg_iov[i].iov_len;
            }
        }
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const fi_addr_t dest = msg ? msg->addr : 0;
        const Profiler::Events::FiSend event{
                .ep_addr = ep_addr,
                .context = msg ? reinterpret_cast<uint64_t>(msg->context) : 0,
                .total_iov_len = total_len,
                .flags = flags,
                .dest_addr = dest,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_send(ep_addr, dest, total_len);
    });

    // ================================================================
    // ep->tagged->inject (fi_tinject)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_tagged_inject_tag>([](ssize_t, fid_ep* ep, const void*, size_t len,
                                                                     fi_addr_t dest_addr, uint64_t, const auto ts_start,
                                                                     const auto ts_end) {
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiInject event{
                .ep_addr = ep_addr,
                .len = len,
                .dest_addr = static_cast<uint64_t>(dest_addr),
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_send(ep_addr, dest_addr, len);
    });

    // ================================================================
    // ep->tagged->senddata (fi_tsenddata)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_tagged_senddata_tag>(
            [](ssize_t, fid_ep* ep, const void*, size_t len, void*, uint64_t, fi_addr_t dest_addr, uint64_t tag,
               void* context, const auto ts_start, const auto ts_end) {
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiSend event{
                .ep_addr = ep_addr,
                .context = reinterpret_cast<uint64_t>(context),
                .total_iov_len = len,
                .flags = tag,
                .dest_addr = dest_addr,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_send(ep_addr, dest_addr, len);
    });

    // ================================================================
    // ep->tagged->injectdata (fi_tinjectdata)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_tagged_injectdata_tag>([](ssize_t, fid_ep* ep, const void*, size_t len,
                                                                         uint64_t, fi_addr_t dest_addr, uint64_t,
                                                                         const auto ts_start, const auto ts_end) {
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiInject event{
                .ep_addr = ep_addr,
                .len = len,
                .dest_addr = static_cast<uint64_t>(dest_addr),
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_send(ep_addr, dest_addr, len);
    });

    // ================================================================
    // ep->tagged->recv (fi_trecv) — dest=0 (recv side)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_tagged_recv_tag>([](ssize_t, fid_ep* ep, void*, size_t len, void*,
                                                                   fi_addr_t, uint64_t tag, uint64_t, void* context,
                                                                   const auto ts_start, const auto ts_end) {
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiRecv event{
                .ep_addr = ep_addr,
                .context = reinterpret_cast<uint64_t>(context),
                .total_iov_len = len,
                .flags = tag,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_recv(ep_addr, len);
    });

    // ================================================================
    // ep->tagged->recvv (fi_trecvv)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_tagged_recvv_tag>(
            [](ssize_t, fid_ep* ep, const struct iovec* iov, void**, size_t count, fi_addr_t, uint64_t tag, uint64_t,
               void* context, const auto ts_start, const auto ts_end) {
        uint64_t total_len = 0;
        if (iov) {
            for (size_t i = 0; i < count; i++) {
                total_len += iov[i].iov_len;
            }
        }
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiRecv event{
                .ep_addr = ep_addr,
                .context = reinterpret_cast<uint64_t>(context),
                .total_iov_len = total_len,
                .flags = tag,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_recv(ep_addr, total_len);
    });

    // ================================================================
    // ep->tagged->recvmsg (fi_trecvmsg)
    // ================================================================
    m_libfabric_profiler->register_post<fi::ep_tagged_recvmsg_tag>(
            [](ssize_t, fid_ep* ep, const fi_msg_tagged* msg, uint64_t flags, const auto ts_start, const auto ts_end) {
        uint64_t total_len = 0;
        if (msg && msg->msg_iov) {
            for (size_t i = 0; i < msg->iov_count; i++) {
                total_len += msg->msg_iov[i].iov_len;
            }
        }
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::FiRecv event{
                .ep_addr = ep_addr,
                .context = msg ? reinterpret_cast<uint64_t>(msg->context) : 0,
                .total_iov_len = total_len,
                .flags = flags,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        fi_track_recv(ep_addr, total_len);
    });

    std::cout << "[Profiler] Loaded Libfabric Trace" << std::endl;
}
