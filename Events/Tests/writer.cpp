#include <array>
#include <chrono>
#include <iostream>
#include <thread>

#include "EventRecorder.hpp"
#include "EventTrait.hpp"
#include "Events.hpp"

// Protobuf
#include "Addressbook.pb.h"

// FlatBuffers
#include "Simple_generated.h"
#include "flatbuffers/flatbuffers.h"

#include "Protobuf/ProtobufEventTrait.hpp"

constexpr auto HelloValue = 12;

// Just a struct
struct SimpleEvent {
    int64_t hello;
};
DEFINE_EVENT_TRAIT(SimpleEvent, Events::EventTypeId::UserDefined, "simple_event");

// Proto needs metadata definitions too
DEFINE_PROTOBUF_EVENT_TRAIT(tutorial::SimpleEvent, Events::EventTypeId::CudaKernelLaunch, "proto_simpleevent");

// Flatbuf table
// DEFINE_EVENT_TRAIT(flatbuffers::Offset<SimpleEventFlat>, Events::EventTypeId::UserDefined, "simple_event_flat");

// Flatbuf struct
DEFINE_EVENT_TRAIT(SimpleEventFlat, Events::EventTypeId::UserDefined, "simple_event_flat");

int main() {
    std::cout << "ok" << std::endl;

    // Regular struct
    constexpr SimpleEvent event{.hello = 12};

    // Protobuf
    // tutorial::SimpleEvent event{};
    // event.set_hello(HelloValue);

    // Flatbuf table
    // flatbuffers::FlatBufferBuilder builder(1024);
    // flatbuffers::Offset<SimpleEventFlat> event = CreateSimpleEventFlat(builder, HelloValue);

    // Flatbuf struct
    // SimpleEventFlat event{HelloValue};

    // NOTE: the runtime config is already the default, just leaving an example here
    using EventRecorder = Events::EventRecorder<Events::DebugCompileTimeConfig>;
    Events::RuntimeConfig config{.EventFileSuffix = ".event"};
    auto& instance = EventRecorder::instance(std::move(config));

    // Warmup
    for (auto i = 0; i < 100; i++) {
        // instance.Record(event);

        auto slot = instance.GetSlot<decltype(event)>();

        // This is to make if constexpr work... Without a templated lambda it won't discard the branch at compile time
        [](auto* ptr) {
            if constexpr (std::is_base_of_v<google::protobuf::Message, std::remove_pointer_t<decltype(ptr)>>) {
                ptr->set_hello(HelloValue);
            } else if constexpr (requires { &std::remove_pointer_t<decltype(ptr)>::mutate_hello; }) {
                ptr->mutate_hello(HelloValue);
            } else {
                ptr->hello = HelloValue;
            }
        }(slot.get());
    }

    constexpr auto num_iterations = 1000000;
    {
        const auto start = std::chrono::high_resolution_clock::now();
        for (auto i = 0; i < num_iterations; i++) {
            instance.Record(event);
        }
        const auto end = std::chrono::high_resolution_clock::now();
        const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

        std::cout << "=== Average time: " << duration / num_iterations << " ns ===\n";
    }

    constexpr auto num_threads = 100;
    std::array<std::thread, num_threads> threads;
    {
        const auto start = std::chrono::high_resolution_clock::now();
        for (auto i = 0; i < num_threads; i++) {
            threads[i] = std::thread([&]() {
                for (auto j = 0; j < num_iterations / num_threads; j++) {
                    instance.Record(event);
                }
            });
        }

        for (auto i = 0; i < num_threads; i++) {
            threads[i].join();
        }

        instance.Flush();

        const auto end = std::chrono::high_resolution_clock::now();
        const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

        std::cout << "=== Average time (thread): " << duration / num_iterations << " ns ===\n";
    }
}
