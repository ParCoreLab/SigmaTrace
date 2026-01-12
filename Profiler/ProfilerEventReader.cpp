#include <fstream>
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
    explicit ProfilerEventReader(const Events::RuntimeConfig &config, const std::filesystem::path &output_path) :
        Base(config), m_output(output_path, std::ios::binary) {
        if (!m_output.is_open()) {
            throw std::runtime_error("Failed to open output file: " + output_path.string());
        }
    }

    // Handle DoorbellRing - tag dispatch via EventTag<T>
    void visit(Events::EventTag<DoorbellRing>, EventHeader &&header, DoorbellRing &&event) {
        std::cout << "[" << header.timestamp << "] Profiler::Events::DoorbellRing: addr=" << event.addr << std::endl;

        // TODO: serialize to m_output
        m_output << "TODO" << std::endl;
    }

private:
    std::ofstream m_output;
};

int main(int argc, char *argv[]) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <events dir> <output file>" << std::endl;
        std::exit(1);
    }

    Events::RuntimeConfig config{};
    config.EventsDirectory = std::filesystem::current_path() / std::filesystem::path{argv[1]} / "events";
    const std::filesystem::path output_path = argv[2];

    std::cout << "Reading events from: " << config.EventsDirectory << "\n";
    std::cout << "Writing output to: " << output_path << "\n\n";

    ProfilerEventReader reader{config, output_path};
    const size_t count = reader.process();

    std::cout << "\nProcessed " << count << " events.\n";

    return 0;
}
