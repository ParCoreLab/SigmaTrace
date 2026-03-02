#pragma once

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_rma.h>

#include "HookTrait.hpp"

// ================================================================
// libfabric Hook Definitions
//
// Most libfabric API functions are static inline wrappers that call through
// ops tables. Only fi_fabric and fi_getinfo are real exported symbols.
//
// Hook strategy:
// 1. Preload hooks: fi_fabric, fi_getinfo (real exported symbols)
// 2. Object-ops hooks on fid_ep: data-path operations (sendmsg, recvmsg, etc.)
//
// Lifecycle operations (fi_domain, fi_endpoint, fi_close, fi_enable, fi_mr_reg,
// fi_av_insert) are static inline in the headers. They call through ops tables
// on different parent objects (fid_fabric, fid_domain, fid, etc.) and would
// require separate object type handling. They are hooked via ops table
// replacement on the parent object in post-callbacks.
// ================================================================

// ================================================================
// Preload hooks (real exported symbols)
// ================================================================
// clang-format off
DEFINE_PRELOAD_HOOK(fi, fi_fabric, "fi_fabric", int, struct fi_fabric_attr*, struct fid_fabric**, void*);
DEFINE_PRELOAD_HOOK(fi, fi_getinfo, "fi_getinfo", int, uint32_t, const char*, const char*, uint64_t, const struct fi_info*, struct fi_info**);

// ================================================================
// Object-ops hooks on fid_ep (data-path through ops tables)
// These capture all higher-level wrappers:
//   fi_send, fi_sendv, fi_sendmsg, fi_inject -> ep->msg->sendmsg
//   fi_recv, fi_recvv, fi_recvmsg            -> ep->msg->recvmsg
//   fi_read, fi_readv, fi_readmsg            -> ep->rma->readmsg
//   fi_write, fi_writev, fi_writemsg         -> ep->rma->writemsg
// ================================================================
DEFINE_OBJECT_OPS_HOOK(fi, ep_msg_sendmsg, "fi_sendmsg", ssize_t, struct fid_ep*, const struct fi_msg*, uint64_t);
DEFINE_OBJECT_OPS_HOOK(fi, ep_msg_recvmsg, "fi_recvmsg", ssize_t, struct fid_ep*, const struct fi_msg*, uint64_t);
DEFINE_OBJECT_OPS_HOOK(fi, ep_rma_readmsg, "fi_readmsg", ssize_t, struct fid_ep*, const struct fi_msg_rma*, uint64_t);
DEFINE_OBJECT_OPS_HOOK(fi, ep_rma_writemsg, "fi_writemsg", ssize_t, struct fid_ep*, const struct fi_msg_rma*, uint64_t);
// clang-format on
