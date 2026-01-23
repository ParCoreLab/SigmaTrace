#pragma once

#include <cstdint>

#include "infiniband/verbs.h"

#include "Common.hpp"
#include "EventTrait.hpp"

// TODO: compile these into an .so or something so future readers can reliable read back maybe

namespace Profiler::Events {
    struct alignas(8) IbvOpenDevice {
        uint64_t context_addr;
        char name[64]{};
        char dev_name[64]{};
        char dev_path[256]{};
        char ibdev_path[256]{};

        ibv_node_type node_type;
        ibv_transport_type transport_type;
    };

    struct alignas(8) IbvCloseDevice {
        uint64_t context_addr;
    };
} // namespace Profiler::Events

// clang-format off
DEFINE_EVENT_TRAIT(Profiler::Events::IbvOpenDevice, Events::EventTypeId::IbvOpenDevice, Events::EventCategory::NIC, "ibv_open_device");
DEFINE_EVENT_TRAIT(Profiler::Events::IbvCloseDevice, Events::EventTypeId::IbvCloseDevice, Events::EventCategory::NIC, "ibv_close_device");
// clang-format on
