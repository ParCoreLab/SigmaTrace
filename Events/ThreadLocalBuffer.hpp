#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include "EventRecorderConfig.hpp"
#include "EventTrait.hpp"

#include "ThreadLocalRingBuffer.hpp"

namespace Events {
    // TODO: this is probably too big for some systems and will break
    // it's stack allocated too...
    static constexpr size_t DefaultCapacity = 4096;

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
                uint64_t generation;

                ThreadControl(const RuntimeConfig& cfg, uint64_t gen) :
                    buffer(std::make_shared<EventRingBuffer<EventT, Capacity>>(cfg)), generation(gen) {
                    std::scoped_lock lock(s_mutex);
                    s_finalizers.push_back([shared_buf = buffer]() {
                        shared_buf->finalize();
                    });
                }
            };

            // TLS destructors run before atexit handlers, so a non-pointer
            // thread_local variable here would observe has_value()==false
            // during profiler_finalize and create a fresh buffer that opens
            // and truncates the same file as the original, discarding pre-finalize
            // data. We need to use a raw pointer that survives until
            // _exit. The pointed-to ThreadControl is leaked, but the buffer it
            // owns stays alive via s_finalizers/s_graveyard and is flushed by
            // finalize_all. I'm not happy about the atomic but it is what it is.
            thread_local ThreadControl* control = nullptr;

            // Function-static thread_locals survive fork in the calling thread,
            // so without an explicit invalidation the child would re-use the
            // parent's buffer (parent PID in path, fd shared with parent). The
            // generation counter is bumped in reset_for_fork so the child
            // reconstructs on next access.
            const auto current_gen = s_fork_generation.load(std::memory_order_relaxed);
            if (control == nullptr || control->generation != current_gen) [[unlikely]] {
                control = new ThreadControl(config, current_gen);
            }

            return *control->buffer;
        }

        // Sometimes the destructors don't get called so we do this instead
        static void finalize_all() noexcept {
            std::lock_guard lock(s_mutex);
            for (auto& fn: s_finalizers) {
                fn();
            }
            s_finalizers.clear();
        }

        // Reinitialize safely on a fork. Park the finalizers (which own the
        // shared_ptr to each pre-fork EventRingBuffer) in a graveyard instead
        // of dropping them: running their destructors here would flush via
        // the inherited fd's shared open file description and clobber the
        // parent's files. Bumping the generation invalidates any thread_local
        // ThreadControl instances that survived fork (e.g. the calling
        // thread's), forcing them to rebuild against fresh files.
        static void reset_for_fork() noexcept {
            new (&s_mutex) std::mutex();
            for (auto& fn: s_finalizers) {
                s_graveyard.push_back(std::move(fn));
            }
            s_finalizers.clear();
            s_fork_generation.fetch_add(1, std::memory_order_relaxed);
        }

    private:
        static inline std::mutex s_mutex;
        static inline std::vector<std::function<void()>> s_finalizers;
        static inline std::vector<std::function<void()>> s_graveyard;
        static inline std::atomic<uint64_t> s_fork_generation{0};
    };
} // namespace Events
