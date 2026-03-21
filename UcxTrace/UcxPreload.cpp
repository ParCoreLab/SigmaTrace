#include "UcxEventSource.hpp"

// ================================================================
namespace ucx {
    Profiler::Profiler() {}
} // namespace ucx
// ================================================================

// Accessor for late-loaded libraries to get the singleton instance via dlsym
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-declarations"
extern "C" ucx::Profiler* sigma_provider_instance() {
    return &ucx::Profiler::instance();
}
#pragma GCC diagnostic pop

// ================================================================
// Lifecycle
// ================================================================
extern "C" ucs_status_t ucp_init_version(unsigned api_major_version, unsigned api_minor_version,
                                         const ucp_params_t* params, const ucp_config_t* config,
                                         ucp_context_h* context_p) {
    return ucx::Profiler::invoke_preload<ucx::ucp_init_version_tag>(api_major_version, api_minor_version, params,
                                                                    config, context_p);
}

extern "C" void ucp_cleanup(ucp_context_h context) {
    /* return */ ucx::Profiler::invoke_preload<ucx::ucp_cleanup_tag>(context);
}

extern "C" ucs_status_t ucp_worker_create(ucp_context_h context, const ucp_worker_params_t* params,
                                          ucp_worker_h* worker_p) {
    return ucx::Profiler::invoke_preload<ucx::ucp_worker_create_tag>(context, params, worker_p);
}

extern "C" void ucp_worker_destroy(ucp_worker_h worker) {
    /* return */ ucx::Profiler::invoke_preload<ucx::ucp_worker_destroy_tag>(worker);
}

extern "C" ucs_status_t ucp_worker_get_address(ucp_worker_h worker, ucp_address_t** address_p,
                                               size_t* address_length_p) {
    return ucx::Profiler::invoke_preload<ucx::ucp_worker_get_address_tag>(worker, address_p, address_length_p);
}

extern "C" ucs_status_t ucp_ep_create(ucp_worker_h worker, const ucp_ep_params_t* params, ucp_ep_h* ep_p) {
    return ucx::Profiler::invoke_preload<ucx::ucp_ep_create_tag>(worker, params, ep_p);
}

extern "C" ucs_status_ptr_t ucp_ep_close_nbx(ucp_ep_h ep, const ucp_request_param_t* param) {
    return ucx::Profiler::invoke_preload<ucx::ucp_ep_close_nbx_tag>(ep, param);
}

// ================================================================
// Tagged messages
// ================================================================
extern "C" ucs_status_ptr_t ucp_tag_send_nbx(ucp_ep_h ep, const void* buffer, size_t count, ucp_tag_t tag,
                                             const ucp_request_param_t* param) {
    return ucx::Profiler::invoke_preload<ucx::ucp_tag_send_nbx_tag>(ep, buffer, count, tag, param);
}

extern "C" ucs_status_ptr_t ucp_tag_recv_nbx(ucp_worker_h worker, void* buffer, size_t count, ucp_tag_t tag,
                                             ucp_tag_t tag_mask, const ucp_request_param_t* param) {
    return ucx::Profiler::invoke_preload<ucx::ucp_tag_recv_nbx_tag>(worker, buffer, count, tag, tag_mask, param);
}

// ================================================================
// RMA
// ================================================================
extern "C" ucs_status_ptr_t ucp_put_nbx(ucp_ep_h ep, const void* buffer, size_t count, uint64_t remote_addr,
                                        ucp_rkey_h rkey, const ucp_request_param_t* param) {
    return ucx::Profiler::invoke_preload<ucx::ucp_put_nbx_tag>(ep, buffer, count, remote_addr, rkey, param);
}

extern "C" ucs_status_ptr_t ucp_get_nbx(ucp_ep_h ep, void* buffer, size_t count, uint64_t remote_addr, ucp_rkey_h rkey,
                                        const ucp_request_param_t* param) {
    return ucx::Profiler::invoke_preload<ucx::ucp_get_nbx_tag>(ep, buffer, count, remote_addr, rkey, param);
}

// ================================================================
// Active messages
// ================================================================
extern "C" ucs_status_ptr_t ucp_am_send_nbx(ucp_ep_h ep, unsigned int id, const void* header, size_t header_length,
                                            const void* buffer, size_t count, const ucp_request_param_t* param) {
    return ucx::Profiler::invoke_preload<ucx::ucp_am_send_nbx_tag>(ep, id, header, header_length, buffer, count, param);
}

// ================================================================
// Memory
// ================================================================
extern "C" ucs_status_t ucp_mem_map(ucp_context_h context, const ucp_mem_map_params_t* params, ucp_mem_h* memh_p) {
    return ucx::Profiler::invoke_preload<ucx::ucp_mem_map_tag>(context, params, memh_p);
}

extern "C" ucs_status_t ucp_mem_unmap(ucp_context_h context, ucp_mem_h memh) {
    return ucx::Profiler::invoke_preload<ucx::ucp_mem_unmap_tag>(context, memh);
}

// ================================================================
// Progress
// ================================================================
extern "C" unsigned ucp_worker_progress(ucp_worker_h worker) {
    return ucx::Profiler::invoke_preload<ucx::ucp_worker_progress_tag>(worker);
}

// ================================================================
static void __attribute__((constructor)) init() {
    ucx::Profiler::instance();
}
// ================================================================
