# Events Library

**[WIP]** A high-performance, lock-free event recording library

Designed for low-overhead tracing of custom events with per-thread ring buffers
and automatic timestamping.

## TODO:

- [ ] Update this readme
- [ ] Add file backing for buffers. Right now buffers are in-memory and lost on destroy.
- [ ] Make sure lockless implementation is actually thread-safe. Resolve related in-code TODOs
- [ ] Try to eliminate dynamic memory allocations
- [ ] Try to eliminate event object copying. We can probably do placement new directly into the ring buffer
- [ ] Add timestamp correction mechanism
- [ ] Add tests for correctness and performance. Maybe compare to https://github.com/cameron314/concurrentqueue

## Features

- Lock-free recording: Per-thread and per-event buffers to avoid synchronization
- Compile-time defined events
- Automatic timestamping using `std::chrono::steady_clock`. Subject to change.
- Minimal binary format with 8-byte alignment

## Usage

```cmake
add_subdirectory(Events)
target_link_libraries(your_target PRIVATE Events)
```

### 1. Define an Event Structure

```cpp
#include "EventTrait.hpp"

// Packed event struct MyEvent
struct alignas(8) PostRecvEvent {
    uint64_t request_id;
    uint32_t status_code;
    uint32_t payload_size;
};

// Register the event with a unique ID and name
DEFINE_EVENT_TRAIT(MyEvent, Events::EventTypeId::UserDefined, "my_event");
```

### 2. Record Events

```cpp
#include "Events/EventRecorder.hpp"

void handle_request(uint64_t id, uint32_t status, uint32_t size) {
    // Record with aggregate initialization
    Events::EventRecorder::instance().Record(MyEvent{
        .request_id = id,
        .status_code = status,
        .payload_size = size
    });

    // Or use emplace construction
    Events::EventRecorder::instance().Emplace<MyEvent>(id, status, size);
}
```

### 3. Query Statistics

```cpp
// Aggregate stats across all threads
auto stats = Events::EventRecorder::get_stats();
printf("Buffered: %zu, Dropped: %zu, Threads: %zu\n",
       stats.total_buffered, stats.total_dropped, stats.active_threads);

// Per-event-type stats
auto event_stats = Events::EventRecorder::instance().get_event_stats<MyEvent>();
printf("%s: %zu buffered, %zu dropped\n",
       event_stats.name, event_stats.buffered, event_stats.dropped);
```

# Architecture

## Thread Model:

There are several layers of buffers for recording events:

- **Thread On Top**: Each thread has its own local `thread_local ThreadBuffers` instance
- **ThreadBuffers**: Each `ThreadBuffers` instance contains its own local
  `std::unordered_map<EventTypeId, std::unique_ptr<IEventBuffer>>`.
  Each entry in the map corresponds to a distinct event type. For N threads, there are N such maps
- **Typed Event Buffers**: Each `TypedEventBuffer<EventT, Capacity> : IEventBuffer` entry in a `ThreadBuffer` contains
  a lock-free `EventRingBuffer<EventT, Capacity>` to store events
- **Ring buffers**: At the lowest level, `EventRingBuffer<EventT, Capacity>` manages a circular buffer of raw bytes
  to store events of type `EventT`. The buffer is stack allocated and assumes a single thread writes to it.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        ThreadBufferRegistry (Singleton)                     │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  std::vector<std::unique_ptr<ThreadBuffers>> m_all_buffers            │  │
│  │  std::mutex m_mutex  (protects registration only)                     │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                    │                                        │
│            ┌───────────────────────┼───────────────────────┐                │
│            │                       │                       │                │
│            ▼                       ▼                       ▼                │
│   ┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐        │
│   │  ThreadBuffers  │    │  ThreadBuffers  │    │  ThreadBuffers  │        │
│   │   (Thread 1)    │    │   (Thread 2)    │    │   (Thread N)    │        │
│   └────────┬────────┘    └────────┬────────┘    └────────┬────────┘        │
└────────────┼──────────────────────┼──────────────────────┼──────────────────┘
             │                      │                      │
             ▼                      ▼                      ▼
    thread_local ptr*      thread_local ptr*      thread_local ptr*
    (cached access)        (cached access)        (cached access)

┌─────────────────────────────────────────────────────────────────────────────┐
│                              ThreadBuffers                                  │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  std::unordered_map<EventTypeId, std::unique_ptr<IEventBuffer>>       │  │
│  │                                                                       │  │
│  │   EventTypeId=1 ──► TypedEventBuffer<EventA>                          │  │
│  │   EventTypeId=2 ──► TypedEventBuffer<EventB>                          │  │
│  │   EventTypeId=3 ──► TypedEventBuffer<EventC>                          │  │
│  │        ...                  ...                                       │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│                    TypedEventBuffer<EventT, Capacity>                       │
│                         implements IEventBuffer                             │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  EventRingBuffer<EventT, Capacity> m_buffer                           │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│                     EventRingBuffer<EventT, Capacity>                       │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  ThreadLocalRingBuffer<ElementSize, Capacity> m_buffer                │  │
│  │  size_t m_dropped_count                                               │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│              ThreadLocalRingBuffer<ElementSize, Capacity>                   │
│                         SINGLE-THREAD ONLY                                  │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  uint64_t m_current  (write index, cache-line aligned)                │  │
│  │  uint8_t m_buffer[ElementSize * Capacity]  (inline array)             │  │
│  │                                                                       │  │
│  │  ┌──────┬──────┬──────┬──────┬──────┬──────┬──────┬──────┐            │  │
│  │  │Slot 0│Slot 1│Slot 2│Slot 3│ ...  │Slot N│      │      │  ◄─┐       │  │
│  │  └──────┴──────┴──────┴──────┴──────┴──────┴──────┴──────┘    │       │  │
│  │           ▲                                         │         │       │  │
│  │           │          circular overwrite ────────────┘         │       │  │
│  │           └───────────────────────────────────────────────────┘       │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
│  Each slot contains:  ┌─────────────────────────────────────┐               │
│                       │ EventHeader (8B) │ EventT data      │ (8B aligned)  │
│                       └─────────────────────────────────────┘               │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Event ID Ranges

Each distinct event **type** has a dedicated ID defined in `Events.hpp`.
Event IDs are stable and must never change once assigned to maintain
backwards compatibility.

Currently allocated ID range (subject to change):

| Range   | Category | Description                  |
|---------|----------|------------------------------|
| 0       | Reserved | Unknown/invalid event        |
| 100-199 | IBVerbs  | InfiniBand verbs operations  |
| 200-299 | CUDA     | GPU kernel/memory operations |
| 300-399 | NCCL     | Collective communication ops |
| 1000+   | Misc     | Other events                 |

## File Structure

```
EventRecorder (singleton)
    │
    ├── TimestampSource (steady_clock epoch)
    │
    └── ThreadBufferRegistry (global)
            │
            └── ThreadBuffers (per-thread)
                    │
                    └── TypedEventBuffer<EventT> (per-event-type)
                            │
                            └── EventRingBuffer<EventT, 4096>
                                    │
                                    └── ThreadLocalRingBuffer (raw storage)
```

## (In-Memory) Storage Format

Each event in the buffer is attached a header and is stored
in the following binary format:

```
┌─────────────────────────────────────────┐
│ EventHeader (8 bytes)                   │
│   └── timestamp: uint64_t (nanoseconds) │
├─────────────────────────────────────────┤
│ EventData (sizeof(EventT))              │
├─────────────────────────────────────────┤
│ Padding (to 8-byte alignment)           │
└─────────────────────────────────────────┘
```

## Thread Safety

- `Record()` and `Emplace()` are thread-safe (each thread writes to its own buffer)
- `get_stats()` and iteration APIs acquire a mutex to traverse thread buffers
- Ring Buffer implementation by itself **is not thread safe** as it assumes it's thread local

## Ring Buffer Capacity

The Ring Buffer implementation has a default capacity of 4096 events per event type per thread.
Currently, as there is no file backing, events are overwritten when the buffer is full with no warning.