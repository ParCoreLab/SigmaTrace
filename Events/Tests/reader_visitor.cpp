// Example: Using EventReaderBase with CRTP and tag dispatch
//
// Derive from EventReaderBase and implement visit() overloads for each
// event type you want to handle. Events without a visit() are skipped.

#include <cstdint>
#include <iostream>

#include "EventReader.hpp"
#include "EventRecorderConfig.hpp"
#include "EventTrait.hpp"

// ============================================================================
// Example Event Definition
// ============================================================================

namespace Example {
    struct alignas(8) SimpleEvent {
        int64_t value;
    };
} // namespace Example

DEFINE_EVENT_TRAIT(Example::SimpleEvent, Events::EventTypeId::UserDefined, "simple_event");

// ============================================================================
// Define enabled events
// ============================================================================

using EnabledEvents = Events::TypeList<Example::SimpleEvent>;

// ============================================================================
// Example Reader Implementation
// ============================================================================

class ProfilerEventReader : public Events::EventReaderBase<ProfilerEventReader, EnabledEvents> {
    using Base = EventReaderBase<ProfilerEventReader, EnabledEvents>;

public:
    using Base::Base;

    // Handle SimpleEvent - tag dispatch via EventTag<T>
    void visit(Events::EventTag<Example::SimpleEvent>, const Events::EventHeader &header,
               const Example::SimpleEvent &event) {
        std::cout << "[" << header.timestamp << "] SimpleEvent: value=" << event.value << "\n";
        ++m_count;
        m_sum += event.value;
    }

    void print_summary() const {
        std::cout << "\nProcessed " << m_count << " events\n";
        std::cout << "Sum of values: " << m_sum << "\n";
    }

private:
    size_t m_count{0};
    int64_t m_sum{0};
};

// ============================================================================
// Main
// ============================================================================

int main() {
    Events::RuntimeConfig config{};
    // TODO: ¯\_(ツ)_/¯
    config.EventsDirectory = std::filesystem::path{"../../"} / config.EventsDirectory;
    std::cout << "Reading events from: " << config.EventsDirectory << "\n\n";

    ProfilerEventReader reader{config};
    size_t total = reader.process();

    std::cout << "\nTotal events processed: " << total << "\n";
    reader.print_summary();

    return 0;
}
