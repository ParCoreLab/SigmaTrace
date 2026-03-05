#ifdef SIGMA_PROVIDER_ucx_trace

#include "../ProfilerProviders.hpp"
#include "../ProfilerShim.hpp"
#include "Common.hpp"

#include "../Events/UcxEvents.hpp"
#include "UcxEventSource.hpp"

// ================================================================================================
void ProfilerCtx::load_ucx(const std::filesystem::path& shim_dir) {
    m_ucx_profiler = load_provider<ucx::Profiler>(shim_dir / Profiler::SIGMA_UCX_PROVIDER_PATH);

    m_ucx_profiler->register_post<ucx::ucp_init_version_tag>(
            [](ucs_status_t status, unsigned, unsigned, const ucp_params_t*, const ucp_config_t*,
               ucp_context_h* context_p, const auto ts_start, const auto ts_end) {
        std::cout << "[Profiler] ucp_init_version_tag called!" << std::endl;

        if (status != 0 || !context_p || !*context_p) {
            return;
        }

        const Profiler::Events::UcpInit event{
                .context_addr = reinterpret_cast<uint64_t>(*context_p),
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
    });

    m_ucx_profiler->register_post<ucx::ucp_worker_create_tag>([](ucs_status_t status, ucp_context_h context,
                                                                 const ucp_worker_params_t*, ucp_worker_h* worker_p,
                                                                 const auto ts_start, const auto ts_end) {
        if (status != 0 || !worker_p || !*worker_p) {
            return;
        }

        const Profiler::Events::UcpWorkerCreate event{
                .context_addr = reinterpret_cast<uint64_t>(context),
                .worker_addr = reinterpret_cast<uint64_t>(*worker_p),
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
    });

    m_ucx_profiler->register_pre<ucx::ucp_worker_destroy_tag>([](ucp_worker_h worker, const auto ts_start) {
        const Profiler::Events::UcpWorkerDestroy event{
                .worker_addr = reinterpret_cast<uint64_t>(worker),
        };

        instance().m_event_recorder->Record(event, ts_start);
    });

    m_ucx_profiler->register_post<ucx::ucp_ep_create_tag>([](ucs_status_t status, ucp_worker_h worker,
                                                             const ucp_ep_params_t*, ucp_ep_h* ep_p,
                                                             const auto ts_start, const auto ts_end) {
        if (status != 0 || !ep_p || !*ep_p) {
            return;
        }

        const Profiler::Events::UcpEpCreate event{
                .worker_addr = reinterpret_cast<uint64_t>(worker),
                .ep_addr = reinterpret_cast<uint64_t>(*ep_p),
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
    });

    m_ucx_profiler->register_pre<ucx::ucp_ep_close_nbx_tag>(
            [](ucp_ep_h ep, const ucp_request_param_t*, const auto ts_start) {
        const Profiler::Events::UcpEpClose event{
                .ep_addr = reinterpret_cast<uint64_t>(ep),
        };
        instance().m_event_recorder->Record(event, ts_start);
    });

    m_ucx_profiler->register_post<ucx::ucp_tag_send_nbx_tag>([](ucs_status_ptr_t request, ucp_ep_h ep, const void*,
                                                                size_t count, ucp_tag_t tag, const ucp_request_param_t*,
                                                                const auto ts_start, const auto ts_end) {
        tls_active_ucp_ep = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::UcpTagSend event{
                .ep_addr = reinterpret_cast<uint64_t>(ep),
                .tag = tag,
                .size = count,
                .request_addr = reinterpret_cast<uint64_t>(request),
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        tls_active_ucp_ep = 0;
    });

    m_ucx_profiler->register_post<ucx::ucp_tag_recv_nbx_tag>(
            [](ucs_status_ptr_t request, ucp_worker_h worker, void*, size_t count, ucp_tag_t tag, ucp_tag_t tag_mask,
               const ucp_request_param_t*, const auto ts_start, const auto ts_end) {
        const Profiler::Events::UcpTagRecv event{
                .worker_addr = reinterpret_cast<uint64_t>(worker),
                .tag = tag,
                .tag_mask = tag_mask,
                .size = count,
                .request_addr = reinterpret_cast<uint64_t>(request),
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
    });

    m_ucx_profiler->register_post<ucx::ucp_put_nbx_tag>(
            [](ucs_status_ptr_t request, ucp_ep_h ep, const void*, size_t count, uint64_t remote_addr, ucp_rkey_h,
               const ucp_request_param_t*, const auto ts_start, const auto ts_end) {
        tls_active_ucp_ep = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::UcpPut event{
                .ep_addr = reinterpret_cast<uint64_t>(ep),
                .size = count,
                .remote_addr = remote_addr,
                .request_addr = reinterpret_cast<uint64_t>(request),
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        tls_active_ucp_ep = 0;
    });

    m_ucx_profiler->register_post<ucx::ucp_get_nbx_tag>([](ucs_status_ptr_t request, ucp_ep_h ep, void*, size_t count,
                                                           uint64_t remote_addr, ucp_rkey_h, const ucp_request_param_t*,
                                                           const auto ts_start, const auto ts_end) {
        tls_active_ucp_ep = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::UcpGet event{
                .ep_addr = reinterpret_cast<uint64_t>(ep),
                .size = count,
                .remote_addr = remote_addr,
                .request_addr = reinterpret_cast<uint64_t>(request),
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        tls_active_ucp_ep = 0;
    });

    m_ucx_profiler->register_post<ucx::ucp_am_send_nbx_tag>(
            [](ucs_status_ptr_t request, ucp_ep_h ep, unsigned int am_id, const void*, size_t header_length,
               const void*, size_t count, const ucp_request_param_t*, const auto ts_start, const auto ts_end) {
        tls_active_ucp_ep = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::UcpAmSend event{
                .ep_addr = reinterpret_cast<uint64_t>(ep),
                .am_id = am_id,
                .header_size = header_length,
                .payload_size = count,
                .request_addr = reinterpret_cast<uint64_t>(request),
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
        tls_active_ucp_ep = 0;
    });

    m_ucx_profiler->register_post<ucx::ucp_mem_map_tag>([](ucs_status_t status, ucp_context_h context,
                                                           const ucp_mem_map_params_t*, ucp_mem_h*, const auto ts_start,
                                                           const auto ts_end) {
        if (status != 0) {
            return;
        }

        const Profiler::Events::UcpMemMap event{
                .context_addr = reinterpret_cast<uint64_t>(context),
                .addr = 0, // Would need to dereference params to get addr
                .size = 0,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
    });

    m_ucx_profiler->register_post<ucx::ucp_mem_unmap_tag>(
            [](ucs_status_t, ucp_context_h context, ucp_mem_h memh, const auto ts_start, const auto ts_end) {
        const Profiler::Events::UcpMemUnmap event{
                .context_addr = reinterpret_cast<uint64_t>(context),
                .mem_addr = reinterpret_cast<uint64_t>(memh),
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
    });

    // TODO: this might spam
    m_ucx_profiler->register_post<ucx::ucp_worker_progress_tag>(
            [](unsigned int num_progressed, ucp_worker_h worker, const auto ts_start, const auto ts_end) {
        if (num_progressed == 0) {
            return;
        }

        const Profiler::Events::UcpWorkerProgress event{
                .worker_addr = reinterpret_cast<uint64_t>(worker),
                .num_progressed = num_progressed,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
    });

    std::cout << "[Profiler] Loaded UCX" << std::endl;
}
// ================================================================================================
#endif
