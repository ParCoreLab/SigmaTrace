#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <numeric>
#include <optional>
#include <vector>

#include "EventRecorderConfig.hpp"
#include "EventTrait.hpp"

constexpr auto HelloValue = 12;

// TODO: hardcoded for now
struct SimpleEvent {
    int64_t hello;
};
DEFINE_EVENT_TRAIT(SimpleEvent, Events::EventTypeId::UserDefined, "simple_event");

using Timestamp = uint64_t;

// Common header for all events
struct alignas(8) EventHeader {
    Timestamp timestamp;
};

struct alignas(8) StoredEvent {
    EventHeader header;
    SimpleEvent data;

    // Calculate padded size to ensure 8-byte alignment for next event
    static constexpr size_t padded_size = ((sizeof(EventHeader) + sizeof(SimpleEvent) + 7) / 8) * 8;
};

// Iterator-like class for streaming events from multiple files in timestamp order
class EventStreamMerger {
private:
    std::vector<std::ifstream> m_file_streams;
    std::vector<std::optional<StoredEvent>> m_current_events;
    size_t remaining_streams_;
    static constexpr size_t event_size_ = sizeof(StoredEvent);

    // Read next event from a specific stream
    bool read_event(size_t stream_index) {
        auto &stream = m_file_streams[stream_index];
        if (!stream.is_open() || stream.eof()) {
            return false;
        }

        StoredEvent event;
        stream.read(reinterpret_cast<char *>(&event), event_size_);
        if (stream.gcount() < static_cast<std::streamsize>(event_size_)) {
            stream.close();
            m_current_events[stream_index] = std::nullopt;
            return false;
        }

        m_current_events[stream_index] = event;
        return true;
    }

public:
    // Construct from vector of file entries
    explicit EventStreamMerger(const std::vector<std::filesystem::directory_entry> &file_entries) :
        remaining_streams_(0) {

        // Open all file streams
        m_file_streams.reserve(file_entries.size());
        for (const auto &entry: file_entries) {
            m_file_streams.emplace_back(entry.path(), std::ios::binary);
        }

        // Initialize current events buffer
        m_current_events.resize(m_file_streams.size());

        // Read first event from each stream
        for (size_t i = 0; i < m_file_streams.size(); i++) {
            if (read_event(i)) {
                remaining_streams_++;
            }
        }
    }

    // Get the next event with the smallest timestamp
    std::optional<StoredEvent> next() {
        if (remaining_streams_ == 0) {
            return std::nullopt;
        }

        // Find the stream with the smallest timestamp
        size_t min_index = 0;
        Timestamp min_timestamp = UINT64_MAX;

        for (size_t i = 0; i < m_current_events.size(); i++) {
            if (m_current_events[i].has_value()) {
                if (m_current_events[i]->header.timestamp < min_timestamp) {
                    min_timestamp = m_current_events[i]->header.timestamp;
                    min_index = i;
                }
            }
        }

        // Get the event to return
        StoredEvent result = *m_current_events[min_index];

        // Read the next event from that stream
        if (!read_event(min_index)) {
            remaining_streams_--;
        }

        return result;
    }

    // Check if there are more events available
    [[nodiscard]] bool has_more() const { return remaining_streams_ > 0; }

    // Get the number of remaining streams
    [[nodiscard]] size_t remaining_streams() const { return remaining_streams_; }
};

int main() {
    std::cout << "Hello World!" << std::endl;

    // Default config
    Events::RuntimeConfig events_config{};

    const auto events_path = /* std::filesystem::current_path() / */ events_config.EventsDirectory;
    const auto dir_iter = std::filesystem::directory_iterator{events_path};

    std::cout << "Listing thread buffer files in: " << events_path << std::endl;

    std::vector<std::filesystem::directory_entry> file_entries;
    std::copy_if(std::filesystem::directory_iterator{events_path}, std::filesystem::directory_iterator{},
                 std::back_inserter(file_entries), [&events_config](const auto &entry) {
                     if (!entry.is_regular_file()) {
                         return false;
                     }

                     const auto filename = entry.path().filename().string();
                     if (!filename.starts_with(events_config.EventFilePrefix) ||
                         !filename.ends_with(events_config.EventFileSuffix)) {
                         return false;
                     }

                     return true;
                 });

    // May help with timestamp matching
    std::ranges::sort(file_entries, [](const auto &a, const auto &b) {
        return a.path().filename() < b.path().filename();
    });

    if (file_entries.empty()) {
        std::cout << "No thread buffer files found." << std::endl;
        return 0;
    }

    std::cout << "Found " << file_entries.size() << " thread buffer files:\n";
    std::cout << file_entries[0].path().stem().string() << std::endl;

    // Calculate total size of all files
    const auto all_files_size = std::accumulate(std::begin(file_entries), std::end(file_entries), 0ull,
                                                [](uint64_t sum, const auto &entry) {
                                                    return sum + entry.file_size();
                                                });

    // Open all files as streams. We will read them all to find the next timestamp and insert into a flat list
    std::vector<std::ifstream> file_streams(file_entries.size());
    std::ranges::generate(file_streams, [n = 0, &file_entries]() mutable {
        return std::ifstream(file_entries[n++].path(), std::ios::binary);
    });

    std::vector<StoredEvent> current_read_events(file_streams.size());

    constexpr size_t event_size = sizeof(StoredEvent);
    const auto total_num_events = all_files_size / event_size;
    std::vector<StoredEvent> merged_events;
    merged_events.reserve(total_num_events);

    std::cout << "Merging " << total_num_events << " events from files..." << std::endl;

    size_t remaining_streams = file_streams.size();

    // Read the first event from each stream
    for (size_t i = 0; i < file_streams.size(); i++) {
        auto &stream = file_streams[i];
        if (!stream.is_open() || stream.eof()) {
            continue;
        }

        auto &current_event = current_read_events[i];

        stream.read(reinterpret_cast<char *>(&current_event), event_size);
        if (stream.gcount() < event_size) {
            // Reached end of file
            stream.close();
            remaining_streams--;
            continue;
        }
    }

    std::ranges::make_heap(current_read_events, [](const auto &a, const auto &b) {
        return a.header.timestamp > b.header.timestamp;
    });

    while (remaining_streams > 0) {
        // Get the event with the smallest timestamp
        std::ranges::pop_heap(current_read_events, [](const auto &a, const auto &b) {
            return a.header.timestamp > b.header.timestamp;
        });
        const auto &next_event = current_read_events.back();

        // Add to merged list
        merged_events.push_back(next_event);

        // Read the next event from the same stream
        const size_t stream_index = &next_event - &current_read_events[0];
        auto &stream = file_streams[stream_index];
        if (!stream.is_open() || stream.eof()) {
            // This stream is done
            current_read_events.pop_back();
            remaining_streams--;
            continue;
        }

        auto &current_event = current_read_events[stream_index];
        stream.read(reinterpret_cast<char *>(&current_event), event_size);
        if (stream.gcount() < event_size) {
            // Reached end of file
            stream.close();
            current_read_events.pop_back();
            remaining_streams--;
            continue;
        }

        // Re-heapify
        std::ranges::push_heap(current_read_events, [](const auto &a, const auto &b) {
            return a.header.timestamp > b.header.timestamp;
        });
    }

    std::cout << "Merged " << merged_events.size() << " events." << std::endl;

    for (const auto &event: merged_events) {
        if (event.data.hello == HelloValue) {
            continue;
        }

        std::cerr << "Data mismatch! Expected " << HelloValue << ", got " << event.data.hello << "\n";
        return 1;
    }

    std::cout << "Validated " << merged_events.size() << " events" << std::endl;

    // Example usage of EventStreamMerger for streaming events
    std::cout << "\nStreaming events using EventStreamMerger..." << std::endl;
    EventStreamMerger merger(file_entries);

    while (const auto &event = merger.next()) {
        // Process event here
        if (event->data.hello == HelloValue) {
            continue;
        }

        std::cerr << "Data mismatch! Expected " << HelloValue << ", got " << event->data.hello << "\n";
        return 1;
    }

    std::cout << "Finished streaming" << std::endl;

    return 0;
}
