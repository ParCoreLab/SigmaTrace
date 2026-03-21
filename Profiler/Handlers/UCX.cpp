#include "../ProfilerProviders.hpp"
#include "../ProfilerShim.hpp"
#include "Common.hpp"

#include <absl/hash/hash.h>
#include <dlfcn.h>

#include "../Events/UcxEvents.hpp"
#include "Dlsym.hpp"
#include "UcxEventSource.hpp"

// Length of the UCX address prefix to hash for cross-process correlation.
// UCX addresses are opaque and variable-length, but the first 32 bytes contain
// the worker UUID which is sufficient for unique fingerprinting.
static constexpr size_t kUcxAddrHashPrefix = 32;

// ================================================================================================
void ProfilerCtx::load_ucx(const std::filesystem::path& shim_dir, const Profiler::ProfilerArgs& args) {
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

        // Query worker address ourselves (like register_nic does for IBV)
        // so we always have the address hash for cross-process correlation
        static auto* fn_get_addr = reinterpret_cast<decltype(ucp_worker_get_address)*>(
                smart_dlsym::real_dlsym(RTLD_NEXT, "ucp_worker_get_address"));
        static auto* fn_release_addr = reinterpret_cast<decltype(ucp_worker_release_address)*>(
                smart_dlsym::real_dlsym(RTLD_NEXT, "ucp_worker_release_address"));

        if (fn_get_addr && fn_release_addr) {
            ucp_address_t* addr = nullptr;
            size_t addr_len = 0;
            if (fn_get_addr(*worker_p, &addr, &addr_len) == UCS_OK && addr && addr_len >= kUcxAddrHashPrefix) {
                const auto hash = absl::HashOf(
                        std::string_view(reinterpret_cast<const char*>(addr), kUcxAddrHashPrefix));

                const Profiler::Events::UcpWorkerAddress addr_event{
                        .worker_addr = reinterpret_cast<uint64_t>(*worker_p),
                        .address_hash = hash,
                };
                instance().m_event_recorder->Record(addr_event, ts_start, ts_end);

                fn_release_addr(*worker_p, addr);
            }
        }
    });

    m_ucx_profiler->register_pre<ucx::ucp_worker_destroy_tag>([](ucp_worker_h worker, const auto ts_start) {
        const Profiler::Events::UcpWorkerDestroy event{
                .worker_addr = reinterpret_cast<uint64_t>(worker),
        };

        instance().m_event_recorder->Record(event, ts_start);
    });

    m_ucx_profiler->register_post<ucx::ucp_worker_get_address_tag>([](ucs_status_t status, ucp_worker_h worker,
                                                                      ucp_address_t** addr_p, size_t* len_p,
                                                                      const auto ts_start, const auto ts_end) {
        if (status != 0 || !addr_p || !*addr_p || !len_p || *len_p < kUcxAddrHashPrefix) {
            return;
        }

        const auto hash = absl::HashOf(
                std::string_view(reinterpret_cast<const char*>(*addr_p), kUcxAddrHashPrefix));

        const Profiler::Events::UcpWorkerAddress event{
                .worker_addr = reinterpret_cast<uint64_t>(worker),
                .address_hash = hash,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
    });

    // Pre-hook: signal that UCX EP creation is in progress on this thread
    m_ucx_profiler->register_pre<ucx::ucp_ep_create_tag>(
            [](ucp_worker_h, const ucp_ep_params_t*, ucp_ep_h*, const auto) {
        t_ucx_ep_creating = true;
        t_ucx_captured_qpn = 0;
    });

    m_ucx_profiler->register_post<ucx::ucp_ep_create_tag>([](ucs_status_t status, ucp_worker_h worker,
                                                             const ucp_ep_params_t* params, ucp_ep_h* ep_p,
                                                             const auto ts_start, const auto ts_end) {
        if (status != 0 || !ep_p || !*ep_p) {
            t_ucx_ep_creating = false;
            return;
        }

        const auto ep_addr = reinterpret_cast<uint64_t>(*ep_p);

        const Profiler::Events::UcpEpCreate event{
                .worker_addr = reinterpret_cast<uint64_t>(worker),
                .ep_addr = ep_addr,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);

        // UCX-native destination: hash a fixed prefix of the remote worker address
        if (params && (params->field_mask & UCP_EP_PARAM_FIELD_REMOTE_ADDRESS) && params->address) {
            const auto remote_hash = absl::HashOf(
                    std::string_view(reinterpret_cast<const char*>(params->address), kUcxAddrHashPrefix));

            const Profiler::Events::UcpEpConnection conn_event{
                    .ep_addr = ep_addr,
                    .remote_addr_hash = remote_hash,
            };
            instance().m_event_recorder->Record(conn_event, ts_start, ts_end);
        }

        // IBV QP correlation: if ibv_create_qp was called during EP creation, capture the mapping
        t_ucx_ep_creating = false;
        if (t_ucx_captured_qpn != 0) {
            const uint32_t qpn = t_ucx_captured_qpn;
            t_ucx_captured_qpn = 0;

            {
                std::scoped_lock lock(instance().m_ucx_ep_qp_mutex);
                instance().m_ucx_ep_qp_map[ep_addr] = qpn;
            }

            instance().m_event_recorder->Record(Profiler::Events::UcpEpQpMapping{.ep_addr = ep_addr, .qp_num = qpn}, 0);
        }
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
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::UcpTagSend event{
                .ep_addr = ep_addr,
                .tag = tag,
                .size = count,
                .request_addr = reinterpret_cast<uint64_t>(request),
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);

        auto& stats = instance().m_ucx_send_registry.get_thread_local()[ep_addr];
        stats.bytes_sent += count;
        stats.send_count++;
    });

    m_ucx_profiler->register_post<ucx::ucp_tag_recv_nbx_tag>(
            [](ucs_status_ptr_t request, ucp_worker_h worker, void*, size_t count, ucp_tag_t tag, ucp_tag_t tag_mask,
               const ucp_request_param_t*, const auto ts_start, const auto ts_end) {
        const auto worker_addr = reinterpret_cast<uint64_t>(worker);
        const Profiler::Events::UcpTagRecv event{
                .worker_addr = worker_addr,
                .tag = tag,
                .tag_mask = tag_mask,
                .size = count,
                .request_addr = reinterpret_cast<uint64_t>(request),
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);

        auto& stats = instance().m_ucx_recv_registry.get_thread_local()[worker_addr];
        stats.bytes_recv += count;
        stats.recv_count++;
    });

    m_ucx_profiler->register_post<ucx::ucp_put_nbx_tag>(
            [](ucs_status_ptr_t request, ucp_ep_h ep, const void*, size_t count, uint64_t remote_addr, ucp_rkey_h,
               const ucp_request_param_t*, const auto ts_start, const auto ts_end) {
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::UcpPut event{
                .ep_addr = ep_addr,
                .size = count,
                .remote_addr = remote_addr,
                .request_addr = reinterpret_cast<uint64_t>(request),
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);

        auto& stats = instance().m_ucx_send_registry.get_thread_local()[ep_addr];
        stats.bytes_sent += count;
        stats.send_count++;
    });

    m_ucx_profiler->register_post<ucx::ucp_get_nbx_tag>([](ucs_status_ptr_t request, ucp_ep_h ep, void*, size_t count,
                                                           uint64_t remote_addr, ucp_rkey_h, const ucp_request_param_t*,
                                                           const auto ts_start, const auto ts_end) {
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::UcpGet event{
                .ep_addr = ep_addr,
                .size = count,
                .remote_addr = remote_addr,
                .request_addr = reinterpret_cast<uint64_t>(request),
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);

        auto& stats = instance().m_ucx_send_registry.get_thread_local()[ep_addr];
        stats.bytes_sent += count;
        stats.send_count++;
    });

    m_ucx_profiler->register_post<ucx::ucp_am_send_nbx_tag>(
            [](ucs_status_ptr_t request, ucp_ep_h ep, unsigned int am_id, const void*, size_t header_length,
               const void*, size_t count, const ucp_request_param_t*, const auto ts_start, const auto ts_end) {
        const auto ep_addr = reinterpret_cast<uint64_t>(ep);
        const Profiler::Events::UcpAmSend event{
                .ep_addr = ep_addr,
                .am_id = am_id,
                .header_size = header_length,
                .payload_size = count,
                .request_addr = reinterpret_cast<uint64_t>(request),
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);

        auto& stats = instance().m_ucx_send_registry.get_thread_local()[ep_addr];
        stats.bytes_sent += header_length + count;
        stats.send_count++;
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

    if (args.TraceMode == Profiler::ProfilerTraceMode::Full) {
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
    }

    std::cout << "[Profiler] Loaded UCX Trace" << std::endl;
}
// ================================================================================================
