#include <cstring>

#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>

#include "LibfabricEventSource.hpp"

// ================================================================
// Per-endpoint private ops-table copies
//
// The CXI provider shares a single fi_ops_msg / fi_ops_rma / fi_ops_tagged
// table across all endpoints. Mutating those tables would corrupt every
// endpoint. Instead, after fi_enable we memcpy each table into a private
// heap allocation, hook the copies, and point ep->msg/rma/tagged at them.
// On close we restore the original pointers.
// ================================================================
namespace {
    struct PerEndpointOps {
        fi_ops_msg msg_copy;
        fi_ops_rma rma_copy;
        fi_ops_tagged tagged_copy;

        // Original provider-shared table pointers for restore on close
        fi_ops_msg* orig_msg;
        fi_ops_rma* orig_rma;
        fi_ops_tagged* orig_tagged;
    };

    std::mutex g_ops_mutex;

    // Original domain fn pointer per fabric
    using fi_domain_fn_t = int (*)(fid_fabric*, fi_info*, fid_domain**, void*);
    std::unordered_map<fid_fabric*, fi_domain_fn_t> g_orig_domain;

    // Original endpoint fn pointer per domain
    using fi_endpoint_fn_t = int (*)(fid_domain*, fi_info*, fid_ep**, void*);
    std::unordered_map<fid_domain*, fi_endpoint_fn_t> g_orig_endpoint;

    // Original av_open fn pointer per domain
    using fi_av_open_fn_t = int (*)(fid_domain*, fi_av_attr*, fid_av**, void*);
    std::unordered_map<fid_domain*, fi_av_open_fn_t> g_orig_av_open;

    // Original fid.ops functions shared across all CXI endpoints
    using fi_close_fn_t = int (*)(fid*);
    using fi_control_fn_t = int (*)(fid*, int, void*);
    using fi_bind_fn_t = int (*)(fid*, fid*, uint64_t);
    fi_close_fn_t g_orig_close = nullptr;
    fi_control_fn_t g_orig_control = nullptr;
    fi_bind_fn_t g_orig_bind = nullptr;

    // Original av->ops->insert shared across all AVs from same provider
    using fi_av_insert_fn_t = int (*)(fid_av*, const void*, size_t, fi_addr_t*, uint64_t, void*);
    fi_av_insert_fn_t g_orig_av_insert = nullptr;

    // Per-AV: fi_addr_t -> address hash mapping
    std::unordered_map<fid_av*, std::unordered_map<fi_addr_t, uint32_t>> g_av_addr_hashes;

    // Per-AV: address length (determined from first insert)
    std::unordered_map<fid_av*, size_t> g_av_addrlen;

    // Per-EP: bound AV pointer
    std::unordered_map<fid_ep*, fid_av*> g_ep_to_av;

    // Per-EP: own address hash (from fi_getname after fi_enable)
    std::unordered_map<fid_ep*, uint32_t> g_ep_own_hash;

    // Endpoints pending data-path hook installation (waiting for fi_enable)
    std::unordered_set<fid_ep*> g_pending_endpoints;

    // Per-endpoint private ops-table copies (keyed by endpoint pointer)
    std::unordered_map<fid_ep*, std::unique_ptr<PerEndpointOps>> g_ep_ops;

    // FNV-1a hash of raw address bytes → uint32_t
    uint32_t hash_addr(const void* addr, size_t len) {
        uint32_t h = 2166136261u;
        const auto* p = static_cast<const uint8_t*>(addr);
        for (size_t i = 0; i < len; i++) {
            h ^= p[i];
            h *= 16777619u;
        }
        return h;
    }
} // namespace

// ================================================================
// Forward declarations for hooked functions
// ================================================================
static int hooked_fi_domain(fid_fabric* fabric, fi_info* info, fid_domain** dom, void* context);
static int hooked_fi_endpoint(fid_domain* domain, fi_info* info, fid_ep** ep, void* context);
static int hooked_fi_av_open(fid_domain* domain, fi_av_attr* attr, fid_av** av, void* context);
static int hooked_fi_av_insert(fid_av* av, const void* addr, size_t count, fi_addr_t* fi_addr, uint64_t flags,
                               void* context);
static int hooked_fi_ep_bind(fid* ep_fid, fid* bfid, uint64_t flags);
static int hooked_fi_ep_control(fid* ep_fid, int command, void* arg);
static int hooked_fi_ep_close(fid* ep_fid);

// ================================================================
namespace libfabric {
    Profiler::Profiler() {
        this->register_post<fi::fi_fabric_tag>([](int ret, fi_fabric_attr*, fid_fabric** fabric, void*, auto, auto) {
            if (ret != 0 || !fabric || !*fabric) {
                return;
            }

            std::lock_guard lock(g_ops_mutex);
            auto* fab = *fabric;
            if (fab->ops && fab->ops->domain) {
                g_orig_domain[fab] = fab->ops->domain;
                fab->ops->domain = hooked_fi_domain;
            }
        });
    }
} // namespace libfabric

// ================================================================
// Hooked fi_domain: call original, then intercept domain->ops->endpoint and av_open
// ================================================================
static int hooked_fi_domain(fid_fabric* fabric, fi_info* info, fid_domain** dom, void* context) {
    fi_domain_fn_t orig;
    {
        std::lock_guard lock(g_ops_mutex);
        const auto it = g_orig_domain.find(fabric);
        if (it == g_orig_domain.end()) {
            return -FI_EINVAL;
        }
        orig = it->second;
    }

    const int ret = orig(fabric, info, dom, context);
    if (ret != 0 || !dom || !*dom) {
        return ret;
    }

    auto* domain = *dom;

    {
        std::lock_guard lock(g_ops_mutex);
        if (domain->ops) {
            if (domain->ops->endpoint) {
                g_orig_endpoint[domain] = domain->ops->endpoint;
                domain->ops->endpoint = hooked_fi_endpoint;
            }
            if (domain->ops->av_open) {
                g_orig_av_open[domain] = domain->ops->av_open;
                domain->ops->av_open = hooked_fi_av_open;
            }
        }
    }

    return ret;
}

// ================================================================
// Hooked fi_av_open: call original, then intercept av->ops->insert
// ================================================================
static int hooked_fi_av_open(fid_domain* domain, fi_av_attr* attr, fid_av** av, void* context) {
    fi_av_open_fn_t orig;
    {
        std::lock_guard lock(g_ops_mutex);
        auto it = g_orig_av_open.find(domain);
        if (it == g_orig_av_open.end()) {
            return -FI_EINVAL;
        }
        orig = it->second;
    }

    int ret = orig(domain, attr, av, context);
    if (ret != 0 || !av || !*av) {
        return ret;
    }

    auto* a = *av;
    {
        std::lock_guard lock(g_ops_mutex);
        if (a->ops && a->ops->insert) {
            if (!g_orig_av_insert) {
                g_orig_av_insert = a->ops->insert;
            }
            a->ops->insert = hooked_fi_av_insert;
        }
    }

    return ret;
}

// ================================================================
// Hooked fi_av_insert: capture fi_addr_t -> raw address hash mapping
// ================================================================
static int hooked_fi_av_insert(fid_av* av, const void* addr, size_t count, fi_addr_t* fi_addr, uint64_t flags,
                               void* context) {
    // Determine address length by probing with fi_av_lookup if not known yet
    size_t addrlen = 0;
    {
        std::lock_guard lock(g_ops_mutex);
        auto it = g_av_addrlen.find(av);
        if (it != g_av_addrlen.end()) {
            addrlen = it->second;
        }
    }

    int ret = g_orig_av_insert(av, addr, count, fi_addr, flags, context);

    if (ret > 0 && addr && fi_addr) {
        // If we don't know addrlen yet, probe with fi_av_lookup
        if (addrlen == 0 && av->ops && av->ops->lookup) {
            size_t probe_len = 0;
            av->ops->lookup(av, fi_addr[0], nullptr, &probe_len);
            if (probe_len > 0) {
                addrlen = probe_len;
            }
        }

        if (addrlen > 0) {
            std::lock_guard lock(g_ops_mutex);
            g_av_addrlen[av] = addrlen;
            auto& hashes = g_av_addr_hashes[av];
            const auto* raw = static_cast<const uint8_t*>(addr);
            for (int i = 0; i < ret && static_cast<size_t>(i) < count; i++) {
                if (fi_addr[i] != FI_ADDR_NOTAVAIL) {
                    hashes[fi_addr[i]] = hash_addr(raw + static_cast<size_t>(i) * addrlen, addrlen);
                }
            }
        }
    }

    return ret;
}

// ================================================================
// Hooked fi_ep_bind: capture AV binding to EP
// ================================================================
static int hooked_fi_ep_bind(struct fid* ep_fid, struct fid* bfid, uint64_t flags) {
    int ret = g_orig_bind ? g_orig_bind(ep_fid, bfid, flags) : -FI_EINVAL;

    if (ret == 0 && bfid && bfid->fclass == FI_CLASS_AV) {
        auto* ep = reinterpret_cast<fid_ep*>(ep_fid);
        auto* av = reinterpret_cast<fid_av*>(bfid);
        std::lock_guard lock(g_ops_mutex);
        g_ep_to_av[ep] = av;
    }

    return ret;
}

// ================================================================
// Hooked fi_endpoint: call original, hook control+close+bind, defer data-path hooks
// ================================================================
static int hooked_fi_endpoint(fid_domain* domain, fi_info* info, fid_ep** ep, void* context) {
    fi_endpoint_fn_t orig;
    {
        std::lock_guard lock(g_ops_mutex);
        auto it = g_orig_endpoint.find(domain);
        if (it == g_orig_endpoint.end()) {
            return -FI_EINVAL;
        }
        orig = it->second;
    }

    int ret = orig(domain, info, ep, context);
    if (ret != 0 || !ep || !*ep) {
        return ret;
    }

    auto* endpoint = *ep;

    // Save original fid.ops functions and hook control+close+bind.
    // Data-path hooks are deferred until fi_enable() because the provider
    // replaces the msg/rma/tagged ops tables during fi_enable.
    {
        std::lock_guard lock(g_ops_mutex);
        if (endpoint->fid.ops) {
            if (!g_orig_close && endpoint->fid.ops->close) {
                g_orig_close = endpoint->fid.ops->close;
            }
            if (!g_orig_control && endpoint->fid.ops->control) {
                g_orig_control = endpoint->fid.ops->control;
            }
            if (!g_orig_bind && endpoint->fid.ops->bind) {
                g_orig_bind = endpoint->fid.ops->bind;
            }
            endpoint->fid.ops->close = hooked_fi_ep_close;
            endpoint->fid.ops->control = hooked_fi_ep_control;
            endpoint->fid.ops->bind = hooked_fi_ep_bind;
        }
        g_pending_endpoints.insert(endpoint);
    }

    return ret;
}

// ================================================================
// Hooked fi_control: after fi_enable, create private ops-table copies,
// install hooks, and capture own address via fi_getname
// ================================================================
static int hooked_fi_ep_control(struct fid* ep_fid, int command, void* arg) {
    int ret = g_orig_control ? g_orig_control(ep_fid, command, arg) : -FI_EINVAL;

    // After successful FI_ENABLE, the provider has finalized the ops tables.
    // Now we can safely copy them and install hooks on the copies.
    if (command == FI_ENABLE && ret == 0) {
        auto* ep = reinterpret_cast<fid_ep*>(ep_fid);
        std::lock_guard lock(g_ops_mutex);
        auto it = g_pending_endpoints.find(ep);
        if (it != g_pending_endpoints.end()) {
            g_pending_endpoints.erase(it);

            // Create private per-endpoint ops-table copies
            auto per_ep = std::make_unique<PerEndpointOps>();

            // Save original shared-table pointers for restore on close
            per_ep->orig_msg = ep->msg;
            per_ep->orig_rma = ep->rma;
            per_ep->orig_tagged = ep->tagged;

            // Deep-copy the provider's ops tables
            if (ep->msg) {
                per_ep->msg_copy = *ep->msg;
            }
            if (ep->rma) {
                per_ep->rma_copy = *ep->rma;
            }
            if (ep->tagged) {
                per_ep->tagged_copy = *ep->tagged;
            }

            // Point the endpoint at our private copies
            if (ep->msg) {
                ep->msg = &per_ep->msg_copy;
            }
            if (ep->rma) {
                ep->rma = &per_ep->rma_copy;
            }
            if (ep->tagged) {
                ep->tagged = &per_ep->tagged_copy;
            }

            g_ep_ops[ep] = std::move(per_ep);

            // Now install() will hook our private copies, not the shared tables
            libfabric::Profiler::instance().install(ep);

            // Capture our own network address via fi_getname
            if (ep->cm && ep->cm->getname) {
                size_t addrlen = 0;
                ep->cm->getname(&ep->fid, nullptr, &addrlen);
                if (addrlen > 0) {
                    std::vector<uint8_t> addr_buf(addrlen);
                    if (ep->cm->getname(&ep->fid, addr_buf.data(), &addrlen) == 0) {
                        g_ep_own_hash[ep] = hash_addr(addr_buf.data(), addrlen);
                    }
                }
            }
        }
    }

    return ret;
}

// ================================================================
// Hooked fi_close for endpoints: restore original ops, uninstall hooks, call original
// ================================================================
static int hooked_fi_ep_close(struct fid* ep_fid) {
    auto* ep = reinterpret_cast<fid_ep*>(ep_fid);

    {
        std::lock_guard lock(g_ops_mutex);

        auto it = g_ep_ops.find(ep);
        if (it != g_ep_ops.end()) {
            // Uninstall hooks (restores original fn ptrs in our private copy — not strictly
            // needed since we're about to throw the copy away, but keeps bookkeeping clean)
            libfabric::Profiler::instance().uninstall(ep);

            // Restore original shared-table pointers so the provider sees its own tables
            ep->msg = it->second->orig_msg;
            ep->rma = it->second->orig_rma;
            ep->tagged = it->second->orig_tagged;

            g_ep_ops.erase(it);
        }
        g_pending_endpoints.erase(ep);
        // NOTE: Do NOT erase g_ep_to_av or g_ep_own_hash here.
        // emit_fi_summaries() needs them at shutdown, which runs after MPI_Finalize
        // has already closed all endpoints. The maps are small and live until exit.
    }

    if (g_orig_close) {
        return g_orig_close(ep_fid);
    }
    return -FI_EINVAL;
}

// ================================================================
// Public API: resolve fi_addr_t to address hash for a given endpoint
// Called from emit_fi_summaries at exit
// ================================================================
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-declarations"
extern "C" uint32_t sigma_fi_get_own_addr_hash(fid_ep* ep) {
    std::lock_guard lock(g_ops_mutex);
    auto it = g_ep_own_hash.find(ep);
    return it != g_ep_own_hash.end() ? it->second : 0;
}

extern "C" uint32_t sigma_fi_resolve_dest(fid_ep* ep, fi_addr_t dest_addr) {
    std::lock_guard lock(g_ops_mutex);
    auto av_it = g_ep_to_av.find(ep);
    if (av_it == g_ep_to_av.end())
        return 0;

    fid_av* av = av_it->second;
    auto hash_map_it = g_av_addr_hashes.find(av);
    if (hash_map_it == g_av_addr_hashes.end())
        return 0;

    auto addr_it = hash_map_it->second.find(dest_addr);
    if (addr_it != hash_map_it->second.end())
        return addr_it->second;

    // Fallback: try fi_av_lookup at runtime
    auto addrlen_it = g_av_addrlen.find(av);
    if (addrlen_it != g_av_addrlen.end() && addrlen_it->second > 0 && av->ops && av->ops->lookup) {
        std::vector<uint8_t> buf(addrlen_it->second);
        size_t len = addrlen_it->second;
        if (av->ops->lookup(av, dest_addr, buf.data(), &len) == 0) {
            uint32_t h = hash_addr(buf.data(), len);
            hash_map_it->second[dest_addr] = h;
            return h;
        }
    }

    return 0;
}
#pragma GCC diagnostic pop

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

// ================================================================
static void __attribute__((constructor)) init() {
    libfabric::Profiler::instance();
}
// ================================================================
