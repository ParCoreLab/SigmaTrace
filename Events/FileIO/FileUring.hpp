#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <thread>

#include <cstdint>
#include <fcntl.h>
#include <liburing.h>

#include <unistd.h>

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
template<size_t ElementSize, size_t Capacity, uint8_t NumBuffers = 3>
class FileBackedBufferUring {

    static constexpr auto BufferSize = ElementSize * Capacity;

    struct WriteRequest {
        uint8_t buffer_index;
        size_t size;
    };

public:
    explicit FileBackedBufferUring(const std::string& file_path) {
        // Open file with O_DIRECT for best io_uring performance (optional, requires aligned buffers)
        // Using standard mode for compatibility
        m_fd = open(file_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (m_fd < 0) {
            throw std::runtime_error("Failed to open file: " + file_path);
        }

        // Initialize io_uring with queue depth
        if (io_uring_queue_init(32, &m_ring, 0) < 0) {
            close(m_fd);
            throw std::runtime_error("Failed to initialize io_uring");
        }

        // Start writer thread
        m_running = true;
        m_writer_thread = std::thread(&FileBackedBufferUring::writer_thread_func, this);
    }

    // Non-copyable
    FileBackedBufferUring(const FileBackedBufferUring&) = delete;
    FileBackedBufferUring& operator=(const FileBackedBufferUring&) = delete;

    ~FileBackedBufferUring() {
        flush();

        // Signal shutdown
        {
            std::lock_guard lock(m_queue_mutex);
            m_running = false;
        }
        m_queue_cv.notify_one();

        // Wait for writer thread to finish
        if (m_writer_thread.joinable()) {
            m_writer_thread.join();
        }

        // Cleanup io_uring and file
        io_uring_queue_exit(&m_ring);
        close(m_fd);
    }

    // Fast access to memory - returns pointer to next slot in current buffer
    uint8_t* get_slot() {
        return m_buffers[m_current_buffer_index] + m_current_buffer_size;
    }

    // Commit the current slot - minimal latency operation
    void commit() {
        m_current_buffer_size += ElementSize;

        // If buffer still has space, return immediately
        if (m_current_buffer_size < BufferSize) {
            return;
        }

        // Wait for a buffer to become available if all are in use
        // This prevents overwriting buffers that haven't been written yet
        m_available_buffers.acquire();

        // Buffer is full - queue it for async write
        submit_buffer(m_current_buffer_index, BufferSize);

        // Move to next buffer
        m_current_buffer_size = 0;
        m_current_buffer_index = (m_current_buffer_index + 1) % NumBuffers;
    }

    // Flush any partial buffer
    void flush() {
        if (m_current_buffer_size > 0) {
            submit_buffer(m_current_buffer_index, m_current_buffer_size);
            m_current_buffer_index = (m_current_buffer_index + 1) % NumBuffers;
            m_current_buffer_size = 0;
        }

        // Wait for all pending writes to complete
        wait_for_completion();
    }

private:
    // Aligned buffers for optimal I/O performance
    alignas(4096) uint8_t m_buffers[NumBuffers][BufferSize]{};

    // Current buffer being filled by producer
    uint8_t m_current_buffer_index{0};
    size_t m_current_buffer_size{0};

    // File descriptor and io_uring
    int m_fd;
    struct io_uring m_ring;
    std::atomic<uint64_t> m_file_offset{0};

    // Writer thread coordination
    std::thread m_writer_thread;
    std::atomic<bool> m_running{false};

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

    // Submit buffer for writing (called by producer thread)
    void submit_buffer(uint8_t buffer_index, size_t size) {
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

                // Check if shutting down
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
                // Queue full, submit and retry
                io_uring_submit(&m_ring);
                sqe = io_uring_get_sqe(&m_ring);
            }

            // Calculate offset for this write
            uint64_t offset = m_file_offset.fetch_add(req.size);

            // Prepare write operation
            io_uring_prep_write(sqe, m_fd, m_buffers[req.buffer_index], req.size, offset);
            io_uring_sqe_set_data(sqe, reinterpret_cast<void*>(static_cast<uintptr_t>(req.buffer_index)));

            // Track in-flight operation
            m_in_flight.fetch_add(1);

            // Submit to kernel
            io_uring_submit(&m_ring);

            // Process completions (non-blocking)
            process_completions();
        }

        // Final cleanup - wait for all I/O to complete
        while (m_in_flight.load() > 0) {
            process_completions();
            if (m_in_flight.load() > 0) {
                // Use condition variable to avoid busy waiting
                std::unique_lock lock(m_completion_mutex);
                m_completion_cv.wait_for(lock, std::chrono::milliseconds(1), [this] {
                    return m_in_flight.load() == 0;
                });
            }
        }
    }

    // Process io_uring completions
    void process_completions() {
        struct io_uring_cqe* cqe;
        unsigned head;
        unsigned count = 0;

        io_uring_for_each_cqe(&m_ring, head, cqe) {
            // Check for errors
            if (cqe->res < 0) {
                // Handle write error (could throw or log)
                // For now, just note it happened
            }

            count++;
        }

        if (count > 0) {
            io_uring_cq_advance(&m_ring, count);

            int prev = m_in_flight.fetch_sub(count);

            // Release semaphore for each completed write
            for (unsigned i = 0; i < count; i++) {
                m_available_buffers.release();
            }

            if (prev <= static_cast<int>(count)) {
                // All operations completed
                std::lock_guard lock(m_completion_mutex);
                m_completion_cv.notify_all();
            }
        }
    }

    // Wait for all pending writes to complete
    void wait_for_completion() {
        std::unique_lock lock(m_completion_mutex);
        m_completion_cv.wait(lock, [this] {
            return m_in_flight.load() == 0;
        });
    }
};
