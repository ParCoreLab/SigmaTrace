#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <boost/lockfree/spsc_queue.hpp>

namespace Profiler {

    // Consumer wait strategy:
    //   BusyWait:   spin with yield(), lowest latency, burns a core per processor.
    //               Best when processes are pinned (e.g. mpirun --bind-to socket).
    //   CondVar:    sleep on condition variable, wake on signal from producer.
    //               Near-zero CPU when idle, near-zero wake latency (~1-2µs futex).
    //               Producer only syscalls when consumer is actually sleeping.
    enum class ConsumerWaitStrategy {
        BusyWait,
        CondVar
    };
    static constexpr auto WaitStrategy = ConsumerWaitStrategy::BusyWait;

    // Generic background processor: per-thread SPSC queues drained by a single consumer thread.
    // T = the snapshot type (fixed-size struct captured on hot path)
    // Capacity = per-thread queue depth (compile-time, power-of-2)
    template<typename T, size_t Capacity = 16384>
    class BackgroundProcessor {
    public:
        using ProcessFn = void (*)(const T&);

        explicit BackgroundProcessor(ProcessFn process_fn) :
            m_process_fn(process_fn), m_consumer([this] {
                consumer_loop();
            }) {}

        ~BackgroundProcessor() {
            shutdown();
        }

        // Non-copyable, non-movable
        BackgroundProcessor(const BackgroundProcessor&) = delete;
        BackgroundProcessor& operator=(const BackgroundProcessor&) = delete;

        // Hot path: enqueue one item into calling thread's SPSC queue.
        // Returns false if queue full (item dropped). Wait-free.
        bool enqueue(const T& item) {
            const bool ok = get_or_create_queue().queue.push(item);
            if constexpr (WaitStrategy == ConsumerWaitStrategy::CondVar) {
                // Only issue a syscall if the consumer is actually sleeping
                if (m_consumer_sleeping.load(std::memory_order_relaxed)) {
                    m_wake_cv.notify_one();
                }
            }
            return ok;
        }

        // Graceful shutdown: signal consumer, drain all remaining items, join thread.
        void shutdown() {
            bool expected = true;
            if (!m_running.compare_exchange_strong(expected, false)) {
                return; // already shut down
            }

            if constexpr (WaitStrategy == ConsumerWaitStrategy::CondVar) {
                m_wake_cv.notify_one();
            }

            if (m_consumer.joinable()) {
                m_consumer.join();
            }

            // Final drain — process any items that arrived between last poll and join
            drain_all();
        }

        // Post-fork abandon: consumer thread is dead in the child process,
        // so we detach (to prevent join on a non-existent thread) and discard queues.
        void abandon() {
            m_running.store(false, std::memory_order_relaxed);
            if (m_consumer.joinable()) {
                m_consumer.detach();
            }
            // Don't drain — the process functions reference parent state
            std::scoped_lock lock(m_registry_mutex);
            m_queues.clear();
        }

    private:
        struct alignas(64) ThreadQueue {
            boost::lockfree::spsc_queue<T, boost::lockfree::capacity<Capacity>> queue;
        };

        ThreadQueue& get_or_create_queue() {
            // Fast path: return cached pointer
            thread_local ThreadQueue* cached = nullptr;
            if (cached) [[likely]] {
                return *cached;
            }

            // Slow path: first call on this thread
            auto tq = std::make_shared<ThreadQueue>();
            cached = tq.get();
            std::scoped_lock lock(m_registry_mutex);
            m_queues.push_back(std::move(tq));
            return *cached;
        }

        void drain_all() {
            std::scoped_lock lock(m_registry_mutex);
            for (const auto& tq: m_queues) {
                tq->queue.consume_all([this](const T& item) {
                    m_process_fn(item);
                });
            }
        }

        // Returns true if any items were consumed
        bool drain_once() {
            bool did_work = false;
            std::scoped_lock lock(m_registry_mutex);
            for (const auto& tq: m_queues) {
                const auto consumed = tq->queue.consume_all([this](const T& item) {
                    m_process_fn(item);
                });
                if (consumed > 0) {
                    did_work = true;
                }
            }
            return did_work;
        }

        void consumer_loop() {
            while (m_running.load(std::memory_order_relaxed)) {
                const bool did_work = drain_once();

                if (!did_work) {
                    if constexpr (WaitStrategy == ConsumerWaitStrategy::BusyWait) {
                        std::this_thread::yield();
                    } else {
                        // Sleep until a producer signals or shutdown
                        std::unique_lock lock(m_wake_mutex);
                        m_consumer_sleeping.store(true, std::memory_order_release);
                        m_wake_cv.wait(lock, [this] {
                            return !m_running.load(std::memory_order_relaxed) || drain_once();
                        });
                        m_consumer_sleeping.store(false, std::memory_order_relaxed);
                    }
                }
            }

            // Final drain after stop signal
            drain_all();
        }

        // Thread-local queue management (shared_ptr for thread-destruction safety)
        // only taken on first enqueue per thread + consumer loop
        std::mutex m_registry_mutex;
        std::vector<std::shared_ptr<ThreadQueue>> m_queues;

        // Consumer
        ProcessFn m_process_fn;
        std::atomic<bool> m_running{true};
        std::thread m_consumer;

        // CondVar wait strategy state
        std::mutex m_wake_mutex;
        std::condition_variable m_wake_cv;
        // avoids spurious notify syscalls
        std::atomic<bool> m_consumer_sleeping{false};
    };

} // namespace Profiler
