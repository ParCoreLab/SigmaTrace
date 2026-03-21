#pragma once

#include <variant>

#include <ucp/api/ucp.h>

#include "HookTrait.hpp"

// ================================================================
// Lifecycle hooks
// ================================================================
// clang-format off
DEFINE_PRELOAD_HOOK(ucx, ucp_init_version, "ucp_init_version", ucs_status_t, unsigned, unsigned, const ucp_params_t*, const ucp_config_t*, ucp_context_h*);
DEFINE_PRELOAD_HOOK(ucx, ucp_cleanup, "ucp_cleanup", std::monostate, ucp_context_h);
DEFINE_PRELOAD_HOOK(ucx, ucp_worker_create, "ucp_worker_create", ucs_status_t, ucp_context_h, const ucp_worker_params_t*, ucp_worker_h*);
DEFINE_PRELOAD_HOOK(ucx, ucp_worker_destroy, "ucp_worker_destroy", std::monostate, ucp_worker_h);
DEFINE_PRELOAD_HOOK(ucx, ucp_worker_get_address, "ucp_worker_get_address", ucs_status_t, ucp_worker_h, ucp_address_t**, size_t*);
DEFINE_PRELOAD_HOOK(ucx, ucp_ep_create, "ucp_ep_create", ucs_status_t, ucp_worker_h, const ucp_ep_params_t*, ucp_ep_h*);
DEFINE_PRELOAD_HOOK(ucx, ucp_ep_close_nbx, "ucp_ep_close_nbx", ucs_status_ptr_t, ucp_ep_h, const ucp_request_param_t*);

// ================================================================
// Tagged message hooks
// ================================================================
DEFINE_PRELOAD_HOOK(ucx, ucp_tag_send_nbx, "ucp_tag_send_nbx", ucs_status_ptr_t, ucp_ep_h, const void*, size_t, ucp_tag_t, const ucp_request_param_t*);
DEFINE_PRELOAD_HOOK(ucx, ucp_tag_recv_nbx, "ucp_tag_recv_nbx", ucs_status_ptr_t, ucp_worker_h, void*, size_t, ucp_tag_t, ucp_tag_t, const ucp_request_param_t*);

// ================================================================
// RMA hooks
// ================================================================
DEFINE_PRELOAD_HOOK(ucx, ucp_put_nbx, "ucp_put_nbx", ucs_status_ptr_t, ucp_ep_h, const void*, size_t, uint64_t, ucp_rkey_h, const ucp_request_param_t*);
DEFINE_PRELOAD_HOOK(ucx, ucp_get_nbx, "ucp_get_nbx", ucs_status_ptr_t, ucp_ep_h, void*, size_t, uint64_t, ucp_rkey_h, const ucp_request_param_t*);

// ================================================================
// Active message hook
// ================================================================
DEFINE_PRELOAD_HOOK(ucx, ucp_am_send_nbx, "ucp_am_send_nbx", ucs_status_ptr_t, ucp_ep_h, unsigned int, const void*, size_t, const void*, size_t, const ucp_request_param_t*);

// ================================================================
// Memory hooks
// ================================================================
DEFINE_PRELOAD_HOOK(ucx, ucp_mem_map, "ucp_mem_map", ucs_status_t, ucp_context_h, const ucp_mem_map_params_t*, ucp_mem_h*);
DEFINE_PRELOAD_HOOK(ucx, ucp_mem_unmap, "ucp_mem_unmap", ucs_status_t, ucp_context_h, ucp_mem_h);

// ================================================================
// Progress hook
// ================================================================
// DEFINE_PRELOAD_HOOK(ucx, ucp_worker_progress, "ucp_worker_progress", unsigned, ucp_worker_h);
DEFINE_PRELOAD_HOOK_PRED(ucx, ucp_worker_progress, "ucp_worker_progress", [](unsigned num_progressed, ucp_worker_h) {
    // Most polls won't have anything
    if (num_progressed == 1) [[likely]] {
        return false;
    }

    return true;
}, unsigned, ucp_worker_h);
// // clang-format on
