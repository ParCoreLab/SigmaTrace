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

            if (m_internal_signal) {
                *m_internal_signal.get() = false;
            }

            // TODO: probably not safe
            if (m_thread && m_thread->joinable()) {
                m_thread->join();
            }
        }

        void start() noexcept {
            m_signal = std::make_shared<std::atomic<bool>>(true);
            m_internal_signal = std::make_shared<std::atomic<bool>>(true);

            // DO NOT CAPTURE `this`! It will be invalidated after move
            // Copy or move values instead
            m_thread = std::make_unique<std::jthread>([hijack_addr = m_hijack_addr,         //
                                                       real_addr = m_real_addr,             //
                                                       on_change = std::move(m_on_change),  //
                                                       internal_signal = m_internal_signal, // Copy shared_ptr
                                                       is_alive_signal = m_signal           // Copy shared_ptr
            ]() {
                auto current_value = *hijack_addr;

                while (*internal_signal.get()) {
                    wait_mem(hijack_addr);

                    // Atomic load for safety
                    // This is possibly still not good! GPU-PCIe-NIC ordering is different from CPU
                    const DataType written_val = __atomic_load_n(hijack_addr, __ATOMIC_ACQUIRE);

                    if (written_val == current_value) {
                        // Spurious wakeup, ignore
                        continue;
                    }

                    // Record/process before forwarding so we can copy WQEs
                    // from the SQ before the NIC consumes and the GPU recycles them.
                    // WE MIGHT NEED TO GET RID OF THE IF CONDITION AND ALWAYS CALL
                    if (*is_alive_signal.get()) {
                        on_change(written_val);
                    }

                    // Forward to NIC after callback
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

        /*
         * We have two signals here. m_signal is for users of this module to set.
         * Once set, the provided callback will stop getting called.
         * The thread will not stop however. In my testing, I found that there are
         * still some writes after the exit that need to be forwarded...
         */
        std::shared_ptr<std::atomic<bool>> m_internal_signal{nullptr};
        std::shared_ptr<std::atomic<bool>> m_signal{nullptr};
        std::unique_ptr<std::jthread> m_thread{nullptr};
    };
} // namespace Profiler::MemoryRepeater
