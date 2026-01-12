#include <iostream>

#include "EventReader.hpp"
#include "EventRecorderConfig.hpp"

#include "Events/Mlx5DvEvents.hpp"

// Convenience
using Events::EventHeader;
using Profiler::Events::DoorbellRing;

using EnabledEvents = Events::TypeList<DoorbellRing>;

class ProfilerEventReader : public Events::EventReaderBase<ProfilerEventReader, EnabledEvents> {
    using Base = EventReaderBase;

public:
    using Base::Base;

    // Handle SimpleEvent - tag dispatch via EventTag<T>
    void visit(Events::EventTag<DoorbellRing>, EventHeader &&header, DoorbellRing &&event) {
        std::cout << "[" << header.timestamp << "] Profiler::Events::DoorbellRing: addr=" << event.addr << "\n";
    }
};

// ============================================================================
// Main
// ============================================================================

int main(int argc, char *argv[]) {
    Events::RuntimeConfig config{};
    // TODO: ¯\_(ツ)_/¯
    // config.EventsDirectory = std::filesystem::path{"../"} / config.EventsDirectory;

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <events dir>" << std::endl;
        std::exit(1);
    }

    // Code is so bad...
    config.EventsDirectory = std::filesystem::current_path() / std::filesystem::path{argv[1]} / "events";

    std::cout << "Reading events from: " << config.EventsDirectory << "\n\n";

    ProfilerEventReader reader{config};
    reader.process();

    return 0;
}
