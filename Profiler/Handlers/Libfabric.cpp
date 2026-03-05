#ifdef SIGMA_PROVIDER_libfabric_trace

#include "../ProfilerProviders.hpp"
#include "../ProfilerShim.hpp"
#include "Common.hpp"

#include "../Events/LibfabricEvents.hpp"
#include "LibfabricEventSource.hpp"
// ================================================================================================
void ProfilerCtx::load_libfabric(const std::filesystem::path& shim_dir) {
    m_libfabric_profiler = load_provider<libfabric::Profiler>(shim_dir / Profiler::SIGMA_LIBFABRIC_PROVIDER_PATH);

    // Object-ops: ep->msg->sendmsg (captures fi_send/fi_sendv/fi_sendmsg/fi_inject)
    m_libfabric_profiler->register_post<fi::ep_msg_sendmsg_tag>(
            [](ssize_t, fid_ep* ep, const fi_msg* msg, uint64_t flags, const auto ts_start, const auto ts_end) {
        tls_active_fi_ep = reinterpret_cast<uint64_t>(ep);
        uint64_t total_len = 0;
        if (msg && msg->msg_iov) {
            for (size_t i = 0; i < msg->iov_count; i++) {
                total_len += msg->msg_iov[i].iov_len;
            }
        }
        const Profiler::Events::FiSend event{
                .ep_addr = reinterpret_cast<uint64_t>(ep),
                .context = msg ? reinterpret_cast<uint64_t>(msg->context) : 0,
                .total_iov_len = total_len,
                .flags = flags,
                .dest_addr = msg ? msg->addr : 0,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        tls_active_fi_ep = 0;
    });

    m_libfabric_profiler->register_post<fi::ep_msg_recvmsg_tag>(
            [](ssize_t, fid_ep* ep, const fi_msg* msg, uint64_t flags, const auto ts_start, const auto ts_end) {
        uint64_t total_len = 0;
        if (msg && msg->msg_iov) {
            for (size_t i = 0; i < msg->iov_count; i++) {
                total_len += msg->msg_iov[i].iov_len;
            }
        }
        const Profiler::Events::FiRecv event{
                .ep_addr = reinterpret_cast<uint64_t>(ep),
                .context = msg ? reinterpret_cast<uint64_t>(msg->context) : 0,
                .total_iov_len = total_len,
                .flags = flags,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
    });

    m_libfabric_profiler->register_post<fi::ep_rma_readmsg_tag>(
            [](ssize_t, fid_ep* ep, const fi_msg_rma* msg, uint64_t, const auto ts_start, const auto ts_end) {
        tls_active_fi_ep = reinterpret_cast<uint64_t>(ep);
        uint64_t total_len = 0;
        uint64_t remote_addr = 0;
        uint64_t remote_key = 0;
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

        const Profiler::Events::FiRdmaRead event{
                .ep_addr = reinterpret_cast<uint64_t>(ep),
                .context = msg ? reinterpret_cast<uint64_t>(msg->context) : 0,
                .total_iov_len = total_len,
                .remote_addr = remote_addr,
                .remote_key = remote_key,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        tls_active_fi_ep = 0;
    });

    m_libfabric_profiler->register_post<fi::ep_rma_writemsg_tag>(
            [](ssize_t, fid_ep* ep, const fi_msg_rma* msg, uint64_t, const auto ts_start, const auto ts_end) {
        tls_active_fi_ep = reinterpret_cast<uint64_t>(ep);
        uint64_t total_len = 0;
        uint64_t remote_addr = 0;
        uint64_t remote_key = 0;
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

        const Profiler::Events::FiRdmaWrite event{
                .ep_addr = reinterpret_cast<uint64_t>(ep),
                .context = msg ? reinterpret_cast<uint64_t>(msg->context) : 0,
                .total_iov_len = total_len,
                .remote_addr = remote_addr,
                .remote_key = remote_key,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        tls_active_fi_ep = 0;
    });

    std::cout << "[Profiler] Loaded libfabric tracing" << std::endl;
}
// ================================================================================================
#endif
