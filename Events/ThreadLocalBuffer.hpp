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
    static constexpr size_t DefaultCapacity = 128;
    // using DefaultCapacity = std::integral_constant<size_t, 16>;

    // Type-erased buffer interface for registry and iteration
    class IEventBuffer {
    public:
        virtual ~IEventBuffer() = default;
        [[nodiscard]] virtual EventTypeId type_id() const = 0;
        [[nodiscard]] virtual const char* type_name() const = 0;
        // [[nodiscard]] virtual size_t size() const = 0;
        [[nodiscard]] virtual size_t capacity() const = 0;
        [[nodiscard]] virtual size_t dropped_count() const = 0;

        virtual void flush() = 0;
    };

    template<typename EventT, size_t Capacity = DefaultCapacity>
    class TypedEventBuffer : public IEventBuffer {
        static_assert(HasEventTrait<EventT>, "EventT must have EventTrait specialization");

    public:
        explicit TypedEventBuffer(const RuntimeConfig& config) : m_buffer(config) {}

        [[nodiscard]] EventTypeId type_id() const noexcept override {
            return EventTrait<EventT>::id;
        }
        [[nodiscard]] const char* type_name() const noexcept override {
            return EventTrait<EventT>::name;
        }
        // [[nodiscard]] size_t size() const override { return m_buffer.size(); }
        [[nodiscard]] size_t capacity() const noexcept override {
            return Capacity;
        }
        [[nodiscard]] size_t dropped_count() const noexcept override {
            return m_buffer.dropped_count();
        }

        void flush() noexcept override {
            m_buffer.flush();
        }

        // Access underlying typed buffer
        EventRingBuffer<EventT, Capacity>& buffer() noexcept {
            return m_buffer;
        }
        const EventRingBuffer<EventT, Capacity>& buffer() const noexcept {
            return m_buffer;
        }

    private:
        EventRingBuffer<EventT, Capacity> m_buffer;
    };

    // Per-thread collection of event buffers (one per event type)
    class ThreadBuffers {
    public:
        ThreadBuffers() : m_thread_id(std::this_thread::get_id()) {}

        // Get or create a buffer for a specific event type
        // TODO: Don't hardcode 4096 here
        template<typename EventT, size_t Capacity = DefaultCapacity>
        TypedEventBuffer<EventT, Capacity>& get_or_create(const RuntimeConfig& config) {
            const auto key = EventTrait<EventT>::id;

            /*
             * TODO: Possible optimization:
             * I'm doing a map lookup here but Events already have a nice numeric IDs
             * that could be used as flat array indices. Right now event IDs are sparse,
             * but I could add a secondary normalized only for enabled events
             * in the type trait. I'm not sure it would be worth it however, both due to the
             * added complexity in the source code, and because we might needlessly create large
             * arrays.
             */
            auto it = m_buffers.find(key);
            if (it != m_buffers.end()) {
                return *static_cast<TypedEventBuffer<EventT, Capacity>*>(it->second.get());
            }

            // First time seeing this event type - create buffer
            // TODO: Do I really need to heap allocate it?
            auto buffer = std::make_unique<TypedEventBuffer<EventT, Capacity>>(config);
            auto* ptr = buffer.get();
            m_buffers[key] = std::move(buffer);
            return *ptr;
        }

        std::thread::id thread_id() const noexcept {
            return m_thread_id;
        }

        // Iterate all buffers (for stats collection, flushing)
        template<typename Func>
        void for_each_buffer(Func&& func) const {
            for (const auto& [id, buffer]: m_buffers) {
                func(static_cast<EventTypeId>(id), *buffer);
            }
        }

    private:
        std::thread::id m_thread_id;
        std::unordered_map<EventTypeId, std::unique_ptr<IEventBuffer>> m_buffers;
    };

    // Global registry of all thread buffers
    // Allows iteration over all threads for stats and flushing
    class ThreadBufferRegistry {
    public:
        static ThreadBufferRegistry& instance() noexcept {
            static ThreadBufferRegistry registry;
            return registry;
        }

        /*
        ~ThreadBufferRegistry() {
            // Mutex here to prevent thread from dying while we're still flushing
            // TODO: may be unnecessary
            std::lock_guard lock(m_mutex);
            std::cout << "ThreadBufferRegistry destroyed, " << m_all_buffers.size()
                      << " thread buffers were registered.\n";
        }
        */

        // Get or create thread-local buffers for the calling thread
        ThreadBuffers& get_thread_buffers() noexcept {
            thread_local ThreadBuffers* cached = nullptr;

            if (!cached) {
                // First call from this thread
                auto buffers = std::make_unique<ThreadBuffers>();
                cached = buffers.get();

                std::lock_guard lock(m_mutex);
                m_all_buffers.push_back(std::move(buffers));
            }

            return *cached;
        }

        // Iterate all thread buffers (for global stats, flushing)
        template<typename Func>
        void for_each_thread(Func&& func) const {
            std::lock_guard lock(m_mutex);
            for (const auto& buffers: m_all_buffers) {
                func(*buffers);
            }
        }

        template<typename Func>
        void for_each_thread(Func&& func) {
            std::lock_guard lock(m_mutex);
            for (auto& buffers: m_all_buffers) {
                func(*buffers);
            }
        }

        // Number of threads with registered buffers
        size_t thread_count() const noexcept {
            std::lock_guard lock(m_mutex);
            return m_all_buffers.size();
        }

    private:
        ThreadBufferRegistry() = default;

        mutable std::mutex m_mutex;
        std::vector<std::unique_ptr<ThreadBuffers>> m_all_buffers;
    };

} // namespace Events
