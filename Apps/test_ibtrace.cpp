// TODO: VERY deprecated

#include <atomic>
#include <cstring>
#include <iostream>

#include "infiniband/verbs.h"

#include "ibverbs_trace.hpp"

using namespace ibv_hook;

struct SampleHooks : HookManager<SampleHooks, post_send_tag, post_recv_tag, poll_cq_tag> {
    // Counters
    std::atomic<uint64_t> sends{0};
    std::atomic<uint64_t> recvs{0};
    std::atomic<uint64_t> completions{0};

    // Bring in base class default hooks (for tags we don't handle)
    using HookManager::post;
    using HookManager::pre;

    void pre(post_send_tag, ibv_qp* qp, ibv_send_wr* wr, ibv_send_wr** bad_wr) {
        ++sends;
    }

    void pre(post_recv_tag, ibv_qp* qp, ibv_recv_wr* wr, ibv_recv_wr** bad_wr) {
        ++recvs;
    }

    void post(post_send_tag, int ret, ibv_qp* qp, ibv_send_wr* wr, ibv_send_wr** bad_wr) {
        if (ret != 0) {
            std::cerr << "post_send failed: " << ret << std::endl;
        }
    }

    void post(poll_cq_tag, int ret, ibv_cq* cq, int ne, ibv_wc* wc) {
        if (ret > 0) {
            completions += ret;
            for (int i = 0; i < ret; ++i) {
                if (wc[i].status != IBV_WC_SUCCESS) {
                    std::cerr << "Completion error: " << wc[i].status << std::endl;
                }
            }
        }
    }

    void print_summary() {
        std::cout << "\n=== Hook Monitoring Summary ===\n"
                  << "  Sends: " << sends << "\n"
                  << "  Recvs: " << recvs << "\n"
                  << "  Completions: " << completions << std::endl;
        print_stats();
    }
};

int main() {
    int num_devices{};
    ibv_device** dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list || !dev_list[0]) {
        std::cerr << "No IB devices" << std::endl;
        return 1;
    }

    std::cout << "Found " << num_devices << " IB device(s)" << std::endl;

    ibv_context* ctx = ibv_open_device(dev_list[0]);
    if (!ctx) {
        std::cerr << "Failed to open device\n";
        ibv_free_device_list(dev_list);
        return 1;
    }

    std::cout << "Opened device: " << ibv_get_device_name(dev_list[0]) << std::endl;

    // ================================================================
    // Hook setup
    // ================================================================

    // Should be done automatically when ibv_open_device is called
    // SampleHooks::instance().install(ctx);

    // Register additional hooks
    SampleHooks::instance().register_pre<post_send_tag>([](auto&&...) {
        std::cout << "Pre post_send hook called" << std::endl;
    });

    SampleHooks::instance().register_post<poll_cq_tag>([](int ret, auto&&...) {
        if (ret > 0) {
            std::cout << "Polled " << ret << " completion(s)" << std::endl;
        }
    });

    // ================================================================
    // RDMA setup
    // ================================================================

    // Query device attributes
    ibv_device_attr dev_attr{};
    if (ibv_query_device(ctx, &dev_attr) != 0) {
        std::cerr << "Failed to query device" << std::endl;
        SampleHooks::instance().uninstall(ctx);
        ibv_close_device(ctx);
        ibv_free_device_list(dev_list);
        return 1;
    }
    std::cout << "Max QPs: " << dev_attr.max_qp << ", Max CQs: " << dev_attr.max_cq << std::endl;

    // Query port attributes
    ibv_port_attr port_attr{};
    if (ibv_query_port(ctx, 1, &port_attr) != 0) {
        std::cerr << "Failed to query port" << std::endl;
        SampleHooks::instance().uninstall(ctx);
        ibv_close_device(ctx);
        ibv_free_device_list(dev_list);
        return 1;
    }
    std::cout << "Port state: " << port_attr.state << " (4=Active), LID: " << port_attr.lid << std::endl;

    // Allocate Protection Domain
    ibv_pd* pd = ibv_alloc_pd(ctx);
    if (!pd) {
        std::cerr << "Failed to allocate PD" << std::endl;
        SampleHooks::instance().uninstall(ctx);
        ibv_close_device(ctx);
        ibv_free_device_list(dev_list);
        return 1;
    }
    std::cout << "Allocated Protection Domain" << std::endl;

    // Create Completion Queue
    ibv_cq* cq = ibv_create_cq(ctx, 16, nullptr, nullptr, 0);
    if (!cq) {
        std::cerr << "Failed to create CQ" << std::endl;
        ibv_dealloc_pd(pd);
        SampleHooks::instance().uninstall(ctx);
        ibv_close_device(ctx);
        ibv_free_device_list(dev_list);
        return 1;
    }
    std::cout << "Created Completion Queue (capacity: 16)" << std::endl;

    // Allocate and register memory buffer
    const std::string message{"Hello RDMA!"};
    constexpr size_t buf_size = 4096;
    void* buf = aligned_alloc(4096, buf_size);
    if (!buf) {
        std::cerr << "Failed to allocate buffer" << std::endl;
        ibv_destroy_cq(cq);
        ibv_dealloc_pd(pd);
        SampleHooks::instance().uninstall(ctx);
        ibv_close_device(ctx);
        ibv_free_device_list(dev_list);
        return 1;
    }
    memset(buf, 0, buf_size);
    message.copy(static_cast<char*>(buf), message.size(), 0);

    ibv_mr* mr =
            ibv_reg_mr(pd, buf, buf_size, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) {
        std::cerr << "Failed to register MR\n";
        free(buf);
        ibv_destroy_cq(cq);
        ibv_dealloc_pd(pd);
        SampleHooks::instance().uninstall(ctx);
        ibv_close_device(ctx);
        ibv_free_device_list(dev_list);
        return 1;
    }
    std::cout << "Registered Memory Region: addr=" << buf << ", size=" << buf_size << ", lkey=0x" << std::hex
              << mr->lkey << ", rkey=0x" << mr->rkey << std::dec << std::endl;

    // Create Queue Pair (RC type)
    ibv_qp_init_attr qp_init_attr{};
    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq;
    qp_init_attr.qp_type = IBV_QPT_RC;
    qp_init_attr.cap.max_send_wr = 16;
    qp_init_attr.cap.max_recv_wr = 16;
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;

    ibv_qp* qp = ibv_create_qp(pd, &qp_init_attr);
    if (!qp) {
        std::cerr << "Failed to create QP" << std::endl;
        ibv_dereg_mr(mr);
        free(buf);
        ibv_destroy_cq(cq);
        ibv_dealloc_pd(pd);
        SampleHooks::instance().uninstall(ctx);
        ibv_close_device(ctx);
        ibv_free_device_list(dev_list);
        return 1;
    }
    std::cout << "Created Queue Pair: qp_num=" << qp->qp_num << std::endl;

    // Transition QP to INIT state
    ibv_qp_attr qp_attr{};
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.pkey_index = 0;
    qp_attr.port_num = 1;
    qp_attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

    if (ibv_modify_qp(qp, &qp_attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS) != 0) {
        std::cerr << "Failed to transition QP to INIT" << std::endl;
    } else {
        std::cout << "QP transitioned to INIT state" << std::endl;
    }

    // Post a receive work request. Should trigger hooks
    ibv_sge recv_sge{};
    recv_sge.addr = reinterpret_cast<uint64_t>(buf);
    recv_sge.length = buf_size;
    recv_sge.lkey = mr->lkey;

    ibv_recv_wr recv_wr{};
    recv_wr.wr_id = 1;
    recv_wr.sg_list = &recv_sge;
    recv_wr.num_sge = 1;
    recv_wr.next = nullptr;

    {
        ibv_recv_wr* bad_recv_wr = nullptr;
        int ret = ibv_post_recv(qp, &recv_wr, &bad_recv_wr);
        if (ret != 0) {
            std::cerr << "ibv_post_recv failed: " << ret << std::endl;
        } else {
            std::cout << "Posted receive WR" << std::endl;
        }
    }

    // Post a send work request. Should trigger hooks
    // Note: This will fail since QP is not in RTS state (need RTR->RTS transition)
    // but it still demonstrates the hook being called
    ibv_sge send_sge{};
    send_sge.addr = reinterpret_cast<uint64_t>(buf);
    send_sge.length = message.size();
    send_sge.lkey = mr->lkey;

    ibv_send_wr send_wr{};
    send_wr.wr_id = 2;
    send_wr.sg_list = &send_sge;
    send_wr.num_sge = 1;
    send_wr.next = nullptr;
    send_wr.opcode = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;

    {
        ibv_send_wr* bad_send_wr = nullptr;
        int ret = ibv_post_send(qp, &send_wr, &bad_send_wr);
        if (ret != 0) {
            std::cerr << "ibv_post_send failed: " << ret << " (expected - QP not in RTS state)" << std::endl;
        } else {
            std::cout << "Posted send WR" << std::endl;
        }
    }

    // Poll CQ. Should trigger hooks
    ibv_wc wc[16];
    int num_completions = ibv_poll_cq(cq, 16, wc);
    std::cout << "Polled CQ: " << num_completions << " completions" << std::endl;

    // Cleanup
    std::cout << "\nCleaning up resources...\n";
    ibv_destroy_qp(qp);
    ibv_dereg_mr(mr);
    free(buf);
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);

    // Uninstall hooks and print summary
    // SampleHooks::instance().print_summary();
    SampleHooks::instance().uninstall(ctx);

    ibv_close_device(ctx);
    ibv_free_device_list(dev_list);
    return 0;
}
