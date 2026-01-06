#pragma once

#include "infiniband/verbs.h"

#include "HookTrait.hpp"

// ================================================================
// IBVerbs Hook Definitions using generic framework
// ================================================================

// ================================================================
// Object-ops hooks (ctx->ops.X style)
// These are installed by replacing function pointers in ibv_context->ops
// ================================================================
DEFINE_OBJECT_OPS_HOOK(ibv, post_send, "ibv_post_send", int, ibv_qp *, ibv_send_wr *, ibv_send_wr **);
DEFINE_OBJECT_OPS_HOOK(ibv, post_recv, "ibv_post_recv", int, ibv_qp *, ibv_recv_wr *, ibv_recv_wr **);
DEFINE_OBJECT_OPS_HOOK(ibv, poll_cq, "ibv_poll_cq", int, ibv_cq *, int, ibv_wc *);
DEFINE_OBJECT_OPS_HOOK(ibv, req_notify_cq, "ibv_req_notify_cq", int, ibv_cq *, int);
DEFINE_OBJECT_OPS_HOOK(ibv, post_srq_recv, "ibv_post_srq_recv", int, ibv_srq *, ibv_recv_wr *, ibv_recv_wr **);

// // ================================================================
// // Preload hooks (LD_PRELOAD / dlsym style)
// // These are installed via LD_PRELOAD interception
// // ================================================================
DEFINE_PRELOAD_HOOK(ibv, open_device, "ibv_open_device", ibv_context *, ibv_device *);
DEFINE_PRELOAD_HOOK(ibv, close_device, "ibv_close_device", int, ibv_context *);
DEFINE_PRELOAD_HOOK(ibv, reg_mr, "ibv_reg_mr", ibv_mr *, ibv_pd *, void *, size_t, int);
DEFINE_PRELOAD_HOOK(ibv, dereg_mr, "ibv_dereg_mr", int, ibv_mr *);
DEFINE_PRELOAD_HOOK(ibv, reg_mr_iova2, "ibv_reg_mr_iova2", ibv_mr *, ibv_pd *, void *, size_t, uint64_t, unsigned int);
DEFINE_PRELOAD_HOOK(ibv, reg_dmabuf_mr, "ibv_reg_dmabuf_mr", ibv_mr *, ibv_pd *, uint64_t, size_t, uint64_t, int, int);
DEFINE_PRELOAD_HOOK(ibv, alloc_pd, "ibv_alloc_pd", ibv_pd *, ibv_context *);
DEFINE_PRELOAD_HOOK(ibv, create_cq, "ibv_create_cq", ibv_cq *, ibv_context *, int, void *, ibv_comp_channel *, int);
DEFINE_PRELOAD_HOOK(ibv, create_qp, "ibv_create_qp", ibv_qp *, ibv_pd *, ibv_qp_init_attr *);
DEFINE_PRELOAD_HOOK(ibv, modify_qp, "ibv_modify_qp", int, ibv_qp *, ibv_qp_attr *, int);
DEFINE_PRELOAD_HOOK(ibv, create_ah, "ibv_create_ah", ibv_ah *, ibv_pd *, ibv_ah_attr *);

// ================================================================
// Convenience namespace for all ibverbs tags
// ================================================================
namespace ibv {
    // Re-export for convenience
    // using namespace ibv;

    // using ObjectOpsTags = std::tuple<post_send_tag>;

    // All object-ops tags
    // using ObjectOpsTags = std::tuple<post_send_tag, post_recv_tag, poll_cq_tag, req_notify_cq_tag,
    // post_srq_recv_tag>;

    // // All preload tags
    // using PreloadTags = std::tuple<open_device_tag, close_device_tag, reg_mr_tag, reg_mr_iova2_tag, dereg_mr_tag,
    //                                reg_dmabuf_mr_tag>;
} // namespace ibv
