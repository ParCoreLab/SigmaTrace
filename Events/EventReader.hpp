#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <queue>
#include <type_traits>
#include <utility>
#include <vector>

#include "EventRecorderConfig.hpp"
#include "EventStorage.hpp"
#include "EventTrait.hpp"

namespace Events {

    // ============================================================================
    // Type List for enabled events
    // ============================================================================

    template<typename... Events>
    struct TypeList {
        static constexpr size_t size = sizeof...(Events);
    };

    // Derived class implements
    // visit(EventTrait<EventT>, const EventFileHeader &, EventHeader&& header, EventT&& event) for EventT
    template<typename Derived, typename EventT>
    concept Visitable = requires(Derived d) {
        // TODO: move the first arg out
        d.visit(std::declval<EventTrait<EventT>>(), std::declval<const EventFileHeader&>(),
                std::declval<EventHeader &&>(), std::declval<EventT &&>());
    };

    // ============================================================================
    template<typename Derived, typename EventList>
    class EventReaderBase;

    template<typename Derived, typename... EnabledEvents>
    class EventReaderBase<Derived, TypeList<EnabledEvents...>> {
    public:
        using EnabledEventList = TypeList<EnabledEvents...>;

        explicit EventReaderBase(RuntimeConfig config) : m_config(std::move(config)) {}

        // Process all events from files, merged by timestamp
        size_t process() {
            // Deferred check - Derived type is complete at this point
            static_assert((Visitable<Derived, EnabledEvents> && ...),
                          "Make sure to define a visitor for all EnabledEvents!");

            if (m_paths.empty()) {
                m_paths = find_event_files();
            }

            if (m_paths.empty()) {
                return 0;
            }

            return process_merged();
        }

        const auto& get_stats() const noexcept {
            return m_event_stats;
        }

    protected:
        Derived& derived() {
            return static_cast<Derived&>(*this);
        }

        std::map<std::string_view, size_t> m_event_stats{};

    private:
        // Single stream state
        struct StreamState {
            std::ifstream stream;
            std::filesystem::path path;
            EventTypeId event_id{EventTypeId::Unknown};
            Timestamp next_timestamp{0};
            bool has_event{false};

            EventFileHeader file_header{};

            explicit StreamState(const std::filesystem::path& p) : stream(p, std::ios::binary), path(p) {
                if (!stream.is_open()) {
                    return;
                }

                // File-wide header
                stream.read(reinterpret_cast<char*>(&file_header), sizeof(file_header));
                if (stream.gcount() != sizeof(file_header)) {
                    // TODO: log better
                    std::cerr << "Failed reading file: " << path << std::endl;

                    stream.close();
                    return;
                }

                if (file_header._check != MAGIC_VALUE) {
                    std::cerr << "Magic value check fail in File header: " << path << std::endl;
                    std::cerr << "File is corrupted or recorded with an older version. Closing stream... " << std::endl;

                    stream.close();
                    return;
                }

                event_id = file_header.event_id;

                peek_next();
            }

            void peek_next() {
                has_event = false;
                if (!stream.is_open() || stream.eof() || stream.peek() == EOF) {
                    return;
                }

                const auto pos = stream.tellg();

                EventHeader header(0, 0);
                stream.read(reinterpret_cast<char*>(&header), sizeof(header));
                if (header._check != MAGIC_VALUE) {
                    std::cerr << "Magic value check fail in " << path << " (" << header._check << ")" << std::endl;
                    std::cerr << "File is corrupted or recorded with an older version. Closing stream... " << std::endl;

                    // A little extreme
                    stream.close();
                    return;
                }

                if (stream.gcount() == sizeof(header)) {
                    next_timestamp = header.timestamp_start;
                    has_event = true;

                    // Go back. Header and data will be read later
                    stream.seekg(pos);
                }
            }

            static EventTypeId match_event_id(const EventId_T id) {
                auto result = EventTypeId::Unknown;
                std::ignore = ((id == static_cast<EventId_T>(EventTrait<EnabledEvents>::id)
                                        ? (result = EventTrait<EnabledEvents>::id, true)
                                        : false) ||
                               ...);
                return result;
            }
        };

        // Heap entry for merge
        struct HeapEntry {
            Timestamp timestamp;
            size_t stream_idx;

            bool operator<(const HeapEntry& o) const {
                return timestamp > o.timestamp;
            }
        };

        [[nodiscard]] std::vector<std::filesystem::path> find_event_files() const {
            std::vector<std::filesystem::path> paths;

            if (!std::filesystem::exists(m_config.EventsDirectory)) {
                return paths;
            }

            for (const auto& entry: std::filesystem::directory_iterator(m_config.EventsDirectory)) {
                if (!entry.is_regular_file()) {
                    continue;
                }

                const auto filename = entry.path().filename().string();
                // if (filename.starts_with(m_config.EventFilePrefix) && filename.ends_with(m_config.EventFileSuffix)) {
                paths.push_back(entry.path());
                // }
            }

            std::sort(paths.begin(), paths.end());
            return paths;
        }

        size_t process_merged() {
            // Open all streams
            std::vector<StreamState> streams;
            streams.reserve(m_paths.size());

            for (const auto& path: m_paths) {
                streams.emplace_back(path);
            }

            // Initialize min-heap - Read one entry from each stream
            std::priority_queue<HeapEntry> heap;
            for (size_t i = 0; i < streams.size(); ++i) {
                if (streams[i].has_event) {
                    heap.push({streams[i].next_timestamp, i});
                }
            }

            size_t count = 0;

            while (!heap.empty()) {
                auto [ts, idx] = heap.top();
                heap.pop();

                auto& state = streams[idx];

                // Dispatch and read event
                if (dispatch_read(state)) {
                    ++count;
                } else {
                    // No reader for this, we need to skip the stream...
                    std::cerr << "WARNING: No reader for ID " << static_cast<EventId_T>(state.event_id) << ". "
                              << "Add its tag to the type list" << std::endl;
                    state.stream.close();
                    continue;
                }

                // Peek next from this stream
                state.peek_next();
                if (state.has_event) {
                    heap.push({state.next_timestamp, idx});
                }
            }

            return count;
        }

        bool dispatch_read(StreamState& state) {
            // Try each enabled event type
            return (try_dispatch<EnabledEvents>(state) || ...);
        }

        template<typename EventT>
        bool try_dispatch(StreamState& state) {
            if (EventTrait<EventT>::id != state.event_id) {
                return false;
            }

            StoredEvent<EventT> stored;
            state.stream.read(reinterpret_cast<char*>(&stored), sizeof(stored));

            if (state.stream.gcount() < static_cast<std::streamsize>(sizeof(stored))) {
                return false;
            }

            // There must be a visitor for this event if we're at this point
            derived().visit(EventTrait<EventT>{}, state.file_header, std::move(stored.header), std::move(stored.data));

            ++m_event_stats[EventTrait<EventT>::name];

            return true;
        }

        RuntimeConfig m_config{};
        std::vector<std::filesystem::path> m_paths;
    };

} // namespace Events
