#pragma once

#include <utility>

#include "EventStorage.hpp"
#include "EventTrait.hpp"
#include "ThreadLocalBuffer.hpp"

namespace Events {
    // Main event recorder interface
    // Thread-safe, lock-free recording via per-thread buffers
    class EventRecorder {
    public:
        /*
         * TODO: maybe this shouldn't be a singleton so we
         * can have multiple recorders with different configs
         * and timestamp sources?
         */
        static EventRecorder &instance() {
            static EventRecorder recorder;
            return recorder;
        }

        // Singleton
        EventRecorder() = default;

        // Non-copyable
        EventRecorder(const EventRecorder &) = delete;
        EventRecorder &operator=(const EventRecorder &) = delete;

        // ================================================================
        // Recording and Saving
        // ===============================================================

        // Primary API: Record an event (automatic timestamp)
        // Usage: recorder.record(MyEvent{ .field1 = value1, ... });
        // TODO: move maybe
        // TODO: option to return a ptr to allocated event in the buffer
        // to initialize directly instead of doing a memcpy later
        // TODO: this might be suboptimal if upstream user creates a new tread every time
        template<HasEventTrait EventT>
        void Record(const EventT &event) {
            using BareT = BareType_t<EventT>;
            const Timestamp ts = m_timestamp_source.now();
            auto &thread_buffers = ThreadBufferRegistry::instance().get_thread_buffers();
            auto &buffer = thread_buffers.get_or_create<BareT>();
            buffer.buffer().record(event, ts);
        }

        // Convenience: Record with in-place construction
        // Usage: recorder.emplace<MyEvent>(arg1, arg2);
        template<HasEventTrait EventT, typename... Args>
        void Emplace(Args &&...args) {
            using BareT = BareType_t<EventT>;
            BareT event{std::forward<Args>(args)...};
            return Record(event);
        }

        // Flush placeholder - can be extended for serialization
        void Flush() {
            // TODO: Implement flush to file/consumer when needed
        }
        // ================================================================

        // ================================================================
        // Statistics
        // ================================================================
        // Aggregate statistics
        struct Stats {
            size_t total_buffered; // Events currently in buffers
            size_t total_dropped;  // Events dropped due to full buffers
            size_t active_threads; // Threads with event buffers
        };

        static Stats get_stats() {
            Stats stats{0, 0, 0};
            ThreadBufferRegistry::instance().for_each_thread([&stats](const ThreadBuffers &buffers) {
                ++stats.active_threads;
                buffers.for_each_buffer([&stats](EventTypeId, const IEventBuffer &buf) {
                    stats.total_buffered += buf.size();
                    stats.total_dropped += buf.dropped_count();
                });
            });
            return stats;
        }

        // Per-event-type statistics
        struct EventTypeStats {
            EventTypeId id;
            const char *name;
            size_t buffered;
            size_t dropped;
            size_t capacity;
        };

        // Collect stats for a specific event type across all threads
        template<HasEventTrait EventT>
        [[nodiscard]] EventTypeStats get_event_stats() const {
            using BareT = BareType_t<EventT>;
            EventTypeStats stats{EventTrait<BareT>::id, EventTrait<BareT>::name, 0, 0, 0};

            const auto target_id = EventTrait<BareT>::id;
            ThreadBufferRegistry::instance().for_each_thread([&stats, target_id](const ThreadBuffers &buffers) {
                buffers.for_each_buffer([&stats, target_id](EventTypeId id, const IEventBuffer &buf) {
                    if (id == target_id) {
                        stats.buffered += buf.size();
                        stats.dropped += buf.dropped_count();
                        stats.capacity += buf.capacity();
                    }
                });
            });
            return stats;
        }
        // ================================================================

        // Accessors
        [[nodiscard]] TimestampSource &timestamp_source() { return m_timestamp_source; }

    private:
        TimestampSource m_timestamp_source;
    };
} // namespace Events
