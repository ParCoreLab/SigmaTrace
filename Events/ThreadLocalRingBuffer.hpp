#pragma once

#include <cstdint>
#include <cstring>

#include "EventStorage.hpp"
#include "EventTrait.hpp"

namespace Events {

    // Cache line size for padding to avoid false sharing
    inline constexpr size_t CACHE_LINE_SIZE = 64;

    // Single thread only ring buffer
    // - Lock-free because we're assuming only one thread
    // - Power-of-2 capacity for efficient masking
    // - Cache-line aligned head/tail to avoid false sharing
    // - Will automatically overwrite without warning when full
    //
    // THIS IS NOT THREAD SAFE !!!!!!!
    //
    // TODO: IMPLEMENT DOUBLE BUFFERING FOR FLUSHING WITHOUT OVERWRITE
    template<size_t ElementSize, size_t Capacity>
    class ThreadLocalRingBuffer {
        static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2 for efficient masking");
        static_assert(ElementSize % 8 == 0, "ElementSize should be 8-byte aligned");

        static constexpr size_t Size = ElementSize * Capacity;

    public:
        ThreadLocalRingBuffer() : m_current(0), m_buffer{} {}

        ~ThreadLocalRingBuffer() {}

        // Non-copyable, TODO: maybe movable
        ThreadLocalRingBuffer(const ThreadLocalRingBuffer &) = delete;
        ThreadLocalRingBuffer &operator=(const ThreadLocalRingBuffer &) = delete;
        // ThreadLocalRingBuffer(ThreadLocalRingBuffer &&) = delete;
        // ThreadLocalRingBuffer &operator=(ThreadLocalRingBuffer &&) = delete;

        // TODO: why void* return?
        uint8_t *get_slot() noexcept {
            const size_t index = m_current & (Capacity - 1);
            return &m_buffer[index * ElementSize];
        }

        // TODO: overflow?
        void commit() noexcept { m_current++; };

        auto current_idx() const noexcept { return m_current; }

        static constexpr auto size() noexcept { return Size; }
        static constexpr auto capacity() noexcept { return Capacity; }

    private:
        // Cache-line aligned to avoid false sharing between producer and consumer
        // alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> m_head;
        // alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> m_tail;
        // uint8_t *m_buffer;

        // TODO: is alignas necessary?
        alignas(CACHE_LINE_SIZE) uint64_t m_current;
        uint8_t m_buffer[Size];
    };

    // Type-safe wrapper for specific event types
    // Provides convenient record/read API with automatic timestamping
    template<typename EventT, size_t Capacity = 4096>
    class EventRingBuffer {
        static_assert(HasEventTrait<EventT>, "EventT must have EventTrait specialization");

        // Element size includes header + event data, padded to 8 bytes
        static constexpr size_t ElementSize = StoredEvent<EventT>::padded_size;

        using Buffer = ThreadLocalRingBuffer<ElementSize, Capacity>;

    public:
        // Record an event with provided timestamp
        // TODO: should probably return true or false
        void record(const EventT &event, Timestamp ts) noexcept {
            if (m_buffer.current_idx() >= m_buffer.capacity()) {
                m_dropped_count++;
            }

            auto *slot = m_buffer.get_slot();

            // Write header
            auto *header = reinterpret_cast<EventHeader *>(slot);
            header->timestamp = ts;

            // Write event data after header
            // TODO: is this ok?
            // TODO: maybe we could just move
            auto *data = reinterpret_cast<EventT *>(static_cast<uint8_t *>(slot) + sizeof(EventHeader));
            std::memcpy(data, &event, sizeof(EventT));

            m_buffer.commit();
        }

        auto dropped_count() const noexcept { return m_dropped_count; }

        static constexpr auto size() noexcept { return Buffer::size(); }
        static constexpr auto capacity() noexcept { return Capacity; }

    private:
        Buffer m_buffer;
        size_t m_dropped_count{0};
    };

} // namespace Events
