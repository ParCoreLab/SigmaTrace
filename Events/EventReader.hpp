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

    // ============================================================================
    // Tag for dispatch - carries event type info
    // ============================================================================

    template<typename EventT>
    struct EventTag {};

    // ============================================================================
    // SFINAE check: does Derived have visit(EventTag<E>, EventHeader, E) ?
    // ============================================================================

    template<typename Derived, typename EventT, typename = void>
    struct HasVisit : std::false_type {};

    template<typename Derived, typename EventT>
    struct HasVisit<
            Derived, EventT,
            std::void_t<decltype(std::declval<Derived>().visit(
                    std::declval<EventTag<EventT>>(), std::declval<EventHeader &&>(), std::declval<EventT &&>()))>>
        : std::true_type {};

    template<typename Derived, typename EventT>
    inline constexpr bool has_visit_v = HasVisit<Derived, EventT>::value;

    // ============================================================================
    // Event Reader Base - CRTP base class
    //
    // Derived class implements:
    //   auto visit(EventTag<MyEvent>, const EventHeader& header, const MyEvent& event)
    //
    // The return value is passed to on_result() if provided, or discarded.
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
            if (m_paths.empty()) {
                m_paths = find_event_files();
            }

            if (m_paths.empty()) {
                return 0;
            }

            return process_merged();
        }

    protected:
        Derived &derived() { return static_cast<Derived &>(*this); }

    private:
        // Single stream state
        struct StreamState {
            std::ifstream stream;
            std::filesystem::path path;
            EventTypeId event_id{EventTypeId::Unknown};
            Timestamp next_timestamp{0};
            bool has_event{false};

            EventFileHeader file_header{};

            explicit StreamState(const std::filesystem::path &p) : stream(p, std::ios::binary), path(p) {
                if (!stream.is_open()) {
                    return;
                }

                // File-wide header
                stream.read(reinterpret_cast<char *>(&file_header), sizeof(file_header));
                if (stream.gcount() != sizeof(file_header)) {
                    // TODO: log
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

                auto pos = stream.tellg();

                EventHeader header{};
                stream.read(reinterpret_cast<char *>(&header), sizeof(header));

                if (stream.gcount() == sizeof(header)) {
                    next_timestamp = header.timestamp;
                    has_event = true;

                    // Go back. Header and data will be read later
                    stream.seekg(pos);
                }
            }

            static EventTypeId detect_event_type(const std::filesystem::path &p) {
                // TODO: this should be in a helper in Events config
                // Also, really ugly code
                // Expects id_other-st
                const std::string filename = p.stem().string();
                const auto end_pos = filename.find('_');
                if (end_pos == std::string::npos) {
                    return EventTypeId::Unknown;
                }

                // TODO: extra copy
                const auto &substr = filename.substr(0, end_pos);
                const auto event_id = std::stoi(substr);
                const auto match = match_event_id(event_id);
                return match;
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

            bool operator<(const HeapEntry &o) const { return timestamp > o.timestamp; }
        };

        [[nodiscard]] std::vector<std::filesystem::path> find_event_files() const {
            std::vector<std::filesystem::path> paths;

            if (!std::filesystem::exists(m_config.EventsDirectory)) {
                return paths;
            }

            for (const auto &entry: std::filesystem::directory_iterator(m_config.EventsDirectory)) {
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

            for (const auto &path: m_paths) {
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

                auto &state = streams[idx];

                // Dispatch and read event
                if (dispatch_read(state)) {
                    ++count;
                } else {
                    // No reader for this, we need to skip the stream...
                    std::cerr << "WARNING: No reader for ID " << static_cast<int>(state.event_id) << std::endl;
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

        bool dispatch_read(StreamState &state) {
            // Try each enabled event type
            return (try_dispatch<EnabledEvents>(state) || ...);
        }

        template<typename EventT>
        bool try_dispatch(StreamState &state) {
            if (EventTrait<EventT>::id != state.event_id) {
                return false;
            }

            StoredEvent<EventT> stored;
            state.stream.read(reinterpret_cast<char *>(&stored), sizeof(stored));

            if (state.stream.gcount() < static_cast<std::streamsize>(sizeof(stored))) {
                return false;
            }

            // There must be a visitor for this event if we're at this point
            derived().visit(EventTag<EventT>{}, std::move(stored.header), std::move(stored.data));

            return true;
        }

        RuntimeConfig m_config{};
        std::vector<std::filesystem::path> m_paths;
    };

} // namespace Events
