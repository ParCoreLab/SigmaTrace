#pragma once

#include <cstdint>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <mutex>
#include <string>

// TODO: consider using liburing for async file I/O
// Bypasses kernel for fast IO apparently
// #include <liburing.h>

/*
 * TODO:
 * - Reserve space in file
 * - Performance test
 * - Error handling/tolerance
 */

namespace FileIO {
    // TODO: rename this
    template<template<size_t ElementSize, size_t Capacity> class Impl, size_t ElementSize, size_t Capacity>
    class Buffer {
        using ImplType = Impl<ElementSize, Capacity>;

    public:
        explicit Buffer(auto &&...args) : m_impl(std::forward<decltype(args)>(args)...) {}

        [[nodiscard]] uint8_t *get_slot() { return m_impl.get_slot(); }
        void commit() { m_impl.commit(); }
        void flush() { m_impl.flush(); }

    private:
        ImplType m_impl;
    };

    template<size_t ElementSize, size_t Capacity, uint8_t NumBuffers = 3>
    class StreamBackedBuffer {
        static constexpr auto BufferSize = ElementSize * Capacity;

    public:
        explicit StreamBackedBuffer(const std::string &file_path) {
            m_stream = std::ofstream(file_path, std::ios::out | std::ios::binary);
        }

        // Non-copyable
        StreamBackedBuffer(const StreamBackedBuffer &) = delete;
        StreamBackedBuffer &operator=(const StreamBackedBuffer &) = delete;

        ~StreamBackedBuffer() { flush(); }

        // Fast access to memory
        // TODO: NOT SAFE AT ALL !!! but maybe that's the compromise
        [[nodiscard]] uint8_t *get_slot() { return m_buffers[m_current_buffer_index] + m_current_buffer_size; }

        void commit() {
            // Continue on the same buffer as usual
            m_current_buffer_size += ElementSize;
            if (m_current_buffer_size < BufferSize) {
                return;
            }

            /*
             * TODO: this is single threaded, but in most of my tests*, it outperformed any threaded versions
             * Said tests were synthetic - millions of messages back to back. Writer thread might be better
             * for a real world application that's more sensitive to latency
             */
            {
                std::lock_guard lock(m_stream_mutex);
                m_stream.write(reinterpret_cast<const char *>(m_buffers[m_current_buffer_index]), BufferSize);
            }

            m_current_buffer_size = 0;
            m_current_buffer_index = (m_current_buffer_index + 1) % NumBuffers;
        }

        void flush() {
            std::lock_guard lock(m_stream_mutex);

            m_stream.write(reinterpret_cast<const char *>(m_buffers[m_current_buffer_index]), m_current_buffer_size);
            m_stream.flush();

            // Reset size to continue using current buffer
            m_current_buffer_size = 0;
        }

    private:
        uint8_t m_buffers[NumBuffers][BufferSize]{};

        // Index of the buffer currently in use
        // TODO: maybe a different type
        size_t m_current_buffer_index{0};

        // Amount of data written to the current buffer
        size_t m_current_buffer_size{0};

        std::ofstream m_stream;
        std::mutex m_stream_mutex;

        void advance_buffer() { m_current_buffer_index = (m_current_buffer_index + 1) % NumBuffers; }
    };

    // TODO: We can dynamically set Capacity based on ElementSize for better aligned access
    template<size_t ElementSize, size_t CapacityHint>
    class MmapBackedBuffer {
        static constexpr size_t HugePageSize = 2 * 1024 * 1024; // 2MB huge pages
        static constexpr size_t ChunkBytes =
                ((ElementSize * CapacityHint + HugePageSize - 1) / HugePageSize) * HugePageSize;

    public:
        explicit MmapBackedBuffer(const std::string &file_path) {
            m_fd = open(file_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
            if (m_fd < 0) {
                throw std::runtime_error("Failed to open file: " + file_path);
            }

            // Pre-allocate initial chunk using fallocate (faster than ftruncate, no zeroing)
            m_mapped_size = ChunkBytes;
            if (fallocate(m_fd, 0, 0, m_mapped_size) < 0) {
                if (ftruncate(m_fd, m_mapped_size) < 0) {
                    close(m_fd);
                    throw std::runtime_error("Failed to resize file");
                }
            }

            m_base = static_cast<uint8_t *>(mmap(nullptr, m_mapped_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0));

            if (m_base == MAP_FAILED) {
                close(m_fd);
                throw std::runtime_error("mmap failed");
            }

            madvise(m_base, m_mapped_size, MADV_SEQUENTIAL);
#ifdef MADV_HUGEPAGE
            madvise(m_base, m_mapped_size, MADV_HUGEPAGE);
#endif
        }

        MmapBackedBuffer(const MmapBackedBuffer &) = delete;
        MmapBackedBuffer &operator=(const MmapBackedBuffer &) = delete;

        ~MmapBackedBuffer() {
            const size_t used_bytes = m_current_index * ElementSize;
            if (m_base && m_base != MAP_FAILED) {
                msync(m_base, used_bytes, MS_ASYNC);
                munmap(m_base, m_mapped_size);
            }
            if (m_fd >= 0) {
                std::ignore = ftruncate(m_fd, used_bytes);
                close(m_fd);
            }
        }

        [[nodiscard]] uint8_t *get_slot() const noexcept { return m_base + m_current_index * ElementSize; }

        void commit() {
            ++m_current_index;

            // Need space for the NEXT slot: current_index * ElementSize + ElementSize
            const size_t required_bytes = (m_current_index + 1) * ElementSize;
            if (required_bytes <= m_mapped_size) [[likely]] {
                return;
            }

            std::cout << "Resizing" << std::endl;

            // Grow file first
            const size_t new_size = m_mapped_size + ChunkBytes;
            if (fallocate(m_fd, 0, 0, new_size) < 0) {
                if (ftruncate(m_fd, new_size) < 0) {
                    throw std::runtime_error("Failed to resize file");
                }
            }

            // Extend mapping in-place using mremap. Linux-specific
            void *new_base = mremap(m_base, m_mapped_size, new_size, MREMAP_MAYMOVE);
            if (new_base == MAP_FAILED) {
                throw std::runtime_error("mremap failed");
            }

            m_base = static_cast<uint8_t *>(new_base);
            madvise(m_base + m_mapped_size, ChunkBytes, MADV_SEQUENTIAL);
#ifdef MADV_HUGEPAGE
            madvise(m_base + m_mapped_size, ChunkBytes, MADV_HUGEPAGE);
#endif
            m_mapped_size = new_size;
        }

        void flush() const { msync(m_base, m_current_index * ElementSize, MS_ASYNC); }

    private:
        int m_fd{-1};
        uint8_t *m_base{nullptr};
        size_t m_current_index{0};
        size_t m_mapped_size{0};
    };

    template<size_t ElementSize, size_t Capacity>
    using StreamBuffer = Buffer<StreamBackedBuffer, ElementSize, Capacity>;

    template<size_t ElementSize, size_t Capacity>
    using MmapBuffer = Buffer<MmapBackedBuffer, ElementSize, Capacity>;
} // namespace FileIO
