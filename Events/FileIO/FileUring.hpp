#pragma once

#include <cstdint>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <queue>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <thread>

#include <liburing.h>

#include "SerializableTrait.hpp"

/*
 * High-performance file-backed buffer using io_uring and a dedicated writer thread.
 *
 * Design:
 * - Producer thread: get_slot() -> commit() with minimal latency
 * - Writer thread: handles all disk I/O via io_uring asynchronously
 * - Triple buffering: one active, one queued for write, one being written
 *
 * Performance characteristics:
 * - commit() is O(1) with minimal locking (just queue push)
 * - Async I/O via io_uring maximizes throughput
 * - Writer thread prevents producer blocking on disk I/O
 */
namespace FileIO {

    template<Serializable FileHeader, std::size_t ElementSize, std::size_t Capacity, uint8_t NumBuffers = 3>
    class UringBackedBuffer {

        static constexpr auto BufferSize = ElementSize * Capacity;

        struct WriteRequest {
            uint8_t buffer_index;
            std::size_t size;
        };

    public:
        explicit UringBackedBuffer(const FileHeader& header, const std::filesystem::path& file_path) {
            m_fd = open(file_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (m_fd < 0) {
                throw std::runtime_error("Failed to open file: " + file_path.string());
            }

            // Initialize io_uring with queue depth
            if (io_uring_queue_init(32, &m_ring, 0) < 0) {
                close(m_fd);
                throw std::runtime_error("Failed to initialize io_uring");
            }

            // Write header synchronously (one-time cost)
            ::write(m_fd, &header, sizeof(FileHeader));
            m_file_offset = sizeof(FileHeader);

            // Start writer thread
            m_running = true;
            m_writer_thread = std::thread(&UringBackedBuffer::writer_thread_func, this);
        }

        // Non-copyable
        UringBackedBuffer(const UringBackedBuffer&) = delete;
        UringBackedBuffer& operator=(const UringBackedBuffer&) = delete;

        ~UringBackedBuffer() {
            finalize();
        }

        // Fast access to memory - returns pointer to next slot in current buffer
        [[nodiscard]] uint8_t* get_slot() noexcept {
            return m_buffers[m_current_buffer_index] + m_current_buffer_size;
        }

        // Commit the current slot - minimal latency operation
        void commit() noexcept {
            m_current_buffer_size += ElementSize;

            if (m_current_buffer_size < BufferSize) {
                return;
            }

            flush_current_buffer();
        }

        [[nodiscard]] uint8_t* reserve(std::size_t count) {
            if (m_current_buffer_size + count * ElementSize > BufferSize) {
                flush_current_buffer();
            }
            return m_buffers[m_current_buffer_index] + m_current_buffer_size;
        }

        void commit_n(std::size_t count) {
            m_current_buffer_size += count * ElementSize;
            if (m_current_buffer_size >= BufferSize) {
                flush_current_buffer();
            }
        }

        // Flush any partial buffer
        void flush() noexcept {
            if (m_current_buffer_size > 0) {
                submit_buffer(m_current_buffer_index, m_current_buffer_size);
                m_current_buffer_index = (m_current_buffer_index + 1) % NumBuffers;
                m_current_buffer_size = 0;
            }

            // Wait for all pending writes to complete
            wait_for_completion();
        }

        void finalize() noexcept {
            if (m_finalized)
                return;
            m_finalized = true;

            flush();

            // Signal shutdown
            {
                std::lock_guard lock(m_queue_mutex);
                m_running = false;
            }
            m_queue_cv.notify_one();

            if (m_writer_thread.joinable()) {
                m_writer_thread.join();
            }

            io_uring_queue_exit(&m_ring);
            if (m_fd >= 0) {
                close(m_fd);
                m_fd = -1;
            }
        }

    private:
        // Aligned buffers for optimal I/O performance
        alignas(4096) uint8_t m_buffers[NumBuffers][BufferSize]{};

        // Current buffer being filled by producer
        uint8_t m_current_buffer_index{0};
        std::size_t m_current_buffer_size{0};

        // File descriptor and io_uring
        int m_fd{-1};
        io_uring m_ring{};
        std::atomic<uint64_t> m_file_offset{0};

        // Writer thread coordination
        std::thread m_writer_thread;
        std::atomic<bool> m_running{false};
        bool m_finalized{false};

        // Queue of write requests
        std::queue<WriteRequest> m_write_queue;
        std::mutex m_queue_mutex;
        std::condition_variable m_queue_cv;

        // Track in-flight operations
        std::atomic<int> m_in_flight{0};
        std::mutex m_completion_mutex;
        std::condition_variable m_completion_cv;

        // Semaphore to prevent buffer overrun (keep 1 buffer always available for producer)
        std::counting_semaphore<NumBuffers> m_available_buffers{NumBuffers - 1};

        void flush_current_buffer() noexcept {
            // Wait for a buffer to become available if all are in use
            m_available_buffers.acquire();

            submit_buffer(m_current_buffer_index, BufferSize);

            m_current_buffer_size = 0;
            m_current_buffer_index = (m_current_buffer_index + 1) % NumBuffers;
        }

        // Submit buffer for writing (called by producer thread)
        void submit_buffer(uint8_t buffer_index, std::size_t size) {
            std::lock_guard lock(m_queue_mutex);
            m_write_queue.push({buffer_index, size});
            m_queue_cv.notify_one();
        }

        // Writer thread function
        void writer_thread_func() {
            while (true) {
                WriteRequest req;

                // Wait for work
                {
                    std::unique_lock lock(m_queue_mutex);
                    m_queue_cv.wait(lock, [this] {
                        return !m_write_queue.empty() || !m_running;
                    });

                    if (!m_running && m_write_queue.empty()) {
                        break;
                    }

                    if (m_write_queue.empty()) {
                        continue;
                    }

                    req = m_write_queue.front();
                    m_write_queue.pop();
                }

                // Submit write to io_uring
                struct io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
                if (!sqe) {
                    io_uring_submit(&m_ring);
                    sqe = io_uring_get_sqe(&m_ring);
                }

                uint64_t offset = m_file_offset.fetch_add(req.size);

                io_uring_prep_write(sqe, m_fd, m_buffers[req.buffer_index], req.size, offset);
                io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<uintptr_t>(req.buffer_index)));

                m_in_flight.fetch_add(1);
                io_uring_submit(&m_ring);

                process_completions();
            }

            // Final cleanup - wait for all I/O to complete
            while (m_in_flight.load() > 0) {
                process_completions();
                if (m_in_flight.load() > 0) {
                    std::unique_lock lock(m_completion_mutex);
                    m_completion_cv.wait_for(lock, std::chrono::milliseconds(1), [this] {
                        return m_in_flight.load() == 0;
                    });
                }
            }
        }

        void process_completions() {
            struct io_uring_cqe* cqe;
            unsigned head;
            unsigned count = 0;

            io_uring_for_each_cqe(&m_ring, head, cqe) {
                count++;
            }

            if (count > 0) {
                io_uring_cq_advance(&m_ring, count);

                int prev = m_in_flight.fetch_sub(count);

                for (unsigned i = 0; i < count; i++) {
                    m_available_buffers.release();
                }

                if (prev <= static_cast<int>(count)) {
                    std::lock_guard lock(m_completion_mutex);
                    m_completion_cv.notify_all();
                }
            }
        }

        void wait_for_completion() {
            std::unique_lock lock(m_completion_mutex);
            m_completion_cv.wait(lock, [this] {
                return m_in_flight.load() == 0;
            });
        }
    };

    template<Serializable FileHeader, std::size_t ElementSize, std::size_t Capacity>
    using UringBuffer = UringBackedBuffer<FileHeader, ElementSize, Capacity>;

} // namespace FileIO
