#pragma once

#include <string_view>

#include "CompositeHookManager.hpp"

#include "UcxHooks.hpp"

namespace ucx {
    constexpr std::string_view LibPath{"libucp.so"};

    template<typename Derived>
    class UcxEventSource
        : public hook::CompositeHookManager<Derived, hook::DefaultHookManagerConfig, int, ucp_init_version_tag,
                                            ucp_cleanup_tag, ucp_worker_create_tag, ucp_worker_destroy_tag,
                                            ucp_worker_get_address_tag, ucp_ep_create_tag, ucp_ep_close_nbx_tag,
                                            ucp_tag_send_nbx_tag, ucp_tag_recv_nbx_tag, ucp_put_nbx_tag,
                                            ucp_get_nbx_tag, ucp_am_send_nbx_tag, ucp_mem_map_tag, ucp_mem_unmap_tag,
                                            ucp_worker_progress_tag> {

    public:
        using Base =
                hook::CompositeHookManager<Derived, hook::DefaultHookManagerConfig, int, ucp_init_version_tag,
                                           ucp_cleanup_tag, ucp_worker_create_tag, ucp_worker_destroy_tag,
                                           ucp_worker_get_address_tag, ucp_ep_create_tag, ucp_ep_close_nbx_tag,
                                           ucp_tag_send_nbx_tag, ucp_tag_recv_nbx_tag, ucp_put_nbx_tag,
                                           ucp_get_nbx_tag, ucp_am_send_nbx_tag, ucp_mem_map_tag, ucp_mem_unmap_tag,
                                           ucp_worker_progress_tag>;

        using Context = Base::Context;

        static void* GetLibHandle() noexcept {
            static auto* handle = smart_dlsym::real_dlopen(LibPath.data(), smart_dlsym::DL_RTLD_NOW);
            return handle;
        }

    protected:
        UcxEventSource() = default;
    };

    // ================================================================
    struct Profiler : UcxEventSource<Profiler> {
        Profiler();
    };

} // namespace ucx
