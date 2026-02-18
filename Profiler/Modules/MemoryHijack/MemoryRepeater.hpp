#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace Profiler::MemoryRepeater {
    void wait_mem(void* addr) noexcept;

    class MemoryRepeaterHandle {
    public:
        using DataType = std::uint64_t;
        using CallbackType = std::function<void(DataType new_val)>;

        explicit MemoryRepeaterHandle(DataType* hijack_addr, void* real_addr, CallbackType on_change) :
            m_hijack_addr(hijack_addr), m_real_addr(real_addr), m_on_change(std::move(on_change)) {}

        // Not safe to copy
        MemoryRepeaterHandle(const MemoryRepeaterHandle&) = delete;
        MemoryRepeaterHandle& operator=(const MemoryRepeaterHandle&) = delete;

        MemoryRepeaterHandle(MemoryRepeaterHandle&& other) = default;

        ~MemoryRepeaterHandle() {
            // Can be invalidated after move{
            if (m_signal) {
                signal_finish();
            }

            // TODO: probably not safe
            if (m_thread && m_thread->joinable()) {
                m_thread->join();
            }
        }

        void start() noexcept {
            m_signal = std::make_shared<std::atomic<bool>>(true);

            // DO NOT CAPTURE `this`! It will be invalidated after move
            // Copy or move values instead
            m_thread = std::make_unique<std::jthread>([hijack_addr = m_hijack_addr,        //
                                                       real_addr = m_real_addr,            //
                                                       on_change = std::move(m_on_change), //
                                                       is_alive_signal = m_signal          // Copy shared_ptr
            ]() {
                auto current_value = *hijack_addr;

                while (*is_alive_signal.get()) {
                    wait_mem(hijack_addr);

                    // Atomic load for safety
                    // This is possibly still not good! GPU-PCIe-NIC ordering is different from CPU
                    const DataType written_val = __atomic_load_n(hijack_addr, __ATOMIC_ACQUIRE);

                    if (written_val == current_value) {
                        // Spurious wakeup, ignore
                        continue;
                    }

                    // Call the callback
                    // TODO: maybe should be async
                    on_change(written_val);

                    if (!*is_alive_signal.get()) {
                        return;
                    }

                    // TODO: maybe coroutine
                    __atomic_store_n(static_cast<uint64_t*>(real_addr), written_val, __ATOMIC_RELEASE);
                    current_value = written_val;
                }
            });
        }

        void signal_finish() const noexcept {
            *m_signal.get() = false;
            m_thread->request_stop();
        }

    private:
        DataType* m_hijack_addr;
        void* m_real_addr;
        CallbackType m_on_change;

        std::shared_ptr<std::atomic<bool>> m_signal{nullptr};
        std::unique_ptr<std::jthread> m_thread{nullptr};
    };
} // namespace Profiler::MemoryRepeater
