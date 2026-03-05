#pragma once

#include <functional>
#include <mutex>
#include <vector>

#include "EventRecorderConfig.hpp"
#include "EventTrait.hpp"

#include "ThreadLocalRingBuffer.hpp"

namespace Events {
    // TODO: too low
    static constexpr size_t DefaultCapacity = 128;

    // Per-thread collection of event buffers (one per event type)
    // Tracks all created buffers so they can be finalized explicitly
    class ThreadBufferRegistry {
        ~ThreadBufferRegistry() {
            finalize_all();
        }

    public:
        template<HasEventTrait EventT, size_t Capacity = DefaultCapacity>
        static auto& get_or_create(const RuntimeConfig& config) noexcept {
            struct ThreadControl {
                std::shared_ptr<EventRingBuffer<EventT, Capacity>> buffer;

                explicit ThreadControl(const RuntimeConfig& cfg) :
                    buffer(std::make_shared<EventRingBuffer<EventT, Capacity>>(cfg)) {
                    register_finalizer();
                }

                void register_finalizer() {
                    std::weak_ptr<EventRingBuffer<EventT, Capacity>> weak_buf = buffer;
                    std::scoped_lock lock(s_mutex);
                    s_finalizers.push_back([weak_buf]() {
                        if (auto shared_buf = weak_buf.lock()) {
                            shared_buf->finalize();
                        }
                    });
                }
            };

            thread_local ThreadControl control(config);

            return *control.buffer;
        }

        // Sometimes the destructors don't get called so we do this instead
        static void finalize_all() noexcept {
            std::lock_guard lock(s_mutex);
            for (auto& fn: s_finalizers) {
                fn();
            }
            s_finalizers.clear();
        }

        // Reinitialize safely on a fork
        static void reset_for_fork() noexcept {
            new (&s_mutex) std::mutex();
            s_finalizers.clear();
        }

    private:
        static inline std::mutex s_mutex;
        static inline std::vector<std::function<void()>> s_finalizers;
    };
} // namespace Events
