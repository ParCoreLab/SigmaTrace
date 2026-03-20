#include "../../ProfilerShim.hpp"
#include "../Common.hpp"
#include "../ProfilerProviders.hpp"

#include "../../Events/IbvEvents.hpp"
#include "IbverbsEventSource.hpp"

#include <infiniband/verbs.h>

#include "IBVCommon.hpp"

void ProfilerCtx::load_ibv(const std::filesystem::path& shim_dir, const Profiler::ProfilerArgs& args) noexcept {
    m_ibv_profiler = load_provider<ibv::Profiler>(shim_dir / Profiler::SIGMA_IBVERBS_PROVIDER_PATH);

    m_ibv_profiler->register_post<ibv::open_device_tag>(
            [](ibv_context* out_context, ibv_device* device, const auto ts_start, const auto ts_end) {
        const auto event = register_nic(instance().m_event_recorder.get(), out_context, device);
        instance().m_event_recorder->Record(event, ts_start, ts_end);
    });

    m_ibv_profiler->register_post<ibv::close_device_tag>(
            [](int, ibv_context* context, const auto ts_start, const auto ts_end) {
        const Profiler::Events::IbvCloseDevice event{
                .context_addr = reinterpret_cast<uint64_t>(context),
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
    });

    m_ibv_profiler->register_post<ibv::create_qp_tag>(
            [](ibv_qp* out_qp, ibv_pd*, ibv_qp_init_attr*, const auto ts_start, const auto ts_end) {
        const Profiler::Events::IbvCreateQp event{
                .context_addr = reinterpret_cast<uint64_t>(out_qp->context),
                .qp_num = out_qp->qp_num,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
    });

    m_ibv_profiler->register_pre<ibv::destroy_qp_tag>([](ibv_qp* qp, const auto ts_start) {
        const Profiler::Events::IbvDestroyQp event{
                // WARNING: not qp->context. Why? I have no clue
                .context_addr = reinterpret_cast<uint64_t>(qp->pd->context),
                .qp_num = qp->qp_num,
        };
        instance().m_event_recorder->Record(event, ts_start);
    });

    m_ibv_profiler->register_post<ibv::modify_qp_tag>(
            [](int, ibv_qp* qp, ibv_qp_attr* attr, int attr_mask, const auto ts_start, const auto ts_end) {
        if (!(attr_mask & IBV_QP_DEST_QPN)) {
            return;
        }

        const auto context_addr = reinterpret_cast<uint64_t>(qp->context);

        using Profiler::Events::IbvModifyQp;
        const IbvModifyQp event{
                .context_addr = context_addr,
                .qp_num = qp->qp_num,
                .dest_qp_num = attr->dest_qp_num,
                .ah_attr_dlid = attr->ah_attr.dlid,
                .ah_attr_grh_dgid =
                        {
                                .subnet_prefix = attr->ah_attr.grh.dgid.global.subnet_prefix,
                                .interface_id = attr->ah_attr.grh.dgid.global.interface_id,
                        },
        };

        instance().m_event_recorder->Record(event, ts_start, ts_end);

        // Extract and record QP connection info
        const auto dest_qpn = attr->dest_qp_num;
        const auto& dgid = attr->ah_attr.grh.dgid;
        const auto dest_interface_id = be64toh(dgid.global.interface_id);
        const auto dest_subnet_prefix = be64toh(dgid.global.subnet_prefix);
        const auto dest_lid = attr->ah_attr.dlid;
        const auto port_num = attr->ah_attr.port_num;

        const Profiler::Events::QpConnectionInfo conn_event{
                .context_addr = context_addr,
                .src_qpn = qp->qp_num,
                .dest_qpn = dest_qpn,
                .dest_interface_id = dest_interface_id,
                .dest_subnet_prefix = dest_subnet_prefix,
                .dest_lid = dest_lid,
                .port_num = port_num,
        };
        instance().m_event_recorder->Record(conn_event, ts_start + 1, ts_end - 1);

        // Store connection info for shutdown summary
        auto& profiler = instance();
        std::scoped_lock lock(profiler.m_qp_connections_mutex);
        profiler.m_qp_connections[qp->qp_num] = QpConnectionEntry{
                .context_addr = context_addr,
                .dest_qpn = dest_qpn,
                .dest_interface_id = dest_interface_id,
                .dest_subnet_prefix = dest_subnet_prefix,
                .dest_lid = dest_lid,
                .port_num = port_num,
        };
    });

    // ================================================
    // Memory registration
    // TODO: this can be done automatically/declaratively by declaring which arg is addr, which one is length, etc.
    m_ibv_profiler->register_post<ibv::reg_mr_tag>(
            [](ibv_mr*, ibv_pd* pd, void* addr, size_t length, int, const auto ts_start, const auto ts_end) {
        const Profiler::Events::IbvMemoryReg event{
                .context_addr = PtrToUint(pd->context),
                .addr = PtrToUint(addr),
                .size = length,
                .type = Profiler::Events::IbvMemoryRegType::ibv_reg_mr_iova2,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
    });

    m_ibv_profiler->register_post<ibv::reg_mr_iova2_tag>([](ibv_mr*, ibv_pd* pd, void* addr, size_t length, uint64_t,
                                                            unsigned int, const auto ts_start, const auto ts_end) {
        const Profiler::Events::IbvMemoryReg event{
                .context_addr = PtrToUint(pd->context),
                .addr = PtrToUint(addr),
                .size = length,
                .type = Profiler::Events::IbvMemoryRegType::ibv_reg_mr_iova2,
        };
        instance().m_event_recorder->Record(event, ts_start, ts_end);
    });

    m_ibv_profiler->register_post<ibv::post_send_tag>(
            [](int, ibv_qp* qp, ibv_send_wr* wr, ibv_send_wr**, const auto ts_start, const auto ts_end) {
        const auto& profiler = instance();
        static const auto trace_mode = profiler.m_args.TraceMode;

        const auto context_addr = PtrToUint(qp->context);

        uint32_t num_wrs = 0;
        uint64_t total_bytes_sent = 0;
        for (const auto* wr_it = wr; wr_it != nullptr; wr_it = wr_it->next) {
            num_wrs += 1;

            // Sum scatter-gather lengths for traffic accounting
            for (auto sg_idx = 0; sg_idx < wr_it->num_sge; sg_idx++) {
                total_bytes_sent += wr_it->sg_list[sg_idx].length;
            }

            if (trace_mode == Profiler::ProfilerTraceMode::Full) {
                using Profiler::Events::IbvPostSendWr;
                const IbvPostSendWr wr_event{
                        .context_addr = context_addr,
                        .wr_id = wr_it->wr_id,
                        .qp_num = qp->qp_num,
                        .num_sge = wr_it->num_sge,
                        .send_flags = wr_it->send_flags,
                        .opcode = static_cast<decltype(IbvPostSendWr::opcode)>(wr_it->opcode),
                };
                profiler.m_event_recorder->Record(wr_event, ts_start + 1, ts_end - 1);
            }
        }

        // Traffic accumulation on consumer thread
        if (total_bytes_sent > 0) {
            auto& stats = instance().m_traffic_registry.get_thread_local()[qp->qp_num];
            stats.bytes_sent += total_bytes_sent;
            stats.send_count++;
        }

        // Simple and Full record this
        if (trace_mode >= Profiler::ProfilerTraceMode::Full) {
            Profiler::Events::IbvPostSend post_send_event{
                    .context_addr = context_addr,
                    .num_wrs = num_wrs,
                    .qp_num = qp->qp_num,
            };

            profiler.m_event_recorder->Record(post_send_event, ts_start, ts_end);
        }
    });

    // No aggregate here, only simple and above
    if (args.TraceMode >= Profiler::ProfilerTraceMode::Simple) {
        m_ibv_profiler->register_post<ibv::post_recv_tag>(
                [](int, ibv_qp* qp, ibv_recv_wr* wr, ibv_recv_wr**, const auto ts_start, const auto ts_end) {
            const auto& profiler = instance();
            static const auto trace_mode = profiler.m_args.TraceMode;

            const auto context_addr = PtrToUint(qp->context);

            uint64_t num_wrs = 0;
            for (const auto* wr_it = wr; wr_it != nullptr; wr_it = wr_it->next) {
                num_wrs++;

                if (trace_mode >= Profiler::ProfilerTraceMode::Simple) {
                    const Profiler::Events::IbvPostRecvWr recv_wr_event{
                            .context_addr = context_addr,
                            .qp_num = qp->qp_num,
                            .wr_id = wr_it->wr_id,
                            .num_sge = wr_it->num_sge,
                    };
                    profiler.m_event_recorder->Record(recv_wr_event, ts_start + 1, ts_end - 1);
                }
            }

            const Profiler::Events::IbvPostRecv recv_event{
                    .context_addr = context_addr,
                    .qp_num = qp->qp_num,
                    .num_wrs = num_wrs,
            };
            profiler.m_event_recorder->Record(recv_event, ts_start, ts_end);
        });
    }

    m_ibv_profiler->register_post<ibv::poll_cq_tag>(
            [](int out_ret, ibv_cq* cq, int num_entries, ibv_wc* wc, const auto ts_start, const auto ts_end) {
        sigma_ignore_unused(num_entries);

        // This is too expensive to log every time
        // {
        //     const Profiler::Events::IbvPollCq poll_cq_event{
        //             .context_addr = context_addr,
        //             .cq_addr = reinterpret_cast<uint64_t>(cq),
        //             // TODO: might be wrong. Look at cq->
        //             .num_requested = num_entries,
        //             .num_completed = out_ret,
        //     };
        //     m_event_recorder->Record(poll_cq_event, ts_start, ts_end);
        // }

        static const auto& profiler = instance();
        static const auto trace_mode = profiler.m_args.TraceMode;

        const auto context_addr = PtrToUint(cq->context);

        for (auto i = 0; i < out_ret; i++) {
            const auto& wc_entry = wc[i];

            // TODO: might need further optimizations...
            // This is a const check
            if (trace_mode == Profiler::ProfilerTraceMode::Full) {
                using Profiler::Events::IbvCompletion;
                const IbvCompletion completion_event{
                        .context_addr = context_addr,
                        .wr_id = wc_entry.wr_id,
                        .qp_num = wc_entry.qp_num,
                        .opcode = static_cast<decltype(IbvCompletion::opcode)>(wc_entry.opcode),
                        .status = static_cast<decltype(IbvCompletion::status)>(wc_entry.status),
                        .byte_len = wc_entry.byte_len,
                };
                profiler.m_event_recorder->Record(completion_event, ts_start + 1, ts_end - 1);
            }

            // Recv traffic accumulation
            const auto is_recv = static_cast<bool>(wc_entry.opcode & IBV_WC_RECV);
            if (is_recv) {
                auto& stats = instance().m_traffic_registry.get_thread_local()[wc_entry.qp_num];
                stats.bytes_recv += wc_entry.byte_len;
                stats.recv_count++;
            }
        }
    });

    std::cout << "[Profiler] loaded Ibverbs Trace" << std::endl;
}
