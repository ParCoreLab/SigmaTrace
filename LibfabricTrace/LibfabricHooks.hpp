#pragma once

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_rma.h>
#include <rdma/fi_tagged.h>

#include "HookTrait.hpp"

// ================================================================
// libfabric Hook Definitions
//
// Each libfabric API function maps to a separate function pointer in the
// endpoint's ops tables. We hook every entry point so that no variant
// (send, sendv, sendmsg, inject, senddata, injectdata, ...) is missed.
// ================================================================

// ================================================================
// Preload hooks
// ================================================================
// clang-format off
DEFINE_PRELOAD_HOOK(fi, fi_fabric, "fi_fabric", int, struct fi_fabric_attr*, struct fid_fabric**, void*);
DEFINE_PRELOAD_HOOK(fi, fi_getinfo, "fi_getinfo", int, uint32_t, const char*, const char*, uint64_t, const struct fi_info*, struct fi_info**);

// ================================================================
// Object-ops hooks on fid_ep: message ops (ep->msg)
// All 9 function pointers in fi_ops_msg (excluding size field)
// ================================================================
DEFINE_OBJECT_OPS_HOOK(fi, ep_msg_recv,       "fi_recv",       ssize_t, struct fid_ep*, void*, size_t, void*, fi_addr_t, void*);
DEFINE_OBJECT_OPS_HOOK(fi, ep_msg_recvv,      "fi_recvv",      ssize_t, struct fid_ep*, const struct iovec*, void**, size_t, fi_addr_t, void*);
DEFINE_OBJECT_OPS_HOOK(fi, ep_msg_recvmsg,    "fi_recvmsg",    ssize_t, struct fid_ep*, const struct fi_msg*, uint64_t);
DEFINE_OBJECT_OPS_HOOK(fi, ep_msg_send,       "fi_send",       ssize_t, struct fid_ep*, const void*, size_t, void*, fi_addr_t, void*);
DEFINE_OBJECT_OPS_HOOK(fi, ep_msg_sendv,      "fi_sendv",      ssize_t, struct fid_ep*, const struct iovec*, void**, size_t, fi_addr_t, void*);
DEFINE_OBJECT_OPS_HOOK(fi, ep_msg_sendmsg,    "fi_sendmsg",    ssize_t, struct fid_ep*, const struct fi_msg*, uint64_t);
DEFINE_OBJECT_OPS_HOOK(fi, ep_msg_inject,     "fi_inject",     ssize_t, struct fid_ep*, const void*, size_t, fi_addr_t);
DEFINE_OBJECT_OPS_HOOK(fi, ep_msg_senddata,   "fi_senddata",   ssize_t, struct fid_ep*, const void*, size_t, void*, uint64_t, fi_addr_t, void*);
DEFINE_OBJECT_OPS_HOOK(fi, ep_msg_injectdata, "fi_injectdata", ssize_t, struct fid_ep*, const void*, size_t, uint64_t, fi_addr_t);

// ================================================================
// Object-ops hooks on fid_ep: RMA ops (ep->rma)
// All 9 function pointers in fi_ops_rma (excluding size field)
// ================================================================
DEFINE_OBJECT_OPS_HOOK(fi, ep_rma_read,       "fi_read",          ssize_t, struct fid_ep*, void*, size_t, void*, fi_addr_t, uint64_t, uint64_t, void*);
DEFINE_OBJECT_OPS_HOOK(fi, ep_rma_readv,      "fi_readv",         ssize_t, struct fid_ep*, const struct iovec*, void**, size_t, fi_addr_t, uint64_t, uint64_t, void*);
DEFINE_OBJECT_OPS_HOOK(fi, ep_rma_readmsg,    "fi_readmsg",       ssize_t, struct fid_ep*, const struct fi_msg_rma*, uint64_t);
DEFINE_OBJECT_OPS_HOOK(fi, ep_rma_write,      "fi_write",         ssize_t, struct fid_ep*, const void*, size_t, void*, fi_addr_t, uint64_t, uint64_t, void*);
DEFINE_OBJECT_OPS_HOOK(fi, ep_rma_writev,     "fi_writev",        ssize_t, struct fid_ep*, const struct iovec*, void**, size_t, fi_addr_t, uint64_t, uint64_t, void*);
DEFINE_OBJECT_OPS_HOOK(fi, ep_rma_writemsg,   "fi_writemsg",      ssize_t, struct fid_ep*, const struct fi_msg_rma*, uint64_t);
DEFINE_OBJECT_OPS_HOOK(fi, ep_rma_inject,     "fi_inject_write",  ssize_t, struct fid_ep*, const void*, size_t, fi_addr_t, uint64_t, uint64_t);
DEFINE_OBJECT_OPS_HOOK(fi, ep_rma_writedata,  "fi_writedata",     ssize_t, struct fid_ep*, const void*, size_t, void*, uint64_t, fi_addr_t, uint64_t, uint64_t, void*);
DEFINE_OBJECT_OPS_HOOK(fi, ep_rma_injectdata, "fi_injectwritedata", ssize_t, struct fid_ep*, const void*, size_t, uint64_t, fi_addr_t, uint64_t, uint64_t);

// ================================================================
// Object-ops hooks on fid_ep: tagged ops (ep->tagged)
// All 9 function pointers in fi_ops_tagged (excluding size field)
// ================================================================
DEFINE_OBJECT_OPS_HOOK(fi, ep_tagged_recv,       "fi_trecv",       ssize_t, struct fid_ep*, void*, size_t, void*, fi_addr_t, uint64_t, uint64_t, void*);
DEFINE_OBJECT_OPS_HOOK(fi, ep_tagged_recvv,      "fi_trecvv",      ssize_t, struct fid_ep*, const struct iovec*, void**, size_t, fi_addr_t, uint64_t, uint64_t, void*);
DEFINE_OBJECT_OPS_HOOK(fi, ep_tagged_recvmsg,    "fi_trecvmsg",    ssize_t, struct fid_ep*, const struct fi_msg_tagged*, uint64_t);
DEFINE_OBJECT_OPS_HOOK(fi, ep_tagged_send,       "fi_tsend",       ssize_t, struct fid_ep*, const void*, size_t, void*, fi_addr_t, uint64_t, void*);
DEFINE_OBJECT_OPS_HOOK(fi, ep_tagged_sendv,      "fi_tsendv",      ssize_t, struct fid_ep*, const struct iovec*, void**, size_t, fi_addr_t, uint64_t, void*);
DEFINE_OBJECT_OPS_HOOK(fi, ep_tagged_sendmsg,    "fi_tsendmsg",    ssize_t, struct fid_ep*, const struct fi_msg_tagged*, uint64_t);
DEFINE_OBJECT_OPS_HOOK(fi, ep_tagged_inject,     "fi_tinject",     ssize_t, struct fid_ep*, const void*, size_t, fi_addr_t, uint64_t);
DEFINE_OBJECT_OPS_HOOK(fi, ep_tagged_senddata,   "fi_tsenddata",   ssize_t, struct fid_ep*, const void*, size_t, void*, uint64_t, fi_addr_t, uint64_t, void*);
DEFINE_OBJECT_OPS_HOOK(fi, ep_tagged_injectdata, "fi_tinjectdata", ssize_t, struct fid_ep*, const void*, size_t, uint64_t, fi_addr_t, uint64_t);
// clang-format on
