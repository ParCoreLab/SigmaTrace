#pragma once

#include <cstdint>

#include "EventRecorderConfig.hpp"
#include "EventStorage.hpp"
#include "EventTrait.hpp"
#include "FileIO/File.hpp"

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
        explicit ThreadLocalRingBuffer(auto &&header, const std::string &file_path) :
            m_current(0), m_file_buffer(std::forward<decltype(header)>(header), file_path) {}

        ~ThreadLocalRingBuffer() = default;

        // Non-copyable, TODO: maybe movable
        ThreadLocalRingBuffer(const ThreadLocalRingBuffer &) = delete;
        ThreadLocalRingBuffer &operator=(const ThreadLocalRingBuffer &) = delete;
        // ThreadLocalRingBuffer(ThreadLocalRingBuffer &&) = delete;
        // ThreadLocalRingBuffer &operator=(ThreadLocalRingBuffer &&) = delete;

        // For automatic commit on destruction
        // This class is copyable and owns a pointer
        // This is some really ugly code :(
        class SlotPtrWrapper {
            friend class ThreadLocalRingBuffer;

        public:
            SlotPtrWrapper(ThreadLocalRingBuffer *base_buffer, uint8_t *ptr) : m_ptr(ptr), m_base_buffer(base_buffer) {}

            // Can't copy
            SlotPtrWrapper(SlotPtrWrapper &) = delete;

            // Can move
            SlotPtrWrapper(SlotPtrWrapper &&other) noexcept : m_ptr(other.m_ptr), m_base_buffer(other.m_base_buffer) {
                other.m_ptr = nullptr;
                other.m_base_buffer = nullptr;
            }

            ~SlotPtrWrapper() {
                // Moved, no commit
                if (m_base_buffer) {
                    m_base_buffer->commit();
                }
            }

            uint8_t *get() const { return m_ptr; }
            uint8_t *&get_internal_ptr() { return m_ptr; }

        private:
            uint8_t *m_ptr;
            ThreadLocalRingBuffer *m_base_buffer;
        };

        // Get slot for next element - file buffer manages its own indexing
        uint8_t *get_slot() noexcept { return m_file_buffer.get_slot(); }

        SlotPtrWrapper get_wrapped_slot() noexcept { return SlotPtrWrapper{this, get_slot()}; }

        // TODO: overflow?
        void commit() noexcept {
            m_current++;
            m_file_buffer.commit();
        }

        void flush() { m_file_buffer.flush(); }

        auto current_idx() const noexcept { return m_current; }

        // static constexpr auto size() noexcept { return Size; }
        static constexpr auto capacity() noexcept { return Capacity; }

    private:
        // Cache-line aligned to avoid false sharing between producer and consumer
        // alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> m_head;
        // alignas(CACHE_LINE_SIZE) std::atomic<uint64_t> m_tail;
        // uint8_t *m_buffer;

        // TODO: is alignas necessary?
        alignas(CACHE_LINE_SIZE) uint64_t m_current;

        // Stream is faster in most cases
        // FileIO::MmapBuffer<ElementSize, Capacity> m_file_buffer;
        FileIO::StreamBuffer<EventFileHeader, ElementSize, Capacity> m_file_buffer;
    };

    // Type-safe wrapper for specific event types
    // Provides convenient record/read API with automatic timestamping
    template<typename EventT, size_t Capacity>
    class EventRingBuffer {
        static_assert(HasEventTrait<EventT>, "EventT must have EventTrait specialization");

        // Element size includes header + event data, padded to 8 bytes
        static constexpr size_t ElementSize = StoredEvent<EventT>::padded_size;

        using Buffer = ThreadLocalRingBuffer<ElementSize, Capacity>;

        // Same as SlotPtrWrapper but typed...
        class TypedSlotPtrWrapper {
            friend class EventRingBuffer;

        public:
            EventHeader *get_header() {
                return reinterpret_cast<EventHeader *>(static_cast<uint8_t *>(m_base_wrapper.get()));
            }

            EventT *get() {
                return reinterpret_cast<EventT *>(static_cast<uint8_t *>(m_base_wrapper.get()) + sizeof(EventHeader));
            }

        private:
            explicit TypedSlotPtrWrapper(Buffer::SlotPtrWrapper &&base_wrapper) :
                m_base_wrapper(std::move(base_wrapper)) {}
            Buffer::SlotPtrWrapper m_base_wrapper;
        };

    public:
        explicit EventRingBuffer(const RuntimeConfig &config) :
            m_event_header{std::move(config.get_event_file_header<EventT>())},
            m_buffer(m_event_header, config.get_event_file_path<EventT>()) {}

        // TODO: switch to span
        EventT *get_slot(const Timestamp ts_start, const Timestamp ts_end) noexcept {
            if (m_buffer.current_idx() >= m_buffer.capacity()) {
                m_dropped_count++;
            }

            auto *slot = m_buffer.get_slot();

            // Write header
            auto *header = reinterpret_cast<EventHeader *>(slot);

            // EventHeader has defaults. We construct with them and copy
            *header = EventHeader(ts_start, ts_end);

            // Write event data after header
            // TODO: is this ok?
            // TODO: maybe we could just move
            auto *data = reinterpret_cast<EventT *>(static_cast<uint8_t *>(slot) + sizeof(EventHeader));

            return data;
        }

        TypedSlotPtrWrapper get_wrapped_slot(const Timestamp ts_start, const Timestamp ts_end) noexcept {
            TypedSlotPtrWrapper wrapped_slot{m_buffer.get_wrapped_slot()};

            // Write header
            auto *header = wrapped_slot.get_header();

            // EventHeader has defaults. We construct with them and copy
            *header = EventHeader(ts_start, ts_end);

            return wrapped_slot;
        }

        // Record an event with provided timestamp
        // TODO: should probably return true or false
        void record(const EventT &event, const Timestamp ts_start, const Timestamp ts_end) noexcept {
            auto *slot = get_slot(ts_start, ts_end);

            EventTrait<EventT>::SerializeTo(event, slot);

            m_buffer.commit();
        }

        void flush() { m_buffer.flush(); }

        auto dropped_count() const noexcept { return m_dropped_count; }

        // static constexpr auto size() noexcept { return Buffer::size(); }
        static constexpr auto capacity() noexcept { return Capacity; }

    private:
        EventFileHeader m_event_header;

        Buffer m_buffer;
        size_t m_dropped_count{0};
    };

} // namespace Events
