#pragma once

#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "EventRecorderConfig.hpp"
#include "EventTrait.hpp"
#include "ThreadLocalRingBuffer.hpp"

// TODO: MAYBE consider using https://github.com/cameron314/concurrentqueue/blob/master/concurrentqueue.h

namespace Events {
    // TODO: too low
    static constexpr size_t DefaultCapacity = 128;

    // Per-thread collection of event buffers (one per event type)
    class ThreadBufferRegistry {
    public:
        ThreadBufferRegistry() = delete;

        template<HasEventTrait EventT, size_t Capacity = DefaultCapacity>
        static auto& get_or_create(const RuntimeConfig& config) noexcept {
            thread_local auto buffer{EventRingBuffer<EventT, Capacity>(config)};
            return buffer;
        }
    };
} // namespace Events
