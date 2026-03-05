#include "LibfabricEventSource.hpp"

// ================================================================
namespace libfabric {
    Profiler::Profiler() {
        // Object-ops hooks (sendmsg, recvmsg, readmsg, writemsg) are installed
        // via install(ep) when the caller notifies us of a new endpoint.
        // Need to call install() after fi_enable succeeds?

        // TODO
        // this->register_post<fi::fi?>([](...) {
        //     if (ctx) {
        //         instance().install(ctx);
        //     }
        // });
    }
} // namespace libfabric

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

extern "C" int fi_getinfo(uint32_t version, const char* node, const char* service, uint64_t flags, const fi_info* hints,
                          fi_info** info) {
    return libfabric::Profiler::invoke_preload<fi::fi_getinfo_tag>(version, node, service, flags, hints, info);
}

// ================================================================
static void __attribute__((constructor)) init() {
    libfabric::Profiler::instance();
}
// ================================================================
