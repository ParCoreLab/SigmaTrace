#pragma once

#include <cstdio>

#include <iostream>

#include "../../Events/IbvEvents.hpp"

#include "Dlsym.hpp"

/*
 * Query NIC info and record on a device open
 *
 * We use context addr + node_guid to uniquely identify NICs in a given node
 * This function is meant to be called on ibv_device_open or mx5dv_device_open
 *
 * It first calls ibv_query_device to get node_guid alongside other attributes
 * If NIC data is already saved, it constructs the IbvOpenDevice event nd returns early
 * Otherwise, it queries each port and saves info.
 *
 * It turns out some applications call ibv_open_device a lot... so we don't want to
 * fetch and record all the data here unnecessarily
 *
 * Note the dynamic symbol resolution: we want this function to be callable from
 * both ibv and mx5dv contexts with any combination of providers loaded. It's possible
 * that the user disables ibv but enables mlx5dv to get a reduced set of events, meaning
 * ibv symbols won't be loaded globally. That being said, the current implementation is jank
 * and should be in a symbol table in a helper module, not this mega function.
 *
 * node_guid is unique per HCA in the node
 *
 * Node (server)
 * ├── HCA 0 (mlx5_0)
 * │   ├── node_guid: 0x1234...      (per-HCA)
 * │   ├── sys_image_guid: 0xABCD... (can span HCAs)
 * │   ├── Port 1
 * │   │   ├── LID: 15               (assigned by SM)
 * │   │   ├── GID[0]: fe80::1234    (link-local, derived from port_guid)
 * │   │   ├── GID[1]: ff02::5678    (if RoCE: IP-based)
 * │   │   └── state: ACTIVE
 * │   └── Port 2
 * │       ├── LID: 16               (different LID)
 * │       ├── GID[0]: fe80::1235
 * │       └── state: ACTIVE
 * └── HCA 1 (mlx5_1)
 *     ├── node_guid: 0x5678...      (different HCA)
 *     ├── sys_image_guid: 0xABCD... (same system)
 *     └── Port 1
 *     ├── LID: 23
 *     └── GID[0]: fe80::5678
 */
static Profiler::Events::IbvOpenDevice register_nic(Events::EventRecorder<>* event_recorder, ibv_context* ctx, ibv_device* device) {
    using fn_query_port_t = int (*)(ibv_context* context, uint8_t port_num, ibv_port_attr* port_attr);

    static auto* libibverbs_handle = smart_dlsym::real_dlopen("libibverbs.so", smart_dlsym::DL_RTLD_NOW);
    if (!libibverbs_handle) {
        std::cerr << "Could not find dlopen libibverbs.so" << std::endl;
        std::abort();
    }

    static auto* fn_query_device = reinterpret_cast<decltype(ibv_query_device)*>(
            smart_dlsym::real_dlsym(libibverbs_handle, "ibv_query_device"));

    static auto* fn_query_port = reinterpret_cast<fn_query_port_t>(
            smart_dlsym::real_dlvsym(libibverbs_handle, "ibv_query_port", "IBVERBS_1.1"));

    static auto* fn_query_gid =
            reinterpret_cast<decltype(ibv_query_gid)*>(smart_dlsym::real_dlsym(libibverbs_handle, "ibv_query_gid"));

    if (!fn_query_device || !fn_query_port || !fn_query_gid) {
        std::cerr << "Could not find ibv_query function symbols" << std::endl;
        std::abort();
    }

    ibv_device_attr attrs{};
    const auto res_device = fn_query_device(ctx, &attrs);
    if (res_device != 0) {
        std::cerr << "Failed to query device of ctx " << ctx << std::endl;
        std::abort();
    }

    const Profiler::Events::IbvOpenDevice event{
            .context_addr = PtrToUint(ctx),
            .node_guid = attrs.node_guid,
    };

    // TODO: this is probably not a good way to check.
    // A better approach is waiting for the user to call the functions below
    // And only query ourselves before Profiler exit if not all NICs get queried
    static std::unordered_set<uint64_t> fetched_nics{};
    if (fetched_nics.contains(attrs.node_guid)) {
        return event;
    }
    fetched_nics.emplace(attrs.node_guid);

    const uint8_t num_ports{attrs.phys_port_cnt};
    for (uint8_t p = 1; p <= num_ports; p++) {
        ibv_port_attr port_attrs{};
        const auto res_port = fn_query_port(ctx, p, &port_attrs);
        if (res_port != 0) {
            std::cerr << "Failed to query port " << p << " of ctx " << ctx << std::endl;
            std::abort();
        }

        Profiler::Events::IbvPortRegistration port_event{
                .port_num = p,
                .lid = port_attrs.lid,
                .state = port_attrs.state,
                .link_layer = port_attrs.link_layer,
                .gid = {},
        };

        // GID index 0 is typically the default (RoCEv2: index 1+ may be different)
        const auto res_gid = fn_query_gid(ctx, p, 0, &port_event.gid);
        if (res_gid != 0) {
            std::cerr << "Failed to query gid of port " << p << " of ctx " << ctx << std::endl;
            std::abort();
        }

        event_recorder->Record(port_event, 1);
    }

    // This isn't traced and should precede other ibv events
    Profiler::Events::IbvDeviceRegistration device_registration_event{
            .context_addr = PtrToUint(ctx),
            .node_guid = attrs.node_guid,
            .sys_image_guid = attrs.sys_image_guid,
            .node_type = device->node_type,
            .transport_type = device->transport_type,
    };

    snprintf(device_registration_event.name, sizeof device->name, "%s", device->name);
    snprintf(device_registration_event.dev_name, sizeof device->dev_name, "%s", device->dev_name);
    snprintf(device_registration_event.dev_path, sizeof device->dev_path, "%s", device->dev_path);
    snprintf(device_registration_event.ibdev_path, sizeof device->ibdev_path, "%s", device->ibdev_path);

    event_recorder->Record(device_registration_event, 0);

    return event;
}