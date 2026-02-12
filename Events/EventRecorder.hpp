#pragma once

#include <utility>

#include "EventRecorderConfig.hpp"
#include "EventStorage.hpp"
#include "EventTrait.hpp"
#include "ThreadLocalBuffer.hpp"

namespace Events {
    // Main event recorder interface
    // Thread-safe, lock-free recording via per-thread buffers
    // Template parameter allows compile-time configuration of timestamp source and other settings
    template<CompileTimeConfig Config = DefaultCompileTimeConfig>
    class EventRecorder {
        using TS = Config::TimestampSourceType;

    public:
        /*
         * TODO: maybe this shouldn't be a singleton so we
         * can have multiple recorders with different configs
         * and timestamp sources?
         * Maybe it should be a unique_ptr even
         * This is some really weird code
         */
        static EventRecorder& instance(RuntimeConfig&& config = RuntimeConfig{}) {
            static EventRecorder recorder(config);
            return recorder;
        }

        // Non-copyable
        EventRecorder(const EventRecorder&) = delete;
        EventRecorder& operator=(const EventRecorder&) = delete;

        // ================================================================
        // Recording and Saving
        // ===============================================================

        // Primary API: Record an event (automatic timestamp)
        // Usage: recorder.record(MyEvent{ .field1 = value1, ... });
        // TODO: move maybe
        // TODO: const T& doesn't make sense for how the function is used
        // to initialize directly instead of doing a memcpy later
        // TODO: this might be suboptimal if upstream user creates a new tread every time
        template<HasEventTrait EventT>
        void Record(const EventT& event, Timestamp ts_start, Timestamp ts_end = {}) {
            using BareT = BareType_t<EventT>;
            auto& event_thread_buffer = ThreadBufferRegistry::get_or_create<BareT>(m_runtime_config);
            event_thread_buffer.record(event, ts_start, ts_end);
        }

        // Records a one-off with only start timestamp
        template<HasEventTrait EventT>
        void Record(const EventT& event) {
            const Timestamp ts = m_timestamp_source.now();
            return Record(event, ts);
        }

        // TODO: repetition
        template<HasEventTrait EventT>
        auto GetSlot() {
            using BareT = BareType_t<EventT>;
            const Timestamp ts = m_timestamp_source.now();
            auto& event_thread_buffer = ThreadBufferRegistry::get_or_create<BareT>(m_runtime_config);
            return event_thread_buffer.get_wrapped_slot(ts);
        }

        // Convenience: Record with in-place construction
        // Usage: recorder.emplace<MyEvent>(arg1, arg2);
        template<HasEventTrait EventT, typename... Args>
        void Emplace(Args&&... args) {
            using BareT = BareType_t<EventT>;
            BareT event{std::forward<Args>(args)...};
            return Record(event);
        }

        // Force writes to file. TODO: may not be thread safe even though there's a buffer inside
        // void Flush() {
        //     ThreadBufferRegistry::instance().for_each_thread([](const ThreadBuffers& buffers) {
        //         buffers.for_each_buffer([](EventTypeId, IEventBuffer& buf) {
        //             buf.flush();
        //         });
        //     });
        // }
        // ================================================================
        // ================================================================
        // Accessors
        [[nodiscard]] const TS& timestamp_source() const {
            return m_timestamp_source;
        }

        [[nodiscard]] const RuntimeConfig& runtime_config() const {
            return m_runtime_config;
        }

    private:
        // Construct with custom runtime configuration
        explicit EventRecorder(RuntimeConfig config = RuntimeConfig{}) : m_runtime_config(std::move(config)) {
            m_runtime_config.ensure_output_directory();
        }

        TS m_timestamp_source{};
        RuntimeConfig m_runtime_config{};
    };

    // Convenience aliases for common configurations
    using DefaultEventRecorder = EventRecorder<DefaultCompileTimeConfig>;
    using PortableEventRecorder = EventRecorder<PortableCompileTimeConfig>;
    using DebugEventRecorder = EventRecorder<DebugCompileTimeConfig>;
} // namespace Events
