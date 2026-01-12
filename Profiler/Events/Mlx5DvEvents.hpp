#pragma once

#include "EventTrait.hpp"

namespace Profiler::Events {
    // Packed struct for ibv_post_send events
    struct alignas(8) DoorbellRing {
        uint64_t addr;

    };
} // namespace Profiler::Events

DEFINE_EVENT_TRAIT(Profiler::Events::DoorbellRing, Events::EventTypeId::Mlx5dvDoorbellRing, "doorbell_ring");
