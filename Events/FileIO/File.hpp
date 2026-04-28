#pragma once

#include <cstddef>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <queue>
#include <semaphore>
#include <string>
#include <thread>
#include <type_traits>
#include <variant>

#include "SerializableTrait.hpp"

/*
 * TODO:
 * - Reserve space in file
 * - Performance test
 * - Error handling/tolerance
 */
namespace FileIO {
    static constexpr std::size_t align_to_page(const std::size_t size) {
        constexpr std::size_t PAGE_SIZE = 4096;
        return (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    }

    template<Serializable FileHeader, std::size_t ElementSize, std::size_t Capacity, uint8_t NumBuffers = 3,
             bool BackgroundWrite = false>
    class StreamBackedBuffer {
        static constexpr auto BufferSize = ElementSize * Capacity;

        struct WriteRequest {
            std::size_t buffer_index;
            std::size_t size;
        };

    public:
        explicit StreamBackedBuffer(const FileHeader& header, const std::filesystem::path& file_path) {
#if defined(SIGMA_DEBUG)
            if (std::filesystem::exists(file_path)) {
                throw std::runtime_error("Trying to write into an existing file: " + file_path.string());
            }
#endif

            m_stream = std::ofstream(file_path, std::ios::out | std::ios::binary);
            m_stream.write(reinterpret_cast<const char*>(&header), sizeof(header));

            if constexpr (BackgroundWrite) {
                bg().m_running = true;
                bg().m_writer_thread = std::thread(&StreamBackedBuffer::writer_thread_func, this);
            }
        }

        // Non-copyable
        StreamBackedBuffer(const StreamBackedBuffer&) = delete;
        StreamBackedBuffer& operator=(const StreamBackedBuffer&) = delete;

        ~StreamBackedBuffer() {
            finalize();
        }

        [[nodiscard]] uint8_t* get_slot() noexcept {
            return m_buffers[m_current_buffer_index] + m_current_buffer_size;
        }

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

        void flush() noexcept {
            if (m_current_buffer_size > 0) {
                if constexpr (BackgroundWrite) {
                    submit_to_writer(m_current_buffer_index, m_current_buffer_size);
                    m_current_buffer_size = 0;
                    m_current_buffer_index = (m_current_buffer_index + 1) % NumBuffers;
                } else {
                    m_stream.write(reinterpret_cast<const char*>(m_buffers[m_current_buffer_index]),
                                   static_cast<std::streamsize>(m_current_buffer_size));
                    m_current_buffer_size = 0;
                }
            }

            if constexpr (BackgroundWrite) {
                // Wait for writer to drain
                wait_for_writer_idle();
            }
            m_stream.flush();
        }

        void finalize() noexcept {
            if (m_finalized)
                return;
            m_finalized = true;

            flush();

            if constexpr (BackgroundWrite) {
                {
                    std::lock_guard lock(bg().m_queue_mutex);
                    bg().m_running = false;
                }
                bg().m_queue_cv.notify_one();
                if (bg().m_writer_thread.joinable()) {
                    bg().m_writer_thread.join();
                }
            }

            m_stream.close();
        }

    private:
        uint8_t m_buffers[NumBuffers][BufferSize]{};
        std::size_t m_current_buffer_index{0};
        std::size_t m_current_buffer_size{0};
        std::ofstream m_stream;
        bool m_finalized{false};

        // Background write machinery — compiled out when BackgroundWrite = false
        struct BackgroundState {
            std::thread m_writer_thread;
            bool m_running{false};
            std::queue<WriteRequest> m_write_queue;
            std::mutex m_queue_mutex;
            std::condition_variable m_queue_cv;
            std::counting_semaphore<NumBuffers> m_available_buffers{NumBuffers - 1};
            std::mutex m_idle_mutex;
            std::condition_variable m_idle_cv;
        };

        // Only instantiate background state when needed
        [[no_unique_address]]
        std::conditional_t<BackgroundWrite, BackgroundState, std::monostate> m_bg{};

        // Accessors that only compile when BackgroundWrite = true
        auto& bg() {
            return m_bg;
        }

        void flush_current_buffer() noexcept {
            if constexpr (BackgroundWrite) {
                bg().m_available_buffers.acquire();
                submit_to_writer(m_current_buffer_index, BufferSize);
            } else {
                m_stream.write(reinterpret_cast<const char*>(m_buffers[m_current_buffer_index]),
                               static_cast<std::streamsize>(BufferSize));
            }
            m_current_buffer_size = 0;
            m_current_buffer_index = (m_current_buffer_index + 1) % NumBuffers;
        }

        // Background write helpers (only used when BackgroundWrite = true)
        void submit_to_writer(std::size_t buffer_index, std::size_t size) {
            std::lock_guard lock(bg().m_queue_mutex);
            bg().m_write_queue.push({buffer_index, size});
            bg().m_queue_cv.notify_one();
        }

        void wait_for_writer_idle() {
            std::unique_lock lock(bg().m_idle_mutex);
            bg().m_idle_cv.wait(lock, [this] {
                std::lock_guard qlock(bg().m_queue_mutex);
                return bg().m_write_queue.empty();
            });
        }

        void writer_thread_func() {
            while (true) {
                WriteRequest req;
                {
                    std::unique_lock lock(bg().m_queue_mutex);
                    bg().m_queue_cv.wait(lock, [this] {
                        return !bg().m_write_queue.empty() || !bg().m_running;
                    });

                    if (!bg().m_running && bg().m_write_queue.empty()) {
                        break;
                    }
                    if (bg().m_write_queue.empty()) {
                        continue;
                    }

                    req = bg().m_write_queue.front();
                    bg().m_write_queue.pop();
                }

                m_stream.write(reinterpret_cast<const char*>(m_buffers[req.buffer_index]),
                               static_cast<std::streamsize>(req.size));

                bg().m_available_buffers.release();

                // Notify idle waiters
                {
                    std::lock_guard lock(bg().m_idle_mutex);
                    bg().m_idle_cv.notify_all();
                }
            }
        }
    };

    // TODO: We can dynamically set Capacity based on ElementSize for better aligned access
    template<Serializable FileHeader, std::size_t ElementSize, std::size_t CapacityHint>
    class MmapBackedBuffer {
        static constexpr std::size_t FileHeaderSize = sizeof(FileHeader);
        static constexpr std::size_t HugePageSize = 2 * 1024 * 1024; // 2MB huge pages
        static constexpr std::size_t ChunkBytes =
                ((ElementSize * CapacityHint + HugePageSize - 1) / HugePageSize) * HugePageSize;

    public:
        explicit MmapBackedBuffer(const FileHeader& header, const std::filesystem::path& file_path) {
#if defined(SIGMA_DEBUG)
            if (std::filesystem::exists(file_path)) {
                throw std::runtime_error("Trying to write into an existing file: " + file_path.string());
            }
#endif

            m_fd = open(file_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
            if (m_fd < 0) {
                throw std::runtime_error("Failed to open file: " + file_path.string());
            }

            // Safeguard
            // TODO: this isn't tested enough
            static_assert(std::is_trivially_copyable_v<FileHeader>);

            // Pre-allocate initial chunk using fallocate (faster than ftruncate, no zeroing)
            m_mapped_size = align_to_page(ChunkBytes + FileHeaderSize);
            if (fallocate(m_fd, 0, 0, static_cast<off_t>(m_mapped_size)) < 0) {
                if (ftruncate(m_fd, static_cast<off_t>(m_mapped_size)) < 0) {
                    close(m_fd);
                    throw std::runtime_error("Failed to resize file");
                }
            }

            m_base = static_cast<uint8_t*>(mmap(nullptr, m_mapped_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0));
            if (m_base == MAP_FAILED) {
                close(m_fd);
                throw std::runtime_error("mmap failed");
            }

            std::memcpy(m_base, &header, FileHeaderSize);

            // Initialize running pointer and pre-computed limit
            m_next_slot = m_base + FileHeaderSize;
            m_limit = m_base + m_mapped_size - ElementSize;

            madvise(m_base, m_mapped_size, MADV_SEQUENTIAL);
#ifdef MADV_HUGEPAGE
            madvise(m_base, m_mapped_size, MADV_HUGEPAGE);
#endif
        }

        MmapBackedBuffer(const MmapBackedBuffer&) = delete;
        MmapBackedBuffer& operator=(const MmapBackedBuffer&) = delete;

        ~MmapBackedBuffer() {
            finalize();
        }

        void finalize() noexcept {
            if (m_base && m_base != MAP_FAILED) {
                const std::size_t used_bytes = static_cast<std::size_t>(m_next_slot - m_base);
                msync(m_base, used_bytes, MS_SYNC);
                munmap(m_base, m_mapped_size);
                if (m_fd >= 0) {
                    std::ignore = ftruncate(m_fd, static_cast<off_t>(used_bytes));
                }
                m_base = nullptr;
                m_next_slot = nullptr;
                m_limit = nullptr;
            }
            if (m_fd >= 0) {
                close(m_fd);
                m_fd = -1;
            }
        }

        [[nodiscard]] uint8_t* get_slot() const noexcept {
            return m_next_slot;
        }

        void commit() {
            m_next_slot += ElementSize;
            if (m_next_slot <= m_limit) [[likely]]
                return;
            grow();
        }

        // Reserve space for N events. Grows if needed. Returns pointer to first slot.
        [[nodiscard]] uint8_t* reserve(std::size_t count) {
            if (count == 0) [[unlikely]]
                return m_next_slot;
            uint8_t* last_needed = m_next_slot + (count - 1) * ElementSize;
            if (last_needed > m_limit) [[unlikely]] {
                grow_to_fit(count);
            }
            return m_next_slot; // must read AFTER potential grow (base may move)
        }

        // Commit N events at once (call after writing all N slots sequentially)
        void commit_n(std::size_t count) {
            m_next_slot += count * ElementSize;
            if (m_next_slot > m_limit) [[unlikely]] {
                grow();
            }
        }

        [[nodiscard]] std::size_t current_index() const noexcept {
            return (m_next_slot - m_base - FileHeaderSize) / ElementSize;
        }

        void flush() const {
            msync(m_base, static_cast<std::size_t>(m_next_slot - m_base), MS_ASYNC);
        }

    private:
        int m_fd{-1};
        uint8_t* m_base{nullptr};
        uint8_t* m_next_slot{nullptr};
        uint8_t* m_limit{nullptr};
        std::size_t m_mapped_size{0};

        __attribute__((noinline)) void grow() {
            grow_to_fit(1);
        }

        __attribute__((noinline)) void grow_to_fit(std::size_t count) {
            // Grow enough to fit 'count' more events beyond current position
            const std::size_t needed = static_cast<std::size_t>(m_next_slot - m_base) + count * ElementSize;
            std::size_t new_size = m_mapped_size;
            while (new_size < needed) {
                new_size = align_to_page(new_size + ChunkBytes);
            }

            // Grow file first
            if (fallocate(m_fd, 0, 0, static_cast<off_t>(new_size)) < 0) {
                if (ftruncate(m_fd, static_cast<off_t>(new_size)) < 0) {
                    throw std::runtime_error("Failed to resize file");
                }
            }

            // Save offset before remap (base may change)
            const std::size_t slot_offset = static_cast<std::size_t>(m_next_slot - m_base);

            // Extend mapping in-place using mremap. Linux-specific
            void* new_base = mremap(m_base, m_mapped_size, new_size, MREMAP_MAYMOVE);

            if (new_base == MAP_FAILED) {
                // We need to manually unmap and remap.
                // Some systems don't let you mremap for "security"

                // Flush existing changes to be safe before tearing down
                msync(m_base, m_mapped_size, MS_ASYNC);

                // Unmap the old region
                if (munmap(m_base, m_mapped_size) != 0) {
                    close(m_fd);
                    throw std::runtime_error("mremap failed, and fallback munmap failed");
                }

                // Map the new, larger region
                new_base = mmap(nullptr, new_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0);
                if (new_base == MAP_FAILED) {
                    close(m_fd);
                    throw std::runtime_error("mremap failed, and fallback mmap failed (ENOMEM)");
                }
            }

            m_base = static_cast<uint8_t*>(new_base);
            m_next_slot = m_base + slot_offset;
            m_limit = m_base + new_size - ElementSize;

            // Re-apply madvise to the NEW section of the file
            madvise(m_base + m_mapped_size, new_size - m_mapped_size, MADV_SEQUENTIAL);
#ifdef MADV_HUGEPAGE
            madvise(m_base + m_mapped_size, new_size - m_mapped_size, MADV_HUGEPAGE);
#endif
            m_mapped_size = new_size;
        }
    };

    template<Serializable FileHeader, std::size_t ElementSize, std::size_t Capacity>
    using StreamBuffer = StreamBackedBuffer<FileHeader, ElementSize, Capacity>;

    template<Serializable FileHeader, std::size_t ElementSize, std::size_t Capacity>
    using BackgroundStreamBuffer = StreamBackedBuffer<FileHeader, ElementSize, Capacity, 3, true>;

    template<typename FileHeader, std::size_t ElementSize, std::size_t Capacity>
    using MmapBuffer = MmapBackedBuffer<FileHeader, ElementSize, Capacity>;
} // namespace FileIO
