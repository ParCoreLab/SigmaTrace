#include <mutex>
#include <unordered_map>

#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>

#include "LibfabricEventSource.hpp"

// ================================================================
// Ops-table interception storage
// ================================================================
namespace {
    std::mutex g_ops_mutex;

    // Original domain fn pointer per fabric
    using fi_domain_fn_t = int (*)(fid_fabric*, fi_info*, fid_domain**, void*);
    std::unordered_map<fid_fabric*, fi_domain_fn_t> g_orig_domain;

    // Original endpoint fn pointer per domain
    using fi_endpoint_fn_t = int (*)(fid_domain*, fi_info*, fid_ep**, void*);
    std::unordered_map<fid_domain*, fi_endpoint_fn_t> g_orig_endpoint;

    // Original close fn pointer per ep
    using fi_close_fn_t = int (*)(fid*);
    std::unordered_map<fid_ep*, fi_close_fn_t> g_orig_ep_close;
} // namespace

// ================================================================
// Forward declarations for hooked functions
// ================================================================
static int hooked_fi_domain(fid_fabric* fabric, fi_info* info, fid_domain** dom, void* context);
static int hooked_fi_endpoint(fid_domain* domain, fi_info* info, fid_ep** ep, void* context);
static int hooked_fi_ep_close(fid* fid);

// ================================================================
namespace libfabric {
    Profiler::Profiler() {
        // After fi_fabric returns, intercept fabric->ops->domain
        // so we can chain down to fi_endpoint and install data-path hooks.
        this->register_post<fi::fi_fabric_tag>([](int ret, fi_fabric_attr*, fid_fabric** fabric, void*, auto, auto) {
            if (ret != 0 || !fabric || !*fabric) {
                return;
            }

            std::lock_guard lock(g_ops_mutex);
            auto* fab = *fabric;
            if (fab->ops && fab->ops->domain) {
                // Save original and replace
                g_orig_domain[fab] = fab->ops->domain;
                // Cast away const on ops table — libfabric ops tables are mutable by design
                fab->ops->domain = hooked_fi_domain;
            }
        });
    }
} // namespace libfabric

// ================================================================
// Hooked fi_domain: call original, then intercept domain->ops->endpoint
// ================================================================
static int hooked_fi_domain(fid_fabric* fabric, fi_info* info, fid_domain** dom, void* context) {
    fi_domain_fn_t orig;
    {
        std::lock_guard lock(g_ops_mutex);
        auto it = g_orig_domain.find(fabric);
        if (it == g_orig_domain.end())
            return -FI_EINVAL;
        orig = it->second;
    }

    int ret = orig(fabric, info, dom, context);
    if (ret != 0 || !dom || !*dom) {
        return ret;
    }

    auto* domain = *dom;

    {
        std::lock_guard lock(g_ops_mutex);
        if (domain->ops && domain->ops->endpoint) {
            g_orig_endpoint[domain] = domain->ops->endpoint;
            domain->ops->endpoint = hooked_fi_endpoint;
        }
    }

    return ret;
}

// ================================================================
// Hooked fi_endpoint: call original, install data-path hooks, intercept close
// ================================================================
static int hooked_fi_endpoint(fid_domain* domain, fi_info* info, fid_ep** ep, void* context) {
    fi_endpoint_fn_t orig;
    {
        std::lock_guard lock(g_ops_mutex);
        auto it = g_orig_endpoint.find(domain);
        if (it == g_orig_endpoint.end())
            return -FI_EINVAL;
        orig = it->second;
    }

    int ret = orig(domain, info, ep, context);
    if (ret != 0 || !ep || !*ep)
        return ret;

    auto* endpoint = *ep;

    // Install data-path hooks (sendmsg, recvmsg, readmsg, writemsg) on the new EP
    libfabric::Profiler::instance().install(endpoint);

    // Intercept ep->fid.ops->close to uninstall hooks on cleanup
    {
        std::lock_guard lock(g_ops_mutex);
        if (endpoint->fid.ops && endpoint->fid.ops->close) {
            g_orig_ep_close[endpoint] = endpoint->fid.ops->close;
            endpoint->fid.ops->close = hooked_fi_ep_close;
        }
    }

    return ret;
}

// ================================================================
// Hooked fi_close for endpoints: uninstall hooks, then call original
// ================================================================
static int hooked_fi_ep_close(fid* fid) {
    auto* ep = reinterpret_cast<fid_ep*>(fid);

    // Uninstall data-path hooks before closing
    libfabric::Profiler::instance().uninstall(ep);

    fi_close_fn_t orig;
    {
        std::lock_guard lock(g_ops_mutex);
        auto it = g_orig_ep_close.find(ep);
        if (it == g_orig_ep_close.end()) {
            // Fallback: just call the fid close directly
            return -FI_EINVAL;
        }
        orig = it->second;
        g_orig_ep_close.erase(it);
    }

    return orig(fid);
}

// ================================================================
// Explicit instantiation
template class libfabric::LibfabricEventSource<libfabric::Profiler>;

// Accessor for late-loaded libraries to get the singleton instance via dlsym
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-declarations"
extern "C" libfabric::Profiler* sigma_provider_instance() {
    return &libfabric::Profiler::instance();
}
#pragma GCC diagnostic pop

// ================================================================
// Preload hooks - only real exported symbols
// ================================================================
extern "C" int fi_fabric(fi_fabric_attr* attr, fid_fabric** fabric, void* context) {
    return libfabric::Profiler::invoke_preload<fi::fi_fabric_tag>(attr, fabric, context);
}

// This recurses infinitely and breaks
// Who knows why
// extern "C" int fi_getinfo(uint32_t version, const char* node, const char* service, uint64_t flags, const fi_info*
// hints,
//                           fi_info** info) {
//     return libfabric::Profiler::invoke_preload<fi::fi_getinfo_tag>(version, node, service, flags, hints, info);
// }

// ================================================================
static void __attribute__((constructor)) init() {
    libfabric::Profiler::instance();
}
// ================================================================
